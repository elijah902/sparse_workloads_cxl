#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace cxltest {

// A fixed-capacity software cache over CXL fetch units.
//
// The cache is parameterized by the fetch unit size: 64 bytes for CXL direct
// mode, or a larger tile for CXL block mode. `line_id` is the byte address
// divided by the fetch unit size, so the cache is independent of the byte-level
// addressing used by TieredArray.
class SoftwareCache {
public:
    enum class Policy { Lru, DirectMapped };

    SoftwareCache(std::size_t capacity_bytes, std::size_t line_bytes, Policy policy);

    std::size_t line_bytes() const { return line_bytes_; }
    std::size_t line_count() const { return slots_.size(); }
    std::size_t capacity_bytes() const { return line_bytes_ * slots_.size(); }
    bool enabled() const { return !slots_.empty(); }

    // On hit, returns a pointer to the cached unit and moves it to the front
    // (LRU) or leaves it in place (direct-mapped). On miss, returns nullptr.
    // Both outcomes update the hit/miss counters.
    const std::uint8_t *lookup(std::uint64_t line_id);

    // Inserts (or replaces) a unit. Returns a pointer to the cached copy.
    // `evicted` is set when an existing unit was displaced.
    const std::uint8_t *insert(std::uint64_t line_id, const std::uint8_t *src, bool *evicted = nullptr);

    // Drops a unit if present. Used after write-through so the next read sees
    // the updated backing store.
    void invalidate(std::uint64_t line_id);

    // Drops every unit, leaving the cache cold. Used between measured runs so
    // each run starts from the same cache state without rewriting the pool.
    void clear();

    std::uint64_t hits() const { return hits_; }
    std::uint64_t misses() const { return misses_; }
    std::uint64_t evictions() const { return evictions_; }
    void reset_stats();

private:
    struct Slot {
        std::uint64_t tag = 0;
        bool valid = false;
    };

    std::uint8_t *slot_data(std::size_t slot);
    std::size_t direct_index(std::uint64_t line_id) const;

    Policy policy_;
    std::size_t line_bytes_;
    std::vector<Slot> slots_;
    std::vector<std::uint8_t> data_;

    // LRU state. The map stores the position of each cached unit in the
    // recency list so hits and invalidations are O(1).
    std::unordered_map<std::uint64_t, std::list<std::size_t>::iterator> lru_map_;
    std::list<std::size_t> lru_order_;
    std::vector<std::size_t> free_slots_;

    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t evictions_ = 0;
};

} // namespace cxltest
