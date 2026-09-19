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

# True only if the instance has zero connected players right now (via
# mbiiez's own instance.is_empty(), the same check its own -u/update flow
# relies on) - a build shouldn't boot players off a live game just to
# deploy. An instance that fails this (currently playing, or its instance
# object can't even be constructed) is left running the OLD binary rather
# than being force-restarted; it'll pick up the new build the next time
# build.sh runs while it happens to be empty.
is_instance_empty() {
    # Constructing instance() prints a stray blank line as a side effect
    # (live-tested: present even with stderr discarded) - `tail -n 1` so
    # that doesn't get captured alongside the real 1/0 answer and break
    # the caller's exact string comparison.
    /opt/openjk/venv/bin/python3 -c "
import sys
sys.path.insert(0, '/root/mbiiez')
from mbiiez.instance import instance
try:
    print('1' if instance('$1').is_empty() else '0')
except Exception:
    print('0')
" 2>/dev/null | tail -n 1
}

echo "==> Finding caded.i386 instances..."
INSTANCES_TO_RESTART=()
CONFIG_DIR="/root/mbiiez/configs"

if [ -d "$CONFIG_DIR" ]; then
    for config_file in "$CONFIG_DIR"/*.json; do
        if [ -f "$config_file" ]; then
            # Extract instance name from filename (without .json extension)
            instance_name=$(basename "$config_file" .json)

            # Check if this instance uses caded.i386 as engine - the one binary
            # this build always produces now, replacing the old per-feature
            # spin.i386/gungame.i386 builds (chaos/gungame/economy/bounty all
            # live in it together, toggled per-instance by cvars).
            if grep -q '"engine"\s*:\s*"caded\.i386"' "$config_file"; then
                if [ "$(is_instance_empty "$instance_name")" == "1" ]; then
                    echo "Stopping instance: $instance_name (empty)"
                    mbii -i "$instance_name" stop || true
                    INSTANCES_TO_RESTART+=("$instance_name")
                else
                    echo "Skipping instance: $instance_name (players connected - will keep running the old binary until it's next rebuilt while empty)"
                fi
            fi
        fi
    done
else
    echo "Warning: Config directory not found at $CONFIG_DIR"
    # Fallback to hardcoded instances
    echo "Checking fallback instances: open, legends, chaos, private, 99"
    for instance in open legends chaos private 99; do
        if [ "$(is_instance_empty "$instance")" == "1" ]; then
            echo "Stopping instance: $instance (empty)"
            mbii -i "$instance" stop || true
            INSTANCES_TO_RESTART+=("$instance")
        else
            echo "Skipping instance: $instance (players connected - will keep running the old binary until it's next rebuilt while empty)"
        fi
    done
fi

echo ""
echo "==> Installing binary to /usr/bin/caded.i386..."
# An instance with players connected (see is_instance_empty above) is
# deliberately left running against the OLD binary rather than being kicked
# - which means /usr/bin/caded.i386 can still be a live, currently-executing
# file right now. A plain `cp` onto that fails outright ("Text file busy").
# Copy to a temp file in the same directory, then atomically rename it into
# place: rename() replaces the directory entry without touching the still-
# running process's already-open file descriptor to the old inode at all -
# it keeps running completely undisturbed, while the next instance that
# execs "caded.i386" (on its next start/restart) picks up the new one.
sudo cp "$BUILD_DIR/mbiided.i386" /usr/bin/caded.i386.new
sudo chmod +x /usr/bin/caded.i386.new
sudo mv -f /usr/bin/caded.i386.new /usr/bin/caded.i386

echo "==> Done! Binary installed at /usr/bin/caded.i386"

echo "==> Restarting instances..."
for instance in "${INSTANCES_TO_RESTART[@]}"; do
    echo "Starting instance: $instance"
    mbii -i "$instance" start || true
done