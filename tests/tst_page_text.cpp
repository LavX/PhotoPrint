#include "CanvasController.h"
#include "ColorPipeline.h"
#include "Document.h"
#include "ImageSource.h"
#include "MainWindow.h"
#include "OverlayGeometry.h"
#include "PageRenderer.h"
#include "ProjectStore.h"
#include "StickerAssets.h"
#include "TextDialog.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFontComboBox>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
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

Page a4Page()
{
    Page page;
    page.id = QStringLiteral("page-text");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    return page;
}

TextItem makeText(const QString &text, double cx, double cy, double fontPt = 12.0,
                  int z = 0)
{
    TextItem item;
    item.id = makeId(QStringLiteral("text"));
    item.text = text;
    item.cx = cx;
    item.cy = cy;
    item.fontPt = fontPt;
    item.z = z;
    return item;
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

DocumentSnapshot textDocument(const QList<TextItem> &texts)
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.texts = texts;
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

/// The darkest pixel of a page raster: a point a glyph is guaranteed to cover.
QPoint inkPoint(const QImage &image)
{
    int best = -1;
    QPoint where;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int luminance = qRed(line[x]) + qGreen(line[x]) + qBlue(line[x]);
            if (best < 0 || luminance < best) {
                best = luminance;
                where = QPoint(x, y);
            }
        }
    }
    return where;
}

/// A pixel deep inside a glyph: dark, and uniform over a 5 x 5 neighbourhood.
/// A sub-pixel shift between two renders cannot change it, which is what makes
/// it a fair print-parity probe.
QPoint solidInkPoint(const QImage &image)
{
    for (int y = 2; y < image.height() - 2; ++y) {
        for (int x = 2; x < image.width() - 2; ++x) {
            const QRgb centre = image.pixel(x, y);
            if (qRed(centre) + qGreen(centre) + qBlue(centre) > 600)
                continue;
            bool uniform = true;
            for (int dy = -2; dy <= 2 && uniform; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    if (channelDistance(image.pixel(x + dx, y + dy), centre) > 2) {
                        uniform = false;
                        break;
                    }
                }
            }
            if (uniform)
                return QPoint(x, y);
        }
    }
    return {};
}

/// The bounding box of every pixel darker than @p threshold on any channel.
QRect inkBounds(const QImage &image, int threshold = 200)
{
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            if (qRed(pixel) >= threshold && qGreen(pixel) >= threshold
                && qBlue(pixel) >= threshold) {
                continue;
            }
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }
    if (right < 0)
        return {};
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

/// The same bounding box, restricted to a horizontal band of rows.
QRect inkBoundsInRows(const QImage &image, int firstRow, int lastRow, int threshold = 200)
{
    firstRow = std::clamp(firstRow, 0, image.height() - 1);
    lastRow = std::clamp(lastRow, firstRow, image.height() - 1);
    int left = image.width();
    int right = -1;
    for (int y = firstRow; y <= lastRow; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            if (qRed(pixel) >= threshold && qGreen(pixel) >= threshold
                && qBlue(pixel) >= threshold) {
                continue;
            }
            left = std::min(left, x);
            right = std::max(right, x);
        }
    }
    if (right < 0)
        return {};
    return QRect(QPoint(left, firstRow), QPoint(right, lastRow));
}

/// The smallest channel distance between any pixel and @p expected. A glyph
/// interior is exactly the picked colour, so a large font gives 0.
int closestDistance(const QImage &image, QRgb expected)
{
    int best = 1000;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            best = std::min(best, channelDistance(line[x], expected));
    }
    return best;
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
    const QString path = dir.filePath(QStringLiteral("text.ppj"));
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

/// True when the whole widget is inside the scroll area's viewport, not merely
/// visible: a control below the fold is visible but unreachable.
bool insideViewport(QScrollArea *scroll, QWidget *widget)
{
    if (!scroll || !widget || !widget->isVisible())
        return false;
    const QRect mapped(widget->mapTo(scroll->viewport(), QPoint(0, 0)), widget->size());
    return scroll->viewport()->rect().contains(mapped);
}

} // namespace

class TestPageText : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void sanitiserNormalisesText();
    void blockGeometryIsMeasuredAtTheReferencePpi();
    void alignmentMovesEachLineInsideTheBlock();
    void rotationQuadSwapsTheBlockExtent();
    void textRendersInThePickedColour();
    void textOpacityZeroFiftyHundred();
    void textRotationTurnsAboutTheCentre();
    void textClipsAtThePageEdge();
    void textOverStickerAndStickerOverTextByZ();
    void sharedZReorderSwapsKinds();
    void equalZPutsStickersBeforeTexts();
    void stickerRaiseCrossesTheText();
    void hitTestingFollowsTheRotatedBlock();
    void addTextPlacesAndSelects();
    void newTextLandsInTheLowerMargin();
    void dragCommitsOneUndoStep();
    void shortCaptionHandlesLeaveTheInteriorDraggable();
    void resizeChangesTheFontSize();
    void rotateHandleTurnsTheText();
    void nudgeIsOneUndoPerBurst();
    void textNudgeThenOpacityKeepsTheMove();
    void opacityThenUndoKeepsTheUndo();
    void typingUpdatesTheCanvasBeforeFocusLeaves();
    void pageSwitchCommitsTheDirtyEditor();
    void deleteIsOneUndoStep();
    void dialogAddsEditsAndRefusesEmpty();
    void persistenceRoundTrips();
    void oldProjectWithoutTextsWritesNoKey();
    void duplicatePageRegeneratesTextIds();
    void orientationChangeTurnsTextsWithThePage();
    void paperChangeKeepsAbsoluteMm();
    void missingFontFallsBack();
    void largeFontStillRenders();
    void elemTabSwitchesBetweenKinds();
    void textPanelControlsWriteOneEdit();
    void previousAndNextCrossBothKinds();
    void printMatchesTheScreenRender();
    void pdfCarriesNoSelectableText();
    void largeFontKeepsElemReachable();
    void evidenceScreenshots();

private:
    QTemporaryDir m_dir;
    QString m_redPath;
    QString m_photoPath;
};

void TestPageText::initTestCase()
{
    QVERIFY(m_dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintTextTest"));
    QSettings().clear();
    qputenv("PHOTOPRINT_NO_RECOVERY", "1");
    qputenv("PHOTOPRINT_NO_SETTINGS", "1");
    qputenv("PHOTOPRINT_NO_CUPS", "1");
    qputenv("XDG_DATA_HOME", m_dir.path().toUtf8());

    m_redPath = writeImage(m_dir.path(), QStringLiteral("red.png"),
                           solidImage(QColor(220, 30, 30)));
    QVERIFY(!m_redPath.isEmpty());
    m_photoPath = writeImage(m_dir.path(), QStringLiteral("photo.png"),
                             solidImage(QColor(40, 90, 160), QSize(400, 300)));
    QVERIFY(!m_photoPath.isEmpty());
}

void TestPageText::sanitiserNormalisesText()
{
    // A project file can carry any line ending; the model has exactly one.
    TextItem windows = makeText(QStringLiteral("a\r\nb\rc"), 105.0, 148.5);
    const TextItem normalised = sanitiseText(windows, QSizeF(210.0, 297.0));
    QCOMPARE(normalised.text, QStringLiteral("a\nb\nc"));

    // Empty and whitespace-only text is not a layer.
    QList<TextItem> many;
    TextItem empty = makeText(QString(), 10.0, 10.0);
    many.append(empty);
    TextItem spaces = makeText(QStringLiteral("   \n\t "), 20.0, 20.0);
    many.append(spaces);
    TextItem real = makeText(QStringLiteral("Nyár"), 30.0, 30.0);
    many.append(real);
    const QList<TextItem> kept = sanitiseTexts(many, QSizeF(210.0, 297.0));
    QCOMPARE(kept.size(), 1);
    QCOMPARE(kept.first().id, real.id);

    // The colour is a normalised QRgb: alpha is forced opaque, because
    // transparency is the item's opacity.
    TextItem coloured = makeText(QStringLiteral("x"), 10.0, 10.0);
    coloured.color = 0x80123456u;
    QCOMPARE(sanitiseText(coloured, QSizeF(210.0, 297.0)).color, 0xff123456u);

    // Every finite field is clamped, a non-finite one becomes the neutral value.
    TextItem broken = makeText(QStringLiteral("x"), 1e9, -1e9);
    broken.fontPt = 1000.0;
    broken.lineSpacing = 9.0;
    broken.opacity = 500;
    broken.z = 999999;
    broken.angleDeg = 720.0;
    broken.align = static_cast<TextAlign>(99);
    const TextItem sane = sanitiseText(broken, QSizeF(210.0, 297.0));
    QCOMPARE(sane.cx, 210.0);
    QCOMPARE(sane.cy, 0.0);
    QCOMPARE(sane.fontPt, kMaxFontPt);
    QCOMPARE(sane.lineSpacing, kMaxLineSpacing);
    QCOMPARE(sane.opacity, 100);
    QCOMPARE(sane.z, kMaxTextZ);
    QCOMPARE(sane.angleDeg, 0.0);
    QCOMPARE(sane.align, TextAlign::Center);

    TextItem nan = makeText(QStringLiteral("x"), std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::quiet_NaN());
    nan.fontPt = std::numeric_limits<double>::quiet_NaN();
    nan.lineSpacing = std::numeric_limits<double>::infinity();
    const TextItem neutral = sanitiseText(nan, QSizeF(210.0, 297.0));
    QCOMPARE(neutral.cx, 105.0);
    QCOMPARE(neutral.cy, 148.5);
    QCOMPARE(neutral.fontPt, 12.0);
    QCOMPARE(neutral.lineSpacing, 1.0);

    // A block smaller than the minimum is raised until it can be clicked again.
    TextItem tiny = makeText(QStringLiteral("."), 105.0, 148.5, kMinFontPt);
    const TextItem raised = sanitiseText(tiny, QSizeF(210.0, 297.0));
    QVERIFY(raised.fontPt > kMinFontPt);
    const QRectF block = OverlayGeometry::textBlockMm(raised);
    QVERIFY(block.width() >= kMinTextBlockMm - 0.001);
    QVERIFY(block.height() >= kMinTextBlockMm - 0.001);
    // Idempotent: a second pass changes nothing.
    QCOMPARE(sanitiseText(raised, QSizeF(210.0, 297.0)).fontPt, raised.fontPt);

    // The list is capped and a repeated id is dropped.
    QList<TextItem> capped;
    for (int i = 0; i < kMaxTextsPerPage + 5; ++i) {
        TextItem item = makeText(QStringLiteral("x"), 10.0, 10.0);
        item.id = QStringLiteral("text-%1").arg(i);
        capped.append(item);
    }
    QCOMPARE(sanitiseTexts(capped, QSizeF(210.0, 297.0)).size(), kMaxTextsPerPage);
    TextItem duplicate = makeText(QStringLiteral("x"), 10.0, 10.0);
    QCOMPARE(sanitiseTexts({duplicate, duplicate}, QSizeF(210.0, 297.0)).size(), 1);

    // An empty id gets a fresh one, as the model rule says: a hand-edited or
    // partially written file must not silently lose a caption.
    TextItem noId = makeText(QStringLiteral("x"), 10.0, 10.0);
    noId.id.clear();
    const QList<TextItem> assigned = sanitiseTexts({noId}, QSizeF(210.0, 297.0));
    QCOMPARE(assigned.size(), 1);
    QVERIFY(!assigned.first().id.isEmpty());
    // A second pass keeps the assigned id, so a repair is idempotent.
    QCOMPARE(sanitiseTexts(assigned, QSizeF(210.0, 297.0)).first().id,
             assigned.first().id);
}

void TestPageText::blockGeometryIsMeasuredAtTheReferencePpi()
{
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0);
    const QRectF block = OverlayGeometry::textBlockMm(item);
    QVERIFY(block.width() > 0.0);
    QVERIFY(block.height() > 0.0);
    // The block is centred on cx/cy, so adding a line grows it symmetrically.
    QVERIFY(std::abs(block.center().x() - item.cx) < 0.001);
    QVERIFY(std::abs(block.center().y() - item.cy) < 0.001);

    TextItem twoLines = item;
    twoLines.text = QStringLiteral("Hello\nHello");
    const QRectF taller = OverlayGeometry::textBlockMm(twoLines);
    QVERIFY(taller.height() > block.height() * 1.5);
    QVERIFY(std::abs(taller.width() - block.width()) < 0.001);

    // Line spacing multiplies the advance, not the glyph height.
    TextItem spaced = twoLines;
    spaced.lineSpacing = 2.0;
    const QRectF wide = OverlayGeometry::textBlockMm(spaced);
    QVERIFY(wide.height() > taller.height() * 1.4);

    // The block is a physical size: the same text rendered at 150 and 300 ppi
    // covers the same millimetres. This is the 1200 ppi reference doing its job.
    const DocumentSnapshot document = textDocument({item});
    const QImage low = render(document, 150.0);
    const QImage high = render(document, 300.0);
    QVERIFY(!low.isNull());
    QVERIFY(!high.isNull());
    const QRect lowInk = inkBounds(low);
    const QRect highInk = inkBounds(high);
    QVERIFY(!lowInk.isEmpty());
    QVERIFY(!highInk.isEmpty());
    const double lowWidthMm = lowInk.width() / (150.0 / 25.4);
    const double highWidthMm = highInk.width() / (300.0 / 25.4);
    QVERIFY2(std::abs(lowWidthMm - highWidthMm) < 0.5,
             qPrintable(QStringLiteral("ink width %1 mm at 150 ppi, %2 mm at 300 ppi")
                            .arg(lowWidthMm)
                            .arg(highWidthMm)));
}

void TestPageText::alignmentMovesEachLineInsideTheBlock()
{
    // A long first line and a short second one: the block width is the long
    // line, so the short line's position inside it is the alignment.
    TextItem item = makeText(QStringLiteral("MMMMMMMM\nM"), 105.0, 148.5, 40.0);
    const QRectF block = OverlayGeometry::textBlockMm(item);
    const double ppi = 150.0;
    const double pxPerMm = ppi / 25.4;
    const int blockTop = qRound((block.top()) * pxPerMm);
    const int blockBottom = qRound((block.bottom()) * pxPerMm);
    const int blockLeft = qRound(block.left() * pxPerMm);
    const int blockRight = qRound(block.right() * pxPerMm);
    // The second line lives in the bottom half of the block.
    const int bandTop = (blockTop + blockBottom) / 2;

    const auto secondLine = [&](TextAlign align) {
        TextItem aligned = item;
        aligned.align = align;
        const QImage page = render(textDocument({aligned}), ppi);
        return inkBoundsInRows(page, bandTop, blockBottom);
    };

    const QRect left = secondLine(TextAlign::Left);
    const QRect centre = secondLine(TextAlign::Center);
    const QRect right = secondLine(TextAlign::Right);
    QVERIFY(!left.isEmpty());
    QVERIFY(!centre.isEmpty());
    QVERIFY(!right.isEmpty());
    // Three different left edges, in the expected order.
    QVERIFY2(left.left() < centre.left() && centre.left() < right.left(),
             qPrintable(QStringLiteral("left edges %1, %2, %3")
                            .arg(left.left())
                            .arg(centre.left())
                            .arg(right.left())));
    // Left starts at the block's left edge, right ends at its right edge. The
    // tolerance covers the glyph's side bearing, which is inside the advance
    // width the block is measured from.
    QVERIFY(std::abs(left.left() - blockLeft) <= 12);
    QVERIFY(std::abs(right.right() - blockRight) <= 12);
    // The block width is the same for all three: alignment never changes it.
    QCOMPARE(OverlayGeometry::textBlockMm(item).width(), block.width());
}

void TestPageText::rotationQuadSwapsTheBlockExtent()
{
    TextItem item = makeText(QStringLiteral("MMMM"), 105.0, 148.5, 40.0);
    const QRectF block = OverlayGeometry::textBlockMm(item);
    const QRectF upright = OverlayGeometry::textQuadMm(item).boundingRect();
    QVERIFY(std::abs(upright.width() - block.width()) < 0.01);
    QVERIFY(std::abs(upright.height() - block.height()) < 0.01);

    item.angleDeg = 90.0;
    const QRectF turned = OverlayGeometry::textQuadMm(item).boundingRect();
    QVERIFY2(std::abs(turned.width() - block.height()) < 0.01,
             qPrintable(QStringLiteral("turned width %1, block height %2")
                            .arg(turned.width())
                            .arg(block.height())));
    QVERIFY2(std::abs(turned.height() - block.width()) < 0.01,
             qPrintable(QStringLiteral("turned height %1, block width %2")
                            .arg(turned.height())
                            .arg(block.width())));
}

void TestPageText::textRendersInThePickedColour()
{
    TextItem item = makeText(QStringLiteral("MMMM"), 105.0, 148.5, 60.0);
    item.color = qRgb(0xcc, 0x00, 0x00);
    const QImage page = render(textDocument({item}));
    QVERIFY(!page.isNull());
    // The glyph interior is exactly the picked sRGB colour: the linear
    // conversion and the sRGB flattening round-trip to within 1/255.
    const int distance = closestDistance(page, qRgb(0xcc, 0x00, 0x00));
    QVERIFY2(distance <= 2,
             qPrintable(QStringLiteral("closest pixel is %1/255 from #cc0000")
                            .arg(distance)));
}

void TestPageText::textOpacityZeroFiftyHundred()
{
    TextItem invisible = makeText(QStringLiteral("MMMM"), 105.0, 148.5, 60.0);
    invisible.opacity = 0;
    const QImage zero = render(textDocument({invisible}));
    QVERIFY(!zero.isNull());
    QVERIFY2(inkBounds(zero).isEmpty(), "opacity 0 left ink on the page");

    TextItem half = makeText(QStringLiteral("MMMM"), 105.0, 148.5, 60.0);
    half.opacity = 50;
    const QImage fifty = render(textDocument({half}));
    QVERIFY(!fifty.isNull());
    const QRgb expected = blendInLinear(qRgb(255, 255, 255), qRgb(0, 0, 0), 0.5);
    const int distance = closestDistance(fifty, expected);
    QVERIFY2(distance <= 3,
             qPrintable(QStringLiteral("opacity 50 is %1/255 from the half blend %2")
                            .arg(distance)
                            .arg(describe(expected))));

    TextItem full = makeText(QStringLiteral("MMMM"), 105.0, 148.5, 60.0);
    const QImage hundred = render(textDocument({full}));
    QVERIFY(!hundred.isNull());
    QVERIFY(closestDistance(hundred, qRgb(0, 0, 0)) <= 2);
}

void TestPageText::textRotationTurnsAboutTheCentre()
{
    TextItem item = makeText(QStringLiteral("MMMM"), 105.0, 148.5, 40.0);
    const QImage upright = render(textDocument({item}));
    QVERIFY(!upright.isNull());
    const QRect uprightInk = inkBounds(upright);
    QVERIFY(!uprightInk.isEmpty());

    item.angleDeg = 90.0;
    const QImage turned = render(textDocument({item}));
    QVERIFY(!turned.isNull());
    const QRect turnedInk = inkBounds(turned);
    QVERIFY(!turnedInk.isEmpty());

    // A quarter turn swaps the ink's width and height, and the centre stays put.
    QVERIFY2(std::abs(turnedInk.width() - uprightInk.height()) <= 4,
             qPrintable(QStringLiteral("turned width %1, upright height %2")
                            .arg(turnedInk.width())
                            .arg(uprightInk.height())));
    QVERIFY2(std::abs(turnedInk.height() - uprightInk.width()) <= 4,
             qPrintable(QStringLiteral("turned height %1, upright width %2")
                            .arg(turnedInk.height())
                            .arg(uprightInk.width())));
    const QPoint uprightCentre = uprightInk.center();
    const QPoint turnedCentre = turnedInk.center();
    QVERIFY(std::abs(uprightCentre.x() - turnedCentre.x()) <= 4);
    QVERIFY(std::abs(uprightCentre.y() - turnedCentre.y()) <= 4);
}

void TestPageText::textClipsAtThePageEdge()
{
    // The centre sits on the left edge, so half the block is off the sheet. The
    // raster is page-sized, so the overflow is cut with no second code path.
    TextItem item = makeText(QStringLiteral("MMMM"), 0.0, 148.5, 60.0);
    const QImage page = render(textDocument({item}));
    QVERIFY(!page.isNull());
    QVERIFY(!inkBounds(page).isEmpty());
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

void TestPageText::textOverStickerAndStickerOverTextByZ()
{
    // A large black caption over a red sticker. The ink point is found on a
    // text-only render, so the probe is guaranteed to sit inside a glyph.
    TextItem text = makeText(QStringLiteral("MMMM"), 105.0, 148.5, 60.0, 1);
    const QImage textOnly = render(textDocument({text}));
    QVERIFY(!textOnly.isNull());
    const QPoint ink = inkPoint(textOnly);
    QVERIFY(!ink.isNull());

    DocumentSnapshot document;
    Page page = a4Page();
    page.stickers.append(makeSticker(m_redPath, 105.0, 148.5, 80.0, 80.0, 0));
    page.texts.append(text);
    document.pages.append(page);
    document.currentPage = 0;

    const QImage textOnTop = render(document);
    QVERIFY(!textOnTop.isNull());
    const QRgb overSticker = textOnTop.pixel(ink);
    QVERIFY2(channelDistance(overSticker, qRgb(0, 0, 0)) <= 3,
             qPrintable(QStringLiteral("text over sticker is %1").arg(describe(overSticker))));

    // Raise the sticker above the text: the same pixel is now the sticker.
    Page raised = page;
    raised.stickers[0].z = 2;
    raised.texts[0].z = 1;
    DocumentSnapshot raisedDocument;
    raisedDocument.pages.append(raised);
    raisedDocument.currentPage = 0;
    const QImage stickerOnTop = render(raisedDocument);
    QVERIFY(!stickerOnTop.isNull());
    const QRgb overText = stickerOnTop.pixel(ink);
    QVERIFY2(channelDistance(overText, qRgb(220, 30, 30)) <= 3,
             qPrintable(QStringLiteral("sticker over text is %1").arg(describe(overText))));
}

void TestPageText::sharedZReorderSwapsKinds()
{
    Page page = a4Page();
    page.stickers.append(makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0, 0));
    page.texts.append(makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1));
    const QString stickerId = page.stickers.first().id;
    const QString textId = page.texts.first().id;

    QList<OverlayGeometry::OverlayRef> order = OverlayGeometry::paintOrder(page);
    QCOMPARE(order.size(), 2);
    QCOMPARE(order.at(0).kind, OverlayGeometry::OverlayKind::Sticker);
    QCOMPARE(order.at(1).kind, OverlayGeometry::OverlayKind::Text);

    // One press raises the sticker above the text: a real step across kinds.
    QVERIFY(OverlayGeometry::reorderOverlay(page, stickerId, 1));
    QCOMPARE(page.stickers.first().z, 1);
    QCOMPARE(page.texts.first().z, 0);
    order = OverlayGeometry::paintOrder(page);
    QCOMPARE(order.at(0).kind, OverlayGeometry::OverlayKind::Text);
    QCOMPARE(order.at(1).kind, OverlayGeometry::OverlayKind::Sticker);

    // Lowering it again restores the previous order.
    QVERIFY(OverlayGeometry::reorderOverlay(page, stickerId, -1));
    QCOMPARE(page.stickers.first().z, 0);
    QCOMPARE(page.texts.first().z, 1);

    // The ends are dead, and an unknown id changes nothing.
    QVERIFY(!OverlayGeometry::reorderOverlay(page, stickerId, -1));
    QVERIFY(!OverlayGeometry::reorderOverlay(page, textId, 1));
    QVERIFY(!OverlayGeometry::reorderOverlay(page, QStringLiteral("nope"), 1));

    // The same step through the controller is one undo.
    CanvasController controller;
    Page withBoth = a4Page();
    withBoth.stickers.append(makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0, 0));
    withBoth.texts.append(makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1));
    const QString controllerStickerId = withBoth.stickers.first().id;
    DocumentSnapshot document;
    document.pages.append(withBoth);
    document.currentPage = 0;
    controller.setDocument(document);
    const int before = controller.undoStack()->count();
    Page edited = controller.document().pages.first();
    QVERIFY(OverlayGeometry::reorderOverlay(edited, controllerStickerId, 1));
    QVERIFY(controller.publishPage(edited, QStringLiteral("Rétegsorrend")).ok());
    QCOMPARE(controller.undoStack()->count(), before + 1);
    QCOMPARE(controller.document().pages.first().stickers.first().z, 1);
    controller.undoStack()->undo();
    QCOMPARE(controller.document().pages.first().stickers.first().z, 0);
}

void TestPageText::equalZPutsStickersBeforeTexts()
{
    // The documented tie break: at an equal z a sticker paints before a text, so
    // the caption is on top. Document order then decides within each kind.
    Page page = a4Page();
    page.stickers.append(makeSticker(m_redPath, 105.0, 148.5, 40.0, 40.0, 5));
    page.texts.append(makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 5));
    const QList<OverlayGeometry::OverlayRef> order = OverlayGeometry::paintOrder(page);
    QCOMPARE(order.size(), 2);
    QCOMPARE(order.at(0).kind, OverlayGeometry::OverlayKind::Sticker);
    QCOMPARE(order.at(1).kind, OverlayGeometry::OverlayKind::Text);

    // Two texts at the same z keep document order.
    Page twoTexts = a4Page();
    TextItem first = makeText(QStringLiteral("first"), 105.0, 100.0, 12.0, 3);
    TextItem second = makeText(QStringLiteral("second"), 105.0, 200.0, 12.0, 3);
    twoTexts.texts = {first, second};
    const QList<OverlayGeometry::OverlayRef> textOrder =
        OverlayGeometry::paintOrder(twoTexts);
    QCOMPARE(textOrder.size(), 2);
    QCOMPARE(twoTexts.texts.at(textOrder.at(0).index).id, first.id);
    QCOMPARE(twoTexts.texts.at(textOrder.at(1).index).id, second.id);
}

void TestPageText::stickerRaiseCrossesTheText()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 2);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    // The text is on top. Select the sticker and raise it: the shared stack
    // swaps them, and the sticker's z is now above the text's.
    const QString stickerId = window.document().pages.first().stickers.first().id;
    QVERIFY(QMetaObject::invokeMethod(&window, "selectSticker", Qt::DirectConnection,
                                      Q_ARG(QString, stickerId)));
    QTest::qWait(100);
    auto *raise = window.findChild<QPushButton *>(QStringLiteral("stickerRaiseButton"));
    QVERIFY(raise);
    QVERIFY(raise->isEnabled());
    const int before = window.undoStackForTest()->count();
    QTest::mouseClick(raise, Qt::LeftButton);
    QTest::qWait(120);

    const Page page = window.document().pages.first();
    QVERIFY(page.stickers.first().z > page.texts.first().z);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    // The text's own raise button puts it back on top.
    QVERIFY(QMetaObject::invokeMethod(&window, "selectText", Qt::DirectConnection,
                                      Q_ARG(QString, item.id)));
    QTest::qWait(100);
    auto *textRaise = window.findChild<QPushButton *>(QStringLiteral("textRaiseButton"));
    QVERIFY(textRaise);
    QVERIFY(textRaise->isEnabled());
    QTest::mouseClick(textRaise, Qt::LeftButton);
    QTest::qWait(120);
    const Page restored = window.document().pages.first();
    QVERIFY(restored.texts.first().z > restored.stickers.first().z);
}

void TestPageText::hitTestingFollowsTheRotatedBlock()
{
    TextItem item = makeText(QStringLiteral("MMMM"), 100.0, 100.0, 40.0);
    const QRectF block = OverlayGeometry::textBlockMm(item);
    QVERIFY(OverlayGeometry::textContains(item, QPointF(100.0, 100.0)));
    QVERIFY(!OverlayGeometry::textContains(item, QPointF(100.0, 100.0 + block.height())));

    // A quarter turn: a point that was outside the upright block is inside the
    // turned one, and the other way round.
    item.angleDeg = 90.0;
    QVERIFY(OverlayGeometry::textContains(item, QPointF(100.0, 100.0 + block.width() / 2.0)));
    QVERIFY(!OverlayGeometry::textContains(item, QPointF(100.0 + block.width() / 2.0, 100.0)));

    // The topmost overlay wins, across both kinds.
    Page page = a4Page();
    page.stickers.append(makeSticker(m_redPath, 100.0, 100.0, 60.0, 60.0, 0));
    TextItem caption = makeText(QStringLiteral("MMMM"), 100.0, 100.0, 40.0, 5);
    page.texts.append(caption);
    const std::optional<OverlayGeometry::OverlayRef> hit =
        OverlayGeometry::overlayAt(page, QPointF(100.0, 100.0));
    QVERIFY(hit);
    QCOMPARE(hit->kind, OverlayGeometry::OverlayKind::Text);
    QCOMPARE(OverlayGeometry::overlayIdAt(page, QPointF(100.0, 100.0)), caption.id);
    QVERIFY(!OverlayGeometry::overlayAt(page, QPointF(10.0, 10.0)));
}

void TestPageText::addTextPlacesAndSelects()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    const int before = window.undoStackForTest()->count();

    TextItem item = makeText(QStringLiteral("Nyár 2026"), 105.0, 148.5, 12.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    const Page page = window.document().pages.first();
    QCOMPARE(page.texts.size(), 1);
    QCOMPARE(page.texts.first().text, QStringLiteral("Nyár 2026"));
    QCOMPARE(page.texts.first().cx, 105.0);
    QCOMPARE(page.texts.first().cy, 148.5);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("inspectorTabs"));
    auto *view = window.findChild<PageView *>();
    QVERIFY(tabs);
    QVERIFY(view);
    QCOMPARE(tabs->currentIndex(), 3);
    QCOMPARE(view->selectedText(), item.id);

    // An empty text is refused: the page never carries an invisible layer.
    TextItem blank = makeText(QStringLiteral("   "), 105.0, 148.5);
    QVERIFY(!window.addTextForTest(blank));
    QCOMPARE(window.document().pages.first().texts.size(), 1);
}

void TestPageText::newTextLandsInTheLowerMargin()
{
    // A page with a photo in the middle: the first caption goes below it, not on
    // top of it, and starts at 14 pt.
    DocumentSnapshot document;
    Page page = a4Page();
    Frame frame;
    frame.id = QStringLiteral("frame-placement");
    frame.sourcePath = m_photoPath;
    frame.sourceSha256 = fileSha256(m_photoPath);
    frame.cx = 105.0;
    frame.cy = 120.0;
    frame.width = 150.0;
    frame.height = 100.0;
    frame.fit = FitMode::Fill;
    page.frames.append(frame);
    document.pages.append(page);
    document.currentPage = 0;

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, document));
    QTest::qWait(150);

    const TextItem item = window.newTextForTest();
    QCOMPARE(item.fontPt, 14.0);
    QCOMPARE(item.cx, 105.0);
    // The photo ends at 170 mm; the caption sits in the margin below it.
    QVERIFY2(item.cy > 170.0,
             qPrintable(QStringLiteral("caption at %1 mm").arg(item.cy)));
    QVERIFY(item.cy < 297.0);

    // A page with no content uses the lower third.
    MainWindow empty;
    empty.resize(1280, 800);
    empty.show();
    QTest::qWait(120);
    const TextItem lower = empty.newTextForTest();
    QCOMPARE(lower.fontPt, 14.0);
    QVERIFY2(lower.cy > 297.0 * 0.6,
             qPrintable(QStringLiteral("caption at %1 mm").arg(lower.cy)));
}

void TestPageText::dragCommitsOneUndoStep()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const TextItem start = window.document().pages.first().texts.first();
    const int before = window.undoStackForTest()->count();

    const QPolygonF quad = view->textQuad(start.id);
    QVERIFY(quad.size() == 4);
    const QPointF from = quad.boundingRect().center();
    const QPointF to = from + QPointF(40.0, 25.0);
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    QTest::mouseMove(view, to.toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    QTest::qWait(150);

    const TextItem moved = window.document().pages.first().texts.first();
    QVERIFY(moved.cx > start.cx);
    QVERIFY(moved.cy > start.cy);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    window.undoStackForTest()->undo();
    QTest::qWait(80);
    const TextItem restored = window.document().pages.first().texts.first();
    QCOMPARE(restored.cx, start.cx);
    QCOMPARE(restored.cy, start.cy);
}

void TestPageText::shortCaptionHandlesLeaveTheInteriorDraggable()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    // A one-word 12 pt caption: the default size, the one the handles used to
    // cover completely.
    TextItem item = makeText(QStringLiteral("Hi"), 105.0, 148.5, 12.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const TextItem start = window.document().pages.first().texts.first();
    const QPolygonF block = view->textQuad(start.id);
    const QPolygonF ring = view->textHandleQuad(start.id);
    QVERIFY(block.size() == 4);
    QVERIFY(ring.size() == 4);

    // The handles sit outside the block: every ring corner is outside the glyph
    // block, so the words are not covered by their own furniture.
    for (int i = 0; i < 4; ++i)
        QVERIFY2(!block.containsPoint(ring.at(i), Qt::OddEvenFill),
                 "a handle corner sits on the glyphs");

    // The interior is always a move, even on a block narrower than the handle
    // radius.
    const QPointF centre = block.boundingRect().center();
    const QPointF to = centre + QPointF(30.0, 20.0);
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    QTest::mouseMove(view, to.toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    QTest::qWait(150);

    const TextItem moved = window.document().pages.first().texts.first();
    QVERIFY2(moved.cx > start.cx, "the interior drag did not move the caption");
    QVERIFY2(moved.cy > start.cy, "the interior drag did not move the caption");
    QCOMPARE(moved.fontPt, start.fontPt);

    // A ring corner still resizes.
    const QPolygonF movedRing = view->textHandleQuad(moved.id);
    const QPointF corner = movedRing.at(2);
    const QPointF outward = corner + (corner - movedRing.boundingRect().center());
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, corner.toPoint());
    QTest::mouseMove(view, outward.toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, outward.toPoint());
    QTest::qWait(150);

    const TextItem resized = window.document().pages.first().texts.first();
    QVERIFY2(resized.fontPt > moved.fontPt, "the ring corner did not resize");
}

void TestPageText::resizeChangesTheFontSize()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const TextItem start = window.document().pages.first().texts.first();
    const int before = window.undoStackForTest()->count();

    // The bottom-right corner of the handle ring, pulled away from the centre.
    const QPolygonF ring = view->textHandleQuad(start.id);
    QVERIFY(ring.size() == 4);
    const QPointF centre = (ring.at(0) + ring.at(2)) / 2.0;
    const QPointF corner = ring.at(2);
    const QPointF outward = corner + (corner - centre);
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, corner.toPoint());
    QTest::mouseMove(view, outward.toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, outward.toPoint());
    QTest::qWait(150);

    const TextItem resized = window.document().pages.first().texts.first();
    QVERIFY2(resized.fontPt > start.fontPt,
             qPrintable(QStringLiteral("font %1 pt did not grow from %2 pt")
                            .arg(resized.fontPt)
                            .arg(start.fontPt)));
    QCOMPARE(resized.cx, start.cx);
    QCOMPARE(resized.cy, start.cy);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);
}

void TestPageText::rotateHandleTurnsTheText()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const TextItem start = window.document().pages.first().texts.first();
    const int before = window.undoStackForTest()->count();

    const QPolygonF ring = view->textHandleQuad(start.id);
    const QPointF topMid = (ring.at(0) + ring.at(1)) / 2.0;
    const QPointF boxCentre = (ring.at(0) + ring.at(2)) / 2.0;
    QPointF direction = topMid - boxCentre;
    const double length = std::hypot(direction.x(), direction.y());
    QVERIFY(length > 0.0);
    direction /= length;
    const QPointF handle = topMid + direction * 30.0;
    // Half a turn around the centre: the handle ends up below the block.
    const QPointF turned = boxCentre - direction * (length + 30.0);
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, handle.toPoint());
    QTest::mouseMove(view, turned.toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, turned.toPoint());
    QTest::qWait(150);

    const TextItem rotated = window.document().pages.first().texts.first();
    QVERIFY(std::abs(rotated.angleDeg - start.angleDeg) > 90.0);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);
}

void TestPageText::nudgeIsOneUndoPerBurst()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(120);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const QString textId = window.document().pages.first().texts.first().id;
    const double startX = window.document().pages.first().texts.first().cx;
    const int before = window.undoStackForTest()->count();

    view->setFocus();
    for (int press = 0; press < 3; ++press)
        QTest::keyClick(view, Qt::Key_Right);
    const std::optional<TextItem> live = view->liveText(textId);
    QVERIFY(live);
    QCOMPARE(live->cx, startX + 3.0);
    QCOMPARE(window.undoStackForTest()->count(), before);

    QTest::qWait(400);
    QCOMPARE(window.document().pages.first().texts.first().cx, startX + 3.0);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);

    // Shift is the fine step, and it is a burst of its own.
    QTest::keyClick(view, Qt::Key_Left, Qt::ShiftModifier);
    QTest::qWait(400);
    QCOMPARE(window.document().pages.first().texts.first().cx, startX + 2.9);
    QCOMPARE(window.undoStackForTest()->count(), before + 2);
}

void TestPageText::textNudgeThenOpacityKeepsTheMove()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(120);

    auto *view = window.findChild<PageView *>();
    auto *slider = window.findChild<QSlider *>(QStringLiteral("textOpacitySlider"));
    QVERIFY(view);
    QVERIFY(slider);
    const double startX = window.document().pages.first().texts.first().cx;
    const int before = window.undoStackForTest()->count();

    view->setFocus();
    QTest::keyClick(view, Qt::Key_Right);
    // Inside the 200 ms burst: the panel action must land the nudge first, not
    // publish a document that still has the old position.
    slider->setValue(40);
    QTest::qWait(450);

    const TextItem text = window.document().pages.first().texts.first();
    QCOMPARE(text.cx, startX + 1.0);
    QCOMPARE(text.opacity, 40);
    QCOMPARE(window.undoStackForTest()->count(), before + 2);

    // The undo pair restores the opacity and then the position.
    window.undoStackForTest()->undo();
    QTest::qWait(80);
    QCOMPARE(window.document().pages.first().texts.first().opacity, 100);
    window.undoStackForTest()->undo();
    QTest::qWait(80);
    QCOMPARE(window.document().pages.first().texts.first().cx, startX);
}

void TestPageText::opacityThenUndoKeepsTheUndo()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(120);

    auto *slider = window.findChild<QSlider *>(QStringLiteral("textOpacitySlider"));
    QVERIFY(slider);

    // Land one opacity step so the undo has something to restore.
    slider->setValue(40);
    QTest::qWait(400);
    QCOMPARE(window.document().pages.first().texts.first().opacity, 40);
    const int before = window.undoStackForTest()->count();

    // Start a second burst and undo inside the debounce window. The pending
    // value must be cancelled, not republished over the undo.
    slider->setValue(60);
    QCOMPARE(window.document().pages.first().texts.first().opacity, 40);
    window.undoStackForTest()->undo();
    QTest::qWait(400);

    QCOMPARE(window.document().pages.first().texts.first().opacity, 100);
    QCOMPARE(window.undoStackForTest()->count(), before);
}

void TestPageText::typingUpdatesTheCanvasBeforeFocusLeaves()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("textEditor"));
    QVERIFY(view);
    QVERIFY(editor);
    const QString textId = window.document().pages.first().texts.first().id;
    const int before = window.undoStackForTest()->count();

    // Type without leaving the field: the canvas follows immediately, and the
    // debounce lands one undo step when the typing stops.
    editor->setFocus();
    editor->setPlainText(QStringLiteral("Nyár"));
    const std::optional<TextItem> live = view->liveText(textId);
    QVERIFY(live);
    QCOMPARE(live->text, QStringLiteral("Nyár"));
    QCOMPARE(window.document().pages.first().texts.first().text, QStringLiteral("Hello"));
    QCOMPARE(window.undoStackForTest()->count(), before);

    QTest::qWait(400);
    QCOMPARE(window.document().pages.first().texts.first().text, QStringLiteral("Nyár"));
    QCOMPARE(window.undoStackForTest()->count(), before + 1);
}

void TestPageText::pageSwitchCommitsTheDirtyEditor()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("textEditor"));
    QVERIFY(editor);
    editor->setPlainText(QStringLiteral("Nyár"));
    // No wait: the debounce has not fired, so the editor is dirty.
    QVERIFY(QMetaObject::invokeMethod(&window, "addPage", Qt::DirectConnection));
    QTest::qWait(150);

    // The paragraph landed on the page it was typed on, not on the new page.
    QCOMPARE(window.document().pages.size(), 2);
    QCOMPARE(window.document().pages.at(0).texts.first().text, QStringLiteral("Nyár"));
    QVERIFY(window.document().pages.at(1).texts.isEmpty());
}

void TestPageText::deleteIsOneUndoStep()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 24.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(120);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const QString textId = window.document().pages.first().texts.first().id;
    const int before = window.undoStackForTest()->count();

    view->setFocus();
    QTest::keyClick(view, Qt::Key_Delete);
    QTest::qWait(150);
    QVERIFY(window.document().pages.first().texts.isEmpty());
    QCOMPARE(window.undoStackForTest()->count(), before + 1);
    window.undoStackForTest()->undo();
    QTest::qWait(80);
    QCOMPARE(window.document().pages.first().texts.size(), 1);
    QCOMPARE(window.document().pages.first().texts.first().id, textId);
}

void TestPageText::dialogAddsEditsAndRefusesEmpty()
{
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 12.0);
    item.bold = true;
    item.align = TextAlign::Left;

    // Add: the dialog returns what the user typed and chose.
    {
        TextDialog dialog(item);
        dialog.show();
        QTest::qWait(50);
        auto *editor = dialog.findChild<QPlainTextEdit *>(QStringLiteral("textDialogEditor"));
        auto *size = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("textDialogSizeSpin"));
        auto *spacing =
            dialog.findChild<QDoubleSpinBox *>(QStringLiteral("textDialogSpacingSpin"));
        QVERIFY(editor);
        QVERIFY(size);
        QVERIFY(spacing);
        editor->setPlainText(QStringLiteral("Nyár\n2026"));
        size->setValue(30.0);
        spacing->setValue(150.0);
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        const TextItem result = dialog.item();
        QCOMPARE(result.text, QStringLiteral("Nyár\n2026"));
        QCOMPARE(result.fontPt, 30.0);
        QCOMPARE(result.lineSpacing, 1.5);
        QVERIFY(result.bold);
        QCOMPARE(result.align, TextAlign::Left);
        // The geometry the dialog was built with is kept: editing never moves
        // the caption.
        QCOMPARE(result.cx, item.cx);
        QCOMPARE(result.cy, item.cy);
        QCOMPARE(result.id, item.id);
    }

    // Empty is refused with the dialog still open.
    {
        TextDialog dialog(item);
        dialog.show();
        QTest::qWait(50);
        auto *editor = dialog.findChild<QPlainTextEdit *>(QStringLiteral("textDialogEditor"));
        auto *hint = dialog.findChild<QLabel *>(QStringLiteral("textDialogEmptyHint"));
        QVERIFY(editor);
        QVERIFY(hint);
        editor->setPlainText(QStringLiteral("   \n\t"));
        dialog.accept();
        QVERIFY(dialog.isVisible());
        QVERIFY(dialog.result() != int(QDialog::Accepted));
        QVERIFY(hint->isVisible());
    }

    // Cancel adds nothing.
    {
        TextDialog dialog(item);
        dialog.show();
        QTest::qWait(50);
        dialog.reject();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }

    // A missing font is reported, not a crash.
    {
        TextItem missing = item;
        missing.fontFamily = QStringLiteral("No Such Font 12345");
        TextDialog dialog(missing);
        dialog.show();
        QTest::qWait(50);
        auto *hint = dialog.findChild<QLabel *>(QStringLiteral("textDialogFontHint"));
        QVERIFY(hint);
        QVERIFY(hint->isVisible());
    }
}

void TestPageText::persistenceRoundTrips()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.background.kind = BackgroundKind::Solid;
    page.background.solid = qRgb(0x33, 0x66, 0xcc);
    page.stickers = {makeSticker(m_redPath, 60.0, 80.0, 30.0, 20.0, 0)};
    TextItem first = makeText(QStringLiteral("Nyár 2026"), 75.0, 120.0, 19.0, 2);
    first.fontFamily = QStringLiteral("DejaVu Sans");
    first.color = qRgb(0xcc, 0x00, 0x00);
    first.bold = true;
    first.italic = true;
    first.underline = true;
    first.align = TextAlign::Right;
    first.lineSpacing = 1.4;
    first.angleDeg = -12.5;
    first.opacity = 65;
    TextItem second = makeText(QStringLiteral("a\nb"), 40.0, 200.0, 12.0, 3);
    page.texts = {first, second};
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
    QCOMPARE(loaded.value().document.pages.first().texts, page.texts);
    QVERIFY(loaded.value().issues.isEmpty());
}

void TestPageText::oldProjectWithoutTextsWritesNoKey()
{
    DocumentSnapshot document;
    document.pages.append(a4Page());
    document.currentPage = 0;

    const QString path = m_dir.filePath(QStringLiteral("legacy.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, document, SaveMode::Normal).ok());
    const QJsonObject root = readJson(path);
    const QJsonObject pageObject = root.value(QStringLiteral("pages")).toArray().at(0).toObject();
    QVERIFY(!pageObject.contains(QStringLiteral("texts")));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QVERIFY(loaded.value().document.pages.first().texts.isEmpty());
}

void TestPageText::duplicatePageRegeneratesTextIds()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.texts = {makeText(QStringLiteral("Hello"), 105.0, 148.5, 12.0, 1)};
    document.pages.append(page);
    document.currentPage = 0;

    CanvasController controller;
    controller.setDocument(document);
    QVERIFY(controller.apply(DuplicatePageEdit{0}).ok());
    QCOMPARE(controller.document().pages.size(), 2);
    const TextItem original = controller.document().pages.at(0).texts.first();
    const TextItem copy = controller.document().pages.at(1).texts.first();
    QCOMPARE(copy.text, original.text);
    QVERIFY(copy.id != original.id);
    QVERIFY(!copy.id.isEmpty());
}

void TestPageText::orientationChangeTurnsTextsWithThePage()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.texts = {makeText(QStringLiteral("Hello"), 60.0, 80.0, 12.0, 1)};
    document.pages.append(page);
    document.currentPage = 0;

    CanvasController controller;
    controller.setDocument(document);
    QVERIFY(controller.apply(SetProjectLandscapeEdit{true}).ok());

    const TextItem turned = controller.document().pages.first().texts.first();
    // Portrait to landscape: (x, y) -> (height - y, x), and the block turns too.
    QCOMPARE(turned.cx, 297.0 - 80.0);
    QCOMPARE(turned.cy, 60.0);
    QCOMPARE(turned.angleDeg, 90.0);

    controller.undoStack()->undo();
    const TextItem restored = controller.document().pages.first().texts.first();
    QCOMPARE(restored.cx, 60.0);
    QCOMPARE(restored.cy, 80.0);
    QCOMPARE(restored.angleDeg, 0.0);
}

void TestPageText::paperChangeKeepsAbsoluteMm()
{
    DocumentSnapshot document;
    Page page = a4Page();
    page.texts = {makeText(QStringLiteral("Hello"), 50.0, 75.0, 12.0, 1)};
    document.pages.append(page);
    document.currentPage = 0;

    CanvasController controller;
    controller.setDocument(document);
    PaperSpec smaller;
    smaller.name = QStringLiteral("10 × 15 cm");
    smaller.physicalWidthMm = 100.0;
    smaller.physicalHeightMm = 150.0;
    QVERIFY(controller.apply(SetProjectPaperEdit{smaller}).ok());

    // Text is placed in absolute millimetres, so a paper change keeps it where
    // it is; only the reachability clamp can move it, and the centre is inside
    // both sheets.
    const TextItem moved = controller.document().pages.first().texts.first();
    QCOMPARE(moved.cx, 50.0);
    QCOMPARE(moved.cy, 75.0);
}

void TestPageText::missingFontFallsBack()
{
    TextItem item = makeText(QStringLiteral("MMMM"), 105.0, 148.5, 40.0);
    item.fontFamily = QStringLiteral("No Such Font 12345");
    const QImage page = render(textDocument({item}));
    QVERIFY(!page.isNull());
    // Qt substitutes a real family, so the caption still has ink.
    QVERIFY(!inkBounds(page).isEmpty());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, textDocument({item})));
    QTest::qWait(150);
    QVERIFY(QMetaObject::invokeMethod(&window, "selectText", Qt::DirectConnection,
                                      Q_ARG(QString, item.id)));
    QTest::qWait(120);
    auto *hint = window.findChild<QLabel *>(QStringLiteral("textFontHint"));
    QVERIFY(hint);
    QVERIFY(hint->isVisible());
}

void TestPageText::largeFontStillRenders()
{
    TextItem item = makeText(QStringLiteral("Nagy"), 105.0, 148.5, kMaxFontPt);
    const QImage page = render(textDocument({item}));
    QVERIFY(!page.isNull());
    QVERIFY(!inkBounds(page).isEmpty());

    // The panel says part of it falls off the page.
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, textDocument({item})));
    QTest::qWait(150);
    QVERIFY(QMetaObject::invokeMethod(&window, "selectText", Qt::DirectConnection,
                                      Q_ARG(QString, item.id)));
    QTest::qWait(120);
    auto *hint = window.findChild<QLabel *>(QStringLiteral("textOverflowHint"));
    QVERIFY(hint);
    QVERIFY(hint->isVisible());
}

void TestPageText::elemTabSwitchesBetweenKinds()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("inspectorTabs"));
    auto *emptyPanel = window.findChild<QWidget *>(QStringLiteral("stickerEmptyPanel"));
    auto *stickerPanel = window.findChild<QWidget *>(QStringLiteral("stickerPanel"));
    auto *textPanel = window.findChild<QWidget *>(QStringLiteral("textPanel"));
    auto *textAddPanel = window.findChild<QWidget *>(QStringLiteral("textAddPanel"));
    auto *addTextButton = window.findChild<QPushButton *>(QStringLiteral("addTextButton"));
    QVERIFY(tabs);
    QVERIFY(emptyPanel);
    QVERIFY(stickerPanel);
    QVERIFY(textPanel);
    QVERIFY(textAddPanel);
    QVERIFY(addTextButton);

    // Nothing selected: the empty state, both add buttons, neither property
    // panel.
    QVERIFY(tabs->isTabEnabled(3));
    tabs->setCurrentIndex(3);
    QTest::qWait(80);
    QVERIFY(emptyPanel->isVisible());
    QVERIFY(!stickerPanel->isVisible());
    QVERIFY(!textPanel->isVisible());
    QVERIFY(textAddPanel->isVisible());
    QVERIFY(addTextButton->isVisible());

    // A text selected: the text properties, not the sticker ones.
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 12.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(120);
    QCOMPARE(tabs->currentIndex(), 3);
    QVERIFY(!emptyPanel->isVisible());
    QVERIFY(!stickerPanel->isVisible());
    QVERIFY(textPanel->isVisible());
    QVERIFY(textAddPanel->isVisible());

    // A sticker selected: the sticker properties, not the text ones.
    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);
    QCOMPARE(tabs->currentIndex(), 3);
    QVERIFY(!emptyPanel->isVisible());
    QVERIFY(stickerPanel->isVisible());
    QVERIFY(!textPanel->isVisible());

    // Escape clears the selection: the empty state is ready again.
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QTest::keyClick(view, Qt::Key_Escape);
    QTest::qWait(120);
    QVERIFY(!emptyPanel->isHidden());
    QVERIFY(stickerPanel->isHidden());
    QVERIFY(textPanel->isHidden());
}

void TestPageText::textPanelControlsWriteOneEdit()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 12.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    auto *right = window.findChild<QToolButton *>(QStringLiteral("textAlignRightButton"));
    auto *left = window.findChild<QToolButton *>(QStringLiteral("textAlignLeftButton"));
    auto *bold = window.findChild<QToolButton *>(QStringLiteral("textBoldButton"));
    auto *size = window.findChild<QDoubleSpinBox *>(QStringLiteral("textSizeSpin"));
    auto *spacing = window.findChild<QDoubleSpinBox *>(QStringLiteral("textSpacingSpin"));
    auto *opacity = window.findChild<QSlider *>(QStringLiteral("textOpacitySlider"));
    QVERIFY(view);
    QVERIFY(right);
    QVERIFY(left);
    QVERIFY(bold);
    QVERIFY(size);
    QVERIFY(spacing);
    QVERIFY(opacity);

    const QString textId = window.document().pages.first().texts.first().id;
    int before = window.undoStackForTest()->count();

    // The three alignment buttons are one choice: clicking right then left must
    // land Right then Left, not read back a stale toggle state.
    QTest::mouseClick(right, Qt::LeftButton);
    QTest::qWait(100);
    QCOMPARE(window.document().pages.first().texts.first().align, TextAlign::Right);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);
    QTest::mouseClick(left, Qt::LeftButton);
    QTest::qWait(100);
    QCOMPARE(window.document().pages.first().texts.first().align, TextAlign::Left);
    QCOMPARE(window.undoStackForTest()->count(), before + 2);

    QTest::mouseClick(bold, Qt::LeftButton);
    QTest::qWait(100);
    QVERIFY(window.document().pages.first().texts.first().bold);
    QCOMPARE(window.undoStackForTest()->count(), before + 3);

    size->setValue(30.0);
    emit size->editingFinished();
    QTest::qWait(100);
    QCOMPARE(window.document().pages.first().texts.first().fontPt, 30.0);
    QCOMPARE(window.undoStackForTest()->count(), before + 4);

    spacing->setValue(150.0);
    emit spacing->editingFinished();
    QTest::qWait(100);
    QCOMPARE(window.document().pages.first().texts.first().lineSpacing, 1.5);
    QCOMPARE(window.undoStackForTest()->count(), before + 5);

    // The opacity slider previews live and lands as one edit.
    before = window.undoStackForTest()->count();
    opacity->setValue(40);
    const std::optional<TextItem> live = view->liveText(textId);
    QVERIFY(live);
    QCOMPARE(live->opacity, 40);
    QCOMPARE(window.document().pages.first().texts.first().opacity, 100);
    QCOMPARE(window.undoStackForTest()->count(), before);
    QTest::qWait(400);
    QCOMPARE(window.document().pages.first().texts.first().opacity, 40);
    QCOMPARE(window.undoStackForTest()->count(), before + 1);
}

void TestPageText::previousAndNextCrossBothKinds()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);

    QVERIFY(QMetaObject::invokeMethod(&window, "addStickerFromPath", Qt::DirectConnection,
                                      Q_ARG(QString, m_redPath)));
    QTest::qWait(150);
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 12.0, 2);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(150);

    auto *view = window.findChild<PageView *>();
    auto *next = window.findChild<QPushButton *>(QStringLiteral("nextStickerButton"));
    auto *previous = window.findChild<QPushButton *>(QStringLiteral("previousStickerButton"));
    QVERIFY(view);
    QVERIFY(next);
    QVERIFY(previous);
    // Always enabled, even with nothing selected.
    QVERIFY(next->isEnabled());
    QVERIFY(previous->isEnabled());

    // The text is selected (it was added last). Next wraps to the sticker at the
    // bottom of the stack, and next again returns to the text.
    QCOMPARE(view->selectedText(), item.id);
    QTest::mouseClick(next, Qt::LeftButton);
    QTest::qWait(80);
    QVERIFY(!view->selectedSticker().isEmpty());
    QVERIFY(view->selectedText().isEmpty());
    QTest::mouseClick(next, Qt::LeftButton);
    QTest::qWait(80);
    QCOMPARE(view->selectedText(), item.id);
    QVERIFY(view->selectedSticker().isEmpty());

    // Previous goes the other way.
    QTest::mouseClick(previous, Qt::LeftButton);
    QTest::qWait(80);
    QVERIFY(!view->selectedSticker().isEmpty());
}

void TestPageText::printMatchesTheScreenRender()
{
    TextItem item = makeText(QStringLiteral("Nyár 2026"), 105.0, 148.5, 40.0);
    item.color = qRgb(0xcc, 0x00, 0x00);
    item.angleDeg = 20.0;
    item.opacity = 80;
    const DocumentSnapshot document = textDocument({item});

    const double ppi = 300.0;
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(document, 0, ppi);
    if (!rendered)
        QFAIL(qPrintable(rendered.error().message));

    const QString pdfPath = evidenceDir() + QStringLiteral("/text-print.pdf");
    RenderOptions options;
    options.printPpi = ppi;
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    QProcess process;
    const QString base = evidenceDir() + QStringLiteral("/text-print");
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, base});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    const QImage pdfPage(base + QStringLiteral(".png"));
    QVERIFY(!pdfPage.isNull());

    // The PDF is the same raster, so the ink and the paper must agree. The probe
    // sits deep inside a glyph, where a sub-pixel shift cannot change the value.
    const QPoint ink = solidInkPoint(rendered.value());
    QVERIFY(!ink.isNull());
    QVERIFY2(channelDistance(pdfPage.pixel(ink), rendered.value().pixel(ink)) <= 2,
             qPrintable(QStringLiteral("PDF ink %1 vs render %2")
                            .arg(describe(pdfPage.pixel(ink)),
                                 describe(rendered.value().pixel(ink)))));
    for (const QPointF &point : {QPointF(5.0, 5.0), QPointF(200.0, 290.0)}) {
        const QRgb fromRender = probe(rendered.value(), point.x(), point.y(), ppi);
        const QRgb fromPdf = probe(pdfPage, point.x(), point.y(), ppi);
        QVERIFY2(channelDistance(fromRender, fromPdf) <= 2,
                 qPrintable(QStringLiteral("PDF vs render at %1,%2 differs: %3 vs %4")
                                .arg(point.x())
                                .arg(point.y())
                                .arg(describe(fromRender), describe(fromPdf))));
    }
}

void TestPageText::pdfCarriesNoSelectableText()
{
    // The text is rasterised into the page image, never embedded as PDF text:
    // the writer streams one image XObject per sheet and has no font resource.
    // pdftotext finding nothing is the proof.
    TextItem item = makeText(QStringLiteral("Nyár 2026"), 105.0, 148.5, 40.0);
    const DocumentSnapshot document = textDocument({item});
    const QString pdfPath = evidenceDir() + QStringLiteral("/text-no-fonts.pdf");
    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    options.printPpi = 300.0;
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));

    QProcess process;
    process.start(QStringLiteral("pdftotext"), {pdfPath, QStringLiteral("-")});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    const QString extracted = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    QVERIFY2(extracted.isEmpty(),
             qPrintable(QStringLiteral("the PDF carries selectable text: %1").arg(extracted)));
}

void TestPageText::largeFontKeepsElemReachable()
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
    TextItem item = makeText(QStringLiteral("Hello"), 105.0, 148.5, 12.0, 1);
    QVERIFY(window.addTextForTest(item));
    QTest::qWait(200);

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("inspectorTabs"));
    auto *panel = window.findChild<QWidget *>(QStringLiteral("textPanel"));
    auto *editor = window.findChild<QPlainTextEdit *>(QStringLiteral("textEditor"));
    auto *editButton = window.findChild<QPushButton *>(QStringLiteral("editTextButton"));
    auto *colour = window.findChild<QPushButton *>(QStringLiteral("textColorButton"));
    auto *spacing = window.findChild<QDoubleSpinBox *>(QStringLiteral("textSpacingSpin"));
    auto *opacity = window.findChild<QSlider *>(QStringLiteral("textOpacitySlider"));
    auto *raise = window.findChild<QPushButton *>(QStringLiteral("textRaiseButton"));
    auto *lower = window.findChild<QPushButton *>(QStringLiteral("textLowerButton"));
    auto *remove = window.findChild<QPushButton *>(QStringLiteral("textDeleteButton"));
    QVERIFY(tabs);
    QVERIFY(panel);
    QVERIFY(editor);
    QVERIFY(editButton);
    QVERIFY(colour);
    QVERIFY(spacing);
    QVERIFY(opacity);
    QVERIFY(raise);
    QVERIFY(lower);
    QVERIFY(remove);
    QCOMPARE(tabs->currentIndex(), 3);
    QVERIFY(panel->isVisible());
    QVERIFY(editor->isVisible());
    QVERIFY(tabs->tabBar()->count() == 4);

    // The whole panel fits the contract viewport: the destructive action and the
    // controls below it are the ones that used to fall off the fold.
    auto *scroll = qobject_cast<QScrollArea *>(tabs->currentWidget());
    QVERIFY(scroll);
    QVERIFY2(scroll->widget()->sizeHint().height() <= scroll->viewport()->height() + 1,
             qPrintable(QStringLiteral("the Elem panel needs %1 px in a %2 px viewport")
                            .arg(scroll->widget()->sizeHint().height())
                            .arg(scroll->viewport()->height())));
    for (QWidget *control : {static_cast<QWidget *>(editButton),
                             static_cast<QWidget *>(colour),
                             static_cast<QWidget *>(spacing),
                             static_cast<QWidget *>(opacity),
                             static_cast<QWidget *>(raise),
                             static_cast<QWidget *>(lower),
                             static_cast<QWidget *>(remove)}) {
        QVERIFY2(insideViewport(scroll, control),
                 qPrintable(QStringLiteral("%1 is below the fold")
                                .arg(control->objectName())));
    }
    QVERIFY(window.grab().save(
        evidenceDir() + QStringLiteral("/ui-text-large-font.png")));
    QApplication::setFont(saved);
}

void TestPageText::evidenceScreenshots()
{
    // The headline case: a real photograph with a caption over it. The sample is
    // the shipped harbour picture, not a solid test block.
    const QString photoPath =
        QStringLiteral(PHOTOPRINT_SOURCE_DIR "/resources/samples/effect-sample.jpg");
    QVERIFY2(QFileInfo::exists(photoPath), qPrintable(photoPath));

    DocumentSnapshot document;
    Page page = a4Page();
    Frame frame;
    frame.id = QStringLiteral("frame-evidence");
    frame.sourcePath = photoPath;
    frame.sourceSha256 = fileSha256(photoPath);
    frame.cx = 105.0;
    frame.cy = 110.0;
    frame.width = 190.0;
    frame.height = 200.0;
    frame.fit = FitMode::Fill;
    page.frames.append(frame);
    TextItem caption = makeText(QStringLiteral("Nyár 2026"), 105.0, 150.0, 36.0, 1);
    caption.color = qRgb(0xff, 0xff, 0xff);
    page.texts.append(caption);
    // The Hungarian double accents the raster path exists to protect.
    TextItem accents = makeText(QStringLiteral("Árvíztűrő tükörfúrógép"), 105.0, 190.0, 18.0, 2);
    accents.color = qRgb(0xff, 0xff, 0xff);
    page.texts.append(accents);
    document.pages.append(page);
    document.currentPage = 0;

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(loadDocument(window, m_dir, document));
    QTest::qWait(250);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-text-on-photo.png")));

    // A real built-in sticker and a caption together: the shared z stack in one
    // picture, with the product's own artwork rather than a red test square.
    DocumentSnapshot stacked = document;
    Sticker star = makeSticker(builtinStickerPath(QStringLiteral("star")), 150.0, 80.0,
                               55.0, 55.0, 3);
    star.imageSha256 = fileSha256(stickerSourcePath(star));
    stacked.pages.first().stickers = {star};
    TextItem over = makeText(QStringLiteral("Csillag"), 150.0, 80.0, 20.0, 4);
    over.color = qRgb(0xff, 0xff, 0xff);
    stacked.pages.first().texts = {over};
    MainWindow stackedWindow;
    stackedWindow.resize(1280, 800);
    stackedWindow.show();
    QVERIFY(loadDocument(stackedWindow, m_dir, stacked));
    QTest::qWait(250);
    QVERIFY(stackedWindow.grab().save(
        evidenceDir() + QStringLiteral("/ui-text-and-sticker.png")));

    // The dialog itself, with a white caption: the checkerboard preview is the
    // only feedback while the user picks a colour, so it has to show white ink.
    {
        TextItem dialogItem = makeText(QStringLiteral("Nyár 2026"), 105.0, 150.0, 36.0);
        dialogItem.color = qRgb(0xff, 0xff, 0xff);
        TextDialog dialog(dialogItem);
        dialog.show();
        QTest::qWait(250);
        QVERIFY(dialog.grab().save(
            evidenceDir() + QStringLiteral("/ui-text-dialog.png")));
        dialog.close();
    }

    // The same page printed at the resolution the app actually uses, not 150 ppi.
    ImageStore store;
    PageRenderer renderer(store);
    const double ppi = 300.0;
    const QString pdfPath = evidenceDir() + QStringLiteral("/text-page.pdf");
    RenderOptions options;
    options.printPpi = ppi;
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));
    QProcess process;
    const QString base = evidenceDir() + QStringLiteral("/text-page");
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, base});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);
    QVERIFY(QFileInfo::exists(base + QStringLiteral(".png")));
    const QImage printed(base + QStringLiteral(".png"));
    QVERIFY(!printed.isNull());
    QVERIFY(!inkBounds(printed).isEmpty());
    // The caption is over the photograph, not on the paper below it: the probe
    // at the caption's centre is not paper.
    const QRgb overPhoto = probe(printed, 105.0, 150.0, ppi);
    QVERIFY2(channelDistance(overPhoto, qRgb(255, 255, 255)) > 20,
             qPrintable(QStringLiteral("the caption centre is paper: %1")
                            .arg(describe(overPhoto))));

    // A one-line manifest, so a reviewer knows what each file proves and at what
    // resolution.
    QFile manifest(evidenceDir() + QStringLiteral("/text-evidence.txt"));
    if (manifest.open(QIODevice::WriteOnly | QIODevice::Text)) {
        manifest.write(
            "ui-text-on-photo.png      real photograph with a white caption over it\n"
            "ui-text-and-sticker.png   built-in star sticker with a caption over it\n"
            "ui-text-dialog.png        the text dialog, white caption on the checkerboard\n"
            "text-page.pdf             300 ppi PDF of the caption over the photograph\n"
            "text-page.png             pdftoppm render of that PDF at 300 ppi\n"
            "text-print.pdf/png        rotated, semi-transparent caption at 300 ppi\n"
            "text-no-fonts.pdf         the same page, no font object in the PDF\n");
        manifest.close();
    }
}

QTEST_MAIN(TestPageText)
#include "tst_page_text.moc"
