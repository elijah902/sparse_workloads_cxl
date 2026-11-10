#!/usr/bin/env bash
# Build this project's client, tests, and experiment binaries.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${JOBS:-4}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$JOBS"
