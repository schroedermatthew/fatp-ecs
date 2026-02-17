#!/usr/bin/env bash
# build.sh — FAT-P ECS build script (Linux/Mac)
# Usage: ./build.sh [--clean] [--no-visual] [--debug]

set -e

BUILD_DIR="build"
CONFIG="Release"
FATP_DIR="../FatP/include"
VISUAL="ON"
BENCH="OFF"

for arg in "$@"; do
    case "$arg" in
        --clean)
            if [ -d "$BUILD_DIR" ]; then
                echo "Cleaning build directory..."
                rm -rf "$BUILD_DIR"
            fi
            ;;
        --no-visual)  VISUAL="OFF" ;;
        --debug)      CONFIG="Debug" ;;
        --bench)      BENCH="ON" ;;
    esac
done

CMAKE_ARGS=(
    -B "$BUILD_DIR"
    -DFATP_INCLUDE_DIR="$FATP_DIR"
    -DCMAKE_BUILD_TYPE="$CONFIG"
    -DFATP_ECS_BUILD_VISUAL_DEMO="$VISUAL"
    -DFATP_ECS_BUILD_BENCH="$BENCH"
)

echo "Configuring ($CONFIG)..."
cmake "${CMAKE_ARGS[@]}"

echo "Building ($CONFIG)..."
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "Running tests..."
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo ""
echo "Build complete. Binaries in $BUILD_DIR/"
echo "  demo              — terminal demo"
[ "$VISUAL" = "ON" ] && echo "  visual_demo       — SDL2 visual demo"
[ "$BENCH" = "ON" ] && echo "  benchmark         — FAT-P ECS vs EnTT benchmark"
