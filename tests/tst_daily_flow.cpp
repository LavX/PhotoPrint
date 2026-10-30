#include "Document.h"
#include "ImageSource.h"
#include "MainWindow.h"
#include "ProjectStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace pp;

namespace {

QImage colourImage(const QColor &colour, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
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
        QImageWriter writer(path, "png");
        if (!writer.write(colourImage(specs.at(i).first, specs.at(i).second.width(),
                                      specs.at(i).second.height())))
            return {};
        paths.append(path);
    }
    return paths;
}

bool overlaps(const Frame &a, const Frame &b)
{
    const QRectF ra(a.cx - a.width / 2.0, a.cy - a.height / 2.0, a.width, a.height);
    const QRectF rb(b.cx - b.width / 2.0, b.cy - b.height / 2.0, b.width, b.height);
    return ra.intersects(rb);
}

} // namespace

class TestDailyFlow : public QObject
{
    Q_OBJECT

private slots:
    void threePhotosOnA4WithoutHelp();
    void keyboardNudgeAndUndoWork();
    void projectRoundTripKeepsTheComposition();
    void noTechnicalVocabularyOnTheDailySurface();
    void smallWindowKeepsPrintVisible();
    void filterControlsUpdateTheFrame();
    void pageCountAppearsOnThePrintButton();
    void selectionReadsStayValidAcrossSelections();
    void cachedSourceStaysValidAfterLaterLoads();
    void digestIgnoresSignedZero();
    void digestIsPinnedToAKnownRecipe();
    void brightnessSliderAndPipelineRangeAgree();
};

void TestDailyFlow::threePhotosOnA4WithoutHelp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    QCOMPARE(photos.size(), 3);

    MainWindow window;
    window.resize(1280, 720);
    window.show();
    QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QTest::qWait(250);

    auto *list = window.findChild<QListWidget *>();
    QVERIFY(list);
    QCOMPARE(list->count(), 3);

    const DocumentSnapshot document = window.document();
    QCOMPARE(document.pages.size(), 1);
    const Page &page = document.pages.first();
    QCOMPARE(page.frames.size(), 3);
    const QSizeF pageSize = pageSizeMm(page);
    for (const Frame &frame : page.frames) {
        QVERIFY(frame.cx - frame.width / 2.0 >= 0.0);
        QVERIFY(frame.cy - frame.height / 2.0 >= 0.0);
        QVERIFY(frame.cx + frame.width / 2.0 <= pageSize.width());
        QVERIFY(frame.cy + frame.height / 2.0 <= pageSize.height());
    }
    for (int a = 0; a < page.frames.size(); ++a) {
        for (int b = a + 1; b < page.frames.size(); ++b)
            QVERIFY(!overlaps(page.frames.at(a), page.frames.at(b)));
    }

    QPushButton *printButton = nullptr;
    for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Nyomtatás"))
            printButton = button;
    }
    QVERIFY(printButton);
    QVERIFY(printButton->isVisible());

    QComboBox *paperCombo = nullptr;
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        if (combo->count() > 0 && combo->itemText(0).startsWith(QStringLiteral("A4")))
            paperCombo = combo;
    }
    QVERIFY(paperCombo);
    QVERIFY(paperCombo->count() >= 6);
    for (int i = 0; i < paperCombo->count(); ++i) {
        const QString label = paperCombo->itemText(i);
        QVERIFY2(label.contains(QStringLiteral("cm")), qPrintable(label));
        QVERIFY2(!label.contains(QStringLiteral("inch"), Qt::CaseInsensitive), qPrintable(label));
    }
}

void TestDailyFlow::keyboardNudgeAndUndoWork()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QTest::qWait(200);

    const DocumentSnapshot before = window.document();
    const QString frameId = before.pages.first().frames.first().id;
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    view->setSelectedFrame(frameId);
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Right);
    QTest::qWait(50);
    const double movedCx = window.document().pages.first().frames.first().cx;
    QVERIFY(movedCx > before.pages.first().frames.first().cx);

    QAction *undo = nullptr;
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->text().contains(QStringLiteral("Vissza")))
            undo = action;
    }
    QVERIFY(undo);
    QVERIFY(undo->isEnabled());
    undo->trigger();
    QTest::qWait(50);
    QCOMPARE(window.document().pages.first().frames.first().cx,
             before.pages.first().frames.first().cx);
}

void TestDailyFlow::projectRoundTripKeepsTheComposition()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    MainWindow window;
    window.show();
    QTest::qWait(80);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QTest::qWait(200);

    const DocumentSnapshot original = window.document();
    const QString path = dir.filePath(QStringLiteral("daily.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, original, SaveMode::Normal).ok());
    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QCOMPARE(loaded.value().document.pages.size(), original.pages.size());
    QCOMPARE(loaded.value().document.pages.first().frames.size(),
             original.pages.first().frames.size());
    QVERIFY(loaded.value().issues.isEmpty());
}

void TestDailyFlow::noTechnicalVocabularyOnTheDailySurface()
{
    MainWindow window;
    window.show();
    QTest::qWait(80);
    const QStringList forbidden = {QStringLiteral("ICC"), QStringLiteral("sRGB"),
                                   QStringLiteral("P3"),  QStringLiteral("DPI"),
                                   QStringLiteral("PPI"), QStringLiteral("CUPS"),
                                   QStringLiteral("driver"), QStringLiteral("profil"),
                                   QStringLiteral("render"), QStringLiteral("PPD")};
    for (QWidget *widget : window.findChildren<QWidget *>()) {
        QString text = widget->property("text").toString()
            + widget->property("windowTitle").toString() + widget->toolTip();
        if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            for (int i = 0; i < combo->count(); ++i)
                text += combo->itemText(i);
        }
        for (const QString &word : forbidden) {
            QVERIFY2(!text.contains(word, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("technical word '%1' on the daily surface: %2")
                                    .arg(word, text)));
        }
    }
}

void TestDailyFlow::smallWindowKeepsPrintVisible()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    MainWindow window;
    window.resize(640, 360);
    window.show();
    QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                     Q_ARG(QStringList, photos)));
    QTest::qWait(200);

    QPushButton *printButton = nullptr;
    QPushButton *addButton = nullptr;
    for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Nyomtatás"))
            printButton = button;
        if (button->text() == QStringLiteral("Fotók hozzáadása"))
            addButton = button;
    }
    QVERIFY(printButton);
    QVERIFY(addButton);
    QVERIFY(printButton->isVisible());
    QVERIFY(addButton->isVisible());
    QVERIFY2(printButton->mapTo(&window, QPoint(0, 0)).y() < window.height(),
             "the print button must stay inside a 640 x 360 window");
    QVERIFY2(window.statusBar()->isVisible(),
             qPrintable(QStringLiteral("window %1x%2 min %3x%4 status %5")
                            .arg(window.width()).arg(window.height())
                            .arg(window.minimumSizeHint().width())
                            .arg(window.minimumSizeHint().height())
                            .arg(window.statusBar()->height())));
    QVERIFY2(window.minimumSizeHint().height() <= 360,
             qPrintable(QStringLiteral("minimum height %1").arg(window.minimumSizeHint().height())));
}

void TestDailyFlow::filterControlsUpdateTheFrame()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QTest::qWait(250);

    QComboBox *filterCombo = nullptr;
    QSlider *strength = nullptr;
    QCheckBox *upscale = nullptr;
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemText(i) == QStringLiteral("Szépia"))
                filterCombo = combo;
        }
    }
    for (QSlider *slider : window.findChildren<QSlider *>()) {
        if (slider->maximum() == 100 && slider->value() == 100)
            strength = slider;
    }
    for (QCheckBox *check : window.findChildren<QCheckBox *>()) {
        if (check->text().contains(QStringLiteral("nagyítása")))
            upscale = check;
    }
    QVERIFY(filterCombo);
    QVERIFY(strength);
    QVERIFY(upscale);

    int sepiaIndex = 0;
    for (int i = 0; i < filterCombo->count(); ++i) {
        if (filterCombo->itemText(i) == QStringLiteral("Szépia"))
            sepiaIndex = i;
    }
    filterCombo->setCurrentIndex(sepiaIndex);
    QTest::qWait(120);
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    {
        const DocumentSnapshot snapshot = window.document();
        const Frame *frame = findFrame(snapshot, view->selectedFrame());
        QVERIFY(frame);
        QCOMPARE(frame->filter, QStringLiteral("sepia"));
    }

    strength->setValue(40);
    strength->sliderReleased();
    QTest::qWait(120);
    {
        const DocumentSnapshot snapshot = window.document();
        const Frame *frame = findFrame(snapshot, view->selectedFrame());
        QVERIFY(frame);
        QCOMPARE(frame->filterStrength, 40);
    }

    upscale->setChecked(true);
    QTest::qWait(120);
    {
        const DocumentSnapshot snapshot = window.document();
        const Frame *frame = findFrame(snapshot, view->selectedFrame());
        QVERIFY(frame);
        QVERIFY(frame->upscale);
    }

    filterCombo->setCurrentIndex(0); // Original
    QTest::qWait(80);
    QComboBox *colorCombo = nullptr;
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemText(i) == QStringLiteral("Szürkeárnyalatos"))
                colorCombo = combo;
        }
    }
    QVERIFY(colorCombo);
    colorCombo->setCurrentIndex(1);
    QTest::qWait(150);
    const QImage composed = view->composedImage();
    QVERIFY(!composed.isNull());
    const QRgb centre = composed.pixel(composed.width() / 2, composed.height() / 2);
    QCOMPARE(qRed(centre), qGreen(centre));
    QCOMPARE(qGreen(centre), qBlue(centre));
}

void TestDailyFlow::pageCountAppearsOnThePrintButton()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    MainWindow window;
    window.show();
    QTest::qWait(80);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QTest::qWait(200);

    QPushButton *addPage = nullptr;
    for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Új lap"))
            addPage = button;
    }
    QVERIFY(addPage);
    addPage->click();
    QTest::qWait(200);

    QPushButton *print = nullptr;
    for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (button->text().contains(QStringLiteral("oldal nyomtatása")))
            print = button;
    }
    QVERIFY(print);
    QCOMPARE(print->text(), QStringLiteral("2 oldal nyomtatása"));
}

void TestDailyFlow::selectionReadsStayValidAcrossSelections()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    QCOMPARE(photos.size(), 3);
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QTest::qWait(250);

    auto *list = window.findChild<QListWidget *>();
    QVERIFY(list);
    QCOMPARE(list->count(), 3);

    QList<QDoubleSpinBox *> sizeSpins;
    for (QDoubleSpinBox *spin : window.findChildren<QDoubleSpinBox *>()) {
        if (spin->suffix() == QStringLiteral(" cm"))
            sizeSpins.append(spin);
    }
    QCOMPARE(sizeSpins.size(), 2);

    // Every selection reads the selected frame twice and the next selection
    // reads it again after the earlier value was used and dropped.
    for (int round = 0; round < 2; ++round) {
        for (int row = 0; row < list->count(); ++row) {
            list->setCurrentRow(row);
            QTest::qWait(30);
            const DocumentSnapshot snapshot = window.document();
            const QString frameId = list->item(row)->data(Qt::UserRole).toString();
            const Frame *frame = findFrame(snapshot, frameId);
            QVERIFY(frame);
            QVERIFY(std::abs(sizeSpins.at(0)->value() - frame->width / 10.0) < 0.01);
            QVERIFY(std::abs(sizeSpins.at(1)->value() - frame->height / 10.0) < 0.01);
        }
    }
}

void TestDailyFlow::cachedSourceStaysValidAfterLaterLoads()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = makePhotos(dir.path());
    QCOMPARE(photos.size(), 3);

    ImageStore store;
    const auto loaded = store.load(photos.at(0), ImportPolicy{});
    QVERIFY(loaded);
    const QSize expectedSize = loaded.value().size();

    const std::optional<SourceImage> first = store.cached(photos.at(0));
    QVERIFY(first);
    QCOMPARE(first->path, loaded.value().path);
    QCOMPARE(first->sha256, loaded.value().sha256);

    // Later loads and a clear must not disturb the value already handed out.
    QVERIFY(store.load(photos.at(1), ImportPolicy{}));
    QVERIFY(store.load(photos.at(2), ImportPolicy{}));
    QCOMPARE(first->sha256, loaded.value().sha256);
    QCOMPARE(first->size(), expectedSize);

    store.clear();
    QVERIFY(!store.cached(photos.at(0)));
    QCOMPARE(first->sha256, loaded.value().sha256);
    QCOMPARE(first->size(), expectedSize);
}

void TestDailyFlow::digestIgnoresSignedZero()
{
    // -0.0 compares equal to 0.0, so both must land on the same cache entry.
    ColorRecipe positive;
    ColorRecipe negative;
    negative.tone.exposureEv = -0.0;
    QCOMPARE(negative.digest(), positive.digest());

    // A tiny negative residual rounds to "-0.000000" in the canonical text.
    // That is the same number as zero for the digest as well.
    ColorRecipe tinyNegative;
    tinyNegative.tone.exposureEv = -1e-9;
    QCOMPARE(tinyNegative.digest(), positive.digest());

    // The normalisation must not swallow real values.
    ColorRecipe different;
    different.tone.exposureEv = -0.001;
    QVERIFY(different.digest() != positive.digest());
}

void TestDailyFlow::digestIsPinnedToAKnownRecipe()
{
    // A hard-coded value: if a Qt upgrade or a formatting change alters the
    // canonical stream, this fails instead of silently invalidating every
    // digest already written to a project or a recovery file.
    ColorRecipe recipe;
    recipe.autoEnhance = true;
    recipe.tone = PhotoTone{0.125, 0.25, -0.375};
    recipe.paperStrength = 0.5;
    recipe.grayscale = true;
    QCOMPARE(recipe.digest().toHex(),
             QByteArray("05938672d916cbc0da16139a1b77bffb939fef87670c7250266c8209e5972b5c"));
}

void TestDailyFlow::brightnessSliderAndPipelineRangeAgree()
{
    // The slider is the only UI for the manual offset, and the pipeline clamps
    // to the same range; the endpoint conversions must agree with it.
    QCOMPARE(sliderToBrightnessEv(kBrightnessSliderMin), kMinBrightnessEv);
    QCOMPARE(sliderToBrightnessEv(kBrightnessSliderMax), kMaxBrightnessEv);
    QCOMPARE(sliderToBrightnessEv(kBrightnessSliderNeutral), 0.0);

    for (int slider = kBrightnessSliderMin; slider <= kBrightnessSliderMax; ++slider)
        QCOMPARE(brightnessEvToSlider(sliderToBrightnessEv(slider)), slider);

    // Out-of-range values cannot walk the slider off its ends.
    QCOMPARE(brightnessEvToSlider(kMaxBrightnessEv + 5.0), kBrightnessSliderMax);
    QCOMPARE(brightnessEvToSlider(kMinBrightnessEv - 5.0), kBrightnessSliderMin);
}

QTEST_MAIN(TestDailyFlow)
#include "tst_daily_flow.moc"
