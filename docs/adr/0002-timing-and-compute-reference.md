# ADR 0002 — Timing model and compute reference

- Status: accepted
- Date: 2026-11-09
- Supersedes: the `t_compute` definition in ADR 0001 and the project plan

## Context

The primary backend is `cxlmemsim_server` over TCP (ADR 0001). The client is
synchronous: each CXL request is a TCP round trip, and the server returns a
modeled latency instead of sleeping. This creates two measurement problems.

1. **TCP CPU cost is large.** A cache-line request costs tens of microseconds of
   host CPU time in the network stack, while the modeled CXL latency is about
   210 ns. If `t_compute` were the CPU time of the CXL run, it would be almost
   entirely transport work, not algorithm work.
2. **Wall time is dominated by TCP.** In the M1 stencil calibration, the modeled
   CXL time was 7.5 ms while wall time was about 2.9 s. Wall time is useful for
   transparency but cannot be the CXL result.

The server also caps a request at 64 bytes. A "block" fetch is therefore a
sequence of 64-byte requests, not one wide transfer.

## Decision

Measure the algorithm's compute term on the **DRAM backend**, then add the
server's modeled CXL latency from the CXL run:

| Quantity | Definition |
|---|---|
| `t_compute` | Median CPU time of the same kernel on the DRAM backend (3 runs) |
| `t_cxl_model` | Sum of server-returned latencies for all requests in the CXL run |
| `t_model` | `t_compute + t_cxl_model` |
| `t_cpu_run` | CPU time of the CXL run itself, including transport work |
| `t_transport_cpu` | `max(0, t_cpu_run - t_compute)` |
| `t_wall` | Real elapsed time of the CXL run, including TCP blocking |

For the DRAM backend, `t_compute = t_cpu_run`, `t_cxl_model = 0`, and
`t_model = t_compute`.

`t_model` is the result used to compare backends and configurations. `t_cpu_run`,
`t_transport_cpu`, and `t_wall` are reported so the transport cost is visible and
is not mistaken for CXL behavior.

## Rationale

This is trace-driven simulation. The algorithm executes the same loop on both
backends; only the memory service changes. Measuring compute on DRAM gives a
transport-free estimate of the algorithm's work, while the CXL run supplies the
access trace and the server's modeled latency.

The approach also makes the comparison reproducible: the DRAM and CXL runs use
the same compiler, flags, input, and kernel.

## Consequences

Positive:

- The modeled CXL signal is not swamped by TCP CPU time.
- Transport cost is measured and reported instead of hidden.
- The DRAM baseline is directly comparable to the CXL result.

Negative:

- The model assumes the algorithm's compute is independent of memory tier. This
  is reasonable for the stencil and CSR kernels, but direction-optimizing BFS
  changes its control flow with frontier size; for those runs the assumption
  must be stated explicitly.
- The CXL run's cache lookups and data copies are part of the host CPU time but
  are not part of the DRAM reference. They are treated as memory-system
  emulation, not algorithm work.
- Block mode does not model a wider transfer. With the 64-byte protocol cap, a
  64 KB block is 1,024 requests. A separate analytical wide-transfer model is
  deferred; if added, it must be labeled as an extension, not a CXLMemSim result.

## Follow-up

- `apps/stencil_bench.cpp` implements this model and prints both the compute
  reference and the transport cost.
- The experiment scripts will run at least 3 measured repetitions and report the
  median and interquartile range; the M1 calibration is a single measured run
  with a 3-run DRAM reference.
- Revisit the wide-transfer model when block-mode results are interpreted.
