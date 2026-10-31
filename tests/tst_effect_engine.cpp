#include "ColorPipeline.h"
#include "EffectCatalogue.h"
#include "EffectEngine.h"
#include "ImageSource.h"
#include "LayoutEngine.h"
#include "PageRenderer.h"
#include "PhotoFilters.h"
#include "ProjectStore.h"

#include <QColor>
#include <QColorSpace>
#include <QCryptographicHash>
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
    void everySignatureLookChangesPixels();
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

void TestEffectEngine::everySignatureLookChangesPixels()
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
    const QImage source = scaledSample(QSize(240, 180));
    QList<QImage> looks;
    QList<QString> keys;
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        const auto result =
            EffectEngine::instance().render(source, recipe, 100, source.size(), false);
        QVERIFY(result.ok());
        double luma = 0.0;
        for (int y = 0; y < result.value().height(); ++y)
            for (int x = 0; x < result.value().width(); ++x)
                luma += qGray(result.value().pixel(x, y));
        luma /= double(result.value().width()) * result.value().height();
        QVERIFY2(luma > 15.0 && luma < 245.0,
                 qPrintable(QStringLiteral("%1 mean luma %2").arg(recipe.key).arg(luma)));
        looks.append(result.value());
        keys.append(recipe.key);
    }
    for (int i = 0; i < looks.size(); ++i) {
        for (int j = i + 1; j < looks.size(); ++j) {
            const double difference = meanDifference(looks.at(i), looks.at(j));
            QVERIFY2(difference > 0.8,
                     qPrintable(QStringLiteral("%1 vs %2 differ by %3")
                                    .arg(keys.at(i), keys.at(j))
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
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        const Golden *golden = nullptr;
        for (const Golden &candidate : goldens) {
            if (recipe.key == QLatin1String(candidate.key))
                golden = &candidate;
        }
        QVERIFY2(golden != nullptr, qPrintable(recipe.key));
        QCOMPARE(parameters.value(recipe.key), QString::fromLatin1(golden->parameters));
        const auto result =
            EffectEngine::instance().render(source, recipe, 100, source.size(), false);
        QVERIFY(result.ok());
        QCOMPARE(QString::fromLatin1(hashImage(result.value()).toHex()),
                 QString::fromLatin1(golden->render));
        checked += 1;
    }
    QCOMPARE(checked, 10);
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

QTEST_MAIN(TestEffectEngine)
#include "tst_effect_engine.moc"
