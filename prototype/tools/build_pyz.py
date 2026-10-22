#!/usr/bin/env python3
"""Build ``teleprompter.pyz`` — the whole app as one runnable archive.

Usage:
    python3 tools/build_pyz.py [output.pyz] [--force] [--run [args...]]

Runs on a plain interpreter: only the standard library (``zipapp``), no
install step. ``--run`` builds the archive and then executes it with the same
interpreter, passing on the remaining arguments.

The archive holds ``teleprompter.py``, the built-in ``samples/`` scores, plus a
generated ``__main__.py``. ``script.txt`` and ``recordings/`` are *not* baked in:
the user's own scripts and takes live next to the ``.pyz`` (or wherever they are
opened from), so the archive stays a single portable file.

The app, ``samples/`` and the output live in the repository root; this script
lives in ``tools/`` and resolves them relative to its parent directory.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import zipapp
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MODULE = ROOT / "teleprompter.py"
SAMPLES = ROOT / "samples"
DEFAULT_OUTPUT = ROOT / "teleprompter.pyz"
INTERPRETER = "/usr/bin/env python3"

# Written by hand rather than passed to zipapp as ``main=``: that shortcut
# generates ``<module>.<fn>()`` and throws the return value away, so the archive
# would always exit 0.
ARCHIVE_MAIN = '''"""Entry point of the teleprompter archive."""
import sys

from teleprompter import main

if __name__ == "__main__":
    sys.exit(main())
'''


def source_files() -> list[Path]:
    """Every file that goes into the archive, plus this bootstrap."""
    return [Path(__file__).resolve(), MODULE, *sorted(SAMPLES.glob("*.txt"))]


def is_stale(output: Path) -> bool:
    """Whether *output* is missing or older than any source file."""
    if not output.is_file():
        return True
    newest = max(path.stat().st_mtime for path in source_files())
    return newest > output.stat().st_mtime


def build(output: Path = DEFAULT_OUTPUT) -> Path:
    """Zip the module into *output* and make it executable."""
    if not MODULE.is_file():
        raise SystemExit(f"error: module not found: {MODULE}")
    samples = sorted(SAMPLES.glob("*.txt"))
    if not samples:
        raise SystemExit(f"error: no sample scripts found in {SAMPLES}")
    with tempfile.TemporaryDirectory() as staging:
        staged = Path(staging)
        shutil.copy2(MODULE, staged / MODULE.name)
        (staged / "samples").mkdir()
        for sample in samples:
            shutil.copy2(sample, staged / "samples" / sample.name)
        (staged / "__main__.py").write_text(ARCHIVE_MAIN, encoding="utf-8")
        zipapp.create_archive(
            staged,
            target=output,
            interpreter=INTERPRETER,
            compressed=True,
        )
    output.chmod(output.stat().st_mode | 0o111)
    return output


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)

    run_requested = "--run" in args
    run_args: list[str] = []
    if run_requested:
        split = args.index("--run")
        run_args = args[split + 1 :]
        args = args[:split]

    force = "--force" in args
    args = [arg for arg in args if arg != "--force"]
    output = Path(args[0]).resolve() if args else DEFAULT_OUTPUT

    if force or is_stale(output):
        built = build(output)
        size = built.stat().st_size
        print(f"built {built} ({size / 1024:.0f} KiB)", flush=True)
    elif not run_requested:
        print(f"{output} is up to date (use --force to rebuild it)", flush=True)

    if not run_requested:
        return 0
    return subprocess.run(
        [sys.executable, str(output), *run_args], check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
