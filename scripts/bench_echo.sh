#!/usr/bin/env bash
# Echo benchmark driver: one row per server configuration, A/B alternated, three repeats.
#
# The box isolates CPUs 2-11 from the scheduler (isolcpus), so every process here is pinned:
# the server takes whole physical cores, the load generator always gets the same core.
set -eo pipefail

get() { printenv "$1" || echo "$2"; }

REPEATS=$(get BENCH_REPEATS 3)
RUN_SECONDS=$(get BENCH_SECONDS 10)
WARMUP=$(get BENCH_WARMUP 2)
CONNS_LIST=$(get BENCH_CONNS "1 64")
DEPTH=$(get BENCH_DEPTH 1)
SIZE=$(get BENCH_SIZE 4096)
HOST=127.0.0.1
PORT=$(get BENCH_PORT 7200)
OUT=$(get BENCH_OUT bench-results)
# Core map on the benchmark box: cores are (0,1) housekeeping, (2,3) (4,5) (6,7) (8,9) (10,11).
# The server gets whole cores 2 and 4, the load generator cores 8 and 10: no core is shared.
ENGINE_CPUS=$(get BENCH_ENGINE_CPUS 2,4)
CLIENT_CPUS=$(get BENCH_CLIENT_CPUS 8,10)
CLIENT_THREADS=$(get BENCH_CLIENT_THREADS 2)

ROOT=$(git rev-parse --show-toplevel)
BIN=$ROOT/build/bin

command -v taskset >/dev/null || { echo 'taskset missing (util-linux)' >&2; exit 1; }
[ -x "$BIN/echo_load" ] || { echo 'build the bench targets first: cmake --build --preset default' >&2; exit 1; }
[ -x "$BIN/epoll_echo" ] || { echo 'epoll_echo missing' >&2; exit 1; }
[ -x "$BIN/echo_bench" ] || { echo 'echo_bench missing' >&2; exit 1; }

mkdir -p "$OUT"
{
    echo "# date=$(date -Is)"
    echo "# kernel=$(uname -r) cpu=$(sed -n 's/^model name.*: //p' /proc/cpuinfo | head -1)"
    echo "# governor=$(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "# git=$(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet || echo +dirty)"
    echo "# engine_cpus=$ENGINE_CPUS client_cpus=$CLIENT_CPUS client_threads=$CLIENT_THREADS"
    echo "# tcp_no_metrics_save=$(sysctl -n net.ipv4.tcp_no_metrics_save 2>/dev/null || echo unknown)"
    echo 'row,run,conns,rps,p50_us,p99_us,p999_us,max_us,msgs,threads,depth,size,client_cpu_pct,server_cpu_pct,errors'
} > "$OUT/results.csv"

SERVER_PID=

# True when nothing is listening on the benchmark port; a stale server would silently answer
# the load generator and every row would measure the same process.
port_free() { ! nc -z "$HOST" "$PORT" 2>/dev/null; }

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
        kill -0 "$SERVER_PID" 2>/dev/null || { wait "$SERVER_PID" || true; return 0; }
        sleep 0.1
    done
    echo "$name ignored SIGINT; killing" >&2
    kill -9 "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

# A server left behind would answer the next run; kill whatever is still up when we bail out.
trap '[ -n "$SERVER_PID" ] && kill -9 "$SERVER_PID" 2>/dev/null' EXIT

# Server CPU over the whole load run, in percent of one core; /proc/<pid>/stat counts at 100 Hz.
cpu_ticks() { awk '{print $14 + $15}' /proc/"$SERVER_PID"/stat; }

run_row() {
    local name=$1 binary=$2 args=$3 cpus=$4 run=$5
    local level csv before after t0 t1 wall_ms server_pct threads
    start_server "$name" "$binary" "$args" "$cpus"
    for level in $CONNS_LIST; do
        # one connection cannot be split across clients, so that level runs single-threaded
        threads=$CLIENT_THREADS
        [ "$level" -lt "$threads" ] && threads=$level
        before=$(cpu_ticks)
        t0=$(date +%s%N)
        csv=$(taskset -c "$CLIENT_CPUS" "$BIN/echo_load" --host "$HOST" --port "$PORT" \
                  --conns "$level" --depth "$DEPTH" --size "$SIZE" --threads "$threads" \
                  --warmup "$WARMUP" --seconds "$RUN_SECONDS" --csv)
        t1=$(date +%s%N)
        after=$(cpu_ticks)
        wall_ms=$(( (t1 - t0) / 1000000 ))
        server_pct=$(awk -v b="$before" -v a="$after" -v w="$wall_ms" \
            'BEGIN { if (w <= 0) print "0.0"; else printf "%.1f", (a - b) / 100.0 / (w / 1000.0) * 100.0 }')
        echo "$name,$run,$level,$csv,$server_pct" >> "$OUT/results.csv"
        echo "$name run $run conns $level: $csv server_cpu=${server_pct}%"
    done
    stop_server "$name"
}

for run in $(seq 1 "$REPEATS"); do
    # A/B alternated on purpose: a drift in clock or cache hits every row the same way.
    run_row epoll "epoll_echo" "" "2" "$run"
    run_row llco1 "echo_bench" "--engines 1" "2" "$run"
    run_row llco2 "echo_bench" "--engines 2" "$ENGINE_CPUS" "$run"
done

echo "results: $OUT/results.csv"
