#pragma once

#include "Document.h"

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

namespace pp {

/// Physical sheet, driver logical sheet and declared imageable area are three
/// different rectangles. Unknown values stay explicitly unknown.
struct PaperGeometry {
    PaperSpec paper;
    bool landscape = false;
    bool borderless = false;
    QString pageSizeOption;      ///< PPD/IPP media key the printer understands

    QRectF imageableMm;          ///< in physical coordinates, top-left origin
    bool imageableKnown = false;

    QPointF physicalOriginInLogicalMm; ///< mapping from physical to logical page
    bool offsetVerified = false;

    QSizeF physicalSizeMm() const;
    QSizeF logicalSizeMm() const;

    /// Physical page rectangle starting at the origin.
    QRectF physicalRectMm() const;

    static PaperGeometry forPaper(const PaperSpec &paper, bool landscape);
};

} // namespace pp
