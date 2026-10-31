#include "EffectEngine.h"

#include <QDebug>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <vector>

void ppInitPhotoPrintResources();

namespace pp {

namespace {

void ensureEffectResources()
{
    static std::once_flag flag;
    std::call_once(flag, [] { ::ppInitPhotoPrintResources(); });
}

inline float clamp01(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

inline float srgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f
                             : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float value)
{
    return value <= 0.0031308f ? value * 12.92f
                               : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

inline float luma709(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

inline float smoothstep(float edge0, float edge1, float x)
{
    if (edge1 <= edge0)
        return x < edge0 ? 0.0f : 1.0f;
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

/// Monotone cubic Hermite through the control points, Fritsch-Carlson slopes.
struct CurveEval {
    QVector<double> x;
    QVector<double> y;
    QVector<double> m;
    bool valid = false;

    static CurveEval build(const QList<QPointF> &points)
    {
        CurveEval curve;
        if (points.size() < 2)
            return curve;
        const int count = points.size();
        curve.x.resize(count);
        curve.y.resize(count);
        curve.m.resize(count);
        for (int i = 0; i < count; ++i) {
            curve.x[i] = points.at(i).x();
            curve.y[i] = points.at(i).y();
        }
        QVector<double> delta(count - 1);
        for (int i = 0; i < count - 1; ++i)
            delta[i] = (curve.y[i + 1] - curve.y[i]) / (curve.x[i + 1] - curve.x[i]);
        curve.m[0] = delta[0];
        curve.m[count - 1] = delta[count - 2];
        for (int i = 1; i < count - 1; ++i) {
            curve.m[i] = delta[i - 1] * delta[i] <= 0.0
                ? 0.0
                : (delta[i - 1] + delta[i]) / 2.0;
        }
        for (int i = 0; i < count - 1; ++i) {
            if (delta[i] == 0.0) {
                curve.m[i] = 0.0;
                curve.m[i + 1] = 0.0;
                continue;
            }
            const double a = curve.m[i] / delta[i];
            const double b = curve.m[i + 1] / delta[i];
            const double sum = a * a + b * b;
            if (sum > 9.0) {
                const double t = 3.0 / std::sqrt(sum);
                curve.m[i] = t * a * delta[i];
                curve.m[i + 1] = t * b * delta[i];
            }
        }
        curve.valid = true;
        return curve;
    }

    double eval(double value) const
    {
        if (!valid)
            return value;
        if (value <= x.first())
            return y.first();
        if (value >= x.last())
            return y.last();
        int i = 0;
        while (i + 1 < x.size() && value > x[i + 1])
            ++i;
        const double h = x[i + 1] - x[i];
        const double t = (value - x[i]) / h;
        const double t2 = t * t;
        const double t3 = t2 * t;
        return (2.0 * t3 - 3.0 * t2 + 1.0) * y[i] + (t3 - 2.0 * t2 + t) * h * m[i]
            + (-2.0 * t3 + 3.0 * t2) * y[i + 1] + (t3 - t2) * h * m[i + 1];
    }
};

struct ToneStagePrep {
    const Stage *stage = nullptr;
    CurveEval master;
    CurveEval channel[3];
    double gain[3] = {1.0, 1.0, 1.0};
    double exposure = 1.0;
    double contrastK = 0.0;
    double levelsDenominator = 1.0;
    int posterizeLevels = 2;
};

double applyTone(const ToneStagePrep &prep, int channel, double value)
{
    const Stage &stage = *prep.stage;
    switch (stage.id) {
    case StageId::WhiteBalance: {
        const double gained = srgbToLinear(float(clamp01(float(value)))) * prep.gain[channel];
        return linearToSrgb(float(std::max(0.0, gained)));
    }
    case StageId::Exposure: {
        const double gained = srgbToLinear(float(clamp01(float(value)))) * prep.exposure;
        return linearToSrgb(float(std::max(0.0, gained)));
    }
    case StageId::Gamma:
        return std::pow(std::max(0.0, value), 1.0 / stage.gamma);
    case StageId::Contrast: {
        const double offset = value - stage.pivot;
        return stage.pivot + offset * (1.0 + prep.contrastK)
            - prep.contrastK * offset * offset * offset;
    }
    case StageId::Curves: {
        double result = value;
        if (prep.channel[channel].valid)
            result = prep.channel[channel].eval(result);
        if (prep.master.valid)
            result = prep.master.eval(result);
        return result;
    }
    case StageId::Levels:
        return std::pow(std::max(0.0, (value - stage.black) / prep.levelsDenominator),
                        1.0 / stage.gamma);
    case StageId::Fade: {
        double result = stage.lift + (1.0 - stage.lift) * std::pow(std::max(0.0, value), stage.toe);
        result = result * (1.0 - stage.roll * result * result);
        return result;
    }
    case StageId::Posterize: {
        const double levels = std::max(2, prep.posterizeLevels);
        return std::round(value * (levels - 1.0)) / (levels - 1.0);
    }
    case StageId::Invert:
        return 1.0 - value;
    default:
        return value;
    }
}

struct ColourStagePrep {
    const Stage *stage = nullptr;
    float factor = 1.0f;
    std::array<float, 9> matrix{};
    float cosAngle = 1.0f;
    float sinAngle = 0.0f;
    float shadow[3] = {0.5f, 0.5f, 0.5f};
    float highlight[3] = {0.5f, 0.5f, 0.5f};
    float shadowStrength = 0.0f;
    float highlightStrength = 0.0f;
    float balance = 0.5f;
    QList<GradientStop> stops;
    float mix = 1.0f;
};

inline void applyColour(const ColourStagePrep &prep, float &r, float &g, float &b)
{
    const Stage &stage = *prep.stage;
    switch (stage.id) {
    case StageId::ColorMatrix: {
        const float lr = srgbToLinear(r);
        const float lg = srgbToLinear(g);
        const float lb = srgbToLinear(b);
        const auto &m = prep.matrix;
        r = linearToSrgb(std::max(0.0f, m[0] * lr + m[1] * lg + m[2] * lb));
        g = linearToSrgb(std::max(0.0f, m[3] * lr + m[4] * lg + m[5] * lb));
        b = linearToSrgb(std::max(0.0f, m[6] * lr + m[7] * lg + m[8] * lb));
        break;
    }
    case StageId::Saturation: {
        const float y = luma709(r, g, b);
        r = y + (r - y) * prep.factor;
        g = y + (g - y) * prep.factor;
        b = y + (b - y) * prep.factor;
        break;
    }
    case StageId::Vibrance: {
        const float chroma = std::max({r, g, b}) - std::min({r, g, b});
        const float factor = 1.0f + (prep.factor - 1.0f) * (1.0f - chroma) * (1.0f - chroma);
        const float y = luma709(r, g, b);
        r = y + (r - y) * factor;
        g = y + (g - y) * factor;
        b = y + (b - y) * factor;
        break;
    }
    case StageId::HueShift: {
        const float y = 0.299f * r + 0.587f * g + 0.114f * b;
        const float i = 0.596f * r - 0.274f * g - 0.322f * b;
        const float q = 0.211f * r - 0.523f * g + 0.312f * b;
        const float rotatedI = i * prep.cosAngle - q * prep.sinAngle;
        const float rotatedQ = i * prep.sinAngle + q * prep.cosAngle;
        r = y + 0.956f * rotatedI + 0.621f * rotatedQ;
        g = y - 0.272f * rotatedI - 0.647f * rotatedQ;
        b = y - 1.106f * rotatedI + 1.703f * rotatedQ;
        break;
    }
    case StageId::SplitTone: {
        const float y = luma709(r, g, b);
        const float balance = std::max(0.02f, prep.balance);
        float shadowWeight = 1.0f - y / balance;
        shadowWeight = shadowWeight > 0.0f ? shadowWeight * shadowWeight : 0.0f;
        float highlightWeight = (y - balance) / std::max(0.02f, 1.0f - balance);
        highlightWeight = highlightWeight > 0.0f ? highlightWeight * highlightWeight : 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float shadowDelta =
                (prep.shadow[c] - 0.5f) * 2.0f * shadowWeight * prep.shadowStrength;
            const float highlightDelta =
                (prep.highlight[c] - 0.5f) * 2.0f * highlightWeight * prep.highlightStrength;
            const float delta = shadowDelta + highlightDelta;
            if (c == 0)
                r += delta;
            else if (c == 1)
                g += delta;
            else
                b += delta;
        }
        break;
    }
    case StageId::GradientMap: {
        const float y = clamp01(luma709(r, g, b));
        float mapped[3] = {prep.stops.last().colour.redF(),
                           prep.stops.last().colour.greenF(),
                           prep.stops.last().colour.blueF()};
        for (int i = 0; i + 1 < prep.stops.size(); ++i) {
            const GradientStop &a = prep.stops.at(i);
            const GradientStop &bStop = prep.stops.at(i + 1);
            if (y <= bStop.position || i + 2 == prep.stops.size()) {
                const float span = float(std::max(1e-6, bStop.position - a.position));
                const float t = clamp01((y - float(a.position)) / span);
                mapped[0] = float(a.colour.redF()) * (1.0f - t) + float(bStop.colour.redF()) * t;
                mapped[1] = float(a.colour.greenF()) * (1.0f - t) + float(bStop.colour.greenF()) * t;
                mapped[2] = float(a.colour.blueF()) * (1.0f - t) + float(bStop.colour.blueF()) * t;
                break;
            }
        }
        r = r * (1.0f - prep.mix) + mapped[0] * prep.mix;
        g = g * (1.0f - prep.mix) + mapped[1] * prep.mix;
        b = b * (1.0f - prep.mix) + mapped[2] * prep.mix;
        break;
    }
    default:
        break;
    }
}

inline float sampleLut(const std::array<float, 1024> &lut, float value)
{
    const float scaled = clamp01(value) * 1023.0f;
    const int index = int(scaled);
    if (index >= 1023)
        return lut[1023];
    const float fraction = scaled - float(index);
    return lut[index] + (lut[index + 1] - lut[index]) * fraction;
}

void boxBlur(EffectRgbBuffer &buffer, int radius, int passes)
{
    if (radius < 1 || buffer.width <= 0 || buffer.height <= 0)
        return;
    const int window = radius * 2 + 1;
    std::vector<float> temp(buffer.pixels.size(), 0.0f);
    const int width = buffer.width;
    const int height = buffer.height;

    for (int pass = 0; pass < std::max(1, passes); ++pass) {
        // Horizontal.
        for (int y = 0; y < height; ++y) {
            const float *row = buffer.at(0, y);
            float *outRow = temp.data() + static_cast<size_t>(y) * width * 3;
            for (int c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (int k = -radius; k <= radius; ++k)
                    sum += row[static_cast<size_t>(std::clamp(k, 0, width - 1)) * 3 + c];
                for (int x = 0; x < width; ++x) {
                    outRow[static_cast<size_t>(x) * 3 + c] = sum / float(window);
                    const int add = std::clamp(x + radius + 1, 0, width - 1);
                    const int sub = std::clamp(x - radius, 0, width - 1);
                    sum += row[static_cast<size_t>(add) * 3 + c]
                        - row[static_cast<size_t>(sub) * 3 + c];
                }
            }
        }
        // Vertical, one row of running sums at a time so both reads and
        // writes stay contiguous. A column-major loop here costs three times
        // as much at 2400 px on this machine.
        std::vector<float> sums(static_cast<size_t>(width) * 3, 0.0f);
        for (int k = -radius; k <= radius; ++k) {
            const float *row =
                temp.data() + static_cast<size_t>(std::clamp(k, 0, height - 1)) * width * 3;
            for (size_t i = 0; i < sums.size(); ++i)
                sums[i] += row[i];
        }
        for (int y = 0; y < height; ++y) {
            float *outRow = buffer.at(0, y);
            for (size_t i = 0; i < sums.size(); ++i)
                outRow[i] = sums[i] / float(window);
            const int add = std::clamp(y + radius + 1, 0, height - 1);
            const int sub = std::clamp(y - radius, 0, height - 1);
            const float *addRow = temp.data() + static_cast<size_t>(add) * width * 3;
            const float *subRow = temp.data() + static_cast<size_t>(sub) * width * 3;
            for (size_t i = 0; i < sums.size(); ++i)
                sums[i] += addRow[i] - subRow[i];
        }
    }
}

void applyBlurMask(EffectRgbBuffer &buffer, const EffectRgbBuffer &sharp, const StageMask &mask)
{
    if (mask.kind == MaskKind::None)
        return;
    const int width = buffer.width;
    const int height = buffer.height;
    for (int y = 0; y < height; ++y) {
        const float ny = (float(y) + 0.5f) / float(height);
        for (int x = 0; x < width; ++x) {
            const float nx = (float(x) + 0.5f) / float(width);
            float weight = 1.0f;
            if (mask.kind == MaskKind::Linear) {
                const float inside =
                    smoothstep(float(mask.start - mask.feather), float(mask.start + mask.feather), ny)
                    * (1.0f - smoothstep(float(mask.end - mask.feather),
                                         float(mask.end + mask.feather), ny));
                weight = 1.0f - inside;
            } else {
                const float dx = nx - float(mask.centre.x());
                const float dy = ny - float(mask.centre.y());
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float radius = float(std::max(1e-4, mask.end));
                weight = smoothstep(radius * (1.0f - float(mask.feather)), radius, distance);
            }
            float *blurred = buffer.at(x, y);
            const float *sharpPixel = sharp.at(x, y);
            for (int c = 0; c < 3; ++c)
                blurred[c] = sharpPixel[c] + (blurred[c] - sharpPixel[c]) * weight;
        }
    }
}

EffectRgbBuffer quarterDownsample(const EffectRgbBuffer &buffer)
{
    const int width = std::max(1, buffer.width / 4);
    const int height = std::max(1, buffer.height / 4);
    EffectRgbBuffer out;
    out.resize(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sum[3] = {0.0f, 0.0f, 0.0f};
            int count = 0;
            for (int dy = 0; dy < 4; ++dy) {
                const int sy = std::min(buffer.height - 1, y * 4 + dy);
                for (int dx = 0; dx < 4; ++dx) {
                    const int sx = std::min(buffer.width - 1, x * 4 + dx);
                    const float *pixel = buffer.at(sx, sy);
                    sum[0] += pixel[0];
                    sum[1] += pixel[1];
                    sum[2] += pixel[2];
                    count += 1;
                }
            }
            float *outPixel = out.at(x, y);
            for (int c = 0; c < 3; ++c)
                outPixel[c] = sum[c] / float(std::max(1, count));
        }
    }
    return out;
}

void applyBloom(EffectRgbBuffer &buffer, const Stage &stage, double scale)
{
    EffectRgbBuffer low = quarterDownsample(buffer);
    for (int y = 0; y < low.height; ++y) {
        for (int x = 0; x < low.width; ++x) {
            float *pixel = low.at(x, y);
            for (int c = 0; c < 3; ++c)
                pixel[c] = std::max(0.0f, pixel[c] - float(stage.threshold));
        }
    }
    const int radius = std::max(1, int(std::lround(stage.radius * scale / 4.0)));
    boxBlur(low, radius, 1);
    const float tint[3] = {float(stage.tintColour.redF()), float(stage.tintColour.greenF()),
                           float(stage.tintColour.blueF())};
    const float intensity = float(stage.intensity);
    for (int y = 0; y < buffer.height; ++y) {
        const float sourceY = std::clamp((float(y) + 0.5f) / 4.0f - 0.5f, 0.0f,
                                         float(low.height - 1));
        const int y0 = int(sourceY);
        const int y1 = std::min(low.height - 1, y0 + 1);
        const float fy = sourceY - float(y0);
        const float *row0 = low.at(0, y0);
        const float *row1 = low.at(0, y1);
        float *pixel = buffer.at(0, y);
        for (int x = 0; x < buffer.width; ++x, pixel += 3) {
            const float sourceX = std::clamp((float(x) + 0.5f) / 4.0f - 0.5f, 0.0f,
                                             float(low.width - 1));
            const int x0 = int(sourceX);
            const int x1 = std::min(low.width - 1, x0 + 1);
            const float fx = sourceX - float(x0);
            for (int c = 0; c < 3; ++c) {
                const float top = row0[x0 * 3 + c] * (1.0f - fx) + row0[x1 * 3 + c] * fx;
                const float bottom = row1[x0 * 3 + c] * (1.0f - fx) + row1[x1 * 3 + c] * fx;
                pixel[c] += (top * (1.0f - fy) + bottom * fy) * tint[c] * intensity;
            }
        }
    }
}

inline float softLight(float base, float blend)
{
    if (blend <= 0.5f)
        return base - (1.0f - 2.0f * blend) * base * (1.0f - base);
    const float d = base <= 0.25f ? ((16.0f * base - 12.0f) * base + 4.0f) * base
                                  : std::sqrt(base);
    return base + (2.0f * blend - 1.0f) * (d - base);
}

inline float overlay(float base, float blend)
{
    return base <= 0.5f ? 2.0f * base * blend : 1.0f - 2.0f * (1.0f - base) * (1.0f - blend);
}

void applyTexture(EffectRgbBuffer &buffer, const Stage &stage, int longSide)
{
    const auto asset = effectTexture(stage.asset);
    if (!asset)
        return;
    const QImage texture = asset.value().convertToFormat(QImage::Format_ARGB32);
    const int width = buffer.width;
    const int height = buffer.height;
    const double scale = EffectEngine::referenceScale(longSide) * std::max(0.25, stage.scale);
    const float tile = std::max(1.0f, float(512.0 * scale));
    const float opacity = float(stage.opacity);
    const int textureWidth = texture.width();
    const int textureHeight = texture.height();

    for (int y = 0; y < height; ++y) {
        const QRgb *textureLine =
            reinterpret_cast<const QRgb *>(texture.constScanLine(
                std::clamp(int(std::fmod((float(y) + 0.5f) / tile, 1.0f)
                               * float(textureHeight)),
                           0, textureHeight - 1)));
        float *pixel = buffer.at(0, y);
        for (int x = 0; x < width; ++x, pixel += 3) {
            const float u = std::fmod((float(x) + 0.5f) / tile, 1.0f) * float(textureWidth);
            const int x0 = std::clamp(int(u), 0, textureWidth - 1);
            const QRgb texel = textureLine[x0];
            const float grey =
                (qRed(texel) + qGreen(texel) + qBlue(texel)) / (3.0f * 255.0f);
            for (int c = 0; c < 3; ++c) {
                float blended = pixel[c] * grey;
                if (stage.blend == BlendMode::SoftLight)
                    blended = softLight(pixel[c], grey);
                else if (stage.blend == BlendMode::Overlay)
                    blended = overlay(pixel[c], grey);
                else if (stage.blend == BlendMode::Screen)
                    blended = 1.0f - (1.0f - pixel[c]) * (1.0f - grey);
                pixel[c] = pixel[c] + (blended - pixel[c]) * opacity;
            }
        }
    }
}

inline quint32 hashNoise(quint32 value)
{
    value *= 2654435761u;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}

inline float noiseValue(quint32 value)
{
    return float(hashNoise(value) & 0xFFFFu) / 32767.5f - 1.0f;
}

void applyGrain(EffectRgbBuffer &buffer, const Stage &stage, double scale)
{
    const int step = std::max(1, int(std::lround(stage.size * scale)));
    const int blocksWide = (buffer.width + step - 1) / step;
    const float amount = float(stage.amount);
    for (int y = 0; y < buffer.height; ++y) {
        const int blockY = y / step;
        float *pixel = buffer.at(0, y);
        for (int x = 0; x < buffer.width; ++x, pixel += 3) {
            const int blockX = x / step;
            const quint32 index = quint32(blockY * blocksWide + blockX);
            const float luma = luma709(pixel[0], pixel[1], pixel[2]);
            const float amplitude = amount * (1.0f - 0.6f * luma);
            if (stage.mono) {
                const float noise = noiseValue(index ^ stage.seed);
                pixel[0] += noise * amplitude;
                pixel[1] += noise * amplitude;
                pixel[2] += noise * amplitude;
            } else {
                pixel[0] += noiseValue((index * 3u) ^ stage.seed) * amplitude;
                pixel[1] += noiseValue((index * 3u + 1u) ^ stage.seed) * amplitude;
                pixel[2] += noiseValue((index * 3u + 2u) ^ stage.seed) * amplitude;
            }
        }
    }
}

void applyVignette(EffectRgbBuffer &buffer, const Stage &stage)
{
    const float aspect = float(buffer.width) / float(std::max(1, buffer.height));
    const float normaliser = std::max(1.0f, aspect * 0.5f + 0.5f);
    const float midpoint = float(stage.midpoint);
    const float feather = float(stage.feather);
    const float amount = float(stage.amount);
    const float roundness = float(std::clamp((stage.roundness + 1.0) / 2.0, 0.0, 1.0));
    std::vector<float> nx2(static_cast<size_t>(buffer.width));
    std::vector<float> nx(static_cast<size_t>(buffer.width));
    for (int x = 0; x < buffer.width; ++x) {
        const float value = ((float(x) + 0.5f) / float(buffer.width) - 0.5f) * 2.0f;
        nx[static_cast<size_t>(x)] = value;
        nx2[static_cast<size_t>(x)] = value * value;
    }
    for (int y = 0; y < buffer.height; ++y) {
        const float ny = ((float(y) + 0.5f) / float(buffer.height) - 0.5f) * 2.0f;
        float *pixel = buffer.at(0, y);
        for (int x = 0; x < buffer.width; ++x, pixel += 3) {
            const float circle =
                std::sqrt(nx2[static_cast<size_t>(x)] + ny * ny);
            const float square = std::max(std::abs(nx[static_cast<size_t>(x)]), std::abs(ny));
            const float distance = circle * roundness + square * (1.0f - roundness);
            const float falloff =
                smoothstep(midpoint, midpoint + feather, distance / normaliser);
            const float factor = 1.0f - amount * falloff;
            pixel[0] *= factor;
            pixel[1] *= factor;
            pixel[2] *= factor;
        }
    }
}

void applyLightLeak(EffectRgbBuffer &buffer, const Stage &stage)
{
    const float colour[3] = {float(stage.tintColour.redF()), float(stage.tintColour.greenF()),
                             float(stage.tintColour.blueF())};
    const float radius = std::max(1e-4f, float(stage.radius));
    const float intensity = float(stage.intensity);
    const float exponent = 1.0f / std::max(0.05f, float(stage.softness));
    for (int y = 0; y < buffer.height; ++y) {
        for (int x = 0; x < buffer.width; ++x) {
            const float dx = (float(x) + 0.5f) / float(buffer.width) - float(stage.centre.x());
            const float dy = (float(y) + 0.5f) / float(buffer.height) - float(stage.centre.y());
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float weight = std::pow(clamp01(1.0f - distance / radius), exponent) * intensity;
            float *pixel = buffer.at(x, y);
            for (int c = 0; c < 3; ++c) {
                if (stage.blend == BlendMode::Add)
                    pixel[c] += colour[c] * weight;
                else
                    pixel[c] = 1.0f - (1.0f - pixel[c]) * (1.0f - colour[c] * weight);
            }
        }
    }
}

void applyExposureMask(EffectRgbBuffer &buffer, const Stage &stage)
{
    const float multiplier = std::pow(2.0f, float(stage.ev));
    const float size = std::max(1e-4f, float(stage.maskSize));
    const float feather = std::max(1e-4f, float(stage.feather));
    for (int y = 0; y < buffer.height; ++y) {
        const float ny = (float(y) + 0.5f) / float(buffer.height);
        for (int x = 0; x < buffer.width; ++x) {
            const float nx = (float(x) + 0.5f) / float(buffer.width);
            float weight = 0.0f;
            if (stage.mask.kind == MaskKind::Linear) {
                const float centre = float(stage.centre.y());
                weight = 1.0f - smoothstep(centre - size * 0.5f, centre + size * 0.5f, ny);
                weight = smoothstep(0.0f, feather, weight);
            } else {
                const float dx = nx - float(stage.centre.x());
                const float dy = ny - float(stage.centre.y());
                const float distance = std::sqrt(dx * dx + dy * dy);
                weight = 1.0f - smoothstep(size * (1.0f - feather), size, distance);
            }
            const float factor = 1.0f + (multiplier - 1.0f) * weight;
            float *pixel = buffer.at(x, y);
            pixel[0] *= factor;
            pixel[1] *= factor;
            pixel[2] *= factor;
        }
    }
}

void applyPosterizeDither(EffectRgbBuffer &buffer, const Stage &stage)
{
    static const int bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const float levels = float(std::max(2, stage.levels));
    for (int y = 0; y < buffer.height; ++y) {
        for (int x = 0; x < buffer.width; ++x) {
            const float offset = (float(bayer[y % 4][x % 4]) + 0.5f) / 16.0f;
            float *pixel = buffer.at(x, y);
            for (int c = 0; c < 3; ++c)
                pixel[c] = std::floor(clamp01(pixel[c]) * (levels - 1.0f) + offset)
                    / (levels - 1.0f);
        }
    }
}

} // namespace

namespace effectstages {

void bakeCurveLut(const QList<Stage> &toneStages, EffectCurveLut &lut)
{
    QVector<ToneStagePrep> prepared;
    prepared.reserve(toneStages.size());
    for (const Stage &stage : toneStages) {
        ToneStagePrep prep;
        prep.stage = &stage;
        if (stage.id == StageId::Curves) {
            prep.master = CurveEval::build(stage.curveMaster);
            prep.channel[0] = CurveEval::build(stage.curveR);
            prep.channel[1] = CurveEval::build(stage.curveG);
            prep.channel[2] = CurveEval::build(stage.curveB);
        }
        if (stage.id == StageId::WhiteBalance) {
            prep.gain[0] = std::pow(2.0, stage.temperature / 200.0);
            prep.gain[1] = std::pow(2.0, -stage.tint / 200.0);
            prep.gain[2] = std::pow(2.0, -stage.temperature / 200.0);
        }
        if (stage.id == StageId::Exposure)
            prep.exposure = std::pow(2.0, stage.ev);
        if (stage.id == StageId::Contrast)
            prep.contrastK = stage.amount / 100.0 * 0.55;
        if (stage.id == StageId::Levels)
            prep.levelsDenominator = std::max(1e-6, stage.white - stage.black);
        if (stage.id == StageId::Posterize)
            prep.posterizeLevels = std::clamp(stage.levels, 2, 32);
        prepared.append(prep);
    }

    for (int channel = 0; channel < 3; ++channel) {
        for (int i = 0; i < 1024; ++i) {
            double value = double(i) / 1023.0;
            for (const ToneStagePrep &prep : prepared)
                value = applyTone(prep, channel, value);
            lut[static_cast<size_t>(channel)][static_cast<size_t>(i)] = float(value);
        }
    }
}

void applyPointStep(EffectRgbBuffer &buffer, const EffectPlanStep &step)
{
    QVector<ColourStagePrep> prepared;
    prepared.reserve(step.colourStages.size());
    for (const Stage &stage : step.colourStages) {
        ColourStagePrep prep;
        prep.stage = &stage;
        switch (stage.id) {
        case StageId::Saturation:
        case StageId::Vibrance:
            prep.factor = 1.0f + float(stage.amount / 100.0);
            break;
        case StageId::HueShift: {
            const double angle = stage.amount * 3.14159265358979323846 / 180.0;
            prep.cosAngle = float(std::cos(angle));
            prep.sinAngle = float(std::sin(angle));
            break;
        }
        case StageId::ColorMatrix:
            for (size_t i = 0; i < 9; ++i)
                prep.matrix[i] = float(stage.matrix[i]);
            break;
        case StageId::SplitTone:
            prep.shadow[0] = float(stage.shadow.redF());
            prep.shadow[1] = float(stage.shadow.greenF());
            prep.shadow[2] = float(stage.shadow.blueF());
            prep.highlight[0] = float(stage.highlight.redF());
            prep.highlight[1] = float(stage.highlight.greenF());
            prep.highlight[2] = float(stage.highlight.blueF());
            prep.shadowStrength = float(stage.shadowStrength);
            prep.highlightStrength = float(stage.highlightStrength);
            prep.balance = float(stage.balance);
            break;
        case StageId::GradientMap:
            prep.stops = stage.stops;
            prep.mix = float(stage.mix);
            break;
        default:
            break;
        }
        prepared.append(prep);
    }

    for (int y = 0; y < buffer.height; ++y) {
        float *pixel = buffer.at(0, y);
        for (int x = 0; x < buffer.width; ++x, pixel += 3) {
            float r = pixel[0];
            float g = pixel[1];
            float b = pixel[2];
            if (step.hasCurve) {
                r = sampleLut(step.curve[0], r);
                g = sampleLut(step.curve[1], g);
                b = sampleLut(step.curve[2], b);
            }
            for (const ColourStagePrep &prep : prepared)
                applyColour(prep, r, g, b);
            pixel[0] = r;
            pixel[1] = g;
            pixel[2] = b;
        }
    }
}

void applySpatialStage(EffectRgbBuffer &buffer, const Stage &stage, int longSide)
{
    const double scale = EffectEngine::referenceScale(longSide);
    switch (stage.id) {
    case StageId::Blur: {
        if (stage.radius <= 0.0)
            break;
        const EffectRgbBuffer sharp = buffer;
        const int radius = std::max(1, int(std::lround(stage.radius * scale)));
        boxBlur(buffer, radius, stage.passes);
        applyBlurMask(buffer, sharp, stage.mask);
        break;
    }
    case StageId::Unsharp: {
        if (stage.radius <= 0.0 || stage.amount <= 0.0)
            break;
        EffectRgbBuffer blurred = buffer;
        boxBlur(blurred, std::max(1, int(std::lround(stage.radius * scale))), 3);
        for (int y = 0; y < buffer.height; ++y) {
            for (int x = 0; x < buffer.width; ++x) {
                float *pixel = buffer.at(x, y);
                const float *soft = blurred.at(x, y);
                for (int c = 0; c < 3; ++c) {
                    const float delta = pixel[c] - soft[c];
                    if (std::abs(delta) > float(stage.threshold))
                        pixel[c] += delta * float(stage.amount);
                }
            }
        }
        break;
    }
    case StageId::LocalContrast: {
        if (stage.radius <= 0.0 || stage.amount <= 0.0)
            break;
        EffectRgbBuffer blurred = buffer;
        boxBlur(blurred, std::max(1, int(std::lround(stage.radius * scale))), 3);
        for (int y = 0; y < buffer.height; ++y) {
            for (int x = 0; x < buffer.width; ++x) {
                float *pixel = buffer.at(x, y);
                const float *soft = blurred.at(x, y);
                float delta = luma709(pixel[0], pixel[1], pixel[2])
                    - luma709(soft[0], soft[1], soft[2]);
                delta = std::clamp(delta, -0.5f, 0.5f) * float(stage.amount);
                pixel[0] += delta;
                pixel[1] += delta;
                pixel[2] += delta;
            }
        }
        break;
    }
    case StageId::Bloom:
        applyBloom(buffer, stage, scale);
        break;
    case StageId::SoftFocus: {
        if (stage.radius <= 0.0 || stage.amount <= 0.0)
            break;
        EffectRgbBuffer blurred = buffer;
        boxBlur(blurred, std::max(1, int(std::lround(stage.radius * scale))), 3);
        const float amount = float(stage.amount);
        for (int y = 0; y < buffer.height; ++y) {
            for (int x = 0; x < buffer.width; ++x) {
                float *pixel = buffer.at(x, y);
                const float *soft = blurred.at(x, y);
                for (int c = 0; c < 3; ++c) {
                    float blended = 0.0f;
                    if (stage.blend == BlendMode::SoftLight)
                        blended = softLight(pixel[c], soft[c]);
                    else if (stage.blend == BlendMode::Multiply)
                        blended = pixel[c] * soft[c];
                    else if (stage.blend == BlendMode::Overlay)
                        blended = overlay(pixel[c], soft[c]);
                    else
                        blended = 1.0f - (1.0f - pixel[c]) * (1.0f - soft[c]);
                    pixel[c] = pixel[c] + (blended - pixel[c]) * amount;
                }
            }
        }
        break;
    }
    case StageId::Grain:
        applyGrain(buffer, stage, scale);
        break;
    case StageId::Vignette:
        applyVignette(buffer, stage);
        break;
    case StageId::LightLeak:
        applyLightLeak(buffer, stage);
        break;
    case StageId::ExposureMask:
        applyExposureMask(buffer, stage);
        break;
    case StageId::Texture:
        applyTexture(buffer, stage, longSide);
        break;
    case StageId::Posterize:
        applyPosterizeDither(buffer, stage);
        break;
    case StageId::Lut3d: {
        int size = 0;
        const auto cube = effectCube(stage.lut, &size);
        if (cube)
            applyCube(buffer, cube.value(), size);
        break;
    }
    default:
        break;
    }
}

void applyCube(EffectRgbBuffer &buffer, const QVector<float> &cube, int size)
{
    if (size < 2 || cube.size() < qsizetype(size) * size * size * 3)
        return;
    const float maxIndex = float(size - 1);
    for (int y = 0; y < buffer.height; ++y) {
        for (int x = 0; x < buffer.width; ++x) {
            float *pixel = buffer.at(x, y);
            float coordinate[3];
            int base[3];
            float fraction[3];
            for (int c = 0; c < 3; ++c) {
                coordinate[c] = clamp01(pixel[c]) * maxIndex;
                base[c] = std::min(size - 2, int(coordinate[c]));
                fraction[c] = coordinate[c] - float(base[c]);
            }
            float result[3] = {0.0f, 0.0f, 0.0f};
            for (int dr = 0; dr <= 1; ++dr) {
                for (int dg = 0; dg <= 1; ++dg) {
                    for (int db = 0; db <= 1; ++db) {
                        const int r = std::min(size - 1, base[0] + dr);
                        const int g = std::min(size - 1, base[1] + dg);
                        const int b = std::min(size - 1, base[2] + db);
                        const float weight = (dr ? fraction[0] : 1.0f - fraction[0])
                            * (dg ? fraction[1] : 1.0f - fraction[1])
                            * (db ? fraction[2] : 1.0f - fraction[2]);
                        const qsizetype index =
                            ((qsizetype(b) * size + g) * size + r) * 3;
                        result[0] += cube[index] * weight;
                        result[1] += cube[index + 1] * weight;
                        result[2] += cube[index + 2] * weight;
                    }
                }
            }
            pixel[0] = result[0];
            pixel[1] = result[1];
            pixel[2] = result[2];
        }
    }
}

} // namespace effectstages

namespace {

struct TextureCache {
    QMutex mutex;
    QHash<QString, QImage> images;
};

TextureCache &textureCache()
{
    static TextureCache cache;
    return cache;
}

struct CubeCache {
    QMutex mutex;
    QHash<QString, QVector<float>> cubes;
};

CubeCache &cubeCache()
{
    static CubeCache cache;
    return cache;
}

} // namespace

Result<QImage> effectTexture(const QString &asset)
{
    ensureEffectResources();
    const QString name = asset.endsWith(QLatin1String(".png")) ? asset
                                                               : asset + QStringLiteral(".png");
    {
        QMutexLocker locker(&textureCache().mutex);
        const auto it = textureCache().images.constFind(name);
        if (it != textureCache().images.constEnd())
            return *it;
    }
    QImage image;
    if (!image.load(QStringLiteral(":/textures/") + name)) {
        qCritical("EffectEngine: texture asset '%s' is missing", qPrintable(asset));
        return error(QStringLiteral("effect-texture-missing"),
                     QStringLiteral("The texture '%1' is missing.").arg(asset));
    }
    {
        QMutexLocker locker(&textureCache().mutex);
        textureCache().images.insert(name, image);
    }
    return image;
}

Result<QVector<float>> effectCube(const QString &name, int *size)
{
    ensureEffectResources();
    QString resource = name;
    if (!resource.endsWith(QLatin1String(".cube")))
        resource += QStringLiteral(".cube");
    {
        QMutexLocker locker(&cubeCache().mutex);
        const auto it = cubeCache().cubes.constFind(resource);
        if (it != cubeCache().cubes.constEnd()) {
            if (size)
                *size = int(std::lround(std::cbrt(double(it.value().size() / 3))));
            return it.value();
        }
    }
    QFile file(QStringLiteral(":/luts/") + resource);
    if (!file.open(QIODevice::ReadOnly)) {
        return error(QStringLiteral("effect-cube-missing"),
                     QStringLiteral("The colour cube '%1' is missing.").arg(name));
    }
    QTextStream stream(&file);
    int cubeSize = 0;
    QVector<float> values;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1String("LUT_3D_SIZE"), Qt::CaseInsensitive)) {
            cubeSize = line.section(QLatin1Char(' '), 1, 1).toInt();
            continue;
        }
        if (line.startsWith(QLatin1String("TITLE"), Qt::CaseInsensitive)
            || line.startsWith(QLatin1String("DOMAIN_MIN"), Qt::CaseInsensitive)
            || line.startsWith(QLatin1String("DOMAIN_MAX"), Qt::CaseInsensitive)) {
            continue;
        }
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() != 3)
            continue;
        values.append(parts.at(0).toFloat());
        values.append(parts.at(1).toFloat());
        values.append(parts.at(2).toFloat());
    }
    if (cubeSize < 2 || cubeSize > 64
        || values.size() != qsizetype(cubeSize) * cubeSize * cubeSize * 3) {
        return error(QStringLiteral("effect-cube-invalid"),
                     QStringLiteral("The colour cube '%1' is malformed.").arg(name));
    }
    {
        QMutexLocker locker(&cubeCache().mutex);
        cubeCache().cubes.insert(resource, values);
    }
    if (size)
        *size = cubeSize;
    return values;
}

} // namespace pp
