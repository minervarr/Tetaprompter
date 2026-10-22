"""Tests for the cmake command construction (no toolchain needed)."""

from __future__ import annotations

import unittest
from pathlib import Path

from teleprompter_build import builder, toolchain


class BuilderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tools = toolchain.Toolchain(
            cmake="/usr/bin/cmake",
            ninja="/opt/ninja/bin/ninja",
            compiler="/usr/bin/g++",
        )
        self.root = Path("/checkout")
        self.build = Path("/checkout/build")

    def test_configure_command(self) -> None:
        cmd = builder.configure_command(
            self.root,
            self.build,
            toolchain=self.tools,
            build_type="Debug",
            extra=("-DTP_BUILD_GUI=ON",),
        )
        self.assertEqual(cmd[:4], ["/usr/bin/cmake", "-S", "/checkout", "-B"])
        self.assertIn("-G", cmd)
        self.assertIn("Ninja", cmd)
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", cmd)
        self.assertIn("-DCMAKE_MAKE_PROGRAM=/opt/ninja/bin/ninja", cmd)
        self.assertIn("-DTP_BUILD_GUI=ON", cmd)

    def test_placeholder_ninja_is_not_pinned(self) -> None:
        # A dry run without ninja must still print a valid cmake line.
        tools = toolchain.Toolchain("cmake", "ninja", None)
        cmd = builder.configure_command(
            self.root, self.build, toolchain=tools, build_type="Release"
        )
        self.assertFalse(any(arg.startswith("-DCMAKE_MAKE_PROGRAM") for arg in cmd))

    def test_build_command(self) -> None:
        cmd = builder.build_command(self.tools, self.build, 8)
        self.assertEqual(
            cmd,
            ["/usr/bin/cmake", "--build", "/checkout/build", "-j", "8"],
        )

    def test_jobs_is_at_least_one(self) -> None:
        self.assertGreaterEqual(toolchain.jobs(), 1)


if __name__ == "__main__":
    unittest.main()
