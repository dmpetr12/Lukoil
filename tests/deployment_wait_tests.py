import importlib.util
from pathlib import Path
import unittest
from unittest.mock import MagicMock, patch

module_path = Path(__file__).resolve().parents[1] / "deploy" / "wait_backend.py"
spec = importlib.util.spec_from_file_location("wait_backend", module_path)
wait = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wait)


class BackendReadinessTests(unittest.TestCase):
    def setUp(self):
        # The transport is mocked: Windows Python need not expose Linux AF_UNIX.
        unix_family = patch.object(wait.socket, "AF_UNIX", 1, create=True)
        unix_family.start()
        self.addCleanup(unix_family.stop)

    def test_accepts_complete_snapshot_split_across_socket_reads(self):
        transport = MagicMock()
        transport.__enter__.return_value = transport
        transport.recv.side_effect = [b'{"ok":true,"state":', b'{"logLevel":"INFO"}}']
        with patch.object(wait.socket, "socket", return_value=transport):
            self.assertTrue(wait.wait_backend())

    def test_does_not_accept_reply_without_loaded_state(self):
        transport = MagicMock()
        transport.__enter__.return_value = transport
        transport.recv.return_value = b'{"ok":true,"state":{}}'
        with patch.object(wait.socket, "socket", return_value=transport), \
                patch.object(wait.time, "monotonic", side_effect=[0, 0, 2]), \
                patch.object(wait.time, "sleep"):
            self.assertFalse(wait.wait_backend(timeout=1))

    def test_returns_failure_when_backend_never_connects(self):
        transport = MagicMock()
        transport.__enter__.side_effect = OSError("not listening")
        with patch.object(wait.socket, "socket", return_value=transport), \
                patch.object(wait.time, "monotonic", side_effect=[0, 0, 2]), \
                patch.object(wait.time, "sleep"):
            self.assertFalse(wait.wait_backend(timeout=1))


if __name__ == "__main__":
    unittest.main()
