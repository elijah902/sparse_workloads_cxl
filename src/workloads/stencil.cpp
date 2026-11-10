#include "workloads/stencil.h"

#include <algorithm>
#include <stdexcept>

namespace cxltest {
namespace {

// One update expression shared by the reference and both kernels so floating
// point results are bit-identical.
inline float stencil_update(float left, float right, float up, float down) {
    return 0.25f * (((left + right) + up) + down);
}

inline bool is_boundary(std::size_t n, std::size_t r, std::size_t c) {
    return r == 0 || r + 1 == n || c == 0 || c + 1 == n;
}

} // namespace

std::size_t stencil_run(TieredArray &grid, const StencilConfig &config) {
    if (config.n < 3) {
        throw std::invalid_argument("stencil_run: n must be at least 3");
    }

    const std::size_t n = config.n;
    const std::size_t plane_elems = n * n;
    const std::size_t plane_bytes = plane_elems * sizeof(float);
    if (grid.size_bytes() < 2 * plane_bytes) {
        throw std::invalid_argument("stencil_run: grid must hold two n*n float planes");
    }

    if (config.steps == 0) {
        return 0;
    }

    std::size_t current = 0;

    if (config.kernel == StencilKernel::Naive) {
        for (std::uint32_t step = 0; step < config.steps; ++step) {
            const std::size_t src = current * plane_elems;
            const std::size_t dst = (1 - current) * plane_elems;

            for (std::size_t r = 0; r < n; ++r) {
                for (std::size_t c = 0; c < n; ++c) {
                    const std::size_t i = r * n + c;
                    float value = 0.0f;
                    if (is_boundary(n, r, c)) {
                        value = grid.load_value<float>(src + i);
                    } else {
                        value = stencil_update(grid.load_value<float>(src + i - 1),
                                               grid.load_value<float>(src + i + 1),
                                               grid.load_value<float>(src + i - n),
                                               grid.load_value<float>(src + i + n));
                    }
                    grid.store_value<float>(dst + i, value);
                }
            }

            current = 1 - current;
        }
        return current;
    }

    // Row-blocked: keep three rows in DRAM, read each source row once, write
    // each destination row once. The per-row read/write calls coalesce CXL
    // traffic far better than per-cell accesses.
    std::vector<float> top(n, 0.0f);
    std::vector<float> middle(n, 0.0f);
    std::vector<float> bottom(n, 0.0f);
    std::vector<float> out(n, 0.0f);

    for (std::uint32_t step = 0; step < config.steps; ++step) {
        const std::size_t src = current * plane_elems;
        const std::size_t dst = (1 - current) * plane_elems;

        grid.read(src * sizeof(float), middle.data(), n * sizeof(float));

        for (std::size_t r = 0; r < n; ++r) {
            if (r + 1 < n) {
                grid.read((src + (r + 1) * n) * sizeof(float), bottom.data(), n * sizeof(float));
            }

            for (std::size_t c = 0; c < n; ++c) {
                if (is_boundary(n, r, c)) {
                    out[c] = middle[c];
                } else {
                    out[c] = stencil_update(middle[c - 1], middle[c + 1], top[c], bottom[c]);
                }
            }

            grid.write((dst + r * n) * sizeof(float), out.data(), n * sizeof(float));

            std::swap(top, middle);
            std::swap(middle, bottom);
        }

        current = 1 - current;
    }

    return current;
}

std::vector<float> stencil_reference(const std::vector<float> &input, const StencilConfig &config) {
    if (config.n < 3) {
        throw std::invalid_argument("stencil_reference: n must be at least 3");
    }

    const std::size_t n = config.n;
    if (input.size() != n * n) {
        throw std::invalid_argument("stencil_reference: input size must be n*n");
    }

    std::vector<float> a = input;
    std::vector<float> b(input.size(), 0.0f);

    for (std::uint32_t step = 0; step < config.steps; ++step) {
        for (std::size_t r = 0; r < n; ++r) {
            for (std::size_t c = 0; c < n; ++c) {
                const std::size_t i = r * n + c;
                if (is_boundary(n, r, c)) {
                    b[i] = a[i];
                } else {
                    b[i] = stencil_update(a[i - 1], a[i + 1], a[i - n], a[i + n]);
                }
            }
        }
        a.swap(b);
    }

    return a;
}

std::vector<float> stencil_read_plane(TieredArray &grid, std::size_t n, std::size_t plane) {
    const std::size_t plane_elems = n * n;
    if (plane > 1) {
        throw std::invalid_argument("stencil_read_plane: plane must be 0 or 1");
    }
    std::vector<float> out(plane_elems);
    grid.read(plane * plane_elems * sizeof(float), out.data(), out.size() * sizeof(float));
    return out;
}

std::uint64_t fnv1a(const void *data, std::size_t len, std::uint64_t seed) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t stencil_checksum(TieredArray &grid, std::size_t n, std::size_t plane) {
    const std::size_t plane_bytes = n * n * sizeof(float);
    std::vector<std::uint8_t> buffer(4096);
    std::uint64_t hash = 1469598103934665603ull;

    std::size_t offset = plane * plane_bytes;
    std::size_t remaining = plane_bytes;
    while (remaining > 0) {
        const std::size_t chunk = std::min(buffer.size(), remaining);
        grid.read(offset, buffer.data(), chunk);
        hash = fnv1a(buffer.data(), chunk, hash);
        offset += chunk;
        remaining -= chunk;
    }
    return hash;
}

} // namespace cxltest
