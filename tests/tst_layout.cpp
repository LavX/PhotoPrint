#include "LayoutEngine.h"
#include "PhotoItem.h"

#include <QPointF>
#include <QtTest>

#include <cmath>
#include <limits>

using namespace pp;

namespace {

Frame makeFrame(double widthMm, double heightMm, FitMode fit = FitMode::Full)
{
    Frame frame;
    frame.id = QStringLiteral("frame-1");
    frame.cx = 100.0;
    frame.cy = 150.0;
    frame.width = widthMm;
    frame.height = heightMm;
    frame.fit = fit;
    return frame;
}

double distance(const QPointF &a, const QPointF &b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

} // namespace

class TestLayout : public QObject
{
    Q_OBJECT

private slots:
    void fitNeverCrops();
    void fillNeverLeavesBands();
    void quarterTurnsSwapDrawnAxes();
    void oddRotationKeepsImageCentre();
    void inverseRoundTrip();
    void invalidFramesAreRejected();
    void nanRotationAndCropAreRejected();
    void unusableFrameHasUnknownResolution();
    void nanCropCentreIsResetNotPropagated();
    void effectivePpiUsesTheVisibleCrop();
    void resizeKeepsAspectAndFixedCorner();
    void cropBoundsKeepTheFrameCovered();
    void zoomAtKeepsThePointUnderTheCursor();
    void imageZoomAndRotationHelpersClampAndReset();
    void resolutionHintUsesDocumentedThresholds();
    void gridKeepsFourAspectsInsideThePage();
    void appendPhotoDoesNotDisturbManualWork();
    void cropStaysInsideAfterPageRotation();
};

void TestLayout::fitNeverCrops()
{
    const Frame frame = makeFrame(100.0, 100.0, FitMode::Full);
    const QSize pixels(400, 200);
    const QSizeF drawn = LayoutEngine::drawnSizeMm(frame, pixels);
    QVERIFY(drawn.width() <= 100.0 + 1e-9);
    QVERIFY(drawn.height() <= 100.0 + 1e-9);
    QVERIFY(std::abs(std::max(drawn.width(), drawn.height()) - 100.0) < 1e-9);

    const QTransform transform = LayoutEngine::imageToPage(frame, pixels);
    const QPointF centre = transform.map(QPointF(200, 100));
    QVERIFY(distance(centre, QPointF(100.0, 150.0)) < 1e-9);
}

void TestLayout::fillNeverLeavesBands()
{
    const Frame frame = makeFrame(100.0, 100.0, FitMode::Fill);
    const QSize pixels(400, 200);
    const QSizeF drawn = LayoutEngine::drawnSizeMm(frame, pixels);
    QVERIFY(drawn.width() >= 100.0 - 1e-9);
    QVERIFY(drawn.height() >= 100.0 - 1e-9);
    QVERIFY(std::abs(std::min(drawn.width(), drawn.height()) - 100.0) < 1e-9);
}

void TestLayout::quarterTurnsSwapDrawnAxes()
{
    Frame frame = makeFrame(100.0, 100.0, FitMode::Fill);
    const QSize pixels(400, 200);

    frame.imageRotationDeg = 0;
    const QSizeF zero = LayoutEngine::drawnSizeMm(frame, pixels);
    QCOMPARE(zero, QSizeF(200.0, 100.0));

    frame.imageRotationDeg = 90;
    const QSizeF ninety = LayoutEngine::drawnSizeMm(frame, pixels);
    QVERIFY(std::abs(ninety.width() - 100.0) < 1e-9);
    QVERIFY(std::abs(ninety.height() - 200.0) < 1e-9);

    frame.imageRotationDeg = 180;
    const QSizeF oneEighty = LayoutEngine::drawnSizeMm(frame, pixels);
    QVERIFY(std::abs(oneEighty.width() - 200.0) < 1e-9);

    frame.imageRotationDeg = 270;
    const QSizeF twoSeventy = LayoutEngine::drawnSizeMm(frame, pixels);
    QVERIFY(std::abs(twoSeventy.width() - 100.0) < 1e-9);
    QVERIFY(std::abs(twoSeventy.height() - 200.0) < 1e-9);
}

void TestLayout::oddRotationKeepsImageCentre()
{
    Frame frame = makeFrame(100.0, 100.0, FitMode::Fill);
    frame.angleDeg = 17.0;
    frame.imageRotationDeg = 17.0;
    frame.cropCenter = QPointF(0.65, 0.35);
    const QSize pixels(400, 200);

    const QTransform transform = LayoutEngine::imageToPage(frame, pixels);
    // The image centre is not the crop centre; the crop centre must land on
    // the frame centre instead.
    const QPointF cropPixel(0.65 * 400, 0.35 * 200);
    QVERIFY(distance(transform.map(cropPixel), QPointF(frame.cx, frame.cy)) < 1e-9);
}

void TestLayout::inverseRoundTrip()
{
    Frame frame = makeFrame(80.0, 50.0, FitMode::Fill);
    frame.angleDeg = -23.0;
    frame.imageRotationDeg = 90.0;
    frame.zoom = 1.7;
    frame.cropCenter = QPointF(0.3, 0.7);
    const QSize pixels(640, 480);

    const QTransform forward = LayoutEngine::imageToPage(frame, pixels);
    const QTransform backward = LayoutEngine::pageToImage(frame, pixels);
    for (const QPointF &point : {QPointF(0, 0), QPointF(640, 0), QPointF(320, 240),
                                 QPointF(120.5, 400.25)}) {
        const QPointF roundTrip = backward.map(forward.map(point));
        QVERIFY2(distance(roundTrip, point) < 1e-6,
                 qPrintable(QStringLiteral("round trip error %1").arg(distance(roundTrip, point))));
    }
}

void TestLayout::invalidFramesAreRejected()
{
    const QSize pixels(400, 200);
    QList<Frame> frames;
    Frame zero = makeFrame(0.0, 100.0);
    frames << zero;
    Frame negative = makeFrame(-10.0, 100.0);
    frames << negative;
    Frame infinite = makeFrame(100.0, std::numeric_limits<double>::infinity());
    frames << infinite;
    Frame nan = makeFrame(100.0, std::numeric_limits<double>::quiet_NaN());
    frames << nan;

    for (const Frame &frame : frames) {
        QVERIFY(!LayoutEngine::validFrame(frame));
        QCOMPARE(LayoutEngine::baseScale(frame, pixels), 0.0);
        // A rejected frame must not masquerade as a valid identity mapping.
        QVERIFY(!LayoutEngine::imageToPage(frame, pixels).isInvertible());
        QVERIFY(!LayoutEngine::pageToImage(frame, pixels).isInvertible());
    }

    // A usable frame with no pixels is equally unmappable.
    const Frame usable = makeFrame(100.0, 100.0);
    QVERIFY(!LayoutEngine::imageToPage(usable, QSize(0, 0)).isInvertible());
    QVERIFY(LayoutEngine::imageToPage(usable, pixels).isInvertible());
}

void TestLayout::nanRotationAndCropAreRejected()
{
    const QSize pixels(400, 200);

    Frame rotated = makeFrame(100.0, 100.0);
    rotated.imageRotationDeg = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!LayoutEngine::validFrame(rotated));
    QCOMPARE(LayoutEngine::baseScale(rotated, pixels), 0.0);
    QVERIFY(!LayoutEngine::imageToPage(rotated, pixels).isInvertible());

    Frame infiniteCrop = makeFrame(100.0, 100.0);
    infiniteCrop.cropCenter = QPointF(std::numeric_limits<double>::infinity(), 0.5);
    QVERIFY(!LayoutEngine::validFrame(infiniteCrop));

    Frame nanCrop = makeFrame(100.0, 100.0);
    nanCrop.cropCenter = QPointF(0.5, std::numeric_limits<double>::quiet_NaN());
    QVERIFY(!LayoutEngine::validFrame(nanCrop));

    // The failure must not look like a valid mapping: identity would be
    // invertible and would map points to plausible page coordinates.
    const QTransform forward = LayoutEngine::imageToPage(nanCrop, pixels);
    QVERIFY(!forward.isInvertible());
    QVERIFY(forward != QTransform());
    QVERIFY(!LayoutEngine::pageToImage(nanCrop, pixels).isInvertible());
}

void TestLayout::unusableFrameHasUnknownResolution()
{
    const QSize pixels(400, 200);

    const Frame zeroWidth = makeFrame(0.0, 100.0);
    QCOMPARE(LayoutEngine::effectivePpi(zeroWidth, pixels), 0.0);
    QCOMPARE(LayoutEngine::resolutionHint(zeroWidth, pixels),
             LayoutEngine::ResolutionHint::Unknown);

    Frame nanRotation = makeFrame(100.0, 50.0, FitMode::Fill);
    nanRotation.imageRotationDeg = std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(LayoutEngine::effectivePpi(nanRotation, pixels), 0.0);
    QCOMPARE(LayoutEngine::resolutionHint(nanRotation, pixels),
             LayoutEngine::ResolutionHint::Unknown);

    // No pixels is also unknown, never "good".
    const Frame frame = makeFrame(100.0, 50.0, FitMode::Fill);
    QCOMPARE(LayoutEngine::effectivePpi(frame, QSize(0, 200)), 0.0);
    QCOMPARE(LayoutEngine::resolutionHint(frame, QSize()), LayoutEngine::ResolutionHint::Unknown);
}

void TestLayout::nanCropCentreIsResetNotPropagated()
{
    const QSize pixels(400, 200);

    // std::clamp lets NaN through, so the guard has to happen before it.
    Frame frame = makeFrame(100.0, 100.0, FitMode::Fill);
    frame.cropCenter = QPointF(std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity());
    const Frame clamped = PhotoItem::clampCrop(frame, pixels);
    QCOMPARE(clamped.cropCenter, QPointF(0.5, 0.5));

    Frame nanAngle = makeFrame(100.0, 100.0, FitMode::Fill);
    nanAngle.angleDeg = std::numeric_limits<double>::quiet_NaN();
    const Frame angleClamped = PhotoItem::clampCrop(nanAngle, pixels);
    QVERIFY(std::isfinite(angleClamped.cropCenter.x()));
    QVERIFY(std::isfinite(angleClamped.cropCenter.y()));

    Frame zoomed = makeFrame(100.0, 60.0, FitMode::Fill);
    zoomed.cropCenter = QPointF(0.4, 0.6);
    QCOMPARE(PhotoItem::zoomAt(zoomed,
                               QPointF(std::numeric_limits<double>::quiet_NaN(), 145.0), 1.5,
                               pixels),
             zoomed);

    Frame nanZoom = zoomed;
    nanZoom.zoom = std::numeric_limits<double>::quiet_NaN();
    const Frame recovered = PhotoItem::zoomAt(nanZoom, QPointF(120.0, 145.0), 1.5, pixels);
    QVERIFY(std::isfinite(recovered.zoom));
    QVERIFY(recovered.zoom > 1.0);
    QVERIFY(std::isfinite(recovered.cropCenter.x()));
    QVERIFY(std::isfinite(recovered.cropCenter.y()));
}

void TestLayout::effectivePpiUsesTheVisibleCrop()
{
    const Frame frame = makeFrame(100.0, 100.0, FitMode::Fill);
    const QSize pixels(400, 200);
    // Fill scale is 0.5 mm/px, so 100 mm shows 200 source pixels.
    const double expected = 200.0 / (100.0 / 25.4);
    QVERIFY(std::abs(LayoutEngine::effectivePpi(frame, pixels) - expected) < 1e-6);
}

void TestLayout::resizeKeepsAspectAndFixedCorner()
{
    Frame frame = makeFrame(100.0, 50.0, FitMode::Fill);
    frame.angleDeg = 0.0;
    const QPointF fixedBefore = PhotoItem::cornerMm(frame, 0); // top-left stays
    const QPointF target(180.0, 200.0);
    const Frame resized = PhotoItem::resizedFromCorner(frame, 2, target, true);

    const double ratioBefore = frame.width / frame.height;
    const double ratioAfter = resized.width / resized.height;
    QVERIFY(std::abs(ratioBefore - ratioAfter) < 1e-9);

    const QPointF fixedAfter = PhotoItem::cornerMm(resized, 0);
    QVERIFY(distance(fixedBefore, fixedAfter) < 1e-9);
    QVERIFY(resized.width > frame.width);
}

void TestLayout::cropBoundsKeepTheFrameCovered()
{
    Frame frame = makeFrame(100.0, 100.0, FitMode::Fill);
    const QSize pixels(400, 200);
    frame.cropCenter = QPointF(0.99, -0.4);
    frame = PhotoItem::clampCrop(frame, pixels);

    const QSizeF image = LayoutEngine::imageSizeMm(frame, pixels);
    const double halfX = 50.0 / image.width();
    QVERIFY(frame.cropCenter.x() >= halfX - 1e-9);
    QVERIFY(frame.cropCenter.x() <= 1.0 - halfX + 1e-9);
    QCOMPARE(frame.cropCenter.y(), 0.5);
}

void TestLayout::zoomAtKeepsThePointUnderTheCursor()
{
    Frame frame = makeFrame(100.0, 60.0, FitMode::Fill);
    const QSize pixels(800, 400);
    frame.zoom = 1.0;
    const QPointF pagePoint(120.0, 145.0);

    auto normalizedUnderCursor = [&](const Frame &candidate) {
        const QPointF pixel = LayoutEngine::pageToImage(candidate, pixels).map(pagePoint);
        return QPointF(pixel.x() / pixels.width(), pixel.y() / pixels.height());
    };

    const QPointF before = normalizedUnderCursor(frame);
    const Frame zoomed = PhotoItem::zoomAt(frame, pagePoint, 1.5, pixels);
    const QPointF after = normalizedUnderCursor(zoomed);
    QVERIFY(zoomed.zoom > frame.zoom);
    QVERIFY2(distance(before, after) < 1e-6,
             qPrintable(QStringLiteral("cursor drift %1").arg(distance(before, after))));
}

void TestLayout::imageZoomAndRotationHelpersClampAndReset()
{
    const QSize pixels(800, 400);
    const Frame original = makeFrame(100.0, 60.0, FitMode::Fill);

    const Frame zoomed = PhotoItem::setZoom(original, 2.5, pixels);
    QCOMPARE(zoomed.zoom, 2.5);
    // The frame rectangle belongs to the page, never to the image controls.
    QCOMPARE(zoomed.cx, original.cx);
    QCOMPARE(zoomed.cy, original.cy);
    QCOMPARE(zoomed.width, original.width);
    QCOMPARE(zoomed.height, original.height);
    QCOMPARE(zoomed.angleDeg, original.angleDeg);

    QCOMPARE(PhotoItem::setZoom(original, 99.0, pixels).zoom, kMaxFrameZoom);
    QCOMPARE(PhotoItem::setZoom(original, 0.01, pixels).zoom, kMinFrameZoom);
    QCOMPARE(PhotoItem::setZoom(original, std::numeric_limits<double>::quiet_NaN(), pixels),
             original);

    const Frame turned = PhotoItem::rotateImageTo(original, 45.0, pixels);
    QCOMPARE(turned.imageRotationDeg, 45.0);
    QCOMPARE(turned.width, original.width);
    QCOMPARE(turned.angleDeg, original.angleDeg);
    // The slider clamps instead of wrapping, so its ends are exact values.
    QCOMPARE(PhotoItem::rotateImageTo(original, 999.0, pixels).imageRotationDeg, 180.0);
    QCOMPARE(PhotoItem::rotateImageTo(original, -999.0, pixels).imageRotationDeg, -180.0);
    QCOMPARE(PhotoItem::rotateImageTo(original, std::numeric_limits<double>::quiet_NaN(),
                                      pixels),
             original);

    // Gestures wrap inside the same range: a quarter turn from 135 is -135.
    QCOMPARE(PhotoItem::rotateImageBy(original, 90.0, pixels).imageRotationDeg, 90.0);
    Frame nearEdge = original;
    nearEdge.imageRotationDeg = 135.0;
    QCOMPARE(PhotoItem::rotateImageBy(nearEdge, 90.0, pixels).imageRotationDeg, -135.0);
    Frame atEdge = original;
    atEdge.imageRotationDeg = -135.0;
    QCOMPARE(PhotoItem::rotateImageBy(atEdge, -90.0, pixels).imageRotationDeg, 135.0);

    Frame worked = original;
    worked.zoom = 4.0;
    worked.imageRotationDeg = -30.0;
    worked.cropCenter = QPointF(0.31, 0.62);
    const Frame reset = PhotoItem::resetInsideFrame(worked);
    QCOMPARE(reset.zoom, 1.0);
    QCOMPARE(reset.imageRotationDeg, 0.0);
    QCOMPARE(reset.cropCenter, QPointF(0.5, 0.5));
    // Fit is the user's choice and survives a reset.
    QCOMPARE(reset.fit, worked.fit);
    QCOMPARE(reset.width, worked.width);
    QCOMPARE(reset.height, worked.height);
}

void TestLayout::resolutionHintUsesDocumentedThresholds()
{
    const Frame frame = makeFrame(100.0, 50.0, FitMode::Fill);
    QCOMPARE(LayoutEngine::resolutionHint(frame, QSize(400, 200)),
             LayoutEngine::ResolutionHint::Low);
    QCOMPARE(LayoutEngine::resolutionHint(frame, QSize(800, 400)),
             LayoutEngine::ResolutionHint::Soft);
    QCOMPARE(LayoutEngine::resolutionHint(frame, QSize(1200, 600)),
             LayoutEngine::ResolutionHint::Good);
}

void TestLayout::gridKeepsFourAspectsInsideThePage()
{
    Page page;
    page.id = QStringLiteral("page");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;

    const QList<QSize> sizes = {QSize(600, 400), QSize(400, 600), QSize(500, 500),
                                QSize(800, 450)};
    QList<Frame> frames;
    QHash<QString, QSize> pixelSizes;
    for (int i = 0; i < sizes.size(); ++i) {
        Frame frame;
        frame.id = QStringLiteral("frame-%1").arg(i);
        frame.fit = FitMode::Full;
        frames.append(frame);
        pixelSizes.insert(frame.id, sizes.at(i));
    }

    const QList<Frame> arranged = LayoutEngine::arrangeGrid(page, frames, pixelSizes);
    QCOMPARE(arranged.size(), 4);
    const QSizeF pageSize = pageSizeMm(page);
    QList<QRectF> bounds;
    for (int i = 0; i < arranged.size(); ++i) {
        const Frame &frame = arranged.at(i);
        QVERIFY(frame.autoPlaced);
        QCOMPARE(frame.angleDeg, 0.0);
        const double ratio = static_cast<double>(sizes.at(i).height()) / sizes.at(i).width();
        QVERIFY(std::abs(frame.height / frame.width - ratio) < 1e-9);
        QVERIFY(frame.cx - frame.width / 2.0 >= 9.9);
        QVERIFY(frame.cy - frame.height / 2.0 >= 9.9);
        QVERIFY(frame.cx + frame.width / 2.0 <= pageSize.width() - 9.9);
        QVERIFY(frame.cy + frame.height / 2.0 <= pageSize.height() - 9.9);
        bounds.append(QRectF(frame.cx - frame.width / 2.0, frame.cy - frame.height / 2.0,
                             frame.width, frame.height));
    }
    for (int a = 0; a < bounds.size(); ++a) {
        for (int b = a + 1; b < bounds.size(); ++b)
            QVERIFY2(!bounds.at(a).intersects(bounds.at(b)),
                     qPrintable(QStringLiteral("frames %1 and %2 overlap").arg(a).arg(b)));
    }
}

void TestLayout::appendPhotoDoesNotDisturbManualWork()
{
    Page page;
    page.id = QStringLiteral("page");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;

    Frame manual = makeFrame(50.0, 40.0, FitMode::Fill);
    manual.id = QStringLiteral("manual");
    manual.cx = 40.0;
    manual.cy = 50.0;
    manual.autoPlaced = false;
    Frame other = makeFrame(30.0, 30.0);
    other.id = QStringLiteral("other");
    other.cx = 150.0;
    other.cy = 200.0;

    Frame added = makeFrame(40.0, 20.0);
    added.id = QStringLiteral("added");

    QHash<QString, QSize> sizes;
    sizes.insert(manual.id, QSize(500, 400));
    sizes.insert(other.id, QSize(300, 300));
    sizes.insert(added.id, QSize(400, 200));

    const QList<Frame> result =
        LayoutEngine::appendPhoto(page, {manual, other}, added, sizes);
    QCOMPARE(result.size(), 3);
    QCOMPARE(result.at(0), manual);
    QCOMPARE(result.at(1), other);
    QCOMPARE(result.at(2), added);
}

void TestLayout::cropStaysInsideAfterPageRotation()
{
    Frame frame = makeFrame(50.0, 50.0, FitMode::Fill);
    frame.angleDeg = 90.0; // a page turned landscape adds this
    frame.imageRotationDeg = 0.0;
    frame.zoom = 1.0;
    frame.cropCenter = QPointF(0.99, 0.02);
    const QSize pixels(400, 200);
    frame = PhotoItem::clampCrop(frame, pixels);

    const QPolygonF quad = PhotoItem::frameQuadMm(frame);
    const QTransform inverse = LayoutEngine::pageToImage(frame, pixels);
    for (const QPointF &corner : quad) {
        const QPointF pixel = inverse.map(corner);
        const double nx = pixel.x() / pixels.width();
        const double ny = pixel.y() / pixels.height();
        QVERIFY2(nx >= -1e-6 && nx <= 1.0 + 1e-6,
                 qPrintable(QStringLiteral("x %1 outside the photograph").arg(nx)));
        QVERIFY2(ny >= -1e-6 && ny <= 1.0 + 1e-6,
                 qPrintable(QStringLiteral("y %1 outside the photograph").arg(ny)));
    }
}

QTEST_GUILESS_MAIN(TestLayout)
#include "tst_layout.moc"
