#!/usr/bin/env bash
# Build a portable AppImage for PhotoPrint.
# Tools are downloaded once into build/appimage-tools (not committed).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$root/build/release}"
tools_dir="${APPIMAGE_TOOLS:-$root/build/appimage-tools}"
out_dir="${OUT_DIR:-$root/build/release}"
appdir="$root/build/AppDir"
version="$(sed -n 's/^project(PhotoPrint VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
version="${version:-0.0.0}"

if [ ! -x "$build_dir/photoprint" ]; then
    echo "Build the release preset first: cmake --preset release && cmake --build --preset release" >&2
    exit 1
fi

mkdir -p "$tools_dir" "$out_dir"
download() {
    local name="$1" url="$2"
    if [ ! -x "$tools_dir/$name" ]; then
        echo "Downloading $name"
        curl -L --fail --max-time 180 -o "$tools_dir/$name" "$url"
        chmod +x "$tools_dir/$name"
    fi
}
download linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
download linuxdeploy-plugin-qt-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
download appimagetool-x86_64.AppImage \
    https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage

export PATH="$tools_dir:$PATH"
export APPIMAGE_EXTRACT_AND_RUN=1
export NO_STRIP=1
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
export ARCH=x86_64

rm -rf "$appdir"
"$tools_dir/linuxdeploy-x86_64.AppImage" \
    --appdir "$appdir" \
    --executable "$build_dir/photoprint" \
    --desktop-file "$root/resources/photoprint.desktop" \
    --icon-file "$root/resources/photoprint.svg" \
    --plugin qt

# linuxdeploy only bundles the host's XCB platform plugin. Add Wayland for
# native Wayland desktops, and minimal for the headless --printers/--startup
# checks. The offscreen plugin is only needed by the development tests, so it
# is not shipped in the AppImage.
qt_plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
mkdir -p "$appdir/usr/plugins/platforms"
for plugin in libqwayland.so libqminimal.so; do
    if [ -f "$qt_plugins/platforms/$plugin" ]; then
        cp -a "$qt_plugins/platforms/$plugin" "$appdir/usr/plugins/platforms/"
    fi
done
for dir in wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
    if [ -d "$qt_plugins/$dir" ]; then
        cp -a "$qt_plugins/$dir" "$appdir/usr/plugins/"
    fi
done
if [ -f /usr/lib64/libQt6WaylandClient.so.6 ]; then
    cp -a /usr/lib64/libQt6WaylandClient.so.6* "$appdir/usr/lib/" 2>/dev/null || true
fi
# Drop Qt modules and image-format plugins the application never loads. Qt's
# PNG support is built in; only the JPEG plugin is needed.
rm -rf "$appdir/usr/qml" "$appdir/usr/plugins/qmltooling" \
    "$appdir/usr/plugins/platforminputcontexts" "$appdir/usr/plugins/platformthemes" \
    "$appdir/usr/plugins/styles" "$appdir/usr/plugins/iconengines"
find "$appdir/usr/plugins/imageformats" -type f ! -name 'libqjpeg.so' -delete
for lib in libQt6Pdf libQt6Quick libQt6Qml libQt6QmlModels libQt6QmlMeta \
           libQt6QmlWorkerScript libQt6Network libQt6PrintSupport \
           libQt6OpenGLWidgets libQt6Svg libKF6Archive libaom librav1e \
           libSvtAv1Enc libjxl libOpenEXR libOpenEXRCore libIex libIlmThread \
           libImath libheif libavif libtiff libwebp libopenjp2 libraw; do
    rm -f "$appdir"/usr/lib/${lib}*.so*
done

# Pull in anything the new plugins need that linuxdeploy did not see.
for _ in 1 2 3; do
    missing="$(find "$appdir" -name '*.so*' -type f -exec ldd {} \; 2>/dev/null \
        | awk '/not found/{print $1}' | sort -u)"
    [ -z "$missing" ] && break
    for lib in $missing; do
        cp -a "/usr/lib64/$lib" "$appdir/usr/lib/" 2>/dev/null \
            || cp -a "/lib64/$lib" "$appdir/usr/lib/" 2>/dev/null || true
    done
done

# AppImage root icon and .DirIcon.
magick "$root/resources/photoprint.svg" -background none -resize 256x256 \
    "$appdir/photoprint.png"
ln -sf photoprint.png "$appdir/.DirIcon"

"$tools_dir/appimagetool-x86_64.AppImage" "$appdir" \
    "$out_dir/PhotoPrint-$version-x86_64.AppImage"

echo "Built $out_dir/PhotoPrint-$version-x86_64.AppImage"
