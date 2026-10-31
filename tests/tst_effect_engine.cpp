#include "ColorPipeline.h"
#include "EffectCatalogue.h"
#include "EffectEngine.h"
#include "ImageSource.h"
#include "PhotoFilters.h"
#include "ProjectStore.h"

#include <QColorSpace>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
