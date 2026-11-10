# Breadth-First Search (BFS) — Core Algorithm

Status: implemented and validated (M2)
Last updated: 2026-11-09

## 1. Purpose

BFS is one of the two core algorithms (with SpMV). It is the project's main
example of an **irregular, latency-bound** workload. The frontier changes shape
every level, access to the graph is data-dependent, and the amount of parallelism
changes as the search proceeds.

The goal is to see how memory-centric design changes when the graph lives in
CXL-attached memory:

- Which graph arrays should live in CXL?
- How much does a software cache over CXL lines help?
- Does direction-optimizing BFS reduce CXL traffic, not just edge work?
- Does spatial prefetching (block mode) help or hurt?

## 2. Problem

Given a graph `G = (V, E)` and a source vertex `s`, compute the number of edges
on a shortest path from `s` to every reachable vertex. Unreachable vertices get
`kUnreachable` (`0xFFFFFFFF`).

The project uses **CSR** (Compressed Sparse Row):

```text
row_ptr:  n + 1 offsets, uint32
col_idx:  nnz neighbor ids, uint32
```

Vertex `u`'s outgoing neighbors are `col_idx[row_ptr[u] .. row_ptr[u+1])`.

## 3. Data placement

| Structure | Location | Reason |
|---|---|---|
| `row_ptr` | CXL (`TieredArray`) | Large, read-only, shared graph data |
| `col_idx` | CXL (`TieredArray`) | The bulk of the graph; irregular reads |
| Frontier list | Host DRAM (`std::vector`) | Small, rewritten every level |
| `in_frontier` bitmap | Host DRAM | One byte per vertex; bottom-up membership test |
| Distances | Host DRAM | One `uint32` per vertex; random writes |

The graph arrays are immutable during the search, so they can live in CXL with
no write traffic. The frontier, bitmap, and distances are hot and small, so they
stay in DRAM. This split is the central memory-centric decision for BFS.

## 4. Graph builders

The repo builds deterministic graphs so results are reproducible. All builders
return a host `CsrGraph`; the benchmark then copies `row_ptr` and `col_idx` into
`TieredArray` objects.

### 4.1 `build_csr`

```cpp
CsrGraph build_csr(uint32_t num_vertices,
                   const std::vector<std::pair<uint32_t, uint32_t>> &edges,
                   bool symmetric);
```

1. Append each edge to its source row's adjacency list.
2. If `symmetric`, also append the reverse edge, so every stored edge has a
   stored reverse. This is what lets bottom-up BFS test incoming edges.
3. Sort and deduplicate each row.
4. Prefix-sum the row sizes into `row_ptr`.
5. Flatten the adjacency lists into `col_idx`.

Self-loops are kept. Duplicate edges are removed.

### 4.2 `make_grid_graph(rows, cols)`

A 2D grid with row-major ids (`id = r * cols + c`) and edges to the right and
down neighbor. With `symmetric = true` the reverse directions are added by
`build_csr`. This gives a regular graph with a wavefront-shaped frontier.

### 4.3 `make_rmat_graph(n, m, seed)`

Graph500-style RMAT with probabilities `a = 0.57`, `b = 0.19`, `c = 0.19`,
`d = 0.05`. `n` is rounded up to a power of two. Each edge is generated bit by
bit:

```cpp
for (uint32_t bit = n >> 1; bit > 0; bit >>= 1) {
    double r = rng.next_double();
    if (r < a) { /* top-left: no bit */ }
    else if (r < a + b) { v |= bit; }        // top-right
    else if (r < a + b + c) { u |= bit; }    // bottom-left
    else { u |= bit; v |= bit; }             // bottom-right
}
```

The random generator is `splitmix64`, seeded explicitly, so a given
`(n, m, seed)` always produces the same graph. RMAT graphs have a giant
component and large frontiers, which is where direction-optimizing BFS matters.

### 4.4 `make_random_graph(n, avg_degree, seed)`

Uniform random edges. Useful as a contrast to RMAT.

## 5. BFS variants

### 5.1 Top-down

For each vertex `u` in the current frontier, scan every outgoing edge `u -> v`.
If `v` is unvisited, set its distance and append it to the next frontier.

```text
for u in frontier:
    for v in neighbors(u):
        if dist[v] == UNREACHABLE:
            dist[v] = level + 1
            next.push_back(v)
```

This is optimal when the frontier is small. It only touches the edges of frontier
vertices.

### 5.2 Bottom-up

For each **unvisited** vertex `v`, scan its stored neighbors and ask whether any
of them is in the current frontier. Stop at the first hit.

```text
for v in 0..n-1:
    if dist[v] != UNREACHABLE: continue
    for w in neighbors(v):
        if in_frontier[w]:
            dist[v] = level + 1
            next.push_back(v)
            break
```

This is better when the frontier is large: it avoids scanning the edges of every
frontier vertex and instead finds each newly reachable vertex once. The cost is
that it scans all vertices every level, even when few are unvisited.

**Requirement:** bottom-up tests *incoming* edges. The implementation reads the
stored reverse direction, so the graph must be symmetric. `bfs_run` throws if a
bottom-up or hybrid run is given a directed graph.

### 5.3 Hybrid (direction-optimizing)

The hybrid starts top-down and switches based on frontier size, following
Beamer's heuristic:

```cpp
const uint32_t switch_to_bottom_up = n / 14;
const uint32_t switch_to_top_down = n / 24;

if (mode == TopDown && frontier.size() > switch_to_bottom_up)
    mode = BottomUp;
else if (mode == BottomUp && frontier.size() < switch_to_top_down)
    mode = TopDown;
```

The idea: while the frontier is small, top-down touches few edges. Once it grows
past `n / 14`, bottom-up is cheaper because most vertices are reachable and the
early-exit neighbor test finds them quickly. When the frontier shrinks below
`n / 24`, switch back.

## 6. Code walkthrough

### 6.1 Public API — `src/workloads/bfs.h`

| Declaration | Meaning |
|---|---|
| `BfsKernel` | `TopDown`, `BottomUp`, `Hybrid` |
| `BfsConfig` | Kernel, source, `symmetric` |
| `BfsStats` | Edge/vertex/array-read counters and frontier statistics |
| `BfsResult` | Distances plus stats |
| `CsrGraph` | Host CSR arrays used to build and verify |
| `build_csr` | Edge list to CSR, optional symmetrization |
| `make_grid_graph`, `make_rmat_graph`, `make_random_graph` | Deterministic generators |
| `bfs_reference` | Host queue-based BFS |
| `bfs_run` | Measured BFS over `TieredArray` graph arrays |

### 6.2 Reference — `src/workloads/bfs.cpp`

```cpp
std::vector<uint32_t> bfs_reference(const CsrGraph &graph, uint32_t source) {
    std::vector<uint32_t> distances(graph.num_vertices, kUnreachable);
    ...
    std::vector<uint32_t> queue;
    distances[source] = 0;
    queue.push_back(source);
    for (std::size_t head = 0; head < queue.size(); ++head) {
        uint32_t u = queue[head];
        for (uint32_t e = graph.row_ptr[u]; e < graph.row_ptr[u + 1]; ++e) {
            uint32_t v = graph.col_idx[e];
            if (distances[v] == kUnreachable) {
                distances[v] = distances[u] + 1;
                queue.push_back(v);
            }
        }
    }
    return distances;
}
```

This is the correctness oracle. It reads host memory directly and is not part of
the measured kernel.

### 6.3 Measured top-down level

```cpp
void top_down_level(TieredArray &row_ptr, TieredArray &col_idx,
                    const std::vector<uint32_t> &frontier,
                    std::vector<uint32_t> &next,
                    std::vector<uint32_t> &distances,
                    uint32_t next_distance, BfsStats &stats) {
    next.clear();
    for (const uint32_t u : frontier) {
        const uint32_t begin = load_row(row_ptr, u, stats);
        const uint32_t end = load_row(row_ptr, u + 1, stats);
        for (uint32_t e = begin; e < end; ++e) {
            ++stats.edges_examined;
            const uint32_t v = load_col(col_idx, e, stats);
            if (distances[v] == kUnreachable) {
                distances[v] = next_distance;
                next.push_back(v);
            }
        }
    }
}
```

`load_row` and `load_col` increment `row_ptr_reads` and `col_idx_reads` before
calling `TieredArray::load_value<uint32_t>`. Each load is one software-cache
lookup; a miss fetches the 64-byte line (direct) or the block (block mode).

### 6.4 Measured bottom-up level

```cpp
for (uint32_t v = 0; v < n; ++v) {
    ++stats.vertices_scanned;
    if (distances[v] != kUnreachable) continue;
    const uint32_t begin = load_row(row_ptr, v, stats);
    const uint32_t end = load_row(row_ptr, v + 1, stats);
    for (uint32_t e = begin; e < end; ++e) {
        ++stats.edges_examined;
        const uint32_t w = load_col(col_idx, e, stats);
        if (in_frontier[w]) {
            distances[v] = next_distance;
            next.push_back(v);
            break;
        }
    }
}
```

`in_frontier` is a byte-per-vertex bitmap in DRAM. The `break` is why bottom-up
examines fewer edges than top-down when the frontier is large.

### 6.5 Main loop — `bfs_run`

```cpp
while (!frontier.empty()) {
    stats.max_frontier = max(stats.max_frontier, frontier.size());
    ++stats.iterations;
    ++distance;

    if (config.kernel == Hybrid) { /* switch mode based on frontier size */ }

    if (mode == TopDown) top_down_level(...);
    else                 bottom_up_level(...);

    // Rebuild the bitmap for the next frontier.
    for (uint32_t u : frontier) in_frontier[u] = 0;
    for (uint32_t u : next)     in_frontier[u] = 1;
    frontier.swap(next);
}
```

`max_distance` is computed from the final distance array. `work_units` is set to
`edges_examined` on `row_ptr`'s stats, so it is counted once when the two array
stats are combined.

### 6.6 Benchmark — `apps/bfs_bench.cpp`

The driver:

1. Builds the graph (grid, RMAT, or random) with a fixed seed.
2. Copies `row_ptr` and `col_idx` into `TieredArray`s on the chosen backend.
3. For CXL, measures a 3-run DRAM reference of the same kernel (median CPU
   time) as `t_compute` (ADR 0002).
4. For each measured run, calls `clear_cache()` and `reset_stats()` so every run
   starts with a cold cache but does not rewrite the pool.
5. Runs BFS, combines the two arrays' `MemoryStats`, and prints the timing model
   plus BFS-specific counters.
6. With `--verify`, compares distances against `bfs_reference`.

`clear_cache()` was added to `SoftwareCache`/`TieredArray` for this: BFS does not
modify the graph, so the pool only needs to be written once, but the software
cache must be cold at the start of each run.

## 7. Correctness

`tests/bfs_test.cpp` checks:

- **Builders:** grid vertex/edge counts, RMAT and random non-empty and symmetric.
- **Reference:** paths, disconnected components, a star, a self-loop, and an
  out-of-range source.
- **All three kernels on six graphs** (grid, random, RMAT, path, disconnected,
  star) against the reference.
- **Exact top-down counters:** `edges_examined` equals the sum of out-degrees of
  reachable vertices; `vertices_scanned == 0`; `row_ptr_reads == 2 ×
  reachable_vertices`.
- **Exact bottom-up counters:** `vertices_scanned == n × iterations`.
- **Invariant for every kernel:** `col_idx_reads == edges_examined`.
- **CXL backends:** direct and block, for all kernels, match the reference and
  report nonzero CXL reads, line fetches, bytes, and modeled latency.

All four CTest cases pass, including the CXL cases when the server is running.

## 8. Memory behavior

### 8.1 Cache reuse

CSR access has strong spatial locality:

- `row_ptr` is read sequentially for increasing vertex ids (16 values per
  64-byte line).
- `col_idx` is read in contiguous ranges per row (16 ids per line).

A software cache therefore captures a lot of reuse. In the RMAT hybrid run,
14,303 lookups produced only 1,373 line fetches (90.6% hit rate). On the small
grid, the entire graph (49 KB) fits in the 64 KB cache, so the miss count is just
the number of distinct lines (770).

### 8.2 Block prefetching

Block mode fetches a tile on every miss. On an **irregular** graph this
over-fetches badly: the frontier jumps around the id space, so most of a fetched
tile is never used. On RMAT hybrid, a 4 KB block fetched 5,555 lines versus 1,373
for direct mode, and modeled latency rose from 0.288 ms to 1.167 ms. A 16 KB
block fetched 8,012 lines.

On the **regular grid**, block mode is neutral: the wavefront eventually uses
every line, and the whole graph fits in the cache, so direct and block both fetch
770 lines and have the same modeled latency.

This is the clearest memory-centric lesson so far: **spatial prefetching helps
regular access and hurts irregular access**. The best block size for graph BFS
is the cache line (i.e., direct mode).

### 8.3 Direction-optimizing BFS

On the RMAT graph, hybrid examined 3,576 edges versus 53,629 for top-down
(15× fewer) and 48,277 for pure bottom-up. It also fetched 1,373 lines versus
4,302 and 4,407. Direction optimization reduces **both** algorithmic work and
CXL traffic, because the bottom-up early exit avoids touching most edges.

Pure bottom-up is worse than top-down when the frontier is small: it scans all
`n` vertices every level and examines many edges before the first hit. This is
why the hybrid starts top-down.

## 9. M2 calibration results

Platform: WSL2, GCC 14.2.0, CXLMemSim `a9d46ec`, server over TCP with
`--default_latency=200` (≈210 ns per 64-byte read). Raw data:
`results/raw/m2_bfs.csv` (regenerate with `scripts/run_bfs_calibration.sh`).

### 9.1 Grid (`n = 2,500`, `m = 9,800`, top-down)

| Backend | Block | Cache | `t_compute` ms | `t_cxl_model` ms | `t_model` ms | Line fetches | Block fetches | Hits | Misses |
|---|---|---|---|---|---|---|---|---|---|
| dram | — | — | 0.075 | 0.000 | 0.075 | 0 | 0 | 0 | 0 |
| cxl-direct | — | 64 KB | 0.086 | 0.162 | 0.248 | 770 | 0 | 14,030 | 770 |
| cxl-block | 4 KB | 64 KB | 0.092 | 0.162 | 0.254 | 770 | 13 | 14,787 | 13 |

### 9.2 RMAT (`n = 4,096`, `m = 53,635`, source 2048)

| Backend | Kernel | `t_compute` ms | `t_cxl_model` ms | `t_model` ms | Edges | Vertex scans | Line fetches | Hits | Misses |
|---|---|---|---|---|---|---|---|---|---|
| dram | top-down | 0.346 | 0.000 | 0.346 | 53,629 | 0 | 0 | 0 | 0 |
| dram | bottom-up | 0.491 | 0.000 | 0.491 | 48,277 | 20,480 | 0 | 0 | 0 |
| dram | hybrid | 0.148 | 0.000 | 0.148 | 3,576 | 12,288 | 0 | 0 | 0 |
| cxl-direct | top-down | 0.295 | 0.903 | 1.198 | 53,629 | 0 | 4,302 | 55,359 | 4,302 |
| cxl-direct | bottom-up | 0.410 | 0.925 | 1.336 | 48,277 | 20,480 | 4,407 | 66,312 | 4,407 |
| cxl-direct | hybrid | 0.137 | 0.288 | 0.425 | 3,576 | 12,288 | 1,373 | 14,303 | 1,373 |

### 9.3 Cache-size sensitivity (RMAT hybrid, direct)

| Cache | `t_cxl_model` ms | Line fetches | Evictions |
|---|---|---|---|
| 8 KB | 0.424 | 2,017 | 1,761 |
| 32 KB | 0.305 | 1,453 | 685 |
| 64 KB | 0.288 | 1,373 | 93 |
| 256 KB | 0.288 | 1,371 | 0 |

The working set is about 230 KB (`row_ptr` 16 KB + `col_idx` 214 KB). 256 KB
holds it entirely; 64 KB is already close, so returns diminish.

### 9.4 Block-size sensitivity (RMAT hybrid)

| Block | `t_cxl_model` ms | Line fetches | Block fetches |
|---|---|---|---|
| direct (64 B) | 0.288 | 1,373 | 0 |
| 1 KB | 0.841 | 4,003 | 252 |
| 4 KB | 1.167 | 5,555 | 89 |
| 16 KB | 1.682 | 8,012 | 35 |

Larger blocks fetch more lines on every miss. The high block-cache hit rate
(98–99%) is misleading: it counts tile fills, not the lines wasted inside each
tile.

### 9.5 Observations

1. **Hybrid wins.** On RMAT, hybrid is 2.8× faster than top-down and 3.1× faster
   than bottom-up in `t_model`, and fetches 3.1× fewer CXL lines.
2. **Pure bottom-up needs a large frontier.** It examines slightly fewer edges
   than top-down but scans all `n` vertices every level, so it loses from a small
   frontier.
3. **Block prefetching is wrong for irregular graphs.** It over-fetches 3–6× the
   lines and multiplies modeled latency, despite a near-100% tile hit rate.
4. **Cache size helps until the working set fits.** 8 KB → 64 KB cuts fetches by
   32%; 256 KB gives no further benefit.
5. **CXL BFS is only ~3× DRAM, unlike the 30× streaming stencil.** CSR access has
   enough spatial locality that a modest software cache captures most of it.
   Irregularity alone does not doom CXL; the access pattern does.
6. **Transport is still large.** `t_cpu_run` is 45–250 ms and `t_wall` is
   150–820 ms, versus 0.3–1.7 ms of modeled CXL time. ADR 0002's separation is
   essential.

## 10. How to run

```bash
# Build
./scripts/build.sh

# Tests (CXL cases need the server)
./scripts/run_server.sh &
ctest --test-dir build --output-on-failure

# DRAM baseline, verify correctness
./build/bfs_bench --backend dram --graph rmat --n 4096 --edges 32768 \
    --seed 7 --source 2048 --kernel hybrid --verify

# CXL direct, hybrid
./build/bfs_bench --backend cxl-direct --graph rmat --n 4096 --edges 32768 \
    --seed 7 --source 2048 --kernel hybrid --cache-kb 64

# Block prefetching (shows over-fetch)
./build/bfs_bench --backend cxl-block --graph rmat --n 4096 --edges 32768 \
    --seed 7 --source 2048 --kernel hybrid --block-kb 4 --cache-kb 64

# Regenerate the M2 table
./scripts/run_bfs_calibration.sh
```

## 11. Limitations and next steps

- Vertex ids and CSR offsets are 32-bit, so graphs are limited to 4 billion
  vertices and edges.
- The benchmark builds the graph in host memory first, then copies it to CXL.
  Setup traffic is excluded from the measured counters.
- The software cache models CXL-side reuse, not host L1/L2/LLC. Cachegrind and
  optional Orion `perf stat` runs cover host cache behavior.
- The hybrid thresholds (`n/14`, `n/24`) are the classic constants and were not
  tuned per graph.
- The TCP client is synchronous, so wall time is dominated by round trips. Batch
  or pipelined requests are a possible extension.
- Next: graph reordering (degree-based or community-based) to improve CXL
  locality, and a comparison against SpMV's structured access.
