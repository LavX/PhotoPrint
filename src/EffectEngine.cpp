#include "EffectEngine.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

QString number(double value)
{
    // A compact, stable spelling for cache keys and digests.
    return QString::number(value, 'g', 12);
}

QString toneDigest(const QList<Stage> &stages)
{
    QStringList parts;
    for (const Stage &stage : stages) {
        parts << stage.idName << number(stage.temperature) << number(stage.tint)
              << number(stage.ev) << number(stage.gamma) << number(stage.amount)
              << number(stage.pivot) << number(stage.black) << number(stage.white)
              << number(stage.lift) << number(stage.toe) << number(stage.roll)
              << QString::number(stage.levels);
        for (const QPointF &point : stage.curveMaster)
            parts << QStringLiteral("m%1,%2").arg(number(point.x()), number(point.y()));
        for (const QPointF &point : stage.curveR)
            parts << QStringLiteral("r%1,%2").arg(number(point.x()), number(point.y()));
        for (const QPointF &point : stage.curveG)
            parts << QStringLiteral("g%1,%2").arg(number(point.x()), number(point.y()));
        for (const QPointF &point : stage.curveB)
            parts << QStringLiteral("b%1,%2").arg(number(point.x()), number(point.y()));
    }
    return parts.join(QLatin1Char(';'));
}

struct LutCache {
    QMutex mutex;
    QHash<QString, EffectCurveLut> entries;
};

LutCache &lutCache()
{
    static LutCache cache;
    return cache;
}

EffectCurveLut curveLutFor(const QList<Stage> &toneStages)
{
    const QString key = toneDigest(toneStages);
    {
        QMutexLocker locker(&lutCache().mutex);
        const auto it = lutCache().entries.constFind(key);
        if (it != lutCache().entries.constEnd())
            return it.value();
    }
    EffectCurveLut lut;
    effectstages::bakeCurveLut(toneStages, lut);
    QMutexLocker locker(&lutCache().mutex);
    if (lutCache().entries.size() > 256)
        lutCache().entries.clear();
    lutCache().entries.insert(key, lut);
    return lut;
}

EffectRgbBuffer toBuffer(const QImage &image)
{
    EffectRgbBuffer buffer;
    buffer.resize(image.width(), image.height());
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        float *out = buffer.at(0, y);
        for (int x = 0; x < image.width(); ++x, out += 3) {
            out[0] = qRed(line[x]) / 255.0f;
            out[1] = qGreen(line[x]) / 255.0f;
            out[2] = qBlue(line[x]) / 255.0f;
        }
    }
    return buffer;
}

QImage toImage(const EffectRgbBuffer &buffer, const QImage &alpha)
{
    QImage out(buffer.width, buffer.height, QImage::Format_ARGB32);
    if (out.isNull())
        return {};
    const QImage alphaImage = alpha.size() == out.size()
        ? alpha.convertToFormat(QImage::Format_ARGB32)
        : alpha.scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < buffer.height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        const float *in = buffer.at(0, y);
        const QRgb *alphaLine = reinterpret_cast<const QRgb *>(alphaImage.constScanLine(y));
        for (int x = 0; x < buffer.width; ++x, in += 3) {
            const int r = int(std::clamp(in[0], 0.0f, 1.0f) * 255.0f + 0.5f);
            const int g = int(std::clamp(in[1], 0.0f, 1.0f) * 255.0f + 0.5f);
            const int b = int(std::clamp(in[2], 0.0f, 1.0f) * 255.0f + 0.5f);
            line[x] = qRgba(r, g, b, qAlpha(alphaLine[x]));
        }
    }
    return out;
}

struct Contributor {
    int start = 0;
    std::vector<float> weights;
};

double sinc(double x)
{
    if (std::abs(x) < 1e-9)
        return 1.0;
    const double pix = M_PI * x;
    return std::sin(pix) / pix;
}

double lanczosKernel(double x, double a)
{
    if (x <= -a || x >= a)
        return 0.0;
    return sinc(x) * sinc(x / a);
}

QVector<Contributor> contributors(int sourceSize, int targetSize)
{
    QVector<Contributor> table(targetSize);
    const double scale = double(targetSize) / double(std::max(1, sourceSize));
    const double filterScale = std::max(1.0, 1.0 / scale);
    const double support = 3.0 * filterScale;
    for (int i = 0; i < targetSize; ++i) {
        const double centre = (i + 0.5) / scale - 0.5;
        const int first = int(std::ceil(centre - support));
        const int last = int(std::floor(centre + support));
        Contributor entry;
        entry.start = first;
        double sum = 0.0;
        for (int j = first; j <= last; ++j) {
            const double weight = lanczosKernel((j - centre) / filterScale, 3.0);
            entry.weights.push_back(float(weight));
            sum += weight;
        }
        if (sum != 0.0) {
            for (float &weight : entry.weights)
                weight = float(weight / sum);
        }
        table[i] = std::move(entry);
    }
    return table;
}

void resamplePlanes(const float *source, int sw, int sh, int sourceChannels, float *target,
                    int tw, int th, int targetChannels, int channels)
{
    const QVector<Contributor> horizontal = contributors(sw, tw);
    const QVector<Contributor> vertical = contributors(sh, th);
    std::vector<float> middle(static_cast<size_t>(tw) * sh * channels, 0.0f);

    for (int y = 0; y < sh; ++y) {
        const float *src = source + static_cast<size_t>(y) * sw * sourceChannels;
        float *row = middle.data() + static_cast<size_t>(y) * tw * channels;
        for (int x = 0; x < tw; ++x) {
            const Contributor &entry = horizontal.at(x);
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (size_t k = 0; k < entry.weights.size(); ++k) {
                const int sx = std::clamp(entry.start + int(k), 0, sw - 1);
                const float weight = entry.weights[k];
                const float *pixel = src + static_cast<size_t>(sx) * sourceChannels;
                for (int c = 0; c < channels; ++c)
                    sum[c] += pixel[c] * weight;
            }
            float *out = row + static_cast<size_t>(x) * channels;
            for (int c = 0; c < channels; ++c)
                out[c] = sum[c];
        }
    }

    for (int y = 0; y < th; ++y) {
        const Contributor &entry = vertical.at(y);
        float *row = target + static_cast<size_t>(y) * tw * targetChannels;
        for (int x = 0; x < tw; ++x) {
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (size_t k = 0; k < entry.weights.size(); ++k) {
                const int sy = std::clamp(entry.start + int(k), 0, sh - 1);
                const float *pixel =
                    middle.data() + (static_cast<size_t>(sy) * tw + x) * channels;
                const float weight = entry.weights[k];
                for (int c = 0; c < channels; ++c)
                    sum[c] += pixel[c] * weight;
            }
            float *out = row + static_cast<size_t>(x) * targetChannels;
            for (int c = 0; c < channels; ++c)
                out[c] = sum[c];
        }
    }
}

EffectRgbBuffer resampleBuffer(const EffectRgbBuffer &source, int width, int height)
{
    EffectRgbBuffer out;
    out.resize(width, height);
    if (source.width == width && source.height == height) {
        out = source;
        return out;
    }
    resamplePlanes(source.pixels.data(), source.width, source.height, 3, out.pixels.data(), width,
                   height, 3, 3);
    return out;
}

QImage resampleImageLanczos(const QImage &source, const QSize &size)
{
    if (source.size() == size)
        return source;
    const QImage input = source.convertToFormat(QImage::Format_ARGB32);
    std::vector<float> pixels(static_cast<size_t>(input.width()) * input.height() * 4);
    for (int y = 0; y < input.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(input.constScanLine(y));
        float *out = pixels.data() + static_cast<size_t>(y) * input.width() * 4;
        for (int x = 0; x < input.width(); ++x, out += 4) {
            out[0] = qRed(line[x]);
            out[1] = qGreen(line[x]);
            out[2] = qBlue(line[x]);
            out[3] = qAlpha(line[x]);
        }
    }
    std::vector<float> scaled(static_cast<size_t>(size.width()) * size.height() * 4);
    resamplePlanes(pixels.data(), input.width(), input.height(), 4, scaled.data(), size.width(),
                   size.height(), 4, 4);
    QImage out(size, QImage::Format_ARGB32);
    if (out.isNull())
        return {};
    for (int y = 0; y < size.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        const float *in = scaled.data() + static_cast<size_t>(y) * size.width() * 4;
        for (int x = 0; x < size.width(); ++x, in += 4) {
            line[x] = qRgba(int(std::clamp(in[0], 0.0f, 255.0f) + 0.5f),
                            int(std::clamp(in[1], 0.0f, 255.0f) + 0.5f),
                            int(std::clamp(in[2], 0.0f, 255.0f) + 0.5f),
                            int(std::clamp(in[3], 0.0f, 255.0f) + 0.5f));
        }
    }
    return out;
}

QImage blend(const QImage &original, const QImage &effect, int strength)
{
    if (strength >= 100)
        return effect;
    QImage out(effect.size(), QImage::Format_ARGB32);
    if (out.isNull())
        return {};
    for (int y = 0; y < effect.height(); ++y) {
        const QRgb *base = reinterpret_cast<const QRgb *>(original.constScanLine(y));
        const QRgb *graded = reinterpret_cast<const QRgb *>(effect.constScanLine(y));
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < effect.width(); ++x) {
            const auto mix = [strength](int from, int to) {
                return (from * (100 - strength) + to * strength + 50) / 100;
            };
            line[x] = qRgba(mix(qRed(base[x]), qRed(graded[x])),
                            mix(qGreen(base[x]), qGreen(graded[x])),
                            mix(qBlue(base[x]), qBlue(graded[x])), qAlpha(base[x]));
        }
    }
    return out;
}

bool stepIsFinish(const EffectPlanStep &step)
{
    return !step.point && step.spatial.finish;
}

Result<void> runSteps(EffectRgbBuffer &buffer, const QList<EffectPlanStep> &steps, int longSide,
                      std::stop_token token)
{
    for (const EffectPlanStep &step : steps) {
        if (token.stop_requested())
            return error(QStringLiteral("cancelled"),
                         QStringLiteral("The effect render was cancelled."));
        if (step.point)
            effectstages::applyPointStep(buffer, step);
        else
            effectstages::applySpatialStage(buffer, step.spatial, longSide);
    }
    return {};
}

} // namespace

EffectEngine &EffectEngine::instance()
{
    static EffectEngine engine;
    return engine;
}

int EffectEngine::gradeLongSide(int targetLongSide)
{
    return std::clamp(targetLongSide, 1, kGradeMax);
}

double EffectEngine::referenceScale(int longSide)
{
    return std::clamp(double(std::max(1, longSide)) / double(kGradeMax), 0.25, 4.0);
}

QSize EffectEngine::sizeForLongSide(const QSize &size, int longSide)
{
    if (size.isEmpty() || size.width() <= 0 || size.height() <= 0)
        return {};
    const int longest = std::max(size.width(), size.height());
    const double factor = double(std::max(1, longSide)) / double(longest);
    return QSize(std::max(1, int(std::lround(size.width() * factor))),
                 std::max(1, int(std::lround(size.height() * factor))));
}

Result<EffectPlan> EffectEngine::compile(const EffectRecipe &recipe)
{
    // A missing material is a build error, not a silent identity. Validating
    // here means render() fails with the resolver's code instead of producing
    // an untextured frame.
    for (const Stage &stage : recipe.stages) {
        if (stage.id == StageId::Texture) {
            const auto texture = effectTexture(stage.asset);
            if (!texture)
                return texture.error();
        } else if (stage.id == StageId::Lut3d) {
            int size = 0;
            const auto cube = effectCube(stage.lut, &size);
            if (!cube)
                return cube.error();
        }
    }

    EffectPlan plan;
    EffectPlanStep point;
    QList<Stage> toneStages;
    bool seenFinish = false;

    const auto appendSpatial = [&](const Stage &stage) {
        EffectPlanStep step;
        step.point = false;
        step.spatial = stage;
        plan.steps.append(step);
        plan.spatialSteps += 1;
        if (stage.finish)
            plan.hasFinishStages = true;
    };

    const auto flush = [&]() {
        if (toneStages.isEmpty() && point.colourStages.isEmpty())
            return;
        point.point = true;
        if (!toneStages.isEmpty()) {
            point.hasCurve = true;
            point.curve = curveLutFor(toneStages);
        }
        plan.steps.append(point);
        plan.pointSteps += 1;
        point = EffectPlanStep{};
        toneStages.clear();
    };

    for (const Stage &stage : recipe.stages) {
        if (stage.finish) {
            seenFinish = true;
        } else if (seenFinish) {
            return error(QStringLiteral("effect-plan"),
                         QStringLiteral("Effect '%1' has a non-finish stage after a finish stage.")
                             .arg(recipe.identity()));
        }

        if (stage.id == StageId::Posterize && stage.dither) {
            // Dither has to see the final raster, so it cannot fold into the
            // 1D curve LUT even though posterize itself is a point operation.
            flush();
            appendSpatial(stage);
            continue;
        }
        if (stage.id == StageId::Lut3d) {
            flush();
            appendSpatial(stage);
            continue;
        }

        switch (stagePhase(stage.id)) {
        case StagePhase::PointTone:
            if (stage.finish)
                return error(QStringLiteral("effect-plan"),
                             QStringLiteral("Effect '%1' flags the point stage '%2' as finish.")
                                 .arg(recipe.identity(), stage.idName));
            toneStages.append(stage);
            break;
        case StagePhase::PointColour:
            if (stage.finish)
                return error(QStringLiteral("effect-plan"),
                             QStringLiteral("Effect '%1' flags the point stage '%2' as finish.")
                                 .arg(recipe.identity(), stage.idName));
            point.colourStages.append(stage);
            break;
        case StagePhase::Spatial:
        case StagePhase::Light:
        case StagePhase::Wand:
            flush();
            appendSpatial(stage);
            break;
        }
    }
    flush();
    if (plan.steps.isEmpty())
        return error(QStringLiteral("effect-plan"),
                     QStringLiteral("Effect '%1' compiles to no steps.").arg(recipe.identity()));
    return plan;
}

Result<QImage> EffectEngine::render(const QImage &srgb, const EffectRecipe &recipe,
                                    int strengthPercent, const QSize &targetSize, bool printPath,
                                    std::stop_token token) const
{
    if (srgb.isNull() || srgb.width() <= 0 || srgb.height() <= 0)
        return error(QStringLiteral("effect-input"),
                     QStringLiteral("No pixels to grade."));
    const int strength = std::clamp(strengthPercent, 0, 100);
    if (strength == 0)
        return srgb;

    const QImage::Format sourceFormat = srgb.format();
    const QImage source = (sourceFormat == QImage::Format_RGB32
                           || sourceFormat == QImage::Format_ARGB32)
        ? srgb
        : srgb.convertToFormat(QImage::Format_ARGB32);
    if (source.isNull())
        return error(QStringLiteral("effect-input"), QStringLiteral("The frame cannot be read."));

    auto plan = compile(recipe);
    if (!plan)
        return plan.error();

    QSize target = targetSize.isValid() && !targetSize.isEmpty() ? targetSize : source.size();
    if (target.width() <= 0 || target.height() <= 0)
        target = source.size();
    if (static_cast<qint64>(target.width()) * target.height() > kMaxOutputPixels)
        return error(QStringLiteral("effect-size"),
                     QStringLiteral("The requested effect raster is too large."));

    if (!printPath) {
        const QImage scaled = resampleImageLanczos(source, target);
        EffectRgbBuffer buffer = toBuffer(scaled);
        const auto run = runSteps(buffer, plan.value().steps,
                                  std::max(target.width(), target.height()), token);
        if (!run)
            return run.error();
        QImage result = toImage(buffer, scaled);
        if (result.isNull())
            return error(QStringLiteral("effect-output"), QStringLiteral("The frame cannot be built."));
        if (strength < 100)
            result = blend(scaled, result, strength);
        result.setColorSpace(source.colorSpace());
        if (sourceFormat == QImage::Format_RGB32)
            result = result.convertToFormat(QImage::Format_RGB32);
        return result;
    }

    // Print path: grade at the reference bound, run the finish stages at the
    // target (or the 8000 px guard), then resample once to the placed raster.
    const int targetLong = std::max(target.width(), target.height());
    const int gradeLong = gradeLongSide(targetLong);
    const int finishLong = std::min(targetLong, kFinishMax);
    const QSize gradeSize = sizeForLongSide(target, gradeLong);
    const QSize finishSize = sizeForLongSide(target, finishLong);

    QList<EffectPlanStep> gradeSteps;
    QList<EffectPlanStep> finishSteps;
    for (const EffectPlanStep &step : plan.value().steps) {
        if (stepIsFinish(step))
            finishSteps.append(step);
        else
            gradeSteps.append(step);
    }

    const QImage gradeImage = resampleImageLanczos(source, gradeSize);
    EffectRgbBuffer buffer = toBuffer(gradeImage);
    const auto graded = runSteps(buffer, gradeSteps, gradeLong, token);
    if (!graded)
        return graded.error();

    if (!finishSteps.isEmpty()) {
        buffer = resampleBuffer(buffer, finishSize.width(), finishSize.height());
        const auto finished = runSteps(buffer, finishSteps, finishLong, token);
        if (!finished)
            return finished.error();
    }

    if (buffer.width != target.width() || buffer.height != target.height())
        buffer = resampleBuffer(buffer, target.width(), target.height());

    const QImage alpha = resampleImageLanczos(source, target);
    QImage result = toImage(buffer, alpha);
    if (result.isNull())
        return error(QStringLiteral("effect-output"), QStringLiteral("The frame cannot be built."));
    if (strength < 100)
        result = blend(alpha, result, strength);
    result.setColorSpace(source.colorSpace());
    if (sourceFormat == QImage::Format_RGB32)
        result = result.convertToFormat(QImage::Format_RGB32);
    return result;
}

} // namespace pp
