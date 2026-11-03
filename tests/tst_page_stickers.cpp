#include "CanvasController.h"
#include "ColorPipeline.h"
#include "Document.h"
#include "ImageSource.h"
#include "MainWindow.h"
#include "OverlayGeometry.h"
#include "PageRenderer.h"
#include "ProjectStore.h"
#include "SlotGeometry.h"
#include "StickerAssets.h"

#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
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
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
#include <zlib.h>

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


/// How many pixels carry the missing-picture red (#b3261e). A healthy page has
/// none of it; the canvas furniture does. The band is narrow on purpose: a red
/// photograph is a different red and must not count.
int placeholderPixels(const QImage &image)
{
    if (image.isNull())
        return 0;
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            if (std::abs(qRed(pixel) - 0xb3) <= 12 && std::abs(qGreen(pixel) - 0x26) <= 12
                && std::abs(qBlue(pixel) - 0x1e) <= 12)
                ++count;
        }
    }
    return count;
}

QByteArray bigEndian32(quint32 value)
{
    QByteArray out(4, Qt::Uninitialized);
    out[0] = char((value >> 24) & 0xff);
    out[1] = char((value >> 16) & 0xff);
    out[2] = char((value >> 8) & 0xff);
    out[3] = char(value & 0xff);
    return out;
}

QByteArray pngChunk(const char *tag, const QByteArray &payload)
{
    QByteArray body(tag, 4);
    body += payload;
    const uLong crc = crc32(0L, reinterpret_cast<const Bytef *>(body.constData()),
                            uInt(body.size()));
    return bigEndian32(quint32(payload.size())) + body + bigEndian32(quint32(crc));
}

/// A PNG that only carries a header, so a test can claim a 12000 x 12000 source
/// without allocating half a gigabyte. The reader reports the size from IHDR and
/// the add path refuses it before any decode.
QString writeHugePngHeader(const QString &directory, const QString &name, int width,
                           int height)
{
    QByteArray header = bigEndian32(quint32(width)) + bigEndian32(quint32(height));
    header.append(char(8)); // bit depth
    header.append(char(6)); // RGBA
    header.append(char(0));
    header.append(char(0));
    header.append(char(0));
    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    png += pngChunk("IHDR", header);
    // A valid image needs an IDAT for the reader to reach the header scan; the
    // add path refuses the source before anything decodes it.
    QByteArray compressed;
    compressed.resize(16);
    compressed.fill('\0');
    png += pngChunk("IDAT", compressed);
    png += pngChunk("IEND", QByteArray());
    const QString path = QDir(directory).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(png);
    file.close();
    return path;
}

/// Recursively removes a directory, so a test can prove a project survives the
/// app's data folder being wiped.
void removeTree(const QString &directory)
{
    QDir dir(directory);
    if (!dir.exists())
        return;
    dir.removeRecursively();
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
    void missingStickerIsCanvasFurniture();
    void unreadableStickerIsReported();
    void sanitiserClampsAndDropsBadIds();
    void rendererSanitisesFabricatedGeometry();
    void addStickerFromPathPlacesAndSelects();
    void builtinStickerIsStoredAsAnAlias();
    void builtinArtworkIsVersionPinned();
    void hugeSourceIsRejectedBeforeAdding();
    void elemTabEmptyStateAndSelection();
    void paletteStaysAvailableWithSelection();
    void emptyHintSkipsAPageWithASticker();
    void nudgeIsOneUndoPerBurst();
    void deleteIsOneUndoStep();
    void dragCommitsOneUndoStep();
    void handleDragResizesAndKeepsTheAspect();
    void opacitySliderPreviewsLive();
    void tallStickerKeepsItsAspect();
    void relinkingResetsTheBoxToTheNewAspect();
    void sizeSpinIsBoundedByThePage();
    void fitToPaperBringsStickersBack();
    void stickerAnnouncedToAssistiveTech();
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
    // Both axes are invalid, so both become the minimum: a drawable box rather
    // than a degenerate sliver.
    QCOMPARE(sane.width, kMinStickerMm);
    QCOMPARE(sane.height, kMinStickerMm);
    QCOMPARE(sane.angleDeg, 0.0);
    QCOMPARE(sane.opacity, 100);
    QCOMPARE(sane.z, kMaxStickerZ);

    // A tall but valid box keeps its aspect: the page bound scales both axes.
    Sticker tall;
    tall.id = QStringLiteral("sticker-tall");
    tall.cx = 105.0;
    tall.cy = 148.5;
    tall.width = 20.0;
    tall.height = 500.0;
    const Sticker bounded = sanitiseSticker(tall, QSizeF(210.0, 297.0));
    QVERIFY(bounded.height <= std::hypot(210.0, 297.0) + 0.001);
    QVERIFY(std::abs(bounded.width / bounded.height - 20.0 / 500.0) < 0.001);

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
    auto *addPanel = window.findChild<QWidget *>(QStringLiteral("stickerAddPanel"));
    auto *addButton = window.findChild<QPushButton *>(QStringLiteral("addStickerButton"));
    auto *setGrid = window.findChild<QWidget *>(QStringLiteral("stickerSetGrid"));
    QVERIFY(tabs);
    QVERIFY(view);
    QVERIFY(emptyPanel);
    QVERIFY(stickerPanel);
    QVERIFY(addPanel);
    QVERIFY(addButton);
    QVERIFY(setGrid);

    // Nothing selected: the tab is enabled, the palette is the way in, and the
    // one line explains the empty state.
    QVERIFY(tabs->isTabEnabled(3));
    tabs->setCurrentIndex(3);
    QTest::qWait(80);
    QVERIFY(emptyPanel->isVisible());
    QVERIFY(!stickerPanel->isVisible());
    QVERIFY(addPanel->isVisible());
    QVERIFY(addButton->isVisible());
    QVERIFY(setGrid->isVisible());
    // The built-in set is a grid of twelve shapes.
    QCOMPARE(setGrid->findChildren<QToolButton *>().size(), 12);

    // Adding one selects it and shows the properties, but the palette stays:
    // a second sticker must not need a deselect first.
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(120);
    QCOMPARE(tabs->currentIndex(), 3);
    QVERIFY(!emptyPanel->isVisible());
    QVERIFY(stickerPanel->isVisible());
    QVERIFY(addPanel->isVisible());
    QVERIFY(addButton->isVisible());
    QVERIFY(setGrid->isVisible());

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

void TestPageStickers::missingStickerIsCanvasFurniture()
{
    Sticker sticker = makeSticker(m_dir.filePath(QStringLiteral("gone.png")), 105.0, 148.5,
                                  40.0, 40.0);
    sticker.imageSha256 = QByteArrayLiteral("0123456789abcdef");
    const DocumentSnapshot document = stickerDocument({sticker});

    // The page raster carries no editor furniture: a lost sticker prints
    // nothing at all, never a red box.
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(document, 0, 150.0);
    if (!rendered)
        QFAIL(qPrintable(rendered.error().message));
    QCOMPARE(placeholderPixels(rendered.value()), 0);

    // The same holds for the PDF the printer uses.
    const QString pdfPath = evidenceDir() + QStringLiteral("/stickers-missing.pdf");
    RenderOptions options;
    options.printPpi = 150.0;
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));
    QProcess process;
    const QString base = evidenceDir() + QStringLiteral("/stickers-missing");
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QStringLiteral("150"), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, base});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    const QImage pdfPage(base + QStringLiteral(".png"));
    QVERIFY(!pdfPage.isNull());
    QCOMPARE(placeholderPixels(pdfPage), 0);

    // The canvas draws the placeholder instead, and the notice says why.
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, document));
    QTest::qWait(200);
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY2(placeholderPixels(view->grab().toImage()) > 0,
             "the canvas does not mark the missing sticker");
    auto *notice = window.findChild<QLabel *>(QStringLiteral("stickerNotice"));
    QVERIFY(notice);
    QVERIFY(notice->isVisible());
    QCOMPARE(notice->text(), QStringLiteral("Ezt a matricát nem találjuk."));

    // Selecting it shows the same sentence in the Elem tab with the picker.
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

void TestPageStickers::rendererSanitisesFabricatedGeometry()
{
    // A caller that never went through the controller can hand the renderer a
    // NaN box. It must still draw a page, and the sanitised sticker is the one
    // that lands.
    Sticker broken;
    broken.id = QStringLiteral("sticker-fabricated");
    broken.imagePath = m_redPath;
    broken.imageSha256 = fileSha256(m_redPath);
    broken.cx = std::numeric_limits<double>::quiet_NaN();
    broken.cy = std::numeric_limits<double>::quiet_NaN();
    broken.width = -10.0;
    broken.height = std::numeric_limits<double>::infinity();
    broken.angleDeg = std::numeric_limits<double>::quiet_NaN();

    const QImage page = render(stickerDocument({broken}));
    QVERIFY(!page.isNull());
    // The centre is clamped onto the sheet and the box to the minimum, so a red
    // square sits exactly on the middle.
    const QRgb centre = probe(page, 105.0, 148.5);
    QVERIFY2(channelDistance(centre, qRgb(220, 30, 30)) <= 4,
             qPrintable(QStringLiteral("fabricated sticker centre is %1")
                            .arg(describe(centre))));
}

void TestPageStickers::builtinStickerIsStoredAsAnAlias()
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
    // The model stores the pinned alias, never a copy on the user's disk.
    QCOMPARE(sticker.imagePath, QStringLiteral("builtin:star@1"));
    QVERIFY(!sticker.imageSha256.isEmpty());
    const QString resource = stickerSourcePath(sticker);
    QVERIFY(resource.startsWith(QStringLiteral(":/stickers/")));
    QVERIFY(QFileInfo::exists(resource));
    QVERIFY(!QFileInfo(sticker.imagePath).exists());

    // A normal save stores the alias and no absolute original.
    const QString firstDir = m_dir.filePath(QStringLiteral("alias-origin"));
    QVERIFY(QDir().mkpath(firstDir));
    const QString firstPath = QDir(firstDir).filePath(QStringLiteral("alias.ppj"));
    ProjectStore store;
    const auto saved = store.save(firstPath, window.document(), SaveMode::Normal);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));
    const QJsonObject root = readJson(firstPath);
    const QJsonObject stickerObject = root.value(QStringLiteral("pages")).toArray().at(0)
                                          .toObject().value(QStringLiteral("stickers"))
                                          .toArray().at(0).toObject();
    QCOMPARE(stickerObject.value(QStringLiteral("image")).toString(),
             QStringLiteral("builtin:star@1"));
    QVERIFY(!stickerObject.contains(QStringLiteral("original")));

    // Move the project to another folder and wipe the app's data folder: the
    // alias still resolves, because nothing was stored outside the project.
    const QString movedDir = m_dir.filePath(QStringLiteral("alias-moved"));
    QVERIFY(QDir().mkpath(movedDir));
    const QString movedPath = QDir(movedDir).filePath(QStringLiteral("alias.ppj"));
    QVERIFY(QFile::rename(firstPath, movedPath));
    removeTree(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    const auto loaded = store.load(movedPath);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QVERIFY(loaded.value().issues.isEmpty());
    const QList<Sticker> stickers = loaded.value().document.pages.first().stickers;
    QCOMPARE(stickers.size(), 1);
    QCOMPARE(stickers.first().imagePath, QStringLiteral("builtin:star@1"));
    QVERIFY(!stickerSourcePath(stickers.first()).isEmpty());

    // A portable copy carries no file for a shipped sticker and still opens.
    const QString portablePath = m_dir.filePath(QStringLiteral("alias-portable.ppj"));
    const auto portable = store.save(portablePath, window.document(), SaveMode::Portable);
    if (!portable)
        QFAIL(qPrintable(portable.error().message));
    const QDir bundle(portablePath + QStringLiteral(".files"));
    QVERIFY(!bundle.exists() || bundle.entryList(QDir::Files).isEmpty());
    const auto portableLoaded = store.load(portablePath);
    if (!portableLoaded)
        QFAIL(qPrintable(portableLoaded.error().message));
    QCOMPARE(portableLoaded.value().document.pages.first().stickers.first().imagePath,
             QStringLiteral("builtin:star@1"));
}

void TestPageStickers::builtinArtworkIsVersionPinned()
{
    // The alias carries the artwork version, and only a known version resolves:
    // a build that ships new art must add a new version instead of silently
    // swapping the pixels of an existing project.
    QVERIFY(isBuiltinStickerPath(QStringLiteral("builtin:star@1")));
    QVERIFY(!isBuiltinStickerPath(QStringLiteral(":/stickers/star-v1.png")));
    QCOMPARE(builtinStickerPath(QStringLiteral("star")), QStringLiteral("builtin:star@1"));
    QVERIFY(builtinStickerPath(QString()).isEmpty());

    const QString current = builtinStickerResource(QStringLiteral("builtin:star@1"));
    QVERIFY(!current.isEmpty());
    QVERIFY(QFileInfo::exists(current));
    QVERIFY(builtinStickerResource(QStringLiteral("builtin:star@2")).isEmpty());
    QVERIFY(builtinStickerResource(QStringLiteral("builtin:nope@1")).isEmpty());
    QVERIFY(builtinStickerResource(QStringLiteral("builtin:star")).isEmpty());
    QVERIFY(builtinStickerResource(QString()).isEmpty());

    // The identity recorded at add time is the shipped file's own identity, so
    // a future build can tell that the artwork changed.
    QVERIFY(!fileSha256(current).isEmpty());

    // An unknown alias in a project is a missing picture, never a crash, and
    // never a placeholder in the raster.
    Sticker stale;
    stale.id = QStringLiteral("sticker-stale");
    stale.imagePath = QStringLiteral("builtin:star@99");
    stale.width = 20.0;
    stale.height = 20.0;
    stale.cx = 105.0;
    stale.cy = 148.5;
    const QImage page = render(stickerDocument({stale}));
    QVERIFY(!page.isNull());
    QCOMPARE(placeholderPixels(page), 0);
}

void TestPageStickers::hugeSourceIsRejectedBeforeAdding()
{
    const QString path =
        writeHugePngHeader(m_dir.path(), QStringLiteral("huge.png"), 12000, 12000);
    QVERIFY(!path.isEmpty());
    QImageReader probe(path);
    probe.setDecideFormatFromContent(true);
    QVERIFY(probe.canRead());
    QCOMPARE(probe.size(), QSize(12000, 12000));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, path)));
    QTest::qWait(120);
    QVERIFY(window.document().pages.first().stickers.isEmpty());
    QCOMPARE(window.statusBar()->currentMessage(),
             QStringLiteral("A matrica túl nagy vagy nem olvasható."));
}

void TestPageStickers::paletteStaysAvailableWithSelection()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);

    auto *addButton = window.findChild<QPushButton *>(QStringLiteral("addStickerButton"));
    auto *heart = window.findChild<QToolButton *>(QStringLiteral("stickerSet_heart"));
    QVERIFY(addButton);
    QVERIFY(heart);
    QVERIFY(addButton->isVisible());
    QVERIFY(heart->isVisible());

    // One click on the palette adds a second sticker without a deselect.
    QTest::mouseClick(heart, Qt::LeftButton);
    QTest::qWait(150);
    const Page page = window.document().pages.first();
    QCOMPARE(page.stickers.size(), 2);
    QCOMPARE(page.stickers.at(1).imagePath, QStringLiteral("builtin:heart@1"));
}

void TestPageStickers::emptyHintSkipsAPageWithASticker()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    // A brand-new page has no content, so the hint invites the first photo.
    QVERIFY(!view->emptyHint().isEmpty());

    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);
    // The plate would cover the sticker, so it is gone as soon as there is one.
    QVERIFY(view->emptyHint().isEmpty());

    const int before = window.undoStackForTest()->count();
    QVERIFY(QMetaObject::invokeMethod(&window, "deleteSelectedSticker", Qt::DirectConnection));
    QTest::qWait(150);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);
    QVERIFY(!view->emptyHint().isEmpty());
}

void TestPageStickers::nudgeIsOneUndoPerBurst()
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
    for (int press = 0; press < 3; ++press)
        QTest::keyClick(view, Qt::Key_Right);
    // The canvas follows every key, but nothing is published yet.
    const std::optional<Sticker> live = view->liveSticker(stickerId);
    QVERIFY(live);
    QCOMPARE(live->cx, startX + 3.0);
    QCOMPARE(window.undoStackForTest()->count(), before);

    QTest::qWait(400);
    QCOMPARE(window.document().pages.first().stickers.first().cx, startX + 3.0);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    // Shift is the fine step, and it is a burst of its own.
    QTest::keyClick(view, Qt::Key_Left, Qt::ShiftModifier);
    QTest::qWait(400);
    QCOMPARE(window.document().pages.first().stickers.first().cx, startX + 2.9);
    QCOMPARE(window.undoStackForTest()->count(), before + 2);

    window.undoStackForTest()->undo();
    QTest::qWait(80);
    QCOMPARE(window.document().pages.first().stickers.first().cx, startX + 3.0);
    window.undoStackForTest()->undo();
    QTest::qWait(80);
    QCOMPARE(window.document().pages.first().stickers.first().cx, startX);
}

void TestPageStickers::deleteIsOneUndoStep()
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
    const int before = window.undoStackForTest()->count();

    view->setFocus();
    QTest::keyClick(view, Qt::Key_Delete);
    QTest::qWait(150);
    QVERIFY(window.document().pages.first().stickers.isEmpty());
    QCOMPARE(window.undoStackForTest()->count(), before + 1);
    window.undoStackForTest()->undo();
    QTest::qWait(80);
    QCOMPARE(window.document().pages.first().stickers.size(), 1);
    QCOMPARE(window.document().pages.first().stickers.first().id, stickerId);
}

void TestPageStickers::opacitySliderPreviewsLive()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    auto *slider = window.findChild<QSlider *>(QStringLiteral("stickerOpacitySlider"));
    auto *readout = window.findChild<QLabel *>(QStringLiteral("stickerOpacityValue"));
    QVERIFY(view);
    QVERIFY(slider);
    QVERIFY(readout);
    const QString stickerId = window.document().pages.first().stickers.first().id;
    const int before = window.undoStackForTest()->count();

    // While the slider moves the canvas follows, but the document does not.
    slider->setValue(40);
    const std::optional<Sticker> live = view->liveSticker(stickerId);
    QVERIFY(live);
    QCOMPARE(live->opacity, 40);
    QCOMPARE(window.document().pages.first().stickers.first().opacity, 100);
    QCOMPARE(readout->text(), QStringLiteral("40 %"));
    QCOMPARE(window.undoStackForTest()->count(), before);

    QTest::qWait(400);
    QCOMPARE(window.document().pages.first().stickers.first().opacity, 40);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    // The size field is centimetres, the model is millimetres, and the height
    // follows the stored aspect.
    auto *size = window.findChild<QDoubleSpinBox *>(QStringLiteral("stickerSizeSpin"));
    QVERIFY(size);
    QCOMPARE(size->suffix(), QStringLiteral(" cm"));
    size->setValue(10.0);
    emit size->editingFinished();
    QTest::qWait(150);
    const Sticker resized = window.document().pages.first().stickers.first();
    QVERIFY(std::abs(resized.width - 100.0) < 0.01);
    QVERIFY(std::abs(resized.height - 100.0) < 0.01);
    QCOMPARE(window.undoStackForTest()->count(), before + 2);
}

void TestPageStickers::tallStickerKeepsItsAspect()
{
    // A 1:10 picture at the default width would be 52.5 x 525 mm, taller than
    // the sheet. It is made smaller as a whole, not squashed on one axis.
    const QSizeF size =
        OverlayGeometry::defaultStickerSizeMm(QSizeF(210.0, 297.0), QSize(100, 1000));
    QVERIFY(std::abs(size.width() / size.height() - 0.1) < 0.001);
    QVERIFY(size.height() <= std::hypot(210.0, 297.0) + 0.001);
    QVERIFY(size.width() <= 52.5 + 0.001);

    // The same rule inside the model, for a hand-edited tall box.
    Sticker tall = makeSticker(m_redPath, 105.0, 148.5, 10.0, 400.0);
    const Sticker sane = sanitiseSticker(tall, QSizeF(210.0, 297.0));
    QVERIFY(std::abs(sane.width / sane.height - 0.025) < 0.001);
    QVERIFY(sane.height <= std::hypot(210.0, 297.0) + 0.001);
}

void TestPageStickers::relinkingResetsTheBoxToTheNewAspect()
{
    const QString tallPath = writeImage(m_dir.path(), QStringLiteral("tall.png"),
                                        solidImage(QColor(40, 160, 90), QSize(100, 1000)));
    QVERIFY(!tallPath.isEmpty());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);
    const Sticker square = window.document().pages.first().stickers.first();
    QVERIFY(std::abs(square.width / square.height - 1.0) < 0.01);

    // Relinking to a 1:10 picture resets the box to that aspect instead of
    // stretching the old square.
    QVERIFY(QMetaObject::invokeMethod(&window, "relinkStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, tallPath)));
    QTest::qWait(150);
    const Sticker relinked = window.document().pages.first().stickers.first();
    QVERIFY(std::abs(relinked.width / relinked.height - 0.1) < 0.005);
    QVERIFY(relinked.height <= std::hypot(210.0, 297.0) + 0.001);
    QCOMPARE(relinked.cx, square.cx);
    QCOMPARE(relinked.cy, square.cy);
}

void TestPageStickers::sizeSpinIsBoundedByThePage()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);

    auto *size = window.findChild<QDoubleSpinBox *>(QStringLiteral("stickerSizeSpin"));
    QVERIFY(size);
    // The spin never offers a value the model would clamp away: the page
    // diagonal is the largest box a sticker can have.
    const double diagonalCm = std::hypot(210.0, 297.0) / 10.0;
    QVERIFY(std::abs(size->maximum() - diagonalCm) < 0.01);
    QVERIFY(size->maximum() < 200.0);
    size->setValue(200.0);
    QCOMPARE(size->value(), size->maximum());
}

void TestPageStickers::fitToPaperBringsStickersBack()
{
    // A sticker half off the sheet is part of what "Igazítás a laphoz" tidies,
    // so the overflow notice has to see it and the action has to move it.
    DocumentSnapshot document;
    Page page = a4Page();
    Sticker sticker = makeSticker(m_redPath, 5.0, 280.0, 60.0, 60.0);
    sticker.angleDeg = 25.0;
    page.stickers = {sticker};
    document.pages.append(page);
    document.currentPage = 0;

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, document));
    QTest::qWait(200);
    auto *fit = window.findChild<QPushButton *>(QStringLiteral("fitPageButton"));
    QVERIFY(fit);
    QVERIFY(fit->isVisible());

    CanvasController controller;
    controller.setDocument(document);
    QVERIFY(controller.apply(FitPageEdit{0}).ok());
    const Sticker fitted = controller.document().pages.first().stickers.first();
    const QRectF box = OverlayGeometry::stickerQuadMm(fitted).boundingRect();
    QVERIFY(box.left() >= -0.05);
    QVERIFY(box.top() >= -0.05);
    QVERIFY(box.right() <= 210.05);
    QVERIFY(box.bottom() <= 297.05);
    QVERIFY(fitted.width < sticker.width);
}

void TestPageStickers::stickerAnnouncedToAssistiveTech()
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
    QVERIFY2(view->accessibleDescription().contains(
                 QStringLiteral("A matrica ki van jelölve.")),
             qPrintable(view->accessibleDescription()));
}

void TestPageStickers::evidenceScreenshots()
{
    // A built-in star at 60 % opacity over a photograph: the slice's own
    // promotional page, and the printed parity evidence.
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
    Sticker star = makeSticker(builtinStickerPath(QStringLiteral("star")), 150.0, 105.0,
                               65.0, 65.0);
    star.imageSha256 = fileSha256(stickerSourcePath(star));
    star.opacity = 60;
    star.angleDeg = -12.0;
    page.stickers.append(star);
    document.pages.append(page);
    document.currentPage = 0;

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, document));
    QTest::qWait(250);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-sticker-on-photo.png")));

    // The palette with a sticker selected: it stays visible, so a second layer
    // is one click away.
    const QString stickerId = window.document().pages.first().stickers.first().id;
    QVERIFY(QMetaObject::invokeMethod(&window, "selectSticker", Qt::DirectConnection,
                                      Q_ARG(QString, stickerId)));
    QTest::qWait(150);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-sticker-set.png")));

    // A clipped sticker: half of it off the sheet, cut at the paper edge.
    DocumentSnapshot clipped = document;
    Sticker edge = star;
    edge.cx = 15.0;
    edge.cy = 280.0;
    edge.angleDeg = 20.0;
    clipped.pages.first().stickers = {edge};
    MainWindow clippedWindow;
    clippedWindow.resize(1280, 800);
    clippedWindow.show();
    QVERIFY(loadDocument(clippedWindow, m_dir, clipped));
    QTest::qWait(200);
    QVERIFY(clippedWindow.grab().save(
        evidenceDir() + QStringLiteral("/ui-sticker-clipped.png")));

    // A missing sticker: the canvas placeholder, never in the raster or PDF.
    Sticker lost = makeSticker(m_dir.filePath(QStringLiteral("evidence-gone.png")),
                               105.0, 148.5, 55.0, 55.0);
    lost.imageSha256 = QByteArrayLiteral("0123456789abcdef");
    MainWindow missingWindow;
    missingWindow.resize(1280, 800);
    missingWindow.show();
    QVERIFY(loadDocument(missingWindow, m_dir, stickerDocument({lost})));
    QTest::qWait(200);
    QVERIFY(missingWindow.grab().save(
        evidenceDir() + QStringLiteral("/ui-sticker-placeholder.png")));

    // The same page printed: the star is on paper, with no placeholder red.
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
    const QImage printed(base + QStringLiteral(".png"));
    QVERIFY(!printed.isNull());
    QCOMPARE(placeholderPixels(printed), 0);
}

QTEST_MAIN(TestPageStickers)
#include "tst_page_stickers.moc"
