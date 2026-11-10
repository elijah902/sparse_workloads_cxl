# Final Report — Memory-Centric Algorithms with CXL-Attached Memory

Status: **M5 — final report and survey complete (2026-11-09)**
Last updated: 2026-11-09

## 1. Executive summary

This project studied memory-centric algorithm design by running hand-written
sparse and dense kernels against a CXL-attached memory pool simulated by
[CXLMemSim](https://github.com/SlugLab/CXLMemSim), accessed through a hand-written
TCP client. Three core algorithms were implemented and calibrated; one stretch
goal was implemented.

| Workload | Milestone | CXL result (`t_model`) | DRAM baseline (`t_model`) | Penalty |
|---|---|---|---|---|
| 2D stencil (streaming, row-blocked) | M1 | ~7.68 ms | ~0.25 ms | ~30x |
| BFS (hybrid, CSR) | M2 | 0.425 ms | 0.148 ms | ~2.9x |
| SpMV (CSR, RMAT) | M3 | 3.10 ms | 0.46 ms | ~6.7x |
| k-NN (brute force, CXL) | M4 | 0.42 ms | 0.09 ms | ~4.8x |

The CXL penalties are workload-dependent and dominated by memory-access
strategy rather than by the algorithm family alone.

## 2. Method

- Host: WSL2 (Debian 13, GCC 14.2, no hardware PMU), CXLMemSim server pinned at
  `a9d46ecce92830455a666fb3b836fe67e6b2e245`, server-side latency model
  (`base + congestion + coherency + size/8`, base 200 ns, observed read 210 ns,
  write 260 ns).
- Client: hand-written TCP client over a packed binary protocol (CXL client in
  `src/cxl/`); server caps requests at 64 bytes.
- Memory abstraction: `TieredArray` with DRAM / CXL-direct / CXL-block backends,
  software cache (LRU / direct-mapped) over fetch units, `MemoryStats`.
- Timing model (ADR 0002): `t_compute` = median of three warm DRAM runs of the same
  kernel; `t_cxl_model` = sum of server-returned latencies; `t_model = t_compute +
  t_cxl_model`; `t_cpu_run`, `t_transport_cpu`, `t_wall` reported separately.
- Correctness: `ctest` suite (six cases) — transport, memory, stencil, BFS, SpMV,
  k-NN.

Full methodology: [docs/experiments/methodology.md](experiments/methodology.md).

## 3. Algorithm results

### 3.1 2D stencil (calibration, M1)

Row-blocked stencil over an `n x n` grid: CXL `t_model` ≈ 7.68 ms vs DRAM ≈ 0.25 ms
(~30x penalty) at the recorded configuration. The naive kernel is far worse;
row blocking coalesces CXL traffic. Block mode did not help because the server
caps requests at 64 bytes, so block mode models prefetch granularity, not link
width.

### 3.2 BFS (M2)

CSR graph in CXL, frontier/visited/distances in DRAM. Direction-optimizing hybrid
beat both pure kernels on RMAT (`n=4096`, `m=53,635`): CXL `t_model` 0.425 ms vs
1.198 ms for top-down, and 3.1x fewer CXL line fetches (1,373 vs 4,302). CXL
penalty vs DRAM ≈ 2.9x. Pure bottom-up loses on small frontiers; block prefetching
over-fetches on irregular graphs.

### 3.3 SpMV (M3)

CSR on RMAT (`n=4096`, `nnz=65,536`): CXL `t_model` 3.10 ms vs DRAM 0.46 ms (~6.7x).
CSR was the best format: SELL-C-σ doubled line fetches vs CSR (24,100 vs 12,545) due
to column-major slice layout and padding; BCSR over-padded badly at small block
sizes (18.2 ms at 4x4). Software cache hit rate ≈ 91%.

### 3.4 k-NN (M4, stretch)

Brute force over a CXL-backed dataset (1024 vectors x 8 dims, 4 queries): CXL
`t_model` 0.42 ms vs DRAM 0.09 ms (~4.8x). Block mode (4 KB) reached a 99.8% cache
hit rate vs 75% for per-vector direct-mode reads; tiling did not change traffic
once the working set fit in the cache.

## 4. Cross-algorithm observations

1. **Access pattern dominates.** The 30x penalty is for streaming/regular access
   with no reuse; BFS/SpMV/k-NN penalties are 3x–7x because a software cache
   captures reuse.
2. **Irregularity is not fatal.** BFS hybrid reduced both work and traffic; CSR
   SpMV hit ~91% cache reuse.
3. **Prefetching backfires on irregular access.** Block mode helped only for
   regular access and over-fetched for BFS/SELL.
4. **Format choice matters.** BCSR/SELL padding eroded the intended benefit.
5. **Transport is real.** `t_wall` >> `t_model` in every CXL run; the timing
   separation in ADR 0002 is what makes the numbers interpretable.

## 5. Survey: memory-centric architectures and CXL / disaggregated memory

> See the full survey at [docs/report/survey.md](report/survey.md).

### 5.1 Memory-centric architectures

- **Processing-in-memory (PIM)** / near-data computing: compute moves next to
  bulk memory (3D-stacked DRAM, HBM). Eliminates or reduces data movement; latency
  penalty replaced by capacity/bandwidth trade.
- **Storage-class memory / persistent memory**: byte-addressable non-volatile
  memory (NVDIMM, SCM) integrated via CXL or PCIe.
- **Memory-centric / data-centric design**: algorithm and memory co-design
  (e.g., blocking for locality, prefetch, hybrid page placement).

### 5.2 CXL protocols and generations

- CXL defines CXL.io, CXL.cache, CXL.mem; device types 1 (IO/cache), 2 (accelerator
  with optional host-managed device memory), 3 (memory expander).
- CXL 1.x/2.0/3.x: 2.0 introduced memory pooling (multi-logical-device controllers)
  and memory expansion; 3.0 (PCIe 6.0 based, 64 GT/s, 256B flits, ~256 GB/s raw
  x16) added enhanced coherency, latency-optimized flits, peer-to-peer DMA,
  memory sharing/pooling enhancements; 3.1 added further fabric/pooling features.
- Host-managed vs device-managed (device-bias) memory; software vs hardware
  coherence.
- Real measured CXL device latency/bandwidth: ~214–394 ns, ~18–52 GB/s; local CXL
  ~200–400 ns, slightly above cross-socket DRAM; remote CXL adds latency/bandwidth
  loss.

### 5.3 Disaggregated memory

- Logical memory pools (VMware), FAM / Global FAM / shared FAM; kernel-managed vs
  application-managed modes; NUMA exposure of CXL memory as a CPU-less node.
- CXL as fabric for memory pooling; hardware vs software disaggregation; loaded
  latency grows with bandwidth utilization.

### 5.4 Relevance to the experiments

- The simulated pool is a Type-3 memory expander (CXL.mem); the client charges
  modeled latency per line fetch.
- Software cache models CXL-side reuse; hardware caches (host L1/L2/LLC, CXL-side
  prefetch) are the real-world counterpart. PEBS unavailable on WSL2, so software
  counters used.

## 6. Conclusions

1. CXL-attached memory adds a multiplicative latency penalty to in-memory
   algorithms; magnitude (3x–30x) tracks reuse and access regularity.
2. Direction-optimizing BFS and format-aware SpMV reduce CXL traffic.
3. Software caching is an effective mitigation; block prefetch helps regular
   access and hurts irregular access.
4. CXL generations (2.0/3.x) target exactly the pooling and latency improvements
   this study approximates with a simple latency model.

## 7. Future work

- Real CXL hardware (Orion) `perf stat` / Cachegrind runs for host caches.
- Real SNAP/Graph500/SuiteSparse datasets.
- QEMU-based CXL model + server integration.
- Final reproducibility check and report freeze.

## 8. References

- CXLMemSim: <https://github.com/SlugLab/CXLMemSim>
- CXL Consortium: <https://www.computeexpresslink.org/>
- Demystifying CXL Memory (MICRO 2023), Logical Memory Pools (HotNets 2023),
  A Programming Model for Disaggregated Memory over CXL (ASPLOS 2026),
  Dissecting CXL Memory Performance at Scale (arXiv 2409.14317).
- CXL 3.0 white paper, CXL 2.0/3.0 announcements.
