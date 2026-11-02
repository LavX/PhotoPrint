#include "SlotGeometry.h"

#include <QSet>

#include <algorithm>
#include <cmath>

namespace pp {
namespace SlotGeometry {

namespace {

bool finite(double value)
{
    return std::isfinite(value);
}

bool finiteRect(const QRectF &rect)
{
    return finite(rect.x()) && finite(rect.y()) && finite(rect.width())
        && finite(rect.height());
}

bool positiveSize(const QSizeF &size)
{
    return finite(size.width()) && finite(size.height()) && size.width() > 0.0
        && size.height() > 0.0;
}

/// Returns the mapped cell unchanged when it is already usable, so the
/// catalogue's pinned arithmetic stays bit-identical, and rebuilds it otherwise:
/// inside the sheet, finite, at least 1 mm on both axes (or the whole sheet side
/// when that is smaller), centred on the mapped centre. This is what keeps a
/// huge margin or a gutter wider than the margin from producing a negative
/// rectangle for the border clamp to chew on.
QRectF clampedCellToSheet(const QRectF &cell, const QSizeF &sheet)
{
    if (!finiteRect(cell) || !positiveSize(sheet))
        return {};
    const double width = sheet.width();
    const double height = sheet.height();
    if (cell.left() >= -kGeometryEpsilonMm && cell.top() >= -kGeometryEpsilonMm
        && cell.right() <= width + kGeometryEpsilonMm
        && cell.bottom() <= height + kGeometryEpsilonMm
        && cell.width() >= kMinSlotSideMm && cell.height() >= kMinSlotSideMm) {
        return cell;
    }

    const double minWidth = std::min(kMinSlotSideMm, width);
    const double minHeight = std::min(kMinSlotSideMm, height);
    const double cellWidth = std::clamp(cell.width(), minWidth, width);
    const double cellHeight = std::clamp(cell.height(), minHeight, height);
    const double centreX = std::clamp(cell.center().x(), 0.0, width);
    const double centreY = std::clamp(cell.center().y(), 0.0, height);
    const double left = std::clamp(centreX - cellWidth / 2.0, 0.0, width - cellWidth);
    const double top = std::clamp(centreY - cellHeight / 2.0, 0.0, height - cellHeight);
    return QRectF(left, top, cellWidth, cellHeight);
}

} // namespace

double automaticMarginMm(const QSizeF &sheetMm)
{
    // The arithmetic is the shipped one, in the shipped order, so the result is
    // bit-identical: max(4, min(10, 0.05 * shortSide)). Reordering this
    // expression would change the last bit on some sheets and move a catalogue
    // photograph.
    const double shortSide = std::min(sheetMm.width(), sheetMm.height());
    if (!finite(shortSide) || shortSide <= 0.0)
        return 0.0;
    return std::max(kMinAutoMarginMm,
                    std::min(kMaxAutoMarginMm, kAutoMarginFraction * shortSide));
}

SlotStyle sanitiseStyle(SlotStyle style)
{
    const auto clampValue = [](double value, double maximum) {
        return finite(value) ? std::clamp(value, 0.0, maximum) : 0.0;
    };
    // A non-finite margin or gutter becomes automatic again, because an infinity
    // there is corruption rather than a direction a person chose. An explicit
    // zero survives: it is a real "no margin" choice.
    const auto clampOptional = [](std::optional<double> value,
                                  double maximum) -> std::optional<double> {
        if (!value)
            return std::nullopt;
        if (!finite(*value))
            return std::nullopt;
        return std::clamp(*value, 0.0, maximum);
    };
    style.marginMm = clampOptional(style.marginMm, kMaxMarginMm);
    style.gutterMm = clampOptional(style.gutterMm, kMaxGutterMm);
    style.cornerRadiusMm = clampValue(style.cornerRadiusMm, kMaxCornerRadiusMm);
    return style;
}

bool sanitiseUnitRect(QRectF *rect)
{
    if (!rect || !finiteRect(*rect))
        return false;

    const double left = std::clamp(rect->x(), 0.0, 1.0);
    const double top = std::clamp(rect->y(), 0.0, 1.0);
    const double width = std::clamp(rect->width(), 0.0, 1.0 - left);
    const double height = std::clamp(rect->height(), 0.0, 1.0 - top);
    if (width < 0.001 || height < 0.001)
        return false;

    *rect = QRectF(left, top, width, height);
    return true;
}

bool unitRectIsValid(const QRectF &rect)
{
    return finiteRect(rect) && rect.x() >= 0.0 && rect.y() >= 0.0 && rect.width() >= 0.001
        && rect.height() >= 0.001 && rect.x() + rect.width() <= 1.0 + 1e-9
        && rect.y() + rect.height() <= 1.0 + 1e-9;
}

double effectiveMarginMm(const SlotStyle &style, const QSizeF &sheetMm)
{
    if (!positiveSize(sheetMm))
        return 0.0;
    const double shortSide = std::min(sheetMm.width(), sheetMm.height());
    const double requested = style.marginMm && finite(*style.marginMm)
        ? *style.marginMm
        : automaticMarginMm(sheetMm);
    const double limit = std::max(0.0, shortSide / 2.0 - 2.0);
    return std::clamp(requested, 0.0, std::min(kMaxMarginMm, limit));
}

double effectiveGutterMm(const SlotStyle &style, const QSizeF &sheetMm)
{
    if (!positiveSize(sheetMm))
        return 0.0;
    const double shortSide = std::min(sheetMm.width(), sheetMm.height());
    const double requested = style.gutterMm && finite(*style.gutterMm)
        ? *style.gutterMm
        : effectiveMarginMm(style, sheetMm);
    const double limit = std::max(0.0, shortSide / 2.0 - 2.0);
    return std::clamp(requested, 0.0, std::min(kMaxGutterMm, limit));
}

QRectF cellFromUnitRect(const QRectF &unitRect, const QSizeF &sheetMm, double marginMm,
                        double gutterMm)
{
    if (!positiveSize(sheetMm) || !finite(marginMm) || !finite(gutterMm))
        return {};
    QRectF unit = unitRect;
    if (!sanitiseUnitRect(&unit))
        return {};

    // An outer slot must sit `margin` from the paper edge; two neighbours must
    // be `gutter` apart. Each cell gives up half a gutter on every side, so the
    // box the unit rectangles are stretched over starts at margin - gutter / 2
    // and is sheet - 2 * margin + gutter wide. For margin == gutter this is
    // exactly the catalogue's content box at margin / 2 with size sheet - margin
    // and a margin / 2 adjustment on every edge.
    const double half = gutterMm / 2.0;
    const QRectF box(marginMm - half, marginMm - half,
                     std::max(0.0, sheetMm.width() - 2.0 * marginMm + gutterMm),
                     std::max(0.0, sheetMm.height() - 2.0 * marginMm + gutterMm));
    QRectF cell(box.x() + unit.x() * box.width(), box.y() + unit.y() * box.height(),
                unit.width() * box.width(), unit.height() * box.height());
    cell.adjust(half, half, -half, -half);
    return clampedCellToSheet(cell, sheetMm);
}

QRectF slotCellMm(const Page &page, const Slot &slot)
{
    const QSizeF sheet = pageSizeMm(page);
    const SlotStyle style = sanitiseStyle(page.slotStyle);
    return cellFromUnitRect(slot.unitRect, sheet, effectiveMarginMm(style, sheet),
                            effectiveGutterMm(style, sheet));
}

QRectF slotPhotoRectMm(const Page &page, const Slot &slot)
{
    // The photograph fills its cell. The white paper between photographs is the
    // gutter, not an inset around each picture.
    return slotCellMm(page, slot);
}

QList<QRectF> cellsMm(const Page &page)
{
    QList<QRectF> cells;
    cells.reserve(page.slotList.size());
    for (const Slot &slot : page.slotList)
        cells.append(slotCellMm(page, slot));
    return cells;
}

double cornerRadiusMm(const SlotStyle &style, const QRectF &cellMm)
{
    if (!finiteRect(cellMm) || cellMm.width() <= 0.0 || cellMm.height() <= 0.0)
        return 0.0;
    const double radius = sanitiseStyle(style).cornerRadiusMm;
    if (radius <= 0.0)
        return 0.0;
    return std::min(std::min(radius, kMaxCornerRadiusMm),
                    std::min(cellMm.width(), cellMm.height()) / 2.0);
}

bool keepCropAcrossAspect(double fromWidth, double fromHeight, double toWidth,
                          double toHeight)
{
    const double current = fromHeight > 0.0 ? fromWidth / fromHeight : 0.0;
    const double target = toHeight > 0.0 ? toWidth / toHeight : 0.0;
    if (!(current > 0.0) || !(target > 0.0))
        return false;
    return std::abs(target - current) <= 0.15 * std::max(target, current);
}

const Slot *slotById(const Page &page, const QString &slotId)
{
    if (slotId.isEmpty())
        return nullptr;
    for (const Slot &slot : page.slotList) {
        if (slot.id == slotId)
            return &slot;
    }
    return nullptr;
}

int slotIndexOf(const Page &page, const QString &slotId)
{
    if (slotId.isEmpty())
        return -1;
    for (int i = 0; i < page.slotList.size(); ++i) {
        if (page.slotList.at(i).id == slotId)
            return i;
    }
    return -1;
}

const Slot *firstEmptySlot(const Page &page)
{
    for (const Slot &slot : page.slotList) {
        if (!frameInSlot(page, slot.id))
            return &slot;
    }
    return nullptr;
}

const Frame *frameInSlot(const Page &page, const QString &slotId)
{
    if (slotId.isEmpty())
        return nullptr;
    for (const Frame &frame : page.frames) {
        if (frame.slotId == slotId)
            return &frame;
    }
    return nullptr;
}

const Slot *slotAt(const Page &page, const QPointF &pagePointMm)
{
    // Later slots win, matching the fill order the picker draws and the order
    // the renderer paints in.
    for (int i = page.slotList.size() - 1; i >= 0; --i) {
        const Slot &slot = page.slotList.at(i);
        if (slotCellMm(page, slot).contains(pagePointMm))
            return &slot;
    }
    return nullptr;
}

bool slotGeometryMatches(const Page &page, const Frame &frame)
{
    if (frame.slotId.isEmpty())
        return false;
    const Slot *slot = slotById(page, frame.slotId);
    if (!slot)
        return false;
    const QRectF photo = slotPhotoRectMm(page, *slot);
    if (photo.width() <= 0.0 || photo.height() <= 0.0)
        return false;
    return std::abs(frame.cx - photo.center().x()) <= kGeometryEpsilonMm
        && std::abs(frame.cy - photo.center().y()) <= kGeometryEpsilonMm
        && std::abs(frame.width - photo.width()) <= kGeometryEpsilonMm
        && std::abs(frame.height - photo.height()) <= kGeometryEpsilonMm
        && std::abs(frame.angleDeg) <= kGeometryEpsilonMm;
}

bool layoutConsistent(const Page &page)
{
    if (page.slotList.size() > kMaxSlotsPerPage)
        return false;

    QSet<QString> slotIds;
    for (const Slot &slot : page.slotList) {
        if (slot.id.isEmpty() || slotIds.contains(slot.id))
            return false;
        slotIds.insert(slot.id);
        if (!unitRectIsValid(slot.unitRect))
            return false;
        const QRectF photo = slotPhotoRectMm(page, slot);
        if (photo.width() <= 0.0 || photo.height() <= 0.0)
            return false;
    }

    QSet<QString> assigned;
    for (const Frame &frame : page.frames) {
        if (frame.slotId.isEmpty())
            continue;
        if (!slotIds.contains(frame.slotId))
            return false;
        if (assigned.contains(frame.slotId))
            return false;
        assigned.insert(frame.slotId);
        if (!slotGeometryMatches(page, frame))
            return false;
    }
    return true;
}

void applySlotGeometry(const Page &page, const Slot &slot, Frame &frame)
{
    const QRectF photo = slotPhotoRectMm(page, slot);
    frame.cx = photo.center().x();
    frame.cy = photo.center().y();
    frame.width = photo.width();
    frame.height = photo.height();
    frame.angleDeg = 0.0;
}

} // namespace SlotGeometry
} // namespace pp
