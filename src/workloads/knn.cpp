#include "workloads/knn.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cxltest {
namespace {

// splitmix64: deterministic generator used only for dataset construction.
struct SplitMix64 {
    std::uint64_t state;

    explicit SplitMix64(std::uint64_t seed) : state(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    double next_double() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
};

double squared_distance(const std::vector<double> &a, const std::vector<double> &b) {
    double sum = 0.0;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const double diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

void fill_best(std::vector<KnnNeighbor> &best, std::uint32_t index, double distance, std::uint32_t k) {
    best.emplace_back(index, distance);
    if (best.size() > k) {
        std::sort(best.begin(), best.end(), [](const KnnNeighbor &a, const KnnNeighbor &b) {
            if (a.second != b.second) {
                return a.second < b.second;
            }
            return a.first < b.first;
        });
        best.resize(k);
    }
}

} // namespace

KnnResult knn_reference(const std::vector<std::vector<double>> &dataset,
                        const std::vector<std::vector<double>> &queries, const KnnConfig &config) {
    KnnResult result;
    result.top_k.resize(queries.size());

    for (std::size_t q = 0; q < queries.size(); ++q) {
        std::vector<KnnNeighbor> best;
        best.reserve(std::min<std::size_t>(config.num_vectors, 64));
        for (std::uint32_t i = 0; i < config.num_vectors; ++i) {
            const auto &vector = dataset[static_cast<std::size_t>(i)];
            if (vector.size() != config.vector_dim) {
                throw std::invalid_argument("knn_reference: dataset vector size mismatch");
            }
            fill_best(best, i, squared_distance(queries[q], vector), config.k);
        }
        if (best.size() > config.k) {
            best.resize(config.k);
        }
        result.top_k[q] = std::move(best);
    }
    return result;
}

KnnResult knn_run(TieredArray &data, const std::vector<std::vector<double>> &queries, const KnnConfig &config) {
    if (queries.size() > static_cast<std::size_t>(0xFFFFFFFF)) {
        throw std::invalid_argument("knn_run: too many queries");
    }

    KnnResult result;
    result.top_k.resize(queries.size());

    std::uint64_t computations = 0;

    for (std::size_t q = 0; q < queries.size(); ++q) {
        std::vector<KnnNeighbor> best;
        best.reserve(64);
        for (std::uint32_t i = 0; i < config.num_vectors; ++i) {
            std::vector<double> vector(config.vector_dim);
            data.read(static_cast<std::size_t>(i) * config.vector_dim * sizeof(double), vector.data(),
                      vector.size() * sizeof(double));
            const double distance = squared_distance(queries[q], vector);
            computations += 1;
            fill_best(best, i, distance, config.k);
        }

        if (best.size() > config.k) {
            best.resize(config.k);
        }
        result.top_k[q] = std::move(best);
    }

    data.add_work_units(computations);
    return result;
}

KnnResult knn_run_tiled(TieredArray &data, const std::vector<std::vector<double>> &queries, const KnnConfig &config,
                       std::uint32_t tile_vectors) {
    if (tile_vectors == 0 || tile_vectors > config.num_vectors) {
        throw std::invalid_argument("knn_run_tiled: tile_vectors must be in [1, num_vectors]");
    }

    KnnResult result;
    result.top_k.resize(queries.size());

    std::uint64_t computations = 0;

    for (std::size_t q = 0; q < queries.size(); ++q) {
        std::vector<KnnNeighbor> best;
        best.reserve(64);

        for (std::uint32_t start = 0; start < config.num_vectors; start += tile_vectors) {
            const std::uint32_t end = std::min(config.num_vectors, start + tile_vectors);
            const std::size_t tile_bytes =
                static_cast<std::size_t>(end - start) * config.vector_dim * sizeof(double);
            std::vector<uint8_t> buffer(tile_bytes);
            data.read(static_cast<std::size_t>(start) * config.vector_dim * sizeof(double), buffer.data(),
                      tile_bytes);
            const auto *values = reinterpret_cast<const double *>(buffer.data());

            for (std::uint32_t i = start; i < end; ++i) {
                const double *vector = values + static_cast<std::size_t>(i - start) * config.vector_dim;
                double sum = 0.0;
                for (std::uint32_t d = 0; d < config.vector_dim; ++d) {
                    const double diff = queries[q][d] - vector[d];
                    sum += diff * diff;
                }
                computations += 1;
                fill_best(best, i, sum, config.k);
            }
        }

        if (best.size() > config.k) {
            best.resize(config.k);
        }
        result.top_k[q] = std::move(best);
    }

    data.add_work_units(computations);
    return result;
}

std::vector<std::vector<double>> make_knn_dataset(std::uint32_t num_vectors, std::uint32_t vector_dim,
                                                 std::uint64_t seed) {
    if (num_vectors == 0) {
        throw std::invalid_argument("make_knn_dataset: num_vectors must be > 0");
    }
    std::vector<std::vector<double>> dataset(num_vectors, std::vector<double>(vector_dim, 0.0));
    SplitMix64 rng(seed);
    for (auto &vector : dataset) {
        for (auto &value : vector) {
            value = static_cast<double>(rng.next_double());
        }
    }
    return dataset;
}

std::vector<std::vector<double>> make_knn_queries(std::uint32_t num_queries, std::uint32_t vector_dim,
                                                  std::uint64_t seed) {
    if (num_queries == 0) {
        throw std::invalid_argument("make_knn_queries: num_queries must be > 0");
    }
    std::vector<std::vector<double>> queries(num_queries, std::vector<double>(vector_dim, 0.0));
    SplitMix64 rng(seed + 1);
    for (auto &query : queries) {
        for (auto &value : query) {
            value = static_cast<double>(rng.next_double());
        }
    }
    return queries;
}

} // namespace cxltest
