#!/usr/bin/env bash
# Build the Debian package inside a Debian container.
#
# The host is Fedora; the Debian build dependencies (qt6-base-dev,
# libmagickwand-7.q16-dev and friends) do not exist there. The package is
# built in a Debian trixie container. The source is mounted read-only at
# /src and copied to a writable directory inside the container, because
# dpkg-buildpackage needs to write build artefacts next to the source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== Debian build (Debian trixie container) ==="
podman run --rm \
    -v "$root:/src:ro" \
    -v "$root/build/deb-out:/out" \
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
