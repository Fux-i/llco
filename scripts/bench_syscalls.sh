#!/usr/bin/env bash
# Counts syscalls per echoed message for the three implementations.
#
# perf cannot attach to threads that are already running (it only follows the leader), so the
# server is started under `perf stat` instead of attached afterwards. Run `sudo -v` first: the
# script uses `sudo -n` and never stores a password.
set -eo pipefail

get() { printenv "$1" || echo "$2"; }
SECONDS_PER_RUN=$(get BENCH_SECONDS 5)
CONNS=$(get BENCH_CONNS 1)
PORT=$(get BENCH_PORT 7312)
OUT=$(get BENCH_OUT bench-results)
ROOT=$(git rev-parse --show-toplevel)
BIN=$ROOT/build/bin

sudo -n true 2>/dev/null || { echo 'run sudo -v first: perf needs root to read tracepoints' >&2; exit 1; }
mkdir -p "$OUT"

# syscall counts for one implementation; the load generator's message count is the denominator
count_one() {
    local bin=$1 args=$2 tag=$3 csv msgs total detail
    sudo -n perf stat -x, -e syscalls:sys_enter_read,syscalls:sys_enter_write,syscalls:sys_enter_recvfrom,syscalls:sys_enter_sendto,syscalls:sys_enter_io_uring_enter,syscalls:sys_enter_epoll_wait \
        -o "$OUT/$tag.perf" -- taskset -c 2 "$BIN/$bin" $args --port "$PORT" > /dev/null 2>&1 &
    sleep 1.2
    csv=$(taskset -c 4,6,8 "$BIN/echo_load" --port "$PORT" --conns "$CONNS" --depth 1 --size 4096 \
              --warmup 0 --seconds "$SECONDS_PER_RUN" --threads 1 --csv)
    sudo -n pkill -INT -f "build/bin/$bin" || true
    sleep 1
    msgs=$(echo "$csv" | cut -d, -f5)
    total=$(awk -F, '$1 ~ /^[0-9]+$/ {s+=$1} END{print s+0}' "$OUT/$tag.perf")
    detail=$(awk -F, '$1 ~ /^[0-9]+$/ {printf "%s=%s ", $3, $1}' "$OUT/$tag.perf" | sed 's/syscalls:sys_enter_//g')
    printf '%s,%s,%s,%.2f,%s\n' "$tag" "$msgs" "$total" \
        "$(awk -v t="$total" -v m="$msgs" 'BEGIN {print (m > 0 ? t / m : 0)}')" "$detail" >> "$OUT/syscalls.csv"
}

echo 'implementation,msgs,syscalls,per_msg,breakdown' > "$OUT/syscalls.csv"
count_one epoll_echo '' epoll
count_one raw_uring_echo '' raw
count_one echo_bench '--engines 1' llco
cat "$OUT/syscalls.csv"
