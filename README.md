# CXL Memory-Centric Algorithms

Study memory-centric algorithm design with CXL-attached memory, using
[CXLMemSim](https://github.com/SlugLab/CXLMemSim) as the memory model.

Core algorithms: **BFS** and **SpMV**. Stretch: **k-NN**. Calibration: **2D
stencil**.

- Full plan: [docs/project-plan.md](docs/project-plan.md)
- Platform decision: [docs/adr/0001-cxl-platform-and-backend.md](docs/adr/0001-cxl-platform-and-backend.md)
- Timing model: [docs/adr/0002-timing-and-compute-reference.md](docs/adr/0002-timing-and-compute-reference.md)
- Methodology: [docs/experiments/methodology.md](docs/experiments/methodology.md)
- Results: [docs/experiments/results.md](docs/experiments/results.md)
- Glossary: [GLOSSARY.md](GLOSSARY.md)

## Status

**M4 complete.** M0 built the CXLMemSim server and validated the TCP client. M1
added the `TieredArray` memory abstraction (DRAM, CXL direct, CXL block), an
LRU/direct-mapped software cache, `MemoryStats` counters, and the calibration 2D
stencil. M2 added CSR graph builders and BFS with top-down, bottom-up, and
direction-optimizing hybrid kernels. M3 added CSR, SELL-C-σ, and BCSR formats with
SpMV kernels. M4 added the k-NN stretch (brute force vs tiled) with
`knn_test`/`knn_bench`.

All six CTest cases pass and the M1/M2/M3/M4 calibration data is recorded.

See [docs/experiments/results.md](docs/experiments/results.md) for the numbers,
[docs/algorithms/bfs.md](docs/algorithms/bfs.md) for the BFS walkthrough, and
[docs/algorithms/spmv.md](docs/algorithms/spmv.md) and
[docs/algorithms/knn.md](docs/algorithms/knn.md) for the SpMV and k-NN walkthroughs,
and [docs/experiments/platform.md](docs/experiments/platform.md) for platform
details.

Next: M5 — cross-algorithm analysis, CXL architecture survey, final report, and
reproduction.

## Platform

The laptop runs WSL2, which has no hardware PMU. CXLMemSim's legacy sampler is
therefore unavailable. This project uses `cxlmemsim_server`, which needs no PMU:
the server owns the simulated CXL pool and returns a modeled latency for each
cache-line request. A small hand-written TCP client connects the algorithms to
the server.

The server's returned latency is charged analytically (ADR 0002):

- `t_compute`: median CPU time of the same kernel on the DRAM backend
- `t_cxl_model`: sum of server-returned latencies
- `t_model = t_compute + t_cxl_model` (the CXL result used in comparisons)
- `t_cpu_run`: CPU time of the CXL run, including transport work
- `t_transport_cpu = max(0, t_cpu_run - t_compute)`
- `t_wall`: real elapsed time, including TCP overhead (reported separately)

## Prerequisites

- Linux or WSL2
- CMake >= 3.20, Ninja, GCC >= 12 (C++20)
- `spdlog` (the build script installs it under `~/.local` if missing)
- No PMU required

## Build

```bash
# 1. Build the pinned CXLMemSim server (and local spdlog if needed)
./scripts/build_cxlmemsim.sh

# 2. Start the server in one terminal
./scripts/run_server.sh

# 3. Build this project, run the tests, and try the benchmarks
./scripts/build.sh
ctest --test-dir build --output-on-failure
./build/stencil_bench --backend dram --kernel row --n 256 --steps 4 --verify
./build/bfs_bench --backend dram --graph rmat --n 4096 --edges 32768 \
    --seed 7 --source 2048 --kernel hybrid --verify
./build/spmv_bench --backend dram --matrix rmat --n 4096 --nnz 65536 \
    --seed 7 --format csr --verify
./build/knn_bench --backend dram --kernel brute --n 1024 --dim 8 --queries 4 --k 2 --verify
```

Expected smoke-test output:

```text
connected to cxlmemsim_server at 127.0.0.1:9999
line A ok: latency=...
line B ok: latency=...
line A write ok: latency=...
repeated reads: 1000, avg modeled latency=...
stats: reads=... writes=... bytes_read=... bytes_written=... modeled_ns=... errors=0
PASS
```

If the server is not running, the CXL tests and the CXL benchmark exit with a
clear message (the CTest cases use skip code 77).

## Benchmarks

Both benchmarks print timing, counters, and (with `--csv`) a machine-readable
row. They share the backend, cache, transport, and timing options.

`stencil_bench` (calibration workload):

| Option | Meaning |
|---|---|
| `--backend dram\|cxl-direct\|cxl-block` | Memory backend |
| `--kernel naive\|row` | Per-cell or row-blocked kernel |
| `--n <size>` | Grid side length |
| `--steps <count>` | Stencil iterations |
| `--block-kb <size>` | Block fetch size for `cxl-block` |
| `--cache-kb <size>` | Software cache size |
| `--repeat <count>` | Number of measured runs |
| `--csv`, `--no-header` | CSV output control |
| `--verify` | Compare against the host reference |

`bfs_bench` (core algorithm):

| Option | Meaning |
|---|---|
| `--graph grid\|rmat\|random` | Graph generator |
| `--kernel topdown\|bottomup\|hybrid` | BFS variant |
| `--n <vertices>` | Target vertex count |
| `--edges <count>` | RMAT edge count (default `8*n`) |
| `--degree <avg>` | Random-graph average degree |
| `--seed <value>` | Deterministic graph seed |
| `--source <vertex>` | BFS source |
| `--directed` | Directed graph (top-down only) |

`spmv_bench` (core algorithm):

| Option | Meaning |
|---|---|
| `--matrix laplacian\|rmat\|random` | Matrix generator |
| `--format csr\|sell\|bcsr` | Sparse format |
| `--n <size>` | Grid side or row count (default 4096) |
| `--cols <count>` | Random-matrix columns (default `n`) |
| `--nnz <count>` | RMAT/random nonzeros (default `8*n`) |
| `--seed <value>` | Matrix seed (default 12345) |
| `--slice-height <rows>` | SELL slice height (default 4) |
| `--sigma <rows>` | SELL sorting window (default 8) |
| `--block-rows <rows>` | BCSR block rows (default 2) |
| `--block-cols <cols>` | BCSR block cols (default 2) |
| `--block-kb <size>` | Block fetch size, default 64 |
| `--cache-kb <size>` | Software cache size, default 64 |
| `--repeat <count>` | Number of measured runs |

`knn_bench` (stretch algorithm):

| Option | Meaning |
|---|---|
| `--kernel brute\|tile` | Brute force or tiled k-NN |
| `--n <vectors>` | Dataset vectors (default 1024) |
| `--dim <dim>` | Vector dim (default 8) |
| `--queries <count>` | Query vectors (default 4) |
| `--k <count>` | Top-k (default 3) |
| `--tile <vectors>` | Tile size for tiled kernel (default 64) |
| `--seed <value>` | Dataset seed (default 77) |
| `--block-kb <size>` | Block fetch size (default 64) |
| `--cache-kb <size>` | Software cache size (default 64) |
| `--repeat <count>` | Number of measured runs |

Regenerate the calibration tables with:

```bash
./scripts/run_stencil_calibration.sh   # results/raw/m1_stencil.csv
./scripts/run_bfs_calibration.sh       # results/raw/m2_bfs.csv
./scripts/run_spmv_calibration.sh     # results/raw/m3_spmv.csv
./scripts/run_knn_calibration.sh      # results/raw/m4_knn.csv
```

## Server options

`scripts/run_server.sh` accepts environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `PORT` | `9999` | TCP port |
| `CAPACITY_MB` | `256` | CXL pool capacity |
| `LATENCY_NS` | `200` | Modeled base latency |
| `BACKING_FILE` | `/tmp/opencode/cxlmemsim.backing` | File-backed pool |
| `TOPOLOGY` | empty | Optional CXLMemSim topology file |

## Layout

```text
src/cxl/          TCP client and wire protocol
src/memory/       TieredArray backends, software cache, memory counters
src/workloads/    Algorithm kernels (stencil, BFS, SpMV, k-NN; final survey next)
apps/             Benchmark drivers
tests/            Transport, memory, and kernel tests
scripts/          Build, run, and calibration scripts
patches/          Minimal patches applied to the pinned CXLMemSim checkout
results/          Raw (git-ignored) and summarized experiment data
docs/             Project plan, ADRs, algorithm and experiment docs
```

## Pinned versions

- CXLMemSim: `a9d46ecce92830455a666fb3b836fe67e6b2e245`
- spdlog: `v1.15.3`

The CXLMemSim checkout needs one local patch:
`patches/0001-remove-unused-bpf-include.patch` removes an unused
`<bpf/bpf.h>` include from `include/perf.h`. The server does not use bpf; the
include is a leftover from the removed bpftime dependency.
