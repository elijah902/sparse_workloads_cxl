# Glossary

Terms used across the project. Keep definitions short and link to the relevant
document where useful.

## Memory and CXL

- **CXL (Compute Express Link):** Cache-coherent interconnect for CPUs, memory,
  accelerators, and memory expanders. Protocols: `.io`, `.cache`, `.mem`.
- **CXL Type 3 device:** A memory-only device (memory expander). The server's
  simulated pool is a Type 3 device.
- **CXL pool:** The address space owned by `cxlmemsim_server`. Addresses in the
  client protocol are offsets into this pool.
- **DRAM tier:** Host memory in the algorithm process. In the experiments it
  holds the frontier, visited state, vectors, and staging buffers.
- **Cache line:** 64 bytes. CXL requests are cache-line sized.
- **Direct mode:** Fetch one 64-byte line from CXL on demand.
- **Block mode:** Fetch a contiguous tile from CXL into a DRAM staging buffer,
  then compute on the tile.
- **Modeled latency (`t_cxl_model`):** Sum of the server-returned `latency_ns`
  values for all CXL requests.
- **Modeled time (`t_model`):** `t_compute + t_cxl_model`. The CXL result used in
  comparisons.
- **Wall time (`t_wall`):** Real elapsed time, including TCP overhead. Reported
  for transparency, not used as the CXL result.
- **PMU:** Performance Monitoring Unit. Hardware counters used by `perf`.
- **PEBS:** Intel Precise Event-Based Sampling. Provides data addresses for
  load-miss samples. Unavailable on Orion and WSL2.
- **Remote access:** A memory access served by the CXL pool instead of host
  DRAM.

## Algorithms and formats

- **BFS:** Breadth-first search. Traverses a graph level by level.
- **Top-down BFS:** Expands the frontier by scanning outgoing edges of frontier
  vertices.
- **Bottom-up BFS:** Scans all vertices and checks whether any neighbor is in the
  frontier. Better for large frontiers.
- **Direction-optimizing BFS:** Switches between top-down and bottom-up based on
  frontier size.
- **CSR (Compressed Sparse Row):** `row_ptr[n+1]`, `col_idx[nnz]`, optional
  `values[nnz]`.
- **SpMV:** Sparse matrix–vector multiplication, `y = A x`.
- **SELL-C-σ:** Sliced ELLPACK with a sorting/blocking parameter. Stores rows in
  fixed-size tiles for regular access.
- **BCSR:** Blocked CSR. Stores small dense blocks instead of individual
  nonzeros.
- **k-NN:** k-nearest-neighbor search over a dense vector dataset.
- **Stencil:** A regular grid update over a fixed neighborhood. Used here as a
  calibration workload.

## Datasets and tools

- **RMAT:** Recursive MATrix graph generator used by Graph500. Produces
  power-law-like graphs.
- **SNAP:** Stanford Network Analysis Project graph datasets.
- **SuiteSparse Matrix Collection:** Repository of sparse matrices used for
  SpMV.
- **CXLMemSim:** The CXL memory simulator used in this project.
- **CXLMemSim server:** `cxlmemsim_server`, the PMU-free server backend that owns
  the simulated CXL pool.
- **CXLMemSim legacy:** `cxlmemsim_legacy`, the PMU-sampling application-level
  backend. Rejected for this project.
- **Cachegrind:** Valgrind tool that simulates CPU caches and reports
  L1/L2/LLC behavior without a PMU.
- **ADR:** Architecture Decision Record. See `docs/adr/`.
