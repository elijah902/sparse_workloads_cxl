# ADR 0001 — CXL platform and simulation backend

- Status: accepted
- Date: 2026-11-09

## Context

The project must use CXLMemSim to study memory-centric algorithms. The available
machines impose hard constraints:

- **Samsung Book laptop (WSL2, Debian 13, i7-1360P, 7.6 GB RAM):** no hardware
  PMU. `perf stat` reports `<not supported>` for cycles, instructions,
  cache-references, cache-misses, and `cycles:pp`.
- **UNCC Orion (RHEL 9.7):** generic user-space counters work
  (`cycles:u`, `instructions:u`, `cache-misses:u`), but Intel PEBS is
  unsupported (`cycles:pp` reports `<not supported>`). GCC is 11.5, which lacks
  C++20 `std::format`.
- Native Linux installation on the laptop is not possible.
- The `cxltime` checkout required by the SlugAllocator path is not publicly
  available; CXLMemSim only references a local path.
- The QEMU path requires a custom QEMU build, a bootable guest image, CXL/DAX
  drivers, and KVM. The repository's launcher is stale, the guest image is a
  Spack package, and WSL2 nested KVM is not guaranteed.

## Decision

Use **`cxlmemsim_server` on WSL2** as the primary CXL model, with a small
hand-written TCP client in this repository.

The server owns the simulated CXL pool and models capacity, latency, congestion,
and coherency. The client sends cache-line-sized read/write requests using the
same packed protocol as the repository's integration tests. The client charges
the server-returned `latency_ns` analytically and reports:

- `t_compute` (measured CPU time),
- `t_cxl_model` (sum of modeled latencies),
- `t_model = t_compute + t_cxl_model`,
- `t_wall` (real time, including TCP overhead, for transparency).

Orion is used only for optional host-cache/DRAM-baseline measurements with
`perf stat`.

## Rejected alternatives

### `cxlmemsim_legacy`

Rejected because:

- It requires Intel PEBS or AMD IBS data addresses. Orion has no PEBS; WSL2 has
  no hardware PMU at all.
- The generic cache-miss fallback can open, but without PEBS it has no data
  address to report. The controller routes by `phys_addr`, so the
  address-dependent model collapses.
- On WSL2 the PEBS constructor throws, `Monitors::enable` returns `-3`, and
  `main` exits.
- It has no explicit ROI or DRAM/CXL pool registration.
- Orion GCC 11.5 cannot compile the required C++20 `std::format`.

### QEMU + `cxlmemsim_server`

Deferred, not rejected forever. It is the intended full-system Type 3 path, but
the setup cost and uncertainty are too high for the first milestone. Revisit only
if the course requires full-system guest execution.

### SlugAllocator + `cxlmemsim_server`

Blocked: the `cxltime` checkout containing `tools/slug_allocator` is not
publicly available, so the LLVM pass and runtime cannot be built reliably.

## Consequences

Positive:

- No PMU, no QEMU, no guest image, no unavailable toolchain.
- Runs on the laptop today.
- Explicit CXL placement and per-line data-movement counters.
- The algorithms remain hand-written and easy to document.

Negative:

- The CXL latency is modeled analytically, not experienced in wall time.
- The server does not model a CPU cache hierarchy; a software cache model is
  added for line reuse.
- TCP overhead is real and must be separated from the CXL result.
- The server's latency model is simple (base latency + congestion + coherency);
  the report must state this limitation.

## Follow-up

- M0: build the server and validate a 64-byte write/read with modeled latency.
- If TCP throughput is insufficient, switch the client transport to the server's
  shared-memory ring buffer (`--comm-mode=shm`) without changing the algorithm
  code.
- Record the CXLMemSim commit (`a9d46ecce92830455a666fb3b836fe67e6b2e245`) in all
  results.
