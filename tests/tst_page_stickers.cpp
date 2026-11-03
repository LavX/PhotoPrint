#include "CanvasController.h"
#include "ColorPipeline.h"
#include "Document.h"
#include "ImageSource.h"
#include "MainWindow.h"
#include "OverlayGeometry.h"
#include "PageRenderer.h"
#include "ProjectStore.h"
#include "SlotGeometry.h"

#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

#include <cmath>
#include <limits>

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

QString describe(QRgb colour)
{
    return QStringLiteral("%1/%2/%3").arg(qRed(colour)).arg(qGreen(colour)).arg(qBlue(colour));
}

QString writeImage(const QString &directory, const QString &name, const QImage &image)
{
    const QString path = QDir(directory).filePath(name);
    QImageWriter writer(path, "png");
    if (!writer.write(image))
        return {};
    return path;
}

QImage solidImage(const QColor &colour, QSize size = QSize(200, 200))
{
    QImage image(size, QImage::Format_ARGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
}

/// A square whose top half is red and bottom half is blue, so a rotation can be
/// told apart from a stretch.
QImage twoToneSquare()
{
    QImage image(200, 200, QImage::Format_ARGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 30, 220));
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, 200, 100), QColor(220, 30, 30));
    painter.end();
    return image;
}

Page a4Page()
{
    Page page;
    page.id = QStringLiteral("page-stickers");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    return page;
}

Sticker makeSticker(const QString &path, double cx, double cy, double width, double height,
                    int z = 0)
{
    Sticker sticker;
    sticker.id = makeId(QStringLiteral("sticker"));
    sticker.imagePath = path;
    sticker.imageSha256 = fileSha256(path);
    sticker.cx = cx;
    sticker.cy = cy;
    sticker.width = width;
    sticker.height = height;
    sticker.z = z;
    return sticker;
}

DocumentSnapshot stickerDocument(const QList<Sticker> &stickers)
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.stickers = stickers;
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

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool loadDocument(MainWindow &window, const QTemporaryDir &dir, const DocumentSnapshot &document)
{
    const QString path = dir.filePath(QStringLiteral("stickers.ppj"));
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

} // namespace

class TestPageStickers : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void stickerRendersAtItsBox();
    void stickerAlphaCompositesInLinearSpace();
    void stickerRotationTurnsAboutTheCentre();
    void stickerClipsAtThePageEdge();
    void stickerOpacityZeroAndHundred();
    void stickersPaintAbovePhotos();
    void equalZKeepsDocumentOrder();
    void raiseAndLowerChangeTheVisibleOrder();
    void hitTestingFollowsTheRotatedBox();
    void resizeKeepsTheAspectAndClamps();
    void defaultPlacementIsCentredAtAQuarterWidth();
    void printMatchesTheScreenRender();
    void persistenceRoundTrips();
    void oldProjectWithoutStickersWritesNoKey();
    void portableCopyCarriesStickerImages();
    void missingStickerPaintsPlaceholderAndReports();
    void unreadableStickerIsReported();
    void sanitiserClampsAndDropsBadIds();
    void addStickerFromPathPlacesAndSelects();
    void builtInStickerCanBeAdded();
    void elemTabEmptyStateAndSelection();
    void keyboardNudgeAndDelete();
    void dragCommitsOneUndoStep();
    void handleDragResizesAndKeepsTheAspect();
    void opacitySliderWritesOneStep();
    void orientationChangeTurnsStickersWithThePage();
    void largeFontKeepsElemReachable();
    void evidenceScreenshots();

private:
    QTemporaryDir m_dir;
    QString m_redPath;
    QString m_twoTonePath;
};

void TestPageStickers::initTestCase()
{
    QVERIFY(m_dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintStickerTest"));
    QSettings().clear();
    qputenv("PHOTOPRINT_NO_RECOVERY", "1");
    qputenv("PHOTOPRINT_NO_SETTINGS", "1");
    qputenv("PHOTOPRINT_NO_CUPS", "1");
    // The built-in set is copied into the app's data folder; keep that inside
    // the test's own directory so the run leaves nothing behind.
    qputenv("XDG_DATA_HOME", m_dir.path().toUtf8());

    m_redPath = writeImage(m_dir.path(), QStringLiteral("red.png"),
                           solidImage(QColor(220, 30, 30)));
    QVERIFY(!m_redPath.isEmpty());
    m_twoTonePath = writeImage(m_dir.path(), QStringLiteral("two-tone.png"), twoToneSquare());
    QVERIFY(!m_twoTonePath.isEmpty());
}

void TestPageStickers::stickerRendersAtItsBox()
{
    const Sticker sticker = makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0);
    const QImage page = render(stickerDocument({sticker}));
    QVERIFY(!page.isNull());
    const QRgb centre = probe(page, 105.0, 148.5);
    QVERIFY2(channelDistance(centre, qRgb(220, 30, 30)) <= 3,
             qPrintable(QStringLiteral("sticker centre is %1").arg(describe(centre))));
    // Outside the box the paper is untouched.
    const QRgb outside = probe(page, 60.0, 100.0);
    QVERIFY2(channelDistance(outside, qRgb(255, 255, 255)) <= 2,
             qPrintable(QStringLiteral("outside the box is %1").arg(describe(outside))));
}

void TestPageStickers::stickerAlphaCompositesInLinearSpace()
{
    const QString path = writeImage(m_dir.path(), QStringLiteral("half-red.png"),
                                    solidImage(QColor(220, 30, 30, 128)));
    QVERIFY(!path.isEmpty());
    const Sticker sticker = makeSticker(path, 105.0, 148.5, 40.0, 40.0);
    const QImage page = render(stickerDocument({sticker}));
    QVERIFY(!page.isNull());
    const QRgb expected =
        blendInLinear(qRgb(255, 255, 255), qRgb(220, 30, 30), 128.0 / 255.0);
    const QRgb actual = probe(page, 105.0, 148.5);
    QVERIFY2(channelDistance(actual, expected) <= 2,
             qPrintable(QStringLiteral("alpha sticker is %1, expected %2")
                            .arg(describe(actual), describe(expected))));
}

void TestPageStickers::stickerRotationTurnsAboutTheCentre()
{
    // The picture's top half is red. A 90 degree clockwise turn moves that half
    // to the right, so the right side of the box is red and the left is blue.
    Sticker sticker = makeSticker(m_twoTonePath, 105.0, 148.5, 60.0, 60.0);
    sticker.angleDeg = 90.0;
    const QImage page = render(stickerDocument({sticker}));
    QVERIFY(!page.isNull());
    const QRgb right = probe(page, 120.0, 148.5);
    const QRgb left = probe(page, 90.0, 148.5);
    QVERIFY2(channelDistance(right, qRgb(220, 30, 30)) <= 4,
             qPrintable(QStringLiteral("rotated right is %1").arg(describe(right))));
    QVERIFY2(channelDistance(left, qRgb(30, 30, 220)) <= 4,
             qPrintable(QStringLiteral("rotated left is %1").arg(describe(left))));
}

void TestPageStickers::stickerClipsAtThePageEdge()
{
    // The centre sits on the left edge, so half the box is off the sheet. The
    // raster is page-sized, so the overflow is cut with no second code path.
    const Sticker sticker = makeSticker(m_redPath, 0.0, 148.5, 40.0, 40.0);
    const QImage page = render(stickerDocument({sticker}));
    QVERIFY(!page.isNull());
    const QRgb inside = probe(page, 2.0, 148.5);
    QVERIFY2(channelDistance(inside, qRgb(220, 30, 30)) <= 3,
             qPrintable(QStringLiteral("visible half is %1").arg(describe(inside))));
    // The four corners of the raster are paper: nothing wrapped around.
    for (const QPointF &corner : {QPointF(1.0, 1.0), QPointF(209.0, 1.0),
                                  QPointF(1.0, 296.0), QPointF(209.0, 296.0)}) {
        const QRgb pixel = probe(page, corner.x(), corner.y());
        QVERIFY2(channelDistance(pixel, qRgb(255, 255, 255)) <= 2,
                 qPrintable(QStringLiteral("corner %1,%2 is %3")
                                .arg(corner.x())
                                .arg(corner.y())
                                .arg(describe(pixel))));
    }
}

void TestPageStickers::stickerOpacityZeroAndHundred()
{
    Sticker invisible = makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0);
    invisible.opacity = 0;
    const QImage zero = render(stickerDocument({invisible}));
    QVERIFY(!zero.isNull());
    QVERIFY2(channelDistance(probe(zero, 105.0, 148.5), qRgb(255, 255, 255)) <= 2,
             "opacity 0 is not the white paper");

    Sticker half = makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0);
    half.opacity = 50;
    const QImage fifty = render(stickerDocument({half}));
    QVERIFY(!fifty.isNull());
    const QRgb expected = blendInLinear(qRgb(255, 255, 255), qRgb(220, 30, 30), 0.5);
    QVERIFY2(channelDistance(probe(fifty, 105.0, 148.5), expected) <= 2,
             "opacity 50 is not a half blend");
}

void TestPageStickers::stickersPaintAbovePhotos()
{
    DocumentSnapshot document;
    Page page = a4Page();
    Frame frame;
    frame.id = QStringLiteral("frame-under");
    frame.sourcePath = m_twoTonePath;
    frame.sourceSha256 = fileSha256(m_twoTonePath);
    frame.cx = 105.0;
    frame.cy = 148.5;
    frame.width = 120.0;
    frame.height = 120.0;
    frame.fit = FitMode::Fill;
    page.frames.append(frame);
    page.stickers.append(makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0));
    document.pages.append(page);
    document.currentPage = 0;

    const QImage rendered = render(document);
    QVERIFY(!rendered.isNull());
    // The photograph is under the sticker, so the sticker's colour wins.
    const QRgb centre = probe(rendered, 105.0, 148.5);
    QVERIFY2(channelDistance(centre, qRgb(220, 30, 30)) <= 3,
             qPrintable(QStringLiteral("sticker over photo is %1").arg(describe(centre))));
    // Outside the sticker the photograph is visible.
    const QRgb photo = probe(rendered, 105.0, 100.0);
    QVERIFY2(channelDistance(photo, qRgb(255, 255, 255)) > 20,
             qPrintable(QStringLiteral("photo is missing at the probe: %1").arg(describe(photo))));
}

void TestPageStickers::equalZKeepsDocumentOrder()
{
    // Two stickers with the same z: the later one in document order paints on
    // top, which is the deterministic tie-break the model promises.
    const QString bluePath = writeImage(m_dir.path(), QStringLiteral("blue.png"),
                                        solidImage(QColor(30, 30, 220)));
    QVERIFY(!bluePath.isEmpty());
    Sticker red = makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0, 0);
    Sticker blue = makeSticker(bluePath, 105.0, 148.5, 40.0, 40.0, 0);
    const QImage page = render(stickerDocument({red, blue}));
    QVERIFY(!page.isNull());
    const QRgb centre = probe(page, 105.0, 148.5);
    QVERIFY2(channelDistance(centre, qRgb(30, 30, 220)) <= 3,
             qPrintable(QStringLiteral("equal z painted %1").arg(describe(centre))));
}

void TestPageStickers::raiseAndLowerChangeTheVisibleOrder()
{
    const QString bluePath = writeImage(m_dir.path(), QStringLiteral("blue-order.png"),
                                        solidImage(QColor(30, 30, 220)));
    QVERIFY(!bluePath.isEmpty());
    Sticker red = makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0, 0);
    Sticker blue = makeSticker(bluePath, 105.0, 148.5, 40.0, 40.0, 1);

    CanvasController controller;
    controller.setDocument(stickerDocument({red, blue}));
    const int before = controller.undoStack()->count();

    // Raise the red one: one press, one real step, even though the two z values
    // are adjacent. The helper renumbers every z to its position.
    Page page = controller.document().pages.first();
    QList<Sticker> order = OverlayGeometry::stickersInPaintOrder(page);
    QVERIFY(OverlayGeometry::reorderSticker(order, red.id, 1));
    page.stickers = order;
    QVERIFY(controller.publishPage(page, QStringLiteral("Rétegsorrend")).ok());
    QCOMPARE(controller.undoStack()->count(), before + 1);

    const QImage raised = render(controller.document());
    QVERIFY(!raised.isNull());
    QVERIFY2(channelDistance(probe(raised, 105.0, 148.5), qRgb(220, 30, 30)) <= 3,
             "the raised sticker is not on top");

    controller.undoStack()->undo();
    const QImage restored = render(controller.document());
    QVERIFY(!restored.isNull());
    QVERIFY2(channelDistance(probe(restored, 105.0, 148.5), qRgb(30, 30, 220)) <= 3,
             "one undo did not restore the previous order");

    // Lowering the top one is the same step in the other direction.
    Page lowered = controller.document().pages.first();
    QList<Sticker> loweredOrder = OverlayGeometry::stickersInPaintOrder(lowered);
    QVERIFY(OverlayGeometry::reorderSticker(loweredOrder, blue.id, -1));
    lowered.stickers = loweredOrder;
    QVERIFY(controller.publishPage(lowered, QStringLiteral("Rétegsorrend")).ok());
    const QImage afterLower = render(controller.document());
    QVERIFY2(channelDistance(probe(afterLower, 105.0, 148.5), qRgb(220, 30, 30)) <= 3,
             "lowering the top sticker did not change the order");
}

void TestPageStickers::hitTestingFollowsTheRotatedBox()
{
    Sticker sticker = makeSticker(m_redPath, 100.0, 100.0, 60.0, 20.0);
    sticker.angleDeg = 90.0;
    // A 60 x 20 box turned 90 degrees is 20 wide and 60 tall.
    QVERIFY(OverlayGeometry::stickerContains(sticker, QPointF(100.0, 120.0)));
    QVERIFY(!OverlayGeometry::stickerContains(sticker, QPointF(120.0, 100.0)));
    QVERIFY(!OverlayGeometry::stickerContains(sticker, QPointF(100.0, 140.0)));

    Page page = a4Page();
    Sticker lower = makeSticker(m_redPath, 100.0, 100.0, 40.0, 40.0, 0);
    Sticker upper = makeSticker(m_redPath, 100.0, 100.0, 40.0, 40.0, 5);
    page.stickers = {lower, upper};
    const Sticker *hit = OverlayGeometry::stickerAt(page, QPointF(100.0, 100.0));
    QVERIFY(hit);
    QCOMPARE(hit->id, upper.id);
    QVERIFY(!OverlayGeometry::stickerAt(page, QPointF(10.0, 10.0)));
}

void TestPageStickers::resizeKeepsTheAspectAndClamps()
{
    Sticker sticker = makeSticker(m_redPath, 100.0, 100.0, 40.0, 20.0);
    // The box doubles when the pointer is twice as far from the centre as the
    // corner was.
    const QPointF doubled(140.0, 120.0);
    const Sticker bigger = OverlayGeometry::resizedSticker(sticker, doubled, QSizeF(210, 297));
    QVERIFY(std::abs(bigger.width / bigger.height - 2.0) < 0.001);
    QVERIFY(bigger.width > sticker.width);

    // A pointer almost on the centre cannot collapse the sticker below the
    // model's minimum, and the aspect survives the clamp.
    const Sticker tiny = OverlayGeometry::resizedSticker(sticker, QPointF(100.5, 100.25),
                                                         QSizeF(210, 297));
    QVERIFY(tiny.width >= kMinStickerMm - 0.001);
    QVERIFY(tiny.height >= kMinStickerMm - 0.001);
    QVERIFY(std::abs(tiny.width / tiny.height - 2.0) < 0.001);

    // A move keeps the centre on the paper.
    const Sticker moved = OverlayGeometry::movedSticker(sticker, QPointF(500.0, -500.0),
                                                        QSizeF(210, 297));
    QCOMPARE(moved.cx, 210.0);
    QCOMPARE(moved.cy, 0.0);
}

void TestPageStickers::defaultPlacementIsCentredAtAQuarterWidth()
{
    const QSizeF size = OverlayGeometry::defaultStickerSizeMm(QSizeF(210.0, 297.0),
                                                              QSize(400, 200));
    QVERIFY(std::abs(size.width() - 52.5) < 0.01); // 25 % of the short side
    QVERIFY(std::abs(size.height() - 26.25) < 0.01); // the picture's aspect
    // An unknown pixel size still places a square.
    const QSizeF square = OverlayGeometry::defaultStickerSizeMm(QSizeF(210.0, 297.0), QSize());
    QVERIFY(std::abs(square.width() - square.height()) < 0.001);
}

void TestPageStickers::printMatchesTheScreenRender()
{
    // A solid sticker, rotated and semi-transparent: the two properties that
    // could take a second code path to paper.
    Sticker sticker = makeSticker(m_redPath, 105.0, 148.5, 60.0, 60.0);
    sticker.angleDeg = 30.0;
    sticker.opacity = 70;
    const DocumentSnapshot document = stickerDocument({sticker});

    const double ppi = 150.0;
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(document, 0, ppi);
    if (!rendered)
        QFAIL(qPrintable(rendered.error().message));

    const QString pdfPath = evidenceDir() + QStringLiteral("/stickers-print.pdf");
    RenderOptions options;
    options.printPpi = ppi;
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    QProcess process;
    const QString base = evidenceDir() + QStringLiteral("/stickers-print");
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, base});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    const QImage pdfPage(base + QStringLiteral(".png"));
    QVERIFY(!pdfPage.isNull());

    // The centre of the sticker and a paper corner: the PDF is the same raster,
    // so the two must agree within the usual 2/255.
    for (const QPointF &point : {QPointF(105.0, 148.5), QPointF(5.0, 5.0),
                                 QPointF(200.0, 290.0)}) {
        const QRgb fromRender = probe(rendered.value(), point.x(), point.y(), ppi);
        const QRgb fromPdf = probe(pdfPage, point.x(), point.y(), ppi);
        const int distance = channelDistance(fromRender, fromPdf);
        QVERIFY2(distance <= 2,
                 qPrintable(QStringLiteral("PDF vs render at %1,%2 differs by %3: %4 vs %5")
                                .arg(point.x())
                                .arg(point.y())
                                .arg(distance)
                                .arg(describe(fromRender), describe(fromPdf))));
    }
}

void TestPageStickers::persistenceRoundTrips()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Solid;
    page.background.solid = qRgb(0x33, 0x66, 0xcc);
    Sticker first = makeSticker(m_redPath, 60.0, 80.0, 30.0, 20.0, 0);
    first.angleDeg = -35.0;
    first.opacity = 65;
    Sticker second = makeSticker(m_twoTonePath, 150.0, 200.0, 45.0, 45.0, 3);
    page.stickers = {first, second};
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
    QCOMPARE(loaded.value().document.pages.first().stickers, page.stickers);
    QVERIFY(loaded.value().issues.isEmpty());
}

void TestPageStickers::oldProjectWithoutStickersWritesNoKey()
{
    DocumentSnapshot document;
    document.pages.append(a4Page());
    document.currentPage = 0;

    const QString path = m_dir.filePath(QStringLiteral("legacy.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, document, SaveMode::Normal).ok());
    const QJsonObject root = readJson(path);
    const QJsonObject pageObject = root.value(QStringLiteral("pages")).toArray().at(0).toObject();
    QVERIFY(!pageObject.contains(QStringLiteral("stickers")));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QVERIFY(loaded.value().document.pages.first().stickers.isEmpty());
}

void TestPageStickers::portableCopyCarriesStickerImages()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.stickers = {makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0)};
    document.pages.append(page);
    document.currentPage = 0;

    const QString originDir = m_dir.filePath(QStringLiteral("portable-stickers"));
    QVERIFY(QDir().mkpath(originDir));
    const QString path = QDir(originDir).filePath(QStringLiteral("portable.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, document, SaveMode::Portable);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));

    const QString bundle = path + QStringLiteral(".files");
    QVERIFY(QDir(bundle).exists());
    QVERIFY(QDir(bundle).entryList(QDir::Files).size() >= 1);

    const QJsonObject root = readJson(path);
    const QJsonObject stickerObject = root.value(QStringLiteral("pages")).toArray().at(0).toObject()
                                          .value(QStringLiteral("stickers")).toArray().at(0)
                                          .toObject();
    QVERIFY(!stickerObject.value(QStringLiteral("image")).toString().isEmpty());
    QVERIFY(!stickerObject.contains(QStringLiteral("original")));

    // The original outside the bundle can disappear; the copy still opens.
    const QString backup = m_dir.filePath(QStringLiteral("red-backup.png"));
    QVERIFY(QFile::rename(m_redPath, backup));
    {
        const auto loaded = store.load(path);
        if (!loaded)
            QFAIL(qPrintable(loaded.error().message));
        const QList<Sticker> stickers = loaded.value().document.pages.first().stickers;
        QCOMPARE(stickers.size(), 1);
        QVERIFY(!stickers.first().imagePath.isEmpty());
        QVERIFY(QFileInfo::exists(stickers.first().imagePath));
        QVERIFY(stickers.first().imagePath.startsWith(bundle));
        QVERIFY(loaded.value().issues.isEmpty());
    }
    QVERIFY(QFile::rename(backup, m_redPath));
}

void TestPageStickers::missingStickerPaintsPlaceholderAndReports()
{
    Sticker sticker = makeSticker(m_dir.filePath(QStringLiteral("gone.png")), 105.0, 148.5,
                                  40.0, 40.0);
    sticker.imageSha256 = QByteArrayLiteral("0123456789abcdef");
    const DocumentSnapshot document = stickerDocument({sticker});

    // The page still renders, and the placeholder's cross marks the centre.
    const QImage rendered = render(document);
    QVERIFY(!rendered.isNull());
    const QRgb centre = probe(rendered, 105.0, 148.5);
    QVERIFY2(qRed(centre) > qGreen(centre) + 40 && qRed(centre) > qBlue(centre) + 40,
             qPrintable(QStringLiteral("no placeholder at the centre: %1").arg(describe(centre))));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, document));
    QTest::qWait(150);
    auto *notice = window.findChild<QLabel *>(QStringLiteral("stickerNotice"));
    QVERIFY(notice);
    QVERIFY(notice->isVisible());
    QCOMPARE(notice->text(), QStringLiteral("Ezt a matricát nem találjuk."));

    // Selecting the sticker shows the same sentence in the Elem tab, with the
    // picker that relinks it.
    const QString stickerId = window.document().pages.first().stickers.first().id;
    QVERIFY(QMetaObject::invokeMethod(&window, "selectSticker", Qt::DirectConnection,
                                      Q_ARG(QString, stickerId)));
    QTest::qWait(120);
    auto *hint = window.findChild<QLabel *>(QStringLiteral("stickerMissingHint"));
    auto *relink = window.findChild<QPushButton *>(QStringLiteral("stickerRelinkButton"));
    QVERIFY(hint);
    QVERIFY(relink);
    QVERIFY(hint->isVisible());
    QVERIFY(relink->isVisible());
    QCOMPARE(hint->text(), notice->text());
}

void TestPageStickers::unreadableStickerIsReported()
{
    QVERIFY(OverlayGeometry::stickerSourceTooLarge(QSize(12000, 12000)));
    QVERIFY(!OverlayGeometry::stickerSourceTooLarge(QSize(4000, 3000)));
    QVERIFY(!OverlayGeometry::stickerSourceTooLarge(QSize()));

    const QString path = m_dir.filePath(QStringLiteral("not-an-image.png"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not an image");
    file.close();

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir,
                         stickerDocument({makeSticker(path, 105.0, 148.5, 40.0, 40.0)})));
    QTest::qWait(150);
    auto *notice = window.findChild<QLabel *>(QStringLiteral("stickerNotice"));
    QVERIFY(notice);
    QVERIFY(notice->isVisible());
    QCOMPARE(notice->text(), QStringLiteral("A matrica túl nagy vagy nem olvasható."));
}

void TestPageStickers::sanitiserClampsAndDropsBadIds()
{
    Sticker broken;
    broken.id = QStringLiteral("sticker-broken");
    broken.cx = std::numeric_limits<double>::quiet_NaN();
    broken.cy = 1e9;
    broken.width = -5.0;
    broken.height = std::numeric_limits<double>::infinity();
    broken.angleDeg = 720.0;
    broken.opacity = 500;
    broken.z = 999999;
    const Sticker sane = sanitiseSticker(broken, QSizeF(210.0, 297.0));
    QCOMPARE(sane.cx, 105.0);
    QCOMPARE(sane.cy, 297.0);
    QCOMPARE(sane.width, kMinStickerMm);
    QCOMPARE(sane.height, kMinStickerMm);
    QCOMPARE(sane.angleDeg, 0.0);
    QCOMPARE(sane.opacity, 100);
    QCOMPARE(sane.z, kMaxStickerZ);

    // An empty or repeated id cannot be selected or undone through, so it is
    // dropped; the list is capped at the model's maximum.
    QList<Sticker> many;
    Sticker noId = makeSticker(m_redPath, 10.0, 10.0, 10.0, 10.0);
    noId.id.clear();
    many.append(noId);
    Sticker duplicate = makeSticker(m_redPath, 20.0, 20.0, 10.0, 10.0);
    many.append(duplicate);
    many.append(duplicate);
    for (int i = 0; i < kMaxStickersPerPage + 5; ++i) {
        Sticker extra = makeSticker(m_redPath, 30.0, 30.0, 10.0, 10.0);
        extra.id = QStringLiteral("sticker-%1").arg(i);
        many.append(extra);
    }
    const QList<Sticker> kept = sanitiseStickers(many, QSizeF(210.0, 297.0));
    QCOMPARE(kept.size(), kMaxStickersPerPage);
    QCOMPARE(kept.first().id, duplicate.id);
}

void TestPageStickers::addStickerFromPathPlacesAndSelects()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    const int before = window.undoStackForTest()->count();

    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);

    const Page page = window.document().pages.first();
    QCOMPARE(page.stickers.size(), 1);
    const Sticker sticker = page.stickers.first();
    QCOMPARE(sticker.cx, 105.0);
    QCOMPARE(sticker.cy, 148.5);
    QVERIFY(std::abs(sticker.width - 52.5) < 0.01);
    QVERIFY(std::abs(sticker.height - 52.5) < 0.01);
    QCOMPARE(sticker.z, 1);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    // The new sticker is selected and the Elem tab opened by itself.
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("inspectorTabs"));
    auto *view = window.findChild<PageView *>();
    QVERIFY(tabs);
    QVERIFY(view);
    QCOMPARE(tabs->currentIndex(), 3);
    QCOMPARE(view->selectedSticker(), sticker.id);

    // A second sticker lands above the first.
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_twoTonePath)));
    QTest::qWait(120);
    const Page two = window.document().pages.first();
    QCOMPARE(two.stickers.size(), 2);
    QCOMPARE(two.stickers.at(1).z, 2);
}

void TestPageStickers::builtInStickerCanBeAdded()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);

    QVERIFY(QMetaObject::invokeMethod(&window, "addBuiltInSticker", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("star"))));
    QTest::qWait(150);
    const Page page = window.document().pages.first();
    QCOMPARE(page.stickers.size(), 1);
    const Sticker sticker = page.stickers.first();
    QVERIFY(!sticker.imagePath.isEmpty());
    QVERIFY(QFileInfo::exists(sticker.imagePath));
    QVERIFY(!sticker.imageSha256.isEmpty());
    // The shipped shape is copied into the app's own data folder, so a portable
    // copy can carry it like a photograph.
    QVERIFY(!sticker.imagePath.startsWith(QStringLiteral(":/")));
}

void TestPageStickers::elemTabEmptyStateAndSelection()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("inspectorTabs"));
    auto *view = window.findChild<PageView *>();
    auto *emptyPanel = window.findChild<QWidget *>(QStringLiteral("stickerEmptyPanel"));
    auto *stickerPanel = window.findChild<QWidget *>(QStringLiteral("stickerPanel"));
    auto *addButton = window.findChild<QPushButton *>(QStringLiteral("addStickerButton"));
    auto *setGrid = window.findChild<QWidget *>(QStringLiteral("stickerSetGrid"));
    QVERIFY(tabs);
    QVERIFY(view);
    QVERIFY(emptyPanel);
    QVERIFY(stickerPanel);
    QVERIFY(addButton);
    QVERIFY(setGrid);

    // Nothing selected: the tab is enabled and offers the way in.
    QVERIFY(tabs->isTabEnabled(3));
    tabs->setCurrentIndex(3);
    QTest::qWait(80);
    QVERIFY(emptyPanel->isVisible());
    QVERIFY(!stickerPanel->isVisible());
    QVERIFY(addButton->isVisible());
    QVERIFY(setGrid->isVisible());
    // The built-in set is a grid of twelve shapes.
    QCOMPARE(setGrid->findChildren<QToolButton *>().size(), 12);

    // Adding one selects it and swaps the panel to the properties.
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(120);
    QCOMPARE(tabs->currentIndex(), 3);
    QVERIFY(!emptyPanel->isVisible());
    QVERIFY(stickerPanel->isVisible());

    // Escape clears the selection: the panel falls back to Lap, and the empty
    // state is ready again the moment the tab is opened.
    QTest::keyClick(view, Qt::Key_Escape);
    QTest::qWait(120);
    QCOMPARE(tabs->currentIndex(), 2);
    QVERIFY(!emptyPanel->isHidden());
    QVERIFY(stickerPanel->isHidden());
    tabs->setCurrentIndex(3);
    QTest::qWait(80);
    QVERIFY(emptyPanel->isVisible());
    QVERIFY(!stickerPanel->isVisible());
}

void TestPageStickers::keyboardNudgeAndDelete()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(120);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const QString stickerId = window.document().pages.first().stickers.first().id;
    const double startX = window.document().pages.first().stickers.first().cx;
    const int before = window.undoStackForTest()->count();

    view->setFocus();
    QTest::keyClick(view, Qt::Key_Right);
    QTest::qWait(80);
    QCOMPARE(window.document().pages.first().stickers.first().cx, startX + 1.0);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    // Shift is the fine step, exactly like a photograph.
    QTest::keyClick(view, Qt::Key_Left, Qt::ShiftModifier);
    QTest::qWait(80);
    QCOMPARE(window.document().pages.first().stickers.first().cx, startX + 0.9);

    // Delete removes it in one step, and one undo brings it back.
    QTest::keyClick(view, Qt::Key_Delete);
    QTest::qWait(120);
    QVERIFY(window.document().pages.first().stickers.isEmpty());
    window.undoStackForTest()->undo();
    QTest::qWait(80);
    QCOMPARE(window.document().pages.first().stickers.size(), 1);
    QCOMPARE(window.document().pages.first().stickers.first().id, stickerId);
}

void TestPageStickers::dragCommitsOneUndoStep()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const Sticker start = window.document().pages.first().stickers.first();
    const int before = window.undoStackForTest()->count();

    // The canvas reports the sticker's rotated box, so the press lands on the
    // same outline the user sees.
    const QPolygonF quad = view->stickerQuad(start.id);
    QVERIFY(!quad.isEmpty());
    const QPointF from = quad.boundingRect().center();
    const QPointF to = from + QPointF(40.0, 25.0);
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    QTest::mouseMove(view, to.toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    QTest::qWait(150);

    const Sticker moved = window.document().pages.first().stickers.first();
    QVERIFY(moved.cx > start.cx);
    QVERIFY(moved.cy > start.cy);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    window.undoStackForTest()->undo();
    QTest::qWait(80);
    const Sticker restored = window.document().pages.first().stickers.first();
    QCOMPARE(restored.cx, start.cx);
    QCOMPARE(restored.cy, start.cy);
}

void TestPageStickers::handleDragResizesAndKeepsTheAspect()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const Sticker start = window.document().pages.first().stickers.first();
    const int before = window.undoStackForTest()->count();

    // The bottom-right corner of the rotated box, pulled away from the centre.
    QPolygonF quad = view->stickerQuad(start.id);
    QVERIFY(quad.size() == 4);
    const QPointF centre = (quad.at(0) + quad.at(2)) / 2.0;
    const QPointF corner = quad.at(2);
    const QPointF outward = corner + (corner - centre);
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, corner.toPoint());
    QTest::mouseMove(view, outward.toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, outward.toPoint());
    QTest::qWait(150);

    const Sticker resized = window.document().pages.first().stickers.first();
    QVERIFY(resized.width > start.width);
    QVERIFY(std::abs(resized.width / resized.height - start.width / start.height) < 0.01);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    // The rotate handle above the top edge turns the sticker about its centre.
    quad = view->stickerQuad(resized.id);
    const QPointF topMid = (quad.at(0) + quad.at(1)) / 2.0;
    const QPointF boxCentre = (quad.at(0) + quad.at(2)) / 2.0;
    QPointF direction = topMid - boxCentre;
    const double length = std::hypot(direction.x(), direction.y());
    QVERIFY(length > 0.0);
    direction /= length;
    const QPointF handle = topMid + direction * 30.0;
    // Half a turn around the centre: the handle ends up below the box.
    const QPointF turned = boxCentre - direction * (length + 30.0);
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, handle.toPoint());
    QTest::mouseMove(view, turned.toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, turned.toPoint());
    QTest::qWait(150);

    const Sticker rotated = window.document().pages.first().stickers.first();
    QVERIFY(std::abs(rotated.angleDeg - resized.angleDeg) > 90.0);
    QCOMPARE(window.undoStackForTest()->count(), before + 2);
}

void TestPageStickers::opacitySliderWritesOneStep()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);

    auto *slider = window.findChild<QSlider *>(QStringLiteral("stickerOpacitySlider"));
    auto *readout = window.findChild<QLabel *>(QStringLiteral("stickerOpacityValue"));
    QVERIFY(slider);
    QVERIFY(readout);
    QCOMPARE(slider->value(), 100);
    const int before = window.undoStackForTest()->count();

    slider->setValue(40);
    QTest::qWait(120);
    QCOMPARE(window.document().pages.first().stickers.first().opacity, 40);
    QCOMPARE(readout->text(), QStringLiteral("40 %"));
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    // The size field is centimetres, the model is millimetres, and the height
    // follows the stored aspect.
    auto *size = window.findChild<QDoubleSpinBox *>(QStringLiteral("stickerSizeSpin"));
    QVERIFY(size);
    QCOMPARE(size->suffix(), QStringLiteral(" cm"));
    size->setValue(10.0);
    emit size->editingFinished();
    QTest::qWait(120);
    const Sticker sticker = window.document().pages.first().stickers.first();
    QVERIFY(std::abs(sticker.width - 100.0) < 0.01);
    QVERIFY(std::abs(sticker.height - 100.0) < 0.01);
    QCOMPARE(window.undoStackForTest()->count(), before + 2);
}

void TestPageStickers::orientationChangeTurnsStickersWithThePage()
{
    DocumentSnapshot document;
    Page page = a4Page();
    Sticker sticker = makeSticker(m_redPath, 60.0, 80.0, 40.0, 20.0);
    page.stickers = {sticker};
    document.pages.append(page);
    document.currentPage = 0;

    CanvasController controller;
    controller.setDocument(document);
    QVERIFY(controller.apply(SetProjectLandscapeEdit{true}).ok());

    const Sticker turned = controller.document().pages.first().stickers.first();
    // Portrait to landscape: (x, y) -> (height - y, x), and the layer turns too.
    QCOMPARE(turned.cx, 297.0 - 80.0);
    QCOMPARE(turned.cy, 60.0);
    QCOMPARE(turned.angleDeg, 90.0);
    QCOMPARE(turned.width, 40.0);
    QCOMPARE(turned.height, 20.0);

    // One undo restores the portrait placement.
    controller.undoStack()->undo();
    const Sticker restored = controller.document().pages.first().stickers.first();
    QCOMPARE(restored.cx, 60.0);
    QCOMPARE(restored.cy, 80.0);
    QCOMPARE(restored.angleDeg, 0.0);
}

void TestPageStickers::largeFontKeepsElemReachable()
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
    QTest::qWait(150);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(200);

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("inspectorTabs"));
    auto *panel = window.findChild<QWidget *>(QStringLiteral("stickerPanel"));
    auto *size = window.findChild<QDoubleSpinBox *>(QStringLiteral("stickerSizeSpin"));
    QVERIFY(tabs);
    QVERIFY(panel);
    QVERIFY(size);
    QCOMPARE(tabs->currentIndex(), 3);
    QVERIFY(panel->isVisible());
    QVERIFY(size->isVisible());
    // The tab strip still fits four labels at the contract size.
    QVERIFY(tabs->tabBar()->count() == 4);
    QVERIFY(window.grab().save(
        evidenceDir() + QStringLiteral("/ui-sticker-large-font.png")));
    QApplication::setFont(saved);
}

void TestPageStickers::evidenceScreenshots()
{
    // A photograph with a sticker on it: the screenshot the slice is judged by.
    // The sticker is green so it reads against the photograph's blue half.
    const QString greenPath = writeImage(m_dir.path(), QStringLiteral("green.png"),
                                         solidImage(QColor(40, 160, 90)));
    QVERIFY(!greenPath.isEmpty());
    DocumentSnapshot document;
    Page page = a4Page();
    Frame frame;
    frame.id = QStringLiteral("frame-evidence");
    frame.sourcePath = m_twoTonePath;
    frame.sourceSha256 = fileSha256(m_twoTonePath);
    frame.cx = 105.0;
    frame.cy = 120.0;
    frame.width = 150.0;
    frame.height = 100.0;
    frame.fit = FitMode::Fill;
    page.frames.append(frame);
    Sticker sticker = makeSticker(greenPath, 160.0, 150.0, 50.0, 50.0);
    sticker.angleDeg = -15.0;
    page.stickers.append(sticker);
    document.pages.append(page);
    document.currentPage = 0;

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, document));
    QTest::qWait(200);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-sticker-on-photo.png")));

    // The built-in set, with nothing selected.
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QTest::keyClick(view, Qt::Key_Escape);
    QTest::qWait(120);
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("inspectorTabs"));
    QVERIFY(tabs);
    tabs->setCurrentIndex(3);
    QTest::qWait(150);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-sticker-set.png")));

    // The same page printed, so the sticker is on paper too.
    ImageStore store;
    PageRenderer renderer(store);
    const double ppi = 150.0;
    const QString pdfPath = evidenceDir() + QStringLiteral("/stickers-page.pdf");
    RenderOptions options;
    options.printPpi = ppi;
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));
    QProcess process;
    const QString base = evidenceDir() + QStringLiteral("/stickers-page");
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, base});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    QVERIFY(QFileInfo::exists(base + QStringLiteral(".png")));
}

QTEST_MAIN(TestPageStickers)
#include "tst_page_stickers.moc"
