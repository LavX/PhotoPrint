#include "Translation.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace pp::i18n {

namespace {

constexpr auto kSettingsKey = "language";

QTranslator *g_translator = nullptr;
QTranslator *g_qtTranslator = nullptr;
QString g_active;

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
            application->removeTranslator(g_translator);
        if (g_qtTranslator)
            application->removeTranslator(g_qtTranslator);
    }
    delete g_translator;
    delete g_qtTranslator;
    g_translator = nullptr;
    g_qtTranslator = nullptr;
    g_active.clear();

    const QString resolved = resolveLocale(code);
    if (resolved.isEmpty())
        return true;
    if (!application)
        return false;

    auto *translator = new QTranslator(application);
    const QString path = QStringLiteral(":/i18n/photoprint_%1.qm").arg(resolved);
    if (!translator->load(path)) {
        delete translator;
        return false;
    }

    // Qt's own stock strings (standard dialog buttons, file dialogs) live in
    // the qtbase catalogue of the system. It is installed first, so the
    // application catalogue installed after it wins wherever both have an
    // entry.
    auto *qtTranslator = new QTranslator(application);
    const QLocale locale(resolved);
    if (qtTranslator->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                           QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        application->installTranslator(qtTranslator);
        g_qtTranslator = qtTranslator;
    } else {
        delete qtTranslator;
    }

    application->installTranslator(translator);
    g_translator = translator;
    g_active = resolved;
    return true;
}

QString activeLanguage()
{
    return g_active;
}

} // namespace pp::i18n
