#!/usr/bin/env python3

import base64
import hashlib
import http.client
import json
import os
import re
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

REQUEST_TIMEOUT_SECONDS = 15 if os.name == "nt" else 2


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def request(port: int, path: str, headers=None, method="GET", body=None):
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    connection = http.client.HTTPSConnection(
        "127.0.0.1", port, context=context, timeout=REQUEST_TIMEOUT_SECONDS)
    connection.request(method, path, body=body, headers=headers or {})
    response = connection.getresponse()
    body = response.read()
    result = response.status, dict(response.getheaders()), body
    connection.close()
    return result


def oversized_headers_only(port: int):
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    connection = http.client.HTTPSConnection(
        "127.0.0.1", port, context=context, timeout=REQUEST_TIMEOUT_SECONDS)
    connection.putrequest("POST", "/api/v1/user/auth/register")
    connection.putheader("Content-Type", "application/json")
    connection.putheader("Content-Length", str(8 * 1024 * 1024 + 1))
    connection.endheaders()
    response = connection.getresponse()
    result = response.status, dict(response.getheaders()), response.read()
    connection.close()
    return result


def chunked_overflow(port: int) -> bytes:
    """Stream a chunked body past the 8 MiB framework cap without ever
    announcing a total length, so rejection can only happen while chunks are
    accumulated. Returns whatever the server sent back."""
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    raw = socket.create_connection(("127.0.0.1", port), timeout=5)
    connection = context.wrap_socket(raw)
    chunk = b"x" * 65536
    header = f"{len(chunk):x}\r\n".encode()
    try:
        connection.sendall(
            b"POST /api/v1/internal/ml/predictions/batch HTTP/1.1\r\n"
            b"Host: 127.0.0.1\r\n"
            b"Content-Type: application/json\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n")
        for _ in range(8 * 1024 * 1024 // len(chunk) + 2):
            try:
                connection.sendall(header + chunk + b"\r\n")
            except OSError:
                break
        connection.settimeout(2)
        received = b""
        while True:
            try:
                part = connection.recv(4096)
            except OSError:
                break
            if not part:
                break
            received += part
    finally:
        connection.close()
    return received


def stop_server(process: subprocess.Popen, timeout: int) -> bool:
    """Stop a test server and report whether Windows required forced termination."""
    forced_windows_stop = False
    if process.poll() is None:
        if os.name == "nt":
            forced_windows_stop = True
            process.terminate()
        else:
            process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
    return forced_windows_stop


class WsClient:
    """Minimal RFC 6455 client on top of the standard library: TLS handshake,
    masked client frames, unmasked server frames, application-level ping/pong
    and close-code parsing."""

    GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, port: int, token: str | None):
        raw = socket.create_connection(("127.0.0.1", port), timeout=5)
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        self._socket = context.wrap_socket(raw, server_hostname="127.0.0.1")
        self._socket.settimeout(5)
        key = base64.b64encode(os.urandom(16)).decode()
        headers = (
            "GET /api/v1/events HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
        )
        if token is not None:
            headers += f"Authorization: Bearer {token}\r\n"
        headers += "\r\n"
        self._socket.sendall(headers.encode())
        response = b""
        while b"\r\n\r\n" not in response:
            part = self._socket.recv(4096)
            if not part:
                break
            response += part
        head, _, remainder = response.partition(b"\r\n\r\n")
        self.status = int(head.split(b" ", 2)[1])
        self.handshake_headers = {}
        for line in head.split(b"\r\n")[1:]:
            name, _, value = line.partition(b":")
            self.handshake_headers[name.strip().lower().decode()] = value.strip().decode()
        self._buffer = bytearray(remainder)
        if self.status == 101:
            expected = base64.b64encode(
                hashlib.sha1((key + self.GUID).encode()).digest()).decode()
            actual = self.handshake_headers.get("sec-websocket-accept")
            assert actual == expected, (
                f"Sec-WebSocket-Accept mismatch: {actual!r} != {expected!r} "
                f"(key={key!r})")
        self.closed = False
        self.close_code = None
        self.close_reason = b""
        self.received = []
        self._lock = threading.Lock()
        self._socket_lock = threading.Lock()
        self._reader_thread = None
        # A real client reads its socket continuously. The background reader
        # answers application pings immediately so the server's pong timeout
        # cannot expire while the test drives the other connections: the
        # phases before the heartbeat check take over a minute on slow
        # Windows CI runners, longer than the server's 60 s pong timeout.
        if self.status == 101:
            self._reader_thread = threading.Thread(
                target=self._read_forever, daemon=True)
            self._reader_thread.start()

    def _read_forever(self):
        # The background reader owns the socket. It never holds _socket_lock
        # while blocked in recv (SSLSocket allows one concurrent reader and
        # writer), so the test thread can always send frames without waiting
        # for a read to time out.
        try:
            while True:
                with self._lock:
                    if self.closed:
                        return
                self._socket.settimeout(1.0)
                try:
                    opcode, payload = self._recv_frame()
                except socket.timeout:
                    continue
                if opcode == 0x1:
                    event = json.loads(payload)
                    if event.get("type") == "ping":
                        self.send_pong()
                    with self._lock:
                        self.received.append(event)
                elif opcode == 0x8:
                    with self._lock:
                        self.closed = True
                        if len(payload) >= 2:
                            self.close_code = int.from_bytes(payload[:2], "big")
                            self.close_reason = payload[2:]
                    return
                elif opcode == 0x9:
                    self._send_frame(0xA, payload)
                # opcode 0xA (pong) and 0x2 (binary) are ignored
        except (OSError, ValueError):
            with self._lock:
                self.closed = True

    def _read_exact(self, count: int) -> bytes:
        while len(self._buffer) < count:
            part = self._socket.recv(65536)
            if not part:
                raise ConnectionError("connection closed while reading")
            self._buffer.extend(part)
        result = bytes(self._buffer[:count])
        del self._buffer[:count]
        return result

    def _send_frame(self, opcode: int, payload: bytes):
        mask = os.urandom(4)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        length = len(payload)
        if length < 126:
            header = bytes([0x80 | opcode, 0x80 | length])
        elif length < 65536:
            header = bytes([0x80 | opcode, 0x80 | 126]) + length.to_bytes(2, "big")
        else:
            header = bytes([0x80 | opcode, 0x80 | 127]) + length.to_bytes(8, "big")
        with self._socket_lock:
            self._socket.sendall(header + mask + masked)

    def send_oversized_header(self, payload_length: int):
        """Announce an oversized inbound frame by sending only its header:
        the server's 1009 check fires at header parse, before the mask or
        payload bytes arrive. Keeping the writer out of the server's close
        path makes the 1009 close frame readable back deterministically —
        sending the full payload instead races the server's close against
        the client's in-flight write (SSLEOFError on the writer)."""
        header = (bytes([0x80 | 0x1, 0x80 | 127])
                  + payload_length.to_bytes(8, "big"))
        with self._socket_lock:
            self._socket.sendall(header)

    def send_text(self, payload: str):
        self._send_frame(0x1, payload.encode())

    def send_pong(self):
        self.send_text('{"type":"pong"}')

    def _recv_frame(self) -> tuple[int, bytes]:
        first, second = self._read_exact(2)
        opcode = first & 0x0F
        length = second & 0x7F
        if length == 126:
            length = int.from_bytes(self._read_exact(2), "big")
        elif length == 127:
            length = int.from_bytes(self._read_exact(8), "big")
        if second & 0x80:
            mask = self._read_exact(4)
            payload = self._read_exact(length)
            payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        else:
            payload = self._read_exact(length)
        return opcode, payload

    def recv_events(self, timeout: float):
        """Block until `timeout` seconds elapse or the connection closes,
        then return every frame collected so far. The background reader owns
        the socket; this polls the shared queue instead of reading."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                if self.closed:
                    break
            time.sleep(0.05)
        with self._lock:
            return list(self.received)

    def wait_for(self, predicate, timeout: float, consume: bool = True):
        """Poll the background reader's collected events until `predicate`
        matches any received event."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                matches = [event for event in self.received if predicate(event)]
                if matches:
                    if consume:
                        self.received = [event for event in self.received
                                         if not predicate(event)]
                    return matches
            time.sleep(0.05)
        return []

    def events_of_type(self, event_type: str):
        with self._lock:
            return [event for event in self.received if event.get("type") == event_type]

    def close(self):
        """Send the close frame, read the server's close reply back (bounded
        wait), then stop the background reader BEFORE the socket fd can be
        reused. Closing the fd while the reader is inside SSL_read/SSL_write
        leaves a live SSL object holding the old fd number: the next fresh
        connection reuses that fd, and the stale reader then consumes the new
        handshake's bytes or writes stale frames into it. Under CPU
        contention this surfaces as WRONG_VERSION_NUMBER / bad record mac on
        the request that follows close() — the reader must be joined before
        this method returns."""
        with self._lock:
            already_closed = self.closed
        if not already_closed:
            try:
                self._send_frame(0x8, b"")
            except OSError:
                pass
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                with self._lock:
                    if self.closed:
                        break
                time.sleep(0.02)
        with self._lock:
            self.closed = True
        try:
            self._socket.close()
        except OSError:
            pass
        if self._reader_thread is not None:
            self._reader_thread.join(timeout=2.0)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: server_smoke_test.py <ncs_server>", file=sys.stderr)
        return 2
    server = Path(sys.argv[1]).resolve()
    if not server.is_file():
        print("server executable is missing", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="ncs-smoke-") as temporary:
        root = Path(temporary)
        certificate = root / "certificate.pem"
        private_key = root / "private-key.pem"
        logs = root / "logs"
        database = root / "charge-platform.db"
        dashboard_snapshot = root / "dashboard.json"
        model_path = root / "models" / "load_rf.pkl"
        worker_script = Path(__file__).resolve().parent.parent / "ml" / "worker.py"
        server_stdout = root / "server-stdout.log"
        server_stderr = root / "server-stderr.log"
        restarted_stdout = root / "server-restarted-stdout.log"
        restarted_stderr = root / "server-restarted-stderr.log"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                "-subj", "/CN=127.0.0.1",
                "-addext", "subjectAltName=IP:127.0.0.1",
                "-keyout", str(private_key), "-out", str(certificate),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        os.chmod(private_key, 0o600)

        # UC-A-09 first-OWNER bootstrap: the one-shot command seeds the
        # production OWNER on the fresh database, never echoes the key, and
        # refuses to run twice.
        bootstrap_key = "Ncs-Bootstrap-2026-K3y"
        without_key = {name: value for name, value in os.environ.items()
                       if name != "NCS_ADMIN_BOOTSTRAP_KEY"}
        with_key = {**without_key, "NCS_ADMIN_BOOTSTRAP_KEY": bootstrap_key}

        def run_bootstrap(environment):
            return subprocess.run(
                [str(server), "--environment", "production", "--database-path",
                 str(database), "--bootstrap-owner", "root_owner"],
                env=environment, capture_output=True, text=True)

        missing_key = run_bootstrap(without_key)
        assert missing_key.returncode == 1
        assert "NCS_ADMIN_BOOTSTRAP_KEY" in missing_key.stderr

        bootstrapped = run_bootstrap(with_key)
        assert bootstrapped.returncode == 0, bootstrapped.stderr
        assert "root_owner" in bootstrapped.stdout and "created" in bootstrapped.stdout
        assert bootstrap_key not in bootstrapped.stdout + bootstrapped.stderr

        again = run_bootstrap(with_key)
        assert again.returncode == 1
        assert "one-shot" in again.stderr

        def server_arguments(server_port: int):
            return [
                str(server),
                "--environment", "development",
                "--listen-address", "127.0.0.1",
                "--port", str(server_port),
                "--worker-threads", "2",
                "--charge-time-scale", "3600",
                "--blocking-worker-threads", "2",
                "--blocking-queue-capacity", "32",
                "--log-directory", str(logs),
                "--database-path", str(database),
                "--tls-certificate", str(certificate),
                "--tls-private-key", str(private_key),
                "--cors-allowed-origins", "https://dashboard.local",
                "--dashboard-snapshot", str(dashboard_snapshot),
                "--python-executable", sys.executable,
                "--ml-worker-script", str(worker_script),
                "--ml-model-path", str(model_path),
            ]

        port = free_port()
        # Do not pipe the server's stderr without draining it: the server logs
        # to stderr, and a full pipe buffer (4 KiB on Windows) would block the
        # request-handling thread and deadlock the whole server.
        process = subprocess.Popen(
            server_arguments(port),
            stdout=open(server_stdout, "w", encoding="utf-8", errors="replace"),
            stderr=open(server_stderr, "w", encoding="utf-8", errors="replace"),
        )
        try:
            live = None
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                try:
                    live = request(port, "/api/v1/system/health/live")
                    break
                except (ConnectionError, OSError, ssl.SSLError):
                    time.sleep(0.05)
            if live is None:
                stderr = server_stderr.read_text(encoding="utf-8", errors="replace")
                raise AssertionError(f"server did not become live: {stderr}")

            status, headers, body = live
            payload = json.loads(body)
            assert status == 200 and payload == {"status": "UP"}
            request_id = headers.get("X-Request-ID") or headers.get("x-request-id")
            assert request_id and str(uuid.UUID(request_id)) == request_id
            assert (headers.get("Strict-Transport-Security")
                    or headers.get("strict-transport-security")) == "max-age=31536000"

            ready_status, _, ready_body = request(port, "/api/v1/system/health/ready")
            ready = json.loads(ready_body)
            assert ready_status == 200 and ready["status"] == "UP"
            assert all(value is True for value in ready["checks"].values())
            assert b"sqlite" not in ready_body.lower() and b"/" not in ready_body

            cors_status, _, cors_body = request(
                port, "/api/v1/system/health/live", {"Origin": "https://evil.invalid"})
            cors = json.loads(cors_body)
            assert cors_status == 403 and cors["code"] == 403

            head_status, head_headers, _ = request(
                port, "/definitely-missing", method="HEAD")
            assert head_status == 404
            assert head_headers.get("X-Request-ID") or head_headers.get("x-request-id")

            options_status, options_headers, _ = request(
                port,
                "/api/v1/system/health/live",
                {
                    "Origin": "https://dashboard.local",
                    "Access-Control-Request-Method": "GET",
                },
                "OPTIONS",
            )
            lower_options = {key.lower(): value for key, value in options_headers.items()}
            assert options_status == 204
            assert lower_options["access-control-allow-origin"] == "https://dashboard.local"
            assert lower_options.get("x-request-id")

            large_status, large_headers, large_body = oversized_headers_only(port)
            large = json.loads(large_body)
            lower_large = {key.lower(): value for key, value in large_headers.items()}
            assert large_status == 413 and large["code"] == 1, (
                large_status, large, lower_large)
            assert large["requestId"] == lower_large["x-request-id"]

            chunked = chunked_overflow(port)
            assert chunked.startswith(b"HTTP/1.1 413"), chunked[:120]
            assert b"X-Request-ID" in chunked, chunked[:120]

            json_headers = {"Content-Type": "application/json; charset=utf-8"}
            code_status, _, code_body = request(
                port,
                "/api/v1/user/auth/sms/code",
                json_headers,
                "POST",
                json.dumps({"phone": "13800138000", "purpose": "REGISTER"}),
            )
            code = json.loads(code_body)["data"]["developmentCode"]
            assert code_status == 200 and len(code) == 6
            register_status, _, register_body = request(
                port,
                "/api/v1/user/auth/register",
                json_headers,
                "POST",
                json.dumps({
                    "username": "smoke_user",
                    "phone": "13800138000",
                    "password": "smoke-password",
                    "smsCode": code,
                    "deviceId": "smoke-terminal",
                }),
            )
            registration = json.loads(register_body)
            token = registration["data"]["accessToken"]
            assert register_status == 201 and registration["data"]["user"]["phoneMasked"] == "138****8000"
            auth_headers = {"Authorization": f"Bearer {token}"}
            profile_status, _, profile_body = request(
                port, "/api/v1/user/me", auth_headers)
            profile = json.loads(profile_body)
            assert profile_status == 200 and profile["data"]["username"] == "smoke_user"

            # WebSocket handshake rejections: missing and unknown tokens.
            rejected = WsClient(port, None)
            assert rejected.status == 401, rejected.status
            garbage = WsClient(port, "garbage-token-" + "0" * 32)
            assert garbage.status == 401, garbage.status

            ws_user = WsClient(port, token)
            assert ws_user.status == 101
            ws_user.recv_events(2.0)
            ready_events = ws_user.events_of_type("session.ready")
            assert len(ready_events) == 1
            assert re.fullmatch(r"EV\d{8}\d{8}", ready_events[0]["eventId"])
            assert ready_events[0]["sequence"] >= 1

            # A second user: scope isolation and heartbeat target.
            code2_status, _, code2_body = request(
                port, "/api/v1/user/auth/sms/code", json_headers, "POST",
                json.dumps({"phone": "13800138001", "purpose": "REGISTER"}))
            code2 = json.loads(code2_body)["data"]["developmentCode"]
            register2_status, _, register2_body = request(
                port,
                "/api/v1/user/auth/register",
                json_headers,
                "POST",
                json.dumps({
                    "username": "smoke_user_b",
                    "phone": "13800138001",
                    "password": "smoke-password-b",
                    "smsCode": code2,
                    "deviceId": "smoke-terminal-b",
                }),
            )
            user_b_token = json.loads(register2_body)["data"]["accessToken"]
            ws_other = WsClient(port, user_b_token)
            assert register2_status == 201 and ws_other.status == 101
            recharge_key = str(uuid.uuid4())
            recharge_status, _, recharge_body = request(
                port,
                "/api/v1/user/wallet/recharges",
                {**auth_headers, **json_headers, "Idempotency-Key": recharge_key},
                "POST",
                json.dumps({"amountCent": 10000}),
            )
            recharge = json.loads(recharge_body)
            assert (
                recharge_status == 200
                and recharge["data"]["balanceAfterCent"] == 10000
            ), (recharge_status, recharge)

            flow_status, _, flow_body = request(
                port,
                "/api/v1/user/flows",
                {**auth_headers, **json_headers, "Idempotency-Key": str(uuid.uuid4())},
                "POST",
                json.dumps({"stationId": 1, "chargerType": 1, "preferredChargerId": None}),
            )
            flow = json.loads(flow_body)["data"]
            assert flow_status == 200 and flow["status"] == 20, (flow_status, flow)

            flow_events = ws_user.wait_for(
                lambda event: event.get("type") == "flow.updated"
                and event["data"].get("flowNo") == flow["flowNo"],
                5.0)
            assert flow_events, "flow.updated did not reach the creator"
            assert flow_events[0]["sequence"] > ready_events[0]["sequence"]

            confirm_status, _, confirm_body = request(
                port,
                f"/api/v1/user/flows/{flow['flowNo']}/quote-confirmations",
                {**auth_headers, **json_headers, "Idempotency-Key": str(uuid.uuid4())},
                "POST",
                json.dumps({
                    "quoteNo": flow["quote"]["quoteNo"],
                    "flowVersion": flow["version"],
                }),
            )
            confirmation = json.loads(confirm_body)["data"]
            assert confirm_status == 200 and confirmation["status"] == 30

            start_status, _, start_body = request(
                port,
                f"/api/v1/user/flows/{flow['flowNo']}/start",
                {**auth_headers, **json_headers, "Idempotency-Key": str(uuid.uuid4())},
                "POST",
                json.dumps({"flowVersion": confirmation["version"]}),
            )
            started = json.loads(start_body)["data"]
            assert start_status == 200 and started["status"] == 40

            progress_events = ws_user.wait_for(
                lambda event: event.get("type") == "charge.progress", 5.0)
            assert progress_events, "charge.progress did not reach the creator"
            progress_data = progress_events[0]["data"]
            assert progress_data["flowNo"] == flow["flowNo"]
            assert set(progress_data) >= {
                "flowNo", "orderNo", "status", "statusText", "durationSec",
                "energyMwh", "amountCent", "powerWatt", "simulatedSoc",
                "calculatedAt"}

            time.sleep(1.2)
            progress_status, _, progress_body = request(
                port, f"/api/v1/user/flows/{flow['flowNo']}/progress", auth_headers)
            progress = json.loads(progress_body)["data"]
            assert progress_status == 200 and progress["energyMwh"] > 0

            # The administrator connects before the settlement so it can
            # observe order.settled and the charger release.
            admin_login_status, _, admin_login_body = request(
                port,
                "/api/v1/admin/auth/login",
                json_headers,
                "POST",
                json.dumps({
                    "username": "admin",
                    "password": "123456",
                    "deviceId": "smoke-admin",
                }),
            )
            admin_token = json.loads(admin_login_body)["data"]["accessToken"]
            admin_headers = {"Authorization": f"Bearer {admin_token}"}
            ws_admin = WsClient(port, admin_token)
            assert admin_login_status == 200 and ws_admin.status == 101
            ws_admin.recv_events(1.0)

            dashboard_login_status, _, dashboard_login_body = request(
                port, "/api/v1/dashboard/auth/login", json_headers, "POST",
                json.dumps({
                    "username": "admin", "password": "123456",
                    "deviceId": "smoke-dashboard",
                }))
            dashboard_token = json.loads(dashboard_login_body)["data"]["accessToken"]
            dashboard_headers = {"Authorization": f"Bearer {dashboard_token}"}
            dashboard_status, _, dashboard_body = request(
                port, "/api/v1/dashboard/summary", dashboard_headers)
            dashboard_data = json.loads(dashboard_body)["data"]
            ws_dashboard = WsClient(port, dashboard_token)
            assert dashboard_login_status == 200 and dashboard_status == 200
            assert dashboard_data["schemaVersion"] == 1
            assert len(dashboard_data["hourlyHeatmap"]) == 168
            assert dashboard_snapshot.is_file()
            assert ws_dashboard.status == 101
            ws_dashboard.recv_events(1.0)

            settle_status, _, settle_body = request(
                port,
                f"/api/v1/user/flows/{flow['flowNo']}/settlements",
                {**auth_headers, **json_headers, "Idempotency-Key": str(uuid.uuid4())},
                "POST",
                json.dumps({"flowVersion": started["version"], "reasonCode": "USER_STOPPED"}),
            )
            receipt = json.loads(settle_body)["data"]
            assert (
                settle_status == 200
                and receipt["status"] == 60
                and receipt["energyMwh"] > 0
            ), (settle_status, receipt)

            settled_events = ws_user.wait_for(
                lambda event: event.get("type") == "order.settled", 5.0)
            assert settled_events, "order.settled did not reach the creator"
            settled_data = settled_events[0]["data"]
            assert settled_data["orderNo"] == receipt["orderNo"]
            assert "paidCent" not in settled_data
            assert "debtAddedCent" not in settled_data
            assert "balanceAfterCent" not in settled_data
            admin_settled = ws_admin.wait_for(
                lambda event: event.get("type") == "order.settled"
                and event["data"].get("orderNo") == receipt["orderNo"],
                5.0)
            assert admin_settled, "order.settled did not reach the administrator"
            admin_release = ws_admin.wait_for(
                lambda event: event.get("type") == "charger.statusChanged"
                and event["data"].get("fromStatus") == 1
                and event["data"].get("toStatus") == 0,
                5.0)
            assert admin_release, "charger release did not reach the administrator"

            # The administrator changes a free charger to faulty and back.
            reauth_status, _, _ = request(
                port, "/api/v1/admin/auth/reauth", {**admin_headers, **json_headers},
                "POST", json.dumps({"password": "123456"}))
            assert reauth_status == 200
            chargers_status, _, chargers_body = request(
                port, "/api/v1/admin/chargers?stationId=1&pageSize=100", admin_headers)
            chargers = json.loads(chargers_body)["data"]["items"]
            assert chargers_status == 200 and len(chargers) >= 2
            free_charger = next(
                charger for charger in chargers
                if charger["id"] != flow["chargerId"] and charger["status"] == 0)
            for target, reason in ((2, "smoke-fault"), (0, "smoke-repair")):
                change_status, _, _ = request(
                    port,
                    f"/api/v1/admin/chargers/{free_charger['id']}/status",
                    {**admin_headers, **json_headers, "Idempotency-Key": str(uuid.uuid4())},
                    "PUT",
                    json.dumps({
                        "targetStatus": target,
                        "reason": reason,
                        "version": free_charger["version"],
                    }),
                )
                assert change_status == 200, change_status
                free_charger["version"] += 1
            admin_charger_changes = ws_admin.wait_for(
                lambda event: event.get("type") == "charger.statusChanged"
                and event["data"].get("chargerId") == free_charger["id"],
                5.0)
            assert len(admin_charger_changes) >= 2
            assert any(event["data"]["toStatus"] == 2 for event in admin_charger_changes)
            assert any(event["data"]["toStatus"] == 0 for event in admin_charger_changes)

            ml_start_status, _, ml_start_body = request(
                port, "/api/v1/admin/ml-tasks",
                {**admin_headers, **json_headers,
                 "Idempotency-Key": str(uuid.uuid4())}, "POST",
                json.dumps({"taskType": "PREDICT", "horizonHours": [1, 6, 24]}))
            ml_task_no = json.loads(ml_start_body)["data"]["taskNo"]
            assert ml_start_status == 202
            ml_task = None
            # CI runners (Debug build, v8 seed with 90 days of history) can
            # push a PREDICT task past 20s; 60s keeps the margin without
            # masking a genuinely hung worker.
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                task_status, _, task_body = request(
                    port, f"/api/v1/admin/ml-tasks/{ml_task_no}", admin_headers)
                assert task_status == 200
                ml_task = json.loads(task_body)["data"]
                if ml_task["status"] not in ("PENDING", "RUNNING"):
                    break
                time.sleep(0.1)
            assert ml_task and ml_task["status"] == "SUCCEEDED", ml_task
            prediction_status, _, prediction_body = request(
                port, "/api/v1/admin/predictions?horizonHour=1", admin_headers)
            assert prediction_status == 200
            # UC-D-02 v8 种子含 5 个站点，无站点过滤时逐站返回一项预测。
            assert len(json.loads(prediction_body)["data"]["items"]) == 5

            # UC-A-09: the bootstrapped OWNER logs in with the one-shot key
            # while flagged for a password change, changes it, and the old key
            # stops working.
            owner_login_status, _, owner_login_body = request(
                port, "/api/v1/admin/auth/login", json_headers, "POST",
                json.dumps({
                    "username": "root_owner",
                    "password": bootstrap_key,
                    "deviceId": "smoke-owner",
                }))
            owner_login = json.loads(owner_login_body)["data"]
            owner_headers = {"Authorization": f"Bearer {owner_login['accessToken']}"}
            assert owner_login_status == 200 and owner_login["admin"]["mustChangePassword"]
            owner_new_password = "Ncs-Owner-New-2026"
            owner_stale_status, _, _ = request(
                port, "/api/v1/admin/me/password",
                {**owner_headers, **json_headers}, "PUT",
                json.dumps({
                    "currentPassword": "wrong-bootstrap-key",
                    "newPassword": owner_new_password,
                }))
            assert owner_stale_status == 401
            owner_change_status, _, owner_change_body = request(
                port, "/api/v1/admin/me/password",
                {**owner_headers, **json_headers}, "PUT",
                json.dumps({
                    "currentPassword": bootstrap_key,
                    "newPassword": owner_new_password,
                }))
            owner_changed = json.loads(owner_change_body)["data"]
            assert owner_change_status == 200 and not owner_changed["mustChangePassword"]
            owner_old_status, _, _ = request(
                port, "/api/v1/admin/auth/login", json_headers, "POST",
                json.dumps({
                    "username": "root_owner",
                    "password": bootstrap_key,
                    "deviceId": "smoke-owner-old",
                }))
            assert owner_old_status == 401
            owner_new_status, _, owner_new_body = request(
                port, "/api/v1/admin/auth/login", json_headers, "POST",
                json.dumps({
                    "username": "root_owner",
                    "password": owner_new_password,
                    "deviceId": "smoke-owner-new",
                }))
            owner_new_login = json.loads(owner_new_body)["data"]
            assert (
                owner_new_status == 200
                and not owner_new_login["admin"]["mustChangePassword"]
            )

            # Scope isolation: user B saw no user-A or admin business events
            # (its own session.ready and application pings are expected).
            ws_other.recv_events(1.0)
            other_events = [
                event for event in ws_other.received
                if event.get("type") not in ("ping", "session.ready")]
            assert not other_events, f"user B received foreign events: {other_events}"

            orders_status, _, orders_body = request(
                port, "/api/v1/user/orders", auth_headers)
            orders = json.loads(orders_body)["data"]
            assert orders_status == 200 and orders["total"] >= 1

            receipt_status, _, receipt_body = request(
                port, f"/api/v1/user/orders/{receipt['orderNo']}", auth_headers)
            assert (
                receipt_status == 200
                and json.loads(receipt_body)["data"]["amountCent"] == receipt["amountCent"]
            )

            logout_status, _, _ = request(
                port, "/api/v1/user/auth/logout", auth_headers, "POST")
            denied_status, _, _ = request(port, "/api/v1/user/me", auth_headers)
            assert logout_status == 200 and denied_status == 401

            ws_user.recv_events(3.0)
            revoked_events = ws_user.events_of_type("session.revoked")
            assert revoked_events, "session.revoked did not reach the revoked peer"
            assert revoked_events[0]["data"]["reason"] == "revoked"
            assert ws_user.closed and ws_user.close_code == 4001, (
                ws_user.closed, ws_user.close_code)

            # Oversized inbound frame: the server answers with a protocol-level
            # 1009 close frame instead of dropping the transport (13.1). Only
            # the frame header is sent: the server must reject at header
            # parse, and sending the full 70 KiB payload races the server's
            # close against the client's in-flight write. The admin token
            # reuses the admin session whose assertions are done; this
            # replaces ws_admin (closed 1001) without disturbing the user-B
            # heartbeat checks below.
            ws_oversized = WsClient(port, admin_token)
            assert ws_oversized.status == 101
            ws_oversized.recv_events(2.0)
            ws_oversized.send_oversized_header(70 * 1024)
            ws_oversized.recv_events(3.0)
            assert ws_oversized.closed and ws_oversized.close_code == 1009, (
                ws_oversized.closed, ws_oversized.close_code)
            ws_oversized.close()

            # Heartbeat: user B's idle connection receives the application
            # ping, answers with pong, and stays connected.
            ping_events = ws_other.wait_for(
                lambda event: event.get("type") == "ping", 40.0)
            assert ping_events, "server heartbeat ping did not arrive"
            assert not ws_other.closed, "the ponging connection was closed"
            ws_other.close()
            ws_admin.close()
            dashboard_refresh = ws_dashboard.wait_for(
                lambda event: event.get("type") == "dashboard.refresh", 35.0)
            assert dashboard_refresh, "dashboard.refresh did not reach Dashboard WebSocket"
            dashboard_logout, _, _ = request(
                port, "/api/v1/dashboard/auth/logout", dashboard_headers, "POST")
            dashboard_logout_retry, _, _ = request(
                port, "/api/v1/dashboard/auth/logout", dashboard_headers, "POST")
            assert dashboard_logout == 200 and dashboard_logout_retry == 200
            ws_dashboard.close()
        except BaseException:
            # Surface server-side diagnostics (errors, structured logs) before
            # the temporary directory is cleaned up.
            stop_server(process, 5)
            print("--- server stderr begin ---", file=sys.stderr)
            print(server_stderr.read_text(encoding="utf-8", errors="replace"),
                  file=sys.stderr)
            print("--- server stderr end ---", file=sys.stderr)
            try:
                log_files = sorted(logs.iterdir()) if logs.is_dir() else []
                newest = [path for path in log_files if path.name.endswith(".log")]
                for path in newest[-2:]:
                    print(f"--- {path.name} begin ---", file=sys.stderr)
                    print(path.read_text(encoding="utf-8", errors="replace")[-8000:],
                          file=sys.stderr)
                    print(f"--- {path.name} end ---", file=sys.stderr)
            except OSError:
                pass
            raise
        finally:
            forced_windows_stop = stop_server(process, 5)
        if process.returncode != 0 and not forced_windows_stop:
            print(server_stderr.read_text(encoding="utf-8", errors="replace"),
                  file=sys.stderr)
            return 1

        restart_port = free_port()
        restarted = subprocess.Popen(
            server_arguments(restart_port),
            stdout=open(restarted_stdout, "w", encoding="utf-8", errors="replace"),
            stderr=open(restarted_stderr, "w", encoding="utf-8", errors="replace"),
        )
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if restarted.poll() is not None:
                    break
                try:
                    status, _, _ = request(
                        restart_port, "/api/v1/system/health/ready")
                    if status == 200:
                        break
                except (ConnectionError, OSError, ssl.SSLError):
                    time.sleep(0.05)
            else:
                raise AssertionError("restarted server did not become ready")

            login_status, _, login_body = request(
                restart_port,
                "/api/v1/user/auth/login/password",
                json_headers,
                "POST",
                json.dumps({
                    "loginName": "smoke_user",
                    "password": "smoke-password",
                    "deviceId": "smoke-terminal-after-restart",
                }),
            )
            persisted_token = json.loads(login_body)["data"]["accessToken"]
            persisted_headers = {"Authorization": f"Bearer {persisted_token}"}
            wallet_status, _, wallet_body = request(
                restart_port, "/api/v1/user/wallet", persisted_headers)
            persisted_balance = json.loads(wallet_body)["data"]["balanceCent"]
            replay_status, _, replay_body = request(
                restart_port,
                "/api/v1/user/wallet/recharges",
                {**persisted_headers, **json_headers, "Idempotency-Key": recharge_key},
                "POST",
                json.dumps({"amountCent": 10000}),
            )
            orders_status, _, orders_body = request(
                restart_port, "/api/v1/user/orders", persisted_headers)
            persisted_orders = json.loads(orders_body)["data"]
            persisted_receipt_status, _, persisted_receipt_body = request(
                restart_port,
                f"/api/v1/user/orders/{receipt['orderNo']}",
                persisted_headers,
            )
            assert login_status == 200
            assert wallet_status == 200 and persisted_balance < 10000
            assert (
                replay_status == 200
                and json.loads(replay_body)["data"]["rechargeNo"]
                    == recharge["data"]["rechargeNo"]
            )
            wallet_after_replay_status, _, wallet_after_replay_body = request(
                restart_port, "/api/v1/user/wallet", persisted_headers)
            assert (
                wallet_after_replay_status == 200
                and json.loads(wallet_after_replay_body)["data"]["balanceCent"]
                    == persisted_balance
            )
            assert orders_status == 200 and persisted_orders["total"] >= 1
            assert (
                persisted_receipt_status == 200
                and json.loads(persisted_receipt_body)["data"]["orderNo"] == receipt["orderNo"]
            )
        except BaseException:
            stop_server(restarted, 5)
            print(restarted_stderr.read_text(encoding="utf-8", errors="replace"),
                  file=sys.stderr)
            raise
        finally:
            forced_windows_stop = stop_server(restarted, 5)
        if restarted.returncode != 0 and not forced_windows_stop:
            print(restarted_stderr.read_text(encoding="utf-8", errors="replace"),
                  file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
