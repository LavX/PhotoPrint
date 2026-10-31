#include "ColorPipeline.h"
#include "Document.h"
#include "MainWindow.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QRgba64>
#include <QThread>
#include <QWheelEvent>
#include <QtTest>
#include <QtMath>

#include <cstdlib>
#include <stop_token>

using namespace pp;

namespace {

// Must match the retained raster ceiling in PageView.
constexpr qint64 kMaxRasterPixels = 8000000;

DocumentSnapshot pageWithFrames(int count)
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-a");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    for (int i = 0; i < count; ++i) {
        Frame frame;
        frame.id = QStringLiteral("frame-%1").arg(i);
        frame.cx = 60.0 + (i % 2) * 90.0;
        frame.cy = 70.0 + (i / 2) * 80.0;
        frame.width = 80.0;
        frame.height = 55.0;
        frame.fit = FitMode::Fill;
        page.frames.append(frame);
    }
    document.pages.append(page);
    return document;
}

/// One red frame over the centre of the page, so a pixel at the raster centre
/// says which document the raster shows.
DocumentSnapshot centredFrame()
{
    DocumentSnapshot document = pageWithFrames(0);
    Frame frame;
    frame.id = QStringLiteral("frame-centre");
    frame.cx = 105.0;
    frame.cy = 148.5;
    frame.width = 120.0;
    frame.height = 90.0;
    frame.fit = FitMode::Fill;
    document.pages[0].frames.append(frame);
    return document;
}

QHash<QString, QImage> preparedImages(const DocumentSnapshot &document)
{
    QHash<QString, QImage> images;
    for (const Frame &frame : document.pages.first().frames) {
        QImage image(400, 300, QImage::Format_RGB32);
        image.fill(QColor(200, 40, 40));
        images.insert(frame.id, image);
    }
    return images;
}

QSize cappedSize(QSize size)
{
    const qint64 area = static_cast<qint64>(size.width()) * size.height();
    if (area <= kMaxRasterPixels)
        return size;
    const double factor = std::sqrt(static_cast<double>(kMaxRasterPixels) / static_cast<double>(area));
    return QSize(std::max(1, static_cast<int>(size.width() * factor)),
                 std::max(1, static_cast<int>(size.height() * factor)));
}

/// The raster size PageView targets for the current view, cap included.
QSize targetRasterSize(const PageView &view)
{
    const QRectF page = view.pageRect();
    if (page.isEmpty())
        return {};
    const double dpr = view.devicePixelRatioF();
    return cappedSize(QSize(std::max(1, qRound(page.width() * dpr)),
                            std::max(1, qRound(page.height() * dpr))));
}

/// The fitted (100%) raster size: what a drag is allowed to compose.
QSize fittedRasterSize(const PageView &view)
{
    const QSize target = targetRasterSize(view);
    if (view.viewZoom() <= 1.0)
        return target;
    return QSize(std::max(1, qRound(target.width() / view.viewZoom())),
                 std::max(1, qRound(target.height() / view.viewZoom())));
}

QPointF pageToWidget(const PageView &view, const QPointF &mm)
{
    const QRectF rect = view.pageRect();
    const QSizeF page = view.pageMm();
    return QPointF(rect.left() + mm.x() / page.width() * rect.width(),
                   rect.top() + mm.y() / page.height() * rect.height());
}

QByteArray rasterHash(const QImage &image)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y) {
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(y)),
                                    image.bytesPerLine()));
    }
    return hash.result();
}

bool closeTo(QSize actual, QSize expected, int tolerance)
{
    return std::abs(actual.width() - expected.width()) <= tolerance
        && std::abs(actual.height() - expected.height()) <= tolerance;
}

/// A linear premultiplied page-like image. `alphaMode` 0 is opaque, 1 fully
/// transparent, 2 a varying alpha.
QImage linearPattern(int width, int height, int alphaMode)
{
    QImage image(width, height, QImage::Format_RGBA64_Premultiplied);
    for (int y = 0; y < height; ++y) {
        QRgba64 *row = reinterpret_cast<QRgba64 *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            quint16 alpha = 65535;
            if (alphaMode == 1)
                alpha = 0;
            else if (alphaMode == 2)
                alpha = static_cast<quint16>((x * 257 + y * 977) % 65536);
            const quint32 share = alpha;
            const quint16 red = static_cast<quint16>(
                (static_cast<quint32>((x * 40503 + y * 7919) % 65536) * share) / 65535);
            const quint16 green = static_cast<quint16>(
                (static_cast<quint32>((x * 12289 + y * 23531) % 65536) * share) / 65535);
            const quint16 blue = static_cast<quint16>(
                (static_cast<quint32>((x * 7919 + y * 40503) % 65536) * share) / 65535);
            row[x] = qRgba64(red, green, blue, alpha);
        }
    }
    return image;
}

void sendWheel(PageView &view, const QPointF &position, int angle)
{
    QWheelEvent event(position, QPointF(view.mapToGlobal(position.toPoint())), QPoint(),
                      QPoint(0, angle), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&view, &event);
}

/// Waits for the raster the current document needs. composedImage() blocks on a
/// running build and starts one if the content is stale, which is exactly what a
/// test wants and the paint path never does.
void settle(PageView &view)
{
    const QImage raster = view.composedImage();
    Q_UNUSED(raster)
    QTest::qWait(20);
}

} // namespace

class TestCanvasCache : public QObject
{
    Q_OBJECT

private slots:
    void panKeepsTheComposedRaster();
    void heldMiddlePanNeverComposes();
    void zoomRecomposesOnceAfterTheDebounce();
    void documentChangeRecomposesOnce();
    void identicalInputsAreIgnored();
    void dragComposesAtTheFittedScaleAndSettlesOnRelease();
    void unrelatedRepaintsDoNotPostponeTheSettle();
    void zoomOutKeepsTheSharpRaster();
    void largeViewportStaysInsideTheCap();
    void cropPanUsesTheFittedScale();
    void cropWheelComposesAtFittedScale();
    void nudgeBurstBuildsOnceAfterThePulse();
    void composedPixelsFollowTheDocument();
    void goldenRasterAtRestIsStable();
    void failedComposeDoesNotLoop();
    void blankCanvasGetsExactlyOneRetry();
    void inFlightBuildIsSupersededByANewDocument();
    void inFlightBuildIsSupersededByAPaperChange();
    void lostMouseUpResetsTheGesture();
    void parallelConversionMatchesSerial();
};

void TestCanvasCache::panKeepsTheComposedRaster()
{
    const DocumentSnapshot document = pageWithFrames(4);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.setViewZoom(2.0);
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(250); // nothing may be pending during the pan
    const quint64 installed = view.compositionCount();
    const quint64 requested = view.compositionRequestCount();
    const QImage first = view.composedImage();
    QVERIFY(!first.isNull());
    QVERIFY(first.width() > 800); // a 200% page, not the fitted one

    QTest::mousePress(&view, Qt::MiddleButton, Qt::NoModifier, QPoint(450, 350));
    for (int step = 0; step < 20; ++step) {
        QTest::mouseMove(&view, QPoint(450 + step * 4, 350 + step * 2));
        QTest::qWait(10);
    }
    QTest::mouseRelease(&view, Qt::MiddleButton, Qt::NoModifier, QPoint(526, 388));
    QTest::qWait(250);

    QCOMPARE(view.compositionCount(), installed);
    QCOMPARE(view.compositionRequestCount(), requested);
    QCOMPARE(view.composedImage(), first);
}

void TestCanvasCache::heldMiddlePanNeverComposes()
{
    // The regression: a held pan used to let the 120 ms settle fire mid-pan and
    // compose at full scale under the button.
    const DocumentSnapshot document = pageWithFrames(4);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);

    // Grab the page, then zoom while the button is held: the settle is armed by
    // the zoom and must be suppressed for as long as the pan lasts.
    const quint64 installed = view.compositionCount();
    const quint64 requested = view.compositionRequestCount();
    QTest::mousePress(&view, Qt::MiddleButton, Qt::NoModifier, QPoint(450, 350));
    view.setViewZoom(2.0);
    for (int step = 0; step < 20; ++step) {
        QTest::mouseMove(&view, QPoint(450 + step * 3, 350 + step));
        QTest::qWait(20); // 400 ms in total, far past the 120 ms debounce
    }
    QCOMPARE(view.compositionCount(), installed);
    QCOMPARE(view.compositionRequestCount(), requested);
    QVERIFY(!view.compositionPending());

    QTest::mouseRelease(&view, Qt::MiddleButton, Qt::NoModifier, QPoint(507, 369));
    // The release arms exactly one settle, which sharpens to the 200% target.
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), installed + 1, 15000);
    QTest::qWait(250);
    QCOMPARE(view.compositionCount(), installed + 1);
    QCOMPARE(view.composedImage().size(), targetRasterSize(view));
}

void TestCanvasCache::zoomRecomposesOnceAfterTheDebounce()
{
    const DocumentSnapshot document = pageWithFrames(4);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(200);
    const quint64 before = view.compositionCount();
    const QSize fitted = targetRasterSize(view);

    // A burst of notches is what the wheel really sends.
    view.setViewZoom(2.0);
    QTest::qWait(20);
    view.setViewZoom(2.2);
    view.setViewZoom(2.0);
    QTest::qWait(30); // still inside the 120 ms debounce
    QCOMPARE(view.compositionCount(), before);

    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 15000);
    QTest::qWait(250);
    QCOMPARE(view.compositionCount(), before + 1);
    const QImage settled = view.composedImage();
    QCOMPARE(settled.size(), targetRasterSize(view));
    QVERIFY(settled.width() > fitted.width() * 1.9);
}

void TestCanvasCache::documentChangeRecomposesOnce()
{
    const DocumentSnapshot document = pageWithFrames(2);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(150);
    const quint64 before = view.compositionCount();
    const quint64 requests = view.compositionRequestCount();

    DocumentSnapshot moved = document;
    moved.pages[0].frames[0].cx += 12.0;
    view.setDocument(moved);
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 15000);
    QCOMPARE(view.compositionRequestCount(), requests + 1);

    // Reading the raster again, and repainting, must not compose a third time.
    view.composedImage();
    view.repaint();
    QTest::qWait(250);
    QCOMPARE(view.compositionCount(), before + 1);
    QCOMPARE(view.compositionRequestCount(), requests + 1);
}

void TestCanvasCache::identicalInputsAreIgnored()
{
    const DocumentSnapshot document = pageWithFrames(3);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(150);
    const quint64 before = view.compositionCount();
    const quint64 requests = view.compositionRequestCount();

    // The same snapshot and an equal (but freshly built) image set: nothing on
    // the page changed, so the raster must survive.
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    QTest::qWait(100);
    QCOMPARE(view.compositionCount(), before);
    QCOMPARE(view.compositionRequestCount(), requests);

    // A real change still gets through.
    DocumentSnapshot changed = document;
    changed.pages[0].frames[0].angleDeg = 5.0;
    view.setDocument(changed);
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 15000);
}

void TestCanvasCache::dragComposesAtTheFittedScaleAndSettlesOnRelease()
{
    const DocumentSnapshot document = pageWithFrames(1);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.setViewZoom(2.0);
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(250);
    const quint64 before = view.compositionCount();
    const QSize target = targetRasterSize(view);
    const QSize fitted = fittedRasterSize(view);
    QVERIFY(target.width() > 800);
    QVERIFY(fitted.width() * 2 <= target.width() + 2);

    const Frame frame = document.pages.first().frames.first();
    const QPointF start = pageToWidget(view, QPointF(frame.cx, frame.cy));
    const QPointF end = start + QPointF(24.0, 12.0);
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start.toPoint());
    QTest::mouseMove(&view, end.toPoint());
    view.repaint();
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 15000);
    QVERIFY(!view.compositionPending());

    // Mid-drag the page is composed at the fitted scale, never at 200%.
    const QImage coarse = view.composedImage();
    QVERIFY2(closeTo(coarse.size(), fitted, 2),
             qPrintable(QStringLiteral("drag raster %1x%2, fitted %3x%4")
                            .arg(coarse.width()).arg(coarse.height())
                            .arg(fitted.width()).arg(fitted.height())));

    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, end.toPoint());
    // The release schedules the full-scale rebuild instead of composing inline.
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 2, 15000);
    settle(view);
    QCOMPARE(view.composedImage().size(), target);
}

void TestCanvasCache::unrelatedRepaintsDoNotPostponeTheSettle()
{
    // The debounce belongs to the zoom, not to the paint: a stream of unrelated
    // repaints must not keep the sharpening pass from landing.
    const DocumentSnapshot document = pageWithFrames(4);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(200);
    const quint64 before = view.compositionCount();

    view.setViewZoom(2.0);
    for (int pass = 0; pass < 15; ++pass) { // 600 ms of repaints, past the debounce
        view.repaint();
        QTest::qWait(40);
    }
    QCOMPARE(view.compositionCount(), before + 1);
    QCOMPARE(view.composedImage().size(), targetRasterSize(view));
}

void TestCanvasCache::zoomOutKeepsTheSharpRaster()
{
    const DocumentSnapshot document = pageWithFrames(4);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(200);

    const quint64 start = view.compositionCount();
    view.setViewZoom(2.2);
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), start + 1, 15000);
    QTest::qWait(250);
    const QImage sharp = view.composedImage();
    const quint64 built = view.compositionCount();

    // 2.2 -> 1.4 leaves a raster 1.57x the target: inside the documented band,
    // so the sharper raster is kept and nothing is rebuilt.
    view.setViewZoom(1.4);
    QTest::qWait(400);
    QCOMPARE(view.compositionCount(), built);
    QCOMPARE(view.composedImage(), sharp);

    // 2.2 -> 1.0 leaves 2.2x, outside the band: that one is rebuilt.
    view.setViewZoom(1.0);
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), built + 1, 15000);
    settle(view);
    QCOMPARE(view.composedImage().size(), targetRasterSize(view));
}

void TestCanvasCache::largeViewportStaysInsideTheCap()
{
    const DocumentSnapshot document = pageWithFrames(2);
    PageView view;
    view.resize(2600, 2000);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(150);

    const double widthAtFit = view.pageRect().width();
    view.setViewZoom(3.0);
    // On a HiDPI screen the fitted page is already over the cap, so the zoom
    // cannot raise the target any further. Force one build and inspect it.
    DocumentSnapshot changed = document;
    changed.pages[0].frames[0].cx += 4.0;
    const quint64 before = view.compositionCount();
    view.setDocument(changed);
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 30000);
    settle(view);

    const QImage composed = view.composedImage();
    QVERIFY(!composed.isNull());
    const qint64 pixels = static_cast<qint64>(composed.width()) * composed.height();
    QVERIFY2(pixels <= kMaxRasterPixels,
             qPrintable(QStringLiteral("raster %1 x %2")
                            .arg(composed.width()).arg(composed.height())));
    QVERIFY(pixels > kMaxRasterPixels / 2); // it does not shrink further than the cap
    QCOMPARE(composed.size(), targetRasterSize(view));

    // The cap touches the raster only: page geometry and hit testing stay exact.
    QVERIFY(std::abs(view.pageRect().width() / widthAtFit - 3.0) < 0.01);
    const QPointF centre = pageToWidget(view, QPointF(105.0, 148.5));
    QVERIFY(view.pageRect().contains(centre));
}

void TestCanvasCache::cropPanUsesTheFittedScale()
{
    const DocumentSnapshot document = pageWithFrames(1);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.setSelectedFrame(document.pages.first().frames.first().id);
    view.setCropMode(true);
    view.setViewZoom(2.0);
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(250);
    const quint64 before = view.compositionCount();
    const QSize target = targetRasterSize(view);
    const QSize fitted = fittedRasterSize(view);

    const Frame frame = document.pages.first().frames.first();
    const QPointF start = pageToWidget(view, QPointF(frame.cx, frame.cy));
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start.toPoint());
    QTest::mouseMove(&view, (start + QPointF(15.0, 8.0)).toPoint());
    view.repaint();
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 15000);

    // The worst case of the gesture policy: a crop pan at 200% must not compose
    // a 200% page, whatever the display scale.
    const QImage coarse = view.composedImage();
    QVERIFY2(closeTo(coarse.size(), fitted, 2),
             qPrintable(QStringLiteral("crop raster %1x%2, fitted %3x%4, target %5x%6")
                            .arg(coarse.width()).arg(coarse.height())
                            .arg(fitted.width()).arg(fitted.height())
                            .arg(target.width()).arg(target.height())));
    QVERIFY(static_cast<qint64>(coarse.width()) * coarse.height() * 3
            < static_cast<qint64>(target.width()) * target.height());

    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier,
                        (start + QPointF(15.0, 8.0)).toPoint());
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 2, 15000);
    settle(view);
    QCOMPARE(view.composedImage().size(), target);
}


void TestCanvasCache::cropWheelComposesAtFittedScale()
{
    // A crop zoom changes the photo inside the frame on every notch, so the
    // canvas has to advance with the wheel: builds at the fitted scale, never
    // the 200% target, and one sharpening pass when the wheel stops.
    const DocumentSnapshot document = pageWithFrames(1);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setSelectedFrame(document.pages.first().frames.first().id);
    view.setCropMode(true);
    view.setViewZoom(2.0);
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(250);
    const QSize target = targetRasterSize(view);
    const QSize fitted = fittedRasterSize(view);
    const quint64 before = view.compositionCount();
    const Frame frame = document.pages.first().frames.first();
    const QPointF centre = pageToWidget(view, QPointF(frame.cx, frame.cy));

    for (int notch = 0; notch < 6; ++notch) {
        sendWheel(view, centre, 40);
        QTest::qWait(30);
    }

    // The picture advanced during the gesture: not zero builds, and each one was
    // composed at the fitted scale rather than at the 200% target.
    QVERIFY2(view.compositionCount() >= before + 2,
             qPrintable(QStringLiteral("builds during the crop wheel: %1")
                            .arg(view.compositionCount() - before)));
    const QImage coarse = view.composedImage();
    QVERIFY2(closeTo(coarse.size(), fitted, 2),
             qPrintable(QStringLiteral("crop wheel raster %1x%2, fitted %3x%4, target %5x%6")
                            .arg(coarse.width()).arg(coarse.height())
                            .arg(fitted.width()).arg(fitted.height())
                            .arg(target.width()).arg(target.height())));
    QVERIFY(coarse.width() < target.width());

    // Once the wheel stops, the settle sharpens to the full target.
    QTRY_VERIFY_WITH_TIMEOUT(closeTo(view.composedImage().size(), target, 2), 15000);
}

void TestCanvasCache::nudgeBurstBuildsOnceAfterThePulse()
{
    // An arrow nudge moves the outline live; only the photo trails. The burst
    // composes once, after it ends, and not once per key press.
    const DocumentSnapshot document = pageWithFrames(1);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setSelectedFrame(document.pages.first().frames.first().id);
    settle(view);
    QTest::qWait(250);
    const quint64 before = view.compositionCount();
    const quint64 requests = view.compositionRequestCount();

    for (int press = 0; press < 6; ++press) {
        QTest::keyClick(&view, Qt::Key_Right);
        QTest::qWait(30);
    }
    // No build while the pulse is alive.
    QCOMPARE(view.compositionCount(), before);
    QCOMPARE(view.compositionRequestCount(), requests);

    // Exactly one build after the burst, at the full target scale.
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 15000);
    QTest::qWait(400);
    QCOMPARE(view.compositionCount(), before + 1);
    QCOMPARE(view.composedImage().size(), targetRasterSize(view));
}

void TestCanvasCache::composedPixelsFollowTheDocument()
{
    const DocumentSnapshot document = centredFrame();
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    settle(view);
    QTest::qWait(150);

    const QImage before = view.composedImage();
    QVERIFY(!before.isNull());
    const QRgb covered = before.pixel(before.width() / 2, before.height() / 2);
    QVERIFY2(qRed(covered) > 150 && qGreen(covered) < 100,
             qPrintable(QStringLiteral("centre %1,%2,%3")
                            .arg(qRed(covered)).arg(qGreen(covered)).arg(qBlue(covered))));

    // Move the photo off the centre: the next installed raster must show paper
    // there, not the old pixels.
    DocumentSnapshot moved = document;
    moved.pages[0].frames[0].cx = 40.0;
    view.setDocument(moved);
    const QImage after = view.composedImage();
    QVERIFY(!after.isNull());
    const QRgb empty = after.pixel(after.width() / 2, after.height() / 2);
    QVERIFY2(qRed(empty) > 240 && qGreen(empty) > 240 && qBlue(empty) > 240,
             qPrintable(QStringLiteral("centre %1,%2,%3")
                            .arg(qRed(empty)).arg(qGreen(empty)).arg(qBlue(empty))));
    QVERIFY(after != before);
}

void TestCanvasCache::goldenRasterAtRestIsStable()
{
    // A raster at rest must not depend on the history of the cache: a view that
    // was zoomed, dragged and zoomed back has to produce exactly the pixels a
    // fresh view produces for the same document.
    const DocumentSnapshot document = pageWithFrames(4);
    const QHash<QString, QImage> images = preparedImages(document);

    PageView fresh;
    fresh.resize(900, 700);
    fresh.setDocument(document);
    fresh.setFrameImages(images);
    fresh.show();
    QVERIFY(QTest::qWaitForWindowExposed(&fresh));
    settle(fresh);
    QTest::qWait(200);
    const QByteArray gold = rasterHash(fresh.composedImage());
    const QSize goldSize = fresh.composedImage().size();
    QCOMPARE(goldSize, targetRasterSize(fresh));

    PageView used;
    used.resize(900, 700);
    used.setDocument(document);
    used.setFrameImages(images);
    used.show();
    QVERIFY(QTest::qWaitForWindowExposed(&used));
    settle(used);
    QTest::qWait(150);

    const quint64 first = used.compositionCount();
    used.setViewZoom(2.2);
    QTRY_VERIFY_WITH_TIMEOUT(used.compositionCount() >= first + 1, 15000);
    QTest::qWait(250);
    used.setViewZoom(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(used.compositionCount() >= first + 2, 15000);
    settle(used);
    QTest::qWait(150);

    QCOMPARE(used.composedImage().size(), goldSize);
    QCOMPARE(rasterHash(used.composedImage()), gold);
    QCOMPARE(rasterHash(fresh.composedImage()), gold);
}

void TestCanvasCache::failedComposeDoesNotLoop()
{
    const DocumentSnapshot document = pageWithFrames(2);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    settle(view);
    QTest::qWait(150);
    const quint64 installed = view.compositionCount();
    const quint64 requests = view.compositionRequestCount();
    const QImage good = view.composedImage();
    QVERIFY(!good.isNull());

    view.failCompositionsForTest(true);
    DocumentSnapshot changed = document;
    changed.pages[0].frames[0].cx += 7.0;
    view.setDocument(changed);
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionRequestCount(), requests + 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.compositionPending(), 15000);

    // A failed build keeps the previous raster and must not be retried by every
    // repaint or by the settle that follows.
    QCOMPARE(view.compositionCount(), installed);
    for (int pass = 0; pass < 10; ++pass) {
        view.repaint();
        QTest::qWait(20);
    }
    QTest::qWait(300);
    QCOMPARE(view.compositionRequestCount(), requests + 1);
    QCOMPARE(view.compositionCount(), installed);
    QCOMPARE(view.composedImage(), good);

    // A real change earns a new attempt, and it succeeds once the fault is gone.
    view.failCompositionsForTest(false);
    DocumentSnapshot again = changed;
    again.pages[0].frames[0].cy += 4.0;
    view.setDocument(again);
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), installed + 1, 15000);
    settle(view);
    QVERIFY(view.composedImage() != good);

    // An explicit retry clears the guard as well.
    const quint64 rebuilt = view.compositionCount();
    view.failCompositionsForTest(true);
    DocumentSnapshot third = again;
    third.pages[0].frames[0].cx -= 3.0;
    view.setDocument(third);
    QTRY_VERIFY_WITH_TIMEOUT(!view.compositionPending(), 15000);
    QCOMPARE(view.compositionCount(), rebuilt);
    view.failCompositionsForTest(false);
    view.retryComposition();
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), rebuilt + 1, 15000);
}


void TestCanvasCache::blankCanvasGetsExactlyOneRetry()
{
    // Nothing on screen to keep: a failed first build must not leave the canvas
    // blank for ever, but the guard must still stop after one retry.
    const DocumentSnapshot document = pageWithFrames(2);
    PageView view;
    view.resize(900, 700);
    view.failCompositionsForTest(true);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QTRY_VERIFY_WITH_TIMEOUT(view.compositionRequestCount() >= 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.compositionPending(), 15000);
    QTest::qWait(400); // a spinning retry would have been counted by now
    QCOMPARE(view.compositionRequestCount(), 2); // the build and one retry
    QCOMPARE(view.compositionCount(), 0);
    QVERIFY(view.composedImage().isNull());

    for (int pass = 0; pass < 5; ++pass) {
        view.repaint();
        QTest::qWait(20);
    }
    QTest::qWait(200);
    QCOMPARE(view.compositionRequestCount(), 2);

    // A working pipeline recovers through the explicit retry.
    view.failCompositionsForTest(false);
    view.retryComposition();
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), 1, 15000);
    QVERIFY(!view.composedImage().isNull());
}

void TestCanvasCache::inFlightBuildIsSupersededByANewDocument()
{
    const DocumentSnapshot centred = centredFrame(); // frame over the page centre
    PageView view;
    view.resize(900, 700);
    view.setDocument(centred);
    view.setFrameImages(preparedImages(centred));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    settle(view);
    QTest::qWait(150);

    // A slow build for a document whose photo does not cover the centre.
    view.setComposeDelayForTest(300);
    DocumentSnapshot left = centredFrame();
    left.pages[0].frames[0].cx = 40.0;
    view.setDocument(left);
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionPending(), 5000);
    const quint64 installed = view.compositionCount();

    // The centre-covering document arrives before that build wakes up.
    view.setDocument(centred);
    settle(view);
    view.setComposeDelayForTest(0);

    // Only the newest document may reach the screen.
    QCOMPARE(view.compositionCount(), installed + 1);
    const QImage raster = view.composedImage();
    QVERIFY(!raster.isNull());
    const QRgb middle = raster.pixel(raster.width() / 2, raster.height() / 2);
    QVERIFY2(qRed(middle) > 150 && qGreen(middle) < 100,
             qPrintable(QStringLiteral("centre %1,%2,%3")
                            .arg(qRed(middle)).arg(qGreen(middle)).arg(qBlue(middle))));
}

void TestCanvasCache::inFlightBuildIsSupersededByAPaperChange()
{
    const DocumentSnapshot a4 = centredFrame(); // 210 x 297 mm
    DocumentSnapshot photo = centredFrame();
    photo.pages[0].paper.name = QStringLiteral("10 x 15 cm");
    photo.pages[0].paper.physicalWidthMm = 100.0;
    photo.pages[0].paper.physicalHeightMm = 150.0;

    PageView view;
    view.resize(900, 700);
    view.setDocument(photo);
    view.setFrameImages(preparedImages(photo));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    settle(view);
    QTest::qWait(150);

    // Start an A4 raster, then go back to the photo paper while it is building.
    view.setComposeDelayForTest(300);
    view.setDocument(a4);
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionPending(), 5000);
    view.setDocument(photo);
    settle(view);
    view.setComposeDelayForTest(0);

    const QImage raster = view.composedImage();
    QVERIFY(!raster.isNull());
    QVERIFY(closeTo(raster.size(), targetRasterSize(view), 2));
    // A4 and 10x15 differ in aspect, so a stale-paper raster would show up here.
    const QRectF page = view.pageRect();
    const double rasterAspect = double(raster.width()) / raster.height();
    const double pageAspect = page.width() / page.height();
    QVERIFY2(std::abs(rasterAspect - pageAspect) < 0.01,
             qPrintable(QStringLiteral("raster aspect %1, page aspect %2")
                            .arg(rasterAspect).arg(pageAspect)));
}

void TestCanvasCache::lostMouseUpResetsTheGesture()
{
    const DocumentSnapshot document = pageWithFrames(1);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.setViewZoom(2.0);
    QTRY_VERIFY_WITH_TIMEOUT(view.compositionCount() >= 1, 15000);
    settle(view);
    QTest::qWait(250);
    const QSize target = targetRasterSize(view);
    const quint64 before = view.compositionCount();

    const Frame frame = document.pages.first().frames.first();
    const QPointF start = pageToWidget(view, QPointF(frame.cx, frame.cy));
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start.toPoint());
    QTest::mouseMove(&view, (start + QPointF(30.0, 20.0)).toPoint());
    view.repaint();
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 15000);
    QVERIFY(view.composedImage().width() < target.width());

    // Alt-Tab: the window deactivates and the mouse-up never arrives. A widget
    // sees that as an activation change.
    QEvent deactivate(QEvent::ActivationChange);
    QApplication::sendEvent(&view, &deactivate);
    QTRY_VERIFY_WITH_TIMEOUT(!view.compositionPending(), 15000);
    settle(view);
    QTRY_VERIFY_WITH_TIMEOUT(closeTo(view.composedImage().size(), target, 2), 15000);
    const quint64 afterReset = view.compositionCount();

    // A plain move without a button held must not continue the old drag.
    QTest::mouseMove(&view, (start + QPointF(120.0, 90.0)).toPoint());
    QTest::qWait(200);
    QCOMPARE(view.compositionCount(), afterReset);
    QVERIFY(closeTo(view.composedImage().size(), target, 2));
}

void TestCanvasCache::parallelConversionMatchesSerial()
{
    // The production banding policy: small pages stay on the calling thread and
    // the band count never exceeds the hardware.
    QCOMPARE(ColorPipeline::bandsFor(QSize(301, 257)), 1);
    QCOMPARE(ColorPipeline::bandsFor(QSize(200, 130)), 1);
    QVERIFY(ColorPipeline::bandsFor(QSize(1346, 1904)) > 1);
    QVERIFY(ColorPipeline::bandsFor(QSize(20000, 20000)) <= 16);
    // One hardware thread is kept for the GUI.
    const int ideal = QThread::idealThreadCount();
    if (ideal > 1)
        QVERIFY(ColorPipeline::bandsFor(QSize(2000, 2000)) <= ideal - 1);

    const QList<QSize> sizes = {QSize(3, 2), QSize(64, 3), QSize(200, 130), QSize(301, 257),
                                QSize(673, 952)};
    for (const QSize size : sizes) {
        for (int alphaMode = 0; alphaMode < 3; ++alphaMode) {
            const QImage linear = linearPattern(size.width(), size.height(), alphaMode);
            for (const bool grayscale : {false, true}) {
                const QImage serial = ColorPipeline::linearToSrgbBanded(linear, grayscale, 1);
                const QImage four = ColorPipeline::linearToSrgbBanded(linear, grayscale, 4);
                const QImage eight = ColorPipeline::linearToSrgbBanded(linear, grayscale, 8);
                const QImage automatic = ColorPipeline::linearToSrgb(linear, grayscale);
                QVERIFY2(!serial.isNull(), qPrintable(QStringLiteral("%1x%2 alpha %3")
                                                          .arg(size.width())
                                                          .arg(size.height())
                                                          .arg(alphaMode)));
                QVERIFY2(serial == four, "4 bands must be bit identical to the serial path");
                QVERIFY2(serial == eight, "8 bands must be bit identical to the serial path");
                QVERIFY2(serial == automatic, "the production banding must match a serial run");
                if (grayscale) {
                    // The neutral path is exact by construction and tst_daily_flow
                    // relies on it, so banding must not disturb it.
                    const QRgb pixel = serial.pixel(serial.width() / 2, serial.height() / 2);
                    QCOMPARE(qRed(pixel), qGreen(pixel));
                    QCOMPARE(qGreen(pixel), qBlue(pixel));
                }
            }
        }
    }

    // A cancelled conversion does no work and reports nothing to install.
    const QImage page = linearPattern(673, 952, 2);
    std::stop_source stop;
    stop.request_stop();
    QVERIFY(ColorPipeline::linearToSrgb(page, false, stop.get_token()).isNull());
    QVERIFY(ColorPipeline::linearToSrgbBanded(page, false, 8, stop.get_token()).isNull());
}

QTEST_MAIN(TestCanvasCache)
#include "tst_canvas_cache.moc"
