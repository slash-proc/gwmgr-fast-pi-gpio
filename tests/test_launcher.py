import unittest
from unittest.mock import patch

from gnwmanager_fastpi.cli import _parser, _strip_remainder_separator, _with_gdb_backend
from gnwmanager_fastpi._native import pid_alive, tcp_ready


class LauncherArgumentTests(unittest.TestCase):
    def test_gnwmanager_remainder_accepts_separator(self):
        options = _parser().parse_args(["--", "serve", "--bind", "0.0.0.0"])
        self.assertEqual(
            _strip_remainder_separator(options.gnwmanager_args),
            ["serve", "--bind", "0.0.0.0"],
        )

    def test_backend_options_are_prepended(self):
        self.assertEqual(
            _with_gdb_backend(["serve", "--bind", "0.0.0.0"], "127.0.0.1", 3333),
            [
                "--gdb-port",
                "3333",
                "--gdb-host",
                "127.0.0.1",
                "--backend",
                "gdb",
                "serve",
                "--bind",
                "0.0.0.0",
            ],
        )

    def test_matching_backend_options_are_preserved(self):
        arguments = [
            "--backend",
            "gdb",
            "--gdb-host",
            "127.0.0.1",
            "--gdb-port",
            "3333",
            "serve",
        ]
        self.assertEqual(_with_gdb_backend(arguments, "127.0.0.1", 3333), arguments)

    def test_conflicting_backend_is_rejected(self):
        with self.assertRaises(ValueError):
            _with_gdb_backend(["--backend", "openocd"], "127.0.0.1", 3333)

    def test_native_fallback_has_expected_socket_contract(self):
        self.assertFalse(tcp_ready("127.0.0.1", 1, timeout=0.01))
        self.assertFalse(pid_alive(-1))

    @patch("gnwmanager_fastpi.cli.tcp_ready", return_value=True)
    def test_wait_for_port_uses_native_boundary(self, ready):
        from gnwmanager_fastpi.cli import _wait_for_port

        process = type("Process", (), {"poll": lambda self: None})()
        _wait_for_port(process, "127.0.0.1", 3333)
        ready.assert_called_once_with("127.0.0.1", 3333)


if __name__ == "__main__":
    unittest.main()
