#include "LegacyPpdReader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace pp {

namespace {

constexpr double kPtToMm = 25.4 / 72.0;

struct Area {
    double llx = 0;
    double lly = 0;
    double urx = 0;
    double ury = 0;
};

QString unquote(const QString &value)
{
    QString text = value.trimmed();
    if (text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"')))
        text = text.mid(1, text.size() - 2);
    return text;
}

QStringList numbers(const QString &value)
{
    static const QRegularExpression re(QStringLiteral("[-+]?[0-9]*\\.?[0-9]+"));
    QStringList result;
    auto it = re.globalMatch(value);
    while (it.hasNext())
        result.append(it.next().captured(0));
    return result;
}

bool parseArea(const QString &value, Area *area)
{
    const QStringList parts = numbers(value);
    if (parts.size() < 4)
        return false;
    area->llx = parts.at(0).toDouble();
    area->lly = parts.at(1).toDouble();
    area->urx = parts.at(2).toDouble();
    area->ury = parts.at(3).toDouble();
    return true;
}

void splitOption(const QString &raw, QString *option, QString *display)
{
    const int slash = raw.indexOf(QLatin1Char('/'));
    if (slash < 0) {
        *option = raw.trimmed();
        *display = *option;
        return;
    }
    *option = raw.left(slash).trimmed();
    *display = raw.mid(slash + 1).trimmed();
    if (display->isEmpty())
        *display = *option;
}

QString pickOption(const QStringList &options, const QStringList &preferred, const QString &fallback)
{
    for (const QString &wanted : preferred) {
        for (const QString &option : options) {
            if (option.compare(wanted, Qt::CaseInsensitive) == 0)
                return option;
        }
    }
    return fallback;
}

} // namespace

QString LegacyPpdReader::fingerprint(const QByteArray &ppdText)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(ppdText, QCryptographicHash::Sha256).toHex());
}

Result<PrinterCapabilities> LegacyPpdReader::parse(const QByteArray &ppdText,
                                                   const QString &queueName)
{
    if (ppdText.isEmpty())
        return error(QStringLiteral("empty-ppd"), QCoreApplication::translate("PhotoPrint", "A nyomtató leírása üres."));

    PrinterCapabilities capabilities;
    capabilities.queueName = queueName;
    capabilities.ppdFingerprint = fingerprint(ppdText);

    QHash<QString, QSizeF> dimensions;  // option -> logical size in mm
    QHash<QString, Area> areas;         // option -> imageable area in points
    QList<PageSizeCapability> pageSizes;
    QStringList mediaOptions;
    QHash<QString, QString> mediaDisplay;
    QStringList qualityOptions;
    QStringList colorOptions;
    QStringList duplexOptions;
    QHash<QString, QString> defaults;

    bool nativeBrother = false;
    bool ippEverywhere = false;

    const QString text = QString::fromLatin1(ppdText);
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        QString line = rawLine.trimmed();
        if (!line.startsWith(QLatin1Char('*')))
            continue;
        line.remove(0, 1);
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString left = line.left(colon).trimmed();
        const QString value = unquote(line.mid(colon + 1));

        const int space = left.indexOf(QLatin1Char(' '));
        const QString key = space < 0 ? left : left.left(space);
        const QString optionRaw = space < 0 ? QString() : left.mid(space + 1).trimmed();

        if (key == QLatin1String("Manufacturer"))
            capabilities.manufacturer = value;
        else if (key == QLatin1String("ModelName"))
            capabilities.modelName = value;
        else if (key == QLatin1String("NickName"))
            capabilities.nickName = value;
        else if (key == QLatin1String("PCFileName"))
            capabilities.ppdFileName = value;
        else if (key.startsWith(QLatin1String("Default"))) {
            const QString name = key.mid(7);
            if (!optionRaw.isEmpty())
                defaults.insert(name, optionRaw);
            else if (!value.isEmpty())
                defaults.insert(name, unquote(value));
        }

        if (key == QLatin1String("cupsFilter") || key == QLatin1String("cupsFilter2"))
            if (value.contains(QLatin1String("image/urf")))
                ippEverywhere = true;

        if (key == QLatin1String("PageSize") && !optionRaw.isEmpty()) {
            QString option;
            QString display;
            splitOption(optionRaw, &option, &display);
            PageSizeCapability page;
            page.option = option;
            page.displayName = display;
            page.borderless = option.endsWith(QLatin1String(".Borderless"), Qt::CaseInsensitive)
                || option.endsWith(QLatin1String("_B"))
                || display.contains(QLatin1String("borderless"), Qt::CaseInsensitive);
            pageSizes.append(page);
        } else if (key == QLatin1String("PaperDimension") && !optionRaw.isEmpty()) {
            QString option;
            QString display;
            splitOption(optionRaw, &option, &display);
            const QStringList parts = numbers(value);
            if (parts.size() >= 2) {
                dimensions.insert(option,
                                  QSizeF(parts.at(0).toDouble() * kPtToMm,
                                         parts.at(1).toDouble() * kPtToMm));
            }
        } else if (key == QLatin1String("ImageableArea") && !optionRaw.isEmpty()) {
            QString option;
            QString display;
            splitOption(optionRaw, &option, &display);
            Area area;
            if (parseArea(value, &area))
                areas.insert(option, area);
        } else if (key == QLatin1String("MediaType") || key == QLatin1String("BRMediaType")) {
            if (key == QLatin1String("BRMediaType"))
                nativeBrother = true;
            if (!optionRaw.isEmpty()) {
                QString option;
                QString display;
                splitOption(optionRaw, &option, &display);
                if (!mediaOptions.contains(option)) {
                    mediaOptions.append(option);
                    mediaDisplay.insert(option, display);
                }
            }
        } else if (key == QLatin1String("Resolution") || key == QLatin1String("BRResolution")
                   || key == QLatin1String("cupsPrintQuality")) {
            if (key == QLatin1String("BRResolution"))
                nativeBrother = true;
            if (!optionRaw.isEmpty()) {
                QString option;
                QString display;
                splitOption(optionRaw, &option, &display);
                if (!qualityOptions.contains(option))
                    qualityOptions.append(option);
            }
        } else if (key == QLatin1String("ColorModel") || key == QLatin1String("BRMonoColor")) {
            if (key == QLatin1String("BRMonoColor"))
                nativeBrother = true;
            if (!optionRaw.isEmpty()) {
                QString option;
                QString display;
                splitOption(optionRaw, &option, &display);
                if (!colorOptions.contains(option))
                    colorOptions.append(option);
            }
        } else if (key == QLatin1String("Duplex")) {
            if (!optionRaw.isEmpty()) {
                QString option;
                QString display;
                splitOption(optionRaw, &option, &display);
                if (!duplexOptions.contains(option))
                    duplexOptions.append(option);
            }
        }
    }

    if (capabilities.ppdFileName == QLatin1String("ippeve.ppd"))
        ippEverywhere = true;

    if (nativeBrother)
        capabilities.driverIdentity = QStringLiteral("brother-native");
    else if (ippEverywhere)
        capabilities.driverIdentity = QStringLiteral("ipp-everywhere");
    else
        capabilities.driverIdentity = QStringLiteral("unknown");

    for (PageSizeCapability &page : pageSizes) {
        const auto size = dimensions.constFind(page.option);
        if (size != dimensions.constEnd()) {
            page.widthMm = size->width();
            page.heightMm = size->height();
        }
        const auto area = areas.constFind(page.option);
        if (area != areas.constEnd() && page.heightMm > 0.0) {
            const double hPt = page.heightMm / kPtToMm;
            page.imageableMm = QRectF(
                QPointF(area->llx * kPtToMm, (hPt - area->ury) * kPtToMm),
                QPointF(area->urx * kPtToMm, (hPt - area->lly) * kPtToMm));
            page.imageableKnown = true;
        }
        if (page.displayName.isEmpty())
            page.displayName = page.option;
    }

    capabilities.pageSizes = pageSizes;
    for (const QString &option : mediaOptions) {
        const QString display = mediaDisplay.value(option, option);
        const bool photo = option.contains(QLatin1String("Glossy"), Qt::CaseInsensitive)
            || display.contains(QLatin1String("Glossy"), Qt::CaseInsensitive)
            || display.contains(QLatin1String("Photo"), Qt::CaseInsensitive)
            || option.contains(QLatin1String("Photo"), Qt::CaseInsensitive);
        capabilities.mediaTypes.append({option, display, photo});
    }
    capabilities.qualityOptions = qualityOptions;
    capabilities.colorOptions = colorOptions;
    capabilities.duplexOptions = duplexOptions;

    capabilities.defaultPageSize = defaults.value(QStringLiteral("PageSize"));
    capabilities.defaultMediaType = defaults.value(QStringLiteral("MediaType"),
                                                   defaults.value(QStringLiteral("BRMediaType")));
    capabilities.defaultQuality = defaults.value(QStringLiteral("cupsPrintQuality"),
                                                 defaults.value(QStringLiteral("BRResolution")));
    capabilities.defaultColor = defaults.value(QStringLiteral("ColorModel"),
                                               defaults.value(QStringLiteral("BRMonoColor")));
    capabilities.defaultDuplex = defaults.value(QStringLiteral("Duplex"));

    if (!capabilities.modelName.isEmpty()) {
        capabilities.displayName =
            (capabilities.manufacturer + QLatin1Char(' ') + capabilities.modelName).simplified();
    } else {
        capabilities.displayName = capabilities.nickName.isEmpty() ? queueName
                                                                    : capabilities.nickName;
    }
    return capabilities;
}

Result<PrinterCapabilities> LegacyPpdReader::readFile(const QString &path, const QString &queueName)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return error(QStringLiteral("ppd-unreadable"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtató beállításait nem tudjuk elolvasni."));
    return parse(file.readAll(), queueName);
}

bool PrinterCapabilities::borderlessSupported() const
{
    return std::any_of(pageSizes.begin(), pageSizes.end(),
                       [](const PageSizeCapability &page) { return page.borderless; });
}

const PageSizeCapability *PrinterCapabilities::findPageSize(const QString &option) const
{
    for (const PageSizeCapability &page : pageSizes) {
        if (page.option == option)
            return &page;
    }
    return nullptr;
}

} // namespace pp
