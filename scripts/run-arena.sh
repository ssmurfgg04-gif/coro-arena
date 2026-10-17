#!/usr/bin/env bash
# run-arena.sh - one load level against one contestant.
#   run-arena.sh <tag> <server> <vus> <ramp_s> <hold_s> <root> <loadgen-bin>
#
# Server on cores 0-1, loadgen on cores 2-3: the "2-core server" shape the
# upcoming challenge targets. Same loadgen binary and flags as the tdsc-bench
# rig, so numbers are directly comparable with the sink/server history.
set -uo pipefail
TAG=$1 SERVER=$2 ROOT=$3 LOADGEN=$4
case "$TAG" in
20k)  VUS=20000  RAMP=60  HOLD=120 ;;
100k) VUS=100000 RAMP=90  HOLD=180 ;;
150k) VUS=150000 RAMP=120 HOLD=240 ;;
200k) VUS=200000 RAMP=150 HOLD=240 ;;
250k) VUS=250000 RAMP=150 HOLD=240 ;;
300k) VUS=300000 RAMP=180 HOLD=240 ;;
400k) VUS=400000 RAMP=210 HOLD=240 ;;
*) echo "unknown level $TAG"; exit 2 ;;
esac
PORT=3100
B=/tmp/bench
rm -rf "$B"; mkdir -p "$B"

log_rig() {
    {
        echo "rig: nproc=$(nproc) ulimit=$(ulimit -n)"
        echo "rig: conntrack_max=$(cat /proc/sys/net/netfilter/nf_conntrack_max 2>/dev/null || echo n/a)"
        echo "rig: conntrack_count=$(cat /proc/sys/net/netfilter/nf_conntrack_count 2>/dev/null || echo n/a)"
        echo "rig: sockstat=$(tr '\n' ' ' < /proc/net/sockstat)"
        echo "rig: mem=$(awk '/MemTotal|MemAvailable/{printf "%s=%dMB ", $1, $2/1024}' /proc/meminfo)"
    } >> "$B/server.log" 2>&1
}

case "$SERVER" in
sink)      BIN="$ROOT/build/sink";   ARGS="$PORT $ROOT/loadgen/feed.json $ROOT/loadgen/post.json 2" ;;
co_server) BIN="$ROOT/build/co_server"; ARGS="$PORT $ROOT/loadgen/feed.json $ROOT/loadgen/post.json 2 128 128 400000" ;;
fio_server) BIN="$ROOT/build/fio_server"; ARGS="$PORT $ROOT/loadgen/feed.json $ROOT/loadgen/post.json 2" ;;
muduo_server) BIN="$ROOT/build/muduo_server"; ARGS="$PORT $ROOT/loadgen/feed.json $ROOT/loadgen/post.json 2" ;;
*) echo "unknown server $SERVER"; exit 2 ;;
esac

echo "== starting $SERVER (taskset 0-1)"
sudo -n bash -c "ulimit -n 1048576; taskset -c 0-1 $BIN $ARGS" > "$B/server.log" 2>&1 &
log_rig

ok=0
for i in $(seq 60); do
    if curl -sf --max-time 2 "http://127.0.0.1:$PORT/health" > /dev/null; then ok=1; break; fi
    sleep 1
done
if [ "$ok" != 1 ]; then
    echo "server never became healthy"; tail -20 "$B/server.log"; exit 1
fi
echo "== $SERVER up"

echo "== bench: $TAG vus=$VUS ramp=${RAMP}s hold=${HOLD}s"
sudo -n bash -c "ulimit -n 1048576; taskset -c 2-3 $LOADGEN 127.0.0.1 $PORT \
  $VUS $RAMP $HOLD $ROOT/loadgen/tokens.json 2 16" > "$B/run.log" 2>&1
RC=$?
cat "$B/run.log"

sudo pkill -f "$BIN" 2>/dev/null
sleep 1

{
    echo "--- rig after ---"
    echo "rig: conntrack_count=$(cat /proc/sys/net/netfilter/nf_conntrack_count 2>/dev/null || echo n/a)"
    echo "rig: sockstat=$(tr '\n' ' ' < /proc/net/sockstat)"
    echo "rig: dmesg_conntrack_drops=$(sudo dmesg 2>/dev/null | grep -c 'table full' || true)"
} >> "$B/server.log" 2>&1

echo "== server stats tail (last 5)"
grep -a "stats:" "$B/server.log" 2>/dev/null | tail -5 || true

RESULT=$(grep -o 'RESULT: [A-Z]*' "$B/run.log" | tail -1 || true)
JSON=$(grep -o 'STATS_JSON: .*' "$B/run.log" | tail -1 | cut -d' ' -f2- || true)
echo "$JSON" > "$B/stats.json"
echo "BENCH_RESULT: $TAG server=$SERVER $RESULT"
exit $RC
