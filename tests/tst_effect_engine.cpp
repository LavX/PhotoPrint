#include "ColorPipeline.h"
#include "EffectCatalogue.h"
#include "EffectEngine.h"
#include "ImageSource.h"
#include "LayoutEngine.h"
#include "PageRenderer.h"
#include "PhotoFilters.h"
#include "ProjectStore.h"
#include "WandStages.h"

#include <QColor>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <cmath>
#include <thread>

using namespace pp;

namespace {

QImage sampleImage()
{
    QImage image(QStringLiteral(PHOTOPRINT_SOURCE_DIR "/resources/samples/effect-sample.jpg"));
    if (image.isNull()) {
        image = QImage(320, 240, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.setPixel(x, y,
                               qRgb((x * 255) / image.width(), (y * 255) / image.height(),
                                    128));
            }
        }
    }
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return image;
}

QImage scaledSample(const QSize &size)
{
    return sampleImage().scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QByteArray hashImage(const QImage &image)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(argb.constScanLine(y)),
                                    argb.width() * 4));
    }
    return hash.result();
}

double meanDifference(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return 1e9;
    double total = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *left = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *right = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            total += std::abs(qRed(left[x]) - qRed(right[x]));
            total += std::abs(qGreen(left[x]) - qGreen(right[x]));
            total += std::abs(qBlue(left[x]) - qBlue(right[x]));
        }
    }
    return total / (double(a.width()) * a.height() * 3.0);
}

/// The ten looks the engine was built around. They keep their own goldens and
/// their own distinctness check; the full catalogue is gated separately below.
const QStringList kSignatureKeys = {
    QStringLiteral("light.venice"),
    QStringLiteral("color.tealorange"),
    QStringLiteral("retro.fadedfilm"),
    QStringLiteral("light.nordic"),
    QStringLiteral("light.goldenhour"),
    QStringLiteral("glow.neonnight"),
    QStringLiteral("color.pastel"),
    QStringLiteral("mono.contrast"),
    QStringLiteral("print.sepia"),
    QStringLiteral("retro.crossprocess"),
};

/// sRGB (0..1) to CIELAB with a D65 white point. The gate uses it because a
/// per-channel difference is not perceptual: two looks can differ by 20 levels
/// of blue and look the same.
void toLab(double r, double g, double b, double *l, double *a, double *bStar)
{
    const auto decode = [](double value) {
        return value <= 0.04045 ? value / 12.92
                                : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const double rl = decode(r);
    const double gl = decode(g);
    const double bl = decode(b);
    const double x = (0.4124564 * rl + 0.3575761 * gl + 0.1804375 * bl) / 0.95047;
    const double y = 0.2126729 * rl + 0.7151522 * gl + 0.0721750 * bl;
    const double z = (0.0193339 * rl + 0.1191920 * gl + 0.9503041 * bl) / 1.08883;
    const auto f = [](double value) {
        return value > 0.008856 ? std::cbrt(value) : (7.787 * value + 16.0 / 116.0);
    };
    *l = 116.0 * f(y) - 16.0;
    *a = 500.0 * (f(x) - f(y));
    *bStar = 200.0 * (f(y) - f(z));
}

/// CIELAB samples for one frame, computed once: the catalogue gates compare
/// thousands of pairs, and converting to Lab inside every comparison is the
/// only thing that makes them slow.
struct LabFrame {
    int width = 0;
    int height = 0;
    std::vector<float> values; ///< 3 floats per pixel
};

LabFrame labFrame(const QImage &image)
{
    LabFrame frame;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    frame.width = argb.width();
    frame.height = argb.height();
    frame.values.resize(static_cast<size_t>(frame.width) * frame.height * 3);
    size_t out = 0;
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            double l = 0.0, a = 0.0, b = 0.0;
            toLab(qRed(line[x]) / 255.0, qGreen(line[x]) / 255.0, qBlue(line[x]) / 255.0,
                  &l, &a, &b);
            frame.values[out++] = float(l);
            frame.values[out++] = float(a);
            frame.values[out++] = float(b);
        }
    }
    return frame;
}

/// Mean CIELAB distance between two frames, sampled every @p stride pixels.
double meanLabDistance(const LabFrame &left, const LabFrame &right, int stride)
{
    if (left.width != right.width || left.height != right.height || left.width <= 0)
        return 1e9;
    double total = 0.0;
    long long count = 0;
    for (int y = 0; y < left.height; y += stride) {
        for (int x = 0; x < left.width; x += stride) {
            const size_t index = (static_cast<size_t>(y) * left.width + x) * 3;
            const double dl = double(left.values[index]) - right.values[index];
            const double da = double(left.values[index + 1]) - right.values[index + 1];
            const double db = double(left.values[index + 2]) - right.values[index + 2];
            total += std::sqrt(dl * dl + da * da + db * db);
            count += 1;
        }
    }
    return total / double(std::max<long long>(1, count));
}

double perceptualDistance(const QImage &left, const QImage &right)
{
    if (left.size() != right.size() || left.isNull() || right.isNull())
        return 1e9;
    return meanLabDistance(labFrame(left), labFrame(right), 1);
}

/// The documented within-category floor, in mean CIELAB distance across the
/// frame at the 240 px browser raster. The tightest shipped pair is
/// basic.neutral vs basic.warmbase at 2.19, so a floor of 2.0 has margin.
constexpr double kDistinctnessFloor = 2.0;

/// The documented cross-category floor. Two looks in different categories are
/// never offered as alternatives in one grid, so the floor is lower, but a
/// near-clone is still a defect.
constexpr double kCrossCategoryFloor = 1.8;
/// The absolute floor: nothing in the catalogue may be within this distance of
/// anything else, whatever the categories say.
constexpr double kCloneFloor = 1.0;

/// The one documented family allowed between kCloneFloor and
/// kCrossCategoryFloor. These looks are corrections, not looks: each changes a
/// neutral frame by one small thing (a cast, a lifted black, a little
/// sharpening, the finest grain), so on a frame that has no cast and no
/// softness they necessarily land within a couple of deltaE of the original, and
/// of each other. They live in different categories and are never shown as
/// alternatives for the same job. Every other cross-category pair must clear
/// kCrossCategoryFloor.
const QStringList kCorrectionFamily = {
    QStringLiteral("basic.neutral"),    QStringLiteral("basic.clean"),
    QStringLiteral("basic.coolbase"),   QStringLiteral("color.neutralize"),
    QStringLiteral("detail.clarity"),   QStringLiteral("grain.fine"),
};

/// Mean length of a run of equal grey values: how coarse a dot screen is.
double meanRunLength(const QImage &image)
{
    long long runs = 0;
    long long pixels = 0;
    for (int y = 0; y < image.height(); y += 7) {
        int run = 1;
        for (int x = 1; x < image.width(); ++x) {
            if (qGray(image.pixel(x, y)) == qGray(image.pixel(x - 1, y))) {
                run += 1;
            } else {
                runs += 1;
                pixels += run;
                run = 1;
            }
        }
        runs += 1;
        pixels += run;
    }
    return double(pixels) / double(std::max<long long>(1, runs));
}

/// High-frequency energy: the standard deviation of the difference between a
/// pixel and its 3 x 3 box mean. Grain and dither raise it, blur lowers it.
double highFrequencyEnergy(const QImage &image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    double sum = 0.0;
    double sumSquares = 0.0;
    int count = 0;
    for (int y = 1; y + 1 < argb.height(); ++y) {
        for (int x = 1; x + 1 < argb.width(); ++x) {
            int mean = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    mean += qGray(argb.pixel(x + dx, y + dy));
            mean /= 9;
            const double delta = double(qGray(argb.pixel(x, y)) - mean);
            sum += delta;
            sumSquares += delta * delta;
            count += 1;
        }
    }
    if (count == 0)
        return 0.0;
    const double average = sum / count;
    return std::sqrt(std::max(0.0, sumSquares / count - average * average));
}

SourceImage memorySource(const QImage &image)
{
    SourceImage source;
    source.path = QStringLiteral("memory");
    source.sha256 = QByteArrayLiteral("memory");
    source.pixels = image;
    source.sourceSpace = QColorSpace(QColorSpace::SRgb);
    source.hasEmbeddedProfile = true;
    source.profileValid = true;
    return source;
}

QByteArray catalogueJson(const QJsonArray &recipes)
{
    const QStringList categories = {QStringLiteral("basic"), QStringLiteral("mono"),
                                    QStringLiteral("retro"), QStringLiteral("color"),
                                    QStringLiteral("blur"), QStringLiteral("detail"),
                                    QStringLiteral("art"), QStringLiteral("warp"),
                                    QStringLiteral("light"), QStringLiteral("grain"),
                                    QStringLiteral("print"), QStringLiteral("glow")};
    QJsonArray categoryArray;
    for (const QString &key : categories)
        categoryArray.append(QJsonObject{{QStringLiteral("key"), key},
                                         {QStringLiteral("name"), key}});
    return QJsonDocument(QJsonObject{{QStringLiteral("categories"), categoryArray},
                                     {QStringLiteral("recipes"), recipes}})
        .toJson(QJsonDocument::Compact);
}

QJsonObject testRecipe(const QString &key, const QString &category, int version, double ev)
{
    return QJsonObject{
        {QStringLiteral("key"), key},
        {QStringLiteral("category"), category},
        {QStringLiteral("version"), version},
        {QStringLiteral("name"), QStringLiteral("Test")},
        {QStringLiteral("stages"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("exposure")},
                                {QStringLiteral("ev"), ev}}}}};
}

} // namespace

class TestEffectEngine : public QObject
{
    Q_OBJECT

private slots:
    void strengthZeroIsIdentity();
    void everyRecipeChangesPixels();
    void signatureLooksStayDistinct();
    void stageOrderIsPreserved();
    void posterizeIsNotFoldedIntoLut();
    void vignetteIsAspectNormalised();
    void finishStagesRunAtTarget();
    void largeFinishRasterWorks();
    void oversizedRasterFailsClearly();
    void printLayerKeepsSourceAspect();
    void printLayerSizeIsCapped();
    void previewBloomMatchesPrint();
    void preparedLinearHonoursVersion();
    void goldensArePinned();
    void rendersAreDeterministic();
    void compilesIntoSegments();
    void ditheredPosterizeIsSpatial();
    void printTargetSizes();
    void previewAndPrintAgreeForPointStages();
    void grainSurvivesThePrintPath();
    void versionPinningSelectsTheOldRecipe();
    void legacyKeysKeepTheOldPath();
    void unknownKeysFallBackToOriginal();
    void alphaIsPreserved();
    void effectVersionRoundTrips();
    void rendersOffTheGuiThread();
    void catalogueDistinctnessGate();
    void catalogueCrossCategoryGate();
    void catalogueRenderHashes();
    void renderCostStaysWithinBudget();
    void wandStagesSurviveThePrintPath();
    void halftoneDotPitchScalesWithFinishRaster();
    void halftoneScreenIsStableAcrossRasters();
    void mirrorReflectsTheLeftHalf();
    void alphaSurvivesAWandPass();
};

void TestEffectEngine::strengthZeroIsIdentity()
{
    const auto recipe =
        EffectCatalogue::instance().resolve(QStringLiteral("light.venice")).value();
    const QImage source = scaledSample(QSize(160, 120));
    const auto result =
        EffectEngine::instance().render(source, recipe, 0, source.size(), false);
    QVERIFY(result.ok());
    QCOMPARE(result.value(), source);
}

void TestEffectEngine::everyRecipeChangesPixels()
{
    const QImage source = scaledSample(QSize(160, 120));
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        const auto result =
            EffectEngine::instance().render(source, recipe, 100, source.size(), false);
        QVERIFY2(result.ok(), qPrintable(recipe.key));
        const double difference = meanDifference(source, result.value());
        QVERIFY2(difference > 0.5,
                 qPrintable(QStringLiteral("%1 changed by %2").arg(recipe.key).arg(difference)));
    }
}

void TestEffectEngine::signatureLooksStayDistinct()
{
    // The ten signature looks are the ones the engine was designed around, so
    // they are gated on their own: every pair must be visibly different and no
    // look may collapse the frame to a flat grey. The full 120-effect catalogue
    // is gated per category in catalogueDistinctnessGate.
    const QImage source = scaledSample(QSize(240, 180));
    QList<QImage> looks;
    for (const QString &key : kSignatureKeys) {
        const auto recipe = EffectCatalogue::instance().resolve(key);
        QVERIFY2(recipe.ok(), qPrintable(key));
        const auto result =
            EffectEngine::instance().render(source, recipe.value(), 100, source.size(), false);
        QVERIFY(result.ok());
        double luma = 0.0;
        for (int y = 0; y < result.value().height(); ++y)
            for (int x = 0; x < result.value().width(); ++x)
                luma += qGray(result.value().pixel(x, y));
        luma /= double(result.value().width()) * result.value().height();
        QVERIFY2(luma > 15.0 && luma < 245.0,
                 qPrintable(QStringLiteral("%1 mean luma %2").arg(key).arg(luma)));
        looks.append(result.value());
    }
    for (int i = 0; i < looks.size(); ++i) {
        for (int j = i + 1; j < looks.size(); ++j) {
            const double difference = meanDifference(looks.at(i), looks.at(j));
            QVERIFY2(difference > 0.8,
                     qPrintable(QStringLiteral("%1 vs %2 differ by %3")
                                    .arg(kSignatureKeys.at(i), kSignatureKeys.at(j))
                                    .arg(difference)));
        }
    }
}

void TestEffectEngine::stageOrderIsPreserved()
{
    const auto catalogueFor = [](const QJsonArray &stages) {
        return EffectCatalogue::fromJson(catalogueJson(QJsonArray{QJsonObject{
            {QStringLiteral("key"), QStringLiteral("color.order")},
            {QStringLiteral("category"), QStringLiteral("color")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("name"), QStringLiteral("Order")},
            {QStringLiteral("stages"), stages}}}));
    };
    const QJsonObject saturation{{QStringLiteral("id"), QStringLiteral("saturation")},
                                 {QStringLiteral("amount"), -100}};
    const QJsonObject exposure{{QStringLiteral("id"), QStringLiteral("exposure")},
                               {QStringLiteral("ev"), 1.0}};
    const auto colourThenTone = catalogueFor(QJsonArray{saturation, exposure});
    const auto toneThenColour = catalogueFor(QJsonArray{exposure, saturation});
    QVERIFY(colourThenTone.ok());
    QVERIFY(toneThenColour.ok());

    const QImage source = scaledSample(QSize(96, 72));
    const auto first = EffectEngine::instance().render(
        source, colourThenTone.value().resolve(QStringLiteral("color.order")).value(), 100,
        source.size(), false);
    const auto second = EffectEngine::instance().render(
        source, toneThenColour.value().resolve(QStringLiteral("color.order")).value(), 100,
        source.size(), false);
    QVERIFY(first.ok());
    QVERIFY(second.ok());
    QVERIFY(hashImage(first.value()) != hashImage(second.value()));

    const auto colourThenTonePlan = EffectEngine::compile(
        colourThenTone.value().resolve(QStringLiteral("color.order")).value());
    const auto toneThenColourPlan = EffectEngine::compile(
        toneThenColour.value().resolve(QStringLiteral("color.order")).value());
    QVERIFY(colourThenTonePlan.ok());
    QVERIFY(toneThenColourPlan.ok());
    QCOMPARE(colourThenTonePlan.value().steps.size(), 2);
    QCOMPARE(colourThenTonePlan.value().steps.at(0).point, true);
    QVERIFY(colourThenTonePlan.value().steps.at(0).toneStages.isEmpty());
    QCOMPARE(colourThenTonePlan.value().steps.at(0).colourStages.first().idName,
             QStringLiteral("saturation"));
    QCOMPARE(colourThenTonePlan.value().steps.at(1).toneStages.first().idName,
             QStringLiteral("exposure"));
    QCOMPARE(toneThenColourPlan.value().steps.size(), 1);
    QCOMPARE(toneThenColourPlan.value().steps.first().toneStages.first().idName,
             QStringLiteral("exposure"));
    QCOMPARE(toneThenColourPlan.value().steps.first().colourStages.first().idName,
             QStringLiteral("saturation"));

    // Every shipped recipe executes exactly the JSON order. A point step
    // records its folded tone stages and its fused colour stages; flattening
    // those with the spatial stages must reproduce the recipe list.
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        QStringList expected;
        for (const Stage &stage : recipe.stages)
            expected << stage.idName;
        const auto plan = EffectEngine::compile(recipe);
        QVERIFY(plan.ok());
        QStringList actual;
        for (const EffectPlanStep &step : plan.value().steps) {
            if (step.point) {
                for (const Stage &stage : step.toneStages)
                    actual << stage.idName;
                for (const Stage &stage : step.colourStages)
                    actual << stage.idName;
            } else {
                actual << step.spatial.idName;
            }
        }
        QCOMPARE(actual, expected);
    }
}

void TestEffectEngine::posterizeIsNotFoldedIntoLut()
{
    const QJsonArray recipes{QJsonObject{
        {QStringLiteral("key"), QStringLiteral("art.poster")},
        {QStringLiteral("category"), QStringLiteral("art")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("name"), QStringLiteral("Poster")},
        {QStringLiteral("stages"),
         QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("posterize")},
                         {QStringLiteral("levels"), 5}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("exposure")},
                         {QStringLiteral("ev"), 0.1}}}},
    }};
    const auto catalogue = EffectCatalogue::fromJson(catalogueJson(recipes));
    QVERIFY(catalogue.ok());
    const auto recipe = catalogue.value().resolve(QStringLiteral("art.poster")).value();
    const auto plan = EffectEngine::compile(recipe);
    QVERIFY(plan.ok());
    // The quantise is its own pass; the exposure after it is a separate point
    // step, so the LUT interpolation cannot smear the posterize staircase.
    QCOMPARE(plan.value().steps.first().point, false);
    QCOMPARE(plan.value().steps.first().spatial.id, StageId::Posterize);
    QCOMPARE(plan.value().pointSteps, 1);
}

void TestEffectEngine::vignetteIsAspectNormalised()
{
    const QJsonArray recipes{QJsonObject{
        {QStringLiteral("key"), QStringLiteral("light.vignette")},
        {QStringLiteral("category"), QStringLiteral("light")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("name"), QStringLiteral("Vignette")},
        {QStringLiteral("stages"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("vignette")},
                                {QStringLiteral("amount"), 0.4},
                                {QStringLiteral("midpoint"), 0.5},
                                {QStringLiteral("feather"), 0.4}}}},
    }};
    const auto catalogue = EffectCatalogue::fromJson(catalogueJson(recipes));
    QVERIFY(catalogue.ok());
    const auto recipe = catalogue.value().resolve(QStringLiteral("light.vignette")).value();

    int firstCorner = -1;
    for (const QSize &size : {QSize(400, 300), QSize(400, 267), QSize(400, 225)}) {
        QImage frame(size, QImage::Format_RGB32);
        frame.fill(QColor(128, 128, 128));
        const auto result = EffectEngine::instance().render(frame, recipe, 100, size, false);
        QVERIFY(result.ok());
        QVERIFY(std::abs(qGray(result.value().pixel(size.width() / 2, size.height() / 2))
                         - 128)
                <= 1);
        const int corner = qGray(result.value().pixel(0, 0));
        const double darkening = 1.0 - double(corner) / 128.0;
        QVERIFY2(std::abs(darkening - 0.4) < 0.02,
                 qPrintable(QStringLiteral("%1x%2 corner darkening %3")
                                .arg(size.width())
                                .arg(size.height())
                                .arg(darkening)));
        if (firstCorner < 0)
            firstCorner = corner;
        else
            QVERIFY(std::abs(corner - firstCorner) <= 2);
    }
}

void TestEffectEngine::finishStagesRunAtTarget()
{
    const QJsonArray recipes{QJsonObject{
        {QStringLiteral("key"), QStringLiteral("grain.test")},
        {QStringLiteral("category"), QStringLiteral("grain")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("name"), QStringLiteral("Grain Test")},
        {QStringLiteral("stages"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("grain")},
                                {QStringLiteral("finish"), true},
                                {QStringLiteral("amount"), 0.08},
                                {QStringLiteral("size"), 1},
                                {QStringLiteral("seed"), 5}}}}}};
    const auto catalogue = EffectCatalogue::fromJson(catalogueJson(recipes));
    QVERIFY(catalogue.ok());
    const auto recipe = catalogue.value().resolve(QStringLiteral("grain.test")).value();
    const QImage source = scaledSample(QSize(1000, 750));

    EffectRenderStats stats;
    const auto result =
        EffectEngine::instance().render(source, recipe, 100, QSize(3200, 2400), true, {}, &stats);
    QVERIFY(result.ok());
    QCOMPARE(result.value().size(), QSize(3200, 2400));
    QCOMPARE(stats.gradeSize, QSize(2400, 1800));
    QCOMPARE(stats.finishSize, QSize(3200, 2400));
    QVERIFY(stats.finishStages);

    // Every shipped grain stage is a finish stage and every recipe carrying
    // grain declares it, so print grain is generated at the target raster.
    for (const EffectRecipe &shipped : EffectCatalogue::instance().recipes()) {
        bool hasGrain = false;
        for (const Stage &stage : shipped.stages) {
            if (stage.id == StageId::Grain) {
                hasGrain = true;
                QVERIFY2(stage.finish, qPrintable(shipped.key));
            }
        }
        const auto plan = EffectEngine::compile(shipped);
        QVERIFY(plan.ok());
        if (hasGrain)
            QVERIFY2(plan.value().hasFinishStages, qPrintable(shipped.key));
    }
}

void TestEffectEngine::largeFinishRasterWorks()
{
    const auto recipe =
        EffectCatalogue::instance().resolve(QStringLiteral("print.sepia")).value();
    const QImage source = scaledSample(QSize(1200, 900));
    EffectRenderStats stats;
    const auto result =
        EffectEngine::instance().render(source, recipe, 100, QSize(8000, 6000), true, {}, &stats);
    QVERIFY(result.ok());
    QCOMPARE(result.value().size(), QSize(8000, 6000));
    QCOMPARE(stats.gradeSize, QSize(2400, 1800));
    QCOMPARE(stats.finishSize, QSize(8000, 6000));
    QVERIFY(stats.finishStages);
    QVERIFY(highFrequencyEnergy(result.value()) > 0.5);
}

void TestEffectEngine::oversizedRasterFailsClearly()
{
    const auto recipe =
        EffectCatalogue::instance().resolve(QStringLiteral("light.venice")).value();
    const QImage source = scaledSample(QSize(320, 240));
    const auto result =
        EffectEngine::instance().render(source, recipe, 100, QSize(20000, 15000), true);
    QVERIFY(!result.ok());
    QCOMPARE(result.error().code, QStringLiteral("effect-size"));
    QVERIFY(result.error().message.contains(QStringLiteral("megapixels")));
}

void TestEffectEngine::printLayerKeepsSourceAspect()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = QDir(directory.path()).filePath(QStringLiteral("aspect.png"));
    QImage source(400, 300, QImage::Format_RGB32);
    source.fill(QColor(90, 90, 90));
    QVERIFY(source.save(path));

    Page page;
    page.paper.name = QStringLiteral("100 x 150 mm");
    page.paper.physicalWidthMm = 100.0;
    page.paper.physicalHeightMm = 150.0;
    Frame frame;
    frame.id = QStringLiteral("aspect-frame");
    frame.sourcePath = path;
    frame.cx = 50.0;
    frame.cy = 75.0;
    frame.width = 100.0;
    frame.height = 150.0;
    frame.autoPlaced = false;
    frame.filterStrength = 100;
    page.frames.append(frame);
    page.frames.first().filter = QStringLiteral("light.venice");
    page.frames.first().fit = FitMode::Full;
    DocumentSnapshot document;
    document.pages.append(page);

    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    const auto render = [&](const QString &filter, FitMode fit) {
        document.pages[0].frames[0].filter = filter;
        document.pages[0].frames[0].fit = fit;
        store.clear();
        const auto rendered = renderer.renderPage(document, 0, 30.0, {}, options);
        if (!rendered)
            return QImage();
        return rendered.value();
    };
    const auto contentRows = [](const QImage &page, int x) {
        int first = -1;
        int last = -1;
        for (int y = 0; y < page.height(); ++y) {
            if (qGray(page.pixel(x, y)) < 250) {
                if (first < 0)
                    first = y;
                last = y;
            }
        }
        return first < 0 ? 0 : last - first + 1;
    };

    const QImage catalogueFull =
        render(QStringLiteral("light.venice"), FitMode::Full);
    const QImage legacyFull = render(QStringLiteral("original"), FitMode::Full);
    QVERIFY(!catalogueFull.isNull());
    QVERIFY(!legacyFull.isNull());
    const int catalogueRows = contentRows(catalogueFull, catalogueFull.width() / 2);
    const int legacyRows = contentRows(legacyFull, legacyFull.width() / 2);
    QVERIFY2(std::abs(catalogueRows - legacyRows) <= 2,
             qPrintable(QStringLiteral("catalogue %1 rows, legacy %2")
                            .arg(catalogueRows)
                            .arg(legacyRows)));
    // 4:3 in a 100 x 150 mm frame with Full is 100 x 75 mm, not the full
    // 150 mm frame height.
    QVERIFY(catalogueRows >= 86 && catalogueRows <= 92);

    const QImage catalogueFill =
        render(QStringLiteral("light.venice"), FitMode::Fill);
    const QImage legacyFill = render(QStringLiteral("original"), FitMode::Fill);
    const int catalogueFillRows = contentRows(catalogueFill, catalogueFill.width() / 2);
    const int legacyFillRows = contentRows(legacyFill, legacyFill.width() / 2);
    QVERIFY(std::abs(catalogueFillRows - legacyFillRows) <= 2);
    QVERIFY(catalogueFillRows >= 174);
}

void TestEffectEngine::printLayerSizeIsCapped()
{
    Frame frame;
    frame.cx = 100.0;
    frame.cy = 140.0;
    frame.width = 200.0;
    frame.height = 280.0;
    frame.fit = FitMode::Full;
    const QSize sourceSize(4000, 3000); // 4:3, so 200 x 150 mm at Full
    const QSize layer = PageRenderer::effectLayerSize(frame, sourceSize, 1200.0);
    QCOMPARE(layer, QSize(8000, 6000));
    QVERIFY(qint64(layer.width()) * layer.height() * 4 < 200LL * 1024 * 1024);

    const QSize uncapped(std::lround(200.0 / 25.4 * 1200.0),
                         std::lround(150.0 / 25.4 * 1200.0));
    QVERIFY(qint64(uncapped.width()) * uncapped.height() > qint64(layer.width()) * layer.height());

    const auto recipe =
        EffectCatalogue::instance().resolve(QStringLiteral("light.venice")).value();
    const QImage source = scaledSample(QSize(1200, 900));
    EffectRenderStats stats;
    const auto prepared =
        EffectEngine::instance().render(source, recipe, 100, layer, true, {}, &stats);
    QVERIFY(prepared.ok());
    QCOMPARE(prepared.value().size(), layer);
    QCOMPARE(stats.finishSize, layer);
}

void TestEffectEngine::previewBloomMatchesPrint()
{
    const QJsonArray recipes{QJsonObject{
        {QStringLiteral("key"), QStringLiteral("glow.bloomtest")},
        {QStringLiteral("category"), QStringLiteral("glow")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("name"), QStringLiteral("Bloom Test")},
        {QStringLiteral("stages"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("bloom")},
                                {QStringLiteral("threshold"), 0.5},
                                {QStringLiteral("radius"), 44},
                                {QStringLiteral("intensity"), 0.5},
                                {QStringLiteral("tint"), QStringLiteral("#ffffff")}}}}}};
    const auto catalogue = EffectCatalogue::fromJson(catalogueJson(recipes));
    QVERIFY(catalogue.ok());
    const auto recipe = catalogue.value().resolve(QStringLiteral("glow.bloomtest")).value();

    QImage source(1600, 1200, QImage::Format_RGB32);
    source.fill(QColor(40, 40, 40));
    for (int y = 420; y < 780; ++y)
        for (int x = 620; x < 980; ++x)
            source.setPixel(x, y, qRgb(255, 255, 255));
    const QImage preview = source.scaled(160, 120, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
    const auto small = EffectEngine::instance().render(preview, recipe, 100, preview.size(), false);
    const auto large = EffectEngine::instance().render(source, recipe, 100, source.size(), true);
    QVERIFY(small.ok());
    QVERIFY(large.ok());
    const QImage downscaled =
        large.value().scaled(preview.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QVERIFY2(meanDifference(small.value(), downscaled) < 4.0,
             qPrintable(QStringLiteral("preview bloom differs by %1")
                            .arg(meanDifference(small.value(), downscaled))));
    QVERIFY(std::abs(EffectEngine::referenceScale(160) - 160.0 / 2400.0) < 1e-9);
}

void TestEffectEngine::preparedLinearHonoursVersion()
{
    ImageStore store;
    const SourceImage source = memorySource(scaledSample(QSize(200, 150)));
    EffectRenderTarget target;
    target.recipeVersion = 1;
    const auto pinned = store.preparedLinear(source, ColorRecipe{}, QStringLiteral("light.venice"),
                                             100, false, 0, {}, target);
    QVERIFY(pinned.ok());
    const auto current = store.preparedLinear(source, ColorRecipe{}, QStringLiteral("light.venice"),
                                              100, false, 0, {});
    QVERIFY(current.ok());
    QCOMPARE(pinned.value(), current.value());

    // A version this build does not ship falls back to the current recipe and
    // says so instead of silently pretending to pin.
    QTest::ignoreMessage(QtWarningMsg,
                         "ImageSource: effect 'light.venice' has no version 99, using the "
                         "current recipe");
    const auto missing = store.preparedLinear(source, ColorRecipe{},
                                              QStringLiteral("light.venice"), 100, false, 0, {},
                                              EffectRenderTarget{99, {}});
    QVERIFY(missing.ok());
    QCOMPARE(missing.value(), current.value());

    // The print target reaches ImageStore and is the linear layer size that
    // PageRenderer composites, not the full source size.
    EffectRenderTarget printed;
    printed.printSize = QSize(400, 300);
    const auto printLayer = store.preparedLinear(
        source, ColorRecipe{}, QStringLiteral("light.venice"), 100, false, 0, {}, printed);
    QVERIFY(printLayer.ok());
    QCOMPARE(printLayer.value().size(), QSize(400, 300));
}

void TestEffectEngine::goldensArePinned()
{
    struct Golden {
        const char *key;
        const char *parameters;
        const char *render;
    };
    static const Golden goldens[] = {
        {"light.venice", "53725ad6865b92d0d397d78249268c2fc961891b3c17d5a43848140fb685197e",
         "0f67c2224c93add5edee64d161c767bf47c70f237e957e6d0adb6c1eba2a3bf9"},
        {"color.tealorange", "e2b8894fdedbb446586accd5d5023711008adee7a927167f8221d5f8b8286068",
         "c0a9d1568d1066730747011280bfbad1ddf1657fc2a3b906ddb9ae18b7565a44"},
        {"retro.fadedfilm", "8182c08be20e2933f4b88a20d8381a52b534c8267b842f594062e996db2d28c4",
         "3735379541f0011019afd876fc6261fc221b3653e20a48aba75d2bb49176c5c1"},
        {"light.nordic", "f2234c7eb120c7ab8d9420fe900f754e72256a6b7d3d23eec8f78aafbcea4f4d",
         "e4b22e0c57d8160c2ed51c16b485c131daf74812c4ac2410eb8c5dc854fc4832"},
        {"light.goldenhour", "8e1a03c15866341efb059a91391e0860350b6b8740ef97a9c7cef6edf8bf509c",
         "ac72d3e53e8e470b80f0fb788dc475988aad6a2d88ad1961e5e8cd96c3bec6ac"},
        {"glow.neonnight", "33ca7ccc102b8572f2d937e5eda62f9553d6087fd050215b1c04d9cc7e740279",
         "6e544c18664028b54b28e34742ac8532d643e27e2831ed7536940b364bef528a"},
        {"color.pastel", "2c582137ff133d15d0108a9659c489eda4d490a9815f467de7745254b6ecb315",
         "2901cb5d98c0a444c5e513423ffc68f19f5f839ad59953b60eab2998a8c73a64"},
        {"mono.contrast", "c405bdc601b8ec6032b683a8b6ccb2ccfc4ca02eaef054ae890ba43108800eb4",
         "c9027d626f6605c718b4b7fe86a5ba0f2947a1db4443231c7bf49ce261710e08"},
        {"print.sepia", "baf3b83b48295c42cfac6b5773da7c2170322e8516ff6931bc2553509e1f7388",
         "33bdf56d9adb728434e40126cd9edf7ac20e2e56ec2da0d0688c611358223240"},
        {"retro.crossprocess", "7dc09d9dd7141edff905ad7260ea150e7d111631f21e7b9066cd6eb7eb25e446",
         "3f81ff66b5d4501a4a2ffc5eb9a327883a5fb87daf595257d647ee7e351a9cd2"},
    };

    QFile sourceFile(QStringLiteral(":/effects/catalogue.json"));
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QJsonArray rawRecipes = QJsonDocument::fromJson(sourceFile.readAll())
                                     .object().value(QStringLiteral("recipes")).toArray();
    QHash<QString, QString> parameters;
    for (const QJsonValue &value : rawRecipes) {
        const QJsonObject object = value.toObject();
        parameters.insert(object.value(QStringLiteral("key")).toString(),
                          QString::fromLatin1(QCryptographicHash::hash(
                              QJsonDocument(object).toJson(QJsonDocument::Compact),
                              QCryptographicHash::Sha256).toHex()));
    }

    const QImage source = scaledSample(QSize(240, 180));
    int checked = 0;
    // The ten signature looks are pinned by key. The Phase E work did not touch
    // their parameters, so no golden was re-baselined; a change here is a change
    // of look and must be argued for, not regenerated.
    for (const Golden &golden : goldens) {
        const auto recipe = EffectCatalogue::instance().resolve(QString::fromLatin1(golden.key));
        QVERIFY2(recipe.ok(), golden.key);
        QCOMPARE(parameters.value(recipe.value().key), QString::fromLatin1(golden.parameters));
        const auto result = EffectEngine::instance().render(source, recipe.value(), 100,
                                                            source.size(), false);
        QVERIFY(result.ok());
        QCOMPARE(QString::fromLatin1(hashImage(result.value()).toHex()),
                 QString::fromLatin1(golden.render));
        checked += 1;
    }
    QCOMPARE(checked, 10);
    QCOMPARE(kSignatureKeys.size(), 10);
}

void TestEffectEngine::rendersAreDeterministic()
{
    const QImage source = scaledSample(QSize(160, 120));
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        const auto first =
            EffectEngine::instance().render(source, recipe, 80, source.size(), false);
        const auto second =
            EffectEngine::instance().render(source, recipe, 80, source.size(), false);
        QVERIFY(first.ok());
        QVERIFY(second.ok());
        QCOMPARE(hashImage(first.value()), hashImage(second.value()));
    }
}

void TestEffectEngine::compilesIntoSegments()
{
    const auto recipe =
        EffectCatalogue::instance().resolve(QStringLiteral("light.venice")).value();
    const auto plan = EffectEngine::compile(recipe);
    QVERIFY(plan.ok());
    QVERIFY(plan.value().pointSteps >= 1);
    QVERIFY(plan.value().spatialSteps >= 3);
    QVERIFY(plan.value().hasFinishStages);
    QVERIFY(plan.value().steps.first().point);
    QVERIFY(plan.value().steps.first().hasCurve);
    bool sawBloom = false;
    for (const EffectPlanStep &step : plan.value().steps) {
        if (!step.point && step.spatial.id == StageId::Bloom)
            sawBloom = true;
    }
    QVERIFY(sawBloom);
}

void TestEffectEngine::ditheredPosterizeIsSpatial()
{
    const QJsonArray recipes{QJsonObject{
        {QStringLiteral("key"), QStringLiteral("basic.test")},
        {QStringLiteral("category"), QStringLiteral("basic")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("name"), QStringLiteral("Test")},
        {QStringLiteral("stages"),
         QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("posterize")},
                         {QStringLiteral("levels"), 5},
                         {QStringLiteral("dither"), true}},
         }}}};
    const auto catalogue = EffectCatalogue::fromJson(catalogueJson(recipes));
    QVERIFY(catalogue.ok());
    const auto recipe = catalogue.value().resolve(QStringLiteral("basic.test"));
    QVERIFY(recipe.ok());
    const auto plan = EffectEngine::compile(recipe.value());
    QVERIFY(plan.ok());
    QCOMPARE(plan.value().pointSteps, 0);
    QCOMPARE(plan.value().spatialSteps, 1);
    QVERIFY(!plan.value().steps.first().point);
    QCOMPARE(plan.value().steps.first().spatial.id, StageId::Posterize);
}

void TestEffectEngine::printTargetSizes()
{
    const auto recipe =
        EffectCatalogue::instance().resolve(QStringLiteral("color.tealorange")).value();
    const QImage source = scaledSample(QSize(400, 300));
    const auto preview =
        EffectEngine::instance().render(source, recipe, 100, QSize(160, 120), false);
    QVERIFY(preview.ok());
    QCOMPARE(preview.value().size(), QSize(160, 120));

    const auto printed =
        EffectEngine::instance().render(source, recipe, 100, QSize(800, 600), true);
    QVERIFY(printed.ok());
    QCOMPARE(printed.value().size(), QSize(800, 600));
}

void TestEffectEngine::previewAndPrintAgreeForPointStages()
{
    const QJsonArray recipes{QJsonObject{
        {QStringLiteral("key"), QStringLiteral("color.test")},
        {QStringLiteral("category"), QStringLiteral("color")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("name"), QStringLiteral("Point Test")},
        {QStringLiteral("stages"),
         QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("whiteBalance")},
                         {QStringLiteral("temperature"), 12},
                         {QStringLiteral("tint"), 3}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("exposure")},
                         {QStringLiteral("ev"), 0.2}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("contrast")},
                         {QStringLiteral("amount"), 18}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("curves")},
                         {QStringLiteral("master"),
                          QJsonArray{QJsonArray{0, 0}, QJsonArray{0.5, 0.55},
                                     QJsonArray{1, 1}}}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("splitTone")},
                         {QStringLiteral("shadow"), QStringLiteral("#203040")},
                         {QStringLiteral("shadowStrength"), 0.3},
                         {QStringLiteral("highlight"), QStringLiteral("#ffe0c0")},
                         {QStringLiteral("highlightStrength"), 0.25},
                         {QStringLiteral("balance"), 0.55}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("saturation")},
                         {QStringLiteral("amount"), 20}},
         }}}};
    const auto catalogue = EffectCatalogue::fromJson(catalogueJson(recipes));
    QVERIFY(catalogue.ok());
    const auto recipe = catalogue.value().resolve(QStringLiteral("color.test")).value();

    const QImage source = scaledSample(QSize(1600, 1200));
    const auto preview = EffectEngine::instance().render(source, recipe, 100, source.size(), false);
    const auto print = EffectEngine::instance().render(source, recipe, 100, source.size(), true);
    QVERIFY(preview.ok());
    QVERIFY(print.ok());
    QCOMPARE(hashImage(preview.value()), hashImage(print.value()));

    // 3200 px is above the grade bound, so this exercises grade -> finish ->
    // one Lanczos upscale rather than the same-raster shortcut above.
    const auto big = EffectEngine::instance().render(source, recipe, 100, QSize(3200, 2400), true);
    QVERIFY(big.ok());
    QCOMPARE(big.value().size(), QSize(3200, 2400));
    const QImage downscaled =
        big.value().scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QVERIFY(meanDifference(preview.value(), downscaled) < 6.0);
}

void TestEffectEngine::grainSurvivesThePrintPath()
{
    const auto recipe =
        EffectCatalogue::instance().resolve(QStringLiteral("light.venice")).value();
    const QImage source = scaledSample(QSize(1600, 1200));
    const auto preview = EffectEngine::instance().render(source, recipe, 100, source.size(), false);
    QVERIFY(preview.ok());

    const auto sameRaster =
        EffectEngine::instance().render(source, recipe, 100, source.size(), true);
    QVERIFY(sameRaster.ok());

    const auto print =
        EffectEngine::instance().render(source, recipe, 100, QSize(3200, 2400), true);
    QVERIFY(print.ok());
    const QImage downscaled =
        print.value().scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    const double previewEnergy = highFrequencyEnergy(preview.value());
    const double printEnergy = highFrequencyEnergy(downscaled);
    QVERIFY(previewEnergy > 0.5);
    QVERIFY(printEnergy > 0.5);
    const double ratio = printEnergy / previewEnergy;
    QVERIFY2(ratio > 0.25 && ratio < 4.0,
             qPrintable(QStringLiteral("grain energy ratio %1").arg(ratio)));
}

void TestEffectEngine::versionPinningSelectsTheOldRecipe()
{
    const QJsonArray recipes{
        testRecipe(QStringLiteral("basic.test"), QStringLiteral("basic"), 1, 0.5),
        testRecipe(QStringLiteral("basic.test"), QStringLiteral("basic"), 2, -0.5),
    };
    const auto catalogue = EffectCatalogue::fromJson(catalogueJson(recipes));
    QVERIFY(catalogue.ok());
    const auto pinned = catalogue.value().resolve(QStringLiteral("basic.test"), 1);
    QVERIFY(pinned.ok());
    QCOMPARE(pinned.value().version, 1);

    const QImage source = scaledSample(QSize(96, 72));
    const auto oldLook =
        EffectEngine::instance().render(source, pinned.value(), 100, source.size(), false);
    const auto current =
        catalogue.value().resolve(QStringLiteral("basic.test")).value();
    const auto newLook =
        EffectEngine::instance().render(source, current, 100, source.size(), false);
    QVERIFY(oldLook.ok());
    QVERIFY(newLook.ok());
    QVERIFY(meanDifference(oldLook.value(), newLook.value()) > 5.0);
}

void TestEffectEngine::legacyKeysKeepTheOldPath()
{
    ImageStore store;
    const SourceImage source = memorySource(scaledSample(QSize(96, 72)));
    const auto linear = store.linear(source, ColorRecipe{}, 0);
    QVERIFY(linear.ok());
    const QImage srgb = ColorPipeline::linearToSrgbWithAlpha(linear.value());
    const QImage expected = PhotoFilters::apply(srgb, FilterPreset::Vivid, 100);
    const QImage expectedLinear = ColorPipeline::srgbToLinear(expected);
    const auto actual =
        store.preparedLinear(source, ColorRecipe{}, QStringLiteral("vivid"), 100, false, 0);
    QVERIFY(actual.ok());
    QCOMPARE(actual.value(), expectedLinear);

    // The black-white key 1.0.x writes stays on the legacy path, and the `bw`
    // spelling means the same preset.
    const auto blackWhite =
        store.preparedLinear(source, ColorRecipe{}, QStringLiteral("blackwhite"), 100, false, 0);
    const auto bw =
        store.preparedLinear(source, ColorRecipe{}, QStringLiteral("bw"), 100, false, 0);
    QVERIFY(blackWhite.ok());
    QVERIFY(bw.ok());
    QCOMPARE(bw.value(), blackWhite.value());
}

void TestEffectEngine::unknownKeysFallBackToOriginal()
{
    ImageStore store;
    const SourceImage source = memorySource(scaledSample(QSize(96, 72)));
    const auto original = store.linear(source, ColorRecipe{}, 0);
    const auto unknown =
        store.preparedLinear(source, ColorRecipe{}, QStringLiteral("not.a.recipe"), 100, false, 0);
    QVERIFY(original.ok());
    QVERIFY(unknown.ok());
    QCOMPARE(unknown.value(), original.value());
}

void TestEffectEngine::alphaIsPreserved()
{
    QImage source(96, 72, QImage::Format_ARGB32);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x)
            source.setPixel(x, y, qRgba(x * 2, y * 2, 128, (x * 255) / source.width()));
    }
    const auto recipe =
        EffectCatalogue::instance().resolve(QStringLiteral("light.venice")).value();
    const auto result =
        EffectEngine::instance().render(source, recipe, 100, source.size(), false);
    QVERIFY(result.ok());
    QCOMPARE(result.value().size(), source.size());
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            QCOMPARE(qAlpha(result.value().pixel(x, y)), qAlpha(source.pixel(x, y)));
        }
    }
}

void TestEffectEngine::effectVersionRoundTrips()
{
    DocumentSnapshot document;
    Page page;
    Frame frame;
    frame.id = QStringLiteral("frame-1");
    frame.sourcePath = QStringLiteral("/tmp/photo.jpg");
    frame.sourceSha256 = QByteArrayLiteral("0123456789abcdef");
    frame.filter = QStringLiteral("light.venice");
    frame.filterStrength = 65;
    frame.effectVersion = 3;
    page.frames.append(frame);
    document.pages.append(page);

    const QByteArray json = ProjectStore::serialize(document, QStringLiteral("/tmp"));
    const QJsonDocument parsed = QJsonDocument::fromJson(json);
    const QJsonObject stored =
        parsed.object().value(QStringLiteral("pages")).toArray().first().toObject()
            .value(QStringLiteral("frames")).toArray().first().toObject();
    QCOMPARE(stored.value(QStringLiteral("effectVersion")).toInt(-1), 3);

    const auto loaded = ProjectStore::deserialize(json);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.value().pages.first().frames.first().effectVersion, 3);

    // A 1.0.1 project has no key at all and follows the current recipe.
    QJsonObject oldRoot = parsed.object();
    QJsonArray pages = oldRoot.value(QStringLiteral("pages")).toArray();
    QJsonObject oldPage = pages.first().toObject();
    QJsonArray frames = oldPage.value(QStringLiteral("frames")).toArray();
    QJsonObject oldFrame = frames.first().toObject();
    oldFrame.remove(QStringLiteral("effectVersion"));
    frames.replace(0, oldFrame);
    oldPage.insert(QStringLiteral("frames"), frames);
    pages.replace(0, oldPage);
    oldRoot.insert(QStringLiteral("pages"), pages);
    const auto legacy = ProjectStore::deserialize(
        QJsonDocument(oldRoot).toJson(QJsonDocument::Compact));
    QVERIFY(legacy.ok());
    QCOMPARE(legacy.value().pages.first().frames.first().effectVersion, 0);
}

void TestEffectEngine::rendersOffTheGuiThread()
{
    const auto recipe =
        EffectCatalogue::instance().resolve(QStringLiteral("light.venice")).value();
    const QImage source = scaledSample(QSize(160, 120));
    const QByteArray expected =
        hashImage(EffectEngine::instance().render(source, recipe, 100, source.size(), false)
                      .value());

    std::atomic<int> failures = 0;
    std::thread first([&] {
        const auto result =
            EffectEngine::instance().render(source, recipe, 100, source.size(), false);
        if (!result.ok() || hashImage(result.value()) != expected)
            failures.fetch_add(1);
    });
    const auto other =
        EffectCatalogue::instance().resolve(QStringLiteral("print.sepia")).value();
    std::thread second([&] {
        const auto result =
            EffectEngine::instance().render(source, other, 100, source.size(), false);
        if (!result.ok())
            failures.fetch_add(1);
    });
    first.join();
    second.join();
    QCOMPARE(failures.load(), 0);
}

void TestEffectEngine::catalogueDistinctnessGate()
{
    // Every shipped look is rendered at the browser raster on the committed
    // sample and compared inside its own category. The metric is the mean
    // CIELAB distance across the frame (see meanLabDistance); the floor is 2.0.
    // The tightest shipped pair is basic.neutral vs basic.warmbase at 2.19, so
    // the gate has margin, but a new look must still clear it.
    const QImage source = scaledSample(QSize(240, 180));
    QHash<QString, QList<LabFrame>> byCategory;
    QHash<QString, QList<QString>> keysByCategory;
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        const auto result =
            EffectEngine::instance().render(source, recipe, 100, source.size(), false);
        if (!result.ok()) {
            QFAIL(qPrintable(recipe.key + QStringLiteral(": ") + result.error().message));
        }
        byCategory[recipe.category].append(labFrame(result.value()));
        keysByCategory[recipe.category].append(recipe.key);
    }
    double tightest = 1e9;
    QString tightestPair;
    for (const QString &category : EffectCatalogue::instance().categoryKeys()) {
        const QList<LabFrame> &looks = byCategory.value(category);
        const QList<QString> &keys = keysByCategory.value(category);
        QCOMPARE(looks.size(), 10);
        for (int i = 0; i < looks.size(); ++i) {
            for (int j = i + 1; j < looks.size(); ++j) {
                const double distance = meanLabDistance(looks.at(i), looks.at(j), 1);
                if (distance < tightest) {
                    tightest = distance;
                    tightestPair = QStringLiteral("%1 vs %2").arg(keys.at(i), keys.at(j));
                }
                QVERIFY2(distance >= kDistinctnessFloor,
                         qPrintable(QStringLiteral("%1 vs %2: deltaE %3 < %4 in '%5' "
                                                   "(tightest so far: %6 at %7)")
                                        .arg(keys.at(i), keys.at(j))
                                        .arg(distance)
                                        .arg(kDistinctnessFloor)
                                        .arg(category)
                                        .arg(tightest)
                                        .arg(tightestPair)));
            }
        }
    }
    // The gate is only meaningful if the catalogue clears it with room; this is
    // where the observed minimum is reported when it does not.
    QVERIFY2(tightest >= kDistinctnessFloor,
             qPrintable(QStringLiteral("tightest within-category pair %1 at deltaE %2, "
                                       "floor %3")
                            .arg(tightestPair)
                            .arg(tightest)
                            .arg(kDistinctnessFloor)));
}

void TestEffectEngine::catalogueCrossCategoryGate()
{
    // No two looks in the whole catalogue may be near-clones, even across
    // categories: a user searching by name should not land on the same picture
    // twice. Every pair must clear kCloneFloor, and every pair outside the
    // documented correction family must clear kCrossCategoryFloor. The tightest
    // offenders before this gate existed were basic.punch vs glow.pop at 0.70,
    // detail.drybrush vs art.impasto at 1.29 and retro.newsprint vs print.news
    // at 1.78; all three were retuned apart. The comparison samples every second
    // pixel of the 240 px render, which is the same measurement at a quarter of
    // the work.
    constexpr int kStride = 2;
    const QImage source = scaledSample(QSize(240, 180));
    QList<LabFrame> looks;
    QList<QString> keys;
    QList<QString> categories;
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        const auto result =
            EffectEngine::instance().render(source, recipe, 100, source.size(), false);
        if (!result.ok()) {
            QFAIL(qPrintable(recipe.key + QStringLiteral(": ") + result.error().message));
        }
        looks.append(labFrame(result.value()));
        keys.append(recipe.key);
        categories.append(recipe.category);
    }
    double tightest = 1e9;
    QString tightestPair;
    for (int i = 0; i < looks.size(); ++i) {
        for (int j = i + 1; j < looks.size(); ++j) {
            if (categories.at(i) == categories.at(j))
                continue;
            const double distance = meanLabDistance(looks.at(i), looks.at(j), kStride);
            const bool family = kCorrectionFamily.contains(keys.at(i))
                && kCorrectionFamily.contains(keys.at(j));
            const double floor = family ? kCloneFloor : kCrossCategoryFloor;
            if (distance < tightest) {
                tightest = distance;
                tightestPair = QStringLiteral("%1 vs %2").arg(keys.at(i), keys.at(j));
            }
            QVERIFY2(distance >= floor,
                     qPrintable(QStringLiteral("%1 vs %2: deltaE %3 < %4%s "
                                               "(tightest so far: %5 at %6)")
                                    .arg(keys.at(i), keys.at(j))
                                    .arg(distance)
                                    .arg(floor)
                                    .arg(family ? QStringLiteral(" (correction family)")
                                                : QString())
                                    .arg(tightest)
                                    .arg(tightestPair)));
        }
    }
    // The documented family is the only allowed slack, and it has to stay above
    // the absolute clone floor with room.
    QVERIFY2(tightest >= kCloneFloor + 0.1,
             qPrintable(QStringLiteral("tightest cross-category pair %1 at deltaE %2")
                            .arg(tightestPair)
                            .arg(tightest)));
}

void TestEffectEngine::catalogueRenderHashes()
{
    // One pinned render hash per shipped recipe at 160 px, all in a single
    // test. The hash is a change detector, not a quality judgement: a different
    // MagickWand version or a retuned recipe changes it, and that is a
    // deliberate act, not a silent one.
    struct Hash {
        const char *key;
        const char *sha256;
    };
    static const Hash hashes[] = {
        {"basic.neutral",
         "085e2e9ff2df391d6f2cffb90e302444eec221f738cd0ecb65c34707fc7d4f2f"},
        {"basic.bright",
         "bee8a72c75e7a786025c16cbbb025e3b47b966c4ca4d40304b642f1355455ea1"},
        {"basic.deep",
         "d1cf1105da9475e8b7ad7b519878149f06767e4959dd029cb433a8450bcac57d"},
        {"basic.punch",
         "3c7b24e4c95a12316ce8f7bc0d9ea4baea06821c154503acf4f2d50ad0eeea92"},
        {"basic.soft",
         "ab4edb454acc016d5319beb66e640fdcc63973dfaeeb1094a111d2c766dd52b0"},
        {"basic.warmbase",
         "66656bbda44c6890a95d1b2a41aa66d26200d5f2f39ddf762d1a6cdc478051bd"},
        {"basic.coolbase",
         "b626550e471b5367817ee778f5bc08ada132ede8dc509d8f9b64aa20d10e2b2b"},
        {"basic.clean",
         "955dda053a7451a81cd49953c84c35805b2f75c0718227ec8edf45817772359e"},
        {"basic.matte",
         "fd325af2aca38f3d6a969b37eddc66326ac7d9421b940a759d9e8e55b73d392b"},
        {"basic.faithful",
         "ef4b5c0bf64588c3fbefded0144c5543613d0be21b78413aff0d1fe2eb5cb10d"},
        {"mono.classic",
         "75737ea08e6d69a2cbdbce97fdd00e49c77ad6089829de4b7556752d23055e1c"},
        {"mono.silver",
         "f170069a5f89a63c41071976132aac6fa038f7254ffca1916a7abe40a0db705f"},
        {"mono.highkey",
         "17591af137fc8d32f0e99b42981ded04a917e7afcad0d5219729cfdc4c753104"},
        {"mono.lowkey",
         "f8bf4ac79ab8c7fb41b801f2b04f0e4ada9d85a16c9e73da896b6b4910d74ba0"},
        {"mono.contrast",
         "8fd9307791eb29d65403e711ebe0eef02158a5c2330f21b522b27e04d9eb0408"},
        {"mono.film",
         "d0fb08588975af23974372c485cd939629826e1155518319e1d08e3a2a0bdf55"},
        {"mono.selenium",
         "6e7836a170b41d2d55de3f41ddddb5291d641dda8684eac17ed60dd199aa6851"},
        {"mono.platinum",
         "d009526ecda512100b31a8e885bb540f30a65eed40cb6a659595fa74f5c965eb"},
        {"mono.infrared",
         "9e2c2b4ec14f5e37ad4c781d4ac09d0c554bb38bd1b67eaf58c33488f4397fc0"},
        {"mono.ink",
         "ae959c189b32a14adb5205e97d46a0acb1977e0b569dee612fb76f9ac60e20a7"},
        {"retro.warmfilm",
         "b7f62d0535b882019c7bff6bda9ecb71155a5fe39907adc54c27cd26bfd8f3b4"},
        {"retro.fadedfilm",
         "80f7a33901b6609e31c9b365d95681b9dd59da770145ffd2e906b7e72b63f6be"},
        {"retro.instant",
         "c4666f90675ffef9a1eeb3482564543a8e71ef18ffaa5f6e8f1e878404a79d09"},
        {"retro.slide",
         "c9ee7c187253f13e698682fc625b2c59a4d7cd2b81919ec32cf86c1f53c2d056"},
        {"retro.seventies",
         "df25de142c69a35712cd6a8fe74e7ea839b5c6f3a2c8561113ebd8e8f10a8554"},
        {"retro.crossprocess",
         "132a5fdbc89a2ee7239dde2c26e6b38a2380f239b939db97345dd54edf9a033d"},
        {"retro.lomo",
         "22ec9c9477cb4fcdf4023b6556a19c3b9c9e279c844e6f1579be9e8ec3a17429"},
        {"retro.daguerre",
         "59bf8b9d791bc1379ea6dc3b4ee2280f0d9a8007a2ed3f70ec95a9578a238e87"},
        {"retro.sunbleach",
         "f5398082763a7c4a101c1cb401c37b8defb182b1164c4c1f9bcdeafc36833173"},
        {"retro.newsprint",
         "fb7f01699cd03542175ae5d32bffb78017b64fe728b13cab2f5d31932526b66f"},
        {"color.vivid",
         "d7306840c9a9dc5541dd17c461e1c6974441203de20a3601ce90fdb64ff056eb"},
        {"color.filmic",
         "963befa875625a2c327d8eb2c906f39559ec4af9ce198ad0ffaa9cd5b11684f3"},
        {"color.pastel",
         "b1ffb9b1193f57dbd36389df1ee4d8064fcccc94452d4188c012e071cbd2e1e1"},
        {"color.tealorange",
         "df7366f4eace93a6f431bc92c84cd68cf9fd6cde6810c1e9959a01a13805c13a"},
        {"color.earth",
         "e2cd3708006c0a4094354b1325a83859401c02f6f79e274a089d4a973b4699cb"},
        {"color.mint",
         "b32b15c24f7ce429e6d3c1594076ba18e05b75ad375ae2a78e02e7a97f4eae8a"},
        {"color.amber",
         "0fbc445fb0cf20dcbcc4f2a78e0abb3b649beb4c1006c5fa5c40a8591b924509"},
        {"color.sapphire",
         "7c3e12387a5a61bda82ec13393752d29fac9cac890b66ff0d6e75dabd54b639d"},
        {"color.neutralize",
         "9a4c1fe92f5877df95ca2ec8c3c3a0d0f0ff96451b796b505a631b220cd156e6"},
        {"color.huepop",
         "02fd1c10642a18c4638e056978a0349d60bb7bada82b6cec40b8baead020ca58"},
        {"blur.soft",
         "68eedf5bb474f6c6e0bec37373d4bf3cf76a85d24e8229364ba8099470e6e211"},
        {"blur.dream",
         "04edd44e47370a0ef670789a7ee44cfaa627637eb50157e2b252f2e53171a034"},
        {"blur.motion",
         "f1c2755eccabc927c9b8ad980bd55b5870363f4607197ea1119b8214cde53e5e"},
        {"blur.zoom",
         "66256ab7411058e051b1d8aad935789fdb36e479f4e3b15a456cd2a5bc3460f8"},
        {"blur.radial",
         "fe4b71a3eb7930277634d44a501636c1ad8cab33ab5f44677dea509b6acbb9f8"},
        {"blur.tilt",
         "ebfa26c5ce4754f4088bd2921e66656cb64cb449645920ac1be62d79d52366a0"},
        {"blur.bokeh",
         "6183b60f823bd5b3ec09f7a7eb7ef9350266beb64355d62aee87fab5f0f0b534"},
        {"blur.fog",
         "5bde8936032f0a8f2ae1eb232957765f469478ad1f90ae1f9ef6c3723f713f98"},
        {"blur.diffusion",
         "22bfca4251aba20a63ea6af035bcd741ee43a4bf5410d1be5c92143334d90904"},
        {"blur.glass",
         "d47be585538290ad8f1f1b9a05bc45b59402e8c09baeac43979754ebcc68b57c"},
        {"detail.crisp",
         "a63c193bf7199429ba1fbe815d7fc37fae5ddab2a665197f6ec8b4149127cea4"},
        {"detail.clarity",
         "26cdf2f528e3c222da40bea1f240d3001396b586f8b2c65756625f7cbc68635c"},
        {"detail.micro",
         "b966bf9cc5827f249e7cf92c6191ea2ce5a05dedb5f3951ea217a90f6bd907f4"},
        {"detail.structure",
         "4d0a5110322b52d8c417bfd22fc5464a2097da7e5fb20db9c85167a09cc2d692"},
        {"detail.emboss",
         "b53328b68d347060df5df01131a94226e37a34ffcc19972de2e4378efcbcbc16"},
        {"detail.edge",
         "ba4ba28a5792b6a7ba6fbf7c504a5a8c54121490dda8ca84658e7cf93f376eb3"},
        {"detail.pencil",
         "5ad1cc185f7bdf75d7966a7599a0da5d047eba7728026f07b030dfe269aa8f39"},
        {"detail.charcoal",
         "08f68877e50dfccda96dbe078da91c47ddd60619e4025d24995aaae7b08f6b95"},
        {"detail.etch",
         "f563b758ea15826d6fe91760db54c3730d3e32419a44a27a9962254eeb3721e7"},
        {"detail.drybrush",
         "4f0ec6d4d02f89868bedc729a2662b0134792ded03a0469ab3a2419a66bb9a9f"},
        {"art.oil",
         "4322bbbd7875c46dd1cb1ed9c6ab2e0d95313c4b18ebe5449c361eb4e6e7ecce"},
        {"art.chalk",
         "168bbac7ceddfc3a2e52ac53ce21df20474e6bcfeec7083d173864eeb0ddd338"},
        {"art.watercolour",
         "e295428b5678a90c3071d0ed9c0c0dcb2bdba69bc681f7fac3c9f78313b2443a"},
        {"art.painterly",
         "891cf54208f899908edd86965f1921cd21743bd85654cb3ac247a4e55b0a1f2b"},
        {"art.popart",
         "7d4b9438326114a5a1f5673e86df6f8d694556402a7049fafb1704eca975531c"},
        {"art.comic",
         "8af972a8c2a95f8cbf35fd5b3340a8efcc0e5de5771449fb9f0ed9b7ff8e97ab"},
        {"art.inkwash",
         "51d73e63d2e1ce7fef867768f4bd4e685b2166c7393ef0ffb5570a657a691db9"},
        {"art.mosaic",
         "8edd76131abebe9a1dea52053dd76e6650d6c2018acd8bab76407474684e294e"},
        {"art.impasto",
         "a260830b1f4d41199d6d98d39cca89733ae2fe8a6f95a4f569d7495635372431"},
        {"art.stencil",
         "cb377dd3ec61ad86b65c2af5a3c77567be7ffde9b243c0be4b1376a354ededca"},
        {"warp.swirl",
         "d169a260416cb29879cbe8aad7f3952d90ce077cac3e56b2fb0e01c14a5057d3"},
        {"warp.vortex",
         "d4434cc118f831ba9d854416c89d9697c7383d0d05a4b7b5450c8a9ea4f0b6ff"},
        {"warp.implode",
         "8d229e25c73b6be3612fe6a0450bc7633a2cbb8510a524aae6af17939f551c08"},
        {"warp.explode",
         "6913b731899dc16634f6052b3afffbb994b3988cb2e2890134ee0065a558925c"},
        {"warp.wave",
         "48b800e8570c65c419e27db8074b550733a2429c9040298b428571d9e89a5369"},
        {"warp.ripple",
         "0ef7dc949fe6ba2026d68f52abdadefaa0af76fd620c781218b892d8253b5e48"},
        {"warp.mirror",
         "c8c848f6eb76804053c6b4fa3f7ba9363a1a9a5c39364e482f18fca11feb3ae2"},
        {"warp.kaleido",
         "3732217ff9cc75e3e403fa50731117c63aecbaf19af6806e78f7edf06a1e02cf"},
        {"warp.pinch",
         "965aa3ca4065b7c6da34c20811ccebfbad71785c8aba69626c5809cf25b0c291"},
        {"warp.stretch",
         "d5c86da4365236fb990be30ba475a03bb3f4d34193bfa11256d0c5493118f382"},
        {"light.goldenhour",
         "04c11d4b4a9798b6885ca5cd81cce50045620d877aba6b5dfa3e4def80f37384"},
        {"light.leak",
         "13935237243779f6e12c91af916cd1878ec0da35046cb8d8a9ac059e446dbac9"},
        {"light.haze",
         "d9dba46ea69a775ce712028eb4cda4521c57379cb17731524ba0969f2db9a428"},
        {"light.venice",
         "67b935499900eabc137375d268d0b3fae671789cd411bb57810a323a5fff972d"},
        {"light.backlight",
         "75716b8ea204a2072f95bb1acbf19c5d5cef730224aacd8dcf72edbc125f2671"},
        {"light.window",
         "8a2f02f3e7482a90a101fea50996333a71a8cb0a0791223b9f7e67e29d0c5a7c"},
        {"light.dusk",
         "32b63bfbaf1f496eb0cc3b24131f8f749769948a52f865f1bd1b0708a88b7597"},
        {"light.dawn",
         "5f7f74361e7aa12e93bacb5ba3b1724d8f026b8aeacb1a9dfe282e47ea9c599e"},
        {"light.spot",
         "2112a13fb124369df4a743e9251452f3a2a52589d21eba663290c6fdf5769c94"},
        {"light.nordic",
         "c269b8313e52d1cb1fd83cadf18c1d980f73f3e62dab6f78063df01909a2b5df"},
        {"grain.fine",
         "52ed7b38dab838028ef7477c8832b03e6ce9863f1097669dbed39cce712ea063"},
        {"grain.medium",
         "ad3dca5293b52b38364252e2bad67d7f3c7a3671e79f80e10d4155949ac815c4"},
        {"grain.coarse",
         "f0d69fbf223d3dbd1d017ffafc4594e0365ffda3344a20c429b10b3165c52ad5"},
        {"grain.push",
         "5205029d65108f7c3a9ef87c60e336732b0765d4a25e34ae5fe2c04aea4e5e41"},
        {"grain.iso",
         "b575d1a48f6a2746c36e8d6c6a372c83c0816f6d5a5e2f0df0e9253c4410311f"},
        {"grain.dust",
         "2447e5f7b0b9417b18dd3b1fc061f29c22c538fda1626f521d48ef24a596e57d"},
        {"grain.paper",
         "b9ac460a09e0bdfd4682a3b4d618c7690bb3f0c81d8acfb969011dcb4f8800f9"},
        {"grain.static",
         "eb2bb9260d8488ed95c576b2c51086a15679898e9c6097b87b598a4aa7746b5a"},
        {"grain.snow",
         "38f54874f9b4ec2699d0c77f0ab41877fdc824010fa5438699f4a6555e7e2fd5"},
        {"grain.base",
         "f88650d715decb7459ff2c0abb81a45ce37e50ef402207cbb67f67c115de9239"},
        {"print.halftone",
         "a59008a6b7d25d3a762ba28b73bdacf46c18686422d361baa346cb0e80656f2c"},
        {"print.riso",
         "22e43084218edb589fec74aef91f94409cd14adf0acd5f9ec68b3f1f489a13da"},
        {"print.news",
         "45f6ec02921111f32104f31ae812e8b237991fedac9ce5f206683963c0f14f11"},
        {"print.cyanotype",
         "c0356dadaa02a2636f2d6de2828cf03e3b511124c2c45042c188af5ee18e9d86"},
        {"print.screen",
         "8adb328fbce8600762e9fce34da14ec802015fa7e7f64b49a7e58f260f188ac0"},
        {"print.litho",
         "f64ce16e1f9a2ea472db4e0784f02433c3d73eda6490a54b3f1b15ba4269e04d"},
        {"print.engrave",
         "ed21cbe05a02f6dc6110e8cd223452d08121e4809b670001a9e84c12ff90a352"},
        {"print.stamp",
         "2f14c2dec635d5c7ea36ca7b1e6e34256197df26fe8ac7a006ed53f3a8f1d27d"},
        {"print.duotone",
         "421058b27488480e2ef9a44202a9671eecdd7a3c8bf70928c4f3cd3282b3b4d6"},
        {"print.sepia",
         "ccd7eb5eee714c0cbc47309c40340ab31240b87ca7aeb3ec34155461fc12c2a1"},
        {"glow.bloom",
         "3af08fb174fd931330ef844a7aeb500b599f5842b366495efb0826e808869acc"},
        {"glow.halo",
         "f1745d260ea1c0472a7f4f91d6a207a92505aa0248567d89dd91380d74895809"},
        {"glow.neonnight",
         "1eb22a234caf2d3780a425b89f3da51ca85448f7806323aa8db194c95321c0cc"},
        {"glow.twotone",
         "ee2c074acf7ffc11c1ec9616c957959bba8d485e6f3c2e0f249767fad6391541"},
        {"glow.pop",
         "8fb4c609ace5b348fd5f23a94909a13f28111b0664f1024c0c5c66edf271c1bb"},
        {"glow.daydream",
         "5f5d1505d642dd87b6ba566122eee85d61fc8f66446813e8032fcdba92e1929e"},
        {"glow.chrome",
         "bd0a256f8e87de7e6afc90d9e85a078ad2c5915dc7910f8370dc3a0f9baa34ae"},
        {"glow.sunset",
         "451d33c0004361abcdee4ea634e0e0e7fcb165b2e9c95d15eb114b3ba39cfdbf"},
        {"glow.aurora",
         "25b5c352ba8da5f5e02e6a5a4f749b81dea364c4f0a30d99d5cb639eb9c8f718"},
        {"glow.ember",
         "e3e7c17b1de40007df3936025b8c2c1d8ac43c44de3ad7c4b03d5e5292963de1"},
    };
    const QImage source = scaledSample(QSize(160, 120));
    QHash<QString, QString> expected;
    for (const Hash &entry : hashes)
        expected.insert(QString::fromLatin1(entry.key), QString::fromLatin1(entry.sha256));
    QCOMPARE(expected.size(), 120);

    int checked = 0;
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        QVERIFY2(expected.contains(recipe.key), qPrintable(recipe.key));
        const auto result =
            EffectEngine::instance().render(source, recipe, 100, source.size(), false);
        if (!result.ok()) {
            QFAIL(qPrintable(recipe.key + QStringLiteral(": ") + result.error().message));
        }
        QCOMPARE(QString::fromLatin1(hashImage(result.value()).toHex()),
                 expected.value(recipe.key));
        checked += 1;
    }
    QCOMPARE(checked, 120);
}

void TestEffectEngine::renderCostStaysWithinBudget()
{
    // Measured on this machine, release build: 120 recipes at 160 px cost about
    // 160 ms in total and the slowest single effect (warp.kaleido) about 25 ms.
    // The budget is deliberately loose so a slower machine or a debug build
    // still passes, but it is tight enough to catch a pass that regresses by an
    // order of magnitude. detail.sketch (446 ms) and blur.pan (222 ms) were
    // replaced with F-class recipes for exactly this reason.
    const QImage source = scaledSample(QSize(160, 120));
    qint64 total = 0;
    QString slowestKey;
    qint64 slowest = 0;
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        QElapsedTimer timer;
        timer.start();
        const auto result =
            EffectEngine::instance().render(source, recipe, 100, source.size(), false);
        const qint64 elapsed = timer.elapsed();
        if (!result.ok()) {
            QFAIL(qPrintable(recipe.key + QStringLiteral(": ") + result.error().message));
        }
        total += elapsed;
        if (elapsed > slowest) {
            slowest = elapsed;
            slowestKey = recipe.key;
        }
    }
    QVERIFY2(slowest <= 200,
             qPrintable(QStringLiteral("%1 took %2 ms at 160 px")
                            .arg(slowestKey)
                            .arg(slowest)));
    QVERIFY2(total <= 5000,
             qPrintable(QStringLiteral("the catalogue took %1 ms at 160 px").arg(total)));
}

void TestEffectEngine::wandStagesSurviveThePrintPath()
{
    // A wand pass runs on the grade raster, so the print path must grade at the
    // 2400 px bound, run the pass there, and upscale once. The result must be a
    // real transform, not an identity and not a crash.
    const QImage source = scaledSample(QSize(1200, 900));
    for (const QString &key : {QStringLiteral("warp.swirl"), QStringLiteral("warp.kaleido"),
                               QStringLiteral("art.oil"), QStringLiteral("detail.emboss"),
                               QStringLiteral("blur.motion")}) {
        const auto recipe = EffectCatalogue::instance().resolve(key);
        QVERIFY2(recipe.ok(), qPrintable(key));
        EffectRenderStats stats;
        const auto printed = EffectEngine::instance().render(source, recipe.value(), 100,
                                                             QSize(1600, 1200), true, {}, &stats);
        QVERIFY2(printed.ok(), qPrintable(key));
        QCOMPARE(printed.value().size(), QSize(1600, 1200));
        // 1600 px is under the grade bound, so the pass runs on the target
        // raster itself; the 2400 px cap is checked below.
        QCOMPARE(stats.gradeSize, QSize(1600, 1200));
        const QImage downscaled = printed.value().scaled(
            QSize(240, 180), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const auto preview =
            EffectEngine::instance().render(source.scaled(QSize(240, 180)), recipe.value(), 100,
                                            QSize(240, 180), false);
        QVERIFY2(preview.ok(), qPrintable(key));
        const double distance = perceptualDistance(preview.value(), downscaled);
        QVERIFY2(distance < 12.0,
                 qPrintable(QStringLiteral("%1 preview and print differ by %2")
                                .arg(key)
                                .arg(distance)));
        QVERIFY2(perceptualDistance(preview.value(), scaledSample(QSize(240, 180))) > 1.0,
                 qPrintable(key));
    }

    // Above the grade bound the pass runs at 2400 px and the print is a single
    // upscale of the graded frame.
    const auto swirl = EffectCatalogue::instance().resolve(QStringLiteral("warp.swirl"));
    QVERIFY(swirl.ok());
    EffectRenderStats capped;
    const auto big = EffectEngine::instance().render(source, swirl.value(), 100,
                                                     QSize(3200, 2400), true, {}, &capped);
    QVERIFY(big.ok());
    QCOMPARE(big.value().size(), QSize(3200, 2400));
    QCOMPARE(capped.gradeSize, QSize(2400, 1800));
}

void TestEffectEngine::halftoneDotPitchScalesWithFinishRaster()
{
    // The dot screen is a finish stage, so it is rendered at the finish raster
    // and the printed dot pitch follows that raster: 8 px at the 2400 reference,
    // 12 px at 3600. Measured on flat frames so the reading is the screen, not
    // the picture in it.
    const auto recipe = EffectCatalogue::instance().resolve(QStringLiteral("print.halftone"));
    QVERIFY(recipe.ok());
    const auto plan = EffectEngine::compile(recipe.value());
    QVERIFY(plan.ok());
    QVERIFY(plan.value().hasFinishStages);
    QCOMPARE(plan.value().wandSteps, 1);
    const auto halftone = plan.value().steps.last();
    QVERIFY(!halftone.point);
    QCOMPARE(halftone.spatial.wandPass, QStringLiteral("halftone"));
    QVERIFY(halftone.spatial.finish);

    double previous = 0.0;
    for (const QSize &size : {QSize(2400, 1800), QSize(3600, 2700)}) {
        QImage frame(size, QImage::Format_RGB32);
        frame.fill(QColor(128, 128, 128));
        const auto applied = applyWandPass(frame, halftone.spatial, size.width());
        QVERIFY2(applied.ok(), qPrintable(halftone.spatial.wandPass));
        const double run = meanRunLength(frame);
        QVERIFY2(run > previous,
                 qPrintable(QStringLiteral("pitch at %1 px: run %2, previous %3")
                                .arg(size.width())
                                .arg(run)
                                .arg(previous)));
        previous = run;
    }

    // The same recipe printed keeps its finish size and stays bilevel.
    const QImage source = scaledSample(QSize(1200, 900));
    const auto printed = EffectEngine::instance().render(source, recipe.value(), 100,
                                                         QSize(1600, 1200), true);
    QVERIFY(printed.ok());
    QCOMPARE(printed.value().size(), QSize(1600, 1200));
    for (int y = 0; y < printed.value().height(); y += 13) {
        for (int x = 0; x < printed.value().width(); x += 7) {
            const QRgb pixel = printed.value().pixel(x, y);
            for (int channel : {qRed(pixel), qGreen(pixel), qBlue(pixel)})
                QVERIFY(channel == 0 || channel == 255);
        }
    }
}

void TestEffectEngine::halftoneScreenIsStableAcrossRasters()
{
    // One screen, one algorithm. The pass is exactly the in-process 8x8 screen
    // at the authored pitch rounded to whole device pixels, whatever the raster:
    // the bug this pins is the old code choosing ImageMagick's threshold map
    // only when the scaled pitch was within 0.05 px of a multiple of eight, so
    // almost every print size silently used a different screen.
    const auto shipped = EffectCatalogue::instance().resolve(QStringLiteral("print.halftone"));
    QVERIFY(shipped.ok());
    EffectRecipe withoutScreen = shipped.value();
    QVERIFY(!withoutScreen.stages.isEmpty());
    QCOMPARE(withoutScreen.stages.last().wandPass, QStringLiteral("halftone"));
    withoutScreen.stages.removeLast();

    QImage flat(1200, 900, QImage::Format_RGB32);
    flat.fill(QColor(128, 128, 128));

    QList<QImage> screens;
    for (const int longSide : {2400, 2432, 2600}) {
        const QSize size(longSide, longSide * 3 / 4);
        // The graded frame the screen sees, from the same recipe with the screen
        // removed.
        const auto graded = EffectEngine::instance().render(flat, withoutScreen, 100, size, true);
        QVERIFY(graded.ok());
        const int period = std::clamp(int(std::lround(8.0 * double(longSide) / 2400.0)), 8, 512);
        QImage expected = graded.value();
        orderedDither8x8(expected, period);

        const auto printed =
            EffectEngine::instance().render(flat, shipped.value(), 100, size, true);
        QVERIFY(printed.ok());
        QCOMPARE(printed.value().size(), size);
        QCOMPARE(hashImage(printed.value()), hashImage(expected));
        screens.append(printed.value().convertToFormat(QImage::Format_ARGB32));

        // The screen must repeat exactly with its pitch.
        const QImage &screen = screens.last();
        for (int y = 0; y < screen.height(); y += 11)
            for (int x = 0; x + period < screen.width(); x += 13)
                QCOMPARE(screen.pixel(x, y), screen.pixel(x + period, y));
    }

    // 2400 and 2432 both land on an 8 px cell, so the screens are bit-identical
    // over the region they share.
    QCOMPARE(screens.size(), 3);
    int worst = 0;
    for (int y = 0; y < 1800; ++y)
        for (int x = 0; x < 2400; ++x) {
            const QRgb a = screens.at(0).pixel(x, y);
            const QRgb b = screens.at(1).pixel(x, y);
            worst = std::max({worst, std::abs(qRed(a) - qRed(b)),
                              std::abs(qGreen(a) - qGreen(b)),
                              std::abs(qBlue(a) - qBlue(b))});
        }
    QCOMPARE(worst, 0);

    // 2600 lands on 9 px: the same matrix one pixel coarser, and the coverage
    // of a flat area must not move with the pitch.
    auto coverage = [](const QImage &screen) {
        long long on = 0;
        long long total = 0;
        for (int y = 0; y < screen.height(); y += 7)
            for (int x = 0; x < screen.width(); x += 7) {
                total += 1;
                if (qGray(screen.pixel(x, y)) > 127)
                    on += 1;
            }
        return double(on) / double(total);
    };
    const double base = coverage(screens.at(0));
    const double coarse = coverage(screens.at(2));
    QVERIFY2(std::abs(base - coarse) < 0.01,
             qPrintable(QStringLiteral("coverage %1 at 8 px vs %2 at 9 px")
                            .arg(base)
                            .arg(coarse)));
}

void TestEffectEngine::mirrorReflectsTheLeftHalf()
{
    // The mirror is the left half reflected onto the right, so the frame is
    // exactly symmetric about its centre axis and the axis itself is smooth.
    // The bug this pins: flopping the frame and cropping the *left* half of the
    // flopped copy produced [left | flipped right] with a hard seam.
    Stage mirror;
    mirror.id = StageId::Wand;
    mirror.idName = QStringLiteral("wand");
    mirror.wandPass = QStringLiteral("mirror");

    QImage ramp(200, 100, QImage::Format_RGB32);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 200; ++x)
            ramp.setPixel(x, y, qRgb(x, x, x));
    QImage mirroredRamp = ramp;
    QVERIFY(applyWandPass(mirroredRamp, mirror, 200).ok());

    int worstAsymmetry = 0;
    int leftChanged = 0;
    int seam = 0;
    for (int y = 0; y < mirroredRamp.height(); ++y) {
        for (int x = 0; x < mirroredRamp.width(); ++x) {
            const QRgb left = mirroredRamp.pixel(x, y);
            const QRgb right = mirroredRamp.pixel(mirroredRamp.width() - 1 - x, y);
            worstAsymmetry = std::max({worstAsymmetry, std::abs(qRed(left) - qRed(right)),
                                       std::abs(qGreen(left) - qGreen(right)),
                                       std::abs(qBlue(left) - qBlue(right))});
            if (x < mirroredRamp.width() / 2) {
                leftChanged = std::max(leftChanged,
                                       std::abs(qGray(left) - qGray(ramp.pixel(x, y))));
            }
        }
        const int axis = mirroredRamp.width() / 2;
        seam = std::max(seam, std::abs(qGray(mirroredRamp.pixel(axis - 1, y))
                                       - qGray(mirroredRamp.pixel(axis, y))));
    }
    QCOMPARE(worstAsymmetry, 0);
    QCOMPARE(leftChanged, 0);
    // The axis joins consecutive columns of the ramp, so it cannot step by more
    // than one grey level. A seam shows up as a jump of half the ramp.
    QVERIFY2(seam <= 1, qPrintable(QStringLiteral("axis step %1").arg(seam)));

    // The shipped recipe adds a contrast stage; a per-pixel curve keeps the
    // symmetry exact, so the sample frame must be symmetric too.
    const auto recipe = EffectCatalogue::instance().resolve(QStringLiteral("warp.mirror"));
    QVERIFY(recipe.ok());
    const QImage sample = scaledSample(QSize(240, 180));
    const auto rendered =
        EffectEngine::instance().render(sample, recipe.value(), 100, sample.size(), false);
    QVERIFY(rendered.ok());
    int sampleAsymmetry = 0;
    for (int y = 0; y < rendered.value().height(); ++y) {
        for (int x = 0; x < rendered.value().width(); ++x) {
            const QRgb left = rendered.value().pixel(x, y);
            const QRgb right = rendered.value().pixel(rendered.value().width() - 1 - x, y);
            sampleAsymmetry = std::max({sampleAsymmetry, std::abs(qRed(left) - qRed(right)),
                                        std::abs(qGreen(left) - qGreen(right)),
                                        std::abs(qBlue(left) - qBlue(right))});
        }
    }
    QCOMPARE(sampleAsymmetry, 0);
}

void TestEffectEngine::alphaSurvivesAWandPass()
{
    // Geometric passes smear the alpha plane, so the engine hands the wand RGB
    // only and restores the source alpha. A halftone in the finish path has to
    // keep the same promise.
    QImage source(120, 90, QImage::Format_ARGB32);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x)
            source.setPixel(x, y, qRgba(40 + x, 90 + y, 150, (x * 2) & 0xff));
    }
    for (const QString &key : {QStringLiteral("warp.mirror"), QStringLiteral("print.halftone"),
                               QStringLiteral("art.mosaic")}) {
        const auto recipe = EffectCatalogue::instance().resolve(key);
        QVERIFY2(recipe.ok(), qPrintable(key));
        const auto result =
            EffectEngine::instance().render(source, recipe.value(), 100, source.size(), false);
        QVERIFY2(result.ok(), qPrintable(key));
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                QCOMPARE(qAlpha(result.value().pixel(x, y)), qAlpha(source.pixel(x, y)));
            }
        }
    }
}

QTEST_MAIN(TestEffectEngine)
#include "tst_effect_engine.moc"
