# CXL Memory-Centric Algorithms — Project Plan

Status: active
Last updated: 2026-11-09

## 1. Objective

Study how memory-centric algorithm design changes when a large data structure
lives in CXL-attached memory instead of local DRAM. The project uses
[CXLMemSim](https://github.com/SlugLab/CXLMemSim) as the CXL memory model and
implements the algorithms by hand.

Core questions:

1. How much does CXL latency change the best algorithm/format choice for BFS and
   SpMV?
2. Which design techniques absorb CXL latency best: blocking, tiling, streaming,
   format changes, or locality-aware traversal?
3. How do data movement, cache reuse, and remote-access counts trade off against
   raw runtime?

The final report also surveys memory-centric architectures and CXL/disaggregated
memory, and connects the survey to the measured results.

## 2. Constraints and platform decision

| Constraint | Consequence |
|---|---|
| Laptop runs WSL2, no hardware PMU | `cxlmemsim_legacy` cannot sample |
| Orion compute nodes have generic counters but no PEBS | Legacy has no data addresses |
| Orion GCC 11.5 lacks C++20 `std::format` | CXLMemSim does not build there as-is |
| Native Linux install is not possible | No bare-metal PMU path on the laptop |
| `cxltime`/SlugAllocator checkout is not publicly available | Instrumentation path is blocked |
| QEMU guest image + KVM are not readily available | Full-system QEMU path is high risk |

**Decision:** use `cxlmemsim_server` on WSL2. The server owns the simulated CXL
pool and models latency, capacity, congestion, and coherency. A small hand-written
TCP client in this repository sends 64-byte read/write requests and charges the
returned latency. No PMU is required.

See [ADR 0001](adr/0001-cxl-platform-and-backend.md) for the full decision record.

### Why legacy is rejected

- Intel PEBS candidates in `src/pebs.cpp` require `precise_ip` and a data
  address. Orion reports `cycles:pp` as `<not supported>`.
- The generic `PERF_COUNT_HW_CACHE_MISSES` fallback may open, but without PEBS
  the kernel does not provide the accessed data address. The model routes by
  `phys_addr`, so the address-dependent behavior collapses.
- On WSL2 even generic hardware counters are unavailable, so the PEBS
  constructor throws and `Monitors::enable` returns `-3`; `main` exits.
- The legacy path also has no explicit ROI or DRAM/CXL pool registration.

## 3. Architecture

```
BFS / SpMV / kNN / stencil   (hand-written algorithms)
            |
            v
      TieredArray
      ├── DRAM backend       memcpy from a host-DRAM copy
      ├── CXL direct         64-byte line fetch on demand
      └── CXL block          tile fetch into a DRAM staging buffer
            |
            v
      CXLClient (TCP)
            |
            v
      cxlmemsim_server
      CXL pool, capacity, latency, congestion, coherency
```

### CXL client protocol

The server's packed request/response format is the same one used by the
repository's integration tests:

```text
ServerRequest:  op(1) | addr(8) | size(8) | timestamp(8) | value(8) | expected(8) | data[64]
ServerResponse: status(1) | latency_ns(8) | old_value(8) | data[64]

OP_READ = 0
OP_WRITE = 1
```

Addresses are offsets into the server's CXL pool. Reads and writes are
cache-line sized (at most 64 bytes). The server returns `latency_ns` but does not
sleep; the client charges the latency analytically.

### Timing methodology

| Quantity | Meaning |
|---|---|
| `t_compute` | CPU time in the algorithm kernel, measured natively |
| `t_cxl_model` | Sum of server-returned latencies for all CXL requests |
| `t_model` | `t_compute + t_cxl_model` — the CXL result used in comparisons |
| `t_wall` | Real elapsed time; includes TCP overhead, reported for transparency |

This is a trace-driven simulation: compute time is measured, CXL time is modeled
by CXLMemSim. TCP overhead must not be allowed to contaminate the CXL result.
The DRAM backend runs the same kernel and gives the baseline.

### Cache behavior

- Primary: a software cache over CXL lines (configurable size, LRU or
  direct-mapped), reporting hits, misses, and evictions.
- Secondary: Cachegrind on small DRAM runs for host L1/L2/LLC behavior.
- Optional: Orion `perf stat -e cycles,instructions,cache-references,cache-misses`
  on the DRAM baseline, clearly labeled as a different machine with generic
  counters.

## 4. Algorithms

### 4.1 BFS (core)

| Item | Plan |
|---|---|
| Input | Undirected CSR graph |
| Baseline | Top-down BFS with a queue/bitmap frontier |
| Variants | Bottom-up bitmap BFS; direction-optimizing hybrid |
| CXL placement | `row_ptr` and `col_idx` in CXL; frontier, visited, and queue in DRAM |
| Access modes | Direct 64-byte lines; block fetch of adjacency slices |
| Metrics | Edges examined, CXL lines/bytes, cache hit rate, `t_model` |
| Correctness | Compare visited set and BFS levels against a reference |

### 4.2 SpMV (core)

| Item | Plan |
|---|---|
| Input | Sparse matrix in Matrix Market format |
| Baseline | CSR SpMV |
| Variants | SELL-C-σ; BCSR |
| CXL placement | `row_ptr`, `col_idx`, `values` in CXL; `x` and `y` in DRAM |
| Access modes | Direct 64-byte lines; block fetch of row/tile data |
| Metrics | GFLOP/s, CXL lines/bytes, cache hit rate, `t_model` |
| Correctness | Compare `y` against a reference CSR implementation |

### 4.3 k-NN (stretch)

| Item | Plan |
|---|---|
| Input | Dense vectors and queries |
| Baseline | Brute-force scan |
| Variant | Tiled/blocked distance computation; structure-of-arrays layout |
| CXL placement | Dataset in CXL; queries and top-k heaps in DRAM |
| Metrics | Distance computations, CXL bytes, cache hit rate, `t_model` |
| Correctness | Compare top-k indices and distances against brute force |

k-NN starts only after BFS and SpMV are correct and measured.

### 4.4 2D stencil (calibration/control)

A small, predictable 5-point stencil used to validate the client, the
direct/block backends, the counters, and the timing model before trusting the
irregular algorithms. It is not a third core algorithm.

## 5. Datasets

| Workload | Datasets | Size target |
|---|---|---|
| BFS | RMAT (Graph500-style), SNAP social/road graphs | Fit in 256 MB CXL |
| SpMV | SuiteSparse: Poisson, social, FEM matrices | Fit in 256 MB CXL |
| kNN | Synthetic vectors (N=100k, D=64/128) | Fit in 256 MB CXL |
| Stencil | Synthetic 2D grid (2048×2048, 4096×4096) | Fit in 256 MB CXL |

Rules:

- Fixed DRAM and CXL budgets; exceeding capacity is an error, not silent swapping.
- Store a checksum and the reference result for every input.
- Keep small fixtures in the repository; generate or download large inputs.

## 6. Experiment matrix

### 6.1 Backends

| Backend | Data location | Fetch operation |
|---|---|---|
| `dram` | Host DRAM | `memcpy` into staging |
| `cxl-direct` | CXL pool | 64-byte line request per access |
| `cxl-block` | CXL pool | Tile request into staging, then compute |

### 6.2 Parameters

| Parameter | Values |
|---|---|
| CXL latency (`--default_latency`) | 80, 100, 200, 400 ns |
| Block size | 4, 16, 64, 256 KB |
| Software cache | 32 KB, 256 KB, 2 MB (selected experiments) |
| Threads | 1 (first pass); optional OpenMP later |

### 6.3 Measurements

- `t_compute`, `t_cxl_model`, `t_model`, `t_wall`.
- CXL reads, writes, lines, bytes.
- Software cache hits, misses, evictions, hit rate.
- Work units: BFS edges examined, SpMV nonzeros, kNN distance computations.
- Derived: effective CXL bandwidth (`bytes / t_cxl_model`), work rate.
- Correctness: checksum and reference comparison.

### 6.4 Repetition and statistics

- One warm-up run, then 3–5 measured runs per configuration.
- Report median and interquartile range.
- Pin the server and client to different cores with `taskset`.
- Record the CXLMemSim commit, compiler, flags, and server options with every
  result.

## 7. Document plan

| Document | Contents |
|---|---|
| `docs/project-plan.md` | This plan: objective, platform, architecture, algorithms, experiments, milestones |
| `docs/adr/0001-cxl-platform-and-backend.md` | Why server + TCP client; why legacy/QEMU/SlugAllocator were rejected |
| `GLOSSARY.md` | CXL, CSR, SELL-C-σ, BCSR, BFS, SpMV, kNN, direct/block, modeled time, etc. |
| `docs/algorithms/bfs.md` | BFS problem, CSR, variants, **code walkthrough**, CXL pattern, correctness, experiments |
| `docs/algorithms/spmv.md` | SpMV formats, variants, **code walkthrough**, CXL pattern, correctness, experiments |
| `docs/algorithms/knn.md` | k-NN stretch: brute force, tiling, **code walkthrough**, experiments |
| `docs/algorithms/stencil.md` | Calibration workload and expected behavior |
| `docs/experiments/platform.md` | Platform constraints, server/client protocol, timing model, limitations |
| `docs/experiments/methodology.md` | Metrics, datasets, repetition, pinning, statistics, cache measurements |
| `docs/experiments/results.md` | Tables and plots produced by the experiment scripts |
| `docs/report/survey.md` | Memory-centric architectures and CXL/disaggregated memory survey |
| `docs/report/final-report.md` | Final report connecting survey, design, and measurements |
| `README.md` | Build, run, reproduce |

Every algorithm document must contain a **code walkthrough** section that maps
the explanation to concrete files, functions, and loops. This satisfies the
course requirement that each implemented algorithm has a document explaining
the code.

## 8. Survey outline

1. Memory wall and data-movement energy.
2. Memory-centric architectures: near-memory computing, PIM, 3D-stacked memory,
   UPMEM, HBM-PIM, AiM.
3. CXL: `.io`/`.cache`/`.mem`, Type 1/2/3 devices, switching, fabric, DCD/GFAM,
   CXL 3.x.
4. Disaggregated memory: pooling, remote memory, OS/runtime support, software
   vs hardware approaches.
5. CXL tiering and management: page placement, migration, hotness tracking.
6. Simulators and evaluation: CXLMemSim, gem5/SimCXL, QEMU CXL, Ramulator,
   DRAMSim3.
7. Algorithm implications for BFS, SpMV, and k-NN: blocking, locality, streaming,
   near-data processing.
8. Open challenges: latency, bandwidth, coherence, security, fault tolerance,
   programming models.

## 9. Milestones

| Phase | Work | Exit criterion |
|---|---|---|
| **M0** | Build `cxlmemsim_server`; implement `CXLClient`; write/read one line | **Done 2026-11-09.** Data round-trips; 210 ns modeled latency; 0 errors |
| **M1** | Repo skeleton, common utilities, dataset loaders, 2D stencil | Stencil runs in DRAM/direct/block; line counts match expectations |
| **M2** | BFS top-down, bottom-up, hybrid; `bfs.md`; sweeps | Correct on all graphs; edges/s, CXL bytes, cache-hit plots |
| **M3** | SpMV CSR, SELL-C-σ, BCSR; `spmv.md`; sweeps | Correct on all matrices; GFLOP/s and CXL-byte plots |
| **M4** | k-NN stretch | Correct top-k; tiled vs brute-force comparison |
| **M5** | Cross-algorithm analysis, survey, final report, reproduction | Report and repo reproduce every figure |

## 10. Risks and mitigations

| Risk | Mitigation |
|---|---|
| TCP round-trip dominates wall time | Report `t_model`; batch requests; switch to SHM ring buffer if needed |
| Server latency model is simple | Sweep `--default_latency`; document the model; avoid absolute hardware claims |
| Server bandwidth model is coarse | Compute effective bandwidth; use multiple connections for concurrency |
| Dataset exceeds 256 MB CXL | Start small; scale after the pipeline works |
| Build issues (spdlog, RAM) | Local spdlog install; `-j4` or `-j2`; pin the CXLMemSim commit |
| Orion cannot build C++20 | Keep experiments on WSL2; use Orion only for optional perf |
| Cache behavior is not hardware-measured on WSL2 | Software CXL cache model; Cachegrind; optional Orion perf |

## 11. Reproducibility

The repository will provide:

- `scripts/build_cxlmemsim.sh` — clone and build the pinned CXLMemSim server.
- `scripts/run_server.sh` — start the server with a recorded configuration.
- `scripts/run_experiments.sh` — run the full matrix and write CSV.
- `scripts/plot_results.py` — regenerate figures from CSV.
- A recorded `results/manifest.json` with commit, compiler, flags, and options.

## 12. Progress log

- **2026-11-09 — M0 complete.** Built `cxlmemsim_server` at commit
  `a9d46ecce92830455a666fb3b836fe67e6b2e245` on WSL2. Implemented the TCP
  `CXLClient` and a smoke test. Two 64-byte lines round-trip correctly; 1,002
  reads and 2 writes completed with 0 errors; modeled latency is 210 ns at a
  200 ns base setting. Reproducible build script and `ctest` both pass.

## 13. References

- CXLMemSim: <https://github.com/SlugLab/CXLMemSim>
- CXL Consortium specifications: <https://www.computeexpresslink.org/>
- Graph500 / RMAT: <https://graph500.org/>
- SuiteSparse Matrix Collection: <https://sparse.tamu.edu/>
- SNAP datasets: <https://snap.stanford.edu/data/>
