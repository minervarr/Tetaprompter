#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -x build/gui/teleprompter ]; then
    ./scripts/build.sh
fi
exec ./build/gui/teleprompter "$@"
