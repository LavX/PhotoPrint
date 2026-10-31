#pragma once

#include "Document.h"
#include "Result.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace pp {

struct CollageTemplate;

/// Pure page mutations for slot-based layouts. Every function takes a finished
/// page and returns a finished page (or an error); nothing here touches a
/// controller, an undo stack or a widget. The caller publishes the result as one
/// edit, so one user action is one undo step.
///
/// Placement semantics, stated once so no caller has to guess:
///  * `placePhoto`, `placePhotos` and `movePhoto` REPLACE an occupied slot: the
///    frame that was there is removed from the page and named in
///    `Report::removedFrameIds`. One undo brings it back. Use `swapPhotos` to
///    exchange two photographs instead of discarding one.
///  * At most one frame can name a slot; every function here restores that
///    invariant before returning.
///  * A new photograph (one whose id is not on the page yet) takes Fill, a
///    centred crop and zoom 1. A photograph already on the page keeps its crop
///    and zoom when the slot aspect is within 15 % of its previous aspect, and
///    resets them otherwise, naming the frame in `Report::cropResetIds`.
///  * A frame rotation is dropped on assignment and named in
///    `Report::rotationResetIds`.
namespace SlotLayoutEngine {

/// The picker's minimum photograph side. The minimum is a rule at the moment a
/// layout is chosen or a slot is split, not a model rule: a later paper change
/// may make a cell smaller, and the resolution hint then speaks instead of the
/// edit being refused.
constexpr double kMinPickerSlotMm = 15.0;

/// One call's side effects, so a dialog can warn honestly. Which fields are
/// meaningful depends on the operation; every function clears the struct first.
struct Report {
    /// Frames whose crop centre and zoom were reset because the slot aspect is
    /// too far (more than 15 %, relative to the larger aspect) from the frame's
    /// previous aspect.
    QStringList cropResetIds;
    /// Frames whose frame rotation was non-zero and is now zero.
    QStringList rotationResetIds;
    /// Photographs the new layout or a drop had no slot for. An existing page
    /// frame stays free where it is; a new payload is not added to the page, so
    /// the caller can fall back to its own free placement.
    QStringList leftFreeIds;
    /// Frames removed because their slot was replaced or emptied.
    QStringList removedFrameIds;
    /// Slots with no photograph after the operation.
    int emptySlotsAfter = 0;
};

/// The order in which photographs are shown, assigned and listed: slotted
/// frames by slot index, then free frames in z-order. A frame without a source
/// is not a photograph and is absent. The indices point into `page.frames`, so a
/// caller with parallel data (thumbnails, a list widget) can reorder it the same
/// way the engine fills slots.
QList<int> frameIndicesInDisplayOrder(const Page &page);

/// The same order with the frames' payload copied out.
QList<Frame> photographsInDisplayOrder(const Page &page);

/// Creates the template's layout on the page and assigns the page's photographs
/// in display order, replacing any existing layout. Fewer photographs than slots
/// is not an error: the rest stay empty. More photographs than slots leave the
/// surplus free and names them in the report.
///
/// Applying the same template twice is a no-op: the slot ids are derived from
/// the template id and the slot index, so the second result equals the page.
Result<Page> applyTemplate(const Page &page, const CollageTemplate &t,
                           Report *report = nullptr);

/// The non-destructive form of applyTemplate: a page without a layout is laid
/// out exactly as applyTemplate does; a page that already has this layout keeps
/// its slots and the photographs already in them and only fills empty slots, in
/// slot order, from its free photographs. A page whose layout belongs to another
/// template is refused with `layout-mismatch` instead of being overwritten.
Result<Page> applyTemplateKeeping(const Page &page, const CollageTemplate &t,
                                  Report *report = nullptr);

/// Fills the page's empty slots, in slot order, from its free photographs. Slots
/// and photographs already assigned are untouched, and a page without a layout
/// changes nothing. This is the top-up API a picker calls after the layout is in
/// place; `applyTemplateKeeping` adds the layout creation on top.
Result<Page> fillEmptySlots(Page page, Report *report = nullptr);

/// Puts one photograph into a slot. `frame` is the payload: when its id is
/// already on the page the frame is updated and moved, otherwise it is added.
/// An occupied slot's frame is removed (replace semantics). See the placement
/// semantics above.
Result<Page> placePhoto(Page page, const QString &slotId, Frame frame,
                        Report *report = nullptr);

/// A multi-drop: the first photograph goes to `slotId` (or the first empty slot
/// when the id is empty), the rest fill the remaining empty slots in slot order.
/// Photographs with no slot are not added to the page and are named in
/// `Report::leftFreeIds`, so the caller can fall back to its own free placement.
Result<Page> placePhotos(Page page, const QString &slotId, const QList<Frame> &frames,
                         Report *report = nullptr);

/// Moves a frame that is already on the page into another slot. The target's
/// occupant is removed (replace semantics), not swapped; use swapPhotos for an
/// exchange.
Result<Page> movePhoto(Page page, const QString &frameId, const QString &slotId,
                       Report *report = nullptr);

/// Exchanges the photograph payload of a frame with the photograph in a slot,
/// leaving both frames and their ids in place. The target must be filled; an
/// empty target behaves like movePhoto. Both crops follow the 15 % rule, so a
/// swap between very different cells resets and reports them.
Result<Page> swapPhotos(Page page, const QString &frameId, const QString &slotId,
                        Report *report = nullptr);

/// Removes the photograph in a slot and leaves the slot empty. Emptying an
/// empty slot is a no-op; an unknown id is an error.
Result<Page> emptySlot(Page page, const QString &slotId, Report *report = nullptr);

/// Clears every slot id, the slot list, the style and the layout label, and
/// keeps the frames exactly where they are as free frames.
Page dissolve(const Page &page);

/// Sanitises the style and re-derives every slotted frame's geometry.
Result<Page> setStyle(Page page, const SlotStyle &style);

/// Splits one slot in two along its longer physical axis, in unit space, so the
/// split survives a paper change. The slot named by `afterSlotId` is split, or
/// the largest slot when the id is empty or unknown; the new slot follows it in
/// fill order. Refused above 12 slots and when a half would be under 15 mm on
/// this paper.
Result<Page> addSlot(Page page, const QString &afterSlotId = QString());

/// Removes a slot and the frame assigned to it. Removing the last slot dissolves
/// the layout: the page keeps its (now free) frames and loses the style.
Result<Page> removeSlot(Page page, const QString &slotId);

/// Re-derives every slotted frame's rectangle and angle from its slot. Dangling
/// slot ids are ignored here; repair() clears them.
Result<Page> remap(Page page);

/// The repair point: sanitises the style, slot ids and unit rectangles, drops
/// duplicate slots, clears dangling and duplicate frame assignments, then
/// re-derives the geometry of every remaining slotted frame. A page with no
/// usable sheet is left untouched, because there is no millimetre geometry to
/// derive and both the preview and the renderer refuse to draw such a page.
Result<Page> repair(Page page);

/// Repairs every page of a document, in place order. Used by the loader and by
/// the controller after every mutation, so no second writer can break the
/// invariant.
Result<DocumentSnapshot> repair(DocumentSnapshot document);

} // namespace SlotLayoutEngine
} // namespace pp
