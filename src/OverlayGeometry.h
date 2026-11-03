#pragma once

#include "Document.h"

#include <QFont>
#include <QFontMetricsF>
#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <optional>

namespace pp {

/// Pure placement, order and hit geometry for the page's overlays. No widgets,
/// no pixels, no document mutation: the renderer, the canvas and the tests
/// share one answer, so a sticker or a caption cannot land in a different place
/// on screen and on paper.
///
/// Conventions: page millimetres, origin at the top-left of the physical sheet,
/// X to the right, Y down, exactly like Frame and SlotGeometry.
namespace OverlayGeometry {

/// The largest source a sticker may have. Above this the decode is slow and
/// pointless: a sticker is drawn at millimetres, and a 144 MP source cannot be
/// read within Qt's default allocation limit anyway. The panel reports it
/// instead of silently painting a placeholder.
constexpr qint64 kStickerMaxSourcePixels = 100'000'000; // 100 MP

/// The fixed reference resolution the text block is measured at. A hit box must
/// not move when the view zooms or when the print ppi changes, so the geometry
/// is always measured here and only the glyphs are drawn at the output ppi.
/// 1200 ppi gives 1/47 mm precision at 12 pt.
constexpr double kTextLayoutPpi = 1200.0;

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

/// The same order for a list that a caller has already sanitised or collected,
/// so the renderer can paint a fabricated snapshot without a `Page`.
QList<Sticker> stickersInPaintOrder(const QList<Sticker> &stickers);

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

// --- Text -----------------------------------------------------------------

/// The font a text item is measured and drawn with. `ppi` is the resolution the
/// glyphs are rasterised at: the block geometry always uses kTextLayoutPpi, the
/// page raster uses the output ppi. The family is left empty for the
/// application font, so a project written on one machine does not pin a family
/// the next machine may lack.
QFont fontFor(const TextItem &item, double ppi);

/// Metrics of the same font, for the line layout.
QFontMetricsF metricsFor(const TextItem &item, double ppi);

/// The hard line breaks of a text: '\n' split, no wrapping. An empty text has
/// no lines at all, which is what makes its block empty.
QStringList lines(const QString &text);

/// The block in page millimetres before rotation, centred on cx/cy, measured at
/// kTextLayoutPpi. The width is the widest line, the height is
/// `(n - 1) * lineHeight * lineSpacing + lineHeight`.
QRectF textBlockMm(const TextItem &item);

/// The rotated block, corner order top-left, top-right, bottom-right,
/// bottom-left as seen before rotation. Used for the selection outline, the
/// handles and the hit test.
QPolygonF textQuadMm(const TextItem &item);

/// True when a page point falls inside the rotated block.
bool textContains(const TextItem &item, const QPointF &pageMm);

/// The smallest font size that still gives a block of at least
/// kMinTextBlockMm on both axes. The sanitiser and the resize gesture share it,
/// so a drag can never preview a size the commit would raise.
double minimumFontPt(const TextItem &item);

/// Moves a text by a page-space delta and pulls its centre back onto the paper.
TextItem movedText(const TextItem &item, const QPointF &delta, const QSizeF &sheetMm);

/// Uniform resize about the centre: the distance from the centre sets the scale
/// and the font size follows it, so the block keeps its shape. The size is
/// clamped to the model's range and to the minimum block.
TextItem resizedText(const TextItem &item, const QPointF &pagePoint, const QSizeF &sheetMm);

/// Turns a text towards the pointer, snapping to 15 degrees when `snap` is set.
TextItem rotatedText(const TextItem &item, const QPointF &pagePoint, bool snap);

// --- The shared overlay stack ---------------------------------------------

/// Which list an overlay lives in. Stickers and texts share one `z` space, so
/// the paint order and the layer order have to name both.
enum class OverlayKind { Sticker, Text };

/// One entry of the shared stack: the kind and the index into that kind's list.
struct OverlayRef {
    OverlayKind kind = OverlayKind::Sticker;
    int index = -1;
    bool operator==(const OverlayRef &) const = default;
};

/// The page's overlays bottom first: ascending `z`, stickers before texts on an
/// equal `z`, and document order within each kind. This is the paint order, the
/// hit-test order (reversed) and the list the panel steps through.
QList<OverlayRef> paintOrder(const QList<Sticker> &stickers, const QList<TextItem> &texts);
QList<OverlayRef> paintOrder(const Page &page);

/// The topmost overlay under a page point, or nothing. Iterates the paint order
/// backwards, so the answer matches what the user can see.
std::optional<OverlayRef> overlayAt(const Page &page, const QPointF &pageMm);

/// The id of the topmost overlay under a page point, or an empty string.
QString overlayIdAt(const Page &page, const QPointF &pageMm);

/// Moves one overlay one real step in the shared stack, towards the top
/// (`delta` +1) or the bottom (-1), and renumbers every `z` to its position
/// across both kinds. Raising a sticker above a text therefore swaps them, and
/// one press always changes the visible order even when several layers share a
/// `z`. Returns false when the overlay is already at that end.
bool reorderOverlay(Page &page, const QString &overlayId, int delta);

} // namespace OverlayGeometry
} // namespace pp
