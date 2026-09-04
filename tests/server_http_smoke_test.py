#!/usr/bin/env python3

import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REQUEST_TIMEOUT_SECONDS = 10 if os.name == "nt" else 2


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def request(port: int, path: str):
    connection = http.client.HTTPConnection(
        "127.0.0.1", port, timeout=REQUEST_TIMEOUT_SECONDS)
    connection.request("GET", path)
    response = connection.getresponse()
    result = (
        response.status,
        {key.lower(): value for key, value in response.getheaders()},
        response.read(),
    )
    connection.close()
    return result


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


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: server_http_smoke_test.py <ncs_server> <ncs_user>", file=sys.stderr)
        return 2

    server = Path(sys.argv[1]).resolve()
    user = Path(sys.argv[2]).resolve()
    if not server.is_file() or not user.is_file():
        print("server or user executable is missing", file=sys.stderr)
        return 2

    # The internal ML client must use the same loopback-only transport without
    # trying to read a CA file that is intentionally absent in HTTP mode.
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "ml"))
    from worker import Client

    assert Client("http://127.0.0.1:1", "x" * 43, "/missing-ca").context is None
    try:
        Client("http://192.0.2.10:1", "x" * 43, "/missing-ca")
        raise AssertionError("non-loopback ML HTTP transport was accepted")
    except ValueError:
        pass

    with tempfile.TemporaryDirectory(prefix="ncs-http-smoke-") as temporary:
        root = Path(temporary)
        logs = root / "server-logs"
        database = root / "charge-platform.db"
        dashboard = root / "dashboard.json"
        port = free_port()
        environment = os.environ.copy()
        environment.pop("NCS_ENV_FILE", None)
        environment["NCS_ALLOW_INSECURE_HTTP"] = "true"
        process = subprocess.Popen(
            [
                str(server),
                "--environment",
                "development",
                "--listen-address",
                "127.0.0.1",
                "--port",
                str(port),
                "--log-directory",
                str(logs),
                "--database-path",
                str(database),
                "--dashboard-snapshot",
                str(dashboard),
            ],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            live = None
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                try:
                    live = request(port, "/api/v1/system/health/live")
                    break
                except (ConnectionError, OSError):
                    time.sleep(0.05)
            if live is None:
                stderr = process.stderr.read() if process.poll() is not None else ""
                raise AssertionError(f"HTTP server did not become live: {stderr}")

            status, headers, body = live
            assert status == 200 and json.loads(body) == {"status": "UP"}
            assert "strict-transport-security" not in headers

            ready_status, _, ready_body = request(port, "/api/v1/system/health/ready")
            assert ready_status == 200 and json.loads(ready_body)["status"] == "UP"

            client_environment = environment.copy()
            client_environment.update(
                {
                    "QT_QPA_PLATFORM": "offscreen",
                    "NCS_ENV": "development",
                    "NCS_SERVER_HOST": "127.0.0.1",
                    "NCS_SERVER_PORT": str(port),
                    "NCS_ALLOW_INSECURE_HTTP": "true",
                    "NCS_LOG_DIR": str(root / "user-logs"),
                }
            )
            client = subprocess.run(
                [str(user), "--api-request-code", "13800138000"],
                env=client_environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=20,
                check=False,
            )
            assert client.returncode == 0, client.stderr
        finally:
            forced_windows_stop = stop_server(process, 8)

        if process.returncode != 0 and not forced_windows_stop:
            print(process.stderr.read(), file=sys.stderr)
            return 1
        log_text = "\n".join(path.read_text(encoding="utf-8") for path in logs.glob("*.log"))
        assert "INSECURE DEVELOPMENT HTTP ENABLED" in log_text
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
