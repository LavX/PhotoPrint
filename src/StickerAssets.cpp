#include "StickerAssets.h"

#include "OverlayGeometry.h"

#include <QFileInfo>
#include <QImageReader>

namespace pp {

namespace {

constexpr char kResourcePrefix[] = ":/stickers/";

} // namespace

bool isBuiltinStickerPath(const QString &path)
{
    return path.startsWith(QLatin1String(kBuiltinStickerPrefix));
}

QString builtinStickerPath(const QString &alias)
{
    if (alias.isEmpty())
        return {};
    return QStringLiteral("%1%2@%3")
        .arg(QLatin1String(kBuiltinStickerPrefix), alias)
        .arg(kBuiltinStickerVersion);
}

QString builtinStickerResource(const QString &path)
{
    if (!isBuiltinStickerPath(path))
        return {};
    const QString rest = path.mid(int(qstrlen(kBuiltinStickerPrefix)));
    const int at = rest.lastIndexOf(QLatin1Char('@'));
    if (at <= 0)
        return {};
    const QString alias = rest.left(at);
    bool ok = false;
    const int version = rest.mid(at + 1).toInt(&ok);
    if (!ok || version < 1)
        return {};
    // The shipped file is the only authority on whether a version is available.
    // The resolver must not compare the version with this build's own: a build
    // at v2 has to answer a project saved at v1 with the v1 file it still
    // ships, or every older project would lose its stickers on the upgrade.
    const QString resource = QStringLiteral("%1%2-v%3.png")
                                 .arg(QLatin1String(kResourcePrefix), alias)
                                 .arg(version);
    if (!QFileInfo::exists(resource))
        return {};
    return resource;
}

QString stickerSourcePath(const Sticker &sticker)
{
    if (isBuiltinStickerPath(sticker.imagePath))
        return builtinStickerResource(sticker.imagePath);
    return sticker.imagePath;
}

QString stickerPickerDirectory(const Sticker &sticker, const QString &lastDir)
{
    if (!isBuiltinStickerPath(sticker.imagePath)) {
        const QString source = stickerSourcePath(sticker);
        if (!source.isEmpty() && !source.startsWith(QLatin1Char(':'))) {
            const QString directory = QFileInfo(source).absolutePath();
            if (!directory.isEmpty())
                return directory;
        }
    }
    // A shipped sticker lives in the resource tree, and a missing one may have
    // no path at all: the last real folder the user picked from is the only
    // sensible start.
    if (!lastDir.isEmpty() && !lastDir.startsWith(QLatin1Char(':')))
        return lastDir;
    return {};
}

StickerLayer loadStickerLayer(ImageStore &store, const Sticker &sticker,
                              const ColorRecipe &recipe, int maxDimension,
                              std::stop_token token)
{
    StickerLayer layer;
    const QString source = stickerSourcePath(sticker);
    if (source.isEmpty()) {
        layer.missing = true;
        return layer;
    }
    // A source that cannot be read, or that is too large to decode within Qt's
    // allocation limit, is skipped before the store tries: a 12000 x 12000
    // picture is 144 MP and would fail the decode anyway.
    QImageReader probe(source);
    probe.setDecideFormatFromContent(true);
    if (!probe.canRead() || OverlayGeometry::stickerSourceTooLarge(probe.size())) {
        layer.missing = true;
        return layer;
    }
    Result<SourceImage> loaded = store.load(source, ImportPolicy{}, token);
    if (!loaded) {
        layer.missing = true;
        return layer;
    }
    // A sticker is not a photograph: it has no per-frame tone and no filter. It
    // does share the document's paper component, so it matches the photographs
    // on the same sheet, and it goes through the same linear working space.
    ColorRecipe base = sanitiseRecipe(recipe);
    base.tone = PhotoTone{};
    Result<QImage> converted = store.linear(loaded.value(), base, maxDimension, token);
    if (!converted) {
        layer.missing = true;
        return layer;
    }
    layer.image = converted.value();
    return layer;
}

} // namespace pp
