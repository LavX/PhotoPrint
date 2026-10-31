#pragma once

#include "Document.h"
#include "Result.h"

#include <QString>
#include <QStringList>

namespace pp {

struct CollageTemplate;

/// Pure page mutations for slot-based layouts. Every function takes a finished
/// page and returns a finished page (or an error); nothing here touches a
/// controller, an undo stack or a widget. The caller publishes the result as one
/// edit, so one user action is one undo step.
namespace SlotLayoutEngine {

/// The picker's minimum photograph side. It is a rule at the moment a layout is
/// chosen or a slot is split, not a model rule: a later paper change may make a
/// cell smaller, and the resolution hint then speaks instead of the edit being
/// refused.
constexpr double kMinPickerSlotMm = 15.0;

/// One apply() call's destructive side effects, so a dialog can warn honestly.
struct Report {
    /// Frames whose crop centre and zoom were reset because the slot aspect is
    /// too far (more than 15 %, relative to the larger aspect) from the frame's
    /// previous aspect.
    QStringList cropResetIds;
    /// Frames whose frame rotation was non-zero and is now zero.
    QStringList rotationResetIds;
    /// Photographs the new layout had no slot for. They stay free, in place.
    QStringList leftFreeIds;
    /// Slots with no photograph after the apply.
    int emptySlotsAfter = 0;
};

/// Creates the template's layout on the page and assigns the page's photographs
/// in display order (slotted frames by slot index, then free frames in z-order).
/// Slot geometry comes from the template's unit rectangles and the page's
/// current style, so the default style reproduces the catalogue's millimetres
/// exactly. A photograph takes Fill, a zero angle and autoPlaced false; the crop
/// survives when the new slot aspect is within 15 % of the old frame aspect and
/// resets to centre and 1 otherwise. Fewer photographs than slots is not an
/// error: the rest stay empty. More photographs than slots leave the surplus
/// free and names them in the report. Frames without a source are not
/// photographs and are never assigned.
///
/// Applying the same template twice is a no-op: the slot ids are derived from
/// the template id and the slot index, so the second result equals the page.
Result<Page> applyTemplate(const Page &page, const CollageTemplate &t,
                           Report *report = nullptr);

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

/// The repair point: sanitises slot ids and unit rectangles, drops duplicate
/// slots, clears dangling and duplicate frame assignments, then re-derives the
/// geometry of every remaining slotted frame. A page with no usable sheet is
/// left untouched, because there is no millimetre geometry to derive and both
/// the preview and the renderer refuse to draw such a page.
Result<Page> repair(Page page);

/// Repairs every page of a document, in place order. Used by the loader and by
/// the controller after every mutation, so no second writer can break the
/// invariant.
Result<DocumentSnapshot> repair(DocumentSnapshot document);

} // namespace SlotLayoutEngine
} // namespace pp
