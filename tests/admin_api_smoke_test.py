#!/usr/bin/env python3
"""Exercise the real admin executable against isolated HTTP and HTTPS servers."""
import http.client
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import time

from server_http_smoke_test import free_port, stop_server


def main():
    server, admin = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="ncs-admin-smoke-") as directory:
        root = Path(directory)
        key, cert = root / "key.pem", root / "cert.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
            "-keyout", str(key), "-out", str(cert)
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for tls in (False, True):
            port = free_port()
            # Avoid a developer's .env, CA or live database affecting the test.
            env = {k: v for k, v in os.environ.items() if not k.startswith("NCS_")}
            env["NCS_ALLOW_INSECURE_HTTP"] = "false" if tls else "true"
            mode = "https" if tls else "http"
            args = [server, "--environment", "development", "--listen-address", "127.0.0.1",
                    "--port", str(port), "--database-path", str(root / f"{mode}.db"),
                    "--log-directory", str(root / f"{mode}-server-logs"),
                    "--dashboard-snapshot", str(root / f"{mode}-dashboard.json")]
            if tls:
                args += ["--tls-certificate", str(cert), "--tls-private-key", str(key)]
            process = subprocess.Popen(args, env=env, cwd=root, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 15
                while True:
                    assert process.poll() is None, "isolated admin test server exited"
                    try:
                        connection = (http.client.HTTPSConnection("127.0.0.1", port, timeout=1,
                                      context=ssl.create_default_context(cafile=str(cert))) if tls
                                      else http.client.HTTPConnection("127.0.0.1", port, timeout=1))
                        connection.request("GET", "/api/v1/system/health/ready")
                        response = connection.getresponse()
                        ready = response.status == 200
                        response.read()
                        connection.close()
                        if ready:
                            break
                    except OSError:
                        pass
                    assert time.monotonic() < deadline, "isolated server readiness timed out"
                    time.sleep(0.05)
                config = root / f"{mode}.env"
                config.write_text(f"NCS_ENV=development\nNCS_SERVER_HOST=127.0.0.1\n"
                                  f"NCS_SERVER_PORT={port}\nNCS_ALLOW_INSECURE_HTTP={'false' if tls else 'true'}\n"
                                  f"NCS_LOG_DIR={root / (mode + '-admin-logs')}\n", encoding="utf-8")
                client_env = {**env, "QT_QPA_PLATFORM": "offscreen",
                              "NCS_ADMIN_TEST_USERNAME": "admin", "NCS_ADMIN_TEST_PASSWORD": "123456"}
                # Verify config-file wiring, not just environment defaults.
                client_env.pop("NCS_ALLOW_INSECURE_HTTP")
                if tls:
                    rejected = subprocess.run([admin, "--config", str(config), "--api-smoke-test"],
                                              env=client_env, cwd=root, capture_output=True, timeout=30)
                    assert rejected.returncode == 7, "untrusted HTTPS certificate was accepted"
                    client_env["NCS_TLS_CA_PATH"] = str(cert)
                result = subprocess.run([admin, "--config", str(config), "--api-smoke-test"],
                                        env=client_env, cwd=root, capture_output=True, timeout=30)
                assert result.returncode == 0, f"admin {mode} API probe failed ({result.returncode})"
            finally:
                stop_server(process, 8)
    print("Admin HTTP, HTTPS trust rejection and custom CA API probes passed")


if __name__ == "__main__":
    main()
