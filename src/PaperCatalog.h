#pragma once

#include "Document.h"
#include "Result.h"

#include <QList>
#include <QString>

#include <memory>

class QSettings;

namespace pp {

/// The fixed paper choices. Sizes are millimetres, the labels are centimetres.
QList<PaperSpec> defaultPapers();

/// Human label with the centimetre size, e.g. "A4 (21 × 29,7 cm)". No inches.
QString paperLabel(const PaperSpec &paper);

/// Size text in centimetres, e.g. "12 × 18 cm".
QString sizeLabelCm(double widthMm, double heightMm);

/// Everyday Hungarian name of a paper type.
QString paperTypeLabel(PaperType type);

/// Stable key used in project files and settings: plain, glossy, matte.
QString paperTypeKey(PaperType type);

/// Reads a key; an unknown or missing key falls back to the legacy photo flag.
PaperType paperTypeFromKey(const QString &key, bool legacyPhoto = false);

/// True when the physical size is one of the fixed photo papers the driver
/// knows by name (9 × 13, 10 × 15, 13 × 18 cm). Borderless is offered only here.
bool isFixedPhotoSize(const PaperSpec &paper);

/// Papers the user defined themselves, kept in QSettings. The file argument is
/// for tests; an empty path uses the application settings.
class CustomPaperStore
{
public:
    explicit CustomPaperStore(const QString &settingsFile = QString());
    ~CustomPaperStore();

    CustomPaperStore(const CustomPaperStore &) = delete;
    CustomPaperStore &operator=(const CustomPaperStore &) = delete;

    /// Every stored paper. No file yet is an empty list; a file that cannot be
    /// read, or whose array cannot be parsed, is an error, so "there are none"
    /// can never be confused with "they cannot be read".
    Result<QList<PaperSpec>> readAll() const;

    /// Convenience wrapper for the read-only callers: an unreadable file looks
    /// like an empty list here. Use readAll() where that matters.
    QList<PaperSpec> all() const;

    /// Adds the paper, or replaces the stored one with the same name.
    Result<void> save(const PaperSpec &paper);
    Result<void> remove(const QString &name);

private:
    std::unique_ptr<QSettings> m_settings;
};

} // namespace pp
