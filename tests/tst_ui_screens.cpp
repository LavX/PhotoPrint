#include "AboutDialog.h"
#include "Document.h"
#include "EffectBrowserDialog.h"
#include "MainWindow.h"
#include "ProjectStore.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"
#include "TemplateBrowserDialog.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QImage>
#include <QImageWriter>
#include <QAbstractItemView>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSettings>
#include <QTimer>
#include <QtTest>

#include <memory>

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

/// Answers the next modal question with `button`. A modal dialog runs its own
/// event loop, so a timer is what reaches into it.
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

} // namespace

class TestUiScreens : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void captureStates();
    void captureSlotInteraction();
    void captureLayoutPanel();
    void captureSectionComposition();

private:
    QTemporaryDir m_settingsDir;
};

void TestUiScreens::initTestCase()
{
    // A language switch is saved and the window keeps a recovery checkpoint: both
    // belong in a temporary profile. A checkpoint left behind would raise a modal
    // question and the capture run would wait for a click that never comes.
    QVERIFY(m_settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintUiScreensTest"));
    QSettings().clear();
    qputenv("PHOTOPRINT_NO_RECOVERY", "1");
    qputenv("PHOTOPRINT_NO_SETTINGS", "1");
    qputenv("PHOTOPRINT_NO_CUPS", "1");
}

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

void TestUiScreens::captureLayoutPanel()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A 2 x 2 layout with four photographs and a 3 mm gap. Every photograph
    // touches a neighbour, so every visible gap edge is a real seam and not
    // empty paper.
    DocumentSnapshot document = fourEmptySlots();
    Page page = document.pages.first();
    SlotStyle style;
    style.marginMm = 10.0;
    style.gutterMm = 3.0;
    const auto styled = SlotLayoutEngine::setStyle(page, style);
    QVERIFY(styled);
    page = styled.value();

    const QList<QColor> colours = {QColor(190, 70, 60), QColor(65, 135, 185),
                                   QColor(80, 150, 95), QColor(195, 150, 55)};
    for (int i = 0; i < page.slotList.size(); ++i) {
        const QString photo =
            makePhoto(dir.path(), QStringLiteral("layout-photo-%1.png").arg(i),
                      colours.at(i), QSize(900, 600));
        QVERIFY(!photo.isEmpty());
        Frame frame;
        frame.id = QStringLiteral("frame-layout-%1").arg(i);
        frame.sourcePath = photo;
        frame.sourceSha256 = fileSha256(photo);
        frame.fit = FitMode::Fill;
        frame.autoPlaced = false;
        const auto placed =
            SlotLayoutEngine::placePhoto(page, page.slotList.at(i).id, frame);
        QVERIFY(placed);
        page = placed.value();
    }
    document.pages[0] = page;

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(loadDocument(window, dir, document));
    QTest::qWait(350);

    auto *panel = window.findChild<QWidget *>(QStringLiteral("layoutPanel"));
    QVERIFY(panel);
    QVERIFY2(panel->isVisible(), "the layout panel must be visible with nothing selected");
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-layout-panel.png")));

    // The page alone, so the seam between two photographs is readable.
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(view->grab().save(evidenceDir() + QStringLiteral("/ui-layout-gaps.png")));

    // The same page as the print path rasterises it. Every cell is a
    // photograph, so the paper-only gaps are unambiguous.
    ImageStore store;
    PageRenderer renderer(store);
    const auto rendered = renderer.renderPage(window.document(), 0, 200.0);
    if (!rendered)
        QFAIL(qPrintable(rendered.error().message));
    const QImage printed = rendered.value();
    QVERIFY(printed.save(evidenceDir() + QStringLiteral("/ui-layout-print.png")));

    const QList<QRectF> cells = SlotGeometry::cellsMm(page);
    QVERIFY(cells.size() == 4);
    const QSizeF sheet = pageSizeMm(page);
    const auto cropMm = [&](const QImage &image, const QRectF &regionMm, int scale) {
        const double pxPerMmX = image.width() / sheet.width();
        const double pxPerMmY = image.height() / sheet.height();
        QRect pixels(qRound(regionMm.left() * pxPerMmX),
                     qRound(regionMm.top() * pxPerMmY),
                     qRound(regionMm.width() * pxPerMmX),
                     qRound(regionMm.height() * pxPerMmY));
        pixels = pixels.intersected(image.rect());
        if (pixels.isEmpty())
            return QImage();
        const QImage crop = image.copy(pixels);
        return crop.scaled(crop.size() * scale, Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation);
    };

    // A 200 % crop of the horizontal seam between the top two photographs: the
    // ink reaches both cell edges and the gap is only paper.
    const QRectF seam(cells.at(0).right() - 5.0, cells.at(0).center().y() - 10.0,
                      (cells.at(1).left() - cells.at(0).right()) + 10.0, 20.0);
    const QImage seamCrop = cropMm(printed, seam, 2);
    QVERIFY(!seamCrop.isNull());
    QVERIFY(seamCrop.save(evidenceDir() +
                          QStringLiteral("/ui-layout-gap-seam-200.png")));
    bool seamPaper = false;
    bool seamRed = false;
    bool seamBlue = false;
    for (int y = 0; y < seamCrop.height(); ++y) {
        for (int x = 0; x < seamCrop.width(); ++x) {
            const QRgb pixel = seamCrop.pixel(x, y);
            seamPaper = seamPaper
                || (qRed(pixel) > 245 && qGreen(pixel) > 245 && qBlue(pixel) > 245);
            seamRed = seamRed || (qRed(pixel) > 120 && qGreen(pixel) < 120);
            seamBlue = seamBlue || (qBlue(pixel) > 120 && qRed(pixel) < 120);
        }
    }
    QVERIFY2(seamPaper && seamRed && seamBlue,
             "the seam crop must show both photographs and the paper between them");

    // The same four photographs with an 8 mm corner radius: the clip cuts each
    // photograph and the paper shows in the corner, with no ring around it.
    DocumentSnapshot rounded = document;
    SlotStyle roundedStyle = rounded.pages.first().slotStyle;
    roundedStyle.cornerRadiusMm = 8.0;
    const auto roundedPage = SlotLayoutEngine::setStyle(rounded.pages.first(), roundedStyle);
    QVERIFY(roundedPage);
    rounded.pages[0] = roundedPage.value();
    const auto roundedRender = renderer.renderPage(rounded, 0, 200.0);
    if (!roundedRender)
        QFAIL(qPrintable(roundedRender.error().message));
    const QImage roundedPrinted = roundedRender.value();
    QVERIFY(roundedPrinted.save(evidenceDir() +
                                QStringLiteral("/ui-layout-rounded-print.png")));
    const QRectF corner(cells.at(0).left() - 2.0, cells.at(0).top() - 2.0, 16.0, 16.0);
    const QImage cornerCrop = cropMm(roundedPrinted, corner, 2);
    QVERIFY(!cornerCrop.isNull());
    QVERIFY(cornerCrop.save(evidenceDir() +
                            QStringLiteral("/ui-layout-rounded-corner-200.png")));
    bool cornerPaper = false;
    bool cornerInk = false;
    for (int y = 0; y < cornerCrop.height(); ++y) {
        for (int x = 0; x < cornerCrop.width(); ++x) {
            const QRgb pixel = cornerCrop.pixel(x, y);
            cornerPaper = cornerPaper
                || (qRed(pixel) > 245 && qGreen(pixel) > 245 && qBlue(pixel) > 245);
            cornerInk = cornerInk || !(qRed(pixel) > 245 && qGreen(pixel) > 245
                                       && qBlue(pixel) > 245);
        }
    }
    QVERIFY2(cornerPaper && cornerInk,
             "the rounded corner crop must show paper and photograph");

    // The same panel at a 1.35x system font. The contract asks for a window that
    // works with larger system text; the longest button used to clip here.
    {
        const QFont saved = QApplication::font();
        QFont large = saved;
        if (large.pointSizeF() > 0.0)
            large.setPointSizeF(saved.pointSizeF() * 1.35);
        else
            large.setPixelSize(int(std::lround(saved.pixelSize() * 1.35)));
        QApplication::setFont(large);
        MainWindow largeWindow;
        largeWindow.resize(1280, 800);
        largeWindow.show();
        QTest::qWait(150);
        QVERIFY(loadDocument(largeWindow, dir, document));
        QTest::qWait(300);
        QVERIFY(largeWindow.grab().save(
            evidenceDir() + QStringLiteral("/ui-layout-panel-large-font.png")));
        QApplication::setFont(saved);
    }

    // Dissolving keeps the photographs and removes the cells and the panel.
    answerNextQuestionWith(QMessageBox::Yes);
    QVERIFY(QMetaObject::invokeMethod(&window, "dissolvePageLayout", Qt::DirectConnection));
    QTest::qWait(250);
    QVERIFY(!window.document().pages.first().hasLayout());
    QVERIFY(!panel->isVisible());
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-layout-dissolved.png")));
}

void TestUiScreens::captureSectionComposition()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A patterned photo: a flat colour would hide the rotation in the capture.
    QImage sample(QSize(900, 600), QImage::Format_RGB32);
    sample.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QPainter painter(&sample);
    painter.fillRect(QRect(0, 0, 450, 600), QColor(190, 95, 60));
    painter.fillRect(QRect(450, 0, 450, 600), QColor(245, 225, 195));
    painter.setPen(QPen(QColor(45, 45, 45), 10));
    painter.drawRect(QRect(40, 40, 820, 520));
    painter.drawEllipse(QPointF(450, 300), 180, 180);
    painter.end();
    const QString photo = dir.filePath(QStringLiteral("section-photo.png"));
    {
        QImageWriter writer(photo, "png");
        QVERIFY(writer.write(sample));
    }

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(loadDocument(window, dir, fourEmptySlots()));
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, QStringList{photo})));
    QVERIFY(window.waitForPreview(15000));
    QTest::qWait(300);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const Page page = window.document().pages.first();
    const QString slotId = page.slotList.at(0).id;
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(slotId).center().toPoint());
    QTest::qWait(150);

    // The section is a clipping window: make the photograph smaller than the
    // cell, move it and turn it, so the capture shows paper around a composed
    // picture instead of a forced cover.
    auto *width = window.findChild<QDoubleSpinBox *>(QStringLiteral("widthSpin"));
    auto *height = window.findChild<QDoubleSpinBox *>(QStringLiteral("heightSpin"));
    auto *angle = window.findChild<QDoubleSpinBox *>(QStringLiteral("angleSpin"));
    QVERIFY(width);
    QVERIFY(height);
    QVERIFY(angle);
    const Slot *slot = SlotGeometry::slotById(page, slotId);
    QVERIFY(slot);
    const QRectF cell = SlotGeometry::slotCellMm(page, *slot);
    width->setValue(cell.width() / 10.0 * 0.7);
    QMetaObject::invokeMethod(width, "editingFinished");
    QTest::qWait(80);
    height->setValue(cell.height() / 10.0 * 0.7);
    QMetaObject::invokeMethod(height, "editingFinished");
    QTest::qWait(80);
    angle->setValue(18.0);
    QMetaObject::invokeMethod(angle, "editingFinished");
    QTest::qWait(500);

    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-section-composition.png")));
    QVERIFY(view->grab().save(evidenceDir() +
                              QStringLiteral("/ui-section-composition-canvas.png")));

    // The compact photo inspector: the image rotation control and the section
    // reset sit together, so the capture shows the restored controls.
    auto *photoPanel = window.findChild<QWidget *>(QStringLiteral("photoPanel"));
    QVERIFY(photoPanel);
    for (QScrollArea *area : window.findChildren<QScrollArea *>()) {
        if (area->widget() && area->widget()->isAncestorOf(photoPanel)) {
            area->ensureWidgetVisible(photoPanel);
            QTest::qWait(150);
        }
    }
    QVERIFY(window.grab().save(evidenceDir() +
                               QStringLiteral("/ui-section-photo-controls.png")));
}

QTEST_MAIN(TestUiScreens)
#include "tst_ui_screens.moc"
