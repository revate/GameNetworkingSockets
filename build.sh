#!/bin/bash
# Build GameNetworkingSockets as a static library for Revate.
# Usage: ./build.sh [debug|release]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_TYPE="${1:-Release}"

if [[ "$BUILD_TYPE" == "debug" ]]; then
    BUILD_TYPE="Debug"
fi

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"

echo "[GNS] Building GameNetworkingSockets ($BUILD_TYPE)..."
echo "[GNS] Source: $SCRIPT_DIR"
echo "[GNS] Build:  $BUILD_DIR"

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DUSE_STEAMWEBRTC=OFF \
    -Dprotobuf_MSVC_STATIC_RUNTIME=OFF

cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -j"$(nproc)"

echo "[GNS] Build complete."
echo "[GNS] Static library: $BUILD_DIR/bin/libGameNetworkingSockets.a (or .lib)"
ls -lh "$BUILD_DIR"/bin/libGameNetworkingSockets* 2>/dev/null || \
ls -lh "$BUILD_DIR"/lib/libGameNetworkingSockets* 2>/dev/null || \
echo "[GNS] Searching for output..." && find "$BUILD_DIR" -name "*.a" -o -name "*.lib" | head -10
