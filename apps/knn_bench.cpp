#include "cxl/cxl_client.h"
#include "memory/tiered_array.h"
#include "workloads/knn.h"

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string backend = "dram";
    std::string kernel = "brute";
    std::uint32_t n = 4096;
    std::uint32_t dim = 8;
    std::uint32_t queries = 4;
    std::uint32_t k = 3;
    std::uint32_t tile = 64;
    std::uint64_t seed = 77;
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
        "  --kernel brute|tile                   default: brute\n"
        "  --n <vectors>                         dataset vectors, default: 4096\n"
        "  --dim <dim>                           vector dim, default: 8\n"
        "  --queries <count>                     query vectors, default: 4\n"
        "  --k <count>                           top-k, default: 3\n"
        "  --tile <vectors>                     tile size for tile kernel, default: 64\n"
        "  --seed <value>                        dataset seed, default: 77\n"
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
        } else if (key == "--kernel") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.kernel = value;
        } else if (key == "--n") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.n = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--dim") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.dim = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--queries") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.queries = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--k") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.k = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--tile") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.tile = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--seed") {
            if (!take_value(argc, argv, i, arg, value)) return -1;
            opts.seed = std::strtoull(value.c_str(), nullptr, 10);
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

std::vector<std::vector<double>> make_dataset(const Options &opts) {
    return cxltest::make_knn_dataset(opts.n, opts.dim, opts.seed);
}

std::vector<std::vector<double>> make_queries(const Options &opts) {
    return cxltest::make_knn_queries(opts.queries, opts.dim, opts.seed + 1);
}

struct KnnArrays {
    std::unique_ptr<cxltest::TieredArray> data;
};

KnnArrays build_arrays(const Options &opts, const std::vector<std::vector<double>> &dataset, cxltest::Backend backend,
                       cxltest::CXLClient *client) {
    KnnArrays arrays;
    const std::size_t bytes = dataset.size() * opts.dim * sizeof(double);
    cxltest::TieredArrayConfig config;
    config.backend = backend;
    if (backend == cxltest::Backend::CxlBlock) {
        config.block_bytes = opts.block_kb * 1024;
    }
    config.size_bytes = bytes;
    config.cxl_base_addr = opts.cxl_base;
    config.cache_bytes = opts.cache_kb * 1024;
    arrays.data = std::make_unique<cxltest::TieredArray>(config, backend == cxltest::Backend::Dram ? nullptr : client);

    std::vector<uint8_t> buffer(bytes, 0);
    for (std::size_t i = 0; i < dataset.size(); ++i) {
        std::memcpy(buffer.data() + i * opts.dim * sizeof(double), dataset[i].data(), opts.dim * sizeof(double));
    }
    arrays.data->load(0, buffer.data(), bytes);
    arrays.data->reset_stats();
    return arrays;
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

    const std::string backend_key = opts.backend == "dram"    ? "dram"
                                    : (opts.backend == "cxl-direct" || opts.backend == "direct")
                                        ? "cxl-direct"
                                        : "cxl-block";

    cxltest::Backend backend;
    if (backend_key == "dram") {
        backend = cxltest::Backend::Dram;
    } else if (backend_key == "cxl-direct") {
        backend = cxltest::Backend::CxlDirect;
    } else {
        backend = cxltest::Backend::CxlBlock;
    }

    const auto dataset = make_dataset(opts);
    const auto queries = make_queries(opts);

    cxltest::CXLClient client(opts.host, opts.port);
    if (backend != cxltest::Backend::Dram) {
        std::string error;
        if (!client.connect(&error)) {
            std::fprintf(stderr, "cannot connect to %s:%u: %s\n", opts.host.c_str(),
                         static_cast<unsigned>(opts.port), error.c_str());
            return 2;
        }
    }

    KnnArrays arrays = build_arrays(opts, dataset, backend, backend == cxltest::Backend::Dram ? nullptr : &client);
    const cxltest::KnnConfig config{opts.k, opts.n, opts.dim};

    std::uint64_t compute_reference_ns = 0;
    if (backend == cxltest::Backend::Dram) {
        std::uint64_t samples[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            arrays.data->clear_cache();
            arrays.data->reset_stats();
            const std::uint64_t start = cpu_ns();
            cxltest::knn_run(*arrays.data, queries, config);
            samples[i] = cpu_ns() - start;
        }
        compute_reference_ns = median3(samples[0], samples[1], samples[2]);
    } else {
        KnnArrays reference = build_arrays(opts, dataset, cxltest::Backend::Dram, nullptr);
        // The compute reference always runs on host DRAM (ADR 0002).
        std::uint64_t samples[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            reference.data->clear_cache();
            reference.data->reset_stats();
            const std::uint64_t start = cpu_ns();
            cxltest::knn_run(*reference.data, queries, config);
            samples[i] = cpu_ns() - start;
        }
        compute_reference_ns = median3(samples[0], samples[1], samples[2]);
    }

    if (opts.csv && opts.header) {
        std::printf("backend,kernel,n_vectors,vector_dim,num_queries,k,tile_vectors,run,t_compute_ms,"
                    "t_cxl_model_ms,t_model_ms,t_cpu_run_ms,t_transport_cpu_ms,t_wall_ms,computations,"
                    "line_fetches,cache_hits,cache_misses,cache_evictions,cxl_reads,cxl_bytes_read,work_units\n");
    }

    for (int run = 0; run < opts.repeat; ++run) {
        arrays.data->clear_cache();
        arrays.data->reset_stats();

        const std::uint64_t cpu_start = cpu_ns();
        const std::uint64_t wall_start = wall_ns();
        const cxltest::KnnResult result = (opts.kernel == "tile") ? cxltest::knn_run_tiled(*arrays.data, queries, config, opts.tile)
                                                                     : cxltest::knn_run(*arrays.data, queries, config);
        const std::uint64_t wall_end = wall_ns();
        const std::uint64_t cpu_end = cpu_ns();

        const cxltest::MemoryStats stats = arrays.data->stats();

        const std::uint64_t t_cpu_run = cpu_end - cpu_start;
        const std::uint64_t t_compute = compute_reference_ns;
        const std::uint64_t t_cxl = stats.modeled_ns;
        const std::uint64_t t_model = t_compute + t_cxl;
        const std::uint64_t t_transport =
            backend == cxltest::Backend::Dram ? 0 : (t_cpu_run > t_compute ? t_cpu_run - t_compute : 0);
        const std::uint64_t t_wall = wall_end - wall_start;
        const std::uint64_t computations = stats.work_units;

        if (opts.verify) {
            const auto expected = cxltest::knn_reference(dataset, queries, config);
            if (result.top_k != expected.top_k) {
                std::fprintf(stderr, "verify failed: k-NN result differs from the reference\n");
                return 1;
            }
        }

        if (opts.csv) {
            std::printf("%s,%s,%u,%u,%u,%u,%u,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                        backend_key.c_str(), opts.kernel.c_str(), opts.n, opts.dim, opts.queries, opts.k, opts.tile,
                        run, static_cast<double>(t_compute) / 1e6, static_cast<double>(t_cxl) / 1e6,
                        static_cast<double>(t_model) / 1e6, static_cast<double>(t_cpu_run) / 1e6,
                        static_cast<double>(t_transport) / 1e6, static_cast<double>(t_wall) / 1e6, computations,
                        stats.line_fetches, stats.cache_hits, stats.cache_misses, stats.cache_evictions,
                        stats.cxl_reads, stats.cxl_bytes_read, stats.work_units);
        } else {
            std::printf("run %d: backend=%s kernel=%s n=%u dim=%u queries=%u k=%u tile=%u\n", run, backend_key.c_str(),
                        opts.kernel.c_str(), opts.n, opts.dim, opts.queries, opts.k, opts.tile);
            std::printf("  t_compute=%.3f ms  t_cxl_model=%.3f ms  t_model=%.3f ms\n",
                        static_cast<double>(t_compute) / 1e6, static_cast<double>(t_cxl) / 1e6,
                        static_cast<double>(t_model) / 1e6);
            std::printf("  t_cpu_run=%.3f ms  t_transport_cpu=%.3f ms  t_wall=%.3f ms\n",
                        static_cast<double>(t_cpu_run) / 1e6, static_cast<double>(t_transport) / 1e6,
                        static_cast<double>(t_wall) / 1e6);
            std::printf("  computations=%" PRIu64 " line_fetches=%" PRIu64 " cache_hits=%" PRIu64
                        " cache_misses=%" PRIu64 " evictions=%" PRIu64 " hit_rate=%.3f\n",
                        computations, stats.line_fetches, stats.cache_hits, stats.cache_misses, stats.cache_evictions,
                        stats.cache_hit_rate());
            std::printf("  cxl_reads=%" PRIu64 " cxl_bytes_read=%" PRIu64 " work_units=%" PRIu64 "\n",
                        stats.cxl_reads, stats.cxl_bytes_read, stats.work_units);
            if (t_cxl > 0) {
                std::printf("  effective_read_bandwidth=%.2f GB/s (modeled)\n",
                            static_cast<double>(stats.cxl_bytes_read) / static_cast<double>(t_cxl));
            }
        }
    }

    return 0;
}
