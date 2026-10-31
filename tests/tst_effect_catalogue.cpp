#include "EffectCatalogue.h"
#include "EffectEngine.h"
#include "ImageSource.h"

#include <QColor>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtTest>

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

} // namespace

class TestEffectCatalogue : public QObject
{
    Q_OBJECT

private slots:
    void embeddedCatalogueLoads();
    void categoriesAreTheTwelve();
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
};

void TestEffectCatalogue::embeddedCatalogueLoads()
{
    const EffectCatalogue &catalogue = EffectCatalogue::instance();
    QVERIFY(!catalogue.isEmpty());
    QCOMPARE(catalogue.recipes().size(), 10);
    for (const EffectRecipe &recipe : catalogue.recipes()) {
        QVERIFY(!recipe.key.isEmpty());
        QVERIFY(kCategories.contains(recipe.category));
        QVERIFY(recipe.version >= 1);
        QVERIFY(recipe.key.startsWith(recipe.category + QLatin1Char('.')));
    }
}

void TestEffectCatalogue::categoriesAreTheTwelve()
{
    const EffectCatalogue &catalogue = EffectCatalogue::instance();
    QCOMPARE(catalogue.categoryKeys(), kCategories);
    for (const QString &key : kCategories)
        QVERIFY(!catalogue.categoryName(key).isEmpty());
}

void TestEffectCatalogue::recipesCompile()
{
    for (const EffectRecipe &recipe : EffectCatalogue::instance().recipes()) {
        const auto plan = EffectEngine::compile(recipe);
        QVERIFY(plan.ok());
        QVERIFY(!plan.value().steps.isEmpty());
        QVERIFY(plan.value().pointSteps + plan.value().spatialSteps
                == plan.value().steps.size());
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
    QCOMPARE(recipes.size(), 10);
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

QTEST_MAIN(TestEffectCatalogue)
#include "tst_effect_catalogue.moc"
