#include "PhotoItem.h"

#include "LayoutEngine.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace pp::PhotoItem {

double minCoverZoom(const Frame &frame, const QSize &pixels);

namespace {

QPointF rotatePoint(const QPointF &point, double degrees)
{
    const double rad = qDegreesToRadians(degrees);
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    return QPointF(c * point.x() - s * point.y(), s * point.x() + c * point.y());
}

void cornerSigns(int corner, double *sx, double *sy)
{
    static const double xs[4] = {-1.0, 1.0, 1.0, -1.0};
    static const double ys[4] = {-1.0, -1.0, 1.0, 1.0};
    const int index = ((corner % 4) + 4) % 4;
    *sx = xs[index];
    *sy = ys[index];
}

/// Raises a Fill frame's zoom to what the current rotation needs to keep all
/// four frame corners inside the photograph. Any other fit is left alone: a
/// Full frame is allowed to show its bands.
Frame ensureCover(Frame frame, const QSize &pixels)
{
    if (frame.fit != FitMode::Fill)
        return frame;
    if (!std::isfinite(frame.zoom) || frame.zoom <= 0.0)
        frame.zoom = 1.0;
    frame.zoom = std::max(frame.zoom, minCoverZoom(frame, pixels));
    return frame;
}

} // namespace

QPolygonF frameQuadMm(const Frame &frame)
{
    QPolygonF quad;
    for (int i = 0; i < 4; ++i) {
        double sx = 0.0;
        double sy = 0.0;
        cornerSigns(i, &sx, &sy);
        const QPointF local(sx * frame.width / 2.0, sy * frame.height / 2.0);
        const QPointF rotated = rotatePoint(local, frame.angleDeg);
        quad << QPointF(frame.cx + rotated.x(), frame.cy + rotated.y());
    }
    return quad;
}

QPointF cornerMm(const Frame &frame, int corner)
{
    double sx = 0.0;
    double sy = 0.0;
    cornerSigns(corner, &sx, &sy);
    const QPointF rotated = rotatePoint(QPointF(sx * frame.width / 2.0, sy * frame.height / 2.0),
                                        frame.angleDeg);
    return QPointF(frame.cx + rotated.x(), frame.cy + rotated.y());
}

QPointF toFrameLocalMm(const Frame &frame, const QPointF &pagePoint)
{
    return rotatePoint(pagePoint - QPointF(frame.cx, frame.cy), -frame.angleDeg);
}

Frame resizedFromCorner(const Frame &frame, int corner, const QPointF &pagePoint, bool keepAspect,
                        double minimumMm)
{
    double sx = 0.0;
    double sy = 0.0;
    cornerSigns(corner, &sx, &sy);

    const QPointF fixed = cornerMm(frame, (corner + 2) % 4);
    const QPointF delta = pagePoint - fixed;
    const QPointF local = rotatePoint(delta, -frame.angleDeg);
    double width = std::max(minimumMm, std::abs(local.x()));
    double height = std::max(minimumMm, std::abs(local.y()));

    if (keepAspect && frame.width > 0.0 && frame.height > 0.0) {
        const double scale = std::max(width / frame.width, height / frame.height);
        width = std::max(minimumMm, frame.width * scale);
        height = std::max(minimumMm, frame.height * scale);
    }

    const QPointF centreOffset =
        rotatePoint(QPointF(sx * width / 2.0, sy * height / 2.0), frame.angleDeg);
    Frame result = frame;
    result.cx = fixed.x() + centreOffset.x();
    result.cy = fixed.y() + centreOffset.y();
    result.width = width;
    result.height = height;
    return result;
}

Frame clampCrop(Frame frame, const QSize &pixels)
{
    // std::clamp compares with `<`, and every comparison involving NaN is
    // false, so a NaN centre passes through untouched. Reset unusable centres
    // before clamping instead of trusting the clamp.
    if (!std::isfinite(frame.cropCenter.x()) || !std::isfinite(frame.cropCenter.y()))
        frame.cropCenter = QPointF(0.5, 0.5);

    const QSizeF image = LayoutEngine::imageSizeMm(frame, pixels);
    if (image.width() <= 0.0 || image.height() <= 0.0)
        return frame;
    // The crop centre is a point in the photograph's own axes. Only the
    // photograph's rotation inside the frame changes which part of the image
    // the frame sees; the frame's own page rotation does not, so including
    // angleDeg here over-restricted slotted and turned frames alike.
    const double rad = qDegreesToRadians(frame.imageRotationDeg);
    const double halfSpanX =
        (std::abs(frame.width * std::cos(rad)) + std::abs(frame.height * std::sin(rad))) / 2.0;
    const double halfSpanY =
        (std::abs(frame.width * std::sin(rad)) + std::abs(frame.height * std::cos(rad))) / 2.0;
    if (!std::isfinite(halfSpanX) || !std::isfinite(halfSpanY))
        return frame;

    const double halfX = halfSpanX / image.width();
    const double halfY = halfSpanY / image.height();
    frame.cropCenter.setX(halfX >= 0.5 ? 0.5 : std::clamp(frame.cropCenter.x(), halfX, 1.0 - halfX));
    frame.cropCenter.setY(halfY >= 0.5 ? 0.5 : std::clamp(frame.cropCenter.y(), halfY, 1.0 - halfY));
    return frame;
}

Frame zoomAt(Frame frame, const QPointF &pagePoint, double factor, const QSize &pixels)
{
    if (!(factor > 0.0) || !std::isfinite(factor))
        return frame;
    if (!std::isfinite(pagePoint.x()) || !std::isfinite(pagePoint.y()))
        return frame;
    // A non-finite or non-positive zoom poisons every scale below; treat it as
    // the neutral value instead of propagating NaN into the crop centre.
    if (!std::isfinite(frame.zoom) || frame.zoom <= 0.0)
        frame.zoom = 1.0;
    const QSizeF before = LayoutEngine::imageSizeMm(frame, pixels);
    if (before.width() <= 0.0 || before.height() <= 0.0)
        return frame;
    double targetZoom = std::clamp(frame.zoom * factor, kMinFrameZoom, kMaxFrameZoom);
    // Zooming out of a Fill cell stops at the point where the photograph still
    // covers it; the cover minimum depends only on the current rotation. A
    // Full frame keeps its bands and may use the whole model range.
    if (frame.fit == FitMode::Fill)
        targetZoom = std::max(targetZoom, minCoverZoom(frame, pixels));
    if (qFuzzyCompare(targetZoom, frame.zoom))
        return frame;

    const QPointF local = toFrameLocalMm(frame, pagePoint);
    const QPointF inImageManifold = rotatePoint(local, -frame.imageRotationDeg);
    const QPointF normalized =
        QPointF(frame.cropCenter.x() + inImageManifold.x() / before.width(),
                frame.cropCenter.y() + inImageManifold.y() / before.height());

    Frame result = frame;
    result.zoom = targetZoom;
    const QSizeF after = LayoutEngine::imageSizeMm(result, pixels);
    if (after.width() <= 0.0 || after.height() <= 0.0)
        return frame;
    result.cropCenter =
        QPointF(normalized.x() - inImageManifold.x() / after.width(),
                normalized.y() - inImageManifold.y() / after.height());
    return clampCrop(result, pixels);
}

Frame setZoom(Frame frame, double zoom, const QSize &pixels)
{
    if (!std::isfinite(zoom))
        return frame;
    frame.zoom = std::clamp(zoom, kMinFrameZoom, kMaxFrameZoom);
    return clampCrop(ensureCover(frame, pixels), pixels);
}

Frame rotateImageTo(Frame frame, double degrees, const QSize &pixels)
{
    if (!std::isfinite(degrees))
        return frame;
    const double target = std::clamp(degrees, kMinImageRotationDeg, kMaxImageRotationDeg);
    const bool sameRotation =
        qFuzzyCompare(target + 360.0, frame.imageRotationDeg + 360.0);
    if (!sameRotation)
        frame.imageRotationDeg = target;
    // A Fill frame must keep covering, and the rotation may have moved the
    // cover minimum; even a no-op angle still settles a stale stored zoom from
    // an older project, so the clamp runs either way.
    Frame covered = ensureCover(frame, pixels);
    covered = clampCrop(covered, pixels);
    return covered;
}

double minCoverZoom(const Frame &frame, const QSize &pixels)
{
    if (frame.fit != FitMode::Fill || pixels.width() <= 0 || pixels.height() <= 0
        || !(frame.width > 0.0) || !(frame.height > 0.0)) {
        return 1.0;
    }
    Frame neutral = frame;
    neutral.zoom = 1.0;
    const QSizeF image = LayoutEngine::imageSizeMm(neutral, pixels);
    if (!(image.width() > 0.0) || !(image.height() > 0.0))
        return 1.0;

    const double rad = qDegreesToRadians(normalizedImageRotationDeg(frame.imageRotationDeg));
    const double c = std::abs(std::cos(rad));
    const double s = std::abs(std::sin(rad));
    // The rotated photograph must contain the frame's bounding box in the
    // image's own axes: frame.width*|cos| + frame.height*|sin| across, and the
    // swapped sum down. Both requirements scale linearly with the zoom.
    const double needWidth = frame.width * c + frame.height * s;
    const double needHeight = frame.width * s + frame.height * c;
    const double required = std::max(needWidth / image.width(), needHeight / image.height());
    if (!std::isfinite(required))
        return 1.0;
    // Fill covers at zoom 1 by construction; the floor also absorbs floating
    // error that would otherwise leave a hairline of paper at 0 degrees.
    return std::clamp(std::max(1.0, required), kMinFrameZoom, kMaxFrameZoom);
}

Frame rotateImageBy(Frame frame, double delta, const QSize &pixels)
{
    if (!std::isfinite(delta))
        return frame;
    return rotateImageTo(frame, normalizedImageRotationDeg(frame.imageRotationDeg + delta),
                         pixels);
}

Frame resetInsideFrame(Frame frame)
{
    frame.zoom = 1.0;
    frame.cropCenter = QPointF(0.5, 0.5);
    frame.imageRotationDeg = 0.0;
    return frame;
}

} // namespace pp::PhotoItem
