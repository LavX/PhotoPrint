#pragma once

#include "PaperGeometry.h"
#include "Result.h"

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <cmath>

namespace pp {

struct PageSizeCapability {
    QString option;
    QString displayName;
    double widthMm = 0.0;   ///< logical driver page
    double heightMm = 0.0;
    bool borderless = false;
    QRectF imageableMm;     ///< physical coordinates, top-left origin
    bool imageableKnown = false;
};

struct MediaTypeCapability {
    QString option;
    QString displayName;
    bool photo = false;
};

struct PrinterCapabilities {
    QString queueName;
    QString displayName;
    QString manufacturer;
    QString modelName;
    QString nickName;
    QString driverIdentity;   ///< brother-native, ipp-everywhere, unknown
    QString ppdFileName;
    QString ppdFingerprint;
    QString deviceUri;
    bool online = true;

    QList<PageSizeCapability> pageSizes;
    QList<MediaTypeCapability> mediaTypes;
    QList<QString> qualityOptions;
    QList<QString> colorOptions;
    QList<QString> duplexOptions;
    QList<QString> resolutionOptions;  ///< PPD keywords, e.g. 300dpi, 600dpi
    QString resolutionOptionName;      ///< the PPD option, normally "Resolution"
    QString defaultResolution;

    QString defaultPageSize;
    QString defaultMediaType;
    QString defaultQuality;
    QString defaultColor;
    QString defaultDuplex;

    bool borderlessSupported() const;
    const PageSizeCapability *findPageSize(const QString &option) const;
};

/// User-facing choice. Everyday names only; the adapter fills technical options.
struct PrintSelection {
    QString queueName;
    PaperSpec paper;
    bool landscape = false;
    bool borderless = false;
    bool fineQuality = true;
    bool color = true;
    int copies = 1;
    int firstPage = 1;
    int lastPage = -1;
    bool scaleToFit = false;
};

/// Fully resolved, explicit print intent. Nothing is left implicit.
/// The only error channel is the Result returned by PrinterAdapter::resolve();
/// `valid` exists so that a default-constructed or deserialised ticket is
/// visibly unusable and is set to true exactly where resolve() succeeds. It is
/// never a second, contradicting error report.
struct PrintTicket {
    bool valid = false;
    QString queueName;
    QString driverIdentity;
    QString ppdFingerprint;
    PaperGeometry geometry;

    QString pageSizeOption;
    QString mediaTypeOption;
    QString qualityOption;
    QString colorOption;
    QString duplexOption;
    QString resolutionOption;  ///< empty when the queue has no resolution option
    QString resolutionValue;
    int printResolutionDpi = 0;

    int copies = 1;
    int firstPage = 1;
    int lastPage = -1;
    bool scaleToFit = false;
};

/// Identity of the print environment a paper preset belongs to.
struct PrintContext {
    QString queueName;
    QString driverIdentity;
    QString ppdFingerprint;
    QString pageSizeOption;
    QString mediaTypeOption;
    QString qualityOption;
    QString colorOption;
    QString inkIdentity = QStringLiteral("unknown");
    double physicalWidthMm = 0.0;
    double physicalHeightMm = 0.0;
    double logicalWidthMm = 0.0;
    double logicalHeightMm = 0.0;
    bool landscape = false;
    bool borderless = false;

    /// Millimetre values come from PPD parsing and are compared with the same
    /// 0.01 mm tolerance as PaperPresetStore::samePrintContext(), so two
    /// contexts that describe the same queue are equal even when the driver
    /// reports 101.59999 instead of 101.6.
    bool operator==(const PrintContext &other) const
    {
        const auto sameMm = [](double a, double b) { return std::abs(a - b) < 0.01; };
        return queueName == other.queueName && driverIdentity == other.driverIdentity
            && ppdFingerprint == other.ppdFingerprint
            && pageSizeOption == other.pageSizeOption
            && mediaTypeOption == other.mediaTypeOption && qualityOption == other.qualityOption
            && colorOption == other.colorOption && inkIdentity == other.inkIdentity
            && sameMm(physicalWidthMm, other.physicalWidthMm)
            && sameMm(physicalHeightMm, other.physicalHeightMm)
            && sameMm(logicalWidthMm, other.logicalWidthMm)
            && sameMm(logicalHeightMm, other.logicalHeightMm)
            && landscape == other.landscape && borderless == other.borderless;
    }
};

class PrinterAdapter
{
public:
    /// Driver option for a user-defined size, e.g. "Custom.120x180mm".
    static QString customPageSizeOption(double widthMm, double heightMm);

    /// True when both dimensions fit the custom range the queue advertises
    /// (88.9-215.9 mm by 127-355.6 mm, in either orientation).
    static bool customSizeInRange(double widthMm, double heightMm);

    static Result<PrintTicket> resolve(const PrinterCapabilities &capabilities,
                                       const PrintSelection &selection);
    static PrintContext contextFor(const PrintTicket &ticket);
};

} // namespace pp
