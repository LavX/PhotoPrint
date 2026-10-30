#pragma once

#include "PaperGeometry.h"
#include "Result.h"

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

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
struct PrintTicket {
    bool valid = false;
    QString errorText;
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

    bool operator==(const PrintContext &) const = default;
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
