#!/usr/bin/env bash
# Build PhotoPrint for aarch64 inside a Fedora 44 container.
#
# The host can be x86_64; podman/qemu handles the emulation. The container
# sees the source at /work and builds into build/release-arm64, so the host
# x86_64 build in build/release is never touched. The directory is wiped
# inside the container first, because a cache created on the host with
# different absolute paths makes CMake refuse to configure, and a stale
# cache would also point the build at host libraries.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== ARM64 build (Fedora 44 container) ==="
podman run --rm --platform linux/arm64 \
    -v "$root:/work" \
    -w /work \
    fedora:44 \
    /bin/bash -c '
set -euo pipefail

export QT_QPA_PLATFORM=offscreen
echo "=== Installing dependencies ==="
dnf install -y --setopt=install_weak_deps=False cmake ninja-build gcc-c++ git \
    qt6-qtbase-devel qt6-linguist lcms2-devel cups-devel zlib-devel \
    ImageMagick-devel poppler-utils dejavu-sans-fonts

echo "=== Configure (Release, aarch64) ==="
rm -rf build/release-arm64
cmake -S . -B build/release-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release

echo "=== Build (Release, aarch64) ==="
cmake --build build/release-arm64

echo "=== ARM64 build complete ==="
ls -lh build/release-arm64/photoprint
'
