#!/usr/bin/env python3
# Assemble the Arch Linux package without pacman.
#
# makepkg needs a working pacman, which is unavailable when building the
# aarch64 package under qemu-user emulation: pacman's Landlock sandbox
# cannot start there. The aarch64 binary already exists, produced by the
# Fedora 44 container that build-arm64.sh and build-rpm-arm64.sh drive, so
# this script lays out the same files the PKGBUILD's package() installs and
# writes the package manifest by hand. The result is a .pkg.tar.zst that
# pacman accepts like any other local package.
import io
import os
import subprocess
import sys
import tarfile
import time

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

label = sys.argv[1] if len(sys.argv) > 1 else "aarch64"
arch = "aarch64" if label == "aarch64" else "x86_64"
binpath = os.path.join(root, "build", "release-arm64", "photoprint") \
    if label == "aarch64" else os.path.join(root, "build", "release", "photoprint")

if not os.path.isfile(binpath):
    sys.exit(f"binary not found: {binpath}, run packaging/build-arm64.sh first")

pkgname = "photoprint"
pkgver = "1.2.2"
pkgrel = 1
# Keep the depends list identical to the PKGBUILD: same runtime libraries,
# so the assembled package and the makepkg package are interchangeable.
depends = ["gcc-libs", "qt6-base", "lcms2", "libcups", "imagemagick", "zlib"]
pkgdesc = "Arrange, crop and print photos on paper"
url = "https://github.com/LavX/PhotoPrint"
pkgdir = "/usr"

# Files the package ships, mapping the destination path inside the package
# to the source file, in the same layout the PKGBUILD's package() uses.
payload = [
    (f"{pkgdir}/bin/photoprint", binpath, 0o755),
    (f"{pkgdir}/share/applications/photoprint.desktop",
     os.path.join(root, "resources", "photoprint.desktop"), 0o644),
    (f"{pkgdir}/share/icons/hicolor/scalable/apps/photoprint.svg",
     os.path.join(root, "resources", "photoprint.svg"), 0o644),
    (f"{pkgdir}/share/icons/hicolor/256x256/apps/photoprint.png",
     os.path.join(root, "resources", "photoprint-256.png"), 0o644),
    (f"{pkgdir}/share/licenses/{pkgname}/LICENSE",
     os.path.join(root, "LICENSE"), 0o644),
]

installed_size = sum(os.path.getsize(src) for _, src, _ in payload)

pkginfo = "\n".join(
    [f"pkgname = {pkgname}",
     f"pkgbase = {pkgname}",
     f"pkgver = {pkgver}-{pkgrel}",
     f"pkgdesc = {pkgdesc}",
     f"url = {url}",
     f"builddate = {int(time.time())}",
     "packager = Laszlo Toth <lavx@lavx.hu>",
     f"size = {installed_size}",
     f"arch = {arch}",
     "license = MIT"]
    + [f"depend = {d}" for d in depends]) + "\n"

# .MTREE entries follow makepkg's format; pacman verifies file identity
# against them when installing. Paths are relative to the package root.
mtree_lines = ["#mtree", "/set type=file uid=0 gid=0 mode=644"]
for dest, src, mode in sorted(payload):
    sha = subprocess.run(["sha256sum", src], capture_output=True,
                         text=True, check=True).stdout.split()[0]
    mtree_lines.append(
        f"{dest} time={int(time.time())}.0 mode={mode:o} uid=0 gid=0 "
        f"size={os.path.getsize(src)} sha256digest={sha}")
mtree = "\n".join(mtree_lines) + "\n"

out_dir = os.path.join(root, "build", "pkg-out")
os.makedirs(out_dir, exist_ok=True)
out_pkg = os.path.join(out_dir, f"{pkgname}-{pkgver}-{pkgrel}-{arch}.pkg.tar.zst")
tmp_pkg = out_pkg + ".tmp"

with open(tmp_pkg, "wb") as raw:
    comp = subprocess.Popen(["zstd", "-q", "-19", "-c", "-"],
                            stdin=subprocess.PIPE, stdout=raw)
    with tarfile.open(fileobj=comp.stdin, mode="w|") as tar:
        for name, content_bytes in ((".PKGINFO", pkginfo.encode()),
                                    (".MTREE", mtree.encode())):
            info = tarfile.TarInfo(name)
            info.size = len(content_bytes)
            info.mode = 0o644
            tar.addfile(info, io.BytesIO(content_bytes))
        for dest, src, mode in payload:
            info = tarfile.TarInfo(dest.lstrip("/"))
            info.size = os.path.getsize(src)
            info.mode = mode
            with open(src, "rb") as f:
                tar.addfile(info, f)
    comp.stdin.close()
    if comp.wait() != 0:
        sys.exit("zstd compression failed")
os.replace(tmp_pkg, out_pkg)
print(f"created {out_pkg} ({os.path.getsize(out_pkg)} bytes)")
