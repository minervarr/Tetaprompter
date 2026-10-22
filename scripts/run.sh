#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

if [ -x build/gui/teleprompter ]; then
    exec ./build/gui/teleprompter "$@"
fi
if [ -x build_release/gui/teleprompter ]; then
    exec ./build_release/gui/teleprompter "$@"
fi

./scripts/build.sh build --debug
if [ ! -x build/gui/teleprompter ]; then
    echo "error: the build produced no GUI binary;" >&2
    echo "       install the Vulkan SDK + Wayland/xkb development files" >&2
    exit 1
fi
exec ./build/gui/teleprompter "$@"
