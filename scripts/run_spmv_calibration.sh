#!/usr/bin/env bash
# Regenerate the M3 SpMV calibration table.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH="${BENCH:-$ROOT/build/spmv_bench}"
OUT="${OUT:-$ROOT/results/raw/m3_spmv.csv}"
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

# Laplacian (5-point grid, side 64 -> 4096x4096, nnz=20224), backend sweep.
laplacian=(--matrix laplacian --n 64 --format csr)
run --backend dram "${laplacian[@]}"
run --backend cxl-direct "${laplacian[@]}" --cache-kb 64
run --backend cxl-block "${laplacian[@]}" --block-kb 4 --cache-kb 64
run --backend cxl-direct --matrix laplacian --n 64 --format sell --slice-height 4 --sigma 8 --cache-kb 64
run --backend cxl-direct --matrix laplacian --n 64 --format bcsr --block-rows 2 --block-cols 2 --cache-kb 64

# RMAT (n=4096, nnz=65536, seed 7): CSR vs SELL vs BCSR, and BCSR block sizes.
rmat=(--matrix rmat --n 4096 --nnz 65536 --seed 7)
run --backend dram "${rmat[@]}" --format csr
run --backend cxl-direct "${rmat[@]}" --format csr --cache-kb 64
run --backend cxl-direct "${rmat[@]}" --format sell --slice-height 4 --sigma 8 --cache-kb 64
run --backend cxl-direct "${rmat[@]}" --format bcsr --block-rows 2 --block-cols 2 --cache-kb 64
run --backend cxl-direct "${rmat[@]}" --format bcsr --block-rows 4 --block-cols 4 --cache-kb 64

# Random sparse matrix (social-like: 8000 x 8000, nnz=64000).
random=(--matrix random --n 8000 --cols 8000 --nnz 64000 --seed 21)
run --backend cxl-direct "${random[@]}" --format csr --cache-kb 64
run --backend cxl-direct "${random[@]}" --format sell --slice-height 4 --sigma 8 --cache-kb 64
run --backend cxl-direct "${random[@]}" --format bcsr --block-rows 4 --block-cols 4 --cache-kb 64

echo "wrote $OUT"
