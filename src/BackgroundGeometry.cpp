#include "BackgroundGeometry.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace pp {
namespace BackgroundGeometry {

QLineF gradientLineMm(const QSizeF &sheetMm, double angleDeg)
{
    if (!(sheetMm.width() > 0.0) || !(sheetMm.height() > 0.0))
        return {};
    const double radians = qDegreesToRadians(angleDeg);
    const double dx = std::cos(radians);
    const double dy = std::sin(radians);
    const QPointF centre(sheetMm.width() / 2.0, sheetMm.height() / 2.0);
    // Half the projection of the sheet onto the gradient direction: the line
    // then starts and ends exactly on the two corners that bound the ramp, so
    // the first and last stop land on the sheet's edges.
    const double half =
        0.5 * (std::abs(sheetMm.width() * dx) + std::abs(sheetMm.height() * dy));
    const QPointF direction(dx, dy);
    return QLineF(centre - direction * half, centre + direction * half);
}

QRectF fillSourcePixels(const QSizeF &sheetMm, const QSize &pixels)
{
    if (pixels.isEmpty() || !(sheetMm.width() > 0.0) || !(sheetMm.height() > 0.0))
        return {};
    // Millimetres per source pixel that make both axes cover the sheet.
    const double scale = std::max(sheetMm.width() / pixels.width(),
                                  sheetMm.height() / pixels.height());
    if (!(scale > 0.0) || !std::isfinite(scale))
        return {};
    const double sourceWidth = sheetMm.width() / scale;
    const double sourceHeight = sheetMm.height() / scale;
    return QRectF((pixels.width() - sourceWidth) / 2.0,
                  (pixels.height() - sourceHeight) / 2.0, sourceWidth, sourceHeight);
}

QRectF fitTargetMm(const QSizeF &sheetMm, const QSize &pixels)
{
    if (pixels.isEmpty() || !(sheetMm.width() > 0.0) || !(sheetMm.height() > 0.0))
        return {};
    const double scale = std::min(sheetMm.width() / pixels.width(),
                                  sheetMm.height() / pixels.height());
    if (!(scale > 0.0) || !std::isfinite(scale))
        return {};
    const double width = pixels.width() * scale;
    const double height = pixels.height() * scale;
    return QRectF((sheetMm.width() - width) / 2.0, (sheetMm.height() - height) / 2.0,
                  width, height);
}

QSizeF tileSizeMm(double tileMm, const QSize &sourcePixels, const QSizeF &sheetMm)
{
    if (sourcePixels.isEmpty() || !(sheetMm.width() > 0.0) || !(sheetMm.height() > 0.0))
        return {};
    double width = tileMm;
    if (!(width > 0.0) || !std::isfinite(width))
        width = sourcePixels.width() / double(kTileNaturalPpi) * 25.4;
    if (!(width > 0.0) || !std::isfinite(width))
        return {};
    double height = width * sourcePixels.height() / sourcePixels.width();
    if (!(height > 0.0) || !std::isfinite(height))
        return {};

    // Raise the tile until the count fits on both axes. The factor is the
    // larger of the two demands, so one pass is enough and the aspect is kept.
    const auto count = [](double extent, double tile) {
        return tile > 0.0 ? int(std::ceil(extent / tile)) : 0;
    };
    double factor = 1.0;
    if (count(sheetMm.width(), width) > kMaxTilesPerAxis)
        factor = std::max(factor, sheetMm.width() / (kMaxTilesPerAxis * width));
    if (count(sheetMm.height(), height) > kMaxTilesPerAxis)
        factor = std::max(factor, sheetMm.height() / (kMaxTilesPerAxis * height));
    return QSizeF(width * factor, height * factor);
}

} // namespace BackgroundGeometry
} // namespace pp
