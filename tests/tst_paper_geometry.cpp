#include "Document.h"
#include "PageRenderer.h"
#include "PaperGeometry.h"

#include <QDir>
#include <QFile>
#include <QColor>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QtTest>

#include <algorithm>

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

PaperSpec photoPaper()
{
    PaperSpec paper;
    paper.name = QStringLiteral("10 × 15 cm");
    paper.physicalWidthMm = 100.0;
    paper.physicalHeightMm = 150.0;
    paper.logicalWidthMm = 101.6;
    paper.logicalHeightMm = 152.4;
    paper.type = PaperType::Glossy;
    return paper;
}

PaperSpec a4Paper()
{
    PaperSpec paper;
    paper.name = QStringLiteral("A4");
    paper.physicalWidthMm = 210.0;
    paper.physicalHeightMm = 297.0;
    return paper;
}

DocumentSnapshot documentFor(const PaperSpec &paper, bool landscape)
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-1");
    page.paper = paper;
    page.landscape = landscape;
    document.pages.append(page);
    return document;
}

QString runTool(const QString &program, const QStringList &arguments, int *exitCode)
{
    QProcess process;
    process.start(program, arguments);
    process.waitForFinished(120000);
    *exitCode = process.exitCode();
    return QString::fromLocal8Bit(process.readAllStandardOutput())
        + QString::fromLocal8Bit(process.readAllStandardError());
}

struct PdfPage {
    double widthPt = 0;
    double heightPt = 0;
    int rotation = -1;
    QImage raster;
};

bool inspect(const QString &pdfPath, const QString &base, PdfPage *page)
{
    int exitCode = -1;
    const QString info = runTool(QStringLiteral("pdfinfo"), {pdfPath}, &exitCode);
    if (exitCode != 0)
        return false;
    const auto sizeMatch = QRegularExpression(QStringLiteral("Page size:\\s+([0-9.]+) x ([0-9.]+) pts"))
                               .match(info);
    if (!sizeMatch.hasMatch())
        return false;
    page->widthPt = sizeMatch.captured(1).toDouble();
    page->heightPt = sizeMatch.captured(2).toDouble();
    const auto rotateMatch = QRegularExpression(QStringLiteral("Page rot:\\s+(-?\\d+)")).match(info);
    page->rotation = rotateMatch.hasMatch() ? rotateMatch.captured(1).toInt() : -1;

    const QString rendered = runTool(QStringLiteral("pdftoppm"),
                                     {QStringLiteral("-r"), QStringLiteral("300"),
                                      QStringLiteral("-png"), QStringLiteral("-singlefile"), pdfPath,
                                      base},
                                     &exitCode);
    Q_UNUSED(rendered)
    if (exitCode != 0)
        return false;
    page->raster = QImage(base + QStringLiteral(".png"));
    return !page->raster.isNull();
}

double darkness(const QImage &image, int x, int y)
{
    const QRgb pixel = image.pixel(x, y);
    const double luminance =
        0.2126 * qRed(pixel) + 0.7152 * qGreen(pixel) + 0.0722 * qBlue(pixel);
    return std::max(0.0, (255.0 - luminance) / 255.0);
}

double centroidY(const QImage &image, int x, int yFrom, int yTo)
{
    double weighted = 0.0;
    double total = 0.0;
    for (int y = yFrom; y <= yTo; ++y) {
        const double weight = darkness(image, x, y);
        weighted += weight * y;
        total += weight;
    }
    return total > 0.0 ? weighted / total : -1.0;
}

double centroidX(const QImage &image, int y, int xFrom, int xTo)
{
    double weighted = 0.0;
    double total = 0.0;
    for (int x = xFrom; x <= xTo; ++x) {
        const double weight = darkness(image, x, y);
        weighted += weight * x;
        total += weight;
    }
    return total > 0.0 ? weighted / total : -1.0;
}

} // namespace

class TestPaperGeometry : public QObject
{
    Q_OBJECT

private slots:
    void physicalAndLogicalSizesStaySeparate();
    void orientationSwapsBothRectangles();
    void portraitLogicalPdfHasExactMediaBox();
    void landscapeLogicalPdfHasExactMediaBox();
    void physicalPdfKeepsTheSmallSheet();
    void marksSitAtPhysicalMillimetres();
    void explicitOffsetIsAppliedOnce();
    void borderlessPdfFillsTheLogicalPage();
    void grayscalePreviewAndPdfAreNeutral();
};

void TestPaperGeometry::physicalAndLogicalSizesStaySeparate()
{
    const PaperGeometry geometry = PaperGeometry::forPaper(photoPaper(), false);
    QCOMPARE(geometry.physicalSizeMm(), QSizeF(100.0, 150.0));
    QCOMPARE(geometry.logicalSizeMm(), QSizeF(101.6, 152.4));
    QVERIFY(!geometry.offsetVerified);
    QCOMPARE(geometry.physicalOriginInLogicalMm, QPointF(0.0, 0.0));
    QVERIFY(!geometry.imageableKnown);
}

void TestPaperGeometry::orientationSwapsBothRectangles()
{
    const PaperGeometry landscape = PaperGeometry::forPaper(photoPaper(), true);
    QCOMPARE(landscape.physicalSizeMm(), QSizeF(150.0, 100.0));
    QCOMPARE(landscape.logicalSizeMm(), QSizeF(152.4, 101.6));

    const PaperGeometry a4 = PaperGeometry::forPaper(a4Paper(), true);
    QCOMPARE(a4.physicalSizeMm(), QSizeF(297.0, 210.0));
    QCOMPARE(a4.logicalSizeMm(), QSizeF(297.0, 210.0));
}

void TestPaperGeometry::portraitLogicalPdfHasExactMediaBox()
{
    const QString path = evidenceDir() + QStringLiteral("/paper-4x6-portrait.pdf");
    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    options.developerMarks = true;
    options.mediaSizeMm = QSizeF(101.6, 152.4);

    const auto artifact = renderer.writePdf(documentFor(photoPaper(), false), path, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    PdfPage page;
    QVERIFY(inspect(path, evidenceDir() + QStringLiteral("/paper-4x6-portrait"), &page));
    QVERIFY2(std::abs(page.widthPt - 288.0) < 0.2,
             qPrintable(QStringLiteral("width %1").arg(page.widthPt)));
    QVERIFY2(std::abs(page.heightPt - 432.0) < 0.2,
             qPrintable(QStringLiteral("height %1").arg(page.heightPt)));
    QCOMPARE(page.rotation, 0);
}

void TestPaperGeometry::landscapeLogicalPdfHasExactMediaBox()
{
    const QString path = evidenceDir() + QStringLiteral("/paper-4x6-landscape.pdf");
    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    options.developerMarks = true;
    options.mediaSizeMm = QSizeF(152.4, 101.6);

    const auto artifact = renderer.writePdf(documentFor(photoPaper(), true), path, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    PdfPage page;
    QVERIFY(inspect(path, evidenceDir() + QStringLiteral("/paper-4x6-landscape"), &page));
    QVERIFY2(std::abs(page.widthPt - 432.0) < 0.2,
             qPrintable(QStringLiteral("width %1").arg(page.widthPt)));
    QVERIFY2(std::abs(page.heightPt - 288.0) < 0.2,
             qPrintable(QStringLiteral("height %1").arg(page.heightPt)));
    QCOMPARE(page.rotation, 0);

    const double pxPerMm = 300.0 / 25.4;
    const double leftTick = centroidX(page.raster, qRound((100.0 - 12.0 - 2.0) * pxPerMm),
                                      qRound(18.0 * pxPerMm), qRound(22.0 * pxPerMm));
    const double rightTick = centroidX(page.raster, qRound((100.0 - 12.0 - 2.0) * pxPerMm),
                                       qRound(68.0 * pxPerMm), qRound(72.0 * pxPerMm));
    QVERIFY(leftTick > 0.0 && rightTick > 0.0);
    QVERIFY2(std::abs((rightTick - leftTick) / pxPerMm - 50.0) < 0.1,
             qPrintable(QStringLiteral("ruler %1 mm").arg((rightTick - leftTick) / pxPerMm)));
}

void TestPaperGeometry::physicalPdfKeepsTheSmallSheet()
{
    const QString path = evidenceDir() + QStringLiteral("/paper-100x150.pdf");
    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    options.developerMarks = true;

    const auto artifact = renderer.writePdf(documentFor(photoPaper(), false), path, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    PdfPage page;
    QVERIFY(inspect(path, evidenceDir() + QStringLiteral("/paper-100x150"), &page));
    QVERIFY2(std::abs(page.widthPt - 100.0 / 25.4 * 72.0) < 0.2,
             qPrintable(QStringLiteral("width %1").arg(page.widthPt)));
    QVERIFY2(std::abs(page.heightPt - 150.0 / 25.4 * 72.0) < 0.2,
             qPrintable(QStringLiteral("height %1").arg(page.heightPt)));
}

void TestPaperGeometry::marksSitAtPhysicalMillimetres()
{
    const QString path = evidenceDir() + QStringLiteral("/paper-marks.pdf");
    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    options.developerMarks = true;
    options.mediaSizeMm = QSizeF(101.6, 152.4);

    const auto artifact = renderer.writePdf(documentFor(photoPaper(), false), path, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    PdfPage page;
    QVERIFY(inspect(path, evidenceDir() + QStringLiteral("/paper-marks"), &page));
    const double pxPerMm = 300.0 / 25.4;

    // Horizontal corner stroke sits exactly 5 mm below the physical top edge,
    // with no second margin offset on top of it.
    const double markY =
        centroidY(page.raster, qRound(10.0 * pxPerMm), qRound(3.0 * pxPerMm),
                  qRound(8.0 * pxPerMm));
    QVERIFY2(markY > 0.0, "corner mark not found");
    QVERIFY2(std::abs(markY / pxPerMm - 5.0) < 0.1,
             qPrintable(QStringLiteral("corner mark at %1 mm").arg(markY / pxPerMm)));
}

void TestPaperGeometry::explicitOffsetIsAppliedOnce()
{
    const QString path = evidenceDir() + QStringLiteral("/paper-offset.pdf");
    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    options.developerMarks = true;
    options.mediaSizeMm = QSizeF(101.6, 152.4);
    options.contentOffsetMm = QPointF(0.8, 1.2);

    const auto artifact = renderer.writePdf(documentFor(photoPaper(), false), path, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    PdfPage page;
    QVERIFY(inspect(path, evidenceDir() + QStringLiteral("/paper-offset"), &page));
    const double pxPerMm = 300.0 / 25.4;
    const double markY =
        centroidY(page.raster, qRound(10.8 * pxPerMm), qRound(5.0 * pxPerMm),
                  qRound(11.0 * pxPerMm));
    QVERIFY2(markY > 0.0, "offset corner mark not found");
    QVERIFY2(std::abs(markY / pxPerMm - 6.2) < 0.1,
             qPrintable(QStringLiteral("offset mark at %1 mm").arg(markY / pxPerMm)));
}

void TestPaperGeometry::borderlessPdfFillsTheLogicalPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("green.png"));
    QImage green(200, 300, QImage::Format_RGB32);
    green.setColorSpace(QColorSpace(QColorSpace::SRgb));
    green.fill(QColor(30, 160, 70));
    QImageWriter writer(imagePath, "png");
    QVERIFY(writer.write(green));

    ImageStore store;
    const auto source = store.load(imagePath, ImportPolicy{});
    QVERIFY(source.ok());

    Frame frame;
    frame.id = QStringLiteral("green");
    frame.sourcePath = source.value().path;
    frame.sourceSha256 = source.value().sha256;
    frame.cx = 50.0;
    frame.cy = 75.0;
    frame.width = 100.0;
    frame.height = 150.0;
    frame.fit = FitMode::Fill;

    Page page;
    page.id = QStringLiteral("page");
    page.paper = photoPaper();
    page.frames.append(frame);
    DocumentSnapshot document;
    document.pages.append(page);

    PageRenderer renderer(store);
    const QString filledPath = dir.filePath(QStringLiteral("borderless-filled.pdf"));
    RenderOptions filled;
    filled.mediaSizeMm = QSizeF(101.6, 152.4);
    filled.contentScale = 101.6 / 100.0;
    filled.contentOffsetMm = QPointF(0.0, 0.0);
    QVERIFY(renderer.writePdf(document, filledPath, {}, filled).ok());

    const QString barePath = dir.filePath(QStringLiteral("borderless-bare.pdf"));
    RenderOptions bare;
    bare.mediaSizeMm = QSizeF(101.6, 152.4);
    QVERIFY(renderer.writePdf(document, barePath, {}, bare).ok());

    PdfPage filledPage;
    PdfPage barePage;
    QVERIFY(inspect(filledPath, dir.filePath(QStringLiteral("filled")), &filledPage));
    QVERIFY(inspect(barePath, dir.filePath(QStringLiteral("bare")), &barePage));
    const double pxPerMm = 300.0 / 25.4;
    auto sample = [&](const QImage &image, double xMm, double yMm) {
        return image.pixel(qRound(xMm * pxPerMm), qRound(yMm * pxPerMm));
    };
    const QRgb filledCorner = sample(filledPage.raster, 0.8, 0.8);
    QVERIFY2(qGreen(filledCorner) > 120 && qRed(filledCorner) < 80,
             qPrintable(QStringLiteral("filled corner %1/%2/%3")
                            .arg(qRed(filledCorner))
                            .arg(qGreen(filledCorner))
                            .arg(qBlue(filledCorner))));
    const QRgb bareCorner = sample(barePage.raster, 101.0, 151.8);
    QVERIFY2(qRed(bareCorner) > 248 && qGreen(bareCorner) > 248 && qBlue(bareCorner) > 248,
             "without cover scale the far edge must show the missing strip");
}

void TestPaperGeometry::grayscalePreviewAndPdfAreNeutral()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("colours.png"));
    QImage colours(300, 200, QImage::Format_RGB32);
    colours.setColorSpace(QColorSpace(QColorSpace::SRgb));
    {
        QPainter painter(&colours);
        painter.fillRect(QRect(0, 0, 100, 200), QColor(220, 40, 30));
        painter.fillRect(QRect(100, 0, 100, 200), QColor(30, 170, 70));
        painter.fillRect(QRect(200, 0, 100, 200), QColor(40, 60, 210));
    }
    QImageWriter writer(imagePath, "png");
    QVERIFY(writer.write(colours));

    ImageStore store;
    const auto source = store.load(imagePath, ImportPolicy{});
    QVERIFY(source.ok());

    Frame frame;
    frame.id = QStringLiteral("colours");
    frame.sourcePath = source.value().path;
    frame.sourceSha256 = source.value().sha256;
    frame.cx = 105.0;
    frame.cy = 148.5;
    frame.width = 180.0;
    frame.height = 120.0;
    frame.fit = FitMode::Fill;

    Page page;
    page.id = QStringLiteral("page");
    page.paper = a4Paper();
    page.frames.append(frame);
    DocumentSnapshot colourDocument;
    colourDocument.pages.append(page);
    DocumentSnapshot grayDocument = colourDocument;
    grayDocument.recipe.grayscale = true;

    PageRenderer renderer(store);
    const auto colourPage = renderer.renderPage(colourDocument, 0, 100.0);
    if (!colourPage)
        QFAIL(qPrintable(colourPage.error().message));
    const auto grayPage = renderer.renderPage(grayDocument, 0, 100.0);
    if (!grayPage)
        QFAIL(qPrintable(grayPage.error().message));

    auto maxChannelSpread = [](const QImage &image, int x, int y) {
        const QRgb pixel = image.pixel(x, y);
        const int maxValue = std::max({qRed(pixel), qGreen(pixel), qBlue(pixel)});
        const int minValue = std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)});
        return maxValue - minValue;
    };

    int colourSpread = 0;
    int graySpread = 0;
    int darkest = 255;
    const double previewPxPerMm = 100.0 / 25.4;
    for (double yMm = 95.0; yMm < 200.0; yMm += 12.0) {
        for (double xMm = 20.0; xMm < 190.0; xMm += 12.0) {
            const int x = qRound(xMm * previewPxPerMm);
            const int y = qRound(yMm * previewPxPerMm);
            colourSpread = std::max(colourSpread, maxChannelSpread(colourPage.value(), x, y));
            graySpread = std::max(graySpread, maxChannelSpread(grayPage.value(), x, y));
            darkest = std::min(darkest, qGray(grayPage.value().pixel(x, y)));
        }
    }
    QVERIFY2(colourSpread > 60, qPrintable(QStringLiteral("colour spread %1").arg(colourSpread)));
    QVERIFY2(graySpread <= 2, qPrintable(QStringLiteral("grey spread %1").arg(graySpread)));
    QVERIFY2(darkest < 200, "the grey page must still contain the photograph");

    const QString pdfPath = dir.filePath(QStringLiteral("grayscale.pdf"));
    RenderOptions pdfOptions;
    pdfOptions.printPpi = 150.0;
    const auto artifact = renderer.writePdf(grayDocument, pdfPath, {}, pdfOptions);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    PdfPage pdfPage;
    QVERIFY(inspect(pdfPath, dir.filePath(QStringLiteral("grayscale")), &pdfPage));
    int pdfSpread = 0;
    const double pdfPxPerMm = 300.0 / 25.4;
    for (double yMm = 95.0; yMm < 200.0; yMm += 20.0) {
        for (double xMm = 20.0; xMm < 190.0; xMm += 20.0) {
            pdfSpread = std::max(pdfSpread,
                                 maxChannelSpread(pdfPage.raster, qRound(xMm * pdfPxPerMm),
                                                  qRound(yMm * pdfPxPerMm)));
        }
    }
    QVERIFY2(pdfSpread <= 2, qPrintable(QStringLiteral("pdf grey spread %1").arg(pdfSpread)));
}

QTEST_GUILESS_MAIN(TestPaperGeometry)
#include "tst_paper_geometry.moc"
