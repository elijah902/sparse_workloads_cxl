#pragma once

#include "cxl/cxl_client.h"
#include "memory/memory_stats.h"
#include "memory/software_cache.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace cxltest {

enum class Backend {
    Dram,      // Host DRAM; accesses are memcpy from a local copy.
    CxlDirect, // CXL pool; fetch one 64-byte line on demand.
    CxlBlock,  // CXL pool; fetch a tile on demand, then serve from the tile.
};

const char *to_string(Backend backend);

struct TieredArrayConfig {
    Backend backend = Backend::Dram;
    std::size_t size_bytes = 0;

    // Offset of this array inside the server's CXL pool. Required for CXL.
    std::uint64_t cxl_base_addr = 0;

    // Tile size for CxlBlock. Ignored by the other backends.
    std::size_t block_bytes = 64 * 1024;

    // Software cache capacity. For CXL, values below one fetch unit are raised
    // to one fetch unit so the backend always has somewhere to place a fetch.
    std::size_t cache_bytes = 0;
    SoftwareCache::Policy cache_policy = SoftwareCache::Policy::Lru;
};

// A byte-addressed array whose backing store is either host DRAM or the
// simulated CXL pool. Algorithms use the typed load/store helpers and stay
// unaware of the transport.
//
// The CXL backends keep a software cache of fetch units:
//   * CxlDirect: 64-byte lines.
//   * CxlBlock:  block_bytes tiles, fetched as consecutive 64-byte requests
//                 because the server protocol caps a request at 64 bytes.
// Writes are write-through and invalidate the touched cache unit.
class TieredArray {
public:
    TieredArray(TieredArrayConfig config, CXLClient *client = nullptr);

    TieredArray(const TieredArray &) = delete;
    TieredArray &operator=(const TieredArray &) = delete;

    // Populate the backing store. For DRAM this is a memcpy; for CXL it issues
    // writes to the pool. Intended for setup, not for the measured kernel.
    void load(std::size_t offset, const void *src, std::size_t len);
    void read(std::size_t offset, void *dst, std::size_t len);
    void write(std::size_t offset, const void *src, std::size_t len);

    template <typename T>
    T load_at(std::size_t byte_offset) {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        T value{};
        read(byte_offset, &value, sizeof(T));
        return value;
    }

    template <typename T>
    void store_at(std::size_t byte_offset, T value) {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        write(byte_offset, &value, sizeof(T));
    }

    template <typename T>
    T load_value(std::size_t index) {
        return load_at<T>(index * sizeof(T));
    }

    template <typename T>
    void store_value(std::size_t index, T value) {
        store_at<T>(index * sizeof(T), value);
    }

    const MemoryStats &stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }
    void add_work_units(std::uint64_t units) { stats_.work_units += units; }

    Backend backend() const { return config_.backend; }
    std::size_t size_bytes() const { return config_.size_bytes; }
    std::size_t fetch_line_bytes() const { return fetch_line_bytes_; }

private:
    void check_range(std::size_t offset, std::size_t len) const;
    const std::uint8_t *fetch_line(std::uint64_t line_id);
    void write_cxl(std::size_t offset, const std::uint8_t *src, std::size_t len);

    TieredArrayConfig config_;
    CXLClient *client_ = nullptr;
    std::vector<std::uint8_t> storage_;
    std::unique_ptr<SoftwareCache> cache_;
    std::vector<std::uint8_t> scratch_;
    std::size_t fetch_line_bytes_ = kCacheLineSize;
    std::size_t storage_bytes_ = 0;
    MemoryStats stats_{};
};

} // namespace cxltest
