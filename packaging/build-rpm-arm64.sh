#!/usr/bin/env bash
# Build the RPM for aarch64 inside a Fedora 44 container.
#
# The host can be x86_64; podman/qemu handles the emulation. The spec is the
# same packaging/photoprint.spec the host build uses, so both architectures
# ship the same file set. cmake-rpm-macros is listed explicitly because the
# weak dependency that normally pulls it in is skipped here.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== ARM64 RPM build (Fedora 44 aarch64 container) ==="
podman run --rm --platform linux/arm64 \
    -v "$root:/work" \
    -w /work \
    fedora:44 \
    /bin/bash -c '
set -euo pipefail

echo "=== Installing build dependencies ==="
dnf install -y --setopt=install_weak_deps=False rpm-build cmake cmake-rpm-macros \
    ninja-build gcc-c++ qt6-qtbase-devel qt6-linguist lcms2-devel cups-devel \
    zlib-devel ImageMagick-devel

echo "=== Building the aarch64 RPM ==="
rm -rf build/rpm-arm64
rpmbuild -bb packaging/photoprint.spec \
    --define "_topdir $PWD/build/rpm-arm64" \
    --define "photoprint_source $PWD"

echo "=== Result ==="
ls -lh build/rpm-arm64/RPMS/aarch64/
'
