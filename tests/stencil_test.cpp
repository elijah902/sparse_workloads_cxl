#include "cxl/cxl_client.h"
#include "workloads/stencil.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
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

std::vector<float> make_input(std::size_t n) {
    std::vector<float> out(n * n);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>((i * 37u + 11u) % 101u) / 101.0f;
    }
    return out;
}

float max_abs_diff(const std::vector<float> &a, const std::vector<float> &b) {
    if (a.size() != b.size()) {
        return 1e30f;
    }
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

void run_dram(cxltest::StencilKernel kernel) {
    constexpr std::size_t kN = 24;
    constexpr std::uint32_t kSteps = 3;
    const std::size_t plane_bytes = kN * kN * sizeof(float);

    cxltest::TieredArrayConfig config;
    config.backend = cxltest::Backend::Dram;
    config.size_bytes = 2 * plane_bytes;

    cxltest::TieredArray grid(config);
    const auto input = make_input(kN);
    grid.load(0, input.data(), plane_bytes);

    const cxltest::StencilConfig stencil{kN, kSteps, kernel};
    const std::size_t final_plane = cxltest::stencil_run(grid, stencil);
    const auto got = cxltest::stencil_read_plane(grid, kN, final_plane);
    const auto reference = cxltest::stencil_reference(input, stencil);

    CHECK(max_abs_diff(got, reference) < 1e-6f);
}

void run_cxl(cxltest::CXLClient &client, cxltest::Backend backend, std::size_t block_bytes,
             std::size_t cache_bytes, std::uint64_t base, cxltest::StencilKernel kernel) {
    constexpr std::size_t kN = 32;
    constexpr std::uint32_t kSteps = 2;
    const std::size_t plane_bytes = kN * kN * sizeof(float);

    cxltest::TieredArrayConfig config;
    config.backend = backend;
    config.size_bytes = 2 * plane_bytes;
    config.cxl_base_addr = base;
    config.block_bytes = block_bytes;
    config.cache_bytes = cache_bytes;

    cxltest::TieredArray grid(config, &client);
    const auto input = make_input(kN);
    grid.load(0, input.data(), plane_bytes);
    grid.reset_stats();

    const cxltest::StencilConfig stencil{kN, kSteps, kernel};
    const std::size_t final_plane = cxltest::stencil_run(grid, stencil);
    const auto stats = grid.stats();

    const auto got = cxltest::stencil_read_plane(grid, kN, final_plane);
    const auto reference = cxltest::stencil_reference(input, stencil);
    CHECK(max_abs_diff(got, reference) < 1e-6f);

    CHECK(stats.cache_evictions == 0);
    CHECK(stats.line_fetches == 128);
    CHECK(stats.cxl_writes > 0);
    CHECK(stats.modeled_ns > 0);

    if (kernel == cxltest::StencilKernel::Naive) {
        // Each step reads 3724 elements: 124 boundary cells with one load and
        // 900 interior cells with four neighbor loads.
        constexpr std::uint64_t kReadsPerStep = 3724;
        constexpr std::uint64_t kReads = kReadsPerStep * kSteps;
        if (backend == cxltest::Backend::CxlDirect) {
            CHECK(stats.cache_misses == 128);
            CHECK(stats.cache_hits == kReads - 128);
            CHECK(stats.block_fetches == 0);
        } else {
            CHECK(stats.cache_misses == 2);
            CHECK(stats.cache_hits == kReads - 2);
            CHECK(stats.block_fetches == 2);
        }
    } else {
        // Row-blocked: one row read per row; the first row of each plane misses.
        constexpr std::uint64_t kRows = kN;
        if (backend == cxltest::Backend::CxlDirect) {
            CHECK(stats.cache_misses == 128);
            CHECK(stats.cache_hits == 0);
            CHECK(stats.block_fetches == 0);
        } else {
            CHECK(stats.cache_misses == 2);
            CHECK(stats.cache_hits == (kRows - 1) * kSteps);
            CHECK(stats.block_fetches == 2);
        }
        CHECK(stats.cxl_writes == 128);
    }
}

} // namespace

int main() {
    run_dram(cxltest::StencilKernel::Naive);
    run_dram(cxltest::StencilKernel::RowBlocked);

    cxltest::CXLClient client("127.0.0.1", 9999);
    std::string error;
    if (client.connect(&error)) {
        std::printf("connected to cxlmemsim_server; running CXL stencil tests\n");
        run_cxl(client, cxltest::Backend::CxlDirect, 64 * 1024, 16 * 1024, 0x00300000,
                cxltest::StencilKernel::Naive);
        run_cxl(client, cxltest::Backend::CxlDirect, 64 * 1024, 16 * 1024, 0x00380000,
                cxltest::StencilKernel::RowBlocked);
        run_cxl(client, cxltest::Backend::CxlBlock, 4096, 8192, 0x00400000,
                cxltest::StencilKernel::Naive);
        run_cxl(client, cxltest::Backend::CxlBlock, 4096, 8192, 0x00480000,
                cxltest::StencilKernel::RowBlocked);
    } else {
        std::printf("SKIP: CXL stencil tests require cxlmemsim_server (%s)\n", error.c_str());
    }

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
