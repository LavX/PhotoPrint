#pragma once

#include "Document.h"

#include <QHash>
#include <QList>
#include <QSize>
#include <QTransform>

namespace pp {

/// Geometry only. Converts between the stored millimetre model and the
/// pixel rectangles used for drawing and interaction. No pixels are touched.
class LayoutEngine
{
public:
    /// Scale in mm per source pixel for a frame, before the user zoom.
    static double baseScale(const Frame &frame, const QSize &orientedPixels);

    /// Bounding size in mm of the photograph as drawn inside the frame,
    /// including the user zoom and the in-frame rotation.
    static QSizeF drawnSizeMm(const Frame &frame, const QSize &orientedPixels);

    /// Size of the drawn photograph along its own axes, before the in-frame
    /// rotation. Used by crop bounds and zoom-at-cursor maths.
    static QSizeF imageSizeMm(const Frame &frame, const QSize &orientedPixels);

    /// source pixel coordinates -> page millimetres.
    static QTransform imageToPage(const Frame &frame, const QSize &orientedPixels);

    /// Inverse of imageToPage; useful for crop dragging.
    static QTransform pageToImage(const Frame &frame, const QSize &orientedPixels);

    /// Effective resolution of the visible crop in the final printed size.
    static double effectivePpi(const Frame &frame, const QSize &orientedPixels);

    enum class ResolutionHint { Good, Soft, Low };
    /// Everyday-language hint thresholds: below 150 ppi is low, 150-240 soft.
    static ResolutionHint resolutionHint(const Frame &frame, const QSize &orientedPixels);

    /// True when the frame geometry is usable (positive, finite size).
    static bool validFrame(const Frame &frame);

    /// First-use grid for a page: two columns up to four photographs, three
    /// above that. Only called while every frame is still auto-placed.
    static QList<Frame> arrangeGrid(const Page &page, const QList<Frame> &frames,
                                    const QHash<QString, QSize> &pixelSizes);

    /// Appends a new photograph. The page is re-gridded only while every
    /// existing frame is still auto-placed, so hand work is never disturbed.
    static QList<Frame> appendPhoto(const Page &page, const QList<Frame> &frames,
                                    const Frame &newFrame,
                                    const QHash<QString, QSize> &pixelSizes);
};

} // namespace pp
