#include "cxl/cxl_client.h"
#include "memory/software_cache.h"
#include "memory/tiered_array.h"

#include <cinttypes>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
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

std::vector<std::uint8_t> make_pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 31u + seed) & 0xffu);
    }
    return out;
}

void test_software_cache() {
    // Four 64-byte lines, LRU.
    cxltest::SoftwareCache lru(256, 64, cxltest::SoftwareCache::Policy::Lru);
    CHECK(lru.line_count() == 4);

    std::vector<std::uint8_t> unit(64, 0x5a);
    CHECK(lru.lookup(0) == nullptr);
    CHECK(lru.lookup(0) == nullptr);
    CHECK(lru.misses() == 2);

    lru.insert(0, unit.data());
    const std::uint8_t *hit = lru.lookup(0);
    CHECK(hit != nullptr && hit[0] == 0x5a);
    CHECK(lru.hits() == 1);

    lru.insert(1, unit.data());
    lru.insert(2, unit.data());
    lru.insert(3, unit.data());
    CHECK(lru.evictions() == 0);
    lru.insert(4, unit.data());
    CHECK(lru.evictions() == 1);
    CHECK(lru.lookup(0) == nullptr); // least recently used was evicted
    CHECK(lru.lookup(4) != nullptr);

    lru.invalidate(4);
    CHECK(lru.lookup(4) == nullptr);

    // Direct-mapped: lines 0 and 4 collide in a four-line cache.
    cxltest::SoftwareCache dm(256, 64, cxltest::SoftwareCache::Policy::DirectMapped);
    dm.insert(0, unit.data());
    CHECK(dm.lookup(0) != nullptr);
    dm.insert(4, unit.data());
    CHECK(dm.evictions() == 1);
    CHECK(dm.lookup(0) == nullptr);
    CHECK(dm.lookup(4) != nullptr);
}

void test_dram() {
    constexpr std::size_t kSize = 4096;

    cxltest::TieredArrayConfig config;
    config.backend = cxltest::Backend::Dram;
    config.size_bytes = kSize;

    cxltest::TieredArray array(config);
    const auto pattern = make_pattern(kSize, 7);
    array.load(0, pattern.data(), pattern.size());

    std::vector<std::uint8_t> got(kSize, 0);
    array.read(0, got.data(), got.size());
    CHECK(got == pattern);
    CHECK(array.stats().host_writes == 1);
    CHECK(array.stats().host_bytes_written == kSize);
    CHECK(array.stats().host_reads == 1);
    CHECK(array.stats().host_bytes_read == kSize);

    array.reset_stats();
    array.store_value<std::uint32_t>(3, 0xdeadbeefu);
    CHECK(array.load_value<std::uint32_t>(3) == 0xdeadbeefu);

    array.store_value<float>(10, 3.5f);
    CHECK(array.load_value<float>(10) == 3.5f);

    array.store_at<std::uint16_t>(100, 0x1234u);
    CHECK(array.load_at<std::uint16_t>(100) == 0x1234u);

    bool threw = false;
    try {
        array.read(kSize - 4, got.data(), 16);
    } catch (const std::out_of_range &) {
        threw = true;
    }
    CHECK(threw);
}

void run_cxl_suite(cxltest::CXLClient &client, cxltest::Backend backend, std::size_t block_bytes,
                   std::size_t cache_bytes, std::uint64_t base) {
    constexpr std::size_t kSize = 8192;
    const std::size_t unit_bytes =
        backend == cxltest::Backend::CxlBlock ? block_bytes : cxltest::kCacheLineSize;
    const std::size_t units = kSize / unit_bytes;

    cxltest::TieredArrayConfig config;
    config.backend = backend;
    config.size_bytes = kSize;
    config.cxl_base_addr = base;
    config.block_bytes = block_bytes;
    config.cache_bytes = cache_bytes;

    cxltest::TieredArray array(config, &client);
    const auto pattern = make_pattern(kSize, 11);
    array.load(0, pattern.data(), pattern.size());
    array.reset_stats();

    std::vector<std::uint8_t> got(kSize, 0);
    array.read(0, got.data(), got.size());
    CHECK(got == pattern);

    const auto &first = array.stats();
    CHECK(first.cache_misses == units);
    CHECK(first.cache_hits == 0);
    CHECK(first.cache_evictions == 0);
    CHECK(first.line_fetches == kSize / cxltest::kCacheLineSize);
    CHECK(first.cxl_reads == kSize / cxltest::kCacheLineSize);
    CHECK(first.cxl_bytes_read == kSize);
    CHECK(first.block_fetches == (backend == cxltest::Backend::CxlBlock ? units : 0));
    CHECK(first.modeled_ns > 0);

    // A second full read should be served entirely from the software cache.
    array.reset_stats();
    array.read(0, got.data(), got.size());
    CHECK(got == pattern);
    CHECK(array.stats().cache_hits == units);
    CHECK(array.stats().cache_misses == 0);
    CHECK(array.stats().line_fetches == 0);

    // A read that crosses a fetch-unit boundary.
    array.reset_stats();
    std::vector<std::uint8_t> crossing(10, 0);
    array.read(unit_bytes - 6, crossing.data(), crossing.size());
    CHECK(crossing == std::vector<std::uint8_t>(pattern.begin() + (unit_bytes - 6),
                                                pattern.begin() + (unit_bytes + 4)));
    CHECK(array.stats().cache_hits == 2);
    CHECK(array.stats().cache_misses == 0);

    // Write-through invalidates the touched unit. A read-back must refetch it.
    array.reset_stats();
    const std::vector<std::uint8_t> update(20, 0xa5);
    array.write(100, update.data(), update.size());
    CHECK(array.stats().cxl_writes == 1);
    CHECK(array.stats().cxl_bytes_written == update.size());

    array.reset_stats();
    std::vector<std::uint8_t> readback(update.size(), 0);
    array.read(100, readback.data(), readback.size());
    CHECK(readback == update);
    CHECK(array.stats().cache_misses == 1);
    if (backend == cxltest::Backend::CxlDirect) {
        CHECK(array.stats().cxl_reads == 1);
        CHECK(array.stats().line_fetches == 1);
    } else {
        CHECK(array.stats().cxl_reads == unit_bytes / cxltest::kCacheLineSize);
        CHECK(array.stats().line_fetches == unit_bytes / cxltest::kCacheLineSize);
        CHECK(array.stats().block_fetches == 1);
    }
}

} // namespace

int main() {
    test_software_cache();
    test_dram();

    cxltest::CXLClient client("127.0.0.1", 9999);
    std::string error;
    if (client.connect(&error)) {
        std::printf("connected to cxlmemsim_server; running CXL backends\n");
        run_cxl_suite(client, cxltest::Backend::CxlDirect, 64 * 1024, 16 * 1024, 0x00100000);
        run_cxl_suite(client, cxltest::Backend::CxlBlock, 4096, 8192, 0x00200000);
    } else {
        std::printf("SKIP: CXL backend tests require cxlmemsim_server (%s)\n", error.c_str());
    }

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
