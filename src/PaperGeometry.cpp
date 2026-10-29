#include "PaperGeometry.h"

namespace pp {

QSizeF PaperGeometry::physicalSizeMm() const
{
    return landscape ? QSizeF(paper.physicalHeightMm, paper.physicalWidthMm)
                     : QSizeF(paper.physicalWidthMm, paper.physicalHeightMm);
}

QSizeF PaperGeometry::logicalSizeMm() const
{
    const double width = paper.logicalWidthMm > 0.0 ? paper.logicalWidthMm : paper.physicalWidthMm;
    const double height =
        paper.logicalHeightMm > 0.0 ? paper.logicalHeightMm : paper.physicalHeightMm;
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
