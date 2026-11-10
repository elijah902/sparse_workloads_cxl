#pragma once

#include "memory/tiered_array.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cxltest {

enum class StencilKernel {
    Naive,      // Per-cell loads/stores. Good for counter calibration.
    RowBlocked, // Row buffers in DRAM; coalesces reads and writes.
};

struct StencilConfig {
    std::size_t n = 0;
    std::uint32_t steps = 0;
    StencilKernel kernel = StencilKernel::Naive;
};

// Runs `steps` 5-point stencil iterations over a two-plane grid. Plane 0 is the
// input; each step reads the current plane and writes the other. Returns the
// index of the final plane (0 or 1).
//
// Boundary cells (first/last row or column) are copied unchanged.
std::size_t stencil_run(TieredArray &grid, const StencilConfig &config);

// Reference implementation over a single host plane.
std::vector<float> stencil_reference(const std::vector<float> &input, const StencilConfig &config);

// Reads one n*n plane out of the grid. Useful for verification.
std::vector<float> stencil_read_plane(TieredArray &grid, std::size_t n, std::size_t plane);

std::uint64_t fnv1a(const void *data, std::size_t len, std::uint64_t seed = 1469598103934665603ull);

std::uint64_t stencil_checksum(TieredArray &grid, std::size_t n, std::size_t plane);

} // namespace cxltest
