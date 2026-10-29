#include "ColorPipeline.h"
#include "ImageSource.h"
#include "PageRenderer.h"
#include "PhotoFilters.h"

#include <QColor>
#include <QDir>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace pp;

namespace {

SourceImage memorySource(const QImage &image)
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

QImage colourImage()
{
    QImage image(64, 48, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(180, 80, 40));
    return image;
}

int channelDistance(QRgb a, QRgb b)
{
    return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                     std::abs(qBlue(a) - qBlue(b))});
}

} // namespace

class TestFilterIntegration : public QObject
{
    Q_OBJECT

private slots:
    void originalIsTheFastIdentity();
    void sepiaChangesPixelsAndZeroStrengthDoesNot();
    void upscaleDoublesThePrintLayerButNotBoundedPreviews();
    void alphaSurvivesTheSrgbRoundTrip();
    void pdfCarriesTheFilter();
};

void TestFilterIntegration::originalIsTheFastIdentity()
{
    ImageStore store;
    const SourceImage source = memorySource(colourImage());
    const auto linear = store.linear(source, ColorRecipe{}, 0);
    const auto prepared =
        store.preparedLinear(source, ColorRecipe{}, QStringLiteral("original"), 100, false, 0);
    QVERIFY(linear.ok());
    QVERIFY(prepared.ok());
    QCOMPARE(prepared.value(), linear.value());
}

void TestFilterIntegration::sepiaChangesPixelsAndZeroStrengthDoesNot()
{
    ImageStore store;
    const SourceImage source = memorySource(colourImage());
    const auto original = store.linear(source, ColorRecipe{}, 0);
    const auto sepia =
        store.preparedLinear(source, ColorRecipe{}, QStringLiteral("sepia"), 100, false, 0);
    QVERIFY(original.ok());
    QVERIFY(sepia.ok());
    QCOMPARE(sepia.value().size(), original.value().size());

    const QImage originalSrgb = ColorPipeline::linearToSrgb(original.value());
    const QImage sepiaSrgb = ColorPipeline::linearToSrgb(sepia.value());
    QVERIFY(channelDistance(originalSrgb.pixel(10, 10), sepiaSrgb.pixel(10, 10)) > 5);

    const auto zero =
        store.preparedLinear(source, ColorRecipe{}, QStringLiteral("sepia"), 0, false, 0);
    QVERIFY(zero.ok());
    const QImage zeroSrgb = ColorPipeline::linearToSrgb(zero.value());
    QVERIFY(channelDistance(originalSrgb.pixel(10, 10), zeroSrgb.pixel(10, 10)) <= 2);
}

void TestFilterIntegration::upscaleDoublesThePrintLayerButNotBoundedPreviews()
{
    ImageStore store;
    const SourceImage source = memorySource(colourImage());
    const auto enlarged =
        store.preparedLinear(source, ColorRecipe{}, QStringLiteral("original"), 100, true, 0);
    QVERIFY(enlarged.ok());
    QCOMPARE(enlarged.value().size(), QSize(128, 96));

    const auto preview =
        store.preparedLinear(source, ColorRecipe{}, QStringLiteral("original"), 100, true, 48);
    QVERIFY(preview.ok());
    QVERIFY(preview.value().width() <= 48);
}

void TestFilterIntegration::alphaSurvivesTheSrgbRoundTrip()
{
    QImage argb(8, 8, QImage::Format_ARGB32);
    argb.fill(QColor(200, 60, 30, 128));
    argb.setColorSpace(QColorSpace(QColorSpace::SRgb));

    const QImage linear = ColorPipeline::srgbToLinear(argb);
    QVERIFY(!linear.isNull());
    const QImage back = ColorPipeline::linearToSrgbWithAlpha(linear);
    QVERIFY(!back.isNull());
    for (int y = 0; y < back.height(); ++y) {
        for (int x = 0; x < back.width(); ++x) {
            const QRgb pixel = back.pixel(x, y);
            QCOMPARE(qAlpha(pixel), 128);
            QVERIFY(channelDistance(pixel, qRgb(200, 60, 30)) <= 2);
        }
    }

    QImage transparent(4, 4, QImage::Format_ARGB32);
    transparent.fill(Qt::transparent);
    const QImage transparentLinear = ColorPipeline::srgbToLinear(transparent);
    const QImage transparentBack = ColorPipeline::linearToSrgbWithAlpha(transparentLinear);
    QCOMPARE(qAlpha(transparentBack.pixel(1, 1)), 0);
}

void TestFilterIntegration::pdfCarriesTheFilter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("photo.png"));
    QVERIFY(colourImage().save(path));

    ImageStore store;
    const auto source = store.load(path, ImportPolicy{});
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
    frame.filter = QStringLiteral("sepia");
    frame.filterStrength = 100;

    Page page;
    page.id = QStringLiteral("page");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    page.frames.append(frame);
    DocumentSnapshot document;
    document.pages.append(page);

    PageRenderer renderer(store);
    const auto originalRender = renderer.renderPage(document, 0, 150.0);
    QVERIFY(originalRender.ok());
    frame.filter = QStringLiteral("original");
    document.pages[0].frames[0] = frame;
    const auto plainRender = renderer.renderPage(document, 0, 150.0);
    QVERIFY(plainRender.ok());
    QVERIFY(channelDistance(originalRender.value().pixel(525, 742),
                            plainRender.value().pixel(525, 742))
            > 5);

    frame.filter = QStringLiteral("sepia");
    document.pages[0].frames[0] = frame;
    const QString pdfPath = dir.filePath(QStringLiteral("sepia.pdf"));
    QVERIFY(renderer.writePdf(document, pdfPath, {}, RenderOptions{}).ok());

    QProcess process;
    const QString base = dir.filePath(QStringLiteral("sepia"));
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QStringLiteral("150"), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, base});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    const QImage pdfPage(base + QStringLiteral(".png"));
    QVERIFY(!pdfPage.isNull());
    const QRgb pdfPixel = pdfPage.pixel(525, 742);
    QVERIFY2(channelDistance(pdfPixel, plainRender.value().pixel(525, 742)) > 5,
             "the exported PDF must carry the sepia look");
}

QTEST_GUILESS_MAIN(TestFilterIntegration)
#include "tst_filter_integration.moc"
