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

    /// Physical sheet size in millimetres. Values that cannot describe a sheet
    /// (zero, negative, NaN, infinite) collapse to zero, so the rectangle is
    /// empty rather than inverted or non-finite.
    QSizeF physicalSizeMm() const;

    /// Logical sheet size the driver expects. The logical size is used only
    /// when both of its axes are declared; otherwise both axes come from the
    /// physical sheet. Mixing per axis would describe a hybrid sheet that is
    /// neither the paper nor the driver page.
    QSizeF logicalSizeMm() const;

    /// Physical page rectangle starting at the origin.
    QRectF physicalRectMm() const;

    static PaperGeometry forPaper(const PaperSpec &paper, bool landscape);
};

} // namespace pp
