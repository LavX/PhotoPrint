#include "OverlayGeometry.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace pp {
namespace OverlayGeometry {

namespace {

/// Rotates a page-space point back into an overlay's own frame.
QPointF toLocal(double cx, double cy, double angleDeg, const QPointF &pagePoint)
{
    const double radians = qDegreesToRadians(-angleDeg);
    const double dx = pagePoint.x() - cx;
    const double dy = pagePoint.y() - cy;
    return QPointF(dx * std::cos(radians) - dy * std::sin(radians),
                   dx * std::sin(radians) + dy * std::cos(radians));
}

/// Rotates a local half-extent box into the page quad.
QPolygonF rotatedQuad(double cx, double cy, double halfWidth, double halfHeight,
                      double angleDeg)
{
    const double radians = qDegreesToRadians(angleDeg);
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const auto corner = [&](double x, double y) {
        return QPointF(cx + x * cosine - y * sine, cy + x * sine + y * cosine);
    };
    return QPolygonF{corner(-halfWidth, -halfHeight), corner(halfWidth, -halfHeight),
                     corner(halfWidth, halfHeight), corner(-halfWidth, halfHeight)};
}

int overlayZ(const QList<Sticker> &stickers, const QList<TextItem> &texts,
             const OverlayRef &ref)
{
    if (ref.kind == OverlayKind::Sticker)
        return stickers.at(ref.index).z;
    return texts.at(ref.index).z;
}

} // namespace

bool stickerSourceTooLarge(const QSize &pixels)
{
    if (pixels.width() <= 0 || pixels.height() <= 0)
        return false;
    return qint64(pixels.width()) * qint64(pixels.height()) > kStickerMaxSourcePixels;
}

QRectF stickerBoxMm(const Sticker &sticker)
{
    return QRectF(sticker.cx - sticker.width / 2.0, sticker.cy - sticker.height / 2.0,
                  sticker.width, sticker.height);
}

QPolygonF stickerQuadMm(const Sticker &sticker)
{
    return rotatedQuad(sticker.cx, sticker.cy, sticker.width / 2.0, sticker.height / 2.0,
                       sticker.angleDeg);
}

bool stickerContains(const Sticker &sticker, const QPointF &pageMm)
{
    if (!(sticker.width > 0.0) || !(sticker.height > 0.0))
        return false;
    const QPointF local = toLocal(sticker.cx, sticker.cy, sticker.angleDeg, pageMm);
    return std::abs(local.x()) <= sticker.width / 2.0
        && std::abs(local.y()) <= sticker.height / 2.0;
}

QList<Sticker> stickersInPaintOrder(const Page &page)
{
    return stickersInPaintOrder(page.stickers);
}

QList<Sticker> stickersInPaintOrder(const QList<Sticker> &stickers)
{
    QList<Sticker> ordered = stickers;
    // Stable by design: two stickers with the same z keep document order, so
    // the visible order is deterministic for a hand-edited or older file.
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Sticker &a, const Sticker &b) { return a.z < b.z; });
    return ordered;
}


QSizeF defaultStickerSizeMm(const QSizeF &sheetMm, const QSize &pixels)
{
    const double shortSide = std::min(sheetMm.width(), sheetMm.height());
    double width = shortSide * 0.25;
    if (!(width > 0.0) || !std::isfinite(width))
        width = 30.0;
    width = std::clamp(width, kMinStickerMm, 300.0);
    double aspect = 1.0;
    if (pixels.width() > 0 && pixels.height() > 0)
        aspect = double(pixels.height()) / double(pixels.width());
    if (!(aspect > 0.0) || !std::isfinite(aspect))
        aspect = 1.0;
    // A tall picture is made smaller as a whole, not squashed on one axis: the
    // default box keeps the pixel aspect and still fits the sheet.
    return boundedStickerSizeMm(QSizeF(width, width * aspect), sheetMm);
}

QPointF cascadedStickerCentreMm(const Page &page, const QSizeF &sheetMm,
                                const QSizeF &boxMm)
{
    const QPointF centre(sheetMm.width() / 2.0, sheetMm.height() / 2.0);
    if (!(sheetMm.width() > 0.0) || !(sheetMm.height() > 0.0))
        return centre;
    // 7 mm is about one comfortable nudge: big enough to be obvious on screen
    // and on paper, small enough that a few stickers still fit around the
    // middle of the sheet.
    constexpr double step = 7.0;
    const double halfWidth = std::max(0.0, boxMm.width() / 2.0);
    const double halfHeight = std::max(0.0, boxMm.height() / 2.0);
    const double margin = 1.0;
    const int columns = std::max(
        1, int(std::floor((sheetMm.width() / 2.0 - halfWidth - margin) / step)));
    const int rows = std::max(
        1, int(std::floor((sheetMm.height() / 2.0 - halfHeight - margin) / step)));
    // The first free slot on a cascade that runs right and then down, so the
    // second click on the same shape is visibly a second object instead of an
    // identical one directly above the first.
    for (int slot = 0; slot < columns * rows; ++slot) {
        const QPointF candidate(centre.x() + (slot % columns) * step,
                                centre.y() + ((slot / columns) % rows) * step);
        bool taken = false;
        for (const Sticker &existing : page.stickers) {
            if (std::abs(existing.cx - candidate.x()) < step / 2.0
                && std::abs(existing.cy - candidate.y()) < step / 2.0) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return candidate;
    }
    // Every slot is taken: place it on the centre anyway. The sanitiser keeps
    // it on the sheet, and the panel still selects it.
    return centre;
}

Sticker movedSticker(const Sticker &sticker, const QPointF &delta, const QSizeF &sheetMm)
{
    Sticker moved = sticker;
    if (std::isfinite(delta.x()))
        moved.cx = sticker.cx + delta.x();
    if (std::isfinite(delta.y()))
        moved.cy = sticker.cy + delta.y();
    if (sheetMm.width() > 0.0 && sheetMm.height() > 0.0) {
        moved.cx = std::clamp(moved.cx, 0.0, sheetMm.width());
        moved.cy = std::clamp(moved.cy, 0.0, sheetMm.height());
    }
    return moved;
}

Sticker resizedSticker(const Sticker &sticker, const QPointF &pagePoint,
                       const QSizeF &sheetMm)
{
    Sticker resized = sticker;
    const QPointF local = toLocal(sticker.cx, sticker.cy, sticker.angleDeg, pagePoint);
    const double start = std::hypot(sticker.width / 2.0, sticker.height / 2.0);
    const double reach = std::hypot(local.x(), local.y());
    if (!(start > 0.0) || !(reach > 0.0) || !std::isfinite(reach))
        return resized;

    double factor = reach / start;
    if (!(factor > 0.0) || !std::isfinite(factor))
        return resized;
    // One uniform scale, bounded as a whole: the aspect cannot drift when a
    // corner is pushed past the minimum or the maximum.
    const QSizeF size = boundedStickerSizeMm(
        QSizeF(sticker.width * factor, sticker.height * factor), sheetMm);
    resized.width = size.width();
    resized.height = size.height();
    return resized;
}

Sticker rotatedSticker(const Sticker &sticker, const QPointF &pagePoint, bool snap)
{
    Sticker rotated = sticker;
    const double dx = pagePoint.x() - sticker.cx;
    const double dy = pagePoint.y() - sticker.cy;
    if (!std::isfinite(dx) || !std::isfinite(dy) || (dx == 0.0 && dy == 0.0))
        return rotated;
    // The handle sits above the top edge, so the pointer's direction plus 90
    // degrees is the rotation the user is asking for, exactly like a frame.
    double angle = qRadiansToDegrees(std::atan2(dy, dx)) + 90.0;
    if (snap)
        angle = std::round(angle / 15.0) * 15.0;
    rotated.angleDeg = normalizedImageRotationDeg(angle);
    return rotated;
}


// --- Text -----------------------------------------------------------------

QFont fontFor(const TextItem &item, double ppi)
{
    QFont font;
    // An empty family is the application font: Qt resolves it at paint time, so
    // a project written on one machine does not pin a family the next may lack.
    if (!item.fontFamily.isEmpty())
        font.setFamily(item.fontFamily);
    const double pixels = item.fontPt * ppi / 72.0;
    font.setPixelSize(std::max(1, int(std::lround(std::isfinite(pixels) ? pixels : 1.0))));
    font.setBold(item.bold);
    font.setItalic(item.italic);
    font.setUnderline(item.underline);
    return font;
}

QFontMetricsF metricsFor(const TextItem &item, double ppi)
{
    return QFontMetricsF(fontFor(item, ppi));
}

QStringList lines(const QString &text)
{
    if (text.isEmpty())
        return {};
    return text.split(QLatin1Char('\n'));
}

QRectF textBlockMm(const TextItem &item)
{
    const QStringList rows = lines(item.text);
    if (rows.isEmpty())
        return {};
    const QFontMetricsF metrics = metricsFor(item, kTextLayoutPpi);
    double width = 0.0;
    for (const QString &row : rows)
        width = std::max(width, metrics.horizontalAdvance(row));
    const double advance = metrics.height() * item.lineSpacing;
    const double blockHeight = (rows.size() - 1) * advance + metrics.height();
    const double pxToMm = 25.4 / kTextLayoutPpi;
    width *= pxToMm;
    const double height = blockHeight * pxToMm;
    if (!std::isfinite(width) || !std::isfinite(height))
        return {};
    return QRectF(item.cx - width / 2.0, item.cy - height / 2.0, width, height);
}

QPolygonF textQuadMm(const TextItem &item)
{
    const QRectF block = textBlockMm(item);
    if (block.isEmpty())
        return {};
    return rotatedQuad(item.cx, item.cy, block.width() / 2.0, block.height() / 2.0,
                       item.angleDeg);
}

QPolygonF textHandleQuadMm(const TextItem &item, double marginMm)
{
    const QRectF block = textBlockMm(item);
    if (block.isEmpty())
        return {};
    const double margin = std::max(0.0, marginMm);
    return rotatedQuad(item.cx, item.cy, block.width() / 2.0 + margin,
                       block.height() / 2.0 + margin, item.angleDeg);
}

bool textContains(const TextItem &item, const QPointF &pageMm)
{
    const QRectF block = textBlockMm(item);
    if (block.isEmpty())
        return false;
    const QPointF local = toLocal(item.cx, item.cy, item.angleDeg, pageMm);
    return std::abs(local.x()) <= block.width() / 2.0
        && std::abs(local.y()) <= block.height() / 2.0;
}

double minimumFontPt(const TextItem &item)
{
    // The block is not exactly linear in the font size: Qt rounds the pixel size
    // and hints the glyphs, so one scale pass can land a fraction under the
    // minimum. The loop measures the real block and grows until it fits, which
    // converges in two or three passes and is idempotent afterwards.
    TextItem probe = item;
    double fontPt = kMinFontPt;
    for (int pass = 0; pass < 8; ++pass) {
        probe.fontPt = fontPt;
        const QRectF block = textBlockMm(probe);
        if (block.isEmpty() || !(block.width() > 0.0) || !(block.height() > 0.0))
            return kMinFontPt;
        const double scale =
            std::max(kMinTextBlockMm / block.width(), kMinTextBlockMm / block.height());
        if (!(scale > 1.0) || !std::isfinite(scale))
            return fontPt;
        const double next = std::min(kMaxFontPt, fontPt * scale * 1.01);
        if (!(next > fontPt))
            return fontPt;
        fontPt = next;
        if (fontPt >= kMaxFontPt)
            return kMaxFontPt;
    }
    return fontPt;
}

TextItem movedText(const TextItem &item, const QPointF &delta, const QSizeF &sheetMm)
{
    TextItem moved = item;
    if (std::isfinite(delta.x()))
        moved.cx = item.cx + delta.x();
    if (std::isfinite(delta.y()))
        moved.cy = item.cy + delta.y();
    if (sheetMm.width() > 0.0 && sheetMm.height() > 0.0) {
        moved.cx = std::clamp(moved.cx, 0.0, sheetMm.width());
        moved.cy = std::clamp(moved.cy, 0.0, sheetMm.height());
    }
    return moved;
}

double textReachMm(const TextItem &item, const QPointF &pageMm)
{
    const QPointF local = toLocal(item.cx, item.cy, item.angleDeg, pageMm);
    return std::hypot(local.x(), local.y());
}

TextItem resizedTextFromGrab(const TextItem &item, const QPointF &pagePoint,
                             double grabRadiusMm)
{
    TextItem resized = item;
    // The handle sits outside the block, so the factor is measured from the
    // radius the pointer had when it grabbed the handle, not from the block
    // corner: otherwise the first pixel of the drag would already scale the
    // caption. The block is derived from the font, so scaling the font is the
    // whole resize.
    if (!(grabRadiusMm > 0.0) || !std::isfinite(grabRadiusMm))
        return resized;
    const double reach = textReachMm(item, pagePoint);
    if (!(reach > 0.0) || !std::isfinite(reach))
        return resized;
    const double factor = reach / grabRadiusMm;
    if (!(factor > 0.0) || !std::isfinite(factor))
        return resized;
    resized.fontPt = std::clamp(item.fontPt * factor, minimumFontPt(item), kMaxFontPt);
    return resized;
}

TextItem rotatedText(const TextItem &item, const QPointF &pagePoint, bool snap)
{
    TextItem rotated = item;
    const double dx = pagePoint.x() - item.cx;
    const double dy = pagePoint.y() - item.cy;
    if (!std::isfinite(dx) || !std::isfinite(dy) || (dx == 0.0 && dy == 0.0))
        return rotated;
    double angle = qRadiansToDegrees(std::atan2(dy, dx)) + 90.0;
    if (snap)
        angle = std::round(angle / 15.0) * 15.0;
    rotated.angleDeg = normalizedImageRotationDeg(angle);
    return rotated;
}

// --- The shared overlay stack ---------------------------------------------

QList<OverlayRef> paintOrder(const QList<Sticker> &stickers, const QList<TextItem> &texts)
{
    QList<OverlayRef> order;
    order.reserve(stickers.size() + texts.size());
    for (int i = 0; i < stickers.size(); ++i)
        order.append({OverlayKind::Sticker, i});
    for (int i = 0; i < texts.size(); ++i)
        order.append({OverlayKind::Text, i});
    // Stable by design: equal z keeps stickers before texts and document order
    // within each kind, so the visible order is deterministic for a hand-edited
    // or older file.
    std::stable_sort(order.begin(), order.end(),
                     [&stickers, &texts](const OverlayRef &a, const OverlayRef &b) {
                         const int za = overlayZ(stickers, texts, a);
                         const int zb = overlayZ(stickers, texts, b);
                         if (za != zb)
                             return za < zb;
                         if (a.kind != b.kind)
                             return a.kind == OverlayKind::Sticker;
                         return a.index < b.index;
                     });
    return order;
}

QList<OverlayRef> paintOrder(const Page &page)
{
    return paintOrder(page.stickers, page.texts);
}

std::optional<OverlayRef> overlayAt(const Page &page, const QPointF &pageMm)
{
    const QList<OverlayRef> order = paintOrder(page);
    for (int i = int(order.size()) - 1; i >= 0; --i) {
        const OverlayRef &ref = order.at(i);
        if (ref.kind == OverlayKind::Sticker) {
            if (stickerContains(page.stickers.at(ref.index), pageMm))
                return ref;
        } else if (textContains(page.texts.at(ref.index), pageMm)) {
            return ref;
        }
    }
    return std::nullopt;
}

QString overlayIdAt(const Page &page, const QPointF &pageMm)
{
    const std::optional<OverlayRef> ref = overlayAt(page, pageMm);
    if (!ref)
        return {};
    return ref->kind == OverlayKind::Sticker ? page.stickers.at(ref->index).id
                                             : page.texts.at(ref->index).id;
}

bool reorderOverlay(Page &page, const QString &overlayId, int delta)
{
    if (delta == 0 || overlayId.isEmpty())
        return false;
    QList<OverlayRef> order = paintOrder(page);
    int index = -1;
    for (int i = 0; i < order.size(); ++i) {
        const OverlayRef &ref = order.at(i);
        const QString id = ref.kind == OverlayKind::Sticker
            ? page.stickers.at(ref.index).id
            : page.texts.at(ref.index).id;
        if (id == overlayId) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return false;
    const int target = std::clamp(index + (delta > 0 ? 1 : -1), 0, int(order.size()) - 1);
    if (target == index)
        return false;
    order.move(index, target);
    // Renumbering across both kinds is what makes one press a real reorder: the
    // moved layer can never tie with its neighbour, and the merged order is
    // exactly the list order afterwards.
    for (int i = 0; i < order.size(); ++i) {
        const OverlayRef &ref = order.at(i);
        if (ref.kind == OverlayKind::Sticker)
            page.stickers[ref.index].z = i;
        else
            page.texts[ref.index].z = i;
    }
    return true;
}

} // namespace OverlayGeometry
} // namespace pp
