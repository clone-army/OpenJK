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
        build-essential cmake curl \
        gcc-multilib g++-multilib \
        libjpeg-dev:i386 \
        libpng-dev:i386 \
        zlib1g-dev:i386

    echo "==> Updating gsl-lite to v0.41.0..."
    GSL_DIR="$OPENJK_DIR/lib/gsl-lite"
    rm -rf "$GSL_DIR"/*
    curl -L https://github.com/gsl-lite/gsl-lite/archive/refs/tags/v0.41.0.tar.gz \
        | tar xz --strip-components=1 -C "$GSL_DIR"
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

echo "==> Stopping TEST Instance"

mbii -i chaos stop
mbii -i chaos stop

echo ""
echo "==> Installing binary to /usr/bin/spin.i386..."
sudo cp "$BUILD_DIR/mbiided.i386" /usr/bin/spin.i386
sudo chmod +x /usr/bin/spin.i386

echo "==> Done! Binary installed at /usr/bin/spin.i386"

echo "==> Restarting chaos Instance"
mbii -i chaos start