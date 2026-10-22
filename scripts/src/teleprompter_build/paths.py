"""Repository layout: the root, the build directory, the manifest.

Everything derived from the repository's own names lives here, so a rename
happens in one file. Linux-only for now, like the app.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any

from .errors import BuildError

CMAKE_FILE = "CMakeLists.txt"
MANIFEST_FILE = "manifest.json"
GUI_BINARY = "teleprompter"
CORE_TEST_BINARY = "teleprompter_core_tests"

# Debug builds into `build/` — the path the docs use for the dev loop, tests
# included. Release gets its own directory so the two can coexist; it matches
# the `build_*/` line in .gitignore.
_DEBUG_BUILD_DIR = "build"
_RELEASE_BUILD_DIR = "build_release"


def current_target() -> str:
    """The platform being targeted. Only ``linux`` is supported today."""
    if os.name == "nt":
        raise BuildError("Windows is not supported by this tool yet (Linux-only for now)")
    if sys.platform == "darwin":
        raise BuildError(
            "macOS is not supported: the teleprompter is a Linux/Wayland app"
        )
    return "linux"


def _default_bases() -> list[Path]:
    """Where to start looking for the checkout: the cwd, then the launcher.

    The launcher matters for the ``.pyz``: run as
    ``scripts/teleprompter_builder.pyz`` from an unrelated directory, its own
    location is what finds the root.
    """
    bases = [Path.cwd().resolve()]
    argv0 = sys.argv[0] if sys.argv and sys.argv[0] else ""
    if argv0:
        try:
            bases.append(Path(argv0).resolve().parent)
        except OSError:
            pass
    return bases


def find_repo_root(start: Path | None = None) -> Path:
    """Walk up to the directory holding both ``CMakeLists.txt`` and ``manifest.json``."""
    bases = [Path(start).resolve()] if start is not None else _default_bases()
    for base in bases:
        for parent in (base, *base.parents):
            if (parent / CMAKE_FILE).is_file() and (parent / MANIFEST_FILE).is_file():
                return parent
    raise BuildError(
        f"not inside a Teleprompter checkout: no directory with both {CMAKE_FILE} "
        f"and {MANIFEST_FILE} above {', '.join(str(b) for b in bases)}"
    )


def build_dir(root: Path, *, debug: bool) -> Path:
    """Build directory for one type: ``build`` (Debug) or ``build_release``."""
    return root / (_DEBUG_BUILD_DIR if debug else _RELEASE_BUILD_DIR)


def binary_path(build_dir: Path) -> Path:
    """The GUI executable a build directory is expected to contain."""
    return build_dir / "gui" / GUI_BINARY


def test_binary_path(build_dir: Path) -> Path:
    """The core test executable, which only a Debug build contains."""
    return build_dir / "tests" / CORE_TEST_BINARY


def display(root: Path, path: Path) -> str:
    """*path* relative to *root* when possible, for shorter console output."""
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def load_manifest(root: Path) -> dict[str, Any]:
    """Parse ``manifest.json`` — the single source of truth for the version."""
    try:
        return json.loads((root / MANIFEST_FILE).read_text(encoding="utf-8"))
    except OSError as exc:
        raise BuildError(f"cannot read {MANIFEST_FILE}: {exc}") from exc
    except ValueError as exc:
        raise BuildError(f"{MANIFEST_FILE} is not valid JSON: {exc}") from exc


def version(root: Path) -> str:
    """The project's version string, straight from ``manifest.json``."""
    return str(load_manifest(root).get("version", "unknown"))
