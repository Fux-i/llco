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
rm -f "$out/trace.json" # only this run's file may pass the check below

if [[ "$demo" == echo_server ]]; then
    # Interactive: the server stays up until Ctrl-C, so the trace covers the real session.
    # Ctrl-C reaches the whole process group; the script ignores it, the server shuts down.
    trap ':' INT TERM
    echo 'connect : printf "hello\n" | nc -N 127.0.0.1 7000'
    echo 'stop    : disconnect the client, then Ctrl-C'
    ( cd "$out" && exec "$build/bin/$demo" ) || true
else
    ( cd "$out" && exec "$build/bin/$demo" )
fi

python3 -m json.tool "$out/trace.json" > /dev/null

echo
echo "trace : $out/trace.json"
echo "open  : https://ui.perfetto.dev, drag the file in"
echo "note  : press W in Perfetto to zoom in"
