# Experiment Methodology

Status: active
Last updated: 2026-11-09

This document defines how every measurement in this project is taken, what the
numbers mean, and which parts are modeled versus measured.

## 1. Scope

The experiments study how memory-centric algorithm design changes when the large
data structure lives in CXL-attached memory instead of host DRAM. The primary
model is `cxlmemsim_server` over TCP (ADR 0001). Host cache behavior is measured
separately with Cachegrind and optional Orion `perf stat` runs.

## 2. Machines and software

| Item | Value |
|---|---|
| Primary machine | Samsung Book laptop, WSL2, Debian 13 |
| CPU | 13th Gen Intel Core i7-1360P, 16 threads |
| RAM | 7.6 GiB |
| Compiler | GCC 14.2.0 |
| CMake | 3.31.6 |
| CXLMemSim commit | `a9d46ecce92830455a666fb3b836fe67e6b2e245` |
| spdlog | 1.15.3, local install under `~/.local` |
| Optional machine | UNCC Orion, RHEL 9.7, GCC 11.5, generic `perf` counters |

Record the machine, compiler, flags, CXLMemSim commit, and server options with
every result set.

## 3. Backends

| Backend | Data location | Fetch operation | Intended use |
|---|---|---|---|
| `dram` | Host DRAM copy | `memcpy` | Baseline and compute reference |
| `cxl-direct` | CXL pool | One 64-byte line per miss | Irregular access, default CXL mode |
| `cxl-block` | CXL pool | A `block_bytes` tile per miss | Spatial prefetching and blocking studies |

All three backends run the same kernel source. Only `TieredArray` changes.

## 4. Timing model

The client is synchronous over TCP, so the CXL run's CPU time contains a large
amount of network-stack work. ADR 0002 defines the separation:

| Quantity | Definition |
|---|---|
| `t_compute` | Median CPU time of the same kernel on the DRAM backend (3 runs) |
| `t_cxl_model` | Sum of server-returned latencies for all CXL requests |
| `t_model` | `t_compute + t_cxl_model` — the CXL result used in comparisons |
| `t_cpu_run` | CPU time of the CXL run itself, including transport work |
| `t_transport_cpu` | `max(0, t_cpu_run - t_compute)` |
| `t_wall` | Real elapsed time of the CXL run, including TCP blocking |

For DRAM, `t_compute = t_cpu_run`, `t_cxl_model = 0`, and `t_model = t_compute`.

CPU time is read with `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`. It excludes time
blocked in `recv`/`send`, which is why it is much smaller than `t_wall`. The DRAM
reference is the transport-free estimate of algorithmic work.

**Do not use `t_wall` as the CXL result.** In M1, `t_wall` was roughly 380× the
modeled CXL time because of TCP round trips. It is reported only for
transparency.

## 5. Counters

`TieredArray` accumulates `MemoryStats`:

| Counter | Meaning |
|---|---|
| `host_reads`, `host_writes` | DRAM backend access calls |
| `host_bytes_read`, `host_bytes_written` | DRAM backend bytes |
| `cxl_reads`, `cxl_writes` | Requests sent to the server |
| `cxl_bytes_read`, `cxl_bytes_written` | Bytes requested |
| `line_fetches` | 64-byte read requests issued by a fetch |
| `block_fetches` | Block-mode cache-unit fills |
| `cache_hits`, `cache_misses` | Software-cache lookup outcomes |
| `cache_evictions` | Cache units displaced |
| `modeled_ns` | Sum of server-returned latencies |
| `work_units` | Algorithm work (cells, edges, nonzeros, distances) |

Derived metrics:

- Cache hit rate = `hits / (hits + misses)`.
- Effective modeled read bandwidth = `cxl_bytes_read / t_cxl_model` (bytes/ns =
  GB/s).
- Work rate = `work_units / t_model`.
- Bytes per work unit = `(cxl_bytes_read + cxl_bytes_written) / work_units`.

Reset counters after loading a dataset and before the measured kernel. Setup
traffic must not be counted.

For a read-only dataset such as the BFS CSR, load the pool once and call
`clear_cache()` plus `reset_stats()` before each measured run. This makes every
run start with a cold cache without paying setup writes again. For a dataset the
kernel modifies (the stencil), reload the input instead.

## 6. Software cache

- Fetch unit: 64 bytes for `cxl-direct`, `block_bytes` for `cxl-block`.
- Capacity: `cache_bytes`, rounded up to at least one fetch unit.
- Policies: LRU (default) or direct-mapped.
- Writes are write-through and invalidate the touched fetch unit, so a read
  after a write always sees the updated backing store.
- The cache models CXL-side reuse, not host L1/L2/LLC.

The cache is a model, not a hardware measurement. Report its size and policy
with every result.

## 7. Server protocol and the 64-byte cap

The wire format is the server's packed `ServerRequest`/`ServerResponse`:

```text
ServerRequest:  op(1) | addr(8) | size(8) | timestamp(8) | value(8) | expected(8) | data[64]
ServerResponse: status(1) | latency_ns(8) | old_value(8) | data[64]

OP_READ = 0
OP_WRITE = 1
```

The server clamps `size` to 64 bytes. Consequences:

- A block fetch is a sequence of 64-byte requests, not one wide transfer.
- Block mode models prefetch granularity and cache behavior, not link width.
- The modeled latency of a block is the sum of its line latencies.
- A wide-transfer model, if added later, is a separate analytical extension and
  must be labeled as such.

At a 200 ns base latency, the M0/M1 server returned about 210 ns for a 64-byte
read and about 260 ns for a write (write penalty plus transfer time). The exact
value depends on congestion and coherency state; record the server options.

## 8. Correctness gates

Every backend must pass the same checks before its numbers are used:

1. `stencil_test` compares the array result against a host reference with a
   tolerance of `1e-6`.
2. `stencil_test` also checks exact cache/line/block counters for `n = 32`,
   `steps = 2`.
3. `tiered_array_test` checks data round-trips, partial reads that cross fetch
   units, write-through invalidation, and the cache policies.
4. `cxl_client_smoke` checks the transport and reports read/write latency.

CXL tests are skipped when the server is not running. A result set is invalid if
the tests fail.

## 9. Procedure

1. Start the server with a recorded configuration (`scripts/run_server.sh`).
2. Run the tests.
3. Warm up: one unmeasured run.
4. Measure 3–5 runs per configuration.
5. Report the median and interquartile range, not the mean.
6. Pin the server and client to different cores with `taskset` when running
   timing sweeps, so the client's TCP work does not preempt the server.
7. Record the server options, CXLMemSim commit, compiler, and flags.

The M1 calibration used one measured run and a 3-run DRAM reference. M2/M3 will
use the full repetition procedure.

## 10. Parameters

| Parameter | Values |
|---|---|
| Backend | `dram`, `cxl-direct`, `cxl-block` |
| CXL base latency | 80, 100, 200, 400 ns |
| Block size | 4, 16, 64, 256 KB |
| Cache size | 32 KB, 256 KB, 2 MB (selected experiments) |
| Cache policy | LRU (default), direct-mapped |
| Threads | 1 for the first pass; OpenMP later if needed |
| Grid / dataset size | small fixtures for tests; larger for sweeps |

## 11. Host cache measurements

WSL2 exposes no hardware PMU. Host cache behavior is measured in two ways:

- **Cachegrind** on small DRAM runs: `valgrind --tool=cachegrind` reports
  instruction/data references, L1/L2/LLC misses, and branch behavior. It is a
  simulation, so label it as such.
- **Orion `perf stat`** on the DRAM baseline:
  `perf stat -e cycles:u,instructions:u,cache-references:u,cache-misses:u`.
  Orion lacks PEBS (`cycles:pp` is unsupported), so these are generic counters,
  not address-attributed samples. Label Orion results as a different machine.

Do not mix Cachegrind, Orion, and CXLMemSim numbers in one table without
labeling the source. They answer different questions.

## 12. Reproducibility

- Pin CXLMemSim to `a9d46ecce92830455a666fb3b836fe67e6b2e245`.
- Apply `patches/0001-remove-unused-bpf-include.patch`.
- Build spdlog locally under `~/.local` if the system package is missing.
- Use `scripts/build_cxlmemsim.sh`, `scripts/run_server.sh`, and
  `scripts/build.sh`.
- Write raw results to `results/raw/` and commit the summary tables and plots.
- Include the CSV header and the server options with every result set.

## 13. Limitations

- The server's latency model is base latency + congestion + coherency, not a
  detailed fabric. Do not make absolute hardware claims.
- The server does not model a CPU cache hierarchy; the software cache is a
  separate model.
- TCP overhead is real and large; `t_model` is the result, not `t_wall`.
- Block mode does not model wider transfers because of the 64-byte cap.
- The DRAM compute reference assumes the algorithm's compute is independent of
  memory tier. State this explicitly for direction-optimizing BFS.
- The M1 `t_compute` values are sub-millisecond and noisy; use larger grids and
  more repetitions for final numbers.

## 14. Commands

```bash
# Build everything
./scripts/build.sh

# Start the server (override with PORT, CAPACITY_MB, LATENCY_NS, ...)
./scripts/run_server.sh &

# Tests
ctest --test-dir build --output-on-failure

# Stencil calibration (CSV)
./scripts/run_stencil_calibration.sh

# Single measurement
./build/stencil_bench --backend cxl-direct --kernel row --n 256 --steps 4 \
    --cache-kb 32 --csv

# Host cache simulation on a small DRAM run
valgrind --tool=cachegrind ./build/stencil_bench --backend dram --kernel row --n 64 --steps 1
```
