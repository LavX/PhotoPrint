#pragma once

#include "PrintTicket.h"
#include "Result.h"

#include <QJsonObject>
#include <QList>
#include <QString>

#include <stop_token>

namespace pp {

/// What a CUPS destination lookup actually said. libcups reports "the scheduler
/// cannot be reached" and "the scheduler has no queues" with the same empty
/// count, so only the status separates them.
enum class LookupAnswer {
    Printers,   ///< the returned list is usable
    NoPrinters, ///< a reachable scheduler with nothing configured
    Unavailable ///< the lookup itself failed
};

/// Classifies a cupsGetDests() result: `count` as returned and `lastError` as
/// cupsLastError() reported straight after the call (an ipp_status_t). Split out
/// so the sticky-error handling can be tested without a scheduler.
LookupAnswer classifyDestinationLookup(int count, int lastError);

/// Clears the libcups last error. CUPS keeps it for the life of the process and
/// CUPS 2.x publishes no setter in its public headers, so an empty enumeration
/// could otherwise inherit a failure left behind by an earlier call. Exposed for
/// the tests, which cannot start a scheduler.
void clearCupsError();

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
