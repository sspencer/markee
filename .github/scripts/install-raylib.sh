#!/usr/bin/env bash
# Build and install raylib from source on Debian/Ubuntu.
#
# Ubuntu does not ship a raylib development package, so CI and release jobs
# build the pinned version below and register it with pkg-config (and the
# dynamic linker when built shared).
#
# Usage: .github/scripts/install-raylib.sh [version] [prefix]
#   RAYLIB_SHARED=OFF builds a static libraylib.a instead (used for portable
#   release binaries; link with `pkg-config --static --libs raylib`).
set -euo pipefail

RAYLIB_VERSION="${1:-${RAYLIB_VERSION:-6.2}}"
PREFIX="${2:-/usr/local}"
SUDO=""
if [[ "$(id -u)" -ne 0 ]]; then SUDO="sudo"; fi

$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
  build-essential cmake git pkg-config ca-certificates \
  libasound2-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev \
  libxinerama-dev libgl1-mesa-dev libglu1-mesa-dev libwayland-dev libxkbcommon-dev

SRC_DIR="$(mktemp -d)"
trap 'rm -rf "$SRC_DIR"' EXIT
git clone --depth 1 --branch "$RAYLIB_VERSION" https://github.com/raysan5/raylib.git "$SRC_DIR/raylib"
cmake -S "$SRC_DIR/raylib" -B "$SRC_DIR/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS="${RAYLIB_SHARED:-ON}" \
  -DBUILD_EXAMPLES=OFF \
  -DPLATFORM=Desktop
cmake --build "$SRC_DIR/build" --parallel "$(nproc)"
$SUDO cmake --install "$SRC_DIR/build"
$SUDO ldconfig
pkg-config --modversion raylib
