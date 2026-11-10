# k-Nearest Neighbors (k-NN) — Stretch Algorithm

Status: implemented and validated (M4)
Last updated: 2026-11-09

## 1. Purpose

k-NN is the stretch goal of the project. Given a dataset of vectors and a query
vector, return the `k` nearest neighbors by (squared Euclidean) distance.

The memory-centric question this stretch asks: when the dataset lives in CXL
memory and queries are local, does the algorithm's access pattern change the
CXL cost, and does tiling the dataset (block-style prefetch) help?

## 2. Problem

- Dataset: `num_vectors` x `vector_dim` dense vectors.
- Queries: `num_queries` dense vectors, held in host DRAM.
- For each query, keep the `k` smallest distances; return `(vector, distance)`.

Work unit: one distance computation (query x vector), recorded via
`TieredArray::add_work_units`.

## 3. Implementations

### 3.1 Brute force — `knn_run`

For each query, scan the dataset vector by vector; each vector is read through
the `TieredArray` (so CXL reads, line fetches, misses are counted) and scored.

### 3.2 Tiled — `knn_run_tiled`

For each query, scan the dataset in tiles of `tile_vectors` vectors; each tile is
read through the `TieredArray` into a host staging buffer, then scored.

Both implementations must return identical top-k lists; `knn_test` checks this
against `knn_reference` for many tile sizes.

## 4. Data placement

| Structure | Location |
|---|---|
| Dataset vectors | CXL (`TieredArray`) |
| Queries | Host DRAM |
| Top-k results | Host DRAM |

## 5. Correctness

`tests/knn_test.cpp` checks:

- Dataset/query generators are deterministic.
- Brute force and tiled kernels (tiles 1, 2, 3, 4, 8, 64 vectors) match the
  host reference on the host-tiered-array path.
- CXL direct and CXL block backends match the reference for brute force and
  tiled k-NN.

All tests pass (part of the six-case CTest suite).

## 6. M4 calibration results

Platform: WSL2, GCC 14.2.0, CXLMemSim `a9d46ec`, server at 200 ns base latency,
TCP, file-backed pool. Raw data: `results/raw/m4_knn.csv`
(regenerate with `scripts/run_knn_calibration.sh`).

Sweep: dataset 1024 vectors x 8 dims (65,536 bytes), 4 queries, k=2.

| Backend | Kernel | Block | Cache | `t_compute` ms | `t_cxl_model` ms | `t_model` ms | Line fetches | Hits | Misses | Hit rate | Bytes read |
|---|---|---|---|---|---|---|---|---|---|---|---|
| dram | brute | — | — | 0.087 | 0.000 | 0.087 | 0 | 0 | 0 | 0 | 0 |
| cxl-direct | brute | — | 64 KB | 0.207 | 0.215 | **0.422** | 1,024 | 3,072 | 1,024 | 75.0% | 65,536 |
| cxl-block | brute | 4 KB | 64 KB | 0.208 | 0.215 | **0.423** | 1,024 | 4,095 | 1 | 99.8% | 65,536 |
| cxl-direct | tile | 64 | 64 KB | 0.100 | 0.215 | **0.315** | 1,024 | 3,072 | 1,024 | 75.0% | 65,536 |
| cxl-direct | tile | 256 | 64 KB | 0.098 | 0.215 | **0.313** | 1,024 | 3,072 | 1,024 | 75.0% | 65,536 |
| cxl-block | tile | 4 KB / 32 | 64 KB | 0.101 | 0.215 | **0.316** | 1,024 | 127 | 1 | 99.2% | 65,536 |

## 7. Observations

1. **CXL k-NN penalty is modest.** On a 1024-vector dataset with 4 queries, the
   brute-force CXL run is ~4.8x the DRAM compute in `t_model` (0.42 ms vs 0.09 ms).
2. **Block mode improves cache reuse.** With 4 KB blocks the brute-force run has
   99.8% hit rate and a single miss; with direct mode (64-byte fetch units) and
   per-vector reads it has a 75% hit rate with 1,024 misses. The block fetch unit
   coalesces a tile before the software cache, so the first vector read pulls in
   the whole block and the rest hit.
3. **Tiling does not change CXL traffic.** Tiles of 32, 64, and 256 vectors all
   produce the same line fetches and similar `t_model`. Once the working set fits
   in the software cache, further tiling is free.
4. **Transport dominates wall time.** `t_cpu_run` is 36–48 ms and `t_wall` is
   194–246 ms versus 0.2–0.4 ms of modeled CXL time, on workloads of only a few
   thousand distance computations.

## 8. How to run

```bash
./scripts/build.sh
./scripts/run_server.sh &
ctest --test-dir build --output-on-failure
./build/knn_bench --backend cxl-block --kernel brute --n 1024 --dim 8 --queries 4 --k 2 --verify
./scripts/run_knn_calibration.sh
```

## 9. Limitations and next steps

- The dataset is synthetic; real SNAP/Graph500-scale vectors remain for the final
  experiments.
- k-NN results are exact; no ANN index was compared.
- Next: fold the best k-NN configuration into the final survey report and move to
  M5 (cross-algorithm analysis and final report).
