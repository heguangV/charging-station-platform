#!/usr/bin/env python3
"""Test-environment helpers for the NCS load-test suite.

Owns the isolated test database, TLS certificates, server subprocess and the
delete-and-recreate database lifecycle required by SRS section 8 (test
environment uses an independent SQLite file; the test database is deleted and
recreated after the test run; no test account or fixed password is used for
production demos).

Server arguments mirror ``tests/server_smoke_test.py``; all paths point into a
private per-run temporary directory so development or demo data is never
touched (development guide section 6).
"""

from __future__ import annotations

import asyncio
import os
import signal
import socket
import ssl
import subprocess
import sys
import time
from pathlib import Path

from .virtual_client import HttpClient, HttpError, TransportError

SERVER_WORKER_THREADS = 2        # acceptance VM is 2-core (NFR-D-01)
SERVER_BLOCKING_WORKER_THREADS = 2
SERVER_BLOCKING_QUEUE_CAPACITY = 64  # default

READY_PATH = "/api/v1/system/health/ready"


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def generate_tls_certificate(root: Path) -> tuple[Path, Path]:
    certificate = root / "certificate.pem"
    private_key = root / "private-key.pem"
    subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-days", "1", "-subj", "/CN=127.0.0.1",
            "-addext", "subjectAltName=IP:127.0.0.1",
            "-keyout", str(private_key), "-out", str(certificate),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    os.chmod(private_key, 0o600)
    return certificate, private_key


def build_server_arguments(server_binary: Path, port: int, root: Path,
                           environment: str = "development") -> list[str]:
    certificate, private_key = generate_tls_certificate(root)
    return [
        str(server_binary),
        "--environment", environment,
        "--listen-address", "127.0.0.1",
        "--port", str(port),
        "--worker-threads", str(SERVER_WORKER_THREADS),
        "--blocking-worker-threads", str(SERVER_BLOCKING_WORKER_THREADS),
        "--blocking-queue-capacity", str(SERVER_BLOCKING_QUEUE_CAPACITY),
        "--log-directory", str(root / "logs"),
        "--database-path", str(root / "charge-platform.db"),
        "--dashboard-snapshot", str(root / "dashboard.json"),
        "--tls-certificate", str(certificate),
        "--tls-private-key", str(private_key),
        "--cors-allowed-origins", "https://dashboard.local",
        "--python-executable", sys.executable,
        "--ml-worker-script",
        str(Path(__file__).resolve().parent.parent.parent / "ml" / "worker.py"),
        "--ml-model-path", str(root / "models" / "load_rf.pkl"),
    ]


class ServerHandle:
    """A started ncs_server bound to an isolated temporary directory."""

    def __init__(self, process: subprocess.Popen, port: int, root: Path,
                 tls_context: ssl.SSLContext):
        self.process = process
        self.port = port
        self.root = root
        self.tls_context = tls_context
        self.database_path = root / "charge-platform.db"
        self.host = "127.0.0.1"

    def database_files(self) -> list[Path]:
        files = [self.database_path]
        for suffix in ("-wal", "-shm", "-journal"):
            candidate = Path(str(self.database_path) + suffix)
            if candidate.exists():
                files.append(candidate)
        return files

    def delete_database(self) -> None:
        """Remove the test database and its sidecar files (SRS section 8)."""
        for path in self.database_files():
            try:
                path.unlink()
            except FileNotFoundError:
                pass

    def client(self) -> HttpClient:
        return HttpClient(self.host, self.port, self.tls_context, timeout=20.0)


def start_server(server_binary: Path, port: int, root: Path,
                 tls_context: ssl.SSLContext) -> ServerHandle:
    arguments = build_server_arguments(server_binary, port, root)
    stdout_file = open(root / "server-stdout.log", "w", encoding="utf-8",
                       errors="replace")
    stderr_file = open(root / "server-stderr.log", "w", encoding="utf-8",
                       errors="replace")
    # Never pipe server output without draining it: a full pipe buffer would
    # block the request-handling thread and deadlock the server.
    process = subprocess.Popen(arguments, stdout=stdout_file,
                               stderr=stderr_file)
    handle = ServerHandle(process, port, root, tls_context)
    return handle


async def request_ready(handle: ServerHandle, timeout: float = 30.0) -> dict:
    """Async readiness poll: returns the /health/ready payload once UP.

    The poll begins immediately after the subprocess is spawned, while the
    server may not have bound its port yet, so every attempt must tolerate
    connection errors (HttpClient.request connects lazily and retries once).
    """
    client = HttpClient(handle.host, handle.port, handle.tls_context,
                        timeout=5.0)
    deadline = time.monotonic() + timeout
    last_error = "server did not become ready"
    while time.monotonic() < deadline:
        if handle.process.poll() is not None:
            raise RuntimeError(
                f"server exited with code {handle.process.returncode}: "
                f"{last_error}")
        try:
            response = await client.request("GET", READY_PATH)
            if response.status == 200:
                payload = response.json()
                if payload.get("status") == "UP" and all(
                        payload.get("checks", {}).values()):
                    await client.close()
                    return payload
        except (OSError, ssl.SSLError, TransportError, HttpError) as exc:
            last_error = f"ready poll failed: {exc}"
        await asyncio.sleep(0.25)
    await client.close()
    raise RuntimeError(last_error)


def stop_server(handle: ServerHandle, timeout: float = 10.0) -> None:
    """Stop the server with SIGTERM and report a non-zero exit if any."""
    if handle.process.poll() is None:
        handle.process.send_signal(signal.SIGTERM)
    try:
        handle.process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        handle.process.kill()
        handle.process.wait(timeout=5)
    if handle.process.returncode != 0:
        raise RuntimeError(
            f"ncs_server exited with code {handle.process.returncode}; "
            f"see {handle.root / 'server-stderr.log'}")
