#include "SheetLayout.h"

#include "PaperGeometry.h"
#include "PhotoItem.h"

#include <QCoreApplication>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pp::SheetLayout {

namespace {

/// The grid floor is compared with the same tolerance everywhere, so a value
/// that is mathematically an exact fit does not lose a column to one unit in
/// the last place.
constexpr double kFitEpsilon = 1e-9;

/// Two scores this close are a tie. The mean visible fraction is a product of
/// ratios, so an exact comparison would let rounding pick between two shapes the
/// user cannot tell apart.
constexpr double kScoreEpsilon = 1e-12;
constexpr double kAreaEpsilon = 1e-9;

bool usableSheet(double value)
{
    return std::isfinite(value) && value > 0.0;
}

Error invalidSheet()
{
    return error(QStringLiteral("invalid-sheet"),
                 QCoreApplication::translate("PhotoPrint", "Érvénytelen papírméret."));
}

Error noFrames()
{
    return error(QStringLiteral("no-frames"),
                 QCoreApplication::translate("PhotoPrint", "Ezen a lapon nincs fotó."));
}

QString millimetres(double value)
{
    return QString::number(value);
}

/// Cells across one axis for a fixed size and gutter. The formula is
/// floor((available + gutter) / (size + gutter)); a copy that does not fit
/// yields zero through the floor itself, so no separate special case is needed.
/// The result stops at kMaxCopiesPerPage: one page never places more copies than
/// the action cap, and the block is then centred over the cells it really uses.
int cellsAlong(double available, double size, double gutter)
{
    if (!(available > 0.0) || !(size > 0.0) || gutter < 0.0)
        return 0;
    const double ratio = (available + gutter) / (size + gutter);
    if (!std::isfinite(ratio) || ratio < 1.0 - kFitEpsilon)
        return 0;
    const double count = std::floor(ratio + kFitEpsilon);
    if (count >= double(kMaxCopiesPerPage))
        return kMaxCopiesPerPage;
    return int(count);
}

/// Source aspect as height over width, or the frame's current aspect when the
/// pixel size is unknown. The current aspect is clamped to the documented
/// 0.1..10 range, so a corrupt frame cannot make a candidate score unreachable.
double orientedAspect(const Frame &frame, const QList<QSize> &sourcePixels, int index)
{
    double aspect = 0.0;
    if (index >= 0 && index < sourcePixels.size()) {
        const QSize pixels = sourcePixels.at(index);
        const double width = pixels.width();
        const double height = pixels.height();
        if (width > 0.0 && height > 0.0) {
            const double turns = std::fmod(std::abs(frame.imageRotationDeg), 180.0);
            const bool quarterTurn = std::abs(turns - 90.0) < 1e-6;
            aspect = quarterTurn ? width / height : height / width;
        }
    }
    if (!(aspect > 0.0) || !std::isfinite(aspect)) {
        if (std::isfinite(frame.width) && std::isfinite(frame.height) && frame.width > 0.0
            && frame.height > 0.0) {
            aspect = frame.height / frame.width;
        } else {
            aspect = 1.0;
        }
    }
    return std::clamp(aspect, 0.1, 10.0);
}

bool cuttable(const Frame &frame)
{
    for (const double value : {frame.cx, frame.cy, frame.width, frame.height, frame.angleDeg}) {
        if (!std::isfinite(value))
            return false;
    }
    return frame.width > 0.0 && frame.height > 0.0;
}

} // namespace

bool Sheet::isValid() const
{
    return usableSheet(width) && usableSheet(height);
}

Sheet sheetOf(const PaperSpec &paper, bool landscape)
{
    // PaperGeometry already collapses an unusable side to zero, so the layout
    // never sees a negative or inverted sheet.
    const QSizeF size = PaperGeometry::forPaper(paper, landscape).physicalSizeMm();
    Sheet sheet;
    if (size.isValid() && size.width() > 0.0 && size.height() > 0.0)
        sheet = Sheet{size.width(), size.height()};
    return sheet;
}

Sheet sheetOf(const Page &page)
{
    return sheetOf(page.paper, page.landscape);
}

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

double marginMm(double requested)
{
    return std::clamp(finiteOr(requested, kMinMarginMm), kMinMarginMm, kMaxMarginMm);
}

double gutterMm(double requested)
{
    return std::clamp(finiteOr(requested, 3.0), 0.0, kMaxGutterMm);
}

double copyMm(double requested, double fallback)
{
    return std::clamp(finiteOr(requested, fallback), kMinCopyMm, kMaxCopyMm);
}

int countOf(int requested)
{
    return std::clamp(requested, 1, kMaxCopies);
}

const QList<CopyPreset> &copyPresets()
{
    // Plain data, not branches in the geometry: every preset is laid out by the
    // same formulas. A custom size needs no entry, it is whatever the width and
    // height fields hold.
    static const QList<CopyPreset> presets = {
        {QStringLiteral("passport"), QStringLiteral("35 × 45 mm (igazolványkép)"), 35.0, 45.0},
        {QStringLiteral("id50x70"), QStringLiteral("50 × 70 mm"), 50.0, 70.0},
        {QStringLiteral("sq40"), QStringLiteral("40 × 40 mm"), 40.0, 40.0},
        {QStringLiteral("s30x40"), QStringLiteral("30 × 40 mm"), 30.0, 40.0},
        {QStringLiteral("s25x35"), QStringLiteral("25 × 35 mm"), 25.0, 35.0},
    };
    return presets;
}

const CopyPreset *findCopyPreset(const QString &id)
{
    for (const CopyPreset &preset : copyPresets()) {
        if (preset.id == id)
            return &preset;
    }
    return nullptr;
}

namespace {

Result<BatchGrid> batchGridFor(const Sheet &sheet, const BatchSpec &spec)
{
    if (!sheet.isValid())
        return invalidSheet();

    BatchGrid grid;
    grid.sheet = sheet;
    grid.copyWidthMm = copyMm(spec.copyWidthMm, 35.0);
    grid.copyHeightMm = copyMm(spec.copyHeightMm, 45.0);
    grid.gutterMm = gutterMm(spec.gutterMm);
    grid.marginMm = marginMm(spec.marginMm);

    const double availableWidth = sheet.width - 2.0 * grid.marginMm;
    const double availableHeight = sheet.height - 2.0 * grid.marginMm;
    if (!(availableWidth > 0.0) || !(availableHeight > 0.0)) {
        return error(QStringLiteral("batch-margin"),
                     QCoreApplication::translate(
                         "PhotoPrint",
                         "Ekkora margó mellett nem marad hely. Csökkentsd a margót."));
    }

    grid.columns = cellsAlong(availableWidth, grid.copyWidthMm, grid.gutterMm);
    grid.rows = cellsAlong(availableHeight, grid.copyHeightMm, grid.gutterMm);
    const qint64 cells = qint64(grid.columns) * qint64(grid.rows);

    if (cells <= 0) {
        const QString message = QCoreApplication::translate(
                                    "PhotoPrint",
                                    "A példány nagyobb, mint a lap. Válassz kisebb méretet, "
                                    "vagy nagyobb papírt.")
            + QLatin1Char(' ')
            + QCoreApplication::translate("PhotoPrint", "Legnagyobb: %1 × %2 mm.")
                  .arg(millimetres(availableWidth), millimetres(availableHeight));
        return error(QStringLiteral("copy-too-large"), message);
    }

    // One page places at most the action cap, so a sheet of ten-millimetre
    // copies reports a fit of 100 and the block is sized for the rows it uses.
    grid.perPage = int(std::min<qint64>(cells, kMaxCopies));
    grid.rows = (grid.perPage + grid.columns - 1) / grid.columns;

    grid.count = spec.fillPage ? grid.perPage : countOf(spec.count);
    const int pages = (grid.count + grid.perPage - 1) / grid.perPage;
    if (pages > kMaxPagesPerAction) {
        return error(QStringLiteral("batch-too-many-pages"),
                     QCoreApplication::translate("PhotoPrint",
                                                 "Ez %1 lap lenne. Csökkentsd a darabszámot.")
                         .arg(pages));
    }

    grid.blockWidthMm =
        grid.columns * grid.copyWidthMm + (grid.columns - 1) * grid.gutterMm;
    grid.blockHeightMm = grid.rows * grid.copyHeightMm + (grid.rows - 1) * grid.gutterMm;
    grid.originXMm = (sheet.width - grid.blockWidthMm) / 2.0;
    grid.originYMm = (sheet.height - grid.blockHeightMm) / 2.0;
    grid.pitchXMm = grid.copyWidthMm + grid.gutterMm;
    grid.pitchYMm = grid.copyHeightMm + grid.gutterMm;

    int remaining = grid.count;
    grid.pagesMm.reserve(pages);
    for (int page = 0; page < pages; ++page) {
        const int onPage = std::min(grid.perPage, remaining);
        QList<QRectF> rects;
        rects.reserve(onPage);
        for (int i = 0; i < onPage; ++i) {
            const int column = i % grid.columns;
            const int row = i / grid.columns;
            const double cx = grid.originXMm + grid.copyWidthMm / 2.0 + column * grid.pitchXMm;
            const double cy = grid.originYMm + grid.copyHeightMm / 2.0 + row * grid.pitchYMm;
            rects.append(QRectF(cx - grid.copyWidthMm / 2.0, cy - grid.copyHeightMm / 2.0,
                                grid.copyWidthMm, grid.copyHeightMm));
        }
        grid.pagesMm.append(rects);
        remaining -= onPage;
    }
    return grid;
}

} // namespace

Result<BatchGrid> batchGrid(const Sheet &sheet, const BatchSpec &spec)
{
    return batchGridFor(sheet, spec);
}

Result<BatchGrid> batchGrid(const PaperSpec &paper, bool landscape, const BatchSpec &spec)
{
    return batchGridFor(sheetOf(paper, landscape), spec);
}

Result<BatchGrid> batchGrid(const Page &page, const BatchSpec &spec)
{
    return batchGridFor(sheetOf(page), spec);
}

Result<QList<Frame>> batchFrames(const Frame &source, const BatchGrid &grid)
{
    QList<Frame> frames;
    frames.reserve(grid.count);
    for (const QList<QRectF> &rects : grid.pagesMm) {
        for (const QRectF &rect : rects) {
            // Start from the source and override only what a copy owns. The
            // identity, the source and its sha, the image rotation, the crop,
            // zoom, tone and effect survive untouched.
            Frame frame = source;
            frame.id = makeId(QStringLiteral("frame"));
            frame.cx = rect.center().x();
            frame.cy = rect.center().y();
            frame.width = rect.width();
            frame.height = rect.height();
            frame.angleDeg = 0.0;
            frame.fit = FitMode::Fill;
            // A copy is hand work as far as the first-use grid is concerned: a
            // photograph added later must not reflow the sheet.
            frame.autoPlaced = false;
            // A label numbers trial-sheet variants, not copies of one photo.
            frame.label.clear();
            frames.append(frame);
        }
    }
    return frames;
}

namespace {

Result<AlignPlan> alignGridFor(const QList<Frame> &frames, const Sheet &sheet,
                               const AlignSpec &spec, const QList<QSize> &sourcePixels)
{
    if (!sheet.isValid())
        return invalidSheet();
    const int count = static_cast<int>(frames.size());
    if (count == 0)
        return noFrames();

    const double margin = marginMm(spec.marginMm);
    const double gutter = gutterMm(spec.gutterMm);
    const double usableWidth = sheet.width - 2.0 * margin;
    const double usableHeight = sheet.height - 2.0 * margin;
    if (!(usableWidth > 0.0) || !(usableHeight > 0.0)) {
        return error(QStringLiteral("align-no-fit"),
                     QCoreApplication::translate("PhotoPrint",
                                                 "Ekkora margó mellett nem marad hely. "
                                                 "Csökkentsd a margót."));
    }

    AlignPlan plan;
    plan.frameCount = count;
    if (count == 1) {
        // One photo is centred and nothing else happens. This is deliberately
        // not "fit the page": FitPageEdit already does that.
        plan.columns = 1;
        plan.rows = 1;
        plan.cellWidthMm = usableWidth;
        plan.cellHeightMm = usableHeight;
        plan.score = 1.0;
        plan.meanVisible = 1.0;
        plan.centresMm.append(QPointF(sheet.width / 2.0, sheet.height / 2.0));
        return plan;
    }

    if (spec.columns < 0 || spec.columns > count) {
        return error(QStringLiteral("align-columns"),
                     QCoreApplication::translate("PhotoPrint", "Érvénytelen oszlopszám."));
    }

    const int first = spec.columns > 0 ? spec.columns : 1;
    const int last = spec.columns > 0 ? spec.columns : count;
    bool haveBest = false;
    double bestArea = 0.0;

    for (int columns = first; columns <= last; ++columns) {
        const int rows = (count + columns - 1) / columns;
        const double cellWidth = (usableWidth - (columns - 1) * gutter) / columns;
        const double cellHeight = (usableHeight - (rows - 1) * gutter) / rows;
        if (!(cellWidth > 0.0) || !(cellHeight > 0.0))
            continue;
        if (cellWidth < kMinCellMm || cellHeight < kMinCellMm)
            continue;

        const double cellAspect = cellHeight / cellWidth;
        double visibleSum = 0.0;
        for (int i = 0; i < count; ++i) {
            const double aspect = orientedAspect(frames.at(i), sourcePixels, i);
            visibleSum += std::min(aspect / cellAspect, cellAspect / aspect);
        }
        const double meanVisible = visibleSum / count;
        // The usage factor charges a grid for its empty cells, so a shape with a
        // hole has to earn the hole back with a better crop.
        const double score = meanVisible * double(count) / double(columns * rows);
        const double area = cellWidth * cellHeight;

        // Ascending column order makes the earlier candidate the one with fewer
        // columns, which is the documented tie-break after cell area.
        const bool better = !haveBest || score > plan.score + kScoreEpsilon
            || (std::abs(score - plan.score) <= kScoreEpsilon
                && (area > bestArea + kAreaEpsilon
                    || (std::abs(area - bestArea) <= kAreaEpsilon && columns < plan.columns)));
        if (better) {
            plan.columns = columns;
            plan.rows = rows;
            plan.cellWidthMm = cellWidth;
            plan.cellHeightMm = cellHeight;
            plan.score = score;
            plan.meanVisible = meanVisible;
            bestArea = area;
            haveBest = true;
        }
    }

    if (!haveBest) {
        if (spec.columns > 0) {
            return error(QStringLiteral("align-no-fit"),
                         QCoreApplication::translate(
                             "PhotoPrint",
                             "Ekkora oszlopszámmal a fotók nem férnek el olvasható méretben."));
        }
        const double fitColumns =
            std::floor((usableWidth + gutter) / (kMinCellMm + gutter) + kFitEpsilon);
        const double fitRows =
            std::floor((usableHeight + gutter) / (kMinCellMm + gutter) + kFitEpsilon);
        const int maxFit = fitColumns >= 1.0 && fitRows >= 1.0
            ? int(std::min(fitColumns * fitRows, double(std::numeric_limits<int>::max())))
            : 0;
        return error(QStringLiteral("align-too-many"),
                     QCoreApplication::translate(
                         "PhotoPrint",
                         "Ezen a lapon legfeljebb %1 fotó fér el olvasható méretben. Most %2 van. "
                         "Oszd el őket több lapra, vagy válassz nagyobb papírt.")
                         .arg(maxFit)
                         .arg(count));
    }

    plan.centresMm.reserve(count);
    for (int row = 0; row < plan.rows; ++row) {
        const int inRow = std::min(plan.columns, count - row * plan.columns);
        const double rowWidth =
            inRow * plan.cellWidthMm + (inRow - 1) * gutter;
        const double rowLeft = margin + (usableWidth - rowWidth) / 2.0;
        for (int column = 0; column < inRow; ++column) {
            const double cx =
                rowLeft + plan.cellWidthMm / 2.0 + column * (plan.cellWidthMm + gutter);
            const double cy =
                margin + plan.cellHeightMm / 2.0 + row * (plan.cellHeightMm + gutter);
            plan.centresMm.append(QPointF(cx, cy));
        }
    }
    return plan;
}

Result<DistributePlan> distributeFor(const QList<Frame> &frames, const Sheet &sheet,
                                     const DistributeSpec &spec)
{
    if (!sheet.isValid())
        return invalidSheet();
    const int count = static_cast<int>(frames.size());
    if (count == 0)
        return noFrames();

    for (const Frame &frame : frames) {
        const double turns = std::isfinite(frame.angleDeg)
            ? std::remainder(frame.angleDeg, 360.0)
            : std::numeric_limits<double>::quiet_NaN();
        if (!std::isfinite(turns) || std::abs(turns) > 1e-9) {
            return error(QStringLiteral("distribute-rotated"),
                         QCoreApplication::translate(
                             "PhotoPrint",
                             "A forgatott fotókat nem lehet térközzel rendezni. Előbb állítsd "
                             "őket egyenesbe."));
        }
    }

    const bool horizontal = spec.axis == DistributeAxis::Horizontal;
    const double margin = marginMm(spec.marginMm);
    const double dimension = horizontal ? sheet.width : sheet.height;
    const double span = dimension - 2.0 * margin;
    const double requestedGap = std::clamp(finiteOr(spec.gapMm, 5.0), 0.0, kMaxGutterMm);

    QList<double> sizes;
    sizes.reserve(count);
    double total = 0.0;
    for (const Frame &frame : frames) {
        const double raw = horizontal ? frame.width : frame.height;
        const double size = std::max(0.0, finiteOr(raw, 0.0));
        sizes.append(size);
        total += size;
    }

    if (!(span > 0.0) || total > span + kFitEpsilon) {
        return error(QStringLiteral("distribute-overflow"),
                     QCoreApplication::translate(
                         "PhotoPrint",
                         "A fotók így nem férnek el a lapon. Kicsinyítsd őket, vagy használd a "
                         "rácsot."));
    }

    const double maxGap = count > 1 ? (span - total) / (count - 1) : 0.0;
    const double gap = std::clamp(std::min(requestedGap, maxGap), 0.0, kMaxGutterMm);
    const double block = total + (count - 1) * gap;
    const double start = (dimension - block) / 2.0;

    QList<int> order;
    order.reserve(count);
    for (int i = 0; i < count; ++i)
        order.append(i);
    const auto leadingEdge = [&frames, horizontal](int index) {
        const Frame &frame = frames.at(index);
        const double centre = horizontal ? frame.cx : frame.cy;
        const double size = horizontal ? frame.width : frame.height;
        return finiteOr(centre - finiteOr(size, 0.0) / 2.0, 0.0);
    };
    std::stable_sort(order.begin(), order.end(), [&leadingEdge](int a, int b) {
        return leadingEdge(a) < leadingEdge(b);
    });

    DistributePlan plan;
    plan.axis = spec.axis;
    plan.requestedGapMm = requestedGap;
    plan.gapMm = gap;
    plan.blockMm = block;
    plan.startMm = start;
    plan.order = order;
    plan.centresMm.reserve(count);
    for (int i = 0; i < count; ++i)
        plan.centresMm.append(QPointF());

    const double cross = (horizontal ? sheet.height : sheet.width) / 2.0;
    double edge = start;
    for (const int index : order) {
        const double along = edge + sizes.at(index) / 2.0;
        plan.centresMm[index] =
            horizontal ? QPointF(along, cross) : QPointF(cross, along);
        edge += sizes.at(index) + gap;
    }
    return plan;
}

} // namespace

Result<AlignPlan> alignGrid(const QList<Frame> &frames, const Sheet &sheet, const AlignSpec &spec,
                            const QList<QSize> &sourcePixels)
{
    return alignGridFor(frames, sheet, spec, sourcePixels);
}

Result<AlignPlan> alignGrid(const Page &page, const AlignSpec &spec,
                            const QList<QSize> &sourcePixels)
{
    return alignGridFor(page.frames, sheetOf(page), spec, sourcePixels);
}

Result<QList<Frame>> alignFrames(const QList<Frame> &frames, const Sheet &sheet,
                                 const AlignSpec &spec, const QList<QSize> &sourcePixels)
{
    const auto plan = alignGridFor(frames, sheet, spec, sourcePixels);
    if (!plan)
        return plan.error();

    QList<Frame> result = frames;
    if (result.size() == 1) {
        // Size and angle stay; only the centre and the autoPlaced flag move.
        result[0].cx = plan.value().centresMm.at(0).x();
        result[0].cy = plan.value().centresMm.at(0).y();
        result[0].autoPlaced = false;
        return result;
    }

    for (int i = 0; i < result.size(); ++i) {
        Frame &frame = result[i];
        frame.cx = plan.value().centresMm.at(i).x();
        frame.cy = plan.value().centresMm.at(i).y();
        frame.width = plan.value().cellWidthMm;
        frame.height = plan.value().cellHeightMm;
        frame.angleDeg = 0.0;
        frame.fit = spec.fill ? FitMode::Fill : FitMode::Full;
        frame.autoPlaced = false;
    }
    return result;
}

Result<QList<Frame>> alignFrames(const Page &page, const AlignSpec &spec,
                                 const QList<QSize> &sourcePixels)
{
    return alignFrames(page.frames, sheetOf(page), spec, sourcePixels);
}

Result<DistributePlan> distribute(const QList<Frame> &frames, const Sheet &sheet,
                                  const DistributeSpec &spec)
{
    return distributeFor(frames, sheet, spec);
}

Result<DistributePlan> distribute(const Page &page, const DistributeSpec &spec)
{
    return distributeFor(page.frames, sheetOf(page), spec);
}

Result<QList<Frame>> distributeFrames(const QList<Frame> &frames, const Sheet &sheet,
                                      const DistributeSpec &spec)
{
    const auto plan = distributeFor(frames, sheet, spec);
    if (!plan)
        return plan.error();

    QList<Frame> result = frames;
    for (int i = 0; i < result.size(); ++i) {
        result[i].cx = plan.value().centresMm.at(i).x();
        result[i].cy = plan.value().centresMm.at(i).y();
        // A distributed row is hand work too: a later photograph must not
        // reflow it. Sizes, angle, fit and the image state stay untouched.
        result[i].autoPlaced = false;
    }
    return result;
}

Result<QList<Frame>> distributeFrames(const Page &page, const DistributeSpec &spec)
{
    return distributeFrames(page.frames, sheetOf(page), spec);
}

QList<QLineF> cutGuideLinesMm(const Page &page)
{
    QList<QLineF> lines;
    for (const Frame &frame : page.frames) {
        if (!cuttable(frame))
            continue;
        // The quad is the source of truth for a rotated frame; an axis-aligned
        // box would cut a slant edge as a stair step.
        const QPolygonF quad = PhotoItem::frameQuadMm(frame);
        if (quad.size() != 4)
            continue;
        for (int i = 0; i < 4; ++i)
            lines.append(QLineF(quad.at(i), quad.at((i + 1) % 4)));
    }
    return lines;
}

} // namespace pp::SheetLayout
