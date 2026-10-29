#pragma once

#include "PrintTicket.h"
#include "Result.h"

#include <QJsonObject>
#include <QList>
#include <QString>

#include <stop_token>

namespace pp {

class PrinterCatalog
{
public:
    /// Read-only discovery through the local CUPS scheduler. Never changes
    /// system configuration and never prints.
    Result<QList<PrinterCapabilities>> refresh(std::stop_token token = {}) const;

    /// Parse a PPD that was captured earlier (tests, offline reference).
    static Result<PrinterCapabilities> fromPpdText(const QString &queueName, const QByteArray &ppd);
    static Result<PrinterCapabilities> fromPpdFile(const QString &queueName, const QString &path);
};

QJsonObject capabilitiesToJson(const PrinterCapabilities &capabilities);
PrinterCapabilities capabilitiesFromJson(const QJsonObject &object);

} // namespace pp
