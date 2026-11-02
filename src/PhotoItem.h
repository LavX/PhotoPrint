#pragma once

#include "Document.h"

#include <QPointF>
#include <QPolygonF>
#include <QSize>

namespace pp::PhotoItem {

/// Frame rectangle in page millimetres, corners clockwise from top-left.
QPolygonF frameQuadMm(const Frame &frame);

/// Handle centre for a corner (0..3, clockwise from top-left) in page mm.
QPointF cornerMm(const Frame &frame, int corner);

/// Page point expressed in the unrotated frame axes, origin at frame centre.
QPointF toFrameLocalMm(const Frame &frame, const QPointF &pagePoint);

/// Result of dragging a corner. The opposite corner stays put; the aspect
/// ratio is preserved unless `keepAspect` is false.
Frame resizedFromCorner(const Frame &frame, int corner, const QPointF &pagePoint, bool keepAspect,
                        double minimumMm = 8.0);

/// Keeps the visible crop inside the photograph.
Frame clampCrop(Frame frame, const QSize &pixels);

/// Multiplies the inner zoom while keeping the image point under `pagePoint`
/// fixed. The page view zoom is untouched.
Frame zoomAt(Frame frame, const QPointF &pagePoint, double factor, const QSize &pixels);

/// The smallest inner zoom at which a Fill frame is fully covered by the
/// rotated photograph, so no paper shows inside the cell. For a frame that is
/// not Fill, or without usable pixels, the neutral 1.0 is returned.
double minCoverZoom(const Frame &frame, const QSize &pixels);

/// Sets the inner zoom directly, the way the inspector's slider does. The
/// zoom is clamped to the shared range and the visible crop is kept inside
/// the photograph. A Fill frame is raised to the cover minimum, so shrinking
/// below it never exposes paper inside the cell. The frame rectangle is never
/// touched.
Frame setZoom(Frame frame, double zoom, const QSize &pixels);

/// Sets the photograph's rotation inside the frame. The angle is clamped to
/// [-180, 180], so a slider position and the stored value always agree. A Fill
/// frame is raised to the zoom its new rotation needs to keep covering.
Frame rotateImageTo(Frame frame, double degrees, const QSize &pixels);

/// Adds `delta` degrees to the photograph's rotation and wraps the result into
/// (-180, 180]. This is what the wheel and the quarter-turn buttons use; a
/// frame rectangle is never touched either.
Frame rotateImageBy(Frame frame, double delta, const QSize &pixels);

/// Puts the photograph back to its neutral state inside the frame without
/// touching the frame's own fit: zoom 1, centred crop and no rotation.
Frame resetInsideFrame(Frame frame);

} // namespace pp::PhotoItem
