#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build=${CCSCOOP_BUILD_DIR:-"$root/build/protocol"}

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build" --parallel 2
ctest --test-dir "$build" --output-on-failure
