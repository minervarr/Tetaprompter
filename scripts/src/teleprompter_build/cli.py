"""Command line: parse arguments, then hand off to exactly one workflow.

Exit codes match the rest of the project: ``0`` success, ``1`` a step failed,
``2`` the command line was wrong.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence

from . import builder, menu, paths, toolchain, ui
from .errors import BuildError, UsageError

PROG = "teleprompter-build"
COMMANDS = ("build", "menu")
MENU_COMMANDS = ("", "menu")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog=PROG,
        description=(
            "Build the teleprompter through cmake + ninja. With no arguments and "
            "a terminal, a menu asks for the build type and whether to build the "
            "Vulkan/Wayland window; with no terminal it builds Debug and enables "
            "the window when its prerequisites are present."
        ),
    )
    parser.add_argument(
        "command",
        nargs="?",
        default=None,
        choices=COMMANDS,
        help="build one variant (default) or show the menu",
    )
    parser.add_argument(
        "--debug",
        dest="build_type",
        action="store_const",
        const="Debug",
        help="Debug build into build/ (default; tests included)",
    )
    parser.add_argument(
        "--release",
        dest="build_type",
        action="store_const",
        const="Release",
        help="Release build into build_release/ (no tests)",
    )
    parser.add_argument(
        "--ninja",
        default="",
        metavar="PATH",
        help="path to ninja (default: $TELEPROMPTER_NINJA, then PATH)",
    )
    gui = parser.add_mutually_exclusive_group()
    gui.add_argument(
        "--gui",
        dest="gui",
        action="store_true",
        help="build the Vulkan/Wayland window even if the probe fails (needs the Vulkan SDK, slangc and Wayland/xkb)",
    )
    gui.add_argument(
        "--no-gui",
        dest="gui",
        action="store_false",
        help="build only the portable core and tests",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the cmake commands and run nothing (works without a toolchain)",
    )
    parser.add_argument(
        "--version",
        action="store_true",
        help="print the project version from manifest.json",
    )
    parser.set_defaults(build_type=None, gui=None)
    return parser


def _passthrough(parser: argparse.ArgumentParser, extra: Sequence[str]) -> list[str]:
    """Extra arguments become extra cmake ``-D`` cache entries."""
    rest = list(extra)
    if rest[:1] == ["--"]:
        rest = rest[1:]
    for arg in rest:
        if not arg.startswith("-D"):
            parser.error(
                f"unrecognised argument: {arg} "
                "(extra cmake arguments must be written as -D<VAR>=<value>)"
            )
    return rest


def _version_or_unknown() -> str:
    try:
        return paths.version(paths.find_repo_root())
    except BuildError:
        return "unknown (run inside the Teleprompter checkout)"


def _resolve_command(
    args: argparse.Namespace, *, reader=input
) -> tuple[str, str]:
    """Return ``(command, build_type)``, asking the menu when none was given."""
    command = args.command or ""
    build_type = args.build_type or "Debug"

    if command in MENU_COMMANDS:
        if command == "menu" and not sys.stdin.isatty():
            raise UsageError("`menu` needs a terminal: use `build`")
        if not sys.stdin.isatty():
            command = "build"
            ui.note("    no terminal: building Debug (see --help for the other modes)")
        else:
            build_type = menu.choose_build_type(reader=reader)
            command = "build"

    return command, build_type


def _resolve_gui(args: argparse.Namespace, *, reader=input) -> bool:
    """Whether this build includes the Vulkan/Wayland window.

    An explicit flag wins. Otherwise the builder probes for the window's build
    prerequisites: a machine without the Vulkan SDK can still build the core
    and its tests, while a machine with them should not need to know a flag
    exists.
    """
    if args.gui is not None:
        if args.gui:
            ui.note("    gui:   on (Vulkan/Wayland window; --no-gui skips it)")
        else:
            ui.note("    gui:   off (explicit --no-gui)")
        return bool(args.gui)

    available, reason = toolchain.gui_prerequisites()
    if sys.stdin.isatty() and (args.command or "") in MENU_COMMANDS:
        chosen = menu.confirm(
            "Build the Vulkan/Wayland window?",
            default=available,
            reader=reader,
        )
    else:
        chosen = available

    if chosen:
        ui.note("    gui:   on (Vulkan/Wayland window; --no-gui skips it)")
        if not available:
            ui.warn(f"GUI requested but {reason}; the configure step will fail")
    else:
        ui.note(f"    gui:   off ({reason}; --gui overrides)")
    return chosen


def run_command(args: argparse.Namespace, extra: Sequence[str]) -> int:
    """Execute one parsed command."""
    paths.current_target()  # Linux-only for now; raises on anything else.
    root = paths.find_repo_root()
    _command, build_type = _resolve_command(args)
    debug = build_type == "Debug"

    ui.step("[1/3] Checking toolchain...")
    tools = toolchain.discover(ninja=args.ninja or None, dry_run=args.dry_run)
    job_count = toolchain.jobs()

    gui = _resolve_gui(args)
    extra = [*extra, f"-DTP_BUILD_GUI={'ON' if gui else 'OFF'}"]

    build_path = paths.build_dir(root, debug=debug)
    binary = builder.build_variant(
        root,
        build_path,
        toolchain=tools,
        build_type=build_type,
        jobs=job_count,
        extra=extra,
        dry_run=args.dry_run,
    )
    if gui:
        ui.ok(f"\nDone: {paths.display(root, binary)}")
    elif debug:
        ui.ok("\nDone: core + tests (no GUI)")
    else:
        ui.ok("\nDone: core (no GUI, no tests in Release)")
    if debug:
        ui.note(
            f"  test: ctest --test-dir {paths.display(root, build_path)} "
            "--output-on-failure"
        )
    if not gui:
        ui.note(
            "  gui:  skipped (--gui builds it once the Vulkan/Wayland "
            "prerequisites are installed)"
        )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args, extra = parser.parse_known_args(None if argv is None else list(argv))
    try:
        passthrough = _passthrough(parser, extra)
        if args.version:
            print(f"{PROG} {_version_or_unknown()}")
            return 0
        return run_command(args, passthrough)
    except UsageError as exc:
        ui.fail(str(exc))
        return 2
    except BuildError as exc:
        ui.fail(str(exc))
        return 1
    except KeyboardInterrupt:
        ui.fail("interrupted")
        return 130
