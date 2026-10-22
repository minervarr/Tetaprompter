#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

echo "== build (Debug)"
./scripts/build.sh build --debug

echo "== C++ core tests"
ctest --test-dir build --output-on-failure

echo "== build tool tests"
./scripts/test_builder.sh

CPP_TOTAL=""
if [ -x build/gui/teleprompter ]; then
    echo "== sample timing"
    ./build/gui/teleprompter --print-timing samples/test-script.txt | tail -1
    ./build/gui/teleprompter --print-timing --words samples/test-script.txt >/dev/null
    CPP_TOTAL=$(./build/gui/teleprompter --print-timing samples/test-script.txt | tail -1)
else
    echo "== sample timing skipped (no GUI binary)"
fi

echo "== Python prototype parity"
python3 -m unittest discover -s prototype/tests >/dev/null
python3 prototype/tools/build_pyz.py --force >/dev/null
PY_TOTAL=$(python3 prototype/teleprompter.pyz --print-timing prototype/samples/test-script.txt | tail -1)
echo "python: $PY_TOTAL"
case "$PY_TOTAL" in
  *"0:23"*) ;;
  *) echo "python sample total changed" >&2; exit 1;;
esac
if [ -n "$CPP_TOTAL" ]; then
    echo "c++:    $CPP_TOTAL"
    case "$CPP_TOTAL" in
      *"0:23"*) ;;
      *) echo "c++ sample total changed" >&2; exit 1;;
    esac
fi

if [ -x build/gui/teleprompter ] && [ -n "${WAYLAND_DISPLAY:-}" ]; then
    echo "== Vulkan capture"
    ./build/gui/teleprompter --seek 8 --capture /tmp/teleprompter-verify.ppm \
        samples/test-script.txt >/dev/null 2>&1
    test -s /tmp/teleprompter-verify.ppm
    ./build/gui/teleprompter --font opendyslexic --seek 8 \
        --capture /tmp/teleprompter-verify-od.ppm samples/test-script.txt >/dev/null 2>&1
    test -s /tmp/teleprompter-verify-od.ppm
    echo "captures: /tmp/teleprompter-verify.ppm /tmp/teleprompter-verify-od.ppm"
fi

echo "== done"
