#!/usr/bin/env bash
# Regenerate the M1 stencil calibration table.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH="${BENCH:-$ROOT/build/stencil_bench}"
OUT="${OUT:-$ROOT/results/raw/m1_stencil.csv}"
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

run --backend dram --kernel row --n 256 --steps 4
run --backend cxl-direct --kernel row --n 256 --steps 4 --cache-kb 32
run --backend cxl-direct --kernel row --n 256 --steps 4 --cache-kb 512
run --backend cxl-block --kernel row --n 256 --steps 4 --block-kb 64 --cache-kb 64
run --backend cxl-block --kernel row --n 256 --steps 4 --block-kb 4 --cache-kb 32
run --backend dram --kernel naive --n 128 --steps 2
run --backend cxl-direct --kernel naive --n 128 --steps 2 --cache-kb 16

echo "wrote $OUT"
