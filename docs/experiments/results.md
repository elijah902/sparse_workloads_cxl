# Results

Status: M1 calibration recorded
Last updated: 2026-11-09

Raw data: `results/raw/m1_stencil.csv` (regenerate with
`scripts/run_stencil_calibration.sh`).

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

## Next

M2 will add BFS (top-down, bottom-up, hybrid) with `row_ptr` and `col_idx` in
CXL and frontier/visited/queue in DRAM. M3 adds SpMV (CSR, SELL-C-σ, BCSR).
Results will be appended here.
