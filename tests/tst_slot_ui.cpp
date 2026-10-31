#include "Document.h"
#include "ImageSource.h"
#include "MainWindow.h"
#include "ProjectStore.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QTemporaryDir>
#include <QUrl>
#include <QWheelEvent>
#include <QtTest>

#include <algorithm>
#include <cmath>

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
DocumentSnapshot documentWithLayout(const QStringList &photos, int slotCount)
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-test");
    page.paper = a4Paper();
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
    void slottedFrameControlsAreDisabled();
    void templateApplyKeepsSlotsOnALaidOutPage();
    void deleteClearsSelectedSlot();
    void invalidDropChangesNothing();
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

QTEST_MAIN(TestSlotUi)
#include "tst_slot_ui.moc"
