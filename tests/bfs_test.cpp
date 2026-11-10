#include "cxl/cxl_client.h"
#include "workloads/bfs.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void check_impl(bool condition, const char *expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, expression);
        ++g_failures;
    }
}

#define CHECK(condition) check_impl((condition), #condition, __LINE__)

struct Arrays {
    std::unique_ptr<cxltest::TieredArray> row_ptr;
    std::unique_ptr<cxltest::TieredArray> col_idx;
};

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

Arrays build_arrays(const cxltest::CsrGraph &graph, cxltest::Backend backend, std::uint64_t base,
                    cxltest::CXLClient *client, std::size_t block_bytes, std::size_t cache_bytes) {
    const std::size_t row_bytes = graph.row_ptr.size() * sizeof(std::uint32_t);
    const std::size_t col_bytes = graph.col_idx.size() * sizeof(std::uint32_t);

    cxltest::TieredArrayConfig row_config;
    row_config.backend = backend;
    row_config.size_bytes = std::max<std::size_t>(row_bytes, 4);
    row_config.cxl_base_addr = base;
    row_config.block_bytes = block_bytes;
    row_config.cache_bytes = cache_bytes;

    cxltest::TieredArrayConfig col_config = row_config;
    col_config.size_bytes = std::max<std::size_t>(col_bytes, 4);
    col_config.cxl_base_addr = base + align_up(row_bytes, 64);

    Arrays arrays;
    arrays.row_ptr = std::make_unique<cxltest::TieredArray>(row_config, client);
    arrays.col_idx = std::make_unique<cxltest::TieredArray>(col_config, client);

    arrays.row_ptr->load(0, graph.row_ptr.data(), row_bytes);
    arrays.col_idx->load(0, graph.col_idx.data(), col_bytes);
    arrays.row_ptr->reset_stats();
    arrays.col_idx->reset_stats();
    return arrays;
}

std::uint64_t expected_top_down_edges(const cxltest::CsrGraph &graph,
                                      const std::vector<std::uint32_t> &distances) {
    std::uint64_t total = 0;
    for (std::uint32_t v = 0; v < graph.num_vertices; ++v) {
        if (distances[v] != cxltest::kUnreachable) {
            total += graph.row_ptr[v + 1] - graph.row_ptr[v];
        }
    }
    return total;
}

std::size_t reachable_count(const std::vector<std::uint32_t> &distances) {
    std::size_t count = 0;
    for (const std::uint32_t d : distances) {
        if (d != cxltest::kUnreachable) {
            ++count;
        }
    }
    return count;
}

std::uint32_t max_distance(const std::vector<std::uint32_t> &distances) {
    std::uint32_t worst = 0;
    for (const std::uint32_t d : distances) {
        if (d != cxltest::kUnreachable) {
            worst = std::max(worst, d);
        }
    }
    return worst;
}

cxltest::CsrGraph path_graph(std::uint32_t n, std::uint32_t begin, std::uint32_t end) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t i = begin; i + 1 < end; ++i) {
        edges.emplace_back(i, i + 1);
    }
    return cxltest::build_csr(n, edges, true);
}

void test_builders() {
    const auto grid = cxltest::make_grid_graph(4, 4, true);
    CHECK(grid.num_vertices == 16);
    // 4x4 grid: 24 undirected edges (12 horizontal + 12 vertical), stored both
    // ways, so 48 directed entries.
    CHECK(grid.num_edges == 48);
    CHECK(grid.symmetric);
    CHECK(grid.row_ptr.size() == 17);
    CHECK(grid.row_ptr[16] == grid.num_edges);

    const auto rmat = cxltest::make_rmat_graph(64, 256, 12345, true);
    CHECK(rmat.num_vertices == 64);
    CHECK(rmat.num_edges > 0);
    CHECK(rmat.symmetric);

    const auto random = cxltest::make_random_graph(100, 6, 99, true);
    CHECK(random.num_vertices == 100);
    CHECK(random.num_edges > 0);
    CHECK(random.symmetric);
}

void test_reference() {
    const auto path = path_graph(10, 0, 10);
    const auto path_dist = cxltest::bfs_reference(path, 0);
    for (std::uint32_t i = 0; i < 10; ++i) {
        CHECK(path_dist[i] == i);
    }

    // Two disconnected paths: vertices 6..9 are unreachable from 0.
    const auto split = path_graph(12, 0, 5);
    const auto split_dist = cxltest::bfs_reference(split, 0);
    CHECK(split_dist[4] == 4);
    CHECK(split_dist[5] == cxltest::kUnreachable);
    CHECK(split_dist[11] == cxltest::kUnreachable);

    // Star: center 0, leaves 1..15 at distance 1.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> star_edges;
    for (std::uint32_t i = 1; i < 16; ++i) {
        star_edges.emplace_back(0, i);
    }
    const auto star = cxltest::build_csr(16, star_edges, true);
    const auto star_dist = cxltest::bfs_reference(star, 0);
    CHECK(star_dist[0] == 0);
    for (std::uint32_t i = 1; i < 16; ++i) {
        CHECK(star_dist[i] == 1);
    }

    // Single vertex with a self-loop.
    const auto loop = cxltest::build_csr(1, {{0, 0}}, true);
    const auto loop_dist = cxltest::bfs_reference(loop, 0);
    CHECK(loop_dist[0] == 0);

    // Source out of range.
    const auto none = cxltest::bfs_reference(path, 999);
    CHECK(none[0] == cxltest::kUnreachable);
}

void check_kernel(const cxltest::CsrGraph &graph, cxltest::BfsKernel kernel, cxltest::Backend backend,
                  cxltest::CXLClient *client, std::uint64_t &base) {
    const std::size_t block_bytes = backend == cxltest::Backend::CxlBlock ? 4096 : 64 * 1024;
    const std::size_t cache_bytes = backend == cxltest::Backend::Dram ? 0 : 64 * 1024;
    const std::uint64_t array_base = base;
    base += 0x00100000;

    auto arrays = build_arrays(graph, backend, array_base, client, block_bytes, cache_bytes);
    const cxltest::BfsConfig config{kernel, 0, graph.symmetric};
    const cxltest::BfsResult result =
        cxltest::bfs_run(*arrays.row_ptr, *arrays.col_idx, graph.num_vertices, config);

    const auto reference = cxltest::bfs_reference(graph, 0);
    CHECK(result.distances == reference);
    CHECK(result.stats.col_idx_reads == result.stats.edges_examined);
    CHECK(result.stats.max_distance == max_distance(reference));
    CHECK(result.stats.max_frontier >= 1);

    if (kernel == cxltest::BfsKernel::TopDown) {
        CHECK(result.stats.edges_examined == expected_top_down_edges(graph, reference));
        CHECK(result.stats.vertices_scanned == 0);
        CHECK(result.stats.row_ptr_reads == 2 * reachable_count(reference));
    }
    if (kernel == cxltest::BfsKernel::BottomUp) {
        CHECK(result.stats.vertices_scanned ==
              static_cast<std::uint64_t>(graph.num_vertices) * result.stats.iterations);
    }

    if (backend != cxltest::Backend::Dram) {
        cxltest::MemoryStats stats = arrays.row_ptr->stats();
        stats.add(arrays.col_idx->stats());
        CHECK(stats.cxl_reads > 0);
        CHECK(stats.line_fetches > 0);
        CHECK(stats.modeled_ns > 0);
        CHECK(stats.cxl_bytes_read > 0);
        CHECK(stats.work_units == result.stats.edges_examined);
    }
}

void test_dram_kernels() {
    std::vector<cxltest::CsrGraph> graphs;
    graphs.push_back(cxltest::make_grid_graph(6, 6, true));
    graphs.push_back(cxltest::make_random_graph(80, 5, 7, true));
    graphs.push_back(cxltest::make_rmat_graph(128, 512, 9, true));
    graphs.push_back(path_graph(12, 0, 12));
    graphs.push_back(path_graph(12, 0, 5)); // disconnected second component
    std::vector<std::pair<std::uint32_t, std::uint32_t>> star_edges;
    for (std::uint32_t i = 1; i < 16; ++i) {
        star_edges.emplace_back(0, i);
    }
    graphs.push_back(cxltest::build_csr(16, star_edges, true));

    const cxltest::BfsKernel kernels[] = {cxltest::BfsKernel::TopDown, cxltest::BfsKernel::BottomUp,
                                          cxltest::BfsKernel::Hybrid};
    std::uint64_t base = 0;
    for (const auto &graph : graphs) {
        for (const auto kernel : kernels) {
            check_kernel(graph, kernel, cxltest::Backend::Dram, nullptr, base);
        }
    }
}

void test_cxl_kernels(cxltest::CXLClient &client) {
    std::vector<cxltest::CsrGraph> graphs;
    graphs.push_back(cxltest::make_grid_graph(8, 8, true));
    graphs.push_back(cxltest::make_random_graph(96, 6, 21, true));

    const cxltest::BfsKernel kernels[] = {cxltest::BfsKernel::TopDown, cxltest::BfsKernel::BottomUp,
                                          cxltest::BfsKernel::Hybrid};
    const cxltest::Backend backends[] = {cxltest::Backend::CxlDirect, cxltest::Backend::CxlBlock};

    std::uint64_t base = 0x00100000;
    for (const auto &graph : graphs) {
        for (const auto backend : backends) {
            for (const auto kernel : kernels) {
                check_kernel(graph, kernel, backend, &client, base);
            }
        }
    }
}

} // namespace

int main() {
    test_builders();
    test_reference();
    test_dram_kernels();

    cxltest::CXLClient client("127.0.0.1", 9999);
    std::string error;
    if (client.connect(&error)) {
        std::printf("connected to cxlmemsim_server; running CXL BFS tests\n");
        test_cxl_kernels(client);
    } else {
        std::printf("SKIP: CXL BFS tests require cxlmemsim_server (%s)\n", error.c_str());
    }

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
