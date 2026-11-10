#!/usr/bin/env bash
# Start the CXLMemSim server with a recorded configuration.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${SERVER:-$ROOT/third_party/CXLMemSim/build/cxlmemsim_server}"
PORT="${PORT:-9999}"
CAPACITY_MB="${CAPACITY_MB:-256}"
LATENCY_NS="${LATENCY_NS:-200}"
BACKING_FILE="${BACKING_FILE:-/tmp/opencode/cxlmemsim.backing}"
TOPOLOGY="${TOPOLOGY:-}"
VERBOSE="${VERBOSE:-2}"

if [[ ! -x "$SERVER" ]]; then
    echo "server not found: $SERVER" >&2
    echo "run ./scripts/build_cxlmemsim.sh first" >&2
    exit 1
fi

mkdir -p "$(dirname "$BACKING_FILE")"

args=(
    --comm-mode=tcp
    --port="$PORT"
    --capacity="$CAPACITY_MB"
    --default_latency="$LATENCY_NS"
    --backing-mode=file
    --backing-file="$BACKING_FILE"
    -v "$VERBOSE"
)
if [[ -n "$TOPOLOGY" ]]; then
    args+=(--topology="$TOPOLOGY")
fi

echo "starting cxlmemsim_server: port=$PORT capacity=${CAPACITY_MB}MB latency=${LATENCY_NS}ns"
exec "$SERVER" "${args[@]}"
