#include "memory/tiered_array.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace cxltest {
namespace {

std::size_t round_up(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

} // namespace

const char *to_string(Backend backend) {
    switch (backend) {
    case Backend::Dram:
        return "dram";
    case Backend::CxlDirect:
        return "cxl-direct";
    case Backend::CxlBlock:
        return "cxl-block";
    }
    return "unknown";
}

TieredArray::TieredArray(TieredArrayConfig config, CXLClient *client)
    : config_(config), client_(client) {
    if (config_.size_bytes == 0) {
        throw std::invalid_argument("TieredArray: size_bytes must be > 0");
    }

    storage_bytes_ = round_up(config_.size_bytes, kCacheLineSize);

    if (config_.backend == Backend::Dram) {
        storage_.assign(storage_bytes_, 0);
        return;
    }

    if (client_ == nullptr) {
        throw std::invalid_argument("TieredArray: CXL backend requires a connected CXLClient");
    }

    if (config_.backend == Backend::CxlBlock) {
        if (config_.block_bytes < kCacheLineSize) {
            throw std::invalid_argument("TieredArray: block_bytes must be at least 64");
        }
        fetch_line_bytes_ = round_up(config_.block_bytes, kCacheLineSize);
    } else {
        fetch_line_bytes_ = kCacheLineSize;
    }

    std::size_t cache_bytes = config_.cache_bytes;
    if (cache_bytes < fetch_line_bytes_) {
        cache_bytes = fetch_line_bytes_;
    }
    cache_ = std::make_unique<SoftwareCache>(cache_bytes, fetch_line_bytes_, config_.cache_policy);
    scratch_.assign(fetch_line_bytes_, 0);
}

void TieredArray::check_range(std::size_t offset, std::size_t len) const {
    if (offset > config_.size_bytes || len > config_.size_bytes - offset) {
        throw std::out_of_range("TieredArray: access outside the array");
    }
}

void TieredArray::clear_cache() {
    if (cache_) {
        cache_->clear();
    }
}

void TieredArray::load(std::size_t offset, const void *src, std::size_t len) {
    check_range(offset, len);
    if (len == 0) {
        return;
    }

    if (config_.backend == Backend::Dram) {
        std::memcpy(storage_.data() + offset, src, len);
        ++stats_.host_writes;
        stats_.host_bytes_written += len;
        return;
    }

    write_cxl(offset, static_cast<const std::uint8_t *>(src), len);
}

void TieredArray::write(std::size_t offset, const void *src, std::size_t len) {
    load(offset, src, len);
}

void TieredArray::read(std::size_t offset, void *dst, std::size_t len) {
    check_range(offset, len);
    if (len == 0) {
        return;
    }

    if (config_.backend == Backend::Dram) {
        std::memcpy(dst, storage_.data() + offset, len);
        ++stats_.host_reads;
        stats_.host_bytes_read += len;
        return;
    }

    auto *out = static_cast<std::uint8_t *>(dst);
    while (len > 0) {
        const std::uint64_t line_id = offset / fetch_line_bytes_;
        const std::size_t line_start = static_cast<std::size_t>(line_id) * fetch_line_bytes_;
        const std::size_t within = offset - line_start;
        const std::size_t chunk = std::min(len, fetch_line_bytes_ - within);

        const std::uint8_t *line = cache_->lookup(line_id);
        if (line == nullptr) {
            ++stats_.cache_misses;
            line = fetch_line(line_id);
        } else {
            ++stats_.cache_hits;
        }

        std::memcpy(out, line + within, chunk);
        offset += chunk;
        out += chunk;
        len -= chunk;
    }
}

const std::uint8_t *TieredArray::fetch_line(std::uint64_t line_id) {
    const std::size_t line_start = static_cast<std::size_t>(line_id) * fetch_line_bytes_;
    const std::size_t available = std::min(fetch_line_bytes_, config_.size_bytes - line_start);

    // Zero the tail so a partial final unit never contains stale bytes.
    std::fill(scratch_.begin(), scratch_.end(), 0);

    std::size_t done = 0;
    while (done < available) {
        const std::size_t chunk = std::min<std::size_t>(kCacheLineSize, available - done);
        const std::uint64_t addr = config_.cxl_base_addr + line_start + done;
        std::uint64_t latency_ns = 0;

        if (!client_->read(addr, scratch_.data() + done, chunk, &latency_ns)) {
            throw std::runtime_error("TieredArray: CXL read failed at address " + std::to_string(addr));
        }

        ++stats_.cxl_reads;
        stats_.cxl_bytes_read += chunk;
        stats_.modeled_ns += latency_ns;
        ++stats_.line_fetches;
        done += chunk;
    }

    if (config_.backend == Backend::CxlBlock) {
        ++stats_.block_fetches;
    }

    bool evicted = false;
    const std::uint8_t *line = cache_->insert(line_id, scratch_.data(), &evicted);
    if (evicted) {
        ++stats_.cache_evictions;
    }
    return line;
}

void TieredArray::write_cxl(std::size_t offset, const std::uint8_t *src, std::size_t len) {
    std::uint64_t last_invalidated = UINT64_MAX;
    std::size_t done = 0;

    while (done < len) {
        const std::size_t addr = offset + done;
        const std::size_t within = addr % kCacheLineSize;
        const std::size_t chunk = std::min<std::size_t>(kCacheLineSize - within, len - done);
        const std::uint64_t cxl_addr = config_.cxl_base_addr + addr;
        std::uint64_t latency_ns = 0;

        if (!client_->write(cxl_addr, src + done, chunk, &latency_ns)) {
            throw std::runtime_error("TieredArray: CXL write failed at address " + std::to_string(cxl_addr));
        }

        ++stats_.cxl_writes;
        stats_.cxl_bytes_written += chunk;
        stats_.modeled_ns += latency_ns;

        const std::uint64_t line_id = addr / fetch_line_bytes_;
        if (line_id != last_invalidated) {
            cache_->invalidate(line_id);
            last_invalidated = line_id;
        }

        done += chunk;
    }
}

} // namespace cxltest
