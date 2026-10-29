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
    QList<PaperPreset> all() const;

    static QString newInkIdentity();
    static QString defaultPath();

private:
    QString m_path;
};

bool samePrintContext(const PrintContext &a, const PrintContext &b);

} // namespace pp
