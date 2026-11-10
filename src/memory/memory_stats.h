#pragma once

#include <cstdint>

namespace cxltest {

// Counters accumulated by a TieredArray. Setup traffic (loading a dataset into
// the CXL pool) is included until reset_stats() is called, so experiments
// should reset after setup and before the measured kernel.
struct MemoryStats {
    std::uint64_t host_reads = 0;
    std::uint64_t host_writes = 0;
    std::uint64_t host_bytes_read = 0;
    std::uint64_t host_bytes_written = 0;

    std::uint64_t cxl_reads = 0;
    std::uint64_t cxl_writes = 0;
    std::uint64_t cxl_bytes_read = 0;
    std::uint64_t cxl_bytes_written = 0;

    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t cache_evictions = 0;

    // A "line fetch" is one 64-byte request to the server. A block fetch is one
    // cache-line-group fill and may contain several line fetches.
    std::uint64_t line_fetches = 0;
    std::uint64_t block_fetches = 0;

    std::uint64_t modeled_ns = 0;
    std::uint64_t work_units = 0;

    void add(const MemoryStats &other) {
        host_reads += other.host_reads;
        host_writes += other.host_writes;
        host_bytes_read += other.host_bytes_read;
        host_bytes_written += other.host_bytes_written;
        cxl_reads += other.cxl_reads;
        cxl_writes += other.cxl_writes;
        cxl_bytes_read += other.cxl_bytes_read;
        cxl_bytes_written += other.cxl_bytes_written;
        cache_hits += other.cache_hits;
        cache_misses += other.cache_misses;
        cache_evictions += other.cache_evictions;
        line_fetches += other.line_fetches;
        block_fetches += other.block_fetches;
        modeled_ns += other.modeled_ns;
        work_units += other.work_units;
    }

    void reset() { *this = MemoryStats{}; }

    double cache_hit_rate() const {
        const std::uint64_t lookups = cache_hits + cache_misses;
        return lookups == 0 ? 0.0 : static_cast<double>(cache_hits) / static_cast<double>(lookups);
    }
};

} // namespace cxltest
