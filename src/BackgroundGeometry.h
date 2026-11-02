#pragma once

#include "Document.h"

#include <QLineF>
#include <QRectF>
#include <QSize>
#include <QSizeF>

namespace pp {

/// Pure placement geometry for the page background. No widgets, no pixels, no
/// document mutation: the renderer and the tests share one answer, so a
/// gradient or a picture cannot land in a different place on screen and on
/// paper.
///
/// Conventions: page millimetres, origin at the top-left of the physical sheet,
/// X to the right, Y down, exactly like Frame and SlotGeometry.
namespace BackgroundGeometry {

/// The gradient line through the sheet centre, long enough that its projection
/// covers the whole sheet. `angleDeg` is clockwise, 0 = left to right.
QLineF gradientLineMm(const QSizeF &sheetMm, double angleDeg);

/// The source rectangle, in image pixels, that covers the sheet for Fill. The
/// overflow is cropped symmetrically, so the picture stays centred.
QRectF fillSourcePixels(const QSizeF &sheetMm, const QSize &pixels);

/// The target rectangle, in page millimetres, that contains the whole picture
/// for Fit. The bands around it show the paper (or the solid colour under it).
QRectF fitTargetMm(const QSizeF &sheetMm, const QSize &pixels);

/// The tile size in millimetres. `tileMm` 0 means the source's natural size at
/// kTileNaturalPpi; the height follows the pixel aspect. The size is raised
/// until the tile count on both axes fits kMaxTilesPerAxis, so a 2 mm tile on
/// A4 cannot ask for fifteen thousand blits.
QSizeF tileSizeMm(double tileMm, const QSize &sourcePixels, const QSizeF &sheetMm);

} // namespace BackgroundGeometry
} // namespace pp
