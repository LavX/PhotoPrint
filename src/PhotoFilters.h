#pragma once

#include <QImage>

namespace pp {

/// Everyday photo looks applied to an already sRGB 8-bit frame.
enum class FilterPreset {
    Original,   ///< No pixels touched.
    Vivid,      ///< Slightly brighter, clearly more saturated, a touch more contrast.
    Warm,       ///< Warmer colour temperature: more red, less blue.
    Cool,       ///< Cooler colour temperature: less red, more blue.
    Sepia,      ///< Classic brown monochrome tint.
    BlackWhite, ///< Neutral monochrome with gentle contrast.
    Vintage,    ///< Sepia, lifted blacks, softened contrast and a soft vignette.
    Crisp,      ///< Local contrast through a light unsharp mask.
};

/// The preset a persisted 1.0.x project key names.
FilterPreset filterFromKey(const QString &key);

/// Presets and enlargement for on-screen and in-page previews. Everything runs
/// through MagickWand on 8-bit sRGB frames; nothing is written to disk and no
/// external process is started.
class PhotoFilters
{
public:
    /// Hard ceiling for generated pixels. Larger requests return a null image
    /// instead of allocating gigabytes for a frame nobody can display.
    static constexpr qint64 kMaxOutputPixels = 80'000'000;

    /// Applies @p preset to an sRGB frame (Format_RGB32 or Format_ARGB32).
    /// @p strengthPercent is 0..100: 0 returns @p srgb unchanged and
    /// pixel-identical, 100 is the full look, values in between blend linearly
    /// in sRGB. Alpha always comes from the input; only RGB is filtered.
    /// Returns @p srgb when the image is empty, the preset is Original, the
    /// frame exceeds kMaxOutputPixels, or MagickWand cannot process it.
    static QImage apply(const QImage &srgb, FilterPreset preset, int strengthPercent);

    /// Lanczos enlargement by @p factor (1 is the identity, 2 doubles the
    /// dimensions) followed by a light unsharp mask. Alpha is resized with the
    /// colour so translucent edges stay smooth. Returns a null image when the
    /// factor is outside 1..8, the input is empty, or the result would exceed
    /// kMaxOutputPixels.
    static QImage upscale(const QImage &srgb, int factor);
};

} // namespace pp
