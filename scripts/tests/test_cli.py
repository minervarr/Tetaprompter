"""Tests for the command line: it must not touch a toolchain to parse.

Run:  ./scripts/test_builder.sh
"""

from __future__ import annotations

import contextlib
import io
import unittest
from unittest import mock

from teleprompter_build import cli, paths


class CliTests(unittest.TestCase):
    def run_cli(self, argv: list[str]) -> tuple[int, str, str]:
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_version_prints_and_exits_zero(self) -> None:
        code, out, _ = self.run_cli(["--version"])
        self.assertEqual(code, 0)
        self.assertIn("teleprompter-build", out)
        self.assertIn(paths.version(paths.find_repo_root()), out)

    def test_unknown_command_is_a_usage_error(self) -> None:
        # argparse itself exits 2 for a command outside the choices.
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as excinfo:
                cli.main(["frobnicate"])
        self.assertEqual(excinfo.exception.code, 2)

    def test_dry_run_build_runs_nothing(self) -> None:
        # --dry-run needs no toolchain and must still print the cmake lines.
        code, out, _ = self.run_cli(["build", "--debug", "--dry-run"])
        self.assertEqual(code, 0)
        self.assertIn("cmake", out)
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", out)
        self.assertIn("-B", out)

    def test_release_uses_build_release(self) -> None:
        code, out, _ = self.run_cli(["build", "--release", "--dry-run"])
        self.assertEqual(code, 0)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", out)
        self.assertIn("build_release", out)

    def test_extra_cmake_argument_must_be_d(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as excinfo:
                cli.main(["build", "--dry-run", "notadefine"])
        self.assertEqual(excinfo.exception.code, 2)

    def test_gui_flag_adds_the_cmake_option(self) -> None:
        # --gui wins without probing the machine, so this works on any toolchain.
        code, out, _ = self.run_cli(["build", "--debug", "--dry-run", "--gui"])
        self.assertEqual(code, 0)
        self.assertIn("-DTP_BUILD_GUI=ON", out)
        self.assertIn("gui:", out)

    def test_no_gui_flag_adds_the_cmake_option(self) -> None:
        code, out, _ = self.run_cli(["build", "--debug", "--dry-run", "--no-gui"])
        self.assertEqual(code, 0)
        self.assertIn("-DTP_BUILD_GUI=OFF", out)
        self.assertNotIn("-DTP_BUILD_GUI=ON", out)

    def test_menu_needs_a_terminal(self) -> None:
        with mock.patch("sys.stdin", io.StringIO()):
            code, _, err = self.run_cli(["menu"])
        self.assertEqual(code, 2)
        self.assertIn("needs a terminal", err)


if __name__ == "__main__":
    unittest.main()
