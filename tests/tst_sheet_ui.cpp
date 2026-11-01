#include "Document.h"
#include "ImageSource.h"
#include "JobController.h"
#include "MainWindow.h"
#include "PaperGeometry.h"
#include "PrintTicket.h"
#include "ProjectStore.h"
#include "SheetLayout.h"
#include "SheetToolsDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QLabel>
#include <QLocale>
#include <QFontMetrics>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

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

/// QVERIFY2 evaluates its description eagerly, so a failure message must not
/// call Result::error() on a successful result.
QString mmTextExpected(double value)
{
    QString text = QLocale().toString(value, 'f', 1);
    if (text.endsWith(QLocale().decimalPoint() + QStringLiteral("0")))
        text.chop(2);
    return text;
}

template <typename T>
QString why(const Result<T> &result)
{
    return result.ok() ? QString() : result.error().message;
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

/// A photograph whose PNG is large enough that hashing it is measurable: a
/// flat colour compresses to a few kilobytes and hides the cost of a
/// per-copy verification.
QString makeNoisyPhoto(const QString &directory, const QString &name, QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QRandomGenerator *random = QRandomGenerator::global();
    for (int y = 0; y < size.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            line[x] = random->generate() | 0xff000000u;
    }
    const QString path = QDir(directory).filePath(name);
    QImageWriter writer(path, "png");
    writer.write(image);
    return path;
}

PaperSpec paperMm(const QString &name, double width, double height, PaperType type)
{
    PaperSpec paper;
    paper.name = name;
    paper.physicalWidthMm = width;
    paper.physicalHeightMm = height;
    paper.type = type;
    return paper;
}

PaperSpec tenByFifteen()
{
    PaperSpec paper = paperMm(QStringLiteral("10 × 15 cm"), 100.0, 150.0, PaperType::Glossy);
    paper.logicalWidthMm = 101.6;
    paper.logicalHeightMm = 152.4;
    return paper;
}

PaperSpec a4()
{
    return paperMm(QStringLiteral("A4"), 210.0, 297.0, PaperType::Plain);
}

Frame frameAt(const QString &path, const QByteArray &sha, double cx, double cy, double width,
              double height)
{
    Frame frame;
    frame.id = makeId(QStringLiteral("frame"));
    frame.sourcePath = path;
    frame.sourceSha256 = sha;
    frame.cx = cx;
    frame.cy = cy;
    frame.width = width;
    frame.height = height;
    frame.fit = FitMode::Fill;
    frame.autoPlaced = false;
    return frame;
}

DocumentSnapshot singlePage(const PaperSpec &paper, const QList<Frame> &frames)
{
    DocumentSnapshot document;
    Page page;
    page.id = makeId(QStringLiteral("page"));
    page.paper = paper;
    page.frames = frames;
    document.pages.append(page);
    document.currentPage = 0;
    return document;
}

QHash<QString, QImage> previewsFor(const DocumentSnapshot &document, QSize size = QSize(600, 400))
{
    QHash<QString, QImage> previews;
    for (const Page &page : document.pages) {
        for (const Frame &frame : page.frames) {
            QImage image(size, QImage::Format_RGB32);
            image.fill(QColor(90, 130, 170));
            previews.insert(frame.id, image);
        }
    }
    return previews;
}

bool loadDocument(MainWindow &window, const QTemporaryDir &dir, const DocumentSnapshot &document)
{
    const QString path = dir.filePath(QStringLiteral("sheet.ppj"));
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

/// The 0.2 mm print line, sampled at the top edge's midpoint, as one luminance
/// value. A white photo on white paper makes any darkening the line's own.
int lineLuma(const QImage &page, double xMm, double yMm, double ppi)
{
    const int px = std::clamp(qRound(xMm / 25.4 * ppi), 0, page.width() - 1);
    const int py = std::clamp(qRound(yMm / 25.4 * ppi), 0, page.height() - 1);
    const QRgb pixel = page.pixel(px, py);
    return qGray(pixel);
}

} // namespace

class TestSheetUi : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void batchDefaultsShowFitAndMargins();
    void batchPresetAndCustomSize();
    void batchCountSplitsPages();
    void batchFillUsesFitCount();
    void batchCapRefusesTooManyPages();
    void tinyPaperRefusesAndCustomCopyFits();

    void batchAppliesOneUndoOnCurrentPage();
    void batchNewPagesLeaveCurrentPageAlone();
    void batchHundredCopiesAcrossSeventeenPages();
    void batchDissolvesTargetLayout();
    void livePreviewDoesNotMutate();

    void alignGridPreservesFieldsAndCentres();
    void alignWarnsOnHeavyCropAndFullPhotoKeepsAll();
    void alignSinglePhotoCentresOnly();
    void alignTooManyPhotosRefused();
    void alignDisabledForLaidOutPage();
    void distributeKeepsSizesAndEqualGaps();
    void distributeShrinksGapAndRefusesOverflow();
    void distributeRefusesRotatedFrame();

    void cutGuidesScreenOverlayDiffersFromPrint();
    void cutGuidesPrintInkIsHalfGrey();
    void printGuardNamesPrintedPages();
    void guidesComboIsUndoable();

    void unreadableSourceDisablesCreate();
    void unavailableFramesFeedTheBatchDialog();
    void batchPreviewNavigatesSheets();
    void batchGuideOverwriteIsStated();
    void fillCapIsNamed();
    void alignDoesNotClaimReplacement();
    void borderlessComboIsUndoable();
    void borderlessConsistencyIsEnforced();
    void printGuardNamesTheGivenPages();
    void guideHintsFollowTheSelection();

    void borderlessOverridePinsExactCopySize();
    void verifyOncePerPrintJob();
    void captureSheetToolDialogs();

private:
    QTemporaryDir m_settingsDir;
};

void TestSheetUi::initTestCase()
{
    QVERIFY(m_settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintSheetUiTest"));
    QSettings().clear();
    qputenv("PHOTOPRINT_NO_RECOVERY", "1");
    qputenv("PHOTOPRINT_NO_SETTINGS", "1");
    qputenv("PHOTOPRINT_NO_CUPS", "1");
}

void TestSheetUi::batchDefaultsShowFitAndMargins()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("portrait.png"), QColor(120, 90, 70), QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    QVERIFY(!sha.isEmpty());
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document));
    QCOMPARE(dialog.currentTabIndexForTest(), 0);
    QVERIFY(dialog.createEnabledForTest());

    const auto grid = dialog.batchGridForTest();
    QVERIFY(grid.has_value());
    QCOMPARE(grid->columns, 2);
    QCOMPARE(grid->rows, 3);
    QCOMPARE(grid->perPage, 6);
    QCOMPARE(grid->count, 6);
    QCOMPARE(grid->pageCount(), 1);
    QVERIFY(std::abs(grid->blockWidthMm - 73.0) < 1e-6);
    QVERIFY(std::abs(grid->blockHeightMm - 141.0) < 1e-6);
    QVERIFY(std::abs(grid->leftMarginMm - 13.5) < 1e-6);
    QVERIFY(std::abs(grid->topMarginMm - 4.5) < 1e-6);
    // The first copy centre is the documented (31, 27).
    QVERIFY(std::abs(grid->pagesMm.first().first().center().x() - 31.0) < 1e-6);
    QVERIFY(std::abs(grid->pagesMm.first().first().center().y() - 27.0) < 1e-6);

    // The dialog reports the fit count and the effective margins, so the
    // "margin is a minimum" rule is visible before commit.
    QVERIFY2(dialog.summaryTextForTest().contains(QStringLiteral("6")),
             qPrintable(dialog.summaryTextForTest()));
    QVERIFY2(dialog.captionTextForTest().contains(mmTextExpected(13.5)),
             qPrintable(dialog.captionTextForTest()));
    QVERIFY2(dialog.captionTextForTest().contains(mmTextExpected(4.5)),
             qPrintable(dialog.captionTextForTest()));
    QVERIFY(!dialog.lastSheetNoteVisibleForTest());
    auto *exact = dialog.findChild<QLabel *>(QStringLiteral("sheetExactSizeNote"));
    QVERIFY(exact);
    QVERIFY2(exact->text().contains(QStringLiteral("pontosan")), qPrintable(exact->text()));

    // The screen reader gets the same fit count and warnings through the
    // preview's accessible description.
    auto *preview = dialog.findChild<SheetPreview *>(QStringLiteral("sheetPreview"));
    QVERIFY(preview);
    QVERIFY2(preview->accessibleDescription().contains(QStringLiteral("6")),
             qPrintable(preview->accessibleDescription()));

    auto *fill = dialog.findChild<QCheckBox *>(QStringLiteral("batchFill"));
    QVERIFY(fill);
    QVERIFY(fill->isChecked());
    auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    QVERIFY(count);
    QCOMPARE(count->value(), 6);
    QVERIFY(!count->isEnabled());
}

void TestSheetUi::batchPresetAndCustomSize()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("square.png"), QColor(80, 120, 90), QSize(700, 700));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document));
    auto *preset = dialog.findChild<QComboBox *>(QStringLiteral("batchPreset"));
    auto *width = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("batchWidthCm"));
    auto *height = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("batchHeightCm"));
    auto *swap = dialog.findChild<QPushButton *>(QStringLiteral("batchSwap"));
    QVERIFY(preset && width && height && swap);

    // 50 × 70 on 10 × 15 portrait fits one column and two rows.
    const int fiftyBySeventy = preset->findData(QStringLiteral("id50x70"));
    QVERIFY(fiftyBySeventy >= 0);
    preset->setCurrentIndex(fiftyBySeventy);
    QVERIFY(std::abs(width->value() - 5.0) < 1e-9);
    QVERIFY(std::abs(height->value() - 7.0) < 1e-9);
    auto grid = dialog.batchGridForTest();
    QVERIFY(grid.has_value());
    QCOMPARE(grid->columns, 1);
    QCOMPARE(grid->rows, 2);
    QCOMPARE(grid->perPage, 2);

    // Swap changes the copy, never the photo orientation.
    swap->click();
    QVERIFY(std::abs(width->value() - 7.0) < 1e-9);
    QVERIFY(std::abs(height->value() - 5.0) < 1e-9);
    grid = dialog.batchGridForTest();
    QVERIFY(grid.has_value());
    QVERIFY(std::abs(grid->copyWidthMm - 70.0) < 1e-6);
    QVERIFY(std::abs(grid->copyHeightMm - 50.0) < 1e-6);

    // Typing a size switches the preset to Custom and the grid follows it.
    width->setValue(3.0);
    height->setValue(4.0);
    QCOMPARE(preset->currentIndex(), preset->count() - 1);
    grid = dialog.batchGridForTest();
    QVERIFY(grid.has_value());
    QVERIFY(std::abs(grid->copyWidthMm - 30.0) < 1e-6);
    QVERIFY(std::abs(grid->copyHeightMm - 40.0) < 1e-6);
    QCOMPARE(grid->perPage, 6);
}

void TestSheetUi::batchCountSplitsPages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("split.png"), QColor(100, 100, 140), QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document));
    auto *fill = dialog.findChild<QCheckBox *>(QStringLiteral("batchFill"));
    auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    QVERIFY(fill && count);
    fill->setChecked(false);
    QVERIFY(count->isEnabled());
    count->setValue(20);

    const auto grid = dialog.batchGridForTest();
    QVERIFY(grid.has_value());
    QCOMPARE(grid->perPage, 6);
    QCOMPARE(grid->pageCount(), 4);
    QCOMPARE(grid->pagesMm.at(0).size(), 6);
    QCOMPARE(grid->pagesMm.at(1).size(), 6);
    QCOMPARE(grid->pagesMm.at(2).size(), 6);
    QCOMPARE(grid->pagesMm.at(3).size(), 2);
    QVERIFY(dialog.lastSheetNoteVisibleForTest());
    // The last sheet starts at the same origin as every other sheet, so a stack
    // can be cut in one pass.
    QCOMPARE(grid->pagesMm.at(3).first().topLeft(), grid->pagesMm.at(0).first().topLeft());
    QVERIFY(dialog.summaryTextForTest().contains(QStringLiteral("4")));
}

void TestSheetUi::batchFillUsesFitCount()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("fill.png"), QColor(140, 90, 100), QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document));
    auto *fill = dialog.findChild<QCheckBox *>(QStringLiteral("batchFill"));
    auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    QVERIFY(fill && count);
    fill->setChecked(false);
    count->setValue(4);
    QCOMPARE(dialog.batchGridForTest()->count, 4);
    fill->setChecked(true);
    QCOMPARE(count->value(), 6);
    QCOMPARE(dialog.batchGridForTest()->count, 6);
    QCOMPARE(dialog.batchGridForTest()->pageCount(), 1);
}

void TestSheetUi::batchCapRefusesTooManyPages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("cap.png"), QColor(70, 110, 130), QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document));
    auto *fill = dialog.findChild<QCheckBox *>(QStringLiteral("batchFill"));
    auto *width = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("batchWidthCm"));
    auto *height = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("batchHeightCm"));
    auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    QVERIFY(fill && width && height && count);
    fill->setChecked(false);
    // One 90 × 130 mm copy per 10 × 15 sheet: 100 copies is 100 pages, above the
    // 20 page cap. Both numbers are named in the message.
    width->setValue(9.0);
    height->setValue(13.0);
    count->setValue(100);
    QVERIFY(!dialog.createEnabledForTest());
    QVERIFY2(dialog.summaryTextForTest().contains(QStringLiteral("100")),
             qPrintable(dialog.summaryTextForTest()));
    QVERIFY2(dialog.summaryTextForTest().contains(QStringLiteral("20")),
             qPrintable(dialog.summaryTextForTest()));
    QVERIFY(!dialog.batchGridForTest().has_value());
    dialog.accept();
    QVERIFY(!dialog.hasEdit());
}

void TestSheetUi::tinyPaperRefusesAndCustomCopyFits()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("tiny.png"), QColor(90, 90, 90), QSize(400, 500));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 20.0, 30.0, 35.0, 45.0);

    // A 40 × 60 mm custom sheet cannot hold a 35 × 45 copy with the 3.5 mm
    // margin: the tool refuses and names the largest that would fit.
    const DocumentSnapshot tiny =
        singlePage(paperMm(QStringLiteral("40 × 60 mm"), 40.0, 60.0, PaperType::Plain), {source});
    SheetToolsDialog refused(SheetToolsDialog::Mode::Batch, tiny, 0, source, previewsFor(tiny));
    QVERIFY(!refused.createEnabledForTest());
    QVERIFY(!refused.batchGridForTest().has_value());
    QVERIFY2(refused.summaryTextForTest().contains(QStringLiteral("33")),
             qPrintable(refused.summaryTextForTest()));

    // A 60 × 80 mm sheet holds exactly one.
    const DocumentSnapshot custom =
        singlePage(paperMm(QStringLiteral("60 × 80 mm"), 60.0, 80.0, PaperType::Plain), {source});
    SheetToolsDialog fits(SheetToolsDialog::Mode::Batch, custom, 0, source, previewsFor(custom));
    const auto grid = fits.batchGridForTest();
    QVERIFY(grid.has_value());
    QCOMPARE(grid->columns, 1);
    QCOMPARE(grid->rows, 1);
    QCOMPARE(grid->perPage, 1);
    QVERIFY(fits.createEnabledForTest());
}

void TestSheetUi::batchAppliesOneUndoOnCurrentPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("undo.png"), QColor(60, 120, 150), QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    source.filter = QStringLiteral("sepia");
    source.filterStrength = 70;
    source.brightness = 0.1;
    source.cropCenter = QPointF(0.4, 0.6);
    source.zoom = 1.2;
    source.imageRotationDeg = 90.0;
    source.label = QStringLiteral("7");
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));
    const DocumentSnapshot before = window.document();
    QCOMPARE(before.pages.size(), 1);
    const int undoBefore = window.undoStackForTest()->count();

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, before, 0, before.pages.at(0).frames.at(0),
                            window.previewImagesForTest(), true, &window);
    auto *fill = dialog.findChild<QCheckBox *>(QStringLiteral("batchFill"));
    auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    auto *guides = dialog.findChild<QComboBox *>(QStringLiteral("batchGuides"));
    QVERIFY(fill && count && guides);
    fill->setChecked(false);
    count->setValue(20);
    guides->setCurrentIndex(guides->findData(int(CutGuides::Print)));
    dialog.accept();
    QVERIFY(dialog.hasEdit());
    QVERIFY(dialog.replacesCurrentPage());

    const auto applied = window.applySheetEdit(dialog.edit());
    QVERIFY2(applied.ok(), qPrintable(why(applied)));
    QCOMPARE(window.undoStackForTest()->count(), undoBefore + 1);
    QCOMPARE(window.undoStackForTest()->undoText(), QStringLiteral("Képek sokszorosítása"));

    const DocumentSnapshot after = window.document();
    QCOMPARE(after.pages.size(), 4);
    QCOMPARE(after.pages.at(0).frames.size(), 6);
    QCOMPARE(after.pages.at(3).frames.size(), 2);
    int total = 0;
    QSet<QString> ids;
    for (const Page &page : after.pages) {
        QCOMPARE(page.cutGuides, CutGuides::Print);
        QCOMPARE(page.borderlessOverride, BorderlessOverride::Off);
        for (const Frame &frame : page.frames) {
            ++total;
            ids.insert(frame.id);
            QCOMPARE(frame.sourcePath, photo);
            QCOMPARE(frame.sourceSha256, sha);
            QCOMPARE(frame.angleDeg, 0.0);
            QVERIFY(frame.fit == FitMode::Fill);
            QVERIFY(!frame.autoPlaced);
            QVERIFY(frame.label.isEmpty());
            QCOMPARE(frame.filter, QStringLiteral("sepia"));
            QCOMPARE(frame.filterStrength, 70);
            QCOMPARE(frame.brightness, 0.1);
            QCOMPARE(frame.cropCenter, QPointF(0.4, 0.6));
            QCOMPARE(frame.zoom, 1.2);
            QCOMPARE(frame.imageRotationDeg, 90.0);
            QVERIFY(frame.slotId.isEmpty());
        }
    }
    QCOMPARE(total, 20);
    QCOMPARE(ids.size(), 20);

    window.undoStackForTest()->undo();
    QVERIFY(window.document() == before);
    QCOMPARE(window.undoStackForTest()->redoText(), QStringLiteral("Képek sokszorosítása"));
    window.undoStackForTest()->redo();
    QCOMPARE(window.document().pages.size(), 4);
}

void TestSheetUi::batchNewPagesLeaveCurrentPageAlone()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("newpages.png"), QColor(150, 110, 80), QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));
    const DocumentSnapshot before = window.document();

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, before, 0, before.pages.at(0).frames.at(0),
                            window.previewImagesForTest(), true, &window);
    auto *fill = dialog.findChild<QCheckBox *>(QStringLiteral("batchFill"));
    auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    auto *placeNew = dialog.findChild<QRadioButton *>(QStringLiteral("batchPlaceNew"));
    QVERIFY(fill && count && placeNew);
    fill->setChecked(false);
    count->setValue(8);
    placeNew->setChecked(true);
    dialog.accept();
    QVERIFY(dialog.hasEdit());
    QVERIFY(!dialog.replacesCurrentPage());

    const auto applied = window.applySheetEdit(dialog.edit());
    QVERIFY2(applied.ok(), qPrintable(why(applied)));
    const DocumentSnapshot after = window.document();
    QCOMPARE(after.pages.size(), 3);
    // The source page keeps its single photograph and its id.
    QCOMPARE(after.pages.at(0).id, before.pages.at(0).id);
    QCOMPARE(after.pages.at(0).frames.size(), 1);
    QCOMPARE(after.pages.at(0).frames.at(0).id, before.pages.at(0).frames.at(0).id);
    QCOMPARE(after.pages.at(1).frames.size(), 6);
    QCOMPARE(after.pages.at(2).frames.size(), 2);
    QCOMPARE(after.currentPage, 1);

    window.undoStackForTest()->undo();
    QVERIFY(window.document() == before);
}

void TestSheetUi::batchHundredCopiesAcrossSeventeenPages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("hundred.png"), QColor(110, 70, 130), QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));
    const int undoBefore = window.undoStackForTest()->count();

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document), true, &window);
    auto *fill = dialog.findChild<QCheckBox *>(QStringLiteral("batchFill"));
    auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    auto *placeNew = dialog.findChild<QRadioButton *>(QStringLiteral("batchPlaceNew"));
    QVERIFY(fill && count && placeNew);
    fill->setChecked(false);
    count->setValue(100);
    placeNew->setChecked(true);
    QCOMPARE(dialog.batchGridForTest()->pageCount(), 17);
    QVERIFY(dialog.createEnabledForTest());
    dialog.accept();
    QVERIFY(dialog.hasEdit());
    QCOMPARE(dialog.edit().newPages.size(), 17);

    const auto applied = window.applySheetEdit(dialog.edit());
    QVERIFY2(applied.ok(), qPrintable(why(applied)));
    QCOMPARE(window.undoStackForTest()->count(), undoBefore + 1);
    const DocumentSnapshot after = window.document();
    QCOMPARE(after.pages.size(), 18);
    int copies = 0;
    for (const Page &page : after.pages)
        copies += page.frames.size();
    QCOMPARE(copies, 101); // the source page's own photo plus the hundred copies
    QVERIFY2(after.pages.at(1).hasLayout() == false, "a copy sheet is a free page");

    window.undoStackForTest()->undo();
    QCOMPARE(window.document().pages.size(), 1);
}

void TestSheetUi::batchDissolvesTargetLayout()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("slotted.png"), QColor(90, 140, 90), QSize(600, 600));
    const QByteArray sha = fileSha256(photo);

    DocumentSnapshot document;
    Page page;
    page.id = makeId(QStringLiteral("page"));
    page.paper = tenByFifteen();
    Slot first;
    first.id = QStringLiteral("slot-a");
    first.unitRect = QRectF(0.0, 0.0, 0.5, 0.5);
    Slot second;
    second.id = QStringLiteral("slot-b");
    second.unitRect = QRectF(0.5, 0.0, 0.5, 0.5);
    page.slotList = {first, second};
    page.layoutId = QStringLiteral("n02-grid");
    Frame slotted = frameAt(photo, sha, 30.0, 40.0, 45.0, 60.0);
    slotted.slotId = first.id;
    page.frames = {slotted};
    document.pages = {page};
    document.currentPage = 0;

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));
    const DocumentSnapshot before = window.document();

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, before, 0, before.pages.at(0).frames.at(0),
                            window.previewImagesForTest(), true, &window);
    // The batch warns that the page's layout is dissolved in the same step.
    QVERIFY2(dialog.warningTextForTest().contains(QStringLiteral("elrendezése")),
             qPrintable(dialog.warningTextForTest()));
    dialog.accept();
    QVERIFY(dialog.hasEdit());
    const auto applied = window.applySheetEdit(dialog.edit());
    QVERIFY2(applied.ok(), qPrintable(why(applied)));

    const Page &after = window.document().pages.at(0);
    QVERIFY(!after.hasLayout());
    QVERIFY(after.layoutId.isEmpty());
    QCOMPARE(after.frames.size(), 6);
    for (const Frame &frame : after.frames)
        QVERIFY(frame.slotId.isEmpty());
}

void TestSheetUi::livePreviewDoesNotMutate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("preview.png"), QColor(100, 130, 90), QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));
    const DocumentSnapshot before = window.document();
    const int undoBefore = window.undoStackForTest()->count();

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, before, 0, source,
                            previewsFor(before), true, &window);
    auto *preset = dialog.findChild<QComboBox *>(QStringLiteral("batchPreset"));
    auto *gutter = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("batchGutter"));
    auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    auto *guides = dialog.findChild<QComboBox *>(QStringLiteral("batchGuides"));
    QVERIFY(preset && gutter && count && guides);
    for (int i = 0; i < preset->count(); ++i) {
        preset->setCurrentIndex(i);
        gutter->setValue(i);
        count->setValue(1 + i);
        guides->setCurrentIndex(i % 3);
    }
    // Every control moved; the document and the undo history did not.
    QVERIFY(window.document() == before);
    QCOMPARE(window.undoStackForTest()->count(), undoBefore);
    auto *preview = dialog.findChild<SheetPreview *>(QStringLiteral("sheetPreview"));
    QVERIFY(preview);
    QVERIFY(!preview->grab().isNull());
    // No accept, no edit.
    QVERIFY(!dialog.hasEdit());

    // The align tab has the same contract: browsing rebuilds its preview and
    // leaves both the document and the undo stack alone.
    SheetToolsDialog align(SheetToolsDialog::Mode::Align, before, 0, std::nullopt,
                           previewsFor(before), true, &window);
    auto *gridMode = align.findChild<QRadioButton *>(QStringLiteral("alignGridMode"));
    auto *gapMode = align.findChild<QRadioButton *>(QStringLiteral("alignGapMode"));
    auto *alignMarginSpin = align.findChild<QDoubleSpinBox *>(QStringLiteral("alignMargin"));
    auto *alignGutterSpin = align.findChild<QDoubleSpinBox *>(QStringLiteral("alignGutter"));
    auto *alignGapSpin = align.findChild<QDoubleSpinBox *>(QStringLiteral("alignGap"));
    QVERIFY(gridMode && gapMode && alignMarginSpin && alignGutterSpin && alignGapSpin);
    gapMode->setChecked(true);
    alignMarginSpin->setValue(12.0);
    alignGapSpin->setValue(7.0);
    gridMode->setChecked(true);
    alignGutterSpin->setValue(4.0);
    QVERIFY(window.document() == before);
    QCOMPARE(window.undoStackForTest()->count(), undoBefore);
    QVERIFY(!align.hasEdit());
    QVERIFY(align.alignPlanForTest().has_value());
}

void TestSheetUi::alignGridPreservesFieldsAndCentres()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("align.png"), QColor(80, 100, 130), QSize(600, 600));
    const QByteArray sha = fileSha256(photo);
    QList<Frame> frames;
    for (int i = 0; i < 4; ++i) {
        Frame frame = frameAt(photo, sha, 20.0 + i * 40.0, 30.0, 50.0, 50.0);
        frame.tone.exposureEv = 0.05 * i;
        frame.filter = i == 1 ? QStringLiteral("sepia") : QStringLiteral("original");
        frame.filterStrength = 40 + i;
        frame.label = QStringLiteral("L%1").arg(i);
        frame.cropCenter = QPointF(0.3 + 0.1 * i, 0.4);
        frame.zoom = 1.0 + 0.1 * i;
        frame.imageRotationDeg = i == 2 ? 90.0 : 0.0;
        frame.autoPlaced = true;
        frame.paperOverride = i == 3;
        frame.paperEv = 0.12;
        frames.append(frame);
    }
    const DocumentSnapshot document = singlePage(a4(), frames);

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt,
                            previewsFor(document, QSize(600, 600)));
    QCOMPARE(dialog.currentTabIndexForTest(), 1);
    auto *margin = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignMargin"));
    auto *gutter = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignGutter"));
    QVERIFY(margin && gutter);
    margin->setValue(10.0);
    gutter->setValue(10.0);

    const auto plan = dialog.alignPlanForTest();
    QVERIFY(plan.has_value());
    QCOMPARE(plan->columns, 2);
    QCOMPARE(plan->rows, 2);
    QVERIFY(std::abs(plan->cellWidthMm - 90.0) < 1e-6);
    QVERIFY(std::abs(plan->cellHeightMm - 133.5) < 1e-6);
    QCOMPARE(plan->centresMm.size(), 4);
    QVERIFY(std::abs(plan->centresMm.at(0).x() - 55.0) < 1e-6);
    QVERIFY(std::abs(plan->centresMm.at(0).y() - 76.75) < 1e-6);
    QVERIFY(std::abs(plan->centresMm.at(1).x() - 155.0) < 1e-6);
    QVERIFY(std::abs(plan->centresMm.at(2).y() - 220.25) < 1e-6);

    dialog.accept();
    QVERIFY(dialog.hasEdit());
    const QList<Frame> aligned = dialog.edit().pages.first().frames.value();
    QCOMPARE(aligned.size(), 4);
    for (int i = 0; i < aligned.size(); ++i) {
        const Frame &before = frames.at(i);
        const Frame &after = aligned.at(i);
        QCOMPARE(after.id, before.id);
        QCOMPARE(after.sourcePath, before.sourcePath);
        QCOMPARE(after.sourceSha256, before.sourceSha256);
        QCOMPARE(after.tone, before.tone);
        QCOMPARE(after.filter, before.filter);
        QCOMPARE(after.filterStrength, before.filterStrength);
        QCOMPARE(after.label, before.label);
        QCOMPARE(after.cropCenter, before.cropCenter);
        QCOMPARE(after.zoom, before.zoom);
        QCOMPARE(after.imageRotationDeg, before.imageRotationDeg);
        QCOMPARE(after.paperOverride, before.paperOverride);
        QCOMPARE(after.paperEv, before.paperEv);
        QVERIFY(std::abs(after.width - 90.0) < 1e-6);
        QVERIFY(std::abs(after.height - 133.5) < 1e-6);
        QCOMPARE(after.angleDeg, 0.0);
        QVERIFY(after.fit == FitMode::Fill);
        QVERIFY(!after.autoPlaced);
    }

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));
    const int undoBefore = window.undoStackForTest()->count();
    const auto applied = window.applySheetEdit(dialog.edit());
    QVERIFY2(applied.ok(), qPrintable(why(applied)));
    QCOMPARE(window.undoStackForTest()->count(), undoBefore + 1);
    QCOMPARE(window.undoStackForTest()->undoText(), QStringLiteral("Fotók rendezése"));
    QCOMPARE(window.document().pages.at(0).frames.size(), 4);
    window.undoStackForTest()->undo();
    QCOMPARE(window.document().pages.at(0).frames.at(0).width, 50.0);
}

void TestSheetUi::alignWarnsOnHeavyCropAndFullPhotoKeepsAll()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("wide.png"), QColor(120, 120, 90), QSize(1800, 300));
    const QByteArray sha = fileSha256(photo);
    QList<Frame> frames;
    for (int i = 0; i < 3; ++i)
        frames.append(frameAt(photo, sha, 20.0 + i * 60.0, 30.0, 50.0, 50.0));
    const DocumentSnapshot document = singlePage(a4(), frames);
    QHash<QString, QImage> previews;
    for (const Frame &frame : frames)
        previews.insert(frame.id, QImage(1800, 300, QImage::Format_RGB32));

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt, previews);
    auto *margin = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignMargin"));
    auto *gutter = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignGutter"));
    auto *whole = dialog.findChild<QCheckBox *>(QStringLiteral("alignFill"));
    QVERIFY(margin && gutter && whole);
    margin->setValue(10.0);
    gutter->setValue(10.0);
    QVERIFY(whole);
    QVERIFY(!whole->isChecked()); // crop to fill by default
    QVERIFY2(dialog.warningTextForTest().contains(QStringLiteral("erősen lemarad")),
             qPrintable(dialog.warningTextForTest()));
    // The alternative it offers is a real action in this dialog, not a promise
    // to spread the photos over more pages.
    QVERIFY2(dialog.warningTextForTest().contains(QStringLiteral("Térköz kiegyenlítése")),
             qPrintable(dialog.warningTextForTest()));
    QVERIFY2(!dialog.warningTextForTest().contains(QStringLiteral("több lapon")),
             qPrintable(dialog.warningTextForTest()));

    whole->setChecked(true);
    QVERIFY(dialog.warningTextForTest().isEmpty());
    dialog.accept();
    QVERIFY(dialog.hasEdit());
    for (const Frame &frame : dialog.edit().pages.first().frames.value())
        QVERIFY(frame.fit == FitMode::Full);
}

void TestSheetUi::alignSinglePhotoCentresOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("one.png"), QColor(90, 90, 120), QSize(400, 500));
    const QByteArray sha = fileSha256(photo);
    const Frame frame = frameAt(photo, sha, 20.0, 25.0, 40.0, 50.0);
    const DocumentSnapshot document = singlePage(a4(), {frame});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt,
                            previewsFor(document));
    QVERIFY(dialog.createEnabledForTest());
    QVERIFY2(dialog.summaryTextForTest().contains(QStringLiteral("középre")),
             qPrintable(dialog.summaryTextForTest()));
    dialog.accept();
    QVERIFY(dialog.hasEdit());
    const QList<Frame> after = dialog.edit().pages.first().frames.value();
    QCOMPARE(after.size(), 1);
    QCOMPARE(after.at(0).width, 40.0);
    QCOMPARE(after.at(0).height, 50.0);
    QVERIFY(std::abs(after.at(0).cx - 105.0) < 1e-6);
    QVERIFY(std::abs(after.at(0).cy - 148.5) < 1e-6);
    QVERIFY(!after.at(0).autoPlaced);
}

void TestSheetUi::alignTooManyPhotosRefused()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("many.png"), QColor(100, 100, 100), QSize(300, 300));
    const QByteArray sha = fileSha256(photo);
    QList<Frame> frames;
    for (int i = 0; i < 120; ++i)
        frames.append(frameAt(photo, sha, 50.0 + i, 50.0, 40.0, 40.0));
    const DocumentSnapshot document = singlePage(a4(), frames);

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt,
                            previewsFor(document));
    auto *margin = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignMargin"));
    auto *gutter = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignGutter"));
    QVERIFY(margin && gutter);
    margin->setValue(10.0);
    gutter->setValue(10.0);
    QVERIFY(!dialog.createEnabledForTest());
    // 8 columns x 11 rows of 15 mm cells is the readable maximum; the message
    // names both the limit and the current count.
    QVERIFY2(dialog.summaryTextForTest().contains(QStringLiteral("88")),
             qPrintable(dialog.summaryTextForTest()));
    QVERIFY2(dialog.summaryTextForTest().contains(QStringLiteral("120")),
             qPrintable(dialog.summaryTextForTest()));
    dialog.accept();
    QVERIFY(!dialog.hasEdit());
}

void TestSheetUi::alignDisabledForLaidOutPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("layout.png"), QColor(110, 110, 110), QSize(300, 300));
    const QByteArray sha = fileSha256(photo);
    DocumentSnapshot document;
    Page page;
    page.id = makeId(QStringLiteral("page"));
    page.paper = a4();
    page.slotList = {Slot{QStringLiteral("slot-1"), QRectF(0.0, 0.0, 0.5, 0.5)}};
    page.layoutId = QStringLiteral("n01");
    Frame frame = frameAt(photo, sha, 50.0, 50.0, 90.0, 120.0);
    frame.slotId = QStringLiteral("slot-1");
    page.frames = {frame};
    document.pages = {page};

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt,
                            previewsFor(document));
    QVERIFY(!dialog.createEnabledForTest());
    QVERIFY2(dialog.summaryTextForTest().contains(QStringLiteral("elrendezés")),
             qPrintable(dialog.summaryTextForTest()));
    dialog.accept();
    QVERIFY(!dialog.hasEdit());
}

void TestSheetUi::distributeKeepsSizesAndEqualGaps()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("distribute.png"), QColor(90, 120, 90), QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    QList<Frame> frames;
    frames.append(frameAt(photo, sha, 40.0, 148.5, 40.0, 30.0));
    frames.append(frameAt(photo, sha, 110.0, 148.5, 60.0, 30.0));
    frames.append(frameAt(photo, sha, 175.0, 148.5, 50.0, 30.0));
    const DocumentSnapshot document = singlePage(a4(), frames);

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt,
                            previewsFor(document));
    auto *gapMode = dialog.findChild<QRadioButton *>(QStringLiteral("alignGapMode"));
    auto *margin = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignMargin"));
    auto *gap = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignGap"));
    QVERIFY(gapMode && margin && gap);
    gapMode->setChecked(true);
    margin->setValue(10.0);
    gap->setValue(5.0);

    const auto plan = dialog.distributePlanForTest();
    QVERIFY(plan.has_value());
    QVERIFY(std::abs(plan->gapMm - 5.0) < 1e-6);
    QVERIFY(std::abs(plan->startMm - 25.0) < 1e-6);
    QCOMPARE(plan->centresMm.size(), 3);
    QVERIFY(std::abs(plan->centresMm.at(0).x() - 45.0) < 1e-6);
    QVERIFY(std::abs(plan->centresMm.at(1).x() - 100.0) < 1e-6);
    QVERIFY(std::abs(plan->centresMm.at(2).x() - 160.0) < 1e-6);
    QVERIFY(std::abs(plan->centresMm.at(0).y() - 148.5) < 1e-6);

    dialog.accept();
    QVERIFY(dialog.hasEdit());
    const QList<Frame> after = dialog.edit().pages.first().frames.value();
    // Sizes and order survive; only the centres move.
    QCOMPARE(after.at(0).width, 40.0);
    QCOMPARE(after.at(1).width, 60.0);
    QCOMPARE(after.at(2).width, 50.0);
    QVERIFY(std::abs(after.at(0).cx - 45.0) < 1e-6);
    QVERIFY(std::abs(after.at(1).cx - 100.0) < 1e-6);
    QVERIFY(std::abs(after.at(2).cx - 160.0) < 1e-6);
}

void TestSheetUi::distributeShrinksGapAndRefusesOverflow()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("gap.png"), QColor(120, 90, 60), QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    QList<Frame> frames;
    frames.append(frameAt(photo, sha, 40.0, 148.5, 40.0, 30.0));
    frames.append(frameAt(photo, sha, 110.0, 148.5, 60.0, 30.0));
    frames.append(frameAt(photo, sha, 175.0, 148.5, 50.0, 30.0));
    const DocumentSnapshot document = singlePage(a4(), frames);

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt,
                            previewsFor(document));
    auto *gapMode = dialog.findChild<QRadioButton *>(QStringLiteral("alignGapMode"));
    auto *margin = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignMargin"));
    auto *gap = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("alignGap"));
    QVERIFY(gapMode && margin && gap);
    gapMode->setChecked(true);
    margin->setValue(10.0);
    gap->setValue(30.0);
    const auto shrunk = dialog.distributePlanForTest();
    QVERIFY(shrunk.has_value());
    QVERIFY(std::abs(shrunk->gapMm - 20.0) < 1e-6);
    QVERIFY(std::abs(shrunk->startMm - 10.0) < 1e-6);
    QVERIFY2(dialog.warningTextForTest().contains(QStringLiteral("20")),
             qPrintable(dialog.warningTextForTest()));

    // Three 80 mm photos cannot fit a 190 mm span even at zero gap.
    QList<Frame> wide;
    for (int i = 0; i < 3; ++i)
        wide.append(frameAt(photo, sha, 40.0 + i * 85.0, 148.5, 80.0, 30.0));
    const DocumentSnapshot tooWide = singlePage(a4(), wide);
    SheetToolsDialog refused(SheetToolsDialog::Mode::Align, tooWide, 0, std::nullopt,
                             previewsFor(tooWide));
    auto *refusedGapMode = refused.findChild<QRadioButton *>(QStringLiteral("alignGapMode"));
    auto *refusedMargin = refused.findChild<QDoubleSpinBox *>(QStringLiteral("alignMargin"));
    QVERIFY(refusedGapMode && refusedMargin);
    refusedGapMode->setChecked(true);
    refusedMargin->setValue(10.0);
    QVERIFY(!refused.createEnabledForTest());
    QVERIFY2(refused.summaryTextForTest().contains(QStringLiteral("nem férnek el")),
             qPrintable(refused.summaryTextForTest()));
}

void TestSheetUi::distributeRefusesRotatedFrame()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("rotated.png"), QColor(60, 90, 120), QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    Frame tilted = frameAt(photo, sha, 60.0, 100.0, 40.0, 30.0);
    tilted.angleDeg = 10.0;
    const DocumentSnapshot document = singlePage(a4(), {tilted});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt,
                            previewsFor(document));
    auto *gapMode = dialog.findChild<QRadioButton *>(QStringLiteral("alignGapMode"));
    QVERIFY(gapMode);
    gapMode->setChecked(true);
    QVERIFY(!dialog.createEnabledForTest());
    QVERIFY2(dialog.summaryTextForTest().contains(QStringLiteral("el van forgatva")),
             qPrintable(dialog.summaryTextForTest()));
}

void TestSheetUi::cutGuidesScreenOverlayDiffersFromPrint()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A dark photo makes the overlay visible on both a dashed and a solid edge.
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("guides.png"), QColor(30, 30, 30), QSize(900, 600));

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(80);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, QStringList{photo})));
    QVERIFY(window.waitForPreview(15000));
    QTest::qWait(200);

    auto *view = window.findChild<PageView *>();
    auto *combo = window.findChild<QComboBox *>(QStringLiteral("cutGuidesCombo"));
    QVERIFY(view && combo);
    QCOMPARE(combo->count(), 3);
    QCOMPARE(window.document().pages.at(0).cutGuides, CutGuides::None);

    const Page page = window.document().pages.at(0);
    QCOMPARE(page.frames.size(), 1);
    const Frame frame = page.frames.first();
    const double leftMm = frame.cx - frame.width / 2.0;
    const double topMm = frame.cy - frame.height / 2.0;
    const double scaleX = view->pageRect().width() / view->pageMm().width();
    const double scaleY = view->pageRect().height() / view->pageMm().height();
    const auto toWidget = [&](double xMm, double yMm) {
        return QPointF(view->pageRect().left() + xMm * scaleX,
                       view->pageRect().top() + yMm * scaleY);
    };

    // A guide pixel is one that differs from the same pixel in the guide-free
    // grab: the photograph's own antialiased edge is identical in both.
    const auto countChanged = [&](const QImage &image, const QImage &bare) {
        int changed = 0;
        int total = 0;
        for (double yMm = topMm + 4.0; yMm < topMm + frame.height - 4.0; yMm += 1.0) {
            const QPointF widget = toWidget(leftMm, yMm);
            const int px = std::clamp(int(std::floor(widget.x())), 0, image.width() - 1);
            const int py = std::clamp(int(std::round(widget.y())), 0, image.height() - 1);
            ++total;
            const auto differs = [](QRgb a, QRgb b) {
                return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b))
                        + std::abs(qBlue(a) - qBlue(b))
                    > 40;
            };
            // A 1 px line can land on either side of the photograph's own
            // antialiased edge, so a narrow window decides whether this row
            // carries ink at all.
            bool ink = false;
            for (int dx = -2; dx <= 2 && !ink; ++dx) {
                const int sx = std::clamp(px + dx, 0, image.width() - 1);
                ink = differs(image.pixel(sx, py), bare.pixel(sx, py));
            }
            if (ink)
                ++changed;
        }
        return std::pair<int, int>(changed, total);
    };

    // None: no mark at all.
    view->setSelectedFrame(QString());
    QTest::qWait(60);
    const QImage bare = view->grab().toImage();
    const auto [noneInk, total] = countChanged(bare, bare);
    QCOMPARE(noneInk, 0);
    QVERIFY(total > 20);

    // Screen: a dashed accent overlay, so some rows are ink and some are not.
    combo->setCurrentIndex(combo->findData(int(CutGuides::Screen)));
    QTest::qWait(120);
    QCOMPARE(window.document().pages.at(0).cutGuides, CutGuides::Screen);
    view->setSelectedFrame(QString());
    QTest::qWait(60);
    const QImage dashed = view->grab().toImage();
    const auto [screenInk, dashedTotal] = countChanged(dashed, bare);
    QVERIFY2(screenInk > 0, "the screen guide drew nothing");
    QVERIFY2(screenInk < dashedTotal, "a dashed guide drew a solid line");

    // Print: the same geometry, solid grey.
    combo->setCurrentIndex(combo->findData(int(CutGuides::Print)));
    QTest::qWait(120);
    QCOMPARE(window.document().pages.at(0).cutGuides, CutGuides::Print);
    view->setSelectedFrame(QString());
    QTest::qWait(60);
    const QImage solid = view->grab().toImage();
    const auto [printInk, solidTotal] = countChanged(solid, bare);
    QVERIFY2(printInk >= solidTotal - 1,
             qPrintable(QStringLiteral("the print guide is not a continuous line: %1 of %2 "
                                       "(screen %3 of %4)")
                            .arg(printInk)
                            .arg(solidTotal)
                            .arg(screenInk)
                            .arg(dashedTotal)));

    // Evidence for the two screen states with the photo still selected: the
    // orange dashed guide has to read as a guide next to the blue selection.
    combo->setCurrentIndex(combo->findData(int(CutGuides::Screen)));
    QTest::qWait(120);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/sheet-guides-screen.png")));
    combo->setCurrentIndex(combo->findData(int(CutGuides::Print)));
    QTest::qWait(120);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/sheet-guides-print.png")));
}

void TestSheetUi::cutGuidesPrintInkIsHalfGrey()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("white.png"), QColor(255, 255, 255), QSize(600, 800));
    const QByteArray sha = fileSha256(photo);
    QVERIFY(!sha.isEmpty());
    const Frame frame = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    DocumentSnapshot document = singlePage(tenByFifteen(), {frame});

    ImageStore store;
    PageRenderer renderer(store);
    const double ppi = 300.0;
    const double edgeY = frame.cy - frame.height / 2.0;
    const double edgeX = frame.cx;

    document.pages[0].cutGuides = CutGuides::None;
    auto none = renderer.renderPage(document, 0, ppi);
    QVERIFY2(none.ok(), qPrintable(why(none)));
    const int noneLuma = lineLuma(none.value(), edgeX, edgeY, ppi);
    QVERIFY2(noneLuma > 245, qPrintable(QString::number(noneLuma)));

    document.pages[0].cutGuides = CutGuides::Screen;
    auto screen = renderer.renderPage(document, 0, ppi);
    QVERIFY2(screen.ok(), qPrintable(why(screen)));
    const int screenLuma = lineLuma(screen.value(), edgeX, edgeY, ppi);
    QVERIFY2(screenLuma > 245, qPrintable(QString::number(screenLuma)));

    document.pages[0].cutGuides = CutGuides::Print;
    auto printed = renderer.renderPage(document, 0, ppi);
    QVERIFY2(printed.ok(), qPrintable(why(printed)));
    const int printedLuma = lineLuma(printed.value(), edgeX, edgeY, ppi);
    // #7F7F7F is 127; the antialiased 0.2 mm line may blend a step either way.
    QVERIFY2(printedLuma >= 105 && printedLuma <= 150,
             qPrintable(QString::number(printedLuma)));
    QVERIFY(printedLuma < noneLuma);
    // The ink is thin: a couple of millimetres inside the copy is white again.
    const int insideLuma = lineLuma(printed.value(), edgeX, edgeY + 3.0, ppi);
    QVERIFY2(insideLuma > 245, qPrintable(QString::number(insideLuma)));

    // Evidence: the printed sheet as the PDF sees it.
    const QString path = evidenceDir() + QStringLiteral("/sheet-guides-print-ink.png");
    QVERIFY(printed.value().save(path));
}

void TestSheetUi::printGuardNamesPrintedPages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("guard.png"), QColor(70, 90, 110), QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    Frame frame = frameAt(photo, sha, 105.0, 148.5, 100.0, 120.0);
    DocumentSnapshot document;
    Page first;
    first.id = makeId(QStringLiteral("page"));
    first.paper = a4();
    first.frames = {frame};
    Page second = first;
    second.id = makeId(QStringLiteral("page"));
    second.cutGuides = CutGuides::Print;
    Page third = first;
    third.id = makeId(QStringLiteral("page"));
    third.cutGuides = CutGuides::Screen;
    document.pages = {first, second, third};
    document.currentPage = 0;

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));

    QVERIFY(requiresCutGuideConfirmation(window.document()));
    const QList<int> printed = pagesPrintingCutGuides(window.document());
    QCOMPARE(printed, QList<int>{2});

    // No: the job must not proceed, and the box named the page list.
    QString boxText;
    {
        QTimer timer;
        timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &timer, [&boxText] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                boxText = box->text();
                if (QAbstractButton *no = box->button(QMessageBox::No))
                    no->click();
            }
        });
        timer.start();
        QVERIFY(!window.confirmCutGuides());
    }
    QVERIFY2(boxText.contains(QStringLiteral("2")), qPrintable(boxText));

    // Yes: the same question, answered the other way.
    {
        QTimer timer;
        timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &timer, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                if (QAbstractButton *yes = box->button(QMessageBox::Yes))
                    yes->click();
            }
        });
        timer.start();
        QVERIFY(window.confirmCutGuides());
    }

    // A screen-only page never asks: no modal appeared, so the timer below
    // never clicked anything and the answer was yes without a question.
    ApplyLayoutEdit clear;
    PageLayout change;
    change.pageIndex = 1;
    change.cutGuides = CutGuides::Screen;
    clear.pages.append(change);
    clear.selectPage = 0;
    clear.actionText = QStringLiteral("Vágójelek");
    QVERIFY(window.applySheetEdit(clear).ok());
    QVERIFY(!requiresCutGuideConfirmation(window.document()));
    int clicks = 0;
    {
        QTimer timer;
        timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &timer, [&clicks] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                ++clicks;
                if (QAbstractButton *no = box->button(QMessageBox::No))
                    no->click();
            }
        });
        timer.start();
        QVERIFY(window.confirmCutGuides());
        QTest::qWait(80);
    }
    QCOMPARE(clicks, 0);
}

void TestSheetUi::guidesComboIsUndoable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("combo.png"), QColor(80, 100, 70), QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    const DocumentSnapshot document =
        singlePage(a4(), {frameAt(photo, sha, 105.0, 148.5, 100.0, 120.0)});

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));

    auto *combo = window.findChild<QComboBox *>(QStringLiteral("cutGuidesCombo"));
    auto *align = window.findChild<QPushButton *>(QStringLiteral("alignPhotosButton"));
    QVERIFY(combo && align);
    QCOMPARE(combo->currentIndex(), 0);
    // A single photo is a legal align: it is centred without resizing.
    QVERIFY(align->isEnabled());

    combo->setCurrentIndex(combo->findData(int(CutGuides::Print)));
    QCOMPARE(window.document().pages.at(0).cutGuides, CutGuides::Print);
    QCOMPARE(window.undoStackForTest()->undoText(), QStringLiteral("Vágójelek"));
    window.undoStackForTest()->undo();
    QCOMPARE(window.document().pages.at(0).cutGuides, CutGuides::None);
    QCOMPARE(combo->currentIndex(), 0);

    // The batch entry is enabled only when a photo is selected.
    auto *batch = window.findChild<QPushButton *>(QStringLiteral("batchCopiesButton"));
    QVERIFY(batch);
    const QString frameId = window.document().pages.at(0).frames.at(0).id;
    QVERIFY(QMetaObject::invokeMethod(&window, "selectFrame", Qt::DirectConnection,
                                      Q_ARG(QString, frameId), Q_ARG(bool, false)));
    QVERIFY(batch->isEnabled());
}

void TestSheetUi::unreadableSourceDisablesCreate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("missing.png"), QColor(100, 100, 100), QSize(400, 500));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document), false);
    QVERIFY(!dialog.createEnabledForTest());
    QCOMPARE(dialog.summaryTextForTest(),
             QStringLiteral("A kiválasztott fotó nem olvasható."));
    dialog.accept();
    QVERIFY(!dialog.hasEdit());
}

void TestSheetUi::unavailableFramesFeedTheBatchDialog()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A file that exists but cannot be decoded: the preview pass marks the frame
    // unavailable, and that set is what disables the batch action. A missing
    // file would raise the project's find-the-photo prompt instead.
    const QString brokenPath = dir.filePath(QStringLiteral("broken.png"));
    {
        QFile bad(brokenPath);
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("this is not a png");
    }
    Frame broken;
    broken.id = makeId(QStringLiteral("frame"));
    broken.sourcePath = brokenPath;
    broken.sourceSha256 = fileSha256(brokenPath);
    broken.cx = 50.0;
    broken.cy = 75.0;
    broken.width = 35.0;
    broken.height = 45.0;
    broken.fit = FitMode::Fill;
    broken.autoPlaced = false;
    const DocumentSnapshot document = singlePage(tenByFifteen(), {broken});

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));
    QVERIFY(window.unavailableFramesForTest().contains(broken.id));

    const bool readable = !window.unavailableFramesForTest().contains(broken.id);
    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, window.document(), 0, broken,
                            window.previewImagesForTest(), readable, &window);
    QVERIFY(!dialog.createEnabledForTest());
    QCOMPARE(dialog.summaryTextForTest(),
             QStringLiteral("A kiválasztott fotó nem olvasható."));
}

void TestSheetUi::batchPreviewNavigatesSheets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("nav.png"), QColor(80, 120, 150), QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document));
    auto *fill = dialog.findChild<QCheckBox *>(QStringLiteral("batchFill"));
    auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    auto *label = dialog.findChild<QLabel *>(QStringLiteral("sheetPageLabel"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("sheetNextPage"));
    auto *prev = dialog.findChild<QPushButton *>(QStringLiteral("sheetPrevPage"));
    auto *create = dialog.findChild<QPushButton *>(QStringLiteral("sheetCreateButton"));
    auto *preview = dialog.findChild<SheetPreview *>(QStringLiteral("sheetPreview"));
    QVERIFY(fill && count && label && next && prev && create && preview);
    fill->setChecked(false);
    count->setValue(20);

    // Four sheets, and the action names the count on the button.
    QVERIFY(label->isVisible() || !label->isHidden());
    QCOMPARE(label->text(), QStringLiteral("1/4. lap"));
    QVERIFY2(create->text().contains(QStringLiteral("4")), qPrintable(create->text()));
    QVERIFY(dialog.lastSheetNoteVisibleForTest());

    next->click();
    QCOMPARE(label->text(), QStringLiteral("2/4. lap"));
    next->click();
    next->click();
    QCOMPARE(label->text(), QStringLiteral("4/4. lap"));
    // The last sheet holds the remainder, two copies.
    QVERIFY2(preview->description().endsWith(QStringLiteral("2")),
             qPrintable(preview->description()));
    QVERIFY(!next->isEnabled());
    prev->click();
    QCOMPARE(label->text(), QStringLiteral("3/4. lap"));
}

void TestSheetUi::batchGuideOverwriteIsStated()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("guides-warning.png"), QColor(120, 90, 90),
                  QSize(600, 900));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(tenByFifteen(), {source});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document));
    auto *guides = dialog.findChild<QComboBox *>(QStringLiteral("batchGuides"));
    QVERIFY(guides);
    QCOMPARE(document.pages.at(0).cutGuides, CutGuides::None);
    guides->setCurrentIndex(guides->findData(int(CutGuides::Print)));
    QVERIFY2(dialog.warningTextForTest().contains(QStringLiteral("vágójel-beállítása")),
             qPrintable(dialog.warningTextForTest()));
    QVERIFY2(dialog.warningTextForTest().contains(QStringLiteral("Nyomtatás")),
             qPrintable(dialog.warningTextForTest()));
    dialog.accept();
    QVERIFY(dialog.hasEdit());
    QCOMPARE(dialog.edit().pages.first().cutGuides.value(), CutGuides::Print);
}

void TestSheetUi::fillCapIsNamed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("capname.png"), QColor(90, 110, 130), QSize(400, 400));
    const QByteArray sha = fileSha256(photo);
    const Frame source = frameAt(photo, sha, 42.0, 44.0, 35.0, 45.0);
    const DocumentSnapshot document = singlePage(a4(), {source});

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0, source,
                            previewsFor(document));
    auto *width = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("batchWidthCm"));
    auto *height = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("batchHeightCm"));
    auto *gutter = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("batchGutter"));
    QVERIFY(width && height && gutter);
    width->setValue(1.0);
    height->setValue(1.0);
    gutter->setValue(0.0);
    const auto grid = dialog.batchGridForTest();
    QVERIFY(grid.has_value());
    // A4 holds 580 of these cells, but one sheet is capped at 100.
    QCOMPARE(grid->perPage, 100);
    QVERIFY2(dialog.summaryTextForTest().contains(QStringLiteral("legfeljebb 100")),
             qPrintable(dialog.summaryTextForTest()));
}

void TestSheetUi::alignDoesNotClaimReplacement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("align-replace.png"), QColor(90, 120, 90),
                  QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    QList<Frame> frames;
    for (int i = 0; i < 2; ++i)
        frames.append(frameAt(photo, sha, 60.0 + i * 80.0, 100.0, 50.0, 40.0));
    const DocumentSnapshot document = singlePage(a4(), frames);

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt,
                            previewsFor(document));
    dialog.accept();
    QVERIFY(dialog.hasEdit());
    // Align edits a page but keeps its frames' identity; it is not the
    // destructive replacement the batch asks about.
    QVERIFY(!dialog.replacesCurrentPage());
}

void TestSheetUi::borderlessComboIsUndoable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("borderless-combo.png"), QColor(90, 90, 120),
                  QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    const DocumentSnapshot document =
        singlePage(tenByFifteen(), {frameAt(photo, sha, 50.0, 75.0, 35.0, 45.0)});

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));

    auto *combo = window.findChild<QComboBox *>(QStringLiteral("borderlessCombo"));
    QVERIFY(combo);
    QCOMPARE(combo->currentIndex(), 0);
    combo->setCurrentIndex(combo->findData(int(BorderlessOverride::On)));
    QCOMPARE(window.document().pages.at(0).borderlessOverride, BorderlessOverride::On);
    QCOMPARE(window.undoStackForTest()->undoText(), QStringLiteral("Szélig nyomtatás"));
    window.undoStackForTest()->undo();
    QCOMPARE(window.document().pages.at(0).borderlessOverride, BorderlessOverride::Auto);
    QCOMPARE(combo->currentIndex(), 0);

    combo->setCurrentIndex(combo->findData(int(BorderlessOverride::Off)));
    QCOMPARE(window.document().pages.at(0).borderlessOverride, BorderlessOverride::Off);
}

void TestSheetUi::borderlessConsistencyIsEnforced()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("borderless-pages.png"), QColor(70, 110, 90),
                  QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    DocumentSnapshot document;
    for (int i = 0; i < 2; ++i) {
        Page page;
        page.id = makeId(QStringLiteral("page"));
        page.paper = tenByFifteen();
        page.frames = {frameAt(photo, sha, 50.0, 75.0, 35.0, 45.0)};
        document.pages.append(page);
    }
    document.pages[0].borderlessOverride = BorderlessOverride::On;
    document.pages[1].borderlessOverride = BorderlessOverride::Off;

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));
    QVERIFY(!window.borderlessConsistentForTest());

    ApplyLayoutEdit unify;
    for (int i = 0; i < 2; ++i) {
        PageLayout change;
        change.pageIndex = i;
        change.borderlessOverride = BorderlessOverride::Off;
        unify.pages.append(change);
    }
    unify.actionText = QStringLiteral("Szélig nyomtatás");
    QVERIFY(window.applySheetEdit(unify).ok());
    QVERIFY(window.borderlessConsistentForTest());
}

void TestSheetUi::printGuardNamesTheGivenPages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("guard-numbers.png"), QColor(80, 90, 110),
                  QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    DocumentSnapshot document;
    for (int i = 0; i < 3; ++i) {
        Page page;
        page.id = makeId(QStringLiteral("page"));
        page.paper = a4();
        page.frames = {frameAt(photo, sha, 105.0, 148.5, 100.0, 120.0)};
        document.pages.append(page);
    }
    document.pages[2].cutGuides = CutGuides::Print;

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));

    QString boxText;
    QTimer timer;
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, &timer, [&boxText] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            boxText = box->text();
            if (QAbstractButton *no = box->button(QMessageBox::No))
                no->click();
        }
    });
    timer.start();
    // The export path names the document's pages, so a current-page export of
    // page 3 stays "3" instead of being renumbered to 1.
    QVERIFY(!window.confirmCutGuides(window.document(), QList<int>{4, 7}));
    timer.stop();
    QVERIFY2(boxText.contains(QStringLiteral("4, 7")), qPrintable(boxText));
}

void TestSheetUi::guideHintsFollowTheSelection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("hints.png"), QColor(90, 110, 80), QSize(600, 400));
    const QByteArray sha = fileSha256(photo);
    const DocumentSnapshot document =
        singlePage(a4(), {frameAt(photo, sha, 105.0, 148.5, 100.0, 120.0)});

    MainWindow window;
    window.resize(1100, 760);
    window.show();
    QTest::qWait(60);
    QVERIFY(loadDocument(window, dir, document));
    auto *combo = window.findChild<QComboBox *>(QStringLiteral("cutGuidesCombo"));
    auto *hint = window.findChild<QLabel *>(QStringLiteral("cutGuidesHint"));
    QVERIFY(combo && hint);
    QVERIFY2(hint->text().contains(QStringLiteral("Nincs vágójel")), qPrintable(hint->text()));
    combo->setCurrentIndex(combo->findData(int(CutGuides::Screen)));
    QVERIFY2(hint->text().contains(QStringLiteral("Csak a képernyőn")), qPrintable(hint->text()));
    combo->setCurrentIndex(combo->findData(int(CutGuides::Print)));
    QVERIFY2(hint->text().contains(QStringLiteral("megerősítést kér")), qPrintable(hint->text()));

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0,
                            document.pages.at(0).frames.at(0), previewsFor(document));
    auto *batchGuides = dialog.findChild<QComboBox *>(QStringLiteral("batchGuides"));
    auto *batchHint = dialog.findChild<QLabel *>(QStringLiteral("batchGuideHint"));
    QVERIFY(batchGuides && batchHint);
    batchGuides->setCurrentIndex(batchGuides->findData(int(CutGuides::Print)));
    QVERIFY2(batchHint->text().contains(QStringLiteral("megerősítést kér")),
             qPrintable(batchHint->text()));
}

void TestSheetUi::borderlessOverridePinsExactCopySize()
{
    const Page page = [&] {
        Page created;
        created.paper = tenByFifteen();
        created.frames = {frameAt(QStringLiteral("/tmp/none.png"), QByteArray("sha"), 31.0, 27.0,
                                  35.0, 45.0)};
        return created;
    }();

    // The tri-state rule: Off wins over a borderless printer, Auto follows it,
    // On pins it.
    QVERIFY(resolvedBorderless(page, true));
    QVERIFY(!resolvedBorderless(page, false));
    Page off = page;
    off.borderlessOverride = BorderlessOverride::Off;
    QVERIFY(!resolvedBorderless(off, true));
    QVERIFY(!resolvedBorderless(off, false));
    Page on = page;
    on.borderlessOverride = BorderlessOverride::On;
    QVERIFY(resolvedBorderless(on, false));
    QVERIFY(resolvedBorderless(on, true));

    // A driver that accepts the 4x6 borderless variant.
    PrinterCapabilities capabilities;
    capabilities.queueName = QStringLiteral("TestQueue");
    capabilities.driverIdentity = QStringLiteral("test");
    PageSizeCapability bordered;
    bordered.option = QStringLiteral("4x6");
    bordered.displayName = QStringLiteral("4x6");
    // The driver's 4x6 page is the logical sheet, 1.6 mm larger than the
    // declared physical paper; that is exactly why the declared size wins.
    bordered.widthMm = 101.6;
    bordered.heightMm = 152.4;
    bordered.borderless = false;
    PageSizeCapability bleed = bordered;
    bleed.option = QStringLiteral("4x6.Borderless");
    bleed.displayName = QStringLiteral("4x6 Borderless");
    bleed.borderless = true;
    capabilities.pageSizes = {bordered, bleed};
    capabilities.mediaTypes = {MediaTypeCapability{QStringLiteral("PhotographicGlossy"),
                                                  QStringLiteral("Glossy"), true}};
    capabilities.qualityOptions = {QStringLiteral("Fine")};
    capabilities.colorOptions = {QStringLiteral("FullColor")};
    capabilities.duplexOptions = {QStringLiteral("None")};

    PrintSelection selection;
    selection.queueName = capabilities.queueName;
    selection.paper = page.paper;
    selection.fineQuality = true;
    selection.color = true;
    selection.copies = 1;
    selection.borderless = resolvedBorderless(off, true);
    const auto exact = PrinterAdapter::resolve(capabilities, selection);
    QVERIFY2(exact.ok(), qPrintable(why(exact)));
    QVERIFY(!exact.value().geometry.borderless);
    // The print and export paths only scale when the ticket is borderless, so a
    // bordered batch sheet keeps contentScale 1.
    const double exactScale = exact.value().geometry.borderless ? 1.016 : 1.0;
    QCOMPARE(exactScale, 1.0);
    // 35 x 45 stays 35 x 45: no bleed enlargement stretches it.
    const double exactWidth = 35.0 * exactScale;
    const double exactHeight = 45.0 * exactScale;
    QVERIFY(std::abs(exactWidth - 35.0) < 1e-9);
    QVERIFY(std::abs(exactHeight - 45.0) < 1e-9);

    // Auto on the same printer is borderless, and the bleed scales the copy by
    // the 10 x 15 cover factor: the 35 x 45 copy would print 35.6 x 45.7.
    selection.borderless = resolvedBorderless(page, true);
    const auto bleedTicket = PrinterAdapter::resolve(capabilities, selection);
    QVERIFY2(bleedTicket.ok(), qPrintable(why(bleedTicket)));
    QVERIFY(bleedTicket.value().geometry.borderless);
    const BorderlessPlacement bleedPlacement =
        borderlessPlacement(bleedTicket.value().geometry.physicalSizeMm(),
                            bleedTicket.value().geometry.logicalSizeMm(),
                            bleedTicket.value().geometry.physicalOriginInLogicalMm);
    QVERIFY(bleedPlacement.scale > 1.01);
    QVERIFY(std::abs(35.0 * bleedPlacement.scale - 35.6) < 0.1);
}

void TestSheetUi::verifyOncePerPrintJob()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makeNoisyPhoto(dir.path(), QStringLiteral("many-copies.png"), QSize(1200, 900));
    const QByteArray sha = fileSha256(photo);
    QVERIFY(!sha.isEmpty());

    SheetLayout::BatchSpec spec;
    spec.copyWidthMm = 35.0;
    spec.copyHeightMm = 45.0;
    spec.gutterMm = 3.0;
    spec.marginMm = SheetLayout::kMinMarginMm;
    spec.fillPage = false;
    spec.count = 100;
    Page target;
    target.id = makeId(QStringLiteral("page"));
    target.paper = tenByFifteen();
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const auto pages = SheetLayout::batchPages(target, spec, source);
    QVERIFY2(pages.ok(), qPrintable(why(pages)));
    QCOMPARE(pages.value().size(), 17);
    int copies = 0;
    for (const Page &page : pages.value())
        copies += page.frames.size();
    QCOMPARE(copies, 100);

    DocumentSnapshot document;
    document.pages = pages.value();
    document.currentPage = 0;

    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    options.verifySources = true;
    options.printPpi = 72.0;
    const QString pdfPath = dir.filePath(QStringLiteral("batch.pdf"));
    QElapsedTimer jobTimer;
    jobTimer.start();
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    const qint64 jobMs = jobTimer.elapsed();
    QVERIFY2(artifact.ok(), qPrintable(why(artifact)));
    QCOMPARE(artifact.value().pageCount, 17);
    QCOMPARE(renderer.verificationCount(), 1);

    // A direct render of one page of copies is one verification as well.
    renderer.resetVerificationCount();
    const auto single = renderer.renderPage(document, 0, 72.0, {}, options);
    QVERIFY2(single.ok(), qPrintable(why(single)));
    QCOMPARE(renderer.verificationCount(), 1);

    // Measurement: the job verifies once; the old path hashed once per copy.
    ImageStore naiveStore;
    QElapsedTimer naiveTimer;
    naiveTimer.start();
    for (int i = 0; i < copies; ++i) {
        const auto verified = naiveStore.reloadVerified(photo, sha);
        QVERIFY2(verified.ok(), qPrintable(why(verified)));
    }
    const qint64 naiveMs = naiveTimer.elapsed();

    ImageStore singleStore;
    QElapsedTimer singleTimer;
    singleTimer.start();
    const auto singleVerify = singleStore.reloadVerified(photo, sha);
    const qint64 singleMs = singleTimer.elapsed();
    QVERIFY2(singleVerify.ok(), qPrintable(why(singleVerify)));

    QFile report(evidenceDir() + QStringLiteral("/sheet-print-verify.txt"));
    QVERIFY(report.open(QIODevice::WriteOnly | QIODevice::Text));
    report.write(QStringLiteral("copies=%1 pages=%2\n").arg(copies).arg(artifact.value().pageCount).toUtf8());
    report.write(QStringLiteral("verifications_per_job=%1\n").arg(renderer.verificationCount()).toUtf8());
    report.write(QStringLiteral("job_ms=%1\n").arg(jobMs).toUtf8());
    report.write(QStringLiteral("single_verify_ms=%1\n").arg(singleMs).toUtf8());
    report.write(QStringLiteral("naive_100_verify_ms=%1\n").arg(naiveMs).toUtf8());
    report.write(QStringLiteral("source_bytes=%1\n").arg(QFileInfo(photo).size()).toUtf8());
    report.close();
}

void TestSheetUi::captureSheetToolDialogs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first =
        makePhoto(dir.path(), QStringLiteral("dialog-a.png"), QColor(70, 110, 160), QSize(900, 600));
    const QString second =
        makePhoto(dir.path(), QStringLiteral("dialog-b.png"), QColor(160, 110, 70), QSize(700, 700));
    const QString third =
        makePhoto(dir.path(), QStringLiteral("dialog-c.png"), QColor(90, 140, 90), QSize(900, 600));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(60);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, (QStringList{first, second, third}))));
    QVERIFY(window.waitForPreview(15000));
    QTest::qWait(250);

    DocumentSnapshot document = window.document();
    QCOMPARE(document.pages.at(0).frames.size(), 3);
    const Frame source = document.pages.at(0).frames.at(0);

    // Guides and selection together: the first photo is selected, the orange
    // dashed guides are on every photo, so both marks are visible at once.
    auto *windowGuides = window.findChild<QComboBox *>(QStringLiteral("cutGuidesCombo"));
    QVERIFY(windowGuides);
    QVERIFY(QMetaObject::invokeMethod(&window, "selectFrame", Qt::DirectConnection,
                                      Q_ARG(QString, source.id), Q_ARG(bool, false)));
    windowGuides->setCurrentIndex(windowGuides->findData(int(CutGuides::Screen)));
    QTest::qWait(150);
    QVERIFY(window.grab().save(
        evidenceDir() + QStringLiteral("/sheet-guides-selection.png")));
    windowGuides->setCurrentIndex(windowGuides->findData(int(CutGuides::None)));
    QTest::qWait(100);

    // Duplicate dialog with the feedback line and Create in view.
    SheetToolsDialog batch(SheetToolsDialog::Mode::Batch, document, 0, source,
                           window.previewImagesForTest(), true, &window);
    batch.resize(1020, 720);
    batch.show();
    QTest::qWait(200);
    QVERIFY(batch.grab().save(evidenceDir() + QStringLiteral("/sheet-batch-dialog.png")));

    // A multi-sheet batch on its last sheet: the page indicator, the last-sheet
    // note and the Create label all name the count.
    auto *fill = batch.findChild<QCheckBox *>(QStringLiteral("batchFill"));
    auto *count = batch.findChild<QSpinBox *>(QStringLiteral("batchCount"));
    auto *next = batch.findChild<QPushButton *>(QStringLiteral("sheetNextPage"));
    auto *pageLabel = batch.findChild<QLabel *>(QStringLiteral("sheetPageLabel"));
    QVERIFY(fill && count && next && pageLabel);
    fill->setChecked(false);
    count->setValue(100);
    while (next->isEnabled())
        next->click();
    QVERIFY2(pageLabel->text().endsWith(QStringLiteral("4/4. lap")),
             qPrintable(pageLabel->text()));
    QTest::qWait(150);
    QVERIFY(batch.grab().save(evidenceDir() + QStringLiteral("/sheet-batch-last-sheet.png")));

    // Align in grid mode on the three-photo page, before the batch replaces it.
    SheetToolsDialog align(SheetToolsDialog::Mode::Align, window.document(),
                           window.document().currentPage, std::nullopt,
                           window.previewImagesForTest(), true, &window);
    align.resize(1020, 720);
    align.show();
    QTest::qWait(220);
    QVERIFY(align.grab().save(evidenceDir() + QStringLiteral("/sheet-align-dialog.png")));
    align.close();

    // Distribute mode on a page of three differently sized photos, which is the
    // case it exists for.
    DocumentSnapshot distributeDoc;
    {
        Page page;
        page.id = makeId(QStringLiteral("page"));
        page.paper = a4();
        page.frames = {frameAt(first, fileSha256(first), 40.0, 148.5, 40.0, 30.0),
                       frameAt(second, fileSha256(second), 110.0, 148.5, 60.0, 30.0),
                       frameAt(third, fileSha256(third), 175.0, 148.5, 50.0, 30.0)};
        distributeDoc.pages.append(page);
    }
    SheetToolsDialog distribute(SheetToolsDialog::Mode::Align, distributeDoc, 0, std::nullopt,
                                previewsFor(distributeDoc, QSize(600, 400)), true, &window);
    auto *gapMode = distribute.findChild<QRadioButton *>(QStringLiteral("alignGapMode"));
    auto *gapMargin = distribute.findChild<QDoubleSpinBox *>(QStringLiteral("alignMargin"));
    auto *gapValue = distribute.findChild<QDoubleSpinBox *>(QStringLiteral("alignGap"));
    QVERIFY(gapMode && gapMargin && gapValue);
    gapMode->setChecked(true);
    gapMargin->setValue(10.0);
    gapValue->setValue(5.0);
    distribute.resize(1020, 720);
    distribute.show();
    QTest::qWait(220);
    QVERIFY(distribute.grab().save(
        evidenceDir() + QStringLiteral("/sheet-align-distribute.png")));
    distribute.close();

    // Apply the batch and show the canvas with the copies on it.
    count->setValue(20);
    batch.accept();
    QVERIFY(batch.hasEdit());
    const auto applied = window.applySheetEdit(batch.edit());
    QVERIFY2(applied.ok(), qPrintable(why(applied)));
    QVERIFY(window.waitForPreview(15000));
    QTest::qWait(300);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/sheet-batch-applied.png")));

    // Large font: the settings scroll, but the feedback line and Create stay in
    // the dialog, so the action is never pressed blind. The application font is
    // raised for the lifetime of this capture, which is what a user with a
    // larger system font sees.
    const QFont previousFont = QApplication::font();
    QFont largeFont = previousFont;
    largeFont.setPointSizeF(previousFont.pointSizeF() + 5.0);
    QApplication::setFont(largeFont);
    SheetToolsDialog large(SheetToolsDialog::Mode::Batch, window.document(),
                           window.document().currentPage,
                           window.document().pages.at(window.document().currentPage).frames.at(0),
                           window.previewImagesForTest(), true, &window);
    large.resize(1020, 760);
    large.show();
    QTest::qWait(250);
    auto *create = large.findChild<QPushButton *>(QStringLiteral("sheetCreateButton"));
    auto *summary = large.findChild<QLabel *>(QStringLiteral("sheetSummary"));
    QVERIFY(create && summary);
    QVERIFY(create->isVisible());
    QVERIFY(summary->isVisible());
    QVERIFY2(QFontMetrics(summary->font()).height() > 20,
             qPrintable(QString::number(QFontMetrics(summary->font()).height())));
    QVERIFY(large.grab().save(evidenceDir() + QStringLiteral("/sheet-batch-large-font.png")));
    large.close();
    QApplication::setFont(previousFont);

    // Print guard: the modal itself as evidence.
    auto *guides = window.findChild<QComboBox *>(QStringLiteral("cutGuidesCombo"));
    QVERIFY(guides);
    guides->setCurrentIndex(guides->findData(int(CutGuides::Print)));
    QTest::qWait(120);
    const QString guardPath = evidenceDir() + QStringLiteral("/sheet-print-guard.png");
    QTimer guardTimer;
    guardTimer.setInterval(10);
    connect(&guardTimer, &QTimer::timeout, &guardTimer, [&guardPath] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            box->grab().save(guardPath);
            if (QAbstractButton *no = box->button(QMessageBox::No))
                no->click();
        }
    });
    guardTimer.start();
    QVERIFY(!window.confirmCutGuides());
    guardTimer.stop();
    QVERIFY(QFileInfo::exists(guardPath));
}

QTEST_MAIN(TestSheetUi)
#include "tst_sheet_ui.moc"
