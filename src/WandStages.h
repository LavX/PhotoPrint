#pragma once

#include "EffectCatalogue.h"
#include "Result.h"

#include <QImage>
#include <QStringList>

namespace pp {

/// The Phase E MagickWand passes. Only the passes that are genuinely geometric
/// or texture-synthetic live here; every colour, tone, grain and dither decision
/// stays in the in-process stages, because the wand has no seed and no
/// determinism contract for noise.
enum class WandPass {
    Swirl,
    Implode,
    Wave,
    RotationalBlur,
    MotionBlur,
    OilPaint,
    Sketch,
    Charcoal,
    Edge,
    Emboss,
    Kuwahara,
    Halftone,
    Mirror,
    Kaleido,
    Stretch,
    Pixelate
};

QString wandPassName(WandPass pass);
bool wandPassFromName(const QString &name, WandPass *pass);
QStringList wandPassNames();

/// The result of probing this install for a pass. A pass whose symbols are
/// absent is not a silent identity: compile() refuses the recipe with the
/// reason, so a broken library cannot ship a look that quietly does nothing.
struct WandCapability {
    bool available = false;
    QString reason; ///< empty when available, otherwise a sentence for the user
};

/// Probes, once per process: are the symbols this pass needs exported by the
/// loaded library? ImageMagick 7 declares `MagickReduceNoiseImage` and
/// `MagickModeImage` in its header but does not export them, so a header-only
/// assumption is a link error; this table is checked against the binary.
WandCapability wandCapability(WandPass pass);

/// True when `MagickOrderedDitherImage` and the shipped threshold maps both
/// work. When this is false the halftone pass uses the in-process 8x8 screen
/// instead, so no recipe depends on `/etc/ImageMagick-7/thresholds.xml`.
bool wandThresholdMapsAvailable();

/// The symbol names a pass needs. Exposed for diagnostics and tests.
QStringList wandRequiredSymbols(WandPass pass);

/// Applies @p stage (a StageId::Wand stage) to @p image in place at the given
/// long side. Every spatial parameter is authored at the 2400 px reference and
/// scaled by longSide / 2400. Alpha is never touched: the frame is handed to
/// MagickWand as RGB and only RGB is read back, because the geometric passes
/// smear the alpha plane and produce halos on a transparent frame.
Result<void> applyWandPass(QImage &image, const Stage &stage, int longSide);

/// The in-process 8x8 clustered-dot screen. The threshold matrix is stretched
/// over one screen cell of @p period pixels, so the dot pitch is the authored
/// pitch and follows the raster exactly. Used for the halftone pass whenever
/// ImageMagick's threshold maps cannot express the requested pitch, and always
/// when those maps are missing: the catalogue does not depend on
/// `/etc/ImageMagick-7/thresholds.xml`. Deterministic, allocation free beyond
/// the image itself.
void orderedDither8x8(QImage &image, double period);

} // namespace pp
