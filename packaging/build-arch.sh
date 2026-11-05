#!/usr/bin/env bash
# Build the Arch Linux package inside an archlinux container.
#
# makepkg refuses to run as root and the Fedora host has no pacman, so the
# package is built in an archlinux container as a non-root builder user. The
# committed PKGBUILD pulls the released tag from GitHub, which also validates
# the exact recipe an Arch user would run.
#
# Without an argument the package targets x86_64; pass "arm64" to build the
# aarch64 package:
#   ./packaging/build-arch.sh            photoprint-1.2.2-1-x86_64.pkg.tar.zst
#   ./packaging/build-arch.sh arm64      photoprint-1.2.2-1-aarch64.pkg.tar.zst
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

platform_args=()
label="x86_64"
image="archlinux:base"
if [ "${1:-}" = "arm64" ]; then
    platform_args=(--platform linux/arm64)
    label="aarch64"
    # Arch publishes official images for x86_64 only; the arm64 base comes
    # from a community rebuild of the same rootfs.
    image="docker.io/menci/archlinuxarm:latest"
fi

# podman refuses to mount a host path that does not exist yet.
mkdir -p "$root/build/pkg-out"

echo "=== Arch package build ($label, container: $image) ==="
podman run --rm "${platform_args[@]}" \
    -v "$root:/src:ro" \
    -v "$root/build/pkg-out:/out" \
    "$image" \
    /bin/bash -c '
set -euo pipefail
mkdir -p /out

echo "=== Installing build dependencies ==="
pacman -Syu --noconfirm --needed base-devel git cmake ninja qt6-base \
    qt6-tools lcms2 libcups imagemagick

echo "=== Preparing the builder user ==="
useradd -m builder

echo "=== Building the package from the released tag ==="
mkdir -p /build
cp /src/packaging/PKGBUILD /build/
chown -R builder:builder /build
su builder -c "cd /build && makepkg -f --noconfirm"

echo "=== Copying result back ==="
cp -v /build/*.pkg.tar.zst /out/
ls -lh /out/
'
