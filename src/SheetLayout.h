#pragma once

#include "Document.h"
#include "Result.h"

#include <QLineF>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>

namespace pp {

/// Layout arithmetic for a portrait sheet: many equal copies of one photograph,
/// and tidying the frames already on a page. Everything is in page millimetres,
/// origin at the top-left corner of the physical sheet, X to the right, Y
/// downwards, exactly like Frame. The physical sheet is the reference, never the
/// driver's logical page: on 10 × 15 cm the logical page is 101.6 × 152.4 mm
/// while the paper is 100 × 150 mm, and a grid built on the logical size drifts
/// against the paper and the cutter.
namespace SheetLayout {

/// Every inkjet clips some border and the model carries no reliable imageable
/// rectangle, so this is the assumed clip. It is used only to explain why the
/// margin floor exists; the geometry below never depends on it.
constexpr double kAssumedClipMm = 3.0;

/// Floor for the requested outer margin: the assumed clip plus half a
/// millimetre of feed tolerance. Raising it to 4 mm would drop 10 × 15 cm
/// landscape from six copies to three, because 45 + 3 + 45 = 93 mm needs a
/// 93 mm content box and a 4 mm margin leaves 92.
constexpr double kMinMarginMm = 3.5;

/// Below this a photograph is not readable, so both the grid search and the
/// batch copy sizing refuse rather than producing a sheet of stamps.
constexpr double kMinCellMm = 15.0;

/// Smallest copy the batch accepts, and the largest.
constexpr double kMinCopyMm = 10.0;
constexpr double kMaxCopyMm = 500.0;

/// Sanitising bounds for the two spacing inputs.
constexpr double kMaxGutterMm = 50.0;
constexpr double kMaxMarginMm = 50.0;

/// One action's limits. The page cap keeps a mistyped count from producing a
/// twenty-sheet document nobody can undo their way out of quickly.
constexpr int kMaxCopies = 100;
constexpr int kMaxCopiesPerPage = 100;
constexpr int kMaxPagesPerAction = 20;

/// A physical sheet in millimetres. Both sides must be finite and positive;
/// anything else is an invalid sheet and the layout refuses it.
struct Sheet {
    double width = 0.0;
    double height = 0.0;

    bool isValid() const;
};

/// The physical sheet of a page or a paper, with the axes swapped for a
/// landscape page. An unusable size becomes an invalid Sheet, never a negative
/// or inverted rectangle.
Sheet sheetOf(const Page &page);
Sheet sheetOf(const PaperSpec &paper, bool landscape);

/// Replaces a non-finite number with a fallback. Every value below can arrive
/// from QSettings, a spin box, a project file or a custom paper, so it is
/// sanitised before any geometry is computed.
double finiteOr(double value, double fallback);

/// Clamped inputs, in the order the document describes: margin to
/// kMinMarginMm..kMaxMarginMm, gutter to 0..kMaxGutterMm, copy sides to
/// kMinCopyMm..kMaxCopyMm and the count to 1..kMaxCopies. A non-finite copy
/// side takes the given fallback, so a missing width and a missing height can
/// default to different values.
double marginMm(double requested);
double gutterMm(double requested);
double copyMm(double requested, double fallback = 35.0);
int countOf(int requested);

/// A fixed copy size offered by the batch dialog. A custom size is not an entry
/// here: it is whatever the user typed into the width and height fields, and it
/// needs no data of its own. 9 × 13 and 10 × 15 cm are deliberately absent,
/// because those are paper sizes, not photo sizes.
struct CopyPreset {
    QString id;
    QString label;
    double widthMm = 0.0;
    double heightMm = 0.0;
};

const QList<CopyPreset> &copyPresets();
const CopyPreset *findCopyPreset(const QString &id);

/// One batch action's inputs. `fillPage` means "as many copies as fit on one
/// sheet" and changes only the count, never the geometry.
struct BatchSpec {
    double copyWidthMm = 35.0;
    double copyHeightMm = 45.0;
    double gutterMm = 3.0;
    double marginMm = kMinMarginMm;
    int count = 6;
    bool fillPage = true;
};

/// The computed grid. One origin and one pitch are used by every sheet, so the
/// first full row of the last sheet sits exactly where it does on the first;
/// the remainder fills row-major from that fixed origin instead of being
/// re-centred, and a stack of sheets can be cut in one guillotine pass.
struct BatchGrid {
    Sheet sheet;
    double copyWidthMm = 0.0;
    double copyHeightMm = 0.0;
    double gutterMm = 0.0;
    double marginMm = 0.0; ///< effective, already raised to kMinMarginMm
    int columns = 0;
    int rows = 0;
    int perPage = 0;
    int count = 0; ///< copies actually produced, after fill and clamping
    double blockWidthMm = 0.0;
    double blockHeightMm = 0.0;
    double originXMm = 0.0; ///< top-left of the block, not of the first copy
    double originYMm = 0.0;
    double pitchXMm = 0.0;
    double pitchYMm = 0.0;
    /// Copy rectangles per sheet, row-major left to right, top to bottom.
    /// Copies never overlap and never leave the sheet.
    QList<QList<QRectF>> pagesMm;

    int pageCount() const { return static_cast<int>(pagesMm.size()); }
};

/// The grid for a sheet. Fails with `invalid-sheet`, `batch-margin` when the
/// margin leaves no room, `copy-too-large` when one copy cannot fit inside the
/// content box, and `batch-too-many-pages` when the count would need more than
/// kMaxPagesPerAction sheets. A requested margin below kMinMarginMm is raised,
/// never honoured, so the outer cut line stays inside the assumed clip.
Result<BatchGrid> batchGrid(const Sheet &sheet, const BatchSpec &spec);
Result<BatchGrid> batchGrid(const PaperSpec &paper, bool landscape, const BatchSpec &spec);
Result<BatchGrid> batchGrid(const Page &page, const BatchSpec &spec);

/// One frame per copy, in page order. Every copy gets a new id, keeps the
/// source and its sha, the image rotation, the crop, zoom, tone and effect, and
/// is forced to angle 0, fit Fill and autoPlaced false. The last two are what
/// make a 35 × 45 cell fill edge to edge and stop a photograph added later from
/// reflowing the sheet. The label is cleared: a copy is not trial-sheet variant.
Result<QList<Frame>> batchFrames(const Frame &source, const BatchGrid &grid);

/// The grid's own inputs. A column count of 0 asks for the best shape; 1..N
/// pins it. `fill` maps to FitMode::Fill, otherwise the whole photo is kept
/// inside the same cell with paper-coloured bands.
struct AlignSpec {
    double marginMm = 10.0;
    double gutterMm = 10.0;
    int columns = 0;
    bool fill = true;
};

/// The chosen equal-cell layout. `centresMm` is in the frame order the caller
/// passed in, so the caller can write the geometry back by index and leave the
/// z-order alone.
struct AlignPlan {
    int columns = 0;
    int rows = 0;
    double cellWidthMm = 0.0;
    double cellHeightMm = 0.0;
    double score = 0.0;      ///< mean visible fraction times cell usage
    double meanVisible = 0.0; ///< 0..1, what Fill keeps on average
    int frameCount = 0;
    QList<QPointF> centresMm;
};

/// Shape search over cols = 1..N. Each candidate is scored by
/// mean(min(a/c, c/a)) * N/(cols*rows), where a is the source aspect and c the
/// cell aspect, so a grid with a hole loses exactly as much as the hole costs.
/// Ties go to the larger cell, then to fewer columns. A candidate whose cell is
/// under kMinCellMm on either axis is skipped; when every candidate is skipped
/// the call fails with `align-too-many` and the readable maximum.
///
/// `sourcePixels` is optional and positional: a valid entry overrides the
/// frame's current aspect, swapped for a 90 or 270 degree image rotation.
///
/// One frame gets a one-cell plan over the content box and no search, because
/// alignFrames() centres it without resizing; the plan's cell size is not used
/// in that case.
Result<AlignPlan> alignGrid(const QList<Frame> &frames, const Sheet &sheet, const AlignSpec &spec,
                            const QList<QSize> &sourcePixels = {});
Result<AlignPlan> alignGrid(const Page &page, const AlignSpec &spec,
                            const QList<QSize> &sourcePixels = {});

/// The frame list the plan produces. Ids, order, source, crop, zoom, tone,
/// effect, label and image rotation survive; only the geometry, angle, fit and
/// autoPlaced change. One frame is centred and left at its own size, which is
/// deliberately not "fit the page": FitPageEdit already does that.
Result<QList<Frame>> alignFrames(const QList<Frame> &frames, const Sheet &sheet,
                                 const AlignSpec &spec,
                                 const QList<QSize> &sourcePixels = {});
Result<QList<Frame>> alignFrames(const Page &page, const AlignSpec &spec,
                                 const QList<QSize> &sourcePixels = {});

enum class DistributeAxis { Horizontal, Vertical };

struct DistributeSpec {
    DistributeAxis axis = DistributeAxis::Horizontal;
    double gapMm = 5.0;
    double marginMm = 10.0;
};

struct DistributePlan {
    DistributeAxis axis = DistributeAxis::Horizontal;
    double requestedGapMm = 0.0;
    double gapMm = 0.0; ///< used gap, never above the request, never negative
    double blockMm = 0.0;
    double startMm = 0.0; ///< leading edge of the first frame
    QList<int> order;     ///< frame indices from the leading edge, stable
    QList<QPointF> centresMm; ///< in the caller's frame order
};

/// One row or one column, sizes unchanged, equal gaps, block centred. The gap
/// shrinks to fit but never below zero, and the photos are never scaled down to
/// make room: that is what the grid mode is for. Sorting is by the current
/// leading edge, stable; the z-order is not touched. A rotated frame is refused
/// with `distribute-rotated`, because its axis-aligned leading edge would no
/// longer describe the piece the user sees.
Result<DistributePlan> distribute(const QList<Frame> &frames, const Sheet &sheet,
                                  const DistributeSpec &spec);
Result<DistributePlan> distribute(const Page &page, const DistributeSpec &spec);
Result<QList<Frame>> distributeFrames(const QList<Frame> &frames, const Sheet &sheet,
                                      const DistributeSpec &spec);
Result<QList<Frame>> distributeFrames(const Page &page, const DistributeSpec &spec);

/// Four edges per frame, clockwise from the top-left corner. The quad comes
/// from PhotoItem::frameQuadMm(), so a rotated frame gets its true edges rather
/// than an axis-aligned box. The screen overlay and the print renderer both use
/// this, so their geometry cannot drift. Frames with unusable geometry are
/// skipped: a corrupt frame draws nothing rather than a huge stray line.
QList<QLineF> cutGuideLinesMm(const Page &page);

} // namespace SheetLayout
} // namespace pp
