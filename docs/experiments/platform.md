# Platform and Backend

This document records the actual platform, the CXLMemSim build, the M0
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
repeated reads: 1000, avg modeled latency=210.0 ns, min=210, max=210
stats: reads=1002 writes=2 bytes_read=64128 bytes_written=128 modeled_ns=210840 errors=0
PASS
```

The 210 ns is consistent with the server model: 200 ns base latency + 8 ns
transfer time for 64 bytes at 64 GB/s, with integer rounding. The important
facts are that data round-trips correctly and the modeled latency is nonzero and
stable.

## Timing model

The server returns `latency_ns` but does not sleep. The client charges the
latency analytically:

| Quantity | Definition |
|---|---|
| `t_compute` | CPU time in the algorithm kernel, measured natively |
| `t_cxl_model` | Sum of server-returned latencies for all CXL requests |
| `t_model` | `t_compute + t_cxl_model` |
| `t_wall` | Real elapsed time, including TCP overhead |

`t_model` is the CXL result used in comparisons. `t_wall` is reported for
transparency but is not used as the CXL result, because TCP round-trip time
would otherwise dominate.

The DRAM backend runs the same kernel against a host-DRAM copy and provides the
baseline. The difference between `t_model` for the DRAM and CXL backends is the
modeled CXL penalty.

## Counters

The client tracks:

- CXL reads, writes, bytes read, bytes written
- Sum of modeled latencies
- Errors

The `TieredArray` layer will add:

- CXL lines fetched
- Software-cache hits, misses, evictions
- Block-mode tile fetches
- Work units (BFS edges, SpMV nonzeros, kNN distance computations)

## Limitations

- The server's latency model is base latency + congestion + coherency penalty.
  It does not model a detailed CXL fabric unless GFAM is enabled.
- The server does not model a CPU cache hierarchy. Cache reuse is modeled in
  software by the `TieredArray` layer.
- TCP overhead is real and is excluded from `t_model` by construction.
- The server's expander bandwidth is fixed at 25 GB/s; effective bandwidth is
  computed from bytes and modeled time.
- WSL2 has no hardware PMU, so host cache behavior is measured with a software
  model, Cachegrind, or optional Orion `perf stat` on the DRAM baseline.

## Reproducing M0

```bash
./scripts/build_cxlmemsim.sh
./scripts/run_server.sh &          # or run in a second terminal
./scripts/build.sh
./build/cxl_client_smoke
ctest --test-dir build --output-on-failure
```
