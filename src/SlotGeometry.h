#pragma once

#include "Document.h"

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

namespace pp {

/// The one module that knows how a slot's unit rectangle becomes millimetres.
/// It is pure: no widgets, no PDF, no document mutation. The catalogue, the
/// layout engine, the loader, the controller and the renderer all call it, so a
/// mapping change cannot drift between the picker's preview and a printed page.
///
/// Conventions: page millimetres, origin at the top-left of the physical sheet,
/// X to the right, Y down, exactly like Frame. The unit rectangle is 0..1 of the
/// content box, the same convention CollageSlot uses. The physical sheet is the
/// reference, never the driver's logical page.
namespace SlotGeometry {

constexpr double kAutoMarginFraction = 0.05;
constexpr double kMinAutoMarginMm = 4.0;
constexpr double kMaxAutoMarginMm = 10.0;
constexpr double kMaxCornerRadiusMm = 30.0;
constexpr double kMaxMarginMm = 50.0; ///< matches SheetLayout::kMaxMarginMm
constexpr double kMaxGutterMm = 50.0;
constexpr double kMinSlotSideMm = 1.0; ///< model floor; 15 mm is a picker floor
constexpr double kGeometryEpsilonMm = 0.01;
constexpr int kMaxSlotsPerPage = 12;
/// A slotted frame must keep at least this much of itself inside its cell, so a
/// drag or a nudge can never make the photograph disappear. The floor is 10 mm,
/// but never more than 15 % of the cell's smaller side: a small cell is not
/// forced to show most of itself.
constexpr double kMinVisibleMm = 10.0;
constexpr double kMinVisibleFraction = 0.15;

/// 5 % of the short side, clamped 4..10 mm. Bit-identical to the shipped
/// collageMarginMm(): the catalogue's pinned geometry is this function's
/// special case, so it must not move a micron.
double automaticMarginMm(const QSizeF &sheetMm);

/// Clamps every field to its range. A non-finite margin or gutter becomes
/// automatic again, because an infinity there is corruption rather than a
/// direction a person chose; an explicit zero survives. A non-finite corner
/// radius becomes square.
SlotStyle sanitiseStyle(SlotStyle style);

/// Clamps a unit rectangle to 0..1 and to x + w <= 1, y + h <= 1. Returns false
/// when the value is not finite or the rectangle is thinner than 0.001 on
/// either axis after clamping, so a corrupt slot is skipped instead of being
/// rendered as a hairline. On success `rect` is normalised in place.
bool sanitiseUnitRect(QRectF *rect);

/// True when the rectangle is already normalised: finite, inside the unit
/// square and at least 0.001 thick on both axes. This is the stricter check the
/// invariant applies to a stored slot, as opposed to the clamping loader.
bool unitRectIsValid(const QRectF &rect);

/// The margin actually used: the explicit value when set, the automatic value
/// otherwise, and never more than half the short side minus 2 mm, so a content
/// box always exists and a paper change can never produce a negative one. An
/// explicit zero is honoured: a page may print edge to edge inside the sheet.
double effectiveMarginMm(const SlotStyle &style, const QSizeF &sheetMm);

/// The gutter actually used: the explicit value when set, the effective margin
/// otherwise. Same paper-aware clamp as the margin.
double effectiveGutterMm(const SlotStyle &style, const QSizeF &sheetMm);

/// One slot's cell in page millimetres, from the unit rectangle and the paper.
/// The cell is the clipping window of the photograph assigned to it: there is
/// no inset, so the white paper between two cells is exactly `gutterMm`. An
/// outer slot sits `marginMm` from the paper edge; for margin == gutter it
/// reduces exactly to the catalogue's mapping. The result is always inside the
/// sheet, finite, non-negative and at least 1 mm on both axes (or as large as
/// the sheet allows), so no caller ever feeds a negative rectangle to a clamp.
QRectF cellFromUnitRect(const QRectF &unitRect, const QSizeF &sheetMm, double marginMm,
                        double gutterMm);

QRectF slotCellMm(const Page &page, const Slot &slot);
QList<QRectF> cellsMm(const Page &page); ///< in slot order

/// True when a photograph keeps its crop and zoom across an assignment. The two
/// aspects are compared relative to the larger one, so the rule is symmetric: a
/// slot within 15 % of the frame's previous aspect keeps the crop, anything
/// further resets it. The catalogue's template apply, the layout engine's
/// placements and the picker all call this one function, so their warning and
/// their effect cannot drift apart. A non-positive aspect is "not comparable"
/// and resets.
bool keepCropAcrossAspect(double fromWidth, double fromHeight, double toWidth,
                          double toHeight);

/// The corner radius a frame's slot asks for, clamped to the cell's shorter
/// side divided by two. Zero for a frame without a slot, so free frames stay
/// rectangular. The radius rounds the cell's clipping window; there is no ring
/// around the photograph.
double cornerRadiusMm(const SlotStyle &style, const QRectF &cellMm);

const Slot *slotById(const Page &page, const QString &slotId);
int slotIndexOf(const Page &page, const QString &slotId);
/// The first slot with no frame, for the single hint line, or null.
const Slot *firstEmptySlot(const Page &page);
/// The frame assigned to a slot, or null. At most one by invariant.
const Frame *frameInSlot(const Page &page, const QString &slotId);
/// The slot under a point, or null. Later slots win, matching the fill order.
const Slot *slotAt(const Page &page, const QPointF &pagePointMm);

/// True when the frame still sits exactly on its slot's cell: same centre and
/// size within kGeometryEpsilonMm and a zero angle. This is a query, not an
/// invariant: a slotted frame owns its geometry and may be smaller, larger,
/// moved or turned inside the cell. It answers "has this frame been touched
/// since it was placed", which the tests use.
bool slotGeometryMatches(const Page &page, const Frame &frame);

/// Structural invariant, for the loader, the controller and the tests: unique
/// non-empty slot ids, normalised unit rectangles, at most 12 slots, every
/// non-empty slotId naming a slot and at most one frame per slot. Geometry is
/// deliberately absent: the slot is a clipping window, not a box that owns the
/// photograph's rectangle. A page without slots is consistent when no frame
/// carries a slot id.
bool layoutConsistent(const Page &page);

/// The smallest visible intersection a slotted frame must keep with its cell:
/// 10 mm, but never more than 15 % of the cell's smaller side. Zero for an
/// unusable cell.
double minimumVisibleMm(const QRectF &cellMm);

/// The visible part of a slotted frame: the intersection of the frame's rotated
/// rectangle with its cell, as a bounding rectangle. Empty when the frame is
/// outside the cell, has no slot, or the slot is dangling.
QRectF visibleRectMm(const Page &page, const Frame &frame);

/// True when the frame's rotated rectangle is not fully inside its cell, so the
/// renderer clips part of the photograph away. A free frame is never clipped.
bool frameIsClipped(const Page &page, const Frame &frame);

/// Moves a slotted frame back until it overlaps its cell by the visible
/// minimum. Size and angle are kept; a frame that still cannot be seen is
/// centred on the cell, so a photograph can never become invisible. A free
/// frame, or one whose slot is dangling, is returned unchanged.
Frame clampFrameToCell(const Page &page, const Frame &frame);

/// Initial placement of a photograph on a cell: the slot's photo rectangle
/// becomes the frame rectangle and the frame angle becomes zero. Crop, zoom,
/// fit, image rotation and every image field are left alone. Called only when a
/// photograph is put into a slot (template apply, a file or library drop, a
/// move); after that the frame's geometry belongs to the user and nothing
/// re-derives it.
void initialiseFrameInSlot(const Page &page, const Slot &slot, Frame &frame);

/// Re-maps every slotted frame proportionally with its slot's cell, from the
/// cell the slot had on `before` to the cell it has on `after`. The frame's
/// centre offset from the cell centre follows the cell's own scale factors; its
/// size follows them too, except for a frame turned off the page axes, which
/// takes one uniform scale (the geometric mean) so a paper or orientation
/// change cannot shear the composition. The angle is unchanged. A slotted frame
/// whose slot is new on `after` is initialised on its cell. Free frames are
/// untouched.
Page remapToPaper(const Page &before, Page after);

} // namespace SlotGeometry
} // namespace pp
