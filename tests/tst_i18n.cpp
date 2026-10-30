#include "MainWindow.h"
#include "Translation.h"

#include <QAction>
#include <QCoreApplication>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

using namespace pp;

namespace {

/// Source strings, so the comparison is against what the code actually says.
const QString kPrint = QStringLiteral("Nyomtatás");
const QString kAbout = QStringLiteral("Névjegy");

/// The messages that carry a count and therefore have plural forms.
const QString kPages = QStringLiteral("%n oldal nyomtatása");
const QString kPhoto = QStringLiteral("%n. fotó");
const QString kPageTooltip = QStringLiteral("%1 · %n fotó");

QString translated(const char *context, const QString &source)
{
    return QCoreApplication::translate(context, source.toUtf8().constData());
}

QString translatedWithCount(const char *context, const QString &source, int count)
{
    return QCoreApplication::translate(context, source.toUtf8().constData(), nullptr, count);
}

QString cataloguePath(const QString &code)
{
    return QStringLiteral(":/i18n/photoprint_%1.qm").arg(code);
}

/// Installs one catalogue into the application for the lifetime of the object,
/// which is the path the interface itself takes. QTranslator::translate picks
/// the plural form but leaves %n in place; the substitution is
/// QCoreApplication::translate's job, so a test that reads numbers has to go
/// through it as well.
class InstalledCatalogue
{
public:
    explicit InstalledCatalogue(const QString &code)
        : m_loaded(m_translator.load(cataloguePath(code)))
    {
        if (m_loaded)
            QCoreApplication::installTranslator(&m_translator);
    }

    ~InstalledCatalogue()
    {
        if (m_loaded)
            QCoreApplication::removeTranslator(&m_translator);
    }

    InstalledCatalogue(const InstalledCatalogue &) = delete;
    InstalledCatalogue &operator=(const InstalledCatalogue &) = delete;

    bool loaded() const { return m_loaded; }

private:
    QTranslator m_translator;
    bool m_loaded = false;
};

} // namespace

class TestI18n : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void translationFileLoads_data();
    void translationFileLoads();
    void languageSettingRoundTrips();
    void systemLocaleIsTheFallback();
    void unsupportedLocaleFallsBackToHungarian_data();
    void unsupportedLocaleFallsBackToHungarian();
    void installedTranslatorChangesTheInterface();
    void languageMenuIsInTheMenuBar();
    void choosingALanguageAppliesImmediately();
    void singularFormIsUsedForOne_data();
    void singularFormIsUsedForOne();
    void pageTooltipKeepsThePaperAndPluralises();
    void everyCatalogueTranslatesCountedMessages_data();
    void everyCatalogueTranslatesCountedMessages();
    void hungarianSourceSubstitutesTheCount();
    void repeatedLanguageSwitchingKeepsOwnershipSane();

private:
    QTemporaryDir m_settingsDir;
};

void TestI18n::initTestCase()
{
    QVERIFY(m_settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintTest"));
    QSettings().clear();
}

void TestI18n::translationFileLoads_data()
{
    QTest::addColumn<QString>("code");
    for (const QString &code : i18n::targetCodes())
        QTest::newRow(qPrintable(code)) << code;
}

void TestI18n::translationFileLoads()
{
    QFETCH(QString, code);

    QTranslator translator;
    const QString path = QStringLiteral(":/i18n/photoprint_%1.qm").arg(code);
    QVERIFY2(translator.load(path), qPrintable(QStringLiteral("cannot load %1").arg(path)));

    // The print button and the About title are the first things a user reads.
    for (const QString &source : {kPrint, kAbout}) {
        const QString text = translator.translate("PhotoPrint", source.toUtf8().constData());
        QVERIFY2(!text.isEmpty(), qPrintable(QStringLiteral("%1: empty translation of %2")
                                                 .arg(code, source)));
        QVERIFY2(text != source, qPrintable(QStringLiteral("%1: %2 is still Hungarian")
                                                .arg(code, source)));
    }

    // Every shipped file carries the whole context, not a handful of strings.
    QVERIFY2(translator.translate("PhotoPrint", "Nyelv") != QStringLiteral("Nyelv"),
             qPrintable(QStringLiteral("%1: language menu title missing").arg(code)));
    QVERIFY(translator.translate("PhotoPrint", "Súgó") != QStringLiteral("Súgó"));
}

void TestI18n::languageSettingRoundTrips()
{
    i18n::saveLanguage(QStringLiteral("de"));
    QCOMPARE(i18n::selectedLanguage(), QStringLiteral("de"));
    QCOMPARE(QSettings().value(QStringLiteral("language")).toString(), QStringLiteral("de"));

    i18n::saveLanguage(QStringLiteral("zh_CN"));
    QCOMPARE(i18n::selectedLanguage(), QStringLiteral("zh_CN"));

    // An explicit Hungarian choice survives a supported system locale.
    i18n::saveLanguage(QStringLiteral("hu"));
    QCOMPARE(i18n::selectedLanguage(), QString());
    QCOMPARE(QSettings().value(QStringLiteral("language")).toString(), QStringLiteral("hu"));

    QSettings().remove(QStringLiteral("language"));
    QVERIFY(!QSettings().contains(QStringLiteral("language")));
}

void TestI18n::systemLocaleIsTheFallback()
{
    QSettings().remove(QStringLiteral("language"));
    // Nothing saved: the system locale decides, and an unsupported one lands on
    // Hungarian, which is what resolveLocale() reports as an empty code.
    QCOMPARE(i18n::selectedLanguage(), i18n::resolveLocale(QLocale::system().name()));
    QCOMPARE(i18n::resolveLocale(QStringLiteral("ko_KR")), QString());
}

void TestI18n::unsupportedLocaleFallsBackToHungarian_data()
{
    QTest::addColumn<QString>("locale");
    QTest::addColumn<QString>("expected");

    QTest::newRow("korean") << QStringLiteral("ko_KR") << QString();
    QTest::newRow("hungarian") << QStringLiteral("hu_HU") << QString();
    QTest::newRow("traditional chinese") << QStringLiteral("zh_TW") << QString();
    QTest::newRow("empty") << QString() << QString();
    QTest::newRow("german plain") << QStringLiteral("de") << QStringLiteral("de");
    QTest::newRow("austrian") << QStringLiteral("de_AT") << QStringLiteral("de");
    QTest::newRow("brazilian") << QStringLiteral("pt_BR") << QStringLiteral("pt");
    QTest::newRow("simplified chinese") << QStringLiteral("zh_CN") << QStringLiteral("zh_CN");
    QTest::newRow("chinese hans") << QStringLiteral("zh_Hans") << QStringLiteral("zh_CN");
    QTest::newRow("english uk") << QStringLiteral("en_GB") << QStringLiteral("en");
}

void TestI18n::unsupportedLocaleFallsBackToHungarian()
{
    QFETCH(QString, locale);
    QFETCH(QString, expected);
    QCOMPARE(i18n::resolveLocale(locale), expected);

    // An unsupported language leaves the interface in the source language.
    QVERIFY(i18n::installLanguage(locale));
    if (expected.isEmpty()) {
        QVERIFY(i18n::activeLanguage().isEmpty());
        QCOMPARE(translated("PhotoPrint", kPrint), kPrint);
    } else {
        QCOMPARE(i18n::activeLanguage(), expected);
    }
    i18n::installLanguage(QString());
    QVERIFY(i18n::activeLanguage().isEmpty());
    QCOMPARE(translated("PhotoPrint", kPrint), kPrint);
}

void TestI18n::installedTranslatorChangesTheInterface()
{
    QVERIFY(i18n::installLanguage(QStringLiteral("de")));
    QCOMPARE(i18n::activeLanguage(), QStringLiteral("de"));
    QCOMPARE(translated("PhotoPrint", kPrint), QStringLiteral("Drucken"));
    QCOMPARE(translated("PhotoPrint", kAbout), QStringLiteral("Über"));

    QVERIFY(i18n::installLanguage(QStringLiteral("ja")));
    QCOMPARE(i18n::activeLanguage(), QStringLiteral("ja"));
    QVERIFY(translated("PhotoPrint", kPrint) != kPrint);
    QVERIFY(translated("PhotoPrint", kAbout) != kAbout);

    // Hungarian needs no translator at all.
    QVERIFY(i18n::installLanguage(QString()));
    QVERIFY(i18n::activeLanguage().isEmpty());
    QCOMPARE(translated("PhotoPrint", kPrint), kPrint);
}

void TestI18n::languageMenuIsInTheMenuBar()
{
    QSettings().remove(QStringLiteral("language"));
    MainWindow window;
    window.show();
    QTest::qWait(50);

    QMenu *languageMenu = window.findChild<QMenu *>(QStringLiteral("languageMenu"));
    QVERIFY2(languageMenu, "the Nyelv menu is missing");
    QVERIFY2(window.menuBar()->actions().contains(languageMenu->menuAction()),
             "the language menu must be a top-level menu, not hidden under Help");

    QMenu *helpMenu = nullptr;
    for (QAction *action : window.menuBar()->actions()) {
        if (action->menu() && action->text() == QStringLiteral("Súgó"))
            helpMenu = action->menu();
    }
    QVERIFY2(helpMenu, "the Help menu is missing");
    QVERIFY(!helpMenu->actions().contains(languageMenu->menuAction()));

    const QList<QAction *> languages = languageMenu->actions();
    QCOMPARE(languages.size(), i18n::targetCodes().size() + 1);
    QCOMPARE(languages.first()->text(), QStringLiteral("English"));
    QCOMPARE(languages.last()->text(), QStringLiteral("Magyar"));

    int checked = 0;
    QString checkedCode;
    for (QAction *action : languages) {
        if (!action->isChecked())
            continue;
        ++checked;
        checkedCode = action->data().toString();
    }
    QCOMPARE(checked, 1);
    QCOMPARE(i18n::resolveLocale(checkedCode), i18n::selectedLanguage());
}

void TestI18n::choosingALanguageAppliesImmediately()
{
    QSettings().remove(QStringLiteral("language"));
    MainWindow window;
    window.show();
    QTest::qWait(50);

    QMenu *languageMenu = window.findChild<QMenu *>(QStringLiteral("languageMenu"));
    QVERIFY(languageMenu);
    QAction *german = nullptr;
    for (QAction *action : languageMenu->actions()) {
        if (action->text() == QStringLiteral("Deutsch")) {
            german = action;
            break;
        }
    }
    QVERIFY2(german, "Deutsch is missing from the language menu");
    german->trigger();
    QTest::qWait(50);

    QCOMPARE(i18n::selectedLanguage(), QStringLiteral("de"));
    QCOMPARE(QSettings().value(QStringLiteral("language")).toString(), QStringLiteral("de"));

    // The window itself has already switched; no restart is needed.
    auto *printButton = window.findChild<QPushButton *>(QStringLiteral("printButton"));
    QVERIFY(printButton);
    QCOMPARE(printButton->text(), QStringLiteral("Drucken"));
    bool helpTranslated = false;
    bool languageTranslated = false;
    for (QAction *action : window.menuBar()->actions()) {
        if (action->text() == QStringLiteral("Hilfe"))
            helpTranslated = true;
        if (action->text() == QStringLiteral("Sprache"))
            languageTranslated = true;
    }
    QVERIFY2(helpTranslated, "the Help menu kept its old language");
    QVERIFY2(languageTranslated, "the language menu kept its old title");

    // The rebuilt menu still marks the active language.
    QMenu *rebuilt = window.findChild<QMenu *>(QStringLiteral("languageMenu"));
    QVERIFY(rebuilt);
    int checked = 0;
    for (QAction *action : rebuilt->actions())
        checked += action->isChecked() ? 1 : 0;
    QCOMPARE(checked, 1);
    QVERIFY2(window.document().pages.size() >= 1, "the document was lost with the rebuild");

    QVERIFY(i18n::installLanguage(QString()));
    QSettings().remove(QStringLiteral("language"));
}

void TestI18n::singularFormIsUsedForOne_data()
{
    QTest::addColumn<QString>("code");
    QTest::addColumn<QString>("singular");
    QTest::addColumn<QString>("plural");

    // A count of one has to select the singular form. The button used to be
    // handed the plural noun unconditionally, which gave "1 Seiten drucken".
    QTest::newRow("en") << QStringLiteral("en") << QStringLiteral("Print 1 page")
                        << QStringLiteral("Print 2 pages");
    QTest::newRow("de") << QStringLiteral("de") << QStringLiteral("1 Seite drucken")
                        << QStringLiteral("2 Seiten drucken");
}

void TestI18n::singularFormIsUsedForOne()
{
    QFETCH(QString, code);
    QFETCH(QString, singular);
    QFETCH(QString, plural);

    InstalledCatalogue catalogue(code);
    QVERIFY2(catalogue.loaded(), qPrintable(code));

    QCOMPARE(translatedWithCount("PhotoPrint", kPages, 1), singular);
    QCOMPARE(translatedWithCount("PhotoPrint", kPages, 2), plural);
    // Eleven is plural in both languages, so the singular must not come back.
    QVERIFY(translatedWithCount("PhotoPrint", kPages, 11) != singular);
}

void TestI18n::pageTooltipKeepsThePaperAndPluralises()
{
    // The page list tooltip is "%1 · %n fotó": the paper name stays a
    // placeholder and the photo count is the numerus argument.
    for (const auto &[code, one, two] :
         {std::tuple<QString, QString, QString>{QStringLiteral("en"),
                                               QStringLiteral("A4 · 1 photo"),
                                               QStringLiteral("A4 · 2 photos")},
          std::tuple<QString, QString, QString>{QStringLiteral("de"),
                                               QStringLiteral("A4 · 1 Foto"),
                                               QStringLiteral("A4 · 2 Fotos")}}) {
        InstalledCatalogue catalogue(code);
        QVERIFY2(catalogue.loaded(), qPrintable(code));
        const QString singular =
            QCoreApplication::translate("PhotoPrint", kPageTooltip.toUtf8().constData(), nullptr, 1)
                .arg(QStringLiteral("A4"));
        const QString plural =
            QCoreApplication::translate("PhotoPrint", kPageTooltip.toUtf8().constData(), nullptr, 2)
                .arg(QStringLiteral("A4"));
        QCOMPARE(singular, one);
        QCOMPARE(plural, two);
    }
}

void TestI18n::everyCatalogueTranslatesCountedMessages_data()
{
    QTest::addColumn<QString>("code");
    for (const QString &code : i18n::targetCodes())
        QTest::newRow(qPrintable(code)) << code;
}

void TestI18n::everyCatalogueTranslatesCountedMessages()
{
    QFETCH(QString, code);

    InstalledCatalogue catalogue(code);
    QVERIFY2(catalogue.loaded(), qPrintable(code));

    // Both counted messages carry a numerus entry in every catalogue. The
    // number always comes from the argument: a leftover %n, or the %1 of the
    // old placeholder, would mean the entry was never converted.
    for (const QString &source : {kPages, kPhoto}) {
        for (const int count : {1, 2, 5, 22}) {
            const QString text = translatedWithCount("PhotoPrint", source, count);
            const QString where = QStringLiteral("%1, count %2, %3").arg(code).arg(count).arg(source);
            QVERIFY2(!text.isEmpty(), qPrintable(where + QStringLiteral(": empty")));
            QVERIFY2(text != source, qPrintable(where + QStringLiteral(": still Hungarian")));
            QVERIFY2(text.contains(QString::number(count)),
                     qPrintable(where + QStringLiteral(": the count is missing")));
            QVERIFY2(!text.contains(QStringLiteral("%n")),
                     qPrintable(where + QStringLiteral(": %n left in the text")));
            QVERIFY2(!text.contains(QStringLiteral("%1")),
                     qPrintable(where + QStringLiteral(": old %1 placeholder left in the text")));
        }
    }
}

void TestI18n::hungarianSourceSubstitutesTheCount()
{
    // Hungarian is the source language and has no catalogue: the string in the
    // code is already the right form for every count, because Hungarian does
    // not pluralise the noun after a numeral ("1 oldal", "2 oldal"). Qt fills
    // in %n itself when no translator is installed, and the print button
    // depends on that.
    QVERIFY(i18n::installLanguage(QString()));
    QCOMPARE(translatedWithCount("PhotoPrint", kPages, 1), QStringLiteral("1 oldal nyomtatása"));
    QCOMPARE(translatedWithCount("PhotoPrint", kPages, 2), QStringLiteral("2 oldal nyomtatása"));
    QCOMPARE(translatedWithCount("PhotoPrint", kPhoto, 3), QStringLiteral("3. fotó"));
}

void TestI18n::repeatedLanguageSwitchingKeepsOwnershipSane()
{
    // Every switch installs a fresh pair of translators and drops the previous
    // pair. Translation.cpp is their only owner; when QCoreApplication owned
    // them as well, the process double deleted them at shutdown.
    for (int round = 0; round < 3; ++round) {
        QVERIFY(i18n::installLanguage(QStringLiteral("de")));
        QCOMPARE(translated("PhotoPrint", kPrint), QStringLiteral("Drucken"));
        QVERIFY(i18n::installLanguage(QStringLiteral("zh_CN")));
        QVERIFY(translated("PhotoPrint", kPrint) != kPrint);
        QVERIFY(i18n::installLanguage(QString()));
        QCOMPARE(translated("PhotoPrint", kPrint), kPrint);
    }
}

QTEST_MAIN(TestI18n)
#include "tst_i18n.moc"
