#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENJK_DIR="$SCRIPT_DIR"
BUILD_DIR="$OPENJK_DIR/build"

INSTALL_DEPS=0
for arg in "$@"; do
    [[ "$arg" == "--install" ]] && INSTALL_DEPS=1
done

if [[ "$INSTALL_DEPS" == "1" ]]; then
    echo "==> Installing dependencies..."
    sudo dpkg --add-architecture i386
    sudo apt-get update
    sudo apt-get install -y \
        build-essential cmake \
        gcc-multilib g++-multilib \
        libjpeg-dev:i386 \
        libpng-dev:i386 \
        zlib1g-dev:i386
fi

echo "==> Configuring CMake (i386)..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
rm -rf *

cmake \
    -DBuildMPDed=ON \
    -DBuildMPEngine=OFF \
    -DBuildMPRdVanilla=OFF \
    -DBuildMPCGame=OFF \
    -DBuildMPUI=OFF \
    -DBuildSPEngine=OFF \
    -DBuildSPGame=OFF \
    -DBuildSPRdVanilla=OFF \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/Toolchains/linux-i686.cmake \
    ..

echo "==> Building..."
make -j$(nproc)

echo ""
echo "==> Build complete. Binary:"
find "$BUILD_DIR" -name "*.i386" -o -name "mbiided*" 2>/dev/null