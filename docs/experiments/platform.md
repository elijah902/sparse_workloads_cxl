# Platform and Backend

This document records the actual platform, the CXLMemSim build, the M0/M1/M2/M3/M4
validation, and the timing model used by all experiments.

## Host

| Item | Value |
|---|---|
| Machine | Samsung Book laptop, WSL2 |
| OS | Debian GNU/Linux 13 (trixie), kernel 6.6.87.2-microsoft-standard-WSL2 |
| CPU | 13th Gen Intel Core i7-1360P, 16 threads (8 cores × 2) |
| RAM | 7.6 GiB |
| Compiler | GCC 14.2.0 |
| CMake | 3.31.6 |
| PMU | None exposed to WSL2 (`cycles:u`, `instructions:u`, `cache-misses:u`, `cycles:pp` all `<not supported>`) |

## CXLMemSim

- Repository: <https://github.com/SlugLab/CXLMemSim>
- Pinned commit: `a9d46ecce92830455a666fb3b836fe67e6b2e245`
- Target: `cxlmemsim_server` (not `cxlmemsim_legacy`)
- Local patch: `patches/0001-remove-unused-bpf-include.patch` removes an unused
  `<bpf/bpf.h>` include from `include/perf.h`.
- spdlog: `v1.15.3`, installed under `~/.local` because sudo is password-protected.

Build:

```bash
./scripts/build_cxlmemsim.sh
```

The script clones the pinned commit, applies the patch idempotently, builds
spdlog if needed, configures with SlugAllocator/RDMA/microbenchmarks disabled,
and builds `cxlmemsim_server`.

## M0 validation

Server:

```bash
./scripts/run_server.sh
```

Configuration used for M0:

| Option | Value |
|---|---|
| `--comm-mode` | `tcp` |
| `--port` | `9999` |
| `--capacity` | `64` MB |
| `--default_latency` | `200` ns |
| `--backing-mode` | `file` |
| `--backing-file` | `/tmp/opencode/cxlmemsim.backing` |

Client smoke test:

```bash
./build/cxl_client_smoke
```

Observed result (2026-11-09):

```text
connected to cxlmemsim_server at 127.0.0.1:9999
line A ok: latency=210 ns
line B ok: latency=210 ns
line A write ok: latency=260 ns
repeated reads: 1000, avg modeled latency=210.7 ns, min=210, max=260
stats: reads=1002 writes=2 bytes_read=64128 bytes_written=128 modeled_ns=211640 errors=0
PASS
```

The 210 ns read is consistent with the server model: 200 ns base latency + 8 ns
transfer time for 64 bytes at 64 GB/s, with integer rounding. A write returns
about 260 ns because the server adds a 50 ns write penalty plus transfer time.
The important facts are that data round-trips correctly and the modeled latency
is nonzero and stable.

## M1 through M4 validation

M1 added the `TieredArray` abstraction, the software cache, `MemoryStats`, and
the 2D stencil. M2 added CSR graph builders and BFS. M3 added CSR, SELL-C-σ, and
BCSR formats with SpMV. M4 added the k-NN stretch (brute force vs tiled). Six
CTest cases pass:

| Test | What it checks |
|---|---|
| `cxl_client_smoke` | Two 64-byte lines round-trip; read and write latency nonzero |
| `tiered_array_test` | DRAM and both CXL backends; cache policies; partial reads; write-through invalidation; exact counters |
| `stencil_test` | Naive and row-blocked kernels match the host reference; exact cache/line/block counters |
| `bfs_test` | All BFS kernels on six graph shapes; exact top-down/bottom-up counters; CXL direct and block |
| `spmv_test` | All SpMV formats on several matrices; exact work accounting; CXL direct and block |
| `knn_test` | Brute force and tiled k-NN match the reference; CXL direct and block |

The CXL cases connect to `cxlmemsim_server`; if it is not running they report a
skip.

```bash
./scripts/run_server.sh &
ctest --test-dir build --output-on-failure
# 1/6 cxl_client_smoke ... Passed
# 2/6 tiered_array_test ... Passed
# 3/6 stencil_test ... Passed
# 4/6 bfs_test ... Passed
# 5/6 spmv_test ... Passed
# 6/6 knn_test ... Passed
```

### M1 calibration summary

Grid `n = 256`, `steps = 4`, row-blocked kernel, server at 200 ns base latency.
`t_compute` is the median DRAM reference (ADR 0002); `t_cxl_model` is the sum of
server-returned latencies.

| Backend | `t_model` ms | Line fetches | Block fetches | Hits | Misses |
|---|---|---|---|---|---|
| dram | 0.252 | 0 | 0 | 0 | 0 |
| cxl-direct (32 KB cache) | 7.677 | 16,384 | 0 | 0 | 16,384 |
| cxl-block (64 KB block, 64 KB cache) | 7.707 | 16,384 | 16 | 1,008 | 16 |

The CXL row kernel costs about 30× the DRAM kernel in `t_model`, almost all of it
modeled memory latency.

### M2 calibration summary

RMAT `n = 4,096`, `m = 53,635`, source 2048, 200 ns base latency. Graph arrays in
CXL; frontier and distances in DRAM.

| Backend | Kernel | `t_model` ms | Edges | Line fetches |
|---|---|---|---|---|
| dram | top-down | 0.346 | 53,629 | 0 |
| cxl-direct | top-down | 1.198 | 53,629 | 4,302 |
| cxl-direct | bottom-up | 1.336 | 48,277 | 4,407 |
| cxl-direct | hybrid | 0.425 | 3,576 | 1,373 |

Hybrid direction optimization cuts both edge work (15×) and CXL line fetches
(3.1×) versus top-down. Full data and observations are in
[docs/experiments/results.md](results.md).

## Timing model

The server returns `latency_ns` but does not sleep. The client charges the
latency analytically. The CXL run's own CPU time contains a large amount of TCP
network-stack work, so it is not used as `t_compute`; the compute term is
measured on the DRAM backend instead (ADR 0002):

| Quantity | Definition |
|---|---|
| `t_compute` | Median CPU time of the same kernel on the DRAM backend (3 runs) |
| `t_cxl_model` | Sum of server-returned latencies for all CXL requests |
| `t_model` | `t_compute + t_cxl_model` |
| `t_cpu_run` | CPU time of the CXL run, including transport work |
| `t_transport_cpu` | `max(0, t_cpu_run - t_compute)` |
| `t_wall` | Real elapsed time, including TCP blocking |

`t_model` is the CXL result used in comparisons. `t_cpu_run`,
`t_transport_cpu`, and `t_wall` are reported for transparency but are not used as
the CXL result, because TCP round-trip time would otherwise dominate.

The DRAM backend runs the same kernel against a host-DRAM copy and provides the
baseline. The difference between `t_model` for the DRAM and CXL backends is the
modeled CXL penalty.

## Counters

`TieredArray` tracks, per backend:

- CXL reads, writes, bytes read, bytes written
- 64-byte line fetches and block-mode fetch-unit fills
- Software-cache hits, misses, evictions
- Sum of modeled latencies (`modeled_ns`)
- Work units (stencil cells, and later BFS edges, SpMV nonzeros, kNN distance
  computations)

The `CXLClient` separately tracks transport-level reads, writes, bytes, modeled
latency, and errors.

## Limitations

- The server's latency model is base latency + congestion + coherency penalty.
  It does not model a detailed CXL fabric unless GFAM is enabled.
- The server does not model a CPU cache hierarchy. Cache reuse is modeled in
  software by the `TieredArray` layer.
- TCP overhead is real and is excluded from `t_model` by construction.
- The server caps a request at 64 bytes, so block mode models prefetch
  granularity, not link width.
- The server's expander bandwidth is fixed at 25 GB/s; effective bandwidth is
  computed from bytes and modeled time.
- WSL2 has no hardware PMU, so host cache behavior is measured with a software
  model, Cachegrind, or optional Orion `perf stat` on the DRAM baseline.

## Reproducing M0 through M4

```bash
./scripts/build_cxlmemsim.sh
./scripts/run_server.sh &          # or run in a second terminal
./scripts/build.sh
./build/cxl_client_smoke
ctest --test-dir build --output-on-failure
./scripts/run_stencil_calibration.sh   # writes results/raw/m1_stencil.csv
./scripts/run_bfs_calibration.sh       # writes results/raw/m2_bfs.csv
./scripts/run_spmv_calibration.sh     # writes results/raw/m3_spmv.csv
./scripts/run_knn_calibration.sh      # writes results/raw/m4_knn.csv
```
