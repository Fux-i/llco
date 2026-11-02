#!/usr/bin/env bash
# Builds one example in the trace preset, runs it in its own directory, and validates the
# trace.json it writes. Run it by hand; no test and no CI invokes this script.
# Usage: scripts/trace.sh [-m|--manual] [demo]
#   echo_server runs a scripted three-client session by default; -m waits for your own clients.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manual=0
demo=""
for arg in "$@"; do
    case "$arg" in
    -m | --manual) manual=1 ;;
    -*) echo "unknown option: $arg" >&2; exit 2 ;;
    *) demo="$arg" ;;
    esac
done
demo="${demo:-run}"

if [[ "$manual" == 1 && "$demo" != echo_server ]]; then
    echo "error: -m/--manual only applies to echo_server" >&2
    exit 2
fi

build="$root/build/_trace"
out="$build/traces/$demo"

cd "$root"
cmake --preset trace > /dev/null
cmake --build --preset trace --target "$demo" > /dev/null
mkdir -p "$out"
rm -f "$out/trace.json" # only this run's file may pass the check below

if [[ "$demo" != echo_server ]]; then
    ( cd "$out" && exec "$build/bin/$demo" )
elif [[ "$manual" == 1 ]]; then
    # Ctrl-C reaches the whole process group; the script ignores it, the server shuts down.
    trap ':' INT TERM
    echo 'connect : printf "hello\n" | nc -N 127.0.0.1 7000'
    echo 'stop    : disconnect the client, then Ctrl-C'
    ( cd "$out" && exec "$build/bin/$demo" ) || true
else
    # Scripted session with real connections, so the trace covers every message.
    ( cd "$out" && exec "$build/bin/$demo" ) &
    server=$!
    trap 'kill -INT "$server" 2>/dev/null || true' EXIT
    python3 "$root/scripts/echo_clients.py"
    kill -INT "$server"
    wait "$server" || true
    trap - EXIT
fi

python3 -m json.tool "$out/trace.json" > /dev/null

echo
echo "trace : $out/trace.json"
echo "open  : https://ui.perfetto.dev, drag the file in"
echo "note  : press W in Perfetto to zoom in"
