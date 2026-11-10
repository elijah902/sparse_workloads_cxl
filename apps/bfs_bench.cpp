#include "cxl/cxl_client.h"
#include "memory/tiered_array.h"
#include "workloads/bfs.h"

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
    std::string kernel = "topdown";
    std::string graph = "grid";
    std::size_t n = 10000;
    std::uint64_t edges = 0;
    std::uint32_t degree = 8;
    std::uint64_t seed = 12345;
    std::uint32_t source = 0;
    std::size_t block_kb = 64;
    std::size_t cache_kb = 64;
    std::string host = "127.0.0.1";
    std::uint16_t port = 9999;
    std::uint64_t cxl_base = 0;
    int repeat = 1;
    bool csv = false;
    bool verify = false;
    bool header = true;
    bool directed = false;
};

void usage(const char *argv0) {
    std::printf(
        "usage: %s [options]\n"
        "  --backend dram|cxl-direct|cxl-block   default: dram\n"
        "  --kernel topdown|bottomup|hybrid      default: topdown\n"
        "  --graph grid|rmat|random              default: grid\n"
        "  --n <vertices>                        target vertex count, default: 10000\n"
        "  --edges <count>                       rmat edge count, default: 8*n\n"
        "  --degree <avg>                        random-graph average degree, default: 8\n"
        "  --seed <value>                        graph seed, default: 12345\n"
        "  --source <vertex>                     BFS source, default: 0\n"
        "  --block-kb <size>                     block fetch size, default: 64\n"
        "  --cache-kb <size>                     software cache, default: 64\n"
        "  --host <addr>                         server host, default: 127.0.0.1\n"
        "  --port <port>                         server port, default: 9999\n"
        "  --cxl-base <offset>                   pool offset, default: 0\n"
        "  --repeat <count>                      measured runs, default: 1\n"
        "  --directed                            build a directed graph (top-down only)\n"
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

bool parse_kernel(const std::string &text, cxltest::BfsKernel &out) {
    if (text == "topdown" || text == "top-down") {
        out = cxltest::BfsKernel::TopDown;
    } else if (text == "bottomup" || text == "bottom-up") {
        out = cxltest::BfsKernel::BottomUp;
    } else if (text == "hybrid" || text == "direction") {
        out = cxltest::BfsKernel::Hybrid;
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
        } else if (key == "--directed") {
            opts.directed = true;
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
        } else if (key == "--graph") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.graph = value;
        } else if (key == "--n") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.n = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        } else if (key == "--edges") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.edges = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "--degree") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.degree = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--seed") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.seed = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "--source") {
            if (!take_value(argc, argv, i, arg, value)) {
                return -1;
            }
            opts.source = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
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

std::uint64_t median3(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

cxltest::CsrGraph build_graph(const Options &opts) {
    const bool symmetric = !opts.directed;
    if (opts.graph == "grid") {
        const auto side = static_cast<std::uint32_t>(std::max<std::size_t>(1, std::llround(std::sqrt(
                                                                                       static_cast<double>(opts.n)))));
        return cxltest::make_grid_graph(side, side, symmetric);
    }
    if (opts.graph == "rmat") {
        const std::uint64_t edges = opts.edges == 0 ? static_cast<std::uint64_t>(opts.n) * 8 : opts.edges;
        return cxltest::make_rmat_graph(static_cast<std::uint32_t>(opts.n), edges, opts.seed, symmetric);
    }
    if (opts.graph == "random") {
        return cxltest::make_random_graph(static_cast<std::uint32_t>(opts.n), opts.degree, opts.seed, symmetric);
    }
    throw std::invalid_argument("unknown graph type: " + opts.graph);
}

struct GraphArrays {
    std::unique_ptr<cxltest::TieredArray> row_ptr;
    std::unique_ptr<cxltest::TieredArray> col_idx;
};

GraphArrays build_arrays(const cxltest::CsrGraph &graph, cxltest::Backend backend, std::uint64_t base,
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

    GraphArrays arrays;
    arrays.row_ptr = std::make_unique<cxltest::TieredArray>(row_config, client);
    arrays.col_idx = std::make_unique<cxltest::TieredArray>(col_config, client);

    arrays.row_ptr->load(0, graph.row_ptr.data(), row_bytes);
    arrays.col_idx->load(0, graph.col_idx.data(), col_bytes);
    arrays.row_ptr->reset_stats();
    arrays.col_idx->reset_stats();
    return arrays;
}

void reset_arrays(GraphArrays &arrays) {
    arrays.row_ptr->clear_cache();
    arrays.col_idx->clear_cache();
    arrays.row_ptr->reset_stats();
    arrays.col_idx->reset_stats();
}

cxltest::MemoryStats combined_stats(const GraphArrays &arrays) {
    cxltest::MemoryStats stats = arrays.row_ptr->stats();
    stats.add(arrays.col_idx->stats());
    return stats;
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
    cxltest::BfsKernel kernel{};
    if (!parse_backend(opts.backend, backend)) {
        std::fprintf(stderr, "invalid backend: %s\n", opts.backend.c_str());
        return 2;
    }
    if (!parse_kernel(opts.kernel, kernel)) {
        std::fprintf(stderr, "invalid kernel: %s\n", opts.kernel.c_str());
        return 2;
    }
    if (opts.n < 2 || opts.repeat < 1) {
        std::fprintf(stderr, "n must be >= 2 and repeat >= 1\n");
        return 2;
    }
    if (opts.directed && kernel != cxltest::BfsKernel::TopDown) {
        std::fprintf(stderr, "bottom-up and hybrid require a symmetric graph; drop --directed\n");
        return 2;
    }

    cxltest::CsrGraph graph;
    try {
        graph = build_graph(opts);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "graph build failed: %s\n", error.what());
        return 2;
    }
    if (opts.source >= graph.num_vertices) {
        std::fprintf(stderr, "source %u is outside the graph (%zu vertices)\n", opts.source, graph.num_vertices);
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

    const std::size_t block_bytes = opts.block_kb * 1024;
    const std::size_t cache_bytes = opts.cache_kb * 1024;

    GraphArrays arrays;
    try {
        arrays = build_arrays(graph, backend, opts.cxl_base, client_ptr, block_bytes, cache_bytes);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "array setup failed: %s\n", error.what());
        return 2;
    }

    const cxltest::BfsConfig config{kernel, opts.source, graph.symmetric};

    // The CXL run's CPU time is dominated by TCP work, so measure the compute
    // term on DRAM (ADR 0002).
    std::uint64_t compute_reference_ns = 0;
    if (backend != cxltest::Backend::Dram) {
        GraphArrays reference = build_arrays(graph, cxltest::Backend::Dram, 0, nullptr, block_bytes, cache_bytes);
        std::uint64_t samples[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            reset_arrays(reference);
            const std::uint64_t start = cpu_ns();
            cxltest::bfs_run(*reference.row_ptr, *reference.col_idx, graph.num_vertices, config);
            samples[i] = cpu_ns() - start;
        }
        compute_reference_ns = median3(samples[0], samples[1], samples[2]);
    }

    if (opts.csv && opts.header) {
        std::printf("backend,graph,kernel,n,m,source,run,t_compute_ms,t_cxl_model_ms,t_model_ms,"
                    "t_cpu_run_ms,t_transport_cpu_ms,t_wall_ms,iterations,max_distance,max_frontier,"
                    "edges_examined,vertices_scanned,row_ptr_reads,col_idx_reads,line_fetches,"
                    "block_fetches,cache_hits,cache_misses,cache_evictions,cxl_reads,cxl_writes,"
                    "cxl_bytes_read,cxl_bytes_written,work_units\n");
    }

    for (int run = 0; run < opts.repeat; ++run) {
        reset_arrays(arrays);

        const std::uint64_t cpu_start = cpu_ns();
        const std::uint64_t wall_start = wall_ns();
        cxltest::BfsResult result =
            cxltest::bfs_run(*arrays.row_ptr, *arrays.col_idx, graph.num_vertices, config);
        const std::uint64_t wall_end = wall_ns();
        const std::uint64_t cpu_end = cpu_ns();

        const cxltest::MemoryStats stats = combined_stats(arrays);

        const std::uint64_t t_cpu_run = cpu_end - cpu_start;
        const std::uint64_t t_compute = backend == cxltest::Backend::Dram ? t_cpu_run : compute_reference_ns;
        const std::uint64_t t_cxl = stats.modeled_ns;
        const std::uint64_t t_model = t_compute + t_cxl;
        const std::uint64_t t_transport = t_cpu_run > t_compute ? t_cpu_run - t_compute : 0;
        const std::uint64_t t_wall = wall_end - wall_start;

        if (opts.verify) {
            const auto reference = cxltest::bfs_reference(graph, opts.source);
            if (result.distances != reference) {
                std::fprintf(stderr, "verify failed: distances differ from the reference\n");
                return 1;
            }
        }

        if (opts.csv) {
            std::printf("%s,%s,%s,%zu,%zu,%u,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%" PRIu64
                        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                        ",%" PRIu64 "\n",
                        cxltest::to_string(backend), opts.graph.c_str(), cxltest::to_string(kernel),
                        graph.num_vertices, graph.num_edges, opts.source, run,
                        static_cast<double>(t_compute) / 1e6, static_cast<double>(t_cxl) / 1e6,
                        static_cast<double>(t_model) / 1e6, static_cast<double>(t_cpu_run) / 1e6,
                        static_cast<double>(t_transport) / 1e6, static_cast<double>(t_wall) / 1e6,
                        result.stats.iterations, result.stats.max_distance, result.stats.max_frontier,
                        result.stats.edges_examined, result.stats.vertices_scanned, result.stats.row_ptr_reads,
                        result.stats.col_idx_reads, stats.line_fetches, stats.block_fetches, stats.cache_hits,
                        stats.cache_misses, stats.cache_evictions, stats.cxl_reads, stats.cxl_writes,
                        stats.cxl_bytes_read, stats.cxl_bytes_written, stats.work_units);
        } else {
            std::printf("run %d: backend=%s graph=%s kernel=%s n=%zu m=%zu source=%u\n", run,
                        cxltest::to_string(backend), opts.graph.c_str(), cxltest::to_string(kernel),
                        graph.num_vertices, graph.num_edges, opts.source);
            std::printf("  t_compute=%.3f ms  t_cxl_model=%.3f ms  t_model=%.3f ms\n",
                        static_cast<double>(t_compute) / 1e6, static_cast<double>(t_cxl) / 1e6,
                        static_cast<double>(t_model) / 1e6);
            std::printf("  t_cpu_run=%.3f ms  t_transport_cpu=%.3f ms  t_wall=%.3f ms\n",
                        static_cast<double>(t_cpu_run) / 1e6, static_cast<double>(t_transport) / 1e6,
                        static_cast<double>(t_wall) / 1e6);
            std::printf("  iterations=%u max_distance=%u max_frontier=%u edges=%" PRIu64
                        " vertices_scanned=%" PRIu64 "\n",
                        result.stats.iterations, result.stats.max_distance, result.stats.max_frontier,
                        result.stats.edges_examined, result.stats.vertices_scanned);
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
