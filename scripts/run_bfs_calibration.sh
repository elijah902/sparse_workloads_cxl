#!/usr/bin/env bash
# Regenerate the M2 BFS calibration table.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH="${BENCH:-$ROOT/build/bfs_bench}"
OUT="${OUT:-$ROOT/results/raw/m2_bfs.csv}"
PORT="${PORT:-9999}"

if [[ ! -x "$BENCH" ]]; then
    echo "benchmark not built: $BENCH" >&2
    echo "run ./scripts/build.sh first" >&2
    exit 1
fi

if ! (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
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

# Grid: backend comparison with the regular wavefront frontier.
run --backend dram --graph grid --n 2500 --kernel topdown
run --backend cxl-direct --graph grid --n 2500 --kernel topdown --cache-kb 64
run --backend cxl-block --graph grid --n 2500 --kernel topdown --block-kb 4 --cache-kb 64

# RMAT: top-down vs bottom-up vs hybrid.
rmat=(--graph rmat --n 4096 --edges 32768 --seed 7 --source 2048)
run --backend dram "${rmat[@]}" --kernel topdown
run --backend dram "${rmat[@]}" --kernel bottomup
run --backend dram "${rmat[@]}" --kernel hybrid
run --backend cxl-direct "${rmat[@]}" --kernel topdown --cache-kb 64
run --backend cxl-direct "${rmat[@]}" --kernel bottomup --cache-kb 64
run --backend cxl-direct "${rmat[@]}" --kernel hybrid --cache-kb 64
run --backend cxl-block "${rmat[@]}" --kernel hybrid --block-kb 4 --cache-kb 64

# Cache-size sensitivity for hybrid on RMAT.
run --backend cxl-direct "${rmat[@]}" --kernel hybrid --cache-kb 8
run --backend cxl-direct "${rmat[@]}" --kernel hybrid --cache-kb 32
run --backend cxl-direct "${rmat[@]}" --kernel hybrid --cache-kb 256

# Block-size sensitivity for hybrid on RMAT.
run --backend cxl-block "${rmat[@]}" --kernel hybrid --block-kb 1 --cache-kb 64
run --backend cxl-block "${rmat[@]}" --kernel hybrid --block-kb 16 --cache-kb 64

echo "wrote $OUT"
