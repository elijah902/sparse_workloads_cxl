#!/usr/bin/env bash
# Regenerate the M4 k-NN calibration table.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH="${BENCH:-$ROOT/build/knn_bench}"
OUT="${OUT:-$ROOT/results/raw/m4_knn.csv}"
PORT="${PORT:-9999}"

if [[ ! -x "$BENCH" ]]; then
    echo "benchmark not built: $BENCH" >&2
    echo "run ./scripts/build.sh first" >&2
    exit 1
fi

if ! (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    echo "cxlmemsim_server not reachable on 127.0.0.1:$PORT" >&2
    echo "start it with ./scripts/run_server.sh" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
: > "$OUT"

first=1
run() {
    if [[ "$first" -eq 1 ]]; then
        "$BENCH" "$@" --port "$PORT" --csv >> "$OUT"
        first=0
    else
        "$BENCH" "$@" --port "$PORT" --csv --no-header >> "$OUT"
    fi
}

common=(--n 1024 --dim 8 --queries 4 --k 2)

# DRAM baselines.
run --backend dram "${common[@]}"

# Brute force over CXL with a small software cache.
run --backend cxl-direct "${common[@]}" --cache-kb 64
run --backend cxl-block "${common[@]}" --block-kb 4096 --cache-kb 64

# Tiled brute force: tile sizes that cross/align with cache lines.
run --backend cxl-direct "${common[@]}" --kernel tile --tile 32 --cache-kb 64
run --backend cxl-direct "${common[@]}" --kernel tile --tile 64 --cache-kb 64
run --backend cxl-direct "${common[@]}" --kernel tile --tile 256 --cache-kb 64

# Block mode with tiles that fit one block.
run --backend cxl-block "${common[@]}" --kernel tile --tile 32 --block-kb 4096 --cache-kb 64
run --backend cxl-block "${common[@]}" --kernel tile --tile 64 --block-kb 4096 --cache-kb 64

echo "wrote $OUT"
