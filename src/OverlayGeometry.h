#pragma once

#include "Document.h"

#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>

namespace pp {

/// Pure placement, order and hit geometry for the page's overlays. No widgets,
/// no pixels, no document mutation: the renderer, the canvas and the tests
/// share one answer, so a sticker cannot land in a different place on screen
/// and on paper.
///
/// Conventions: page millimetres, origin at the top-left of the physical sheet,
/// X to the right, Y down, exactly like Frame and SlotGeometry.
namespace OverlayGeometry {

/// The largest source a sticker may have. Above this the decode is slow and
/// pointless: a sticker is drawn at millimetres, and a 144 MP source cannot be
/// read within Qt's default allocation limit anyway. The panel reports it
/// instead of silently painting a placeholder.
constexpr qint64 kStickerMaxSourcePixels = 100'000'000; // 100 MP

/// True when the source is larger than kStickerMaxSourcePixels. A zero or
/// negative size is "unknown", not "too large", so a reader that cannot report
/// a size still gets a chance to decode.
bool stickerSourceTooLarge(const QSize &pixels);

/// The sticker's box in page millimetres before rotation, centred on cx/cy.
QRectF stickerBoxMm(const Sticker &sticker);

/// The rotated box, corner order top-left, top-right, bottom-right,
/// bottom-left as seen before rotation. Used for the selection outline, the
/// handles and the hit test, so all three agree by construction.
QPolygonF stickerQuadMm(const Sticker &sticker);

/// True when a page point falls inside the rotated box. The point is rotated
/// back about the centre instead of testing the quad, because that is the same
/// arithmetic the painter uses and it needs no polygon fill rule.
bool stickerContains(const Sticker &sticker, const QPointF &pageMm);

/// The page's stickers bottom first: ascending `z`, and document order within
/// an equal `z`. This is the paint order and the layer list the panel steps
/// through.
QList<Sticker> stickersInPaintOrder(const Page &page);

/// The topmost sticker under a page point, or null. Iterates the paint order
/// backwards, so the answer matches what the user can see.
const Sticker *stickerAt(const Page &page, const QPointF &pageMm);

/// The default box for a freshly added sticker: centred on the sheet, 25 % of
/// the short side wide, height from the picture's pixel aspect. An empty pixel
/// size gives a square, so a caller without decoded pixels still places
/// something sensible.
QSizeF defaultStickerSizeMm(const QSizeF &sheetMm, const QSize &pixels);

/// Moves a sticker by a page-space delta and pulls its centre back onto the
/// paper. The box may still hang over the edge.
Sticker movedSticker(const Sticker &sticker, const QPointF &delta, const QSizeF &sheetMm);

/// Uniform resize about the centre from a pointer position: the distance from
/// the centre sets the scale, so the pixel aspect is kept by construction. The
/// width is clamped to the model's range.
Sticker resizedSticker(const Sticker &sticker, const QPointF &pagePoint,
                       const QSizeF &sheetMm);

/// Turns a sticker towards the pointer. `snap` rounds to 15 degrees, which is
/// what Shift does while dragging a frame handle.
Sticker rotatedSticker(const Sticker &sticker, const QPointF &pagePoint, bool snap);

/// Moves one sticker one step in paint order, towards the top (`delta` +1) or
/// the bottom (-1), and renumbers every `z` to its position. The renumbering is
/// what makes one press a real reorder even when several stickers share a `z`:
/// moving one against a neighbour can never tie with it. Returns false when the
/// sticker is already at that end.
bool reorderSticker(QList<Sticker> &paintOrder, const QString &stickerId, int delta);

} // namespace OverlayGeometry
} // namespace pp
