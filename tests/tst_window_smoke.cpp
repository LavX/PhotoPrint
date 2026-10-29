#include "MainWindow.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QListWidget>
#include <QPixmap>
#include <QSettings>
#include <QTemporaryDir>
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

const QString kRealPhoto = QStringLiteral("/home/lavx/Downloads/IMG20260830150231.jpg");

} // namespace

class TestWindowSmoke : public QObject
{
    Q_OBJECT

private slots:
    void windowShowsTheRealPhoto();
    void emptyStateUsesPlainLanguage();
    void landscapePhotoPaperChangesTheSheet();
    void printChoicesReachTheDocument();
    void rememberedChoicesComeBack();
};

void TestWindowSmoke::windowShowsTheRealPhoto()
{
    QVERIFY(QFileInfo::exists(kRealPhoto));
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(150);

    const bool invoked = QMetaObject::invokeMethod(
        &window, "addDroppedFiles", Qt::DirectConnection,
        Q_ARG(QStringList, QStringList{kRealPhoto}));
    QVERIFY(invoked);
    QTest::qWait(400);

    auto *list = window.findChild<QListWidget *>();
    QVERIFY(list);
    QCOMPARE(list->count(), 1);

    const QPixmap shot = window.grab();
    QVERIFY(!shot.isNull());
    QVERIFY(shot.save(evidenceDir() + QStringLiteral("/window-slice1.png")));

    const QImage image = shot.toImage();
    QSet<QRgb> colours;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4)
            colours.insert(image.pixel(x, y));
    }
    QVERIFY2(colours.size() > 100,
             qPrintable(QStringLiteral("distinct window colours %1").arg(colours.size())));

    QListWidgetItem *firstItem = window.findChildren<QListWidget *>().first()->item(0);
    QVERIFY(firstItem);
    QVERIFY2(!firstItem->toolTip().isEmpty(), "the thumbnail keeps the file name as a tooltip");
}

void TestWindowSmoke::emptyStateUsesPlainLanguage()
{
    MainWindow window;
    window.show();
    QTest::qWait(100);
    auto *list = window.findChild<QListWidget *>();
    QVERIFY(list);
    QCOMPARE(list->count(), 0);
    // No technical vocabulary belongs to the starting surface.
    const QString forbidden = QStringLiteral("ICC sRGB P3 DPI PPI CUPS driver profil render");
    const QStringList words = forbidden.split(QLatin1Char(' '));
    for (QWidget *widget : window.findChildren<QWidget *>()) {
        const QString text = widget->property("text").toString()
            + widget->property("windowTitle").toString() + widget->toolTip();
        for (const QString &word : words) {
            QVERIFY2(!text.contains(word, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("technical word '%1' on screen").arg(word)));
        }
    }
}

void TestWindowSmoke::landscapePhotoPaperChangesTheSheet()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(100);
    const QList<QComboBox *> combos = window.findChildren<QComboBox *>();
    QVERIFY(combos.size() >= 3);
    QComboBox *paper = combos.at(1);
    QComboBox *orientation = combos.at(2);
    QCOMPARE(paper->currentIndex(), 0);
    int photoIndex = -1;
    for (int i = 0; i < paper->count(); ++i) {
        if (paper->itemText(i).startsWith(QStringLiteral("10 × 15 cm")))
            photoIndex = i;
    }
    QVERIFY2(photoIndex >= 0, "10 × 15 cm paper missing");
    paper->setCurrentIndex(photoIndex);
    orientation->setCurrentIndex(1);
    QTest::qWait(100);

    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, QStringList{kRealPhoto})));
    QTest::qWait(250);

    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    QVERIFY(view->pageMm().width() > view->pageMm().height());
    QVERIFY(std::abs(view->pageMm().width() - 150.0) < 0.01);
    QVERIFY(std::abs(view->pageMm().height() - 100.0) < 0.01);

    const QPixmap shot = window.grab();
    QVERIFY(shot.save(evidenceDir() + QStringLiteral("/window-slice3.png")));
}

void TestWindowSmoke::printChoicesReachTheDocument()
{
    MainWindow window;
    window.show();
    QTest::qWait(80);

    QComboBox *quality = nullptr;
    QComboBox *colour = nullptr;
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemText(i) == QStringLiteral("Kiváló"))
                quality = combo;
            if (combo->itemText(i) == QStringLiteral("Szürkeárnyalatos"))
                colour = combo;
        }
    }
    QVERIFY(quality);
    QVERIFY(colour);
    // Everyday defaults: best quality and colour.
    QCOMPARE(quality->currentIndex(), 0);
    QCOMPARE(colour->currentIndex(), 0);

    quality->setCurrentIndex(1);
    colour->setCurrentIndex(1);
    QTest::qWait(50);

    const DocumentSnapshot document = window.document();
    QVERIFY(!document.fineQuality);
    QVERIFY(document.recipe.grayscale);
}

void TestWindowSmoke::rememberedChoicesComeBack()
{
    qunsetenv("PHOTOPRINT_NO_SETTINGS");
    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintTest"));
    {
        QSettings settings;
        settings.clear();
    }

    {
        MainWindow window;
        window.show();
        QTest::qWait(80);
        const QList<QComboBox *> combos = window.findChildren<QComboBox *>();
        QVERIFY(combos.size() >= 3);
        for (int i = 0; i < combos.at(1)->count(); ++i) {
            if (combos.at(1)->itemText(i).startsWith(QStringLiteral("10 × 15 cm")))
                combos.at(1)->setCurrentIndex(i);
        }
        combos.at(2)->setCurrentIndex(1); // landscape
        QTest::qWait(80);
    }
    {
        MainWindow window;
        window.show();
        QTest::qWait(80);
        const QList<QComboBox *> combos = window.findChildren<QComboBox *>();
        QVERIFY(combos.size() >= 3);
        QVERIFY(combos.at(1)->currentText().startsWith(QStringLiteral("10 × 15 cm")));
        QCOMPARE(combos.at(2)->currentIndex(), 1);
        const DocumentSnapshot document = window.document();
        QCOMPARE(document.pages.first().paper.name, QStringLiteral("10 × 15 cm"));
        QVERIFY(document.pages.first().landscape);
    }
    qputenv("PHOTOPRINT_NO_SETTINGS", "1");
}

QTEST_MAIN(TestWindowSmoke)
#include "tst_window_smoke.moc"
