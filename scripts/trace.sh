#!/usr/bin/env bash
# Builds one example in the trace preset, runs it in its own directory, and validates the
# trace.json it writes. Run it by hand; no test and no CI invokes this script.
# Usage: scripts/trace.sh [demo]   (demo defaults to "run")
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
demo="${1:-run}"
build="$root/build/_trace"
out="$build/traces/$demo"

cd "$root"
cmake --preset trace > /dev/null
cmake --build --preset trace --target "$demo" > /dev/null
mkdir -p "$out"

if [[ "$demo" == echo_server ]]; then
    # It runs until Ctrl-C, so poke it once and then ask it to shut down cleanly.
    ( cd "$out" && exec "$build/bin/$demo" ) &
    server=$!
    sleep 1
    printf 'hello\n' | nc -N 127.0.0.1 7000 > /dev/null
    kill -INT "$server"
    wait "$server"
else
    ( cd "$out" && exec "$build/bin/$demo" )
fi

python3 -m json.tool "$out/trace.json" > /dev/null

echo
echo "trace : $out/trace.json"
echo "open  : https://ui.perfetto.dev, drag the file in"
echo "note  : the demo runs in microseconds, press W in Perfetto to zoom in"
