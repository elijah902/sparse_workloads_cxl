#pragma once

#include "cxl/cxl_protocol.h"

#include <array>
#include <cstdint>
#include <string>

namespace cxltest {

// A single-connection client for cxlmemsim_server's TCP protocol.
//
// The client is not thread-safe: use one connection per thread. The server is
// thread-per-connection, so multiple clients can run concurrently.
class CXLClient {
public:
    struct Stats {
        std::uint64_t reads = 0;
        std::uint64_t writes = 0;
        std::uint64_t bytes_read = 0;
        std::uint64_t bytes_written = 0;
        std::uint64_t modeled_ns = 0;
        std::uint64_t errors = 0;
    };

    CXLClient(std::string host, std::uint16_t port);
    ~CXLClient();

    CXLClient(const CXLClient &) = delete;
    CXLClient &operator=(const CXLClient &) = delete;

    // Returns false and fills `error` on failure.
    bool connect(std::string *error = nullptr);
    void close();
    bool connected() const { return fd_ >= 0; }

    // Read/write up to 64 bytes. `addr` is an offset into the server's CXL pool.
    bool read(std::uint64_t addr, void *out, std::size_t size, std::uint64_t *latency_ns = nullptr);
    bool write(std::uint64_t addr, const void *in, std::size_t size);

    // Cache-line helpers. `addr` must be 64-byte aligned.
    bool read_line(std::uint64_t addr, std::array<std::uint8_t, kCacheLineSize> &out,
                   std::uint64_t *latency_ns = nullptr);
    bool write_line(std::uint64_t addr, const std::array<std::uint8_t, kCacheLineSize> &in);

    const Stats &stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

private:
    bool transact(ServerRequest &req, ServerResponse &resp);
    bool send_all(const void *buf, std::size_t len);
    bool recv_all(void *buf, std::size_t len);

    std::string host_;
    std::uint16_t port_;
    int fd_ = -1;
    Stats stats_{};
};

} // namespace cxltest
