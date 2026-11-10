#include "cxl/cxl_client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cxltest {
namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string errno_string(const char *what) {
    return std::string(what) + ": " + std::strerror(errno);
}

} // namespace

CXLClient::CXLClient(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port) {}

CXLClient::~CXLClient() { close(); }

bool CXLClient::connect(std::string *error) {
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *result = nullptr;
    const std::string port = std::to_string(port_);
    const int rc = ::getaddrinfo(host_.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        if (error) {
            *error = "getaddrinfo(" + host_ + "): " + gai_strerror(rc);
        }
        return false;
    }

    std::string last_error = "no addresses";
    for (addrinfo *it = result; it != nullptr; it = it->ai_next) {
        const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            last_error = errno_string("socket");
            continue;
        }
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            fd_ = fd;
            break;
        }
        last_error = errno_string("connect");
        ::close(fd);
    }
    ::freeaddrinfo(result);

    if (fd_ < 0) {
        if (error) {
            *error = last_error;
        }
        return false;
    }

    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
}

void CXLClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool CXLClient::send_all(const void *buf, std::size_t len) {
    const auto *p = static_cast<const std::uint8_t *>(buf);
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd_, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool CXLClient::recv_all(void *buf, std::size_t len) {
    auto *p = static_cast<std::uint8_t *>(buf);
    std::size_t received = 0;
    while (received < len) {
        const ssize_t n = ::recv(fd_, p + received, len - received, MSG_WAITALL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        received += static_cast<std::size_t>(n);
    }
    return true;
}

bool CXLClient::transact(ServerRequest &req, ServerResponse &resp) {
    if (fd_ < 0) {
        ++stats_.errors;
        return false;
    }

    req.timestamp = now_ns();
    resp = {};

    if (!send_all(&req, sizeof(req)) || !recv_all(&resp, sizeof(resp))) {
        ++stats_.errors;
        return false;
    }
    if (resp.status != 0) {
        ++stats_.errors;
        return false;
    }
    return true;
}

bool CXLClient::read(std::uint64_t addr, void *out, std::size_t size, std::uint64_t *latency_ns) {
    if (size == 0 || size > kCacheLineSize || out == nullptr) {
        ++stats_.errors;
        return false;
    }

    ServerRequest req{};
    req.op_type = kOpRead;
    req.addr = addr;
    req.size = size;

    ServerResponse resp{};
    if (!transact(req, resp)) {
        return false;
    }

    std::memcpy(out, resp.data, size);
    ++stats_.reads;
    stats_.bytes_read += size;
    stats_.modeled_ns += resp.latency_ns;
    if (latency_ns != nullptr) {
        *latency_ns = resp.latency_ns;
    }
    return true;
}

bool CXLClient::write(std::uint64_t addr, const void *in, std::size_t size,
                      std::uint64_t *latency_ns) {
    if (size == 0 || size > kCacheLineSize || in == nullptr) {
        ++stats_.errors;
        return false;
    }

    ServerRequest req{};
    req.op_type = kOpWrite;
    req.addr = addr;
    req.size = size;
    std::memcpy(req.data, in, size);

    ServerResponse resp{};
    if (!transact(req, resp)) {
        return false;
    }

    ++stats_.writes;
    stats_.bytes_written += size;
    stats_.modeled_ns += resp.latency_ns;
    if (latency_ns != nullptr) {
        *latency_ns = resp.latency_ns;
    }
    return true;
}

bool CXLClient::read_line(std::uint64_t addr, std::array<std::uint8_t, kCacheLineSize> &out,
                          std::uint64_t *latency_ns) {
    if ((addr % kCacheLineSize) != 0) {
        ++stats_.errors;
        return false;
    }
    return read(addr, out.data(), out.size(), latency_ns);
}

bool CXLClient::write_line(std::uint64_t addr,
                           const std::array<std::uint8_t, kCacheLineSize> &in) {
    if ((addr % kCacheLineSize) != 0) {
        ++stats_.errors;
        return false;
    }
    return write(addr, in.data(), in.size());
}

} // namespace cxltest
