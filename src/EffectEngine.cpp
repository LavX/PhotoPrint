#include "EffectEngine.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <thread>

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

void runBands(int height, int bandRows, const std::function<void(int, int)> &fn)
{
    if (height <= 0)
        return;
    const int bands = (height + bandRows - 1) / bandRows;
    const int threads = std::min({QThread::idealThreadCount(), 8, bands});
    if (threads <= 1) {
        fn(0, height);
        return;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            for (;;) {
                const int index = next.fetch_add(1);
                if (index >= bands)
                    break;
                const int first = index * bandRows;
                fn(first, std::min(height, first + bandRows));
            }
        });
    }
    for (std::thread &worker : workers)
        worker.join();
}

QImage resampleImageLanczos(const QImage &source, const QSize &size)
{
    if (source.isNull() || size.isEmpty())
        return {};
    if (source.size() == size)
        return source;

    const QImage input = source.convertToFormat(QImage::Format_ARGB32);
    const int sw = input.width();
    const int sh = input.height();
    const int tw = size.width();
    const int th = size.height();
    const QVector<Contributor> horizontal = contributors(sw, tw);
    const QVector<Contributor> vertical = contributors(sh, th);

    QImage out(tw, th, QImage::Format_ARGB32);
    if (out.isNull())
        return {};

    // Horizontal resampling is done for a band of output rows at a time. Only
    // the source rows that the band's vertical taps touch are materialised, so
    // peak memory is a few megabytes even at 8000 px, not a full-width float
    // copy of the image.
    constexpr int kBandRows = 64;
    const auto processBand = [&](int y0, int y1) {
        std::vector<float> middle;
        int sourceFirst = vertical.at(y0).start;
        int sourceLast =
            vertical.at(y0).start + int(vertical.at(y0).weights.size()) - 1;
        for (int y = y0 + 1; y < y1; ++y) {
            const Contributor &entry = vertical.at(y);
            sourceFirst = std::min(sourceFirst, entry.start);
            sourceLast = std::max(sourceLast, entry.start + int(entry.weights.size()) - 1);
        }
        sourceFirst = std::clamp(sourceFirst, 0, sh - 1);
        sourceLast = std::clamp(sourceLast, 0, sh - 1);
        const int rows = sourceLast - sourceFirst + 1;
        middle.assign(static_cast<size_t>(rows) * tw * 4, 0.0f);

        for (int sy = sourceFirst; sy <= sourceLast; ++sy) {
            const QRgb *srcLine = reinterpret_cast<const QRgb *>(input.constScanLine(sy));
            float *middleRow =
                middle.data() + static_cast<size_t>(sy - sourceFirst) * tw * 4;
            for (int x = 0; x < tw; ++x) {
                const Contributor &entry = horizontal.at(x);
                float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (size_t k = 0; k < entry.weights.size(); ++k) {
                    const int sx = std::clamp(entry.start + int(k), 0, sw - 1);
                    const float weight = entry.weights[k];
                    const QRgb pixel = srcLine[sx];
                    sum[0] += float(qRed(pixel)) * weight;
                    sum[1] += float(qGreen(pixel)) * weight;
                    sum[2] += float(qBlue(pixel)) * weight;
                    sum[3] += float(qAlpha(pixel)) * weight;
                }
                float *outPixel = middleRow + static_cast<size_t>(x) * 4;
                outPixel[0] = sum[0];
                outPixel[1] = sum[1];
                outPixel[2] = sum[2];
                outPixel[3] = sum[3];
            }
        }

        for (int y = y0; y < y1; ++y) {
            const Contributor &entry = vertical.at(y);
            QRgb *outLine = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < tw; ++x) {
                float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (size_t k = 0; k < entry.weights.size(); ++k) {
                    const int sy = std::clamp(entry.start + int(k), sourceFirst, sourceLast);
                    const float *pixel =
                        middle.data()
                        + (static_cast<size_t>(sy - sourceFirst) * tw + x) * 4;
                    const float weight = entry.weights[k];
                    sum[0] += pixel[0] * weight;
                    sum[1] += pixel[1] * weight;
                    sum[2] += pixel[2] * weight;
                    sum[3] += pixel[3] * weight;
                }
                outLine[x] = qRgba(int(std::clamp(sum[0], 0.0f, 255.0f) + 0.5f),
                                   int(std::clamp(sum[1], 0.0f, 255.0f) + 0.5f),
                                   int(std::clamp(sum[2], 0.0f, 255.0f) + 0.5f),
                                   int(std::clamp(sum[3], 0.0f, 255.0f) + 0.5f));
            }
        }
    };
    if (qint64(tw) * th >= 2'000'000)
        runBands(th, kBandRows, processBand);
    else
        processBand(0, th);
    return out;
}

void blendInPlace(QImage &effect, const QImage &original, int strength)
{
    if (strength >= 100 || effect.isNull() || original.size() != effect.size())
        return;
    if (effect.format() != QImage::Format_ARGB32)
        effect = effect.convertToFormat(QImage::Format_ARGB32);
    effect.detach();
    for (int y = 0; y < effect.height(); ++y) {
        const QRgb *base = reinterpret_cast<const QRgb *>(original.constScanLine(y));
        QRgb *line = reinterpret_cast<QRgb *>(effect.scanLine(y));
        for (int x = 0; x < effect.width(); ++x) {
            const auto mix = [strength](int from, int to) {
                return (from * (100 - strength) + to * strength + 50) / 100;
            };
            line[x] = qRgba(mix(qRed(base[x]), qRed(line[x])),
                            mix(qGreen(base[x]), qGreen(line[x])),
                            mix(qBlue(base[x]), qBlue(line[x])), qAlpha(base[x]));
        }
    }
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

Result<void> runFinishSteps(QImage &image, const QList<EffectPlanStep> &steps, int longSide,
                            std::stop_token token)
{
    for (const EffectPlanStep &step : steps) {
        if (token.stop_requested())
            return error(QStringLiteral("cancelled"),
                         QStringLiteral("The effect render was cancelled."));
        effectstages::applyFinishStep(image, step.spatial, longSide);
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
    // Small previews need the same *relative* radius as the print. The old
    // 0.25 floor made a 160 px bloom 2.5-3.75x too broad; 0.02 only guards
    // against zero/negative sizes.
    return std::clamp(double(std::max(1, longSide)) / double(kGradeMax), 0.02, 4.0);
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
    bool segmentHasColour = false;
    bool seenFinish = false;

    const auto finishAllowed = [](const Stage &stage) {
        if (stage.id == StageId::Grain || stage.id == StageId::Vignette
            || stage.id == StageId::LightLeak || stage.id == StageId::ExposureMask)
            return true;
        // Only a dithered posterize is a finish decision; the plain quantise
        // stays on the grade raster.
        return stage.id == StageId::Posterize && stage.dither;
    };
    for (const Stage &stage : recipe.stages) {
        if (stage.finish && !finishAllowed(stage)) {
            return error(QStringLiteral("effect-plan"),
                         QStringLiteral("Effect '%1' flags '%2' as a finish stage, but that "
                                        "stage does not run on the 8-bit finish raster.")
                             .arg(recipe.identity(), stage.idName));
        }
    }

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
        point.toneStages = toneStages;
        if (!toneStages.isEmpty()) {
            point.hasCurve = true;
            point.curve = curveLutFor(toneStages);
        }
        plan.steps.append(point);
        plan.pointSteps += 1;
        point = EffectPlanStep{};
        toneStages.clear();
        segmentHasColour = false;
    };

    for (const Stage &stage : recipe.stages) {
        if (stage.finish) {
            seenFinish = true;
        } else if (seenFinish) {
            return error(QStringLiteral("effect-plan"),
                         QStringLiteral("Effect '%1' has a non-finish stage after a finish stage.")
                             .arg(recipe.identity()));
        }

        if (stage.id == StageId::Posterize) {
            // A LUT interpolates between its 1024 entries, which smears a
            // posterize staircase. Run the quantise as its own point pass;
            // a dithered posterize is additionally a finish stage.
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
            // Phase A runs first in a segment, then Phase B. An A stage after
            // a B stage must start a new segment or the LUT would hoist it
            // ahead of the colour stages and change the look.
            if (segmentHasColour)
                flush();
            toneStages.append(stage);
            break;
        case StagePhase::PointColour:
            if (stage.finish)
                return error(QStringLiteral("effect-plan"),
                             QStringLiteral("Effect '%1' flags the point stage '%2' as finish.")
                                 .arg(recipe.identity(), stage.idName));
            point.colourStages.append(stage);
            segmentHasColour = true;
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
                                    std::stop_token token, EffectRenderStats *stats) const
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
    const qint64 targetPixels = static_cast<qint64>(target.width()) * target.height();
    if (targetPixels > kMaxOutputPixels) {
        return error(QStringLiteral("effect-size"),
                     QStringLiteral("The requested effect raster is %1 megapixels; the engine "
                                    "supports up to %2. Lower the print resolution.")
                         .arg(double(targetPixels) / 1'000'000.0, 0, 'f', 1)
                         .arg(kMaxOutputPixels / 1'000'000));
    }

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
            blendInPlace(result, scaled, strength);
        result.setColorSpace(source.colorSpace());
        if (sourceFormat == QImage::Format_RGB32)
            result = result.convertToFormat(QImage::Format_RGB32);
        return result;
    }

    // Print path: grade and spatial work at the 2400 px bound in float, then
    // upscale the 8-bit result to the target and run the finish stages in
    // place there. No full-resolution float buffer is ever allocated.
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

    QImage gradedImage = toImage(buffer, gradeImage);
    if (gradedImage.isNull())
        return error(QStringLiteral("effect-output"), QStringLiteral("The frame cannot be built."));

    if (stats) {
        stats->gradeSize = gradeSize;
        stats->finishSize = QSize();
        stats->finishStages = false;
    }

    QImage flattened = std::move(gradedImage);
    if (!finishSteps.isEmpty()) {
        flattened = resampleImageLanczos(flattened, finishSize);
        const auto finished = runFinishSteps(flattened, finishSteps, finishLong, token);
        if (!finished)
            return finished.error();
        if (stats) {
            stats->finishSize = finishSize;
            stats->finishStages = true;
        }
    }

    QImage result = std::move(flattened);
    if (result.size() != target)
        result = resampleImageLanczos(result, target);

    if (strength < 100) {
        const QImage base = resampleImageLanczos(source, target);
        blendInPlace(result, base, strength);
    }
    result.setColorSpace(source.colorSpace());
    if (sourceFormat == QImage::Format_RGB32)
        result = result.convertToFormat(QImage::Format_RGB32);
    return result;
}

} // namespace pp
