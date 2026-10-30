#!/usr/bin/env bash
# Build a portable AppImage for PhotoPrint.
#
# The helper tools are downloaded once into build/appimage-tools (not
# committed) and reused afterwards: a rebuild with the tools already present
# never touches the network.
#
# Every tool is verified against a pinned SHA-256 before it is executed. The
# pins name the current upstream "continuous" builds, which are mutable, so a
# new upstream build needs a new pin here (sha256sum <file>). Each pin can be
# overridden for a single run when a different build is trusted:
#
#   LINUXDEPLOY_SHA256=<hex>     linuxdeploy-x86_64.AppImage
#   LINUXDEPLOY_QT_SHA256=<hex>  linuxdeploy-plugin-qt-x86_64.AppImage
#   APPIMAGETOOL_SHA256=<hex>    appimagetool-x86_64.AppImage
#
# A cached tool that does not match its pin is refused; delete it to fetch the
# pinned build again.
set -euo pipefail

LINUXDEPLOY_SHA256="${LINUXDEPLOY_SHA256:-8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1}"
LINUXDEPLOY_QT_SHA256="${LINUXDEPLOY_QT_SHA256:-cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617}"
APPIMAGETOOL_SHA256="${APPIMAGETOOL_SHA256:-793fca0ce4bcc309dc65d3f66669f58e3755643e4dadbe03932e7b436128494f}"

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

command -v sha256sum >/dev/null 2>&1 || {
    echo "sha256sum is required to verify the AppImage tools" >&2
    exit 1
}

mkdir -p "$tools_dir" "$out_dir"

hash_of() {
    sha256sum "$1" | awk '{print $1}'
}

# Compares a file against its pin and explains both ways out of a mismatch.
check_hash() {
    local path="$1" expected="$2" name="$3" override="$4"
    local actual
    actual="$(hash_of "$path")"
    [ "$actual" = "$expected" ] && return 0
    {
        echo "$name failed its SHA-256 check; it was not used"
        echo "  expected: $expected"
        echo "  actual:   $actual"
        echo "A cached copy is discarded and fetched again automatically. If this"
        echo "was a fresh download, the upstream continuous build changed and the"
        echo "pin in this script must be updated. Alternatively set $override to"
        echo "the hash you trust."
    } >&2
    return 1
}

# Fetch once, verify, then move into place, so a partial or wrong download is
# never executed. A tool that is already present is verified but not fetched.
ensure_tool() {
    local name="$1" expected="$2" url="$3" override="$4"
    local path="$tools_dir/$name"
    # Per-process temp name: two builds sharing the tools cache must not write
    # the same partial file.
    local partial="$path.part.$$"
    if [ -f "$path" ]; then
        if check_hash "$path" "$expected" "$name" "$override"; then
            [ -x "$path" ] || chmod +x "$path"
            return 0
        fi
        echo "Discarding the stale cached copy of $name" >&2
        rm -f "$path"
    fi
    echo "Downloading $name"
    rm -f "$partial"
    if ! curl -L --fail --max-time 300 -o "$partial" "$url"; then
        rm -f "$partial"
        return 1
    fi
    if ! check_hash "$partial" "$expected" "$name" "$override"; then
        rm -f "$partial"
        return 1
    fi
    chmod +x "$partial"
    mv -f "$partial" "$path"
}

ensure_tool linuxdeploy-x86_64.AppImage "$LINUXDEPLOY_SHA256" \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
    LINUXDEPLOY_SHA256
ensure_tool linuxdeploy-plugin-qt-x86_64.AppImage "$LINUXDEPLOY_QT_SHA256" \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage \
    LINUXDEPLOY_QT_SHA256
ensure_tool appimagetool-x86_64.AppImage "$APPIMAGETOOL_SHA256" \
    https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage \
    APPIMAGETOOL_SHA256

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
# Re-copy the JPEG plugin from the host Qt installation: the copy linuxdeploy
# leaves behind is not byte-identical and crashed at startup on Fedora 44.
if [ -f "$qt_plugins/imageformats/libqjpeg.so" ]; then
    cp -a "$qt_plugins/imageformats/libqjpeg.so" \
        "$appdir/usr/plugins/imageformats/libqjpeg.so"
fi
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

# Replace the symlinked AppRun with a launcher that also registers a desktop
# entry the first time the AppImage runs. Wayland compositors take the window
# icon and the launcher name from that entry, not from the binary, so without
# it KDE and GNOME show a generic icon in the title bar.
rm -f "$appdir/AppRun"
cat > "$appdir/AppRun" <<'LAUNCHER'
#!/bin/sh
here="$(dirname "$(readlink -f "$0")")"

# A desktop entry value is a string, and the key file runs its own escape pass
# before the quoting pass: a backslash is an escape character in the file as
# well, and an escape the file does not know (\") makes the whole entry
# unreadable. The quoted argument therefore gets one backslash in front of
# \ " ` and $, and the file doubles every one of them again, so a literal
# backslash in the path is written as four. % starts a field code and a literal
# one is doubled. Nothing else is special, and the line is written directly
# instead of going through a sed replacement, so & and | in the path stay
# literal too.
desktop_exec_value() {
    printf '%s' "$1" \
        | sed -e 's/[\\"`$]/\\&/g' -e 's/\\/\\\\/g' -e 's/%/%%/g'
}

# Known limitation: a '%' in the AppImage path cannot be represented in a
# desktop Exec line that every reader accepts (GLib rejects %% in a quoted
# argument), so such a path keeps the generic icon.
#
# Wayland compositors take the window icon and the launcher entry from a
# desktop file the session can see, not from the binary. Register one on the
# first run and refresh the caches so the icon appears without a re-login.
configure_desktop() {
    [ -n "${APPIMAGE:-}" ] || return 0
    data="${XDG_DATA_HOME:-$HOME/.local/share}"
    mkdir -p "$data/applications" "$data/icons/hicolor" || return 0
    cp -R "$here/usr/share/icons/hicolor/." "$data/icons/hicolor/" 2>/dev/null || true
    if [ -f "$here/usr/share/applications/photoprint.desktop" ] \
        && [ ! -f "$data/applications/photoprint.desktop" ] \
        && [ ! -f /usr/share/applications/photoprint.desktop ]; then
        # Written to a private temporary file and renamed into place: a
        # truncated entry left behind by a failed write would look like a
        # finished registration, and the guard above would never regenerate it.
        # The name carries the pid so two first runs cannot share one file, and
        # nothing is renamed unless every line made it into the file: the exit
        # status of the loop below is only that of its last command.
        partial="$data/applications/photoprint.desktop.part.$$"
        failed=0
        {
            while IFS= read -r line || [ -n "$line" ]; do
                case "$line" in
                Exec=*)
                    exec_value="$(desktop_exec_value "$APPIMAGE")" \
                        || { failed=1; break; }
                    printf 'Exec="%s" %%F\n' "$exec_value" \
                        || { failed=1; break; }
                    ;;
                *)
                    printf '%s\n' "$line" || { failed=1; break; }
                    ;;
                esac
            done < "$here/usr/share/applications/photoprint.desktop"
        } > "$partial" || failed=1
        if [ "$failed" -eq 0 ] \
            && mv -f "$partial" "$data/applications/photoprint.desktop"; then
            command -v update-desktop-database >/dev/null 2>&1 \
                && update-desktop-database -q "$data/applications" 2>/dev/null
            command -v kbuildsycoca6 >/dev/null 2>&1 \
                && kbuildsycoca6 --noincremental >/dev/null 2>&1 &
        else
            rm -f "$partial"
        fi
    fi
    command -v gtk-update-icon-cache >/dev/null 2>&1 \
        && gtk-update-icon-cache -f -t "$data/icons/hicolor" >/dev/null 2>&1
    return 0
}
configure_desktop
exec "$here/usr/bin/photoprint" "$@"
LAUNCHER
chmod +x "$appdir/AppRun"

# Ship fixed-size PNGs as well as the SVG: some compositors only look at the
# theme's PNG entries for the window icon.
icons_dir="$appdir/usr/share/icons/hicolor"
for size in 16 22 24 32 48 64 128 256 512; do
    mkdir -p "$icons_dir/${size}x${size}/apps"
    magick "$root/resources/photoprint.svg" -background none -resize "${size}x${size}" \
        "$icons_dir/${size}x${size}/apps/photoprint.png"
done

# AppImage root icon and .DirIcon.
magick "$root/resources/photoprint.svg" -background none -resize 256x256 \
    "$appdir/photoprint.png"
ln -sf photoprint.png "$appdir/.DirIcon"

"$tools_dir/appimagetool-x86_64.AppImage" "$appdir" \
    "$out_dir/PhotoPrint-$version-x86_64.AppImage"

echo "Built $out_dir/PhotoPrint-$version-x86_64.AppImage"
