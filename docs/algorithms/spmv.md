# Sparse Matrix-Vector Multiplication (SpMV) — Core Algorithm

Status: implemented and validated (M3)
Last updated: 2026-11-09

## 1. Purpose

SpMV is one of the two core algorithms of this project (with BFS):

```text
y = A x
```

Given a sparse matrix `A` and a dense vector `x`, compute the product `y`. The
project studies which sparse format and which CXL placement make SpMV the most
memory-efficient.

The project has already seen that BFS is irregular, latency-bound, and benefits
from direction optimization. SpMV is the other hand of irregular sparse access,
and the choice of sparse format determines how much index traffic and cache
locality the CXL backend sees.

## 2. Problem

- `A` is sparse: `nnz` nonzeros out of `rows * cols` entries.
- `x` is dense: one double per column.
- `y` is dense: one double per row.
- Work unit: one nonzero multiply-add (`nnz`), recorded on the values array.

## 3. Sparse formats

All three formats represent the same logical matrix; the kernels must return
bit-equivalent (within 1e-9 relative) results.

| Format | Stores | Index size | Fill | Best for |
|---|---|---|---|---|
| CSR | `row_ptr[n+1]`, `col[nnz]`, `val[nnz]` | nnz | none | Row-wise access, sequential per row |
| SELL-C-σ | `slice_ptr[n_slices+1]`, `col`, `values` | `nnz_stored >= nnz` | Slices of sorted, padded rows | Similar-length rows, reduced bandwidth |
| BCSR | `row_ptr[n_block_rows+1]`, `block_col[nb]`, `values[nb*br*bc]` | dense tiles | Padding | Block-wise access, vectorization |

### 3.1 CSR

Layout:

```text
row_ptr: n + 1 uint32 offsets
col_idx: nnz uint32 columns
values:  nnz double values
```

The kernel (`spmv_csr`) is a direct row loop:

```cpp
for row in 0..rows-1:
    begin = row_ptr[row]
    end   = row_ptr[row + 1]
    sum = 0
    for e in begin..end:
        sum += values[e] * x[col_idx[e]]
    y[row] = sum
```

The whole row is read as one contiguous run, which maps directly onto the CSR
software cache.

### 3.2 SELL-C-σ

Rows are grouped into slices of `slice_height` rows, sorted by length inside
windows of `sigma`, and padded to the longest row in each slice. Within a slice
the layout is column-major:

```text
offset(slice, k, lane) = slice_ptr[slice] + k * slice_height + lane
```

where `k` indexes the k-th column of a row and `lane` the lane within the slice.
Invalid rows (past the end of the matrix) hold `row_id = kInvalidRow`.

To_sell builds:

1. An order vector sorted by decreasing row length inside each sigma-wide
   window.
2. Slice boundaries at multiples of `slice_height`, each slice padded to the
   longest row in it.
3. Entries copied per slice/lane/k; padding holds `col=0`, `value=0`.

The kernel (`spmv_sell`) accumulates per-lane partial sums and writes back per
valid lane. Padding slots are still looked up (contributing zero) — this is
exactly the waste being measured.

### 3.3 BCSR

A matrix is divided into `block_rows x block_cols` tiles. Each block row stores
a sorted list of occupied column blocks plus dense tile values. Duplicates are
summed into the same slot, matching the CSR reference.

The kernel (`spmv_bcsr`) walks block rows, accumulates one partial sum per row
inside the block, and scatters the result.

## 4. Data placement

For all formats the same split holds: the matrix arrays live in CXL through
`TieredArray`, the dense `x` and `y` stay in host DRAM.

| Array | Location | Reason |
|---|---|---|
| CSR/SSELL/BCSR index arrays | CXL (`TieredArray`) | Large, read-only, shared with algorithms |
| values | CXL (`TieredArray`) | `nnz` reads per product |
| `x` | DRAM | Hot dense vector |
| `y` | DRAM | Hot dense vector |

The values array is read once per nonzero, so for an `nnz`-sized product the CXL
traffic is dominated by the index arrays and the number of distinct cache lines.

## 5. Correctness

`tests/spmv_test.cpp` checks:

- **Laplacian structure:** 5-point grid has `nnz = 4*side*(side-1) + side^2`
  entries, diagonals equal to the row degree, off-diagonals `-1`, and the
  Laplacian annihilates the constant vector.
- **Generators:** RMAT and random matrices have consistent row pointers and
  in-range columns; empty matrices are valid.
- **Formats:** one-row-per-slice SELL stores exactly `nnz` entries (no padding);
  wider slices introduce padding; 1x1 BCSR reproduces CSR exactly; 2x2 BCSR pads
  to dense tiles. SELL row ids form a permutation of valid rows.
- **All formats** on several matrices (Laplacian, RMAT, random, empty rows, empty
  matrix) against the host reference, including formats whose block/slice shapes
  are larger than the matrix.
- **Exact work accounting:** `values` stats report `nnz` work units.
- **CXL backends:** direct and block, for all formats, match the reference and
  report nonzero CXL reads, line fetches, bytes, and modeled latency.

All five CTest cases pass, including the CXL cases when the server is running.

## 6. M3 calibration results

Platform: WSL2, GCC 14.2.0, CXLMemSim `a9d46ec`, server at 200 ns base latency
(≈210 ns per 64-byte read), TCP client, file-backed pool. Raw data:
`results/raw/m3_spmv.csv` (regenerate with `scripts/run_spmv_calibration.sh`).

### 6.1 Laplacian (`side = 64`, so 4096 x 4096, `nnz = 20,224`)

| Backend | Format | `t_compute` ms | `t_cxl_model` ms | `t_model` ms | Line fetches | Block fetches | Hits | Misses | Hit rate |
|---|---|---|---|---|---|---|---|---|---|
| dram | csr | 0.194 | 0.000 | 0.194 | 0 | 0 | 0 | 0 | 0 |
| cxl-direct | csr | 0.289 | 0.850 | **1.139** | 4,049 | 0 | 44,591 | 4,049 | **91.8%** |  |
| cxl-block | csr | 0.191 | 0.850 | **1.042** | 4,049 | 65 | 48,575 | 65 | 98.7% |  |

### 6.2 RMAT (`n = 4096`, `nnz = 65,536`, seed 7), CXL direct, 64 KB cache

| Backend | Format | Block | `t_compute` ms | `t_cxl_model` ms | `t_model` ms | Line fetches | Hits | Misses |
|---|---|---|---|---|---|---|---|---|
| dram | csr | — | 0.463 | 0.000 | 0.463 | 0 | 0 | 0 |
| cxl-direct | csr | — | 0.469 | 2.634 | **3.103** | 12,545 | 126,719 | 12,545 |
| cxl-direct | sell | 4 | 1.071 | 5.061 | **6.132** | 24,100 | 234,308 | 24,100 |
| cxl-direct | bcsr | 2x2 | 1.107 | 5.412 | **6.519** | 25,770 | 206,241 | 25,770 |
| cxl-direct | bcsr | 4x4 | 2.837 | 15.362 | **18.199** | 73,152 | 531,308 | 73,152 |

### 6.3 Random sparse (`rows = cols = 8000`, `nnz = 64,000`), CXL direct, 64 KB cache

| Backend | Format | Block | `t_cxl_model` ms | `t_model` ms | Line fetches | Hits | Misses |
|---|---|---|---|---|---|---|---|
| cxl-direct | csr | — | 2.625 | **3.242** | 12,501 | 131,499 | 12,501 |
| cxl-direct | sell | 4 | 3.137 | **3.888** | 14,936 | 147,024 | 14,936 |
| cxl-direct | bcsr | 4x4 | 27.512 | **32.416** | 131,011 | 951,792 | 131,011 |

## 7. Observations

1. **CSR is the best of the three on all matrices tested.** SELL and BCSR never
   beat CSR in `t_model` or line fetches on the RMAT and random matrices. The
   exception is block mode with a small block, which reduces modeled latency
   slightly for the Laplacian (1.042 vs 1.139) while fetching the same number of
   lines.

2. **SELL's layout hurts.** With `slice_height = 4` the column-major layout puts
   consecutive k values for one lane 16 bytes apart (stride `slice_height*4`),
   destroying cache-line locality inside the CXL software cache. Line fetches
   double versus CSR (24,100 vs 12,545 on RMAT), and padding (128,180 stored for
   65,536 logical entries) adds traffic without helping.

3. **BCSR padding is brutal at small block sizes.** A 2x2 block for a matrix with
   average degree ~16 fills most tiles densely anyway; 4x4 blocks store
   566,976 entries for 65,536 logical ones on RMAT and raise `t_model` from 3.1 ms
   to 18.2 ms. The BCSR kernel's dense-tile iteration amortizes well only for
   genuinely dense submatrices.

4. **CXL SpMV is roughly 6.7x DRAM.** CSR on RMAT: `t_model` 3.10 ms vs DRAM
   0.46 ms. This is the best CXL penalty seen so far, behind BFS's ~3x, because
   the CSR access pattern concentrates index traffic into contiguous row ranges
   that a 64 KB software cache captures.

5. **The software cache captures most reuse.** On RMAT CSR: 126,719 hits against
   12,545 misses (91%). The row-major layout means a row's whole index range is
   read from one cache unit.

6. **Block mode helps modestly.** On the Laplacian, block mode lowers
   `t_model` from 1.139 ms to 1.042 ms (7%) by coalescing reads; on irregular
   matrices it over-pays. With the server's 64-byte request cap, block mode
   models prefetch granularity, not link width.

7. **Transport dominates wall time.** `t_cpu_run` is 77–2,241 ms and `t_wall` is
   336–11,616 ms versus 0.2–18.2 ms of modeled CXL time, on SpMV workloads up to
   millions of cache-line round trips.

## 8. How to run

```bash
./scripts/build.sh
./scripts/run_server.sh &
ctest --test-dir build --output-on-failure
./build/spmv_bench --backend cxl-direct --matrix rmat --n 4096 --nnz 65536 \
    --seed 7 --format csr --verify
./scripts/run_spmv_calibration.sh
```

## 9. Limitations and next steps

- SELL's current implementation always iterates over the padded slice length, so
  padding cost is fully visible. A length-aware SELL that skips padding lanes
  would separate format quality from fill waste; the current result is still
  informative.
- BCSR's best case (dense blocks) is untested; dense matrices should be added.
- `nnz` and id ranges are 32-bit, limiting matrices to 4 billion entries.
- The TCP client is synchronous; batching/pipelining is a future optimization.
- Next: the same sweeps against SpMV on real SNAP/Graph500 matrices, then the
  final survey report.
