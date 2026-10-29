#include "LayoutEngine.h"

#include <QTransform>
#include <QtMath>

#include <cmath>

namespace pp {

namespace {

bool finitePositive(double value)
{
    return std::isfinite(value) && value > 0.0;
}

double normalizedDegrees(double degrees)
{
    double value = std::fmod(degrees, 360.0);
    if (value < 0.0)
        value += 360.0;
    return value;
}

} // namespace

double LayoutEngine::baseScale(const Frame &frame, const QSize &orientedPixels)
{
    if (!validFrame(frame) || orientedPixels.width() <= 0 || orientedPixels.height() <= 0)
        return 0.0;
    const double iw = orientedPixels.width();
    const double ih = orientedPixels.height();
    const double sx = frame.width / iw;
    const double sy = frame.height / ih;
    const double base = frame.fit == FitMode::Full ? std::min(sx, sy) : std::max(sx, sy);
    const double zoom = std::isfinite(frame.zoom) && frame.zoom > 0.0 ? frame.zoom : 1.0;
    return base * zoom;
}

QSizeF LayoutEngine::drawnSizeMm(const Frame &frame, const QSize &orientedPixels)
{
    const double scale = baseScale(frame, orientedPixels);
    if (scale <= 0.0)
        return {};
    const double iw = orientedPixels.width() * scale;
    const double ih = orientedPixels.height() * scale;
    const double rad = qDegreesToRadians(normalizedDegrees(frame.imageRotationDeg));
    const double c = std::abs(std::cos(rad));
    const double s = std::abs(std::sin(rad));
    return QSizeF(iw * c + ih * s, iw * s + ih * c);
}

QSizeF LayoutEngine::imageSizeMm(const Frame &frame, const QSize &orientedPixels)
{
    const double scale = baseScale(frame, orientedPixels);
    if (scale <= 0.0)
        return {};
    return QSizeF(orientedPixels.width() * scale, orientedPixels.height() * scale);
}

QTransform LayoutEngine::imageToPage(const Frame &frame, const QSize &orientedPixels)
{
    const double scale = baseScale(frame, orientedPixels);
    if (scale <= 0.0)
        return {};

    const double iw = orientedPixels.width();
    const double ih = orientedPixels.height();
    const double rad = qDegreesToRadians(normalizedDegrees(frame.imageRotationDeg));
    const double cosR = std::cos(rad);
    const double sinR = std::sin(rad);

    // Offset of the crop centre from the image centre, in page-axis mm.
    const double dx = (frame.cropCenter.x() - 0.5) * iw * scale;
    const double dy = (frame.cropCenter.y() - 0.5) * ih * scale;
    const double offsetX = cosR * dx - sinR * dy;
    const double offsetY = sinR * dx + cosR * dy;

    QTransform t;
    t.translate(frame.cx, frame.cy);
    t.rotate(frame.angleDeg);
    t.translate(-offsetX, -offsetY);
    t.rotate(frame.imageRotationDeg);
    t.scale(scale, scale);
    t.translate(-iw / 2.0, -ih / 2.0);
    return t;
}

QTransform LayoutEngine::pageToImage(const Frame &frame, const QSize &orientedPixels)
{
    bool invertible = false;
    const QTransform inverse = imageToPage(frame, orientedPixels).inverted(&invertible);
    return invertible ? inverse : QTransform();
}

double LayoutEngine::effectivePpi(const Frame &frame, const QSize &orientedPixels)
{
    if (!validFrame(frame) || orientedPixels.width() <= 0)
        return 0.0;
    const double scale = baseScale(frame, orientedPixels);
    if (scale <= 0.0)
        return 0.0;
    // A visible crop of frame.width mm shows frame.width / scale source pixels.
    const double visiblePixels = frame.width / scale;
    return visiblePixels / (frame.width / 25.4);
}

LayoutEngine::ResolutionHint LayoutEngine::resolutionHint(const Frame &frame,
                                                          const QSize &orientedPixels)
{
    const double ppi = effectivePpi(frame, orientedPixels);
    if (ppi <= 0.0)
        return ResolutionHint::Good;
    if (ppi < 150.0)
        return ResolutionHint::Low;
    if (ppi < 240.0)
        return ResolutionHint::Soft;
    return ResolutionHint::Good;
}

QList<Frame> LayoutEngine::arrangeGrid(const Page &page, const QList<Frame> &frames,
                                       const QHash<QString, QSize> &pixelSizes)
{
    QList<Frame> result = frames;
    const int count = result.size();
    if (count < 2)
        return result;
    const QSizeF size = pageSizeMm(page);
    if (size.width() <= 0.0 || size.height() <= 0.0)
        return result;

    const int columns = count <= 4 ? 2 : 3;
    const int rows = (count + columns - 1) / columns;
    const double margin = 10.0;
    const double gutter = 8.0;
    const double cellWidth =
        (size.width() - 2.0 * margin - (columns - 1) * gutter) / columns;
    const double cellHeight = (size.height() - 2.0 * margin - (rows - 1) * gutter) / rows;
    const double padding = 2.0;

    for (int i = 0; i < count; ++i) {
        const int column = i % columns;
        const int row = i / columns;
        const double cellX = margin + column * (cellWidth + gutter);
        const double cellY = margin + row * (cellHeight + gutter);
        Frame &frame = result[i];
        const QSize pixels = pixelSizes.value(frame.id);
        frame.angleDeg = 0.0;
        frame.imageRotationDeg = 0.0;
        frame.zoom = 1.0;
        frame.cropCenter = QPointF(0.5, 0.5);
        frame.fit = FitMode::Full;
        frame.autoPlaced = true;
        if (pixels.width() > 0 && pixels.height() > 0) {
            const double ratio = static_cast<double>(pixels.height()) / pixels.width();
            double width = cellWidth - 2.0 * padding;
            double height = width * ratio;
            if (height > cellHeight - 2.0 * padding) {
                height = cellHeight - 2.0 * padding;
                width = height / ratio;
            }
            frame.width = width;
            frame.height = height;
        } else {
            frame.width = cellWidth - 2.0 * padding;
            frame.height = cellHeight - 2.0 * padding;
        }
        frame.cx = cellX + cellWidth / 2.0;
        frame.cy = cellY + cellHeight / 2.0;
    }
    return result;
}

QList<Frame> LayoutEngine::appendPhoto(const Page &page, const QList<Frame> &frames,
                                       const Frame &newFrame,
                                       const QHash<QString, QSize> &pixelSizes)
{
    bool allAuto = !frames.isEmpty();
    for (const Frame &frame : frames) {
        if (!frame.autoPlaced) {
            allAuto = false;
            break;
        }
    }
    QList<Frame> result = frames;
    result.append(newFrame);
    if (allAuto)
        return arrangeGrid(page, result, pixelSizes);
    return result;
}

bool LayoutEngine::validFrame(const Frame &frame)
{
    return finitePositive(frame.width) && finitePositive(frame.height) && std::isfinite(frame.cx)
        && std::isfinite(frame.cy) && std::isfinite(frame.angleDeg) && std::isfinite(frame.zoom);
}

} // namespace pp
