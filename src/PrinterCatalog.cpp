#include "PrinterCatalog.h"

#include "LegacyPpdReader.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>

#include <cups/cups.h>
#include <cups/ppd.h>

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

Result<QList<PrinterCapabilities>> PrinterCatalog::refresh(std::stop_token token) const
{
    cups_dest_t *dests = nullptr;
    const int count = cupsGetDests(&dests);
    if (count <= 0) {
        if (dests)
            cupsFreeDests(count, dests);
        return error(QStringLiteral("cups-unavailable"),
                     QCoreApplication::translate("PhotoPrint",
                                                  "A nyomtató most nem érhető el. Ellenőrizd, hogy be "
                                                  "van-e kapcsolva."));
    }

    QList<PrinterCapabilities> result;
    for (int i = 0; i < count; ++i) {
        if (token.stop_requested())
            break;
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
            const auto parsed = LegacyPpdReader::readFile(QString::fromLocal8Bit(ppdPath), name);
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
