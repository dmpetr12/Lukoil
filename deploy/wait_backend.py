"""Wait for a usable backend snapshot before starting the HMI (Linux)."""
import json
import os
import socket
import sys
import tempfile
import time


def wait_backend(timeout=20):
    deadline = time.monotonic() + timeout
    address = os.path.join(tempfile.gettempdir(), "emergency_panel_backend")
    while time.monotonic() < deadline:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(1)
                client.connect(address)
                client.sendall(b'{"cmd":"getState"}')
                data = b""
                while len(data) < 1024 * 1024:
                    chunk = client.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                    try:
                        reply = json.loads(data)
                    except (ValueError, UnicodeDecodeError):
                        continue
                    if reply.get("ok") is True and reply.get("state", {}).get("logLevel"):
                        return True
                    break
        except (OSError, ValueError):
            pass
        time.sleep(0.2)
    return False


if __name__ == "__main__":
    if not wait_backend():
        sys.exit("Backend IPC did not become ready within 20 seconds")
