#include "PrinterCatalog.h"

#include "LegacyPpdReader.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonArray>

#include <cups/cups.h>
#include <cups/ppd.h>

// CUPS 2.x dropped cupsSetError() from its public headers, but libcups still
// exports the setter its own client code uses. Declared weak: if a future CUPS
// stops exporting it, the reference stays null and an empty enumeration simply
// keeps the old, sticky behaviour instead of the program failing to link.
//
// This declaration belongs at global scope. Inside the unnamed namespace it
// would have internal linkage, which contradicts what it declares: a function
// that libcups defines in another translation unit. GCC happens to emit a weak
// undefined reference for it anyway, but that is compiler behaviour, not a
// promise, and the linkage in the source would be wrong. At global scope the
// reference is a weak undefined symbol: resolved against libcups when it
// exports one, left null when it does not.
extern "C" __attribute__((weak)) void _cupsSetError(ipp_status_t status, const char *message,
                                                    int localize);

namespace pp {

namespace {

QString optionValue(cups_dest_t *dest, const char *name)
{
    const char *value = cupsGetOption(name, dest->num_options, dest->options);
    return value ? QString::fromLocal8Bit(value) : QString();
}

QJsonObject pageToJson(const PageSizeCapability &page)
{
    QJsonObject object;
    object.insert(QStringLiteral("option"), page.option);
    object.insert(QStringLiteral("display"), page.displayName);
    object.insert(QStringLiteral("widthMm"), page.widthMm);
    object.insert(QStringLiteral("heightMm"), page.heightMm);
    object.insert(QStringLiteral("borderless"), page.borderless);
    object.insert(QStringLiteral("imageableKnown"), page.imageableKnown);
    if (page.imageableKnown) {
        object.insert(QStringLiteral("imageableMm"),
                      QJsonArray{page.imageableMm.left(), page.imageableMm.top(),
                                 page.imageableMm.right(), page.imageableMm.bottom()});
    }
    return object;
}

PageSizeCapability pageFromJson(const QJsonObject &object)
{
    PageSizeCapability page;
    page.option = object.value(QStringLiteral("option")).toString();
    page.displayName = object.value(QStringLiteral("display")).toString();
    page.widthMm = object.value(QStringLiteral("widthMm")).toDouble();
    page.heightMm = object.value(QStringLiteral("heightMm")).toDouble();
    page.borderless = object.value(QStringLiteral("borderless")).toBool();
    page.imageableKnown = object.value(QStringLiteral("imageableKnown")).toBool();
    const QJsonArray area = object.value(QStringLiteral("imageableMm")).toArray();
    if (area.size() == 4) {
        page.imageableMm = QRectF(area.at(0).toDouble(), area.at(1).toDouble(),
                                  area.at(2).toDouble() - area.at(0).toDouble(),
                                  area.at(3).toDouble() - area.at(1).toDouble());
    }
    return page;
}

} // namespace

void clearCupsError()
{
    if (_cupsSetError)
        _cupsSetError(IPP_STATUS_OK, nullptr, 0);
}

LookupAnswer classifyDestinationLookup(int count, int lastError)
{
    if (count > 0)
        return LookupAnswer::Printers;
    // libcups reports a failed lookup as 0, exactly like "no queues
    // configured", so the status is the only signal that separates the two.
    // A negative count is not something libcups returns today; it stays an
    // error rather than being read as an empty answer.
    if (count < 0 || lastError != IPP_STATUS_OK)
        return LookupAnswer::Unavailable;
    return LookupAnswer::NoPrinters;
}

Result<QList<PrinterCapabilities>> PrinterCatalog::refresh(std::stop_token token) const
{
    // The last error is sticky: one left behind by an earlier call would make a
    // successful, empty enumeration look like a broken scheduler. A real failure
    // always sets a fresh one, so clearing it here cannot hide anything.
    clearCupsError();
    cups_dest_t *dests = nullptr;
    const int count = cupsGetDests(&dests);
    const int lastError = int(cupsLastError());
    switch (classifyDestinationLookup(count, lastError)) {
    case LookupAnswer::Unavailable:
        // libcups normally returns no array on a failed lookup, but nothing in
        // its contract promises that, and a leaked one would outlive the call.
        if (dests)
            cupsFreeDests(count, dests);
        return error(QStringLiteral("cups-unavailable"),
                     QCoreApplication::translate("PhotoPrint",
                                                 "A nyomtató most nem érhető el. Ellenőrizd, hogy be "
                                                 "van-e kapcsolva."));
    case LookupAnswer::NoPrinters:
        // A reachable scheduler with no queues is a real answer, not a CUPS
        // failure, and must not be reported as one.
        if (dests)
            cupsFreeDests(count, dests);
        return error(QStringLiteral("no-printers"),
                     QCoreApplication::translate("PhotoPrint", "Nincs nyomtató"));
    case LookupAnswer::Printers:
        break;
    }

    QList<PrinterCapabilities> result;
    for (int i = 0; i < count; ++i) {
        if (token.stop_requested()) {
            // Never hand a half-read queue list to the caller as if it were the
            // complete answer.
            cupsFreeDests(count, dests);
            return error(QStringLiteral("cups-cancelled"),
                         QCoreApplication::translate("PhotoPrint", "A művelet megszakadt."));
        }
        cups_dest_t *dest = &dests[i];
        const QString name = QString::fromLocal8Bit(dest->name);
        PrinterCapabilities capabilities;
        capabilities.queueName = name;
        capabilities.displayName = optionValue(dest, "printer-info");
        if (capabilities.displayName.isEmpty())
            capabilities.displayName = name;
        capabilities.deviceUri = optionValue(dest, "device-uri");
        const QString accepting = optionValue(dest, "printer-is-accepting-jobs");
        capabilities.online = accepting.isEmpty() || accepting != QLatin1String("false");

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        const char *ppdPath = cupsGetPPD(dest->name);
#pragma GCC diagnostic pop
        if (ppdPath) {
            const QString tempPpd = QString::fromLocal8Bit(ppdPath);
            const auto parsed = LegacyPpdReader::readFile(tempPpd, name);
            // cupsGetPPD() hands out a temporary file that belongs to the
            // caller; nothing else deletes it, so one refresh would leak one
            // file per queue.
            if (!QFile::remove(tempPpd))
                qWarning("Could not remove the temporary PPD %s", qUtf8Printable(tempPpd));
            if (parsed) {
                capabilities = parsed.value();
                capabilities.displayName = optionValue(dest, "printer-info");
                if (capabilities.displayName.isEmpty())
                    capabilities.displayName = name;
                capabilities.deviceUri = optionValue(dest, "device-uri");
                capabilities.online = accepting.isEmpty() || accepting != QLatin1String("false");
            } else {
                capabilities.driverIdentity = QStringLiteral("unknown");
            }
        } else {
            capabilities.driverIdentity = QStringLiteral("unknown");
        }
        result.append(capabilities);
    }
    cupsFreeDests(count, dests);
    return result;
}

Result<PrinterCapabilities> PrinterCatalog::fromPpdText(const QString &queueName,
                                                        const QByteArray &ppd)
{
    return LegacyPpdReader::parse(ppd, queueName);
}

Result<PrinterCapabilities> PrinterCatalog::fromPpdFile(const QString &queueName,
                                                        const QString &path)
{
    return LegacyPpdReader::readFile(path, queueName);
}

QJsonObject capabilitiesToJson(const PrinterCapabilities &capabilities)
{
    QJsonObject object;
    object.insert(QStringLiteral("queue"), capabilities.queueName);
    object.insert(QStringLiteral("display"), capabilities.displayName);
    object.insert(QStringLiteral("manufacturer"), capabilities.manufacturer);
    object.insert(QStringLiteral("model"), capabilities.modelName);
    object.insert(QStringLiteral("nickName"), capabilities.nickName);
    object.insert(QStringLiteral("driverIdentity"), capabilities.driverIdentity);
    object.insert(QStringLiteral("ppdFileName"), capabilities.ppdFileName);
    object.insert(QStringLiteral("ppdFingerprint"), capabilities.ppdFingerprint);
    object.insert(QStringLiteral("deviceUri"), capabilities.deviceUri);
    object.insert(QStringLiteral("online"), capabilities.online);

    QJsonArray pages;
    for (const PageSizeCapability &page : capabilities.pageSizes)
        pages.append(pageToJson(page));
    object.insert(QStringLiteral("pageSizes"), pages);

    QJsonArray media;
    for (const MediaTypeCapability &type : capabilities.mediaTypes) {
        media.append(QJsonObject{{QStringLiteral("option"), type.option},
                                 {QStringLiteral("display"), type.displayName},
                                 {QStringLiteral("photo"), type.photo}});
    }
    object.insert(QStringLiteral("mediaTypes"), media);

    auto stringArray = [](const QList<QString> &values) {
        QJsonArray array;
        for (const QString &value : values)
            array.append(value);
        return array;
    };
    object.insert(QStringLiteral("qualityOptions"), stringArray(capabilities.qualityOptions));
    object.insert(QStringLiteral("resolutionOptions"), stringArray(capabilities.resolutionOptions));
    object.insert(QStringLiteral("resolutionOptionName"), capabilities.resolutionOptionName);
    object.insert(QStringLiteral("colorOptions"), stringArray(capabilities.colorOptions));
    object.insert(QStringLiteral("duplexOptions"), stringArray(capabilities.duplexOptions));
    object.insert(QStringLiteral("defaultPageSize"), capabilities.defaultPageSize);
    object.insert(QStringLiteral("defaultMediaType"), capabilities.defaultMediaType);
    object.insert(QStringLiteral("defaultQuality"), capabilities.defaultQuality);
    object.insert(QStringLiteral("defaultResolution"), capabilities.defaultResolution);
    object.insert(QStringLiteral("defaultColor"), capabilities.defaultColor);
    object.insert(QStringLiteral("defaultDuplex"), capabilities.defaultDuplex);
    return object;
}

PrinterCapabilities capabilitiesFromJson(const QJsonObject &object)
{
    PrinterCapabilities capabilities;
    capabilities.queueName = object.value(QStringLiteral("queue")).toString();
    capabilities.displayName = object.value(QStringLiteral("display")).toString();
    capabilities.manufacturer = object.value(QStringLiteral("manufacturer")).toString();
    capabilities.modelName = object.value(QStringLiteral("model")).toString();
    capabilities.nickName = object.value(QStringLiteral("nickName")).toString();
    capabilities.driverIdentity = object.value(QStringLiteral("driverIdentity")).toString();
    capabilities.ppdFileName = object.value(QStringLiteral("ppdFileName")).toString();
    capabilities.ppdFingerprint = object.value(QStringLiteral("ppdFingerprint")).toString();
    capabilities.deviceUri = object.value(QStringLiteral("deviceUri")).toString();
    capabilities.online = object.value(QStringLiteral("online")).toBool(true);

    for (const QJsonValue &value : object.value(QStringLiteral("pageSizes")).toArray())
        capabilities.pageSizes.append(pageFromJson(value.toObject()));
    for (const QJsonValue &value : object.value(QStringLiteral("mediaTypes")).toArray()) {
        const QJsonObject media = value.toObject();
        capabilities.mediaTypes.append({media.value(QStringLiteral("option")).toString(),
                                        media.value(QStringLiteral("display")).toString(),
                                        media.value(QStringLiteral("photo")).toBool()});
    }
    auto readArray = [&](const QString &key) {
        QList<QString> values;
        for (const QJsonValue &value : object.value(key).toArray())
            values.append(value.toString());
        return values;
    };
    capabilities.qualityOptions = readArray(QStringLiteral("qualityOptions"));
    capabilities.resolutionOptions = readArray(QStringLiteral("resolutionOptions"));
    capabilities.colorOptions = readArray(QStringLiteral("colorOptions"));
    capabilities.duplexOptions = readArray(QStringLiteral("duplexOptions"));
    capabilities.defaultPageSize = object.value(QStringLiteral("defaultPageSize")).toString();
    capabilities.defaultMediaType = object.value(QStringLiteral("defaultMediaType")).toString();
    capabilities.defaultQuality = object.value(QStringLiteral("defaultQuality")).toString();
    capabilities.resolutionOptionName = object.value(QStringLiteral("resolutionOptionName")).toString();
    capabilities.defaultResolution = object.value(QStringLiteral("defaultResolution")).toString();
    capabilities.defaultColor = object.value(QStringLiteral("defaultColor")).toString();
    capabilities.defaultDuplex = object.value(QStringLiteral("defaultDuplex")).toString();
    return capabilities;
}

} // namespace pp
