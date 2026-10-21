#!/usr/bin/env bash
# Builds the trace-enabled tree, runs the demo, and checks the generated trace.json.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build/_trace"

cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_CXX_COMPILER=clang++-22 \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLCO_TRACE=ON > /dev/null
cmake --build "$build" --target trace > /dev/null

cd "$build"
./examples/trace

python3 -m json.tool trace.json > /dev/null
grep -q '"name":"resume"' trace.json

echo
echo "trace : $build/trace.json"
echo "open  : https://ui.perfetto.dev, drag the file in"
echo "note  : the whole trace spans ~50us, press W in Perfetto to zoom in"
