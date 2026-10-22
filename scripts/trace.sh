#!/usr/bin/env bash
# Runs a trace-emitting demo from the trace preset and validates its JSON output.
# Usage: scripts/trace.sh [demo]   (demo defaults to "trace")
# The demo must call trace::flush(argv[1]); tests and other programs write no trace.
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
[[ -f "$out" ]] || {
    echo "error: $demo wrote no trace to $out; a demo must call trace::flush(argv[1])" >&2
    exit 1
}
python3 -m json.tool "$out" > /dev/null

echo
echo "trace : $out"
echo "open  : https://ui.perfetto.dev, drag the file in"
echo "note  : the demo runs in microseconds, press W in Perfetto to zoom in"
