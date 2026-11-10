#pragma once

#include "memory/tiered_array.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cxltest {

enum class SpmvFormat {
    Csr,  // Compressed Sparse Row
    Sell, // SELL-C-sigma (sliced ELLPACK with a sorting window)
    Bcsr, // Blocked CSR
};

const char *to_string(SpmvFormat format);

// Host CSR matrix. Used to build the other formats and as the correctness
// oracle. Duplicate (row, col) entries are allowed and summed.
struct CsrMatrix {
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::size_t nnz = 0;
    std::vector<std::uint32_t> row_ptr; // rows + 1
    std::vector<std::uint32_t> col_idx; // nnz
    std::vector<double> values;         // nnz
};

// SELL-C-sigma. Rows are grouped into slices of `slice_height` rows, sorted by
// length within windows of `sigma`, and padded to the longest row in the slice.
struct SellMatrix {
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::size_t nnz = 0;
    std::size_t nnz_stored = 0; // includes padding
    std::uint32_t slice_height = 0;
    std::uint32_t sigma = 0;
    std::vector<std::uint32_t> slice_ptr; // num_slices + 1, offsets into col/values
    std::vector<std::uint32_t> col;       // nnz_stored
    std::vector<double> values;           // nnz_stored
    std::vector<std::uint32_t> row_ids;   // num_slices * slice_height, original row or kInvalidRow
};

constexpr std::uint32_t kInvalidRow = 0xFFFFFFFFu;

// Blocked CSR. Each stored block is a dense `block_rows x block_cols` tile.
struct BcsrMatrix {
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::size_t nnz = 0;
    std::size_t nnz_stored = 0; // number of blocks * block_rows * block_cols
    std::uint32_t block_rows = 0;
    std::uint32_t block_cols = 0;
    std::vector<std::uint32_t> row_ptr;   // num_block_rows + 1
    std::vector<std::uint32_t> block_col; // number of stored blocks
    std::vector<double> values;           // number of blocks * block_rows * block_cols
};

struct SpmvResult {
    std::vector<double> y;
};

// Deterministic matrix generators.
CsrMatrix make_laplacian_matrix(std::uint32_t side); // side x side 5-point grid
CsrMatrix make_rmat_matrix(std::uint32_t n, std::uint64_t nnz, std::uint64_t seed);
CsrMatrix make_random_matrix(std::uint32_t rows, std::uint32_t cols, std::uint64_t nnz, std::uint64_t seed);

// Format conversion from CSR.
SellMatrix to_sell(const CsrMatrix &matrix, std::uint32_t slice_height, std::uint32_t sigma);
BcsrMatrix to_bcsr(const CsrMatrix &matrix, std::uint32_t block_rows, std::uint32_t block_cols);

// Host reference SpMV.
std::vector<double> spmv_reference(const CsrMatrix &matrix, const std::vector<double> &x);

// Measured kernels. Matrix arrays are served by the tiered arrays; x and y stay
// in host DRAM. `nnz` is the logical nonzero count and is recorded as work units
// on the values array.
SpmvResult spmv_csr(TieredArray &row_ptr, TieredArray &col_idx, TieredArray &values,
                    const std::vector<double> &x, std::size_t rows, std::size_t cols, std::size_t nnz);

SpmvResult spmv_sell(TieredArray &slice_ptr, TieredArray &col, TieredArray &values,
                     const std::vector<std::uint32_t> &row_ids, const std::vector<double> &x,
                     std::size_t rows, std::size_t cols, std::size_t nnz, std::uint32_t slice_height);

SpmvResult spmv_bcsr(TieredArray &row_ptr, TieredArray &block_col, TieredArray &values,
                     const std::vector<double> &x, std::size_t rows, std::size_t cols, std::size_t nnz,
                     std::uint32_t block_rows, std::uint32_t block_cols);

} // namespace cxltest
