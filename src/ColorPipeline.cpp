#include "ColorPipeline.h"

#include <QColorSpace>
#include <QRgba64>
#include <QThread>

#include <lcms2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <system_error>
#include <thread>
#include <vector>

namespace pp {

namespace {

struct WorkingSpace {
    cmsHPROFILE linearRec2020 = nullptr;
    cmsHPROFILE srgb = nullptr;
    cmsToneCurve *linearCurve = nullptr;

    WorkingSpace()
    {
        const cmsCIExyY whitePoint = {0.3127, 0.3290, 1.0}; // D65
        const cmsCIExyYTRIPLE primaries = {
            {0.708, 0.292, 1.0}, // Rec.2020 red
            {0.170, 0.797, 1.0}, // Rec.2020 green
            {0.131, 0.046, 1.0}, // Rec.2020 blue
        };
        linearCurve = cmsBuildGamma(nullptr, 1.0);
        cmsToneCurve *curves[3] = {linearCurve, linearCurve, linearCurve};
        linearRec2020 = cmsCreateRGBProfile(&whitePoint, &primaries, curves);
        srgb = cmsCreate_sRGBProfile();
    }
};

WorkingSpace &workingSpace()
{
    static WorkingSpace space;
    return space;
}

double clamp01(double value)
{
    if (!std::isfinite(value))
        return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

void applyTone(double *rgb, const ColorRecipe &recipe)
{
    if (ColorPipeline::recipeIsIdentity(recipe))
        return;
    const double exposure = std::pow(2.0, recipe.tone.exposureEv);
    const double shadow = std::clamp(recipe.tone.shadowLift, 0.0, 1.0);
    const double highlight = std::clamp(recipe.tone.highlightCompress, 0.0, 0.35);
    for (int i = 0; i < 3; ++i) {
        double value = clamp01(rgb[i] * exposure);
        value += shadow * value * (1.0 - value) * (1.0 - value);
        value -= highlight * std::pow(value, 4.0);
        rgb[i] = clamp01(value);
    }
}

quint16 to16(double value)
{
    return static_cast<quint16>(std::lround(clamp01(value) * 65535.0));
}

/// One thread's worth of rows to convert. The pointers come from the source and
/// target QImage once, before any band runs, so no thread touches the QImage
/// headers.
struct RowBand {
    const uchar *source = nullptr;
    qsizetype sourceStride = 0;
    uchar *target = nullptr;
    qsizetype targetStride = 0;
};

constexpr int kMaxBands = 16;
constexpr int kMinRowsPerBand = 64;
// Banding has fixed costs (thread start-up plus one lcms transform per band,
// about a millisecond each). Below this pixel count the calling thread finishes
// first, so small pages are converted serially.
constexpr qint64 kMinBandedPixels = 262144; // 512 x 512

int availableBands()
{
    // Leave one hardware thread for the GUI: a conversion that saturates every
    // core pushes input and paint latencies into the tens of milliseconds.
    const int hardware = QThread::idealThreadCount();
    return std::clamp(hardware - 1, 1, kMaxBands);
}

/// Converts rows [y0, y1) of the band. Each call owns its lcms transform: a
/// transform holds per-conversion state and must never be shared by threads.
/// The per-pixel maths is the serial path unchanged, so the result is bit
/// identical whatever the banding.
bool convertLinearRows(const RowBand &band, int width, int y0, int y1, bool grayscale,
                       std::stop_token token)
{
    WorkingSpace &space = workingSpace();
    cmsHTRANSFORM transform = cmsCreateTransform(space.linearRec2020, TYPE_RGB_16, space.srgb,
                                                 TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC,
                                                 cmsFLAGS_BLACKPOINTCOMPENSATION);
    if (!transform)
        return false;

    std::vector<quint16> straightened(static_cast<size_t>(width) * 3);
    std::vector<uchar> srgb(static_cast<size_t>(width) * 3);
    bool complete = true;
    for (int y = y0; y < y1; ++y) {
        if (token.stop_requested()) {
            complete = false;
            break;
        }
        const QRgba64 *src =
            reinterpret_cast<const QRgba64 *>(band.source + static_cast<qsizetype>(y) * band.sourceStride);
        for (int x = 0; x < width; ++x) {
            const quint16 a = src[x].alpha();
            // The page is flattened onto white paper: premultiplied colour plus
            // the remaining white share is the straight linear value.
            const double remaining = 1.0 - a / 65535.0;
            straightened[x * 3] = static_cast<quint16>(std::min<quint32>(
                65535, static_cast<quint32>(src[x].red()) + static_cast<quint32>(remaining * 65535.0)));
            straightened[x * 3 + 1] = static_cast<quint16>(std::min<quint32>(
                65535, static_cast<quint32>(src[x].green()) + static_cast<quint32>(remaining * 65535.0)));
            straightened[x * 3 + 2] = static_cast<quint16>(std::min<quint32>(
                65535, static_cast<quint32>(src[x].blue()) + static_cast<quint32>(remaining * 65535.0)));
            if (grayscale) {
                // Rec.709 luma on the linear values. Neutral input survives the
                // Rec.2020 -> sRGB transform unchanged, so the result is grey.
                const double luminance = (0.2126 * straightened[x * 3]
                                          + 0.7152 * straightened[x * 3 + 1]
                                          + 0.0722 * straightened[x * 3 + 2])
                    / 65535.0;
                const quint16 value = to16(luminance);
                straightened[x * 3] = value;
                straightened[x * 3 + 1] = value;
                straightened[x * 3 + 2] = value;
            }
        }
        cmsDoTransform(transform, straightened.data(), srgb.data(), width);
        QRgb *dst = reinterpret_cast<QRgb *>(band.target + static_cast<qsizetype>(y) * band.targetStride);
        for (int x = 0; x < width; ++x)
            dst[x] = qRgb(srgb[x * 3], srgb[x * 3 + 1], srgb[x * 3 + 2]);
    }

    cmsDeleteTransform(transform);
    return complete;
}

} // namespace

bool ColorPipeline::recipeIsIdentity(const ColorRecipe &recipe)
{
    return !recipe.autoEnhance && recipe.tone.exposureEv == 0.0 && recipe.tone.shadowLift == 0.0
        && recipe.tone.highlightCompress == 0.0 && recipe.paperStrength == 0.0;
}

QImage ColorPipeline::toLinear(const SourceImage &source, const ColorRecipe &recipe,
                               int maxDimension, std::stop_token token)
{
    QImage input = source.pixels;
    if (input.isNull())
        return {};

    if (maxDimension > 0 && std::max(input.width(), input.height()) > maxDimension)
        input = input.scaled(maxDimension, maxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    const int width = input.width();
    const int height = input.height();
    const bool hasAlpha = input.hasAlphaChannel();
    const QImage rgbInput = input.convertToFormat(QImage::Format_RGB888);
    const QImage alphaInput = hasAlpha ? input.convertToFormat(QImage::Format_ARGB32) : QImage();
    if (rgbInput.isNull())
        return {};

    QImage output(width, height, QImage::Format_RGBA64_Premultiplied);
    if (output.isNull())
        return {};

    WorkingSpace &space = workingSpace();
    const QColorSpace sourceSpace = source.profileValid && source.sourceSpace.isValid()
        ? source.sourceSpace
        : QColorSpace(QColorSpace::SRgb);
    const QByteArray icc = sourceSpace.iccProfile();
    cmsHPROFILE sourceProfile = nullptr;
    if (!icc.isEmpty())
        sourceProfile = cmsOpenProfileFromMem(icc.constData(), static_cast<cmsUInt32Number>(icc.size()));
    const bool openedSource = sourceProfile != nullptr;
    if (!sourceProfile)
        sourceProfile = space.srgb;

    cmsHTRANSFORM transform = cmsCreateTransform(sourceProfile, TYPE_RGB_8, space.linearRec2020,
                                                 TYPE_RGB_16, INTENT_RELATIVE_COLORIMETRIC,
                                                 cmsFLAGS_BLACKPOINTCOMPENSATION);
    auto cleanup = [&] {
        if (transform)
            cmsDeleteTransform(transform);
        if (openedSource)
            cmsCloseProfile(sourceProfile);
    };
    if (!transform) {
        cleanup();
        return {};
    }

    std::vector<quint16> linear(static_cast<size_t>(width) * 3);
    for (int y = 0; y < height; ++y) {
        if (token.stop_requested()) {
            cleanup();
            return {};
        }
        cmsDoTransform(transform, rgbInput.constScanLine(y), linear.data(), width);
        const QRgb *alphaLine =
            hasAlpha ? reinterpret_cast<const QRgb *>(alphaInput.constScanLine(y)) : nullptr;
        QRgba64 *dst = reinterpret_cast<QRgba64 *>(output.scanLine(y));
        for (int x = 0; x < width; ++x) {
            double rgb[3] = {linear[x * 3] / 65535.0, linear[x * 3 + 1] / 65535.0,
                             linear[x * 3 + 2] / 65535.0};
            applyTone(rgb, recipe);
            if (recipe.paperStrength != 0.0) {
                const double paperExposure = std::pow(2.0, recipe.paperStrength);
                for (int c = 0; c < 3; ++c)
                    rgb[c] = clamp01(rgb[c] * paperExposure);
            }
            const quint16 a = alphaLine ? static_cast<quint16>(qAlpha(alphaLine[x]) * 257) : 65535;
            const quint32 alpha = a;
            const quint16 red = static_cast<quint16>((static_cast<quint32>(to16(rgb[0])) * alpha + 32767) / 65535);
            const quint16 green = static_cast<quint16>((static_cast<quint32>(to16(rgb[1])) * alpha + 32767) / 65535);
            const quint16 blue = static_cast<quint16>((static_cast<quint32>(to16(rgb[2])) * alpha + 32767) / 65535);
            dst[x] = qRgba64(red, green, blue, a);
        }
    }

    cleanup();
    return output;
}

QImage ColorPipeline::linearToSrgb(const QImage &linearPremultiplied, bool grayscale,
                                   std::stop_token token)
{
    return linearToSrgbBanded(linearPremultiplied, grayscale, 0, token);
}

int ColorPipeline::bandsFor(const QSize &size)
{
    if (size.isEmpty()
        || static_cast<qint64>(size.width()) * size.height() < kMinBandedPixels)
        return 1;
    return std::clamp(availableBands(), 1, std::max(1, size.height() / kMinRowsPerBand));
}

QImage ColorPipeline::linearToSrgbBanded(const QImage &linearPremultiplied, bool grayscale, int bands,
                                         std::stop_token token)
{
    if (linearPremultiplied.isNull())
        return {};
    const QImage input = linearPremultiplied.format() == QImage::Format_RGBA64_Premultiplied
        ? linearPremultiplied
        : linearPremultiplied.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    if (input.isNull())
        return {};

    const int width = input.width();
    const int height = input.height();
    QImage output(width, height, QImage::Format_RGB32);
    if (output.isNull())
        return {};

    RowBand band;
    band.source = input.constBits();
    band.sourceStride = input.bytesPerLine();
    band.target = output.bits(); // detaches once, before any band starts
    band.targetStride = output.bytesPerLine();

    const int count = bands > 0 ? std::clamp(bands, 1, std::max(1, height / kMinRowsPerBand))
                                : bandsFor(QSize(width, height));
    if (count <= 1) {
        if (!convertLinearRows(band, width, 0, height, grayscale, token))
            return {};
        return output;
    }

    const int rows = (height + count - 1) / count;
    // Self-contained bands on plain threads: the number of threads is bounded by
    // the hardware and by the band count, and no band ever waits on another, so
    // the conversion cannot deadlock behind a busy thread pool.
    std::atomic<bool> failed{false};
    const auto runBand = [&](int y0, int y1) {
        if (!convertLinearRows(band, width, y0, y1, grayscale, token))
            failed.store(true, std::memory_order_relaxed);
    };
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(count - 1));
    for (int index = 0; index < count; ++index) {
        const int y0 = index * rows;
        const int y1 = std::min(height, y0 + rows);
        if (index + 1 == count) {
            runBand(y0, y1); // the calling thread converts the last band
            break;
        }
        try {
            workers.emplace_back([&runBand, y0, y1] { runBand(y0, y1); });
        } catch (const std::system_error &) {
            // The machine cannot give us another thread: convert here instead.
            runBand(y0, y1);
        }
    }
    for (std::thread &worker : workers)
        worker.join();
    if (failed.load())
        return {};
    return output;
}

QImage ColorPipeline::linearToSrgbWithAlpha(const QImage &linearPremultiplied,
                                            std::stop_token token)
{
    if (linearPremultiplied.isNull())
        return {};
    const QImage input = linearPremultiplied.format() == QImage::Format_RGBA64_Premultiplied
        ? linearPremultiplied
        : linearPremultiplied.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    if (input.isNull())
        return {};

    const int width = input.width();
    const int height = input.height();
    QImage output(width, height, QImage::Format_ARGB32);
    if (output.isNull())
        return {};

    WorkingSpace &space = workingSpace();
    cmsHTRANSFORM transform = cmsCreateTransform(space.linearRec2020, TYPE_RGB_16, space.srgb,
                                                 TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC,
                                                 cmsFLAGS_BLACKPOINTCOMPENSATION);
    if (!transform)
        return {};

    std::vector<quint16> straight(static_cast<size_t>(width) * 3);
    std::vector<uchar> srgb(static_cast<size_t>(width) * 3);
    for (int y = 0; y < height; ++y) {
        if (token.stop_requested()) {
            cmsDeleteTransform(transform);
            return {};
        }
        const QRgba64 *src = reinterpret_cast<const QRgba64 *>(input.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const quint16 a = src[x].alpha();
            if (a == 0) {
                straight[x * 3] = straight[x * 3 + 1] = straight[x * 3 + 2] = 0;
                continue;
            }
            const quint32 alpha = a;
            straight[x * 3] = static_cast<quint16>(std::min<quint32>(
                65535, (static_cast<quint32>(src[x].red()) * 65535 + alpha / 2) / alpha));
            straight[x * 3 + 1] = static_cast<quint16>(std::min<quint32>(
                65535, (static_cast<quint32>(src[x].green()) * 65535 + alpha / 2) / alpha));
            straight[x * 3 + 2] = static_cast<quint16>(std::min<quint32>(
                65535, (static_cast<quint32>(src[x].blue()) * 65535 + alpha / 2) / alpha));
        }
        cmsDoTransform(transform, straight.data(), srgb.data(), width);
        QRgb *dst = reinterpret_cast<QRgb *>(output.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int alpha = qRound(src[x].alpha() / 257.0);
            dst[x] = qRgba(srgb[x * 3], srgb[x * 3 + 1], srgb[x * 3 + 2], alpha);
        }
    }

    cmsDeleteTransform(transform);
    return output;
}

QImage ColorPipeline::srgbToLinear(const QImage &srgbArgb, std::stop_token token)
{
    if (srgbArgb.isNull())
        return {};
    const QImage input = srgbArgb.convertToFormat(QImage::Format_ARGB32);
    if (input.isNull())
        return {};
    const int width = input.width();
    const int height = input.height();
    QImage output(width, height, QImage::Format_RGBA64_Premultiplied);
    if (output.isNull())
        return {};

    WorkingSpace &space = workingSpace();
    cmsHTRANSFORM transform = cmsCreateTransform(space.srgb, TYPE_RGB_8, space.linearRec2020,
                                                 TYPE_RGB_16, INTENT_RELATIVE_COLORIMETRIC,
                                                 cmsFLAGS_BLACKPOINTCOMPENSATION);
    if (!transform)
        return {};

    std::vector<uchar> rgb(static_cast<size_t>(width) * 3);
    std::vector<quint16> linear(static_cast<size_t>(width) * 3);
    for (int y = 0; y < height; ++y) {
        if (token.stop_requested()) {
            cmsDeleteTransform(transform);
            return {};
        }
        const QRgb *src = reinterpret_cast<const QRgb *>(input.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            rgb[x * 3] = static_cast<uchar>(qRed(src[x]));
            rgb[x * 3 + 1] = static_cast<uchar>(qGreen(src[x]));
            rgb[x * 3 + 2] = static_cast<uchar>(qBlue(src[x]));
        }
        cmsDoTransform(transform, rgb.data(), linear.data(), width);
        QRgba64 *dst = reinterpret_cast<QRgba64 *>(output.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const quint16 a = static_cast<quint16>(qAlpha(src[x]) * 257);
            const quint32 alpha = a;
            const quint16 r = static_cast<quint16>((static_cast<quint32>(linear[x * 3]) * alpha + 32767) / 65535);
            const quint16 g = static_cast<quint16>((static_cast<quint32>(linear[x * 3 + 1]) * alpha + 32767) / 65535);
            const quint16 b = static_cast<quint16>((static_cast<quint32>(linear[x * 3 + 2]) * alpha + 32767) / 65535);
            dst[x] = qRgba64(r, g, b, a);
        }
    }

    cmsDeleteTransform(transform);
    return output;
}

QImage ColorPipeline::toSrgb(const SourceImage &source, const ColorRecipe &recipe, int maxDimension,
                             std::stop_token token)
{
    const QImage linear = toLinear(source, recipe, maxDimension, token);
    if (linear.isNull())
        return {};
    return linearToSrgb(linear, recipe.grayscale, token);
}

QByteArray ColorPipeline::srgbProfileBytes()
{
    cmsHPROFILE srgb = workingSpace().srgb;
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(srgb, nullptr, &size) || size == 0)
        return {};
    QByteArray bytes(static_cast<int>(size), Qt::Uninitialized);
    if (!cmsSaveProfileToMem(srgb, bytes.data(), &size))
        return {};
    return bytes;
}

} // namespace pp
