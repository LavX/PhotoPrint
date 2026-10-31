#include "Document.h"
#include "EffectBrowserDialog.h"
#include "EffectCatalogue.h"
#include "MainWindow.h"
#include "ProjectStore.h"

#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageWriter>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

using namespace pp;

Q_DECLARE_METATYPE(pp::LoadedProject)

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

void writeTiming(const QString &name, qint64 milliseconds)
{
    QFile file(evidenceDir() + QStringLiteral("/effect-browser-timing.txt"));
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;
    file.write(QStringLiteral("%1 %2 ms\n").arg(name).arg(milliseconds).toUtf8());
}

/// A real photograph is not needed for the tile tests; this has enough colour
/// for every effect to produce a different tile.
QImage sampleImage()
{
    QImage image(480, 320, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixel(x, y, qRgb(x * 255 / image.width(),
                                      y * 255 / image.height(),
                                      160 - (x + y) * 80 / (image.width() + image.height())));
        }
    }
    return image;
}

EffectBrowserRequest requestWith(const QString &key, int strength = 100)
{
    EffectBrowserRequest request;
    request.currentKey = key;
    request.currentStrength = strength;
    request.sample = sampleImage();
    request.sampleHash = QStringLiteral("test-sample");
    return request;
}

QListWidget *categoryList(EffectBrowserDialog &dialog)
{
    return dialog.findChild<QListWidget *>(QStringLiteral("categoryList"));
}

bool selectCategory(EffectBrowserDialog &dialog, const QString &key)
{
    QListWidget *list = categoryList(dialog);
    if (!list)
        return false;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole).toString() == key) {
            list->setCurrentRow(row);
            return true;
        }
    }
    return false;
}

QAbstractButton *tile(EffectBrowserDialog &dialog, const QString &key)
{
    return dialog.findChild<QAbstractButton *>(QStringLiteral("effectTile_%1").arg(key));
}

QStringList makePhotos(const QString &directory)
{
    const QList<QPair<QColor, QSize>> specs = {
        {QColor(190, 70, 60), QSize(600, 400)},
        {QColor(60, 150, 90), QSize(400, 600)},
        {QColor(70, 90, 190), QSize(500, 500)},
    };
    QStringList paths;
    for (int i = 0; i < specs.size(); ++i) {
        const QString path = QDir(directory).filePath(QStringLiteral("photo-%1.png").arg(i + 1));
        QImage image(specs.at(i).second, QImage::Format_RGB32);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(specs.at(i).first);
        QImageWriter writer(path, "png");
        if (!writer.write(image))
            return {};
        paths.append(path);
    }
    return paths;
}

QAction *undoAction(MainWindow &window)
{
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->text().contains(QStringLiteral("Vissza")))
            return action;
    }
    return nullptr;
}

} // namespace

class TestEffectBrowser : public QObject
{
    Q_OBJECT

private slots:
    void categoriesHaveTenTilesAndOriginal();
    void searchFindsNameAndCategory();
    void originalTileAppliesOriginal();
    void doubleClickAppliesImmediately();
    void keyboardNavigationWalksTheGrid();
    void ditherRecipesDisableStrength();
    void slowAndApproximateFlagsAreSet();
    void applyWritesKeyVersionAndStrengthInOneUndo();
    void browserStrengthEditsTheFrameLive();
    void ditherEffectDisablesTheSideStrength();
    void countedResultLabelUsesPluralForms();
    void legacyKeysKeepTheirPreviousLookLabel();
    void legacyProjectShowsPreviousLookInThePanel();
    void browserOpensWithoutSynchronousRendering();
    void pagePreviewNoLongerBlocksTheGui();
};

void TestEffectBrowser::categoriesHaveTenTilesAndOriginal()
{
    EffectBrowserDialog dialog(requestWith(QStringLiteral("basic.neutral")));
    dialog.show();
    QVERIFY(dialog.waitForRendering());

    QCOMPARE(dialog.categoryCount(), 12);
    // The applied effect carries the check mark and the bold name.
    QVERIFY(dialog.tileMarkedCurrent(QStringLiteral("basic.neutral")));
    QVERIFY(!dialog.tileMarkedCurrent(QStringLiteral("basic.punch")));
    const QStringList labels = {QStringLiteral("Alap"),       QStringLiteral("Fekete-fehér"),
                                QStringLiteral("Retró"),      QStringLiteral("Szín"),
                                QStringLiteral("Elmosás"),    QStringLiteral("Élesség"),
                                QStringLiteral("Művészi"),    QStringLiteral("Torzítás"),
                                QStringLiteral("Fény"),       QStringLiteral("Szemcse"),
                                QStringLiteral("Nyomat"),     QStringLiteral("Ragyogás")};
    QListWidget *list = categoryList(dialog);
    QVERIFY(list);
    for (int row = 0; row < list->count(); ++row)
        QCOMPARE(list->item(row)->text(), labels.at(row));

    const QStringList categories = EffectCatalogue::instance().categoryKeys();
    QCOMPARE(categories.size(), 12);    for (const QString &category : categories) {
        QVERIFY2(selectCategory(dialog, category), qPrintable(category));
        QTest::qWait(10);
        QCOMPARE(dialog.visibleTileCount(), 11);
        const QStringList keys = dialog.visibleTileKeys();
        QCOMPARE(keys.first(), QStringLiteral("original"));
        QSet<QString> unique;
        for (int i = 1; i < keys.size(); ++i) {
            QVERIFY2(keys.at(i).startsWith(category + QLatin1Char('.')),
                     qPrintable(keys.at(i)));
            unique.insert(keys.at(i));
            QVERIFY2(!dialog.tileLabelFor(keys.at(i)).isEmpty(), qPrintable(keys.at(i)));
        }
        QCOMPARE(unique.size(), 10);
    }
}

void TestEffectBrowser::searchFindsNameAndCategory()
{
    EffectBrowserDialog dialog(requestWith(QStringLiteral("basic.neutral")));
    dialog.show();
    QVERIFY(dialog.waitForRendering());
    QLineEdit *search = dialog.findChild<QLineEdit *>(QStringLiteral("searchField"));
    QVERIFY(search);

    search->setText(QStringLiteral("Venice"));
    QTest::qWait(10);
    QCOMPARE(dialog.visibleTileKeys(), QStringList{QStringLiteral("light.venice")});

    // The translated category name searches its whole group, accents omitted.
    search->setText(QStringLiteral("fekete"));
    QTest::qWait(10);
    QCOMPARE(dialog.visibleTileKeys().size(), 10);
    for (const QString &key : dialog.visibleTileKeys())
        QVERIFY(key.startsWith(QStringLiteral("mono.")));

    search->setText(QStringLiteral("muveszi"));
    QTest::qWait(10);
    QCOMPARE(dialog.visibleTileKeys().size(), 10);
    for (const QString &key : dialog.visibleTileKeys())
        QVERIFY(key.startsWith(QStringLiteral("art.")));

    search->setText(QStringLiteral("zzzz"));
    QTest::qWait(10);
    QCOMPARE(dialog.visibleTileKeys().size(), 0);
    auto *count = dialog.findChild<QLabel *>(QStringLiteral("resultCount"));
    QVERIFY(count);
    QCOMPARE(count->text(), QStringLiteral("Nincs találat."));

    search->clear();
    QTest::qWait(10);
    QCOMPARE(dialog.visibleTileCount(), 11);
}

void TestEffectBrowser::originalTileAppliesOriginal()
{
    EffectBrowserDialog dialog(requestWith(QStringLiteral("mono.classic"), 40));
    dialog.show();
    QVERIFY(dialog.waitForRendering());

    QAbstractButton *original = tile(dialog, QStringLiteral("original"));
    QVERIFY(original);
    QTest::mouseClick(original, Qt::LeftButton, Qt::NoModifier, original->rect().center());
    QTest::qWait(10);
    QCOMPARE(dialog.selectedKey(), QStringLiteral("original"));
    QVERIFY(!dialog.strengthEnabled());
    QCOMPARE(dialog.strengthValue(), 100);

    QSignalSpy applied(&dialog, &EffectBrowserDialog::effectChosen);
    auto *apply = dialog.findChild<QPushButton *>(QStringLiteral("applyButton"));
    QVERIFY(apply);
    apply->click();
    QCOMPARE(applied.count(), 1);
    const QList<QVariant> arguments = applied.first();
    QCOMPARE(arguments.at(0).toString(), QStringLiteral("original"));
    QCOMPARE(arguments.at(1).toInt(), 0);
    QCOMPARE(arguments.at(2).toInt(), 100);
}

void TestEffectBrowser::doubleClickAppliesImmediately()
{
    EffectBrowserDialog dialog(requestWith(QStringLiteral("basic.neutral")));
    dialog.show();
    QVERIFY(dialog.waitForRendering());
    QAbstractButton *tile = dialog.findChild<QAbstractButton *>(
        QStringLiteral("effectTile_basic.bright"));
    QVERIFY(tile);
    QSignalSpy applied(&dialog, &EffectBrowserDialog::effectChosen);
    QTest::mouseDClick(tile, Qt::LeftButton, Qt::NoModifier, tile->rect().center());
    QCOMPARE(applied.count(), 1);
    QCOMPARE(applied.first().at(0).toString(), QStringLiteral("basic.bright"));
    QCOMPARE(applied.first().at(1).toInt(), 1);
    QCOMPARE(applied.first().at(2).toInt(), 100);
}

void TestEffectBrowser::keyboardNavigationWalksTheGrid()
{
    EffectBrowserDialog dialog(requestWith(QStringLiteral("basic.neutral")));
    dialog.show();
    QVERIFY(dialog.waitForRendering());

    QAbstractButton *first = tile(dialog, QStringLiteral("basic.neutral"));
    QVERIFY(first);
    QVERIFY(first->focusPolicy() & Qt::StrongFocus);
    dialog.activateWindow();
    first->setFocus(Qt::OtherFocusReason);
    QTRY_VERIFY(first->hasFocus());

    const QStringList keys = dialog.visibleTileKeys();
    QCOMPARE(keys.size(), 11);

    QTest::keyClick(first, Qt::Key_Right);
    QTRY_COMPARE(dialog.selectedKey(), keys.at(2));
    QAbstractButton *current = tile(dialog, dialog.selectedKey());
    QVERIFY(current);
    QTRY_VERIFY(current->hasFocus());
    QTest::keyClick(current, Qt::Key_Down);
    QTRY_COMPARE(dialog.selectedKey(), keys.at(7));
    current = tile(dialog, dialog.selectedKey());
    QVERIFY(current);
    QTest::keyClick(current, Qt::Key_Home);
    QTRY_COMPARE(dialog.selectedKey(), QStringLiteral("original"));
    current = tile(dialog, dialog.selectedKey());
    QVERIFY(current);
    QTest::keyClick(current, Qt::Key_End);
    QTRY_COMPARE(dialog.selectedKey(), keys.last());
    current = tile(dialog, dialog.selectedKey());
    QVERIFY(current);
    QTest::keyClick(current, Qt::Key_Left);
    QTRY_COMPARE(dialog.selectedKey(), keys.at(keys.size() - 2));
    current = tile(dialog, dialog.selectedKey());
    QVERIFY(current);
    QTest::keyClick(current, Qt::Key_Up);
    QTRY_COMPARE(dialog.selectedKey(), keys.at(keys.size() - 7));
}

void TestEffectBrowser::ditherRecipesDisableStrength()
{
    for (const QString &key : {QStringLiteral("print.halftone"), QStringLiteral("print.riso"),
                               QStringLiteral("print.news"),
                               QStringLiteral("retro.newsprint")}) {
        QVERIFY2(effectIgnoresStrength(key), qPrintable(key));
    }
    QVERIFY(!effectIgnoresStrength(QStringLiteral("basic.punch")));
    QVERIFY(!effectIgnoresStrength(QStringLiteral("original")));
    QCOMPARE(effectStrengthReason(),
             QStringLiteral("Ez az effekt teljes erősséggel jelenik meg."));

    EffectBrowserDialog dialog(requestWith(QStringLiteral("basic.punch")));
    dialog.show();
    QVERIFY(dialog.waitForRendering());
    QVERIFY(selectCategory(dialog, QStringLiteral("print")));
    QTest::qWait(10);
    QAbstractButton *halftone = tile(dialog, QStringLiteral("print.halftone"));
    QVERIFY(halftone);
    QTest::mouseClick(halftone, Qt::LeftButton, Qt::NoModifier, halftone->rect().center());
    QTest::qWait(10);
    QVERIFY(!dialog.strengthEnabled());
    QCOMPARE(dialog.strengthValue(), 100);
    QCOMPARE(dialog.strengthHintText(),
             QStringLiteral("Ez az effekt teljes erősséggel jelenik meg."));

    QSignalSpy applied(&dialog, &EffectBrowserDialog::effectChosen);
    dialog.findChild<QPushButton *>(QStringLiteral("applyButton"))->click();
    QCOMPARE(applied.count(), 1);
    QCOMPARE(applied.first().at(0).toString(), QStringLiteral("print.halftone"));
    QCOMPARE(applied.first().at(1).toInt(), 1);
    QCOMPARE(applied.first().at(2).toInt(), 100);
}

void TestEffectBrowser::slowAndApproximateFlagsAreSet()
{
    const EffectCatalogue &catalogue = EffectCatalogue::instance();
    for (const QString &key :
         {QStringLiteral("art.impasto"), QStringLiteral("detail.drybrush"),
          QStringLiteral("blur.motion"), QStringLiteral("warp.kaleido"),
          QStringLiteral("print.halftone"), QStringLiteral("grain.dust")}) {
        const auto recipe = catalogue.resolve(key);
        QVERIFY2(recipe.ok(), qPrintable(key));
        QVERIFY2(recipe.value().slow, qPrintable(key));
    }
    for (const QString &key :
         {QStringLiteral("print.halftone"), QStringLiteral("print.riso"),
          QStringLiteral("print.news"), QStringLiteral("retro.newsprint"),
          QStringLiteral("grain.dust"), QStringLiteral("grain.paper")}) {
        const auto recipe = catalogue.resolve(key);
        QVERIFY2(recipe.ok(), qPrintable(key));
        QVERIFY2(recipe.value().approximate, qPrintable(key));
    }
    // A clean point recipe is neither.
    const auto plain = catalogue.resolve(QStringLiteral("basic.neutral"));
    QVERIFY(plain.ok());
    QVERIFY(!plain.value().slow);
    QVERIFY(!plain.value().approximate);

    EffectBrowserDialog dialog(requestWith(QStringLiteral("basic.punch")));
    dialog.show();
    QVERIFY(dialog.waitForRendering());
    QVERIFY(selectCategory(dialog, QStringLiteral("art")));
    QTest::qWait(10);
    QAbstractButton *impasto = tile(dialog, QStringLiteral("art.impasto"));
    QVERIFY(impasto);
    QVERIFY(impasto->property("slow").toBool());
    QTest::mouseClick(impasto, Qt::LeftButton, Qt::NoModifier, impasto->rect().center());
    QTest::qWait(10);
    QCOMPARE(dialog.adjacentHintText(), QString());

    QVERIFY(selectCategory(dialog, QStringLiteral("print")));
    QTest::qWait(10);
    QAbstractButton *halftone = tile(dialog, QStringLiteral("print.halftone"));
    QVERIFY(halftone);
    QTest::mouseClick(halftone, Qt::LeftButton, Qt::NoModifier, halftone->rect().center());
    QTest::qWait(10);
    QCOMPARE(dialog.adjacentHintText(), QStringLiteral("Az előnézet hozzávetőleges"));
}

void TestEffectBrowser::applyWritesKeyVersionAndStrengthInOneUndo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    QCOMPARE(photos.size(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QVERIFY(window.waitForPreview(10000));

    auto *button = window.findChild<QPushButton *>(QStringLiteral("effectButton"));
    QVERIFY(button);
    button->click();
    QPointer<EffectBrowserDialog> dialog = window.findChild<EffectBrowserDialog *>();
    QVERIFY(dialog);
    QVERIFY(dialog->waitForRendering());

    QVERIFY(selectCategory(*dialog, QStringLiteral("basic")));
    QTest::qWait(10);
    QAbstractButton *punch = tile(*dialog, QStringLiteral("basic.punch"));
    QVERIFY(punch);
    QTest::mouseClick(punch, Qt::LeftButton, Qt::NoModifier, punch->rect().center());
    QTest::qWait(10);

    auto *strength = dialog->findChild<QSlider *>(QStringLiteral("strengthSlider"));
    QVERIFY(strength);
    QVERIFY(strength->isEnabled());
    strength->setValue(70);
    QTest::qWait(10);

    dialog->findChild<QPushButton *>(QStringLiteral("applyButton"))->click();
    QTRY_VERIFY(!dialog);

    const DocumentSnapshot after = window.document();
    QCOMPARE(after.pages.first().frames.size(), 3);
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const QString frameId = view->selectedFrame();
    const Frame *applied = findFrame(after, frameId);
    QVERIFY(applied);
    QCOMPARE(applied->filter, QStringLiteral("basic.punch"));
    QCOMPARE(applied->effectVersion, 1);
    QCOMPARE(applied->filterStrength, 70);
    QCOMPARE(button->text(), QStringLiteral("Punch"));

    QAction *undo = undoAction(window);
    QVERIFY(undo);
    QVERIFY(undo->isEnabled());
    undo->trigger();
    QTest::qWait(50);
    const DocumentSnapshot before = window.document();
    const Frame *restored = findFrame(before, frameId);
    QVERIFY(restored);
    QCOMPARE(restored->filter, QStringLiteral("original"));
    QCOMPARE(restored->effectVersion, 0);
    QCOMPARE(restored->filterStrength, 100);
    QCOMPARE(button->text(), QStringLiteral("Eredeti"));
}

void TestEffectBrowser::browserStrengthEditsTheFrameLive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos.mid(0, 1))));
    QVERIFY(window.waitForPreview(15000));

    auto *button = window.findChild<QPushButton *>(QStringLiteral("effectButton"));
    QVERIFY(button);

    // Apply one effect first: the strength slider only edits a real effect.
    button->click();
    QPointer<EffectBrowserDialog> dialog = window.findChild<EffectBrowserDialog *>();
    QVERIFY(dialog && dialog->waitForRendering());
    QAbstractButton *punch = tile(*dialog, QStringLiteral("basic.punch"));
    QVERIFY(punch);
    QTest::mouseClick(punch, Qt::LeftButton, Qt::NoModifier, punch->rect().center());
    dialog->findChild<QPushButton *>(QStringLiteral("applyButton"))->click();
    QTRY_VERIFY(!dialog);
    QVERIFY(window.waitForPreview(15000));

    button->click();
    dialog = window.findChild<EffectBrowserDialog *>();
    QVERIFY(dialog && dialog->waitForRendering());
    QVERIFY(dialog->strengthEnabled());
    auto *slider = dialog->findChild<QSlider *>(QStringLiteral("strengthSlider"));
    QVERIFY(slider);
    slider->setValue(35);
    QTest::qWait(80);
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    {
        const DocumentSnapshot snapshot = window.document();
        const Frame *frame = findFrame(snapshot, view->selectedFrame());
        QVERIFY(frame);
        QCOMPARE(frame->filter, QStringLiteral("basic.punch"));
        QCOMPARE(frame->filterStrength, 35);
    }
    // Closing the browser without Apply keeps the live strength edit.
    dialog->close();
    QTRY_VERIFY(!dialog);
    QVERIFY(window.waitForPreview(15000));
}

void TestEffectBrowser::ditherEffectDisablesTheSideStrength()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos.mid(0, 1))));
    QVERIFY(window.waitForPreview(15000));

    auto *button = window.findChild<QPushButton *>(QStringLiteral("effectButton"));
    QVERIFY(button);
    button->click();
    QPointer<EffectBrowserDialog> dialog = window.findChild<EffectBrowserDialog *>();
    QVERIFY(dialog && dialog->waitForRendering());
    QVERIFY(selectCategory(*dialog, QStringLiteral("print")));
    QTest::qWait(20);
    QAbstractButton *halftone = tile(*dialog, QStringLiteral("print.halftone"));
    QVERIFY(halftone);
    QTest::mouseClick(halftone, Qt::LeftButton, Qt::NoModifier, halftone->rect().center());
    dialog->findChild<QPushButton *>(QStringLiteral("applyButton"))->click();
    QTRY_VERIFY(!dialog);
    QVERIFY(window.waitForPreview(15000));

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const DocumentSnapshot snapshot = window.document();
    const Frame *frame = findFrame(snapshot, view->selectedFrame());
    QVERIFY(frame);
    QCOMPARE(frame->filter, QStringLiteral("print.halftone"));
    QCOMPARE(frame->effectVersion, 1);
    QCOMPARE(frame->filterStrength, 100);

    auto *sideStrength = window.findChild<QSlider *>(QStringLiteral("effectStrength"));
    QVERIFY(sideStrength);
    QVERIFY(!sideStrength->isEnabled());
    QCOMPARE(sideStrength->value(), 100);
    auto *hint = window.findChild<QLabel *>(QStringLiteral("effectStrengthHint"));
    QVERIFY(hint);
    QVERIFY(hint->isVisible());
    QCOMPARE(hint->text(), QStringLiteral("Ez az effekt teljes erősséggel jelenik meg."));
}

void TestEffectBrowser::countedResultLabelUsesPluralForms()
{
    // The dialog's result counter is the new counted string. Every catalogue
    // carries a numerus entry for it, so a translator that has one selects the
    // singular and plural forms the language expects.
    QTranslator translator;
    QVERIFY(translator.load(QStringLiteral(":/i18n/photoprint_en.qm")));
    QVERIFY(QCoreApplication::installTranslator(&translator));
    QCOMPARE(QCoreApplication::translate("PhotoPrint", "%n effekt", nullptr, 1),
             QStringLiteral("1 effect"));
    QCOMPARE(QCoreApplication::translate("PhotoPrint", "%n effekt", nullptr, 2),
             QStringLiteral("2 effects"));
    QCoreApplication::removeTranslator(&translator);

    QTranslator german;
    QVERIFY(german.load(QStringLiteral(":/i18n/photoprint_de.qm")));
    QVERIFY(QCoreApplication::installTranslator(&german));
    QCOMPARE(QCoreApplication::translate("PhotoPrint", "%n effekt", nullptr, 1),
             QStringLiteral("1 Effekt"));
    QCOMPARE(QCoreApplication::translate("PhotoPrint", "%n effekt", nullptr, 2),
             QStringLiteral("2 Effekte"));
    QCoreApplication::removeTranslator(&german);
}

void TestEffectBrowser::legacyKeysKeepTheirPreviousLookLabel()
{
    QCOMPARE(effectButtonLabel(QStringLiteral("original")), QStringLiteral("Eredeti"));
    QCOMPARE(effectButtonLabel(QStringLiteral("basic.punch")), QStringLiteral("Punch"));
    for (const QString &legacy : {QStringLiteral("sepia"), QStringLiteral("vintage"),
                                  QStringLiteral("crisp"), QStringLiteral("bw")}) {
        QVERIFY2(isLegacyPresetKey(legacy), qPrintable(legacy));
        QCOMPARE(effectButtonLabel(legacy), QStringLiteral("Korábbi megjelenés"));
    }
    QCOMPARE(legacyEffectLabel(), QStringLiteral("Korábbi megjelenés"));
    QCOMPARE(newEffectAffordance(), QStringLiteral("Új effekt választása"));

    // A project opened from 1.0.x has a legacy key; the browser must not claim
    // the frame is untouched and must not mark a catalogue tile as current.
    EffectBrowserDialog dialog(requestWith(QStringLiteral("sepia")));
    dialog.show();
    QVERIFY(dialog.waitForRendering());
    QCOMPARE(dialog.currentKey(), QStringLiteral("sepia"));
    QCOMPARE(dialog.visibleTileCount(), 11);
    for (const QString &key : dialog.visibleTileKeys())
        QVERIFY(!dialog.tileMarkedCurrent(key));
}

void TestEffectBrowser::legacyProjectShowsPreviousLookInThePanel()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    QCOMPARE(photos.size(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QVERIFY(window.waitForPreview(15000));

    // A project saved by 1.0.x keeps its legacy key. Load one through the same
    // slot the open and recovery paths use.
    DocumentSnapshot document = window.document();
    const QString frameId = document.pages.first().frames.first().id;
    document.pages.first().frames.first().filter = QStringLiteral("sepia");
    document.pages.first().frames.first().effectVersion = 0;
    document.pages.first().frames.first().filterStrength = 60;
    const QString path = dir.filePath(QStringLiteral("legacy.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, document, SaveMode::Normal).ok());
    const auto loaded = store.load(path);
    QVERIFY(loaded);

    qRegisterMetaType<pp::LoadedProject>("pp::LoadedProject");
    QVERIFY(QMetaObject::invokeMethod(&window, "applyLoadedProject", Qt::DirectConnection,
                                      Q_ARG(pp::LoadedProject, loaded.value()),
                                      Q_ARG(bool, false)));
    QVERIFY(QMetaObject::invokeMethod(&window, "selectFrame", Qt::DirectConnection,
                                      Q_ARG(QString, frameId), Q_ARG(bool, true)));

    auto *button = window.findChild<QPushButton *>(QStringLiteral("effectButton"));
    auto *hint = window.findChild<QLabel *>(QStringLiteral("legacyEffectHint"));
    auto *strength = window.findChild<QSlider *>(QStringLiteral("effectStrength"));
    QVERIFY(button);
    QVERIFY(hint);
    QVERIFY(strength);
    QCOMPARE(button->text(), QStringLiteral("Korábbi megjelenés"));
    QVERIFY(hint->isVisible());
    QCOMPARE(hint->text(), QStringLiteral("Új effekt választása"));
    QVERIFY(strength->isEnabled());
    QCOMPARE(strength->value(), 60);

    // Opening the browser must not silently rewrite the frozen key.
    button->click();
    QPointer<EffectBrowserDialog> dialog = window.findChild<EffectBrowserDialog *>();
    QVERIFY(dialog && dialog->waitForRendering());
    QCOMPARE(dialog->currentKey(), QStringLiteral("sepia"));
    for (const QString &key : dialog->visibleTileKeys())
        QVERIFY(!dialog->tileMarkedCurrent(key));
    dialog->close();
    QTRY_VERIFY(!dialog);
    const DocumentSnapshot unchanged = window.document();
    const Frame *frame = findFrame(unchanged, frameId);
    QVERIFY(frame);
    QCOMPARE(frame->filter, QStringLiteral("sepia"));
}

void TestEffectBrowser::browserOpensWithoutSynchronousRendering()
{
    EffectBrowserRequest request = requestWith(QStringLiteral("basic.neutral"));
    QElapsedTimer timer;
    timer.start();
    EffectBrowserDialog dialog(request);
    dialog.show();
    const qint64 openMs = timer.elapsed();
    QVERIFY2(openMs < 400, qPrintable(QStringLiteral("open took %1 ms").arg(openMs)));
    // Only the Original tile is ready synchronously. A synchronous catalogue
    // render would have filled all eleven before show returned.
    QCOMPARE(dialog.renderedTileCount(), 1);
    QVERIFY(dialog.visibleTileCount() > 1);

    QElapsedTimer renderTimer;
    renderTimer.start();
    QVERIFY(dialog.waitForRendering(15000));
    const qint64 categoryMs = renderTimer.elapsed();
    QCOMPARE(dialog.renderedTileCount(), dialog.visibleTileCount());
    QVERIFY2(categoryMs < 15000,
             qPrintable(QStringLiteral("one category took %1 ms").arg(categoryMs)));
    writeTiming(QStringLiteral("browser-open"), openMs);
    writeTiming(QStringLiteral("browser-first-category"), categoryMs);
}

void TestEffectBrowser::pagePreviewNoLongerBlocksTheGui()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    QCOMPARE(photos.size(), 3);

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(80);

    QElapsedTimer timer;
    timer.start();
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    const qint64 addMs = timer.elapsed();
    QVERIFY(window.previewPending());
    QVERIFY2(addMs < 600, qPrintable(QStringLiteral("adding photos blocked %1 ms").arg(addMs)));

    timer.restart();
    QVERIFY(window.waitForPreview(15000));
    const qint64 firstPreviewMs = timer.elapsed();
    writeTiming(QStringLiteral("page-preview-add-three"), addMs);
    writeTiming(QStringLiteral("page-preview-first-settle"), firstPreviewMs);

    // One document change (the effect apply) must return at once and leave the
    // previous raster on screen; the worker delivers the new page later.
    auto *button = window.findChild<QPushButton *>(QStringLiteral("effectButton"));
    QVERIFY(button);
    button->click();
    QPointer<EffectBrowserDialog> dialog = window.findChild<EffectBrowserDialog *>();
    QVERIFY(dialog);
    QVERIFY(dialog->waitForRendering());

    QVERIFY(selectCategory(*dialog, QStringLiteral("basic")));
    QTest::qWait(10);
    QAbstractButton *punch = tile(*dialog, QStringLiteral("basic.punch"));
    QVERIFY(punch);
    QTest::mouseClick(punch, Qt::LeftButton, Qt::NoModifier, punch->rect().center());
    auto *apply = dialog->findChild<QPushButton *>(QStringLiteral("applyButton"));
    QVERIFY(apply);
    timer.restart();
    apply->click();
    const qint64 applyMs = timer.elapsed();
    QVERIFY2(applyMs < 200,
             qPrintable(QStringLiteral("effect apply blocked the GUI for %1 ms").arg(applyMs)));
    QVERIFY(window.previewPending());
    QVERIFY(window.waitForPreview(15000));
    writeTiming(QStringLiteral("page-preview-effect-apply"), applyMs);
}

QTEST_MAIN(TestEffectBrowser)
#include "tst_effect_browser.moc"
