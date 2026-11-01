#include "Translation.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

#include <memory>

namespace pp::i18n {

namespace {

constexpr auto kSettingsKey = "language";

// The translators are owned here, not by the application. QCoreApplication
// deletes every child it has at shutdown, so a translator parented to it must
// not be deleted by hand as well; keeping ownership in one place avoids the
// double delete.
std::unique_ptr<QTranslator> g_translator;
std::unique_ptr<QTranslator> g_qtTranslator;
QString g_active;
QString g_qtBase;

/// Qt's own catalogue: standard buttons, the colour picker, its alpha controls.
/// It is installed before the PhotoPrint catalogue, so the application wins
/// wherever both have an entry. A missing file is not an error, it only leaves
/// Qt's dialogs in English.
void installQtBase(QCoreApplication *application, const QString &code)
{
    if (!application || code.isEmpty())
        return;
    auto translator = std::make_unique<QTranslator>();
    const QLocale locale(code);
    if (!translator->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        return;
    }
    application->installTranslator(translator.get());
    g_qtTranslator = std::move(translator);
    g_qtBase = code;
}

QString normalized(const QString &name)
{
    QString value = name.trimmed();
    value.replace(QLatin1Char('-'), QLatin1Char('_'));
    return value;
}

QString sourceLanguage()
{
    return QStringLiteral("hu");
}

} // namespace

const QVector<Language> &targetLanguages()
{
    static const QVector<Language> languages = {
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("zh_CN"), QStringLiteral("中文")},
        {QStringLiteral("hi"), QStringLiteral("हिन्दी")},
        {QStringLiteral("es"), QStringLiteral("Español")},
        {QStringLiteral("fr"), QStringLiteral("Français")},
        {QStringLiteral("pt"), QStringLiteral("Português")},
        {QStringLiteral("ru"), QStringLiteral("Русский")},
        {QStringLiteral("ja"), QStringLiteral("日本語")},
        {QStringLiteral("de"), QStringLiteral("Deutsch")},
        {QStringLiteral("it"), QStringLiteral("Italiano")},
    };
    return languages;
}

QStringList targetCodes()
{
    QStringList codes;
    for (const Language &language : targetLanguages())
        codes.append(language.code);
    return codes;
}

QString nativeName(const QString &code)
{
    if (code.isEmpty() || code.compare(sourceLanguage(), Qt::CaseInsensitive) == 0)
        return QStringLiteral("Magyar");
    const QString canonical = resolveLocale(code);
    for (const Language &language : targetLanguages()) {
        if (language.code == canonical)
            return language.nativeName;
    }
    return QStringLiteral("Magyar");
}

QString resolveLocale(const QString &localeName)
{
    const QString name = normalized(localeName);
    if (name.isEmpty())
        return {};

    const QStringList parts = name.split(QLatin1Char('_'));
    const QString language = parts.first().toLower();
    if (language == sourceLanguage())
        return {};

    for (const Language &target : targetLanguages()) {
        if (target.code.compare(name, Qt::CaseInsensitive) == 0)
            return target.code;
    }

    if (language == QLatin1String("zh")) {
        // Only Simplified Chinese ships; Traditional Chinese falls back.
        const QString territory = parts.size() > 1 ? parts.at(1).toLower() : QString();
        if (territory.isEmpty() || territory == QLatin1String("cn")
            || territory == QLatin1String("hans") || territory == QLatin1String("sg"))
            return QStringLiteral("zh_CN");
        return {};
    }

    for (const Language &target : targetLanguages()) {
        if (target.code.section(QLatin1Char('_'), 0, 0).compare(language, Qt::CaseInsensitive) == 0)
            return target.code;
    }
    return {};
}

QString selectedLanguage()
{
    QSettings settings;
    const QVariant saved = settings.value(QString::fromLatin1(kSettingsKey));
    if (saved.isValid()) {
        const QString name = normalized(saved.toString());
        if (name.isEmpty() || name.compare(sourceLanguage(), Qt::CaseInsensitive) == 0)
            return {};
        return resolveLocale(name);
    }
    return resolveLocale(QLocale::system().name());
}

void saveLanguage(const QString &code)
{
    const QString resolved = resolveLocale(code);
    QSettings settings;
    settings.setValue(QString::fromLatin1(kSettingsKey),
                      resolved.isEmpty() ? sourceLanguage() : resolved);
}

bool installLanguage(const QString &code)
{
    QCoreApplication *application = QCoreApplication::instance();
    if (application) {
        if (g_translator)
            application->removeTranslator(g_translator.get());
        if (g_qtTranslator)
            application->removeTranslator(g_qtTranslator.get());
    }
    g_translator.reset();
    g_qtTranslator.reset();
    g_active.clear();
    g_qtBase.clear();

    const QString resolved = resolveLocale(code);
    if (resolved.isEmpty()) {
        // Hungarian is the source language: the interface needs no catalogue of
        // its own. Qt's stock strings do, or every standard button and the colour
        // picker would stay English in a Hungarian window.
        if (application)
            installQtBase(application, sourceLanguage());
        return true;
    }
    if (!application)
        return false;

    auto translator = std::make_unique<QTranslator>();
    const QString path = QStringLiteral(":/i18n/photoprint_%1.qm").arg(resolved);
    if (!translator->load(path))
        return false;

    installQtBase(application, resolved);

    application->installTranslator(translator.get());
    g_translator = std::move(translator);
    g_active = resolved;
    return true;
}

QString activeLanguage()
{
    return g_active;
}

QString installedQtBaseLanguage()
{
    return g_qtBase;
}

} // namespace pp::i18n
