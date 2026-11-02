#include "BackgroundGeometry.h"
#include "CanvasController.h"
#include "ColorPipeline.h"
#include "Document.h"
#include "ImageSource.h"
#include "MainWindow.h"
#include "PageRenderer.h"
#include "ProjectStore.h"
#include "SlotGeometry.h"

#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QTabWidget>
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

QRgb blendInLinear(QRgb background, QRgb foreground, double alpha)
{
    const int r = linearToSrgb(srgbToLinear(qRed(foreground)) * alpha
                               + srgbToLinear(qRed(background)) * (1.0 - alpha));
    const int g = linearToSrgb(srgbToLinear(qGreen(foreground)) * alpha
                               + srgbToLinear(qGreen(background)) * (1.0 - alpha));
    const int b = linearToSrgb(srgbToLinear(qBlue(foreground)) * alpha
                               + srgbToLinear(qBlue(background)) * (1.0 - alpha));
    return qRgb(r, g, b);
}

int channelDistance(QRgb a, QRgb b)
{
    return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                     std::abs(qBlue(a) - qBlue(b))});
}

QString writeImage(const QString &directory, const QString &name, const QImage &image)
{
    const QString path = QDir(directory).filePath(name);
    QImageWriter writer(path, "png");
    if (!writer.write(image))
        return {};
    return path;
}

/// A 2:1 picture whose left half is red and right half is blue, so a crop or a
/// tile can be told apart from a stretch.
QImage twoToneImage()
{
    QImage image(400, 200, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(220, 30, 30));
    QPainter painter(&image);
    painter.fillRect(QRect(200, 0, 200, 200), QColor(30, 30, 220));
    painter.end();
    return image;
}

QImage solidImage(const QColor &colour, QSize size = QSize(400, 200))
{
    QImage image(size, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
}

Page a4Page()
{
    Page page;
    page.id = QStringLiteral("page-background");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    return page;
}

DocumentSnapshot solidDocument(QRgb colour, double opacity = 100.0)
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Solid;
    page.background.solid = colour;
    page.background.opacity = opacity;
    document.pages.append(page);
    document.currentPage = 0;
    return document;
}

DocumentSnapshot gradientDocument(QRgb from, QRgb to, double angleDeg)
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Gradient;
    page.background.stops = {BackgroundStop{0.0, from}, BackgroundStop{1.0, to}};
    page.background.angleDeg = angleDeg;
    document.pages.append(page);
    document.currentPage = 0;
    return document;
}

DocumentSnapshot pictureDocument(const QString &path, BackgroundFit fit, double tileMm = 0.0)
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Image;
    page.background.imagePath = path;
    page.background.imageSha256 = fileSha256(path);
    page.background.fit = fit;
    page.background.tileMm = tileMm;
    document.pages.append(page);
    document.currentPage = 0;
    return document;
}

QImage render(const DocumentSnapshot &document, double ppi = 150.0)
{
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(document, 0, ppi);
    if (!rendered)
        return {};
    return rendered.value();
}

QRgb probe(const QImage &image, double xMm, double yMm, double ppi = 150.0)
{
    const double pxPerMm = ppi / 25.4;
    const int x = std::clamp(qRound(xMm * pxPerMm), 0, image.width() - 1);
    const int y = std::clamp(qRound(yMm * pxPerMm), 0, image.height() - 1);
    return image.pixel(x, y);
}

QString describe(QRgb colour)
{
    return QStringLiteral("%1/%2/%3").arg(qRed(colour)).arg(qGreen(colour)).arg(qBlue(colour));
}

bool loadDocument(MainWindow &window, const QTemporaryDir &dir, const DocumentSnapshot &document)
{
    const QString path = dir.filePath(QStringLiteral("background.ppj"));
    ProjectStore store;
    if (!store.save(path, document, SaveMode::Normal).ok())
        return false;
    const auto loaded = store.load(path);
    if (!loaded)
        return false;
    qRegisterMetaType<pp::LoadedProject>("pp::LoadedProject");
    if (!QMetaObject::invokeMethod(&window, "applyLoadedProject", Qt::DirectConnection,
                                   Q_ARG(pp::LoadedProject, loaded.value()), Q_ARG(bool, false))) {
        return false;
    }
    return window.waitForPreview(15000);
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

class TestPageBackground : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void solidColourMatchesThePickedValue();
    void solidOpacityBlendsInLinearSpace();
    void gradientRunsInLinearLight();
    void gradientAngleTurnsTheRamp();
    void pictureFillCoversTheSheet();
    void pictureFitLeavesBands();
    void pictureTileRepeatsAndIsCapped();
    void backgroundShowsInGapsAndEmptyCells();
    void freeCanvasPagePaintsTheBackground();
    void printMatchesTheScreenRender();
    void persistenceRoundTrips();
    void oldProjectWithoutBackgroundLoadsWhite();
    void portableCopyCarriesThePicture();
    void missingPicturePaintsWhiteAndReports();
    void undoRestoresThePreviousBackground();
    void resetReturnsToWhite();
    void largeFontKeepsThePanelReachable();
    void evidenceScreenshots();

private:
    QTemporaryDir m_dir;
    QString m_twoTonePath;
    QString m_solidPath;
};

void TestPageBackground::initTestCase()
{
    QVERIFY(m_dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintBackgroundTest"));
    QSettings().clear();
    qputenv("PHOTOPRINT_NO_RECOVERY", "1");
    qputenv("PHOTOPRINT_NO_SETTINGS", "1");
    qputenv("PHOTOPRINT_NO_CUPS", "1");

    m_twoTonePath = writeImage(m_dir.path(), QStringLiteral("two-tone.png"), twoToneImage());
    QVERIFY(!m_twoTonePath.isEmpty());
    m_solidPath = writeImage(m_dir.path(), QStringLiteral("solid.png"),
                             solidImage(QColor(30, 140, 60)));
    QVERIFY(!m_solidPath.isEmpty());
}

void TestPageBackground::solidColourMatchesThePickedValue()
{
    // The linear-canvas trap: painting the raw QColor would print about
    // #8fb0e8 instead of #3366cc. The 2/255 tolerance is the same one the
    // existing alpha test uses.
    const QRgb picked = qRgb(0x33, 0x66, 0xcc);
    const QImage page = render(solidDocument(picked));
    QVERIFY(!page.isNull());
    const QRgb actual = probe(page, 105.0, 148.5);
    QVERIFY2(channelDistance(actual, picked) <= 2,
             qPrintable(QStringLiteral("solid %1 differs: %2")
                            .arg(describe(picked), describe(actual))));
}

void TestPageBackground::solidOpacityBlendsInLinearSpace()
{
    // The blend happens in linear light, so the reference is
    // srgb(0.5 * linear(bg) + 0.5 * 1.0), not 0.5 * srgb(bg) + 0.5 * 255.
    const QRgb picked = qRgb(0x33, 0x66, 0xcc);
    const QRgb expected = blendInLinear(qRgb(255, 255, 255), picked, 0.5);
    const QImage page = render(solidDocument(picked, 50.0));
    QVERIFY(!page.isNull());
    const QRgb actual = probe(page, 105.0, 148.5);
    QVERIFY2(channelDistance(actual, expected) <= 2,
             qPrintable(QStringLiteral("50 %% %1 differs: %2 expected %3")
                            .arg(describe(picked), describe(actual), describe(expected))));
}

void TestPageBackground::gradientRunsInLinearLight()
{
    // Qt interpolates the stops in the destination format, which here is linear
    // Rec.2020. A black-to-white ramp therefore has its midpoint near sRGB 188,
    // not 128. The probe is the guard: if a Qt update changes the interpolation
    // space, this fails loudly instead of printing a washed-out ramp.
    const QImage page = render(gradientDocument(qRgb(0, 0, 0), qRgb(255, 255, 255), 0.0));
    QVERIFY(!page.isNull());
    const QRgb middle = probe(page, 105.0, 148.5);
    QVERIFY2(std::abs(qRed(middle) - 188) <= 3,
             qPrintable(QStringLiteral("gradient midpoint is %1, expected about 188")
                            .arg(qRed(middle))));
    const QRgb left = probe(page, 0.0, 148.5);
    const QRgb right = probe(page, 209.9, 148.5);
    QVERIFY2(qRed(left) <= 3, qPrintable(QStringLiteral("left end is %1").arg(qRed(left))));
    QVERIFY2(qRed(right) >= 252, qPrintable(QStringLiteral("right end is %1").arg(qRed(right))));
}

void TestPageBackground::gradientAngleTurnsTheRamp()
{
    const QImage page = render(gradientDocument(qRgb(0, 0, 0), qRgb(255, 255, 255), 90.0));
    QVERIFY(!page.isNull());
    const QRgb middle = probe(page, 105.0, 148.5);
    QVERIFY2(std::abs(qRed(middle) - 188) <= 3,
             qPrintable(QStringLiteral("vertical midpoint is %1").arg(qRed(middle))));
    // A 90 degree ramp is constant along a horizontal scan.
    const QRgb left = probe(page, 20.0, 148.5);
    const QRgb right = probe(page, 190.0, 148.5);
    QVERIFY2(channelDistance(left, right) <= 2,
             qPrintable(QStringLiteral("horizontal scan is not constant: %1 vs %2")
                            .arg(describe(left), describe(right))));
}

void TestPageBackground::pictureFillCoversTheSheet()
{
    const QImage page = render(pictureDocument(m_twoTonePath, BackgroundFit::Fill));
    QVERIFY(!page.isNull());
    // The 2:1 picture is cropped to the portrait sheet: the left half of the
    // page is the red half, the right half is the blue half, and no paper shows.
    const QRgb left = probe(page, 5.0, 148.5);
    const QRgb right = probe(page, 205.0, 148.5);
    QVERIFY2(channelDistance(left, qRgb(220, 30, 30)) <= 3,
             qPrintable(QStringLiteral("fill left is %1").arg(describe(left))));
    QVERIFY2(channelDistance(right, qRgb(30, 30, 220)) <= 3,
             qPrintable(QStringLiteral("fill right is %1").arg(describe(right))));
    // The corners are covered too: Fill leaves no paper anywhere.
    for (const QPointF &corner : {QPointF(1.0, 1.0), QPointF(209.0, 1.0),
                                  QPointF(1.0, 296.0), QPointF(209.0, 296.0)}) {
        const QRgb pixel = probe(page, corner.x(), corner.y());
        QVERIFY2(channelDistance(pixel, qRgb(255, 255, 255)) > 20,
                 qPrintable(QStringLiteral("fill left paper at %1,%2: %3")
                                .arg(corner.x())
                                .arg(corner.y())
                                .arg(describe(pixel))));
    }
}

void TestPageBackground::pictureFitLeavesBands()
{
    const QImage page = render(pictureDocument(m_twoTonePath, BackgroundFit::Fit));
    QVERIFY(!page.isNull());
    // A 2:1 picture on A4 is 210 x 105 mm, centred: bands above and below.
    const QRgb topBand = probe(page, 105.0, 20.0);
    const QRgb bottomBand = probe(page, 105.0, 277.0);
    QVERIFY2(channelDistance(topBand, qRgb(255, 255, 255)) <= 2,
             qPrintable(QStringLiteral("top band is %1").arg(describe(topBand))));
    QVERIFY2(channelDistance(bottomBand, qRgb(255, 255, 255)) <= 2,
             qPrintable(QStringLiteral("bottom band is %1").arg(describe(bottomBand))));
    // The picture is centred within 0.1 mm: the first non-white row is at
    // (297 - 105) / 2 = 96 mm.
    const double pxPerMm = 150.0 / 25.4;
    int firstRow = -1;
    for (int y = 0; y < page.height(); ++y) {
        if (channelDistance(page.pixel(page.width() / 2, y), qRgb(255, 255, 255)) > 20) {
            firstRow = y;
            break;
        }
    }
    QVERIFY(firstRow >= 0);
    const double firstMm = firstRow / pxPerMm;
    QVERIFY2(std::abs(firstMm - 96.0) <= 0.5,
             qPrintable(QStringLiteral("fit starts at %1 mm, expected 96").arg(firstMm)));
}

void TestPageBackground::pictureTileRepeatsAndIsCapped()
{
    const QImage page = render(pictureDocument(m_twoTonePath, BackgroundFit::Tile, 50.0));
    QVERIFY(!page.isNull());
    // A 50 mm wide tile of a 2:1 picture is 50 x 25 mm. The pixel one millimetre
    // into the second tile equals the pixel one millimetre into the first.
    const QRgb first = probe(page, 1.0, 1.0);
    const QRgb second = probe(page, 51.0, 26.0);
    QVERIFY2(channelDistance(first, second) <= 2,
             qPrintable(QStringLiteral("tile repeat differs: %1 vs %2")
                            .arg(describe(first), describe(second))));

    // A tile that would ask for thousands of blits is raised, not skipped.
    const QSizeF raised = BackgroundGeometry::tileSizeMm(
        0.1, QSize(400, 200), QSizeF(210.0, 297.0));
    QVERIFY(raised.width() > 0.0);
    QVERIFY(std::ceil(210.0 / raised.width()) <= kMaxTilesPerAxis);
    QVERIFY(std::ceil(297.0 / raised.height()) <= kMaxTilesPerAxis);
    const QImage capped = render(pictureDocument(m_twoTonePath, BackgroundFit::Tile, 0.1));
    QVERIFY(!capped.isNull());
}

void TestPageBackground::backgroundShowsInGapsAndEmptyCells()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Solid;
    page.background.solid = qRgb(0x33, 0x66, 0xcc);
    page.slotList = {Slot{QStringLiteral("slot-left"), QRectF(0.0, 0.0, 0.5, 1.0)},
                     Slot{QStringLiteral("slot-right"), QRectF(0.5, 0.0, 0.5, 1.0)}};
    // The left cell holds a photograph; the right cell stays empty.
    Frame frame;
    frame.id = QStringLiteral("frame-left");
    frame.sourcePath = m_solidPath;
    frame.sourceSha256 = fileSha256(m_solidPath);
    frame.slotId = QStringLiteral("slot-left");
    const QRectF cell = SlotGeometry::slotCellMm(page, page.slotList.first());
    frame.cx = cell.center().x();
    frame.cy = cell.center().y();
    frame.width = cell.width();
    frame.height = cell.height();
    frame.fit = FitMode::Fill;
    page.frames.append(frame);
    document.pages.append(page);
    document.currentPage = 0;

    const QImage rendered = render(document);
    QVERIFY(!rendered.isNull());
    const QRgb background = qRgb(0x33, 0x66, 0xcc);
    // The empty right cell, the gutter between the cells and the outer margin
    // all show the background, never white.
    const QRectF rightCell = SlotGeometry::slotCellMm(page, page.slotList.at(1));
    const QRgb inEmptyCell = probe(rendered, rightCell.center().x(), rightCell.center().y());
    const QRgb inGutter = probe(rendered, 105.0, 148.5);
    const QRgb inMargin = probe(rendered, 2.0, 2.0);
    for (const QRgb &pixel : {inEmptyCell, inGutter, inMargin}) {
        QVERIFY2(channelDistance(pixel, background) <= 2,
                 qPrintable(QStringLiteral("background missing: %1 expected %2")
                                .arg(describe(pixel), describe(background))));
    }
}

void TestPageBackground::freeCanvasPagePaintsTheBackground()
{
    // The default page type: no slot layout. The old early return would have
    // skipped the background entirely.
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Solid;
    page.background.solid = qRgb(0xcc, 0x33, 0x33);
    Frame frame;
    frame.id = QStringLiteral("frame-free");
    frame.sourcePath = m_solidPath;
    frame.sourceSha256 = fileSha256(m_solidPath);
    frame.cx = 105.0;
    frame.cy = 148.5;
    frame.width = 60.0;
    frame.height = 40.0;
    frame.fit = FitMode::Fill;
    page.frames.append(frame);
    document.pages.append(page);
    document.currentPage = 0;

    const QImage rendered = render(document);
    QVERIFY(!rendered.isNull());
    const QRgb corner = probe(rendered, 5.0, 5.0);
    QVERIFY2(channelDistance(corner, qRgb(0xcc, 0x33, 0x33)) <= 2,
             qPrintable(QStringLiteral("free page corner is %1").arg(describe(corner))));
}

void TestPageBackground::printMatchesTheScreenRender()
{
    const double ppi = 150.0;
    const QList<DocumentSnapshot> documents = {
        solidDocument(qRgb(0x33, 0x66, 0xcc)),
        gradientDocument(qRgb(0, 0, 0), qRgb(255, 255, 255), 45.0),
        pictureDocument(m_twoTonePath, BackgroundFit::Fill),
    };
    const QStringList names = {QStringLiteral("solid"), QStringLiteral("gradient"),
                               QStringLiteral("picture")};
    for (int i = 0; i < documents.size(); ++i) {
        ImageStore store;
        PageRenderer renderer(store);
        const auto rendered = renderer.renderPage(documents.at(i), 0, ppi);
        if (!rendered)
            QFAIL(qPrintable(rendered.error().message));

        const QString pdfPath =
            evidenceDir() + QStringLiteral("/background-%1.pdf").arg(names.at(i));
        RenderOptions options;
        options.printPpi = ppi;
        const auto artifact = renderer.writePdf(documents.at(i), pdfPath, {}, options);
        if (!artifact)
            QFAIL(qPrintable(artifact.error().message));

        QProcess process;
        const QString base =
            evidenceDir() + QStringLiteral("/background-%1").arg(names.at(i));
        process.start(QStringLiteral("pdftoppm"),
                      {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-png"),
                       QStringLiteral("-singlefile"), pdfPath, base});
        QVERIFY(process.waitForFinished(60000));
        QCOMPARE(process.exitCode(), 0);
        const QImage pdfPage(base + QStringLiteral(".png"));
        QVERIFY(!pdfPage.isNull());

        const QList<QPointF> probes = {QPointF(5.0, 5.0), QPointF(30.0, 148.5),
                                       QPointF(180.0, 148.5), QPointF(205.0, 292.0)};
        for (const QPointF &point : probes) {
            const QRgb fromRender = probe(rendered.value(), point.x(), point.y(), ppi);
            const QRgb fromPdf = probe(pdfPage, point.x(), point.y(), ppi);
            const int distance = channelDistance(fromRender, fromPdf);
            QVERIFY2(distance <= 2,
                     qPrintable(QStringLiteral("%1 PDF vs render at %2,%3 differs by %4: "
                                               "%5 vs %6")
                                    .arg(names.at(i))
                                    .arg(point.x())
                                    .arg(point.y())
                                    .arg(distance)
                                    .arg(describe(fromRender), describe(fromPdf))));
        }
    }
}

void TestPageBackground::persistenceRoundTrips()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Gradient;
    page.background.solid = qRgb(0x11, 0x22, 0x33);
    page.background.stops = {BackgroundStop{0.0, qRgb(0xff, 0x00, 0x00)},
                             BackgroundStop{0.4, qRgb(0x00, 0xff, 0x00)},
                             BackgroundStop{1.0, qRgb(0x00, 0x00, 0xff)}};
    page.background.angleDeg = 45.0;
    page.background.fit = BackgroundFit::Tile;
    page.background.tileMm = 30.0;
    page.background.opacity = 70.0;
    page.background.imagePath = m_twoTonePath;
    page.background.imageSha256 = fileSha256(m_twoTonePath);
    document.pages.append(page);
    document.currentPage = 0;

    const QString path = m_dir.filePath(QStringLiteral("round-trip.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, document, SaveMode::Normal);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));
    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QCOMPARE(loaded.value().document.pages.first().background, page.background);
    QVERIFY(loaded.value().issues.isEmpty());
}

void TestPageBackground::oldProjectWithoutBackgroundLoadsWhite()
{
    DocumentSnapshot document;
    Page page = a4Page();
    document.pages.append(page);
    document.currentPage = 0;

    const QString path = m_dir.filePath(QStringLiteral("legacy.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, document, SaveMode::Normal).ok());
    // A page that never chose a background writes no key at all, so a 1.0.1
    // project stays byte-identical.
    const QJsonObject root = readJson(path);
    const QJsonObject pageObject = root.value(QStringLiteral("pages")).toArray().at(0).toObject();
    QVERIFY(!pageObject.contains(QStringLiteral("background")));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QCOMPARE(loaded.value().document.pages.first().background, PageBackground{});
    QCOMPARE(loaded.value().document.pages.first().background.kind, BackgroundKind::None);
}

void TestPageBackground::portableCopyCarriesThePicture()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Image;
    page.background.imagePath = m_twoTonePath;
    page.background.imageSha256 = fileSha256(m_twoTonePath);
    page.background.fit = BackgroundFit::Fill;
    document.pages.append(page);
    document.currentPage = 0;

    const QString originDir = m_dir.filePath(QStringLiteral("portable-origin"));
    QVERIFY(QDir().mkpath(originDir));
    const QString path = QDir(originDir).filePath(QStringLiteral("portable.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, document, SaveMode::Portable);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));

    const QString bundle = path + QStringLiteral(".files");
    QVERIFY(QDir(bundle).exists());
    QVERIFY(QDir(bundle).entryList(QDir::Files).size() >= 1);

    // The bundle carries the picture and stores no absolute path.
    const QJsonObject root = readJson(path);
    const QJsonObject background =
        root.value(QStringLiteral("pages")).toArray().at(0).toObject()
            .value(QStringLiteral("background")).toObject();
    QVERIFY(!background.value(QStringLiteral("image")).toString().isEmpty());
    QVERIFY(!background.contains(QStringLiteral("original")));

    // The original outside the bundle can disappear; the copy still opens.
    const QString original = m_twoTonePath;
    const QString backup = m_dir.filePath(QStringLiteral("two-tone-backup.png"));
    QVERIFY(QFile::rename(original, backup));
    {
        const auto loaded = store.load(path);
        if (!loaded)
            QFAIL(qPrintable(loaded.error().message));
        const PageBackground &loadedBackground =
            loaded.value().document.pages.first().background;
        QCOMPARE(loadedBackground.kind, BackgroundKind::Image);
        QVERIFY(!loadedBackground.imagePath.isEmpty());
        QVERIFY(QFileInfo::exists(loadedBackground.imagePath));
        QVERIFY(loadedBackground.imagePath.startsWith(bundle));
        QVERIFY(loaded.value().issues.isEmpty());
    }
    QVERIFY(QFile::rename(backup, original));
}

void TestPageBackground::missingPicturePaintsWhiteAndReports()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Image;
    page.background.imagePath = m_dir.filePath(QStringLiteral("gone.png"));
    page.background.imageSha256 = QByteArrayLiteral("0123456789abcdef");
    document.pages.append(page);
    document.currentPage = 0;

    // The renderer paints the white paper and never aborts the page.
    const QImage rendered = render(document);
    QVERIFY(!rendered.isNull());
    const QRgb centre = probe(rendered, 105.0, 148.5);
    QVERIFY2(channelDistance(centre, qRgb(255, 255, 255)) <= 2,
             qPrintable(QStringLiteral("missing picture painted %1").arg(describe(centre))));

    // The panel and the notice bar say why.
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, document));
    QTest::qWait(150);
    auto *hint = window.findChild<QLabel *>(QStringLiteral("backgroundMissingHint"));
    auto *relink = window.findChild<QPushButton *>(QStringLiteral("backgroundRelinkButton"));
    QVERIFY(hint);
    QVERIFY(relink);
    QVERIFY(hint->isVisible());
    QVERIFY(relink->isVisible());
}

void TestPageBackground::undoRestoresThePreviousBackground()
{
    CanvasController controller;
    controller.setDocument(solidDocument(qRgb(0x33, 0x66, 0xcc)));
    const int before = controller.undoStack()->count();

    Page page = controller.document().pages.first();
    page.background.kind = BackgroundKind::Gradient;
    page.background.stops = {BackgroundStop{0.0, qRgb(0, 0, 0)},
                             BackgroundStop{1.0, qRgb(255, 255, 255)}};
    const auto published = controller.publishPage(page, QStringLiteral("Háttér"));
    QVERIFY(published.ok());
    QCOMPARE(controller.undoStack()->count(), before + 1);
    QCOMPARE(controller.document().pages.first().background.kind, BackgroundKind::Gradient);

    controller.undoStack()->undo();
    QCOMPARE(controller.document().pages.first().background.kind, BackgroundKind::Solid);
    QCOMPARE(controller.document().pages.first().background.solid, qRgb(0x33, 0x66, 0xcc));
}

void TestPageBackground::resetReturnsToWhite()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, solidDocument(qRgb(0x33, 0x66, 0xcc))));
    QTest::qWait(150);

    auto *reset = window.findChild<QPushButton *>(QStringLiteral("backgroundResetButton"));
    QVERIFY(reset);
    QVERIFY(reset->isVisible());
    const int before = window.undoStackForTest()->count();
    QTest::mouseClick(reset, Qt::LeftButton);
    QTest::qWait(100);
    QCOMPARE(window.document().pages.first().background.kind, BackgroundKind::None);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);
    // The colour is kept, so switching back to a solid restores it.
    QCOMPARE(window.document().pages.first().background.solid, qRgb(0x33, 0x66, 0xcc));
}

void TestPageBackground::largeFontKeepsThePanelReachable()
{
    const QFont saved = QApplication::font();
    QFont large = saved;
    if (large.pointSizeF() > 0.0)
        large.setPointSizeF(saved.pointSizeF() * 1.35);
    else
        large.setPixelSize(int(std::lround(saved.pixelSize() * 1.35)));
    QApplication::setFont(large);

    MainWindow window;
    window.resize(1280, 720);
    window.show();
    QVERIFY(loadDocument(window, m_dir, solidDocument(qRgb(0x33, 0x66, 0xcc))));
    QTest::qWait(200);

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("inspectorTabs"));
    auto *view = window.findChild<PageView *>();
    QVERIFY(tabs);
    QVERIFY(view);
    QTest::keyClick(view, Qt::Key_Escape);
    QCOMPARE(tabs->currentIndex(), 2);

    auto *panel = window.findChild<QWidget *>(QStringLiteral("backgroundPanel"));
    auto *kind = window.findChild<QComboBox *>(QStringLiteral("backgroundKindCombo"));
    QVERIFY(panel);
    QVERIFY(kind);
    QVERIFY(panel->isVisible());
    QVERIFY(kind->isVisible());

    // The Lap page scrolls, so the background group is reachable even when the
    // layout controls above it fill the column.
    auto *scroll = qobject_cast<QScrollArea *>(tabs->currentWidget());
    QVERIFY(scroll);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QTest::qWait(120);
    QVERIFY(window.grab().save(
        evidenceDir() + QStringLiteral("/ui-background-large-font.png")));
    QApplication::setFont(saved);
}

void TestPageBackground::evidenceScreenshots()
{
    const struct {
        QString name;
        DocumentSnapshot document;
    } cases[] = {
        {QStringLiteral("solid"), solidDocument(qRgb(0x33, 0x66, 0xcc))},
        {QStringLiteral("gradient"),
         gradientDocument(qRgb(0x10, 0x2a, 0x6b), qRgb(0xf2, 0xc9, 0x4c), 45.0)},
        {QStringLiteral("picture"), pictureDocument(m_twoTonePath, BackgroundFit::Fill)},
    };
    for (const auto &entry : cases) {
        MainWindow window;
        window.resize(1280, 800);
        window.show();
        QVERIFY(loadDocument(window, m_dir, entry.document));
        QTest::qWait(200);
        QVERIFY2(window.grab().save(evidenceDir()
                                    + QStringLiteral("/ui-background-%1.png").arg(entry.name)),
                 qPrintable(entry.name));
    }
}

QTEST_MAIN(TestPageBackground)
#include "tst_page_background.moc"
