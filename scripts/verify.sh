#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

echo "== C++ core tests"
./build/tests/teleprompter_core_tests

echo "== sample timing"
./build/gui/teleprompter --print-timing samples/test-script.txt | tail -1
./build/gui/teleprompter --print-timing --words samples/test-script.txt >/dev/null

echo "== Python prototype parity"
python3 -m unittest discover -s prototype/tests >/dev/null
python3 prototype/tools/build_pyz.py --force >/dev/null
PY_TOTAL=$(python3 prototype/teleprompter.pyz --print-timing prototype/samples/test-script.txt | tail -1)
CPP_TOTAL=$(./build/gui/teleprompter --print-timing samples/test-script.txt | tail -1)
echo "python: $PY_TOTAL"
echo "c++:    $CPP_TOTAL"
case "$PY_TOTAL" in
  *"0:23"*) ;;
  *) echo "python sample total changed" >&2; exit 1;;
esac
case "$CPP_TOTAL" in
  *"0:23"*) ;;
  *) echo "c++ sample total changed" >&2; exit 1;;
esac

if [ -n "${WAYLAND_DISPLAY:-}" ]; then
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
