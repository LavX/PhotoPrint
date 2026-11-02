#include "CollageTemplates.h"
#include "Document.h"
#include "MainWindow.h"
#include "PaperCatalog.h"
#include "ProjectStore.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"
#include "TemplateBrowserDialog.h"
#include "Translation.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QImageWriter>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace pp;

Q_DECLARE_METATYPE(pp::LoadedProject)

namespace {

/// A frame whose non-geometry fields are all set, so a field the template lost
/// shows up as a failed comparison instead of a value that happens to match.
Frame frameWith(const QString &id, double cx, double cy, double width, double height)
{
    Frame frame;
    frame.id = id;
    frame.sourcePath = QStringLiteral("/photos/%1.jpg").arg(id);
    frame.sourceSha256 = QByteArrayLiteral("sha-") + id.toUtf8();
    frame.cx = cx;
    frame.cy = cy;
    frame.width = width;
    frame.height = height;
    frame.angleDeg = 0.0;
    frame.imageRotationDeg = 90.0;
    frame.zoom = 1.4;
    frame.cropCenter = QPointF(0.3, 0.7);
    frame.fit = FitMode::Full;
    frame.autoPlaced = true;
    frame.autoTone = false;
    frame.tone.exposureEv = 0.1;
    frame.brightness = 0.05;
    frame.paperOverride = true;
    frame.paperEv = 0.12;
    frame.label = QStringLiteral("#1");
    frame.filter = QStringLiteral("mono.classic");
    frame.filterStrength = 55;
    frame.effectVersion = 3;
    frame.upscale = true;
    return frame;
}

/// `count` frames in the page's own order. The geometry does not have to be a
/// real layout: a dialog only reads the count and the order.
QList<Frame> framesFor(int count)
{
    QList<Frame> frames;
    for (int i = 0; i < count; ++i) {
        frames.append(frameWith(QStringLiteral("frame-%1").arg(i + 1), 30.0 + 10.0 * i,
                                40.0 + 10.0 * i, 80.0, 20.0));
    }
    return frames;
}

PaperSpec paperNamed(const QString &name)
{
    for (const PaperSpec &paper : defaultPapers()) {
        if (paper.name == name)
            return paper;
    }
    return {};
}

PaperSpec tinyPaper()
{
    PaperSpec paper;
    paper.name = QStringLiteral("Kicsi");
    paper.physicalWidthMm = 60.0;
    paper.physicalHeightMm = 80.0;
    paper.custom = true;
    paper.type = PaperType::Glossy;
    return paper;
}

TemplateBrowserRequest requestWith(const PaperSpec &paper, const QList<Frame> &frames,
                                   const QList<QImage> &photos = {}, bool landscape = false)
{
    TemplateBrowserRequest request;
    request.paper = paper;
    request.landscape = landscape;
    request.frames = frames;
    request.photos = photos;
    return request;
}

QImage flatImage(const QColor &colour, int side = 90)
{
    QImage image(side, side, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
}

QString makePhoto(const QString &directory, const QString &name, const QColor &colour, QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    const QString path = QDir(directory).filePath(name);
    QImageWriter writer(path, "png");
    writer.write(image);
    return path;
}

QStringList makePhotos(const QString &directory, int count)
{
    const QList<QColor> colours = {QColor(190, 70, 60), QColor(60, 150, 90), QColor(70, 90, 190),
                                   QColor(200, 170, 40), QColor(140, 60, 160)};
    QStringList paths;
    for (int i = 0; i < count; ++i) {
        paths.append(makePhoto(directory, QStringLiteral("photo-%1.png").arg(i + 1),
                               colours.at(i % colours.size()), QSize(600, 400)));
    }
    return paths;
}

bool addPhotos(MainWindow &window, const QStringList &paths)
{
    return QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                     Q_ARG(QStringList, paths));
}

/// The Edit menu action the canvas exposes for the last edit. The undo text is
/// what the person reads, so it is the right thing to assert on.
QAction *undoAction(MainWindow &window)
{
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("Vissza")
            || action->text().startsWith(QStringLiteral("Vissza ")))
            return action;
    }
    return nullptr;
}

QString positionBase()
{
    return QStringLiteral("Az új elrendezés minden fotót a saját helyére tesz.");
}

QString emptySlotsSentence(int count)
{
    // The counted lines are %n messages with numerus forms, not %1 arguments.
    return QCoreApplication::translate("PhotoPrint", "%n hely üresen marad.", nullptr, count);
}

QString freePhotosSentence(int count)
{
    return QCoreApplication::translate("PhotoPrint", "%n fotó szabadon marad.", nullptr, count);
}

const Frame *frameInSlot(const Page &page, const QString &slotId)
{
    return SlotGeometry::frameInSlot(page, slotId);
}

TemplateBrowserDialog *openPicker(MainWindow &window)
{
    auto *button = window.findChild<QPushButton *>(QStringLiteral("templatesButton"));
    if (!button)
        return nullptr;
    button->click();
    return window.findChild<TemplateBrowserDialog *>();
}

} // namespace

class TestTemplateBrowser : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // The picker on its own.
    void chipsCoverEveryCount();
    void everyCountShowsItsLayoutsAtThePageAspect_data();
    void everyCountShowsItsLayoutsAtThePageAspect();
    void opensWithNothingChosen();
    void browseWorksWithZeroOneAndThreePhotos();
    void noCountGateForAnyPhotoCount();
    void elevenFramesApplyAndNameTheFreeOne();
    void appliesToAnEmptyPageWithNoPhotos();
    void currentLayoutIsMarkedAndNamed();
    void staleLayoutIdIsNotClaimed();
    void tinyPaperDisablesWithTheMessage();
    void templateNamesAndAccessibleNames();
    void positionNoticeNamesEveryReset();
    void previewDrawsThePhotosInFillOrder();
    void chosenLayoutCarriesACheck();
    void keyboardMovesBetweenChipsAndTiles();
    void pageChangeWhileOpenFollowsThePage();
    void chosenLayoutThatStopsFittingIsDropped();
    void activationWaitsForTheViewToFinish();
    void nudgingAPhotoDoesNotRebuildThePreviews();

    // The window: applying, undoing, and the guard against a reflow.
    void applyingOneTemplateIsOneUndo();
    void applyingFromThePickerClosesIt();
    void emptyPageAppliesFromThePicker();
    void pickerMarksTheLayoutJustApplied();
    void secondClickReusesTheOpenPicker();
    void selectionSurvivesApply();
    void applyingToASecondPageLeavesTheFirstAlone();
    void reapplyingTheSameLayoutChangesNothing();
    void applyingADifferentLayoutReplacesTheOldOne();
    void addingAPhotoKeepsTemplatedGeometry();
    void openingAndCancellingChangesNothing();
    void languageRebuildClosesThePicker();

private:
    QTemporaryDir m_settingsDir;
};

void TestTemplateBrowser::initTestCase()
{
    // The language menu writes the choice; keep it out of the real settings.
    QVERIFY(m_settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintTest"));
    QSettings().clear();
}

void TestTemplateBrowser::chipsCoverEveryCount()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), {}));
    dialog.show();
    QTest::qWait(20);

    QCOMPARE(dialog.countChipCount(), 9);
    for (int i = 0; i < dialog.countChipCount(); ++i)
        QCOMPARE(dialog.countChipValue(i), i + 2);

    // Browsing is the discovery path for an empty page, so no chip is ever
    // disabled or empty.
    for (int count = 2; count <= 10; ++count) {
        dialog.selectCount(count);
        QCOMPARE(dialog.selectedCount(), count);
        QCOMPARE(dialog.templateTileCount(), 6);
        const QStringList ids = dialog.templateIds();
        QCOMPARE(ids.size(), 6);
        QSet<QString> unique;
        for (const QString &id : ids)
            unique.insert(id);
        QCOMPARE(unique.size(), 6);
        QVERIFY(dialog.countChipEnabled(count));
    }
}

void TestTemplateBrowser::everyCountShowsItsLayoutsAtThePageAspect_data()
{
    // The page aspect is the paper's, not a fixed 10 × 15 cm: one portrait A4,
    // one landscape photo paper whose logical size differs from its physical
    // one, and one small sheet where the layouts are tight.
    QTest::addColumn<QString>("paper");
    QTest::addColumn<bool>("landscape");
    QTest::newRow("A4 portrait") << QStringLiteral("A4") << false;
    QTest::newRow("10 × 15 landscape") << QStringLiteral("10 × 15 cm") << true;
    QTest::newRow("A6 portrait") << QStringLiteral("A6") << false;
    QTest::newRow("13 × 18 landscape") << QStringLiteral("13 × 18 cm") << true;
}

void TestTemplateBrowser::everyCountShowsItsLayoutsAtThePageAspect()
{
    QFETCH(QString, paper);
    QFETCH(bool, landscape);
    const PaperSpec spec = paperNamed(paper);

    TemplateBrowserDialog dialog(requestWith(spec, framesFor(3), {}, landscape));
    dialog.show();
    QTest::qWait(20);

    const double width = landscape ? spec.physicalHeightMm : spec.physicalWidthMm;
    const double height = landscape ? spec.physicalWidthMm : spec.physicalHeightMm;
    const double pageAspect = height / width;
    for (int count = 2; count <= 10; ++count) {
        dialog.selectCount(count);
        QCOMPARE(dialog.templateTileCount(), 6);
        for (const QString &id : dialog.templateIds()) {
            const QImage tile = dialog.templateTileImage(id);
            QVERIFY2(!tile.isNull(), qPrintable(id));
            const double aspect = double(tile.height()) / double(tile.width());
            QVERIFY2(std::abs(aspect - pageAspect) < 0.03,
                     qPrintable(QStringLiteral("%1: aspect %2, page %3")
                                    .arg(id)
                                    .arg(aspect)
                                    .arg(pageAspect)));
        }
    }
}

void TestTemplateBrowser::opensWithNothingChosen()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), framesFor(3)));
    dialog.show();
    QTest::qWait(20);

    // There is no template id in a project, so the picker must not claim the
    // page already uses a layout.
    QCOMPARE(dialog.selectedTemplateId(), QString());
    QVERIFY(!dialog.applyEnabled());
    QVERIFY(dialog.positionNoticeText().isEmpty());
    for (const QString &id : dialog.templateIds()) {
        QVERIFY2(!dialog.templateMarkedChosen(id), qPrintable(id));
        QVERIFY2(dialog.templateAccessibleDescription(id).isEmpty(), qPrintable(id));
    }
    // The chip still follows the page, so the right layouts are one click away,
    // and the focus starts there rather than on a layout nobody chose.
    QCOMPARE(dialog.selectedCount(), 3);
    QVERIFY(dialog.focusWidget());
    QVERIFY2(qobject_cast<QPushButton *>(dialog.focusWidget()) != nullptr,
             "the picker opens with the focus on a layout");

    // Apply stays off until a layout is clicked, and the check follows the click.
    dialog.selectTemplate(dialog.templateIds().at(2));
    QCOMPARE(dialog.selectedTemplateId(), dialog.templateIds().at(2));
    QVERIFY(dialog.applyEnabled());
    QVERIFY(dialog.templateMarkedChosen(dialog.templateIds().at(2)));
    QCOMPARE(dialog.templateAccessibleDescription(dialog.templateIds().at(2)),
             QStringLiteral("Kiválasztott"));

    // A refusal from the window lands inside the open picker.
    dialog.reportApplyFailure(QStringLiteral("próba hiba"));
    QCOMPARE(dialog.applyErrorText(), QStringLiteral("próba hiba"));
}

void TestTemplateBrowser::browseWorksWithZeroOneAndThreePhotos()
{
    const PaperSpec paper = paperNamed(QStringLiteral("A4"));
    for (const int photos : {0, 1, 3}) {
        TemplateBrowserDialog dialog(requestWith(paper, framesFor(photos)));
        dialog.show();
        QTest::qWait(20);
        const QString where = QStringLiteral("%1 photos").arg(photos);

        for (int count = 2; count <= 10; ++count) {
            dialog.selectCount(count);
            QVERIFY2(dialog.templateTileCount() == 6, qPrintable(where));
            // A count is a filter, not a gate: a layout that needs another
            // count is applicable, because the rest of its cells stay empty or
            // the surplus photographs stay free.
            dialog.selectTemplate(dialog.templateIds().first());
            QVERIFY2(dialog.applyEnabled(), qPrintable(where));
            QVERIFY2(!dialog.positionNoticeText().isEmpty(), qPrintable(where));
        }
        // The old mismatch machinery is gone from the surface, not just hidden.
        QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("templateMismatch")));
        QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("templateDirection")));
        QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("templateCapacity")));
        QVERIFY(!dialog.findChild<QPushButton *>(QStringLiteral("templateAddPhotosButton")));
    }
}

void TestTemplateBrowser::noCountGateForAnyPhotoCount()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), framesFor(3)));
    dialog.show();
    QTest::qWait(20);

    // Every count is applicable on the same three-photo page: the count is a
    // filter, and the line says what the layout cannot hold instead of refusing
    // the choice.
    for (int count = 2; count <= 10; ++count) {
        dialog.selectCount(count);
        dialog.selectTemplate(dialog.templateIds().first());
        QVERIFY2(dialog.applyEnabled(), qPrintable(QString::number(count)));
    }

    // Two cells, three photographs: one stays free.
    dialog.selectCount(2);
    dialog.selectTemplate(dialog.templateIds().first());
    QVERIFY2(dialog.positionNoticeText().contains(freePhotosSentence(1)),
             qPrintable(dialog.positionNoticeText()));

    // Five cells, three photographs: two stay empty.
    dialog.selectCount(5);
    dialog.selectTemplate(dialog.templateIds().first());
    QVERIFY2(dialog.positionNoticeText().contains(emptySlotsSentence(2)),
             qPrintable(dialog.positionNoticeText()));
}

void TestTemplateBrowser::elevenFramesApplyAndNameTheFreeOne()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), framesFor(11)));
    dialog.show();
    QTest::qWait(20);
    dialog.selectCount(10);
    QCOMPARE(dialog.templateTileCount(), 6);
    dialog.selectTemplate(dialog.templateIds().first());
    QVERIFY(dialog.applyEnabled());
    QVERIFY2(dialog.positionNoticeText().contains(freePhotosSentence(1)),
             qPrintable(dialog.positionNoticeText()));
    QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("templateCapacity")));
}

void TestTemplateBrowser::appliesToAnEmptyPageWithNoPhotos()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), {}));
    dialog.show();
    QTest::qWait(20);
    dialog.selectCount(4);
    QCOMPARE(dialog.templateTileCount(), 6);
    dialog.selectTemplate(QStringLiteral("n04-grid-2x2"));
    QVERIFY(dialog.applyEnabled());
    QVERIFY2(dialog.positionNoticeText().contains(emptySlotsSentence(4)),
             qPrintable(dialog.positionNoticeText()));

    // The preview paints numbered placeholders, not empty paper: the corner of
    // the first cell carries the tint and the fill-order badge.
    const QImage tile = dialog.templateTileImage(QStringLiteral("n04-grid-2x2"));
    QVERIFY(!tile.isNull());
    bool placeholder = false;
    for (int y = int(tile.height() * 0.12); y < int(tile.height() * 0.22) && !placeholder; ++y) {
        for (int x = int(tile.width() * 0.12); x < int(tile.width() * 0.22); ++x) {
            if (qRed(tile.pixel(x, y)) < 250)
                placeholder = true;
        }
    }
    QVERIFY2(placeholder, "the empty cells are not drawn as placeholders");
}

void TestTemplateBrowser::currentLayoutIsMarkedAndNamed()
{
    const PaperSpec paper = paperNamed(QStringLiteral("A4"));
    const QString id = QStringLiteral("n04-grid-2x2");
    const CollageTemplate *layout = CollageTemplates::instance().find(id);
    QVERIFY(layout);

    // A real page that already uses the layout, through the engine.
    Page page;
    page.paper = paper;
    page.frames = framesFor(3);
    const auto laidOut = SlotLayoutEngine::applyTemplate(page, *layout);
    QVERIFY(laidOut.ok());

    TemplateBrowserRequest request = requestWith(paper, laidOut.value().frames);
    request.slotList = laidOut.value().slotList;
    request.slotStyle = laidOut.value().slotStyle;
    request.layoutId = laidOut.value().layoutId;

    TemplateBrowserDialog dialog(request);
    dialog.show();
    QTest::qWait(20);

    // The chip follows the layout's count, not the three photographs.
    QCOMPARE(dialog.selectedCount(), 4);
    QCOMPARE(dialog.selectedTemplateId(), id);
    QVERIFY(dialog.templateMarkedChosen(id));
    QVERIFY(dialog.templateIsCurrent(id));
    QVERIFY2(dialog.currentLayoutText().contains(layout->nameEn),
             qPrintable(dialog.currentLayoutText()));
    QVERIFY(dialog.applyEnabled());
    QCOMPARE(dialog.positionNoticeText(),
             QStringLiteral("Ez az elrendezés már a lapon van."));

    // The current layout is painted with its own mark: the same tile without a
    // current layout differs in the bottom-left corner, where the ring sits.
    TemplateBrowserDialog plain(requestWith(paper, laidOut.value().frames));
    plain.show();
    QTest::qWait(20);
    plain.selectCount(4);
    plain.selectTemplate(id);
    const QImage markedCurrent = dialog.templateTileImage(id);
    const QImage plainTile = plain.templateTileImage(id);
    QVERIFY(!markedCurrent.isNull());
    QVERIFY(markedCurrent.size() == plainTile.size());
    QCOMPARE(markedCurrent.pixel(2, 2), plainTile.pixel(2, 2));
    QVERIFY(markedCurrent.pixel(9, markedCurrent.height() - 12)
            != plainTile.pixel(9, plainTile.height() - 12));
}

void TestTemplateBrowser::staleLayoutIdIsNotClaimed()
{
    const PaperSpec paper = paperNamed(QStringLiteral("A4"));
    const QString id = QStringLiteral("n04-grid-2x2");
    const CollageTemplate *layout = CollageTemplates::instance().find(id);
    QVERIFY(layout);

    Page page;
    page.paper = paper;
    page.frames = framesFor(2);
    const auto laidOut = SlotLayoutEngine::applyTemplate(page, *layout);
    QVERIFY(laidOut.ok());

    // Adding a cell makes the page a hand-made layout even though the project
    // still carries the old label; the picker must not claim the catalogue entry.
    const auto enlarged = SlotLayoutEngine::addSlot(laidOut.value(),
                                                    laidOut.value().slotList.at(0).id);
    QVERIFY(enlarged.ok());
    QCOMPARE(enlarged.value().slotList.size(), 5);

    TemplateBrowserRequest request = requestWith(paper, enlarged.value().frames);
    request.slotList = enlarged.value().slotList;
    request.slotStyle = enlarged.value().slotStyle;
    request.layoutId = id;
    TemplateBrowserDialog dialog(request);
    dialog.show();
    QTest::qWait(20);

    QVERIFY(dialog.currentLayoutId().isEmpty());
    QVERIFY(!dialog.templateIsCurrent(id));
    QVERIFY(dialog.currentLayoutText().isEmpty());
    // With no honest current layout the chip follows the photographs again.
    QCOMPARE(dialog.selectedCount(), 2);

    // Restoring the matching cells brings the label and the mark back.
    dialog.setPage(paper, false, laidOut.value().frames, {}, laidOut.value().slotList,
                   laidOut.value().slotStyle, id);
    QVERIFY(dialog.waitForPendingSync());
    QCOMPARE(dialog.currentLayoutId(), id);
    QVERIFY(dialog.templateIsCurrent(id));
    QVERIFY2(dialog.currentLayoutText().contains(layout->nameEn),
             qPrintable(dialog.currentLayoutText()));
}

void TestTemplateBrowser::tinyPaperDisablesWithTheMessage()
{
    const PaperSpec paper = tinyPaper();

    // On 60 × 80 mm two photos still map; the ten-photo layouts cannot.
    TemplateBrowserDialog twoOnTiny(requestWith(paper, framesFor(2)));
    twoOnTiny.show();
    QTest::qWait(20);
    twoOnTiny.selectCount(2);
    QCOMPARE(twoOnTiny.templateTileCount(), 6);
    for (const QString &id : twoOnTiny.templateIds())
        QVERIFY2(twoOnTiny.templateMapped(id), qPrintable(id));
    QVERIFY(twoOnTiny.paperMessageText().isEmpty());
    twoOnTiny.selectTemplate(twoOnTiny.templateIds().first());
    QVERIFY(twoOnTiny.applyEnabled());

    TemplateBrowserDialog tenOnTiny(requestWith(paper, framesFor(10)));
    tenOnTiny.show();
    QTest::qWait(20);
    tenOnTiny.selectCount(10);
    QCOMPARE(tenOnTiny.templateTileCount(), 6);
    for (const QString &id : tenOnTiny.templateIds())
        QVERIFY2(!tenOnTiny.templateMapped(id), qPrintable(id));
    // Nothing here fits, so the page says why and Apply stays off.
    QVERIFY(!tenOnTiny.applyEnabled());
    QVERIFY2(tenOnTiny.paperMessageText().contains(QStringLiteral("túl kicsi")),
             qPrintable(tenOnTiny.paperMessageText()));
    QVERIFY2(tenOnTiny.paperMessageText().contains(QStringLiteral("Kicsi")),
             qPrintable(tenOnTiny.paperMessageText()));

    // A chosen layout that does not fit keeps its message and never enables
    // Apply next to it.
    tenOnTiny.selectTemplate(tenOnTiny.templateIds().at(3));
    QVERIFY(!tenOnTiny.applyEnabled());
    QVERIFY(!tenOnTiny.paperMessageText().isEmpty());

    // Every count on a paper that cannot hold any layout must browse without
    // crashing, and must always leave Apply off.
    for (int count = 2; count <= 10; ++count) {
        tenOnTiny.selectCount(count);
        QCOMPARE(tenOnTiny.templateTileCount(), 6);
        QVERIFY(!tenOnTiny.applyEnabled());
    }
}

void TestTemplateBrowser::templateNamesAndAccessibleNames()
{
    const auto &catalogue = CollageTemplates::instance();
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), framesFor(6)));
    dialog.show();
    QTest::qWait(20);

    // The grid is named after the page it describes, not after the catalogue.
    QVERIFY2(dialog.gridAccessibleName().contains(QStringLiteral("A4")),
             qPrintable(dialog.gridAccessibleName()));
    QVERIFY2(dialog.gridAccessibleName().contains(QStringLiteral("Sablonok ehhez a laphoz")),
             qPrintable(dialog.gridAccessibleName()));

    for (int count = 2; count <= 10; ++count) {
        dialog.selectCount(count);
        for (const QString &id : dialog.templateIds()) {
            const CollageTemplate *t = catalogue.find(id);
            QVERIFY(t);
            // The label is the single English product name, never a translated
            // sentence.
            QCOMPARE(dialog.templateLabel(id), t->nameEn);
            QVERIFY(!t->nameEn.isEmpty());
            const QString accessible = dialog.templateAccessibleName(id);
            QVERIFY2(accessible.contains(QString::number(count)), qPrintable(accessible));
            QVERIFY2(accessible.contains(t->nameEn), qPrintable(accessible));
        }
    }
}

void TestTemplateBrowser::positionNoticeNamesEveryReset()
{
    const PaperSpec paper = paperNamed(QStringLiteral("A4"));
    const auto &catalogue = CollageTemplates::instance();
    const CollageTemplate *t = catalogue.find(QStringLiteral("n03-equal-cols"));
    QVERIFY(t);
    const auto mapped = catalogue.slotsMm(*t, paper, false);
    QVERIFY(mapped.ok());

    // Frames much wider than any slot: the crop centre and zoom cannot survive.
    TemplateBrowserDialog reset(requestWith(paper, framesFor(3)));
    reset.show();
    QTest::qWait(20);
    reset.selectCount(3);
    reset.selectTemplate(QStringLiteral("n03-equal-cols"));
    QCOMPARE(reset.positionNoticeText(),
             QStringLiteral("Az új elrendezés minden fotót a saját helyére tesz, a "
                            "kivágásokat ehhez igazítjuk."));
    QVERIFY(reset.applyEnabled());

    // Frames already at the mapped slot geometry keep their crop and their
    // place, so the notice only says the positions are taken from the layout.
    QList<Frame> matching;
    int index = 0;
    for (const QRectF &slot : mapped.value()) {
        matching.append(frameWith(QStringLiteral("exact-%1").arg(++index), slot.center().x(),
                                  slot.center().y(), slot.width(), slot.height()));
    }
    TemplateBrowserDialog exact(requestWith(paper, matching));
    exact.show();
    QTest::qWait(20);
    exact.selectCount(3);
    exact.selectTemplate(QStringLiteral("n03-equal-cols"));
    QCOMPARE(exact.positionNoticeText(), positionBase());

    // A rotated frame at the right aspect only loses its rotation.
    QList<Frame> rotated = matching;
    for (Frame &frame : rotated)
        frame.angleDeg = 12.0;
    TemplateBrowserDialog rotation(requestWith(paper, rotated));
    rotation.show();
    QTest::qWait(20);
    rotation.selectCount(3);
    rotation.selectTemplate(QStringLiteral("n03-equal-cols"));
    QCOMPARE(rotation.positionNoticeText(),
             QStringLiteral("Az új elrendezés minden fotót a saját helyére tesz, az "
                            "elforgatást nullára állítjuk."));

    // Both resets at once in one sentence, never two lines.
    QList<Frame> both = framesFor(3);
    for (Frame &frame : both)
        frame.angleDeg = 12.0;
    TemplateBrowserDialog everything(requestWith(paper, both));
    everything.show();
    QTest::qWait(20);
    everything.selectCount(3);
    everything.selectTemplate(QStringLiteral("n03-equal-cols"));
    QCOMPARE(everything.positionNoticeText(),
             QStringLiteral("Az új elrendezés minden fotót a saját helyére tesz, a kivágásokat "
                            "és az elforgatást is ehhez igazítjuk."));
}

void TestTemplateBrowser::previewDrawsThePhotosInFillOrder()
{
    // The stacked pair puts photo 1 on top and photo 2 below; the preview must
    // show the page's own order, not a sorted one.
    QList<QImage> photos = {flatImage(QColor(200, 40, 40)), flatImage(QColor(40, 60, 200))};
    TemplateBrowserDialog dialog(
        requestWith(paperNamed(QStringLiteral("10 × 15 cm")), framesFor(2), photos));
    dialog.show();
    QTest::qWait(20);
    dialog.selectCount(2);
    dialog.selectTemplate(QStringLiteral("n02-equal-stack"));

    const QImage tile = dialog.templateTileImage(QStringLiteral("n02-equal-stack"));
    QVERIFY(!tile.isNull());
    const QRgb top = tile.pixel(tile.width() / 2, int(tile.height() * 0.25));
    const QRgb bottom = tile.pixel(tile.width() / 2, int(tile.height() * 0.75));
    QVERIFY2(qRed(top) > qBlue(top), "the first photo is not in the first slot");
    QVERIFY2(qBlue(bottom) > qRed(bottom), "the second photo is not in the second slot");
}

void TestTemplateBrowser::chosenLayoutCarriesACheck()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), framesFor(3)));
    dialog.show();
    QTest::qWait(20);
    dialog.selectCount(3);

    const QStringList ids = dialog.templateIds();
    QVERIFY(ids.size() >= 2);
    dialog.selectTemplate(ids.at(1));
    QCOMPARE(dialog.selectedTemplateId(), ids.at(1));
    QVERIFY(dialog.templateMarkedChosen(ids.at(1)));
    QVERIFY(!dialog.templateMarkedChosen(ids.at(0)));

    // A mark is not only a colour: the chosen preview differs from its own
    // unmarked rendering, which is what the badge adds.
    dialog.selectTemplate(ids.at(0));
    const QImage first = dialog.templateTileImage(ids.at(0));
    dialog.selectTemplate(ids.at(1));
    const QImage second = dialog.templateTileImage(ids.at(0));
    QVERIFY(first != second);

    // The badge sits in the bottom-right corner of the page, away from the slot
    // numbers, which are drawn in the top-left corner of their slot.
    dialog.selectTemplate(ids.at(1));
    const QImage markedImage = dialog.templateTileImage(ids.at(1));
    dialog.selectTemplate(ids.at(0));
    const QImage plainImage = dialog.templateTileImage(ids.at(1));
    QCOMPARE(markedImage.size(), plainImage.size());
    QCOMPARE(markedImage.pixel(2, 2), plainImage.pixel(2, 2));
    QVERIFY(markedImage.pixel(markedImage.width() - 12, markedImage.height() - 12)
            != plainImage.pixel(plainImage.width() - 12, plainImage.height() - 12));
}

void TestTemplateBrowser::keyboardMovesBetweenChipsAndTiles()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), framesFor(3)));
    dialog.show();
    QTest::qWait(20);
    dialog.selectCount(3);

    // The arrow keys walk the chips and take the selection with them, without
    // leaving the row.
    auto chipFor = [&dialog](int count) -> QPushButton * {
        for (QPushButton *chip : dialog.findChildren<QPushButton *>(
                 QStringLiteral("templateCountChip"))) {
            if (chip->text() == QString::number(count))
                return chip;
        }
        return nullptr;
    };
    QPushButton *three = chipFor(3);
    QVERIFY(three);
    three->setFocus();
    QTest::keyClick(three, Qt::Key_Right);
    QCOMPARE(dialog.selectedCount(), 4);
    QTest::keyClick(chipFor(4), Qt::Key_Left);
    QCOMPARE(dialog.selectedCount(), 3);
    // The ends hold: ten has no right neighbour and two no left one.
    dialog.selectCount(10);
    QTest::keyClick(chipFor(10), Qt::Key_Right);
    QCOMPARE(dialog.selectedCount(), 10);
    dialog.selectCount(2);
    QTest::keyClick(chipFor(2), Qt::Key_Left);
    QCOMPARE(dialog.selectedCount(), 2);

    // The tiles are reachable, change with the arrow keys and the choice follows.
    auto *grid = dialog.findChild<QListWidget *>(QStringLiteral("templateGrid"));
    QVERIFY(grid);
    QCOMPARE(grid->count(), 6);
    grid->setFocus();
    QTest::qWait(20);
    // Focus may hand the view its first current item; the arrow key always
    // moves on from there and the choice follows.
    const int before = grid->currentRow();
    QTest::keyClick(grid, Qt::Key_Right);
    const int expected = before < 0 ? 0 : before + 1;
    QCOMPARE(grid->currentRow(), expected);
    QCOMPARE(dialog.selectedTemplateId(), dialog.templateIds().at(expected));
}

void TestTemplateBrowser::pageChangeWhileOpenFollowsThePage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 4);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos.mid(0, 3)));
    QVERIFY(window.waitForPreview(15000));

    TemplateBrowserDialog *picker = openPicker(window);
    QVERIFY(picker);
    QCOMPARE(picker->selectedCount(), 3);
    QVERIFY(picker->selectedTemplateId().isEmpty());

    // Fourth photo: the picker follows to the count the page now has.
    QVERIFY(addPhotos(window, QStringList{photos.at(3)}));
    QVERIFY(picker->waitForPendingSync());
    QCOMPARE(picker->selectedCount(), 4);
    QCOMPARE(picker->templateTileCount(), 6);

    // Fifth photo: the chip follows again, and a smaller layout still applies,
    // naming the two photographs it cannot hold.
    QVERIFY(addPhotos(window, QStringList{photos.at(0)}));
    QVERIFY(picker->waitForPendingSync());
    QCOMPARE(picker->selectedCount(), 5);
    picker->selectCount(3);
    picker->selectTemplate(picker->templateIds().first());
    QVERIFY(picker->applyEnabled());
    QVERIFY2(picker->positionNoticeText().contains(freePhotosSentence(2)),
             qPrintable(picker->positionNoticeText()));
    picker->reject();
}

void TestTemplateBrowser::chosenLayoutThatStopsFittingIsDropped()
{
    const PaperSpec a4 = paperNamed(QStringLiteral("A4"));
    const PaperSpec tiny = tinyPaper();

    TemplateBrowserDialog dialog(requestWith(a4, framesFor(3)));
    dialog.show();
    QTest::qWait(20);
    dialog.selectCount(3);
    dialog.selectTemplate(QStringLiteral("n03-equal-cols"));
    QVERIFY(dialog.applyEnabled());

    // The paper shrinks under the same count: the chosen layout no longer maps,
    // so it is dropped instead of a second one being picked for the person.
    dialog.setPage(tiny, false, framesFor(3));
    QVERIFY(dialog.waitForPendingSync());
    QVERIFY(!dialog.templateMapped(QStringLiteral("n03-equal-cols")));
    QCOMPARE(dialog.selectedTemplateId(), QString());
    QVERIFY(!dialog.applyEnabled());
    QVERIFY2(dialog.paperMessageText().contains(QStringLiteral("túl kicsi")),
             qPrintable(dialog.paperMessageText()));

    // A layout that survives the same change keeps its choice and its Apply.
    dialog.setPage(a4, false, framesFor(3));
    QVERIFY(dialog.waitForPendingSync());
    dialog.selectTemplate(QStringLiteral("n03-equal-rows"));
    QVERIFY(dialog.applyEnabled());
    dialog.setPage(tiny, false, framesFor(3));
    QVERIFY(dialog.waitForPendingSync());
    QVERIFY(dialog.templateMapped(QStringLiteral("n03-equal-rows")));
    QCOMPARE(dialog.selectedTemplateId(), QStringLiteral("n03-equal-rows"));
    QVERIFY(dialog.applyEnabled());
}

void TestTemplateBrowser::activationWaitsForTheViewToFinish()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), framesFor(3)));
    dialog.show();
    QTest::qWait(20);
    dialog.selectCount(3);

    auto *grid = dialog.findChild<QListWidget *>(QStringLiteral("templateGrid"));
    QVERIFY(grid);
    grid->setCurrentRow(0);
    QVERIFY(dialog.applyEnabled());

    QSignalSpy chosen(&dialog, &TemplateBrowserDialog::templateChosen);
    QTest::keyClick(grid, Qt::Key_Return);
    // The choice is deferred: activating the item must not rebuild the grid
    // from inside the view's own emission.
    QCOMPARE(chosen.count(), 0);
    QTRY_COMPARE(chosen.count(), 1);
    QCOMPARE(chosen.first().first().toString(), dialog.templateIds().at(0));
    QCOMPARE(grid->count(), 6);
}

void TestTemplateBrowser::nudgingAPhotoDoesNotRebuildThePreviews()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));

    TemplateBrowserDialog *picker = openPicker(window);
    QVERIFY(picker);
    const int rebuilds = picker->tileRebuildCount();
    const QString tileId = picker->templateIds().first();
    picker->selectTemplate(tileId);

    // A nudge is a document change, but it changes no thumbnail.
    const QString frameId = window.document().pages.first().frames.first().id;
    QVERIFY(QMetaObject::invokeMethod(&window, "selectFrame", Qt::DirectConnection,
                                      Q_ARG(QString, frameId), Q_ARG(bool, true)));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QTest::keyClick(view, Qt::Key_Right);
    QVERIFY(picker->waitForPendingSync());
    QCOMPARE(picker->tileRebuildCount(), rebuilds);
    QCOMPARE(picker->selectedTemplateId(), tileId);

    // A real change still rebuilds exactly once for the burst.
    QVERIFY(addPhotos(window, QStringList{photos.at(0)}));
    QVERIFY(picker->waitForPendingSync());
    QCOMPARE(picker->tileRebuildCount(), rebuilds + 1);
    picker->reject();
}

void TestTemplateBrowser::applyingOneTemplateIsOneUndo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));

    // Reload the page through the project path, so the window starts clean and
    // the dirty marker can only come from the template.
    DocumentSnapshot saved = window.document();
    QCOMPARE(saved.pages.size(), 1);
    const QString path = dir.filePath(QStringLiteral("three.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, saved, SaveMode::Normal).ok());
    const auto loaded = store.load(path);
    QVERIFY(loaded);
    qRegisterMetaType<pp::LoadedProject>("pp::LoadedProject");
    QVERIFY(QMetaObject::invokeMethod(&window, "applyLoadedProject", Qt::DirectConnection,
                                      Q_ARG(pp::LoadedProject, loaded.value()),
                                      Q_ARG(bool, false)));
    QVERIFY(window.waitForPreview(15000));
    QVERIFY(!window.windowTitle().endsWith(QStringLiteral("*")));

    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    QCOMPARE(page.frames.size(), 3);
    QVERIFY(!page.hasLayout());

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const quint64 compositions = view->compositionCount();

    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied),
                                      Q_ARG(QString, QStringLiteral("n03-equal-cols"))));
    QVERIFY(applied);

    const Page after = window.document().pages.first();
    // The page gains a real layout: three cells, the label, and the same three
    // photographs assigned to them in display order. Identity and the whole
    // image state survive; the slot owns the geometry.
    QCOMPARE(after.layoutId, QStringLiteral("n03-equal-cols"));
    QCOMPARE(after.slotList.size(), 3);
    QCOMPARE(after.frames.size(), 3);
    QVERIFY(SlotGeometry::layoutConsistent(after));
    for (int i = 0; i < after.frames.size(); ++i) {
        const Frame &frame = after.frames.at(i);
        const Frame &original = page.frames.at(i);
        QCOMPARE(frame.id, original.id);
        QCOMPARE(frame.sourcePath, original.sourcePath);
        QCOMPARE(frame.sourceSha256, original.sourceSha256);
        QCOMPARE(frame.imageRotationDeg, original.imageRotationDeg);
        QCOMPARE(frame.label, original.label);
        QCOMPARE(frame.filter, original.filter);
        QCOMPARE(frame.effectVersion, original.effectVersion);
        QCOMPARE(frame.upscale, original.upscale);
        QCOMPARE(frame.fit, FitMode::Fill);
        QCOMPARE(frame.angleDeg, 0.0);
        QVERIFY(!frame.autoPlaced);
        QCOMPARE(frame.slotId, after.slotList.at(i).id);
        const QRectF photo = SlotGeometry::slotCellMm(after, after.slotList.at(i));
        QVERIFY(std::abs(frame.cx - photo.center().x()) < 0.01);
        QVERIFY(std::abs(frame.cy - photo.center().y()) < 0.01);
        QVERIFY(std::abs(frame.width - photo.width()) < 0.01);
        QVERIFY(std::abs(frame.height - photo.height()) < 0.01);
    }

    // The edit is marked in the title and named after the action.
    QVERIFY(window.windowTitle().endsWith(QStringLiteral("*")));
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY2(undo->text().contains(QStringLiteral("Sablon alkalmazása")),
             qPrintable(undo->text()));
    QVERIFY2(!undo->text().contains(QStringLiteral("Elrendezés")), qPrintable(undo->text()));

    // One undo restores the whole previous page, cells and all.
    QVERIFY(window.waitForPreview(15000));
    undo->trigger();
    QVERIFY(window.document() == before);

    // The new layout is one page raster rebuild, not one per frame.
    QVERIFY(window.waitForPreview(15000));
    QTRY_VERIFY_WITH_TIMEOUT(view->compositionCount() > compositions, 5000);
    QTest::qWait(300);
    const quint64 rebuilds = view->compositionCount() - compositions;
    QVERIFY2(rebuilds <= 2, qPrintable(QStringLiteral("%1 page rebuilds").arg(rebuilds)));
}

void TestTemplateBrowser::applyingFromThePickerClosesIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));

    TemplateBrowserDialog *picker = openPicker(window);
    QVERIFY(picker);
    picker->selectTemplate(QStringLiteral("n03-equal-rows"));
    QVERIFY(picker->applyEnabled());
    picker->findChild<QPushButton *>(QStringLiteral("templateApplyButton"))->click();

    QTRY_VERIFY(!window.findChild<TemplateBrowserDialog *>());
    const Page page = window.document().pages.first();
    QCOMPARE(page.layoutId, QStringLiteral("n03-equal-rows"));
    QCOMPARE(page.slotList.size(), 3);
    QCOMPARE(page.frames.size(), 3);
    QVERIFY(SlotGeometry::layoutConsistent(page));
    for (const Frame &frame : page.frames) {
        QVERIFY(!frame.autoPlaced);
        QVERIFY(!frame.slotId.isEmpty());
    }
}

void TestTemplateBrowser::emptyPageAppliesFromThePicker()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QCOMPARE(window.document().pages.first().frames.size(), 0);

    // An empty page browses every layout, and Apply is enabled as soon as one
    // is chosen: the sections are the point, the photographs come later.
    TemplateBrowserDialog *picker = openPicker(window);
    QVERIFY(picker);
    QCOMPARE(picker->selectedTemplateId(), QString());
    QVERIFY(!picker->applyEnabled());
    picker->selectCount(4);
    picker->selectTemplate(QStringLiteral("n04-grid-2x2"));
    QVERIFY(picker->applyEnabled());
    QVERIFY2(picker->positionNoticeText().contains(emptySlotsSentence(4)),
             qPrintable(picker->positionNoticeText()));
    picker->findChild<QPushButton *>(QStringLiteral("templateApplyButton"))->click();
    QTRY_VERIFY(!window.findChild<TemplateBrowserDialog *>());
    QVERIFY(window.waitForPreview(15000));

    const Page page = window.document().pages.first();
    QCOMPARE(page.layoutId, QStringLiteral("n04-grid-2x2"));
    QCOMPARE(page.slotList.size(), 4);
    QCOMPARE(page.frames.size(), 0);
    // The cells are visible on the canvas immediately, as numbered drop targets.
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    for (const Slot &slot : page.slotList)
        QCOMPARE(view->slotState(slot.id), PageView::SlotState::Empty);

    // One undo takes the sections away again.
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(!window.document().pages.first().hasLayout());
    QCOMPARE(window.document().pages.first().frames.size(), 0);
}

void TestTemplateBrowser::pickerMarksTheLayoutJustApplied()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));

    const QString id = QStringLiteral("n03-equal-cols");
    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied), Q_ARG(QString, id)));
    QVERIFY(applied);

    // Opening the picker on the laid-out page marks and names the layout it
    // already uses, on the layout's own count.
    TemplateBrowserDialog *picker = openPicker(window);
    QVERIFY(picker);
    QCOMPARE(picker->currentLayoutId(), id);
    QCOMPARE(picker->selectedCount(), 3);
    QCOMPARE(picker->selectedTemplateId(), id);
    QVERIFY(picker->templateIsCurrent(id));
    QVERIFY(picker->templateMarkedChosen(id));
    QVERIFY2(picker->currentLayoutText().contains(picker->templateLabel(id)),
             qPrintable(picker->currentLayoutText()));
    QVERIFY(picker->applyEnabled());
    QCOMPARE(picker->positionNoticeText(),
             QStringLiteral("Ez az elrendezés már a lapon van."));

    // Applying it again from the picker is the explained no-op: the dialog
    // closes, the document and the undo history stay exactly as they were.
    const DocumentSnapshot laidOut = window.document();
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    const QString undoText = undo->text();
    const bool undoEnabled = undo->isEnabled();
    picker->findChild<QPushButton *>(QStringLiteral("templateApplyButton"))->click();
    QTRY_VERIFY(!window.findChild<TemplateBrowserDialog *>());
    QVERIFY(window.document() == laidOut);
    QCOMPARE(undo->text(), undoText);
    QCOMPARE(undo->isEnabled(), undoEnabled);
}

void TestTemplateBrowser::secondClickReusesTheOpenPicker()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos));

    TemplateBrowserDialog *first = openPicker(window);
    QVERIFY(first);
    TemplateBrowserDialog *second = openPicker(window);
    QCOMPARE(second, first);
    QCOMPARE(window.findChildren<TemplateBrowserDialog *>().size(), 1);

    // The menu and the page strip open the same one instance as well.
    QVERIFY(QMetaObject::invokeMethod(&window, "openTemplateBrowser", Qt::DirectConnection));
    QCOMPARE(window.findChildren<TemplateBrowserDialog *>().size(), 1);
    first->reject();
    QTest::qWait(80);
    QVERIFY(!window.findChild<TemplateBrowserDialog *>());
}

void TestTemplateBrowser::selectionSurvivesApply()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));

    const QString selected = window.document().pages.first().frames.at(1).id;
    QVERIFY(QMetaObject::invokeMethod(&window, "selectFrame", Qt::DirectConnection,
                                      Q_ARG(QString, selected), Q_ARG(bool, true)));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QCOMPARE(view->selectedFrame(), selected);

    TemplateBrowserDialog *picker = openPicker(window);
    QVERIFY(picker);
    picker->selectTemplate(QStringLiteral("n03-equal-cols"));
    picker->findChild<QPushButton *>(QStringLiteral("templateApplyButton"))->click();
    QTRY_VERIFY(!window.findChild<TemplateBrowserDialog *>());
    QVERIFY(window.waitForPreview(15000));

    // The ids survive the layout, so the selection is still the same frame.
    QVERIFY(findFrame(window.document(), selected));
    QCOMPARE(view->selectedFrame(), selected);
}

void TestTemplateBrowser::applyingToASecondPageLeavesTheFirstAlone()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.resize(1280, 900);
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));
    const QList<Frame> firstPage = window.document().pages.first().frames;

    QVERIFY(QMetaObject::invokeMethod(&window, "addPage", Qt::DirectConnection));
    QTest::qWait(120);
    QCOMPARE(window.document().pages.size(), 2);
    QCOMPARE(window.document().currentPage, 1);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));
    QCOMPARE(window.document().pages.at(1).frames.size(), 3);
    const Page secondPage = window.document().pages.at(1);

    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied),
                                      Q_ARG(QString, QStringLiteral("n03-equal-cols"))));
    QVERIFY(applied);

    const DocumentSnapshot document = window.document();
    // Only the page the picker was opened for changes.
    QCOMPARE(document.pages.first().frames, firstPage);
    QVERIFY(!document.pages.first().hasLayout());
    const Page laidOut = document.pages.at(1);
    QCOMPARE(laidOut.layoutId, QStringLiteral("n03-equal-cols"));
    QCOMPARE(laidOut.slotList.size(), 3);
    QCOMPARE(laidOut.frames.size(), 3);
    QVERIFY(SlotGeometry::layoutConsistent(laidOut));
    for (int i = 0; i < secondPage.frames.size(); ++i) {
        const Frame &frame = laidOut.frames.at(i);
        QCOMPARE(frame.id, secondPage.frames.at(i).id);
        QVERIFY(!frame.autoPlaced);
        QCOMPARE(frame.fit, FitMode::Fill);
        QVERIFY(!frame.slotId.isEmpty());
    }

    undoAction(window)->trigger();
    QCOMPARE(window.document().pages.first().frames, firstPage);
    QCOMPARE(window.document().pages.at(1), secondPage);
}

void TestTemplateBrowser::reapplyingTheSameLayoutChangesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));

    const QString templateId = QStringLiteral("n03-equal-rows");
    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied), Q_ARG(QString, templateId)));
    QVERIFY(applied);

    // Save the laid-out page and load it back: a clean project with frames and
    // no memory of the template.
    const QString path = dir.filePath(QStringLiteral("laid-out.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, window.document(), SaveMode::Normal).ok());
    const auto loaded = store.load(path);
    QVERIFY(loaded);
    qRegisterMetaType<pp::LoadedProject>("pp::LoadedProject");
    QVERIFY(QMetaObject::invokeMethod(&window, "applyLoadedProject", Qt::DirectConnection,
                                      Q_ARG(pp::LoadedProject, loaded.value()),
                                      Q_ARG(bool, false)));
    QVERIFY(!window.windowTitle().endsWith(QStringLiteral("*")));
    const DocumentSnapshot laidOutDocument = window.document();
    const Page reloaded = laidOutDocument.pages.first();
    QCOMPARE(reloaded.layoutId, templateId);
    QCOMPARE(reloaded.slotList.size(), 3);
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY(!undo->isEnabled());

    // The same layout on the same page is a no-op: the page already uses it, so
    // there is nothing to top up and no second edit.
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied), Q_ARG(QString, templateId)));
    QVERIFY(applied);
    QVERIFY(window.document() == laidOutDocument);
    QVERIFY(!undo->isEnabled());
    QVERIFY(!window.windowTitle().endsWith(QStringLiteral("*")));
}

void TestTemplateBrowser::applyingADifferentLayoutReplacesTheOldOne()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));

    bool applied = true;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied),
                                      Q_ARG(QString, QStringLiteral("n03-equal-cols"))));
    QVERIFY(applied);
    const DocumentSnapshot withThree = window.document();
    QCOMPARE(withThree.pages.first().slotList.size(), 3);

    // A two-cell layout on the same three-photo page: the old cells are
    // replaced, two photographs take the new cells and the third stays free.
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied),
                                      Q_ARG(QString, QStringLiteral("n02-equal-stack"))));
    QVERIFY(applied);

    const Page after = window.document().pages.first();
    QCOMPARE(after.layoutId, QStringLiteral("n02-equal-stack"));
    QCOMPARE(after.slotList.size(), 2);
    QCOMPARE(after.frames.size(), 3);
    QVERIFY(SlotGeometry::layoutConsistent(after));
    int slotted = 0;
    int free = 0;
    for (const Frame &frame : after.frames) {
        if (frame.slotId.isEmpty())
            ++free;
        else
            ++slotted;
    }
    QCOMPARE(slotted, 2);
    QCOMPARE(free, 1);

    // One undo brings the previous three-cell layout back.
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QVERIFY(window.document() == withThree);
}

void TestTemplateBrowser::addingAPhotoKeepsTemplatedGeometry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 4);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(addPhotos(window, photos.mid(0, 3)));
    QVERIFY(window.waitForPreview(15000));

    const QString templateId = QStringLiteral("n03-equal-rows");
    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied), Q_ARG(QString, templateId)));
    QVERIFY(applied);

    const QList<Frame> templated = window.document().pages.first().frames;
    QCOMPARE(templated.size(), 3);
    for (const Frame &frame : templated) {
        QVERIFY(!frame.autoPlaced);
        QVERIFY(!frame.slotId.isEmpty());
    }

    // The cells are full, so the next photo goes to a new page with the same
    // layout: the arrangement is never overlapped by a free frame.
    QVERIFY(addPhotos(window, QStringList{photos.at(3)}));
    QVERIFY(window.waitForPreview(15000));

    const DocumentSnapshot document = window.document();
    QCOMPARE(document.pages.size(), 2);
    const Page page = document.pages.first();
    QCOMPARE(page.frames.size(), 3);
    QCOMPARE(page.slotList.size(), 3);
    QCOMPARE(page.layoutId, templateId);
    for (int i = 0; i < templated.size(); ++i) {
        const Frame &frame = page.frames.at(i);
        QCOMPARE(frame.id, templated.at(i).id);
        QCOMPARE(frame.slotId, templated.at(i).slotId);
        QCOMPARE(frame.cx, templated.at(i).cx);
        QCOMPARE(frame.cy, templated.at(i).cy);
        QCOMPARE(frame.width, templated.at(i).width);
        QCOMPARE(frame.height, templated.at(i).height);
    }
    const Page added = document.pages.at(1);
    QVERIFY(added.hasLayout());
    QCOMPARE(added.layoutId, templateId);
    QCOMPARE(added.slotList.size(), 3);
    QCOMPARE(added.frames.size(), 1);
    QCOMPARE(added.frames.first().sourcePath, photos.at(3));
    QVERIFY(!added.frames.first().slotId.isEmpty());
}

void TestTemplateBrowser::openingAndCancellingChangesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(addPhotos(window, photos));
    QVERIFY(window.waitForPreview(15000));

    const DocumentSnapshot before = window.document();
    const QString title = window.windowTitle();

    TemplateBrowserDialog *dialog = openPicker(window);
    QVERIFY(dialog);
    QVERIFY(dialog->isVisible());
    QCOMPARE(dialog->windowTitle(), QStringLiteral("Sablonok ehhez a laphoz"));
    // Three photos on the page: the count-3 layouts are the ones offered, and
    // nothing is chosen yet.
    QCOMPARE(dialog->selectedCount(), 3);
    QCOMPARE(dialog->selectedTemplateId(), QString());
    QVERIFY(!dialog->applyEnabled());
    QCOMPARE(window.document(), before);
    QCOMPARE(window.windowTitle(), title);

    dialog->reject();
    QTest::qWait(80);
    QCOMPARE(window.document(), before);
    QCOMPARE(window.windowTitle(), title);

    // The Edit menu and the page strip open the same picker.
    QAction *fromMenu = window.findChild<QAction *>(QStringLiteral("templatesAction"));
    QVERIFY(fromMenu);
    fromMenu->trigger();
    QVERIFY(window.findChild<TemplateBrowserDialog *>());
    window.findChild<TemplateBrowserDialog *>()->reject();
    QTest::qWait(80);

    auto *fromStrip = window.findChild<QPushButton *>(QStringLiteral("pageStripTemplatesButton"));
    QVERIFY(fromStrip);
    fromStrip->click();
    QVERIFY(window.findChild<TemplateBrowserDialog *>());
    window.findChild<TemplateBrowserDialog *>()->reject();
    QTest::qWait(80);
    QCOMPARE(window.document(), before);
}

void TestTemplateBrowser::languageRebuildClosesThePicker()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path(), 3);

    MainWindow window;
    window.show();
    QTest::qWait(80);
    QVERIFY(addPhotos(window, photos));

    TemplateBrowserDialog *picker = openPicker(window);
    QVERIFY(picker);
    QCOMPARE(picker->windowTitle(), QStringLiteral("Sablonok ehhez a laphoz"));

    // The language menu is the real path: it saves the choice and rebuilds the
    // interface on the next event loop turn.
    QMenu *languageMenu = window.findChild<QMenu *>(QStringLiteral("languageMenu"));
    QVERIFY(languageMenu);
    QAction *germanAction = nullptr;
    for (QAction *action : languageMenu->actions()) {
        if (action->text() == QStringLiteral("Deutsch"))
            germanAction = action;
    }
    QVERIFY(germanAction);
    germanAction->trigger();
    QTRY_VERIFY(i18n::activeLanguage() == QStringLiteral("de"));
    QTRY_VERIFY(!window.findChild<TemplateBrowserDialog *>());
    // The picker carries translated labels, so it is closed rather than left in
    // the previous language; the window keeps working.
    QCOMPARE(window.document().pages.first().frames.size(), 3);

    TemplateBrowserDialog *german = openPicker(window);
    QVERIFY(german);
    QCOMPARE(german->windowTitle(), QStringLiteral("Vorlagen für diese Seite"));
    QCOMPARE(german->findChild<QPushButton *>(QStringLiteral("templateCancelButton"))->text(),
             QStringLiteral("Abbrechen"));
    german->reject();

    // Back to the source language, through the same menu.
    QAction *hungarianAction = nullptr;
    for (QAction *action : window.findChild<QMenu *>(QStringLiteral("languageMenu"))->actions()) {
        if (action->text() == QStringLiteral("Magyar"))
            hungarianAction = action;
    }
    QVERIFY(hungarianAction);
    hungarianAction->trigger();
    QTRY_VERIFY(i18n::activeLanguage().isEmpty());
    QTest::qWait(80);
}

QTEST_MAIN(TestTemplateBrowser)
#include "tst_template_browser.moc"
