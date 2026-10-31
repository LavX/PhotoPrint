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
    void loaderSanitisesBrokenLayouts();

    void oneEditPerAction();
    void validatorRefusesBrokenSlotEdits();

    void emptySlotMatRenders();
    void borderColourIsLinearNotSrgb();
    void roundedCornersClipThePhoto();
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
    style.borderMm = 500.0;
    style.cornerRadiusMm = 500.0;
    style.borderColor = QColor(); // invalid
    const SlotStyle sane = SlotGeometry::sanitiseStyle(style);
    QCOMPARE(sane.marginMm, SlotGeometry::kMaxMarginMm);
    QCOMPARE(sane.gutterMm, SlotGeometry::kMaxGutterMm);
    QCOMPARE(sane.borderMm, SlotGeometry::kMaxBorderMm);
    QCOMPARE(sane.cornerRadiusMm, SlotGeometry::kMaxCornerRadiusMm);
    QCOMPARE(sane.borderColor, QColor(255, 255, 255));

    SlotStyle broken;
    broken.marginMm = std::numeric_limits<double>::quiet_NaN();
    broken.gutterMm = std::numeric_limits<double>::infinity();
    broken.borderMm = -std::numeric_limits<double>::infinity();
    broken.cornerRadiusMm = std::numeric_limits<double>::quiet_NaN();
    const SlotStyle defaults = SlotGeometry::sanitiseStyle(broken);
    QCOMPARE(defaults.marginMm, 0.0);
    QCOMPARE(defaults.gutterMm, 0.0);
    QCOMPARE(defaults.borderMm, 0.0);
    QCOMPARE(defaults.cornerRadiusMm, 0.0);

    // A border can never eat more than 45 % of the shorter side.
    const QRectF cell(0.0, 0.0, 20.0, 20.0);
    QVERIFY(nearRect(SlotGeometry::photoRectMm(cell, 50.0), QRectF(9.0, 9.0, 2.0, 2.0)));
    QVERIFY(nearRect(SlotGeometry::photoRectMm(cell, 3.0), QRectF(3.0, 3.0, 14.0, 14.0)));
    QVERIFY(nearRect(SlotGeometry::photoRectMm(cell,
                                               std::numeric_limits<double>::quiet_NaN()),
                     cell));

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
    page.slotStyle.borderMm = 500.0;
    page.slotStyle.cornerRadiusMm = 500.0;

    const QRectF cell = SlotGeometry::slotCellMm(page, slot);
    QVERIFY(nearRect(cell, QRectF(48.0, 48.0, 4.0, 54.0)));
    const QRectF photo = SlotGeometry::slotPhotoRectMm(page, slot);
    QVERIFY(photo.width() > 0.0 && photo.height() > 0.0);
    QVERIFY(near(photo.width(), 0.4));
    QVERIFY(photo.left() >= cell.left() - 1e-9);
    QVERIFY(photo.right() <= cell.right() + 1e-9);
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
    style.borderMm = 3.0;
    style.borderColor = QColor(20, 40, 60);
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
    style.borderMm = 3.5;
    style.borderColor = QColor(51, 102, 153);
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
    QCOMPARE(restoredLaidOut.slotStyle.marginMm, 8.0);
    QCOMPARE(restoredLaidOut.slotStyle.gutterMm, 4.0);
    QCOMPARE(restoredLaidOut.slotStyle.borderMm, 3.5);
    QCOMPARE(restoredLaidOut.slotStyle.borderColor, QColor(51, 102, 153));
    QCOMPARE(restoredLaidOut.slotStyle.cornerRadiusMm, 2.0);
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
        styleObject.insert(QStringLiteral("borderMm"), 5.0);
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
        styleObject.insert(QStringLiteral("borderMm"), 4.0);
        styleObject.insert(QStringLiteral("borderColor"), QStringLiteral("#123456"));
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
    style.borderMm = 4.0;
    style.borderColor = QColor(10, 20, 30);
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

void TestSlotLayout::emptySlotMatRenders()
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
    style.borderMm = 4.0;
    style.borderColor = QColor(0, 0, 0);
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
        const QRgb mat = pixelAtMm(image, 150.0, cell.left() + 1.0, cell.center().y());
        QVERIFY2(isNear(mat, 0, 0, 0, 12),
                 qPrintable(QStringLiteral("empty slot %1 mat is %2,%3,%4")
                                .arg(index)
                                .arg(qRed(mat))
                                .arg(qGreen(mat))
                                .arg(qBlue(mat))));
        const QRgb inside = pixelAtMm(image, 150.0, cell.center().x(), cell.center().y());
        QVERIFY2(isWhite(inside), "an empty slot printed something other than paper");
    }
    // A filled slot prints its photograph, not the mat, in the middle.
    const QRgb filled = pixelAtMm(image, 150.0, cells.at(0).center().x(),
                                  cells.at(0).center().y());
    QVERIFY(!isWhite(filled));
    QVERIFY(std::abs(qRed(filled) - 128) <= 25);
    // The sheet itself stays paper white outside the layout.
    QVERIFY(isWhite(pixelAtMm(image, 150.0, 1.0, 1.0)));
}

void TestSlotLayout::borderColourIsLinearNotSrgb()
{
    // #808080 written straight into the linear canvas would come back around
    // 188 after the sRGB conversion. Converted into the working space it comes
    // back as the grey the user picked.
    Page page = pageWith(a4Paper(), false);
    Slot slot;
    slot.id = QStringLiteral("slot-only");
    slot.unitRect = QRectF(0.0, 0.0, 1.0, 1.0);
    page.slotList.append(slot);
    page.layoutId = QStringLiteral("hand");
    SlotStyle style;
    style.borderMm = 6.0;
    style.borderColor = QColor(128, 128, 128);
    page.slotStyle = style;
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
    const QRgb mat = pixelAtMm(image, 150.0, cell.left() + 3.0, cell.center().y());
    QVERIFY2(isNear(mat, 128, 128, 128, 5),
             qPrintable(QStringLiteral("mat printed as %1,%2,%3")
                            .arg(qRed(mat))
                            .arg(qGreen(mat))
                            .arg(qBlue(mat))));
    QVERIFY(qRed(mat) < 150);
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
    // is inside it and prints the photograph.
    QVERIFY(isWhite(pixelAtMm(image, 150.0, cell.left() + 2.0, cell.top() + 2.0)));
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.left() + 9.0, cell.top() + 9.0)));
    QVERIFY(isBluish(pixelAtMm(image, 150.0, cell.center().x(), cell.center().y())));
}

QTEST_MAIN(TestSlotLayout)
#include "tst_slot_layout.moc"
