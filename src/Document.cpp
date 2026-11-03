#include "Document.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QSet>

namespace pp {

namespace {

void writeDouble(QDataStream &stream, double value)
{
    // Fixed textual precision keeps the digest stable across platforms while
    // still covering the 0.01 mm model precision.
    QString text = QString::number(value, 'f', 6);
    // A negative value that rounds to zero formats as "-0.000000", which is the
    // same number as "0.000000". Normalise the text, not the double: the double
    // check alone misses values such as -1e-9.
    if (text.startsWith(QLatin1Char('-'))) {
        QString digits = text.mid(1);
        digits.remove(QLatin1Char('.'));
        digits.remove(QLatin1Char('0'));
        if (digits.isEmpty())
            text.remove(0, 1);
    }
    stream << text;
}

} // namespace

QByteArray ColorRecipe::digest() const
{
    QByteArray raw;
    QDataStream stream(&raw, QIODevice::WriteOnly);
    // Pinned so a Qt upgrade cannot silently change the stream layout and
    // therefore every cache key and recipe digest. Qt_6_0 is the version the
    // unpinned stream used in 1.0.0, so existing digests stay unchanged.
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << qint32(version);
    stream << qint8(autoEnhance ? 1 : 0);
    writeDouble(stream, tone.exposureEv);
    writeDouble(stream, tone.shadowLift);
    writeDouble(stream, tone.highlightCompress);
    writeDouble(stream, paperStrength);
    stream << qint8(grayscale ? 1 : 0);
    return QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
}

const Frame *findFrame(const DocumentSnapshot &snapshot, const QString &frameId, int *pageIndex)
{
    for (int i = 0; i < snapshot.pages.size(); ++i) {
        const Page &page = snapshot.pages.at(i);
        for (const Frame &frame : page.frames) {
            if (frame.id == frameId) {
                if (pageIndex)
                    *pageIndex = i;
                return &frame;
            }
        }
    }
    return nullptr;
}

Frame *findFrame(DocumentSnapshot &snapshot, const QString &frameId, int *pageIndex)
{
    for (int i = 0; i < snapshot.pages.size(); ++i) {
        Page &page = snapshot.pages[i];
        for (Frame &frame : page.frames) {
            if (frame.id == frameId) {
                if (pageIndex)
                    *pageIndex = i;
                return &frame;
            }
        }
    }
    return nullptr;
}

const Page *findPage(const DocumentSnapshot &snapshot, const QString &pageId, int *pageIndex)
{
    for (int i = 0; i < snapshot.pages.size(); ++i) {
        if (snapshot.pages.at(i).id == pageId) {
            if (pageIndex)
                *pageIndex = i;
            return &snapshot.pages.at(i);
        }
    }
    return nullptr;
}

int pageIndexOf(const DocumentSnapshot &snapshot, const QString &pageId)
{
    int index = -1;
    findPage(snapshot, pageId, &index);
    return index;
}

PhotoTone sanitiseTone(const PhotoTone &tone)
{
    PhotoTone sane;
    sane.exposureEv = std::isfinite(tone.exposureEv)
        ? std::clamp(tone.exposureEv, kMinToneExposureEv, kMaxToneExposureEv)
        : 0.0;
    sane.shadowLift = std::isfinite(tone.shadowLift)
        ? std::clamp(tone.shadowLift, 0.0, kMaxToneShadowLift)
        : 0.0;
    sane.highlightCompress = std::isfinite(tone.highlightCompress)
        ? std::clamp(tone.highlightCompress, 0.0, kMaxToneHighlightCompress)
        : 0.0;
    return sane;
}

PhotoTone effectiveTone(const Frame &frame)
{
    PhotoTone tone = frame.autoTone ? frame.tone : PhotoTone{};
    // A project file can name any number, so the frame's own tone is sanitised
    // here, at the boundary where it becomes a recipe: an unclamped exposure
    // would drive the frame to solid black or white in the renderer.
    tone = sanitiseTone(tone);
    // The manual offset is clamped on its own, then added: a project file can
    // name any brightness too, and the sum still has to stay in the range the
    // renderer was promised.
    tone.exposureEv += clampBrightnessEv(frame.brightness);
    tone.exposureEv = std::clamp(tone.exposureEv, kMinToneExposureEv, kMaxToneExposureEv);
    return tone;
}

QSizeF pageSizeMm(const Page &page)
{
    return page.landscape ? QSizeF(page.paper.physicalHeightMm, page.paper.physicalWidthMm)
                          : QSizeF(page.paper.physicalWidthMm, page.paper.physicalHeightMm);
}

QString cutGuidesKey(CutGuides value)
{
    switch (value) {
    case CutGuides::Screen:
        return QStringLiteral("screen");
    case CutGuides::Print:
        return QStringLiteral("print");
    case CutGuides::None:
        break;
    }
    return QStringLiteral("none");
}

CutGuides cutGuidesFromKey(const QString &key)
{
    // A strict whitelist: any other spelling, including an empty missing key,
    // is the safe default rather than a guess at what the author meant.
    if (key == QLatin1String("screen"))
        return CutGuides::Screen;
    if (key == QLatin1String("print"))
        return CutGuides::Print;
    return CutGuides::None;
}

QString borderlessOverrideKey(BorderlessOverride value)
{
    switch (value) {
    case BorderlessOverride::Off:
        return QStringLiteral("off");
    case BorderlessOverride::On:
        return QStringLiteral("on");
    case BorderlessOverride::Auto:
        break;
    }
    return QStringLiteral("auto");
}

PageBackground sanitiseBackground(PageBackground background)
{
    // An unknown enum value can only arrive from a caller that cast an integer;
    // the loader maps strings through a whitelist. Normalising here keeps the
    // renderer's switch exhaustive in practice.
    switch (background.kind) {
    case BackgroundKind::None:
    case BackgroundKind::Solid:
    case BackgroundKind::Gradient:
    case BackgroundKind::Image:
        break;
    default:
        background.kind = BackgroundKind::None;
        break;
    }
    switch (background.fit) {
    case BackgroundFit::Fill:
    case BackgroundFit::Fit:
    case BackgroundFit::Tile:
        break;
    default:
        background.fit = BackgroundFit::Fill;
        break;
    }

    // The model has no per-colour alpha: transparency is the layer's opacity.
    // Forcing the alpha byte here is what keeps a colour picked with an alpha
    // channel from compositing darker than the swatch shows.
    background.solid = 0xff000000u | (background.solid & 0x00ffffffu);

    QList<BackgroundStop> stops;
    stops.reserve(std::min<int>(background.stops.size(), kMaxGradientStops));
    for (const BackgroundStop &stored : background.stops) {
        if (!std::isfinite(stored.first))
            continue; // a broken position is corruption, not a direction
        stops.append({std::clamp(stored.first, 0.0, 1.0),
                      0xff000000u | (stored.second & 0x00ffffffu)});
    }
    std::stable_sort(stops.begin(), stops.end(),
                     [](const BackgroundStop &a, const BackgroundStop &b) {
                         return a.first < b.first;
                     });
    if (stops.size() > kMaxGradientStops) {
        // Keep both endpoints and sample the middle evenly. Dropping the tail
        // would change the gradient's end colour and dropping the head would
        // change its start; a hand-edited 12-stop file still renders its first
        // and last colour at the right places.
        QList<BackgroundStop> sampled;
        sampled.reserve(kMaxGradientStops);
        const int last = int(stops.size()) - 1;
        for (int i = 0; i < kMaxGradientStops; ++i) {
            const int index = int(std::lround(
                double(i) * double(last) / double(kMaxGradientStops - 1)));
            sampled.append(stops.at(std::clamp(index, 0, last)));
        }
        stops = sampled;
    }
    background.stops = stops;

    // A non-finite angle is corruption, so it becomes the neutral direction
    // rather than saturating to a diagonal nobody asked for. A finite one is
    // wrapped into [0, 360), which is the range the panel's combo can show.
    if (std::isfinite(background.angleDeg)) {
        background.angleDeg = std::fmod(background.angleDeg, 360.0);
        if (background.angleDeg < 0.0)
            background.angleDeg += 360.0;
    } else {
        background.angleDeg = 0.0;
    }

    background.tileMm = std::isfinite(background.tileMm)
        ? std::clamp(background.tileMm, 0.0, kMaxTileMm)
        : 0.0;
    background.opacity = std::isfinite(background.opacity)
        ? std::clamp(background.opacity, 0.0, 100.0)
        : 100.0;
    return background;
}

Sticker sanitiseSticker(Sticker sticker, const QSizeF &sheetMm)
{
    const bool validSheet = sheetMm.width() > 0.0 && sheetMm.height() > 0.0
        && std::isfinite(sheetMm.width()) && std::isfinite(sheetMm.height());
    const double sheetWidth = validSheet ? sheetMm.width() : kMaxStickerMm;
    const double sheetHeight = validSheet ? sheetMm.height() : kMaxStickerMm;

    // A size that cannot be drawn is raised to the minimum instead of becoming
    // zero: a zero-size sticker could never be seen or clicked again, so the
    // page would carry an invisible layer only undo could remove. A finite
    // value is bounded as one uniform scale, so a tall picture is made smaller
    // rather than squashed on one axis.
    const QSizeF size = boundedStickerSizeMm(
        QSizeF(std::isfinite(sticker.width) ? sticker.width : kMinStickerMm,
               std::isfinite(sticker.height) ? sticker.height : kMinStickerMm),
        QSizeF(sheetWidth, sheetHeight));
    sticker.width = size.width();
    sticker.height = size.height();

    // Reachability invariant: a sticker may hang over the edge (a legitimate
    // bleed look), but its centre always stays on the paper, so it can be
    // clicked, nudged and deleted. Without this a layer dragged off the sheet
    // would be unreachable and only undo could bring it back.
    sticker.cx = std::isfinite(sticker.cx)
        ? std::clamp(sticker.cx, 0.0, sheetWidth)
        : sheetWidth / 2.0;
    sticker.cy = std::isfinite(sticker.cy)
        ? std::clamp(sticker.cy, 0.0, sheetHeight)
        : sheetHeight / 2.0;

    // The renderer, the handles and the panel all read the same normalised
    // angle, so a hand-edited 720 becomes 0 and a NaN becomes no rotation.
    sticker.angleDeg = normalizedImageRotationDeg(sticker.angleDeg);
    sticker.opacity = std::clamp(sticker.opacity, 0, 100);
    sticker.z = std::clamp(sticker.z, kMinStickerZ, kMaxStickerZ);
    return sticker;
}

QSizeF boundedStickerSizeMm(QSizeF size, const QSizeF &sheetMm)
{
    const bool validSheet = sheetMm.width() > 0.0 && sheetMm.height() > 0.0
        && std::isfinite(sheetMm.width()) && std::isfinite(sheetMm.height());
    const double diagonal = validSheet ? std::hypot(sheetMm.width(), sheetMm.height())
                                       : kMaxStickerMm;
    const double maxSize = std::min(kMaxStickerMm, std::max(kMinStickerMm, diagonal));

    // A zero, negative or non-finite axis has no aspect to keep. It becomes the
    // minimum and the other axis is bounded from there, so a corrupt size still
    // draws something clickable instead of a degenerate sliver.
    double width = std::isfinite(size.width()) && size.width() > 0.0 ? size.width()
                                                                     : kMinStickerMm;
    double height = std::isfinite(size.height()) && size.height() > 0.0 ? size.height()
                                                                        : kMinStickerMm;

    const double longest = std::max(width, height);
    if (longest > maxSize) {
        const double factor = maxSize / longest;
        width *= factor;
        height *= factor;
    }
    const double shortest = std::min(width, height);
    if (shortest < kMinStickerMm) {
        const double factor = kMinStickerMm / shortest;
        if (std::max(width, height) * factor <= maxSize) {
            width *= factor;
            height *= factor;
        } else {
            // The two bounds cannot both hold (a very long, very thin picture).
            // The page bound wins, because a layer larger than the sheet is the
            // one that cannot be handled; the thin side stays clickable along
            // its length.
            const double fit = maxSize / std::max(width, height);
            width *= fit;
            height *= fit;
        }
    }
    return QSizeF(width, height);
}

QList<Sticker> sanitiseStickers(const QList<Sticker> &stickers, const QSizeF &sheetMm)
{
    QList<Sticker> kept;
    QSet<QString> ids;
    kept.reserve(std::min<int>(stickers.size(), kMaxStickersPerPage));
    for (const Sticker &stored : stickers) {
        if (kept.size() >= kMaxStickersPerPage)
            break;
        // An empty or repeated id cannot be referenced, selected or undone
        // through, so it is dropped rather than given a fresh one: a repair
        // point that invents identity would make a hand-edited file change
        // every time it is loaded.
        if (stored.id.isEmpty() || ids.contains(stored.id))
            continue;
        ids.insert(stored.id);
        kept.append(sanitiseSticker(stored, sheetMm));
    }
    return kept;
}

BorderlessOverride borderlessOverrideFromKey(const QString &key)
{
    if (key == QLatin1String("off"))
        return BorderlessOverride::Off;
    if (key == QLatin1String("on"))
        return BorderlessOverride::On;
    return BorderlessOverride::Auto;
}

} // namespace pp
