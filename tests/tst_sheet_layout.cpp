#include "CanvasController.h"
#include "Document.h"
#include "ProjectStore.h"
#include "SheetLayout.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace pp;
using namespace pp::SheetLayout;

namespace {

constexpr double kEpsilon = 1e-6;

bool near(double a, double b, double tolerance = kEpsilon)
{
    return std::abs(a - b) <= tolerance;
}

bool nearPoint(const QPointF &point, double x, double y)
{
    return near(point.x(), x) && near(point.y(), y);
}

/// Message for a failed Result. QVERIFY2 evaluates its description even when
/// the check passes, and Result::error() asserts on success, so the caller must
/// never reach for error() directly in a verification message.
template <typename T>
QString failureText(const Result<T> &result)
{
    return result.ok() ? QStringLiteral("no error") : result.error().message;
}

Sheet sheetOfSize(double width, double height)
{
    return Sheet{width, height};
}

PaperSpec paperOfSize(double width, double height, const QString &name = QString())
{
    PaperSpec paper;
    paper.name = name;
    paper.physicalWidthMm = width;
    paper.physicalHeightMm = height;
    return paper;
}

Page pageOfSize(double width, double height, bool landscape = false)
{
    Page page;
    page.id = QStringLiteral("page-%1-%2").arg(width).arg(height);
    page.paper = paperOfSize(width, height);
    page.landscape = landscape;
    return page;
}

/// A frame whose non-geometry fields are all non-default, so a field the layout
/// forgets to copy shows up as a failed comparison instead of a lucky match.
Frame sampleFrame(int index)
{
    Frame frame;
    frame.id = QStringLiteral("frame-%1").arg(index);
    frame.sourcePath = QStringLiteral("/photos/photo-%1.jpg").arg(index);
    frame.sourceSha256 = QByteArrayLiteral("sha-") + QByteArray::number(index);
    frame.cx = 20.0 + 15.0 * index;
    frame.cy = 30.0 + 10.0 * index;
    frame.width = 60.0;
    frame.height = 90.0;
    frame.angleDeg = 12.0;
    frame.imageRotationDeg = 90.0 * (index % 4);
    frame.zoom = 1.25;
    frame.cropCenter = QPointF(0.3, 0.4);
    frame.fit = FitMode::Full;
    frame.autoPlaced = true;
    frame.tone.exposureEv = 0.1;
    frame.tone.shadowLift = 0.02;
    frame.tone.highlightCompress = 0.01;
    frame.autoTone = false;
    frame.brightness = 0.05;
    frame.paperOverride = true;
    frame.paperEv = 0.12;
    frame.label = QStringLiteral("#%1").arg(index + 1);
    frame.filter = QStringLiteral("print.sepia");
    frame.filterStrength = 40 + index;
    frame.effectVersion = 2 + index;
    frame.upscale = true;
    return frame;
}

QList<Frame> framesWithAspect(int count, double width, double height)
{
    QList<Frame> frames;
    frames.reserve(count);
    for (int i = 0; i < count; ++i) {
        Frame frame = sampleFrame(i);
        frame.width = width;
        frame.height = height;
        frame.angleDeg = 0.0;
        frame.imageRotationDeg = 0.0;
        frame.cx = 10.0 + 5.0 * i;
        frame.cy = 10.0 + 3.0 * i;
        frames.append(frame);
    }
    return frames;
}

QList<QRectF> allRects(const BatchGrid &grid)
{
    QList<QRectF> rects;
    for (const QList<QRectF> &page : grid.pagesMm)
        rects.append(page);
    return rects;
}

bool batchGridIsSane(const BatchGrid &grid, QString *why)
{
    if (grid.columns < 1 || grid.rows < 1) {
        *why = QStringLiteral("no cells");
        return false;
    }
    if (grid.pagesMm.isEmpty()) {
        *why = QStringLiteral("no pages");
        return false;
    }
    const QList<QRectF> rects = allRects(grid);
    if (rects.size() != grid.count) {
        *why = QStringLiteral("produced %1 rects for %2 copies").arg(rects.size()).arg(grid.count);
        return false;
    }

    double minLeft = std::numeric_limits<double>::max();
    double maxRight = std::numeric_limits<double>::lowest();
    double minTop = std::numeric_limits<double>::max();
    double maxBottom = std::numeric_limits<double>::lowest();
    for (const QRectF &rect : rects) {
        if (!near(rect.width(), grid.copyWidthMm) || !near(rect.height(), grid.copyHeightMm)) {
            *why = QStringLiteral("a copy changed size");
            return false;
        }
        if (rect.left() < -kEpsilon || rect.top() < -kEpsilon
            || rect.right() > grid.sheet.width + kEpsilon
            || rect.bottom() > grid.sheet.height + kEpsilon) {
            *why = QStringLiteral("a copy left the sheet");
            return false;
        }
        minLeft = std::min(minLeft, rect.left());
        maxRight = std::max(maxRight, rect.right());
        minTop = std::min(minTop, rect.top());
        maxBottom = std::max(maxBottom, rect.bottom());
    }
    if (minLeft < grid.marginMm - kEpsilon || minTop < grid.marginMm - kEpsilon
        || grid.sheet.width - maxRight < grid.marginMm - kEpsilon
        || grid.sheet.height - maxBottom < grid.marginMm - kEpsilon) {
        *why = QStringLiteral("the requested margin is not a minimum");
        return false;
    }
    if (!near(minLeft, grid.sheet.width - maxRight)
        || !near(minTop, grid.sheet.height - maxBottom)) {
        *why = QStringLiteral("the block is not centred");
        return false;
    }

    for (int i = 0; i < rects.size(); ++i) {
        for (int j = i + 1; j < rects.size(); ++j) {
            const QRectF a = rects.at(i);
            const QRectF b = rects.at(j);
            const double gapX = std::max(a.left() - b.right(), b.left() - a.right());
            const double gapY = std::max(a.top() - b.bottom(), b.top() - a.bottom());
            if (std::max(gapX, gapY) < grid.gutterMm - kEpsilon) {
                *why = QStringLiteral("copies %1 and %2 overlap or are too close").arg(i).arg(j);
                return false;
            }
            const bool sameRow =
                a.top() < b.bottom() - kEpsilon && b.top() < a.bottom() - kEpsilon;
            if (sameRow && gapX > kEpsilon && gapX < grid.copyWidthMm - kEpsilon
                && !near(gapX, grid.gutterMm)) {
                *why = QStringLiteral("gutter between %1 and %2 is %3").arg(i).arg(j).arg(gapX);
                return false;
            }
            const bool sameColumn =
                a.left() < b.right() - kEpsilon && b.left() < a.right() - kEpsilon;
            if (sameColumn && gapY > kEpsilon && gapY < grid.copyHeightMm - kEpsilon
                && !near(gapY, grid.gutterMm)) {
                *why = QStringLiteral("vertical gutter between %1 and %2 is %3")
                           .arg(i)
                           .arg(j)
                           .arg(gapY);
                return false;
            }
        }
    }
    return true;
}

bool alignPlanIsSane(const AlignPlan &plan, const QList<Frame> &frames, const Sheet &sheet,
                     double margin, double gutter, QString *why)
{
    const int count = static_cast<int>(frames.size());
    if (plan.columns < 1 || plan.rows < 1) {
        *why = QStringLiteral("no cells");
        return false;
    }
    if (plan.rows != (count + plan.columns - 1) / plan.columns) {
        *why = QStringLiteral("row count does not cover the photos");
        return false;
    }
    if (plan.centresMm.size() != count) {
        *why = QStringLiteral("centre count is %1 for %2 photos")
                   .arg(plan.centresMm.size())
                   .arg(count);
        return false;
    }

    QList<QRectF> rects;
    rects.reserve(count);
    for (const QPointF &centre : plan.centresMm) {
        rects.append(QRectF(centre.x() - plan.cellWidthMm / 2.0,
                            centre.y() - plan.cellHeightMm / 2.0, plan.cellWidthMm,
                            plan.cellHeightMm));
    }
    for (const QRectF &rect : rects) {
        if (rect.left() < margin - kEpsilon || rect.top() < margin - kEpsilon
            || rect.right() > sheet.width - margin + kEpsilon
            || rect.bottom() > sheet.height - margin + kEpsilon) {
            *why = QStringLiteral("a cell crossed the margin");
            return false;
        }
    }
    for (int i = 0; i < rects.size(); ++i) {
        for (int j = i + 1; j < rects.size(); ++j) {
            const QRectF a = rects.at(i);
            const QRectF b = rects.at(j);
            const double gapX = std::max(a.left() - b.right(), b.left() - a.right());
            const double gapY = std::max(a.top() - b.bottom(), b.top() - a.bottom());
            if (std::max(gapX, gapY) < gutter - kEpsilon) {
                *why = QStringLiteral("cells %1 and %2 overlap").arg(i).arg(j);
                return false;
            }
        }
    }
    // A full row spans the content box exactly, so both outer margins are the
    // requested value; only the last (partial) row grows.
    for (int row = 0; row + 1 < plan.rows; ++row) {
        const int first = row * plan.columns;
        const int last = first + plan.columns - 1;
        if (!near(rects.at(first).left(), margin)
            || !near(rects.at(last).right(), sheet.width - margin)) {
            *why = QStringLiteral("row %1 is not full width").arg(row);
            return false;
        }
    }
    return true;
}

Frame frameForPage(int index, const QString &pageId)
{
    Frame frame = sampleFrame(index);
    frame.sourcePath = QStringLiteral("/photos/%1-%2.jpg").arg(pageId).arg(index);
    frame.sourceSha256 = QByteArrayLiteral("sha-") + pageId.toUtf8() + QByteArray::number(index);
    return frame;
}

QJsonObject pageObjectAt(const QByteArray &json, int index)
{
    return QJsonDocument::fromJson(json).object()
        .value(QStringLiteral("pages"))
        .toArray()
        .at(index)
        .toObject();
}

QByteArray jsonWithPage(const QByteArray &json, int index, const QJsonObject &page)
{
    QJsonObject root = QJsonDocument::fromJson(json).object();
    QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    pages.replace(index, page);
    root.insert(QStringLiteral("pages"), pages);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

class TestSheetLayout : public QObject
{
    Q_OBJECT

private slots:
    void presetsAreTheOfferedSizes();
    void sanitisesInputs();
    void batchWorkedExamples_data();
    void batchWorkedExamples();
    void batchCentresA4PortraitBlock();
    void batchIsSaneOnEveryPaper();
    void batchSplitsOverflowPages();
    void batchCapsAtOneHundredCopies();
    void batchFramesPreserveTheSource();
    void batchRefusesBadSheets();
    void batchTinyCustomSheets();

    void alignWorkedExamples_data();
    void alignWorkedExamples();
    void alignScoresMatchTheResearch();
    void alignIsSaneForTwoToTwelve();
    void alignPreservesEveryNonGeometryField();
    void alignFullPhotoKeepsTheSameCells();
    void alignOneFrameOnlyCentres();
    void alignRefusesOverCapacity();
    void alignColumnOverride();

    void distributeEqualGaps();
    void distributeShrinksTheGapAndCentres();
    void distributeKeepsSizesAndOrder();
    void distributeRefusesOverflow();
    void distributeRefusesRotated();
    void distributeOneFrameCentres();

    void cutGuideLinesFollowTheFrameQuad();
    void cutGuideLinesSkipBrokenFrames();

    void layoutFieldsSurviveSerialization();
    void oldProjectsGetTheDefaults();
    void unknownLayoutKeysFailClosed();
    void layoutFieldsTakePartInPageEquality();

    void applyLayoutEditIsOneUndoStep();
    void applyLayoutEditNoOpGuard();
    void applyLayoutEditCanChangeOnlyTheGuides();
};

void TestSheetLayout::presetsAreTheOfferedSizes()
{
    const QList<CopyPreset> &presets = copyPresets();
    QCOMPARE(presets.size(), 5);

    QStringList ids;
    for (const CopyPreset &preset : presets) {
        ids.append(preset.id);
        QVERIFY(!preset.label.isEmpty());
        QVERIFY(preset.widthMm >= kMinCopyMm && preset.heightMm >= kMinCopyMm);
        // Paper sizes are not copy sizes.
        QVERIFY(!(preset.widthMm == 90.0 && preset.heightMm == 130.0));
        QVERIFY(!(preset.widthMm == 100.0 && preset.heightMm == 150.0));
    }
    QCOMPARE(ids, QStringList({QStringLiteral("passport"), QStringLiteral("id50x70"),
                               QStringLiteral("sq40"), QStringLiteral("s30x40"),
                               QStringLiteral("s25x35")}));

    const CopyPreset *passport = findCopyPreset(QStringLiteral("passport"));
    QVERIFY(passport);
    QCOMPARE(passport->widthMm, 35.0);
    QCOMPARE(passport->heightMm, 45.0);
    QVERIFY(findCopyPreset(QStringLiteral("p10x15")) == nullptr);
    QVERIFY(findCopyPreset(QString()) == nullptr);
}

void TestSheetLayout::sanitisesInputs()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    QCOMPARE(marginMm(nan), kMinMarginMm);
    QCOMPARE(marginMm(inf), kMinMarginMm);
    QCOMPARE(marginMm(0.0), kMinMarginMm);
    QCOMPARE(marginMm(-12.0), kMinMarginMm);
    QCOMPARE(marginMm(4.5), 4.5);
    QCOMPARE(marginMm(500.0), kMaxMarginMm);

    QCOMPARE(gutterMm(nan), 3.0);
    QCOMPARE(gutterMm(-2.0), 0.0);
    QCOMPARE(gutterMm(500.0), kMaxGutterMm);

    QCOMPARE(copyMm(nan, 45.0), 45.0);
    QCOMPARE(copyMm(0.0), kMinCopyMm);
    QCOMPARE(copyMm(-20.0), kMinCopyMm);
    QCOMPARE(copyMm(1000.0), kMaxCopyMm);

    QCOMPARE(countOf(0), 1);
    QCOMPARE(countOf(-7), 1);
    QCOMPARE(countOf(1000), kMaxCopies);

    const BatchSpec spec;
    for (const Sheet &sheet : {Sheet{}, Sheet{0.0, 150.0}, Sheet{-10.0, 150.0},
                               Sheet{nan, 150.0}, Sheet{100.0, inf}}) {
        const auto grid = batchGrid(sheet, spec);
        QVERIFY(!grid);
        QCOMPARE(grid.error().code, QStringLiteral("invalid-sheet"));
    }

    BatchSpec noisy;
    noisy.copyWidthMm = nan;
    noisy.copyHeightMm = nan;
    noisy.gutterMm = nan;
    noisy.marginMm = nan;
    noisy.count = 0;
    noisy.fillPage = false;
    const auto grid = batchGrid(sheetOfSize(100.0, 150.0), noisy);
    QVERIFY2(grid.ok(), qPrintable(failureText(grid)));
    QCOMPARE(grid.value().copyWidthMm, 35.0);
    QCOMPARE(grid.value().copyHeightMm, 45.0);
    QCOMPARE(grid.value().gutterMm, 3.0);
    QCOMPARE(grid.value().marginMm, kMinMarginMm);
    QCOMPARE(grid.value().count, 1);

    // A landscape page swaps the sheet, not the copy.
    const Page landscape = pageOfSize(100.0, 150.0, true);
    const auto pageGrid = batchGrid(landscape, BatchSpec{});
    QVERIFY(pageGrid.ok());
    QCOMPARE(pageGrid.value().sheet.width, 150.0);
    QCOMPARE(pageGrid.value().sheet.height, 100.0);
    QCOMPARE(pageGrid.value().columns, 3);
}

void TestSheetLayout::batchWorkedExamples_data()
{
    QTest::addColumn<double>("sheetWidth");
    QTest::addColumn<double>("sheetHeight");
    QTest::addColumn<double>("margin");
    QTest::addColumn<double>("copyWidth");
    QTest::addColumn<double>("copyHeight");
    QTest::addColumn<int>("columns");
    QTest::addColumn<int>("rows");
    QTest::addColumn<int>("fit");
    QTest::addColumn<double>("blockWidth");
    QTest::addColumn<double>("blockHeight");
    QTest::addColumn<double>("originX");
    QTest::addColumn<double>("originY");
    QTest::addColumn<double>("firstX");
    QTest::addColumn<double>("firstY");

    QTest::newRow("10x15 portrait") << 100.0 << 150.0 << 3.5 << 35.0 << 45.0 << 2 << 3 << 6
                                    << 73.0 << 141.0 << 13.5 << 4.5 << 31.0 << 27.0;
    QTest::newRow("10x15 landscape") << 150.0 << 100.0 << 3.5 << 35.0 << 45.0 << 3 << 2 << 6
                                     << 111.0 << 93.0 << 19.5 << 3.5 << 37.0 << 26.0;
    QTest::newRow("A4 portrait") << 210.0 << 297.0 << 3.5 << 35.0 << 45.0 << 5 << 6 << 30
                                 << 187.0 << 285.0 << 11.5 << 6.0 << 29.0 << 28.5;
    QTest::newRow("A4 landscape") << 297.0 << 210.0 << 3.5 << 35.0 << 45.0 << 7 << 4 << 28
                                  << 263.0 << 189.0 << 17.0 << 10.5 << 34.5 << 33.0;
    QTest::newRow("10x15 portrait at 5mm") << 100.0 << 150.0 << 5.0 << 35.0 << 45.0 << 2 << 2
                                           << 4 << 73.0 << 93.0 << 13.5 << 28.5 << 31.0 << 51.0;
    QTest::newRow("10x15 landscape at 5mm") << 150.0 << 100.0 << 5.0 << 35.0 << 45.0 << 3 << 1
                                            << 3 << 111.0 << 45.0 << 19.5 << 27.5 << 37.0 << 50.0;
    QTest::newRow("A4 portrait at 5mm") << 210.0 << 297.0 << 5.0 << 35.0 << 45.0 << 5 << 6 << 30
                                        << 187.0 << 285.0 << 11.5 << 6.0 << 29.0 << 28.5;
    QTest::newRow("A4 landscape at 5mm") << 297.0 << 210.0 << 5.0 << 35.0 << 45.0 << 7 << 4 << 28
                                         << 263.0 << 189.0 << 17.0 << 10.5 << 34.5 << 33.0;
    QTest::newRow("50x70 on 10x15") << 100.0 << 150.0 << 3.5 << 50.0 << 70.0 << 1 << 2 << 2
                                    << 50.0 << 143.0 << 25.0 << 3.5 << 50.0 << 38.5;
    QTest::newRow("10x15 copy on A4") << 210.0 << 297.0 << 3.5 << 100.0 << 150.0 << 2 << 1 << 2
                                      << 203.0 << 150.0 << 3.5 << 73.5 << 53.5 << 148.5;
}

void TestSheetLayout::batchWorkedExamples()
{
    QFETCH(double, sheetWidth);
    QFETCH(double, sheetHeight);
    QFETCH(double, margin);
    QFETCH(double, copyWidth);
    QFETCH(double, copyHeight);
    QFETCH(int, columns);
    QFETCH(int, rows);
    QFETCH(int, fit);
    QFETCH(double, blockWidth);
    QFETCH(double, blockHeight);
    QFETCH(double, originX);
    QFETCH(double, originY);
    QFETCH(double, firstX);
    QFETCH(double, firstY);

    BatchSpec spec;
    spec.copyWidthMm = copyWidth;
    spec.copyHeightMm = copyHeight;
    spec.gutterMm = 3.0;
    spec.marginMm = margin;
    spec.fillPage = true;

    const auto grid = batchGrid(sheetOfSize(sheetWidth, sheetHeight), spec);
    QVERIFY2(grid.ok(), qPrintable(failureText(grid)));
    const BatchGrid &value = grid.value();

    QCOMPARE(value.columns, columns);
    QCOMPARE(value.rows, rows);
    QCOMPARE(value.perPage, fit);
    QCOMPARE(value.count, fit);
    QCOMPARE(value.pageCount(), 1);
    QVERIFY(near(value.blockWidthMm, blockWidth));
    QVERIFY(near(value.blockHeightMm, blockHeight));
    QVERIFY(near(value.originXMm, originX));
    QVERIFY(near(value.originYMm, originY));
    QCOMPARE(value.pagesMm.first().size(), fit);

    const QRectF first = value.pagesMm.first().first();
    QVERIFY(nearPoint(first.center(), firstX, firstY));

    QString why;
    QVERIFY2(batchGridIsSane(value, &why), qPrintable(why));
}

void TestSheetLayout::batchCentresA4PortraitBlock()
{
    BatchSpec spec;
    spec.fillPage = true;
    const auto grid = batchGrid(sheetOfSize(210.0, 297.0), spec);
    QVERIFY(grid.ok());
    QCOMPARE(grid.value().perPage, 30);

    const QList<QRectF> &rects = grid.value().pagesMm.first();
    // 30 copies fill five columns and six rows; the last one is bottom-right.
    QVERIFY(nearPoint(rects.first().center(), 29.0, 28.5));
    QVERIFY(nearPoint(rects.last().center(), 181.0, 268.5));
}

void TestSheetLayout::batchIsSaneOnEveryPaper()
{
    const QList<QSizeF> papers = {
        QSizeF(210.0, 297.0), QSizeF(148.0, 210.0), QSizeF(105.0, 148.0),
        QSizeF(90.0, 130.0),  QSizeF(100.0, 150.0), QSizeF(130.0, 180.0),
        QSizeF(60.0, 80.0),   QSizeF(210.0, 210.0),
    };

    BatchSpec spec;
    spec.copyWidthMm = 35.0;
    spec.copyHeightMm = 45.0;
    spec.gutterMm = 3.0;
    spec.marginMm = 3.5;
    spec.fillPage = true;

    for (const QSizeF &paper : papers) {
        for (const bool landscape : {false, true}) {
            const Sheet sheet = landscape ? sheetOfSize(paper.height(), paper.width())
                                          : sheetOfSize(paper.width(), paper.height());
            const auto grid = batchGrid(sheet, spec);
            const QString where = QStringLiteral("%1 × %2 %3")
                                      .arg(sheet.width)
                                      .arg(sheet.height)
                                      .arg(landscape ? QStringLiteral("landscape")
                                                     : QStringLiteral("portrait"));
            QVERIFY2(grid.ok(), qPrintable(where + QStringLiteral(": ") + failureText(grid)));
            const BatchGrid &value = grid.value();

            // Below the hundred-copy cap the fit is exactly the grid.
            QCOMPARE(value.perPage, value.columns * value.rows);
            QString why;
            QVERIFY2(batchGridIsSane(value, &why), qPrintable(where + QStringLiteral(": ") + why));
        }
    }
}

void TestSheetLayout::batchSplitsOverflowPages()
{
    BatchSpec spec;
    spec.gutterMm = 3.0;
    spec.marginMm = 3.5;
    spec.count = 20;
    spec.fillPage = false;

    const auto grid = batchGrid(sheetOfSize(100.0, 150.0), spec);
    QVERIFY2(grid.ok(), qPrintable(failureText(grid)));
    QCOMPARE(grid.value().perPage, 6);
    QCOMPARE(grid.value().count, 20);
    QCOMPARE(grid.value().pageCount(), 4);

    const QList<int> expected = {6, 6, 6, 2};
    for (int page = 0; page < expected.size(); ++page)
        QCOMPARE(grid.value().pagesMm.at(page).size(), expected.at(page));

    // Every sheet uses the same origin and pitch, so the remainder starts at the
    // block's top-left cell and the stack can be cut in one pass.
    const QRectF firstOfPageOne = grid.value().pagesMm.at(0).first();
    const QRectF firstOfPageFour = grid.value().pagesMm.at(3).first();
    QVERIFY(nearPoint(firstOfPageOne.center(), 31.0, 27.0));
    QCOMPARE(firstOfPageFour, firstOfPageOne);

    const auto frames = batchFrames(sampleFrame(0), grid.value());
    QVERIFY(frames.ok());
    QCOMPARE(frames.value().size(), 20);
}

void TestSheetLayout::batchCapsAtOneHundredCopies()
{
    BatchSpec spec;
    spec.count = 100;
    spec.fillPage = false;

    const auto grid = batchGrid(sheetOfSize(210.0, 297.0), spec);
    QVERIFY2(grid.ok(), qPrintable(failureText(grid)));
    QCOMPARE(grid.value().perPage, 30);
    QCOMPARE(grid.value().count, 100);
    QCOMPARE(grid.value().pageCount(), 4);

    const auto frames = batchFrames(sampleFrame(0), grid.value());
    QVERIFY(frames.ok());
    QCOMPARE(frames.value().size(), 100);

    QSet<QString> ids;
    for (const Frame &frame : frames.value()) {
        QVERIFY(!frame.id.isEmpty());
        QVERIFY(!ids.contains(frame.id));
        ids.insert(frame.id);
        QCOMPARE(frame.sourcePath, QStringLiteral("/photos/photo-0.jpg"));
        QCOMPARE(frame.sourceSha256, QByteArrayLiteral("sha-0"));
        QVERIFY(near(frame.width, 35.0));
        QVERIFY(near(frame.height, 45.0));
        QCOMPARE(frame.angleDeg, 0.0);
        QVERIFY(frame.fit == FitMode::Fill);
        QVERIFY(!frame.autoPlaced);
        QVERIFY(frame.label.isEmpty());
    }

    // Above the action cap the count is clamped, not multiplied into pages.
    spec.count = 1000;
    const auto capped = batchGrid(sheetOfSize(210.0, 297.0), spec);
    QVERIFY(capped.ok());
    QCOMPARE(capped.value().count, kMaxCopies);
}

void TestSheetLayout::batchFramesPreserveTheSource()
{
    const Frame source = sampleFrame(0);
    BatchSpec spec;
    spec.fillPage = true;
    const auto grid = batchGrid(sheetOfSize(100.0, 150.0), spec);
    QVERIFY(grid.ok());

    const auto frames = batchFrames(source, grid.value());
    QVERIFY2(frames.ok(), qPrintable(failureText(frames)));
    QCOMPARE(frames.value().size(), 6);

    const QList<QRectF> &rects = grid.value().pagesMm.first();
    for (int i = 0; i < frames.value().size(); ++i) {
        const Frame &frame = frames.value().at(i);
        QVERIFY(frame.id != source.id);
        QCOMPARE(frame.sourcePath, source.sourcePath);
        QCOMPARE(frame.sourceSha256, source.sourceSha256);
        QCOMPARE(frame.imageRotationDeg, source.imageRotationDeg);
        QCOMPARE(frame.zoom, source.zoom);
        QCOMPARE(frame.cropCenter, source.cropCenter);
        QVERIFY(frame.tone == source.tone);
        QCOMPARE(frame.autoTone, source.autoTone);
        QCOMPARE(frame.brightness, source.brightness);
        QCOMPARE(frame.paperOverride, source.paperOverride);
        QCOMPARE(frame.paperEv, source.paperEv);
        QCOMPARE(frame.filter, source.filter);
        QCOMPARE(frame.filterStrength, source.filterStrength);
        QCOMPARE(frame.effectVersion, source.effectVersion);
        QCOMPARE(frame.upscale, source.upscale);

        QCOMPARE(frame.angleDeg, 0.0);
        QVERIFY(frame.fit == FitMode::Fill);
        QVERIFY(!frame.autoPlaced);
        QVERIFY(frame.label.isEmpty());

        QVERIFY(near(frame.cx, rects.at(i).center().x()));
        QVERIFY(near(frame.cy, rects.at(i).center().y()));
        QVERIFY(near(frame.width, rects.at(i).width()));
        QVERIFY(near(frame.height, rects.at(i).height()));
    }
}

void TestSheetLayout::batchRefusesBadSheets()
{
    BatchSpec spec;

    // A copy larger than the content box is refused with the largest size.
    const auto tooLarge = batchGrid(sheetOfSize(60.0, 80.0), BatchSpec{});
    QVERIFY(tooLarge.ok());
    const auto onTiny = batchGrid(sheetOfSize(30.0, 40.0), spec);
    QVERIFY(!onTiny);
    QCOMPARE(onTiny.error().code, QStringLiteral("copy-too-large"));

    // A margin that eats the whole sheet is its own message.
    spec.marginMm = 50.0;
    const auto noRoom = batchGrid(sheetOfSize(100.0, 150.0), spec);
    QVERIFY(!noRoom);
    QCOMPARE(noRoom.error().code, QStringLiteral("batch-margin"));

    // One copy per sheet and a hundred copies is more pages than one action.
    spec.marginMm = kMinMarginMm;
    spec.fillPage = false;
    spec.count = 100;
    spec.copyWidthMm = 35.0;
    spec.copyHeightMm = 45.0;
    const auto manyPages = batchGrid(sheetOfSize(60.0, 80.0), spec);
    QVERIFY(!manyPages);
    QCOMPARE(manyPages.error().code, QStringLiteral("batch-too-many-pages"));
}

void TestSheetLayout::batchTinyCustomSheets()
{
    BatchSpec spec;
    spec.fillPage = true;

    // 60 × 80 mm holds exactly one passport copy at the minimum margin.
    const auto one = batchGrid(sheetOfSize(60.0, 80.0), spec);
    QVERIFY(one.ok());
    QCOMPARE(one.value().perPage, 1);
    QCOMPARE(one.value().count, 1);
    QCOMPARE(one.value().columns, 1);
    QCOMPARE(one.value().rows, 1);
    QVERIFY(near(one.value().originXMm, 12.5));
    QVERIFY(near(one.value().originYMm, 17.5));

    // A 60 × 80 sheet used through the Page overload gives the same grid.
    const Page page = pageOfSize(60.0, 80.0);
    const auto throughPage = batchGrid(page, spec);
    QVERIFY(throughPage.ok());
    QCOMPARE(throughPage.value().perPage, 1);
}

void TestSheetLayout::alignWorkedExamples_data()
{
    QTest::addColumn<double>("sheetWidth");
    QTest::addColumn<double>("sheetHeight");
    QTest::addColumn<int>("count");
    QTest::addColumn<double>("frameWidth");
    QTest::addColumn<double>("frameHeight");
    QTest::addColumn<int>("columns");
    QTest::addColumn<int>("rows");
    QTest::addColumn<double>("cellWidth");
    QTest::addColumn<double>("cellHeight");

    QTest::newRow("A4 four square") << 210.0 << 297.0 << 4 << 60.0 << 60.0 << 2 << 2 << 90.0
                                    << 133.5;
    QTest::newRow("A4 seven portrait") << 210.0 << 297.0 << 7 << 60.0 << 90.0 << 3 << 3
                                       << 170.0 / 3.0 << 257.0 / 3.0;
    QTest::newRow("A4 seven landscape") << 210.0 << 297.0 << 7 << 90.0 << 60.0 << 2 << 4 << 90.0
                                        << 61.75;
    QTest::newRow("A4 three landscape") << 210.0 << 297.0 << 3 << 90.0 << 60.0 << 1 << 3 << 190.0
                                        << 257.0 / 3.0;
    QTest::newRow("A4 three portrait") << 210.0 << 297.0 << 3 << 60.0 << 90.0 << 2 << 2 << 90.0
                                       << 133.5;
}

void TestSheetLayout::alignWorkedExamples()
{
    QFETCH(double, sheetWidth);
    QFETCH(double, sheetHeight);
    QFETCH(int, count);
    QFETCH(double, frameWidth);
    QFETCH(double, frameHeight);
    QFETCH(int, columns);
    QFETCH(int, rows);
    QFETCH(double, cellWidth);
    QFETCH(double, cellHeight);

    const QList<Frame> frames = framesWithAspect(count, frameWidth, frameHeight);
    AlignSpec spec;
    spec.marginMm = 10.0;
    spec.gutterMm = 10.0;

    const auto plan = alignGrid(frames, sheetOfSize(sheetWidth, sheetHeight), spec);
    QVERIFY2(plan.ok(), qPrintable(failureText(plan)));
    QCOMPARE(plan.value().columns, columns);
    QCOMPARE(plan.value().rows, rows);
    QVERIFY(near(plan.value().cellWidthMm, cellWidth));
    QVERIFY(near(plan.value().cellHeightMm, cellHeight));
    QCOMPARE(plan.value().centresMm.size(), count);

    QString why;
    QVERIFY2(alignPlanIsSane(plan.value(), frames, sheetOfSize(sheetWidth, sheetHeight), 10.0,
                             10.0, &why),
             qPrintable(why));

    // Four square photos land in a 2 × 2 grid with the documented centres.
    if (count == 4) {
        QVERIFY(nearPoint(plan.value().centresMm.at(0), 55.0, 76.75));
        QVERIFY(nearPoint(plan.value().centresMm.at(1), 155.0, 76.75));
        QVERIFY(nearPoint(plan.value().centresMm.at(2), 55.0, 220.25));
        QVERIFY(nearPoint(plan.value().centresMm.at(3), 155.0, 220.25));
    }

    // Seven portraits leave the last row with one cell, centred on the page.
    if (count == 7 && frameWidth < frameHeight) {
        const double rowLeft = 10.0 + (190.0 - cellWidth) / 2.0;
        QVERIFY(near(plan.value().centresMm.at(6).x(), rowLeft + cellWidth / 2.0));
    }
}

void TestSheetLayout::alignScoresMatchTheResearch()
{
    AlignSpec spec;
    spec.marginMm = 10.0;
    spec.gutterMm = 10.0;

    const auto portrait =
        alignGrid(framesWithAspect(7, 60.0, 90.0), sheetOfSize(210.0, 297.0), spec);
    QVERIFY(portrait.ok());
    QCOMPARE(portrait.value().columns, 3);
    QCOMPARE(portrait.value().rows, 3);
    QVERIFY(near(portrait.value().score, 0.772, 0.001));

    const auto landscape =
        alignGrid(framesWithAspect(7, 90.0, 60.0), sheetOfSize(210.0, 297.0), spec);
    QVERIFY(landscape.ok());
    QCOMPARE(landscape.value().columns, 2);
    QCOMPARE(landscape.value().rows, 4);
    QVERIFY(near(landscape.value().score, 0.851, 0.002));
}

void TestSheetLayout::alignIsSaneForTwoToTwelve()
{
    AlignSpec spec;
    spec.marginMm = 10.0;
    spec.gutterMm = 10.0;

    for (int count = 2; count <= 12; ++count) {
        QList<Frame> frames;
        frames.reserve(count);
        for (int i = 0; i < count; ++i) {
            // Alternate portrait and landscape sources so the shape search sees
            // a mixed set, which is the case the usage factor exists for.
            const bool portrait = i % 2 == 0;
            Frame frame = sampleFrame(i);
            frame.width = portrait ? 60.0 : 90.0;
            frame.height = portrait ? 90.0 : 60.0;
            frame.angleDeg = 12.0;
            frame.imageRotationDeg = 0.0;
            frames.append(frame);
        }

        const auto plan = alignGrid(frames, sheetOfSize(210.0, 297.0), spec);
        QVERIFY2(plan.ok(), qPrintable(failureText(plan)));
        QString why;
        QVERIFY2(alignPlanIsSane(plan.value(), frames, sheetOfSize(210.0, 297.0), 10.0, 10.0,
                                 &why),
                 qPrintable(QStringLiteral("N=%1: %2").arg(count).arg(why)));

        const auto applied = alignFrames(frames, sheetOfSize(210.0, 297.0), spec);
        QVERIFY(applied.ok());
        QCOMPARE(applied.value().size(), count);
        for (int i = 0; i < count; ++i) {
            QVERIFY(near(applied.value().at(i).width, plan.value().cellWidthMm));
            QVERIFY(near(applied.value().at(i).height, plan.value().cellHeightMm));
            QVERIFY(near(applied.value().at(i).cx, plan.value().centresMm.at(i).x()));
            QVERIFY(near(applied.value().at(i).cy, plan.value().centresMm.at(i).y()));
        }
    }
}

void TestSheetLayout::alignPreservesEveryNonGeometryField()
{
    QList<Frame> frames;
    for (int i = 0; i < 5; ++i) {
        Frame frame = sampleFrame(i);
        frame.imageRotationDeg = (i % 2 == 0) ? 0.0 : 90.0;
        frames.append(frame);
    }
    const QList<Frame> before = frames;

    AlignSpec spec;
    spec.marginMm = 10.0;
    spec.gutterMm = 10.0;
    spec.fill = true;
    const auto aligned = alignFrames(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY2(aligned.ok(), qPrintable(failureText(aligned)));
    QCOMPARE(aligned.value().size(), before.size());

    const auto plan = alignGrid(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(plan.ok());

    for (int i = 0; i < aligned.value().size(); ++i) {
        const Frame &after = aligned.value().at(i);
        const Frame &was = before.at(i);

        QCOMPARE(after.id, was.id);
        QCOMPARE(after.sourcePath, was.sourcePath);
        QCOMPARE(after.sourceSha256, was.sourceSha256);
        QCOMPARE(after.imageRotationDeg, was.imageRotationDeg);
        QCOMPARE(after.zoom, was.zoom);
        QCOMPARE(after.cropCenter, was.cropCenter);
        QVERIFY(after.tone == was.tone);
        QCOMPARE(after.autoTone, was.autoTone);
        QCOMPARE(after.brightness, was.brightness);
        QCOMPARE(after.paperOverride, was.paperOverride);
        QCOMPARE(after.paperEv, was.paperEv);
        QCOMPARE(after.label, was.label);
        QCOMPARE(after.filter, was.filter);
        QCOMPARE(after.filterStrength, was.filterStrength);
        QCOMPARE(after.effectVersion, was.effectVersion);
        QCOMPARE(after.upscale, was.upscale);

        // Only the geometry, angle, fit and autoPlaced change.
        QCOMPARE(after.angleDeg, 0.0);
        QVERIFY(after.fit == FitMode::Fill);
        QVERIFY(!after.autoPlaced);
        QVERIFY(near(after.cx, plan.value().centresMm.at(i).x()));
        QVERIFY(near(after.cy, plan.value().centresMm.at(i).y()));
        QVERIFY(near(after.width, plan.value().cellWidthMm));
        QVERIFY(near(after.height, plan.value().cellHeightMm));
    }
}

void TestSheetLayout::alignFullPhotoKeepsTheSameCells()
{
    const QList<Frame> frames = framesWithAspect(4, 60.0, 60.0);
    AlignSpec fillSpec;
    fillSpec.marginMm = 10.0;
    fillSpec.gutterMm = 10.0;
    fillSpec.fill = true;
    AlignSpec fullSpec = fillSpec;
    fullSpec.fill = false;

    const auto filled = alignFrames(frames, sheetOfSize(210.0, 297.0), fillSpec);
    const auto full = alignFrames(frames, sheetOfSize(210.0, 297.0), fullSpec);
    QVERIFY(filled.ok());
    QVERIFY(full.ok());
    QCOMPARE(filled.value().size(), full.value().size());
    for (int i = 0; i < filled.value().size(); ++i) {
        QVERIFY(near(filled.value().at(i).cx, full.value().at(i).cx));
        QVERIFY(near(filled.value().at(i).cy, full.value().at(i).cy));
        QVERIFY(near(filled.value().at(i).width, full.value().at(i).width));
        QVERIFY(near(filled.value().at(i).height, full.value().at(i).height));
        QVERIFY(filled.value().at(i).fit == FitMode::Fill);
        QVERIFY(full.value().at(i).fit == FitMode::Full);
    }
}

void TestSheetLayout::alignOneFrameOnlyCentres()
{
    Frame frame = sampleFrame(0);
    frame.cx = 12.0;
    frame.cy = 15.0;
    frame.width = 60.0;
    frame.height = 40.0;
    frame.angleDeg = 12.0;
    frame.fit = FitMode::Full;

    AlignSpec spec;
    spec.marginMm = 10.0;
    spec.gutterMm = 10.0;
    const auto aligned = alignFrames(QList<Frame>{frame}, sheetOfSize(210.0, 297.0), spec);
    QVERIFY2(aligned.ok(), qPrintable(failureText(aligned)));
    QCOMPARE(aligned.value().size(), 1);
    QVERIFY(nearPoint(QPointF(aligned.value().first().cx, aligned.value().first().cy), 105.0,
                      148.5));
    QCOMPARE(aligned.value().first().width, 60.0);
    QCOMPARE(aligned.value().first().height, 40.0);
    QCOMPARE(aligned.value().first().angleDeg, 12.0);
    QVERIFY(aligned.value().first().fit == FitMode::Full);
    QCOMPARE(aligned.value().first().id, frame.id);
    QVERIFY(!aligned.value().first().autoPlaced);
}

void TestSheetLayout::alignRefusesOverCapacity()
{
    const QList<Frame> frames = framesWithAspect(120, 60.0, 90.0);
    AlignSpec spec;
    spec.marginMm = 10.0;
    spec.gutterMm = 10.0;

    const auto plan = alignGrid(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(!plan);
    QCOMPARE(plan.error().code, QStringLiteral("align-too-many"));
    QVERIFY2(plan.error().message.contains(QStringLiteral("88")),
             qPrintable(failureText(plan)));
    QVERIFY2(plan.error().message.contains(QStringLiteral("120")),
             qPrintable(failureText(plan)));

    const auto applied = alignFrames(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(!applied);
    QCOMPARE(applied.error().code, QStringLiteral("align-too-many"));

    // No photos or an impossible margin is refused before the search.
    const auto empty = alignFrames({}, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(!empty);
    QCOMPARE(empty.error().code, QStringLiteral("no-frames"));

    AlignSpec wideMargin;
    wideMargin.marginMm = 50.0;
    const auto narrow = alignGrid(framesWithAspect(2, 60.0, 90.0), sheetOfSize(100.0, 150.0),
                                  wideMargin);
    QVERIFY(!narrow);
    QCOMPARE(narrow.error().code, QStringLiteral("align-no-fit"));
}

void TestSheetLayout::alignColumnOverride()
{
    const QList<Frame> frames = framesWithAspect(5, 60.0, 90.0);
    AlignSpec spec;
    spec.marginMm = 10.0;
    spec.gutterMm = 10.0;
    spec.columns = 3;

    const auto plan = alignGrid(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(plan.ok());
    QCOMPARE(plan.value().columns, 3);
    QCOMPARE(plan.value().rows, 2);
    QVERIFY(near(plan.value().cellWidthMm, (190.0 - 20.0) / 3.0));

    spec.columns = 6;
    const auto bad = alignGrid(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(!bad);
    QCOMPARE(bad.error().code, QStringLiteral("align-columns"));

    spec.columns = 5;
    const auto oneRow = alignGrid(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(oneRow.ok());
    QCOMPARE(oneRow.value().rows, 1);
}

void TestSheetLayout::distributeEqualGaps()
{
    QList<Frame> frames;
    for (int i = 0; i < 3; ++i) {
        Frame frame = sampleFrame(i);
        frame.angleDeg = 0.0;
        frame.height = 30.0;
        frames.append(frame);
    }
    frames[0].width = 40.0;
    frames[1].width = 60.0;
    frames[2].width = 50.0;
    frames[0].cx = 20.0;
    frames[1].cx = 170.0;
    frames[2].cx = 80.0;

    DistributeSpec spec;
    spec.axis = DistributeAxis::Horizontal;
    spec.gapMm = 5.0;
    spec.marginMm = 10.0;

    const auto plan = distribute(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY2(plan.ok(), qPrintable(failureText(plan)));
    QVERIFY(near(plan.value().gapMm, 5.0));
    QVERIFY(near(plan.value().blockMm, 160.0));
    QVERIFY(near(plan.value().startMm, 25.0));
    QCOMPARE(plan.value().order, QList<int>({0, 2, 1}));

    // Left edges 25, 70 and 125, in the caller's frame order.
    QVERIFY(near(plan.value().centresMm.at(0).x() - 20.0, 25.0));
    QVERIFY(near(plan.value().centresMm.at(2).x() - 25.0, 70.0));
    QVERIFY(near(plan.value().centresMm.at(1).x() - 30.0, 125.0));
    for (const QPointF &centre : plan.value().centresMm)
        QVERIFY(near(centre.y(), 148.5));

    const auto applied = distributeFrames(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(applied.ok());
    QVERIFY(near(applied.value().at(1).cx - 30.0, 125.0));
    QVERIFY(!applied.value().at(1).autoPlaced);
}

void TestSheetLayout::distributeShrinksTheGapAndCentres()
{
    const double widths[3] = {40.0, 50.0, 60.0};
    QList<Frame> frames;
    for (int i = 0; i < 3; ++i) {
        Frame frame = sampleFrame(i);
        frame.angleDeg = 0.0;
        frame.width = widths[i];
        frame.height = 30.0;
        frame.cx = 10.0 + 20.0 * i;
        frames.append(frame);
    }

    DistributeSpec spec;
    spec.gapMm = 30.0;
    spec.marginMm = 10.0;
    const auto plan = distribute(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(plan.ok());
    QVERIFY(near(plan.value().gapMm, 20.0));
    QVERIFY(near(plan.value().blockMm, 190.0));
    QVERIFY(near(plan.value().startMm, 10.0));

    const auto applied = distributeFrames(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(applied.ok());
    QCOMPARE(applied.value().at(0).width, 40.0);
    QCOMPARE(applied.value().at(1).width, 50.0);
    QCOMPARE(applied.value().at(2).width, 60.0);
    QCOMPARE(applied.value().at(0).height, 30.0);
    QVERIFY(near(applied.value().at(0).cx - 20.0, 10.0));
    QVERIFY(near(applied.value().at(1).cx - 25.0, 70.0));
    QVERIFY(near(applied.value().at(2).cx - 30.0, 140.0));
}

void TestSheetLayout::distributeKeepsSizesAndOrder()
{
    QList<Frame> frames = framesWithAspect(3, 50.0, 40.0);
    DistributeSpec spec;
    spec.axis = DistributeAxis::Vertical;
    spec.gapMm = 5.0;
    spec.marginMm = 10.0;

    const auto applied = distributeFrames(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY2(applied.ok(), qPrintable(failureText(applied)));
    for (int i = 0; i < applied.value().size(); ++i) {
        QCOMPARE(applied.value().at(i).id, frames.at(i).id);
        QCOMPARE(applied.value().at(i).width, frames.at(i).width);
        QCOMPARE(applied.value().at(i).height, frames.at(i).height);
        QCOMPARE(applied.value().at(i).filter, frames.at(i).filter);
        QCOMPARE(applied.value().at(i).label, frames.at(i).label);
        QCOMPARE(applied.value().at(i).cx, 105.0);
    }
    QVERIFY(near(applied.value().at(0).cy, 103.5));
    QVERIFY(near(applied.value().at(1).cy, 148.5));
    QVERIFY(near(applied.value().at(2).cy, 193.5));
}

void TestSheetLayout::distributeRefusesOverflow()
{
    QList<Frame> frames;
    for (int i = 0; i < 3; ++i) {
        Frame frame = sampleFrame(i);
        frame.angleDeg = 0.0;
        frame.width = 100.0;
        frame.height = 30.0;
        frames.append(frame);
    }
    DistributeSpec spec;
    spec.marginMm = 10.0;
    const auto plan = distribute(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(!plan);
    QCOMPARE(plan.error().code, QStringLiteral("distribute-overflow"));

    const auto applied = distributeFrames(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(!applied);
    QCOMPARE(applied.error().code, QStringLiteral("distribute-overflow"));
}

void TestSheetLayout::distributeRefusesRotated()
{
    QList<Frame> frames = framesWithAspect(3, 40.0, 30.0);
    frames[1].angleDeg = 12.0;

    DistributeSpec spec;
    spec.marginMm = 10.0;
    const auto plan = distribute(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(!plan);
    QCOMPARE(plan.error().code, QStringLiteral("distribute-rotated"));
    QVERIFY(!plan.error().message.isEmpty());

    // A full turn is not a rotation.
    frames[1].angleDeg = 360.0;
    const auto applied = distributeFrames(frames, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(applied.ok());
}

void TestSheetLayout::distributeOneFrameCentres()
{
    Frame frame = sampleFrame(0);
    frame.angleDeg = 0.0;
    frame.cx = 10.0;
    frame.cy = 20.0;
    frame.width = 40.0;
    frame.height = 30.0;

    DistributeSpec spec;
    spec.marginMm = 10.0;
    const auto applied = distributeFrames(QList<Frame>{frame}, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(applied.ok());
    QCOMPARE(applied.value().size(), 1);
    QCOMPARE(applied.value().first().width, 40.0);
    QCOMPARE(applied.value().first().height, 30.0);
    QVERIFY(nearPoint(QPointF(applied.value().first().cx, applied.value().first().cy), 105.0,
                      148.5));

    const auto empty = distributeFrames({}, sheetOfSize(210.0, 297.0), spec);
    QVERIFY(!empty);
    QCOMPARE(empty.error().code, QStringLiteral("no-frames"));
}

void TestSheetLayout::cutGuideLinesFollowTheFrameQuad()
{
    Page page = pageOfSize(210.0, 297.0);
    Frame frame;
    frame.id = QStringLiteral("rotated");
    frame.cx = 100.0;
    frame.cy = 100.0;
    frame.width = 40.0;
    frame.height = 20.0;
    frame.angleDeg = 90.0;
    page.frames.append(frame);

    const QList<QLineF> lines = cutGuideLinesMm(page);
    QCOMPARE(lines.size(), 4);

    // A quarter turn swaps the sides: the true edges are 40 mm vertical and
    // 20 mm horizontal, not the other way round.
    QCOMPARE(lines.at(0), QLineF(QPointF(110.0, 80.0), QPointF(110.0, 120.0)));
    QCOMPARE(lines.at(1), QLineF(QPointF(110.0, 120.0), QPointF(90.0, 120.0)));
    QCOMPARE(lines.at(2), QLineF(QPointF(90.0, 120.0), QPointF(90.0, 80.0)));
    QCOMPARE(lines.at(3), QLineF(QPointF(90.0, 80.0), QPointF(110.0, 80.0)));

    // An upright frame gets the plain rectangle edges.
    Page upright = pageOfSize(210.0, 297.0);
    Frame flat = frame;
    flat.angleDeg = 0.0;
    upright.frames.append(flat);
    const QList<QLineF> square = cutGuideLinesMm(upright);
    QCOMPARE(square.size(), 4);
    QCOMPARE(square.at(0), QLineF(QPointF(80.0, 90.0), QPointF(120.0, 90.0)));
}

void TestSheetLayout::cutGuideLinesSkipBrokenFrames()
{
    Page page = pageOfSize(210.0, 297.0);
    Frame good = sampleFrame(0);
    good.angleDeg = 0.0;
    page.frames.append(good);

    Frame zero = sampleFrame(1);
    zero.width = 0.0;
    page.frames.append(zero);

    Frame huge = sampleFrame(2);
    huge.cx = std::numeric_limits<double>::quiet_NaN();
    page.frames.append(huge);

    QCOMPARE(cutGuideLinesMm(page).size(), 4);
    QCOMPARE(cutGuideLinesMm(Page{}).size(), 0);
}

void TestSheetLayout::layoutFieldsSurviveSerialization()
{
    DocumentSnapshot document;
    document.currentPage = 1;

    Page printed = pageOfSize(100.0, 150.0);
    printed.cutGuides = CutGuides::Print;
    printed.borderlessOverride = BorderlessOverride::On;
    Frame printedFrame = frameForPage(0, printed.id);
    printedFrame.sourcePath.clear();
    printed.frames.append(printedFrame);
    document.pages.append(printed);

    Page screen = pageOfSize(210.0, 297.0);
    screen.cutGuides = CutGuides::Screen;
    screen.borderlessOverride = BorderlessOverride::Off;
    Frame screenFrame = frameForPage(1, screen.id);
    screenFrame.sourcePath.clear();
    screen.frames.append(screenFrame);
    document.pages.append(screen);

    Page plain = pageOfSize(100.0, 150.0, true);
    Frame plainFrame = frameForPage(2, plain.id);
    plainFrame.sourcePath.clear();
    plain.frames.append(plainFrame);
    document.pages.append(plain);

    const QByteArray json = ProjectStore::serialize(document, QStringLiteral("/var/tmp/pp"));
    const QJsonObject first = pageObjectAt(json, 0);
    QCOMPARE(first.value(QStringLiteral("cutGuides")).toString(), QStringLiteral("print"));
    QCOMPARE(first.value(QStringLiteral("borderlessOverride")).toString(),
             QStringLiteral("on"));
    const QJsonObject second = pageObjectAt(json, 1);
    QCOMPARE(second.value(QStringLiteral("cutGuides")).toString(), QStringLiteral("screen"));
    QCOMPARE(second.value(QStringLiteral("borderlessOverride")).toString(),
             QStringLiteral("off"));
    // A default page carries neither key, which is what an older build reads.
    const QJsonObject third = pageObjectAt(json, 2);
    QVERIFY(!third.contains(QStringLiteral("cutGuides")));
    QVERIFY(!third.contains(QStringLiteral("borderlessOverride")));

    const auto parsed = ProjectStore::deserialize(json);
    QVERIFY2(parsed.ok(), qPrintable(failureText(parsed)));
    QCOMPARE(parsed.value(), document);
    QVERIFY(parsed.value().pages.at(0).cutGuides == CutGuides::Print);
    QVERIFY(parsed.value().pages.at(0).borderlessOverride == BorderlessOverride::On);
    QVERIFY(parsed.value().pages.at(1).cutGuides == CutGuides::Screen);
    QVERIFY(parsed.value().pages.at(1).borderlessOverride == BorderlessOverride::Off);
    QVERIFY(parsed.value().pages.at(2).cutGuides == CutGuides::None);
    QVERIFY(parsed.value().pages.at(2).borderlessOverride == BorderlessOverride::Auto);
}

void TestSheetLayout::oldProjectsGetTheDefaults()
{
    DocumentSnapshot document;
    Page page = pageOfSize(210.0, 297.0);
    Frame legacyFrame = frameForPage(0, page.id);
    legacyFrame.sourcePath.clear();
    page.frames.append(legacyFrame);
    document.pages.append(page);

    const auto parsed =
        ProjectStore::deserialize(ProjectStore::serialize(document, QStringLiteral("/var/tmp/pp")));
    QVERIFY(parsed.ok());
    QVERIFY(parsed.value().pages.first().cutGuides == CutGuides::None);
    QVERIFY(parsed.value().pages.first().borderlessOverride == BorderlessOverride::Auto);
    QCOMPARE(parsed.value(), document);

    // A 1.0.1 file has no page keys at all. Removing them by hand is the same
    // thing the old writer produced.
    QByteArray json = ProjectStore::serialize(document, QStringLiteral("/var/tmp/pp"));
    QJsonObject root = QJsonDocument::fromJson(json).object();
    QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    QJsonObject first = pages.at(0).toObject();
    first.remove(QStringLiteral("cutGuides"));
    first.remove(QStringLiteral("borderlessOverride"));
    pages.replace(0, first);
    root.insert(QStringLiteral("pages"), pages);
    json = QJsonDocument(root).toJson(QJsonDocument::Compact);

    const auto legacy = ProjectStore::deserialize(json);
    QVERIFY(legacy.ok());
    QVERIFY(legacy.value().pages.first().cutGuides == CutGuides::None);
    QVERIFY(legacy.value().pages.first().borderlessOverride == BorderlessOverride::Auto);
}

void TestSheetLayout::unknownLayoutKeysFailClosed()
{
    DocumentSnapshot document;
    Page page = pageOfSize(210.0, 297.0);
    page.cutGuides = CutGuides::Print;
    page.borderlessOverride = BorderlessOverride::On;
    page.frames.append(frameForPage(0, page.id));
    document.pages.append(page);

    QByteArray json = ProjectStore::serialize(document, QStringLiteral("/var/tmp/pp"));
    QJsonObject first = pageObjectAt(json, 0);
    first.insert(QStringLiteral("cutGuides"), QStringLiteral("bogus"));
    first.insert(QStringLiteral("borderlessOverride"), QStringLiteral("sometimes"));
    json = jsonWithPage(json, 0, first);

    const auto parsed = ProjectStore::deserialize(json);
    QVERIFY(parsed.ok());
    // Fail closed: a corrupt file never puts marks on the paper.
    QVERIFY(parsed.value().pages.first().cutGuides == CutGuides::None);
    QVERIFY(parsed.value().pages.first().borderlessOverride == BorderlessOverride::Auto);

    // The explicit "auto" spelling is understood, not rejected.
    first.insert(QStringLiteral("borderlessOverride"), QStringLiteral("auto"));
    json = jsonWithPage(json, 0, first);
    const auto explicitAuto = ProjectStore::deserialize(json);
    QVERIFY(explicitAuto.ok());
    QVERIFY(explicitAuto.value().pages.first().borderlessOverride
            == BorderlessOverride::Auto);
}

void TestSheetLayout::layoutFieldsTakePartInPageEquality()
{
    Page a = pageOfSize(210.0, 297.0);
    Page b = a;
    QVERIFY(a == b);

    b.cutGuides = CutGuides::Screen;
    QVERIFY(a != b);
    b.cutGuides = CutGuides::None;
    QVERIFY(a == b);

    b.borderlessOverride = BorderlessOverride::Off;
    QVERIFY(a != b);
    b.borderlessOverride = BorderlessOverride::Auto;
    QVERIFY(a == b);
}

void TestSheetLayout::applyLayoutEditIsOneUndoStep()
{
    DocumentSnapshot document;
    Page first = pageOfSize(210.0, 297.0);
    first.frames = {frameForPage(0, first.id), frameForPage(1, first.id)};
    document.pages.append(first);

    CanvasController controller;
    controller.setDocument(document);
    const DocumentSnapshot before = controller.document();

    QList<Frame> laidOut;
    for (int i = 0; i < 3; ++i)
        laidOut.append(frameForPage(10 + i, first.id));

    Page second = pageOfSize(210.0, 297.0);
    second.id = QStringLiteral("page-new");
    second.frames = {frameForPage(20, second.id)};
    second.cutGuides = CutGuides::Print;
    second.borderlessOverride = BorderlessOverride::On;

    ApplyLayoutEdit edit;
    PageLayout change;
    change.pageIndex = 0;
    change.frames = laidOut;
    change.cutGuides = CutGuides::Screen;
    edit.pages.append(change);
    edit.newPages.append(second);
    edit.insertIndex = 1;
    edit.selectPage = 1;
    edit.actionText = QStringLiteral("Képek sokszorosítása");

    controller.apply(edit);
    QCOMPARE(controller.undoStack()->count(), 1);
    QCOMPARE(controller.undoStack()->undoText(), QStringLiteral("Képek sokszorosítása"));
    QCOMPARE(controller.pageCount(), 2);
    QCOMPARE(controller.document().currentPage, 1);
    QCOMPARE(controller.document().pages.at(0).frames, laidOut);
    QVERIFY(controller.document().pages.at(0).cutGuides == CutGuides::Screen);
    QCOMPARE(controller.document().pages.at(1).id, QStringLiteral("page-new"));
    QVERIFY(controller.document().pages.at(1).cutGuides == CutGuides::Print);
    QVERIFY(controller.document().pages.at(1).borderlessOverride
            == BorderlessOverride::On);

    const DocumentSnapshot after = controller.document();
    controller.undoStack()->undo();
    QCOMPARE(controller.document(), before);
    QCOMPARE(controller.document().currentPage, 0);
    QCOMPARE(controller.pageCount(), 1);

    controller.undoStack()->redo();
    QCOMPARE(controller.document(), after);
    QCOMPARE(controller.document().currentPage, 1);
}

void TestSheetLayout::applyLayoutEditNoOpGuard()
{
    DocumentSnapshot document;
    Page page = pageOfSize(210.0, 297.0);
    page.frames = {frameForPage(0, page.id)};
    document.pages.append(page);

    CanvasController controller;
    controller.setDocument(document);
    QCOMPARE(controller.undoStack()->count(), 0);

    ApplyLayoutEdit edit;
    PageLayout change;
    change.pageIndex = 0;
    change.frames = page.frames;
    edit.pages.append(change);
    controller.apply(edit);

    QCOMPARE(controller.undoStack()->count(), 0);
    QCOMPARE(controller.document().currentPage, 0);
    QCOMPARE(controller.document(), document);

    // Selecting a different page without changing the layout is navigation,
    // not an edit, so it adds no undo step either.
    edit.selectPage = 0;
    controller.apply(edit);
    QCOMPARE(controller.undoStack()->count(), 0);
}

void TestSheetLayout::applyLayoutEditCanChangeOnlyTheGuides()
{
    DocumentSnapshot document;
    Page page = pageOfSize(210.0, 297.0);
    page.frames = {frameForPage(0, page.id)};
    document.pages.append(page);

    CanvasController controller;
    controller.setDocument(document);
    const QList<Frame> framesBefore = controller.document().pages.first().frames;

    ApplyLayoutEdit edit;
    PageLayout change;
    change.pageIndex = 0;
    change.cutGuides = CutGuides::Print;
    edit.pages.append(change);
    controller.apply(edit);

    QCOMPARE(controller.undoStack()->count(), 1);
    QCOMPARE(controller.undoStack()->undoText(), QStringLiteral("Elrendezés"));
    QCOMPARE(controller.document().pages.first().frames, framesBefore);
    QVERIFY(controller.document().pages.first().cutGuides == CutGuides::Print);

    controller.undoStack()->undo();
    QVERIFY(controller.document().pages.first().cutGuides == CutGuides::None);
    QCOMPARE(controller.document().pages.first().frames, framesBefore);
}

QTEST_MAIN(TestSheetLayout)
#include "tst_sheet_layout.moc"
