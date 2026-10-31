#include "CollageTemplates.h"
#include "Document.h"
#include "MainWindow.h"
#include "PaperCatalog.h"
#include "ProjectStore.h"
#include "TemplateBrowserDialog.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QListWidget>
#include <QPushButton>
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
                                   const QList<QImage> &photos = {})
{
    TemplateBrowserRequest request;
    request.paper = paper;
    request.landscape = false;
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

QString mismatchLine(int needed, int onPage)
{
    return QStringLiteral("Ehhez a sablonhoz %1 fotó kell, a lapon most %2 van.")
        .arg(needed)
        .arg(onPage);
}

} // namespace

class TestTemplateBrowser : public QObject
{
    Q_OBJECT

private slots:
    // The picker on its own.
    void chipsCoverEveryCount();
    void everyCountShowsItsLayoutsAtThePageAspect();
    void browseWorksWithZeroOneAndThreePhotos();
    void mismatchDisablesApplyAndOffersPhotos();
    void elevenFramesShowTheCapacityLine();
    void tinyPaperDisablesWithTheMessage();
    void templateNamesAndAccessibleNames();
    void cropWarningComesFromTheApplyReport();
    void previewDrawsThePhotosInFillOrder();
    void chosenLayoutCarriesACheck();
    void keyboardMovesBetweenChipsAndTiles();

    // The window: applying, undoing, and the guard against a reflow.
    void applyingOneTemplateIsOneUndo();
    void addingAPhotoKeepsTemplatedGeometry();
    void openingAndCancellingChangesNothing();
};

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
    }
    // Browsing is never disabled, whatever the page holds.
    for (int count = 2; count <= 10; ++count)
        QVERIFY(dialog.countChipEnabled(count));
    dialog.selectCount(5);
    QCOMPARE(dialog.selectedCount(), 5);
}

void TestTemplateBrowser::everyCountShowsItsLayoutsAtThePageAspect()
{
    const PaperSpec paper = paperNamed(QStringLiteral("10 × 15 cm"));
    TemplateBrowserDialog dialog(requestWith(paper, framesFor(3)));
    dialog.show();
    QTest::qWait(20);

    const double pageAspect = paper.physicalHeightMm / paper.physicalWidthMm;
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
            // A layout that needs more photos than the page holds cannot be
            // applied, but it stays visible and selectable.
            QCOMPARE(dialog.applyEnabled(), photos == count);
            if (photos != count) {
                QVERIFY2(dialog.countMismatchText() == mismatchLine(count, photos),
                         qPrintable(where + QStringLiteral(": ") + dialog.countMismatchText()));
                QVERIFY2(dialog.addPhotosVisible(), qPrintable(where));
            }
        }
    }
}

void TestTemplateBrowser::mismatchDisablesApplyAndOffersPhotos()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), framesFor(3)));
    dialog.show();
    QTest::qWait(20);
    dialog.selectCount(5);

    QCOMPARE(dialog.countMismatchText(), mismatchLine(5, 3));
    QVERIFY(!dialog.applyEnabled());
    QVERIFY(dialog.addPhotosVisible());

    QSignalSpy added(&dialog, &TemplateBrowserDialog::addPhotosRequested);
    auto *button = dialog.findChild<QPushButton *>(QStringLiteral("templateAddPhotosButton"));
    QVERIFY(button);
    QVERIFY(button->isVisible());
    button->click();
    QCOMPARE(added.count(), 1);

    // The same picker applies once the page holds the right number.
    dialog.selectCount(3);
    QVERIFY(dialog.countMismatchText().isEmpty());
    QVERIFY(!dialog.addPhotosVisible());
    QVERIFY(dialog.applyEnabled());
}

void TestTemplateBrowser::elevenFramesShowTheCapacityLine()
{
    TemplateBrowserDialog dialog(requestWith(paperNamed(QStringLiteral("A4")), framesFor(11)));
    dialog.show();
    QTest::qWait(20);
    dialog.selectCount(10);

    QCOMPARE(dialog.capacityText(),
             QStringLiteral("Ehhez a laphoz legfeljebb 10 fotóhoz van sablon."));
    QVERIFY(!dialog.applyEnabled());
    // Twelve frames are beyond the catalogue, but browsing still works.
    QCOMPARE(dialog.templateTileCount(), 6);
    QVERIFY(dialog.countMismatchText().isEmpty());
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
        QVERIFY2(twoOnTiny.templateEnabled(id), qPrintable(id));
    QVERIFY(twoOnTiny.applyEnabled());
    QVERIFY(twoOnTiny.paperMessageText().isEmpty());

    TemplateBrowserDialog tenOnTiny(requestWith(paper, framesFor(10)));
    tenOnTiny.show();
    QTest::qWait(20);
    tenOnTiny.selectCount(10);
    QCOMPARE(tenOnTiny.templateTileCount(), 6);
    for (const QString &id : tenOnTiny.templateIds())
        QVERIFY2(!tenOnTiny.templateEnabled(id), qPrintable(id));
    QVERIFY(!tenOnTiny.applyEnabled());
    QVERIFY2(tenOnTiny.paperMessageText().contains(QStringLiteral("túl kicsi")),
             qPrintable(tenOnTiny.paperMessageText()));
    QVERIFY2(tenOnTiny.paperMessageText().contains(QStringLiteral("Kicsi")),
             qPrintable(tenOnTiny.paperMessageText()));

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

void TestTemplateBrowser::cropWarningComesFromTheApplyReport()
{
    const PaperSpec paper = paperNamed(QStringLiteral("A4"));
    const auto &catalogue = CollageTemplates::instance();
    const CollageTemplate *t = catalogue.find(QStringLiteral("n03-equal-cols"));
    QVERIFY(t);

    // Frames much wider than any slot: the crop centre and zoom cannot survive.
    TemplateBrowserDialog reset(requestWith(paper, framesFor(3)));
    reset.show();
    QTest::qWait(20);
    reset.selectCount(3);
    reset.selectTemplate(QStringLiteral("n03-equal-cols"));
    QCOMPARE(reset.cropWarningText(),
             QStringLiteral("A kivágásokat az új elrendezéshez igazítjuk."));
    QVERIFY(reset.applyEnabled());

    // Frames already at the mapped slot geometry keep their crop, so there is
    // nothing to warn about.
    const auto mapped = catalogue.slotsMm(*t, paper, false);
    QVERIFY(mapped.ok());
    QList<Frame> matching;
    int index = 0;
    for (const QRectF &slot : mapped.value()) {
        Frame frame = frameWith(QStringLiteral("exact-%1").arg(++index), slot.center().x(),
                                slot.center().y(), slot.width(), slot.height());
        frame.angleDeg = 0.0;
        matching.append(frame);
    }
    TemplateBrowserDialog exact(requestWith(paper, matching));
    exact.show();
    QTest::qWait(20);
    exact.selectCount(3);
    exact.selectTemplate(QStringLiteral("n03-equal-cols"));
    QVERIFY(exact.cropWarningText().isEmpty());
    QVERIFY(exact.applyEnabled());
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

    // The tiles are reachable and change with the arrow keys as well.
    auto *grid = dialog.findChild<QListWidget *>(QStringLiteral("templateGrid"));
    QVERIFY(grid);
    grid->setFocus();
    QCOMPARE(grid->count(), 6);
    grid->setCurrentRow(0);
    QTest::keyClick(grid, Qt::Key_Right);
    QCOMPARE(grid->currentRow(), 1);
    QCOMPARE(dialog.selectedTemplateId(), dialog.templateIds().at(1));
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

    const CollageTemplate *chosen =
        CollageTemplates::instance().find(QStringLiteral("n03-equal-cols"));
    QVERIFY(chosen);
    const auto expected = CollageTemplates::instance().apply(*chosen, page);
    QVERIFY(expected.ok());

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const quint64 compositions = view->compositionCount();

    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied),
                                      Q_ARG(QString, QStringLiteral("n03-equal-cols"))));
    QVERIFY(applied);

    const Page after = window.document().pages.first();
    // The engine's list, field for field: ids, sources, sha, rotation, label,
    // tone and the effect all survive, only the layout changes.
    QCOMPARE(after.frames, expected.value());
    for (int i = 0; i < after.frames.size(); ++i) {
        const Frame &frame = after.frames.at(i);
        QCOMPARE(frame.id, page.frames.at(i).id);
        QCOMPARE(frame.sourcePath, page.frames.at(i).sourcePath);
        QCOMPARE(frame.sourceSha256, page.frames.at(i).sourceSha256);
        QCOMPARE(frame.imageRotationDeg, page.frames.at(i).imageRotationDeg);
        QCOMPARE(frame.label, page.frames.at(i).label);
        QCOMPARE(frame.filter, page.frames.at(i).filter);
        QCOMPARE(frame.effectVersion, page.frames.at(i).effectVersion);
        QCOMPARE(frame.upscale, page.frames.at(i).upscale);
        QCOMPARE(frame.fit, FitMode::Fill);
        QCOMPARE(frame.angleDeg, 0.0);
        QCOMPARE(frame.autoPlaced, false);
    }

    // The edit is marked in the title and named after the action.
    QVERIFY(window.windowTitle().endsWith(QStringLiteral("*")));
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY2(undo->text().contains(QStringLiteral("Sablon alkalmazása")),
             qPrintable(undo->text()));
    QVERIFY2(!undo->text().contains(QStringLiteral("Elrendezés")), qPrintable(undo->text()));

    // One undo restores the whole previous page.
    QVERIFY(window.waitForPreview(15000));
    undo->trigger();
    QCOMPARE(window.document().pages.first().frames, page.frames);

    // The new layout is one page raster rebuild, not one per frame.
    QVERIFY(window.waitForPreview(15000));
    QTRY_VERIFY_WITH_TIMEOUT(view->compositionCount() > compositions, 5000);
    QTest::qWait(300);
    const quint64 rebuilds = view->compositionCount() - compositions;
    QVERIFY2(rebuilds <= 2, qPrintable(QStringLiteral("%1 page rebuilds").arg(rebuilds)));
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
    for (const Frame &frame : templated)
        QVERIFY(!frame.autoPlaced);

    // A template layout is hand work: the next photo is added beside it and
    // never reflows the arrangement.
    QVERIFY(addPhotos(window, QStringList{photos.at(3)}));
    QVERIFY(window.waitForPreview(15000));

    const Page page = window.document().pages.first();
    QCOMPARE(page.frames.size(), 4);
    for (int i = 0; i < templated.size(); ++i) {
        const Frame &frame = page.frames.at(i);
        QCOMPARE(frame.id, templated.at(i).id);
        QCOMPARE(frame.cx, templated.at(i).cx);
        QCOMPARE(frame.cy, templated.at(i).cy);
        QCOMPARE(frame.width, templated.at(i).width);
        QCOMPARE(frame.height, templated.at(i).height);
    }
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

    auto *button = window.findChild<QPushButton *>(QStringLiteral("templatesButton"));
    QVERIFY(button);
    button->click();
    auto *dialog = window.findChild<TemplateBrowserDialog *>();
    QVERIFY(dialog);
    QVERIFY(dialog->isVisible());
    QCOMPARE(dialog->windowTitle(), QStringLiteral("Sablonok ehhez a laphoz"));
    // Three photos on the page: the count-3 layouts are the applicable ones.
    QCOMPARE(dialog->selectedCount(), 3);
    QVERIFY(dialog->applyEnabled());
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
    auto *second = window.findChild<TemplateBrowserDialog *>();
    QVERIFY(second);
    QVERIFY(second->isVisible());
    second->reject();
    QTest::qWait(80);

    auto *fromStrip = window.findChild<QPushButton *>(QStringLiteral("pageStripTemplatesButton"));
    QVERIFY(fromStrip);
    fromStrip->click();
    auto *third = window.findChild<TemplateBrowserDialog *>();
    QVERIFY(third);
    QVERIFY(third->isVisible());
    third->reject();
    QTest::qWait(80);
    QCOMPARE(window.document(), before);
}

QTEST_MAIN(TestTemplateBrowser)
#include "tst_template_browser.moc"
