#!/usr/bin/env python3
"""Asyncio virtual clients for the NCS load-test suite (NFR-P-04 / NFR-P-05).

Standard-library-only (Python 3.10+) building blocks:

* ``HttpClient``: one persistent HTTP/1.1 connection over TLS with keep-alive.
* ``WebSocketClient``: a minimal RFC 6455 client (masked client frames,
  unmasked server frames, protocol-level ping/pong, application-level
  ``{"type":"ping"}``/``{"type":"pong"}``), mirroring the synchronous client in
  ``tests/server_smoke_test.py``.
* ``TokenBucketPacer``: client-side pacing so the harness never trips the
  server's own per-IP rate limiter outside the deliberately measured phases.
* ``LatencyStats`` / ``percentile``: latency statistics for the evidence
  reports.

All traffic is loopback HTTPS/WSS with a self-signed certificate and no
certificate verification, exactly like the smoke test; NFR-D-01's insecure
HTTP mode is never used.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import os
import ssl
import statistics
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def default_tls_context() -> ssl.SSLContext:
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    return context


class TransportError(Exception):
    """Connection-level failure that a caller may retry."""


class HttpError(Exception):
    """A request that could not be completed after a single reconnect."""


@dataclass
class HttpResponse:
    status: int
    headers: dict = field(default_factory=dict)
    body: bytes = b""

    def json(self):
        return json.loads(self.body.decode("utf-8"))


class HttpClient:
    """One keep-alive HTTP/1.1 connection over TLS."""

    def __init__(self, host: str, port: int, ssl_context: ssl.SSLContext,
                 timeout: float = 20.0):
        self.host = host
        self.port = port
        self.ssl_context = ssl_context
        self.timeout = timeout
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None

    async def connect(self) -> None:
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(
                self.host, self.port, ssl=self.ssl_context,
                server_hostname=self.host),
            timeout=self.timeout)
        self._writer.transport.set_write_buffer_limits(high=1024 * 1024)

    async def close(self) -> None:
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except (OSError, ssl.SSLError):
                pass
            self._writer = None

    async def _read_headers(self) -> tuple[int, dict]:
        lines = []
        while True:
            line = await asyncio.wait_for(
                self._reader.readline(), timeout=self.timeout)
            if not line:
                raise TransportError("connection closed while reading headers")
            if line in (b"\r\n", b"\n"):
                break
            lines.append(line)
        if not lines:
            raise TransportError("empty response head")
        status_line = lines[0].decode("latin-1").rstrip("\r\n")
        parts = status_line.split(" ", 2)
        if len(parts) < 2 or not parts[0].startswith("HTTP/"):
            raise TransportError(f"malformed status line: {status_line!r}")
        headers = {}
        for raw in lines[1:]:
            text = raw.decode("latin-1").rstrip("\r\n")
            name, _, value = text.partition(":")
            headers[name.strip().lower()] = value.strip()
        return int(parts[1]), headers

    async def _read_body(self, status: int, headers: dict) -> bytes:
        if status in (204, 304):
            return b""
        length = headers.get("content-length")
        if length is not None:
            remaining = int(length)
            chunks = []
            while remaining > 0:
                chunk = await asyncio.wait_for(
                    self._reader.read(remaining), timeout=self.timeout)
                if not chunk:
                    raise TransportError("connection closed mid-body")
                chunks.append(chunk)
                remaining -= len(chunk)
            return b"".join(chunks)
        if headers.get("transfer-encoding", "").lower() == "chunked":
            chunks = []
            while True:
                size_line = await asyncio.wait_for(
                    self._reader.readline(), timeout=self.timeout)
                try:
                    size = int(size_line.strip().split(b";")[0], 16)
                except ValueError as exc:
                    raise TransportError(
                        f"malformed chunk size {size_line!r}") from exc
                if size == 0:
                    # Trailer section ends with a blank line.
                    while True:
                        trailer = await asyncio.wait_for(
                            self._reader.readline(), timeout=self.timeout)
                        if trailer in (b"\r\n", b"\n", b""):
                            break
                    break
                chunk = b""
                while len(chunk) < size:
                    part = await asyncio.wait_for(
                        self._reader.read(size - len(chunk)),
                        timeout=self.timeout)
                    if not part:
                        raise TransportError("connection closed mid-chunk")
                    chunk += part
                chunks.append(chunk)
                await asyncio.wait_for(
                    self._reader.readline(), timeout=self.timeout)  # CRLF
            return b"".join(chunks)
        # No framing: read until the server closes.
        chunks = []
        while True:
            part = await asyncio.wait_for(self._reader.read(65536),
                                          timeout=self.timeout)
            if not part:
                break
            chunks.append(part)
        return b"".join(chunks)

    async def request(self, method: str, path: str,
                      headers: Optional[dict] = None,
                      body: Optional[bytes] = None) -> HttpResponse:
        if self._writer is None or self._writer.is_closing():
            await self.connect()
        payload = body or b""
        merged = dict(headers or {})
        merged.setdefault("Host", self.host)
        merged.setdefault("Connection", "keep-alive")
        if payload:
            merged.setdefault("Content-Type", "application/json")
            merged.setdefault("Content-Length", str(len(payload)))
        head = f"{method} {path} HTTP/1.1\r\n"
        head += "".join(f"{name}: {value}\r\n" for name, value in merged.items())
        try:
            self._writer.write(head.encode("latin-1") + b"\r\n" + payload)
            await self._writer.drain()
            status, resp_headers = await self._read_headers()
            body_bytes = await self._read_body(status, resp_headers)
            return HttpResponse(status, resp_headers, body_bytes)
        except (ConnectionError, OSError, ssl.SSLError, asyncio.TimeoutError,
                TransportError):
            # A single reconnect for a stale keep-alive connection.
            await self.close()
            await self.connect()
            try:
                self._writer.write(head.encode("latin-1") + b"\r\n" + payload)
                await self._writer.drain()
                status, resp_headers = await self._read_headers()
                body_bytes = await self._read_body(status, resp_headers)
                return HttpResponse(status, resp_headers, body_bytes)
            except (ConnectionError, OSError, ssl.SSLError,
                    asyncio.TimeoutError, TransportError) as exc:
                raise HttpError(f"{method} {path} failed: {exc}") from exc

    async def get_json(self, path: str, headers: Optional[dict] = None) -> HttpResponse:
        return await self.request("GET", path, headers=headers)

    async def post_json(self, path: str, body: dict, headers: Optional[dict] = None) -> HttpResponse:
        headers = dict(headers or {})
        headers.setdefault("Content-Type", "application/json; charset=utf-8")
        return await self.request("POST", path, headers=headers,
                                  body=json.dumps(body).encode("utf-8"))


class WebSocketClient:
    """RFC 6455 client with an automatic pong task.

    The reader task answers protocol pings and application-level
    ``{"type":"ping"}`` frames immediately, so the server's 60-second pong
    timeout cannot fire while other connections are being driven.
    """

    def __init__(self, host: str, port: int, ssl_context: ssl.SSLContext,
                 token: Optional[str] = None, timeout: float = 15.0,
                 read_timeout: float = 90.0):
        self.host = host
        self.port = port
        self.ssl_context = ssl_context
        self.token = token
        self.timeout = timeout
        # Reader timeout must comfortably exceed the server's heartbeat
        # cadence (application ping after 30 s of inbound silence, close only
        # after 60 s without a pong) so idle sessions are never dropped by
        # the client itself.
        self.read_timeout = read_timeout
        self.status = 0
        self.events: list[dict] = []
        self.closed = False
        self.close_code: Optional[int] = None
        self.ready = asyncio.Event()
        self._writer: Optional[asyncio.StreamWriter] = None
        self._reader_task: Optional[asyncio.Task] = None

    async def connect(self) -> None:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port, ssl=self.ssl_context,
                                    server_hostname=self.host),
            timeout=self.timeout)
        self._writer = writer
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET /api/v1/events HTTP/1.1\r\n"
            f"Host: {self.host}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
        )
        if self.token is not None:
            request += f"Authorization: Bearer {self.token}\r\n"
        request += "\r\n"
        writer.write(request.encode("latin-1"))
        await writer.drain()
        head = bytearray()
        while b"\r\n\r\n" not in head:
            part = await asyncio.wait_for(reader.read(4096), timeout=self.timeout)
            if not part:
                break
            head.extend(part)
        raw = bytes(head)
        head_bytes, separator, remainder = raw.partition(b"\r\n\r\n")
        if not separator:
            raise TransportError("websocket handshake response was truncated")
        text = head_bytes.decode("latin-1")
        lines = text.split("\r\n")
        self.status = int(lines[0].split(" ", 2)[1])
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(":")
            headers[name.strip().lower()] = value.strip()
        if self.status == 101:
            expected = base64.b64encode(
                hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            if headers.get("sec-websocket-accept") != expected:
                raise TransportError("Sec-WebSocket-Accept mismatch")
            self._reader_task = asyncio.create_task(
                self._read_forever(reader, bytearray(remainder)))

    async def _read_forever(self, reader: asyncio.StreamReader,
                            buffer: bytearray) -> None:
        try:
            while not self.closed:
                while len(buffer) < 2:
                    part = await asyncio.wait_for(reader.read(65536),
                                                  timeout=self.read_timeout)
                    if not part:
                        self.closed = True
                        return
                    buffer.extend(part)
                first = buffer[0]
                opcode = first & 0x0F
                second = buffer[1]
                length = second & 0x7F
                offset = 2
                if length == 126:
                    while len(buffer) < 4:
                        part = await asyncio.wait_for(reader.read(65536),
                                                      timeout=self.read_timeout)
                        if not part:
                            self.closed = True
                            return
                        buffer.extend(part)
                    length = int.from_bytes(bytes(buffer[2:4]), "big")
                    offset = 4
                elif length == 127:
                    while len(buffer) < 10:
                        part = await asyncio.wait_for(reader.read(65536),
                                                      timeout=self.read_timeout)
                        if not part:
                            self.closed = True
                            return
                        buffer.extend(part)
                    length = int.from_bytes(bytes(buffer[2:10]), "big")
                    offset = 10
                masked = bool(second & 0x80)
                if masked:
                    while len(buffer) < offset + 4:
                        part = await asyncio.wait_for(reader.read(65536),
                                                      timeout=self.read_timeout)
                        if not part:
                            self.closed = True
                            return
                        buffer.extend(part)
                    mask = bytes(buffer[offset:offset + 4])
                    offset += 4
                while len(buffer) < offset + length:
                    part = await asyncio.wait_for(reader.read(65536),
                                                  timeout=self.read_timeout)
                    if not part:
                        self.closed = True
                        return
                    buffer.extend(part)
                payload = bytes(buffer[offset:offset + length])
                del buffer[:offset + length]
                if masked:
                    payload = bytes(byte ^ mask[i % 4]
                                    for i, byte in enumerate(payload))
                if opcode == 0x8:  # close
                    self.closed = True
                    if len(payload) >= 2:
                        self.close_code = int.from_bytes(payload[:2], "big")
                    return
                if opcode == 0x9:  # protocol ping
                    await self._send_frame(0xA, payload)
                    continue
                if opcode == 0x1:  # text
                    event = json.loads(payload.decode("utf-8"))
                    if event.get("type") == "ping":
                        await self.send_pong()
                    self.events.append(event)
                # 0xA pong and 0x2 binary are ignored.
        except (asyncio.CancelledError, ConnectionError, OSError,
                ssl.SSLError, asyncio.TimeoutError, ValueError):
            self.closed = True

    async def _send_frame(self, opcode: int, payload: bytes) -> None:
        if self._writer is None or self.closed:
            return
        mask = os.urandom(4)
        masked = bytes(byte ^ mask[index % 4]
                       for index, byte in enumerate(payload))
        length = len(payload)
        if length < 126:
            header = bytes([0x80 | opcode, 0x80 | length])
        elif length < 65536:
            header = bytes([0x80 | opcode, 0x80 | 126]) + length.to_bytes(2, "big")
        else:
            header = bytes([0x80 | opcode, 0x80 | 127]) + length.to_bytes(8, "big")
        self._writer.write(header + mask + masked)
        await self._writer.drain()

    async def send_pong(self) -> None:
        await self._send_frame(0x1, b'{"type":"pong"}')

    async def events_of_type(self, event_type: str) -> list:
        return [event for event in self.events if event.get("type") == event_type]

    async def wait_ready(self, timeout: float = 10.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if any(event.get("type") == "session.ready" for event in self.events):
                return True
            await asyncio.sleep(0.05)
        return False

    async def abort(self) -> None:
        """Close a connection whose handshake was refused (no upgrade)."""
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except (ConnectionError, OSError, ssl.SSLError):
                pass
            self._writer = None

    async def close(self) -> None:
        if self.closed or self._writer is None:
            return
        try:
            await self._send_frame(0x8, b"")
            await asyncio.sleep(0.1)
        except (ConnectionError, OSError, ssl.SSLError):
            pass
        try:
            self._writer.close()
            await self._writer.wait_closed()
        except (ConnectionError, OSError, ssl.SSLError):
            pass
        self.closed = True
        if self._reader_task is not None:
            self._reader_task.cancel()


class TokenBucketPacer:
    """Asyncio token bucket for pacing outbound request rates.

    Refills ``refill_per_sec`` tokens per second up to ``burst``.  Used with a
    refill slightly below the server's per-IP limiter (20/s, burst 50) so every
    phase except the measured peak stays clear of HTTP 429 responses.
    """

    def __init__(self, refill_per_sec: float, burst: float):
        self.refill_per_sec = refill_per_sec
        self.burst = burst
        self._tokens = burst
        self._updated = time.monotonic()
        self._lock = asyncio.Lock()

    async def acquire(self, count: float = 1.0) -> None:
        while True:
            async with self._lock:
                now = time.monotonic()
                elapsed = now - self._updated
                self._tokens = min(self.burst,
                                   self._tokens + elapsed * self.refill_per_sec)
                self._updated = now
                if self._tokens >= count:
                    self._tokens -= count
                    return
                wait_sec = (count - self._tokens) / self.refill_per_sec
            await asyncio.sleep(min(max(wait_sec, 0.01), 5.0))


@dataclass
class LatencyStats:
    samples: int = 0
    failures: int = 0
    rate_limited: int = 0
    values_ms: list = field(default_factory=list)
    min_ms: float = 0.0
    max_ms: float = 0.0
    p50_ms: float = 0.0
    p95_ms: float = 0.0
    p99_ms: float = 0.0
    mean_ms: float = 0.0
    duration_sec: float = 0.0
    achieved_rps: float = 0.0

    def add_sample(self, latency_ms: float) -> None:
        self.samples += 1
        self.values_ms.append(latency_ms)

    def finish(self) -> dict:
        values = sorted(self.values_ms)
        if values:
            self.min_ms = values[0]
            self.max_ms = values[-1]
            self.p50_ms = percentile(values, 50)
            self.p95_ms = percentile(values, 95)
            self.p99_ms = percentile(values, 99)
            self.mean_ms = statistics.fmean(values)
        if self.duration_sec > 0:
            self.achieved_rps = self.samples / self.duration_sec
        return {
            "samples": self.samples,
            "failures": self.failures,
            "rateLimited": self.rate_limited,
            "minMs": round(self.min_ms, 2),
            "p50Ms": round(self.p50_ms, 2),
            "p95Ms": round(self.p95_ms, 2),
            "p99Ms": round(self.p99_ms, 2),
            "maxMs": round(self.max_ms, 2),
            "meanMs": round(self.mean_ms, 2),
            "durationSec": round(self.duration_sec, 2),
            "achievedRps": round(self.achieved_rps, 2),
        }


def percentile(values: list, rank: float) -> float:
    """Linear-interpolated percentile on a sorted sample list."""
    if not values:
        return 0.0
    if len(values) == 1:
        return float(values[0])
    position = (len(values) - 1) * rank / 100.0
    lower = int(position)
    upper = lower + 1
    weight = position - lower
    return values[lower] * (1.0 - weight) + values[upper] * weight
