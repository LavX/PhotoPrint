#pragma once

#include "ImageSource.h"
#include "Result.h"

#include <QImage>

#include <stop_token>

namespace pp {

/// Converts source pixels through LittleCMS2 only. Qt decodes the file but
/// never converts its colour values. The working space is linear Rec.2020/D65.
class ColorPipeline
{
public:
    /// Linear Rec.2020, premultiplied 16-bit RGBA. Alpha is retained so several
    /// layers can be composited before the page is flattened onto white.
    static QImage toLinear(const SourceImage &source, const ColorRecipe &recipe,
                           int maxDimension = 0, std::stop_token token = {});

    /// Flattens a linear premultiplied layer set onto white paper and converts
    /// the finished page to sRGB 8-bit for screen or PDF. With `grayscale` the
    /// linear luminance is written into all three channels before the sRGB
    /// conversion, so preview and export are neutral by construction. Rows are
    /// converted in parallel bands; the output is bit identical to a serial run.
    static QImage linearToSrgb(const QImage &linearPremultiplied, bool grayscale = false,
                               std::stop_token token = {});

    /// Same conversion, split into exactly `bands` row strips. `bands <= 1`
    /// converts on the calling thread, 0 picks one band per hardware thread
    /// (capped at 16). Every band creates its own lcms transform because lcms
    /// transforms are stateful and are not reentrant. Exists so tests can
    /// compare the serial and parallel paths; production code should call
    /// linearToSrgb().
    static QImage linearToSrgbBanded(const QImage &linearPremultiplied, bool grayscale, int bands,
                                     std::stop_token token = {});

    /// Linear premultiplied -> sRGB with alpha preserved (no white flatten).
    static QImage linearToSrgbWithAlpha(const QImage &linearPremultiplied,
                                        std::stop_token token = {});

    /// sRGB with alpha -> linear premultiplied, used by filters that work in sRGB.
    static QImage srgbToLinear(const QImage &srgbArgb, std::stop_token token = {});

    /// Convenience for single-image previews: toLinear followed by linearToSrgb.
    static QImage toSrgb(const SourceImage &source, const ColorRecipe &recipe, int maxDimension = 0,
                         std::stop_token token = {});

    /// Neutral tone parameters produce the identity result.
    static bool recipeIsIdentity(const ColorRecipe &recipe);

    /// sRGB ICC profile bytes used as the PDF output intent and image colour space.
    static QByteArray srgbProfileBytes();
};

} // namespace pp
