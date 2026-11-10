# 2D Stencil — Calibration Workload

Status: implemented and validated (M1)
Last updated: 2026-11-09

## 1. Purpose

The 2D stencil is the calibration and control workload. It is regular,
predictable, and easy to verify, so it is used to validate:

- the `CXLClient` transport and the `TieredArray` backends,
- the software cache and its counters,
- the direct versus block fetch paths,
- the timing model from ADR 0002.

It is **not** one of the core algorithms. BFS and SpMV are the core algorithms;
k-NN is the stretch goal.

## 2. Problem

A 5-point stencil over an `n × n` grid of `float` values:

```text
next[r][c] = 0.25 * (cur[r-1][c] + cur[r+1][c] + cur[r][c-1] + cur[r][c+1])
```

Boundary cells (`r == 0`, `r == n-1`, `c == 0`, or `c == n-1`) are copied
unchanged. This is a Dirichlet-style fixed boundary.

The grid uses two planes to avoid an in-place read/write race:

```text
plane 0: current input
plane 1: next output
```

Each step reads one plane and writes the other, then swaps their roles. After
`steps` iterations the result is in plane `steps % 2` (plane 0 when
`steps == 0`).

## 3. Data layout

The grid is one `TieredArray` holding `2 * n * n * sizeof(float)` bytes:

```text
offset 0              plane 0: n*n floats
offset n*n*4          plane 1: n*n floats
```

For `n = 256`, each plane is 256 KB and the grid is 512 KB. For `n = 2048`, each
plane is 16 MB and the grid is 32 MB.

The array is placed in one backend:

| Backend | Backing store | Fetch unit |
|---|---|---|
| `dram` | Host DRAM copy | None (memcpy) |
| `cxl-direct` | CXL pool | 64-byte line |
| `cxl-block` | CXL pool | `block_bytes` tile |

## 4. Kernels

### 4.1 Naive (`StencilKernel::Naive`)

One `load_value`/`store_value` per cell:

```cpp
for (r, c) {
    if (boundary) {
        value = grid.load_value<float>(src + i);
    } else {
        value = stencil_update(grid.load_value<float>(src + i - 1),
                               grid.load_value<float>(src + i + 1),
                               grid.load_value<float>(src + i - n),
                               grid.load_value<float>(src + i + n));
    }
    grid.store_value<float>(dst + i, value);
}
```

This is the counter-calibration kernel. It exposes every individual access, so
cache hits, misses, and line fetches can be checked exactly. It also shows the
cost of uncoalesced writes: every cell is a separate CXL write request.

### 4.2 Row-blocked (`StencilKernel::RowBlocked`)

Three row buffers (`top`, `middle`, `bottom`) and one output row buffer live in
host DRAM:

```cpp
read row 0 into middle;
for (r) {
    read row r+1 into bottom;
    for (c) {
        out[c] = boundary ? middle[c]
                          : stencil_update(middle[c-1], middle[c+1], top[c], bottom[c]);
    }
    write out to row r of the destination plane;
    rotate(top, middle, bottom);
}
```

Each source row is read once and each destination row is written once. The
per-row `read`/`write` calls coalesce CXL traffic into 64-byte requests. This is
the realistic stencil kernel and the one used for the calibration sweep.

## 5. Code walkthrough

### 5.1 Public API — `src/workloads/stencil.h`

| Declaration | Meaning |
|---|---|
| `StencilKernel` | Selects the naive or row-blocked kernel |
| `StencilConfig` | `n`, `steps`, `kernel` |
| `stencil_run(grid, config)` | Runs the kernel; returns the final plane index |
| `stencil_reference(input, config)` | Host-only reference over one plane |
| `stencil_read_plane(grid, n, plane)` | Reads a plane for verification |
| `fnv1a` | 64-bit FNV-1a checksum |
| `stencil_checksum(grid, n, plane)` | Checksums a plane through the array |

### 5.2 Update expression — `src/workloads/stencil.cpp`

```cpp
inline float stencil_update(float left, float right, float up, float down) {
    return 0.25f * (((left + right) + up) + down);
}
```

The reference and both kernels call this same function with the same argument
order, so floating-point results are bit-identical on one build.

### 5.3 Naive loop

`stencil_run` computes `plane_elems = n*n`, validates that the grid holds two
planes, then loops:

```cpp
for (std::uint32_t step = 0; step < config.steps; ++step) {
    const std::size_t src = current * plane_elems;
    const std::size_t dst = (1 - current) * plane_elems;
    for (std::size_t r = 0; r < n; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
            const std::size_t i = r * n + c;
            ...
        }
    }
    current = 1 - current;
}
```

`src` and `dst` are element offsets, not byte offsets. `load_value<float>(index)`
multiplies by `sizeof(float)` internally.

### 5.4 Row-blocked loop

```cpp
grid.read(src * sizeof(float), middle.data(), n * sizeof(float));
for (std::size_t r = 0; r < n; ++r) {
    if (r + 1 < n) {
        grid.read((src + (r + 1) * n) * sizeof(float), bottom.data(), n * sizeof(float));
    }
    ...
    grid.write((dst + r * n) * sizeof(float), out.data(), n * sizeof(float));
    std::swap(top, middle);
    std::swap(middle, bottom);
}
```

The rotations keep `middle` on row `r`, `top` on row `r-1`, and `bottom` on row
`r+1`. At `r == 0` and `r == n-1` the missing row is never used because the
boundary path copies `middle[c]`.

### 5.5 Transport path — `src/memory/tiered_array.cpp`

Every `load_value`/`read`/`write` goes through `TieredArray`:

- `read(offset, dst, len)` splits the request by the fetch unit (`64` bytes for
  direct, `block_bytes` for block).
- `cache_->lookup(line_id)` updates hit/miss counters.
- On a miss, `fetch_line(line_id)` issues one or more `client_->read` calls of at
  most 64 bytes, sums the returned latency, and inserts the unit into the cache.
- `write(offset, src, len)` is write-through: it splits by 64-byte lines, issues
  `client_->write` calls, and invalidates the touched fetch unit.
- `stats_` accumulates CXL requests, bytes, line fetches, block fetches, cache
  events, and modeled nanoseconds.

### 5.6 Tests — `tests/stencil_test.cpp`

The tests use `n = 32`, `steps = 2` (two 4 KB planes) and check both the result
and exact counters. The reference comparison uses a tolerance of `1e-6`; the
counters are deterministic.

### 5.7 Benchmark — `apps/stencil_bench.cpp`

The benchmark:

1. Connects to the server for CXL backends.
2. Measures a 3-run DRAM reference of the same kernel (median CPU time).
3. Reloads both planes, resets counters, and runs the measured kernel.
4. Reports `t_compute`, `t_cxl_model`, `t_model`, `t_cpu_run`,
   `t_transport_cpu`, and `t_wall` (ADR 0002).

## 6. Memory behavior

### 6.1 Direct mode

A row of `n` floats is `4n` bytes. For `n = 32`, a row is 128 bytes = two
64-byte lines. A naive cell access fetches the line containing it; neighboring
cells in the same row hit. Vertical neighbors are in other lines, but a small
cache keeps the three-row working set.

### 6.2 Block mode

A block fetch reads `block_bytes` as consecutive 64-byte requests. Because the
server protocol caps a request at 64 bytes, the modeled latency of a block is
the sum of its line latencies. Block mode therefore models **prefetch
granularity**, not a wider link. This is a documented limitation, not a bug.

For the row-blocked kernel, a 4 KB block covers 16 rows at `n = 256` (each row
is 1 KB), so one block fetch serves 16 row reads. The line-fetch count is
unchanged, but cache hits rise and block fetches fall.

### 6.3 Cache behavior

The row-blocked kernel reads each source line exactly once per step, so a
software cache cannot improve it: the cache either fills and evicts (small
capacity) or stays full without hits (large capacity). The naive kernel has
intra-row and three-row reuse, so it produces many hits; the row-blocked kernel
captures the same reuse explicitly in DRAM row buffers.

This is a useful control result: blocking can replace a cache, and a cache does
not help a pure streaming pass.

## 7. Correctness and calibration

The tests assert both the numerical result and exact counters for `n = 32`,
`steps = 2`:

| Kernel | Backend | Reads | Misses | Hits | Line fetches | Block fetches | Writes |
|---|---|---|---|---|---|---|---|
| naive | cxl-direct | 7,448 | 128 | 7,320 | 128 | 0 | 2,048 |
| naive | cxl-block (4 KB) | 7,448 | 2 | 7,446 | 128 | 2 | 2,048 |
| row | cxl-direct | 128 | 128 | 0 | 128 | 0 | 128 |
| row | cxl-block (4 KB) | 64 | 2 | 62 | 128 | 2 | 128 |

Reads count cache lookups. For the naive kernel each boundary cell does one
lookup and each interior cell does four; for `n = 32` that is `124 + 900*4 =
3,724` lookups per step. The row kernel does one row read per row: 32 lookups
per step in block mode, and 64 in direct mode because a 128-byte row spans two
64-byte lines. Writes are one per cell for the naive kernel and one per row
(64-byte requests) for the row kernel.

The exact counts are also a check on the transport: a change in the protocol or
the cache invalidates the test.

## 8. Calibration results (M1, 200 ns base latency)

Server: `--capacity=64 --default_latency=200`, file-backed, TCP. Client:
`n = 256`, `steps = 4`, row-blocked unless noted. `t_compute` is the median DRAM
reference; `t_cxl_model` is the server's modeled latency.

| Backend | Kernel | Block | Cache | `t_compute` ms | `t_cxl_model` ms | `t_model` ms | `t_cpu_run` ms | `t_wall` ms | Line fetches | Block fetches | Hits | Misses |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| dram | row | — | — | 0.252 | 0.000 | 0.252 | 0.252 | 0.245 | 0 | 0 | 0 | 0 |
| cxl-direct | row | — | 32 KB | 0.181 | 7.496 | 7.677 | 1008.5 | 2878.0 | 16,384 | 0 | 0 | 16,384 |
| cxl-direct | row | — | 512 KB | 0.234 | 7.496 | 7.730 | 1112.8 | 3172.2 | 16,384 | 0 | 0 | 16,384 |
| cxl-block | row | 64 KB | 64 KB | 0.211 | 7.496 | 7.707 | 1028.7 | 2966.1 | 16,384 | 16 | 1,008 | 16 |
| cxl-block | row | 4 KB | 32 KB | 0.216 | 7.496 | 7.712 | 899.2 | 2635.5 | 16,384 | 256 | 768 | 256 |
| dram | naive | — | — | 0.674 | 0.000 | 0.674 | 0.674 | 0.665 | 0 | 0 | 0 | 0 |
| cxl-direct | naive (`n=128`) | — | 16 KB | 0.697 | 7.100 | 7.797 | 1145.7 | 3454.6 | 2,048 | 0 | 125,976 | 2,048 |

Observations:

- The CXL row kernel costs about 30× the DRAM kernel in `t_model`, almost all of
  it modeled memory latency.
- Cache capacity changes evictions (15,872 → 0) but not hits, because the row
  kernel is a streaming pass. Cache size alone does not help a streaming kernel.
- Block mode reduces block fetches but not line fetches or `t_cxl_model`: the
  server caps requests at 64 bytes.
- The naive kernel captures reuse in the cache (125,976 hits) and fetches only
  2,048 lines, but its per-cell writes (32,768 requests) dominate. Coalescing
  writes matters more than read caching here.
- Transport CPU and wall time are orders of magnitude above the modeled CXL
  time. That is why ADR 0002 separates `t_compute` from `t_cpu_run` and `t_wall`.

The `t_compute` values are sub-millisecond and noisy; the M2/M3 experiment
scripts will use larger grids and more repetitions and report median/IQR.

## 9. How to run

```bash
# Build
./scripts/build.sh

# Tests (CXL cases need the server; they are skipped otherwise)
./scripts/run_server.sh &
ctest --test-dir build --output-on-failure

# One DRAM run
./build/stencil_bench --backend dram --kernel row --n 256 --steps 4 --verify

# One CXL run
./build/stencil_bench --backend cxl-direct --kernel row --n 256 --steps 4 --cache-kb 32

# CSV for scripts
./build/stencil_bench --backend cxl-block --kernel row --n 256 --steps 4 \
    --block-kb 4 --cache-kb 32 --csv
```

Regenerate the M1 table:

```bash
./scripts/run_stencil_calibration.sh
```

## 10. Limitations and next steps

- The server caps a request at 64 bytes, so block mode does not model a wide
  transfer. A separate analytical wide-transfer model is deferred.
- The cache is a software model; it does not model host L1/L2/LLC. Cachegrind and
  optional Orion `perf stat` runs cover host cache behavior on small DRAM runs.
- The stencil is a control workload. The core algorithm documents are
  `bfs.md` and `spmv.md`.
