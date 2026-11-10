#include "memory/software_cache.h"

#include <cstring>
#include <stdexcept>

namespace cxltest {

SoftwareCache::SoftwareCache(std::size_t capacity_bytes, std::size_t line_bytes, Policy policy)
    : policy_(policy), line_bytes_(line_bytes) {
    if (line_bytes_ == 0) {
        throw std::invalid_argument("SoftwareCache: line_bytes must be > 0");
    }

    // A cache with zero capacity still needs one slot to hold the unit being
    // consumed. Callers that want to bypass the cache should not construct one.
    std::size_t count = capacity_bytes / line_bytes_;
    if (count == 0) {
        count = 1;
    }

    slots_.resize(count);
    data_.resize(count * line_bytes_);

    if (policy_ == Policy::Lru) {
        free_slots_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            free_slots_.push_back(i);
        }
    }
}

std::uint8_t *SoftwareCache::slot_data(std::size_t slot) {
    return data_.data() + slot * line_bytes_;
}

std::size_t SoftwareCache::direct_index(std::uint64_t line_id) const {
    const auto count = static_cast<std::uint64_t>(slots_.size());
    return static_cast<std::size_t>(line_id % count);
}

const std::uint8_t *SoftwareCache::lookup(std::uint64_t line_id) {
    if (policy_ == Policy::DirectMapped) {
        const std::size_t index = direct_index(line_id);
        Slot &slot = slots_[index];
        if (slot.valid && slot.tag == line_id) {
            ++hits_;
            return slot_data(index);
        }
        ++misses_;
        return nullptr;
    }

    const auto it = lru_map_.find(line_id);
    if (it == lru_map_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    lru_order_.splice(lru_order_.begin(), lru_order_, it->second);
    return slot_data(*it->second);
}

const std::uint8_t *SoftwareCache::insert(std::uint64_t line_id, const std::uint8_t *src, bool *evicted) {
    bool did_evict = false;
    std::size_t slot = 0;

    if (policy_ == Policy::DirectMapped) {
        slot = direct_index(line_id);
        if (slots_[slot].valid && slots_[slot].tag != line_id) {
            did_evict = true;
        }
        slots_[slot].tag = line_id;
        slots_[slot].valid = true;
    } else {
        const auto it = lru_map_.find(line_id);
        if (it != lru_map_.end()) {
            slot = *it->second;
            lru_order_.splice(lru_order_.begin(), lru_order_, it->second);
        } else if (!free_slots_.empty()) {
            slot = free_slots_.back();
            free_slots_.pop_back();
            slots_[slot].tag = line_id;
            slots_[slot].valid = true;
            lru_order_.push_front(slot);
            lru_map_[line_id] = lru_order_.begin();
        } else {
            slot = lru_order_.back();
            lru_order_.pop_back();
            lru_map_.erase(slots_[slot].tag);
            did_evict = true;
            slots_[slot].tag = line_id;
            lru_order_.push_front(slot);
            lru_map_[line_id] = lru_order_.begin();
        }
    }

    std::memcpy(slot_data(slot), src, line_bytes_);
    if (did_evict) {
        ++evictions_;
    }
    if (evicted != nullptr) {
        *evicted = did_evict;
    }
    return slot_data(slot);
}

void SoftwareCache::invalidate(std::uint64_t line_id) {
    if (policy_ == Policy::DirectMapped) {
        const std::size_t index = direct_index(line_id);
        if (slots_[index].valid && slots_[index].tag == line_id) {
            slots_[index].valid = false;
        }
        return;
    }

    const auto it = lru_map_.find(line_id);
    if (it == lru_map_.end()) {
        return;
    }
    const std::size_t slot = *it->second;
    lru_order_.erase(it->second);
    lru_map_.erase(it);
    slots_[slot].valid = false;
    free_slots_.push_back(slot);
}

void SoftwareCache::clear() {
    if (policy_ == Policy::DirectMapped) {
        for (auto &slot : slots_) {
            slot.valid = false;
        }
        return;
    }

    lru_map_.clear();
    lru_order_.clear();
    free_slots_.clear();
    free_slots_.reserve(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slots_[i].valid = false;
        free_slots_.push_back(i);
    }
}

void SoftwareCache::reset_stats() {
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
}

} // namespace cxltest
