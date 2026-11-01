#include "Document.h"
#include "CollageTemplates.h"
#include "ImageSource.h"
#include "MainWindow.h"
#include "ProjectStore.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace pp;

namespace {

// The wheel burst commit waits 200 ms after the last notch.
constexpr int kZoomBurstMs = 200;

PaperSpec a4Paper()
{
    PaperSpec paper;
    paper.name = QStringLiteral("A4");
    paper.physicalWidthMm = 210.0;
    paper.physicalHeightMm = 297.0;
    paper.type = PaperType::Plain;
    return paper;
}

/// The smallest catalogue sheet: every clamp in the layout panel has a visible
/// effect on it.
PaperSpec tenByFifteenPaper()
{
    PaperSpec paper;
    paper.name = QStringLiteral("10 × 15 cm");
    paper.physicalWidthMm = 100.0;
    paper.physicalHeightMm = 150.0;
    paper.type = PaperType::Glossy;
    return paper;
}

/// A sheet small enough that a 15 mm half-cell is impossible: the split refuses.
PaperSpec tinyPaper()
{
    PaperSpec paper;
    paper.name = QStringLiteral("40 × 60 mm");
    paper.physicalWidthMm = 40.0;
    paper.physicalHeightMm = 60.0;
    paper.type = PaperType::Plain;
    paper.custom = true;
    return paper;
}

QString makePhoto(const QString &directory, const QString &name, const QColor &colour,
                  QSize size = QSize(600, 400))
{
    QImage image(size, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    const QString path = QDir(directory).filePath(name);
    QImageWriter writer(path, "png");
    if (!writer.write(image))
        return {};
    return path;
}

/// A tiling grid of `count` slots, in fill order. The rectangles are split
/// evenly, so a cell always exists on the default automatic margin.
QList<Slot> gridSlots(int count)
{
    QList<Slot> cells;
    const int columns = count <= 2 ? std::max(1, count) : 2;
    const int rows = (count + columns - 1) / columns;
    for (int i = 0; i < count; ++i) {
        const int row = i / columns;
        const int column = i % columns;
        Slot slot;
        slot.id = QStringLiteral("slot-test-%1").arg(i);
        slot.unitRect = QRectF(double(column) / columns, double(row) / rows,
                               1.0 / columns, 1.0 / rows);
        cells.append(slot);
    }
    return cells;
}

/// A page with a real layout: `slotCount` cells and the first photographs
/// assigned to the first cells, in fill order. Geometry comes from the engine,
/// exactly as it would on a page loaded from a project.
DocumentSnapshot documentWithLayout(const QStringList &photos, int slotCount,
                                    const PaperSpec &paper = a4Paper())
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-test");
    page.paper = paper;
    page.slotList = gridSlots(slotCount);
    page.layoutId = QStringLiteral("hand-made");
    document.pages.append(page);
    document.currentPage = 0;

    Page built = page;
    for (int i = 0; i < photos.size() && i < slotCount; ++i) {
        Frame frame;
        frame.id = QStringLiteral("frame-test-%1").arg(i);
        frame.sourcePath = photos.at(i);
        frame.sourceSha256 = fileSha256(photos.at(i));
        frame.fit = FitMode::Fill;
        frame.autoPlaced = false;
        const auto placed =
            SlotLayoutEngine::placePhoto(built, built.slotList.at(i).id, frame);
        if (!placed.ok())
            return {};
        built = placed.value();
    }
    document.pages[0] = built;
    return document;
}

/// A page without slots and one free frame per path, large enough to crop and
/// zoom. The ids are stable so a test can address them directly.
DocumentSnapshot documentWithFreeFrames(const QStringList &photos)
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-free");
    page.paper = a4Paper();
    for (int i = 0; i < photos.size(); ++i) {
        Frame frame;
        frame.id = QStringLiteral("frame-free-%1").arg(i);
        frame.sourcePath = photos.at(i);
        frame.sourceSha256 = fileSha256(photos.at(i));
        frame.cx = 60.0 + 80.0 * i;
        frame.cy = 80.0 + 100.0 * i;
        frame.width = 70.0;
        frame.height = 50.0;
        frame.fit = FitMode::Fill;
        frame.cropCenter = QPointF(0.5, 0.5);
        frame.autoPlaced = false;
        page.frames.append(frame);
    }
    document.pages.append(page);
    document.currentPage = 0;
    return document;
}

/// Loads a document through the real project path, so the window sees a layout
/// the way it sees one from disk: no test-only setter, no private shortcut.
bool loadDocument(MainWindow &window, const QTemporaryDir &dir, const DocumentSnapshot &document,
                  const QString &name)
{
    const QString path = dir.filePath(name);
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

QAction *undoAction(MainWindow &window)
{
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("Vissza")
            || action->text().startsWith(QStringLiteral("Vissza "))) {
            return action;
        }
    }
    return nullptr;
}

void dropFiles(PageView &view, const QPointF &position, const QStringList &paths)
{
    QMimeData mime;
    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString &path : paths)
        urls.append(QUrl::fromLocalFile(path));
    mime.setUrls(urls);
    QDropEvent event(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    view.dropForTest(&event);
}

void dropFrame(PageView &view, const QPointF &position, const QString &frameId)
{
    QMimeData mime;
    mime.setData(QString::fromLatin1(kFrameMimeType), frameId.toUtf8());
    QDropEvent event(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    view.dropForTest(&event);
}

void sendWheel(PageView &view, const QPointF &position, int angle,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QWheelEvent event(position, QPointF(view.mapToGlobal(position.toPoint())), QPoint(),
                      QPoint(0, angle), Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&view, &event);
}

QPointF pageToWidget(const PageView &view, const QPointF &mm)
{
    const QRectF rect = view.pageRect();
    const QSizeF page = view.pageMm();
    if (page.isEmpty())
        return {};
    return QPointF(rect.left() + mm.x() / page.width() * rect.width(),
                   rect.top() + mm.y() / page.height() * rect.height());
}

QRgb rasterPixelAtMm(const QImage &image, const QSizeF &pageMm, const QPointF &mm)
{
    if (image.isNull() || pageMm.isEmpty())
        return qRgb(0, 0, 0);
    const int x = std::clamp(int(mm.x() / pageMm.width() * image.width()), 0,
                             image.width() - 1);
    const int y = std::clamp(int(mm.y() / pageMm.height() * image.height()), 0,
                             image.height() - 1);
    return image.pixel(x, y);
}

const Frame *frameInSlot(const Page &page, const QString &slotId)
{
    return SlotGeometry::frameInSlot(page, slotId);
}

/// One of the layout panel's spin boxes, by object name.
QDoubleSpinBox *spinNamed(MainWindow &window, const char *name)
{
    return window.findChild<QDoubleSpinBox *>(QString::fromLatin1(name));
}

/// Finishes a spin edit the way the panel commits one: the editing finished,
/// not every keystroke. A test cannot type, so it delivers the same signal.
void commitSpin(QDoubleSpinBox *spin)
{
    if (!spin)
        return;
    QMetaObject::invokeMethod(spin, "editingFinished");
}

/// The undo action's text for one command: the stack prefixes its own title.
bool undoLabelIs(MainWindow &window, const QString &label)
{
    QAction *action = undoAction(window);
    return action && action->text() == QStringLiteral("Vissza ") + label;
}

/// Answers the next modal question with `button`. A modal dialog runs its own
/// event loop, so a timer is what reaches into it; the flag is on the heap
/// because the timer may outlive this call's frame when no dialog ever appears.
void answerNextQuestionWith(QMessageBox::StandardButton button)
{
    auto tries = std::make_shared<int>(0);
    auto *timer = new QTimer;
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, button, tries] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            if (QAbstractButton *target = box->button(button))
                target->click();
            timer->stop();
            timer->deleteLater();
            return;
        }
        if (++*tries > 1000) {
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}

/// The linear layer the window's preview job prepares for the page raster, so
/// the canvas can be compared with the print raster of the same page.
QImage preparedPixels(const QString &path)
{
    ImageStore store;
    const auto source = store.load(path, ImportPolicy{});
    if (!source)
        return {};
    const auto pixels = store.preparedLinear(source.value(), ColorRecipe{},
                                             QStringLiteral("original"), 100, false, 1600);
    return pixels ? pixels.value() : QImage();
}

} // namespace

class TestSlotUi : public QObject
{
    Q_OBJECT

private slots:
    void clickSelectsSlotAndEscapeClears();
    void selectionSurvivesDocumentChange();
    void placeholderStates();
    void placeholdersNeverPrint();
    void fileDropOnSlotIsOneUndo();
    void backgroundDropFillsFirstEmptySlot();
    void multiFileDropIsOneUndo();
    void libraryDragMovesSwapsAndDetaches();
    void dragInsideSlotPansThePhoto();
    void wheelZoomsPhotoCtrlZoomsPage();
    void addPhotoFillsFirstEmptySlot();
    void applyLayoutToAnEmptyPageThenFill();
    void topUpFillsTheDetachedPhoto();
    void surplusFilesGoToANewPage();
    void filledCellDropReplacesAndUndoes();
    void hoverOnFilledCellSaysReplace();
    void multiFileDropOnFreeCanvasIsOneUndo();
    void freeCanvasLibraryDropMovesThePhoto();
    void undoPromotesSelectedCell();
    void missingStateAfterPreviewFinished();
    void fitPageStaysHiddenOnALaidOutPageThatOverflows();
    void cropWheelAfterSelectionChangeIsSafe();
    void externalReplacementCancelsWheelBurst();
    void slottedFrameControlsAreDisabled();
    void templateApplyKeepsSlotsOnALaidOutPage();
    void deleteClearsSelectedSlot();
    void invalidDropChangesNothing();
    void layoutPanelSurvivesAnEmptySelection();
    void layoutNameIsTheEnglishCatalogueName();
    void marginAndGutterAreAutomaticOrZero();
    void styleValuesClampOnSmallPaper();
    void borderColourWithAlphaRoundTrips();
    void oneUndoPerStyleChange();
    void addSlotSplitsAndRefusesAtTheCap();
    void removeSlotLeavesTheGap();
    void dissolveKeepsThePhotographs();
    void borderPrintsAndTheCanvasAgrees();
};

void TestSlotUi::clickSelectsSlotAndEscapeClears()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("one.png"), QColor(180, 80, 60));
    QVERIFY(!photo.isEmpty());
    const DocumentSnapshot document = documentWithLayout({photo}, 4);
    QVERIFY(!document.pages.isEmpty());
    const Page page = document.pages.first();
    const QString filledSlot = page.slotList.at(0).id;
    const QString emptySlot = page.slotList.at(1).id;
    const QString frameId = frameInSlot(page, filledSlot)->id;

    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages({});
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    // An empty cell is a first-class selection.
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                      view.slotRect(emptySlot).center().toPoint());
    QCOMPARE(view.selectedSlot(), emptySlot);
    QVERIFY(view.selectedFrame().isEmpty());

    QTest::keyClick(&view, Qt::Key_Escape);
    QVERIFY(view.selectedSlot().isEmpty());
    QVERIFY(view.selectedFrame().isEmpty());

    // Clicking the mat or the photograph of a filled cell selects the photo and
    // its cell.
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                      view.slotRect(filledSlot).center().toPoint());
    QCOMPARE(view.selectedFrame(), frameId);
    QCOMPARE(view.selectedSlot(), filledSlot);

    // Clicking the paper outside every cell clears both.
    QVERIFY(!view.pageRect().contains(QPointF(3.0, 3.0)));
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(3, 3));
    QVERIFY(view.selectedSlot().isEmpty());
    QVERIFY(view.selectedFrame().isEmpty());
}

void TestSlotUi::selectionSurvivesDocumentChange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("one.png"), QColor(60, 120, 180));
    DocumentSnapshot document = documentWithLayout({photo}, 4);
    QVERIFY(!document.pages.isEmpty());
    const QString slotId = document.pages.first().slotList.at(1).id;

    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setSelectedSlot(slotId);
    QCOMPARE(view.selectedSlot(), slotId);

    // A document change that keeps the cell keeps the selection.
    DocumentSnapshot moved = document;
    moved.pages[0].slotStyle.borderMm = 3.0;
    auto restyled = SlotLayoutEngine::repair(moved.pages.at(0));
    QVERIFY(restyled.ok());
    moved.pages[0] = restyled.value();
    view.setDocument(moved);
    QCOMPARE(view.selectedSlot(), slotId);

    // Removing the cell clears the selection instead of leaving a stale id.
    auto removed = SlotLayoutEngine::removeSlot(moved.pages.at(0), slotId);
    QVERIFY(removed.ok());
    DocumentSnapshot withoutSlot = moved;
    withoutSlot.pages[0] = removed.value();
    view.setDocument(withoutSlot);
    QVERIFY(view.selectedSlot().isEmpty());
}

void TestSlotUi::placeholderStates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("one.png"), QColor(90, 160, 90));
    const DocumentSnapshot document = documentWithLayout({photo}, 4);
    QVERIFY(!document.pages.isEmpty());
    const Page page = document.pages.first();
    const QString filledSlot = page.slotList.at(0).id;
    const QString emptySlot = page.slotList.at(1).id;
    const QString frameId = frameInSlot(page, filledSlot)->id;

    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    // Nothing decoded yet: a fresh drop is loading, never "not found".
    view.setFrameImages({});
    QCOMPARE(view.slotState(filledSlot), PageView::SlotState::Loading);
    QCOMPARE(view.slotState(emptySlot), PageView::SlotState::Empty);
    QCOMPARE(view.slotState(QStringLiteral("slot-that-does-not-exist")),
             PageView::SlotState::Empty);

    // The window proves the source unreadable only after a preview pass.
    view.setUnavailableFrames({frameId});
    QCOMPARE(view.slotState(filledSlot), PageView::SlotState::Missing);

    // Decoded pixels are the only thing that makes a cell filled.
    QImage pixels(40, 30, QImage::Format_RGB32);
    pixels.fill(QColor(90, 160, 90));
    view.setFrameImages({{frameId, pixels}});
    QCOMPARE(view.slotState(filledSlot), PageView::SlotState::Filled);
}

void TestSlotUi::placeholdersNeverPrint()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("one.png"), QColor(200, 40, 40));
    const DocumentSnapshot document = documentWithLayout({photo}, 4);
    QVERIFY(!document.pages.isEmpty());
    const Page page = document.pages.first();
    const QString emptySlot = page.slotList.at(1).id;

    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages({});
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTest::qWait(100);

    // The composed raster is the page content the PDF path shares: an empty
    // cell is white paper, never the dashed editor placeholder.
    const QImage composed = view.composedImage();
    QVERIFY(!composed.isNull());
    const Slot *empty = SlotGeometry::slotById(page, emptySlot);
    QVERIFY(empty);
    const QRgb centre =
        rasterPixelAtMm(composed, view.pageMm(), SlotGeometry::slotCellMm(page, *empty).center());
    QVERIFY2(qRed(centre) >= 250 && qGreen(centre) >= 250 && qBlue(centre) >= 250,
             qPrintable(QStringLiteral("empty cell printed %1,%2,%3")
                            .arg(qRed(centre))
                            .arg(qGreen(centre))
                            .arg(qBlue(centre))));

    // The widget does show an accent placeholder inside that cell.
    const QRectF cell = view.slotRect(emptySlot);
    const QImage screen = view.grab().toImage();
    int accent = 0;
    const QRect sample = cell.toRect().intersected(screen.rect());
    for (int y = sample.top(); y <= sample.bottom(); ++y) {
        for (int x = sample.left(); x <= sample.right(); ++x) {
            const QRgb pixel = screen.pixel(x, y);
            if (qBlue(pixel) > 150 && qRed(pixel) < 140 && qGreen(pixel) < 180)
                ++accent;
        }
    }
    QVERIFY2(accent > 10, qPrintable(QStringLiteral("accent placeholder pixels %1").arg(accent)));
}

void TestSlotUi::fileDropOnSlotIsOneUndo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("drop.png"), QColor(70, 140, 200));
    QVERIFY(!photo.isEmpty());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({}, 4), QStringLiteral("empty.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(window.document().pages.first().hasLayout());

    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    const Slot target = page.slotList.at(1);
    const QPointF centre = view->slotRect(target.id).center();
    QVERIFY(!centre.isNull());
    QVERIFY2(view->slotAtWidget(centre) == target.id,
             qPrintable(QStringLiteral("slot under drop point: '%1' wanted '%2'")
                            .arg(view->slotAtWidget(centre), target.id)));
    QVERIFY2(view->slotState(target.id) == PageView::SlotState::Empty,
             qPrintable(QStringLiteral("state before drop %1").arg(int(view->slotState(target.id)))));

    dropFiles(*view, centre, {photo});
    // The drop decodes through the normal preview path; it must not claim the
    // photo is missing while the pixels are still on their way.
    QVERIFY(view->slotState(target.id) != PageView::SlotState::Empty);
    QTest::qWait(50);
    QVERIFY(view->slotState(target.id) != PageView::SlotState::Missing);
    QVERIFY(window.waitForPreview(15000));

    const DocumentSnapshot dropped = window.document();
    const Page after = dropped.pages.first();
    QCOMPARE(after.frames.size(), 1);
    const Frame *placed = frameInSlot(after, target.id);
    QVERIFY(placed);
    QCOMPARE(placed->sourcePath, photo);
    QCOMPARE(placed->slotId, target.id);
    const QRectF expected = SlotGeometry::slotPhotoRectMm(after, target);
    QVERIFY(std::abs(placed->cx - expected.center().x()) < 0.01);
    QVERIFY(std::abs(placed->cy - expected.center().y()) < 0.01);
    QVERIFY(std::abs(placed->width - expected.width()) < 0.01);
    QVERIFY(std::abs(placed->height - expected.height()) < 0.01);
    QVERIFY(!placed->autoPlaced);

    auto *list = window.findChild<QListWidget *>();
    QVERIFY(list);
    QCOMPARE(list->count(), 1);

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY(undo->isEnabled());
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
}

void TestSlotUi::backgroundDropFillsFirstEmptySlot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("bg.png"), QColor(120, 90, 180));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({}, 4), QStringLiteral("empty.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);

    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    // A point inside the paper but outside every cell: the automatic margin.
    const QPointF background = view->pageRect().topLeft() + QPointF(4.0, 4.0);
    QVERIFY(view->pageRect().contains(background));
    QVERIFY(view->slotAtWidget(background).isEmpty());

    dropFiles(*view, background, {photo});
    QVERIFY(window.waitForPreview(15000));

    const Page after = window.document().pages.first();
    QCOMPARE(after.frames.size(), 1);
    QVERIFY(frameInSlot(after, page.slotList.at(0).id));

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
}

void TestSlotUi::multiFileDropIsOneUndo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = {
        makePhoto(dir.path(), QStringLiteral("a.png"), QColor(200, 60, 60)),
        makePhoto(dir.path(), QStringLiteral("b.png"), QColor(60, 200, 60)),
        makePhoto(dir.path(), QStringLiteral("c.png"), QColor(60, 60, 200)),
    };
    for (const QString &photo : photos)
        QVERIFY(!photo.isEmpty());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({}, 4), QStringLiteral("empty.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);

    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    const QPointF centre = view->slotRect(page.slotList.at(0).id).center();

    dropFiles(*view, centre, photos);
    QVERIFY(window.waitForPreview(15000));

    const Page after = window.document().pages.first();
    QCOMPARE(after.frames.size(), 3);
    // The first file lands on the target cell, the rest fill the following
    // empty cells in slot order.
    for (int i = 0; i < photos.size(); ++i) {
        const Frame *placed = frameInSlot(after, page.slotList.at(i).id);
        QVERIFY(placed);
        QCOMPARE(placed->sourcePath, photos.at(i));
    }

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
}

void TestSlotUi::libraryDragMovesSwapsAndDetaches()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photoA = makePhoto(dir.path(), QStringLiteral("a.png"), QColor(210, 70, 70));
    const QString photoB = makePhoto(dir.path(), QStringLiteral("b.png"), QColor(70, 70, 210));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photoA, photoB}, 4),
                         QStringLiteral("two.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(window.waitForPreview(15000));

    Page page = window.document().pages.first();
    const QString slot0 = page.slotList.at(0).id;
    const QString slot1 = page.slotList.at(1).id;
    const QString slot2 = page.slotList.at(2).id;
    const QString frameA = frameInSlot(page, slot0)->id;
    const QString frameB = frameInSlot(page, slot1)->id;
    const QString originalA = frameInSlot(page, slot0)->sourcePath;
    const QString originalB = frameInSlot(page, slot1)->sourcePath;

    // Move: an empty target cell takes the photograph and the old cell empties.
    const DocumentSnapshot beforeMove = window.document();
    dropFrame(*view, view->slotRect(slot2).center(), frameA);
    QTest::qWait(50);
    page = window.document().pages.first();
    QVERIFY(!frameInSlot(page, slot0));
    QVERIFY(frameInSlot(page, slot2));
    QCOMPARE(frameInSlot(page, slot2)->id, frameA);
    QCOMPARE(window.document().pages.first().frames.size(), 2);
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == beforeMove);

    // Swap: a filled target cell exchanges the photographs, not the frames.
    const DocumentSnapshot beforeSwap = window.document();
    dropFrame(*view, view->slotRect(slot1).center(), frameA);
    QTest::qWait(50);
    page = window.document().pages.first();
    QCOMPARE(frameInSlot(page, slot0)->id, frameA);
    QCOMPARE(frameInSlot(page, slot1)->id, frameB);
    QCOMPARE(frameInSlot(page, slot0)->sourcePath, originalB);
    QCOMPARE(frameInSlot(page, slot1)->sourcePath, originalA);
    undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == beforeSwap);

    // Detach: dropped outside every cell, the photo becomes a free frame and
    // the cell stays empty. One undo puts it back.
    const DocumentSnapshot beforeDetach = window.document();
    const QPointF background = view->pageRect().topLeft() + QPointF(4.0, 4.0);
    QVERIFY(view->slotAtWidget(background).isEmpty());
    dropFrame(*view, background, frameA);
    QTest::qWait(50);
    page = window.document().pages.first();
    QVERIFY(!frameInSlot(page, slot0));
    bool free = false;
    for (const Frame &frame : page.frames) {
        if (frame.id == frameA && frame.slotId.isEmpty())
            free = true;
    }
    QVERIFY(free);
    undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == beforeDetach);
}

void TestSlotUi::dragInsideSlotPansThePhoto()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("pan.png"), QColor(120, 160, 90));
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4), QStringLiteral("pan.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(window.waitForPreview(15000));

    const Page page = window.document().pages.first();
    const QString slotId = page.slotList.at(0).id;
    const QPointF centre = view->slotRect(slotId).center();
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    const Frame before = *frameInSlot(window.document().pages.first(), slotId);
    const DocumentSnapshot beforeDrag = window.document();

    // Select first: the inspector appears and the canvas changes width, so the
    // gesture starts from the cell's current position.
    const QPointF start = view->slotRect(slotId).center();
    QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, start.toPoint());
    QTest::mouseMove(view, (start + QPointF(60.0, 30.0)).toPoint());
    QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier,
                        (start + QPointF(60.0, 30.0)).toPoint());
    QTest::qWait(50);

    const Frame after = *frameInSlot(window.document().pages.first(), slotId);
    // The cell did not move: the photograph inside it did.
    QCOMPARE(after.cx, before.cx);
    QCOMPARE(after.cy, before.cy);
    QCOMPARE(after.width, before.width);
    QCOMPARE(after.height, before.height);
    QVERIFY(after.cropCenter != before.cropCenter);

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == beforeDrag);
}

void TestSlotUi::wheelZoomsPhotoCtrlZoomsPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("zoom.png"), QColor(90, 140, 90));
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4), QStringLiteral("zoom.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(window.waitForPreview(15000));

    const Page page = window.document().pages.first();
    const QString slotId = page.slotList.at(0).id;
    const QString frameId = frameInSlot(page, slotId)->id;
    const QPointF centre = view->slotRect(slotId).center();

    // Selecting the cell and wheeling zooms the photograph, never the page.
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    QCOMPARE(view->selectedFrame(), frameId);
    // The inspector appears with a selection and the canvas changes width, so
    // the cell is located again before the wheel.
    const QPointF wheelPoint = view->slotRect(slotId).center();
    QVERIFY2(view->slotAtWidget(wheelPoint) == slotId,
             qPrintable(QStringLiteral("slot under wheel: '%1' wanted '%2'")
                            .arg(view->slotAtWidget(wheelPoint), slotId)));
    QVERIFY2(view->slotState(slotId) == PageView::SlotState::Filled,
             qPrintable(QStringLiteral("state under wheel %1").arg(int(view->slotState(slotId)))));
    const DocumentSnapshot beforeWheel = window.document();
    const double pageZoom = view->viewZoom();
    for (int notch = 0; notch < 5; ++notch) {
        sendWheel(*view, wheelPoint, 120);
        QTest::qWait(25);
    }
    QTest::qWait(kZoomBurstMs + 150);

    QCOMPARE(view->viewZoom(), pageZoom);
    const Frame zoomed = *SlotGeometry::frameInSlot(window.document().pages.first(), slotId);
    QVERIFY2(zoomed.zoom > 1.0,
             qPrintable(QStringLiteral("photo zoom %1").arg(zoomed.zoom)));

    // The whole burst is one edit: one undo restores the exact pre-wheel page.
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == beforeWheel);

    // Ctrl+wheel moves the page view and leaves the photo alone.
    const double frameZoom = SlotGeometry::frameInSlot(window.document().pages.first(), slotId)->zoom;
    sendWheel(*view, view->slotRect(slotId).center(), 120, Qt::ControlModifier);
    QTest::qWait(50);
    QVERIFY(view->viewZoom() > pageZoom);
    QCOMPARE(SlotGeometry::frameInSlot(window.document().pages.first(), slotId)->zoom, frameZoom);
}

void TestSlotUi::addPhotoFillsFirstEmptySlot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("add.png"), QColor(150, 120, 60));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({}, 4), QStringLiteral("empty.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);

    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, QStringList{photo})));
    QVERIFY(window.waitForPreview(15000));

    const Page after = window.document().pages.first();
    QCOMPARE(after.frames.size(), 1);
    QVERIFY(frameInSlot(after, page.slotList.at(0).id));
    QVERIFY(!frameInSlot(after, page.slotList.at(1).id));

    auto *list = window.findChild<QListWidget *>();
    QVERIFY(list);
    QCOMPARE(list->count(), 1);

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
}

void TestSlotUi::applyLayoutToAnEmptyPageThenFill()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("later.png"), QColor(90, 110, 190));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(window.document().pages.first().frames.isEmpty());

    // An empty page takes a layout: four sections, the label, no photographs.
    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied),
                                      Q_ARG(QString, QStringLiteral("n04-grid-2x2"))));
    QVERIFY(applied);

    Page page = window.document().pages.first();
    QCOMPARE(page.layoutId, QStringLiteral("n04-grid-2x2"));
    QCOMPARE(page.slotList.size(), 4);
    QCOMPARE(page.frames.size(), 0);
    QVERIFY(SlotGeometry::layoutConsistent(page));

    // The four numbered cells are on the canvas immediately, all empty and all
    // drop targets. The page now has a layout, so its panel appears without a
    // selection and without a reload.
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    auto *panel = window.findChild<QWidget *>(QStringLiteral("layoutPanel"));
    QVERIFY(panel);
    QVERIFY2(panel->isVisible(), "the layout panel did not appear with the layout");
    for (const Slot &slot : page.slotList) {
        QCOMPARE(view->slotState(slot.id), PageView::SlotState::Empty);
        QVERIFY(!view->slotRect(slot.id).isEmpty());
    }

    // Filling later goes through the ordinary drop path.
    dropFiles(*view, view->slotRect(page.slotList.at(2).id).center(), {photo});
    QVERIFY(window.waitForPreview(15000));

    page = window.document().pages.first();
    QCOMPARE(page.frames.size(), 1);
    const Frame *placed = frameInSlot(page, page.slotList.at(2).id);
    QVERIFY(placed);
    QCOMPARE(placed->sourcePath, photo);
    QCOMPARE(view->slotState(page.slotList.at(0).id), PageView::SlotState::Empty);
    QCOMPARE(view->slotState(page.slotList.at(2).id), PageView::SlotState::Filled);
}

void TestSlotUi::topUpFillsTheDetachedPhoto()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photoA = makePhoto(dir.path(), QStringLiteral("ta.png"), QColor(190, 70, 70));
    const QString photoB = makePhoto(dir.path(), QStringLiteral("tb.png"), QColor(70, 70, 190));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied),
                                      Q_ARG(QString, QStringLiteral("n04-grid-2x2"))));
    QVERIFY(applied);
    const QStringList both = {photoA, photoB};
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, both)));
    QVERIFY(window.waitForPreview(15000));

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    Page page = window.document().pages.first();
    const QString filledSlot = page.slotList.at(0).id;
    const QString emptiedSlot = page.slotList.at(1).id;
    const QString frameB = frameInSlot(page, emptiedSlot)->id;

    // Detach the second photograph: it leaves its cell and becomes free.
    const QPointF background = view->pageRect().topLeft() + QPointF(4.0, 4.0);
    QVERIFY(view->slotAtWidget(background).isEmpty());
    dropFrame(*view, background, frameB);
    QTest::qWait(50);
    page = window.document().pages.first();
    QVERIFY(!frameInSlot(page, emptiedSlot));
    QVERIFY(frameInSlot(page, filledSlot));
    bool free = false;
    for (const Frame &frame : page.frames) {
        if (frame.id == frameB && frame.slotId.isEmpty())
            free = true;
    }
    QVERIFY(free);
    const DocumentSnapshot detached = window.document();

    // Re-applying the same layout is a top-up: the cells and the assigned photo
    // stay, and the free photograph returns to the first empty cell.
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied),
                                      Q_ARG(QString, QStringLiteral("n04-grid-2x2"))));
    QVERIFY(applied);
    page = window.document().pages.first();
    QCOMPARE(page.layoutId, QStringLiteral("n04-grid-2x2"));
    QCOMPARE(page.frames.size(), 2);
    QVERIFY(frameInSlot(page, filledSlot));
    const Frame *topped = frameInSlot(page, emptiedSlot);
    QVERIFY(topped);
    QCOMPARE(topped->id, frameB);
    QVERIFY(SlotGeometry::layoutConsistent(page));

    // The top-up is one edit: one undo restores the detached arrangement.
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == detached);
}

void TestSlotUi::surplusFilesGoToANewPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = {
        makePhoto(dir.path(), QStringLiteral("s1.png"), QColor(200, 60, 60)),
        makePhoto(dir.path(), QStringLiteral("s2.png"), QColor(60, 200, 60)),
        makePhoto(dir.path(), QStringLiteral("s3.png"), QColor(60, 60, 200)),
    };
    for (const QString &photo : photos)
        QVERIFY(!photo.isEmpty());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({}, 2), QStringLiteral("two.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    QCOMPARE(page.slotList.size(), 2);

    // Three files into two cells: the first two fill the page, the third goes to
    // a new page with the same layout. Nothing overlaps the sections.
    dropFiles(*view, view->slotRect(page.slotList.at(0).id).center(), photos);
    QVERIFY(window.waitForPreview(15000));

    const DocumentSnapshot after = window.document();
    QCOMPARE(after.pages.size(), 2);
    const Page first = after.pages.at(0);
    QCOMPARE(first.frames.size(), 2);
    for (const Frame &frame : first.frames)
        QVERIFY(!frame.slotId.isEmpty());
    const Page second = after.pages.at(1);
    QVERIFY(second.hasLayout());
    QCOMPARE(second.layoutId, first.layoutId);
    QCOMPARE(second.slotList.size(), first.slotList.size());
    QCOMPARE(second.frames.size(), 1);
    QVERIFY(SlotGeometry::layoutConsistent(second));
    QVERIFY2(window.statusBar()->currentMessage().contains(
                 QStringLiteral("A többi fotó új lapra került.")),
             qPrintable(window.statusBar()->currentMessage()));

    // One action, one undo: the extra page and the fills go together.
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
}

void TestSlotUi::filledCellDropReplacesAndUndoes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString oldPhoto = makePhoto(dir.path(), QStringLiteral("old.png"), QColor(190, 60, 60));
    const QString newPhoto = makePhoto(dir.path(), QStringLiteral("new.png"), QColor(60, 60, 190));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({oldPhoto}, 4),
                         QStringLiteral("filled.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(window.waitForPreview(15000));

    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    const QString slotId = page.slotList.at(0).id;
    const Frame *oldFrame = frameInSlot(page, slotId);
    QVERIFY(oldFrame);
    const QString oldId = oldFrame->id;

    // Dropping onto a filled cell replaces the photo; the cue is tested on its
    // own, this is the actual replacement and its undo.
    dropFiles(*view, view->slotRect(slotId).center(), {newPhoto});
    QVERIFY(window.waitForPreview(15000));

    const Page after = window.document().pages.first();
    QCOMPARE(after.frames.size(), 1);
    const Frame *placed = frameInSlot(after, slotId);
    QVERIFY(placed);
    QCOMPARE(placed->sourcePath, newPhoto);
    QVERIFY(placed->id != oldId);

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
    QCOMPARE(frameInSlot(window.document().pages.first(), slotId)->sourcePath, oldPhoto);
}

void TestSlotUi::hoverOnFilledCellSaysReplace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("cue.png"), QColor(120, 120, 60));
    const DocumentSnapshot document = documentWithLayout({photo}, 4);
    QVERIFY(!document.pages.isEmpty());
    const Page page = document.pages.first();
    const QString filledSlot = page.slotList.at(0).id;
    const QString emptySlot = page.slotList.at(1).id;
    const QString frameId = frameInSlot(page, filledSlot)->id;

    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setAccessibleDescription(QStringLiteral("alap"));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QMimeData mime;
    mime.setData(QString::fromLatin1(kFrameMimeType), frameId.toUtf8());
    const QPoint filledCentre = view.slotRect(filledSlot).center().toPoint();
    const QPoint emptyCentre = view.slotRect(emptySlot).center().toPoint();

    QDragEnterEvent enter(filledCentre, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    view.dragEnterForTest(&enter);
    QDragMoveEvent overFilled(filledCentre, Qt::CopyAction, &mime, Qt::LeftButton,
                              Qt::NoModifier);
    view.dragMoveForTest(&overFilled);
    QCOMPARE(view.dropCue(), QStringLiteral("Csere"));
    QCOMPARE(view.accessibleDescription(), QStringLiteral("Csere"));

    // The cue is painted, not only announced: the orange badge sits in the
    // filled cell while the drag hovers it.
    const QImage hovering = view.grab().toImage();
    const QRect cell = view.slotRect(filledSlot).toRect().intersected(hovering.rect());
    int cuePixels = 0;
    for (int y = cell.top(); y <= cell.bottom(); ++y) {
        for (int x = cell.left(); x <= cell.right(); ++x) {
            const QRgb pixel = hovering.pixel(x, y);
            if (qRed(pixel) > 150 && qGreen(pixel) < 130 && qBlue(pixel) < 100)
                ++cuePixels;
        }
    }
    QVERIFY2(cuePixels > 20,
             qPrintable(QStringLiteral("replace cue pixels %1").arg(cuePixels)));

    // An empty cell keeps the ordinary drop target; the cue goes away.
    QDragMoveEvent overEmpty(emptyCentre, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    view.dragMoveForTest(&overEmpty);
    QVERIFY(view.dropCue().isEmpty());
    QCOMPARE(view.accessibleDescription(), QStringLiteral("alap"));

    // Leaving the drag restores the base description as well.
    QDragEnterEvent enterAgain(filledCentre, Qt::CopyAction, &mime, Qt::LeftButton,
                               Qt::NoModifier);
    view.dragEnterForTest(&enterAgain);
    QDragMoveEvent overFilledAgain(filledCentre, Qt::CopyAction, &mime, Qt::LeftButton,
                                   Qt::NoModifier);
    view.dragMoveForTest(&overFilledAgain);
    QCOMPARE(view.dropCue(), QStringLiteral("Csere"));
    QDragLeaveEvent leave;
    view.dragLeaveForTest(&leave);
    QVERIFY(view.dropCue().isEmpty());
    QCOMPARE(view.accessibleDescription(), QStringLiteral("alap"));
}

void TestSlotUi::multiFileDropOnFreeCanvasIsOneUndo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = {
        makePhoto(dir.path(), QStringLiteral("f1.png"), QColor(180, 70, 70)),
        makePhoto(dir.path(), QStringLiteral("f2.png"), QColor(70, 180, 70)),
        makePhoto(dir.path(), QStringLiteral("f3.png"), QColor(70, 70, 180)),
    };

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    const DocumentSnapshot before = window.document();
    QVERIFY(!before.pages.first().hasLayout());

    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QVERIFY(window.waitForPreview(15000));
    QCOMPARE(window.document().pages.first().frames.size(), 3);

    // One undo removes all three, not one photograph per drop.
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
}

void TestSlotUi::freeCanvasLibraryDropMovesThePhoto()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("move.png"), QColor(90, 160, 90));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, QStringList{photo})));
    QVERIFY(window.waitForPreview(15000));

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const DocumentSnapshot before = window.document();
    QVERIFY(!before.pages.first().hasLayout());
    const Frame oldFrame = before.pages.first().frames.first();

    const QPointF target = view->pageRect().center() + QPointF(-30.0, 25.0);
    dropFrame(*view, target, oldFrame.id);
    QTest::qWait(50);

    const Frame moved = window.document().pages.first().frames.first();
    QVERIFY(moved.cx != oldFrame.cx || moved.cy != oldFrame.cy);
    QCOMPARE(moved.id, oldFrame.id);

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
}

void TestSlotUi::undoPromotesSelectedCell()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("undo.png"), QColor(150, 90, 150));
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4),
                         QStringLiteral("undo.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(window.waitForPreview(15000));

    const Page page = window.document().pages.first();
    const QString slotId = page.slotList.at(0).id;
    const QString frameId = frameInSlot(page, slotId)->id;

    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(slotId).center().toPoint());
    QCOMPARE(view->selectedFrame(), frameId);
    QTest::keyClick(view, Qt::Key_Delete);
    QTest::qWait(50);
    QVERIFY(view->selectedFrame().isEmpty());
    QCOMPARE(view->selectedSlot(), slotId);

    // The undo returns the photograph to the selected cell: the photo becomes
    // the selection, so Delete acts on the photo rather than emptying the cell.
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QCOMPARE(view->selectedFrame(), frameId);
    QCOMPARE(view->selectedSlot(), slotId);
}

void TestSlotUi::missingStateAfterPreviewFinished()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString textPath = dir.filePath(QStringLiteral("not-a-photo.txt"));
    {
        QFile file(textPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is not an image");
    }

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({textPath}, 4),
                         QStringLiteral("missing.ppj")));
    QVERIFY(window.waitForPreview(15000));

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const Page page = window.document().pages.first();
    // The preview pass proves a source unreadable; until then it is loading, and
    // after the pass the cell says so instead of showing an empty download.
    QCOMPARE(view->slotState(page.slotList.at(0).id), PageView::SlotState::Missing);
    QCOMPARE(view->slotState(page.slotList.at(1).id), PageView::SlotState::Empty);
}

void TestSlotUi::fitPageStaysHiddenOnALaidOutPageThatOverflows()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("fit.png"), QColor(120, 140, 90));

    DocumentSnapshot overflowing = documentWithLayout({}, 4);
    QVERIFY(!overflowing.pages.isEmpty());
    Frame outside;
    outside.id = QStringLiteral("frame-outside");
    outside.sourcePath = photo;
    outside.sourceSha256 = fileSha256(photo);
    outside.cx = -40.0;
    outside.cy = -40.0;
    outside.width = 60.0;
    outside.height = 60.0;
    outside.autoPlaced = false;
    overflowing.pages[0].frames.append(outside);
    // An overflowing free frame without a layout is what shows the button.
    DocumentSnapshot freePage = overflowing;
    freePage.pages[0].slotList.clear();
    freePage.pages[0].slotStyle = SlotStyle{};
    freePage.pages[0].layoutId.clear();

    QTemporaryDir secondDir;
    QVERIFY(secondDir.isValid());

    {
        MainWindow window;
        window.resize(1280, 800);
        window.show();
        QTest::qWait(100);
        QVERIFY(loadDocument(window, dir, overflowing, QStringLiteral("fit-layout.ppj")));
        auto *fit = window.findChild<QPushButton *>(QStringLiteral("fitPageButton"));
        QVERIFY(fit);
        QVERIFY2(!fit->isVisible(), "Fit page is offered on a laid-out page");
        QVERIFY(!fit->isEnabled());
        const DocumentSnapshot before = window.document();
        QVERIFY(QMetaObject::invokeMethod(&window, "fitPage", Qt::DirectConnection));
        QTest::qWait(50);
        QVERIFY(window.document() == before);
    }
    {
        MainWindow window;
        window.resize(1280, 800);
        window.show();
        QTest::qWait(100);
        QVERIFY(loadDocument(window, secondDir, freePage, QStringLiteral("fit-free.ppj")));
        auto *fit = window.findChild<QPushButton *>(QStringLiteral("fitPageButton"));
        QVERIFY(fit);
        QVERIFY2(fit->isVisible(),
                 qPrintable(QStringLiteral("Fit page must be offered on a free canvas: frames %1 slots %2")
                                .arg(window.document().pages.first().frames.size())
                                .arg(window.document().pages.first().slotList.size())));
        QVERIFY(fit->isEnabled());
    }
}

void TestSlotUi::cropWheelAfterSelectionChangeIsSafe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photoA = makePhoto(dir.path(), QStringLiteral("ca.png"), QColor(190, 70, 70));
    const QString photoB = makePhoto(dir.path(), QStringLiteral("cb.png"), QColor(70, 70, 190));
    const DocumentSnapshot document = documentWithFreeFrames({photoA, photoB});
    QVERIFY(document.pages.first().frames.size() == 2);
    const QString idA = document.pages.first().frames.at(0).id;
    const QString idB = document.pages.first().frames.at(1).id;

    QImage pixels(600, 400, QImage::Format_RGB32);
    pixels.fill(QColor(140, 140, 140));

    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages({{idA, pixels}, {idB, pixels}});
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setSelectedFrame(idA);
    view.setCropMode(true);

    // Commits behave like the window: apply the frame and publish the new
    // document synchronously, which is what used to free the wheel target.
    DocumentSnapshot live = document;
    int commits = 0;
    QStringList order;
    view.onFrameCommitted = [&view, &live, &commits, &order](const Frame &frame) {
        ++commits;
        order.append(frame.id);
        for (Frame &stored : live.pages[0].frames) {
            if (stored.id == frame.id)
                stored = frame;
        }
        view.setDocument(live);
    };

    const QPointF centreA = pageToWidget(view, QPointF(document.pages[0].frames.at(0).cx,
                                                       document.pages[0].frames.at(0).cy));
    sendWheel(view, centreA, 120);
    QTest::qWait(30);
    QCOMPARE(commits, 0); // the burst is still open

    // A new selection flushes the burst synchronously and starts another.
    view.setSelectedFrame(idB);
    const QPointF centreB = pageToWidget(view, QPointF(document.pages[0].frames.at(1).cx,
                                                       document.pages[0].frames.at(1).cy));
    sendWheel(view, centreB, 120);
    QVERIFY(commits >= 1);
    QCOMPARE(order.first(), idA);

    QTest::qWait(kZoomBurstMs + 150);
    QCOMPARE(commits, 2);
    QCOMPARE(order.at(1), idB);
    for (const Frame &frame : live.pages[0].frames) {
        if (frame.id == idA || frame.id == idB)
            QVERIFY2(frame.zoom > 1.0, qPrintable(frame.id));
    }
}

void TestSlotUi::externalReplacementCancelsWheelBurst()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("ext.png"), QColor(90, 120, 190));
    const DocumentSnapshot document = documentWithLayout({photo}, 4);
    QVERIFY(!document.pages.isEmpty());
    const Page page = document.pages.first();
    const QString slotId = page.slotList.at(0).id;
    const QString frameId = frameInSlot(page, slotId)->id;

    QImage pixels(600, 400, QImage::Format_RGB32);
    pixels.fill(QColor(140, 140, 140));

    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages({{frameId, pixels}});
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    int commits = 0;
    view.onFrameCommitted = [&commits](const Frame &) { ++commits; };

    sendWheel(view, view.slotRect(slotId).center(), 120);
    QTest::qWait(30);

    // The document is replaced from outside (an undo, a project load) while the
    // burst is still open: its stale zoom must not be committed afterwards.
    DocumentSnapshot replaced = document;
    replaced.pages[0].frames[0].cx += 5.0;
    view.setDocument(replaced);
    QTest::qWait(kZoomBurstMs + 150);
    QCOMPARE(commits, 0);
}

void TestSlotUi::slottedFrameControlsAreDisabled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("spins.png"), QColor(80, 80, 160));
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4), QStringLiteral("spins.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(window.waitForPreview(15000));

    const Page page = window.document().pages.first();
    const QString slotId = page.slotList.at(0).id;
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(slotId).center().toPoint());
    QCOMPARE(view->selectedFrame(), frameInSlot(page, slotId)->id);

    auto *width = window.findChild<QDoubleSpinBox *>(QStringLiteral("widthSpin"));
    auto *height = window.findChild<QDoubleSpinBox *>(QStringLiteral("heightSpin"));
    auto *angle = window.findChild<QDoubleSpinBox *>(QStringLiteral("angleSpin"));
    QVERIFY(width);
    QVERIFY(height);
    QVERIFY(angle);
    QVERIFY(!width->isEnabled());
    QVERIFY(!height->isEnabled());
    QVERIFY(!angle->isEnabled());

    // "Igazítás a laphoz" is a no-op on a laid-out page: no edit, no undo step.
    const DocumentSnapshot before = window.document();
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    const QString undoText = undo->text();
    QVERIFY(QMetaObject::invokeMethod(&window, "fitPage", Qt::DirectConnection));
    QTest::qWait(50);
    QVERIFY(window.document() == before);
    QCOMPARE(undo->text(), undoText);

    // A free canvas keeps its controls: nothing above is a blanket disable.
    MainWindow freeWindow;
    freeWindow.resize(1280, 800);
    freeWindow.show();
    QTest::qWait(80);
    QVERIFY(QMetaObject::invokeMethod(&freeWindow, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, QStringList{photo})));
    QTest::qWait(100);
    auto *freeWidth =
        freeWindow.findChild<QDoubleSpinBox *>(QStringLiteral("widthSpin"));
    QVERIFY(freeWidth);
    QVERIFY(freeWidth->isEnabled());
}

void TestSlotUi::templateApplyKeepsSlotsOnALaidOutPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photoA = makePhoto(dir.path(), QStringLiteral("ta.png"), QColor(190, 90, 60));
    const QString photoB = makePhoto(dir.path(), QStringLiteral("tb.png"), QColor(60, 90, 190));
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photoA, photoB}, 4),
                         QStringLiteral("template.ppj")));
    const DocumentSnapshot before = window.document();
    QVERIFY(before.pages.first().hasLayout());

    bool applied = false;
    QVERIFY(QMetaObject::invokeMethod(&window, "applyTemplate", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, applied),
                                      Q_ARG(QString, QStringLiteral("n02-equal-side"))));
    QVERIFY(applied);

    const Page after = window.document().pages.first();
    // The picker still works on a laid-out page, and it does so through the
    // slot engine: the new cells own every photograph, nothing floats free.
    QVERIFY(after.hasLayout());
    QCOMPARE(after.layoutId, QStringLiteral("n02-equal-side"));
    QCOMPARE(after.slotList.size(), 2);
    QCOMPARE(after.frames.size(), 2);
    for (const Frame &frame : after.frames)
        QVERIFY(!frame.slotId.isEmpty());
    QVERIFY(SlotGeometry::layoutConsistent(after));

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
}

void TestSlotUi::deleteClearsSelectedSlot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("delete.png"), QColor(160, 60, 120));
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4), QStringLiteral("del.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(window.waitForPreview(15000));

    const Page page = window.document().pages.first();
    const QString filledSlot = page.slotList.at(0).id;
    const QString emptySlot = page.slotList.at(1).id;
    const DocumentSnapshot before = window.document();

    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(filledSlot).center().toPoint());
    QTest::keyClick(view, Qt::Key_Delete);
    QTest::qWait(50);
    Page after = window.document().pages.first();
    QVERIFY(!frameInSlot(after, filledSlot));
    QVERIFY(SlotGeometry::slotById(after, filledSlot));

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);

    // Delete on an empty cell makes no edit at all.
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(emptySlot).center().toPoint());
    QCOMPARE(view->selectedSlot(), emptySlot);
    undo = undoAction(window);
    QVERIFY(undo);
    const QString undoText = undo->text();
    const bool undoEnabled = undo->isEnabled();
    QTest::keyClick(view, Qt::Key_Delete);
    QTest::qWait(50);
    QVERIFY(window.document() == before);
    QCOMPARE(undo->text(), undoText);
    QCOMPARE(undo->isEnabled(), undoEnabled);
}

void TestSlotUi::invalidDropChangesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString notAPhoto = dir.filePath(QStringLiteral("notes.txt"));
    {
        QFile file(notAPhoto);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a photograph");
    }

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({}, 4), QStringLiteral("empty.ppj")));
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);

    const DocumentSnapshot before = window.document();
    const QString slotId = before.pages.first().slotList.at(0).id;
    dropFiles(*view, view->slotRect(slotId).center(), {notAPhoto});
    QTest::qWait(100);
    QVERIFY(window.document() == before);

    // Escape cancels the selection without touching the page.
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(slotId).center().toPoint());
    QCOMPARE(view->selectedSlot(), slotId);
    QTest::keyClick(view, Qt::Key_Escape);
    QVERIFY(view->selectedSlot().isEmpty());
    QVERIFY(window.document() == before);
}

void TestSlotUi::layoutPanelSurvivesAnEmptySelection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("panel.png"), QColor(120, 90, 60));
    QVERIFY(!photo.isEmpty());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4),
                         QStringLiteral("panel.ppj")));
    QVERIFY(window.waitForPreview(15000));

    auto *panel = window.findChild<QWidget *>(QStringLiteral("layoutPanel"));
    auto *photoPanel = window.findChild<QWidget *>(QStringLiteral("photoPanel"));
    QVERIFY(panel);
    QVERIFY(photoPanel);
    // Nothing is selected, and the page's own inspector is still there.
    QVERIFY2(panel->isVisible(), "the layout panel hid with no photograph selected");
    QVERIFY2(!photoPanel->isVisible(), "the photo inspector showed with nothing selected");

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const Page page = window.document().pages.first();
    const QString slotId = page.slotList.at(0).id;
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(slotId).center().toPoint());
    QCOMPARE(view->selectedFrame(), frameInSlot(page, slotId)->id);
    // One frame selected: both inspectors coexist.
    QVERIFY(panel->isVisible());
    QVERIFY(photoPanel->isVisible());

    QTest::keyClick(view, Qt::Key_Escape);
    QVERIFY(view->selectedFrame().isEmpty());
    QVERIFY(panel->isVisible());
    QVERIFY(!photoPanel->isVisible());

    // A free canvas has no layout, so it has no layout panel.
    QTemporaryDir freeDir;
    QVERIFY(freeDir.isValid());
    MainWindow freeWindow;
    freeWindow.resize(1280, 800);
    freeWindow.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(freeWindow, freeDir, documentWithFreeFrames({photo}),
                         QStringLiteral("free.ppj")));
    auto *freePanel = freeWindow.findChild<QWidget *>(QStringLiteral("layoutPanel"));
    QVERIFY(freePanel);
    QVERIFY2(!freePanel->isVisible(), "a free canvas showed a layout panel");
}

void TestSlotUi::layoutNameIsTheEnglishCatalogueName()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    QVERIFY(!catalogue.catalogue().isEmpty());
    const CollageTemplate &chosen = catalogue.catalogue().first();
    QVERIFY(!chosen.nameEn.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DocumentSnapshot document = documentWithLayout({}, 4);
    QVERIFY(!document.pages.isEmpty());
    document.pages[0].layoutId = chosen.id;

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, document, QStringLiteral("name.ppj")));

    auto *name = window.findChild<QLabel *>(QStringLiteral("layoutNameLabel"));
    auto *count = window.findChild<QLabel *>(QStringLiteral("layoutCountLabel"));
    QVERIFY(name);
    QVERIFY(count);
    QCOMPARE(name->text(), chosen.nameEn);
    QCOMPARE(count->text(), QStringLiteral("4 hely"));

    // A hand-made layout has no catalogue name, so the panel says so instead of
    // showing an empty line or an internal id.
    QTemporaryDir handmadeDir;
    QVERIFY(handmadeDir.isValid());
    DocumentSnapshot handmade = documentWithLayout({}, 3);
    handmade.pages[0].layoutId.clear();
    MainWindow handmadeWindow;
    handmadeWindow.resize(1280, 800);
    handmadeWindow.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(handmadeWindow, handmadeDir, handmade,
                         QStringLiteral("handmade.ppj")));
    auto *handmadeName = handmadeWindow.findChild<QLabel *>(QStringLiteral("layoutNameLabel"));
    auto *handmadeCount = handmadeWindow.findChild<QLabel *>(QStringLiteral("layoutCountLabel"));
    QVERIFY(handmadeName);
    QVERIFY(handmadeCount);
    QCOMPARE(handmadeName->text(), QStringLiteral("Saját elrendezés"));
    QCOMPARE(handmadeCount->text(), QStringLiteral("3 hely"));
}

void TestSlotUi::marginAndGutterAreAutomaticOrZero()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("margin.png"), QColor(60, 120, 90));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4),
                         QStringLiteral("margin.ppj")));
    QVERIFY(window.waitForPreview(15000));

    auto *marginSpin = spinNamed(window, "marginSpin");
    auto *marginAuto = window.findChild<QCheckBox *>(QStringLiteral("marginAutoCheck"));
    auto *gutterSpin = spinNamed(window, "gutterSpin");
    auto *gutterAuto = window.findChild<QCheckBox *>(QStringLiteral("gutterAutoCheck"));
    QVERIFY(marginSpin);
    QVERIFY(marginAuto);
    QVERIFY(gutterSpin);
    QVERIFY(gutterAuto);

    // Automatic is the default, the spin is read-only and it shows the value the
    // page actually uses: 5 % of A4's short side, capped at 10 mm.
    QVERIFY(marginAuto->isChecked());
    QVERIFY(gutterAuto->isChecked());
    QVERIFY(!marginSpin->isEnabled());
    QVERIFY(!gutterSpin->isEnabled());
    QCOMPARE(marginSpin->value(), 10.0);
    Page page = window.document().pages.first();
    QVERIFY(!page.slotStyle.marginMm.has_value());
    QVERIFY(!page.slotStyle.gutterMm.has_value());
    const QRectF automaticCell = SlotGeometry::slotCellMm(page, page.slotList.at(0));
    QCOMPARE(automaticCell.left(), 10.0);

    // Turning the switch off pins the value that was showing, as one edit.
    marginAuto->setChecked(false);
    QTest::qWait(50);
    QVERIFY(marginSpin->isEnabled());
    page = window.document().pages.first();
    QVERIFY(page.slotStyle.marginMm.has_value());
    QCOMPARE(*page.slotStyle.marginMm, 10.0);
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY2(undoLabelIs(window, QStringLiteral("Margó beállítása")),
             qPrintable(undo->text()));

    // 0 mm is a real margin, not "automatic" again: the cells reach the edge.
    marginSpin->setValue(0.0);
    commitSpin(marginSpin);
    QTest::qWait(50);
    page = window.document().pages.first();
    QVERIFY(page.slotStyle.marginMm.has_value());
    QCOMPARE(*page.slotStyle.marginMm, 0.0);
    QCOMPARE(SlotGeometry::slotCellMm(page, page.slotList.at(0)).left(), 0.0);

    // The switch brings the automatic margin back, in one step.
    marginAuto->setChecked(true);
    QTest::qWait(50);
    page = window.document().pages.first();
    QVERIFY(!page.slotStyle.marginMm.has_value());
    QCOMPARE(SlotGeometry::slotCellMm(page, page.slotList.at(0)).left(), 10.0);
    undo->trigger();
    QTest::qWait(50);
    page = window.document().pages.first();
    QVERIFY(page.slotStyle.marginMm.has_value());
    QCOMPARE(*page.slotStyle.marginMm, 0.0);

    // The gutter has its own switch and its own value.
    gutterAuto->setChecked(false);
    QTest::qWait(50);
    page = window.document().pages.first();
    QVERIFY(page.slotStyle.gutterMm.has_value());
    gutterSpin->setValue(2.5);
    commitSpin(gutterSpin);
    QTest::qWait(50);
    page = window.document().pages.first();
    QVERIFY(page.slotStyle.gutterMm.has_value());
    QCOMPARE(*page.slotStyle.gutterMm, 2.5);
    QVERIFY(SlotGeometry::layoutConsistent(page));
}

void TestSlotUi::styleValuesClampOnSmallPaper()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("clamp.png"), QColor(160, 100, 40));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4, tenByFifteenPaper()),
                         QStringLiteral("clamp.ppj")));
    QVERIFY(window.waitForPreview(15000));

    auto *marginSpin = spinNamed(window, "marginSpin");
    auto *borderSpin = spinNamed(window, "borderSpin");
    auto *radiusSpin = spinNamed(window, "radiusSpin");
    auto *marginAuto = window.findChild<QCheckBox *>(QStringLiteral("marginAutoCheck"));
    QVERIFY(marginSpin);
    QVERIFY(borderSpin);
    QVERIFY(radiusSpin);
    QVERIFY(marginAuto);

    Page page = window.document().pages.first();
    // A margin may never eat the content box: half the short side minus 2 mm.
    QCOMPARE(marginSpin->maximum(), 48.0);

    // Asking for more than the paper allows stores the clamped value, and the
    // control shows that value instead of the number that was typed.
    marginAuto->setChecked(false);
    QTest::qWait(30);
    marginSpin->setValue(50.0);
    commitSpin(marginSpin);
    QTest::qWait(30);
    page = window.document().pages.first();
    QVERIFY(page.slotStyle.marginMm.has_value());
    QCOMPARE(*page.slotStyle.marginMm, 48.0);
    QCOMPARE(marginSpin->value(), 48.0);
    // Back to automatic, so the cells are the full size again for the two
    // per-cell clamps below.
    marginAuto->setChecked(true);
    QTest::qWait(30);
    page = window.document().pages.first();
    QVERIFY(!page.slotStyle.marginMm.has_value());

    const QRectF cell = SlotGeometry::slotCellMm(page, page.slotList.at(0));
    const double shortestCell = std::min(cell.width(), cell.height());
    QVERIFY(shortestCell > 0.0);
    // A border may never eat more than 45 % of a cell's shorter side, and a
    // radius never more than half a cell. The spin is a tenth of a millimetre
    // control, so the limit it shows is the clamp rounded to its own step.
    QVERIFY(borderSpin->maximum() <= 0.45 * shortestCell + 1e-9);
    QVERIFY(borderSpin->maximum() > 0.45 * shortestCell - 0.1);
    QVERIFY(radiusSpin->maximum() <= shortestCell / 2.0 + 0.1);
    QVERIFY(radiusSpin->maximum() > shortestCell / 2.0 - 0.1);

    borderSpin->setValue(50.0);
    commitSpin(borderSpin);
    QTest::qWait(30);
    radiusSpin->setValue(100.0);
    commitSpin(radiusSpin);
    QTest::qWait(60);

    page = window.document().pages.first();
    // What was stored is exactly what the control shows.
    QCOMPARE(page.slotStyle.borderMm, borderSpin->maximum());
    QCOMPARE(page.slotStyle.cornerRadiusMm, radiusSpin->maximum());
    QCOMPARE(borderSpin->value(), borderSpin->maximum());
    QCOMPARE(radiusSpin->value(), radiusSpin->maximum());
    QVERIFY(SlotGeometry::layoutConsistent(page));
    // A photograph keeps at least a tenth of its cell, and every cell survives.
    for (const Slot &slot : page.slotList) {
        const QRectF slotCell = SlotGeometry::slotCellMm(page, slot);
        const QRectF photoRect = SlotGeometry::slotPhotoRectMm(page, slot);
        QVERIFY(slotCell.width() >= 1.0);
        QVERIFY(slotCell.height() >= 1.0);
        QVERIFY(photoRect.width() >= slotCell.width() / 10.0 - 1e-9);
        QVERIFY(photoRect.height() >= slotCell.height() / 10.0 - 1e-9);
    }
}

void TestSlotUi::borderColourWithAlphaRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("colour.png"), QColor(40, 90, 150));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4),
                         QStringLiteral("colour.ppj")));

    // The colour dialog's answer arrives through one function, so the panel has
    // one writer for the colour and the test needs no modal dialog.
    const QColor chosen(20, 40, 60, 128);
    QVERIFY(QMetaObject::invokeMethod(&window, "applySlotBorderColour",
                                      Qt::DirectConnection, Q_ARG(QColor, chosen)));
    QTest::qWait(50);
    Page page = window.document().pages.first();
    QCOMPARE(page.slotStyle.borderColor, chosen);
    QCOMPARE(page.slotStyle.borderColor.alpha(), 128);
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY2(undoLabelIs(window, QStringLiteral("Szegély beállítása")),
             qPrintable(undo->text()));

    // Alpha survives the project file: a translucent mat is a saved choice.
    const QString path = dir.filePath(QStringLiteral("colour-saved.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, window.document(), SaveMode::Normal).ok());
    const auto loaded = store.load(path);
    QVERIFY(loaded);
    QCOMPARE(loaded.value().document.pages.first().slotStyle.borderColor, chosen);
    QCOMPARE(loaded.value().document.pages.first().slotStyle.borderColor.alpha(), 128);

    undo->trigger();
    QTest::qWait(50);
    page = window.document().pages.first();
    QCOMPARE(page.slotStyle.borderColor, QColor(255, 255, 255));
}

void TestSlotUi::oneUndoPerStyleChange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("undo.png"), QColor(90, 60, 140));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4),
                         QStringLiteral("style-undo.ppj")));

    auto *borderSpin = spinNamed(window, "borderSpin");
    QVERIFY(borderSpin);
    QAction *undo = undoAction(window);
    QVERIFY(undo);

    borderSpin->setValue(3.0);
    commitSpin(borderSpin);
    QTest::qWait(50);
    const DocumentSnapshot afterThree = window.document();
    QCOMPARE(afterThree.pages.first().slotStyle.borderMm, 3.0);
    QVERIFY2(undoLabelIs(window, QStringLiteral("Szegély beállítása")),
             qPrintable(undo->text()));

    borderSpin->setValue(6.0);
    commitSpin(borderSpin);
    QTest::qWait(50);
    QCOMPARE(window.document().pages.first().slotStyle.borderMm, 6.0);

    // One step back is the 3 mm state, not the 0 mm one: one action, one entry.
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == afterThree);

    // A focus loss on an untouched spin is not an edit: no new entry.
    const QString text = undo->text();
    commitSpin(borderSpin);
    QTest::qWait(30);
    QVERIFY(window.document() == afterThree);
    QCOMPARE(undo->text(), text);

    // Radius and colour are their own single steps as well.
    auto *radiusSpin = spinNamed(window, "radiusSpin");
    QVERIFY(radiusSpin);
    radiusSpin->setValue(4.0);
    commitSpin(radiusSpin);
    QTest::qWait(50);
    const DocumentSnapshot afterRadius = window.document();
    QCOMPARE(afterRadius.pages.first().slotStyle.cornerRadiusMm, 4.0);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == afterThree);

    QVERIFY(QMetaObject::invokeMethod(&window, "applySlotBorderColour", Qt::DirectConnection,
                                      Q_ARG(QColor, QColor(0, 0, 0))));
    QTest::qWait(50);
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == afterThree);
}

void TestSlotUi::addSlotSplitsAndRefusesAtTheCap()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photoA = makePhoto(dir.path(), QStringLiteral("add-a.png"), QColor(200, 80, 80));
    const QString photoB = makePhoto(dir.path(), QStringLiteral("add-b.png"), QColor(80, 80, 200));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photoA, photoB}, 2),
                         QStringLiteral("add.ppj")));
    QVERIFY(window.waitForPreview(15000));

    auto *view = window.findChild<PageView *>();
    auto *addButton = window.findChild<QPushButton *>(QStringLiteral("addSlotButton"));
    auto *removeButton = window.findChild<QPushButton *>(QStringLiteral("removeSlotButton"));
    QVERIFY(view);
    QVERIFY(addButton);
    QVERIFY(removeButton);
    QVERIFY(addButton->isEnabled());
    QVERIFY2(!removeButton->isEnabled(), "removing a cell needs a selected cell");

    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    QCOMPARE(page.slotList.size(), 2);
    const QString splitId = page.slotList.at(0).id;
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(splitId).center().toPoint());
    QCOMPARE(view->selectedSlot(), splitId);
    QVERIFY(removeButton->isEnabled());

    QVERIFY(QMetaObject::invokeMethod(&window, "addSlotToPage", Qt::DirectConnection));
    QTest::qWait(50);
    const Page added = window.document().pages.first();
    QCOMPARE(added.slotList.size(), 3);
    QVERIFY(SlotGeometry::layoutConsistent(added));
    // The new cell follows the split one in fill order, so the order the panel
    // fills and lists is still the order on the paper.
    QCOMPARE(added.slotList.at(0).id, splitId);
    QCOMPARE(added.slotList.at(2).id, page.slotList.at(1).id);
    QVERIFY(added.slotList.at(1).id != splitId);
    QVERIFY(added.slotList.at(1).id != page.slotList.at(1).id);
    // Both halves are still big enough to read a photograph in.
    const QSizeF sheet = pageSizeMm(added);
    const double margin = SlotGeometry::effectiveMarginMm(added.slotStyle, sheet);
    const double gutter = SlotGeometry::effectiveGutterMm(added.slotStyle, sheet);
    for (const Slot &slot : added.slotList) {
        const QRectF splitCell =
            SlotGeometry::cellFromUnitRect(slot.unitRect, sheet, margin, gutter);
        QVERIFY(splitCell.width() >= 15.0 - 1e-6);
        QVERIFY(splitCell.height() >= 15.0 - 1e-6);
    }
    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY2(undoLabelIs(window, QStringLiteral("Hely hozzáadása")),
             qPrintable(undo->text()));
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);

    // The 15 mm floor is refused on a sheet where a half cell would be smaller.
    QTemporaryDir tinyDir;
    QVERIFY(tinyDir.isValid());
    MainWindow tinyWindow;
    tinyWindow.resize(1280, 800);
    tinyWindow.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(tinyWindow, tinyDir, documentWithLayout({}, 4, tinyPaper()),
                         QStringLiteral("tiny.ppj")));
    const DocumentSnapshot tinyBefore = tinyWindow.document();
    QVERIFY(QMetaObject::invokeMethod(&tinyWindow, "addSlotToPage", Qt::DirectConnection));
    QTest::qWait(30);
    QVERIFY2(tinyWindow.document() == tinyBefore, "a 15 mm floor was crossed");
    QVERIFY2(tinyWindow.statusBar()->currentMessage().contains(QStringLiteral("túl kicsi")),
             qPrintable(tinyWindow.statusBar()->currentMessage()));

    // The 12-cell cap refuses the thirteenth and says why.
    QTemporaryDir capDir;
    QVERIFY(capDir.isValid());
    MainWindow capWindow;
    capWindow.resize(1280, 800);
    capWindow.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(capWindow, capDir, documentWithLayout({}, 12),
                         QStringLiteral("cap.ppj")));
    auto *capAdd = capWindow.findChild<QPushButton *>(QStringLiteral("addSlotButton"));
    QVERIFY(capAdd);
    QVERIFY2(!capAdd->isEnabled(), "the thirteenth cell was offered");
    const DocumentSnapshot capBefore = capWindow.document();
    QVERIFY(QMetaObject::invokeMethod(&capWindow, "addSlotToPage", Qt::DirectConnection));
    QTest::qWait(30);
    QVERIFY(capWindow.document() == capBefore);
    QCOMPARE(capWindow.statusBar()->currentMessage(),
             QStringLiteral("Egy lapon legfeljebb 12 hely lehet."));
}

void TestSlotUi::removeSlotLeavesTheGap()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("remove.png"), QColor(70, 160, 120));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photo}, 4),
                         QStringLiteral("remove.ppj")));
    QVERIFY(window.waitForPreview(15000));

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    const QString removedId = page.slotList.at(0).id;
    QVERIFY(frameInSlot(page, removedId));
    QHash<QString, QRectF> cellsBefore;
    const QList<QRectF> mapped = SlotGeometry::cellsMm(page);
    for (int i = 0; i < page.slotList.size(); ++i)
        cellsBefore.insert(page.slotList.at(i).id, mapped.at(i));

    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(removedId).center().toPoint());
    QCOMPARE(view->selectedSlot(), removedId);
    QVERIFY(QMetaObject::invokeMethod(&window, "removeSlotFromPage", Qt::DirectConnection));
    QTest::qWait(50);

    Page after = window.document().pages.first();
    QCOMPARE(after.slotList.size(), 3);
    QVERIFY(!SlotGeometry::slotById(after, removedId));
    // The photograph went with its cell.
    QCOMPARE(after.frames.size(), 0);
    // Every other cell kept its rectangle: the neighbours did not slide
    // together, so the space the removed cell leaves stays empty, and the panel
    // says so.
    QCOMPARE(window.statusBar()->currentMessage(),
             QStringLiteral("A többi hely a helyén marad."));
    const QList<QRectF> cellsAfter = SlotGeometry::cellsMm(after);
    QCOMPARE(cellsAfter.size(), 3);
    for (int i = 0; i < after.slotList.size(); ++i) {
        const auto was = cellsBefore.constFind(after.slotList.at(i).id);
        QVERIFY(was != cellsBefore.constEnd());
        QCOMPARE(cellsAfter.at(i).left(), was->left());
        QCOMPARE(cellsAfter.at(i).top(), was->top());
        QCOMPARE(cellsAfter.at(i).width(), was->width());
        QCOMPARE(cellsAfter.at(i).height(), was->height());
    }
    QVERIFY(SlotGeometry::layoutConsistent(after));

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY2(undoLabelIs(window, QStringLiteral("Hely eltávolítása")),
             qPrintable(undo->text()));
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
}

void TestSlotUi::dissolveKeepsThePhotographs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photoA = makePhoto(dir.path(), QStringLiteral("dis-a.png"), QColor(180, 120, 60));
    const QString photoB = makePhoto(dir.path(), QStringLiteral("dis-b.png"), QColor(60, 120, 180));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(loadDocument(window, dir, documentWithLayout({photoA, photoB}, 4),
                         QStringLiteral("dissolve.ppj")));
    QVERIFY(window.waitForPreview(15000));

    auto *panel = window.findChild<QWidget *>(QStringLiteral("layoutPanel"));
    QVERIFY(panel);
    QVERIFY(panel->isVisible());
    const DocumentSnapshot before = window.document();
    const Page page = before.pages.first();
    QCOMPARE(page.frames.size(), 2);

    // "No" is a decision too: nothing changes.
    answerNextQuestionWith(QMessageBox::No);
    QVERIFY(QMetaObject::invokeMethod(&window, "dissolvePageLayout", Qt::DirectConnection));
    QTest::qWait(30);
    QVERIFY(window.document() == before);

    // "Yes" removes the cells and keeps the photographs where they are.
    answerNextQuestionWith(QMessageBox::Yes);
    QVERIFY(QMetaObject::invokeMethod(&window, "dissolvePageLayout", Qt::DirectConnection));
    QTest::qWait(50);

    const Page dissolved = window.document().pages.first();
    QVERIFY(!dissolved.hasLayout());
    QVERIFY(dissolved.slotList.isEmpty());
    QVERIFY(dissolved.layoutId.isEmpty());
    QCOMPARE(dissolved.frames.size(), page.frames.size());
    for (int i = 0; i < dissolved.frames.size(); ++i) {
        const Frame &was = page.frames.at(i);
        const Frame &now = dissolved.frames.at(i);
        QCOMPARE(now.id, was.id);
        QVERIFY(now.slotId.isEmpty());
        QCOMPARE(now.cx, was.cx);
        QCOMPARE(now.cy, was.cy);
        QCOMPARE(now.width, was.width);
        QCOMPARE(now.height, was.height);
        QCOMPARE(now.angleDeg, 0.0);
    }
    // The panel followed the page: no layout, no layout panel.
    QVERIFY(!panel->isVisible());

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY2(undoLabelIs(window, QStringLiteral("Elrendezés megszüntetése")),
             qPrintable(undo->text()));
    undo->trigger();
    QTest::qWait(50);
    QVERIFY(window.document() == before);
    QVERIFY(panel->isVisible());
}

void TestSlotUi::borderPrintsAndTheCanvasAgrees()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = makePhoto(dir.path(), QStringLiteral("mat.png"), QColor(128, 128, 128));

    DocumentSnapshot document = documentWithLayout({photo}, 4);
    QVERIFY(!document.pages.isEmpty());
    const Frame *firstFrame =
        frameInSlot(document.pages.first(), document.pages.first().slotList.at(0).id);
    QVERIFY(firstFrame);
    const QString frameId = firstFrame->id;

    // A 3 mm black mat, chosen through the engine by the same page the panel
    // builds, so this test measures the style and not the panel.
    SlotStyle style;
    style.borderMm = 3.0;
    style.borderColor = QColor(0, 0, 0);
    auto styled = SlotLayoutEngine::setStyle(document.pages.first(), style);
    QVERIFY(styled);
    document.pages[0] = styled.value();
    const Page page = document.pages.first();
    const Slot &filled = page.slotList.at(0);
    const Slot &empty = page.slotList.at(1);
    const QRectF filledCell = SlotGeometry::slotCellMm(page, filled);
    const QRectF filledPhoto = SlotGeometry::slotPhotoRectMm(page, filled);
    const QRectF emptyCell = SlotGeometry::slotCellMm(page, empty);
    // 1.5 mm inside a cell edge is inside the mat and outside the photograph.
    const QPointF matPoint(filledCell.left() + 1.5, filledCell.center().y());
    const QPointF photoPoint = filledPhoto.center();
    const QPointF emptyMatPoint(emptyCell.left() + 1.5, emptyCell.center().y());
    QVERIFY(filledPhoto.width() < filledCell.width());
    QVERIFY(filledCell.contains(matPoint));

    // The print path.
    ImageStore store;
    PageRenderer renderer(store);
    const double ppi = 150.0;
    const auto rendered = renderer.renderPage(document, 0, ppi);
    if (!rendered)
        QFAIL(qPrintable(rendered.error().message));
    const QImage printed = rendered.value();
    QVERIFY(!printed.isNull());
    const QSizeF sheet = pageSizeMm(page);
    const auto printedAt = [&](const QPointF &mm) {
        const int x = std::clamp(int(mm.x() / sheet.width() * printed.width()), 0,
                                 printed.width() - 1);
        const int y = std::clamp(int(mm.y() / sheet.height() * printed.height()), 0,
                                 printed.height() - 1);
        return printed.pixel(x, y);
    };
    const auto isDark = [](QRgb pixel) {
        return qRed(pixel) <= 30 && qGreen(pixel) <= 30 && qBlue(pixel) <= 30;
    };
    const auto isGrey = [](QRgb pixel) {
        return qRed(pixel) >= 95 && qRed(pixel) <= 165 && qGreen(pixel) >= 95
            && qGreen(pixel) <= 165 && qBlue(pixel) >= 95 && qBlue(pixel) <= 165;
    };

    QVERIFY2(isDark(printedAt(matPoint)),
             qPrintable(QStringLiteral("the 3 mm mat printed %1/%2/%3")
                            .arg(qRed(printedAt(matPoint)))
                            .arg(qGreen(printedAt(matPoint)))
                            .arg(qBlue(printedAt(matPoint)))));
    QVERIFY2(isGrey(printedAt(photoPoint)), "the photograph must print inside its mat");
    // An empty cell prints its mat and nothing else: the ring is there, the
    // middle is paper, and no placeholder reaches the raster.
    QVERIFY2(isDark(printedAt(emptyMatPoint)), "an empty cell must still print its mat");
    const QRgb emptyCentre = printedAt(emptyCell.center());
    QVERIFY2(qRed(emptyCentre) >= 245 && qGreen(emptyCentre) >= 245 && qBlue(emptyCentre) >= 245,
             qPrintable(QStringLiteral("an empty cell printed %1/%2/%3 in its middle")
                            .arg(qRed(emptyCentre))
                            .arg(qGreen(emptyCentre))
                            .arg(qBlue(emptyCentre))));

    // The canvas raster is made by the same painter at the same millimetres, so
    // what the screen shows and what the printer gets cannot drift apart.
    const QImage pixels = preparedPixels(photo);
    QVERIFY(!pixels.isNull());
    PageView view;
    view.resize(900, 700);
    view.setDocument(document);
    view.setFrameImages({{frameId, pixels}});
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    const QImage canvas = view.composedImage();
    QVERIFY(!canvas.isNull());
    const auto canvasAt = [&](const QPointF &mm) {
        return rasterPixelAtMm(canvas, view.pageMm(), mm);
    };
    for (const QPointF &probe : {matPoint, photoPoint, emptyMatPoint, emptyCell.center()}) {
        const QRgb onScreen = canvasAt(probe);
        const QRgb ink = printedAt(probe);
        const int distance = std::max({std::abs(qRed(onScreen) - qRed(ink)),
                                       std::abs(qGreen(onScreen) - qGreen(ink)),
                                       std::abs(qBlue(onScreen) - qBlue(ink))});
        QVERIFY2(distance <= 4,
                 qPrintable(QStringLiteral("screen %1/%2/%3 and print %4/%5/%6 differ at %7,%8 by %9")
                                .arg(qRed(onScreen))
                                .arg(qGreen(onScreen))
                                .arg(qBlue(onScreen))
                                .arg(qRed(ink))
                                .arg(qGreen(ink))
                                .arg(qBlue(ink))
                                .arg(probe.x())
                                .arg(probe.y())
                                .arg(distance)));
    }
}

QTEST_MAIN(TestSlotUi)
#include "tst_slot_ui.moc"