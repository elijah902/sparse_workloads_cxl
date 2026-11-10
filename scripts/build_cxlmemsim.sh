#!/usr/bin/env bash
# Build the pinned CXLMemSim server and its spdlog dependency.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXL_DIR="${CXL_DIR:-$ROOT/third_party/CXLMemSim}"
CXL_REPO="${CXL_REPO:-https://github.com/SlugLab/CXLMemSim.git}"
CXL_COMMIT="${CXL_COMMIT:-a9d46ecce92830455a666fb3b836fe67e6b2e245}"
SPDLOG_PREFIX="${SPDLOG_PREFIX:-$HOME/.local}"
SPDLOG_VERSION="${SPDLOG_VERSION:-v1.15.3}"
JOBS="${JOBS:-4}"

mkdir -p "$ROOT/third_party"

if [[ ! -d "$CXL_DIR/.git" ]]; then
    echo "cloning CXLMemSim into $CXL_DIR"
    git clone "$CXL_REPO" "$CXL_DIR"
fi

echo "checking out CXLMemSim $CXL_COMMIT"
git -C "$CXL_DIR" fetch --depth 1 origin "$CXL_COMMIT" 2>/dev/null || true
git -C "$CXL_DIR" checkout --detach "$CXL_COMMIT"

PATCH="$ROOT/patches/0001-remove-unused-bpf-include.patch"
if [[ -f "$PATCH" ]]; then
    if git -C "$CXL_DIR" apply --reverse --check "$PATCH" 2>/dev/null; then
        echo "patch already applied: $(basename "$PATCH")"
    else
        echo "applying $(basename "$PATCH")"
        git -C "$CXL_DIR" apply "$PATCH"
    fi
fi

if [[ ! -f "$SPDLOG_PREFIX/lib/cmake/spdlog/spdlogConfig.cmake" ]]; then
    echo "building spdlog $SPDLOG_VERSION into $SPDLOG_PREFIX"
    SPDLOG_SRC="$ROOT/third_party/spdlog"
    SPDLOG_BUILD="$ROOT/third_party/spdlog-build"
    if [[ ! -d "$SPDLOG_SRC/.git" ]]; then
        git clone --depth 1 --branch "$SPDLOG_VERSION" \
            https://github.com/gabime/spdlog.git "$SPDLOG_SRC"
    fi
    cmake -S "$SPDLOG_SRC" -B "$SPDLOG_BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SPDLOG_PREFIX" \
        -DSPDLOG_BUILD_EXAMPLE=OFF \
        -DSPDLOG_BUILD_TESTS=OFF \
        -DSPDLOG_BUILD_BENCH=OFF \
        -DSPDLOG_BUILD_SHARED=OFF
    cmake --build "$SPDLOG_BUILD" --target install -j"$JOBS"
fi

echo "configuring CXLMemSim"
cmake -S "$CXL_DIR" -B "$CXL_DIR/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$SPDLOG_PREFIX" \
    -DCXLMEMSIM_BUILD_MICROBENCHMARKS=OFF \
    -DCXLMEMSIM_ENABLE_SLUGALLOCATOR=OFF \
    -DCXLMEMSIM_ENABLE_RDMA=OFF

echo "building cxlmemsim_server"
cmake --build "$CXL_DIR/build" --target cxlmemsim_server -j"$JOBS"

echo "server: $CXL_DIR/build/cxlmemsim_server"
