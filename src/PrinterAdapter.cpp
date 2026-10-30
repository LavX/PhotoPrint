#include "PrintTicket.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

constexpr double kTolerance = 0.45;

// Custom media range advertised by the DCP-T730DW IPP queue
// (media-col-database: 88.9-215.9 mm wide, 127-355.6 mm high).
constexpr double kCustomMinWidthMm = 88.9;
constexpr double kCustomMaxWidthMm = 215.9;
constexpr double kCustomMinHeightMm = 127.0;
constexpr double kCustomMaxHeightMm = 355.6;

bool approx(double a, double b, double tolerance = kTolerance)
{
    return std::abs(a - b) <= tolerance;
}

bool sameSize(double width, double height, double otherWidth, double otherHeight)
{
    return (approx(width, otherWidth) && approx(height, otherHeight))
        || (approx(width, otherHeight) && approx(height, otherWidth));
}

bool isA4(const PaperSpec &paper)
{
    return sameSize(paper.physicalWidthMm, paper.physicalHeightMm, 210.0, 297.0);
}

struct PhotoMapping {
    double width;
    double height;
    QStringList bordered;
    QStringList borderless;
};

const PhotoMapping *photoMapping(const PaperSpec &paper)
{
    static const PhotoMapping mappings[] = {
        {88.9, 127.0,
         {QStringLiteral("BrPhotoL_S"), QStringLiteral("3.5x5")},
         {QStringLiteral("BrPhotoL_B"), QStringLiteral("3.5x5.Borderless")}},
        {100.0, 150.0,
         {QStringLiteral("BrPostC4x6_S"), QStringLiteral("4x6")},
         {QStringLiteral("BrPostC4x6_B"), QStringLiteral("4x6.Borderless")}},
        {127.0, 177.8,
         {QStringLiteral("BrPhoto2L_S"), QStringLiteral("5x7")},
         {QStringLiteral("BrPhoto2L_B"), QStringLiteral("5x7.Borderless")}},
    };
    for (const PhotoMapping &mapping : mappings) {
        if (sameSize(paper.physicalWidthMm, paper.physicalHeightMm, mapping.width, mapping.height))
            return &mapping;
    }
    return nullptr;
}

const PageSizeCapability *findNamed(const PrinterCapabilities &capabilities,
                                    const QStringList &names, bool borderless)
{
    for (const QString &name : names) {
        for (const PageSizeCapability &page : capabilities.pageSizes) {
            if (page.option.compare(name, Qt::CaseInsensitive) == 0
                && page.borderless == borderless)
                return &page;
        }
    }
    return nullptr;
}

const PageSizeCapability *findBorderedVariant(const PrinterCapabilities &capabilities,
                                              const PageSizeCapability &page, bool borderless)
{
    for (const PageSizeCapability &candidate : capabilities.pageSizes) {
        if (candidate.borderless == borderless && approx(candidate.widthMm, page.widthMm)
            && approx(candidate.heightMm, page.heightMm))
            return &candidate;
    }
    return nullptr;
}

const PageSizeCapability *findBySize(const PrinterCapabilities &capabilities, double width,
                                     double height, bool borderless)
{
    const PageSizeCapability *borderlessFallback = nullptr;
    for (const PageSizeCapability &page : capabilities.pageSizes) {
        if (!sameSize(page.widthMm, page.heightMm, width, height))
            continue;
        if (page.borderless == borderless)
            return &page;
        if (!borderless && !borderlessFallback && page.borderless)
            borderlessFallback = &page;
    }
    return borderlessFallback;
}

struct ChosenPage {
    PageSizeCapability page;
    bool found = false;
};

QString dimensionText(double millimetres)
{
    QString text = QString::number(millimetres, 'f', 1);
    if (text.endsWith(QLatin1String(".0")))
        text.chop(2);
    return text;
}

/// A user-defined sheet never borrows a fixed page mode unless its physical
/// size is exactly one of them; otherwise the driver gets its custom option.
ChosenPage choosePage(const PrinterCapabilities &capabilities,
                      const PrintSelection &selection)
{
    ChosenPage chosen;
    const PaperSpec &paper = selection.paper;
    const double targetWidth = paper.logicalWidthMm > 0.0 ? paper.logicalWidthMm
                                                          : paper.physicalWidthMm;
    const double targetHeight = paper.logicalHeightMm > 0.0 ? paper.logicalHeightMm
                                                            : paper.physicalHeightMm;

    if (paper.custom && !photoMapping(paper)) {
        chosen.page.option = PrinterAdapter::customPageSizeOption(paper.physicalWidthMm,
                                                                  paper.physicalHeightMm);
        chosen.page.displayName = chosen.page.option;
        chosen.page.widthMm = targetWidth;
        chosen.page.heightMm = targetHeight;
        chosen.page.borderless = false;
        chosen.found = true;
        return chosen;
    }

    QStringList names;
    if (const PhotoMapping *mapping = photoMapping(paper)) {
        names = selection.borderless ? mapping->borderless : mapping->bordered;
    } else if (isA4(paper)) {
        names = selection.borderless
            ? QStringList{QStringLiteral("A4.Borderless"), QStringLiteral("BrA4_B")}
            : QStringList{QStringLiteral("A4")};
    }

    if (const PageSizeCapability *page = findNamed(capabilities, names, selection.borderless)) {
        chosen.page = *page;
        chosen.found = true;
        return chosen;
    }
    if (const PageSizeCapability *page = findBySize(capabilities, targetWidth, targetHeight,
                                                    selection.borderless)) {
        chosen.page = *page;
        chosen.found = true;
        return chosen;
    }
    if (const PageSizeCapability *page = findBySize(capabilities, paper.physicalWidthMm,
                                                    paper.physicalHeightMm, selection.borderless)) {
        chosen.page = *page;
        chosen.found = true;
        return chosen;
    }
    return chosen;
}

QString pickOption(const QList<QString> &options, const QStringList &preferred,
                   const QString &fallback)
{
    for (const QString &wanted : preferred) {
        for (const QString &option : options) {
            if (option.compare(wanted, Qt::CaseInsensitive) == 0)
                return option;
        }
    }
    return fallback;
}

QString pickMedia(const PrinterCapabilities &capabilities, PaperType type)
{
    if (capabilities.mediaTypes.isEmpty())
        return {};

    QStringList preferred;
    switch (type) {
    case PaperType::Glossy:
        preferred = {QStringLiteral("PhotographicGlossy"), QStringLiteral("Glossy"),
                     QStringLiteral("Photo"), QStringLiteral("com.brother-bp-71")};
        break;
    case PaperType::Matte:
        // The driver has no matt keyword; matt photo paper is sent as inkjet.
        preferred = {QStringLiteral("Inkjet"), QStringLiteral("StationeryInkjet"),
                     QStringLiteral("Matte")};
        break;
    case PaperType::Plain:
        preferred = {QStringLiteral("Plain"), QStringLiteral("Stationery")};
        break;
    }
    for (const QString &wanted : preferred) {
        for (const MediaTypeCapability &media : capabilities.mediaTypes) {
            if (media.option.compare(wanted, Qt::CaseInsensitive) == 0)
                return media.option;
        }
    }

    if (type == PaperType::Glossy) {
        for (const MediaTypeCapability &media : capabilities.mediaTypes) {
            if (media.photo)
                return media.option;
        }
    } else {
        for (const MediaTypeCapability &media : capabilities.mediaTypes) {
            if (!media.photo)
                return media.option;
        }
    }
    return capabilities.mediaTypes.first().option;
}

} // namespace

QString PrinterAdapter::customPageSizeOption(double widthMm, double heightMm)
{
    return QStringLiteral("Custom.%1x%2mm").arg(dimensionText(widthMm), dimensionText(heightMm));
}

bool PrinterAdapter::customSizeInRange(double widthMm, double heightMm)
{
    const auto inRange = [](double value, double low, double high) {
        return value >= low - kTolerance && value <= high + kTolerance;
    };
    return (inRange(widthMm, kCustomMinWidthMm, kCustomMaxWidthMm)
            && inRange(heightMm, kCustomMinHeightMm, kCustomMaxHeightMm))
        || (inRange(heightMm, kCustomMinWidthMm, kCustomMaxWidthMm)
            && inRange(widthMm, kCustomMinHeightMm, kCustomMaxHeightMm));
}

Result<PrintTicket> PrinterAdapter::resolve(const PrinterCapabilities &capabilities,
                                            const PrintSelection &selection)
{
    if (capabilities.queueName.isEmpty())
        return error(QStringLiteral("no-queue"), QCoreApplication::translate("PhotoPrint", "Válassz nyomtatót."));
    if (selection.copies < 1 || selection.copies > 99)
        return error(QStringLiteral("copies"), QCoreApplication::translate("PhotoPrint", "A példányszám 1 és 99 között lehet."));

    const PaperSpec &paper = selection.paper;
    // Borderless is only mapped for the fixed photo sheets the driver knows.
    const bool borderlessPaper = paper.isPhotoPaper() && photoMapping(paper) != nullptr;
    if (selection.borderless && !borderlessPaper) {
        return error(QStringLiteral("borderless-unsupported"),
                     QCoreApplication::translate("PhotoPrint", "Ehhez a papírhoz nincs szélig nyomtatás."));
    }
    if (paper.custom && !photoMapping(paper)
        && !customSizeInRange(paper.physicalWidthMm, paper.physicalHeightMm)) {
        return error(QStringLiteral("custom-size-range"),
                     QCoreApplication::translate("PhotoPrint",
                                                    "Ez a papírméret nem nyomtatható. A nyomtató "
                                                    "8,9–21,6 × 12,7–35,6 cm közötti egyedi méretet "
                                                    "fogad el."));
    }

    ChosenPage chosen = choosePage(capabilities, selection);
    if (!chosen.found && selection.borderless) {
        PrintSelection relaxed = selection;
        relaxed.borderless = false;
        chosen = choosePage(capabilities, relaxed);
    }
    if (!chosen.found) {
        return error(QStringLiteral("paper-unsupported"),
                     QCoreApplication::translate("PhotoPrint", "Ezzel a papírral nem tudunk nyomtatni. Válassz másik papírt."));
    }
    if (selection.borderless && !chosen.page.borderless) {
        const PageSizeCapability *borderlessPage =
            findBorderedVariant(capabilities, chosen.page, true);
        if (!borderlessPage) {
            return error(QStringLiteral("borderless-unsupported"),
                         QCoreApplication::translate("PhotoPrint", "Ehhez a papírhoz nincs szélig nyomtatás."));
        }
        chosen.page = *borderlessPage;
    }

    PrintTicket ticket;
    ticket.valid = true;
    ticket.queueName = capabilities.queueName;
    ticket.driverIdentity = capabilities.driverIdentity;
    ticket.ppdFingerprint = capabilities.ppdFingerprint;
    ticket.pageSizeOption = chosen.page.option;
    ticket.mediaTypeOption = pickMedia(capabilities, paper.type);
    ticket.qualityOption = selection.fineQuality
        ? pickOption(capabilities.qualityOptions,
                     {QStringLiteral("Fine"), QStringLiteral("High")},
                     capabilities.defaultQuality)
        : pickOption(capabilities.qualityOptions, {QStringLiteral("Normal")},
                     capabilities.defaultQuality);
    ticket.colorOption = selection.color
        ? pickOption(capabilities.colorOptions,
                     {QStringLiteral("FullColor"), QStringLiteral("RGB"),
                      QStringLiteral("Color")},
                     capabilities.defaultColor)
        : pickOption(capabilities.colorOptions,
                     {QStringLiteral("Gray"), QStringLiteral("Grayscale"),
                      QStringLiteral("Mono")},
                     capabilities.defaultColor);
    ticket.duplexOption = pickOption(capabilities.duplexOptions, {QStringLiteral("None")},
                                     capabilities.defaultDuplex);
    ticket.copies = selection.copies;
    ticket.firstPage = std::max(1, selection.firstPage);
    ticket.lastPage = selection.lastPage >= ticket.firstPage ? selection.lastPage : -1;
    ticket.scaleToFit = selection.scaleToFit;

    ticket.geometry = PaperGeometry::forPaper(paper, selection.landscape);
    ticket.geometry.pageSizeOption = chosen.page.option;
    ticket.geometry.borderless = chosen.page.borderless;
    ticket.geometry.imageableMm =
        chosen.page.imageableMm.translated(-ticket.geometry.physicalOriginInLogicalMm);
    ticket.geometry.imageableKnown = chosen.page.imageableKnown;
    return ticket;
}

PrintContext PrinterAdapter::contextFor(const PrintTicket &ticket)
{
    PrintContext context;
    context.queueName = ticket.queueName;
    context.driverIdentity = ticket.driverIdentity;
    context.ppdFingerprint = ticket.ppdFingerprint;
    context.pageSizeOption = ticket.pageSizeOption;
    context.mediaTypeOption = ticket.mediaTypeOption;
    context.qualityOption = ticket.qualityOption;
    context.colorOption = ticket.colorOption;
    context.physicalWidthMm = ticket.geometry.physicalSizeMm().width();
    context.physicalHeightMm = ticket.geometry.physicalSizeMm().height();
    context.logicalWidthMm = ticket.geometry.logicalSizeMm().width();
    context.logicalHeightMm = ticket.geometry.logicalSizeMm().height();
    context.landscape = ticket.geometry.landscape;
    context.borderless = ticket.geometry.borderless;
    return context;
}

} // namespace pp
