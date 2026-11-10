#include "cxl/cxl_client.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr std::uint64_t kLineA = 0x1000;
constexpr std::uint64_t kLineB = 0x1040;
constexpr std::uint64_t kReadIterations = 1000;

std::array<std::uint8_t, cxltest::kCacheLineSize> pattern(std::uint8_t seed) {
    std::array<std::uint8_t, cxltest::kCacheLineSize> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i * 3);
    }
    return out;
}

int fail(const char *message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    std::uint16_t port = 9999;
    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    }

    cxltest::CXLClient client(host, port);
    std::string error;
    if (!client.connect(&error)) {
        std::fprintf(stderr,
                     "SKIP: cannot connect to cxlmemsim_server at %s:%u (%s)\n"
                     "Start it with: ./scripts/run_server.sh\n",
                     host, static_cast<unsigned>(port), error.c_str());
        return 77;
    }
    std::printf("connected to cxlmemsim_server at %s:%u\n", host, static_cast<unsigned>(port));

    const auto a = pattern(7);
    const auto b = pattern(200);

    if (!client.write_line(kLineA, a)) {
        return fail("write line A");
    }
    if (!client.write_line(kLineB, b)) {
        return fail("write line B");
    }

    std::array<std::uint8_t, cxltest::kCacheLineSize> got_a{};
    std::array<std::uint8_t, cxltest::kCacheLineSize> got_b{};
    std::uint64_t latency_a = 0;
    std::uint64_t latency_b = 0;
    if (!client.read_line(kLineA, got_a, &latency_a)) {
        return fail("read line A");
    }
    if (!client.read_line(kLineB, got_b, &latency_b)) {
        return fail("read line B");
    }

    if (got_a != a) {
        return fail("line A data mismatch");
    }
    if (got_b != b) {
        return fail("line B data mismatch");
    }
    if (latency_a == 0 || latency_b == 0) {
        return fail("modeled latency was zero");
    }

    std::printf("line A ok: latency=%llu ns\n", static_cast<unsigned long long>(latency_a));
    std::printf("line B ok: latency=%llu ns\n", static_cast<unsigned long long>(latency_b));

    // A short repeated-read loop checks that the connection stays healthy and
    // gives a first look at the modeled latency distribution.
    std::uint64_t sum_latency = 0;
    std::uint64_t min_latency = UINT64_MAX;
    std::uint64_t max_latency = 0;
    for (std::uint64_t i = 0; i < kReadIterations; ++i) {
        const std::uint64_t addr = kLineA + (i % 16) * cxltest::kCacheLineSize;
        std::array<std::uint8_t, cxltest::kCacheLineSize> tmp{};
        std::uint64_t latency = 0;
        if (!client.read_line(addr, tmp, &latency)) {
            return fail("repeated read");
        }
        sum_latency += latency;
        min_latency = latency < min_latency ? latency : min_latency;
        max_latency = latency > max_latency ? latency : max_latency;
    }

    const auto &stats = client.stats();
    std::printf("repeated reads: %llu, avg modeled latency=%.1f ns, min=%llu, max=%llu\n",
                static_cast<unsigned long long>(kReadIterations),
                static_cast<double>(sum_latency) / static_cast<double>(kReadIterations),
                static_cast<unsigned long long>(min_latency),
                static_cast<unsigned long long>(max_latency));
    std::printf("stats: reads=%llu writes=%llu bytes_read=%llu bytes_written=%llu "
                "modeled_ns=%llu errors=%llu\n",
                static_cast<unsigned long long>(stats.reads),
                static_cast<unsigned long long>(stats.writes),
                static_cast<unsigned long long>(stats.bytes_read),
                static_cast<unsigned long long>(stats.bytes_written),
                static_cast<unsigned long long>(stats.modeled_ns),
                static_cast<unsigned long long>(stats.errors));

    if (stats.errors != 0) {
        return fail("client reported errors");
    }

    std::printf("PASS\n");
    return 0;
}
