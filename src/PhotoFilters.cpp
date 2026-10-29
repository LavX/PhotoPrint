#include "PhotoFilters.h"

#include <MagickWand/MagickWand.h>

#include <QColorSpace>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace pp {

QString filterKey(FilterPreset preset)
{
    switch (preset) {
    case FilterPreset::Vivid:
        return QStringLiteral("vivid");
    case FilterPreset::Warm:
        return QStringLiteral("warm");
    case FilterPreset::Cool:
        return QStringLiteral("cool");
    case FilterPreset::Sepia:
        return QStringLiteral("sepia");
    case FilterPreset::BlackWhite:
        return QStringLiteral("blackwhite");
    case FilterPreset::Vintage:
        return QStringLiteral("vintage");
    case FilterPreset::Crisp:
        return QStringLiteral("crisp");
    case FilterPreset::Original:
        break;
    }
    return QStringLiteral("original");
}

FilterPreset filterFromKey(const QString &key)
{
    if (key == QLatin1String("vivid"))
        return FilterPreset::Vivid;
    if (key == QLatin1String("warm"))
        return FilterPreset::Warm;
    if (key == QLatin1String("cool"))
        return FilterPreset::Cool;
    if (key == QLatin1String("sepia"))
        return FilterPreset::Sepia;
    if (key == QLatin1String("blackwhite"))
        return FilterPreset::BlackWhite;
    if (key == QLatin1String("vintage"))
        return FilterPreset::Vintage;
    if (key == QLatin1String("crisp"))
        return FilterPreset::Crisp;
    return FilterPreset::Original;
}

QString filterLabel(FilterPreset preset)
{
    switch (preset) {
    case FilterPreset::Vivid:
        return QStringLiteral("Élénk");
    case FilterPreset::Warm:
        return QStringLiteral("Meleg");
    case FilterPreset::Cool:
        return QStringLiteral("Hűvös");
    case FilterPreset::Sepia:
        return QStringLiteral("Szépia");
    case FilterPreset::BlackWhite:
        return QStringLiteral("Fekete-fehér");
    case FilterPreset::Vintage:
        return QStringLiteral("Vintage");
    case FilterPreset::Crisp:
        return QStringLiteral("Éles");
    case FilterPreset::Original:
        break;
    }
    return QStringLiteral("Eredeti");
}

namespace {

void ensureMagick()
{
    static std::once_flag flag;
    std::call_once(flag, [] { MagickWandGenesis(); });
}

struct WandDeleter {
    void operator()(MagickWand *wand) const
    {
        if (wand)
            DestroyMagickWand(wand);
    }
};

using WandPtr = std::unique_ptr<MagickWand, WandDeleter>;

/// Copies the frame into an RGBA byte buffer and hands it to MagickWand as an
/// sRGB image with an explicit alpha channel.
WandPtr importFrame(const QImage &image)
{
    ensureMagick();
    WandPtr wand(NewMagickWand());
    if (!wand)
        return {};

    const int width = image.width();
    const int height = image.height();
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        unsigned char *out = pixels.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x, out += 4) {
            const QRgb pixel = line[x];
            out[0] = static_cast<unsigned char>(qRed(pixel));
            out[1] = static_cast<unsigned char>(qGreen(pixel));
            out[2] = static_cast<unsigned char>(qBlue(pixel));
            out[3] = static_cast<unsigned char>(qAlpha(pixel));
        }
    }

    if (MagickConstituteImage(wand.get(), static_cast<size_t>(width), static_cast<size_t>(height),
                              "RGBA", CharPixel, pixels.data()) == MagickFalse)
        return {};
    if (MagickSetImageColorspace(wand.get(), sRGBColorspace) == MagickFalse)
        return {};
    if (MagickSetImageAlphaChannel(wand.get(), SetAlphaChannel) == MagickFalse)
        return {};
    return wand;
}

/// Reads the frame back as 8-bit RGBA. When @p alphaSource is given its alpha
/// bytes win, so the filter never changes transparency.
QImage exportFrame(MagickWand *wand, const QImage *alphaSource)
{
    const int width = static_cast<int>(MagickGetImageWidth(wand));
    const int height = static_cast<int>(MagickGetImageHeight(wand));
    if (width <= 0 || height <= 0)
        return {};
    if (static_cast<qint64>(width) * height > PhotoFilters::kMaxOutputPixels)
        return {};

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    if (MagickExportImagePixels(wand, 0, 0, static_cast<size_t>(width),
                                static_cast<size_t>(height), "RGBA", CharPixel,
                                pixels.data()) == MagickFalse)
        return {};

    QImage out(width, height, QImage::Format_ARGB32);
    if (out.isNull())
        return {};
    for (int y = 0; y < height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        const unsigned char *in = pixels.data() + static_cast<size_t>(y) * width * 4;
        if (alphaSource) {
            const QRgb *sourceLine =
                reinterpret_cast<const QRgb *>(alphaSource->constScanLine(y));
            for (int x = 0; x < width; ++x, in += 4)
                line[x] = qRgba(in[0], in[1], in[2], qAlpha(sourceLine[x]));
        } else {
            for (int x = 0; x < width; ++x, in += 4)
                line[x] = qRgba(in[0], in[1], in[2], in[3]);
        }
    }
    return out;
}

/// Linear blend in sRGB between the untouched frame and the full effect.
QImage blend(const QImage &original, const QImage &filtered, int strengthPercent)
{
    const int strength = std::clamp(strengthPercent, 0, 100);
    if (strength == 100)
        return filtered;

    QImage out(original.size(), QImage::Format_ARGB32);
    if (out.isNull())
        return {};
    const int width = original.width();
    for (int y = 0; y < original.height(); ++y) {
        const QRgb *base = reinterpret_cast<const QRgb *>(original.constScanLine(y));
        const QRgb *effect = reinterpret_cast<const QRgb *>(filtered.constScanLine(y));
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb a = base[x];
            const QRgb b = effect[x];
            const auto mix = [strength](int from, int to) {
                return (from * (100 - strength) + to * strength + 50) / 100;
            };
            line[x] = qRgba(mix(qRed(a), qRed(b)), mix(qGreen(a), qGreen(b)),
                            mix(qBlue(a), qBlue(b)), qAlpha(a));
        }
    }
    return out;
}

/// Per-channel gain. The wand channel mask limits the following evaluate to
/// one channel, which is how the colour temperature presets shift red against
/// blue without touching green.
bool scaleChannel(MagickWand *wand, ChannelType channel, double factor)
{
    MagickSetImageChannelMask(wand, channel);
    const bool ok = MagickEvaluateImage(wand, MultiplyEvaluateOperator, factor) != MagickFalse;
    MagickSetImageChannelMask(wand, DefaultChannels);
    return ok;
}

bool sepiaTone(MagickWand *wand, double percent)
{
    return MagickSepiaToneImage(wand, percent / 100.0 * static_cast<double>(QuantumRange))
           != MagickFalse;
}

/// VignetteImage paints the area outside the vignette with the background
/// colour, so the background has to be black for a darkening falloff.
bool darkenCorners(MagickWand *wand)
{
    PixelWand *black = NewPixelWand();
    if (!black)
        return false;
    PixelSetColor(black, "black");
    const bool ok = MagickSetImageBackgroundColor(wand, black) != MagickFalse
                    && MagickVignetteImage(wand, 0.0, 40.0, 0.0, 0.0) != MagickFalse;
    DestroyPixelWand(black);
    return ok;
}

bool applyPreset(MagickWand *wand, FilterPreset preset)
{
    switch (preset) {
    case FilterPreset::Original:
        return true;
    case FilterPreset::Vivid:
        return MagickModulateImage(wand, 103.0, 128.0, 100.0) != MagickFalse
               && MagickContrastImage(wand, MagickTrue) != MagickFalse;
    case FilterPreset::Warm: {
        const bool red = scaleChannel(wand, RedChannel, 1.07);
        const bool green = scaleChannel(wand, GreenChannel, 1.01);
        const bool blue = scaleChannel(wand, BlueChannel, 0.94);
        return red && green && blue;
    }
    case FilterPreset::Cool: {
        const bool red = scaleChannel(wand, RedChannel, 0.94);
        const bool green = scaleChannel(wand, GreenChannel, 1.01);
        const bool blue = scaleChannel(wand, BlueChannel, 1.07);
        return red && green && blue;
    }
    case FilterPreset::Sepia:
        return sepiaTone(wand, 80.0)
               && MagickContrastImage(wand, MagickTrue) != MagickFalse;
    case FilterPreset::BlackWhite:
        return MagickModulateImage(wand, 100.0, 0.0, 100.0) != MagickFalse
               && MagickContrastImage(wand, MagickTrue) != MagickFalse;
    case FilterPreset::Vintage:
        return sepiaTone(wand, 70.0) && MagickModulateImage(wand, 102.0, 85.0, 100.0) != MagickFalse
               && MagickSigmoidalContrastImage(wand, MagickFalse, 4.0,
                                               0.5 * static_cast<double>(QuantumRange))
                      != MagickFalse
               && darkenCorners(wand);
    case FilterPreset::Crisp:
        return MagickUnsharpMaskImage(wand, 0.0, 0.8, 0.7, 0.02) != MagickFalse;
    }
    return false;
}

QImage filtered(const QImage &source, FilterPreset preset)
{
    WandPtr wand = importFrame(source);
    if (!wand)
        return {};
    if (!applyPreset(wand.get(), preset))
        return {};
    return exportFrame(wand.get(), &source);
}

} // namespace

QImage PhotoFilters::apply(const QImage &srgb, FilterPreset preset, int strengthPercent)
{
    if (srgb.isNull() || srgb.width() <= 0 || srgb.height() <= 0)
        return srgb;
    if (preset == FilterPreset::Original || strengthPercent <= 0)
        return srgb;
    if (static_cast<qint64>(srgb.width()) * srgb.height() > kMaxOutputPixels)
        return srgb;

    const QImage::Format sourceFormat = srgb.format();
    const QImage working =
        (sourceFormat == QImage::Format_RGB32 || sourceFormat == QImage::Format_ARGB32)
            ? srgb
            : srgb.convertToFormat(QImage::Format_ARGB32);
    if (working.isNull())
        return srgb;

    const QImage effect = filtered(working, preset);
    if (effect.isNull())
        return srgb;

    QImage result = blend(working, effect, std::min(strengthPercent, 100));
    if (result.isNull())
        return srgb;
    result.setColorSpace(working.colorSpace());

    if (sourceFormat == QImage::Format_RGB32)
        return result.convertToFormat(QImage::Format_RGB32);
    return result;
}

QImage PhotoFilters::upscale(const QImage &srgb, int factor)
{
    if (srgb.isNull() || srgb.width() <= 0 || srgb.height() <= 0)
        return {};
    if (factor == 1)
        return srgb;
    if (factor < 1 || factor > 8)
        return {};

    const qint64 targetWidth = static_cast<qint64>(srgb.width()) * factor;
    const qint64 targetHeight = static_cast<qint64>(srgb.height()) * factor;
    if (targetWidth * targetHeight > kMaxOutputPixels)
        return {};

    const QImage::Format sourceFormat = srgb.format();
    const QImage working =
        (sourceFormat == QImage::Format_RGB32 || sourceFormat == QImage::Format_ARGB32)
            ? srgb
            : srgb.convertToFormat(QImage::Format_ARGB32);
    if (working.isNull())
        return {};

    WandPtr wand = importFrame(working);
    if (!wand)
        return {};
    if (MagickResizeImage(wand.get(), static_cast<size_t>(targetWidth),
                          static_cast<size_t>(targetHeight), LanczosFilter)
        == MagickFalse)
        return {};
    if (MagickUnsharpMaskImage(wand.get(), 0.0, 0.7, 0.5, 0.0) == MagickFalse)
        return {};

    QImage result = exportFrame(wand.get(), nullptr);
    if (result.isNull())
        return {};
    result.setColorSpace(working.colorSpace());
    if (sourceFormat == QImage::Format_RGB32)
        return result.convertToFormat(QImage::Format_RGB32);
    return result;
}

} // namespace pp
