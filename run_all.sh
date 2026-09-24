#!/bin/bash
# Usage:
#   ./run_all.sh                                # default matrix
#   ./run_all.sh "single_threaded events_cbg"   # legacy: override just the executor list
#   MATRIX_SUBS="1 10 100" \
#   MATRIX_THREADS="1 4" \
#   MATRIX_CBGS="mutually_exclusive reentrant" \
#   MATRIX_WORK_US="0 500" \
#   ./run_all.sh
#
# Runs the full cartesian product (executor x subscriptions x timers x threads
# x callback group x callback work x payload x rate) and then invokes the plot
# container to produce comparison plots from every resulting CSV in
# data/results.
#
# Any axis can be overridden via environment variable. Space-separated.
set -euo pipefail

# -----------------------------------------------------------------------------
# Matrix definition. Every axis is a space-separated list — edit inline or
# override from the environment. The cartesian product is executed in order.
#
# The default sweeps entity count, because that is where the wait-set
# executors and the events-queue executors diverge: a wait set is rebuilt and
# rescanned per wakeup, so its cost grows with the number of entities, while
# an events queue only ever touches the entity that actually became ready.
# -----------------------------------------------------------------------------
MATRIX_EXECUTORS="${MATRIX_EXECUTORS:-single_threaded multi_threaded events_cbg events callback_isolated}"
MATRIX_SUBS="${MATRIX_SUBS:-1 10 100}"
MATRIX_TIMERS="${MATRIX_TIMERS:-0}"
MATRIX_NODES="${MATRIX_NODES:-1}"
MATRIX_THREADS="${MATRIX_THREADS:-4}"
MATRIX_CBGS="${MATRIX_CBGS:-mutually_exclusive}"
MATRIX_WORK_US="${MATRIX_WORK_US:-0}"
MATRIX_PAYLOADS="${MATRIX_PAYLOADS:-1024}"
MATRIX_MODES="${MATRIX_MODES:-burst}"
# multi_rate only: per-topic rates are spread linearly across this range.
MATRIX_RATE_MIN="${MATRIX_RATE_MIN:-63.0}"
MATRIX_RATE_MAX="${MATRIX_RATE_MAX:-2000.0}"
# process = generator in its own process; composed = generator loaded into the
# container under test, which is the only way intra-process transport applies.
MATRIX_GENERATOR="${MATRIX_GENERATOR:-process}"
# Axis. Only meaningful with MATRIX_GENERATOR=composed: with the generator in
# its own process the messages go through the middleware regardless.
MATRIX_IPCS="${MATRIX_IPCS:-false}"
MATRIX_RATE="${MATRIX_RATE:-100.0}"
MATRIX_DURATION="${MATRIX_DURATION:-60.0}"
MATRIX_WARMUP="${MATRIX_WARMUP:-5.0}"
# Label axis for the final plot. Runs that differ only on an axis missing from
# here collapse onto one row, so widen it when sweeping extra axes.
MATRIX_PLOT_GROUPBY="${MATRIX_PLOT_GROUPBY:-subs_threads_cbg}"

# Middleware is not an axis of the comparison, but it does set the floor on
# dispatch latency, so it is recorded in every CSV and can be swept by hand.
export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_cyclonedds_cpp}"

# Legacy positional override: `./run_all.sh "single_threaded events_cbg"`
# replaces MATRIX_EXECUTORS so existing workflows don't break.
if [[ $# -ge 1 && -n "${1:-}" ]]; then
    MATRIX_EXECUTORS="$1"
fi

COMPOSE=(docker compose -f docker/docker-compose.yaml)

mkdir -p data/results

count_axis() {
    local n=0 _
    for _ in $1; do n=$((n + 1)); done
    echo "$n"
}

total=1
for axis in "$MATRIX_EXECUTORS" "$MATRIX_SUBS" "$MATRIX_TIMERS" "$MATRIX_NODES" \
            "$MATRIX_THREADS" "$MATRIX_CBGS" "$MATRIX_WORK_US" "$MATRIX_PAYLOADS" \
            "$MATRIX_MODES" "$MATRIX_IPCS"; do
    total=$((total * $(count_axis "$axis")))
done

echo "[run_all] matrix: $total combinations"
echo "           executors=[$MATRIX_EXECUTORS]"
echo "           subs=[$MATRIX_SUBS]  timers=[$MATRIX_TIMERS]  nodes=[$MATRIX_NODES]"
echo "           threads=[$MATRIX_THREADS]  callback_groups=[$MATRIX_CBGS]"
echo "           work_us=[$MATRIX_WORK_US]  payloads=[$MATRIX_PAYLOADS]  modes=[$MATRIX_MODES]"
echo "           generator=$MATRIX_GENERATOR  ipc=$MATRIX_IPCS  rate_spread=$MATRIX_RATE_MIN-$MATRIX_RATE_MAX Hz"
echo "           rate=$MATRIX_RATE  duration=$MATRIX_DURATION  rmw=$RMW_IMPLEMENTATION"
echo "           estimated wall time: ~$(python3 -c "print(round($total * ($MATRIX_DURATION + $MATRIX_WARMUP + 8) / 60.0, 1))") min"

i=0
for executor in $MATRIX_EXECUTORS; do
  for subs in $MATRIX_SUBS; do
    for timers in $MATRIX_TIMERS; do
      for nodes in $MATRIX_NODES; do
        for threads in $MATRIX_THREADS; do
          for cbg in $MATRIX_CBGS; do
            for work in $MATRIX_WORK_US; do
              for payload in $MATRIX_PAYLOADS; do
                for mode in $MATRIX_MODES; do
                 for ipc in $MATRIX_IPCS; do
                  i=$((i + 1))
                  echo
                  echo "=========================================================="
                  echo "[run_all] ($i/$total) executor=$executor subs=$subs timers=$timers" \
                       "nodes=$nodes threads=$threads cbg=$cbg work=${work}us" \
                       "payload=${payload}B mode=$mode ipc=$ipc"
                  echo "=========================================================="
                  "${COMPOSE[@]}" run --rm --build "$executor" \
                      ros2 launch executor_comparison executor_bench.launch.py \
                          executor:="$executor" \
                          num_subscriptions:="$subs" \
                          num_timers:="$timers" \
                          num_nodes:="$nodes" \
                          num_threads:="$threads" \
                          callback_group:="$cbg" \
                          callback_work_us:="$work" \
                          payload_bytes:="$payload" \
                          publish_mode:="$mode" \
                          rate_min_hz:="$MATRIX_RATE_MIN" \
                          rate_max_hz:="$MATRIX_RATE_MAX" \
                          generator_mode:="$MATRIX_GENERATOR" \
                          use_intra_process_comms:="$ipc" \
                          publish_rate_hz:="$MATRIX_RATE" \
                          warmup_s:="$MATRIX_WARMUP" \
                          duration_s:="$MATRIX_DURATION"
                 done
                done
              done
            done
          done
        done
      done
    done
  done
done

echo
echo "=========================================================="
echo "[run_all] generating comparison plots from data/results..."
echo "=========================================================="
"${COMPOSE[@]}" run --rm --build plot \
    --group-by "$MATRIX_PLOT_GROUPBY" \
    --title "All runs ($(date '+%Y-%m-%d %H:%M:%S'))"
