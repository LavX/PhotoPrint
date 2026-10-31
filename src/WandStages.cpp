#include "WandStages.h"

#include "EffectEngine.h"

#include <MagickWand/MagickWand.h>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace pp {

namespace {

constexpr int kPassCount = 16;
static_assert(kPassCount == static_cast<int>(WandPass::Pixelate) + 1,
              "kPassCount must match the WandPass enum");

std::once_flag g_magickOnce;

void ensureMagick()
{
    std::call_once(g_magickOnce, [] { MagickWandGenesis(); });
}

/// Handle of the MagickWand library this process linked against. A probe must
/// look at the binary, not the header: ImageMagick 7 declares functions it does
/// not export, so `dlsym` is the only honest test.
void *magickHandle()
{
    static void *handle = [] {
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void *>(&MagickGetImageWidth), &info) != 0
            && info.dli_fname) {
            if (void *loaded = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD))
                return loaded;
        }
        return dlopen(nullptr, RTLD_LAZY);
    }();
    return handle;
}

bool hasSymbol(const QString &name)
{
    const QByteArray latin = name.toLatin1();
    void *handle = magickHandle();
    if (!handle)
        return false;
    return dlsym(handle, latin.constData()) != nullptr;
}

void drainException(MagickWand *wand)
{
    ExceptionType severity = UndefinedException;
    char *description = MagickGetException(wand, &severity);
    if (description)
        MagickRelinquishMemory(description);
}

struct WandDeleter {
    void operator()(MagickWand *wand) const
    {
        if (wand)
            DestroyMagickWand(wand);
    }
};

struct PixelWandDeleter {
    void operator()(PixelWand *wand) const
    {
        if (wand)
            DestroyPixelWand(wand);
    }
};

using WandPtr = std::unique_ptr<MagickWand, WandDeleter>;
using PixelWandPtr = std::unique_ptr<PixelWand, PixelWandDeleter>;

/// Row-major RGB bytes, the only thing the wand sees. Alpha is deliberately
/// absent: a geometric pass smears the alpha plane and halos a transparent
/// frame, so the caller re-attaches the source alpha on the way out.
std::vector<unsigned char> rgbBytes(const QImage &image)
{
    const QImage source = image.format() == QImage::Format_RGB32
        ? image
        : image.convertToFormat(QImage::Format_RGB32);
    std::vector<unsigned char> bytes(static_cast<size_t>(source.width())
                                     * static_cast<size_t>(source.height()) * 3);
    size_t out = 0;
    for (int y = 0; y < source.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const QRgb pixel = line[x];
            bytes[out++] = static_cast<unsigned char>(qRed(pixel));
            bytes[out++] = static_cast<unsigned char>(qGreen(pixel));
            bytes[out++] = static_cast<unsigned char>(qBlue(pixel));
        }
    }
    return bytes;
}

WandPtr importRgb(const QImage &image)
{
    ensureMagick();
    WandPtr wand(NewMagickWand());
    if (!wand)
        return {};
    const std::vector<unsigned char> bytes = rgbBytes(image);
    if (MagickConstituteImage(wand.get(), static_cast<size_t>(image.width()),
                              static_cast<size_t>(image.height()), "RGB", CharPixel,
                              bytes.data())
        == MagickFalse) {
        drainException(wand.get());
        return {};
    }
    return wand;
}

/// Reads the frame back as 8-bit RGB. A wand call that changed the canvas size
/// is only acceptable when the caller crops it back, so the size is checked by
/// the caller, not silently accepted here.
QImage exportRgb(MagickWand *wand)
{
    const int width = static_cast<int>(MagickGetImageWidth(wand));
    const int height = static_cast<int>(MagickGetImageHeight(wand));
    if (width <= 0 || height <= 0)
        return {};
    std::vector<unsigned char> bytes(static_cast<size_t>(width) * height * 3);
    if (MagickExportImagePixels(wand, 0, 0, static_cast<size_t>(width),
                                static_cast<size_t>(height), "RGB", CharPixel,
                                bytes.data())
        == MagickFalse) {
        drainException(wand);
        return {};
    }
    QImage out(width, height, QImage::Format_RGB32);
    if (out.isNull())
        return {};
    size_t in = 0;
    for (int y = 0; y < height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            line[x] = qRgb(bytes[in], bytes[in + 1], bytes[in + 2]);
            in += 3;
        }
    }
    return out;
}

std::vector<unsigned char> alphaBytes(const QImage &image)
{
    if (!image.hasAlphaChannel())
        return {};
    const QImage source = image.format() == QImage::Format_ARGB32
        ? image
        : image.convertToFormat(QImage::Format_ARGB32);
    std::vector<unsigned char> alpha(static_cast<size_t>(source.width())
                                     * static_cast<size_t>(source.height()));
    size_t out = 0;
    for (int y = 0; y < source.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < source.width(); ++x)
            alpha[out++] = static_cast<unsigned char>(qAlpha(line[x]));
    }
    return alpha;
}

QImage withAlpha(const QImage &rgb, const std::vector<unsigned char> &alpha)
{
    if (alpha.empty())
        return rgb;
    if (alpha.size() != static_cast<size_t>(rgb.width()) * static_cast<size_t>(rgb.height()))
        return rgb;
    QImage out(rgb.size(), QImage::Format_ARGB32);
    if (out.isNull())
        return rgb;
    size_t index = 0;
    for (int y = 0; y < rgb.height(); ++y) {
        const QRgb *source = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < rgb.width(); ++x)
            line[x] = qRgba(qRed(source[x]), qGreen(source[x]), qBlue(source[x]),
                            alpha[index++]);
    }
    return out;
}

/// The passes that are still too expensive at the print raster. Motion blur
/// costs more than a second at 2400 px and sketch about 24 s, and both are
/// smooth enough that running them on a bounded raster and upscaling is
/// invisible. Everything else runs at the grade raster.
int passRasterCap(WandPass pass)
{
    switch (pass) {
    case WandPass::MotionBlur:
        return 1600;
    case WandPass::Sketch:
        return 1200;
    // Kuwahara is quadratic in sigma and its output is flat regions, so running
    // it on a bounded raster and upscaling is invisible.
    case WandPass::Kuwahara:
        return 1600;
    default:
        return 0;
    }
}
/// The 8x8 clustered-dot threshold screen. Rank order is the distance of the
/// cell from the tile centre, so each tile grows one round dot instead of a
/// dispersed crosshatch. Built once, deterministic, no external data: the
/// catalogue must not depend on ImageMagick's `thresholds.xml`.
const std::array<std::array<int, 8>, 8> &clusteredDot()
{
    static const std::array<std::array<int, 8>, 8> matrix = [] {
        std::array<std::pair<double, int>, 64> cells{};
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const double dx = std::abs(double(x) + 0.5 - 4.0);
                const double dy = std::abs(double(y) + 0.5 - 4.0);
                cells[static_cast<size_t>(y * 8 + x)] = {dx * dx + dy * dy, y * 8 + x};
            }
        }
        std::sort(cells.begin(), cells.end());
        std::array<std::array<int, 8>, 8> out{};
        for (int rank = 0; rank < 64; ++rank) {
            const int index = cells[static_cast<size_t>(rank)].second;
            out[static_cast<size_t>(index / 8)][static_cast<size_t>(index % 8)] = rank;
        }
        return out;
    }();
    return matrix;
}

QImage scaledTo(const QImage &image, const QSize &size, Qt::TransformationMode mode)
{
    if (image.size() == size)
        return image;
    return image.scaled(size, Qt::IgnoreAspectRatio, mode);
}

// -- individual passes ------------------------------------------------------
// -- individual passes ------------------------------------------------------

bool runSwirl(QImage &image, const Stage &stage, double)
{
    if (std::abs(stage.wandDegrees) < 0.01)
        return true;
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const MagickBooleanType ok =
        MagickSwirlImage(wand.get(), stage.wandDegrees, UndefinedInterpolatePixel);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runImplode(QImage &image, const Stage &stage, double)
{
    if (std::abs(stage.wandAmount) < 0.001)
        return true;
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const MagickBooleanType ok =
        MagickImplodeImage(wand.get(), stage.wandAmount, UndefinedInterpolatePixel);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runWave(QImage &image, const Stage &stage, double scale)
{
    // Amplitude and length are pixels at the 2400 px reference. Both floors
    // keep a thumbnail from becoming the identity: below two pixels of
    // amplitude the wave has nothing to bend that a 160 px tile can show.
    const double amplitude = std::max(2.0, stage.wandAmplitude * scale);
    const double length = std::max(6.0, stage.wandLength * scale);
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const size_t width = MagickGetImageWidth(wand.get());
    const size_t height = MagickGetImageHeight(wand.get());
    // MagickWaveImage grows the canvas by twice the amplitude so the displaced
    // rows have somewhere to go. Cropping the growth off about the centre is
    // what keeps the frame size stable.
    const MagickBooleanType ok =
        MagickWaveImage(wand.get(), amplitude, length, UndefinedInterpolatePixel);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    const size_t grown = MagickGetImageHeight(wand.get());
    if (grown != height) {
        const ssize_t offset = static_cast<ssize_t>((grown - height) / 2);
        if (MagickCropImage(wand.get(), width, height, 0, offset) == MagickFalse) {
            drainException(wand.get());
            return false;
        }
    }
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runRotationalBlur(QImage &image, const Stage &stage, double)
{
    if (std::abs(stage.wandAngle) < 0.01)
        return true;
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    // ImageMagick 7 has no radial blur; a small rotational blur is the honest
    // approximation and what the recipes are authored against.
    const MagickBooleanType ok = MagickRotationalBlurImage(wand.get(), stage.wandAngle);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runMotionBlur(QImage &image, const Stage &stage, double scale)
{
    // A one-pixel smear with a sub-pixel kernel is indistinguishable from the
    // untouched frame, so the preview floors the kernel at 3 px / 1 sigma. The
    // print path is exact; the thumbnail is a readable miniature, not a no-op.
    const double radius = std::max(3.0, stage.wandRadius * scale);
    const double sigma = std::max(1.0, stage.wandSigma * scale);
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const MagickBooleanType ok =
        MagickMotionBlurImage(wand.get(), radius, sigma, stage.wandAngle);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runOilPaint(QImage &image, const Stage &stage, double scale)
{
    // A one-pixel median is a no-op: at a 240 px preview a 4 px stroke would
    // round to 1, so the paint never appears in the tile that sells the effect.
    const double radius = std::max(2.0, std::round(stage.wandRadius * scale));
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const MagickBooleanType ok = MagickOilPaintImage(wand.get(), radius, 0.0);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runSketch(QImage &image, const Stage &stage, double scale)
{
    const double sigma = std::max(1.0, std::round(stage.wandSigma * scale));
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const MagickBooleanType ok = MagickSketchImage(wand.get(), 0.0, sigma, 0.0);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runCharcoal(QImage &image, const Stage &stage, double scale)
{
    const double sigma = std::max(0.5, stage.wandSigma * scale);
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const MagickBooleanType ok = MagickCharcoalImage(wand.get(), 0.0, sigma);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runEdge(QImage &image, const Stage &stage, double scale)
{
    const double radius = std::max(1.0, std::round(stage.wandRadius * scale));
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const MagickBooleanType ok = MagickEdgeImage(wand.get(), radius);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runEmboss(QImage &image, const Stage &stage, double scale)
{
    const double sigma = std::max(0.5, stage.wandSigma * scale);
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const MagickBooleanType ok = MagickEmbossImage(wand.get(), 0.0, sigma);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runKuwahara(QImage &image, const Stage &stage, double scale)
{
    // Same floor as oil paint: below two pixels the quadrant filter returns the
    // frame it was given.
    const double sigma = std::max(2.0, std::round(stage.wandSigma * scale));
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const MagickBooleanType ok = MagickKuwaharaImage(wand.get(), 0.0, sigma);
    drainException(wand.get());
    if (ok == MagickFalse)
        return false;
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runMirror(QImage &image, const Stage &, double)
{
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const size_t width = MagickGetImageWidth(wand.get());
    const size_t height = MagickGetImageHeight(wand.get());
    WandPtr half(CloneMagickWand(wand.get()));
    if (!half)
        return false;
    if (MagickFlopImage(half.get()) == MagickFalse) {
        drainException(half.get());
        return false;
    }
    // The flopped frame's *right* half is the mirror image of the original's
    // left half, so copying it over the right half makes the frame symmetric
    // about the centre axis. Taking the flopped frame's left half instead
    // produces [left | flipped right] with a hard seam where the two halves
    // meet; that was the original bug.
    if (MagickCropImage(half.get(), width / 2, height,
                        static_cast<ssize_t>(width - width / 2), 0)
        == MagickFalse) {
        drainException(half.get());
        return false;
    }
    if (MagickCompositeImage(wand.get(), half.get(), CopyCompositeOp, MagickFalse,
                             static_cast<ssize_t>(width - width / 2), 0)
        == MagickFalse) {
        drainException(wand.get());
        return false;
    }
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runKaleido(QImage &image, const Stage &stage, double)
{
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const size_t width = MagickGetImageWidth(wand.get());
    const size_t height = MagickGetImageHeight(wand.get());
    const int segments = std::clamp(stage.wandSegments, 2, 12);
    PixelWandPtr background(NewPixelWand());
    if (!background || PixelSetColor(background.get(), "black") == MagickFalse)
        return false;
    if (MagickSetImageBackgroundColor(wand.get(), background.get()) == MagickFalse) {
        drainException(wand.get());
        return false;
    }
    WandPtr canvas(CloneMagickWand(wand.get()));
    if (!canvas)
        return false;
    for (int index = 1; index < segments; ++index) {
        WandPtr clone(CloneMagickWand(wand.get()));
        if (!clone)
            return false;
        MagickSetImageBackgroundColor(clone.get(), background.get());
        const double angle = 360.0 / double(segments) * double(index);
        if (MagickRotateImage(clone.get(), background.get(), angle) == MagickFalse) {
            drainException(clone.get());
            return false;
        }
        const size_t cloneWidth = MagickGetImageWidth(clone.get());
        const size_t cloneHeight = MagickGetImageHeight(clone.get());
        const ssize_t offsetX = static_cast<ssize_t>((cloneWidth - width) / 2);
        const ssize_t offsetY = static_cast<ssize_t>((cloneHeight - height) / 2);
        if (MagickCropImage(clone.get(), width, height, offsetX, offsetY)
            == MagickFalse) {
            drainException(clone.get());
            return false;
        }
        if (MagickCompositeImage(canvas.get(), clone.get(), OverlayCompositeOp,
                                 MagickFalse, 0, 0)
            == MagickFalse) {
            drainException(canvas.get());
            return false;
        }
    }
    if (MagickCompositeImage(wand.get(), canvas.get(), CopyCompositeOp, MagickFalse, 0, 0)
        == MagickFalse) {
        drainException(wand.get());
        return false;
    }
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runStretch(QImage &image, const Stage &stage, double)
{
    // A factor below 1 would have to pad the frame to crop back to it; the
    // loader rejects that, so this only ever widens.
    const double factor = std::clamp(stage.wandFactor, 1.0, 2.0);
    if (std::abs(factor - 1.0) < 1e-4)
        return true;
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const size_t width = MagickGetImageWidth(wand.get());
    const size_t height = MagickGetImageHeight(wand.get());
    const size_t stretched = std::max<size_t>(1, static_cast<size_t>(
                                                    std::lround(double(width) * factor)));
    if (MagickResizeImage(wand.get(), stretched, height, LanczosFilter) == MagickFalse) {
        drainException(wand.get());
        return false;
    }
    const ssize_t offsetX = static_cast<ssize_t>((stretched - width) / 2);
    if (MagickCropImage(wand.get(), width, height, offsetX, 0) == MagickFalse) {
        drainException(wand.get());
        return false;
    }
    image = exportRgb(wand.get());
    return !image.isNull();
}

bool runPixelate(QImage &image, const Stage &stage, double scale)
{
    const int block = std::max(2, static_cast<int>(std::lround(stage.wandSize * scale)));
    WandPtr wand = importRgb(image);
    if (!wand)
        return false;
    const size_t width = MagickGetImageWidth(wand.get());
    const size_t height = MagickGetImageHeight(wand.get());
    const size_t smallWidth = std::max<size_t>(1, width / static_cast<size_t>(block));
    const size_t smallHeight = std::max<size_t>(1, height / static_cast<size_t>(block));
    if (MagickResizeImage(wand.get(), smallWidth, smallHeight, PointFilter) == MagickFalse
        || MagickResizeImage(wand.get(), width, height, PointFilter) == MagickFalse) {
        drainException(wand.get());
        return false;
    }
    image = exportRgb(wand.get());
    return !image.isNull();
}

/// The dot screen. The 8x8 threshold matrix is stretched over one screen cell
/// of @p period pixels, so the dot pitch follows the raster exactly: the
/// authored period is a fraction of the reference raster and a preview needs a
/// sub-8-pixel screen. This is the only screen the pass has: ImageMagick's
/// threshold maps are an 8 px tile, so using them meant switching algorithms
/// whenever the scaled pitch was not a whole multiple of 8, and a 1.3 percent
/// change of print size flipped the screen shape. The in-process screen also
/// avoids a Q16 wand of the whole finish raster (594 MB at 7087 px) for what is
/// a 40-line dither.
bool runHalftone(QImage &image, const Stage &stage, double scale)
{
    // A pitch of 8.1 px cannot tile a pixel grid: the dot phase would drift by
    // a pixel every ~76 px and leave a visible beat in a flat area. Rounding to
    // whole pixels keeps the screen exactly periodic, which is what a real
    // screen does; the pitch still scales with the raster over any real change
    // of print size. Below 8 px the 8x8 matrix has too few addressable samples
    // to form a dot at all (a 2 px cell degenerates into a striped quantise), so
    // the cell is floored at the smallest screen that still reads as a screen:
    // a 2400 px raster and anything above it gets exactly the authored pitch,
    // a smaller one keeps an 8 px screen instead of a degenerate finer one.
    const int period = std::clamp(static_cast<int>(std::lround(stage.wandSize * scale)),
                                  8, 512);
    const QSize original = image.size();
    QImage working = image;
    orderedDither8x8(working, period);
    if (working.isNull() || working.size() != original)
        return false;
    image = working;
    return true;
}

} // namespace

QString wandPassName(WandPass pass)
{
    switch (pass) {
    case WandPass::Swirl: return QStringLiteral("swirl");
    case WandPass::Implode: return QStringLiteral("implode");
    case WandPass::Wave: return QStringLiteral("wave");
    case WandPass::RotationalBlur: return QStringLiteral("rotationalBlur");
    case WandPass::MotionBlur: return QStringLiteral("motionBlur");
    case WandPass::OilPaint: return QStringLiteral("oilPaint");
    case WandPass::Sketch: return QStringLiteral("sketch");
    case WandPass::Charcoal: return QStringLiteral("charcoal");
    case WandPass::Edge: return QStringLiteral("edge");
    case WandPass::Emboss: return QStringLiteral("emboss");
    case WandPass::Kuwahara: return QStringLiteral("kuwahara");
    case WandPass::Halftone: return QStringLiteral("halftone");
    case WandPass::Mirror: return QStringLiteral("mirror");
    case WandPass::Kaleido: return QStringLiteral("kaleido");
    case WandPass::Stretch: return QStringLiteral("stretch");
    case WandPass::Pixelate: return QStringLiteral("pixelate");
    }
    return {};
}

bool wandPassFromName(const QString &name, WandPass *pass)
{
    for (int index = 0; index < kPassCount; ++index) {
        const auto candidate = static_cast<WandPass>(index);
        if (wandPassName(candidate) == name) {
            if (pass)
                *pass = candidate;
            return true;
        }
    }
    return false;
}

QStringList wandPassNames()
{
    QStringList names;
    for (int index = 0; index < kPassCount; ++index)
        names << wandPassName(static_cast<WandPass>(index));
    return names;
}

QStringList wandRequiredSymbols(WandPass pass)
{
    switch (pass) {
    case WandPass::Swirl: return {QStringLiteral("MagickSwirlImage")};
    case WandPass::Implode: return {QStringLiteral("MagickImplodeImage")};
    case WandPass::Wave: return {QStringLiteral("MagickWaveImage")};
    case WandPass::RotationalBlur: return {QStringLiteral("MagickRotationalBlurImage")};
    case WandPass::MotionBlur: return {QStringLiteral("MagickMotionBlurImage")};
    case WandPass::OilPaint: return {QStringLiteral("MagickOilPaintImage")};
    case WandPass::Sketch: return {QStringLiteral("MagickSketchImage")};
    case WandPass::Charcoal: return {QStringLiteral("MagickCharcoalImage")};
    case WandPass::Edge: return {QStringLiteral("MagickEdgeImage")};
    case WandPass::Emboss: return {QStringLiteral("MagickEmbossImage")};
    case WandPass::Kuwahara: return {QStringLiteral("MagickKuwaharaImage")};
    // The screen is in-process, so the halftone needs nothing from the library.
    case WandPass::Halftone: return {};
    case WandPass::Mirror:
        return {QStringLiteral("CloneMagickWand"), QStringLiteral("MagickFlopImage"),
                QStringLiteral("MagickCropImage"), QStringLiteral("MagickCompositeImage")};
    case WandPass::Kaleido:
        return {QStringLiteral("CloneMagickWand"), QStringLiteral("MagickRotateImage"),
                QStringLiteral("MagickSetImageBackgroundColor"),
                QStringLiteral("MagickCropImage"), QStringLiteral("MagickCompositeImage")};
    case WandPass::Stretch:
        return {QStringLiteral("MagickResizeImage"), QStringLiteral("MagickCropImage")};
    case WandPass::Pixelate: return {QStringLiteral("MagickResizeImage")};
    }
    return {};
}

WandCapability wandCapability(WandPass pass)
{
    static const std::array<WandCapability, kPassCount> table = [] {
        std::array<WandCapability, kPassCount> out{};
        for (int index = 0; index < kPassCount; ++index) {
            const auto candidate = static_cast<WandPass>(index);
            WandCapability capability;
            capability.available = true;
            for (const QString &symbol : wandRequiredSymbols(candidate)) {
                if (hasSymbol(symbol)) {
                    continue;
                }
                capability.available = false;
                capability.reason =
                    QStringLiteral("This ImageMagick build does not provide %1, so the "
                                   "\"%2\" pass cannot run.")
                        .arg(symbol, wandPassName(candidate));
                break;
            }
            out[static_cast<size_t>(index)] = capability;
        }
        return out;
    }();
    return table[static_cast<size_t>(pass)];
}

void orderedDither8x8(QImage &image, int period)
{
    if (image.isNull())
        return;
    if (image.format() != QImage::Format_ARGB32 && image.format() != QImage::Format_RGB32) {
        QImage converted = image.convertToFormat(QImage::Format_ARGB32);
        if (converted.isNull())
            return;
        image = converted;
    }
    image.detach();
    // One screen cell is `period` whole pixels across and holds the whole 8x8
    // threshold matrix, so the dot pitch is the authored pitch, the screen is
    // exactly periodic, and the raster scales it. When the cell is a multiple of
    // eight every matrix entry maps to a whole block of pixels and the sample is
    // exact. Any other period is sampled from the same matrix as a continuous,
    // wrapping field: a nearest sample would cover some ranks twice and shift
    // the tone of a flat area by several percent, while the interpolated field
    // keeps the mean of the screen.
    const int cell = std::clamp(period, 1, 1024);
    const auto &matrix = clusteredDot();
    const int block = cell % 8 == 0 ? cell / 8 : 0;

    std::vector<int> columnLow(static_cast<size_t>(image.width()));
    std::vector<int> columnHigh(static_cast<size_t>(image.width()));
    std::vector<double> columnFraction(static_cast<size_t>(image.width()));
    for (int x = 0; x < image.width(); ++x) {
        double u = 0.0;
        if (block > 0) {
            u = double((x % cell) / block);
        } else {
            u = (double(x % cell) + 0.5) * 8.0 / double(cell) - 0.5;
            u -= std::floor(u / 8.0) * 8.0;
        }
        const int low = std::clamp(static_cast<int>(u), 0, 7);
        columnLow[static_cast<size_t>(x)] = low;
        columnHigh[static_cast<size_t>(x)] = (low + 1) % 8;
        columnFraction[static_cast<size_t>(x)] = u - double(low);
    }

    for (int y = 0; y < image.height(); ++y) {
        double v = 0.0;
        if (block > 0) {
            v = double((y % cell) / block);
        } else {
            v = (double(y % cell) + 0.5) * 8.0 / double(cell) - 0.5;
            v -= std::floor(v / 8.0) * 8.0;
        }
        const int rowLow = std::clamp(static_cast<int>(v), 0, 7);
        const int rowHigh = (rowLow + 1) % 8;
        const double fy = v - double(rowLow);
        const auto &top = matrix[static_cast<size_t>(rowLow)];
        const auto &bottom = matrix[static_cast<size_t>(rowHigh)];
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int low = columnLow[static_cast<size_t>(x)];
            const int high = columnHigh[static_cast<size_t>(x)];
            const double fx = columnFraction[static_cast<size_t>(x)];
            const double rank = (double(top[static_cast<size_t>(low)]) * (1.0 - fx)
                                 + double(top[static_cast<size_t>(high)]) * fx)
                    * (1.0 - fy)
                + (double(bottom[static_cast<size_t>(low)]) * (1.0 - fx)
                   + double(bottom[static_cast<size_t>(high)]) * fx)
                    * fy;
            const int threshold =
                static_cast<int>(std::lround((rank + 1.0) * 256.0 / 65.0));
            const QRgb pixel = line[x];
            line[x] = qRgba(qRed(pixel) >= threshold ? 255 : 0,
                            qGreen(pixel) >= threshold ? 255 : 0,
                            qBlue(pixel) >= threshold ? 255 : 0, qAlpha(pixel));
        }
    }
}

Result<void> applyWandPass(QImage &image, const Stage &stage, int longSide)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return error(QStringLiteral("effect-input"), QStringLiteral("No pixels to transform."));
    WandPass pass = WandPass::Swirl;
    if (!wandPassFromName(stage.wandPass, &pass)) {
        return error(QStringLiteral("effect-wand-unknown"),
                     QStringLiteral("Unknown MagickWand pass '%1'.").arg(stage.wandPass));
    }
    const WandCapability capability = wandCapability(pass);
    if (!capability.available) {
        return error(QStringLiteral("effect-unavailable"), capability.reason);
    }
    ensureMagick();

    const std::vector<unsigned char> alpha = alphaBytes(image);
    const double scale = EffectEngine::referenceScale(longSide);
    QImage working = image.format() == QImage::Format_RGB32
        ? image
        : image.convertToFormat(QImage::Format_RGB32);
    if (working.isNull())
        return error(QStringLiteral("effect-input"), QStringLiteral("The frame cannot be read."));

    // A bounded pass runs on the cap raster and is scaled back; the smooth
    // passes survive that, a dot screen would not (so halftone has no cap).
    const int cap = passRasterCap(pass);
    const int longest = std::max(working.width(), working.height());
    QSize cappedSize;
    if (cap > 0 && longest > cap) {
        const double factor = double(cap) / double(longest);
        cappedSize = QSize(std::max(1, int(std::lround(working.width() * factor))),
                           std::max(1, int(std::lround(working.height() * factor))));
        working = scaledTo(working, cappedSize, Qt::SmoothTransformation);
    }
    const QSize frameSize = working.size();

    bool ok = false;
    switch (pass) {
    case WandPass::Swirl: ok = runSwirl(working, stage, scale); break;
    case WandPass::Implode: ok = runImplode(working, stage, scale); break;
    case WandPass::Wave: ok = runWave(working, stage, scale); break;
    case WandPass::RotationalBlur: ok = runRotationalBlur(working, stage, scale); break;
    case WandPass::MotionBlur: ok = runMotionBlur(working, stage, scale); break;
    case WandPass::OilPaint: ok = runOilPaint(working, stage, scale); break;
    case WandPass::Sketch: ok = runSketch(working, stage, scale); break;
    case WandPass::Charcoal: ok = runCharcoal(working, stage, scale); break;
    case WandPass::Edge: ok = runEdge(working, stage, scale); break;
    case WandPass::Emboss: ok = runEmboss(working, stage, scale); break;
    case WandPass::Kuwahara: ok = runKuwahara(working, stage, scale); break;
    case WandPass::Halftone: ok = runHalftone(working, stage, scale); break;
    case WandPass::Mirror: ok = runMirror(working, stage, scale); break;
    case WandPass::Kaleido: ok = runKaleido(working, stage, scale); break;
    case WandPass::Stretch: ok = runStretch(working, stage, scale); break;
    case WandPass::Pixelate: ok = runPixelate(working, stage, scale); break;
    }
    if (!ok || working.isNull() || working.size() != frameSize) {
        return error(QStringLiteral("effect-wand-failed"),
                     QStringLiteral("The \"%1\" pass could not be applied to this frame.")
                         .arg(wandPassName(pass)));
    }
    if (!cappedSize.isEmpty())
        working = scaledTo(working, image.size(), Qt::SmoothTransformation);
    if (working.isNull() || working.size() != image.size()) {
        return error(QStringLiteral("effect-wand-failed"),
                     QStringLiteral("The \"%1\" pass changed the frame size.")
                         .arg(wandPassName(pass)));
    }
    image = withAlpha(working, alpha);
    return {};
}

} // namespace pp
