#!/bin/bash
# Usage: ./run_single.sh [executor_service] [extra compose args...]
# where executor_service is one of: single_threaded,
#                                   multi_threaded,
#                                   events_cbg,
#                                   events,
#                                   callback_isolated
# Defaults to single_threaded. Runs one executor end-to-end and exits;
# plotting is handled separately by ./run_all.sh.
#
# Launch args can be appended by overriding the command, e.g.
#   docker compose -f docker/docker-compose.yaml run --rm events_cbg \
#       ros2 launch executor_comparison executor_bench.launch.py \
#           executor:=events_cbg num_subscriptions:=100
set -euo pipefail

EXECUTOR_SERVICE="${1:-single_threaded}"
shift || true

# Ensure the host-side data dir exists so the mount works cleanly.
mkdir -p data/results

COMPOSE=(docker compose -f docker/docker-compose.yaml)

"${COMPOSE[@]}" up \
    --remove-orphans --build --abort-on-container-exit "${EXECUTOR_SERVICE}" "$@"
