#!/usr/bin/env bash
# Echo benchmark driver: one row per server configuration, A/B alternated, three repeats.
#
# The box isolates CPUs 2-11 from the scheduler (isolcpus), so every process is pinned:
# cores (2,3) (4,5) (6,7) (8,9) (10,11) are whole physical cores, (0,1) is housekeeping.
# Latency rows give the server one core and the load generator three; saturation rows
# give the server one to three cores and keep the load generator on a fixed pair.
set -eo pipefail

get() { printenv "$1" || echo "$2"; }

REPEATS=$(get BENCH_REPEATS 3)
RUN_SECONDS=$(get BENCH_SECONDS 10)
WARMUP=$(get BENCH_WARMUP 2)
DEPTH=$(get BENCH_DEPTH 1)
SIZE=$(get BENCH_SIZE 4096)
HOST=127.0.0.1
PORT=$(get BENCH_PORT 7200)
OUT=$(get BENCH_OUT bench-results)
PERF=$(get BENCH_PERF 0) # unused: syscalls/msg is measured by scripts/bench_syscalls.sh

ROOT=$(git rev-parse --show-toplevel)
BIN=$ROOT/build/bin

# name|binary|server args|server cpus|client cpus|client threads|connections
#
# Server cpus and client cpus never share a physical core: the pairs are (0,1)(2,3)(4,5)
# (6,7)(8,9)(10,11), so a 3-engine server on 2,4,6 leaves 8,9,10,11,0,1 to the load
# generator. Both sides pin explicitly -- isolcpus leaves no load balancing, so unpinned
# threads all land on one core and the measurement stops meaning anything (ADR-0013).
ROWS=(
    'epoll-1|epoll_echo||2|4,6,8|1|1'
    'raw-1|raw_uring_echo||2|4,6,8|1|1'
    'llco1-1|echo_bench|--engines 1 --cpus 2|2|4,6,8|1|1'
    'epoll-64|epoll_echo||2|8,9,10,11,0,1|6|384'
    'raw-64|raw_uring_echo||2|8,9,10,11,0,1|6|384'
    'llco1-64|echo_bench|--engines 1 --cpus 2|2|8,9,10,11,0,1|6|384'
    'llco2-64|echo_bench|--engines 2 --cpus 2,4|2,4|8,9,10,11,0,1|6|384'
    'llco3-64|echo_bench|--engines 3 --cpus 2,4,6|2,4,6|8,9,10,11,0,1|6|384'
)

command -v taskset >/dev/null || { echo 'taskset missing (util-linux)' >&2; exit 1; }
for tool in echo_load echo_bench epoll_echo raw_uring_echo; do
    [ -x "$BIN/$tool" ] || { echo "$tool missing: cmake --build --preset default" >&2; exit 1; }
done

mkdir -p "$OUT"
{
    echo "# kernel=$(uname -r) cpu=$(sed -n 's/^model name.*: //p' /proc/cpuinfo | head -1)"
    echo "# governor=$(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor 2>/dev/null || echo unknown) epp=$(cat /sys/devices/system/cpu/cpu2/cpufreq/energy_performance_preference 2>/dev/null || echo unknown)"
    echo "# git=$(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet || echo +dirty)"
    echo "# depth=$DEPTH size=$SIZE warmup=$WARMUP seconds=$RUN_SECONDS repeats=$REPEATS perf=$PERF"
    echo "# tcp_no_metrics_save=$(sysctl -n net.ipv4.tcp_no_metrics_save 2>/dev/null || echo unknown) paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)"
    echo 'row,run,conns,rps,p50_us,p99_us,p999_us,max_us,msgs,threads,depth,size,client_cpu_pct,server_cpu_pct,syscalls_per_msg,errors'
} > "$OUT/results.csv"

SERVER_PID=
PERF_PID=

# True when nothing is listening on the benchmark port: a stale server would silently answer
# the load generator and every row would measure the same process.
port_free() { ! nc -z "$HOST" "$PORT" 2>/dev/null; }

# Server CPU over the whole load run, in percent of one core; /proc/<pid>/stat counts at 100 Hz.
cpu_ticks() { awk '{print $14 + $15}' /proc/"$SERVER_PID"/stat; }

cleanup() {
    [ -n "$PERF_PID" ] && kill "$PERF_PID" 2>/dev/null
    [ -n "$SERVER_PID" ] && kill -9 "$SERVER_PID" 2>/dev/null
    return 0
}
trap cleanup EXIT

start_server() {
    local name=$1 binary=$2 args=$3 cpus=$4
    if ! port_free; then
        echo "$PORT is already in use: kill the stale server before measuring" >&2
        exit 1
    fi
    taskset -c "$cpus" "$BIN/$binary" $args --port "$PORT" > "$OUT/$name.log" 2>&1 &
    SERVER_PID=$!
    local i
    for i in $(seq 1 100); do
        nc -z "$HOST" "$PORT" 2>/dev/null && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || { echo "$name died at startup:" >&2; cat "$OUT/$name.log" >&2; exit 1; }
        sleep 0.1
    done
    echo "$name never listened on $PORT" >&2
    exit 1
}

stop_server() {
    local name=$1 i
    kill -INT "$SERVER_PID" 2>/dev/null || true
    for i in $(seq 1 60); do
        kill -0 "$SERVER_PID" 2>/dev/null || { wait "$SERVER_PID" || true; SERVER_PID=; return 0; }
        sleep 0.1
    done
    echo "$name ignored SIGINT; killing" >&2
    kill -9 "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

# Counts the server's syscalls while the load runs, then divides by the measured messages.
start_perf() {
    local name=$1
    PERF_PID=
    [ "$PERF" = 1 ] || return 0
    perf stat -x, -p "$SERVER_PID" -e 'syscalls:sys_enter_*' \
        -o "$OUT/$name.perf" -- sleep "$((WARMUP + RUN_SECONDS + 2))" >/dev/null 2>&1 &
    PERF_PID=$!
}

perf_total() {
    local name=$1
    [ -f "$OUT/$name.perf" ] || { echo 0; return 0; }
    awk -F, '$1 ~ /^[0-9]+$/ { sum += $1 } END { print sum + 0 }' "$OUT/$name.perf"
}

run_row() {
    local spec=$1 run=$2
    local name binary args server_cpus client_cpus threads conns
    IFS='|' read -r name binary args server_cpus client_cpus threads conns <<< "$spec"
    local csv before after t0 t1 wall_ms server_pct syscalls per_msg
    start_server "$name" "$binary" "$args" "$server_cpus"
    start_perf "$name"
    before=$(cpu_ticks)
    t0=$(date +%s%N)
    csv=$(taskset -c "$client_cpus" "$BIN/echo_load" --host "$HOST" --port "$PORT" \
              --conns "$conns" --depth "$DEPTH" --size "$SIZE" --threads "$threads" \
              --cpus "$client_cpus" \
              --warmup "$WARMUP" --seconds "$RUN_SECONDS" --csv)
    t1=$(date +%s%N)
    after=$(cpu_ticks)
    [ -n "$PERF_PID" ] && { wait "$PERF_PID" 2>/dev/null || true; PERF_PID=; }
    wall_ms=$(( (t1 - t0) / 1000000 ))
    server_pct=$(awk -v b="$before" -v a="$after" -v w="$wall_ms" \
        'BEGIN { if (w <= 0) print "0.0"; else printf "%.1f", (a - b) / 100.0 / (w / 1000.0) * 100.0 }')
    syscalls=$(perf_total "$name")
    per_msg=$(awk -v s="$syscalls" -v m="$(echo "$csv" | cut -d, -f5)" \
        'BEGIN { if (m + 0 <= 0) print "0.00"; else printf "%.2f", s / m }')
    stop_server "$name"
    echo "$name,$run,$conns,$csv,$server_pct,$per_msg" >> "$OUT/results.csv"
    echo "$name run $run: $csv server_cpu=$server_pct% syscalls/msg=$per_msg"
}

for run in $(seq 1 "$REPEATS"); do
    # A/B alternated on purpose: a clock or cache drift hits every row the same way.
    for spec in "${ROWS[@]}"; do
        run_row "$spec" "$run"
    done
done

echo "results: $OUT/results.csv"
