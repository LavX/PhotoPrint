#pragma once

#include "Result.h"

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QList>
#include <QMap>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <array>

namespace pp {

/// The twenty-six stages of the grading engine. The id is the JSON name and the
/// only thing a recipe may reference; the phase decides how a plan is compiled.
enum class StageId {
    // Phase A, point tone.
    WhiteBalance,
    Exposure,
    Gamma,
    Contrast,
    Curves,
    Levels,
    Fade,
    Posterize,
    Invert,
    // Phase B, point colour.
    ColorMatrix,
    Saturation,
    Vibrance,
    HueShift,
    SplitTone,
    GradientMap,
    Lut3d,
    // Phase C, spatial detail.
    Blur,
    Unsharp,
    LocalContrast,
    Bloom,
    SoftFocus,
    Grain,
    // Phase D, light and material.
    Vignette,
    LightLeak,
    ExposureMask,
    Texture,
    // Phase E, MagickWand. One id, the pass is named by `wandPass`, because the
    // sixteen passes share a compiler treatment and differ only in parameters.
    Wand
};

enum class StagePhase {
    PointTone,   ///< Phase A, folds into the 1D curve LUT.
    PointColour, ///< Phase B, fused into one pass per segment.
    Spatial,     ///< Phase C, ends a segment.
    Light,       ///< Phase D, ends a segment.
    Wand         ///< Phase E, a MagickWand pass.
};

/// How a stage is allowed to end a segment. Dithered posterize is deliberately
/// a spatial stage: the dither has to see the final raster, so it cannot fold.
StagePhase stagePhase(StageId id);
QString stageName(StageId id);
bool stageFromName(const QString &name, StageId *id);

enum class MaskKind { None, Linear, Radial };

/// A spatial mask, used by blur and (later) local contrast. Coordinates are
/// normalised to the frame, so nothing here depends on the raster size.
struct StageMask {
    MaskKind kind = MaskKind::None;
    QPointF centre{0.5, 0.5};
    double start = 0.0;
    double end = 1.0;
    double feather = 0.15;
};

enum class BlendMode { Normal, Screen, Multiply, SoftLight, Overlay, Add };

struct GradientStop {
    double position = 0.0;
    QColor colour = QColor(0, 0, 0);
};

/// One entry of a recipe. A single struct carries every parameter of every
/// stage: the loader validates the fields that belong to the stage id and
/// rejects the rest, so a typo cannot silently become a default. Numeric values
/// are clamped to their documented range rather than trusted.
struct Stage {
    QString idName; ///< the JSON id, kept for diagnostics and digests
    StageId id = StageId::Exposure;
    bool finish = false; ///< runs on the finish raster in the print path

    // Phase A.
    double temperature = 0.0; ///< -100..100
    double tint = 0.0;        ///< -100..100
    double ev = 0.0;          ///< -3..3
    double gamma = 1.0;       ///< 0.2..5
    double amount = 0.0;      ///< contrast/vibrance/saturation/unsharp etc.
    double pivot = 0.45;      ///< 0..1
    QList<QPointF> curveMaster;
    QList<QPointF> curveR;
    QList<QPointF> curveG;
    QList<QPointF> curveB;
    double black = 0.0; ///< levels 0..1
    double white = 1.0; ///< levels 0..1
    double lift = 0.0;  ///< fade 0..0.3
    double toe = 0.6;   ///< fade 0..1
    double roll = 0.0;  ///< fade 0..0.3
    int levels = 2;     ///< posterize 2..32
    bool dither = false;

    // Phase B.
    std::array<double, 9> matrix{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    QColor shadow = QColor(128, 128, 128);
    double shadowStrength = 0.0;
    QColor highlight = QColor(128, 128, 128);
    double highlightStrength = 0.0;
    double balance = 0.5;
    QList<GradientStop> stops;
    double mix = 1.0; ///< gradient map: 0 keeps the source, 1 replaces it
    QString lut;      ///< lut3d: cube name

    // Phase C.
    double radius = 0.0; ///< pixels at the 2400 px reference
    int passes = 3;
    StageMask mask;
    double threshold = 0.0;
    double intensity = 0.0;
    QColor tintColour = QColor(255, 255, 255);
    double size = 1.0; ///< grain size in pixels at the reference
    quint32 seed = 0;
    bool mono = true;

    // Phase D.
    double midpoint = 0.5;
    double feather = 0.4;
    double roundness = 0.0;
    QPointF centre{0.5, 0.5};
    double softness = 0.5;
    BlendMode blend = BlendMode::Normal;
    QString asset;
    double opacity = 0.0;
    double scale = 1.0;
    double maskSize = 0.5;

    // Phase E: the MagickWand pass and its parameters. Every spatial value is
    // authored at the 2400 px reference and scaled by the render raster.
    QString wandPass; ///< swirl, implode, wave, oilPaint, halftone, ...
    double wandDegrees = 0.0;
    double wandAmount = 0.0;
    double wandAmplitude = 0.0;
    double wandLength = 0.0;
    double wandAngle = 0.0;
    double wandRadius = 0.0;
    double wandSigma = 0.0;
    double wandFactor = 1.0;
    double wandSize = 0.0; ///< halftone screen period / pixelate block
    int wandSegments = 6;  ///< kaleidoscope wedges
    QString wandMap;       ///< halftone threshold map, empty for the built-in screen
};

/// One versioned look. `version` is part of the identity: a retune adds a new
/// version and the old one stays resolvable, so an old project keeps its look.
struct EffectRecipe {
    QString key;
    QString category;
    int version = 1;
    QString name;        ///< the single English product name
    QString description; ///< optional one-line tooltip text
    QList<Stage> stages;

    /// `key@version`, the stable identity used by caches and tests.
    QString identity() const
    {
        return QStringLiteral("%1@%2").arg(key).arg(version);
    }
};

/// The shipped catalogue: twelve categories and the recipes that resolve from
/// `resources/effects/catalogue.json`.
class EffectCatalogue
{
public:
    /// Loads and validates the embedded catalogue. A malformed catalogue is a
    /// bug in the shipped data, so instance() reports it loudly and returns an
    /// empty catalogue rather than guessing.
    static const EffectCatalogue &instance();

    /// Strict parser used by the tests and by instance(). Unknown ids, unknown
    /// fields and out-of-range values are errors, not silent defaults.
    static Result<EffectCatalogue> fromJson(const QByteArray &json);

    /// Resolves `key` or `key@version`. A version of 0 (the project default)
    /// picks the newest version; an exact version is honoured even when a newer
    /// one exists.
    Result<EffectRecipe> resolve(const QString &keyWithVersion, int version = 0) const;

    /// The newest version of every recipe, ordered as in the catalogue.
    QList<EffectRecipe> recipes() const { return m_current; }
    QStringList categoryKeys() const { return m_categoryKeys; }
    QString categoryName(const QString &key) const { return m_categoryNames.value(key); }
    bool isEmpty() const { return m_current.isEmpty(); }

private:
    QList<EffectRecipe> m_current;
    QHash<QString, QMap<int, EffectRecipe>> m_versions;
    QStringList m_categoryKeys;
    QHash<QString, QString> m_categoryNames;
};

} // namespace pp
