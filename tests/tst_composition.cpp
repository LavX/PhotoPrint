#include "Document.h"
#include "ImageSource.h"
#include "LayoutEngine.h"
#include "PageRenderer.h"
#include "Result.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace pp;

namespace {

QString evidenceDir()
{
#ifdef PHOTOPRINT_EVIDENCE_DIR
    const QString dir = QStringLiteral(PHOTOPRINT_EVIDENCE_DIR);
#else
    const QString dir = QDir::currentPath() + QStringLiteral("/build/evidence");
#endif
    QDir().mkpath(dir);
    return dir;
}

double srgbToLinear(int channel)
{
    const double value = channel / 255.0;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

int linearToSrgb(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    const double encoded =
        value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(std::clamp(encoded, 0.0, 1.0) * 255.0));
}

QRgb blendInLinear(QRgb background, QRgb foreground, int alpha)
{
    const double a = alpha / 255.0;
    const int r = linearToSrgb(srgbToLinear(qRed(foreground)) * a
                               + srgbToLinear(qRed(background)) * (1.0 - a));
    const int g = linearToSrgb(srgbToLinear(qGreen(foreground)) * a
                               + srgbToLinear(qGreen(background)) * (1.0 - a));
    const int b = linearToSrgb(srgbToLinear(qBlue(foreground)) * a
                               + srgbToLinear(qBlue(background)) * (1.0 - a));
    return qRgb(r, g, b);
}

int channelDistance(QRgb a, QRgb b)
{
    return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                     std::abs(qBlue(a) - qBlue(b))});
}

QImage backgroundImage()
{
    QImage image(800, 600, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 140, 60));
    return image;
}

QImage overlayImage()
{
    QImage image(400, 200, QImage::Format_ARGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(QRect(0, 0, 200, 100), QColor(220, 20, 20, 128));
    painter.fillRect(QRect(200, 0, 200, 100), QColor(220, 20, 20, 255));
    painter.fillRect(QRect(0, 100, 200, 100), QColor(220, 20, 20, 0));
    painter.fillRect(QRect(200, 100, 200, 100), QColor(20, 20, 220, 128));
    painter.end();
    return image;
}

struct Case {
    QTemporaryDir dir;
    DocumentSnapshot document;
    SourceImage background;
    SourceImage overlay;
    Frame overlayFrame;
    bool ready = false;
    QString error;
};

bool buildCase(Case &testCase, ImageStore &store)
{
    const QString backgroundPath = testCase.dir.filePath(QStringLiteral("background.jpg"));
    QImageWriter backgroundWriter(backgroundPath, "jpeg");
    backgroundWriter.setQuality(95);
    if (!backgroundWriter.write(backgroundImage())) {
        testCase.error = QStringLiteral("background write failed");
        return false;
    }
    const QString overlayPath = testCase.dir.filePath(QStringLiteral("overlay.png"));
    QImageWriter overlayWriter(overlayPath, "png");
    if (!overlayWriter.write(overlayImage())) {
        testCase.error = QStringLiteral("overlay write failed");
        return false;
    }

    const auto background = store.load(backgroundPath, ImportPolicy{});
    const auto overlay = store.load(overlayPath, ImportPolicy{});
    if (!background) {
        testCase.error = background.error().message;
        return false;
    }
    if (!overlay) {
        testCase.error = overlay.error().message;
        return false;
    }
    testCase.background = background.value();
    testCase.overlay = overlay.value();

    Page page;
    page.id = QStringLiteral("page-1");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;

    Frame backgroundFrame;
    backgroundFrame.id = QStringLiteral("background");
    backgroundFrame.sourcePath = testCase.background.path;
    backgroundFrame.sourceSha256 = testCase.background.sha256;
    backgroundFrame.cx = 105.0;
    backgroundFrame.cy = 148.5;
    backgroundFrame.width = 120.0;
    backgroundFrame.height = 90.0;
    backgroundFrame.fit = FitMode::Fill;
    page.frames.append(backgroundFrame);

    Frame overlayFrame;
    overlayFrame.id = QStringLiteral("overlay");
    overlayFrame.sourcePath = testCase.overlay.path;
    overlayFrame.sourceSha256 = testCase.overlay.sha256;
    overlayFrame.cx = 105.0;
    overlayFrame.cy = 148.5;
    overlayFrame.width = 60.0;
    overlayFrame.height = 30.0;
    overlayFrame.fit = FitMode::Fill;
    overlayFrame.angleDeg = 17.0;
    overlayFrame.cropCenter = QPointF(0.5, 0.5);
    page.frames.append(overlayFrame);
    testCase.overlayFrame = overlayFrame;
    testCase.document.pages.append(page);
    testCase.ready = true;
    return true;
}

} // namespace

class TestComposition : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void emptyCodeStillMeansFailure();
    void alphaOverlayBlendsInLinearSpace();
    void pdfMatchesThePageRender();

private:
    QTemporaryDir m_dir;
    Case m_case;
};

void TestComposition::initTestCase()
{
    QVERIFY(m_dir.isValid());
    ImageStore store;
    QVERIFY2(buildCase(m_case, store), qPrintable(m_case.error));
}

void TestComposition::emptyCodeStillMeansFailure()
{
    // A failure constructed without a code is still a failure; the message a
    // person would read must never be discarded.
    const Result<void> failure = error(QString(), QStringLiteral("missing detail"));
    QVERIFY(!failure.ok());
    QVERIFY(!failure);
    QCOMPARE(failure.error().code, QString());
    QCOMPARE(failure.error().message, QStringLiteral("missing detail"));

    const Result<void> success;
    QVERIFY(success.ok());
    QVERIFY(success);

    const Result<int> value(7);
    QVERIFY(value.ok());
    QCOMPARE(value.value(), 7);

    const Result<int> failed = error(QStringLiteral("render"), QStringLiteral("failed"));
    QVERIFY(!failed.ok());
    QCOMPARE(failed.error().code, QStringLiteral("render"));
}

void TestComposition::alphaOverlayBlendsInLinearSpace()
{
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(m_case.document, 0, 200.0);
    if (!rendered)
        QFAIL(qPrintable(rendered.error().message));
    const QImage page = rendered.value();
    const double pxPerMm = 200.0 / 25.4;

    const QImage overlay = m_case.overlay.pixels;
    const QImage background = m_case.background.pixels;
    const QTransform transform =
        LayoutEngine::imageToPage(m_case.overlayFrame, overlay.size());

    struct Probe {
        QPoint source;
        int alpha;
        QRgb foreground;
        QRgb expected;
        const char *name;
    };
    const QRgb backgroundPixel = background.pixel(10, 10);
    const QList<Probe> probes = {
        {QPoint(100, 50), 128, overlay.pixel(100, 50),
         blendInLinear(backgroundPixel, overlay.pixel(100, 50), 128), "half red"},
        {QPoint(300, 50), 255, overlay.pixel(300, 50),
         blendInLinear(backgroundPixel, overlay.pixel(300, 50), 255), "opaque red"},
        {QPoint(100, 150), 0, overlay.pixel(100, 150), backgroundPixel, "transparent"},
        {QPoint(300, 150), 128, overlay.pixel(300, 150),
         blendInLinear(backgroundPixel, overlay.pixel(300, 150), 128), "half blue"},
    };

    for (const Probe &probe : probes) {
        const QPointF pagePoint = transform.map(QPointF(probe.source));
        const QRgb actual =
            page.pixel(qRound(pagePoint.x() * pxPerMm), qRound(pagePoint.y() * pxPerMm));
        const int distance = channelDistance(actual, probe.expected);
        QVERIFY2(distance <= 2,
                 qPrintable(QStringLiteral("%1 differs by %2: actual %3/%4/%5 expected %6/%7/%8")
                                .arg(QString::fromLatin1(probe.name))
                                .arg(distance)
                                .arg(qRed(actual))
                                .arg(qGreen(actual))
                                .arg(qBlue(actual))
                                .arg(qRed(probe.expected))
                                .arg(qGreen(probe.expected))
                                .arg(qBlue(probe.expected))));
    }

    // A transparent PNG must never hide the photograph below it with white.
    const QPointF transparentPoint = transform.map(QPointF(100, 150));
    const QRgb transparentPixel = page.pixel(qRound(transparentPoint.x() * pxPerMm),
                                             qRound(transparentPoint.y() * pxPerMm));
    QVERIFY2(channelDistance(transparentPixel, backgroundPixel) <= 2,
             "transparent area lost the layer underneath");
}

void TestComposition::pdfMatchesThePageRender()
{
    ImageStore store;
    PageRenderer renderer(store);
    const double ppi = 200.0;
    const auto rendered = renderer.renderPage(m_case.document, 0, ppi);
    if (!rendered)
        QFAIL(qPrintable(rendered.error().message));

    const QString pdfPath = evidenceDir() + QStringLiteral("/alpha-composition.pdf");
    RenderOptions options;
    options.printPpi = ppi;
    const auto artifact = renderer.writePdf(m_case.document, pdfPath, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    QProcess process;
    const QString base = evidenceDir() + QStringLiteral("/alpha-composition");
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, base});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    const QImage pdfPage(base + QStringLiteral(".png"));
    QVERIFY(!pdfPage.isNull());

    const QImage overlay = m_case.overlay.pixels;
    const QTransform transform =
        LayoutEngine::imageToPage(m_case.overlayFrame, overlay.size());
    const double pxPerMm = ppi / 25.4;
    const QRgb backgroundPixel = m_case.background.pixels.pixel(10, 10);

    const QList<QPoint> probes = {QPoint(100, 50), QPoint(300, 50), QPoint(100, 150),
                                  QPoint(300, 150)};
    for (const QPoint &probe : probes) {
        const QPointF pagePoint = transform.map(QPointF(probe));
        const int x = qRound(pagePoint.x() * pxPerMm);
        const int y = qRound(pagePoint.y() * pxPerMm);
        const QRgb fromRender = rendered.value().pixel(x, y);
        const QRgb fromPdf = pdfPage.pixel(x, y);
        const int distance = channelDistance(fromRender, fromPdf);
        QVERIFY2(distance <= 2,
                 qPrintable(QStringLiteral("PDF vs render at %1,%2 differs by %3: %4/%5/%6 vs %7/%8/%9")
                                .arg(probe.x())
                                .arg(probe.y())
                                .arg(distance)
                                .arg(qRed(fromRender))
                                .arg(qGreen(fromRender))
                                .arg(qBlue(fromRender))
                                .arg(qRed(fromPdf))
                                .arg(qGreen(fromPdf))
                                .arg(qBlue(fromPdf))));
    }

    const QPointF transparentPoint = transform.map(QPointF(100, 150));
    const QRgb pdfTransparent = pdfPage.pixel(qRound(transparentPoint.x() * pxPerMm),
                                              qRound(transparentPoint.y() * pxPerMm));
    QVERIFY2(channelDistance(pdfTransparent, backgroundPixel) <= 2,
             "PDF flattened a transparent area to white");
}

QTEST_GUILESS_MAIN(TestComposition)
#include "tst_composition.moc"
