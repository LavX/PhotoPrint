#pragma once

#include "EffectCatalogue.h"
#include "Result.h"

#include <QImage>
#include <QList>
#include <QSize>

#include <algorithm>
#include <array>
#include <stop_token>
#include <vector>

namespace pp {

/// Interleaved float RGB working buffer, display-referred sRGB in 0..1. Alpha
/// never enters this buffer: the engine copies the source alpha back on export,
/// exactly as PhotoFilters::blend does.
struct EffectRgbBuffer {
    int width = 0;
    int height = 0;
    std::vector<float> pixels; ///< 3 floats per pixel, row-major

    void resize(int w, int h)
    {
        width = w;
        height = h;
        pixels.assign(static_cast<size_t>(std::max(0, w)) * std::max(0, h) * 3, 0.0f);
    }
    float *at(int x, int y) { return pixels.data() + (static_cast<size_t>(y) * width + x) * 3; }
    const float *at(int x, int y) const
    {
        return pixels.data() + (static_cast<size_t>(y) * width + x) * 3;
    }
};

/// One compiled segment. A point step is one 1D LUT pass plus its fused Phase B
/// stages; a spatial step ends the segment and runs after it.
struct EffectPlanStep {
    bool point = true;
    bool hasCurve = false;
    std::array<std::array<float, 1024>, 3> curve{};
    QList<Stage> toneStages;   ///< Phase A stages folded into this step, in order
    QList<Stage> colourStages; ///< Phase B stages fused into this step, in order
    Stage spatial;
};

using EffectCurveLut = std::array<std::array<float, 1024>, 3>;

struct EffectPlan {
    QList<EffectPlanStep> steps;
    int pointSteps = 0;
    int spatialSteps = 0;
    bool hasFinishStages = false;
};

/// Optional diagnostics for tests and the performance notes. `finishSize` is
/// the raster the finish stages actually ran at, not the requested target.
struct EffectRenderStats {
    QSize gradeSize;
    QSize finishSize;
    bool finishStages = false;
};

/// Staged grading engine. One shared instance owns the LUT, cube and texture
/// caches; the caches are guarded by a mutex, the stages themselves are pure
/// functions over EffectRgbBuffer. There is no OpenMP: measured per-stage
/// parallel-for is slower than serial at preview sizes.
class EffectEngine
{
public:
    /// Printed grade raster bound and the finish raster memory guard.
    static constexpr int kGradeMax = 2400;
    static constexpr int kFinishMax = 8000;
    /// Final pixel guard, the same spirit as PhotoFilters::kMaxOutputPixels.
    /// 80 MP covers 8000x6000 and a 9921x7441 4:3 frame. A full-bleed A4 at
    /// 1200 ppi is 9921x14031 (139 MP) and is refused with `effect-size`;
    /// lower the print resolution. The guard is checked before any allocation.
    static constexpr qint64 kMaxOutputPixels = 80'000'000;

    static EffectEngine &instance();

    /// Compiles a recipe into alternating point and spatial segments. The
    /// dithered posterize is spatial by construction, because it must see the
    /// final raster. A lut3d stage is a separate point pass with its own cube.
    static Result<EffectPlan> compile(const EffectRecipe &recipe);

    /// Grade long side: the target capped at kGradeMax. Never zero.
    static int gradeLongSide(int targetLongSide);
    /// Size with the same aspect whose long side is @p longSide.
    static QSize sizeForLongSide(const QSize &size, int longSide);

    /// Renders @p srgb. For the preview path @p targetSize should equal the
    /// source size; for print it is the placed raster. Strength 0 returns the
    /// input unchanged, 100 is the full recipe, and the blend is sRGB-linear
    /// like the legacy filters.
    Result<QImage> render(const QImage &srgb, const EffectRecipe &recipe, int strengthPercent,
                          const QSize &targetSize, bool printPath,
                          std::stop_token token = {}, EffectRenderStats *stats = nullptr) const;

    /// A recipe's working space scale factor for spatial parameters: defined
    /// at a 2400 px reference long side, clamped to 0.02..4. The low floor
    /// keeps a 160 px preview's bloom the same relative size as the print.
    static double referenceScale(int longSide);

private:
    EffectEngine() = default;
    EffectEngine(const EffectEngine &) = delete;
    EffectEngine &operator=(const EffectEngine &) = delete;
};

/// Texture resolver. A missing asset is a hard error with a logged diagnostic,
/// never a silent identity. The five shipped tiles live in `:/textures`.
Result<QImage> effectTexture(const QString &asset);

/// Loads a `.cube` artifact from `:/luts/<name>.cube`. Exposed for tests; the
/// shipped recipes do not use external cubes yet.
Result<QVector<float>> effectCube(const QString &name, int *size);

namespace effectstages {

void bakeCurveLut(const QList<Stage> &toneStages, EffectCurveLut &lut);
void applyPointStep(EffectRgbBuffer &buffer, const EffectPlanStep &step);
void applySpatialStage(EffectRgbBuffer &buffer, const Stage &stage, int longSide);
/// Finish stages run on the 8-bit target raster, after the one upscale, so no
/// full-resolution float buffer is ever allocated.
void applyFinishStep(QImage &image, const Stage &stage, int longSide);
/// Trilinear cube sampling; @p cube holds size^3 * 3 floats with red fastest.
void applyCube(EffectRgbBuffer &buffer, const QVector<float> &cube, int size);

} // namespace effectstages

} // namespace pp
