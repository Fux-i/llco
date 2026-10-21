#!/usr/bin/env bash
# Runs a trace-emitting demo from the trace preset and validates its JSON output.
# Usage: scripts/trace.sh [demo]   (demo defaults to "trace"; e.g. "resume_on")
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build/_trace"
demo="${1:-trace}"
out="$build/traces/$demo.json"

cd "$root"
cmake --preset trace > /dev/null
cmake --build --preset trace --target "$demo" > /dev/null
mkdir -p "$build/traces"

"$build/bin/$demo" "$out"
python3 -m json.tool "$out" > /dev/null

echo
echo "trace : $out"
echo "open  : https://ui.perfetto.dev, drag the file in"
echo "note  : the demo runs in microseconds, press W in Perfetto to zoom in"
