#pragma once

#include "memory/tiered_array.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace cxltest {

struct KnnConfig {
    std::uint32_t k = 1;
    std::uint32_t num_vectors = 0;
    std::uint32_t vector_dim = 0;
};

// One candidate: nearest-neighbor index plus the squared distance.
using KnnNeighbor = std::pair<std::uint32_t, double>;

struct KnnResult {
    std::vector<std::vector<KnnNeighbor>> top_k; // one best-k list per query
};

// Host-side brute-force k-NN over a dataset of full vectors.
KnnResult knn_reference(const std::vector<std::vector<double>> &dataset,
                        const std::vector<std::vector<double>> &queries, const KnnConfig &config);

// Brute-force k-NN. Each query scans the full dataset; every vector is read
// through the tiered array, so CXL reads, line fetches, and misses are counted.
KnnResult knn_run(TieredArray &data, const std::vector<std::vector<double>> &queries, const KnnConfig &config);

// Tiled k-NN. Each query scans the dataset in tiles of `tile_vectors` vectors;
// every tile is read through the tiered array into a host staging buffer before
// the distance computation runs. This is the block-mode style access pattern.
KnnResult knn_run_tiled(TieredArray &data, const std::vector<std::vector<double>> &queries,
                        const KnnConfig &config, std::uint32_t tile_vectors);

// Deterministic dataset and query generators (vectors are unit-scaled noise).
std::vector<std::vector<double>> make_knn_dataset(std::uint32_t num_vectors, std::uint32_t vector_dim,
                                                  std::uint64_t seed);
std::vector<std::vector<double>> make_knn_queries(std::uint32_t num_queries, std::uint32_t vector_dim,
                                                  std::uint64_t seed);

} // namespace cxltest
