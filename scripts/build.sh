#!/usr/bin/env bash
# Linux entry point for the build tooling: a thin shim, so `scripts/build.sh`
# keeps working for muscle memory and manifest.json. It prefers python3 (the
# zipapp is stdlib-only, and this machine has a new enough interpreter), then
# uv as a fallback. The real work lives in teleprompter_builder.pyz, built by
# build_pyz.py if needed.
#
# Everything after this script's name is passed straight to the builder, e.g.
#   ./scripts/build.sh                     # Debug into build/ (the dev loop)
#   ./scripts/build.sh build --release     # Release into build_release/
#   ./scripts/build.sh build --no-gui      # core + tests only
#   ./scripts/build.sh build --dry-run
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

if command -v python3 >/dev/null 2>&1; then
    exec python3 build_pyz.py --run "$@"
fi
if command -v uv >/dev/null 2>&1; then
    exec uv run --no-project python build_pyz.py --run "$@"
fi

echo "error: neither python3 nor uv is on PATH" >&2
echo "       install Python 3.12+: https://www.python.org/ (or uv: https://docs.astral.sh/uv/)" >&2
exit 1
