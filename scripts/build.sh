#!/usr/bin/env bash
# Prism - build both halves and stage them into build/dist
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_TYPE="${BUILD_TYPE:-Release}"
MINGW_PREFIX="${MINGW_PREFIX:-x86_64-w64-mingw32}"
JOBS="$(nproc 2>/dev/null || echo 4)"

die() { printf '\nerror: %s\n' "$1" >&2; exit 1; }

command -v "${MINGW_PREFIX}-g++" >/dev/null \
    || die "${MINGW_PREFIX}-g++ not found. Fedora: dnf install mingw64-gcc-c++"
command -v winegcc >/dev/null \
    || die "winegcc not found. Fedora: dnf install wine-devel"
pkg-config --exists libpipewire-0.3 \
    || die "libpipewire-0.3 development files not found. Fedora: dnf install pipewire-devel"
pkg-config --exists gio-unix-2.0 \
    || die "gio-unix-2.0 development files not found. Fedora: dnf install glib2-devel"

echo "==> Prism.exe (${MINGW_PREFIX}, ${BUILD_TYPE})"
cmake -B build/exe \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DPRISM_MINGW_PREFIX="${MINGW_PREFIX}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build build/exe -j"${JOBS}"

echo "==> PrismCapture.dll (winegcc)"
if [ "${BUILD_TYPE}" = "Debug" ]; then
    make -C bridge debug
else
    make -C bridge
fi

echo "==> staging build/dist"
mkdir -p build/dist
cp -f build/exe/dist/Prism.exe build/dist/
cp -f build/bridge/PrismCapture.dll build/dist/

echo
echo "Built:"
ls -la build/dist
echo
echo "Next: ./scripts/run-prism.sh --test-pattern"
