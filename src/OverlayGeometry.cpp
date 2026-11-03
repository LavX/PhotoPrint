#include "OverlayGeometry.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace pp {
namespace OverlayGeometry {

namespace {

/// Rotates a page-space point back into the sticker's own frame.
QPointF toLocal(const Sticker &sticker, const QPointF &pagePoint)
{
    const double radians = qDegreesToRadians(-sticker.angleDeg);
    const double dx = pagePoint.x() - sticker.cx;
    const double dy = pagePoint.y() - sticker.cy;
    return QPointF(dx * std::cos(radians) - dy * std::sin(radians),
                   dx * std::sin(radians) + dy * std::cos(radians));
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
    const double halfWidth = sticker.width / 2.0;
    const double halfHeight = sticker.height / 2.0;
    const double radians = qDegreesToRadians(sticker.angleDeg);
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const auto corner = [&](double x, double y) {
        return QPointF(sticker.cx + x * cosine - y * sine,
                       sticker.cy + x * sine + y * cosine);
    };
    return QPolygonF{corner(-halfWidth, -halfHeight), corner(halfWidth, -halfHeight),
                     corner(halfWidth, halfHeight), corner(-halfWidth, halfHeight)};
}

bool stickerContains(const Sticker &sticker, const QPointF &pageMm)
{
    if (!(sticker.width > 0.0) || !(sticker.height > 0.0))
        return false;
    const QPointF local = toLocal(sticker, pageMm);
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

const Sticker *stickerAt(const Page &page, const QPointF &pageMm)
{
    QList<int> order;
    order.reserve(page.stickers.size());
    for (int i = 0; i < page.stickers.size(); ++i)
        order.append(i);
    std::stable_sort(order.begin(), order.end(), [&page](int a, int b) {
        return page.stickers.at(a).z < page.stickers.at(b).z;
    });
    for (int i = int(order.size()) - 1; i >= 0; --i) {
        const Sticker &sticker = page.stickers.at(order.at(i));
        if (stickerContains(sticker, pageMm))
            return &sticker;
    }
    return nullptr;
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
    const QPointF local = toLocal(sticker, pagePoint);
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

bool reorderSticker(QList<Sticker> &paintOrder, const QString &stickerId, int delta)
{
    if (delta == 0)
        return false;
    int index = -1;
    for (int i = 0; i < paintOrder.size(); ++i) {
        if (paintOrder.at(i).id == stickerId) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return false;
    const int target = std::clamp(index + (delta > 0 ? 1 : -1), 0,
                                  int(paintOrder.size()) - 1);
    if (target == index)
        return false;
    paintOrder.move(index, target);
    for (int i = 0; i < paintOrder.size(); ++i)
        paintOrder[i].z = i;
    return true;
}

} // namespace OverlayGeometry
} // namespace pp
