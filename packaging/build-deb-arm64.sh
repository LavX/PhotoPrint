#!/usr/bin/env bash
# Build the Debian package for arm64 inside a Debian trixie container.
#
# Same recipe as build-deb.sh, but the container runs as linux/arm64, so
# dpkg-buildpackage produces photoprint_<version>_arm64.deb. The host can be
# x86_64; podman/qemu handles the emulation, so the build is slower than a
# native one.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# podman refuses to mount a host path that does not exist yet.
mkdir -p "$root/build/deb-out-arm64"

echo "=== Debian arm64 build (Debian trixie arm64 container) ==="
podman run --rm --platform linux/arm64 \
    -v "$root:/src:ro" \
    -v "$root/build/deb-out-arm64:/out" \
    -w /src \
    debian:trixie \
    /bin/bash -c '
set -euo pipefail
mkdir -p /out

echo "=== Installing build dependencies ==="
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential debhelper cmake ninja-build qt6-base-dev qt6-tools-dev-tools \
    qt6-l10n-tools libxkbcommon-dev \
    liblcms2-dev libcups2-dev zlib1g-dev libmagickwand-7.q16-dev poppler-utils

echo "=== Copying source to writable dir ==="
# build/ and Testing/ hold multi-gigabyte build artefacts the package build
# never reads; copying them slows the container down for nothing.
rm -rf /work
mkdir -p /work
tar -C /src --exclude=./build --exclude=./Testing -cf - . | tar -C /work -xf -

echo "=== Building package ==="
cd /work
dpkg-buildpackage -us -uc -b

echo "=== Copying result back ==="
cp -v ../*.deb /out/
ls -lh /out/
'
