#include "CanvasController.h"
#include "CollageTemplates.h"
#include "Document.h"
#include "ImageSource.h"
#include "LayoutEngine.h"
#include "PageRenderer.h"
#include "PaperCatalog.h"
#include "PaperGeometry.h"
#include "ProjectStore.h"
#include "SheetLayout.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"
#include "TemplateBrowserDialog.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRectF>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace pp;

namespace {

/// Tolerances: the mapping is exact arithmetic, so 1e-9 mm is generous for a
/// comparison and still catches a moved catalogue photograph.
bool near(double a, double b, double epsilon = 1e-9)
{
    return std::abs(a - b) <= epsilon;
}

bool nearRect(const QRectF &a, const QRectF &b, double epsilon = 1e-9)
{
    return near(a.x(), b.x(), epsilon) && near(a.y(), b.y(), epsilon)
        && near(a.width(), b.width(), epsilon) && near(a.height(), b.height(), epsilon);
}

PaperSpec paperNamed(const QString &name)
{
    for (const PaperSpec &paper : defaultPapers()) {
        if (paper.name == name)
            return paper;
    }
    return {};
}

PaperSpec a4Paper()
{
    return paperNamed(QStringLiteral("A4"));
}

PaperSpec photoPaper()
{
    return paperNamed(QStringLiteral("10 × 15 cm"));
}

PaperSpec a5Paper()
{
    return paperNamed(QStringLiteral("A5"));
}

Page pageWith(const PaperSpec &paper, bool landscape)
{
    Page page;
    page.id = QStringLiteral("page-test");
    page.paper = paper;
    page.landscape = landscape;
    return page;
}

Frame photoFrame(int index)
{
    Frame frame;
    frame.id = QStringLiteral("frame-%1").arg(index);
    frame.sourcePath = QStringLiteral("/photos/photo-%1.jpg").arg(index);
    frame.sourceSha256 = QByteArrayLiteral("sha-") + QByteArray::number(index);
    frame.cx = 40.0 + index * 20.0;
    frame.cy = 60.0 + index * 20.0;
    frame.width = 60.0;
    frame.height = 40.0;
    frame.fit = FitMode::Fill;
    frame.autoPlaced = false;
    return frame;
}

const CollageTemplate &templateNamed(const QString &id)
{
    const CollageTemplate *t = CollageTemplates::instance().find(id);
    Q_ASSERT(t);
    return *t;
}

/// A page carrying the template's unit rectangles and nothing else, so the
/// geometry module can be compared with the catalogue without any layout policy.
Page unitPage(const CollageTemplate &t, const PaperSpec &paper, bool landscape)
{
    Page page = pageWith(paper, landscape);
    for (int i = 0; i < t.slotList.size(); ++i) {
        Slot slot;
        slot.id = QStringLiteral("slot-%1").arg(i);
        slot.unitRect = t.slotList.at(i).rect;
        page.slotList.append(slot);
    }
    return page;
}

Page layoutPage(const CollageTemplate &t, const PaperSpec &paper, bool landscape, int photos)
{
    Page page = pageWith(paper, landscape);
    for (int i = 0; i < photos; ++i)
        page.frames.append(photoFrame(i));
    const auto applied = SlotLayoutEngine::applyTemplate(page, t);
    Q_ASSERT(applied.ok());
    return applied.value();
}

QString writePhoto(const QTemporaryDir &dir, const QString &name, const QColor &colour)
{
    const QString path = dir.filePath(name);
    QImage image(240, 160, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    QImageWriter writer(path, "png");
    if (!writer.write(image))
        return {};
    return path;
}

Frame frameFor(const QString &path, const QString &id)
{
    Frame frame;
    frame.id = id;
    frame.sourcePath = path;
    frame.sourceSha256 = fileSha256(path);
    frame.fit = FitMode::Fill;
    frame.cropCenter = QPointF(0.5, 0.5);
    frame.autoPlaced = false;
    return frame;
}

Frame sourceFrame(const QString &id, const QString &path)
{
    Frame frame;
    frame.id = id;
    frame.sourcePath = path;
    frame.sourceSha256 = path.isEmpty() ? QByteArray() : fileSha256(path);
    frame.fit = FitMode::Fill;
    frame.cropCenter = QPointF(0.5, 0.5);
    frame.autoPlaced = false;
    frame.cx = 40.0;
    frame.cy = 50.0;
    frame.width = 120.0;
    frame.height = 80.0;
    return frame;
}

const Frame *frameById(const Page &page, const QString &id)
{
    for (const Frame &frame : page.frames) {
        if (frame.id == id)
            return &frame;
    }
    return nullptr;
}

QRgb pixelAtMm(const QImage &page, double ppi, double xMm, double yMm)
{
    const int x = std::clamp(qRound(xMm / 25.4 * ppi), 0, page.width() - 1);
    const int y = std::clamp(qRound(yMm / 25.4 * ppi), 0, page.height() - 1);
    return page.pixel(x, y);
}

bool isWhite(QRgb pixel)
{
    return qRed(pixel) > 248 && qGreen(pixel) > 248 && qBlue(pixel) > 248;
}

bool isNear(QRgb pixel, int r, int g, int b, int tolerance)
{
    return std::abs(qRed(pixel) - r) <= tolerance && std::abs(qGreen(pixel) - g) <= tolerance
        && std::abs(qBlue(pixel) - b) <= tolerance;
}

bool isBluish(QRgb pixel)
{
    return qBlue(pixel) > 180 && qRed(pixel) < 90 && qGreen(pixel) < 90;
}

bool isReddish(QRgb pixel)
{
    return qRed(pixel) > 180 && qGreen(pixel) < 90 && qBlue(pixel) < 90;
}

QJsonObject minimalFrameJson(const QString &id)
{
    QJsonObject frame;
    frame.insert(QStringLiteral("id"), id);
    frame.insert(QStringLiteral("cx"), 50.0);
    frame.insert(QStringLiteral("cy"), 70.0);
    frame.insert(QStringLiteral("width"), 40.0);
    frame.insert(QStringLiteral("height"), 30.0);
    frame.insert(QStringLiteral("sha256"), QString());
    return frame;
}

QJsonObject minimalPageJson(const QString &id, const QJsonArray &frames)
{
    QJsonObject paper;
    paper.insert(QStringLiteral("name"), QStringLiteral("A4"));
    paper.insert(QStringLiteral("physicalWidthMm"), 210.0);
    paper.insert(QStringLiteral("physicalHeightMm"), 297.0);
    paper.insert(QStringLiteral("logicalWidthMm"), 0.0);
    paper.insert(QStringLiteral("logicalHeightMm"), 0.0);
    paper.insert(QStringLiteral("borderlessSupported"), false);
    paper.insert(QStringLiteral("hasCheckedOffset"), false);
    paper.insert(QStringLiteral("offsetXMm"), 0.0);
    paper.insert(QStringLiteral("offsetYMm"), 0.0);
    paper.insert(QStringLiteral("type"), QStringLiteral("plain"));
    paper.insert(QStringLiteral("custom"), false);
    QJsonObject page;
    page.insert(QStringLiteral("id"), id);
    page.insert(QStringLiteral("paper"), paper);
    page.insert(QStringLiteral("landscape"), false);
    page.insert(QStringLiteral("frames"), frames);
    return page;
}

QJsonObject minimalProjectJson(const QJsonObject &page)
{
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("photoprint-project"));
    root.insert(QStringLiteral("version"), ProjectStore::kFormatVersion);
    root.insert(QStringLiteral("recipe"), QJsonObject{});
    root.insert(QStringLiteral("currentPage"), 0);
    root.insert(QStringLiteral("fineQuality"), true);
    root.insert(QStringLiteral("pages"), QJsonArray{page});
    return root;
}

QByteArray encoded(const QJsonObject &root)
{
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QJsonObject slotJson(const QString &id, const QJsonArray &unit)
{
    return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("unit"), unit}};
}

/// A two-cell layout carrying a saved style, loaded through the project parser
/// so the mat migration runs exactly as it does when a file is opened.
Result<DocumentSnapshot> layoutWithSavedStyle(const QJsonObject &styleObject)
{
    QJsonObject layout;
    layout.insert(QStringLiteral("slots"),
                  QJsonArray{slotJson(QStringLiteral("slot-a"),
                                      QJsonArray{0.0, 0.0, 0.5, 1.0}),
                             slotJson(QStringLiteral("slot-b"),
                                      QJsonArray{0.5, 0.0, 0.5, 1.0})});
    layout.insert(QStringLiteral("style"), styleObject);
    QJsonObject page = minimalPageJson(QStringLiteral("page-a"), QJsonArray{});
    page.insert(QStringLiteral("layout"), layout);
    return ProjectStore::deserialize(encoded(minimalProjectJson(page)));
}

} // namespace

class TestSlotLayout : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void mappingMatchesTheCatalogue();
    void explicitMarginStillMatchesTheCatalogue();
    void marginAndGutterFollowTheStyle();
    void remapOnPaperAndOrientation();

    void styleAndGeometryClamps();
    void hugeMarginOnTenByFifteen();

    void applyToEmptyPage();
    void fewerPhotosThanSlots();
    void morePhotosThanSlots();
    void applyingTwiceChangesNothing();
    void applyCropAndRotationPolicy();

    void addSlotSplitsTheLongerAxis();
    void addSlotStopsAtThePickerFloor();
    void slotCountIsCapped();
    void removeSlotTakesItsPhoto();
    void dissolveKeepsFrames();

    void repairClearsDanglingAndDuplicateIds();
    void repairAfterEveryControllerMutation();
    void invariantRejectsBrokenPages();

    void persistenceRoundTrip();
    void layoutFreeProjectStaysByteIdentical();
    void oldProjectsLoadWithoutALayout();
    void legacyMatMigratesToGap();
    void loaderSanitisesBrokenLayouts();

    void oneEditPerAction();
    void validatorRefusesBrokenSlotEdits();

    void emptySlotPrintsPaper();
    void photoFillsItsCellWithoutAMat();
    void gapIsTheWhitePaperBetweenPhotos();
    void gapChangesSpacingNotThePhotoInset();
    void roundedCornersClipThePhoto();

    void placePhotoOnEmptySlot();
    void placePhotoReplacesOccupant();
    void placementKeepsOrResetsTheCrop();
    void movePhotoToAnotherSlot();
    void swapPhotosExchangesPayloads();
    void emptySlotRemovesThePhoto();
    void placePhotosMultiDrop();
    void placementErrors();

    void topUpKeepsExistingPhotos();
    void fillEmptySlotsWithoutATemplate();

    void publishPageStyleOnly();
    void publishPageFramesAndSlots();
    void staleGeometryDoesNotPushAnUndoStep();
    void validatorErrorCodes();

    void zeroMarginIsNotAutomatic();
    void displayOrderPutsSlotsFirst();
    void pickerPreviewMapsPhotosToSlots();

    void repairSanitisesTheStyle();
    void removeLastSlotClearsDanglingIds();
    void layoutPageWithoutFramesPrintsPaper();
    void layoutFreeSaveBytesAreStable();
    void nanUnitRectIsDropped();
    void twelveSlotsRoundTrip();
    void slotAtAndFirstEmptySlot();
};

void TestSlotLayout::initTestCase()
{
    QVERIFY(!CollageTemplates::instance().isEmpty());
    QVERIFY2(CollageTemplates::loadError().isEmpty(),
             qPrintable(CollageTemplates::loadError()));
    QCOMPARE(defaultPapers().size(), 6);
}

void TestSlotLayout::mappingMatchesTheCatalogue()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    int mapped = 0;
    for (const PaperSpec &paper : defaultPapers()) {
        for (bool landscape : {false, true}) {
            const QSizeF sheet = pageSizeMm(pageWith(paper, landscape));
            QVERIFY(near(SlotGeometry::automaticMarginMm(sheet), collageMarginMm(sheet), 1e-12));
            for (const CollageTemplate &t : catalogue.catalogue()) {
                const auto expected = catalogue.slotsMm(t, paper, landscape);
                if (!expected.ok())
                    QFAIL(qPrintable(expected.error().message));
                const Page page = unitPage(t, paper, landscape);
                const QList<QRectF> actual = SlotGeometry::cellsMm(page);
                QCOMPARE(actual.size(), expected.value().size());
                for (int i = 0; i < actual.size(); ++i) {
                    const QRectF &want = expected.value().at(i);
                    const QRectF &got = actual.at(i);
                    QVERIFY2(near(got.x(), want.x()) && near(got.y(), want.y())
                                 && near(got.width(), want.width())
                                 && near(got.height(), want.height()),
                             qPrintable(QStringLiteral("%1 on %2: slot %3 moved")
                                            .arg(t.id, paper.name)
                                            .arg(i)));
                }
                QVERIFY(SlotGeometry::layoutConsistent(page));
                ++mapped;
            }
        }
    }
    QCOMPARE(mapped, 55 * 6 * 2);
}

void TestSlotLayout::explicitMarginStillMatchesTheCatalogue()
{
    // The catalogue's pinned geometry is the margin == gutter special case of
    // the general mapper. Writing the automatic value explicitly must not move
    // it by a bit, on any paper or orientation.
    const CollageTemplates &catalogue = CollageTemplates::instance();
    for (const PaperSpec &paper : defaultPapers()) {
        for (bool landscape : {false, true}) {
            const QSizeF sheet = pageSizeMm(pageWith(paper, landscape));
            const double margin = collageMarginMm(sheet);
            for (const CollageTemplate &t : catalogue.catalogue()) {
                const auto expected = catalogue.slotsMm(t, paper, landscape);
                QVERIFY(expected.ok());
                Page page = unitPage(t, paper, landscape);
                page.slotStyle.marginMm = margin;
                page.slotStyle.gutterMm = margin;
                const QList<QRectF> actual = SlotGeometry::cellsMm(page);
                QCOMPARE(actual.size(), expected.value().size());
                for (int i = 0; i < actual.size(); ++i)
                    QVERIFY(nearRect(actual.at(i), expected.value().at(i)));
            }
        }
    }
}

void TestSlotLayout::marginAndGutterFollowTheStyle()
{
    // An outer slot sits `margin` from the paper edge, neighbours are `gutter`
    // apart. On A4: content box 196 x 283 starting at 7, each half gives up 3.
    Page page = unitPage(templateNamed(QStringLiteral("n02-equal-side")), a4Paper(), false);
    page.slotStyle.marginMm = 10.0;
    page.slotStyle.gutterMm = 6.0;
    const QList<QRectF> cells = SlotGeometry::cellsMm(page);
    QCOMPARE(cells.size(), 2);
    QVERIFY(nearRect(cells.at(0), QRectF(10.0, 10.0, 92.0, 277.0)));
    QVERIFY(nearRect(cells.at(1), QRectF(108.0, 10.0, 92.0, 277.0)));
    QVERIFY(near(cells.at(0).left(), 10.0));
    QVERIFY(near(cells.at(1).left() - cells.at(0).right(), 6.0));
    QVERIFY(near(210.0 - cells.at(1).right(), 10.0));
    QVERIFY(near(cells.at(0).top(), 10.0));
    QVERIFY(near(297.0 - cells.at(0).bottom(), 10.0));
}

void TestSlotLayout::remapOnPaperAndOrientation()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 0);
    QVERIFY(page.hasLayout());
    QCOMPARE(page.slotList.size(), 4);
    const QList<QRectF> units = {page.slotList.at(0).unitRect, page.slotList.at(1).unitRect,
                                 page.slotList.at(2).unitRect, page.slotList.at(3).unitRect};
    const QList<QRectF> a4Cells = SlotGeometry::cellsMm(page);

    page.paper = photoPaper();
    auto repaired = SlotLayoutEngine::repair(page);
    QVERIFY(repaired.ok());
    page = repaired.value();
    QVERIFY(SlotGeometry::layoutConsistent(page));
    const QList<QRectF> photoCells = SlotGeometry::cellsMm(page);
    for (int i = 0; i < units.size(); ++i) {
        QVERIFY(nearRect(page.slotList.at(i).unitRect, units.at(i)));
        QVERIFY(!nearRect(photoCells.at(i), a4Cells.at(i)));
        QVERIFY(photoCells.at(i).left() >= -1e-9);
        QVERIFY(photoCells.at(i).top() >= -1e-9);
        QVERIFY(photoCells.at(i).right() <= 100.0 + 1e-9);
        QVERIFY(photoCells.at(i).bottom() <= 150.0 + 1e-9);
    }

    page.landscape = true;
    repaired = SlotLayoutEngine::repair(page);
    QVERIFY(repaired.ok());
    page = repaired.value();
    QVERIFY(SlotGeometry::layoutConsistent(page));
    const QList<QRectF> turned = SlotGeometry::cellsMm(page);
    for (int i = 0; i < units.size(); ++i) {
        QVERIFY(nearRect(page.slotList.at(i).unitRect, units.at(i)));
        QVERIFY(turned.at(i).right() <= 150.0 + 1e-9);
        QVERIFY(turned.at(i).bottom() <= 100.0 + 1e-9);
    }

    // A sheet that cannot hold the catalogue margin still maps: cells clamp to
    // the model floor instead of going negative or being refused.
    Page tiny = pageWith(PaperSpec{QStringLiteral("30 × 30"), 30.0, 30.0}, false);
    Slot slot;
    slot.id = QStringLiteral("slot-only");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    tiny.slotList.append(slot);
    QVERIFY(SlotGeometry::layoutConsistent(tiny));
    const QRectF tinyCell = SlotGeometry::slotCellMm(tiny, slot);
    QVERIFY(tinyCell.width() >= 1.0 - 1e-9);
    QVERIFY(tinyCell.height() >= 1.0 - 1e-9);
    QVERIFY(tinyCell.left() >= 0.0);
    QVERIFY(tinyCell.top() >= 0.0);
    QVERIFY(tinyCell.right() <= 30.0 + 1e-9);
    QVERIFY(tinyCell.bottom() <= 30.0 + 1e-9);
}

void TestSlotLayout::styleAndGeometryClamps()
{
    SlotStyle style;
    style.marginMm = 500.0;
    style.gutterMm = 500.0;
    style.cornerRadiusMm = 500.0;
    const SlotStyle sane = SlotGeometry::sanitiseStyle(style);
    QCOMPARE(sane.marginMm.value_or(-1.0), SlotGeometry::kMaxMarginMm);
    QCOMPARE(sane.gutterMm.value_or(-1.0), SlotGeometry::kMaxGutterMm);
    QCOMPARE(sane.cornerRadiusMm, SlotGeometry::kMaxCornerRadiusMm);

    SlotStyle broken;
    broken.marginMm = std::numeric_limits<double>::quiet_NaN();
    broken.gutterMm = std::numeric_limits<double>::infinity();
    broken.cornerRadiusMm = std::numeric_limits<double>::quiet_NaN();
    const SlotStyle defaults = SlotGeometry::sanitiseStyle(broken);
    QVERIFY(!defaults.marginMm.has_value());
    QVERIFY(!defaults.gutterMm.has_value());
    QCOMPARE(defaults.cornerRadiusMm, 0.0);

    // A radius is clamped by the cell, not only by its own maximum.
    SlotStyle radius;
    radius.cornerRadiusMm = 100.0;
    QCOMPARE(SlotGeometry::cornerRadiusMm(radius, QRectF(0.0, 0.0, 40.0, 60.0)), 20.0);
    radius.cornerRadiusMm = 12.0;
    QCOMPARE(SlotGeometry::cornerRadiusMm(radius, QRectF(0.0, 0.0, 40.0, 60.0)), 12.0);

    // A gutter wider than the margin pushes the raw box outside the sheet; the
    // mapper clamps it instead of producing a negative cell.
    const QRectF wide = SlotGeometry::cellFromUnitRect(QRectF(0.0, 0.0, 1.0, 1.0),
                                                       QSizeF(100.0, 150.0), 2.0, 50.0);
    QVERIFY(wide.left() >= 0.0);
    QVERIFY(wide.top() >= 0.0);
    QVERIFY(wide.width() > 0.0 && wide.height() > 0.0);
    QVERIFY(wide.right() <= 100.0 + 1e-9);
    QVERIFY(wide.bottom() <= 150.0 + 1e-9);
}

void TestSlotLayout::hugeMarginOnTenByFifteen()
{
    // 10 x 15 is the small sheet the automatic margin is tuned for. A 50 mm
    // margin is clamped to the paper-aware 48 mm, leaving a 4 mm wide content
    // box; the model still describes it without a negative or empty rectangle.
    Page page = pageWith(photoPaper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-only");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);
    page.slotStyle.marginMm = 500.0;
    page.slotStyle.gutterMm = 500.0;
    page.slotStyle.cornerRadiusMm = 500.0;

    const QRectF cell = SlotGeometry::slotCellMm(page, slot);
    QVERIFY(nearRect(cell, QRectF(48.0, 48.0, 4.0, 54.0)));
    // The photograph is the cell: no mat eats into it.
    QVERIFY(nearRect(SlotGeometry::slotPhotoRectMm(page, slot), cell));
    QVERIFY(SlotGeometry::layoutConsistent(page));
    QCOMPARE(SlotGeometry::cornerRadiusMm(page.slotStyle, cell), 2.0);
}

void TestSlotLayout::applyToEmptyPage()
{
    const CollageTemplate &t = templateNamed(QStringLiteral("n06-grid-3x2"));
    const auto applied = SlotLayoutEngine::applyTemplate(pageWith(a4Paper(), false), t);
    QVERIFY(applied.ok());
    const Page &page = applied.value();
    QVERIFY(page.hasLayout());
    QCOMPARE(page.slotList.size(), 6);
    QVERIFY(page.frames.isEmpty());
    QCOMPARE(page.layoutId, t.id);
    QVERIFY(SlotGeometry::layoutConsistent(page));
    for (const Slot &slot : page.slotList) {
        QVERIFY(!slot.id.isEmpty());
        QVERIFY(!SlotGeometry::frameInSlot(page, slot.id));
        QVERIFY(SlotGeometry::slotPhotoRectMm(page, slot).width() > 0.0);
    }
}

void TestSlotLayout::fewerPhotosThanSlots()
{
    const CollageTemplate &t = templateNamed(QStringLiteral("n04-grid-2x2"));
    Page page = pageWith(a4Paper(), false);
    page.frames.append(photoFrame(0));
    page.frames.append(photoFrame(1));

    SlotLayoutEngine::Report report;
    const auto applied = SlotLayoutEngine::applyTemplate(page, t, &report);
    QVERIFY(applied.ok());
    const Page &result = applied.value();
    QCOMPARE(result.frames.size(), 2);
    QCOMPARE(result.slotList.size(), 4);
    QCOMPARE(report.emptySlotsAfter, 2);
    QVERIFY(report.leftFreeIds.isEmpty());
    QCOMPARE(SlotGeometry::frameInSlot(result, result.slotList.at(0).id)->id,
             QStringLiteral("frame-0"));
    QCOMPARE(SlotGeometry::frameInSlot(result, result.slotList.at(1).id)->id,
             QStringLiteral("frame-1"));
    QVERIFY(!SlotGeometry::frameInSlot(result, result.slotList.at(2).id));
    QVERIFY(!SlotGeometry::frameInSlot(result, result.slotList.at(3).id));
    for (const Frame &frame : result.frames) {
        QVERIFY(!frame.slotId.isEmpty());
        QVERIFY(SlotGeometry::slotGeometryMatches(result, frame));
        QCOMPARE(frame.angleDeg, 0.0);
        QVERIFY(frame.fit == FitMode::Fill);
        QVERIFY(!frame.autoPlaced);
    }
    QVERIFY(SlotGeometry::layoutConsistent(result));
}

void TestSlotLayout::morePhotosThanSlots()
{
    const CollageTemplate &t = templateNamed(QStringLiteral("n04-grid-2x2"));
    Page page = pageWith(a4Paper(), false);
    for (int i = 0; i < 6; ++i)
        page.frames.append(photoFrame(i));

    SlotLayoutEngine::Report report;
    const auto applied = SlotLayoutEngine::applyTemplate(page, t, &report);
    QVERIFY(applied.ok());
    const Page &result = applied.value();
    QCOMPARE(result.frames.size(), 6);
    QCOMPARE(result.slotList.size(), 4);
    QCOMPARE(report.leftFreeIds.size(), 2);
    QCOMPARE(report.leftFreeIds.at(0), QStringLiteral("frame-4"));
    QCOMPARE(report.leftFreeIds.at(1), QStringLiteral("frame-5"));
    QCOMPARE(report.emptySlotsAfter, 0);
    QVERIFY(result.frames.at(4).slotId.isEmpty());
    QVERIFY(result.frames.at(5).slotId.isEmpty());
    QVERIFY(SlotGeometry::layoutConsistent(result));
}

void TestSlotLayout::applyingTwiceChangesNothing()
{
    const CollageTemplate &t = templateNamed(QStringLiteral("n05-three-two"));
    Page page = pageWith(photoPaper(), false);
    for (int i = 0; i < 3; ++i)
        page.frames.append(photoFrame(i));
    const auto first = SlotLayoutEngine::applyTemplate(page, t);
    QVERIFY(first.ok());

    const auto second = SlotLayoutEngine::applyTemplate(first.value(), t);
    QVERIFY(second.ok());
    QVERIFY(second.value() == first.value());
}

void TestSlotLayout::applyCropAndRotationPolicy()
{
    const CollageTemplate &t = templateNamed(QStringLiteral("n02-equal-side"));
    Page page = pageWith(a4Paper(), false);
    // Match the slot aspect so the crop is kept; a rotated frame is reported.
    const QList<QRectF> cells = SlotGeometry::cellsMm(unitPage(t, a4Paper(), false));
    for (int i = 0; i < 2; ++i) {
        Frame frame = photoFrame(i);
        frame.width = cells.at(i).width();
        frame.height = cells.at(i).height();
        frame.cropCenter = QPointF(0.3, 0.4);
        frame.zoom = 1.25;
        frame.angleDeg = 12.0;
        page.frames.append(frame);
    }
    SlotLayoutEngine::Report kept;
    const auto applied = SlotLayoutEngine::applyTemplate(page, t, &kept);
    QVERIFY(applied.ok());
    QCOMPARE(kept.rotationResetIds.size(), 2);
    QVERIFY(kept.cropResetIds.isEmpty());
    for (const Frame &frame : applied.value().frames) {
        QVERIFY(near(frame.cropCenter.x(), 0.3));
        QVERIFY(near(frame.cropCenter.y(), 0.4));
        QVERIFY(near(frame.zoom, 1.25));
    }

    // A slot far from the previous aspect resets the crop and zoom.
    Page reset = pageWith(a4Paper(), false);
    for (int i = 0; i < 2; ++i) {
        Frame frame = photoFrame(i); // 3:2, while the A4 half is tall
        frame.cropCenter = QPointF(0.2, 0.8);
        frame.zoom = 4.0;
        reset.frames.append(frame);
    }
    SlotLayoutEngine::Report report;
    const auto resetResult = SlotLayoutEngine::applyTemplate(reset, t, &report);
    QVERIFY(resetResult.ok());
    QCOMPARE(report.cropResetIds.size(), 2);
    QVERIFY(report.rotationResetIds.isEmpty());
    for (const Frame &frame : resetResult.value().frames) {
        QVERIFY(near(frame.cropCenter.x(), 0.5));
        QVERIFY(near(frame.cropCenter.y(), 0.5));
        QCOMPARE(frame.zoom, 1.0);
    }
}

void TestSlotLayout::addSlotSplitsTheLongerAxis()
{
    const CollageTemplate &t = templateNamed(QStringLiteral("n02-equal-side"));
    Page page = layoutPage(t, a4Paper(), false, 0);
    QCOMPARE(page.slotList.size(), 2);

    const auto added = SlotLayoutEngine::addSlot(page, page.slotList.at(0).id);
    QVERIFY(added.ok());
    const Page &result = added.value();
    QCOMPARE(result.slotList.size(), 3);
    // The A4 half is 100 x 287, so the longer axis is the height: the slot is
    // cut into a top and a bottom half, in unit space.
    QVERIFY(nearRect(result.slotList.at(0).unitRect, QRectF(0.0, 0.0, 0.5, 0.5)));
    QVERIFY(nearRect(result.slotList.at(1).unitRect, QRectF(0.0, 0.5, 0.5, 0.5)));
    QVERIFY(nearRect(result.slotList.at(2).unitRect, t.slotList.at(1).rect));
    for (const Slot &slot : result.slotList) {
        const QRectF cell = SlotGeometry::slotCellMm(result, slot);
        QVERIFY(cell.width() >= SlotLayoutEngine::kMinPickerSlotMm - 1e-9);
        QVERIFY(cell.height() >= SlotLayoutEngine::kMinPickerSlotMm - 1e-9);
    }
    QVERIFY(SlotGeometry::layoutConsistent(result));
}

void TestSlotLayout::addSlotStopsAtThePickerFloor()
{
    // A 60 x 60 sheet with the automatic 4 mm margin leaves a 52 x 52 cell.
    // Each split gives up a gutter, so 52 -> 24 x 52 -> 24 x 24, and the next
    // split would be 10 mm: refused rather than producing an unreadable cell.
    Page page = pageWith(PaperSpec{QStringLiteral("60 × 60"), 60.0, 60.0}, false);
    Slot slot;
    slot.id = QStringLiteral("slot-only");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);

    for (int round = 0; round < 3; ++round) {
        const auto added = SlotLayoutEngine::addSlot(page, QString());
        QVERIFY(added.ok());
        page = added.value();
        QCOMPARE(page.slotList.size(), round + 2);
        QVERIFY(SlotGeometry::layoutConsistent(page));
    }
    QCOMPARE(page.slotList.size(), 4);

    const auto refused = SlotLayoutEngine::addSlot(page, QString());
    QVERIFY(!refused.ok());
    QCOMPARE(refused.error().code, QStringLiteral("slot-too-small"));
    QVERIFY(!refused.error().message.isEmpty());
    QCOMPARE(page.slotList.size(), 4); // the input was not touched
}

void TestSlotLayout::slotCountIsCapped()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 0);
    while (page.slotList.size() < SlotGeometry::kMaxSlotsPerPage) {
        const auto added = SlotLayoutEngine::addSlot(page, QString());
        if (!added.ok())
            QFAIL(qPrintable(added.error().message));
        page = added.value();
    }
    QCOMPARE(page.slotList.size(), 12);
    QVERIFY(SlotGeometry::layoutConsistent(page));

    const auto refused = SlotLayoutEngine::addSlot(page, QString());
    QVERIFY(!refused.ok());
    QCOMPARE(refused.error().code, QStringLiteral("slot-cap"));
    QVERIFY(refused.error().message.contains(QStringLiteral("12")));
}

void TestSlotLayout::removeSlotTakesItsPhoto()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 2);
    QCOMPARE(page.slotList.size(), 4);
    QCOMPARE(page.frames.size(), 2);
    const QString filledId = page.slotList.at(0).id;
    const QString survivorId = page.slotList.at(1).id;

    auto removed = SlotLayoutEngine::removeSlot(page, filledId);
    QVERIFY(removed.ok());
    page = removed.value();
    QCOMPARE(page.slotList.size(), 3);
    QCOMPARE(page.frames.size(), 1);
    QCOMPARE(page.frames.at(0).slotId, survivorId);
    QVERIFY(SlotGeometry::slotGeometryMatches(page, page.frames.at(0)));
    QCOMPARE(SlotGeometry::firstEmptySlot(page) != nullptr, true);

    const auto missing = SlotLayoutEngine::removeSlot(page, QStringLiteral("slot-missing"));
    QVERIFY(!missing.ok());
    QCOMPARE(missing.error().code, QStringLiteral("slot-missing"));

    while (!page.slotList.isEmpty()) {
        const auto step = SlotLayoutEngine::removeSlot(page, page.slotList.first().id);
        if (!step.ok())
            QFAIL(qPrintable(step.error().message));
        page = step.value();
    }
    QVERIFY(!page.hasLayout());
    QCOMPARE(page.slotStyle, SlotStyle{});
    QVERIFY(page.layoutId.isEmpty());
    QVERIFY(page.frames.isEmpty());
}

void TestSlotLayout::dissolveKeepsFrames()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n05-three-two")), photoPaper(), false, 3);
    SlotStyle style;
    style.gutterMm = 3.0;
    style.cornerRadiusMm = 2.0;
    auto styled = SlotLayoutEngine::setStyle(page, style);
    QVERIFY(styled.ok());
    page = styled.value();
    QVERIFY(page.hasLayout());
    const QList<Frame> before = page.frames;

    const Page dissolved = SlotLayoutEngine::dissolve(page);
    QVERIFY(!dissolved.hasLayout());
    QVERIFY(dissolved.layoutId.isEmpty());
    QCOMPARE(dissolved.slotStyle, SlotStyle{});
    QCOMPARE(dissolved.frames.size(), before.size());
    for (int i = 0; i < before.size(); ++i) {
        QVERIFY(dissolved.frames.at(i).slotId.isEmpty());
        QCOMPARE(dissolved.frames.at(i).id, before.at(i).id);
        QVERIFY(near(dissolved.frames.at(i).cx, before.at(i).cx));
        QVERIFY(near(dissolved.frames.at(i).cy, before.at(i).cy));
        QVERIFY(near(dissolved.frames.at(i).width, before.at(i).width));
        QVERIFY(near(dissolved.frames.at(i).height, before.at(i).height));
    }
    QVERIFY(SlotGeometry::layoutConsistent(dissolved));
}

void TestSlotLayout::repairClearsDanglingAndDuplicateIds()
{
    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-a");
    slot.unitRect = QRectF(0.0, 0.0, 0.5, 1.0);
    page.slotList.append(slot);

    Frame dangling = photoFrame(0);
    dangling.slotId = QStringLiteral("slot-missing");
    const QPointF danglingCentre(dangling.cx, dangling.cy);
    page.frames.append(dangling);

    Frame first = photoFrame(1);
    first.slotId = slot.id;
    first.cx = 1.0;
    first.width = 5.0;
    page.frames.append(first);

    Frame second = photoFrame(2);
    second.slotId = slot.id;
    page.frames.append(second);

    QVERIFY(!SlotGeometry::layoutConsistent(page));
    const auto repaired = SlotLayoutEngine::repair(page);
    QVERIFY(repaired.ok());
    const Page &result = repaired.value();
    QVERIFY(SlotGeometry::layoutConsistent(result));
    QVERIFY(result.frames.at(0).slotId.isEmpty());
    QVERIFY(near(result.frames.at(0).cx, danglingCentre.x())); // geometry kept
    QCOMPARE(result.frames.at(1).slotId, slot.id);
    QVERIFY(SlotGeometry::slotGeometryMatches(result, result.frames.at(1)));
    QVERIFY(result.frames.at(2).slotId.isEmpty());

    // The controller's loader path clears a dangling id as well.
    DocumentSnapshot document;
    document.pages.append(page);
    CanvasController controller;
    controller.setDocument(document);
    QVERIFY(controller.document().pages.at(0).frames.at(0).slotId.isEmpty());
    QVERIFY(SlotGeometry::layoutConsistent(controller.document().pages.at(0)));
}

void TestSlotLayout::repairAfterEveryControllerMutation()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 3);
    Frame free = photoFrame(9);
    free.slotId.clear();
    free.cx = 180.0;
    free.cy = 260.0;
    free.width = 20.0;
    free.height = 15.0;
    page.frames.append(free);

    DocumentSnapshot document;
    document.pages.append(page);
    CanvasController controller;
    controller.setDocument(document);
    const DocumentSnapshot initial = controller.document();

    auto pageNow = [&controller]() -> const Page & {
        return controller.document().pages.at(0);
    };
    auto slotFrame = [&pageNow]() -> const Frame & { return pageNow().frames.at(0); };
    auto matches = [&pageNow]() {
        for (const Frame &frame : pageNow().frames) {
            if (frame.slotId.isEmpty())
                continue;
            if (!SlotGeometry::slotGeometryMatches(pageNow(), frame))
                return false;
        }
        return true;
    };

    // A frame change that tries to move a slotted frame is corrected.
    Frame moved = slotFrame();
    moved.cx += 10.0;
    moved.angleDeg = 33.0;
    controller.apply(FrameChange{moved.id, moved});
    QVERIFY(SlotGeometry::layoutConsistent(pageNow()));
    QVERIFY(matches());
    QCOMPARE(slotFrame().angleDeg, 0.0);

    // SetFramesEdit with drifted geometry.
    QList<Frame> drift = pageNow().frames;
    drift[0].width += 5.0;
    drift[1].height += 7.0;
    controller.apply(SetFramesEdit{0, drift});
    QVERIFY(SlotGeometry::layoutConsistent(pageNow()));
    QVERIFY(matches());

    // Align writes new geometry into every frame and keeps the slot ids; repair
    // has to win over it, or a slot could be moved by the align tool.
    SheetLayout::AlignSpec align;
    align.marginMm = 10.0;
    align.gutterMm = 10.0;
    const auto aligned = SheetLayout::alignFrames(pageNow(), align);
    QVERIFY(aligned.ok());
    controller.apply(SetFramesEdit{0, aligned.value()});
    QVERIFY(SlotGeometry::layoutConsistent(pageNow()));
    QVERIFY(matches());

    // The first-use grid is a no-op on a laid-out page in practice, but a
    // hand-built autoPlaced frame list must still end up consistent.
    QList<Frame> autoFrames = pageNow().frames;
    for (Frame &frame : autoFrames)
        frame.autoPlaced = true;
    const QList<Frame> regridded = LayoutEngine::arrangeGrid(pageNow(), autoFrames, {});
    controller.apply(SetFramesEdit{0, regridded});
    QVERIFY(SlotGeometry::layoutConsistent(pageNow()));
    QVERIFY(matches());

    // A paper change remaps from the unit rectangles, it does not scale frames.
    const QList<QRectF> units = {pageNow().slotList.at(0).unitRect,
                                 pageNow().slotList.at(3).unitRect};
    controller.apply(SetPaperEdit{0, a5Paper()});
    QCOMPARE(pageNow().paper, a5Paper());
    QVERIFY(SlotGeometry::layoutConsistent(pageNow()));
    QVERIFY(matches());
    QVERIFY(nearRect(pageNow().slotList.at(0).unitRect, units.at(0)));
    QVERIFY(nearRect(pageNow().slotList.at(3).unitRect, units.at(1)));

    // Orientation: free frames turn, slotted frames stay axis-aligned.
    controller.apply(RotatePageEdit{0});
    QVERIFY(pageNow().landscape);
    QVERIFY(SlotGeometry::layoutConsistent(pageNow()));
    QVERIFY(matches());
    for (const Frame &frame : pageNow().frames) {
        if (!frame.slotId.isEmpty())
            QCOMPARE(frame.angleDeg, 0.0);
    }

    // Undo everything: each intermediate state must satisfy the invariant, and
    // the fully undone document must be the one before the first edit.
    while (controller.canUndo()) {
        controller.undoStack()->undo();
        QVERIFY(SlotGeometry::layoutConsistent(controller.document().pages.at(0)));
    }
    QVERIFY(controller.document() == initial);
    QCOMPARE(controller.undoStack()->index(), 0);
}

void TestSlotLayout::invariantRejectsBrokenPages()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n02-equal-side")), a4Paper(), false, 2);
    QVERIFY(SlotGeometry::layoutConsistent(page));
    for (const Frame &frame : page.frames) {
        QVERIFY(SlotGeometry::slotGeometryMatches(page, frame));
        QVERIFY(frame.angleDeg == 0.0 || frame.slotId.isEmpty());
    }

    Page dangling = page;
    dangling.frames[0].slotId = QStringLiteral("nope");
    QVERIFY(!SlotGeometry::layoutConsistent(dangling));

    Page duplicate = page;
    duplicate.frames[1].slotId = duplicate.frames[0].slotId;
    QVERIFY(!SlotGeometry::layoutConsistent(duplicate));

    Page drifted = page;
    drifted.frames[0].cx += 5.0;
    QVERIFY(!SlotGeometry::layoutConsistent(drifted));

    Page rotated = page;
    rotated.frames[0].angleDeg = 5.0;
    QVERIFY(!SlotGeometry::layoutConsistent(rotated));

    Page outside = page;
    outside.slotList[0].unitRect = QRectF(-0.2, 0.0, 0.6, 1.0);
    QVERIFY(!SlotGeometry::layoutConsistent(outside));

    Page many = page;
    while (many.slotList.size() <= SlotGeometry::kMaxSlotsPerPage) {
        Slot extra;
        extra.id = QStringLiteral("extra-%1").arg(many.slotList.size());
        extra.unitRect = QRectF(0.9, 0.9, 0.05, 0.05);
        many.slotList.append(extra);
    }
    QVERIFY(!SlotGeometry::layoutConsistent(many));
}

void TestSlotLayout::persistenceRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = writePhoto(dir, QStringLiteral("first.png"), QColor(120, 120, 120));
    const QString second = writePhoto(dir, QStringLiteral("second.png"), QColor(40, 90, 160));
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    DocumentSnapshot document;
    Page laidOut = pageWith(a4Paper(), false);
    laidOut.frames.append(frameFor(first, QStringLiteral("frame-first")));
    laidOut.frames.append(frameFor(second, QStringLiteral("frame-second")));
    auto applied = SlotLayoutEngine::applyTemplate(
        laidOut, templateNamed(QStringLiteral("n04-grid-2x2")));
    QVERIFY(applied.ok());
    laidOut = applied.value();
    SlotStyle style;
    style.marginMm = 8.0;
    style.gutterMm = 4.0;
    style.cornerRadiusMm = 2.0;
    auto styled = SlotLayoutEngine::setStyle(laidOut, style);
    QVERIFY(styled.ok());
    laidOut = styled.value();
    document.pages.append(laidOut);

    Page free = pageWith(photoPaper(), true);
    free.frames.append(frameFor(second, QStringLiteral("frame-free")));
    document.pages.append(free);
    document.currentPage = 1;

    const QString path = dir.filePath(QStringLiteral("project.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, document, SaveMode::Normal);
    if (!saved.ok())
        QFAIL(qPrintable(saved.error().message));
    const auto loaded = store.load(path);
    if (!loaded.ok())
        QFAIL(qPrintable(loaded.error().message));
    const DocumentSnapshot restored = loaded.value().document;

    QCOMPARE(restored.pages.size(), 2);
    const Page &restoredLaidOut = restored.pages.at(0);
    QVERIFY(restoredLaidOut.hasLayout());
    QCOMPARE(restoredLaidOut.slotList.size(), 4);
    QCOMPARE(restoredLaidOut.layoutId, QStringLiteral("n04-grid-2x2"));
    QCOMPARE(restoredLaidOut.slotStyle.marginMm.value_or(-1.0), 8.0);
    QCOMPARE(restoredLaidOut.slotStyle.gutterMm.value_or(-1.0), 4.0);
    QCOMPARE(restoredLaidOut.slotStyle.cornerRadiusMm, 2.0);

    // The saved style carries the gap and the radius, never a mat.
    const QByteArray savedJson = ProjectStore::serialize(document, QString());
    const QJsonObject styleObject = QJsonDocument::fromJson(savedJson)
                                        .object()
                                        .value(QStringLiteral("pages"))
                                        .toArray()
                                        .at(0)
                                        .toObject()
                                        .value(QStringLiteral("layout"))
                                        .toObject()
                                        .value(QStringLiteral("style"))
                                        .toObject();
    QVERIFY(!styleObject.contains(QStringLiteral("borderMm")));
    QVERIFY(!styleObject.contains(QStringLiteral("borderColor")));
    QCOMPARE(styleObject.value(QStringLiteral("gutterMm")).toDouble(), 4.0);
    for (int i = 0; i < restoredLaidOut.slotList.size(); ++i) {
        QCOMPARE(restoredLaidOut.slotList.at(i).id, laidOut.slotList.at(i).id);
        QVERIFY(nearRect(restoredLaidOut.slotList.at(i).unitRect,
                         laidOut.slotList.at(i).unitRect, 1e-12));
    }
    QCOMPARE(restoredLaidOut.frames.size(), 2);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(restoredLaidOut.frames.at(i).slotId, laidOut.frames.at(i).slotId);
        QVERIFY(nearRect(QRectF(restoredLaidOut.frames.at(i).cx
                                     - restoredLaidOut.frames.at(i).width / 2.0,
                                 restoredLaidOut.frames.at(i).cy
                                     - restoredLaidOut.frames.at(i).height / 2.0,
                                 restoredLaidOut.frames.at(i).width,
                                 restoredLaidOut.frames.at(i).height),
                         QRectF(laidOut.frames.at(i).cx - laidOut.frames.at(i).width / 2.0,
                                laidOut.frames.at(i).cy - laidOut.frames.at(i).height / 2.0,
                                laidOut.frames.at(i).width, laidOut.frames.at(i).height),
                         1e-9));
    }
    QVERIFY(SlotGeometry::layoutConsistent(restoredLaidOut));

    const Page &restoredFree = restored.pages.at(1);
    QVERIFY(!restoredFree.hasLayout());
    QVERIFY(restoredFree.layoutId.isEmpty());
    QCOMPARE(restoredFree.slotStyle, SlotStyle{});
    QCOMPARE(restoredFree.frames.size(), 1);
    QVERIFY(restoredFree.frames.at(0).slotId.isEmpty());
}

void TestSlotLayout::layoutFreeProjectStaysByteIdentical()
{
    DocumentSnapshot document;
    Page page = pageWith(a4Paper(), false);
    page.frames.append(photoFrame(0));
    document.pages.append(page);

    const QByteArray json = ProjectStore::serialize(document, QString());
    QVERIFY(!json.isEmpty());
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject pageObject = root.value(QStringLiteral("pages")).toArray().at(0).toObject();
    QVERIFY(!pageObject.contains(QStringLiteral("layout")));
    const QJsonObject frameObject =
        pageObject.value(QStringLiteral("frames")).toArray().at(0).toObject();
    QVERIFY(!frameObject.contains(QStringLiteral("slotId")));

    const auto reparsed = ProjectStore::deserialize(json);
    QVERIFY(reparsed.ok());
    QVERIFY(!reparsed.value().pages.at(0).hasLayout());
    QCOMPARE(reparsed.value().pages.at(0).frames.size(), 1);
}

void TestSlotLayout::oldProjectsLoadWithoutALayout()
{
    // The repository's own 1.0.1 project still opens, with every page a free
    // canvas. Its photographs may be missing on this machine; that is a source
    // issue, not a layout one.
    const QString path = QStringLiteral(PHOTOPRINT_SOURCE_DIR "/test.ppj");
    QVERIFY(QFileInfo::exists(path));
    ProjectStore store;
    const auto loaded = store.load(path);
    if (!loaded.ok())
        QFAIL(qPrintable(loaded.error().message));
    QVERIFY(!loaded.value().document.pages.isEmpty());
    for (const Page &page : loaded.value().document.pages) {
        QVERIFY(!page.hasLayout());
        QVERIFY(page.layoutId.isEmpty());
        QVERIFY(page.slotStyle == SlotStyle{});
        for (const Frame &frame : page.frames)
            QVERIFY(frame.slotId.isEmpty());
    }
}

void TestSlotLayout::legacyMatMigratesToGap()
{
    // A saved 3 mm mat inset each photo by 3 mm, so the paper edge was
    // margin + 3 mm and two neighbours were gutter + 6 mm apart. Migration
    // keeps both distances and keeps the visible corner radius.
    QJsonObject style;
    style.insert(QStringLiteral("marginMm"), 8.0);
    style.insert(QStringLiteral("gutterMm"), 4.0);
    style.insert(QStringLiteral("borderMm"), 3.0);
    style.insert(QStringLiteral("borderColor"), QStringLiteral("#123456"));
    style.insert(QStringLiteral("cornerRadiusMm"), 8.0);
    const auto parsed = layoutWithSavedStyle(style);
    QVERIFY(parsed.ok());
    const Page &page = parsed.value().pages.at(0);
    QCOMPARE(page.slotStyle.marginMm.value_or(-1.0), 11.0);
    QCOMPARE(page.slotStyle.gutterMm.value_or(-1.0), 10.0);
    // The old clip started at the cell edge; the photo was 3 mm inside it, so
    // the visible radius was 8 - 3 = 5 mm.
    QCOMPARE(page.slotStyle.cornerRadiusMm, 5.0);
    const QList<QRectF> cells = SlotGeometry::cellsMm(page);
    QVERIFY(near(cells.at(0).left(), 8.0 + 3.0));
    QVERIFY(near(cells.at(1).left() - cells.at(0).right(), 4.0 + 2.0 * 3.0));
    QVERIFY(SlotGeometry::layoutConsistent(page));

    // An automatic margin and gutter were an absence, not numbers: resolve
    // them first, then add the border once to the margin and twice to the gap.
    QJsonObject automatic;
    automatic.insert(QStringLiteral("borderMm"), 3.0);
    const auto autoParsed = layoutWithSavedStyle(automatic);
    QVERIFY(autoParsed.ok());
    const double autoMargin = SlotGeometry::automaticMarginMm(QSizeF(210.0, 297.0));
    const Page &autoPage = autoParsed.value().pages.at(0);
    QCOMPARE(autoPage.slotStyle.marginMm.value_or(-1.0), autoMargin + 3.0);
    QCOMPARE(autoPage.slotStyle.gutterMm.value_or(-1.0), autoMargin + 6.0);
    const QList<QRectF> autoCells = SlotGeometry::cellsMm(autoPage);
    QVERIFY(near(autoCells.at(0).left(), autoMargin + 3.0));
    QVERIFY(near(autoCells.at(1).left() - autoCells.at(0).right(),
                 autoMargin + 6.0));

    // A radius smaller than the mat cannot go negative.
    QJsonObject tinyRadius;
    tinyRadius.insert(QStringLiteral("borderMm"), 3.0);
    tinyRadius.insert(QStringLiteral("cornerRadiusMm"), 2.0);
    const auto tinyParsed = layoutWithSavedStyle(tinyRadius);
    QVERIFY(tinyParsed.ok());
    QCOMPARE(tinyParsed.value().pages.at(0).slotStyle.cornerRadiusMm, 0.0);

    // A zero mat changes nothing: both values stay automatic and the radius
    // stays as saved.
    QJsonObject none;
    none.insert(QStringLiteral("borderMm"), 0.0);
    none.insert(QStringLiteral("borderColor"), QStringLiteral("#ffffff"));
    none.insert(QStringLiteral("cornerRadiusMm"), 4.0);
    const auto noneParsed = layoutWithSavedStyle(none);
    QVERIFY(noneParsed.ok());
    QVERIFY(!noneParsed.value().pages.at(0).slotStyle.marginMm.has_value());
    QVERIFY(!noneParsed.value().pages.at(0).slotStyle.gutterMm.has_value());
    QCOMPARE(noneParsed.value().pages.at(0).slotStyle.cornerRadiusMm, 4.0);

    // The migrated values obey their ranges.
    QJsonObject wide;
    wide.insert(QStringLiteral("gutterMm"), 50.0);
    wide.insert(QStringLiteral("borderMm"), 20.0);
    const auto wideParsed = layoutWithSavedStyle(wide);
    QVERIFY(wideParsed.ok());
    QCOMPARE(wideParsed.value().pages.at(0).slotStyle.gutterMm.value_or(-1.0),
             SlotGeometry::kMaxGutterMm);
    QCOMPARE(wideParsed.value().pages.at(0).slotStyle.marginMm.value_or(-1.0),
             std::min(SlotGeometry::kMaxMarginMm,
                      SlotGeometry::automaticMarginMm(QSizeF(210.0, 297.0)) + 20.0));

    // Saving writes no mat keys, and loading those bytes again leaves the
    // migrated spacing and radius untouched.
    const auto savedStyle = [](const DocumentSnapshot &document) {
        return QJsonDocument::fromJson(ProjectStore::serialize(document, QString()))
            .object()
            .value(QStringLiteral("pages"))
            .toArray()
            .at(0)
            .toObject()
            .value(QStringLiteral("layout"))
            .toObject()
            .value(QStringLiteral("style"))
            .toObject();
    };
    for (const DocumentSnapshot *document :
         {&parsed.value(), &autoParsed.value()}) {
        const QByteArray saved = ProjectStore::serialize(*document, QString());
        const QJsonObject styleObject = savedStyle(*document);
        QVERIFY(!styleObject.contains(QStringLiteral("borderMm")));
        QVERIFY(!styleObject.contains(QStringLiteral("borderColor")));
        QVERIFY(styleObject.contains(QStringLiteral("marginMm")));
        QVERIFY(styleObject.contains(QStringLiteral("gutterMm")));

        const auto reloaded = ProjectStore::deserialize(saved);
        QVERIFY(reloaded.ok());
        QCOMPARE(reloaded.value().pages.at(0).slotStyle,
                 document->pages.at(0).slotStyle);
    }
}

void TestSlotLayout::loaderSanitisesBrokenLayouts()
{
    // An unknown slot id becomes a free frame.
    {
        QJsonObject layout;
        layout.insert(QStringLiteral("slots"),
                      QJsonArray{slotJson(QStringLiteral("slot-a"),
                                          QJsonArray{0.0, 0.0, 1.0, 1.0})});
        QJsonObject frame = minimalFrameJson(QStringLiteral("frame-a"));
        frame.insert(QStringLiteral("slotId"), QStringLiteral("slot-b"));
        QJsonObject page = minimalPageJson(QStringLiteral("page-a"), QJsonArray{frame});
        page.insert(QStringLiteral("layout"), layout);
        const auto parsed = ProjectStore::deserialize(encoded(minimalProjectJson(page)));
        QVERIFY(parsed.ok());
        QVERIFY(parsed.value().pages.at(0).hasLayout());
        QVERIFY(parsed.value().pages.at(0).frames.at(0).slotId.isEmpty());
        QVERIFY(SlotGeometry::layoutConsistent(parsed.value().pages.at(0)));
    }

    // A duplicate slot id is dropped, and the frame stays with the survivor.
    {
        QJsonObject layout;
        layout.insert(QStringLiteral("slots"),
                      QJsonArray{slotJson(QStringLiteral("slot-a"),
                                          QJsonArray{0.0, 0.0, 0.5, 1.0}),
                                 slotJson(QStringLiteral("slot-a"),
                                          QJsonArray{0.5, 0.0, 0.5, 1.0})});
        QJsonObject frame = minimalFrameJson(QStringLiteral("frame-a"));
        frame.insert(QStringLiteral("slotId"), QStringLiteral("slot-a"));
        QJsonObject page = minimalPageJson(QStringLiteral("page-a"), QJsonArray{frame});
        page.insert(QStringLiteral("layout"), layout);
        const auto parsed = ProjectStore::deserialize(encoded(minimalProjectJson(page)));
        QVERIFY(parsed.ok());
        QCOMPARE(parsed.value().pages.at(0).slotList.size(), 1);
        QCOMPARE(parsed.value().pages.at(0).frames.at(0).slotId,
                 QStringLiteral("slot-a"));
        QVERIFY(SlotGeometry::layoutConsistent(parsed.value().pages.at(0)));
    }

    // A unit array of three numbers cannot describe a slot; with no other slot
    // the whole layout, style included, is dropped.
    {
        QJsonObject layout;
        layout.insert(QStringLiteral("slots"),
                      QJsonArray{slotJson(QStringLiteral("slot-a"),
                                          QJsonArray{0.0, 0.0, 1.0})});
        QJsonObject styleObject;
        styleObject.insert(QStringLiteral("gutterMm"), 5.0);
        layout.insert(QStringLiteral("style"), styleObject);
        QJsonObject page = minimalPageJson(QStringLiteral("page-a"), QJsonArray{});
        page.insert(QStringLiteral("layout"), layout);
        const auto parsed = ProjectStore::deserialize(encoded(minimalProjectJson(page)));
        QVERIFY(parsed.ok());
        QVERIFY(!parsed.value().pages.at(0).hasLayout());
        QCOMPARE(parsed.value().pages.at(0).slotStyle, SlotStyle{});
    }

    // A unit rectangle outside the square is clamped, not dropped.
    {
        QJsonObject layout;
        layout.insert(QStringLiteral("slots"),
                      QJsonArray{slotJson(QStringLiteral("slot-a"),
                                          QJsonArray{-1.0, -1.0, 3.0, 3.0})});
        QJsonObject page = minimalPageJson(QStringLiteral("page-a"), QJsonArray{});
        page.insert(QStringLiteral("layout"), layout);
        const auto parsed = ProjectStore::deserialize(encoded(minimalProjectJson(page)));
        QVERIFY(parsed.ok());
        QVERIFY(parsed.value().pages.at(0).hasLayout());
        QVERIFY(nearRect(parsed.value().pages.at(0).slotList.at(0).unitRect,
                         QRectF(0.0, 0.0, 1.0, 1.0)));
        QVERIFY(SlotGeometry::layoutConsistent(parsed.value().pages.at(0)));
    }

    // A style without slots is dropped, exactly as the design requires.
    {
        QJsonObject styleObject;
        styleObject.insert(QStringLiteral("gutterMm"), 4.0);
        styleObject.insert(QStringLiteral("cornerRadiusMm"), 2.0);
        QJsonObject layout;
        layout.insert(QStringLiteral("style"), styleObject);
        QJsonObject page = minimalPageJson(QStringLiteral("page-a"), QJsonArray{});
        page.insert(QStringLiteral("layout"), layout);
        const auto parsed = ProjectStore::deserialize(encoded(minimalProjectJson(page)));
        QVERIFY(parsed.ok());
        QVERIFY(!parsed.value().pages.at(0).hasLayout());
        QCOMPARE(parsed.value().pages.at(0).slotStyle, SlotStyle{});
        QVERIFY(parsed.value().pages.at(0).layoutId.isEmpty());
    }

    // A thirteen-slot file is truncated to the model cap.
    {
        QJsonArray slotArray;
        for (int i = 0; i < 13; ++i) {
            slotArray.append(slotJson(QStringLiteral("slot-%1").arg(i),
                                  QJsonArray{0.0, 0.0, 0.5, 0.5}));
        }
        QJsonObject layout;
        layout.insert(QStringLiteral("slots"), slotArray);
        QJsonObject page = minimalPageJson(QStringLiteral("page-a"), QJsonArray{});
        page.insert(QStringLiteral("layout"), layout);
        const auto parsed = ProjectStore::deserialize(encoded(minimalProjectJson(page)));
        QVERIFY(parsed.ok());
        QCOMPARE(parsed.value().pages.at(0).slotList.size(),
                 SlotGeometry::kMaxSlotsPerPage);
        QVERIFY(SlotGeometry::layoutConsistent(parsed.value().pages.at(0)));
    }
}

void TestSlotLayout::oneEditPerAction()
{
    DocumentSnapshot document;
    document.pages.append(pageWith(a4Paper(), false));
    CanvasController controller;
    controller.setDocument(document);
    QCOMPARE(controller.undoStack()->count(), 0);

    const CollageTemplate &t = templateNamed(QStringLiteral("n02-equal-side"));
    const auto applied = SlotLayoutEngine::applyTemplate(controller.document().pages.at(0), t);
    QVERIFY(applied.ok());
    const Page withLayout = applied.value();

    ApplyLayoutEdit edit;
    PageLayout change;
    change.pageIndex = 0;
    change.slotList = withLayout.slotList;
    change.slotStyle = withLayout.slotStyle;
    change.layoutId = withLayout.layoutId;
    edit.pages.append(change);
    edit.actionText = QStringLiteral("Sablon alkalmazása");
    controller.apply(edit);
    QCOMPARE(controller.undoStack()->count(), 1);
    QVERIFY(controller.document().pages.at(0) == withLayout);

    // The same finished page again is not an edit at all.
    controller.apply(edit);
    QCOMPARE(controller.undoStack()->count(), 1);

    // A pure page selection is navigation, not undoable.
    controller.apply(ApplyLayoutEdit{});
    QCOMPARE(controller.undoStack()->count(), 1);

    controller.undoStack()->undo();
    QVERIFY(controller.document() == document);
    controller.undoStack()->redo();
    QVERIFY(controller.document().pages.at(0) == withLayout);

    // A style change is one more step, and undo restores the previous page.
    SlotStyle style;
    style.gutterMm = 4.0;
    const auto styled = SlotLayoutEngine::setStyle(withLayout, style);
    QVERIFY(styled.ok());
    ApplyLayoutEdit styleEdit;
    PageLayout styleChange;
    styleChange.pageIndex = 0;
    styleChange.slotStyle = styled.value().slotStyle;
    styleEdit.pages.append(styleChange);
    controller.apply(styleEdit);
    QCOMPARE(controller.undoStack()->count(), 2);
    QVERIFY(controller.document().pages.at(0).slotStyle == styled.value().slotStyle);
    controller.undoStack()->undo();
    QVERIFY(controller.document().pages.at(0) == withLayout);
}

void TestSlotLayout::validatorRefusesBrokenSlotEdits()
{
    DocumentSnapshot document;
    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-a");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);
    page.frames.append(photoFrame(0));
    page.frames[0].slotId = slot.id;
    SlotGeometry::applySlotGeometry(page, slot, page.frames[0]);
    document.pages.append(page);

    auto editWithSlots = [](const QList<Slot> &slotValues, const QList<Frame> &frames) {
        ApplyLayoutEdit edit;
        PageLayout change;
        change.pageIndex = 0;
        change.slotList = slotValues;
        change.frames = frames;
        edit.pages.append(change);
        return edit;
    };

    QVERIFY(validateLayoutEdit(document, editWithSlots(document.pages.at(0).slotList,
                                                       document.pages.at(0).frames))
                .ok());

    QList<Slot> duplicate = page.slotList;
    duplicate.append(slot);
    QList<Frame> unchangedFrames = page.frames;
    QVERIFY(!validateLayoutEdit(document, editWithSlots(duplicate, unchangedFrames)).ok());

    QList<Slot> emptyId = page.slotList;
    emptyId[0].id.clear();
    QVERIFY(!validateLayoutEdit(document, editWithSlots(emptyId, unchangedFrames)).ok());

    QList<Slot> outside = page.slotList;
    outside[0].unitRect = QRectF(-0.5, 0.0, 1.0, 1.0);
    QVERIFY(!validateLayoutEdit(document, editWithSlots(outside, unchangedFrames)).ok());

    QList<Slot> noSlots;
    QVERIFY(!validateLayoutEdit(document, editWithSlots(noSlots, unchangedFrames)).ok());

    QList<Slot> many;
    for (int i = 0; i < SlotGeometry::kMaxSlotsPerPage + 1; ++i) {
        Slot extra;
        extra.id = QStringLiteral("slot-%1").arg(i);
        extra.unitRect = QRectF(0.0, 0.0, 0.1, 0.1);
        many.append(extra);
    }
    QVERIFY(!validateLayoutEdit(document, editWithSlots(many, QList<Frame>{})).ok());

    // Two frames in one slot is refused too.
    QList<Frame> doubled = page.frames;
    doubled.append(photoFrame(1));
    doubled[1].slotId = slot.id;
    QVERIFY(!validateLayoutEdit(document, editWithSlots(page.slotList, doubled)).ok());
}

void TestSlotLayout::emptySlotPrintsPaper()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString grey = writePhoto(dir, QStringLiteral("grey.png"), QColor(128, 128, 128));
    QVERIFY(!grey.isEmpty());

    Page page = pageWith(a4Paper(), false);
    page.frames.append(frameFor(grey, QStringLiteral("photo-0")));
    page.frames.append(frameFor(grey, QStringLiteral("photo-1")));
    auto applied = SlotLayoutEngine::applyTemplate(
        page, templateNamed(QStringLiteral("n04-grid-2x2")));
    QVERIFY(applied.ok());
    page = applied.value();
    SlotStyle style;
    style.gutterMm = 4.0;
    auto styled = SlotLayoutEngine::setStyle(page, style);
    QVERIFY(styled.ok());
    page = styled.value();

    DocumentSnapshot document;
    document.pages.append(page);

    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(document, 0, 150.0);
    if (!rendered.ok())
        QFAIL(qPrintable(rendered.error().message));
    const QImage image = rendered.value();
    QVERIFY(!image.isNull());

    const QList<QRectF> cells = SlotGeometry::cellsMm(page);
    QCOMPARE(cells.size(), 4);
    for (int index : {2, 3}) {
        const QRectF &cell = cells.at(index);
        // An empty cell is paper edge to edge: no mat, no placeholder tint.
        QVERIFY2(isWhite(pixelAtMm(image, 150.0, cell.left() + 1.0, cell.center().y())),
                 "an empty cell printed a mat at its edge");
        QVERIFY2(isWhite(pixelAtMm(image, 150.0, cell.center().x(), cell.center().y())),
                 "an empty slot printed something other than paper");
    }
    // A filled slot prints its photograph all the way to the cell edge.
    const QRgb filled =
        pixelAtMm(image, 150.0, cells.at(0).right() - 1.0, cells.at(0).center().y());
    QVERIFY(!isWhite(filled));
    QVERIFY(std::abs(qRed(filled) - 128) <= 25);
    // The sheet itself stays paper white outside the layout.
    QVERIFY(isWhite(pixelAtMm(image, 150.0, 1.0, 1.0)));
}

void TestSlotLayout::photoFillsItsCellWithoutAMat()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString blue = writePhoto(dir, QStringLiteral("blue.png"), QColor(30, 30, 220));
    QVERIFY(!blue.isEmpty());

    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-only");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);
    page.layoutId = QStringLiteral("hand");
    SlotStyle style;
    style.marginMm = 10.0;
    style.gutterMm = 3.0;
    page.slotStyle = style;
    Frame frame = frameFor(blue, QStringLiteral("photo-0"));
    frame.slotId = slot.id;
    SlotGeometry::applySlotGeometry(page, slot, frame);
    page.frames.append(frame);
    QVERIFY(SlotGeometry::layoutConsistent(page));

    const QRectF cell = SlotGeometry::slotCellMm(page, slot);
    QVERIFY(nearRect(SlotGeometry::slotPhotoRectMm(page, slot), cell));

    DocumentSnapshot document;
    document.pages.append(page);
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(document, 0, 150.0);
    if (!rendered.ok())
        QFAIL(qPrintable(rendered.error().message));
    const QImage image = rendered.value();

    // Half a millimetre inside each edge is still the photograph: there is no
    // inset and no ring. Half a millimetre outside the cell is paper.
    const double inside = 0.5;
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.left() + inside, cell.center().y())));
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.right() - inside, cell.center().y())));
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.center().x(), cell.top() + inside)));
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.center().x(), cell.bottom() - inside)));
    QVERIFY(isWhite(pixelAtMm(image, 150.0, cell.left() - inside, cell.center().y())));
    QVERIFY(isWhite(pixelAtMm(image, 150.0, cell.center().x(), cell.top() - inside)));
}

void TestSlotLayout::gapIsTheWhitePaperBetweenPhotos()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString red = writePhoto(dir, QStringLiteral("red.png"), QColor(210, 30, 30));
    const QString blue = writePhoto(dir, QStringLiteral("blue.png"), QColor(30, 30, 220));
    QVERIFY(!red.isEmpty() && !blue.isEmpty());

    Page page = pageWith(a4Paper(), false);
    page.frames.append(frameFor(red, QStringLiteral("photo-0")));
    page.frames.append(frameFor(blue, QStringLiteral("photo-1")));
    auto applied = SlotLayoutEngine::applyTemplate(
        page, templateNamed(QStringLiteral("n04-grid-2x2")));
    QVERIFY(applied.ok());
    page = applied.value();
    SlotStyle style;
    style.marginMm = 10.0;
    style.gutterMm = 3.0;
    auto styled = SlotLayoutEngine::setStyle(page, style);
    QVERIFY(styled.ok());
    page = styled.value();

    const QList<QRectF> cells = SlotGeometry::cellsMm(page);
    QCOMPARE(cells.size(), 4);
    QVERIFY(near(cells.at(1).left() - cells.at(0).right(), 3.0));
    QVERIFY(near(cells.at(2).top() - cells.at(0).bottom(), 3.0));

    DocumentSnapshot document;
    document.pages.append(page);
    ImageStore store;
    PageRenderer renderer(store);
    const double ppi = 300.0;
    const auto rendered = renderer.renderPage(document, 0, ppi);
    if (!rendered.ok())
        QFAIL(qPrintable(rendered.error().message));
    const QImage image = rendered.value();
    const double pxPerMm = ppi / 25.4;

    // Measure the white run between the two photographs on the top row.
    const int y = qRound(cells.at(0).center().y() * pxPerMm);
    const int from = qRound(cells.at(0).left() * pxPerMm);
    const int to = qRound(cells.at(1).right() * pxPerMm);
    const int middle = qRound((cells.at(0).right() + cells.at(1).left()) / 2.0 * pxPerMm);
    int lastPhoto = from;
    int firstPhoto = to;
    for (int x = from; x <= middle; ++x) {
        if (!isWhite(image.pixel(x, y)))
            lastPhoto = x;
    }
    for (int x = middle; x <= to; ++x) {
        if (!isWhite(image.pixel(x, y))) {
            firstPhoto = x;
            break;
        }
    }
    const double measuredGapMm = (firstPhoto - lastPhoto - 1) / pxPerMm;
    QVERIFY2(std::abs(measuredGapMm - 3.0) <= 0.3,
             qPrintable(QStringLiteral("a 3 mm gap measured %1 mm").arg(measuredGapMm)));

    // No inset around either photograph: the ink reaches each cell edge.
    QVERIFY(!isWhite(pixelAtMm(image, ppi, cells.at(0).right() - 0.5, cells.at(0).center().y())));
    QVERIFY(!isWhite(pixelAtMm(image, ppi, cells.at(1).left() + 0.5, cells.at(1).center().y())));
    // The gap itself is paper, not a coloured frame.
    QVERIFY(isWhite(pixelAtMm(image, ppi, cells.at(0).right() + 1.5, cells.at(0).center().y())));
    QVERIFY(isWhite(pixelAtMm(image, ppi, cells.at(1).left() - 1.5, cells.at(1).center().y())));
}

void TestSlotLayout::gapChangesSpacingNotThePhotoInset()
{
    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-only");
    slot.unitRect = QRectF(0.0, 0.0, 0.5, 1.0);
    page.slotList.append(slot);
    page.layoutId = QStringLiteral("hand");

    page.slotStyle.marginMm = 10.0;
    page.slotStyle.gutterMm = 3.0;
    const QRectF narrowCell = SlotGeometry::slotCellMm(page, slot);
    const QRectF narrowPhoto = SlotGeometry::slotPhotoRectMm(page, slot);
    QVERIFY(near(narrowCell.left(), 10.0));
    QVERIFY(nearRect(narrowPhoto, narrowCell));

    page.slotStyle.gutterMm = 6.0;
    const QRectF wideCell = SlotGeometry::slotCellMm(page, slot);
    const QRectF widePhoto = SlotGeometry::slotPhotoRectMm(page, slot);
    // The cell gives up 1.5 mm on the shared edge, and the photograph follows
    // the cell exactly. Nothing else is taken from the picture.
    QVERIFY(near(wideCell.left(), 10.0));
    QVERIFY(near(narrowCell.width() - wideCell.width(), 1.5));
    QVERIFY(nearRect(widePhoto, wideCell));
    QVERIFY(near(widePhoto.width(), wideCell.width()));
}

void TestSlotLayout::roundedCornersClipThePhoto()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString blue = writePhoto(dir, QStringLiteral("blue.png"), QColor(30, 30, 220));
    QVERIFY(!blue.isEmpty());

    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-only");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);
    page.layoutId = QStringLiteral("hand");
    SlotStyle style;
    style.cornerRadiusMm = 15.0;
    page.slotStyle = style;
    Frame frame = frameFor(blue, QStringLiteral("photo-0"));
    frame.slotId = slot.id;
    SlotGeometry::applySlotGeometry(page, slot, frame);
    page.frames.append(frame);
    QVERIFY(SlotGeometry::layoutConsistent(page));

    DocumentSnapshot document;
    document.pages.append(page);
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(document, 0, 150.0);
    if (!rendered.ok())
        QFAIL(qPrintable(rendered.error().message));
    const QImage image = rendered.value();

    const QRectF cell = SlotGeometry::slotCellMm(page, slot);
    // The rounded corner is the cell corner with a 15 mm radius: (left + 2,
    // top + 2) lies outside the arc and prints paper white; (left + 9, top + 9)
    // is inside it and prints the photograph. There is no ring: the clip cuts
    // the picture, and the paper shows through the cut corner.
    QVERIFY(isWhite(pixelAtMm(image, 150.0, cell.left() + 2.0, cell.top() + 2.0)));
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.left() + 9.0, cell.top() + 9.0)));
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.center().x(), cell.center().y())));
    // At mid-edge the photograph reaches the cell edge, so the radius is a
    // corner clip and not a band around the picture.
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.left() + 0.5, cell.center().y())));
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.center().x(), cell.top() + 0.5)));
}

void TestSlotLayout::placePhotoOnEmptySlot()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 0);
    const QString target = page.slotList.at(1).id;
    Frame photo = sourceFrame(QStringLiteral("frame-new"), QStringLiteral("/photos/new.png"));

    SlotLayoutEngine::Report report;
    const auto placed = SlotLayoutEngine::placePhoto(page, target, photo, &report);
    if (!placed.ok())
        QFAIL(qPrintable(placed.error().message));
    const Page &result = placed.value();
    QCOMPARE(result.frames.size(), 1);
    const Frame *landed = frameById(result, photo.id);
    QVERIFY(landed);
    QCOMPARE(landed->slotId, target);
    QVERIFY(SlotGeometry::slotGeometryMatches(result, *landed));
    QVERIFY(landed->fit == FitMode::Fill);
    QVERIFY(!landed->autoPlaced);
    QVERIFY(near(landed->cropCenter.x(), 0.5));
    QVERIFY(near(landed->cropCenter.y(), 0.5));
    QCOMPARE(landed->zoom, 1.0);
    QCOMPARE(report.emptySlotsAfter, 3);
    QVERIFY(report.removedFrameIds.isEmpty());
    QVERIFY(report.cropResetIds.isEmpty());
    QVERIFY(SlotGeometry::layoutConsistent(result));
}

void TestSlotLayout::placePhotoReplacesOccupant()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 2);
    const QString target = page.slotList.at(0).id;
    const QString replaced = page.frames.at(0).id;
    Frame replacement =
        sourceFrame(QStringLiteral("frame-replacement"), QStringLiteral("/photos/replacement.png"));

    SlotLayoutEngine::Report report;
    const auto placed = SlotLayoutEngine::placePhoto(page, target, replacement, &report);
    QVERIFY(placed.ok());
    const Page &result = placed.value();
    QCOMPARE(report.removedFrameIds, QStringList{replaced});
    QCOMPARE(result.frames.size(), 2);
    QVERIFY(!frameById(result, replaced));
    const Frame *landed = frameById(result, replacement.id);
    QVERIFY(landed);
    QCOMPARE(landed->slotId, target);
    QVERIFY(SlotGeometry::slotGeometryMatches(result, *landed));
    // The untouched photograph is still in its own slot.
    QCOMPARE(frameById(result, page.frames.at(1).id)->slotId, page.slotList.at(1).id);
    QVERIFY(SlotGeometry::layoutConsistent(result));
}

void TestSlotLayout::placementKeepsOrResetsTheCrop()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 0);
    const Slot &slot0 = page.slotList.at(0);
    const QRectF cell = SlotGeometry::slotPhotoRectMm(page, slot0);

    Frame keep = sourceFrame(QStringLiteral("frame-keep"), QStringLiteral("/photos/keep.png"));
    keep.width = cell.width();
    keep.height = cell.height();
    keep.cropCenter = QPointF(0.3, 0.7);
    keep.zoom = 2.5;
    keep.angleDeg = 15.0;
    // The crop rule applies to a photograph that is already on the page; a brand
    // new drop starts centred at 1x by definition.
    Page keepPage = page;
    keepPage.frames.append(keep);
    SlotLayoutEngine::Report kept;
    const auto keptResult = SlotLayoutEngine::placePhoto(keepPage, slot0.id, keep, &kept);
    QVERIFY(keptResult.ok());
    const Frame *keptFrame = frameById(keptResult.value(), keep.id);
    QVERIFY(keptFrame);
    QVERIFY(near(keptFrame->cropCenter.x(), 0.3));
    QVERIFY(near(keptFrame->cropCenter.y(), 0.7));
    QVERIFY(near(keptFrame->zoom, 2.5));
    QCOMPARE(keptFrame->angleDeg, 0.0);
    QCOMPARE(kept.rotationResetIds, QStringList{keep.id});
    QVERIFY(kept.cropResetIds.isEmpty());

    Frame reset = sourceFrame(QStringLiteral("frame-reset"), QStringLiteral("/photos/reset.png"));
    reset.width = 150.0;
    reset.height = 20.0;
    reset.cropCenter = QPointF(0.2, 0.8);
    reset.zoom = 4.0;
    Page resetPage = page;
    resetPage.frames.append(reset);
    SlotLayoutEngine::Report resetReport;
    const auto resetResult = SlotLayoutEngine::placePhoto(resetPage, slot0.id, reset, &resetReport);
    QVERIFY(resetResult.ok());
    const Frame *resetFrame = frameById(resetResult.value(), reset.id);
    QVERIFY(resetFrame);
    QVERIFY(near(resetFrame->cropCenter.x(), 0.5));
    QVERIFY(near(resetFrame->cropCenter.y(), 0.5));
    QCOMPARE(resetFrame->zoom, 1.0);
    QCOMPARE(resetReport.cropResetIds, QStringList{reset.id});
}

void TestSlotLayout::movePhotoToAnotherSlot()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 4);
    const QString movedId = page.frames.at(0).id;
    const QString removedId = page.frames.at(2).id;
    const QString target = page.slotList.at(2).id;

    SlotLayoutEngine::Report report;
    const auto moved = SlotLayoutEngine::movePhoto(page, movedId, target, &report);
    QVERIFY(moved.ok());
    const Page &result = moved.value();
    QCOMPARE(report.removedFrameIds, QStringList{removedId});
    QCOMPARE(result.frames.size(), 3);
    const Frame *landed = frameById(result, movedId);
    QVERIFY(landed);
    QCOMPARE(landed->slotId, target);
    QVERIFY(SlotGeometry::slotGeometryMatches(result, *landed));
    QVERIFY(!frameById(result, removedId));
    QVERIFY(!SlotGeometry::frameInSlot(result, page.slotList.at(0).id));
    // The four-cell grid has one cell aspect, so the crop survives the move.
    QVERIFY(report.cropResetIds.isEmpty());
    QVERIFY(SlotGeometry::layoutConsistent(result));
}

void TestSlotLayout::swapPhotosExchangesPayloads()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 4);
    page.frames[0].cropCenter = QPointF(0.2, 0.3);
    page.frames[0].zoom = 3.0;
    page.frames[2].cropCenter = QPointF(0.7, 0.8);
    page.frames[2].zoom = 1.7;
    const QString sourceId = page.frames.at(0).id;
    const QString targetId = page.frames.at(2).id;
    const QString targetSlot = page.slotList.at(2).id;
    const double sourceCx = page.frames.at(0).cx;
    const double sourceCy = page.frames.at(0).cy;
    const double targetCx = page.frames.at(2).cx;
    const double targetCy = page.frames.at(2).cy;

    SlotLayoutEngine::Report report;
    const auto swapped = SlotLayoutEngine::swapPhotos(page, sourceId, targetSlot, &report);
    QVERIFY(swapped.ok());
    const Page &result = swapped.value();
    QCOMPARE(result.frames.size(), 4);
    const Frame *source = frameById(result, sourceId);
    const Frame *target = frameById(result, targetId);
    QVERIFY(source);
    QVERIFY(target);
    // Payload exchanged, identity and assignment intact.
    QCOMPARE(source->sourcePath, QStringLiteral("/photos/photo-2.jpg"));
    QCOMPARE(target->sourcePath, QStringLiteral("/photos/photo-0.jpg"));
    QCOMPARE(source->id, sourceId);
    QCOMPARE(target->id, targetId);
    QCOMPARE(source->slotId, page.slotList.at(0).id);
    QCOMPARE(target->slotId, targetSlot);
    QVERIFY(near(source->cropCenter.x(), 0.7));
    QVERIFY(near(source->cropCenter.y(), 0.8));
    QVERIFY(near(source->zoom, 1.7));
    QVERIFY(near(target->cropCenter.x(), 0.2));
    QVERIFY(near(target->cropCenter.y(), 0.3));
    QVERIFY(near(target->zoom, 3.0));
    QVERIFY(report.cropResetIds.isEmpty());
    QCOMPARE(source->cx, sourceCx);
    QCOMPARE(source->cy, sourceCy);
    QCOMPARE(target->cx, targetCx);
    QCOMPARE(target->cy, targetCy);
    QVERIFY(SlotGeometry::layoutConsistent(result));

    // A swap with an empty target is a move.
    Page one = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 1);
    const auto toEmpty = SlotLayoutEngine::swapPhotos(one, one.frames.at(0).id,
                                                      one.slotList.at(2).id);
    QVERIFY(toEmpty.ok());
    QCOMPARE(frameById(toEmpty.value(), one.frames.at(0).id)->slotId, one.slotList.at(2).id);
}

void TestSlotLayout::emptySlotRemovesThePhoto()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 2);
    const QString filled = page.slotList.at(0).id;
    const QString emptied = page.frames.at(0).id;
    SlotLayoutEngine::Report report;
    const auto emptiedResult = SlotLayoutEngine::emptySlot(page, filled, &report);
    QVERIFY(emptiedResult.ok());
    QCOMPARE(report.removedFrameIds, QStringList{emptied});
    QCOMPARE(emptiedResult.value().frames.size(), 1);
    QVERIFY(!SlotGeometry::frameInSlot(emptiedResult.value(), filled));
    QVERIFY(SlotGeometry::layoutConsistent(emptiedResult.value()));

    // Emptying an empty slot is a no-op, not an error.
    const auto again = SlotLayoutEngine::emptySlot(emptiedResult.value(), filled);
    QVERIFY(again.ok());
    QCOMPARE(again.value().frames.size(), 1);

    const auto unknown = SlotLayoutEngine::emptySlot(page, QStringLiteral("slot-missing"));
    QVERIFY(!unknown.ok());
    QCOMPARE(unknown.error().code, QStringLiteral("slot-missing"));
}

void TestSlotLayout::placePhotosMultiDrop()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n06-grid-3x2")), a4Paper(), false, 2);
    QCOMPARE(page.frames.size(), 2);
    const QString target = page.slotList.at(4).id;

    QList<Frame> drop;
    for (int i = 0; i < 6; ++i) {
        drop.append(sourceFrame(QStringLiteral("drop-%1").arg(i),
                                QStringLiteral("/photos/drop-%1.png").arg(i)));
    }
    SlotLayoutEngine::Report report;
    const auto placed = SlotLayoutEngine::placePhotos(page, target, drop, &report);
    if (!placed.ok())
        QFAIL(qPrintable(placed.error().message));
    const Page &result = placed.value();
    // Two existing photographs plus four placed; five empty slots were free at
    // the start, so one dropped photograph has no slot and stays out of the page.
    QCOMPARE(result.frames.size(), 6);
    QCOMPARE(report.leftFreeIds.size(), 2);
    QCOMPARE(report.leftFreeIds.at(0), QStringLiteral("drop-4"));
    QCOMPARE(report.leftFreeIds.at(1), QStringLiteral("drop-5"));
    QCOMPARE(report.emptySlotsAfter, 0);
    QCOMPARE(frameById(result, QStringLiteral("drop-0"))->slotId, target);
    // Then the empty slots in fill order, skipping the stated slot.
    QCOMPARE(frameById(result, QStringLiteral("drop-1"))->slotId, page.slotList.at(2).id);
    QCOMPARE(frameById(result, QStringLiteral("drop-2"))->slotId, page.slotList.at(3).id);
    QCOMPARE(frameById(result, QStringLiteral("drop-3"))->slotId, page.slotList.at(5).id);
    QVERIFY(!frameById(result, QStringLiteral("drop-4")));
    QVERIFY(SlotGeometry::layoutConsistent(result));

    // A drop that finds no slot leaves every payload free.
    Page full = layoutPage(templateNamed(QStringLiteral("n02-equal-side")), a4Paper(), false, 2);
    QList<Frame> extra = {sourceFrame(QStringLiteral("extra-0"), QStringLiteral("/photos/e0.png")),
                          sourceFrame(QStringLiteral("extra-1"), QStringLiteral("/photos/e1.png"))};
    SlotLayoutEngine::Report fullReport;
    const auto refused = SlotLayoutEngine::placePhotos(full, QString(), extra, &fullReport);
    QVERIFY(refused.ok());
    QCOMPARE(refused.value().frames.size(), 2);
    const QStringList leftover = {QStringLiteral("extra-0"), QStringLiteral("extra-1")};
    QCOMPARE(fullReport.leftFreeIds, leftover);
}

void TestSlotLayout::placementErrors()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 2);
    const Frame photo = sourceFrame(QStringLiteral("frame-photo"), QStringLiteral("/photos/p.png"));

    const auto missingSlot =
        SlotLayoutEngine::placePhoto(page, QStringLiteral("missing"), photo);
    QVERIFY(!missingSlot.ok());
    QCOMPARE(missingSlot.error().code, QStringLiteral("slot-missing"));

    const auto missingFrame = SlotLayoutEngine::movePhoto(
        page, QStringLiteral("missing"), page.slotList.at(2).id);
    QVERIFY(!missingFrame.ok());
    QCOMPARE(missingFrame.error().code, QStringLiteral("frame-missing"));

    const auto missingSwap = SlotLayoutEngine::swapPhotos(
        page, QStringLiteral("missing"), page.slotList.at(2).id);
    QVERIFY(!missingSwap.ok());
    QCOMPARE(missingSwap.error().code, QStringLiteral("frame-missing"));

    Frame sourceLess = photo;
    sourceLess.id = QStringLiteral("frame-no-source");
    sourceLess.sourcePath.clear();
    const auto noSource =
        SlotLayoutEngine::placePhoto(page, page.slotList.at(2).id, sourceLess);
    QVERIFY(!noSource.ok());
    QCOMPARE(noSource.error().code, QStringLiteral("frame-source"));

    const QList<Frame> duplicate = {photo, photo};
    const auto duplicated =
        SlotLayoutEngine::placePhotos(page, page.slotList.at(2).id, duplicate);
    QVERIFY(!duplicated.ok());
    QCOMPARE(duplicated.error().code, QStringLiteral("frame-id"));

    const auto badTarget =
        SlotLayoutEngine::placePhotos(page, QStringLiteral("missing"), {photo});
    QVERIFY(!badTarget.ok());
    QCOMPARE(badTarget.error().code, QStringLiteral("slot-missing"));
}

void TestSlotLayout::topUpKeepsExistingPhotos()
{
    const CollageTemplate &t = templateNamed(QStringLiteral("n06-grid-3x2"));
    Page page = layoutPage(t, a4Paper(), false, 4);
    const QStringList slotIds = {page.slotList.at(0).id, page.slotList.at(1).id,
                                 page.slotList.at(2).id, page.slotList.at(3).id};
    const QStringList assignedIds = {page.frames.at(0).id, page.frames.at(1).id,
                                     page.frames.at(2).id, page.frames.at(3).id};
    page.frames.append(sourceFrame(QStringLiteral("free-0"), QStringLiteral("/photos/f0.png")));
    page.frames.append(sourceFrame(QStringLiteral("free-1"), QStringLiteral("/photos/f1.png")));

    SlotLayoutEngine::Report report;
    const auto topped = SlotLayoutEngine::applyTemplateKeeping(page, t, &report);
    if (!topped.ok())
        QFAIL(qPrintable(topped.error().message));
    const Page &result = topped.value();
    QCOMPARE(result.slotList.size(), 6);
    // The layout was not rebuilt: every slot id survived.
    for (int i = 0; i < 6; ++i)
        QCOMPARE(result.slotList.at(i).id, page.slotList.at(i).id);
    // Existing photographs did not move.
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(result.slotList.at(i).id, slotIds.at(i));
        QCOMPARE(frameById(result, assignedIds.at(i))->slotId, slotIds.at(i));
    }
    QCOMPARE(frameById(result, QStringLiteral("free-0"))->slotId, page.slotList.at(4).id);
    QCOMPARE(frameById(result, QStringLiteral("free-1"))->slotId, page.slotList.at(5).id);
    QCOMPARE(report.emptySlotsAfter, 0);
    QVERIFY(SlotGeometry::layoutConsistent(result));

    // A different template is refused rather than overwriting the page.
    const auto mismatch = SlotLayoutEngine::applyTemplateKeeping(
        page, templateNamed(QStringLiteral("n04-grid-2x2")));
    QVERIFY(!mismatch.ok());
    QCOMPARE(mismatch.error().code, QStringLiteral("layout-mismatch"));

    // A page without a layout is laid out, exactly as applyTemplate would.
    Page empty = pageWith(a4Paper(), false);
    const auto built = SlotLayoutEngine::applyTemplateKeeping(empty, t);
    QVERIFY(built.ok());
    QVERIFY(built.value().hasLayout());
    QCOMPARE(built.value().slotList.size(), 6);
}

void TestSlotLayout::fillEmptySlotsWithoutATemplate()
{
    Page free = pageWith(a4Paper(), false);
    free.frames.append(sourceFrame(QStringLiteral("free-0"), QStringLiteral("/photos/f0.png")));
    const auto untouched = SlotLayoutEngine::fillEmptySlots(free);
    QVERIFY(untouched.ok());
    QVERIFY(!untouched.value().hasLayout());
    QCOMPARE(untouched.value().frames.size(), 1);

    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 0);
    page.frames.append(sourceFrame(QStringLiteral("free-0"), QStringLiteral("/photos/f0.png")));
    page.frames.append(sourceFrame(QStringLiteral("free-1"), QStringLiteral("/photos/f1.png")));
    SlotLayoutEngine::Report report;
    const auto filled = SlotLayoutEngine::fillEmptySlots(page, &report);
    QVERIFY(filled.ok());
    QCOMPARE(filled.value().frames.size(), 2);
    QCOMPARE(report.emptySlotsAfter, 2);
    QCOMPARE(frameById(filled.value(), QStringLiteral("free-0"))->slotId,
             page.slotList.at(0).id);
    QCOMPARE(frameById(filled.value(), QStringLiteral("free-1"))->slotId,
             page.slotList.at(1).id);
    QVERIFY(SlotGeometry::layoutConsistent(filled.value()));
}

void TestSlotLayout::publishPageStyleOnly()
{
    DocumentSnapshot document;
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 2);
    document.pages.append(page);
    CanvasController controller;
    controller.setDocument(document);
    QCOMPARE(controller.undoStack()->count(), 0);

    Page styled = controller.document().pages.at(0);
    styled.slotStyle.gutterMm = 4.0;
    const auto published = controller.publishPage(styled, QStringLiteral("Szegély beállítása"));
    if (!published.ok())
        QFAIL(qPrintable(published.error().message));
    QCOMPARE(controller.undoStack()->count(), 1);
    const Page &after = controller.document().pages.at(0);
    QCOMPARE(after.slotList.size(), 4);
    QCOMPARE(after.frames.size(), 2);
    QCOMPARE(after.slotStyle.gutterMm.value_or(-1.0), 4.0);
    QVERIFY(SlotGeometry::layoutConsistent(after));
    // The gap changed the cell size, so repair re-derived the photograph
    // rectangle from the slot.
    for (const Frame &frame : after.frames) {
        QVERIFY(SlotGeometry::slotGeometryMatches(after, frame));
        QVERIFY(frame.width < 97.5);
    }

    // Publishing the same finished page again is not an edit.
    QVERIFY(controller.publishPage(after).ok());
    QCOMPARE(controller.undoStack()->count(), 1);

    controller.undoStack()->undo();
    QVERIFY(controller.document() == document);
}

void TestSlotLayout::publishPageFramesAndSlots()
{
    DocumentSnapshot document;
    document.pages.append(pageWith(a4Paper(), false));
    CanvasController controller;
    controller.setDocument(document);

    Page page = controller.document().pages.at(0);
    const auto applied =
        SlotLayoutEngine::applyTemplate(page, templateNamed(QStringLiteral("n04-grid-2x2")));
    QVERIFY(applied.ok());
    page = applied.value();
    const auto placed = SlotLayoutEngine::placePhoto(
        page, page.slotList.at(2).id,
        sourceFrame(QStringLiteral("frame-drop"), QStringLiteral("/photos/drop.png")));
    QVERIFY(placed.ok());
    page = placed.value();

    const auto published = controller.publishPage(page, QStringLiteral("Sablon alkalmazása"));
    QVERIFY(published.ok());
    QCOMPARE(controller.undoStack()->count(), 1);
    QVERIFY(controller.document().pages.at(0) == page);
    controller.undoStack()->undo();
    QVERIFY(controller.document() == document);
}

void TestSlotLayout::staleGeometryDoesNotPushAnUndoStep()
{
    DocumentSnapshot document;
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 2);
    document.pages.append(page);
    CanvasController controller;
    controller.setDocument(document);

    // A frames edit that only drifts slotted geometry repairs back to the same
    // page; it must not leave an undo step behind.
    QList<Frame> drifted = controller.document().pages.at(0).frames;
    drifted[0].cx += 25.0;
    drifted[1].angleDeg = 30.0;
    const auto framesEdit = controller.apply(SetFramesEdit{0, drifted});
    QVERIFY(framesEdit.ok());
    QCOMPARE(controller.undoStack()->count(), 0);
    QVERIFY(SlotGeometry::layoutConsistent(controller.document().pages.at(0)));

    Frame moved = drifted.at(0);
    moved.cx += 10.0;
    const auto frameEdit = controller.apply(FrameChange{moved.id, moved});
    QVERIFY(frameEdit.ok());
    QCOMPARE(controller.undoStack()->count(), 0);

    // The same through the publish API: a page whose only difference is stale
    // slotted geometry is not an edit after repair.
    Page driftedPage = controller.document().pages.at(0);
    driftedPage.frames[1].cx += 40.0;
    const auto publishEdit = controller.publishPage(driftedPage);
    QVERIFY(publishEdit.ok());
    QCOMPARE(controller.undoStack()->count(), 0);

    // A real crop edit still pushes exactly one step.
    Frame cropped = controller.document().pages.at(0).frames.at(0);
    cropped.cropCenter = QPointF(0.2, 0.3);
    QVERIFY(controller.apply(FrameChange{cropped.id, cropped}).ok());
    QCOMPARE(controller.undoStack()->count(), 1);
    controller.undoStack()->undo();
    QVERIFY(controller.document() == document);
}

void TestSlotLayout::validatorErrorCodes()
{
    DocumentSnapshot document;
    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-a");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);
    page.frames.append(photoFrame(0));
    page.frames[0].slotId = slot.id;
    SlotGeometry::applySlotGeometry(page, slot, page.frames[0]);
    document.pages.append(page);

    auto codeOf = [&document](const QList<Slot> &slotValues, const QList<Frame> &frames,
                              const std::optional<SlotStyle> &style = std::nullopt,
                              int pageIndex = 0) {
        ApplyLayoutEdit edit;
        PageLayout change;
        change.pageIndex = pageIndex;
        change.slotList = slotValues;
        change.frames = frames;
        if (style)
            change.slotStyle = *style;
        edit.pages.append(change);
        const auto valid = validateLayoutEdit(document, edit);
        return valid.ok() ? QString() : valid.error().code;
    };

    QCOMPARE(codeOf(page.slotList, page.frames), QString());
    QList<Slot> duplicate = page.slotList;
    duplicate.append(slot);
    QCOMPARE(codeOf(duplicate, page.frames), QStringLiteral("layout-slot-id"));
    QList<Slot> emptyId = page.slotList;
    emptyId[0].id.clear();
    QCOMPARE(codeOf(emptyId, page.frames), QStringLiteral("layout-slot-id"));
    QList<Slot> outside = page.slotList;
    outside[0].unitRect = QRectF(-0.5, 0.0, 1.0, 1.0);
    QCOMPARE(codeOf(outside, page.frames), QStringLiteral("layout-slot-unit"));
    QList<Frame> dangling = page.frames;
    dangling[0].slotId = QStringLiteral("missing");
    QCOMPARE(codeOf(page.slotList, dangling), QStringLiteral("layout-slot-dangling"));
    QList<Frame> doubled = page.frames;
    doubled.append(photoFrame(1));
    doubled[1].slotId = slot.id;
    QCOMPARE(codeOf(page.slotList, doubled), QStringLiteral("layout-slot-duplicate"));

    QList<Slot> many;
    for (int i = 0; i < SlotGeometry::kMaxSlotsPerPage + 1; ++i) {
        Slot extra;
        extra.id = QStringLiteral("slot-%1").arg(i);
        extra.unitRect = QRectF(0.0, 0.0, 0.1, 0.1);
        many.append(extra);
    }
    QCOMPARE(codeOf(many, {}), QStringLiteral("layout-slot-cap"));

    SlotStyle big;
    big.marginMm = 500.0;
    QCOMPARE(codeOf(page.slotList, page.frames, big), QStringLiteral("layout-slot-style"));
    SlotStyle nan;
    nan.cornerRadiusMm = std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(codeOf(page.slotList, page.frames, nan), QStringLiteral("layout-slot-style"));

    ApplyLayoutEdit badIndex;
    PageLayout change;
    change.pageIndex = 3;
    badIndex.pages.append(change);
    QCOMPARE(validateLayoutEdit(document, badIndex).error().code,
             QStringLiteral("layout-page-index"));
}

void TestSlotLayout::zeroMarginIsNotAutomatic()
{
    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-only");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);

    // Auto: A4's 5 % is 10.5 mm, capped at 10 mm.
    const QRectF automatic = SlotGeometry::slotCellMm(page, slot);
    QVERIFY(near(automatic.left(), 10.0));
    QVERIFY(near(automatic.width(), 190.0));

    // An explicit zero is a real zero, not "automatic".
    page.slotStyle.marginMm = 0.0;
    page.slotStyle.gutterMm = 0.0;
    QCOMPARE(SlotGeometry::effectiveMarginMm(page.slotStyle, pageSizeMm(page)), 0.0);
    const QRectF flush = SlotGeometry::slotCellMm(page, slot);
    QVERIFY(nearRect(flush, QRectF(0.0, 0.0, 210.0, 297.0)));

    DocumentSnapshot explicitDoc;
    explicitDoc.pages.append(page);
    const QByteArray explicitJson = ProjectStore::serialize(explicitDoc, QString());
    const QJsonObject explicitRoot = QJsonDocument::fromJson(explicitJson).object();
    const QJsonObject explicitStyle = explicitRoot.value(QStringLiteral("pages"))
                                          .toArray()
                                          .at(0)
                                          .toObject()
                                          .value(QStringLiteral("layout"))
                                          .toObject()
                                          .value(QStringLiteral("style"))
                                          .toObject();
    QVERIFY(explicitStyle.contains(QStringLiteral("marginMm")));
    QCOMPARE(explicitStyle.value(QStringLiteral("marginMm")).toDouble(), 0.0);
    const auto reparsedExplicit = ProjectStore::deserialize(explicitJson);
    QVERIFY(reparsedExplicit.ok());
    const SlotStyle loaded = reparsedExplicit.value().pages.at(0).slotStyle;
    QVERIFY(loaded.marginMm.has_value());
    QCOMPARE(*loaded.marginMm, 0.0);
    QVERIFY(loaded.gutterMm.has_value());
    QCOMPARE(*loaded.gutterMm, 0.0);
    QVERIFY(nearRect(SlotGeometry::slotCellMm(reparsedExplicit.value().pages.at(0),
                                              reparsedExplicit.value().pages.at(0).slotList.at(0)),
                     flush));

    // Auto persists as absent keys, even when the style object exists for a
    // corner radius, and loads back as automatic.
    Page autoPage = pageWith(a4Paper(), false);
    autoPage.slotList.append(slot);
    autoPage.slotStyle.cornerRadiusMm = 2.0;
    DocumentSnapshot autoDoc;
    autoDoc.pages.append(autoPage);
    const QByteArray autoJson = ProjectStore::serialize(autoDoc, QString());
    const QJsonObject autoStyle = QJsonDocument::fromJson(autoJson)
                                      .object()
                                      .value(QStringLiteral("pages"))
                                      .toArray()
                                      .at(0)
                                      .toObject()
                                      .value(QStringLiteral("layout"))
                                      .toObject()
                                      .value(QStringLiteral("style"))
                                      .toObject();
    QVERIFY(!autoStyle.contains(QStringLiteral("marginMm")));
    QVERIFY(!autoStyle.contains(QStringLiteral("gutterMm")));
    const auto reparsedAuto = ProjectStore::deserialize(autoJson);
    QVERIFY(reparsedAuto.ok());
    QVERIFY(!reparsedAuto.value().pages.at(0).slotStyle.marginMm.has_value());
    QVERIFY(near(SlotGeometry::slotCellMm(reparsedAuto.value().pages.at(0),
                                          reparsedAuto.value().pages.at(0).slotList.at(0))
                     .left(),
                 10.0));
}

void TestSlotLayout::displayOrderPutsSlotsFirst()
{
    Page page = pageWith(a4Paper(), false);
    Slot first;
    first.id = QStringLiteral("slot-0");
    first.unitRect = QRectF(0.0, 0.0, 0.5, 1.0);
    Slot second;
    second.id = QStringLiteral("slot-1");
    second.unitRect = QRectF(0.5, 0.0, 0.5, 1.0);
    page.slotList = {first, second};

    Frame freeFront = sourceFrame(QStringLiteral("free-front"), QStringLiteral("/photos/a.png"));
    Frame inSecond = sourceFrame(QStringLiteral("in-second"), QStringLiteral("/photos/b.png"));
    inSecond.slotId = second.id;
    Frame inFirst = sourceFrame(QStringLiteral("in-first"), QStringLiteral("/photos/c.png"));
    inFirst.slotId = first.id;
    Frame noSource = sourceFrame(QStringLiteral("no-source"), QString());
    page.frames = {freeFront, inSecond, inFirst, noSource};

    const QList<int> order = SlotLayoutEngine::frameIndicesInDisplayOrder(page);
    QCOMPARE(order, QList<int>({2, 1, 0, 3}));

    const QList<Frame> photos = SlotLayoutEngine::photographsInDisplayOrder(page);
    QCOMPARE(int(photos.size()), 3);
    QCOMPARE(photos.at(0).id, QStringLiteral("in-first"));
    QCOMPARE(photos.at(1).id, QStringLiteral("in-second"));
    QCOMPARE(photos.at(2).id, QStringLiteral("free-front"));

    // A page without slots keeps z-order, which is the old behaviour.
    Page freePage = pageWith(a4Paper(), false);
    freePage.frames = {freeFront, inSecond, inFirst};
    QCOMPARE(SlotLayoutEngine::frameIndicesInDisplayOrder(freePage), QList<int>({0, 1, 2}));
}

void TestSlotLayout::pickerPreviewMapsPhotosToSlots()
{
    const CollageTemplate &t = templateNamed(QStringLiteral("n02-equal-side"));
    Page page = pageWith(a4Paper(), false);
    for (int i = 0; i < 2; ++i) {
        Slot slot;
        slot.id = QStringLiteral("slot-%1").arg(i);
        slot.unitRect = t.slotList.at(i).rect;
        page.slotList.append(slot);
    }
    // The page stores the blue frame first (z-order), but slot 0 holds red.
    Frame blue = sourceFrame(QStringLiteral("frame-blue"), QStringLiteral("/photos/blue.png"));
    blue.slotId = page.slotList.at(1).id;
    Frame red = sourceFrame(QStringLiteral("frame-red"), QStringLiteral("/photos/red.png"));
    red.slotId = page.slotList.at(0).id;
    page.frames = {blue, red};
    const auto repaired = SlotLayoutEngine::repair(page);
    QVERIFY(repaired.ok());
    page = repaired.value();
    QVERIFY(SlotGeometry::layoutConsistent(page));

    QImage redThumb(80, 60, QImage::Format_RGB32);
    redThumb.fill(QColor(220, 30, 30));
    QImage blueThumb(80, 60, QImage::Format_RGB32);
    blueThumb.fill(QColor(30, 30, 220));

    TemplateBrowserRequest request;
    request.paper = page.paper;
    request.landscape = page.landscape;
    request.frames = page.frames;
    request.slotList = page.slotList;
    request.photos = {blueThumb, redThumb};
    TemplateBrowserDialog dialog(request);

    const QImage tile = dialog.templateTileImage(t.id);
    QVERIFY(!tile.isNull());
    const qreal dpr = std::max<qreal>(1.0, tile.devicePixelRatio());
    const double logicalWidth = tile.width() / dpr;
    const double logicalHeight = tile.height() / dpr;
    const QRectF sheet(0.5, 0.5, logicalWidth - 1.0, logicalHeight - 1.0);
    const QSizeF mm = pageSizeMm(page);
    const double scaleX = sheet.width() / mm.width();
    const double scaleY = sheet.height() / mm.height();
    const auto mapping = CollageTemplates::instance().slotsMm(t, page.paper, page.landscape);
    QVERIFY(mapping.ok());
    for (int i = 0; i < 2; ++i) {
        const QRectF &slot = mapping.value().at(i);
        const double x = (sheet.left() + (slot.x() + slot.width() / 2.0) * scaleX) * dpr;
        const double y = (sheet.top() + (slot.y() + slot.height() / 2.0) * scaleY) * dpr;
        const QRgb pixel = tile.pixel(std::clamp(int(std::lround(x)), 0, tile.width() - 1),
                                      std::clamp(int(std::lround(y)), 0, tile.height() - 1));
        if (i == 0)
            QVERIFY2(isReddish(pixel), "the red photo belongs in the first slot");
        else
            QVERIFY2(isBluish(pixel), "the blue photo belongs in the second slot");
    }
}

void TestSlotLayout::repairSanitisesTheStyle()
{
    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-only");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);
    Frame frame = sourceFrame(QStringLiteral("frame-0"), QStringLiteral("/photos/0.png"));
    frame.slotId = slot.id;
    page.frames.append(frame);

    page.slotStyle.marginMm = 500.0;
    page.slotStyle.gutterMm = -4.0;
    page.slotStyle.cornerRadiusMm = 100.0;
    const auto repaired = SlotLayoutEngine::repair(page);
    QVERIFY(repaired.ok());
    const Page &result = repaired.value();
    QCOMPARE(result.slotStyle.marginMm.value_or(-1.0), SlotGeometry::kMaxMarginMm);
    QCOMPARE(result.slotStyle.gutterMm.value_or(-1.0), 0.0);
    QCOMPARE(result.slotStyle.cornerRadiusMm, SlotGeometry::kMaxCornerRadiusMm);
    QVERIFY(SlotGeometry::layoutConsistent(result));

    // A non-finite gap is corruption, so it becomes automatic again.
    page.slotStyle = SlotStyle{};
    page.slotStyle.gutterMm = std::numeric_limits<double>::quiet_NaN();
    const auto nan = SlotLayoutEngine::repair(page);
    QVERIFY(nan.ok());
    QVERIFY(!nan.value().slotStyle.gutterMm.has_value());
}

void TestSlotLayout::removeLastSlotClearsDanglingIds()
{
    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-a");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);
    Frame assigned = sourceFrame(QStringLiteral("frame-assigned"), QStringLiteral("/photos/a.png"));
    assigned.slotId = slot.id;
    page.frames.append(assigned);
    // A frame naming a slot that never existed is dangling input. Removing the
    // last real slot must still clear it, not leave it pointing at nothing.
    Frame ghost = sourceFrame(QStringLiteral("frame-ghost"), QStringLiteral("/photos/b.png"));
    ghost.slotId = QStringLiteral("slot-ghost");
    page.frames.append(ghost);

    const auto removed = SlotLayoutEngine::removeSlot(page, slot.id);
    QVERIFY(removed.ok());
    const Page &result = removed.value();
    QVERIFY(!result.hasLayout());
    QCOMPARE(result.frames.size(), 1);
    QCOMPARE(result.frames.at(0).id, QStringLiteral("frame-ghost"));
    QVERIFY(result.frames.at(0).slotId.isEmpty());
    QCOMPARE(result.slotStyle, SlotStyle{});
    QVERIFY(result.layoutId.isEmpty());
    QVERIFY(SlotGeometry::layoutConsistent(result));
}

void TestSlotLayout::layoutPageWithoutFramesPrintsPaper()
{
    Page page = layoutPage(templateNamed(QStringLiteral("n04-grid-2x2")), a4Paper(), false, 0);
    SlotStyle style;
    style.gutterMm = 3.0;
    const auto styled = SlotLayoutEngine::setStyle(page, style);
    QVERIFY(styled.ok());
    page = styled.value();
    QVERIFY(page.frames.isEmpty());

    DocumentSnapshot document;
    document.pages.append(page);
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(document, 0, 150.0);
    if (!rendered.ok())
        QFAIL(qPrintable(rendered.error().message));
    const QImage image = rendered.value();
    for (const QRectF &cell : SlotGeometry::cellsMm(page)) {
        // An empty layout is blank paper: no mat, no placeholder, no tint.
        QVERIFY(isWhite(pixelAtMm(image, 150.0, cell.left() + 1.0, cell.center().y())));
        QVERIFY(isWhite(pixelAtMm(image, 150.0, cell.center().x(), cell.center().y())));
    }
}

void TestSlotLayout::layoutFreeSaveBytesAreStable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = writePhoto(dir, QStringLiteral("one.png"), QColor(90, 90, 90));
    const QString second = writePhoto(dir, QStringLiteral("two.png"), QColor(30, 60, 90));
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    DocumentSnapshot document;
    Page page = pageWith(a4Paper(), false);
    page.frames.append(frameFor(first, QStringLiteral("frame-one")));
    page.frames.append(frameFor(second, QStringLiteral("frame-two")));
    document.pages.append(page);

    const QString path = dir.filePath(QStringLiteral("project.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, document, SaveMode::Normal).ok());
    const auto loaded = store.load(path);
    if (!loaded.ok())
        QFAIL(qPrintable(loaded.error().message));

    const QByteArray firstBytes = ProjectStore::serialize(document, dir.path());
    const QByteArray secondBytes =
        ProjectStore::serialize(loaded.value().document, dir.path());
    // A layout-free project has no new keys and re-serialises byte for byte.
    QCOMPARE(firstBytes, secondBytes);
    QVERIFY(!firstBytes.contains("\"layout\""));
    QVERIFY(!firstBytes.contains("\"slotId\""));
    QVERIFY(!firstBytes.contains("\"marginMm\""));
}

void TestSlotLayout::nanUnitRectIsDropped()
{
    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-nan");
    slot.unitRect = QRectF(0.0, 0.0, std::numeric_limits<double>::quiet_NaN(), 1.0);
    page.slotList.append(slot);
    page.layoutId = QStringLiteral("broken");
    Frame frame = sourceFrame(QStringLiteral("frame-0"), QStringLiteral("/photos/0.png"));
    frame.slotId = slot.id;
    page.frames.append(frame);

    const auto repaired = SlotLayoutEngine::repair(page);
    QVERIFY(repaired.ok());
    const Page &result = repaired.value();
    QVERIFY(!result.hasLayout());
    QVERIFY(result.frames.at(0).slotId.isEmpty());
    QVERIFY(result.layoutId.isEmpty());
    QVERIFY(SlotGeometry::layoutConsistent(result));

    // The strict validator and the clamps agree that this rectangle is unusable.
    QRectF nanRect = slot.unitRect;
    QVERIFY(!SlotGeometry::sanitiseUnitRect(&nanRect));
    QVERIFY(!SlotGeometry::unitRectIsValid(slot.unitRect));
    QVERIFY(SlotGeometry::cellFromUnitRect(slot.unitRect, QSizeF(210.0, 297.0), 10.0, 10.0)
                .isEmpty());
}

void TestSlotLayout::twelveSlotsRoundTrip()
{
    Page page = pageWith(a4Paper(), false);
    for (int i = 0; i < SlotGeometry::kMaxSlotsPerPage; ++i) {
        Slot slot;
        slot.id = QStringLiteral("slot-%1").arg(i);
        slot.unitRect = QRectF((i % 4) * 0.25, (i / 4) * (1.0 / 3.0), 0.25, 1.0 / 3.0);
        page.slotList.append(slot);
        Frame frame = sourceFrame(QStringLiteral("frame-%1").arg(i),
                                  QStringLiteral("/photos/%1.png").arg(i));
        frame.slotId = slot.id;
        page.frames.append(frame);
    }
    const auto repaired = SlotLayoutEngine::repair(page);
    QVERIFY(repaired.ok());
    page = repaired.value();
    QCOMPARE(page.slotList.size(), 12);
    QCOMPARE(page.frames.size(), 12);
    QVERIFY(SlotGeometry::layoutConsistent(page));
    for (const Slot &slot : page.slotList) {
        QVERIFY(SlotGeometry::slotCellMm(page, slot).width() >= 1.0);
        QVERIFY(SlotGeometry::slotCellMm(page, slot).height() >= 1.0);
    }
    const auto refused = SlotLayoutEngine::addSlot(page, QString());
    QVERIFY(!refused.ok());
    QCOMPARE(refused.error().code, QStringLiteral("slot-cap"));

    DocumentSnapshot document;
    document.pages.append(page);
    const QByteArray json = ProjectStore::serialize(document, QString());
    const auto reparsed = ProjectStore::deserialize(json);
    QVERIFY(reparsed.ok());
    const Page &loaded = reparsed.value().pages.at(0);
    QCOMPARE(loaded.slotList.size(), 12);
    QCOMPARE(loaded.frames.size(), 12);
    for (int i = 0; i < 12; ++i) {
        QCOMPARE(loaded.slotList.at(i).id, page.slotList.at(i).id);
        QVERIFY(nearRect(loaded.slotList.at(i).unitRect, page.slotList.at(i).unitRect, 1e-9));
        QCOMPARE(loaded.frames.at(i).slotId, page.frames.at(i).slotId);
        QVERIFY(SlotGeometry::slotGeometryMatches(loaded, loaded.frames.at(i)));
    }
    QVERIFY(SlotGeometry::layoutConsistent(loaded));

    const auto removed = SlotLayoutEngine::removeSlot(page, page.slotList.at(0).id);
    QVERIFY(removed.ok());
    QCOMPARE(removed.value().slotList.size(), 11);
    QCOMPARE(removed.value().frames.size(), 11);
    QVERIFY(SlotGeometry::layoutConsistent(removed.value()));
}

void TestSlotLayout::slotAtAndFirstEmptySlot()
{
    Page page = pageWith(a4Paper(), false);
    Slot left;
    left.id = QStringLiteral("slot-left");
    left.unitRect = QRectF(0.0, 0.0, 0.5, 1.0);
    Slot topRight;
    topRight.id = QStringLiteral("slot-top-right");
    topRight.unitRect = QRectF(0.5, 0.0, 0.5, 0.5);
    Slot bottomRight;
    bottomRight.id = QStringLiteral("slot-bottom-right");
    bottomRight.unitRect = QRectF(0.5, 0.5, 0.5, 0.5);
    page.slotList = {left, topRight, bottomRight};
    Frame inLeft = sourceFrame(QStringLiteral("frame-left"), QStringLiteral("/photos/l.png"));
    inLeft.slotId = left.id;
    page.frames.append(inLeft);
    const auto repaired = SlotLayoutEngine::repair(page);
    QVERIFY(repaired.ok());
    page = repaired.value();

    QCOMPARE(SlotGeometry::firstEmptySlot(page)->id, topRight.id);
    const QRectF leftCell = SlotGeometry::slotCellMm(page, left);
    QCOMPARE(SlotGeometry::slotAt(page, leftCell.center())->id, left.id);
    const QRectF topCell = SlotGeometry::slotCellMm(page, topRight);
    QCOMPARE(SlotGeometry::slotAt(page, topCell.center())->id, topRight.id);
    // The paper margin is not part of any slot.
    QVERIFY(!SlotGeometry::slotAt(page, QPointF(0.5, 0.5)));

    // A later slot wins where two overlap.
    Slot cover;
    cover.id = QStringLiteral("slot-cover");
    cover.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    Page overlapping = page;
    overlapping.slotList.append(cover);
    QCOMPARE(SlotGeometry::slotAt(overlapping, leftCell.center())->id, cover.id);

    Frame inTop = sourceFrame(QStringLiteral("frame-top"), QStringLiteral("/photos/t.png"));
    inTop.slotId = topRight.id;
    Page twoFilled = page;
    twoFilled.frames.append(inTop);
    const auto repairedTwo = SlotLayoutEngine::repair(twoFilled);
    QVERIFY(repairedTwo.ok());
    QCOMPARE(SlotGeometry::firstEmptySlot(repairedTwo.value())->id, bottomRight.id);
}

QTEST_MAIN(TestSlotLayout)
#include "tst_slot_layout.moc"
