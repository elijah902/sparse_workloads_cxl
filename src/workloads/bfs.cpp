#include "workloads/bfs.h"

#include <algorithm>
#include <stdexcept>

namespace cxltest {
namespace {

// splitmix64: small, fast, deterministic. Used only for graph generation.
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

    std::uint64_t bounded(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }
};

std::uint32_t round_up_power_of_two(std::uint32_t value) {
    if (value <= 1) {
        return 1;
    }
    std::uint32_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

// Load helpers that record the algorithm's reads separately from the cache
// counters kept by TieredArray.
inline std::uint32_t load_row(TieredArray &row_ptr, std::size_t index, BfsStats &stats) {
    ++stats.row_ptr_reads;
    return row_ptr.load_value<std::uint32_t>(index);
}

inline std::uint32_t load_col(TieredArray &col_idx, std::size_t index, BfsStats &stats) {
    ++stats.col_idx_reads;
    return col_idx.load_value<std::uint32_t>(index);
}

void top_down_level(TieredArray &row_ptr, TieredArray &col_idx, const std::vector<std::uint32_t> &frontier,
                    std::vector<std::uint32_t> &next, std::vector<std::uint32_t> &distances,
                    std::uint32_t next_distance, BfsStats &stats) {
    next.clear();
    for (const std::uint32_t u : frontier) {
        const std::uint32_t begin = load_row(row_ptr, u, stats);
        const std::uint32_t end = load_row(row_ptr, u + 1, stats);
        for (std::uint32_t e = begin; e < end; ++e) {
            ++stats.edges_examined;
            const std::uint32_t v = load_col(col_idx, e, stats);
            if (distances[v] == kUnreachable) {
                distances[v] = next_distance;
                next.push_back(v);
            }
        }
    }
}

void bottom_up_level(TieredArray &row_ptr, TieredArray &col_idx, const std::vector<std::uint8_t> &in_frontier,
                     std::vector<std::uint32_t> &next, std::vector<std::uint32_t> &distances,
                     std::uint32_t next_distance, BfsStats &stats) {
    next.clear();
    const auto n = static_cast<std::uint32_t>(distances.size());
    for (std::uint32_t v = 0; v < n; ++v) {
        ++stats.vertices_scanned;
        if (distances[v] != kUnreachable) {
            continue;
        }
        const std::uint32_t begin = load_row(row_ptr, v, stats);
        const std::uint32_t end = load_row(row_ptr, v + 1, stats);
        for (std::uint32_t e = begin; e < end; ++e) {
            ++stats.edges_examined;
            const std::uint32_t w = load_col(col_idx, e, stats);
            if (in_frontier[w]) {
                distances[v] = next_distance;
                next.push_back(v);
                break;
            }
        }
    }
}

} // namespace

const char *to_string(BfsKernel kernel) {
    switch (kernel) {
    case BfsKernel::TopDown:
        return "topdown";
    case BfsKernel::BottomUp:
        return "bottomup";
    case BfsKernel::Hybrid:
        return "hybrid";
    }
    return "unknown";
}

CsrGraph build_csr(std::uint32_t num_vertices,
                   const std::vector<std::pair<std::uint32_t, std::uint32_t>> &edges, bool symmetric) {
    std::vector<std::vector<std::uint32_t>> adjacency(num_vertices);
    for (const auto &[u, v] : edges) {
        if (u >= num_vertices || v >= num_vertices) {
            continue;
        }
        adjacency[u].push_back(v);
        if (symmetric && u != v) {
            adjacency[v].push_back(u);
        }
    }

    CsrGraph graph;
    graph.num_vertices = num_vertices;
    graph.symmetric = symmetric;
    graph.row_ptr.resize(static_cast<std::size_t>(num_vertices) + 1);
    graph.row_ptr[0] = 0;

    for (std::uint32_t i = 0; i < num_vertices; ++i) {
        auto &row = adjacency[i];
        std::sort(row.begin(), row.end());
        row.erase(std::unique(row.begin(), row.end()), row.end());
        graph.row_ptr[i + 1] = graph.row_ptr[i] + static_cast<std::uint32_t>(row.size());
    }

    graph.num_edges = graph.row_ptr[num_vertices];
    graph.col_idx.resize(graph.num_edges);
    std::size_t cursor = 0;
    for (std::uint32_t i = 0; i < num_vertices; ++i) {
        for (const std::uint32_t v : adjacency[i]) {
            graph.col_idx[cursor++] = v;
        }
    }
    return graph;
}

CsrGraph make_grid_graph(std::uint32_t rows, std::uint32_t cols, bool symmetric) {
    if (rows == 0 || cols == 0) {
        throw std::invalid_argument("make_grid_graph: rows and cols must be > 0");
    }
    const std::uint64_t total = static_cast<std::uint64_t>(rows) * cols;
    if (total > 0xFFFFFFFFull) {
        throw std::invalid_argument("make_grid_graph: too many vertices for 32-bit ids");
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    edges.reserve(static_cast<std::size_t>(total) * 2);
    for (std::uint32_t r = 0; r < rows; ++r) {
        for (std::uint32_t c = 0; c < cols; ++c) {
            const std::uint32_t id = r * cols + c;
            if (r + 1 < rows) {
                edges.emplace_back(id, (r + 1) * cols + c);
            }
            if (c + 1 < cols) {
                edges.emplace_back(id, r * cols + c + 1);
            }
        }
    }
    return build_csr(static_cast<std::uint32_t>(total), edges, symmetric);
}

CsrGraph make_rmat_graph(std::uint32_t num_vertices, std::uint64_t num_edges, std::uint64_t seed,
                         bool symmetric) {
    const std::uint32_t n = round_up_power_of_two(num_vertices);
    if (n < 2) {
        throw std::invalid_argument("make_rmat_graph: num_vertices must be at least 2");
    }

    // Graph500 RMAT probabilities.
    constexpr double a = 0.57;
    constexpr double b = 0.19;
    constexpr double c = 0.19;

    SplitMix64 rng(seed);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    edges.reserve(static_cast<std::size_t>(num_edges));

    for (std::uint64_t e = 0; e < num_edges; ++e) {
        std::uint32_t u = 0;
        std::uint32_t v = 0;
        for (std::uint32_t bit = n >> 1; bit > 0; bit >>= 1) {
            const double r = rng.next_double();
            if (r < a) {
                // top-left: neither bit set
            } else if (r < a + b) {
                v |= bit;
            } else if (r < a + b + c) {
                u |= bit;
            } else {
                u |= bit;
                v |= bit;
            }
        }
        edges.emplace_back(u, v);
    }
    return build_csr(n, edges, symmetric);
}

CsrGraph make_random_graph(std::uint32_t num_vertices, std::uint32_t avg_degree, std::uint64_t seed,
                           bool symmetric) {
    if (num_vertices == 0) {
        throw std::invalid_argument("make_random_graph: num_vertices must be > 0");
    }
    SplitMix64 rng(seed);
    const std::uint64_t edge_count = static_cast<std::uint64_t>(num_vertices) * avg_degree;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    edges.reserve(static_cast<std::size_t>(edge_count));
    for (std::uint64_t i = 0; i < edge_count; ++i) {
        const auto u = static_cast<std::uint32_t>(rng.bounded(num_vertices));
        const auto v = static_cast<std::uint32_t>(rng.bounded(num_vertices));
        edges.emplace_back(u, v);
    }
    return build_csr(num_vertices, edges, symmetric);
}

std::vector<std::uint32_t> bfs_reference(const CsrGraph &graph, std::uint32_t source) {
    std::vector<std::uint32_t> distances(graph.num_vertices, kUnreachable);
    if (source >= graph.num_vertices) {
        return distances;
    }

    std::vector<std::uint32_t> queue;
    queue.reserve(graph.num_vertices);
    distances[source] = 0;
    queue.push_back(source);

    std::size_t head = 0;
    while (head < queue.size()) {
        const std::uint32_t u = queue[head++];
        for (std::uint32_t e = graph.row_ptr[u]; e < graph.row_ptr[u + 1]; ++e) {
            const std::uint32_t v = graph.col_idx[e];
            if (distances[v] == kUnreachable) {
                distances[v] = distances[u] + 1;
                queue.push_back(v);
            }
        }
    }
    return distances;
}

BfsResult bfs_run(TieredArray &row_ptr, TieredArray &col_idx, std::size_t num_vertices,
                  const BfsConfig &config) {
    BfsResult result;
    result.distances.assign(num_vertices, kUnreachable);

    if (num_vertices == 0 || config.source >= num_vertices) {
        return result;
    }
    if (config.kernel != BfsKernel::TopDown && !config.symmetric) {
        throw std::invalid_argument("bfs_run: bottom-up and hybrid require a symmetric graph");
    }

    const auto n = static_cast<std::uint32_t>(num_vertices);
    result.distances[config.source] = 0;

    std::vector<std::uint32_t> frontier{config.source};
    std::vector<std::uint32_t> next;
    std::vector<std::uint8_t> in_frontier(num_vertices, 0);
    in_frontier[config.source] = 1;

    BfsKernel mode = config.kernel;
    if (mode == BfsKernel::Hybrid) {
        mode = BfsKernel::TopDown;
    }

    // Beamer-style direction thresholds: switch to bottom-up when the frontier
    // is a large fraction of the graph, and back when it shrinks.
    const std::uint32_t switch_to_bottom_up = n / 14;
    const std::uint32_t switch_to_top_down = n / 24;

    std::uint32_t distance = 0;
    while (!frontier.empty()) {
        result.stats.max_frontier = std::max(result.stats.max_frontier, static_cast<std::uint32_t>(frontier.size()));
        ++result.stats.iterations;
        ++distance;

        if (config.kernel == BfsKernel::Hybrid) {
            if (mode == BfsKernel::TopDown && frontier.size() > switch_to_bottom_up) {
                mode = BfsKernel::BottomUp;
            } else if (mode == BfsKernel::BottomUp && frontier.size() < switch_to_top_down) {
                mode = BfsKernel::TopDown;
            }
        }

        if (mode == BfsKernel::TopDown) {
            top_down_level(row_ptr, col_idx, frontier, next, result.distances, distance, result.stats);
        } else {
            bottom_up_level(row_ptr, col_idx, in_frontier, next, result.distances, distance, result.stats);
        }

        // Rebuild the frontier bitmap for the next round.
        for (const std::uint32_t u : frontier) {
            in_frontier[u] = 0;
        }
        for (const std::uint32_t u : next) {
            in_frontier[u] = 1;
        }
        frontier.swap(next);
    }

    for (const std::uint32_t d : result.distances) {
        if (d != kUnreachable) {
            result.stats.max_distance = std::max(result.stats.max_distance, d);
        }
    }

    row_ptr.add_work_units(result.stats.edges_examined);
    return result;
}

} // namespace cxltest
