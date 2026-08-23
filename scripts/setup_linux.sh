#!/usr/bin/env bash
set -euo pipefail

apt-get update
apt-get install -y \
    build-essential \
    gcc-14 g++-14 \
    clang-18 \
    cmake \
    ninja-build \
    git \
    pkg-config \
    libjemalloc-dev \
    liburing-dev \
    libssl-dev \
    libboost-dev \
    libboost-system-dev \
    libdw-dev

update-alternatives --install /usr/bin/gcc     gcc     /usr/bin/gcc-14     14
update-alternatives --install /usr/bin/g++     g++     /usr/bin/g++-14     14
update-alternatives --install /usr/bin/gcov    gcov    /usr/bin/gcov-14    14

echo "Setup complete. Compiler: $(g++ --version | head -1)"
