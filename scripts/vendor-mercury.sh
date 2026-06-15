#!/usr/bin/env bash
# Provision Mercury (Mochi) for SPDK's --with-mercury (spdk-xmu Slice C1).
#
# Builds Mercury with the na_ofi (libfabric) + na_sm (shared-mem) plugins into
# <worktree>/vendor/mercury-install, the prefix --with-mercury=<dir> expects.
# This captures the exact recipe validated in spdk-xmu.1 so a fresh checkout can
# reproduce the build (the vendor/ tree itself is a build artifact, not tracked).
#
# Requires: cmake, a C/C++ toolchain, libfabric-devel, boost headers.
#   Fedora: sudo dnf install cmake gcc-c++ libfabric libfabric-devel boost-devel
#
# NOTE: Mercury 2.4.1 is the floor — 2.4.0 fails to compile against libfabric 2.3
# (removed FI_LOG_SUBSYS_MAX / FI_LOG_MAX enum sentinels); 2.4.1 fixes it upstream.
set -euo pipefail

MERCURY_TAG=${MERCURY_TAG:-v2.4.1}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)   # spdk worktree root
VENDOR="$HERE/vendor"
SRC="$VENDOR/mercury"
BUILD="$SRC/build"
PREFIX="$VENDOR/mercury-install"

command -v cmake >/dev/null || { echo "FAIL: cmake not found" >&2; exit 1; }
pkg-config --exists libfabric || { echo "FAIL: libfabric-devel not found (pkg-config libfabric)" >&2; exit 1; }

mkdir -p "$VENDOR"
if [ ! -d "$SRC/.git" ]; then
  git clone --branch "$MERCURY_TAG" --depth 1 --recurse-submodules \
    https://github.com/mercury-hpc/mercury.git "$SRC"
fi

cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_SHARED_LIBS=ON \
  -DNA_USE_OFI=ON \
  -DNA_USE_SM=ON \
  -DMERCURY_USE_BOOST_PP=ON \
  -DMERCURY_USE_CHECKSUMS=ON \
  -DBUILD_TESTING=OFF
cmake --build "$BUILD" -j"$(nproc)"
cmake --install "$BUILD"

echo
echo "Mercury $MERCURY_TAG installed to $PREFIX"
echo "Configure SPDK with:  ./configure ... --with-mercury=$PREFIX"
echo "(at runtime, consumers need $PREFIX/lib on the loader path or an -rpath)"
