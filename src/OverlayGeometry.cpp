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
    QList<Sticker> ordered = page.stickers;
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
    return QSizeF(width, width * aspect);
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
    const double diagonal = sheetMm.width() > 0.0 && sheetMm.height() > 0.0
        ? std::hypot(sheetMm.width(), sheetMm.height())
        : kMaxStickerMm;
    const double maxSize = std::min(kMaxStickerMm, std::max(kMinStickerMm, diagonal));
    // Clamp the factor rather than each axis, so the aspect cannot drift when a
    // corner is pushed past the minimum or the maximum.
    const double smallestSide = std::min(sticker.width, sticker.height);
    if (smallestSide > 0.0)
        factor = std::max(factor, kMinStickerMm / smallestSide);
    const double largestSide = std::max(sticker.width, sticker.height);
    if (largestSide > 0.0)
        factor = std::min(factor, maxSize / largestSide);
    resized.width = std::clamp(sticker.width * factor, kMinStickerMm, maxSize);
    resized.height = std::clamp(sticker.height * factor, kMinStickerMm, maxSize);
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
