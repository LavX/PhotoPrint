#include "SlotLayoutEngine.h"

#include "CollageTemplates.h"
#include "SlotGeometry.h"

#include <QCoreApplication>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace pp {
namespace SlotLayoutEngine {

namespace {

Error translated(const QString &code, const char *source)
{
    return error(code, QCoreApplication::translate("PhotoPrint", source));
}

/// Frames that are photographs and can take a slot, in display order: slotted
/// frames by slot index, then free frames in z-order. A frame without a source
/// is not a photograph and never takes a slot, which is what keeps an empty
/// placeholder frame from stealing one.
QList<int> assignableFrameOrder(const Page &page)
{
    QList<int> order;
    QSet<int> used;
    for (const Slot &slot : page.slotList) {
        for (int i = 0; i < page.frames.size(); ++i) {
            if (used.contains(i))
                continue;
            if (page.frames.at(i).slotId != slot.id)
                continue;
            used.insert(i);
            if (!page.frames.at(i).sourcePath.isEmpty())
                order.append(i);
            break;
        }
    }
    for (int i = 0; i < page.frames.size(); ++i) {
        if (!used.contains(i) && !page.frames.at(i).sourcePath.isEmpty())
            order.append(i);
    }
    return order;
}

/// The catalogue id and the fill index make the id stable, so re-applying the
/// same template is a no-op instead of a page full of new identities.
QString slotIdFor(const QString &layoutId, int index)
{
    const QString stem = layoutId.isEmpty() ? QStringLiteral("layout") : layoutId;
    return QStringLiteral("slot-%1-%2").arg(stem).arg(index);
}

/// The crop survives when the two aspects are within 15 % of the larger one.
/// This is the catalogue's rule, moved here so the engine and the picker cannot
/// drift.
bool keepCropAcrossAspect(double fromWidth, double fromHeight, double toWidth,
                          double toHeight)
{
    const double current = fromHeight > 0.0 ? fromWidth / fromHeight : 0.0;
    const double target = toHeight > 0.0 ? toWidth / toHeight : 0.0;
    if (!(current > 0.0) || !(target > 0.0))
        return false;
    return std::abs(target - current) <= 0.15 * std::max(target, current);
}

int largestSlotIndex(const Page &page)
{
    int best = -1;
    double bestArea = -1.0;
    for (int i = 0; i < page.slotList.size(); ++i) {
        QRectF cell = SlotGeometry::slotCellMm(page, page.slotList.at(i));
        double area = cell.width() * cell.height();
        if (!(std::isfinite(area)) || area <= 0.0) {
            const QRectF &unit = page.slotList.at(i).unitRect;
            area = unit.width() * unit.height();
        }
        if (area > bestArea) {
            bestArea = area;
            best = i;
        }
    }
    return best;
}

QString paperReference(const Page &page)
{
    if (!page.paper.name.isEmpty())
        return page.paper.name;
    const QSizeF sheet = pageSizeMm(page);
    return QStringLiteral("%1 × %2 mm").arg(sheet.width()).arg(sheet.height());
}

} // namespace

Result<Page> applyTemplate(const Page &page, const CollageTemplate &t, Report *report)
{
    if (report)
        *report = Report{};

    if (t.slotList.isEmpty() || t.slotList.size() != t.photoCount)
        return error(QStringLiteral("template-invalid"),
                     QStringLiteral("a template must have exactly one slot per photograph"));
    if (t.slotList.size() > SlotGeometry::kMaxSlotsPerPage)
        return translated(QStringLiteral("slot-cap"),
                          "Egy lapon legfeljebb 12 hely lehet.");

    Page result = page;
    result.slotList.clear();
    result.slotList.reserve(t.slotList.size());
    for (int i = 0; i < t.slotList.size(); ++i) {
        Slot slot;
        slot.id = slotIdFor(t.id, i);
        slot.unitRect = t.slotList.at(i).rect;
        QRectF unit = slot.unitRect;
        if (!SlotGeometry::sanitiseUnitRect(&unit))
            return error(QStringLiteral("template-invalid"),
                         QStringLiteral("a template slot is not a usable unit rectangle"));
        slot.unitRect = unit;
        result.slotList.append(slot);
    }
    // The layout label is a label only. It is kept verbatim even for a template
    // built by hand, so a screen can name what the page was laid out from.
    result.layoutId = t.id;

    // Nothing keeps its old assignment: the new layout fills from the first
    // slot, and any photograph it cannot place becomes a free frame.
    for (Frame &frame : result.frames)
        frame.slotId.clear();

    const QList<int> order = assignableFrameOrder(page);
    const int assigned = std::min<int>(order.size(), result.slotList.size());
    for (int i = 0; i < assigned; ++i) {
        Frame &frame = result.frames[order.at(i)];
        const Slot &slot = result.slotList.at(i);
        const QRectF photo = SlotGeometry::slotPhotoRectMm(result, slot);

        if (!keepCropAcrossAspect(frame.width, frame.height, photo.width(), photo.height())) {
            frame.cropCenter = QPointF(0.5, 0.5);
            frame.zoom = 1.0;
            if (report)
                report->cropResetIds.append(frame.id);
        }
        if (frame.angleDeg != 0.0 && report)
            report->rotationResetIds.append(frame.id);

        frame.slotId = slot.id;
        frame.fit = FitMode::Fill;
        // A laid-out page is hand work as far as the first-use grid is
        // concerned: a photograph added later must never reflow it.
        frame.autoPlaced = false;
        SlotGeometry::applySlotGeometry(result, slot, frame);
    }
    for (int i = assigned; i < order.size(); ++i) {
        if (report)
            report->leftFreeIds.append(result.frames.at(order.at(i)).id);
    }

    if (report) {
        int empty = 0;
        for (const Slot &slot : result.slotList) {
            if (!SlotGeometry::frameInSlot(result, slot.id))
                ++empty;
        }
        report->emptySlotsAfter = empty;
    }

    if (!SlotGeometry::layoutConsistent(result))
        return error(QStringLiteral("slot-invalid"),
                     QStringLiteral("the layout engine produced an inconsistent page"));
    return result;
}

Page dissolve(const Page &page)
{
    Page result = page;
    for (Frame &frame : result.frames)
        frame.slotId.clear();
    result.slotList.clear();
    result.slotStyle = SlotStyle{};
    result.layoutId.clear();
    return result;
}

Result<Page> setStyle(Page page, const SlotStyle &style)
{
    page.slotStyle = SlotGeometry::sanitiseStyle(style);
    return remap(std::move(page));
}

Result<Page> addSlot(Page page, const QString &afterSlotId)
{
    if (page.slotList.isEmpty())
        return translated(QStringLiteral("no-layout"),
                          "Ehhez a laphoz még nincs elrendezés.");
    if (page.slotList.size() >= SlotGeometry::kMaxSlotsPerPage)
        return translated(QStringLiteral("slot-cap"),
                          "Egy lapon legfeljebb 12 hely lehet.");

    int index = SlotGeometry::slotIndexOf(page, afterSlotId);
    if (index < 0)
        index = largestSlotIndex(page);
    if (index < 0)
        return translated(QStringLiteral("no-layout"),
                          "Ehhez a laphoz még nincs elrendezés.");

    const Slot target = page.slotList.at(index);
    const QRectF cell = SlotGeometry::slotCellMm(page, target);

    Slot first = target;
    Slot second = target;
    second.id = makeId(QStringLiteral("slot"));
    if (cell.width() >= cell.height()) {
        // A wide slot splits into two side-by-side cells, a tall one into two
        // stacked cells: the cut follows the longer physical axis, in unit
        // space, so a later paper change keeps the same split.
        const double half = target.unitRect.width() / 2.0;
        first.unitRect = QRectF(target.unitRect.x(), target.unitRect.y(), half,
                                target.unitRect.height());
        second.unitRect = QRectF(target.unitRect.x() + half, target.unitRect.y(),
                                 target.unitRect.width() - half, target.unitRect.height());
    } else {
        const double half = target.unitRect.height() / 2.0;
        first.unitRect = QRectF(target.unitRect.x(), target.unitRect.y(),
                                target.unitRect.width(), half);
        second.unitRect = QRectF(target.unitRect.x(), target.unitRect.y() + half,
                                 target.unitRect.width(), target.unitRect.height() - half);
    }

    Page candidate = page;
    candidate.slotList[index] = first;
    candidate.slotList.insert(index + 1, second);

    // The 15 mm floor is a picker-time rule: both halves must stay readable on
    // the current paper. A paper change later clamps the cells instead.
    const QRectF firstCell = SlotGeometry::slotCellMm(candidate, first);
    const QRectF secondCell = SlotGeometry::slotCellMm(candidate, second);
    const double smallest = std::min({firstCell.width(), firstCell.height(),
                                      secondCell.width(), secondCell.height()});
    if (!(smallest >= kMinPickerSlotMm - 1e-6)) {
        return error(QStringLiteral("slot-too-small"),
                     QCoreApplication::translate(
                         "PhotoPrint",
                         "Ezen a papíron (%1) ez az elrendezés túl kicsi lenne. "
                         "Válassz nagyobb papírt.")
                         .arg(paperReference(page)));
    }

    return repair(std::move(candidate));
}

Result<Page> removeSlot(Page page, const QString &slotId)
{
    const int index = SlotGeometry::slotIndexOf(page, slotId);
    if (index < 0)
        return translated(QStringLiteral("slot-missing"),
                          "Ez a hely már nem létezik.");

    page.slotList.removeAt(index);
    // The photograph goes with its cell: a photo landing as a free frame on top
    // of a layout is worse than losing it to one undo.
    for (int i = page.frames.size() - 1; i >= 0; --i) {
        if (page.frames.at(i).slotId == slotId)
            page.frames.removeAt(i);
    }

    if (page.slotList.isEmpty()) {
        // No slots left: there is nothing to style, and a style without slots is
        // dropped by the loader anyway, so the in-memory page matches a reload.
        page.slotStyle = SlotStyle{};
        page.layoutId.clear();
        return page;
    }
    return repair(std::move(page));
}

Result<Page> remap(Page page)
{
    const QSizeF sheet = pageSizeMm(page);
    if (!(std::isfinite(sheet.width()) && std::isfinite(sheet.height())
          && sheet.width() > 0.0 && sheet.height() > 0.0)) {
        return page;
    }
    for (Frame &frame : page.frames) {
        if (frame.slotId.isEmpty())
            continue;
        const Slot *slot = SlotGeometry::slotById(page, frame.slotId);
        if (!slot)
            continue;
        SlotGeometry::applySlotGeometry(page, *slot, frame);
    }
    return page;
}

Result<Page> repair(Page page)
{
    // Slots first: a dropped or truncated slot must not keep a frame pointing at
    // it. The order is deliberate, because clearing assignments before the slot
    // list is final would leave a dangling id behind.
    QList<Slot> keptSlots;
    QSet<QString> slotIds;
    keptSlots.reserve(std::min<int>(page.slotList.size(), SlotGeometry::kMaxSlotsPerPage));
    for (const Slot &stored : page.slotList) {
        if (keptSlots.size() >= SlotGeometry::kMaxSlotsPerPage)
            break;
        if (stored.id.isEmpty() || slotIds.contains(stored.id))
            continue;
        Slot slot = stored;
        if (!SlotGeometry::sanitiseUnitRect(&slot.unitRect))
            continue;
        slotIds.insert(slot.id);
        keptSlots.append(slot);
    }
    page.slotList = keptSlots;

    QSet<QString> assigned;
    for (Frame &frame : page.frames) {
        if (frame.slotId.isEmpty())
            continue;
        if (!slotIds.contains(frame.slotId) || assigned.contains(frame.slotId)) {
            frame.slotId.clear();
            continue;
        }
        assigned.insert(frame.slotId);
    }

    // A style and a label describe slots. With no slot left they describe
    // nothing, and the loader would drop them on the next open, so the in-memory
    // page is kept canonical rather than differing from its own saved form.
    if (page.slotList.isEmpty()) {
        page.slotStyle = SlotStyle{};
        page.layoutId.clear();
    }

    return remap(std::move(page));
}

Result<DocumentSnapshot> repair(DocumentSnapshot document)
{
    for (Page &page : document.pages) {
        auto repaired = repair(page);
        if (!repaired)
            return repaired.error();
        page = repaired.value();
    }
    return document;
}

} // namespace SlotLayoutEngine
} // namespace pp
