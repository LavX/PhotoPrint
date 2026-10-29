# PhotoPrint

A lightweight, local desktop application for arranging family photos on paper,
cropping them, and printing them. It is built for people who do not want to
learn image processing or printer technology: add photos, choose the paper,
place them on the sheet, and print.

Built with C++20 and Qt 6 Widgets. Colour conversion uses LittleCMS2, printing
uses the existing CUPS queues, and photo filters/upscaling use ImageMagick's
MagickWand.

## Features

- Import JPEG and PNG at original resolution, with EXIF orientation and embedded
  ICC profiles (Display P3 included).
- A white paper canvas with direct move, resize, rotate, fit/fill and a separate
  crop mode; keyboard nudging and undo/redo.
- Multiple photos and pages, layer order, page copy/delete/reorder.
- Photo-library sidebar, canvas and context-sensitive inspector.
- Automatic, conservative tone correction plus a manual brightness slider and
  eight filter presets with strength control.
- 2x Lanczos enlargement for print (with a light sharpen; it is not an AI model).
- Paper sizes in centimetres: A4, A5, A6, 9 × 13, 10 × 15 and 13 × 18 cm, plus
  saved custom paper profiles and glossy/matte/plain types.
- Print quality (highest/normal), colour or grayscale, copies and page count.
- The chosen printer, paper, orientation and print settings are remembered.
- Versioned `.ppj` projects, portable copies that carry the originals, atomic
  saves and two-slot crash recovery.
- One print action = one snapshot, one PDF, one CUPS submission; no automatic
  reprint after an uncertain response.
- Follows the system light/dark colour scheme.

## Requirements

- Linux desktop (Fedora 44 is the tested environment).
- CMake 3.21 or newer and a C++20 compiler.
- Qt 6.5 or newer: Core, Gui, Widgets, Concurrent, Test.
- LittleCMS2, CUPS, zlib and ImageMagick 7 development files
  (`MagickWand`).
- Poppler command-line tools (`pdfinfo`, `pdftoppm`, `pdfimages`) for the
  PDF evidence tests.

Fedora development packages used here:

```bash
sudo dnf install cmake ninja-build gcc-c++ qt6-qtbase-devel qt6-linguist \
  lcms2-devel cups-devel zlib-devel ImageMagick-devel poppler-utils
```

Package names differ on other distributions; the same libraries are required.

## Build and run

```bash
cmake --preset dev
cmake --build --preset dev
./build/dev/photoprint
```

Release build:

```bash
cmake --preset release
cmake --build --preset release
./build/release/photoprint
```

## Tests

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir build/dev --output-on-failure
```

The suite covers colour conversion, geometry, PDF output, alpha composition,
project recovery, filters, tone correction, print submission and the daily UI
flow. A few acceptance tests read the local sample photo at
`/home/lavx/Downloads/IMG20260830150231.jpg`; on another machine, adjust that
path or skip those tests. Tests never print.

## Portable AppImage

```bash
./packaging/build-appimage.sh
# build/release/PhotoPrint-<version>-x86_64.AppImage
```

The script downloads the standard AppImage tooling once into
`build/appimage-tools`, bundles Qt, LittleCMS2, CUPS and ImageMagick, and adds
the XCB, Wayland and offscreen platform plugins. The resulting AppImage runs on
Wayland and X11 desktops without installing PhotoPrint. If FUSE is unavailable,
run it with `--appimage-extract-and-run`.

## Fedora package

```bash
rpmbuild -bb packaging/photoprint.spec \
  --define "_topdir $PWD/build/rpm" \
  --define "photoprint_source $PWD"
```

The spec builds from the local source tree without network access. It is
Fedora-specific; other distributions can build with CMake and package the
result themselves.

## Platform notes

- Fedora 44 with KDE Plasma/Wayland is the environment this was developed and
  measured on, but the application does **not** link to KDE libraries. It uses
  Qt Widgets only, so GNOME, XFCE and other Linux desktops work the same way.
- It runs on Wayland and X11. The window chrome follows the system theme; the
  application itself provides both light and dark palettes.
- macOS and Windows are not tested. The CUPS printer discovery and the RPM
  packaging are Linux-specific.
- The current interface is Hungarian; a translation layer is being added for
  ten languages.

## Usage

The photo library is on the left, the paper is in the centre, and the inspector
for the selected photo is on the right. Printer, paper and orientation are in
the top bar. The bottom bar holds page handling, print quality, colour mode,
copies and the Print button. PDF export is available from the File menu.

A project file (`.ppj`) stores the pages, paper, geometry, crop, tone, filters
and source identities. Saving is atomic; the recovery snapshots are separate
from the print path.

## Documentation

- `docs/development/progress.md` – per-slice status, commands and evidence.
- `docs/development/acceptance-results.md` – offline, filter, measurement,
  package and physical-print status, kept separate.
- `docs/development/filters.md` – filter and upscaling design.
- `planning/` – the original planning documents, preserved.

## Limitations

Physical print acceptance on the Brother DCP-T730DW with SWAN 100 × 150 mm
paper is still open, as are the Wayland performance measurement and the
20-source memory target. Matte paper has no distinct driver media type and is
sent as inkjet paper. Multi-select and cross-page dragging are not implemented.
See `docs/development/acceptance-results.md` for the exact status.

## License

MIT. See `LICENSE`.
