#pragma once

#include "memory/tiered_array.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace cxltest {

// Distance value for an unreachable vertex.
constexpr std::uint32_t kUnreachable = 0xFFFFFFFFu;

enum class BfsKernel {
    TopDown,  // Scan the outgoing edges of each frontier vertex.
    BottomUp, // Scan unvisited vertices and test for a frontier neighbor.
    Hybrid,   // Switch direction based on frontier size.
};

const char *to_string(BfsKernel kernel);

struct BfsConfig {
    BfsKernel kernel = BfsKernel::TopDown;
    std::uint32_t source = 0;

    // Bottom-up and hybrid BFS test incoming edges by reading the stored
    // reverse direction, so they require a symmetric graph. Top-down works on
    // either. Set to false only for top-down on a directed graph.
    bool symmetric = true;
};

struct BfsStats {
    std::uint64_t edges_examined = 0;   // Outgoing edges visited
    std::uint64_t vertices_scanned = 0; // Vertices tested (bottom-up only)
    std::uint64_t row_ptr_reads = 0;    // TieredArray reads of row_ptr
    std::uint64_t col_idx_reads = 0;    // TieredArray reads of col_idx
    std::uint32_t iterations = 0;       // BFS expansion rounds
    std::uint32_t max_distance = 0;     // Largest finite distance
    std::uint32_t max_frontier = 0;     // Largest frontier size
};

struct BfsResult {
    std::vector<std::uint32_t> distances;
    BfsStats stats;
};

// A CSR graph on the host. The measured kernels read the same arrays through
// TieredArray; this copy is used to build the graph and to verify results.
struct CsrGraph {
    std::size_t num_vertices = 0;
    std::size_t num_edges = 0;
    bool symmetric = false;
    std::vector<std::uint32_t> row_ptr; // num_vertices + 1
    std::vector<std::uint32_t> col_idx; // num_edges
};

// Build CSR from an edge list. Self-loops are kept; duplicate edges per row are
// removed. When `symmetric` is true, every edge (u, v) also stores (v, u).
CsrGraph build_csr(std::uint32_t num_vertices,
                   const std::vector<std::pair<std::uint32_t, std::uint32_t>> &edges,
                   bool symmetric);

// Deterministic graph generators.
CsrGraph make_grid_graph(std::uint32_t rows, std::uint32_t cols, bool symmetric = true);
CsrGraph make_rmat_graph(std::uint32_t num_vertices, std::uint64_t num_edges, std::uint64_t seed,
                         bool symmetric = true);
CsrGraph make_random_graph(std::uint32_t num_vertices, std::uint32_t avg_degree, std::uint64_t seed,
                           bool symmetric = true);

// Host reference BFS.
std::vector<std::uint32_t> bfs_reference(const CsrGraph &graph, std::uint32_t source);

// Run BFS with row_ptr and col_idx served by the tiered arrays. The frontier,
// visited bitmap, and distances stay in host DRAM.
//
// Work units are recorded as edges examined (on row_ptr's stats).
BfsResult bfs_run(TieredArray &row_ptr, TieredArray &col_idx, std::size_t num_vertices,
                  const BfsConfig &config);

} // namespace cxltest
