#include "cxl/cxl_client.h"
#include "workloads/spmv.h"

#include <algorithm>
#include <cmath>
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

bool close(double a, double b) {
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= 1e-9 * scale;
}

bool vectors_close(const std::vector<double> &a, const std::vector<double> &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!close(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

std::vector<double> make_x(std::size_t cols) {
    std::vector<double> x(cols);
    for (std::size_t i = 0; i < cols; ++i) {
        x[i] = 1.0 + static_cast<double>(i % 13) / 13.0;
    }
    return x;
}

struct BuiltArrays {
    std::unique_ptr<cxltest::TieredArray> first;
    std::unique_ptr<cxltest::TieredArray> second;
    std::unique_ptr<cxltest::TieredArray> third;
    std::vector<std::uint32_t> row_ids;
};

cxltest::TieredArrayConfig make_config(cxltest::Backend backend, std::size_t size_bytes, std::uint64_t &base,
                                       std::size_t block_bytes, std::size_t cache_bytes) {
    cxltest::TieredArrayConfig config;
    config.backend = backend;
    config.size_bytes = std::max<std::size_t>(size_bytes, 8);
    config.cxl_base_addr = base;
    config.block_bytes = block_bytes;
    config.cache_bytes = cache_bytes;
    base += 0x00100000;
    return config;
}

BuiltArrays build_csr(const cxltest::CsrMatrix &matrix, cxltest::Backend backend, std::uint64_t &base,
                      cxltest::CXLClient *client, std::size_t block_bytes, std::size_t cache_bytes) {
    BuiltArrays arrays;
    arrays.first = std::make_unique<cxltest::TieredArray>(
        make_config(backend, matrix.row_ptr.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes), client);
    arrays.second = std::make_unique<cxltest::TieredArray>(
        make_config(backend, matrix.col_idx.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes), client);
    arrays.third = std::make_unique<cxltest::TieredArray>(
        make_config(backend, matrix.values.size() * sizeof(double), base, block_bytes, cache_bytes), client);

    arrays.first->load(0, matrix.row_ptr.data(), matrix.row_ptr.size() * sizeof(std::uint32_t));
    if (!matrix.col_idx.empty()) {
        arrays.second->load(0, matrix.col_idx.data(), matrix.col_idx.size() * sizeof(std::uint32_t));
    }
    if (!matrix.values.empty()) {
        arrays.third->load(0, matrix.values.data(), matrix.values.size() * sizeof(double));
    }
    arrays.first->reset_stats();
    arrays.second->reset_stats();
    arrays.third->reset_stats();
    return arrays;
}

BuiltArrays build_sell(const cxltest::SellMatrix &matrix, cxltest::Backend backend, std::uint64_t &base,
                       cxltest::CXLClient *client, std::size_t block_bytes, std::size_t cache_bytes) {
    BuiltArrays arrays;
    arrays.first = std::make_unique<cxltest::TieredArray>(
        make_config(backend, matrix.slice_ptr.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes), client);
    arrays.second = std::make_unique<cxltest::TieredArray>(
        make_config(backend, matrix.col.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes), client);
    arrays.third = std::make_unique<cxltest::TieredArray>(
        make_config(backend, matrix.values.size() * sizeof(double), base, block_bytes, cache_bytes), client);

    arrays.first->load(0, matrix.slice_ptr.data(), matrix.slice_ptr.size() * sizeof(std::uint32_t));
    if (!matrix.col.empty()) {
        arrays.second->load(0, matrix.col.data(), matrix.col.size() * sizeof(std::uint32_t));
    }
    if (!matrix.values.empty()) {
        arrays.third->load(0, matrix.values.data(), matrix.values.size() * sizeof(double));
    }
    arrays.row_ids = matrix.row_ids;
    arrays.first->reset_stats();
    arrays.second->reset_stats();
    arrays.third->reset_stats();
    return arrays;
}

BuiltArrays build_bcsr(const cxltest::BcsrMatrix &matrix, cxltest::Backend backend, std::uint64_t &base,
                       cxltest::CXLClient *client, std::size_t block_bytes, std::size_t cache_bytes) {
    BuiltArrays arrays;
    arrays.first = std::make_unique<cxltest::TieredArray>(
        make_config(backend, matrix.row_ptr.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes), client);
    arrays.second = std::make_unique<cxltest::TieredArray>(
        make_config(backend, matrix.block_col.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes), client);
    arrays.third = std::make_unique<cxltest::TieredArray>(
        make_config(backend, matrix.values.size() * sizeof(double), base, block_bytes, cache_bytes), client);

    arrays.first->load(0, matrix.row_ptr.data(), matrix.row_ptr.size() * sizeof(std::uint32_t));
    if (!matrix.block_col.empty()) {
        arrays.second->load(0, matrix.block_col.data(), matrix.block_col.size() * sizeof(std::uint32_t));
    }
    if (!matrix.values.empty()) {
        arrays.third->load(0, matrix.values.data(), matrix.values.size() * sizeof(double));
    }
    arrays.first->reset_stats();
    arrays.second->reset_stats();
    arrays.third->reset_stats();
    return arrays;
}

void check_stats(const BuiltArrays &arrays, std::size_t nnz, bool cxl) {
    CHECK(arrays.third->stats().work_units == nnz);
    if (cxl) {
        cxltest::MemoryStats stats = arrays.first->stats();
        stats.add(arrays.second->stats());
        stats.add(arrays.third->stats());
        CHECK(stats.cxl_reads > 0);
        CHECK(stats.line_fetches > 0);
        CHECK(stats.modeled_ns > 0);
        CHECK(stats.cxl_bytes_read > 0);
    }
}

void check_csr(const cxltest::CsrMatrix &matrix, cxltest::Backend backend, cxltest::CXLClient *client,
               std::uint64_t &base, const std::vector<double> &x, const std::vector<double> &reference) {
    const std::size_t block_bytes = backend == cxltest::Backend::CxlBlock ? 4096 : 64 * 1024;
    const std::size_t cache_bytes = backend == cxltest::Backend::Dram ? 0 : 64 * 1024;

    auto arrays = build_csr(matrix, backend, base, client, block_bytes, cache_bytes);
    const cxltest::SpmvResult result = cxltest::spmv_csr(*arrays.first, *arrays.second, *arrays.third, x, matrix.rows,
                                                        matrix.cols, matrix.nnz);
    CHECK(result.y.size() == matrix.rows);
    CHECK(vectors_close(result.y, reference));
    check_stats(arrays, matrix.nnz, backend != cxltest::Backend::Dram);
}

void check_sell(const cxltest::CsrMatrix &matrix, std::uint32_t slice_height, std::uint32_t sigma,
                cxltest::Backend backend, cxltest::CXLClient *client, std::uint64_t &base,
                const std::vector<double> &x, const std::vector<double> &reference) {
    const std::size_t block_bytes = backend == cxltest::Backend::CxlBlock ? 4096 : 64 * 1024;
    const std::size_t cache_bytes = backend == cxltest::Backend::Dram ? 0 : 64 * 1024;

    const cxltest::SellMatrix sell = cxltest::to_sell(matrix, slice_height, sigma);
    auto arrays = build_sell(sell, backend, base, client, block_bytes, cache_bytes);
    const cxltest::SpmvResult result = cxltest::spmv_sell(*arrays.first, *arrays.second, *arrays.third,
                                                          arrays.row_ids, x, matrix.rows, matrix.cols, matrix.nnz,
                                                          slice_height);
    CHECK(result.y.size() == matrix.rows);
    CHECK(vectors_close(result.y, reference));
    check_stats(arrays, matrix.nnz, backend != cxltest::Backend::Dram);
}

void check_bcsr(const cxltest::CsrMatrix &matrix, std::uint32_t block_rows, std::uint32_t block_cols,
                cxltest::Backend backend, cxltest::CXLClient *client, std::uint64_t &base,
                const std::vector<double> &x, const std::vector<double> &reference) {
    const std::size_t block_bytes = backend == cxltest::Backend::CxlBlock ? 4096 : 64 * 1024;
    const std::size_t cache_bytes = backend == cxltest::Backend::Dram ? 0 : 64 * 1024;

    const cxltest::BcsrMatrix bcsr = cxltest::to_bcsr(matrix, block_rows, block_cols);
    auto arrays = build_bcsr(bcsr, backend, base, client, block_bytes, cache_bytes);
    const cxltest::SpmvResult result = cxltest::spmv_bcsr(*arrays.first, *arrays.second, *arrays.third, x,
                                                          matrix.rows, matrix.cols, matrix.nnz, block_rows,
                                                          block_cols);
    CHECK(result.y.size() == matrix.rows);
    CHECK(vectors_close(result.y, reference));
    check_stats(arrays, matrix.nnz, backend != cxltest::Backend::Dram);
}

void test_laplacian_structure() {
    const std::uint32_t side = 5;
    const auto matrix = cxltest::make_laplacian_matrix(side);
    CHECK(matrix.rows == side * side);
    CHECK(matrix.cols == side * side);
    // 5-point grid: 2 * (2 * side * (side - 1)) + side * side.
    CHECK(matrix.nnz == 4 * side * (side - 1) + side * side);
    CHECK(matrix.row_ptr.size() == matrix.rows + 1);
    CHECK(matrix.row_ptr[matrix.rows] == matrix.nnz);

    for (std::uint32_t r = 0; r < side; ++r) {
        for (std::uint32_t c = 0; c < side; ++c) {
            const std::uint32_t row = r * side + c;
            std::uint32_t degree = 0;
            if (r > 0) ++degree;
            if (r + 1 < side) ++degree;
            if (c > 0) ++degree;
            if (c + 1 < side) ++degree;

            bool saw_diagonal = false;
            for (std::uint32_t e = matrix.row_ptr[row]; e < matrix.row_ptr[row + 1]; ++e) {
                if (matrix.col_idx[e] == row) {
                    CHECK(close(matrix.values[e], static_cast<double>(degree)));
                    saw_diagonal = true;
                } else {
                    CHECK(close(matrix.values[e], -1.0));
                }
            }
            CHECK(saw_diagonal);
        }
    }

    // The graph Laplacian annihilates a constant vector.
    const std::vector<double> ones(matrix.cols, 1.0);
    const auto y = cxltest::spmv_reference(matrix, ones);
    for (const double value : y) {
        CHECK(close(value, 0.0));
    }
}

void test_generators() {
    const auto rmat = cxltest::make_rmat_matrix(64, 256, 12345);
    CHECK(rmat.rows == 64);
    CHECK(rmat.cols == 64);
    CHECK(rmat.nnz == 256);
    CHECK(rmat.row_ptr[rmat.rows] == rmat.nnz);
    for (std::size_t i = 0; i < rmat.rows; ++i) {
        CHECK(rmat.row_ptr[i] <= rmat.row_ptr[i + 1]);
    }
    for (const std::uint32_t col : rmat.col_idx) {
        CHECK(col < rmat.cols);
    }

    const auto random = cxltest::make_random_matrix(40, 30, 200, 99);
    CHECK(random.rows == 40);
    CHECK(random.cols == 30);
    CHECK(random.nnz == 200);
    CHECK(random.row_ptr[random.rows] == random.nnz);
    for (const std::uint32_t col : random.col_idx) {
        CHECK(col < random.cols);
    }

    // Empty matrix: valid, all-zero output.
    const auto empty = cxltest::make_random_matrix(10, 10, 0, 5);
    CHECK(empty.nnz == 0);
    CHECK(empty.row_ptr[10] == 0);
    const auto x = make_x(10);
    const auto y = cxltest::spmv_reference(empty, x);
    CHECK(y.size() == 10);
    for (const double value : y) {
        CHECK(close(value, 0.0));
    }
}

void test_formats() {
    const auto matrix = cxltest::make_laplacian_matrix(8);

    // A one-row-per-slice SELL stores exactly nnz entries (no padding). Row
    // order changes because SELL sorts within each sigma window.
    const auto sell1 = cxltest::to_sell(matrix, 1, 4);
    CHECK(sell1.nnz_stored == matrix.nnz);
    CHECK(sell1.row_ids.size() == matrix.rows);
    std::vector<int> order_seen(matrix.rows, 0);
    for (const std::uint32_t row : sell1.row_ids) {
        CHECK(row < matrix.rows);
        ++order_seen[row];
    }
    for (const int count : order_seen) {
        CHECK(count == 1);
    }

    // Wider slices introduce padding.
    const auto sell4 = cxltest::to_sell(matrix, 4, 8);
    CHECK(sell4.nnz_stored >= matrix.nnz);
    CHECK(sell4.row_ids.size() % 4 == 0);
    std::vector<int> seen(matrix.rows, 0);
    for (const std::uint32_t row : sell4.row_ids) {
        if (row != cxltest::kInvalidRow) {
            CHECK(row < matrix.rows);
            ++seen[row];
        }
    }
    for (const int count : seen) {
        CHECK(count == 1);
    }

    // 1x1 blocks reproduce CSR exactly.
    const auto bcsr1 = cxltest::to_bcsr(matrix, 1, 1);
    CHECK(bcsr1.nnz_stored == matrix.nnz);
    CHECK(bcsr1.block_col.size() == matrix.nnz);

    // 2x2 blocks pad to dense tiles.
    const auto bcsr2 = cxltest::to_bcsr(matrix, 2, 2);
    CHECK(bcsr2.nnz_stored >= matrix.nnz);
    CHECK(bcsr2.nnz_stored % 4 == 0);
    const std::size_t block_cols = (matrix.cols + 1) / 2;
    for (const std::uint32_t block_col : bcsr2.block_col) {
        CHECK(block_col < block_cols);
    }
}

void test_dram_kernels() {
    std::vector<cxltest::CsrMatrix> matrices;
    matrices.push_back(cxltest::make_laplacian_matrix(6));
    matrices.push_back(cxltest::make_rmat_matrix(64, 200, 7));
    matrices.push_back(cxltest::make_random_matrix(50, 40, 300, 21));
    matrices.push_back(cxltest::make_random_matrix(64, 64, 10, 3)); // many empty rows
    matrices.push_back(cxltest::make_random_matrix(10, 10, 0, 5));  // empty

    const std::uint32_t sell_shapes[][2] = {{1, 4}, {2, 4}, {4, 8}, {8, 16}};
    const std::uint32_t bcsr_shapes[][2] = {{1, 1}, {2, 2}, {3, 2}, {4, 4}};

    std::uint64_t base = 0;
    for (const auto &matrix : matrices) {
        const auto x = make_x(matrix.cols);
        const auto reference = cxltest::spmv_reference(matrix, x);

        check_csr(matrix, cxltest::Backend::Dram, nullptr, base, x, reference);
        for (const auto &shape : sell_shapes) {
            check_sell(matrix, shape[0], shape[1], cxltest::Backend::Dram, nullptr, base, x, reference);
        }
        for (const auto &shape : bcsr_shapes) {
            check_bcsr(matrix, shape[0], shape[1], cxltest::Backend::Dram, nullptr, base, x, reference);
        }
    }
}

void test_edge_cases() {
    // Single entry.
    std::vector<cxltest::CsrMatrix> matrices;
    matrices.push_back(cxltest::make_random_matrix(1, 1, 1, 11));

    // Block and slice shapes larger than the matrix.
    const auto small = cxltest::make_laplacian_matrix(5);
    const auto x = make_x(small.cols);
    const auto reference = cxltest::spmv_reference(small, x);

    std::uint64_t base = 0;
    check_sell(small, 16, 32, cxltest::Backend::Dram, nullptr, base, x, reference);
    check_bcsr(small, 8, 8, cxltest::Backend::Dram, nullptr, base, x, reference);

    for (const auto &matrix : matrices) {
        const auto one_x = make_x(matrix.cols);
        const auto one_ref = cxltest::spmv_reference(matrix, one_x);
        check_csr(matrix, cxltest::Backend::Dram, nullptr, base, one_x, one_ref);
        check_sell(matrix, 4, 4, cxltest::Backend::Dram, nullptr, base, one_x, one_ref);
        check_bcsr(matrix, 4, 4, cxltest::Backend::Dram, nullptr, base, one_x, one_ref);
    }
}

void test_cxl_kernels(cxltest::CXLClient &client) {
    std::vector<cxltest::CsrMatrix> matrices;
    matrices.push_back(cxltest::make_laplacian_matrix(8));
    matrices.push_back(cxltest::make_rmat_matrix(64, 256, 9));
    matrices.push_back(cxltest::make_random_matrix(48, 40, 240, 13));

    const cxltest::Backend backends[] = {cxltest::Backend::CxlDirect, cxltest::Backend::CxlBlock};

    std::uint64_t base = 0x00100000;
    for (const auto &matrix : matrices) {
        const auto x = make_x(matrix.cols);
        const auto reference = cxltest::spmv_reference(matrix, x);
        for (const auto backend : backends) {
            check_csr(matrix, backend, &client, base, x, reference);
            check_sell(matrix, 4, 8, backend, &client, base, x, reference);
            check_bcsr(matrix, 2, 2, backend, &client, base, x, reference);
        }
    }
}

} // namespace

int main() {
    test_laplacian_structure();
    test_generators();
    test_formats();
    test_dram_kernels();
    test_edge_cases();

    cxltest::CXLClient client("127.0.0.1", 9999);
    std::string error;
    if (client.connect(&error)) {
        std::printf("connected to cxlmemsim_server; running CXL SpMV tests\n");
        test_cxl_kernels(client);
    } else {
        std::printf("SKIP: CXL SpMV tests require cxlmemsim_server (%s)\n", error.c_str());
    }

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
