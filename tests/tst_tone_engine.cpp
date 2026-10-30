#include "ColorPipeline.h"
#include "Document.h"
#include "ImageSource.h"
#include "PageRenderer.h"
#include "ToneEngine.h"

#include <QColorSpace>
#include <QDir>
#include <QImage>
#include <QImageWriter>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <vector>

using namespace pp;

namespace {

QImage linearImage(const std::vector<std::pair<double, int>> &regions, int width = 200,
                   int height = 200)
{
    QImage image(width, height, QImage::Format_RGBA64_Premultiplied);
    int y = 0;
    for (const auto &region : regions) {
        const double value = region.first;
        const int rows = region.second;
        const quint16 level = static_cast<quint16>(std::lround(std::clamp(value, 0.0, 1.0) * 65535.0));
        for (int row = 0; row < rows && y < height; ++row, ++y) {
            QRgba64 *line = reinterpret_cast<QRgba64 *>(image.scanLine(y));
            for (int x = 0; x < width; ++x)
                line[x] = qRgba64(level, level, level, 65535);
        }
    }
    return image;
}

QImage rampLinear(int height = 200)
{
    QImage image(1, height, QImage::Format_RGBA64_Premultiplied);
    for (int y = 0; y < height; ++y) {
        const quint16 level = static_cast<quint16>(std::lround(65535.0 * y / (height - 1)));
        QRgba64 *line = reinterpret_cast<QRgba64 *>(image.scanLine(y));
        line[0] = qRgba64(level, level, level, 65535);
    }
    return image;
}

SourceImage rampSource()
{
    QImage ramp(256, 1, QImage::Format_RGB32);
    ramp.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int x = 0; x < 256; ++x)
        ramp.setPixel(x, 0, qRgb(x, x, x));

    SourceImage source;
    source.path = QStringLiteral("memory-ramp");
    source.sha256 = QByteArrayLiteral("ramp");
    source.pixels = ramp;
    source.sourceSpace = QColorSpace(QColorSpace::SRgb);
    source.hasEmbeddedProfile = true;
    source.profileValid = true;
    return source;
}

} // namespace

class TestToneEngine : public QObject
{
    Q_OBJECT

private slots:
    void underexposedFrameGetsUsefulLift();
    void deliberatelyDarkSceneStaysRestrained();
    void correctAndHighKeyFramesNeedNoHelp();
    void brightWindowProtectsHighlights();
    void suggestionIsDeterministic();
    void nonRgba64InputMatchesTheConvertedSample();
    void curveIsMonotonicAndProtectsEnds();
    void disabledToneIsTheIdentity();
    void brightnessOffsetIsApplied();
    void skinTonesStayRestrained();
    void exposurePreservesColourRatios();
    void pdfUsesTheSameToneRecipe();
    void brightnessReachesThePdf();
};

namespace {
QImage colourSample(QRgb colour, int width = 200, int height = 200)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
}

SourceImage sourceFrom(const QImage &image)
{
    SourceImage source;
    source.path = QStringLiteral("memory");
    source.sha256 = QByteArrayLiteral("memory");
    source.pixels = image;
    source.sourceSpace = QColorSpace(QColorSpace::SRgb);
    source.hasEmbeddedProfile = true;
    source.profileValid = true;
    return source;
}

double srgbToLinear(int value)
{
    const double v = value / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
} // namespace

void TestToneEngine::underexposedFrameGetsUsefulLift()
{
    const PhotoTone tone = ToneEngine::suggest(
        linearImage({{0.08, 160}, {0.4, 40}}));
    QVERIFY2(tone.exposureEv > 0.2, qPrintable(QStringLiteral("ev %1").arg(tone.exposureEv)));
    QVERIFY(tone.exposureEv <= 0.35);
    QVERIFY(tone.shadowLift >= 0.0);
    QVERIFY(tone.shadowLift <= 0.12);
}

void TestToneEngine::deliberatelyDarkSceneStaysRestrained()
{
    const PhotoTone dark = ToneEngine::suggest(linearImage({{0.0, 100}, {0.18, 100}}));
    QVERIFY2(dark.exposureEv >= 0.0 && dark.exposureEv <= 0.15,
             qPrintable(QStringLiteral("dark ev %1").arg(dark.exposureEv)));
}

void TestToneEngine::correctAndHighKeyFramesNeedNoHelp()
{
    const PhotoTone correct = ToneEngine::suggest(linearImage({{0.18, 160}, {0.7, 40}}));
    QCOMPARE(correct.exposureEv, 0.0);

    const PhotoTone highKey = ToneEngine::suggest(linearImage({{0.5, 100}, {0.95, 100}}));
    QCOMPARE(highKey.exposureEv, 0.0);
}

void TestToneEngine::brightWindowProtectsHighlights()
{
    const PhotoTone tone =
        ToneEngine::suggest(linearImage({{0.03, 160}, {0.15, 38}, {0.98, 2}}));
    QVERIFY(tone.exposureEv <= 0.1);
    QVERIFY(std::pow(2.0, tone.exposureEv) * 0.98 <= 0.99);
}

void TestToneEngine::suggestionIsDeterministic()
{
    const QImage sample = linearImage({{0.08, 120}, {0.3, 80}});
    QCOMPARE(ToneEngine::suggest(sample), ToneEngine::suggest(sample));
}

void TestToneEngine::nonRgba64InputMatchesTheConvertedSample()
{
    // The engine is handed whatever the pipeline has; it must not assume the
    // sample is already RGBA64. Same values in two layouts must agree exactly.
    const int width = 64;
    const int height = 64;
    const int level8 = 20; // dark enough to produce a real suggestion
    const quint16 level16 = static_cast<quint16>(level8 * 257);

    QImage eightBit(width, height, QImage::Format_RGB32);
    QImage sixtyFour(width, height, QImage::Format_RGBA64_Premultiplied);
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(eightBit.scanLine(y));
        auto *wide = reinterpret_cast<QRgba64 *>(sixtyFour.scanLine(y));
        for (int x = 0; x < width; ++x) {
            line[x] = qRgb(level8, level8, level8);
            wide[x] = qRgba64(level16, level16, level16, 65535);
        }
    }

    const PhotoTone reference = ToneEngine::suggest(sixtyFour);
    QVERIFY(reference.exposureEv > 0.0);
    QCOMPARE(ToneEngine::suggest(eightBit), reference);
    QCOMPARE(ToneEngine::suggest(eightBit.convertToFormat(QImage::Format_Grayscale8)), reference);

    // A null sample stays neutral instead of reading a missing scanline.
    QCOMPARE(ToneEngine::suggest(QImage()), PhotoTone{});
}

void TestToneEngine::curveIsMonotonicAndProtectsEnds()
{
    ColorRecipe recipe;
    recipe.tone.exposureEv = 0.3;
    recipe.tone.shadowLift = 0.12;
    recipe.tone.highlightCompress = 0.06;

    const QImage output = ColorPipeline::toSrgb(rampSource(), recipe);
    QVERIFY(!output.isNull());
    int previous = -1;
    for (int x = 0; x < output.width(); ++x) {
        const int value = qGreen(output.pixel(x, 0));
        QVERIFY2(value >= previous, qPrintable(QStringLiteral("dip at %1").arg(x)));
        previous = value;
    }
    QVERIFY(qGreen(output.pixel(0, 0)) <= 4);
    QVERIFY(qGreen(output.pixel(255, 0)) >= 245);
}

void TestToneEngine::disabledToneIsTheIdentity()
{
    Frame frame;
    frame.autoTone = false;
    frame.brightness = 0.0;
    QCOMPARE(effectiveTone(frame), PhotoTone{});

    ColorRecipe recipe;
    recipe.tone = effectiveTone(frame);
    const QImage output = ColorPipeline::toSrgb(rampSource(), recipe);
    const QImage reference =
        ColorPipeline::toSrgb(rampSource(), ColorRecipe{});
    QCOMPARE(output, reference);
}

void TestToneEngine::brightnessOffsetIsApplied()
{
    Frame frame;
    frame.autoTone = true;
    frame.tone.exposureEv = 0.1;
    frame.brightness = 0.2;
    QVERIFY(std::abs(effectiveTone(frame).exposureEv - 0.3) < 1e-9);

    frame.autoTone = false;
    QVERIFY(std::abs(effectiveTone(frame).exposureEv - 0.2) < 1e-9);
}

void TestToneEngine::skinTonesStayRestrained()
{
    const PhotoTone tone = ToneEngine::suggest(linearImage({{0.23, 200}}));
    QVERIFY(tone.exposureEv <= 0.05);
}

void TestToneEngine::exposurePreservesColourRatios()
{
    QImage image(2, 1, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.setPixel(0, 0, qRgb(200, 100, 50));
    image.setPixel(1, 0, qRgb(100, 50, 25));

    ColorRecipe recipe;
    recipe.tone.exposureEv = 0.3;
    const QImage output = ColorPipeline::toSrgb(sourceFrom(image), recipe);
    QVERIFY(!output.isNull());
    const double before = srgbToLinear(qRed(image.pixel(0, 0)))
        / srgbToLinear(qRed(image.pixel(1, 0)));
    const double after =
        srgbToLinear(qRed(output.pixel(0, 0))) / srgbToLinear(qRed(output.pixel(1, 0)));
    QVERIFY2(std::abs(after / before - 1.0) < 0.01,
             qPrintable(QStringLiteral("ratio changed from %1 to %2").arg(before).arg(after)));
}

void TestToneEngine::pdfUsesTheSameToneRecipe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage ramp(256, 256, QImage::Format_RGB32);
    ramp.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x)
            ramp.setPixel(x, y, qRgb(x, x, x));
    }
    const QString path = dir.filePath(QStringLiteral("ramp.png"));
    QImageWriter writer(path, "png");
    QVERIFY(writer.write(ramp));

    ImageStore store;
    const auto source = store.load(path, ImportPolicy{});
    QVERIFY(source.ok());

    Frame frame;
    frame.id = QStringLiteral("ramp");
    frame.sourcePath = source.value().path;
    frame.sourceSha256 = source.value().sha256;
    frame.cx = 105.0;
    frame.cy = 148.5;
    frame.width = 100.0;
    frame.height = 100.0;
    frame.fit = FitMode::Fill;
    frame.tone.exposureEv = 0.3;
    frame.tone.shadowLift = 0.05;
    frame.autoTone = true;

    Page page;
    page.id = QStringLiteral("page");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    page.frames.append(frame);
    DocumentSnapshot document;
    document.pages.append(page);

    PageRenderer renderer(store);
    const double ppi = 150.0;
    const auto rendered = renderer.renderPage(document, 0, ppi);
    QVERIFY(rendered.ok());

    const QString pdfPath = dir.filePath(QStringLiteral("tone.pdf"));
    RenderOptions options;
    options.printPpi = ppi;
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    QVERIFY(artifact.ok());

    QProcess process;
    const QString base = dir.filePath(QStringLiteral("tone"));
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, base});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    const QImage pdfPage(base + QStringLiteral(".png"));
    QVERIFY(!pdfPage.isNull());

    const double pxPerMm = ppi / 25.4;
    for (const QPoint &point : {QPoint(80, 120), QPoint(105, 148), QPoint(130, 170)}) {
        const int x = qRound(point.x() * pxPerMm);
        const int y = qRound(point.y() * pxPerMm);
        QVERIFY(std::abs(qGreen(rendered.value().pixel(x, y)) - qGreen(pdfPage.pixel(x, y))) <= 2);
    }
}

void TestToneEngine::brightnessReachesThePdf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage ramp(256, 256, QImage::Format_RGB32);
    ramp.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x)
            ramp.setPixel(x, y, qRgb(x, x, x));
    }
    const QString sourcePath = dir.filePath(QStringLiteral("brightness.png"));
    QVERIFY(ramp.save(sourcePath));

    ImageStore store;
    const auto source = store.load(sourcePath, ImportPolicy{});
    QVERIFY(source.ok());

    Frame frame;
    frame.id = QStringLiteral("frame");
    frame.sourcePath = source.value().path;
    frame.sourceSha256 = source.value().sha256;
    frame.cx = 105.0;
    frame.cy = 148.5;
    frame.width = 100.0;
    frame.height = 100.0;
    frame.fit = FitMode::Fill;
    frame.autoTone = false;
    frame.brightness = 0.0;

    Page page;
    page.id = QStringLiteral("page");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    page.frames.append(frame);
    DocumentSnapshot document;
    document.pages.append(page);

    PageRenderer renderer(store);
    const double ppi = 150.0;
    const auto base = renderer.renderPage(document, 0, ppi);
    QVERIFY(base.ok());

    document.pages[0].frames[0].brightness = 0.3;
    const auto brighter = renderer.renderPage(document, 0, ppi);
    QVERIFY(brighter.ok());
    const int x = qRound(105.0 * ppi / 25.4);
    const int y = qRound(148.5 * ppi / 25.4);
    QVERIFY2(qGreen(brighter.value().pixel(x, y)) > qGreen(base.value().pixel(x, y)) + 5,
             "the brightness slider must change the page");

    const QString pdfPath = dir.filePath(QStringLiteral("brightness.pdf"));
    RenderOptions options;
    options.printPpi = ppi;
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    QVERIFY(artifact.ok());
    QProcess process;
    const QString baseName = dir.filePath(QStringLiteral("brightness"));
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, baseName});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    const QImage pdfPage(baseName + QStringLiteral(".png"));
    QVERIFY(!pdfPage.isNull());
    QVERIFY2(qGreen(pdfPage.pixel(x, y)) > qGreen(base.value().pixel(x, y)) + 5,
             "the brightness must reach the exported PDF, not just the screen");
}

QTEST_GUILESS_MAIN(TestToneEngine)
#include "tst_tone_engine.moc"
