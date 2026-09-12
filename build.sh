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

echo "==> Finding and stopping spin.i386 instances..."
INSTANCES_TO_RESTART=()
CONFIG_DIR="/root/mbiiez/config"

if [ -d "$CONFIG_DIR" ]; then
    for config_file in "$CONFIG_DIR"/*.json; do
        if [ -f "$config_file" ]; then
            # Extract instance name from filename (without .json extension)
            instance_name=$(basename "$config_file" .json)
            
            # Check if this instance uses spin.i386 as engine
            if grep -q '"engine"\s*:\s*"spin\.i386"' "$config_file"; then
                echo "Stopping instance: $instance_name"
                mbii -i "$instance_name" stop || true
                INSTANCES_TO_RESTART+=("$instance_name")
            fi
        fi
    done
else
    echo "Warning: Config directory not found at $CONFIG_DIR"
    # Fallback to hardcoded instances
    echo "Stopping fallback instances: chaos and private"
    mbii -i chaos stop || true
    mbii -i private stop || true
    INSTANCES_TO_RESTART=("chaos" "private")
fi

echo ""
echo "==> Installing binary to /usr/bin/spin.i386..."
sudo cp "$BUILD_DIR/mbiided.i386" /usr/bin/spin.i386
sudo chmod +x /usr/bin/spin.i386

echo "==> Done! Binary installed at /usr/bin/spin.i386"

echo "==> Restarting instances..."
for instance in "${INSTANCES_TO_RESTART[@]}"; do
    echo "Starting instance: $instance"
    mbii -i "$instance" start || true
done