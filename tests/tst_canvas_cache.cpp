#include "ColorPipeline.h"
#include "Document.h"
#include "MainWindow.h"

#include <QColor>
#include <QImage>
#include <QRgba64>
#include <QtTest>
#include <QtMath>

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

QHash<QString, QImage> preparedImages(const DocumentSnapshot &document)
{
    QHash<QString, QImage> images;
    int index = 0;
    for (const Frame &frame : document.pages.first().frames) {
        QImage image(400, 300, QImage::Format_RGB32);
        image.fill(QColor(60 + index * 50, 110, 180 - index * 30));
        images.insert(frame.id, image);
        ++index;
    }
    return images;
}

/// The raster size PageView targets for the current view, cap included.
QSize targetRasterSize(const PageView &view)
{
    const QRectF page = view.pageRect();
    if (page.isEmpty())
        return {};
    const double dpr = view.devicePixelRatioF();
    QSize size(std::max(1, qRound(page.width() * dpr)), std::max(1, qRound(page.height() * dpr)));
    const qint64 area = static_cast<qint64>(size.width()) * size.height();
    if (area > kMaxRasterPixels) {
        const double factor = std::sqrt(static_cast<double>(kMaxRasterPixels) / static_cast<double>(area));
        size = QSize(std::max(1, static_cast<int>(size.width() * factor)),
                     std::max(1, static_cast<int>(size.height() * factor)));
    }
    return size;
}

QPointF pageToWidget(const PageView &view, const QPointF &mm)
{
    const QRectF rect = view.pageRect();
    const QSizeF page = view.pageMm();
    return QPointF(rect.left() + mm.x() / page.width() * rect.width(),
                   rect.top() + mm.y() / page.height() * rect.height());
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

} // namespace

class TestCanvasCache : public QObject
{
    Q_OBJECT

private slots:
    void panKeepsTheComposedRaster();
    void zoomRecomposesOnceAfterTheDebounce();
    void documentChangeRecomposesOnce();
    void identicalInputsAreIgnored();
    void dragComposesCoarseAndSettlesOnRelease();
    void largeViewportStaysInsideTheCap();
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
    QTRY_VERIFY(view.compositionCount() >= 1);
    QTest::qWait(200); // let the zoom settle, so nothing is pending during the pan
    const quint64 before = view.compositionCount();
    const QImage first = view.composedImage();
    QVERIFY(!first.isNull());
    QVERIFY(first.width() > 800); // a 200% page, not the fitted one

    QTest::mousePress(&view, Qt::MiddleButton, Qt::NoModifier, QPoint(450, 350));
    for (int step = 0; step < 20; ++step)
        QTest::mouseMove(&view, QPoint(450 + step * 4, 350 + step * 2));
    QTest::mouseRelease(&view, Qt::MiddleButton, Qt::NoModifier, QPoint(526, 388));
    QTest::qWait(200);

    QCOMPARE(view.compositionCount(), before);
    QCOMPARE(view.composedImage(), first);
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
    QTRY_VERIFY(view.compositionCount() >= 1);
    const quint64 before = view.compositionCount();
    const QSize fitted = targetRasterSize(view);

    view.setViewZoom(2.0);
    // A burst of notches is what the wheel really sends.
    QTest::qWait(20);
    view.setViewZoom(2.2);
    view.setViewZoom(2.0);
    QTest::qWait(30); // still inside the 120 ms debounce
    QCOMPARE(view.compositionCount(), before);

    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 3000);
    QTest::qWait(200);
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
    QTRY_VERIFY(view.compositionCount() >= 1);
    QTest::qWait(150);
    const quint64 before = view.compositionCount();

    DocumentSnapshot moved = document;
    moved.pages[0].frames[0].cx += 12.0;
    view.setDocument(moved);
    const QImage composed = view.composedImage();
    QVERIFY(!composed.isNull());
    QCOMPARE(view.compositionCount(), before + 1);

    // Reading the raster again, and repainting, must not compose a third time.
    view.composedImage();
    view.repaint();
    QCOMPARE(view.compositionCount(), before + 1);
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
    QTRY_VERIFY(view.compositionCount() >= 1);
    QTest::qWait(150);
    const quint64 before = view.compositionCount();

    // The same snapshot and an equal (but freshly built) image set: nothing on
    // the page changed, so the raster must survive.
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    QTest::qWait(60);
    QCOMPARE(view.compositionCount(), before);

    // A real change still gets through.
    DocumentSnapshot changed = document;
    changed.pages[0].frames[0].angleDeg = 5.0;
    view.setDocument(changed);
    view.composedImage();
    QCOMPARE(view.compositionCount(), before + 1);
}

void TestCanvasCache::dragComposesCoarseAndSettlesOnRelease()
{
    const DocumentSnapshot document = pageWithFrames(1);
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages(preparedImages(document));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.setViewZoom(2.0);
    QTRY_VERIFY(view.compositionCount() >= 1);
    QTest::qWait(200);
    const quint64 before = view.compositionCount();
    const QSize target = targetRasterSize(view);
    QVERIFY(target.width() > 800);

    const Frame frame = document.pages.first().frames.first();
    const QPointF start = pageToWidget(view, QPointF(frame.cx, frame.cy));
    const QPointF end = start + QPointF(24.0, 12.0);
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start.toPoint());
    QTest::mouseMove(&view, end.toPoint());

    // Mid-drag the page is composed at the fitted scale, never at 200%.
    const QImage coarse = view.composedImage();
    QCOMPARE(view.compositionCount(), before + 1);
    QVERIFY(coarse.width() < target.width() * 0.75);

    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, end.toPoint());
    QTest::qWait(100);
    const QImage settled = view.composedImage();
    QVERIFY(settled.width() >= target.width() * 0.98);
    QVERIFY(settled.width() <= target.width() * 1.35);
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
    QTRY_VERIFY(view.compositionCount() >= 1);
    QTest::qWait(150);

    const double widthAtFit = view.pageRect().width();
    const quint64 before = view.compositionCount();
    view.setViewZoom(3.0);
    QTRY_COMPARE_WITH_TIMEOUT(view.compositionCount(), before + 1, 15000);

    const QImage composed = view.composedImage();
    QVERIFY(!composed.isNull());
    const qint64 pixels = static_cast<qint64>(composed.width()) * composed.height();
    QVERIFY2(pixels <= kMaxRasterPixels, qPrintable(QStringLiteral("raster %1 x %2")
                                                        .arg(composed.width())
                                                        .arg(composed.height())));
    QVERIFY(pixels > kMaxRasterPixels / 2); // it does not shrink further than the cap
    QCOMPARE(composed.size(), targetRasterSize(view));

    // The cap touches the raster only: page geometry and hit testing stay exact.
    QVERIFY(std::abs(view.pageRect().width() / widthAtFit - 3.0) < 0.01);
    const QPointF centre = pageToWidget(view, QPointF(105.0, 148.5));
    QVERIFY(view.pageRect().contains(centre));
}

void TestCanvasCache::parallelConversionMatchesSerial()
{
    const QList<QSize> sizes = {QSize(3, 2), QSize(64, 3), QSize(200, 130), QSize(301, 257)};
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
                QVERIFY2(serial == automatic, "the automatic banding must match a serial run");
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
}

QTEST_MAIN(TestCanvasCache)
#include "tst_canvas_cache.moc"
