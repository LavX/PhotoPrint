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

Error slotMissing()
{
    return translated(QStringLiteral("slot-missing"), QT_TRANSLATE_NOOP("PhotoPrint", "Ez a hely már nem létezik."));
}

Error frameMissing()
{
    return translated(QStringLiteral("frame-missing"),QT_TRANSLATE_NOOP("PhotoPrint", "Egy fotó nem található ezen a lapon."));
}

Error slotInvalid()
{
    return translated(QStringLiteral("slot-invalid"),QT_TRANSLATE_NOOP("PhotoPrint", "Az elrendezés nem építhető fel ezen a lapon."));
}

/// A photograph is a frame with a source. A source-less frame is a placeholder
/// with no pixels and must never take a slot.
bool isPhotograph(const Frame &frame)
{
    return !frame.sourcePath.isEmpty();
}

int frameIndexById(const Page &page, const QString &frameId)
{
    for (int i = 0; i < page.frames.size(); ++i) {
        if (page.frames.at(i).id == frameId)
            return i;
    }
    return -1;
}

int occupantIndex(const Page &page, const QString &slotId)
{
    for (int i = 0; i < page.frames.size(); ++i) {
        if (page.frames.at(i).slotId == slotId)
            return i;
    }
    return -1;
}

int emptySlotCount(const Page &page)
{
    int empty = 0;
    for (const Slot &slot : page.slotList) {
        if (!SlotGeometry::frameInSlot(page, slot.id))
            ++empty;
    }
    return empty;
}

/// The catalogue id and the fill index make the id stable, so re-applying the
/// same template is a no-op instead of a page full of new identities.
QString slotIdFor(const QString &layoutId, int index)
{
    const QString stem = layoutId.isEmpty() ? QStringLiteral("layout") : layoutId;
    return QStringLiteral("slot-%1-%2").arg(stem).arg(index);
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

/// Writes one photograph into one slot, in place. The caller clears the report
/// once and repairs once; this function only does the assignment.
///
/// Replace semantics: whichever frame currently names the slot and is not the
/// payload itself is removed and reported. A payload whose id is already on the
/// page is updated and moved; otherwise it is added to the page. A fresh payload
/// takes Fill, a centred crop and zoom 1. An existing payload keeps its fit and
/// its crop when the slot aspect is within 15 % of its previous aspect.
Result<void> assignFrame(Page &page, const QString &slotId, Frame payload, bool forceFill,
                         Report *report)
{
    const Slot *slot = SlotGeometry::slotById(page, slotId);
    if (!slot)
        return slotMissing();
    const QRectF photo = SlotGeometry::slotCellMm(page, *slot);

    const bool existing = frameIndexById(page, payload.id) >= 0;
    if (!existing && payload.sourcePath.isEmpty()) {
        return translated(QStringLiteral("frame-source"),QT_TRANSLATE_NOOP("PhotoPrint", "A fotónak nincs forrásfájlja."));
    }

    for (int i = page.frames.size() - 1; i >= 0; --i) {
        if (page.frames.at(i).slotId != slotId)
            continue;
        if (page.frames.at(i).id == payload.id)
            continue;
        if (report)
            report->removedFrameIds.append(page.frames.at(i).id);
        page.frames.removeAt(i);
    }

    int index = frameIndexById(page, payload.id);
    if (index < 0) {
        page.frames.append(payload);
        index = page.frames.size() - 1;
    } else {
        page.frames[index] = payload;
    }
    Frame &frame = page.frames[index];

    if (existing) {
        if (!SlotGeometry::keepCropAcrossAspect(frame.width, frame.height, photo.width(),
                                                photo.height())) {
            frame.cropCenter = QPointF(0.5, 0.5);
            frame.zoom = 1.0;
            if (report)
                report->cropResetIds.append(frame.id);
        }
        if (frame.angleDeg != 0.0 && report)
            report->rotationResetIds.append(frame.id);
    } else {
        // A fresh photograph has no crop worth keeping; the drop rule is the
        // centre of the image at 1x.
        frame.cropCenter = QPointF(0.5, 0.5);
        frame.zoom = 1.0;
    }
    if (forceFill || !existing)
        frame.fit = FitMode::Fill;
    frame.slotId = slotId;
    // A laid-out page is hand work as far as the first-use grid is concerned: a
    // photograph added later must never reflow it.
    frame.autoPlaced = false;
    // The drop puts the photograph on the cell. From here on the frame's
    // rectangle, size and angle are the user's: the slot only clips it.
    SlotGeometry::initialiseFrameInSlot(page, *slot, frame);
    return Result<void>();
}

/// Swaps everything image-side between two frames and leaves identity, geometry,
/// frame rotation and assignment alone. This is what a library drag between two
/// filled slots means: the photographs change places, the frames do not.
void swapImageState(Frame &a, Frame &b)
{
    std::swap(a.sourcePath, b.sourcePath);
    std::swap(a.sourceSha256, b.sourceSha256);
    std::swap(a.imageRotationDeg, b.imageRotationDeg);
    std::swap(a.zoom, b.zoom);
    std::swap(a.cropCenter, b.cropCenter);
    std::swap(a.fit, b.fit);
    std::swap(a.tone, b.tone);
    std::swap(a.autoTone, b.autoTone);
    std::swap(a.brightness, b.brightness);
    std::swap(a.paperOverride, b.paperOverride);
    std::swap(a.paperEv, b.paperEv);
    std::swap(a.label, b.label);
    std::swap(a.filter, b.filter);
    std::swap(a.filterStrength, b.filterStrength);
    std::swap(a.effectVersion, b.effectVersion);
    std::swap(a.upscale, b.upscale);
}

/// The last step of every placement: restore the invariant, count what is still
/// empty and refuse a result the rest of the program could not trust.
Result<Page> finish(Page page, Report *report)
{
    auto repaired = repair(std::move(page));
    if (!repaired)
        return repaired.error();
    if (!SlotGeometry::layoutConsistent(repaired.value()))
        return slotInvalid();
    if (report)
        report->emptySlotsAfter = emptySlotCount(repaired.value());
    return repaired;
}

} // namespace

QList<int> frameIndicesInDisplayOrder(const Page &page)
{
    QList<int> order;
    QSet<int> used;
    for (const Slot &slot : page.slotList) {
        for (int i = 0; i < page.frames.size(); ++i) {
            if (used.contains(i))
                continue;
            if (page.frames.at(i).slotId != slot.id)
                continue;
            order.append(i);
            used.insert(i);
            break;
        }
    }
    // Free frames keep their z-order after the slots. A frame whose slot id is
    // dangling is not used by any slot loop above, so it lands here too.
    for (int i = 0; i < page.frames.size(); ++i) {
        if (!used.contains(i))
            order.append(i);
    }
    return order;
}

QList<Frame> photographsInDisplayOrder(const Page &page)
{
    QList<Frame> photos;
    for (int index : frameIndicesInDisplayOrder(page)) {
        if (isPhotograph(page.frames.at(index)))
            photos.append(page.frames.at(index));
    }
    return photos;
}

Result<Page> applyTemplate(const Page &page, const CollageTemplate &t, Report *report)
{
    if (report)
        *report = Report{};

    if (t.slotList.isEmpty() || t.slotList.size() != t.photoCount)
        return translated(QStringLiteral("template-invalid"),QT_TRANSLATE_NOOP("PhotoPrint", "A sablonban minden fotóhoz pontosan egy helynek kell tartoznia."));
    if (t.slotList.size() > SlotGeometry::kMaxSlotsPerPage)
        return translated(QStringLiteral("slot-cap"),QT_TRANSLATE_NOOP("PhotoPrint", "Egy lapon legfeljebb 12 hely lehet."));

    Page result = page;
    result.slotList.clear();
    result.slotList.reserve(t.slotList.size());
    for (int i = 0; i < t.slotList.size(); ++i) {
        Slot slot;
        slot.id = slotIdFor(t.id, i);
        slot.unitRect = t.slotList.at(i).rect;
        QRectF unit = slot.unitRect;
        if (!SlotGeometry::sanitiseUnitRect(&unit))
            return translated(QStringLiteral("template-invalid"),QT_TRANSLATE_NOOP("PhotoPrint", "A sablon egyik helye érvénytelen."));
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

    // The indices point into page.frames, and no frame is added or removed while
    // every target slot is empty, so they stay valid in `result` as well.
    const QList<int> order = frameIndicesInDisplayOrder(page);
    QList<int> photos;
    photos.reserve(order.size());
    for (int index : order) {
        if (isPhotograph(page.frames.at(index)))
            photos.append(index);
    }

    const int assigned = std::min<int>(photos.size(), result.slotList.size());
    for (int i = 0; i < assigned; ++i) {
        const Slot &slot = result.slotList.at(i);
        auto placed = assignFrame(result, slot.id, result.frames.at(photos.at(i)), true,
                                  report);
        if (!placed)
            return placed.error();
    }
    for (int i = assigned; i < photos.size(); ++i) {
        if (report)
            report->leftFreeIds.append(result.frames.at(photos.at(i)).id);
    }

    if (report)
        report->emptySlotsAfter = emptySlotCount(result);

    if (!SlotGeometry::layoutConsistent(result))
        return slotInvalid();
    return result;
}

Result<Page> applyTemplateKeeping(const Page &page, const CollageTemplate &t, Report *report)
{
    if (!page.hasLayout())
        return applyTemplate(page, t, report);
    if (!page.layoutId.isEmpty() && page.layoutId != t.id) {
        return translated(QStringLiteral("layout-mismatch"),QT_TRANSLATE_NOOP("PhotoPrint", "Ehhez a laphoz másik elrendezés tartozik."));
    }
    return fillEmptySlots(page, report);
}

Result<Page> fillEmptySlots(Page page, Report *report)
{
    if (report)
        *report = Report{};
    if (!page.hasLayout())
        return page;

    // The free photographs are captured before the first assignment, so a frame
    // moved into a slot cannot be picked up a second time.
    QList<QString> freeIds;
    for (int index : frameIndicesInDisplayOrder(page)) {
        if (page.frames.at(index).slotId.isEmpty()
            && isPhotograph(page.frames.at(index))) {
            freeIds.append(page.frames.at(index).id);
        }
    }

    int next = 0;
    for (const Slot &slot : page.slotList) {
        if (SlotGeometry::frameInSlot(page, slot.id))
            continue;
        if (next >= freeIds.size())
            break;
        const int index = frameIndexById(page, freeIds.at(next));
        if (index < 0)
            continue;
        auto placed = assignFrame(page, slot.id, page.frames.at(index), false, report);
        if (!placed)
            return placed.error();
        ++next;
    }
    for (int i = next; i < freeIds.size(); ++i) {
        if (report)
            report->leftFreeIds.append(freeIds.at(i));
    }
    return finish(std::move(page), report);
}

Result<Page> placePhoto(Page page, const QString &slotId, Frame frame, Report *report)
{
    if (report)
        *report = Report{};
    if (!SlotGeometry::slotById(page, slotId))
        return slotMissing();
    if (frame.id.isEmpty())
        return error(QStringLiteral("frame-id"),
                     QStringLiteral("a placed frame must have an id"));
    auto placed = assignFrame(page, slotId, std::move(frame), false, report);
    if (!placed)
        return placed.error();
    return finish(std::move(page), report);
}

Result<Page> placePhotos(Page page, const QString &slotId, const QList<Frame> &frames,
                         Report *report)
{
    if (report)
        *report = Report{};
    if (frames.isEmpty())
        return finish(std::move(page), report);
    if (!slotId.isEmpty() && !SlotGeometry::slotById(page, slotId))
        return slotMissing();

    QSet<QString> ids;
    for (const Frame &frame : frames) {
        if (frame.id.isEmpty() || ids.contains(frame.id)) {
            return error(QStringLiteral("frame-id"),
                         QStringLiteral("a placement list needs unique, non-empty frame ids"));
        }
        ids.insert(frame.id);
    }

    // The stated slot first (or the first empty one), then the slots that were
    // empty before the drop, in fill order. Slots occupied at the start never
    // take a second photograph, even if an earlier payload moves out of one.
    QList<QString> targets;
    if (!slotId.isEmpty()) {
        targets.append(slotId);
    } else if (const Slot *first = SlotGeometry::firstEmptySlot(page)) {
        targets.append(first->id);
    }
    for (const Slot &slot : page.slotList) {
        if (targets.contains(slot.id))
            continue;
        if (!SlotGeometry::frameInSlot(page, slot.id))
            targets.append(slot.id);
    }

    int placed = 0;
    for (const Frame &frame : frames) {
        if (placed >= targets.size()) {
            if (report)
                report->leftFreeIds.append(frame.id);
            continue;
        }
        auto assigned = assignFrame(page, targets.at(placed), frame, false, report);
        if (!assigned)
            return assigned.error();
        ++placed;
    }
    return finish(std::move(page), report);
}

Result<Page> movePhoto(Page page, const QString &frameId, const QString &slotId,
                       Report *report)
{
    if (report)
        *report = Report{};
    if (!SlotGeometry::slotById(page, slotId))
        return slotMissing();
    const int index = frameIndexById(page, frameId);
    if (index < 0)
        return frameMissing();
    auto assigned = assignFrame(page, slotId, page.frames.at(index), false, report);
    if (!assigned)
        return assigned.error();
    return finish(std::move(page), report);
}

Result<Page> swapPhotos(Page page, const QString &frameId, const QString &slotId,
                        Report *report)
{
    if (report)
        *report = Report{};
    if (!SlotGeometry::slotById(page, slotId))
        return slotMissing();
    const int sourceIndex = frameIndexById(page, frameId);
    if (sourceIndex < 0)
        return frameMissing();
    const int targetIndex = occupantIndex(page, slotId);
    if (targetIndex < 0 || page.frames.at(targetIndex).id == frameId)
        return movePhoto(std::move(page), frameId, slotId, report);

    Frame &source = page.frames[sourceIndex];
    Frame &target = page.frames[targetIndex];
    // Each photograph's crop was tuned for the aspect of the frame it came from;
    // compare that with the frame it lands in before the payload swaps, because
    // the geometry does not change.
    const bool keepForSource =
        SlotGeometry::keepCropAcrossAspect(target.width, target.height, source.width,
                                           source.height);
    const bool keepForTarget =
        SlotGeometry::keepCropAcrossAspect(source.width, source.height, target.width,
                                           target.height);
    swapImageState(source, target);

    if (!keepForSource) {
        source.cropCenter = QPointF(0.5, 0.5);
        source.zoom = 1.0;
        if (report)
            report->cropResetIds.append(source.id);
    }
    if (!keepForTarget) {
        target.cropCenter = QPointF(0.5, 0.5);
        target.zoom = 1.0;
        if (report)
            report->cropResetIds.append(target.id);
    }
    // Both frames keep their own rectangle, size and angle: a swap exchanges
    // the photographs, not the places they were composed in.
    return finish(std::move(page), report);
}

Result<Page> emptySlot(Page page, const QString &slotId, Report *report)
{
    if (report)
        *report = Report{};
    if (!SlotGeometry::slotById(page, slotId))
        return slotMissing();
    for (int i = page.frames.size() - 1; i >= 0; --i) {
        if (page.frames.at(i).slotId != slotId)
            continue;
        if (report)
            report->removedFrameIds.append(page.frames.at(i).id);
        page.frames.removeAt(i);
    }
    return finish(std::move(page), report);
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
    const Page before = page;
    page.slotStyle = SlotGeometry::sanitiseStyle(style);
    return SlotGeometry::remapToPaper(before, std::move(page));
}

Result<Page> addSlot(Page page, const QString &afterSlotId)
{
    if (page.slotList.isEmpty())
        return translated(QStringLiteral("no-layout"),QT_TRANSLATE_NOOP("PhotoPrint", "Ehhez a laphoz még nincs elrendezés."));
    if (page.slotList.size() >= SlotGeometry::kMaxSlotsPerPage)
        return translated(QStringLiteral("slot-cap"),QT_TRANSLATE_NOOP("PhotoPrint", "Egy lapon legfeljebb 12 hely lehet."));

    int index = SlotGeometry::slotIndexOf(page, afterSlotId);
    if (index < 0)
        index = largestSlotIndex(page);
    if (index < 0)
        return translated(QStringLiteral("no-layout"),QT_TRANSLATE_NOOP("PhotoPrint", "Ehhez a laphoz még nincs elrendezés."));

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

    return repair(SlotGeometry::remapToPaper(page, std::move(candidate)));
}

Result<Page> removeSlot(Page page, const QString &slotId)
{
    const int index = SlotGeometry::slotIndexOf(page, slotId);
    if (index < 0)
        return slotMissing();

    page.slotList.removeAt(index);
    // The photograph goes with its cell: a photo landing as a free frame on top
    // of a layout is worse than losing it to one undo.
    for (int i = page.frames.size() - 1; i >= 0; --i) {
        if (page.frames.at(i).slotId == slotId)
            page.frames.removeAt(i);
    }
    // Even the last slot goes through the repair point: another frame may still
    // name the slot that has just disappeared, and a style without slots is
    // dropped here exactly as the loader would drop it.
    return finish(std::move(page), nullptr);
}

Result<Page> repair(Page page)
{
    // Style first: ranges and finiteness are model rules, and a caller that
    // filled the struct directly must not be able to print a negative margin
    // or a NaN gap.
    page.slotStyle = SlotGeometry::sanitiseStyle(page.slotStyle);

    // Slots next: a dropped or truncated slot must not keep a frame pointing at
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

    // Geometry is the user's composition inside the cell, so it is not
    // re-derived. It is only clamped: a frame that a hand-edited file or an old
    // build left fully outside its cell is moved back until a visible part of
    // the photograph is inside the window, instead of loading invisible.
    for (Frame &frame : page.frames) {
        if (frame.slotId.isEmpty())
            continue;
        frame = SlotGeometry::clampFrameToCell(page, frame);
    }
    return page;
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
