#include "AboutDialog.h"
#include "Document.h"
#include "EffectBrowserDialog.h"
#include "MainWindow.h"
#include "ProjectStore.h"
#include "TemplateBrowserDialog.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QImageWriter>
#include <QAbstractItemView>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QtTest>

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

/// A page with four empty cells in a 2 x 2 grid. The layout reaches the window
/// through the normal project path, the same way a saved composition does.
DocumentSnapshot fourEmptySlots()
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-slots");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    page.slotList = {
        Slot{QStringLiteral("slot-evidence-0"), QRectF(0.0, 0.0, 0.5, 0.5)},
        Slot{QStringLiteral("slot-evidence-1"), QRectF(0.5, 0.0, 0.5, 0.5)},
        Slot{QStringLiteral("slot-evidence-2"), QRectF(0.0, 0.5, 0.5, 0.5)},
        Slot{QStringLiteral("slot-evidence-3"), QRectF(0.5, 0.5, 0.5, 0.5)},
    };
    page.layoutId = QStringLiteral("n04-grid-2x2");
    document.pages.append(page);
    document.currentPage = 0;
    return document;
}

bool loadDocument(MainWindow &window, const QTemporaryDir &dir, const DocumentSnapshot &document)
{
    const QString path = dir.filePath(QStringLiteral("slots.ppj"));
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

class TestUiScreens : public QObject
{
    Q_OBJECT

private slots:
    void captureStates();
    void captureSlotInteraction();
};

void TestUiScreens::captureStates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = {
        QStringLiteral("/home/lavx/Downloads/IMG20260830150231.jpg"),
        makePhoto(dir.path(), QStringLiteral("landscape.png"), QColor(70, 130, 170), QSize(900, 600)),
        makePhoto(dir.path(), QStringLiteral("square.png"), QColor(160, 110, 70), QSize(700, 700)),
    };
    for (const QString &photo : photos)
        QVERIFY2(QFileInfo::exists(photo), qPrintable(photo));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(150);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-empty.png")));

    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QTest::qWait(450);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-three-photos.png")));

    // Evidence for the collage template picker. Three photos on the page, so
    // the count-3 layouts are the applicable ones; a chosen layout shows the
    // check and what applying it changes.
    auto *templatesButton = window.findChild<QPushButton *>(QStringLiteral("templatesButton"));
    QVERIFY(templatesButton);
    templatesButton->click();
    auto *picker = window.findChild<TemplateBrowserDialog *>();
    QVERIFY(picker);
    QVERIFY(picker->isVisible());
    picker->selectTemplate(picker->templateIds().first());
    QTest::qWait(150);
    QVERIFY(picker->grab().save(evidenceDir() + QStringLiteral("/ui-template-picker.png")));

    // More photos than the chosen count: the picker explains what to do and
    // offers no add button.
    picker->selectCount(2);
    QTest::qWait(120);
    QVERIFY(picker->grab().save(evidenceDir() + QStringLiteral("/ui-template-picker-2.png")));

    // Fewer photos than the chosen count: the one direction a button can fix.
    picker->selectCount(5);
    QTest::qWait(120);
    QVERIFY(picker->grab().save(
        evidenceDir() + QStringLiteral("/ui-template-picker-add-photos.png")));
    picker->close();
    QTest::qWait(120);

    // Evidence for the effect browser. The first grab has no photo selected,
    // so it is the bundled CC0 sample rather than a flat test colour.
    EffectBrowserRequest sampleRequest;
    EffectBrowserDialog sampleBrowser(sampleRequest, &window);
    sampleBrowser.show();
    QVERIFY(sampleBrowser.waitForRendering(15000));
    QTest::qWait(120);
    QVERIFY(sampleBrowser.grab().save(
        evidenceDir() + QStringLiteral("/ui-effect-browser-sample.png")));

    // The Nyomat category carries the slow marker on Engraving and the
    // approximate hint on Halftone, both in one frame.
    auto *categories =
        sampleBrowser.findChild<QListWidget *>(QStringLiteral("categoryList"));
    QVERIFY(categories);
    for (int row = 0; row < categories->count(); ++row) {
        if (categories->item(row)->data(Qt::UserRole).toString() == QStringLiteral("print"))
            categories->setCurrentRow(row);
    }
    QTest::qWait(30);
    QVERIFY(sampleBrowser.waitForRendering(15000));
    auto *halftone = sampleBrowser.findChild<QAbstractButton *>(
        QStringLiteral("effectTile_print.halftone"));
    QVERIFY(halftone);
    QTest::mouseClick(halftone, Qt::LeftButton, Qt::NoModifier, halftone->rect().center());
    QVERIFY(sampleBrowser.waitForSelectedRender(15000));
    QTest::qWait(120);
    QVERIFY(sampleBrowser.grab().save(
        evidenceDir() + QStringLiteral("/ui-effect-browser-print.png")));
    sampleBrowser.close();
    QTest::qWait(120);

    if (auto *scroll = window.findChild<QScrollArea *>()) {
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QTest::qWait(120);
    }
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-controls-bottom.png")));

    if (auto *scroll = window.findChild<QScrollArea *>())
        scroll->verticalScrollBar()->setValue(0);
    QComboBox *paper = nullptr;
    QComboBox *orientation = nullptr;
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        if (combo->count() > 0 && combo->itemText(0).startsWith(QStringLiteral("A4")))
            paper = combo;
        if (combo->count() == 2 && combo->itemText(0) == QStringLiteral("Álló"))
            orientation = combo;
    }
    QVERIFY(paper);
    QVERIFY(orientation);
    for (int i = 0; i < paper->count(); ++i) {
        if (paper->itemText(i).startsWith(QStringLiteral("10 × 15 cm")))
            paper->setCurrentIndex(i);
    }
    orientation->setCurrentIndex(1);
    QTest::qWait(350);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-photo-paper.png")));

    if (paper) {
        paper->showPopup();
        QTest::qWait(120);
        if (QWidget *popup = paper->view() ? paper->view()->window() : nullptr)
            popup->grab().save(evidenceDir() + QStringLiteral("/ui-paper-popup.png"));
        paper->hidePopup();
    }

    AboutDialog about(&window);
    about.show();
    QTest::qWait(120);
    QVERIFY(about.grab().save(evidenceDir() + QStringLiteral("/ui-dialog.png")));
    about.close();

    for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Új lap")) {
            button->click();
            break;
        }
    }
    QTest::qWait(200);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-pages.png")));

    window.resize(640, 360);
    QTest::qWait(250);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-small.png")));
}

void TestUiScreens::captureSlotInteraction()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("slot-photo.png"), QColor(75, 130, 175), QSize(900, 600));
    QVERIFY(!photo.isEmpty());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(loadDocument(window, dir, fourEmptySlots()));
    QTest::qWait(300);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const Page page = window.document().pages.first();
    QVERIFY(page.hasLayout());
    QCOMPARE(page.slotList.size(), 4);

    // Empty cells: the dashed drop targets and the one hint line.
    QVERIFY(view->slotState(page.slotList.at(0).id) == PageView::SlotState::Empty);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-slots-empty.png")));

    // One selected cell: a solid ring around the cell, no photograph.
    const QString selected = page.slotList.at(1).id;
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(selected).center().toPoint());
    QCOMPARE(view->selectedSlot(), selected);
    QTest::qWait(150);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-slot-selected.png")));

    // One filled cell next to the empty ones.
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, QStringList{photo})));
    QVERIFY(window.waitForPreview(15000));
    QTest::qWait(350);
    QVERIFY(view->slotState(page.slotList.at(0).id) == PageView::SlotState::Filled);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-slot-filled.png")));
}

QTEST_MAIN(TestUiScreens)
#include "tst_ui_screens.moc"
