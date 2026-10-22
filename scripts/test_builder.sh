#!/bin/sh
# Run the build tool's own tests; no C++ toolchain or display needed.
set -eu
cd "$(dirname "$0")/.."
PYTHONPATH=scripts/src exec python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
