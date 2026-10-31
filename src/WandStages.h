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

/// The symbol names a pass needs. Exposed for diagnostics and tests.
QStringList wandRequiredSymbols(WandPass pass);

/// Applies @p stage (a StageId::Wand stage) to @p image in place at the given
/// long side. Every spatial parameter is authored at the 2400 px reference and
/// scaled by longSide / 2400. Alpha is never touched: the frame is handed to
/// MagickWand as RGB and only RGB is read back, because the geometric passes
/// smear the alpha plane and produce halos on a transparent frame.
Result<void> applyWandPass(QImage &image, const Stage &stage, int longSide);

/// The clustered-dot screen the halftone pass uses, at a cell of @p period whole
/// pixels. Thresholds grow from the centre of the cell and are spaced evenly in
/// that order, so the dot pitch is the authored pitch, the coverage of a flat
/// patch is the same at every pitch, and the screen is exactly periodic. This is
/// the only screen: no recipe depends on `/etc/ImageMagick-7/thresholds.xml`, and
/// the screen cannot change shape when the print size moves by a percent.
/// Deterministic and allocation free beyond one per-pitch threshold table.
void orderedDither8x8(QImage &image, int period);

} // namespace pp
