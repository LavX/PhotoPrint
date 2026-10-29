#pragma once

#include "PrintTicket.h"
#include "Result.h"

#include <QByteArray>
#include <QString>

namespace pp {

/// Reads the PPD subset that matters for a correct ticket. Legacy PPD data
/// never leaks past this class.
class LegacyPpdReader
{
public:
    static Result<PrinterCapabilities> parse(const QByteArray &ppdText,
                                             const QString &queueName = QString());
    static Result<PrinterCapabilities> readFile(const QString &path,
                                                const QString &queueName = QString());
    static QString fingerprint(const QByteArray &ppdText);
};

} // namespace pp
