#include "ColorPipeline.h"
#include "Document.h"
#include "ImageSource.h"
#include "LayoutEngine.h"
#include "PageRenderer.h"

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

using namespace pp;

namespace {

const QString kRealPhoto = QStringLiteral("/home/lavx/Downloads/IMG20260830150231.jpg");

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

QImage makePatchImage(const QColorSpace &space)
{
    QImage image(400, 200, QImage::Format_RGB32);
    image.setColorSpace(space);
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, 200, 100), QColor(230, 40, 40));
    painter.fillRect(QRect(200, 0, 200, 100), QColor(40, 200, 60));
    painter.fillRect(QRect(0, 100, 200, 100), QColor(40, 60, 220));
    painter.fillRect(QRect(200, 100, 200, 100), QColor(128, 128, 128));
    painter.end();
    return image;
}

int channelDistance(QRgb a, QRgb b)
{
    return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                     std::abs(qBlue(a) - qBlue(b))});
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

struct Fixture {
    QTemporaryDir dir;
    DocumentSnapshot snapshot;
    SourceImage patchSource;
    bool ready = false;
    QString error;
};

/// A4 page with the real family photo and one synthetic P3 colour field so the
/// exported PDF proves both real-photo handling and the P3 -> sRGB conversion.
bool buildFixture(Fixture &fixture, ImageStore &store)
{
    const auto photo = store.load(kRealPhoto, ImportPolicy{});
    if (!photo) {
        fixture.error = photo.error().message;
        return false;
    }

    const QString patchPath = fixture.dir.filePath(QStringLiteral("p3-fields.png"));
    const QImage patch = makePatchImage(QColorSpace(QColorSpace::DisplayP3));
    QImageWriter writer(patchPath, "png");
    if (!writer.write(patch)) {
        fixture.error = QStringLiteral("patch write failed");
        return false;
    }
    const auto loadedPatch = store.load(patchPath, ImportPolicy{});
    if (!loadedPatch) {
        fixture.error = loadedPatch.error().message;
        return false;
    }
    fixture.patchSource = loadedPatch.value();

    Page page;
    page.id = makeId(QStringLiteral("page"));
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    page.landscape = false;

    Frame photoFrame;
    photoFrame.id = makeId(QStringLiteral("frame"));
    photoFrame.sourcePath = photo.value().path;
    photoFrame.sourceSha256 = photo.value().sha256;
    photoFrame.cx = 90.0;
    photoFrame.cy = 150.0;
    photoFrame.width = 120.0;
    photoFrame.height = 90.0;
    photoFrame.fit = FitMode::Full;
    page.frames.append(photoFrame);

    Frame patchFrame;
    patchFrame.id = makeId(QStringLiteral("frame"));
    patchFrame.sourcePath = fixture.patchSource.path;
    patchFrame.sourceSha256 = fixture.patchSource.sha256;
    patchFrame.cx = 185.0;
    patchFrame.cy = 20.0;
    patchFrame.width = 40.0;
    patchFrame.height = 20.0;
    patchFrame.fit = FitMode::Full;
    page.frames.append(patchFrame);

    fixture.snapshot.pages.append(page);
    fixture.ready = true;
    return true;
}

double meanLuminance(const QImage &image, const QRect &area)
{
    const QRect clipped = area.intersected(image.rect());
    if (clipped.isEmpty())
        return 0.0;
    double sum = 0.0;
    qint64 count = 0;
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            sum += 0.2126 * qRed(pixel) + 0.7152 * qGreen(pixel) + 0.0722 * qBlue(pixel);
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

} // namespace

class TestFirstSheetPdf : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void realPhotoIsDrawnOnRealPaper();
    void pdfIsVerifiableWithoutPrinter();
    void originalFileStaysUntouched();
    void cleanupTestCase();

private:
    QTemporaryDir m_dir;
    Fixture m_fixture;
};

void TestFirstSheetPdf::initTestCase()
{
    QVERIFY(QFileInfo::exists(kRealPhoto));
    QVERIFY(m_dir.isValid());
    ImageStore store;
    QVERIFY2(buildFixture(m_fixture, store), qPrintable(m_fixture.error));
}

void TestFirstSheetPdf::realPhotoIsDrawnOnRealPaper()
{
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(m_fixture.snapshot, 0, 300.0);
    if (!rendered) QFAIL(qPrintable(rendered.error().message));
    const QImage page = rendered.value();

    QVERIFY(std::abs(page.width() - 210.0 / 25.4 * 300.0) <= 1.0);
    QVERIFY(std::abs(page.height() - 297.0 / 25.4 * 300.0) <= 1.0);

    // Fit mode leaves white paper beside the portrait photograph.
    const QRect leftBand(qRound(30.0 / 25.4 * 300.0), 0, qRound(15.0 / 25.4 * 300.0),
                         page.height());
    QVERIFY2(meanLuminance(page, leftBand) > 254.999,
             qPrintable(QStringLiteral("left band luminance %1").arg(meanLuminance(page, leftBand))));

    // The photograph area is real image content, not a placeholder fill.
    const QRect photoArea(qRound(70.0 / 25.4 * 300.0), qRound(120.0 / 25.4 * 300.0),
                          qRound(40.0 / 25.4 * 300.0), qRound(60.0 / 25.4 * 300.0));
    const double mean = meanLuminance(page, photoArea);
    QVERIFY2(mean < 250.0, qPrintable(QStringLiteral("photo area luminance %1").arg(mean)));

    // Pixel variance proves photographic detail rather than one flat colour.
    QSet<QRgb> colours;
    for (int y = photoArea.top(); y <= photoArea.bottom(); y += 3) {
        for (int x = photoArea.left(); x <= photoArea.right(); x += 3)
            colours.insert(page.pixel(x, y));
    }
    QVERIFY2(colours.size() > 50,
             qPrintable(QStringLiteral("distinct colours %1").arg(colours.size())));
}

void TestFirstSheetPdf::pdfIsVerifiableWithoutPrinter()
{
    ImageStore store;
    PageRenderer renderer(store);
    const QString pdfPath = evidenceDir() + QStringLiteral("/first-sheet.pdf");
    RenderOptions options;
    options.developerMarks = true;
    options.verifySources = true;
    const auto artifact = renderer.writePdf(m_fixture.snapshot, pdfPath, {}, options);
    if (!artifact) QFAIL(qPrintable(artifact.error().message));

    QVERIFY(QFileInfo::exists(pdfPath));
    QCOMPARE(artifact.value().pageCount, 1);
    QVERIFY(std::abs(artifact.value().firstPageSizeMm.width() - 210.0) < 0.01);
    QVERIFY(std::abs(artifact.value().firstPageSizeMm.height() - 297.0) < 0.01);
    QVERIFY(!artifact.value().sha256.isEmpty());

    int exitCode = -1;
    const QString info = runTool(QStringLiteral("pdfinfo"), {pdfPath}, &exitCode);
    QCOMPARE(exitCode, 0);
    const QRegularExpression pageSizeRe(
        QStringLiteral("Page size:\\s+([0-9.]+) x ([0-9.]+) pts"));
    const auto match = pageSizeRe.match(info);
    QVERIFY2(match.hasMatch(), qPrintable(info));
    QVERIFY2(std::abs(match.captured(1).toDouble() - 595.2756) < 0.2, qPrintable(match.captured(1)));
    QVERIFY2(std::abs(match.captured(2).toDouble() - 841.8898) < 0.2, qPrintable(match.captured(2)));

    const QRegularExpression pagesRe(QStringLiteral("Pages:\\s+(\\d+)"));
    const auto pagesMatch = pagesRe.match(info);
    QVERIFY(pagesMatch.hasMatch());
    QCOMPARE(pagesMatch.captured(1).toInt(), 1);

    // Independent renderer: poppler, not our own rasteriser.
    const QString base = evidenceDir() + QStringLiteral("/first-sheet");
    const QString render = runTool(QStringLiteral("pdftoppm"),
                                   {QStringLiteral("-r"), QStringLiteral("300"),
                                    QStringLiteral("-png"), QStringLiteral("-singlefile"), pdfPath, base},
                                   &exitCode);
    QCOMPARE(exitCode, 0);
    QVERIFY2(QFileInfo::exists(base + QStringLiteral(".png")), qPrintable(render));

    const QImage rendered(base + QStringLiteral(".png"));
    QVERIFY(!rendered.isNull());
    QVERIFY(std::abs(rendered.width() - 210.0 / 25.4 * 300.0) <= 1.0);
    QVERIFY(std::abs(rendered.height() - 297.0 / 25.4 * 300.0) <= 1.0);

    const double pxPerMm = 300.0 / 25.4;
    auto darkness = [&](int x, int y) {
        const QRgb pixel = rendered.pixel(x, y);
        const double luminance = 0.2126 * qRed(pixel) + 0.7152 * qGreen(pixel)
            + 0.0722 * qBlue(pixel);
        return std::max(0.0, (255.0 - luminance) / 255.0);
    };
    auto centroidX = [&](int y, int xFrom, int xTo) {
        double weighted = 0.0;
        double total = 0.0;
        for (int x = xFrom; x <= xTo; ++x) {
            const double weight = darkness(x, y);
            weighted += weight * x;
            total += weight;
        }
        return total > 0.0 ? weighted / total : -1.0;
    };
    auto centroidY = [&](int x, int yFrom, int yTo) {
        double weighted = 0.0;
        double total = 0.0;
        for (int y = yFrom; y <= yTo; ++y) {
            const double weight = darkness(x, y);
            weighted += weight * y;
            total += weight;
        }
        return total > 0.0 ? weighted / total : -1.0;
    };

    // Top-left corner mark: horizontal stroke sits 5 mm below the top edge.
    const double markY = centroidY(qRound(10.0 * pxPerMm), qRound(3.0 * pxPerMm),
                                   qRound(7.0 * pxPerMm));
    QVERIFY2(markY > 0.0, "top-left corner mark not found");
    QVERIFY2(std::abs(markY - 5.0 * pxPerMm) < 1.2,
             qPrintable(QStringLiteral("corner mark at %1 px, expected %2")
                            .arg(markY)
                            .arg(5.0 * pxPerMm)));

    // Ruler element: 50 mm between the end tick centres.
    const double rulerYmm = 297.0 - 12.0;
    const int tickRow = qRound((rulerYmm - 2.0) * pxPerMm);
    const double leftTick = centroidX(tickRow, qRound(18.0 * pxPerMm), qRound(22.0 * pxPerMm));
    const double rightTick = centroidX(tickRow, qRound(68.0 * pxPerMm), qRound(72.0 * pxPerMm));
    QVERIFY2(leftTick > 0.0 && rightTick > 0.0, "ruler ticks not found");
    const double measuredMm = (rightTick - leftTick) / pxPerMm;
    QVERIFY2(std::abs(measuredMm - 50.0) < 0.1,
             qPrintable(QStringLiteral("ruler measured %1 mm").arg(measuredMm)));

    // Converted P3 colour fields inside the exported raster.
    const QImage expected = ColorPipeline::toSrgb(m_fixture.patchSource, ColorRecipe{});
    QVERIFY(!expected.isNull());
    const QList<QPoint> patchCentres = {QPoint(175, 15), QPoint(195, 15), QPoint(175, 25),
                                        QPoint(195, 25)};
    const QList<QPoint> sourcePoints = {QPoint(100, 50), QPoint(300, 50), QPoint(100, 150),
                                        QPoint(300, 150)};
    for (int i = 0; i < patchCentres.size(); ++i) {
        const QPoint pageMm = patchCentres.at(i);
        const QRgb inPdf = rendered.pixel(qRound(pageMm.x() * pxPerMm), qRound(pageMm.y() * pxPerMm));
        const QRgb wanted = expected.pixel(sourcePoints.at(i));
        const int distance = channelDistance(inPdf, wanted);
        QVERIFY2(distance <= 8,
                 qPrintable(QStringLiteral("PDF field %1 differs by %2: pdf %3/%4/%5, sRGB %6/%7/%8")
                                .arg(i)
                                .arg(distance)
                                .arg(qRed(inPdf))
                                .arg(qGreen(inPdf))
                                .arg(qBlue(inPdf))
                                .arg(qRed(wanted))
                                .arg(qGreen(wanted))
                                .arg(qBlue(wanted))));
    }

    const QString images = runTool(QStringLiteral("pdfimages"),
                                   {QStringLiteral("-list"), pdfPath}, &exitCode);
    QCOMPARE(exitCode, 0);
    QVERIFY2(images.contains(QStringLiteral("icc")), qPrintable(images));

    // Lossless encoding and a real embedded profile, checked on the bytes.
    QFile pdfFile(pdfPath);
    QVERIFY(pdfFile.open(QIODevice::ReadOnly));
    const QByteArray pdfBytes = pdfFile.readAll();
    QVERIFY(pdfBytes.contains("/FlateDecode"));
    QVERIFY(pdfBytes.contains("/ICCBased"));
    QVERIFY(pdfBytes.contains("/OutputIntents"));
}

void TestFirstSheetPdf::originalFileStaysUntouched()
{
    const QByteArray before = fileSha256(kRealPhoto);
    ImageStore store;
    PageRenderer renderer(store);
    const auto artifact = renderer.writePdf(m_fixture.snapshot, evidenceDir() + QStringLiteral("/sha-check.pdf"),
                                            {}, RenderOptions{});
    QVERIFY(artifact.ok());
    QCOMPARE(fileSha256(kRealPhoto), before);
    QFile::remove(evidenceDir() + QStringLiteral("/sha-check.pdf"));
}

void TestFirstSheetPdf::cleanupTestCase() {}

QTEST_GUILESS_MAIN(TestFirstSheetPdf)
#include "tst_first_sheet_pdf.moc"
