# Results

Status: M1/M2/M3/M4 calibration recorded
Last updated: 2026-11-09

Raw data: `results/raw/m1_stencil.csv`, `results/raw/m2_bfs.csv`,
`results/raw/m3_spmv.csv`, and `results/raw/m4_knn.csv` (regenerate with
`scripts/run_stencil_calibration.sh`, `scripts/run_bfs_calibration.sh`,
`scripts/run_spmv_calibration.sh`, and `scripts/run_knn_calibration.sh`).

## M1 — Stencil calibration

Platform: WSL2, Debian 13, GCC 14.2.0, CXLMemSim `a9d46ec`, server over TCP with
`--capacity=64 --default_latency=200`, file-backed pool.

Grid: `n = 256`, `steps = 4`, row-blocked kernel unless noted. `t_compute` is the
median DRAM reference (ADR 0002). `t_cxl_model` is the sum of server-returned
latencies. `t_model = t_compute + t_cxl_model`.

| Backend | Kernel | Block | Cache | `t_compute` ms | `t_cxl_model` ms | `t_model` ms | `t_cpu_run` ms | `t_wall` ms | Line fetches | Block fetches | Hits | Misses | Evictions |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| dram | row | — | — | 0.252 | 0.000 | 0.252 | 0.252 | 0.245 | 0 | 0 | 0 | 0 | 0 |
| cxl-direct | row | — | 32 KB | 0.181 | 7.496 | 7.677 | 1008.5 | 2878.0 | 16,384 | 0 | 0 | 16,384 | 15,872 |
| cxl-direct | row | — | 512 KB | 0.234 | 7.496 | 7.730 | 1112.8 | 3172.2 | 16,384 | 0 | 0 | 16,384 | 0 |
| cxl-block | row | 64 KB | 64 KB | 0.211 | 7.496 | 7.707 | 1028.7 | 2966.1 | 16,384 | 16 | 1,008 | 16 | 15 |
| cxl-block | row | 4 KB | 32 KB | 0.216 | 7.496 | 7.712 | 899.2 | 2635.5 | 16,384 | 256 | 768 | 256 | 248 |
| dram | naive | — | — | 0.674 | 0.000 | 0.674 | 0.674 | 0.665 | 0 | 0 | 0 | 0 | 0 |
| cxl-direct | naive (`n=128`) | — | 16 KB | 0.697 | 7.100 | 7.797 | 1145.7 | 3454.6 | 2,048 | 0 | 125,976 | 2,048 | 1,792 |

### Observations

1. **CXL latency dominates the modeled result.** The row kernel's `t_model` is
   about 30× the DRAM kernel's. Almost all of the gap is `t_cxl_model`
   (7.496 ms), not compute.
2. **Cache size alone does not help a streaming pass.** The row kernel reads
   each line once per step. Increasing the cache from 32 KB to 512 KB removes
   evictions (15,872 → 0) but leaves hits at zero.
3. **Block mode does not reduce modeled latency.** With the 64-byte server cap,
   a 64 KB block is 1,024 line requests. Block fetches fall (256 → 16), but line
   fetches and `t_cxl_model` are unchanged. Block mode models prefetch
   granularity, not link width.
4. **Read caching helps the naive kernel, but uncoalesced writes dominate.** The
   naive kernel fetches only 2,048 lines and records 125,976 hits, but it issues
   32,768 write requests (one per cell per step). Coalescing writes is more
   important than read caching for this kernel.
5. **Transport cost is real and must be separated.** `t_cpu_run` is about 1 s and
   `t_wall` about 3 s, versus 7.5 ms of modeled CXL time. ADR 0002's compute
   reference keeps the transport out of `t_model`.
6. **`t_compute` is noisy at this size.** Values range from 0.18 to 0.25 ms for
   the same kernel. The final experiments will use larger grids and 3–5
   repetitions with median/IQR.

### Correctness

`ctest` passes:

- `cxl_client_smoke`: two 64-byte lines round-trip; read latency 210 ns, write
  latency 260 ns at a 200 ns base.
- `tiered_array_test`: DRAM and both CXL backends; exact cache/line counters.
- `stencil_test`: DRAM, direct, and block kernels match the reference; exact
  counter checks for `n = 32`, `steps = 2`.

## M2 — BFS

Raw data: `results/raw/m2_bfs.csv` (regenerate with
`scripts/run_bfs_calibration.sh`). Platform and server settings are the same as
M1: 200 ns base latency, TCP, file-backed pool. Full walkthrough:
[docs/algorithms/bfs.md](../algorithms/bfs.md).

Graph arrays (`row_ptr`, `col_idx`) live in CXL; frontier, bitmap, and distances
stay in DRAM. `t_compute` is the median DRAM reference (ADR 0002).

### Grid (`n = 2,500`, `m = 9,800`, top-down)

| Backend | Block | Cache | `t_model` ms | Line fetches | Block fetches | Hits | Misses |
|---|---|---|---|---|---|---|---|
| dram | — | — | 0.075 | 0 | 0 | 0 | 0 |
| cxl-direct | — | 64 KB | 0.248 | 770 | 0 | 14,030 | 770 |
| cxl-block | 4 KB | 64 KB | 0.254 | 770 | 13 | 14,787 | 13 |

The whole 49 KB graph fits in the 64 KB cache, so direct and block fetch the
same 770 lines. Block mode is neutral here because the wavefront eventually uses
every line.

### RMAT (`n = 4,096`, `m = 53,635`, source 2048)

| Backend | Kernel | `t_model` ms | Edges | Vertex scans | Line fetches | Hits | Misses |
|---|---|---|---|---|---|---|---|
| dram | top-down | 0.346 | 53,629 | 0 | 0 | 0 | 0 |
| dram | bottom-up | 0.491 | 48,277 | 20,480 | 0 | 0 | 0 |
| dram | hybrid | 0.148 | 3,576 | 12,288 | 0 | 0 | 0 |
| cxl-direct | top-down | 1.198 | 53,629 | 0 | 4,302 | 55,359 | 4,302 |
| cxl-direct | bottom-up | 1.336 | 48,277 | 20,480 | 4,407 | 66,312 | 4,407 |
| cxl-direct | hybrid | 0.425 | 3,576 | 12,288 | 1,373 | 14,303 | 1,373 |

### Cache-size sensitivity (RMAT hybrid, direct)

| Cache | `t_model` ms | Line fetches | Evictions |
|---|---|---|---|
| 8 KB | 0.549 | 2,017 | 1,761 |
| 32 KB | 0.429 | 1,453 | 685 |
| 64 KB | 0.425 | 1,373 | 93 |
| 256 KB | 0.416 | 1,371 | 0 |

### Block-size sensitivity (RMAT hybrid)

| Block | `t_model` ms | Line fetches | Block fetches |
|---|---|---|---|
| direct (64 B) | 0.425 | 1,373 | 0 |
| 1 KB | 0.966 | 4,003 | 252 |
| 4 KB | 1.299 | 5,555 | 89 |
| 16 KB | 1.832 | 8,012 | 35 |

### Observations

1. **Hybrid wins on RMAT.** It is 2.8× faster than top-down and 3.1× faster than
   bottom-up in `t_model`, and fetches 3.1× fewer CXL lines. It performs two
   top-down rounds (small frontier) and three bottom-up rounds (12,288 =
   3 × 4,096 vertex scans).
2. **Pure bottom-up needs a large frontier.** It examines slightly fewer edges
   than top-down but scans all `n` vertices every level, so it loses when the
   frontier is small.
3. **Block prefetching is wrong for irregular graphs.** On RMAT it over-fetches
   3–6× the lines and multiplies modeled latency, even though the tile hit rate
   is 98–99%. The hit rate counts tile fills, not the lines wasted inside each
   tile.
4. **Cache size helps until the working set fits.** The graph is about 230 KB;
   8 KB → 64 KB cuts fetches by 32%, and 256 KB adds nothing.
5. **CXL BFS is only ~3× DRAM.** Unlike the 30× streaming stencil, CSR access has
   enough spatial locality that a modest software cache captures most of it.
6. **Transport dominates wall time.** `t_cpu_run` is 45–250 ms and `t_wall` is
   150–820 ms, versus 0.3–1.7 ms of modeled CXL time.

### Correctness

`ctest` passes all four cases. `bfs_test` checks all three kernels on six graph
shapes against the reference, exact top-down and bottom-up counters, the
`col_idx_reads == edges_examined` invariant, and the CXL direct/block backends.

## M3 — SpMV

Raw data: `results/raw/m3_spmv.csv` (regenerate with
`scripts/run_spmv_calibration.sh`). Platform and server settings are the same as
M2: 200 ns base latency, TCP, file-backed pool. Full walkthrough:
[docs/algorithms/spmv.md](../algorithms/spmv.md).

### Laplacian (5-point grid, side = 64, 4096 x 4096, nnz = 20,224)

| Backend | Format | Block | `t_model` ms | Line fetches | Hits | Misses |
|---|---|---|---|---|---|---|
| dram | csr | — | 0.194 | 0 | 0 | 0 |
| cxl-direct | csr | — | 1.139 | 4,049 | 44,591 | 4,049 |
| cxl-block | csr | 4 KB | 1.042 | 4,049 | 48,575 | 65 |

Block mode cuts modeled latency by 7% (1.042 vs 1.139 ms) by coalescing reads;
the hit-rate difference is small (98.7% vs 91.8%) because the whole matrix fits in
the 64 KB cache.

### RMAT (`n = 4096`, `nnz = 65,536`, seed 7), CXL direct, 64 KB cache

| Backend | Format | Block | `t_cxl_model` ms | `t_model` ms | Line fetches | Hits | Misses |
|---|---|---|---|---|---|---|---|
| dram | csr | — | 0.000 | 0.463 | 0 | 0 | 0 |
| cxl-direct | csr | — | 2.634 | **3.103** | 12,545 | 126,719 | 12,545 |
| cxl-direct | sell | slice 4 / sigma 8 | 5.061 | **6.132** | 24,100 | 234,308 | 24,100 |
| cxl-direct | bcsr | 2x2 | 5.412 | **6.519** | 25,770 | 206,241 | 25,770 |
| cxl-direct | bcsr | 4x4 | 15.362 | **18.199** | 73,152 | 531,308 | 73,152 |

### Random sparse (`rows = cols = 8000`, `nnz = 64,000), CXL direct, 64 KB cache

| Backend | Format | Block | `t_cxl_model` ms | `t_model` ms | Line fetches | Hits | Misses |
|---|---|---|---|---|---|---|---|
| cxl-direct | csr | — | 2.625 | **3.242** | 12,501 | 131,499 | 12,501 |
| cxl-direct | sell | slice 4 / sigma 8 | 3.137 | **3.888** | 14,936 | 147,024 | 14,936 |
| cxl-direct | bcsr | 4x4 | 27.512 | **32.416** | 131,011 | 951,792 | 131,011 |

### Observations

1. **CSR is the best format so far in all SpMV sweeps.** SELL and BCSR never beat
   CSR in `t_model` or line fetches on RMAT or random matrices; they only tied on
   the very small, perfectly regular Laplacian.
2. **SELL's column-major layout costs.** With `slice_height = 4`, consecutive `k`
   for a lane sit 16 bytes apart, destroying CXL cache-line reuse; line fetches
   double versus CSR (24,100 vs 12,545 on RMAT) and padding adds traffic
   (128,180 stored for 65,536 logical entries).
3. **BCSR padding dominates at small blocks.** 2x2 blocks already hurt RMAT
   (`t_model` 6.52 ms vs 3.10 ms for CSR); 4x4 blocks hurt badly
   (`t_model` 18.2 ms, 73,152 line fetches, 4.7 MB read).
4. **CXL SpMV on RMAT is ~6.7x DRAM** (CSR `t_model` 3.10 ms vs 0.46 ms). Best CXL
   penalty observed across the calibration workloads so far, behind the 30x
   streaming stencil and the ~3x irregular BFS.
5. **Software cache captures most reuse:** 91% hits on RMAT CSR (12,545 misses of
   139,344 lookups), 90.8% on the random matrix.
6. **Transport dominates wall time.** `t_cpu_run` is 77–2,241 ms and `t_wall` is
   336–11,616 ms versus 0.2–18.2 ms of modeled CXL time.

## Next

M5 adds the cross-algorithm analysis, survey report, and final report. Results
will be appended here.

## M4 — k-NN calibration

Raw data: `results/raw/m4_knn.csv` (regenerate with
`scripts/run_knn_calibration.sh`). Platform and server settings are the same as
M3: 200 ns base latency, TCP, file-backed pool. Full walkthrough:
[docs/algorithms/knn.md](../algorithms/knn.md).

Sweep: dataset 1024 vectors x 8 dims (65,536 bytes), 4 queries, k=2.

| Backend | Kernel | Block | Cache | `t_model` ms | Line fetches | Hits | Misses | Hit rate | Bytes read |
|---|---|---|---|---|---|---|---|---|---|
| dram | brute | — | — | 0.087 | 0 | 0 | 0 | 0 | 0 |
| cxl-direct | brute | — | 64 KB | 0.422 | 1,024 | 3,072 | 1,024 | 75.0% | 65,536 |
| cxl-block | brute | 4 KB | 64 KB | 0.423 | 1,024 | 4,095 | 1 | 99.8% | 65,536 |
| cxl-direct | tile | 64 | 64 KB | 0.315 | 1,024 | 3,072 | 1,024 | 75.0% | 65,536 |
| cxl-direct | tile | 256 | 64 KB | 0.313 | 1,024 | 3,072 | 1,024 | 75.0% | 65,536 |
| cxl-block | tile | 4 KB / 32 | 64 KB | 0.316 | 1,024 | 127 | 1 | 99.2% | 65,536 |

### Observations

1. **CXL k-NN penalty is modest.** Brute force on CXL is ~4.8x the DRAM compute in
   `t_model` (0.42 ms vs 0.09 ms).
2. **Block mode improves reuse.** Brute force with 4 KB blocks reaches a 99.8% hit
   rate; per-vector direct-mode reads reach 75%.
3. **Tiling is free once the working set fits.** Tiles of 32/64/256 vectors give
   similar CXL traffic.

## Final report

M5 closes the loop: cross-algorithm analysis, survey report, and final report.
