#include "MainWindow.h"
#include "Translation.h"

#include <QAction>
#include <QCoreApplication>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

#include <tuple>

using namespace pp;

namespace {

/// Source strings, so the comparison is against what the code actually says.
const QString kPrint = QStringLiteral("Nyomtatás");
const QString kAbout = QStringLiteral("Névjegy");

/// The messages that carry a count and therefore have plural forms.
const QString kPages = QStringLiteral("%n oldal nyomtatása");
const QString kPageTooltip = QStringLiteral("%1 · %n fotó");

/// The browser's result counter. It is counted as well, so every catalogue
/// needs a numerus entry for it.
const QString kEffectCount = QStringLiteral("%n effekt");

/// The position of a photo on the page. It is an index, not a count, so it
/// carries a plain %1 and has no plural forms.
const QString kPhotoIndex = QStringLiteral("%1. fotó");

QString translated(const char *context, const QString &source)
{
    return QCoreApplication::translate(context, source.toUtf8().constData());
}

QString translatedWithCount(const char *context, const QString &source, int count)
{
    return QCoreApplication::translate(context, source.toUtf8().constData(), nullptr, count);
}

/// What an untranslated message looks like: the source text with %n expanded,
/// which is what Qt produces when no catalogue has an entry for it. Comparing a
/// counted translation against the bare source is not enough, because
/// QCoreApplication::translate substitutes %n in the source as well.
QString hungarianFallback(const QString &source, int count)
{
    return QString(source).replace(QStringLiteral("%n"), QString::number(count));
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
    void everyCatalogueTranslatesTheEffectBrowser_data();
    void everyCatalogueTranslatesTheEffectBrowser();
    void everyCatalogueTranslatesThePhotoIndex_data();
    void everyCatalogueTranslatesThePhotoIndex();
    void everyCatalogueTranslatesTheLayoutMessages_data();
    void everyCatalogueTranslatesTheLayoutMessages();
    void everyCatalogueTranslatesTheSlotUiMessages_data();
    void everyCatalogueTranslatesTheSlotUiMessages();
    void everyCataloguePluralisesTheSlotCounts_data();
    void everyCataloguePluralisesTheSlotCounts();
    void everyCatalogueTranslatesTheSheetToolMessages_data();
    void everyCatalogueTranslatesTheSheetToolMessages();
    void everyCataloguePluralisesTheSheetToolCounts_data();
    void everyCataloguePluralisesTheSheetToolCounts();
    void sourceLanguageInstallsTheQtCatalogue();
    void hungarianSourceSubstitutesTheCount();
    void repeatedLanguageSwitchingLeavesTranslatorsUnparented();

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
    // number always comes from the argument: a text that is still the
    // Hungarian fallback, or one without the number in it, would mean the
    // entry was never converted. A leftover %n cannot be observed from here:
    // QCoreApplication::translate substitutes it before the text comes back.
    for (const QString &source : {kPages, kPageTooltip, kEffectCount}) {
        for (const int count : {1, 2, 5, 22}) {
            const QString text = translatedWithCount("PhotoPrint", source, count);
            const QString where = QStringLiteral("%1, count %2, %3").arg(code).arg(count).arg(source);
            QVERIFY2(!text.isEmpty(), qPrintable(where + QStringLiteral(": empty")));
            QVERIFY2(text != hungarianFallback(source, count),
                     qPrintable(where + QStringLiteral(": still the Hungarian fallback")));
            // %1 is the paper name and stays a placeholder here; it can itself
            // hold the digits of the count ("%1 · 1 photo"), so it is taken out
            // before the count is looked for.
            const QString withoutPaper = QString(text).replace(QStringLiteral("%1"), QString());
            QVERIFY2(withoutPaper.contains(QString::number(count)),
                     qPrintable(where + QStringLiteral(": the count is missing")));
            // %1 is filled in later by the caller. It has to survive exactly
            // when the source has it.
            QCOMPARE(text.contains(QStringLiteral("%1")), source.contains(QStringLiteral("%1")));
        }
    }
}

void TestI18n::everyCatalogueTranslatesTheEffectBrowser_data()
{
    QTest::addColumn<QString>("code");
    for (const QString &code : i18n::targetCodes())
        QTest::newRow(qPrintable(code)) << code;
}

void TestI18n::everyCatalogueTranslatesTheEffectBrowser()
{
    QFETCH(QString, code);

    InstalledCatalogue catalogue(code);
    QVERIFY2(catalogue.loaded(), qPrintable(code));

    // Every new browser and busy-overlay string, plus the category labels.
    // Retró is deliberately absent: "Retro" is the same word in several target
    // languages, so it cannot be told apart from the source fallback. Every
    // string here has a genuinely different translation in all ten catalogues.
    for (const QString &source : {
             QStringLiteral("Effekt kiválasztása"),
             QStringLiteral("Új effekt választása"),
             QStringLiteral("Effekt erőssége"),
             QStringLiteral("Effektek"),
             QStringLiteral("Keresés név vagy kategória szerint"),
             QStringLiteral("Keresés"),
             QStringLiteral("Kategóriák"),
             QStringLiteral("Az előnézet hozzávetőleges"),
             QStringLiteral("lassú"),
             QStringLiteral("Nincs találat."),
             QStringLiteral("Alkalmaz"),
             QStringLiteral("Korábbi megjelenés"),
             QStringLiteral("Ez az effekt teljes erősséggel jelenik meg."),
             QStringLiteral("Az eredeti kép effekt nélkül."),
             QStringLiteral("Feldolgozás…"),
             QStringLiteral("Mégsem"),
             QStringLiteral("Alap"),
             QStringLiteral("Fekete-fehér"),
             QStringLiteral("Szín"),
             QStringLiteral("Elmosás"),
             QStringLiteral("Élesség"),
             QStringLiteral("Művészi"),
             QStringLiteral("Torzítás"),
             QStringLiteral("Fény"),
             QStringLiteral("Szemcse"),
             QStringLiteral("Nyomat"),
             QStringLiteral("Ragyogás"),
         }) {
        const QString text = translated("PhotoPrint", source);
        const QString where = QStringLiteral("%1: %2").arg(code, source);
        QVERIFY2(!text.isEmpty(), qPrintable(where + QStringLiteral(" is empty")));
        QVERIFY2(text != source,
                 qPrintable(where + QStringLiteral(" is still the Hungarian source")));
    }
}

void TestI18n::everyCatalogueTranslatesThePhotoIndex_data()
{
    QTest::addColumn<QString>("code");
    for (const QString &code : i18n::targetCodes())
        QTest::newRow(qPrintable(code)) << code;
}

void TestI18n::everyCatalogueTranslatesThePhotoIndex()
{
    QFETCH(QString, code);

    InstalledCatalogue catalogue(code);
    QVERIFY2(catalogue.loaded(), qPrintable(code));

    // The photo index is an ordinal, not a quantity, so it is translated as a
    // plain %1 message with no numerus entry.
    const QString text = translated("PhotoPrint", kPhotoIndex).arg(4);
    const QString where = QStringLiteral("%1: %2").arg(code, kPhotoIndex);
    QVERIFY2(!text.isEmpty(), qPrintable(where + QStringLiteral(" is empty")));
    QVERIFY2(text != QStringLiteral("4. fotó"),
             qPrintable(where + QStringLiteral(" is still Hungarian")));
    QVERIFY2(text.contains(QLatin1Char('4')),
             qPrintable(where + QStringLiteral(" lost the index")));
    QVERIFY2(!text.contains(QStringLiteral("%1")),
             qPrintable(where + QStringLiteral(" left the %1 placeholder in the text")));
}

void TestI18n::everyCatalogueTranslatesTheLayoutMessages_data()
{
    QTest::addColumn<QString>("code");
    for (const QString &code : i18n::targetCodes())
        QTest::newRow(qPrintable(code)) << code;
}

void TestI18n::everyCatalogueTranslatesTheLayoutMessages()
{
    QFETCH(QString, code);

    InstalledCatalogue catalogue(code);
    QVERIFY2(catalogue.loaded(), qPrintable(code));

    // Every slot-layout sentence a person can read, including the errors the
    // picker or the canvas can show. Internal codes and diagnostics are English
    // and are not in this list.
    for (const QString &source : {
             QStringLiteral("Egy lapon legfeljebb 12 hely lehet."),
             QStringLiteral("A sablonban minden fotóhoz pontosan egy helynek kell tartoznia."),
             QStringLiteral("A sablon egyik helye érvénytelen."),
             QStringLiteral("Ehhez a laphoz még nincs elrendezés."),
             QStringLiteral("Ehhez a laphoz másik elrendezés tartozik."),
             QStringLiteral("Ez a hely már nem létezik."),
             QStringLiteral("Egy fotó nem található ezen a lapon."),
             QStringLiteral("A fotónak nincs forrásfájlja."),
             QStringLiteral("Az elrendezés nem építhető fel ezen a lapon."),
             QStringLiteral("A hely azonosítója üres vagy ismétlődik."),
             QStringLiteral("A hely a lapon kívülre esik."),
             QStringLiteral("Egy fotó olyan helyre hivatkozik, amely nem létezik."),
             QStringLiteral("Két fotó nem kerülhet ugyanabba a helybe."),
             QStringLiteral("A margó, a hézag vagy a szegély értéke érvénytelen."),
         }) {
        const QString text = translated("PhotoPrint", source);
        const QString where = QStringLiteral("%1: %2").arg(code, source);
        QVERIFY2(!text.isEmpty(), qPrintable(where + QStringLiteral(" is empty")));
        QVERIFY2(text != source,
                 qPrintable(where + QStringLiteral(" is still the Hungarian source")));
    }
}

void TestI18n::everyCatalogueTranslatesTheSlotUiMessages_data()
{
    QTest::addColumn<QString>("code");
    for (const QString &code : i18n::targetCodes())
        QTest::newRow(qPrintable(code)) << code;
}

void TestI18n::everyCatalogueTranslatesTheSlotUiMessages()
{
    QFETCH(QString, code);

    InstalledCatalogue catalogue(code);
    QVERIFY2(catalogue.loaded(), qPrintable(code));

    // Every sentence the slot interaction and the picker added: the canvas hint,
    // the five undo labels, the picker's current-layout lines and the two
    // surplus/drop messages, plus the layout panel's own controls and one-line
    // explanations.
    for (const QString &source : {
             QStringLiteral("Húzd ide a fotót"),
             QStringLiteral("Fotó beillesztése"),
             QStringLiteral("Fotó áthelyezése"),
             QStringLiteral("Képek cseréje"),
             QStringLiteral("Fotó leválasztása"),
             QStringLiteral("Hely ürítése"),
             QStringLiteral("Jelenlegi elrendezés: %1"),
             QStringLiteral("Jelenlegi elrendezés"),
             QStringLiteral("Ez az elrendezés már a lapon van."),
             QStringLiteral("A többi fotó új lapra került."),
             QStringLiteral("Csere"),
             QStringLiteral("Elrendezés"),
             QStringLiteral("Saját elrendezés"),
             QStringLiteral("Külső margó"),
             QStringLiteral("Képek közti hézag"),
             QStringLiteral("Automatikus margó"),
             QStringLiteral("Automatikus hézag"),
             QStringLiteral("Szegély"),
             QStringLiteral("Szegély színe"),
             QStringLiteral("Sarok lekerekítése"),
             QStringLiteral("Hely hozzáadása"),
             QStringLiteral("Hely törlése"),
             QStringLiteral("Elrendezés megszüntetése"),
             QStringLiteral("Sablonok…"),
             QStringLiteral("Válaszd ki a helyet a lapon."),
             QStringLiteral("A fotók megmaradnak a helyükön."),
             QStringLiteral("A többi hely a helyén marad."),
             QStringLiteral("A fotó szabadon marad a lapon, a többi hely a helyén marad."),
             QStringLiteral("Az elrendezés megszűnt."),
             QStringLiteral("Az elrendezés megszűnt, a fotó a lapon marad."),
             QStringLiteral("Az elrendezés megszűnik, a fotók szabadon maradnak a helyükön."),
             QStringLiteral("Margó beállítása"),
             QStringLiteral("Szegély beállítása"),
             // The in-slot photograph inspector: its section, the two sliders,
             // the quarter turns, the reset and its tooltip and hint.
             QStringLiteral("Kép a helyen belül"),
             QStringLiteral("Nagyítás"),
             QStringLiteral("Forgatás"),
             QStringLiteral("Balra 90°"),
             QStringLiteral("Jobbra 90°"),
             QStringLiteral("Alaphelyzet"),
             QStringLiteral("Visszaállítja a kép nagyítását, helyét és forgatását."),
             QStringLiteral("A fotó a helyén belül mozgatható, nagyítható és forgatható."),
         }) {
        const QString text = translated("PhotoPrint", source);
        const QString where = QStringLiteral("%1: %2").arg(code, source);
        QVERIFY2(!text.isEmpty(), qPrintable(where + QStringLiteral(" is empty")));
        QVERIFY2(text != source,
                 qPrintable(where + QStringLiteral(" is still the Hungarian source")));
    }
}

void TestI18n::everyCataloguePluralisesTheSlotCounts_data()
{
    QTest::addColumn<QString>("code");
    for (const QString &code : i18n::targetCodes())
        QTest::newRow(qPrintable(code)) << code;
}

void TestI18n::everyCataloguePluralisesTheSlotCounts()
{
    QFETCH(QString, code);

    InstalledCatalogue catalogue(code);
    QVERIFY2(catalogue.loaded(), qPrintable(code));

    // The three quantity lines are %n messages with numerus entries, not %1
    // arguments substituted by the caller.
    for (const QString &source : {QStringLiteral("%n hely üresen marad."),
                                  QStringLiteral("%n fotó szabadon marad."),
                                  QStringLiteral("%n hely")}) {
        for (const int count : {1, 2, 5}) {
            const QString text = translatedWithCount("PhotoPrint", source, count);
            const QString where =
                QStringLiteral("%1, count %2, %3").arg(code).arg(count).arg(source);
            QVERIFY2(!text.isEmpty(), qPrintable(where + QStringLiteral(": empty")));
            QVERIFY2(text != hungarianFallback(source, count),
                     qPrintable(where + QStringLiteral(": still the Hungarian fallback")));
            QVERIFY2(text.contains(QString::number(count)),
                     qPrintable(where + QStringLiteral(": the count is missing")));
            QVERIFY2(!text.contains(QStringLiteral("%n")),
                     qPrintable(where + QStringLiteral(": %n was left in the text")));
        }
        const bool hasPluralForms =
            code != QStringLiteral("ja") && code != QStringLiteral("zh_CN");
        if (hasPluralForms) {
            QVERIFY2(translatedWithCount("PhotoPrint", source, 1)
                         != translatedWithCount("PhotoPrint", source, 2),
                     qPrintable(QStringLiteral("%1: %2 has no numerus difference")
                                    .arg(code, source)));
        }
    }
}

void TestI18n::everyCatalogueTranslatesTheSheetToolMessages_data()
{
    QTest::addColumn<QString>("code");
    for (const QString &code : i18n::targetCodes())
        QTest::newRow(qPrintable(code)) << code;
}

void TestI18n::everyCatalogueTranslatesTheSheetToolMessages()
{
    QFETCH(QString, code);

    InstalledCatalogue catalogue(code);
    QVERIFY2(catalogue.loaded(), qPrintable(code));

    // Every sentence the two sheet tools and the cut-guide control add: dialog
    // labels, hints, refusals, the print guard and the page panel.
    for (const QString &source : {
             QStringLiteral("Képek sokszorosítása"),
             QStringLiteral("Fotók rendezése"),
             QStringLiteral("Előnézet"),
             QStringLiteral("Ugyanaz a fotó több példányban, egyenes rácsban."),
             QStringLiteral("Fotó: %1"),
             QStringLiteral("Válassz ki egy fotót a sokszorosításhoz."),
             QStringLiteral("Méret"),
             QStringLiteral("Egyedi méret"),
             QStringLiteral("Szélesség és magasság cseréje"),
             QStringLiteral("Legkisebb margó"),
             QStringLiteral("Darabszám"),
             QStringLiteral("Amennyi befér"),
             QStringLiteral("Vágójelek"),
             QStringLiteral("Nincs"),
             QStringLiteral("Képernyőn"),
             QStringLiteral("Hely"),
             QStringLiteral("Az aktuális lapra"),
             QStringLiteral("Új lapokra"),
             QStringLiteral("Összegzés"),
             QStringLiteral("Létrehozás"),
             QStringLiteral("Rácsba"),
             QStringLiteral("Térköz kiegyenlítése"),
             QStringLiteral("Oszlopok"),
             QStringLiteral("A kép egésze látszódjon"),
             QStringLiteral("Automatikus"),
             QStringLiteral("Egymás mellett"),
             QStringLiteral("Egymás alatt"),
             QStringLiteral("Irány"),
             QStringLiteral("Rendezés"),
             QStringLiteral("Sokszorosítás…"),
             QStringLiteral("Lap"),
             QStringLiteral("Fotók rendezése…"),
             QStringLiteral("Fotók cseréje"),
             QStringLiteral("Vágójelek a nyomaton"),
             QStringLiteral("A kiválasztott fotó szélei lemaradnak."),
             QStringLiteral("Egy fotó van a lapon. Csak középre igazítjuk, a mérete nem változik."),
             QStringLiteral("Ehhez a laphoz elrendezés tartozik. Szüntesd meg az elrendezést az "
                            "igazításhoz."),
             QStringLiteral("Így kerül a lapra."),
             QStringLiteral("Szélek: %1 mm oldalt, %2 mm felül és alul."),
             QStringLiteral("A nyomatra vágójelek kerülnek a következő lapokon: %1. Folytatod?"),
             QStringLiteral("A térköz %1 mm lett, hogy a fotók elférjenek."),
             QStringLiteral("A kiválasztott fotó nem olvasható."),
             QStringLiteral("Méret csere"),
             QStringLiteral("A nyomaton is megjelenik; a vágás a vonal közepén halad, nyomtatás "
                            "előtt megerősítést kér."),
             QStringLiteral("Csak a képernyőn látszik, a nyomatra nem kerül."),
             QStringLiteral("Nincs vágójel a képernyőn és a nyomaton."),
             QStringLiteral("A lap vágójel-beállítása is módosul: %1."),
             QStringLiteral("A fotók egy része erősen lemarad. Kapcsold be a »A kép egésze "
                            "látszódjon« opciót, vagy válaszd a »Térköz kiegyenlítése« módot."),
             QStringLiteral("A példányok pontosan a választott méretben nyomtatódnak, nagyítás "
                            "nélkül."),
             QStringLiteral("Egy lapra legfeljebb %1 példány kerül."),
             QStringLiteral("%1/%2. lap"),
             QStringLiteral("Előző lap"),
             QStringLiteral("Következő lap"),
             QStringLiteral("Figyelmeztetés"),
             QStringLiteral("Szélig nyomtatás"),
             QStringLiteral("Kikapcsolva"),
             QStringLiteral("Be"),
             QStringLiteral("A lapok szélig nyomtatása eltér. Állítsd egységesre."),
         }) {
        const QString text = translated("PhotoPrint", source);
        const QString where = QStringLiteral("%1: %2").arg(code, source);
        QVERIFY2(!text.isEmpty(), qPrintable(where + QStringLiteral(" is empty")));
        QVERIFY2(text != source,
                 qPrintable(where + QStringLiteral(" is still the Hungarian source")));
    }
}

void TestI18n::everyCataloguePluralisesTheSheetToolCounts_data()
{
    QTest::addColumn<QString>("code");
    for (const QString &code : i18n::targetCodes())
        QTest::newRow(qPrintable(code)) << code;
}

void TestI18n::everyCataloguePluralisesTheSheetToolCounts()
{
    QFETCH(QString, code);

    InstalledCatalogue catalogue(code);
    QVERIFY2(catalogue.loaded(), qPrintable(code));

    // The sheet tools count copies, sheets, photos and cells. All of them are
    // %n messages with numerus entries in every catalogue.
    for (const QString &source : {
             QStringLiteral("%n példány fér el egy lapra."),
             QStringLiteral("%n lapra kerül."),
             QStringLiteral("%n fotó helyére"),
             QStringLiteral("%n fotó: %1 oszlop × %2 sor. Egy cella %3 × %4 mm."),
             QStringLiteral("%n fotó, %1 mm térköz. A margó %2 mm."),
             QStringLiteral("A lapon most %n fotó van. A példányok ezek helyére kerülnek. Egy "
                            "lépésben visszavonható."),
             QStringLiteral("Létrehozás (%n lap)"),
         }) {
        for (const int count : {1, 2, 5}) {
            const QString text = translatedWithCount("PhotoPrint", source, count);
            const QString where =
                QStringLiteral("%1, count %2, %3").arg(code).arg(count).arg(source);
            QVERIFY2(!text.isEmpty(), qPrintable(where + QStringLiteral(": empty")));
            QVERIFY2(text != hungarianFallback(source, count),
                     qPrintable(where + QStringLiteral(": still the Hungarian fallback")));
            QVERIFY2(text.contains(QString::number(count)),
                     qPrintable(where + QStringLiteral(": the count is missing")));
            QVERIFY2(!text.contains(QStringLiteral("%n")),
                     qPrintable(where + QStringLiteral(": %n was left in the text")));
            // The source's %1 placeholders must survive in the same shape.
            QCOMPARE(text.contains(QStringLiteral("%1")),
                     source.contains(QStringLiteral("%1")));
        }
        const bool hasPluralForms =
            code != QStringLiteral("ja") && code != QStringLiteral("zh_CN");
        if (hasPluralForms) {
            QVERIFY2(translatedWithCount("PhotoPrint", source, 1)
                         != translatedWithCount("PhotoPrint", source, 2),
                     qPrintable(QStringLiteral("%1: %2 has no numerus difference")
                                    .arg(code, source)));
        }
    }
}

void TestI18n::sourceLanguageInstallsTheQtCatalogue()
{    // Hungarian is the source language, so PhotoPrint needs no catalogue of its
    // own. Qt's stock strings do, or every standard dialog button and the colour
    // picker would stay English in a Hungarian window.
    QVERIFY(i18n::installLanguage(QString()));
    QVERIFY(i18n::activeLanguage().isEmpty());
    if (i18n::installedQtBaseLanguage().isEmpty())
        QSKIP("this system ships no qtbase_hu catalogue");
    QCOMPARE(i18n::installedQtBaseLanguage(), QStringLiteral("hu"));
    QCOMPARE(QCoreApplication::translate("QPlatformTheme", "&Yes"), QStringLiteral("&Igen"));
    QCOMPARE(QCoreApplication::translate("QPlatformTheme", "&No"), QStringLiteral("&Nem"));
    QVERIFY2(QCoreApplication::translate("QColorDialog", "Select Color")
                 != QStringLiteral("Select Color"),
             "the colour picker is still English in Hungarian");
    QVERIFY2(QCoreApplication::translate("QColorDialog", "A&lpha channel:")
                 != QStringLiteral("A&lpha channel:"),
             "the colour picker's alpha control is still English in Hungarian");

    // A target language gets its own Qt catalogue, never a leftover Hungarian
    // one from the previous choice.
    QVERIFY(i18n::installLanguage(QStringLiteral("de")));
    QVERIFY(i18n::installedQtBaseLanguage() != QStringLiteral("hu"));
    QVERIFY(i18n::installLanguage(QString()));
    QCOMPARE(i18n::installedQtBaseLanguage(), QStringLiteral("hu"));
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
    QCOMPARE(translated("PhotoPrint", kPhotoIndex).arg(3), QStringLiteral("3. fotó"));
}

void TestI18n::repeatedLanguageSwitchingLeavesTranslatorsUnparented()
{
    // Switching languages repeatedly keeps working, and every translator stays
    // unparented while it is installed. Translation.cpp is their only owner;
    // the shutdown double free this guards against cannot be observed from
    // inside a test, but its cause can: a translator parented to the
    // application would be deleted by QCoreApplication as well.
    //
    // A platform theme may install translators of its own as children of the
    // application before the window is built, so the baseline is what the
    // process started with; switching a language must add none of ours.
    QSet<QTranslator *> baseline;
    for (QTranslator *translator : QCoreApplication::instance()->findChildren<QTranslator *>())
        baseline.insert(translator);
    const auto parented = [&baseline] {
        for (QTranslator *translator :
             QCoreApplication::instance()->findChildren<QTranslator *>()) {
            if (!baseline.contains(translator))
                return true;
        }
        return false;
    };
    for (int round = 0; round < 3; ++round) {
        QVERIFY(i18n::installLanguage(QStringLiteral("de")));
        QCOMPARE(translated("PhotoPrint", kPrint), QStringLiteral("Drucken"));
        QVERIFY2(!parented(), "a translator is parented to the application");
        QVERIFY(i18n::installLanguage(QStringLiteral("zh_CN")));
        QVERIFY(translated("PhotoPrint", kPrint) != kPrint);
        QVERIFY2(!parented(), "a translator is parented to the application");
        QVERIFY(i18n::installLanguage(QString()));
        QCOMPARE(translated("PhotoPrint", kPrint), kPrint);
        QVERIFY2(!parented(), "a translator is parented to the application");
    }
}

QTEST_MAIN(TestI18n)
#include "tst_i18n.moc"
