#include "EffectCatalogue.h"
#include "EffectEngine.h"
#include "ImageSource.h"
#include "WandStages.h"

#include <QColor>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace pp;

namespace {

const QStringList kCategories = {
    QStringLiteral("basic"), QStringLiteral("mono"),  QStringLiteral("retro"),
    QStringLiteral("color"), QStringLiteral("blur"),  QStringLiteral("detail"),
    QStringLiteral("art"),   QStringLiteral("warp"),  QStringLiteral("light"),
    QStringLiteral("grain"), QStringLiteral("print"), QStringLiteral("glow"),
};

QByteArray catalogueWith(const QJsonArray &recipes)
{
    QJsonArray categories;
    for (const QString &key : kCategories) {
        categories.append(QJsonObject{{QStringLiteral("key"), key},
                                      {QStringLiteral("name"), key.toUpper()}});
    }
    const QJsonObject root{{QStringLiteral("schema"), QStringLiteral("photoprint.effects/1")},
                           {QStringLiteral("version"), 1},
                           {QStringLiteral("categories"), categories},
                           {QStringLiteral("recipes"), recipes}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QJsonObject recipe(const QString &key, const QString &category, int version,
                   const QJsonArray &stages)
{
    return QJsonObject{{QStringLiteral("key"), key},
                       {QStringLiteral("category"), category},
                       {QStringLiteral("version"), version},
                       {QStringLiteral("name"), QStringLiteral("Test Look")},
                       {QStringLiteral("stages"), stages}};
}

QJsonObject stage(const QString &id, const QJsonObject &extra = {})
{
    QJsonObject object = extra;
    object.insert(QStringLiteral("id"), id);
    return object;
}

QJsonObject wand(const QString &pass, const QJsonObject &extra = {})
{
    QJsonObject object = extra;
    object.insert(QStringLiteral("pass"), pass);
    return stage(QStringLiteral("wand"), object);
}

/// Parses a single-recipe catalogue, the way the loader is exercised for the
/// schema cases.
Result<EffectCatalogue> catalogueFor(const QString &key, const QString &category,
                                     const QJsonArray &stages)
{
    return EffectCatalogue::fromJson(
        catalogueWith(QJsonArray{recipe(key, category, 1, stages)}));
}

/// Mean length of a run of equal grey values along every seventh row: a cheap
/// and stable measure of how coarse a dot screen is.
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

} // namespace

class TestEffectCatalogue : public QObject
{
    Q_OBJECT

private slots:
    void embeddedCatalogueLoads();
    void categoriesAreTheTwelve();
    void everyCategoryShipsTenLooks();
    void recipesCompile();
    void recipesUseSingleStringNames();
    void textureAssetsExist();
    void sepiaDramaUsesPaperFine();
    void versionsResolveAndPin();
    void unknownKeysFail();
    void strictSchemaRejectsTypos();
    void enforceStageBudget();
    void zeroStopGradientIsRejected();
    void unsupportedBlendIsRejected();
    void legacyKeysStayFrozen();
    void everyStageIdIsKnown();
    void effectNamesAreUnique();
    void wandStageSchemaIsStrict();
    void halftoneMustBeLast();
    void everyWandPassIsPresentOrDisabled();
    void halftoneScreenIsDeterministic();
};

void TestEffectCatalogue::embeddedCatalogueLoads()
{
    const EffectCatalogue &catalogue = EffectCatalogue::instance();
    QVERIFY(!catalogue.isEmpty());
    QCOMPARE(catalogue.recipes().size(), 120);
    QSet<QString> keys;
    for (const EffectRecipe &recipe : catalogue.recipes()) {
        QVERIFY(!recipe.key.isEmpty());
        QVERIFY(kCategories.contains(recipe.category));
        QCOMPARE(recipe.version, 1);
        QVERIFY(recipe.key.startsWith(recipe.category + QLatin1Char('.')));
        QVERIFY(!recipe.name.isEmpty());
        QVERIFY(!keys.contains(recipe.key));
        keys.insert(recipe.key);
    }
    QCOMPARE(keys.size(), 120);
}

void TestEffectCatalogue::categoriesAreTheTwelve()
{
    const EffectCatalogue &catalogue = EffectCatalogue::instance();
    QCOMPARE(catalogue.categoryKeys(), kCategories);
    for (const QString &key : kCategories)
        QVERIFY(!catalogue.categoryName(key).isEmpty());
    QCOMPARE(catalogue.categoryName(QStringLiteral("basic")), QStringLiteral("Basics"));
    QCOMPARE(catalogue.categoryName(QStringLiteral("mono")), QStringLiteral("Black & white"));
}

void TestEffectCatalogue::everyCategoryShipsTenLooks()
{
    const EffectCatalogue &catalogue = EffectCatalogue::instance();
    QHash<QString, int> counts;
    for (const EffectRecipe &recipe : catalogue.recipes()) {
        counts[recipe.category] += 1;
        // The key is the identity a project stores, so the category lead is
        // part of the contract, not a naming habit.
        QVERIFY(recipe.key.startsWith(recipe.category + QLatin1Char('.')));
    }
    for (const QString &category : kCategories) {
        QVERIFY2(counts.value(category) == 10,
                 qPrintable(QStringLiteral("%1 has %2 effects")
                                .arg(category)
                                .arg(counts.value(category))));
    }
    QCOMPARE(counts.size(), 12);
}

void TestEffectCatalogue::recipesCompile()
{
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        const auto plan = EffectEngine::compile(recipe);
        if (!plan.ok()) {
            QFAIL(qPrintable(recipe.key + QStringLiteral(": ") + plan.error().message));
        }
        QVERIFY(!plan.value().steps.isEmpty());
        QVERIFY(plan.value().pointSteps + plan.value().spatialSteps
                == plan.value().steps.size());
        QVERIFY(plan.value().wandSteps >= 0);
        QVERIFY(plan.value().wandSteps <= 3);
        bool seenFinish = false;
        for (const EffectPlanStep &step : plan.value().steps) {
            if (step.point) {
                QVERIFY(!step.colourStages.isEmpty() || step.hasCurve);
            } else {
                QVERIFY(!seenFinish || step.spatial.finish);
                seenFinish = seenFinish || step.spatial.finish;
            }
        }
    }
}

void TestEffectCatalogue::recipesUseSingleStringNames()
{
    QFile file(QStringLiteral(":/effects/catalogue.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    QVERIFY(document.isObject());
    const QJsonArray recipes = document.object().value(QStringLiteral("recipes")).toArray();
    QCOMPARE(recipes.size(), 120);
    for (const QJsonValue &value : recipes) {
        const QJsonObject recipe = value.toObject();
        QVERIFY(recipe.value(QStringLiteral("name")).isString());
        QVERIFY(!recipe.value(QStringLiteral("name")).toString().isEmpty());
        QVERIFY(!recipe.contains(QStringLiteral("nameHu")));
        QVERIFY(!recipe.value(QStringLiteral("description")).toString().isEmpty());
    }
}

void TestEffectCatalogue::textureAssetsExist()
{
    for (const QString &name : {QStringLiteral("paper-fine"), QStringLiteral("paper-news"),
                                QStringLiteral("paper-soft"), QStringLiteral("paper-wash"),
                                QStringLiteral("dust-fine")}) {
        const auto texture = effectTexture(name);
        QVERIFY(texture.ok());
        QCOMPARE(texture.value().size(), QSize(512, 512));
    }
    QVERIFY(!effectTexture(QStringLiteral("does-not-exist")).ok());

    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        for (const Stage &stage : recipe.stages) {
            if (stage.id == StageId::Texture)
                QVERIFY(effectTexture(stage.asset).ok());
        }
    }
}

void TestEffectCatalogue::sepiaDramaUsesPaperFine()
{
    const auto recipe = EffectCatalogue::instance().resolve(QStringLiteral("print.sepia"));
    QVERIFY(recipe.ok());
    bool found = false;
    for (const Stage &stage : recipe.value().stages) {
        if (stage.id == StageId::Texture) {
            QCOMPARE(stage.asset, QStringLiteral("paper-fine"));
            found = true;
        }
    }
    QVERIFY(found);
}

void TestEffectCatalogue::versionsResolveAndPin()
{
    const QJsonArray recipes{
        recipe(QStringLiteral("light.venice"), QStringLiteral("light"), 1,
               QJsonArray{stage(QStringLiteral("exposure"),
                                {{QStringLiteral("ev"), 0.5}})}),
        recipe(QStringLiteral("light.venice"), QStringLiteral("light"), 2,
               QJsonArray{stage(QStringLiteral("exposure"),
                                {{QStringLiteral("ev"), -0.5}})}),
    };
    const auto catalogue = EffectCatalogue::fromJson(catalogueWith(recipes));
    QVERIFY(catalogue.ok());

    const auto current = catalogue.value().resolve(QStringLiteral("light.venice"));
    QVERIFY(current.ok());
    QCOMPARE(current.value().version, 2);

    const auto pinned = catalogue.value().resolve(QStringLiteral("light.venice"), 1);
    QVERIFY(pinned.ok());
    QCOMPARE(pinned.value().version, 1);
    QCOMPARE(pinned.value().stages.first().ev, 0.5);

    const auto atSyntax = catalogue.value().resolve(QStringLiteral("light.venice@1"));
    QVERIFY(atSyntax.ok());
    QCOMPARE(atSyntax.value().version, 1);

    const auto missing = catalogue.value().resolve(QStringLiteral("light.venice"), 9);
    QVERIFY(!missing.ok());
    QCOMPARE(missing.error().code, QStringLiteral("effect-version"));
}

void TestEffectCatalogue::unknownKeysFail()
{
    const auto recipe = EffectCatalogue::instance().resolve(QStringLiteral("light.nope"));
    QVERIFY(!recipe.ok());
    QCOMPARE(recipe.error().code, QStringLiteral("effect-unknown"));
}

void TestEffectCatalogue::strictSchemaRejectsTypos()
{
    const QJsonArray unknownStage{recipe(
        QStringLiteral("basic.test"), QStringLiteral("basic"), 1,
        QJsonArray{stage(QStringLiteral("exposre"), {{QStringLiteral("ev"), 0.1}})})};
    QVERIFY(!EffectCatalogue::fromJson(catalogueWith(unknownStage)).ok());

    const QJsonArray unknownField{recipe(
        QStringLiteral("basic.test"), QStringLiteral("basic"), 1,
        QJsonArray{stage(QStringLiteral("exposure"),
                         {{QStringLiteral("ev"), 0.1}, {QStringLiteral("evv"), 0.1}})})};
    QVERIFY(!EffectCatalogue::fromJson(catalogueWith(unknownField)).ok());

    QJsonObject object = recipe(QStringLiteral("basic.test"), QStringLiteral("basic"), 1,
                                QJsonArray{stage(QStringLiteral("exposure"))});
    object.insert(QStringLiteral("name"), 5);
    QVERIFY(!EffectCatalogue::fromJson(catalogueWith({object})).ok());

    QJsonObject wrongCategory =
        recipe(QStringLiteral("light.test"), QStringLiteral("basic"), 1,
               QJsonArray{stage(QStringLiteral("exposure"))});
    QVERIFY(!EffectCatalogue::fromJson(catalogueWith({wrongCategory})).ok());

    const QJsonArray duplicate{
        recipe(QStringLiteral("basic.test"), QStringLiteral("basic"), 1,
               QJsonArray{stage(QStringLiteral("exposure"))}),
        recipe(QStringLiteral("basic.test"), QStringLiteral("basic"), 1,
               QJsonArray{stage(QStringLiteral("exposure"))}),
    };
    QVERIFY(!EffectCatalogue::fromJson(catalogueWith(duplicate)).ok());
}

void TestEffectCatalogue::enforceStageBudget()
{
    QJsonArray stages;
    for (int i = 0; i < 13; ++i)
        stages.append(stage(QStringLiteral("exposure"), {{QStringLiteral("ev"), 0.01}}));
    const QJsonArray tooMany{
        recipe(QStringLiteral("basic.test"), QStringLiteral("basic"), 1, stages)};
    QVERIFY(!EffectCatalogue::fromJson(catalogueWith(tooMany)).ok());

    const QJsonArray none{recipe(QStringLiteral("basic.test"), QStringLiteral("basic"), 1,
                                 QJsonArray{})};
    QVERIFY(!EffectCatalogue::fromJson(catalogueWith(none)).ok());
}

void TestEffectCatalogue::zeroStopGradientIsRejected()
{
    const auto make = [](const QJsonArray &stops) {
        return catalogueWith(QJsonArray{recipe(
            QStringLiteral("print.test"), QStringLiteral("print"), 1,
            QJsonArray{stage(QStringLiteral("gradientMap"),
                             {{QStringLiteral("stops"), stops}})})});
    };
    QVERIFY(!EffectCatalogue::fromJson(make(QJsonArray{})).ok());
    QVERIFY(!EffectCatalogue::fromJson(make(QJsonArray{QJsonObject{
        {QStringLiteral("position"), 0.0},
        {QStringLiteral("colour"), QStringLiteral("#000000")}}})).ok());
    QVERIFY(EffectCatalogue::fromJson(make(QJsonArray{
        QJsonObject{{QStringLiteral("position"), 0.0},
                    {QStringLiteral("colour"), QStringLiteral("#000000")}},
        QJsonObject{{QStringLiteral("position"), 1.0},
                    {QStringLiteral("colour"), QStringLiteral("#ffffff")}}})).ok());
}

void TestEffectCatalogue::unsupportedBlendIsRejected()
{
    const auto catalogueFor = [](const QString &stageId, const QJsonObject &extra) {
        return EffectCatalogue::fromJson(catalogueWith(QJsonArray{recipe(
            QStringLiteral("basic.test"), QStringLiteral("basic"), 1,
            QJsonArray{stage(stageId, extra)})}));
    };
    QVERIFY(!catalogueFor(QStringLiteral("texture"),
                          {{QStringLiteral("asset"), QStringLiteral("paper-fine")},
                           {QStringLiteral("blend"), QStringLiteral("add")}}).ok());
    QVERIFY(!catalogueFor(QStringLiteral("softFocus"),
                          {{QStringLiteral("blend"), QStringLiteral("add")}}).ok());
    QVERIFY(!catalogueFor(QStringLiteral("lightLeak"),
                          {{QStringLiteral("blend"), QStringLiteral("multiply")}}).ok());
    QVERIFY(catalogueFor(QStringLiteral("texture"),
                         {{QStringLiteral("asset"), QStringLiteral("paper-fine")},
                          {QStringLiteral("blend"), QStringLiteral("multiply")}}).ok());
}

void TestEffectCatalogue::legacyKeysStayFrozen()
{
    for (const QString &key :
         {QStringLiteral("original"), QStringLiteral("vivid"), QStringLiteral("warm"),
          QStringLiteral("cool"), QStringLiteral("sepia"), QStringLiteral("bw"),
          QStringLiteral("blackwhite"), QStringLiteral("vintage"), QStringLiteral("crisp")}) {
        QVERIFY(isLegacyPresetKey(key));
        QVERIFY(!EffectCatalogue::instance().resolve(key).ok());
    }
    QVERIFY(!isLegacyPresetKey(QStringLiteral("light.venice")));
}

void TestEffectCatalogue::everyStageIdIsKnown()
{
    const QStringList passes = wandPassNames();
    QCOMPARE(passes.size(), 16);
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        for (int index = 0; index < recipe.stages.size(); ++index) {
            const Stage &stage = recipe.stages.at(index);
            StageId id = StageId::Exposure;
            QVERIFY2(stageFromName(stage.idName, &id),
                     qPrintable(recipe.key + QStringLiteral(": ") + stage.idName));
            QCOMPARE(stage.id, id);
            QCOMPARE(stageName(id), stage.idName);
            const StagePhase phase = stagePhase(id);
            QVERIFY(phase == StagePhase::PointTone || phase == StagePhase::PointColour
                    || phase == StagePhase::Spatial || phase == StagePhase::Light
                    || phase == StagePhase::Wand);

            if (id == StageId::Wand) {
                WandPass pass = WandPass::Swirl;
                QVERIFY2(wandPassFromName(stage.wandPass, &pass),
                         qPrintable(recipe.key + QStringLiteral(": ") + stage.wandPass));
                QCOMPARE(phase, StagePhase::Wand);
                // A wand halftone destroys the grade, so it is finish and last.
                if (pass == WandPass::Halftone) {
                    QVERIFY2(stage.finish, qPrintable(recipe.key));
                    QCOMPARE(index, recipe.stages.size() - 1);
                }
                // Every pass carries at least one authored parameter, except
                // mirror, which needs none.
                if (pass != WandPass::Mirror && pass != WandPass::Halftone)
                    QVERIFY2(!wandRequiredSymbols(pass).isEmpty(),
                             qPrintable(recipe.key));
            }
            if (id == StageId::Grain)
                QVERIFY2(stage.finish, qPrintable(recipe.key));
        }
    }
}

void TestEffectCatalogue::effectNamesAreUnique()
{
    // One name per effect. Two effects sharing a name is two effects a user
    // cannot tell apart in a list, which is how "Newsprint" ended up naming both
    // a retro look and a print look.
    QSet<QString> seen;
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        const QString folded = recipe.name.toCaseFolded().trimmed();
        QVERIFY2(!folded.isEmpty(), qPrintable(recipe.key));
        QVERIFY2(!seen.contains(folded),
                 qPrintable(QStringLiteral("'%1' names both %2 and another effect")
                                .arg(recipe.name, recipe.key)));
        seen.insert(folded);
    }
    QCOMPARE(seen.size(), 120);
}

void TestEffectCatalogue::wandStageSchemaIsStrict()
{
    const auto parse = [](const QJsonArray &stages) {
        return catalogueFor(QStringLiteral("warp.test"), QStringLiteral("warp"), stages);
    };
    // A pass the engine does not implement cannot be named.
    QVERIFY(!parse(QJsonArray{wand(QStringLiteral("lensFlare"))}).ok());
    // A field that belongs to another pass is a load error, not a default.
    QVERIFY(!parse(QJsonArray{wand(QStringLiteral("swirl"),
                                   {{QStringLiteral("radius"), 3.0}})}).ok());
    QVERIFY(!parse(QJsonArray{wand(QStringLiteral("mirror"),
                                   {{QStringLiteral("degrees"), 3.0}})}).ok());
    // The magnitude of every pass is required explicitly.
    QVERIFY(!parse(QJsonArray{wand(QStringLiteral("implode"))}).ok());
    QVERIFY(!parse(QJsonArray{wand(QStringLiteral("wave"),
                                   {{QStringLiteral("amplitude"), 12.0}})}).ok());
    QVERIFY(!parse(QJsonArray{wand(QStringLiteral("oilPaint"))}).ok());
    QVERIFY(!parse(QJsonArray{wand(QStringLiteral("pixelate"))}).ok());
    // The screen is in-process, so a threshold map name is not part of the
    // schema at all: naming one is a load error like any other stray field.
    QVERIFY(!parse(QJsonArray{wand(QStringLiteral("halftone"),
                                   {{QStringLiteral("map"), QStringLiteral("h8x8a")}})}).ok());
    QVERIFY(parse(QJsonArray{wand(QStringLiteral("halftone"),
                                  {{QStringLiteral("size"), 8.0}})}).ok());
    // Valid passes parse, and the values are clamped to their documented range.
    const auto oil = parse(QJsonArray{wand(QStringLiteral("oilPaint"),
                                           {{QStringLiteral("radius"), 400.0}})});
    QVERIFY(oil.ok());
    QCOMPARE(oil.value().recipes().first().stages.first().wandRadius, 100.0);
    const auto mirror = parse(QJsonArray{wand(QStringLiteral("mirror"))});
    QVERIFY(mirror.ok());
    QCOMPARE(mirror.value().recipes().first().stages.first().wandPass,
             QStringLiteral("mirror"));
    // A halftone is finish whether the recipe says so or not.
    const auto halftone = parse(QJsonArray{wand(QStringLiteral("halftone"))});
    QVERIFY(halftone.ok());
    QVERIFY(halftone.value().recipes().first().stages.first().finish);
}

void TestEffectCatalogue::halftoneMustBeLast()
{
    const auto parse = [](const QJsonArray &stages) {
        return catalogueFor(QStringLiteral("print.test"), QStringLiteral("print"), stages);
    };
    QVERIFY(!parse(QJsonArray{wand(QStringLiteral("halftone")),
                             stage(QStringLiteral("saturation"),
                                   {{QStringLiteral("amount"), -100}})}).ok());
    QVERIFY(parse(QJsonArray{stage(QStringLiteral("saturation"),
                                   {{QStringLiteral("amount"), -100}}),
                             wand(QStringLiteral("halftone"))}).ok());

    // The compiler enforces the same rule for a hand-built recipe.
    EffectRecipe recipe;
    recipe.key = QStringLiteral("print.test");
    recipe.category = QStringLiteral("print");
    recipe.stages = {Stage{}, Stage{}};
    recipe.stages[0].id = StageId::Wand;
    recipe.stages[0].idName = QStringLiteral("wand");
    recipe.stages[0].wandPass = QStringLiteral("halftone");
    recipe.stages[0].finish = true;
    recipe.stages[1].id = StageId::Grain;
    recipe.stages[1].idName = QStringLiteral("grain");
    recipe.stages[1].finish = true;
    recipe.stages[1].amount = 0.02;
    const auto plan = EffectEngine::compile(recipe);
    QVERIFY(!plan.ok());
    QCOMPARE(plan.error().code, QStringLiteral("effect-plan"));
}

void TestEffectCatalogue::everyWandPassIsPresentOrDisabled()
{
    // The probe reads the loaded library, not the header: ImageMagick 7
    // declares functions it does not export, and a missing symbol must disable
    // the recipes that need it with a reason rather than render an identity.
    const EffectCatalogue &catalogue = EffectCatalogue::instance();
    for (const QString &name : wandPassNames()) {
        WandPass pass = WandPass::Swirl;
        QVERIFY(wandPassFromName(name, &pass));
        const WandCapability capability = wandCapability(pass);
        // Halftone always works: the in-process screen is the fallback.
        if (pass != WandPass::Halftone)
            QVERIFY2(!wandRequiredSymbols(pass).isEmpty(), qPrintable(name));

        for (const EffectRecipe &recipe : catalogue.recipes()) {
            bool uses = false;
            for (const Stage &stage : recipe.stages)
                uses = uses || (stage.id == StageId::Wand && stage.wandPass == name);
            if (!uses)
                continue;
            const auto plan = EffectEngine::compile(recipe);
            if (capability.available) {
                if (!plan.ok()) {
                    QFAIL(qPrintable(name + QStringLiteral(": ") + recipe.key
                                     + QStringLiteral(": ") + plan.error().message));
                }
            } else {
                QVERIFY(!plan.ok());
                QCOMPARE(plan.error().code, QStringLiteral("effect-unavailable"));
                QVERIFY(!plan.error().message.isEmpty());
                QVERIFY(plan.error().message.contains(name));
            }
        }
    }
    // A pass that is unavailable and unused cannot hide a broken catalogue:
    // the shipped set must use only passes this build can run.
    for (const QString &name : wandPassNames()) {
        WandPass pass = WandPass::Swirl;
        wandPassFromName(name, &pass);
        const WandCapability capability = wandCapability(pass);
        if (capability.available)
            continue;
        bool used = false;
        for (const EffectRecipe &recipe : catalogue.recipes())
            for (const Stage &stage : recipe.stages)
                used = used || (stage.id == StageId::Wand && stage.wandPass == name);
        QVERIFY2(!used, qPrintable(QStringLiteral("shipped recipes need the unavailable "
                                                  "pass %1: %2")
                                       .arg(name, capability.reason)));
    }
}

void TestEffectCatalogue::halftoneScreenIsDeterministic()
{
    QImage source(64, 64, QImage::Format_ARGB32);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x)
            source.setPixel(x, y, qRgba(x * 4, 128, 64, (x * 3) & 0xff));
    }

    QImage first = source;
    QImage second = source;
    orderedDither8x8(first, 8);
    orderedDither8x8(second, 8);
    QCOMPARE(first, second);

    // A screen is bilevel per channel and leaves alpha alone.
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            const QRgb pixel = first.pixel(x, y);
            for (int channel : {qRed(pixel), qGreen(pixel), qBlue(pixel)})
                QVERIFY(channel == 0 || channel == 255);
            QCOMPARE(qAlpha(pixel), qAlpha(source.pixel(x, y)));
        }
    }

    // The dot pitch follows the cell size, which is how the finish raster
    // scales the screen. Below eight pixels a cell cannot hold the matrix: it
    // degenerates into a flat patch, which is why the pass floors the pitch.
    QImage fine = source;
    QImage coarse = source;
    orderedDither8x8(fine, 8);
    orderedDither8x8(coarse, 16);
    QVERIFY2(meanRunLength(coarse) > meanRunLength(fine) * 1.5,
             qPrintable(QStringLiteral("%1 vs %2")
                            .arg(meanRunLength(fine))
                            .arg(meanRunLength(coarse))));
}

QTEST_MAIN(TestEffectCatalogue)
#include "tst_effect_catalogue.moc"
