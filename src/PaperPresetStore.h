#pragma once

#include "PrintTicket.h"
#include "Result.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace pp {

struct PaperPreset {
    QString id;
    PrintContext context;
    double paperEv = 0.0;
    QString label;
    QDateTime savedAt;
};

/// Remembers one favourite paper correction per exact print environment.
class PaperPresetStore
{
public:
    explicit PaperPresetStore(QString path = QString());

    Result<void> save(const PaperPreset &preset);
    std::optional<PaperPreset> match(const PrintContext &context) const;

    /// Every readable preset. A missing file is an empty list; a corrupt file
    /// is an error, so callers can tell "no favourites yet" from "cannot read".
    Result<QList<PaperPreset>> readAll() const;

    /// Convenience wrapper kept for the read-only callers: an unreadable file
    /// looks like an empty list here. Use readAll() where that matters.
    QList<PaperPreset> all() const;

    static QString newInkIdentity();
    static QString defaultPath();

private:
    QString m_path;
};

bool samePrintContext(const PrintContext &a, const PrintContext &b);

} // namespace pp
