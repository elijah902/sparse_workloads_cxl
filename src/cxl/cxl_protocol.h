#pragma once

// Wire format shared with cxlmemsim_server. The server declares these structs
// as __attribute__((packed)) in src/main_server.cc; the layout must match
// exactly.

#include <cstddef>
#include <cstdint>

namespace cxltest {

inline constexpr std::uint8_t kOpRead = 0;
inline constexpr std::uint8_t kOpWrite = 1;
inline constexpr std::size_t kCacheLineSize = 64;

#pragma pack(push, 1)
struct ServerRequest {
    std::uint8_t op_type;
    std::uint64_t addr;
    std::uint64_t size;
    std::uint64_t timestamp;
    std::uint64_t value;
    std::uint64_t expected;
    std::uint8_t data[kCacheLineSize];
};

struct ServerResponse {
    std::uint8_t status;
    std::uint64_t latency_ns;
    std::uint64_t old_value;
    std::uint8_t data[kCacheLineSize];
};
#pragma pack(pop)

static_assert(sizeof(ServerRequest) == 105, "ServerRequest wire size changed");
static_assert(sizeof(ServerResponse) == 81, "ServerResponse wire size changed");

} // namespace cxltest
