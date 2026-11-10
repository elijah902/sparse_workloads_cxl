#include "cxl/cxl_client.h"
#include "memory/tiered_array.h"
#include "workloads/spmv.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string backend = "dram";
    std::string format = "csr";
    std::string matrix = "laplacian";
    std::size_t n = 4096;
    std::size_t cols = 0;
    std::uint64_t nnz = 0;
    std::uint64_t seed = 12345;
    std::uint32_t slice_height = 4;
    std::uint32_t sigma = 8;
    std::uint32_t block_rows = 2;
    std::uint32_t block_cols = 2;
    std::size_t block_kb = 64;
    std::size_t cache_kb = 64;
    std::string host = "127.0.0.1";
    std::uint16_t port = 9999;
    std::uint64_t cxl_base = 0;
    int repeat = 1;
    bool csv = false;
    bool verify = false;
    bool header = true;
};

void usage(const char *argv0) {
    std::printf(
        "usage: %s [options]\n"
        "  --backend dram|cxl-direct|cxl-block   default: dram\n"
        "  --format csr|sell|bcsr                default: csr\n"
        "  --matrix laplacian|rmat|random        default: laplacian\n"
        "  --n <size>                            grid side or row count, default: 4096\n"
        "  --cols <count>                        random-matrix columns, default: n\n"
        "  --nnz <count>                         rmat/random nonzeros, default: 8*n\n"
        "  --seed <value>                        matrix seed, default: 12345\n"
        "  --slice-height <rows>                 SELL slice height, default: 4\n"
        "  --sigma <rows>                        SELL sorting window, default: 8\n"
        "  --block-rows <rows>                   BCSR block rows, default: 2\n"
        "  --block-cols <cols>                   BCSR block cols, default: 2\n"
        "  --block-kb <size>                     block fetch size, default: 64\n"
        "  --cache-kb <size>                     software cache, default: 64\n"
        "  --host <addr>                         server host, default: 127.0.0.1\n"
        "  --port <port>                         server port, default: 9999\n"
        "  --cxl-base <offset>                   pool offset, default: 0\n"
        "  --repeat <count>                      measured runs, default: 1\n"
        "  --csv                                 emit CSV rows\n"
        "  --no-header                           omit the CSV header\n"
        "  --verify                              compare against the reference\n",
        argv0);
}

bool take_value(int argc, char **argv, int &i, const std::string &arg, std::string &out) {
    const auto eq = arg.find('=');
    if (eq != std::string::npos) {
        out = arg.substr(eq + 1);
        return true;
    }
    if (i + 1 >= argc) {
        return false;
    }
    out = argv[++i];
    return true;
}

bool parse_backend(const std::string &text, cxltest::Backend &out) {
    if (text == "dram") {
        out = cxltest::Backend::Dram;
    } else if (text == "cxl-direct" || text == "direct") {
        out = cxltest::Backend::CxlDirect;
    } else if (text == "cxl-block" || text == "block") {
        out = cxltest::Backend::CxlBlock;
    } else {
        return false;
    }
    return true;
}

bool parse_format(const std::string &text, cxltest::SpmvFormat &out) {
    if (text == "csr") {
        out = cxltest::SpmvFormat::Csr;
    } else if (text == "sell" || text == "sell-c-sigma") {
        out = cxltest::SpmvFormat::Sell;
    } else if (text == "bcsr" || text == "blocked") {
        out = cxltest::SpmvFormat::Bcsr;
    } else {
        return false;
    }
    return true;
}

int parse_options(int argc, char **argv, Options &opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string key = arg.substr(0, arg.find('='));

        if (key == "--help" || key == "-h") {
            usage(argv[0]);
            return 1;
        }

        std::string value;
        if (key == "--csv") {
            opts.csv = true;
        } else if (key == "--no-header") {
            opts.header = false;
        } else if (key == "--verify") {
            opts.verify = true;
        } else if (key == "--backend") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.backend = value;
        } else if (key == "--format") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.format = value;
        } else if (key == "--matrix") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.matrix = value;
        } else if (key == "--n") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.n = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        } else if (key == "--cols") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.cols = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        } else if (key == "--nnz") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.nnz = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "--seed") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.seed = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "--slice-height") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.slice_height = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--sigma") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.sigma = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--block-rows") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.block_rows = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--block-cols") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.block_cols = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--block-kb") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.block_kb = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        } else if (key == "--cache-kb") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.cache_kb = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        } else if (key == "--host") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.host = value;
        } else if (key == "--port") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--cxl-base") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.cxl_base = std::strtoull(value.c_str(), nullptr, 0);
        } else if (key == "--repeat") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.repeat = std::atoi(value.c_str());
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            return -1;
        }
    }
    return 0;
}

std::uint64_t wall_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t cpu_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint64_t median3(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

cxltest::CsrMatrix build_matrix(const Options &opts) {
    if (opts.matrix == "laplacian") {
        return cxltest::make_laplacian_matrix(static_cast<std::uint32_t>(opts.n));
    }
    if (opts.matrix == "rmat") {
        const std::uint64_t nnz = opts.nnz == 0 ? static_cast<std::uint64_t>(opts.n) * 8 : opts.nnz;
        return cxltest::make_rmat_matrix(static_cast<std::uint32_t>(opts.n), nnz, opts.seed);
    }
    if (opts.matrix == "random") {
        const std::size_t cols = opts.cols == 0 ? opts.n : opts.cols;
        const std::uint64_t nnz = opts.nnz == 0 ? static_cast<std::uint64_t>(opts.n) * 8 : opts.nnz;
        return cxltest::make_random_matrix(static_cast<std::uint32_t>(opts.n), static_cast<std::uint32_t>(cols), nnz,
                                           opts.seed);
    }
    throw std::invalid_argument("unknown matrix type: " + opts.matrix);
}

std::vector<double> make_x(std::size_t cols) {
    std::vector<double> x(cols);
    for (std::size_t i = 0; i < cols; ++i) {
        x[i] = 1.0 + static_cast<double>(i % 13) / 13.0;
    }
    return x;
}

struct SpmvArrays {
    std::unique_ptr<cxltest::TieredArray> first;
    std::unique_ptr<cxltest::TieredArray> second;
    std::unique_ptr<cxltest::TieredArray> third;
    std::vector<std::uint32_t> row_ids;
    std::size_t nnz_stored = 0;
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

SpmvArrays build_arrays(const cxltest::CsrMatrix &matrix, cxltest::SpmvFormat format, cxltest::Backend backend,
                        std::uint64_t base, cxltest::CXLClient *client, const Options &opts) {
    const std::size_t block_bytes = opts.block_kb * 1024;
    const std::size_t cache_bytes = opts.cache_kb * 1024;

    SpmvArrays arrays;

    if (format == cxltest::SpmvFormat::Csr) {
        arrays.first = std::make_unique<cxltest::TieredArray>(
            make_config(backend, matrix.row_ptr.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes),
            client);
        arrays.second = std::make_unique<cxltest::TieredArray>(
            make_config(backend, matrix.col_idx.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes),
            client);
        arrays.third = std::make_unique<cxltest::TieredArray>(
            make_config(backend, matrix.values.size() * sizeof(double), base, block_bytes, cache_bytes), client);

        arrays.first->load(0, matrix.row_ptr.data(), matrix.row_ptr.size() * sizeof(std::uint32_t));
        if (!matrix.col_idx.empty()) {
            arrays.second->load(0, matrix.col_idx.data(), matrix.col_idx.size() * sizeof(std::uint32_t));
        }
        if (!matrix.values.empty()) {
            arrays.third->load(0, matrix.values.data(), matrix.values.size() * sizeof(double));
        }
        arrays.nnz_stored = matrix.nnz;
    } else if (format == cxltest::SpmvFormat::Sell) {
        const cxltest::SellMatrix sell = cxltest::to_sell(matrix, opts.slice_height, opts.sigma);
        arrays.first = std::make_unique<cxltest::TieredArray>(
            make_config(backend, sell.slice_ptr.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes),
            client);
        arrays.second = std::make_unique<cxltest::TieredArray>(
            make_config(backend, sell.col.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes), client);
        arrays.third = std::make_unique<cxltest::TieredArray>(
            make_config(backend, sell.values.size() * sizeof(double), base, block_bytes, cache_bytes), client);

        arrays.first->load(0, sell.slice_ptr.data(), sell.slice_ptr.size() * sizeof(std::uint32_t));
        if (!sell.col.empty()) {
            arrays.second->load(0, sell.col.data(), sell.col.size() * sizeof(std::uint32_t));
        }
        if (!sell.values.empty()) {
            arrays.third->load(0, sell.values.data(), sell.values.size() * sizeof(double));
        }
        arrays.row_ids = sell.row_ids;
        arrays.nnz_stored = sell.nnz_stored;
    } else {
        const cxltest::BcsrMatrix bcsr = cxltest::to_bcsr(matrix, opts.block_rows, opts.block_cols);
        arrays.first = std::make_unique<cxltest::TieredArray>(
            make_config(backend, bcsr.row_ptr.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes),
            client);
        arrays.second = std::make_unique<cxltest::TieredArray>(
            make_config(backend, bcsr.block_col.size() * sizeof(std::uint32_t), base, block_bytes, cache_bytes),
            client);
        arrays.third = std::make_unique<cxltest::TieredArray>(
            make_config(backend, bcsr.values.size() * sizeof(double), base, block_bytes, cache_bytes), client);

        arrays.first->load(0, bcsr.row_ptr.data(), bcsr.row_ptr.size() * sizeof(std::uint32_t));
        if (!bcsr.block_col.empty()) {
            arrays.second->load(0, bcsr.block_col.data(), bcsr.block_col.size() * sizeof(std::uint32_t));
        }
        if (!bcsr.values.empty()) {
            arrays.third->load(0, bcsr.values.data(), bcsr.values.size() * sizeof(double));
        }
        arrays.nnz_stored = bcsr.nnz_stored;
    }

    arrays.first->reset_stats();
    arrays.second->reset_stats();
    arrays.third->reset_stats();
    return arrays;
}

cxltest::SpmvResult run_kernel(SpmvArrays &arrays, cxltest::SpmvFormat format, const cxltest::CsrMatrix &matrix,
                               const std::vector<double> &x, const Options &opts) {
    if (format == cxltest::SpmvFormat::Csr) {
        return cxltest::spmv_csr(*arrays.first, *arrays.second, *arrays.third, x, matrix.rows, matrix.cols,
                                 matrix.nnz);
    }
    if (format == cxltest::SpmvFormat::Sell) {
        return cxltest::spmv_sell(*arrays.first, *arrays.second, *arrays.third, arrays.row_ids, x, matrix.rows,
                                  matrix.cols, matrix.nnz, opts.slice_height);
    }
    return cxltest::spmv_bcsr(*arrays.first, *arrays.second, *arrays.third, x, matrix.rows, matrix.cols, matrix.nnz,
                              opts.block_rows, opts.block_cols);
}

void reset_arrays(SpmvArrays &arrays) {
    arrays.first->clear_cache();
    arrays.second->clear_cache();
    arrays.third->clear_cache();
    arrays.first->reset_stats();
    arrays.second->reset_stats();
    arrays.third->reset_stats();
}

cxltest::MemoryStats combined_stats(const SpmvArrays &arrays) {
    cxltest::MemoryStats stats = arrays.first->stats();
    stats.add(arrays.second->stats());
    stats.add(arrays.third->stats());
    return stats;
}

bool close(double a, double b) {
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= 1e-9 * scale;
}

bool verify_result(const std::vector<double> &actual, const std::vector<double> &expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!close(actual[i], expected[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    Options opts;
    const int parse_result = parse_options(argc, argv, opts);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        return 2;
    }

    cxltest::Backend backend{};
    cxltest::SpmvFormat format{};
    if (!parse_backend(opts.backend, backend)) {
        std::fprintf(stderr, "invalid backend: %s\n", opts.backend.c_str());
        return 2;
    }
    if (!parse_format(opts.format, format)) {
        std::fprintf(stderr, "invalid format: %s\n", opts.format.c_str());
        return 2;
    }
    if (opts.n == 0 || opts.repeat < 1 || opts.slice_height == 0 || opts.sigma == 0 || opts.block_rows == 0 ||
        opts.block_cols == 0) {
        std::fprintf(stderr, "n, repeat, slice/sigma, and block dimensions must be positive\n");
        return 2;
    }

    cxltest::CsrMatrix matrix;
    try {
        matrix = build_matrix(opts);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "matrix build failed: %s\n", error.what());
        return 2;
    }

    const std::vector<double> x = make_x(matrix.cols);
    const std::vector<double> reference = cxltest::spmv_reference(matrix, x);

    cxltest::CXLClient client(opts.host, opts.port);
    cxltest::CXLClient *client_ptr = nullptr;
    if (backend != cxltest::Backend::Dram) {
        std::string error;
        if (!client.connect(&error)) {
            std::fprintf(stderr, "cannot connect to %s:%u: %s\n", opts.host.c_str(),
                         static_cast<unsigned>(opts.port), error.c_str());
            return 2;
        }
        client_ptr = &client;
    }

    SpmvArrays arrays;
    try {
        arrays = build_arrays(matrix, format, backend, opts.cxl_base, client_ptr, opts);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "array setup failed: %s\n", error.what());
        return 2;
    }

    // Measure the compute term on DRAM as a warm median of three runs (ADR
    // 0002). Using the same term for the DRAM and CXL rows keeps the comparison
    // consistent: a single cold DRAM run can be several times slower because of
    // first-touch page faults on the matrix arrays.
    std::uint64_t compute_reference_ns = 0;
    if (backend == cxltest::Backend::Dram) {
        std::uint64_t samples[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            reset_arrays(arrays);
            const std::uint64_t start = cpu_ns();
            run_kernel(arrays, format, matrix, x, opts);
            samples[i] = cpu_ns() - start;
        }
        compute_reference_ns = median3(samples[0], samples[1], samples[2]);
    } else {
        SpmvArrays reference_arrays = build_arrays(matrix, format, cxltest::Backend::Dram, 0, nullptr, opts);
        std::uint64_t samples[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            reset_arrays(reference_arrays);
            const std::uint64_t start = cpu_ns();
            run_kernel(reference_arrays, format, matrix, x, opts);
            samples[i] = cpu_ns() - start;
        }
        compute_reference_ns = median3(samples[0], samples[1], samples[2]);
    }

    if (opts.csv && opts.header) {
        std::printf("backend,format,matrix,rows,cols,nnz,nnz_stored,slice_height,sigma,block_rows,block_cols,"
                    "seed,run,t_compute_ms,t_cxl_model_ms,t_model_ms,t_cpu_run_ms,t_transport_cpu_ms,t_wall_ms,"
                    "line_fetches,block_fetches,cache_hits,cache_misses,cache_evictions,cxl_reads,cxl_writes,"
                    "cxl_bytes_read,cxl_bytes_written,work_units\n");
    }

    for (int run = 0; run < opts.repeat; ++run) {
        reset_arrays(arrays);

        const std::uint64_t cpu_start = cpu_ns();
        const std::uint64_t wall_start = wall_ns();
        const cxltest::SpmvResult result = run_kernel(arrays, format, matrix, x, opts);
        const std::uint64_t wall_end = wall_ns();
        const std::uint64_t cpu_end = cpu_ns();

        const cxltest::MemoryStats stats = combined_stats(arrays);

        const std::uint64_t t_cpu_run = cpu_end - cpu_start;
        const std::uint64_t t_compute = compute_reference_ns;
        const std::uint64_t t_cxl = stats.modeled_ns;
        const std::uint64_t t_model = t_compute + t_cxl;
        const std::uint64_t t_transport =
            backend == cxltest::Backend::Dram ? 0 : (t_cpu_run > t_compute ? t_cpu_run - t_compute : 0);
        const std::uint64_t t_wall = wall_end - wall_start;

        if (opts.verify && !verify_result(result.y, reference)) {
            std::fprintf(stderr, "verify failed: result differs from the reference\n");
            return 1;
        }

        if (opts.csv) {
            std::printf("%s,%s,%s,%zu,%zu,%zu,%zu,%u,%u,%u,%u,%" PRIu64 ",%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                        cxltest::to_string(backend), cxltest::to_string(format), opts.matrix.c_str(), matrix.rows,
                        matrix.cols, matrix.nnz, arrays.nnz_stored, opts.slice_height, opts.sigma, opts.block_rows,
                        opts.block_cols, opts.seed, run, static_cast<double>(t_compute) / 1e6,
                        static_cast<double>(t_cxl) / 1e6, static_cast<double>(t_model) / 1e6,
                        static_cast<double>(t_cpu_run) / 1e6, static_cast<double>(t_transport) / 1e6,
                        static_cast<double>(t_wall) / 1e6, stats.line_fetches, stats.block_fetches, stats.cache_hits,
                        stats.cache_misses, stats.cache_evictions, stats.cxl_reads, stats.cxl_writes,
                        stats.cxl_bytes_read, stats.cxl_bytes_written, stats.work_units);
        } else {
            std::printf("run %d: backend=%s format=%s matrix=%s rows=%zu cols=%zu nnz=%zu stored=%zu\n", run,
                        cxltest::to_string(backend), cxltest::to_string(format), opts.matrix.c_str(), matrix.rows,
                        matrix.cols, matrix.nnz, arrays.nnz_stored);
            std::printf("  t_compute=%.3f ms  t_cxl_model=%.3f ms  t_model=%.3f ms\n",
                        static_cast<double>(t_compute) / 1e6, static_cast<double>(t_cxl) / 1e6,
                        static_cast<double>(t_model) / 1e6);
            std::printf("  t_cpu_run=%.3f ms  t_transport_cpu=%.3f ms  t_wall=%.3f ms\n",
                        static_cast<double>(t_cpu_run) / 1e6, static_cast<double>(t_transport) / 1e6,
                        static_cast<double>(t_wall) / 1e6);
            std::printf("  line_fetches=%" PRIu64 " block_fetches=%" PRIu64 " cache_hits=%" PRIu64
                        " cache_misses=%" PRIu64 " evictions=%" PRIu64 " hit_rate=%.3f\n",
                        stats.line_fetches, stats.block_fetches, stats.cache_hits, stats.cache_misses,
                        stats.cache_evictions, stats.cache_hit_rate());
            std::printf("  cxl_reads=%" PRIu64 " cxl_writes=%" PRIu64 " bytes_read=%" PRIu64
                        " bytes_written=%" PRIu64 " work_units=%" PRIu64 "\n",
                        stats.cxl_reads, stats.cxl_writes, stats.cxl_bytes_read, stats.cxl_bytes_written,
                        stats.work_units);
            if (t_cxl > 0) {
                std::printf("  effective_read_bandwidth=%.2f GB/s (modeled)\n",
                            static_cast<double>(stats.cxl_bytes_read) / static_cast<double>(t_cxl));
            }
        }
    }

    return 0;
}
