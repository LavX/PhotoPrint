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

} // namespace pp::PhotoItem
