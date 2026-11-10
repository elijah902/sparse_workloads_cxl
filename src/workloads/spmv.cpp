#include "workloads/spmv.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace cxltest {
namespace {

// splitmix64: deterministic generator used only for matrix construction.
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

struct Triplet {
    std::uint32_t row;
    std::uint32_t col;
    double value;
};

// Sort triplets by (row, col) and flatten into CSR. Duplicate (row, col)
// entries are kept; the reference and the CSR kernel sum them.
CsrMatrix build_from_triplets(std::size_t rows, std::size_t cols, std::vector<Triplet> &triplets) {
    std::sort(triplets.begin(), triplets.end(), [](const Triplet &a, const Triplet &b) {
        if (a.row != b.row) {
            return a.row < b.row;
        }
        return a.col < b.col;
    });

    CsrMatrix matrix;
    matrix.rows = rows;
    matrix.cols = cols;
    matrix.nnz = triplets.size();
    matrix.row_ptr.assign(rows + 1, 0);

    for (const Triplet &t : triplets) {
        ++matrix.row_ptr[t.row + 1];
    }
    for (std::size_t i = 1; i <= rows; ++i) {
        matrix.row_ptr[i] += matrix.row_ptr[i - 1];
    }

    matrix.col_idx.resize(matrix.nnz);
    matrix.values.resize(matrix.nnz);

    std::vector<std::uint32_t> cursor(matrix.row_ptr.begin(), matrix.row_ptr.end() - 1);
    for (const Triplet &t : triplets) {
        const std::uint32_t pos = cursor[t.row]++;
        matrix.col_idx[pos] = t.col;
        matrix.values[pos] = t.value;
    }
    return matrix;
}

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

std::uint32_t row_length(const CsrMatrix &matrix, std::uint32_t row) {
    return matrix.row_ptr[row + 1] - matrix.row_ptr[row];
}

} // namespace

const char *to_string(SpmvFormat format) {
    switch (format) {
    case SpmvFormat::Csr:
        return "csr";
    case SpmvFormat::Sell:
        return "sell";
    case SpmvFormat::Bcsr:
        return "bcsr";
    }
    return "unknown";
}

CsrMatrix make_laplacian_matrix(std::uint32_t side) {
    if (side == 0) {
        throw std::invalid_argument("make_laplacian_matrix: side must be > 0");
    }
    const std::uint64_t total = static_cast<std::uint64_t>(side) * side;
    if (total > 0xFFFFFFFFull) {
        throw std::invalid_argument("make_laplacian_matrix: too many rows for 32-bit ids");
    }

    std::vector<Triplet> triplets;
    triplets.reserve(static_cast<std::size_t>(total) * 5);

    for (std::uint32_t r = 0; r < side; ++r) {
        for (std::uint32_t c = 0; c < side; ++c) {
            const std::uint32_t id = r * side + c;
            std::uint32_t degree = 0;
            if (r > 0) {
                ++degree;
                triplets.push_back({id, id - side, -1.0});
            }
            if (r + 1 < side) {
                ++degree;
                triplets.push_back({id, id + side, -1.0});
            }
            if (c > 0) {
                ++degree;
                triplets.push_back({id, id - 1, -1.0});
            }
            if (c + 1 < side) {
                ++degree;
                triplets.push_back({id, id + 1, -1.0});
            }
            triplets.push_back({id, id, static_cast<double>(degree)});
        }
    }

    return build_from_triplets(total, total, triplets);
}

CsrMatrix make_rmat_matrix(std::uint32_t n, std::uint64_t nnz, std::uint64_t seed) {
    const std::uint32_t vertices = round_up_power_of_two(n);
    if (vertices < 2) {
        throw std::invalid_argument("make_rmat_matrix: n must be at least 2");
    }

    constexpr double a = 0.57;
    constexpr double b = 0.19;
    constexpr double c = 0.19;

    SplitMix64 rng(seed);
    std::vector<Triplet> triplets;
    triplets.reserve(static_cast<std::size_t>(nnz));

    for (std::uint64_t e = 0; e < nnz; ++e) {
        std::uint32_t u = 0;
        std::uint32_t v = 0;
        for (std::uint32_t bit = vertices >> 1; bit > 0; bit >>= 1) {
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
        triplets.push_back({u, v, 2.0 * rng.next_double() - 1.0});
    }

    return build_from_triplets(vertices, vertices, triplets);
}

CsrMatrix make_random_matrix(std::uint32_t rows, std::uint32_t cols, std::uint64_t nnz, std::uint64_t seed) {
    if (rows == 0 || cols == 0) {
        throw std::invalid_argument("make_random_matrix: rows and cols must be > 0");
    }

    SplitMix64 rng(seed);
    std::vector<Triplet> triplets;
    triplets.reserve(static_cast<std::size_t>(nnz));

    for (std::uint64_t e = 0; e < nnz; ++e) {
        const auto r = static_cast<std::uint32_t>(rng.bounded(rows));
        const auto col = static_cast<std::uint32_t>(rng.bounded(cols));
        triplets.push_back({r, col, 2.0 * rng.next_double() - 1.0});
    }

    return build_from_triplets(rows, cols, triplets);
}

SellMatrix to_sell(const CsrMatrix &matrix, std::uint32_t slice_height, std::uint32_t sigma) {
    if (slice_height == 0 || sigma == 0) {
        throw std::invalid_argument("to_sell: slice_height and sigma must be > 0");
    }

    const std::size_t rows = matrix.rows;

    // Sort rows by decreasing length inside each sigma-wide window. Similar
    // lengths end up in the same slice, which reduces padding.
    std::vector<std::uint32_t> order(rows);
    std::iota(order.begin(), order.end(), 0u);
    for (std::size_t window = 0; window < rows; window += sigma) {
        const std::size_t stop = std::min(rows, window + sigma);
        std::sort(order.begin() + static_cast<std::ptrdiff_t>(window),
                  order.begin() + static_cast<std::ptrdiff_t>(stop),
                  [&](std::uint32_t lhs, std::uint32_t rhs) {
                      const std::uint32_t llen = row_length(matrix, lhs);
                      const std::uint32_t rlen = row_length(matrix, rhs);
                      if (llen != rlen) {
                          return llen > rlen;
                      }
                      return lhs < rhs;
                  });
    }

    SellMatrix sell;
    sell.rows = rows;
    sell.cols = matrix.cols;
    sell.nnz = matrix.nnz;
    sell.slice_height = slice_height;
    sell.sigma = sigma;

    const std::size_t num_slices = (rows + slice_height - 1) / slice_height;
    sell.slice_ptr.assign(num_slices + 1, 0);
    sell.row_ids.assign(num_slices * slice_height, kInvalidRow);

    // First pass: slice offsets depend on the longest row in each slice.
    for (std::size_t slice = 0; slice < num_slices; ++slice) {
        std::uint32_t max_len = 0;
        for (std::uint32_t lane = 0; lane < slice_height; ++lane) {
            const std::size_t index = slice * slice_height + lane;
            if (index < rows) {
                max_len = std::max(max_len, row_length(matrix, order[index]));
            }
        }
        sell.slice_ptr[slice + 1] =
            sell.slice_ptr[slice] + static_cast<std::uint32_t>(max_len) * slice_height;
    }

    sell.nnz_stored = sell.slice_ptr[num_slices];
    sell.col.assign(sell.nnz_stored, 0);
    sell.values.assign(sell.nnz_stored, 0.0);

    // Second pass: copy entries. The layout inside a slice is column-major:
    //   offset(slice, k, lane) = slice_ptr[slice] + k * slice_height + lane
    // so a fixed k is contiguous across lanes.
    for (std::size_t slice = 0; slice < num_slices; ++slice) {
        const std::uint32_t base = sell.slice_ptr[slice];
        for (std::uint32_t lane = 0; lane < slice_height; ++lane) {
            const std::size_t index = slice * slice_height + lane;
            if (index >= rows) {
                continue;
            }
            const std::uint32_t row = order[index];
            sell.row_ids[index] = row;
            const std::uint32_t begin = matrix.row_ptr[row];
            const std::uint32_t len = row_length(matrix, row);
            for (std::uint32_t k = 0; k < len; ++k) {
                const std::size_t pos = base + static_cast<std::size_t>(k) * slice_height + lane;
                sell.col[pos] = matrix.col_idx[begin + k];
                sell.values[pos] = matrix.values[begin + k];
            }
        }
    }

    return sell;
}

BcsrMatrix to_bcsr(const CsrMatrix &matrix, std::uint32_t block_rows, std::uint32_t block_cols) {
    if (block_rows == 0 || block_cols == 0) {
        throw std::invalid_argument("to_bcsr: block_rows and block_cols must be > 0");
    }

    BcsrMatrix bcsr;
    bcsr.rows = matrix.rows;
    bcsr.cols = matrix.cols;
    bcsr.nnz = matrix.nnz;
    bcsr.block_rows = block_rows;
    bcsr.block_cols = block_cols;

    const std::size_t num_block_rows = (matrix.rows + block_rows - 1) / block_rows;

    // Collect the occupied column blocks for each block row.
    std::vector<std::vector<std::uint32_t>> occupied(num_block_rows);
    for (std::size_t br = 0; br < num_block_rows; ++br) {
        const std::size_t row_begin = br * block_rows;
        const std::size_t row_end = std::min(matrix.rows, row_begin + block_rows);
        auto &blocks = occupied[br];
        for (std::size_t row = row_begin; row < row_end; ++row) {
            for (std::uint32_t e = matrix.row_ptr[row]; e < matrix.row_ptr[row + 1]; ++e) {
                blocks.push_back(matrix.col_idx[e] / block_cols);
            }
        }
        std::sort(blocks.begin(), blocks.end());
        blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    }

    bcsr.row_ptr.assign(num_block_rows + 1, 0);
    for (std::size_t br = 0; br < num_block_rows; ++br) {
        bcsr.row_ptr[br + 1] = bcsr.row_ptr[br] + static_cast<std::uint32_t>(occupied[br].size());
    }

    const std::size_t num_blocks = bcsr.row_ptr[num_block_rows];
    bcsr.block_col.resize(num_blocks);
    bcsr.values.assign(num_blocks * block_rows * block_cols, 0.0);
    bcsr.nnz_stored = num_blocks * block_rows * block_cols;

    // Scatter the nonzero entries into their dense blocks. Duplicate entries
    // that land in the same slot are summed, matching the CSR reference.
    for (std::size_t br = 0; br < num_block_rows; ++br) {
        const std::size_t row_begin = br * block_rows;
        const std::size_t row_end = std::min(matrix.rows, row_begin + block_rows);
        for (std::size_t block_index = 0; block_index < occupied[br].size(); ++block_index) {
            const std::uint32_t block = bcsr.row_ptr[br] + static_cast<std::uint32_t>(block_index);
            const std::uint32_t block_col = occupied[br][block_index];
            bcsr.block_col[block] = block_col;
            const std::size_t base = static_cast<std::size_t>(block) * block_rows * block_cols;

            for (std::size_t row = row_begin; row < row_end; ++row) {
                for (std::uint32_t e = matrix.row_ptr[row]; e < matrix.row_ptr[row + 1]; ++e) {
                    if (matrix.col_idx[e] / block_cols != block_col) {
                        continue;
                    }
                    const std::size_t i = row - row_begin;
                    const std::size_t j = matrix.col_idx[e] - static_cast<std::size_t>(block_col) * block_cols;
                    bcsr.values[base + i * block_cols + j] += matrix.values[e];
                }
            }
        }
    }

    return bcsr;
}

std::vector<double> spmv_reference(const CsrMatrix &matrix, const std::vector<double> &x) {
    if (x.size() < matrix.cols) {
        throw std::invalid_argument("spmv_reference: x is shorter than the matrix column count");
    }
    std::vector<double> y(matrix.rows, 0.0);
    for (std::size_t row = 0; row < matrix.rows; ++row) {
        double sum = 0.0;
        for (std::uint32_t e = matrix.row_ptr[row]; e < matrix.row_ptr[row + 1]; ++e) {
            sum += matrix.values[e] * x[matrix.col_idx[e]];
        }
        y[row] = sum;
    }
    return y;
}

SpmvResult spmv_csr(TieredArray &row_ptr, TieredArray &col_idx, TieredArray &values,
                    const std::vector<double> &x, std::size_t rows, std::size_t cols, std::size_t nnz) {
    if (x.size() < cols) {
        throw std::invalid_argument("spmv_csr: x is shorter than the matrix column count");
    }

    SpmvResult result;
    result.y.assign(rows, 0.0);

    for (std::size_t row = 0; row < rows; ++row) {
        const std::uint32_t begin = row_ptr.load_value<std::uint32_t>(row);
        const std::uint32_t end = row_ptr.load_value<std::uint32_t>(row + 1);
        double sum = 0.0;
        for (std::uint32_t e = begin; e < end; ++e) {
            const std::uint32_t col = col_idx.load_value<std::uint32_t>(e);
            const double value = values.load_value<double>(e);
            sum += value * x[col];
        }
        result.y[row] = sum;
    }

    values.add_work_units(nnz);
    return result;
}

SpmvResult spmv_sell(TieredArray &slice_ptr, TieredArray &col, TieredArray &values,
                     const std::vector<std::uint32_t> &row_ids, const std::vector<double> &x,
                     std::size_t rows, std::size_t cols, std::size_t nnz, std::uint32_t slice_height) {
    if (x.size() < cols) {
        throw std::invalid_argument("spmv_sell: x is shorter than the matrix column count");
    }
    if (slice_height == 0 || row_ids.size() % slice_height != 0) {
        throw std::invalid_argument("spmv_sell: row_ids must be a multiple of slice_height");
    }

    SpmvResult result;
    result.y.assign(rows, 0.0);

    const std::size_t num_slices = row_ids.size() / slice_height;
    std::vector<double> accum(slice_height, 0.0);
    std::vector<std::uint8_t> valid(slice_height, 0);

    for (std::size_t slice = 0; slice < num_slices; ++slice) {
        const std::uint32_t base = slice_ptr.load_value<std::uint32_t>(slice);
        const std::uint32_t stop = slice_ptr.load_value<std::uint32_t>(slice + 1);
        const std::uint32_t max_len = (stop - base) / slice_height;

        std::fill(accum.begin(), accum.end(), 0.0);
        for (std::uint32_t lane = 0; lane < slice_height; ++lane) {
            const std::uint32_t row = row_ids[slice * slice_height + lane];
            valid[lane] = (row != kInvalidRow && row < rows) ? 1 : 0;
        }

        // Padding slots hold col 0 and value 0, so they still cost a lookup but
        // contribute nothing. That overhead is part of what SELL trades away.
        for (std::uint32_t k = 0; k < max_len; ++k) {
            for (std::uint32_t lane = 0; lane < slice_height; ++lane) {
                if (!valid[lane]) {
                    continue;
                }
                const std::size_t pos = base + static_cast<std::size_t>(k) * slice_height + lane;
                const std::uint32_t column = col.load_value<std::uint32_t>(pos);
                const double value = values.load_value<double>(pos);
                accum[lane] += value * x[column];
            }
        }

        for (std::uint32_t lane = 0; lane < slice_height; ++lane) {
            if (valid[lane]) {
                result.y[row_ids[slice * slice_height + lane]] = accum[lane];
            }
        }
    }

    values.add_work_units(nnz);
    return result;
}

SpmvResult spmv_bcsr(TieredArray &row_ptr, TieredArray &block_col, TieredArray &values,
                     const std::vector<double> &x, std::size_t rows, std::size_t cols, std::size_t nnz,
                     std::uint32_t block_rows, std::uint32_t block_cols) {
    if (x.size() < cols) {
        throw std::invalid_argument("spmv_bcsr: x is shorter than the matrix column count");
    }
    if (block_rows == 0 || block_cols == 0) {
        throw std::invalid_argument("spmv_bcsr: block dimensions must be > 0");
    }

    SpmvResult result;
    result.y.assign(rows, 0.0);

    const std::size_t num_block_rows = (rows + block_rows - 1) / block_rows;
    std::vector<double> accum(block_rows, 0.0);

    for (std::size_t block_row = 0; block_row < num_block_rows; ++block_row) {
        const std::uint32_t begin = row_ptr.load_value<std::uint32_t>(block_row);
        const std::uint32_t end = row_ptr.load_value<std::uint32_t>(block_row + 1);

        std::fill(accum.begin(), accum.end(), 0.0);

        for (std::uint32_t block = begin; block < end; ++block) {
            const std::uint32_t block_column = block_col.load_value<std::uint32_t>(block);
            const std::size_t base = static_cast<std::size_t>(block) * block_rows * block_cols;
            for (std::uint32_t i = 0; i < block_rows; ++i) {
                const std::size_t row = block_row * block_rows + i;
                if (row >= rows) {
                    continue;
                }
                for (std::uint32_t j = 0; j < block_cols; ++j) {
                    const std::size_t column = static_cast<std::size_t>(block_column) * block_cols + j;
                    if (column >= cols) {
                        continue;
                    }
                    const double value = values.load_value<double>(base + i * block_cols + j);
                    accum[i] += value * x[column];
                }
            }
        }

        for (std::uint32_t i = 0; i < block_rows; ++i) {
            const std::size_t row = block_row * block_rows + i;
            if (row < rows) {
                result.y[row] = accum[i];
            }
        }
    }

    values.add_work_units(nnz);
    return result;
}

} // namespace cxltest
