# Survey — Memory-Centric Architectures and CXL / Disaggregated Memory

Status: **M5 — survey draft complete (2026-11-09)**
Last updated: 2026-11-09

Related documents: [final-report.md](final-report.md) (connects the survey to the
measurements), [ADR 0002](../adr/0002-timing-and-compute-reference.md)
(timing methodology), [results.md](../experiments/results.md) (measurements).

## 1. Memory-centric architectures

- **Processing-in-memory (PIM) / near-data computing:** computation is placed
  next to bulk memory (3D-stacked DRAM, HBM). Data movement is reduced or
  eliminated at the cost of capacity, bandwidth, and specialized hardware.
- **Storage-class memory / persistent memory:** byte-addressable non-volatile
  memory (NVDIMM, SCM) attached via CXL or PCIe; combines persistence with
  memory-like access.
- **Memory-centric algorithm design:** the algorithm is designed around the
  memory hierarchy (blocking, prefetch, reuse-aware traversal, format choice).
  This project adopts that stance: kernels are hand-written and tuned for the
  CXL-backed access pattern.

## 2. CXL protocols and generations

- **Protocols:** CXL.io (I/O/enumeration), CXL.cache (device caching of host
  memory), CXL.mem (CPU access to device-attached memory). Device types: Type 1
  (IO/cache), Type 2 (accelerator with optional host-managed device memory),
  Type 3 (memory expander).
- **Generations:** CXL 1.0/1.1; CXL 2.0 introduced memory pooling (multi-logical-device
  memory controllers) and memory expansion; CXL 3.0 (PCIe 6.0-based, 64 GT/s, 256 B
  flits, ~256 GB/s raw on x16) added enhanced coherency, latency-optimized flit
  transfers, peer-to-peer DMA, and memory sharing/pooling; CXL 3.1 added further
  fabric and pooling enhancements.
- **Memory regions:** host-managed vs device-managed (device-bias) memory; software
  vs hardware cache coherence.

## 3. Disaggregated memory

- Logical memory pools (VMware), fabric-attached memory (FAM) and global/shared
  FAMs; kernel-managed vs application-managed CXL memory modes.
- NUMA view of CXL memory as a CPU-less memory node; latency/bandwidth of CXL
  remote memory vs local DRAM; loaded-latency effects under bandwidth pressure.

## 4. Relationship to the project experiments

- The CXLMemSim server models a Type-3 memory expander reached through CXL.mem;
  the hand-written client models the coherent line-fetch path.
- The software cache in `TieredArray` models CXL-side reuse; hardware caches
  (host L1/L2/LLC, device-side prefetch) are the real-world counterpart, and CXL
  3.0's enhanced coherency/prefetch capabilities are the long-run direction.
- The measured penalties (3x–30x) are consistent with the gap between local DRAM
  (~82 ns / ~97 GB/s) and CXL-attached memory (modeled ~200 ns / synthetic; measured
  CXL devices 214–394 ns, 18–52 GB/s) reported in the literature.
