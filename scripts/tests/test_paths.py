"""Tests for the repository layout: root discovery, build dirs, the manifest."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

from teleprompter_build import paths
from teleprompter_build.errors import BuildError


def make_checkout(base: Path) -> None:
    (base / "CMakeLists.txt").write_text("project(test)\n", encoding="utf-8")
    (base / "manifest.json").write_text(
        json.dumps({"version": "9.9.9"}), encoding="utf-8"
    )


class PathsTests(unittest.TestCase):
    def test_finds_the_root_and_walks_up(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            make_checkout(root)
            nested = root / "scripts" / "src"
            nested.mkdir(parents=True)
            self.assertEqual(paths.find_repo_root(root), root)
            self.assertEqual(paths.find_repo_root(nested), root)

    def test_refuses_a_directory_without_the_markers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(BuildError):
                paths.find_repo_root(Path(tmp))

    def test_build_directory_names(self) -> None:
        root = Path("/checkout")
        self.assertEqual(paths.build_dir(root, debug=True).name, "build")
        self.assertEqual(paths.build_dir(root, debug=False).name, "build_release")

    def test_binary_paths(self) -> None:
        build = Path("/checkout/build")
        self.assertEqual(paths.binary_path(build), build / "gui" / "teleprompter")
        self.assertEqual(
            paths.test_binary_path(build),
            build / "tests" / "teleprompter_core_tests",
        )

    def test_version_comes_from_the_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_checkout(root)
            self.assertEqual(paths.version(root), "9.9.9")

    def test_invalid_manifest_is_a_build_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "manifest.json").write_text("{not json", encoding="utf-8")
            with self.assertRaises(BuildError):
                paths.version(root)

    def test_display_is_relative_to_the_root(self) -> None:
        root = Path("/checkout")
        self.assertEqual(paths.display(root, root / "build" / "gui"), "build/gui")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux-only tool")
    def test_current_target(self) -> None:
        self.assertEqual(paths.current_target(), "linux")


if __name__ == "__main__":
    unittest.main()
