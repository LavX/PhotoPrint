#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

/// Language selection and the Qt translation files that ship with PhotoPrint.
///
/// Hungarian is the source language: it is what the code says, what the tests
/// read, and what is shown when no translation is installed. The ten target
/// languages each have a .qm file embedded at :/i18n.
namespace pp::i18n {

struct Language {
    QString code;       ///< Stable code, also the .qm file suffix, e.g. "de".
    QString nativeName; ///< The name shown in the menu, written in that language.
};

/// The ten target languages, in the order the menu lists them. Hungarian is the
/// source language and is not part of this list.
const QVector<Language> &targetLanguages();

/// Canonical codes of the target languages, in menu order.
QStringList targetCodes();

/// The name to show for @p code; "Magyar" for the source language.
QString nativeName(const QString &code);

/// Maps a locale name ("de_AT", "pt_BR", "zh_CN", "ko_KR") onto a supported
/// code. Returns an empty string for Hungarian, which is both the source
/// language and the fallback for every locale we do not ship.
QString resolveLocale(const QString &localeName);

/// The language the next start uses: the saved choice when there is one,
/// otherwise the system locale if it is supported, otherwise Hungarian ("").
QString selectedLanguage();

/// Remembers @p code for the next start. Hungarian is stored explicitly so that
/// a deliberate choice survives a change of the system locale.
void saveLanguage(const QString &code);

/// Loads the translation for @p code into the application. An empty code or
/// "hu" removes any installed translator. Returns false when the file cannot be
/// read, in which case the interface stays in Hungarian.
bool installLanguage(const QString &code);

/// The language installed in this process; empty means Hungarian.
QString activeLanguage();

} // namespace pp::i18n
