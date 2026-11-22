#include "cxl/cxl_client.h"
#include "memory/tiered_array.h"
#include "workloads/spmv.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
cxltest::CsrMatrix read_matrix(const std::string &path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open matrix: " + path);
    std::string line;
    if (!std::getline(file, line) || line.rfind("%%MatrixMarket matrix coordinate", 0) != 0)
        throw std::runtime_error("expected a coordinate Matrix Market file");
    const bool symmetric = line.find("symmetric") != std::string::npos;
    do { if (!std::getline(file, line)) throw std::runtime_error("missing matrix dimensions"); }
    while (line.empty() || line[0] == '%');
    std::istringstream dims(line);
    std::size_t rows, cols, count;
    if (!(dims >> rows >> cols >> count)) throw std::runtime_error("invalid matrix dimensions");
    std::vector<std::vector<std::pair<std::uint32_t, double>>> entries(rows);
    for (std::size_t i = 0; i < count; ++i) {
        do { if (!std::getline(file, line)) throw std::runtime_error("truncated matrix"); }
        while (line.empty() || line[0] == '%');
        std::istringstream item(line);
        std::size_t r, c; double v = 1.0;
        if (!(item >> r >> c) || (item >> v, r == 0 || r > rows || c == 0 || c > cols))
            throw std::runtime_error("invalid matrix entry");
        entries[r - 1].emplace_back(static_cast<std::uint32_t>(c - 1), v);
        if (symmetric && r != c) entries[c - 1].emplace_back(static_cast<std::uint32_t>(r - 1), v);
    }
    cxltest::CsrMatrix m; m.rows = rows; m.cols = cols; m.row_ptr.resize(rows + 1);
    for (std::size_t r = 0; r < rows; ++r) {
        m.row_ptr[r] = static_cast<std::uint32_t>(m.col_idx.size());
        for (auto [c, v] : entries[r]) { m.col_idx.push_back(c); m.values.push_back(v); }
    }
    m.row_ptr[rows] = static_cast<std::uint32_t>(m.col_idx.size()); m.nnz = m.col_idx.size();
    return m;
}
}

int main(int argc, char **argv) {
    std::string matrix_path, backend_name = "dram", host = "127.0.0.1";
    unsigned port = 9999; int repeat = 3;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string { if (++i >= argc) throw std::runtime_error("missing option value"); return argv[i]; };
        if (arg == "--matrix") matrix_path = value();
        else if (arg == "--backend") backend_name = value();
        else if (arg == "--host") host = value();
        else if (arg == "--port") port = std::stoul(value());
        else if (arg == "--repeat") repeat = std::stoi(value());
        else if (arg == "--help") { std::cout << "serial_spmv --matrix FILE [--backend dram|cxl-direct|cxl-block] [--host HOST] [--port PORT] [--repeat N]\n"; return 0; }
        else throw std::runtime_error("unknown option: " + arg);
    }
    try {
        if (matrix_path.empty() || repeat < 1) throw std::runtime_error("provide --matrix FILE and positive --repeat");
        const auto matrix = read_matrix(matrix_path);
        cxltest::Backend backend;
        if (backend_name == "dram") backend = cxltest::Backend::Dram;
        else if (backend_name == "cxl-direct") backend = cxltest::Backend::CxlDirect;
        else if (backend_name == "cxl-block") backend = cxltest::Backend::CxlBlock;
        else throw std::runtime_error("backend must be dram, cxl-direct, or cxl-block");
        cxltest::CXLClient client(host, static_cast<std::uint16_t>(port));
        if (backend != cxltest::Backend::Dram) { std::string error; if (!client.connect(&error)) throw std::runtime_error(error); }
        std::uint64_t base = 0;
        auto make_array = [&](std::size_t bytes) {
            cxltest::TieredArrayConfig cfg; cfg.backend = backend; cfg.size_bytes = bytes; cfg.cxl_base_addr = base;
            cfg.cache_bytes = 1024 * 1024; cfg.block_bytes = 64 * 1024; base += 0x10000000;
            return std::make_unique<cxltest::TieredArray>(cfg, backend == cxltest::Backend::Dram ? nullptr : &client);
        };
        auto rp = make_array(matrix.row_ptr.size() * sizeof(std::uint32_t));
        auto ci = make_array(matrix.col_idx.size() * sizeof(std::uint32_t));
        auto va = make_array(matrix.values.size() * sizeof(double));
        rp->load(0, matrix.row_ptr.data(), matrix.row_ptr.size() * sizeof(std::uint32_t));
        ci->load(0, matrix.col_idx.data(), matrix.col_idx.size() * sizeof(std::uint32_t));
        va->load(0, matrix.values.data(), matrix.values.size() * sizeof(double));
        rp->reset_stats(); ci->reset_stats(); va->reset_stats();
        std::vector<double> x(matrix.cols, 1.0);
        for (int r = 0; r < repeat; ++r) {
            rp->clear_cache(); ci->clear_cache(); va->clear_cache();
            rp->reset_stats(); ci->reset_stats(); va->reset_stats();
            auto start = std::chrono::steady_clock::now();
            auto result = cxltest::spmv_csr(*rp, *ci, *va, x, matrix.rows, matrix.cols, matrix.nnz);
            auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            auto stats = rp->stats(); stats.add(ci->stats()); stats.add(va->stats());
            std::printf("backend=%s run=%d rows=%zu cols=%zu nnz=%zu ms=%.3f cxl_reads=%llu bytes_read=%llu cache_hit_rate=%.3f checksum=%.9g\n",
                backend_name.c_str(), r + 1, matrix.rows, matrix.cols, matrix.nnz, elapsed,
                static_cast<unsigned long long>(stats.cxl_reads), static_cast<unsigned long long>(stats.cxl_bytes_read),
                stats.cache_hit_rate(), result.y.empty() ? 0.0 : result.y[0]);
        }
    } catch (const std::exception &e) { std::cerr << "serial_spmv: " << e.what() << '\n'; return 2; }
}
