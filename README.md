# CXL Memory-Centric Algorithms

Study memory-centric algorithm design with CXL-attached memory, using
[CXLMemSim](https://github.com/SlugLab/CXLMemSim) as the memory model.

Core algorithms: **BFS** and **SpMV**. Stretch: **k-NN**. Calibration: **2D
stencil**.

- Full plan: [docs/project-plan.md](docs/project-plan.md)
- Platform decision: [docs/adr/0001-cxl-platform-and-backend.md](docs/adr/0001-cxl-platform-and-backend.md)
- Glossary: [GLOSSARY.md](GLOSSARY.md)

## Status

**M0 complete.** The CXLMemSim server builds on WSL2 and the client smoke test
validates two 64-byte lines with a stable modeled latency of 210 ns (200 ns base
+ transfer time). See [docs/experiments/platform.md](docs/experiments/platform.md).

## Platform

The laptop runs WSL2, which has no hardware PMU. CXLMemSim's legacy sampler is
therefore unavailable. This project uses `cxlmemsim_server`, which needs no PMU:
the server owns the simulated CXL pool and returns a modeled latency for each
cache-line request. A small hand-written TCP client connects the algorithms to
the server.

The server's returned latency is charged analytically:

- `t_compute`: measured CPU time in the algorithm kernel
- `t_cxl_model`: sum of server-returned latencies
- `t_model = t_compute + t_cxl_model`
- `t_wall`: real elapsed time, including TCP overhead (reported separately)

## Prerequisites

- Linux or WSL2
- CMake >= 3.25, Ninja, GCC >= 13 (C++20 `std::format`)
- `spdlog` (the build script installs it under `~/.local` if missing)
- No PMU required

## Build

```bash
# 1. Build the pinned CXLMemSim server (and local spdlog if needed)
./scripts/build_cxlmemsim.sh

# 2. Start the server in one terminal
./scripts/run_server.sh

# 3. Build this project and run the smoke test
./scripts/build.sh
./build/cxl_client_smoke
```

Expected smoke-test output:

```text
connected to cxlmemsim_server at 127.0.0.1:9999
line A ok: latency=...
line B ok: latency=...
repeated reads: 1000, avg modeled latency=...
stats: reads=... writes=... bytes_read=... bytes_written=... modeled_ns=... errors=0
PASS
```

If the server is not running, the smoke test exits with code 77 and prints the
start command.

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
src/cxl/        TCP client and wire protocol
tests/          Client smoke test
scripts/        Build and run scripts
patches/        Minimal patches applied to the pinned CXLMemSim checkout
docs/           Project plan, ADR, algorithm and experiment docs
```

## Pinned versions

- CXLMemSim: `a9d46ecce92830455a666fb3b836fe67e6b2e245`
- spdlog: `v1.15.3`

The CXLMemSim checkout needs one local patch:
`patches/0001-remove-unused-bpf-include.patch` removes an unused
`<bpf/bpf.h>` include from `include/perf.h`. The server does not use bpf; the
include is a leftover from the removed bpftime dependency.
