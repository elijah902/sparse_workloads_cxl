#include "cxl/cxl_client.h"
#include "memory/tiered_array.h"
#include "workloads/knn.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
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

bool neighbors_equal(const std::vector<cxltest::KnnNeighbor> &a, const std::vector<cxltest::KnnNeighbor> &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first || a[i].second != b[i].second) {
            return false;
        }
    }
    return true;
}

void check_query_results(const std::vector<std::vector<cxltest::KnnNeighbor>> &got,
                         const std::vector<std::vector<cxltest::KnnNeighbor>> &expected, const char *what) {
    CHECK(got.size() == expected.size());
    for (std::size_t q = 0; q < got.size(); ++q) {
        CHECK(neighbors_equal(got[q], expected[q]));
    }
    if (got != expected) {
        std::fprintf(stderr, "  mismatch in %s\n", what);
    }
}

cxltest::KnnConfig make_config(std::uint32_t k, std::uint32_t num_vectors, std::uint32_t vector_dim) {
    cxltest::KnnConfig config;
    config.k = k;
    config.num_vectors = num_vectors;
    config.vector_dim = vector_dim;
    return config;
}

struct HostArrays {
    std::unique_ptr<cxltest::TieredArray> data;
};

HostArrays build_host_arrays(const std::vector<std::vector<double>> &dataset, std::uint32_t vector_dim) {
    HostArrays arrays;
    const std::size_t bytes = dataset.size() * vector_dim * sizeof(double);
    cxltest::TieredArrayConfig config;
    config.backend = cxltest::Backend::Dram;
    config.size_bytes = bytes;
    arrays.data = std::make_unique<cxltest::TieredArray>(config);

    std::vector<uint8_t> buffer(bytes, 0);
    for (std::size_t i = 0; i < dataset.size(); ++i) {
        std::memcpy(buffer.data() + i * vector_dim * sizeof(double), dataset[i].data(), vector_dim * sizeof(double));
    }
    arrays.data->load(0, buffer.data(), bytes);
    arrays.data->reset_stats();
    return arrays;
}

void test_host_kernels(const std::vector<std::vector<double>> &dataset, const std::vector<std::vector<double>> &queries,
                      const cxltest::KnnConfig &config) {
    const auto expected = cxltest::knn_reference(dataset, queries, config);

    HostArrays arrays = build_host_arrays(dataset, config.vector_dim);
    const cxltest::KnnResult brute = cxltest::knn_run(*arrays.data, queries, config);
    check_query_results(brute.top_k, expected.top_k, "brute force");

    for (std::uint32_t tile : {1u, 2u, 3u, 4u, 8u, 64u}) {
        if (tile > config.num_vectors) {
            continue;
        }
        const cxltest::KnnResult tiled = cxltest::knn_run_tiled(*arrays.data, queries, config, tile);
        check_query_results(tiled.top_k, expected.top_k, "tiled");
    }
}

void test_cxl_kernels(cxltest::CXLClient &client) {
    const auto dataset = cxltest::make_knn_dataset(24, 4, 77);
    const auto queries = cxltest::make_knn_queries(3, 4, 78);
    const auto config = make_config(3, static_cast<std::uint32_t>(dataset.size()),
                                   static_cast<std::uint32_t>(dataset[0].size()));

    const std::size_t bytes = dataset.size() * config.vector_dim * sizeof(double);

    cxltest::TieredArray direct(cxltest::TieredArrayConfig{cxltest::Backend::CxlDirect, bytes, 0x00200000, 4096,
                                                           64 * 1024},
                                &client);
    std::vector<uint8_t> buffer(bytes, 0);
    for (std::size_t i = 0; i < dataset.size(); ++i) {
        std::memcpy(buffer.data() + i * config.vector_dim * sizeof(double), dataset[i].data(), config.vector_dim * sizeof(double));
    }
    direct.load(0, buffer.data(), bytes);
    direct.reset_stats();

    const auto got_direct = cxltest::knn_run(direct, queries, config);
    const auto expected = cxltest::knn_reference(dataset, queries, config);
    check_query_results(got_direct.top_k, expected.top_k, "cxl-direct brute");
    const auto got_direct_tiled = cxltest::knn_run_tiled(direct, queries, config, 2);
    check_query_results(got_direct_tiled.top_k, expected.top_k, "cxl-direct tiled");

    cxltest::TieredArray block(cxltest::TieredArrayConfig{cxltest::Backend::CxlBlock, bytes, 0x00300000, 4096,
                                                          64 * 1024},
                               &client);
    std::vector<uint8_t> buffer_block(bytes, 0);
    for (std::size_t i = 0; i < dataset.size(); ++i) {
        std::memcpy(buffer_block.data() + i * config.vector_dim * sizeof(double), dataset[i].data(),
                    config.vector_dim * sizeof(double));
    }
    block.load(0, buffer_block.data(), bytes);
    block.reset_stats();
    const auto got_block = cxltest::knn_run(block, queries, config);
    check_query_results(got_block.top_k, expected.top_k, "cxl-block brute");
    const auto got_block_tiled = cxltest::knn_run_tiled(block, queries, config, 4);
    check_query_results(got_block_tiled.top_k, expected.top_k, "cxl-block tiled");
}

void test_generators() {
    const auto dataset = cxltest::make_knn_dataset(10, 4, 77);
    CHECK(dataset.size() == 10);
    CHECK(dataset[0].size() == 4);
    const auto again = cxltest::make_knn_dataset(10, 4, 77);
    CHECK(dataset == again);

    const auto queries = cxltest::make_knn_queries(3, 4, 78);
    CHECK(queries.size() == 3);
    CHECK(queries[0].size() == 4);
}

} // namespace

int main() {
    const auto dataset = cxltest::make_knn_dataset(10, 4, 77);
    const auto queries = cxltest::make_knn_queries(2, 4, 78);
    const auto config = make_config(3, static_cast<std::uint32_t>(dataset.size()),
                                   static_cast<std::uint32_t>(dataset[0].size()));
    test_host_kernels(dataset, queries, config);
    test_generators();

    cxltest::CXLClient client("127.0.0.1", 9999);
    std::string error;
    if (client.connect(&error)) {
        std::printf("connected to cxlmemsim_server; running CXL k-NN tests\n");
        test_cxl_kernels(client);
    } else {
        std::printf("SKIP: CXL k-NN tests require cxlmemsim_server (%s)\n", error.c_str());
    }

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
