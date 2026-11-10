#include "cxl/cxl_client.h"
#include "memory/tiered_array.h"
#include "workloads/stencil.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string backend = "dram";
    std::string kernel = "row";
    std::size_t n = 512;
    std::uint32_t steps = 4;
    std::size_t block_kb = 64;
    std::size_t cache_kb = 32;
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
        "  --kernel naive|row                    default: row\n"
        "  --n <size>                            grid side, default: 512\n"
        "  --steps <count>                       stencil iterations, default: 4\n"
        "  --block-kb <size>                     block fetch size, default: 64\n"
        "  --cache-kb <size>                     software cache, default: 32\n"
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

bool parse_kernel(const std::string &text, cxltest::StencilKernel &out) {
    if (text == "naive") {
        out = cxltest::StencilKernel::Naive;
    } else if (text == "row" || text == "row-blocked") {
        out = cxltest::StencilKernel::RowBlocked;
    } else {
        return false;
    }
    return true;
}

// Returns 0 on success, 1 for help, -1 for an invalid argument.
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
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.backend = value;
        } else if (key == "--kernel") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.kernel = value;
        } else if (key == "--n") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.n = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        } else if (key == "--steps") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.steps = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--block-kb") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.block_kb = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        } else if (key == "--cache-kb") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.cache_kb = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        } else if (key == "--host") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.host = value;
        } else if (key == "--port") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--cxl-base") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.cxl_base = std::strtoull(value.c_str(), nullptr, 0);
        } else if (key == "--repeat") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
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

std::vector<float> make_input(std::size_t n) {
    std::vector<float> out(n * n);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>((i * 37u + 11u) % 101u) / 101.0f;
    }
    return out;
}

float max_abs_diff(const std::vector<float> &a, const std::vector<float> &b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

std::uint64_t median3(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
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
    cxltest::StencilKernel kernel{};
    if (!parse_backend(opts.backend, backend)) {
        std::fprintf(stderr, "invalid backend: %s\n", opts.backend.c_str());
        return 2;
    }
    if (!parse_kernel(opts.kernel, kernel)) {
        std::fprintf(stderr, "invalid kernel: %s\n", opts.kernel.c_str());
        return 2;
    }
    if (opts.n < 3 || opts.steps == 0 || opts.repeat < 1) {
        std::fprintf(stderr, "n must be >= 3, steps >= 1, repeat >= 1\n");
        return 2;
    }

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

    const std::size_t plane_elems = opts.n * opts.n;
    const std::size_t plane_bytes = plane_elems * sizeof(float);

    cxltest::TieredArrayConfig config;
    config.backend = backend;
    config.size_bytes = 2 * plane_bytes;
    config.cxl_base_addr = opts.cxl_base;
    config.block_bytes = opts.block_kb * 1024;
    config.cache_bytes = opts.cache_kb * 1024;

    cxltest::TieredArray grid(config, client_ptr);
    const std::vector<float> input = make_input(opts.n);
    const std::vector<float> zeros(plane_elems, 0.0f);

    // The synchronous TCP client spends real CPU time in the network stack, so
    // the CXL run's CPU time is not the algorithm's compute time. Measure the
    // same kernel on DRAM instead and use that as t_compute. The CXL run's own
    // CPU time is reported separately as t_cpu_run.
    std::uint64_t compute_reference_ns = 0;
    if (backend != cxltest::Backend::Dram) {
        cxltest::TieredArrayConfig reference_config;
        reference_config.backend = cxltest::Backend::Dram;
        reference_config.size_bytes = 2 * plane_bytes;
        cxltest::TieredArray reference_grid(reference_config);

        std::uint64_t samples[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            reference_grid.load(0, input.data(), plane_bytes);
            reference_grid.load(plane_bytes, zeros.data(), plane_bytes);
            const std::uint64_t start = cpu_ns();
            cxltest::stencil_run(reference_grid, {opts.n, opts.steps, kernel});
            samples[i] = cpu_ns() - start;
        }
        compute_reference_ns = median3(samples[0], samples[1], samples[2]);
    }

    if (opts.csv && opts.header) {
        std::printf("backend,kernel,n,steps,block_kb,cache_kb,run,t_compute_ms,t_cxl_model_ms,"
                    "t_model_ms,t_cpu_run_ms,t_transport_cpu_ms,t_wall_ms,line_fetches,block_fetches,"
                    "cache_hits,cache_misses,cache_evictions,cxl_reads,cxl_writes,cxl_bytes_read,"
                    "cxl_bytes_written,work_units\n");
    }

    for (int run = 0; run < opts.repeat; ++run) {
        // Reset both planes so every measured run starts from the same state.
        grid.load(0, input.data(), plane_bytes);
        grid.load(plane_bytes, zeros.data(), plane_bytes);
        grid.reset_stats();

        const std::uint64_t cpu_start = cpu_ns();
        const std::uint64_t wall_start = wall_ns();
        const std::size_t final_plane =
            cxltest::stencil_run(grid, {opts.n, opts.steps, kernel});
        const std::uint64_t wall_end = wall_ns();
        const std::uint64_t cpu_end = cpu_ns();

        grid.add_work_units(static_cast<std::uint64_t>(plane_elems) * opts.steps);
        const cxltest::MemoryStats stats = grid.stats();

        const std::uint64_t t_cpu_run = cpu_end - cpu_start;
        const std::uint64_t t_compute =
            backend == cxltest::Backend::Dram ? t_cpu_run : compute_reference_ns;
        const std::uint64_t t_cxl = stats.modeled_ns;
        const std::uint64_t t_model = t_compute + t_cxl;
        const std::uint64_t t_transport = t_cpu_run > t_compute ? t_cpu_run - t_compute : 0;
        const std::uint64_t t_wall = wall_end - wall_start;

        if (opts.verify) {
            const auto got = cxltest::stencil_read_plane(grid, opts.n, final_plane);
            const auto reference = cxltest::stencil_reference(input, {opts.n, opts.steps, kernel});
            const float worst = max_abs_diff(got, reference);
            if (worst > 1e-5f) {
                std::fprintf(stderr, "verify failed: max abs diff %g\n", static_cast<double>(worst));
                return 1;
            }
        }

        if (opts.csv) {
            std::printf(
                "%s,%s,%zu,%u,%zu,%zu,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 "\n",
                cxltest::to_string(backend), opts.kernel.c_str(), opts.n, opts.steps, opts.block_kb,
                opts.cache_kb, run, static_cast<double>(t_compute) / 1e6,
                static_cast<double>(t_cxl) / 1e6, static_cast<double>(t_model) / 1e6,
                static_cast<double>(t_cpu_run) / 1e6, static_cast<double>(t_transport) / 1e6,
                static_cast<double>(t_wall) / 1e6, stats.line_fetches, stats.block_fetches,
                stats.cache_hits, stats.cache_misses, stats.cache_evictions, stats.cxl_reads,
                stats.cxl_writes, stats.cxl_bytes_read, stats.cxl_bytes_written, stats.work_units);
        } else {
            std::printf("run %d: backend=%s kernel=%s n=%zu steps=%u block=%zuKB cache=%zuKB\n", run,
                        cxltest::to_string(backend), opts.kernel.c_str(), opts.n, opts.steps,
                        opts.block_kb, opts.cache_kb);
            std::printf("  t_compute=%.3f ms  t_cxl_model=%.3f ms  t_model=%.3f ms\n",
                        static_cast<double>(t_compute) / 1e6, static_cast<double>(t_cxl) / 1e6,
                        static_cast<double>(t_model) / 1e6);
            std::printf("  t_cpu_run=%.3f ms  t_transport_cpu=%.3f ms  t_wall=%.3f ms\n",
                        static_cast<double>(t_cpu_run) / 1e6,
                        static_cast<double>(t_transport) / 1e6,
                        static_cast<double>(t_wall) / 1e6);
            std::printf("  line_fetches=%" PRIu64 " block_fetches=%" PRIu64
                        " cache_hits=%" PRIu64 " cache_misses=%" PRIu64
                        " evictions=%" PRIu64 " hit_rate=%.3f\n",
                        stats.line_fetches, stats.block_fetches, stats.cache_hits,
                        stats.cache_misses, stats.cache_evictions, stats.cache_hit_rate());
            std::printf("  cxl_reads=%" PRIu64 " cxl_writes=%" PRIu64 " bytes_read=%" PRIu64
                        " bytes_written=%" PRIu64 " work_units=%" PRIu64 "\n",
                        stats.cxl_reads, stats.cxl_writes, stats.cxl_bytes_read,
                        stats.cxl_bytes_written, stats.work_units);
            if (t_cxl > 0) {
                std::printf("  effective_read_bandwidth=%.2f GB/s (modeled)\n",
                            static_cast<double>(stats.cxl_bytes_read) / static_cast<double>(t_cxl));
            }
        }
    }

    return 0;
}
