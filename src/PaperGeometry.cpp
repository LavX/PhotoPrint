#include "PaperGeometry.h"

#include <cmath>

namespace pp {

namespace {

/// Millimetres that can describe a real sheet. Anything else becomes zero, so
/// a bogus value yields an empty rectangle instead of an inverted one or one
/// with non-finite coordinates.
double usableMm(double value)
{
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

} // namespace

QSizeF PaperGeometry::physicalSizeMm() const
{
    const double width = usableMm(paper.physicalWidthMm);
    const double height = usableMm(paper.physicalHeightMm);
    return landscape ? QSizeF(height, width) : QSizeF(width, height);
}

QSizeF PaperGeometry::logicalSizeMm() const
{
    const double declaredWidth = usableMm(paper.logicalWidthMm);
    const double declaredHeight = usableMm(paper.logicalHeightMm);
    // One decision for the whole sheet: either the driver page is declared or
    // it is not. Falling back per axis produced rectangles that matched neither.
    const bool declared = declaredWidth > 0.0 && declaredHeight > 0.0;
    const double width = declared ? declaredWidth : usableMm(paper.physicalWidthMm);
    const double height = declared ? declaredHeight : usableMm(paper.physicalHeightMm);
    return landscape ? QSizeF(height, width) : QSizeF(width, height);
}

QRectF PaperGeometry::physicalRectMm() const
{
    return QRectF(QPointF(0, 0), physicalSizeMm());
}

PaperGeometry PaperGeometry::forPaper(const PaperSpec &paper, bool landscape)
{
    PaperGeometry geometry;
    geometry.paper = paper;
    geometry.landscape = landscape;
    geometry.imageableMm = geometry.physicalRectMm();
    geometry.imageableKnown = false;
    geometry.physicalOriginInLogicalMm = QPointF(0, 0);
    geometry.offsetVerified = false;
    return geometry;
}

} // namespace pp
