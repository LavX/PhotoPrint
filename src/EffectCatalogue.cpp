#include "EffectCatalogue.h"

#include "WandStages.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <mutex>

// Defined at global scope: Q_INIT_RESOURCE declares the initialiser at the
// scope of the call, and the generated symbol has no namespace. Keeping this
// out of pp and out of an anonymous namespace is what lets a static library
// pull the resource object in.
void ppInitPhotoPrintResources()
{
    Q_INIT_RESOURCE(resources);
}

namespace pp {

StagePhase stagePhase(StageId id)
{
    switch (id) {
    case StageId::WhiteBalance:
    case StageId::Exposure:
    case StageId::Gamma:
    case StageId::Contrast:
    case StageId::Curves:
    case StageId::Levels:
    case StageId::Fade:
    case StageId::Posterize:
    case StageId::Invert:
        return StagePhase::PointTone;
    case StageId::ColorMatrix:
    case StageId::Saturation:
    case StageId::Vibrance:
    case StageId::HueShift:
    case StageId::SplitTone:
    case StageId::GradientMap:
    case StageId::Lut3d:
        return StagePhase::PointColour;
    case StageId::Blur:
    case StageId::Unsharp:
    case StageId::LocalContrast:
    case StageId::Bloom:
    case StageId::SoftFocus:
    case StageId::Grain:
        return StagePhase::Spatial;
    case StageId::Vignette:
    case StageId::LightLeak:
    case StageId::ExposureMask:
    case StageId::Texture:
        return StagePhase::Light;
    case StageId::Wand:
        return StagePhase::Wand;
    }
    return StagePhase::PointTone;
}

QString stageName(StageId id)
{
    switch (id) {
    case StageId::WhiteBalance: return QStringLiteral("whiteBalance");
    case StageId::Exposure: return QStringLiteral("exposure");
    case StageId::Gamma: return QStringLiteral("gamma");
    case StageId::Contrast: return QStringLiteral("contrast");
    case StageId::Curves: return QStringLiteral("curves");
    case StageId::Levels: return QStringLiteral("levels");
    case StageId::Fade: return QStringLiteral("fade");
    case StageId::Posterize: return QStringLiteral("posterize");
    case StageId::Invert: return QStringLiteral("invert");
    case StageId::ColorMatrix: return QStringLiteral("colorMatrix");
    case StageId::Saturation: return QStringLiteral("saturation");
    case StageId::Vibrance: return QStringLiteral("vibrance");
    case StageId::HueShift: return QStringLiteral("hueShift");
    case StageId::SplitTone: return QStringLiteral("splitTone");
    case StageId::GradientMap: return QStringLiteral("gradientMap");
    case StageId::Lut3d: return QStringLiteral("lut3d");
    case StageId::Blur: return QStringLiteral("blur");
    case StageId::Unsharp: return QStringLiteral("unsharp");
    case StageId::LocalContrast: return QStringLiteral("localContrast");
    case StageId::Bloom: return QStringLiteral("bloom");
    case StageId::SoftFocus: return QStringLiteral("softFocus");
    case StageId::Grain: return QStringLiteral("grain");
    case StageId::Vignette: return QStringLiteral("vignette");
    case StageId::LightLeak: return QStringLiteral("lightLeak");
    case StageId::ExposureMask: return QStringLiteral("exposureMask");
    case StageId::Texture: return QStringLiteral("texture");
    case StageId::Wand: return QStringLiteral("wand");
    }
    return {};
}

bool stageFromName(const QString &name, StageId *id)
{
    static const QHash<QString, StageId> table = [] {
        QHash<QString, StageId> map;
        for (int i = 0; i <= static_cast<int>(StageId::Wand); ++i) {
            const auto value = static_cast<StageId>(i);
            map.insert(stageName(value), value);
        }
        return map;
    }();
    const auto it = table.constFind(name);
    if (it == table.constEnd())
        return false;
    if (id)
        *id = it.value();
    return true;
}

namespace {

void ensureEffectResources()
{
    // The catalogue and the textures live in a static library, so the resource
    // object is not linked unless something references its initialiser.
    static std::once_flag flag;
    std::call_once(flag, [] { ::ppInitPhotoPrintResources(); });
}

const QStringList kCategoryKeys = {
    QStringLiteral("basic"), QStringLiteral("mono"),  QStringLiteral("retro"),
    QStringLiteral("color"), QStringLiteral("blur"),  QStringLiteral("detail"),
    QStringLiteral("art"),   QStringLiteral("warp"),  QStringLiteral("light"),
    QStringLiteral("grain"), QStringLiteral("print"), QStringLiteral("glow"),
};

Error parseError(const QString &where, const QString &what)
{
    return error(QStringLiteral("catalogue-invalid"),
                 QStringLiteral("%1: %2").arg(where, what));
}

bool hasUnknown(const QJsonObject &object, const QSet<QString> &allowed, QString *field)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            if (field)
                *field = it.key();
            return true;
        }
    }
    return false;
}

double clamped(const QJsonObject &object, const QString &field, double fallback, double low,
               double high)
{
    const QJsonValue value = object.value(field);
    if (!value.isDouble())
        return fallback;
    const double raw = value.toDouble();
    if (!std::isfinite(raw))
        return fallback;
    return std::clamp(raw, low, high);
}

QColor colourValue(const QJsonValue &value, const QColor &fallback)
{
    if (!value.isString())
        return fallback;
    const QColor colour(value.toString());
    return colour.isValid() ? colour : fallback;
}

bool readCurve(const QJsonObject &object, const QString &field, QList<QPointF> *out, Error *err,
               const QString &where)
{
    const QJsonValue value = object.value(field);
    if (value.isUndefined())
        return true;
    if (!value.isArray()) {
        *err = parseError(where, QStringLiteral("'%1' must be an array").arg(field));
        return false;
    }
    const QJsonArray points = value.toArray();
    if (points.size() < 2 || points.size() > 8) {
        *err = parseError(where, QStringLiteral("'%1' needs 2..8 points").arg(field));
        return false;
    }
    double previous = -1.0;
    for (const QJsonValue &entry : points) {
        const QJsonArray pair = entry.toArray();
        if (pair.size() != 2 || !pair.at(0).isDouble() || !pair.at(1).isDouble()) {
            *err = parseError(where, QStringLiteral("'%1' points must be [x, y]").arg(field));
            return false;
        }
        const double x = pair.at(0).toDouble();
        const double y = pair.at(1).toDouble();
        if (!std::isfinite(x) || !std::isfinite(y) || x <= previous || x < 0.0 || x > 1.0
            || y < 0.0 || y > 1.0) {
            *err = parseError(where, QStringLiteral("'%1' points are out of order or range").arg(field));
            return false;
        }
        previous = x;
        out->append(QPointF(x, y));
    }
    return true;
}

bool readStops(const QJsonObject &object, QList<GradientStop> *out, Error *err,
               const QString &where)
{
    const QJsonValue value = object.value(QStringLiteral("stops"));
    if (value.isUndefined())
        return true;
    if (!value.isArray()) {
        *err = parseError(where, QStringLiteral("'stops' must be an array"));
        return false;
    }
    const QJsonArray stops = value.toArray();
    if (stops.size() < 2 || stops.size() > 4) {
        *err = parseError(where, QStringLiteral("'stops' needs 2..4 entries"));
        return false;
    }
    for (int i = 0; i < stops.size(); ++i) {
        if (!stops.at(i).isObject()) {
            *err = parseError(where, QStringLiteral("'stops' entries must be objects"));
            return false;
        }
        const QJsonObject entry = stops.at(i).toObject();
        QString unknown;
        if (hasUnknown(entry,
                       {QStringLiteral("position"), QStringLiteral("colour")},
                       &unknown)) {
            *err = parseError(where,
                              QStringLiteral("unknown stop field '%1'").arg(unknown));
            return false;
        }
        const double fallback = stops.size() == 1 ? 0.0
                                                  : double(i) / double(stops.size() - 1);
        GradientStop stop;
        stop.position = clamped(entry, QStringLiteral("position"), fallback, 0.0, 1.0);
        stop.colour = colourValue(entry.value(QStringLiteral("colour")), QColor(0, 0, 0));
        if (i > 0 && stop.position < out->last().position) {
            *err = parseError(where, QStringLiteral("'stops' positions must not decrease"));
            return false;
        }
        out->append(stop);
    }
    return true;
}

bool readMask(const QJsonObject &object, StageMask *mask, Error *err, const QString &where)
{
    const QJsonValue value = object.value(QStringLiteral("mask"));
    if (value.isUndefined())
        return true;
    if (!value.isObject()) {
        *err = parseError(where, QStringLiteral("'mask' must be an object"));
        return false;
    }
    const QJsonObject entry = value.toObject();
    QString unknown;
    if (hasUnknown(entry, {QStringLiteral("kind"), QStringLiteral("centre"),
                           QStringLiteral("start"), QStringLiteral("end"),
                           QStringLiteral("feather")},
                   &unknown)) {
        *err = parseError(where, QStringLiteral("unknown field '%1'").arg(unknown));
        return false;
    }
    const QString kind = entry.value(QStringLiteral("kind")).toString(QStringLiteral("none"));
    if (kind == QLatin1String("none"))
        mask->kind = MaskKind::None;
    else if (kind == QLatin1String("linear"))
        mask->kind = MaskKind::Linear;
    else if (kind == QLatin1String("radial"))
        mask->kind = MaskKind::Radial;
    else {
        *err = parseError(where, QStringLiteral("unknown mask kind '%1'").arg(kind));
        return false;
    }
    const QJsonArray centre = entry.value(QStringLiteral("centre")).toArray();
    if (centre.size() == 2) {
        mask->centre = QPointF(std::clamp(centre.at(0).toDouble(), 0.0, 1.0),
                               std::clamp(centre.at(1).toDouble(), 0.0, 1.0));
    }
    mask->start = clamped(entry, QStringLiteral("start"), mask->start, 0.0, 1.0);
    mask->end = clamped(entry, QStringLiteral("end"), mask->end, 0.0, 1.0);
    mask->feather = clamped(entry, QStringLiteral("feather"), mask->feather, 0.0, 1.0);
    if (mask->end < mask->start)
        std::swap(mask->start, mask->end);
    return true;
}

/// The threshold maps this build may name. The list is the set the research
/// verified against ImageMagick 7.1.2; a map outside it is a load error rather
/// than a runtime surprise, and when no map can be loaded at all the halftone
/// pass falls back to the in-process 8x8 screen.
const QStringList &halftoneMaps()
{
    static const QStringList maps = {
        QStringLiteral("h4x4a"),   QStringLiteral("h6x6a"),   QStringLiteral("h8x8a"),
        QStringLiteral("h4x4o"),   QStringLiteral("h6x6o"),   QStringLiteral("h8x8o"),
        QStringLiteral("h16x16o"), QStringLiteral("o2x2"),    QStringLiteral("o3x3"),
        QStringLiteral("o4x4"),    QStringLiteral("o8x8"),    QStringLiteral("c5x5b"),
        QStringLiteral("c6x6b"),   QStringLiteral("c7x7b"),   QStringLiteral("checks"),
        QStringLiteral("threshold")};
    return maps;
}

/// The numeric fields a single wand pass accepts, beyond `id`, `finish` and
/// `pass`. A field that belongs to another pass is a load error, so a wrong
/// parameter name cannot quietly become a default.
QSet<QString> wandFieldsForPass(const QString &pass)
{
    if (pass == QLatin1String("swirl"))
        return {QStringLiteral("degrees")};
    if (pass == QLatin1String("implode"))
        return {QStringLiteral("amount")};
    if (pass == QLatin1String("wave"))
        return {QStringLiteral("amplitude"), QStringLiteral("length")};
    if (pass == QLatin1String("rotationalBlur"))
        return {QStringLiteral("angle")};
    if (pass == QLatin1String("motionBlur"))
        return {QStringLiteral("radius"), QStringLiteral("sigma"), QStringLiteral("angle")};
    if (pass == QLatin1String("oilPaint") || pass == QLatin1String("edge"))
        return {QStringLiteral("radius")};
    if (pass == QLatin1String("sketch") || pass == QLatin1String("charcoal")
        || pass == QLatin1String("emboss") || pass == QLatin1String("kuwahara"))
        return {QStringLiteral("sigma")};
    if (pass == QLatin1String("halftone"))
        return {QStringLiteral("map"), QStringLiteral("size")};
    if (pass == QLatin1String("mirror"))
        return {};
    if (pass == QLatin1String("kaleido"))
        return {QStringLiteral("segments")};
    if (pass == QLatin1String("stretch"))
        return {QStringLiteral("factor")};
    if (pass == QLatin1String("pixelate"))
        return {QStringLiteral("size")};
    return {};
}

/// Reads a required numeric field. A missing parameter is a load error: a wand
/// pass with a silent default would render a different look than the recipe
/// reads.
bool requiredNumber(const QJsonObject &object, const QString &field, double low, double high,
                    double *out, Error *err, const QString &where)
{
    const QJsonValue value = object.value(field);
    if (!value.isDouble()) {
        *err = parseError(where, QStringLiteral("'%1' is required and must be a number")
                                    .arg(field));
        return false;
    }
    const double raw = value.toDouble();
    if (!std::isfinite(raw)) {
        *err = parseError(where, QStringLiteral("'%1' must be finite").arg(field));
        return false;
    }
    *out = std::clamp(raw, low, high);
    return true;
}

bool blendIsAllowed(BlendMode mode, std::initializer_list<BlendMode> allowed)
{
    for (BlendMode candidate : allowed) {
        if (candidate == mode)
            return true;
    }
    return false;
}

bool readBlend(const QJsonObject &object, BlendMode fallback, BlendMode *out,
               std::initializer_list<BlendMode> allowed)
{
    const QString name = object.value(QStringLiteral("blend")).toString();
    BlendMode mode = fallback;
    if (!name.isEmpty()) {
        if (name == QLatin1String("screen"))
            mode = BlendMode::Screen;
        else if (name == QLatin1String("multiply"))
            mode = BlendMode::Multiply;
        else if (name == QLatin1String("softlight"))
            mode = BlendMode::SoftLight;
        else if (name == QLatin1String("overlay"))
            mode = BlendMode::Overlay;
        else if (name == QLatin1String("add"))
            mode = BlendMode::Add;
        else
            return false;
    }
    // A stage that ignores a blend mode must reject it at load time rather
    // than silently rendering a different look than the recipe says.
    if (!blendIsAllowed(mode, allowed))
        return false;
    *out = mode;
    return true;
}

QSet<QString> allowedFields(StageId id)
{
    QSet<QString> fields{QStringLiteral("id"), QStringLiteral("finish")};
    switch (id) {
    case StageId::WhiteBalance:
        fields += {QStringLiteral("temperature"), QStringLiteral("tint")};
        break;
    case StageId::Exposure:
        fields += {QStringLiteral("ev")};
        break;
    case StageId::Gamma:
        fields += {QStringLiteral("gamma")};
        break;
    case StageId::Contrast:
        fields += {QStringLiteral("amount"), QStringLiteral("pivot")};
        break;
    case StageId::Curves:
        fields += {QStringLiteral("master"), QStringLiteral("r"), QStringLiteral("g"),
                   QStringLiteral("b")};
        break;
    case StageId::Levels:
        fields += {QStringLiteral("black"), QStringLiteral("white"), QStringLiteral("gamma")};
        break;
    case StageId::Fade:
        fields += {QStringLiteral("lift"), QStringLiteral("toe"), QStringLiteral("roll")};
        break;
    case StageId::Posterize:
        fields += {QStringLiteral("levels"), QStringLiteral("dither")};
        break;
    case StageId::Invert:
        break;
    case StageId::ColorMatrix:
        fields += {QStringLiteral("matrix")};
        break;
    case StageId::Saturation:
    case StageId::Vibrance:
        fields += {QStringLiteral("amount")};
        break;
    case StageId::HueShift:
        fields += {QStringLiteral("degrees")};
        break;
    case StageId::SplitTone:
        fields += {QStringLiteral("shadow"), QStringLiteral("shadowStrength"),
                   QStringLiteral("highlight"), QStringLiteral("highlightStrength"),
                   QStringLiteral("balance")};
        break;
    case StageId::GradientMap:
        fields += {QStringLiteral("stops"), QStringLiteral("mix")};
        break;
    case StageId::Lut3d:
        fields += {QStringLiteral("lut")};
        break;
    case StageId::Blur:
        fields += {QStringLiteral("radius"), QStringLiteral("passes"), QStringLiteral("mask")};
        break;
    case StageId::Unsharp:
        fields += {QStringLiteral("radius"), QStringLiteral("amount"), QStringLiteral("threshold")};
        break;
    case StageId::LocalContrast:
        fields += {QStringLiteral("radius"), QStringLiteral("amount")};
        break;
    case StageId::Bloom:
        fields += {QStringLiteral("threshold"), QStringLiteral("radius"),
                   QStringLiteral("intensity"), QStringLiteral("tint")};
        break;
    case StageId::SoftFocus:
        fields += {QStringLiteral("radius"), QStringLiteral("amount"), QStringLiteral("blend")};
        break;
    case StageId::Grain:
        fields += {QStringLiteral("amount"), QStringLiteral("size"), QStringLiteral("seed"),
                   QStringLiteral("mono")};
        break;
    case StageId::Vignette:
        fields += {QStringLiteral("amount"), QStringLiteral("midpoint"),
                   QStringLiteral("feather"), QStringLiteral("roundness")};
        break;
    case StageId::LightLeak:
        fields += {QStringLiteral("colour"), QStringLiteral("centre"), QStringLiteral("radius"),
                   QStringLiteral("intensity"), QStringLiteral("softness"),
                   QStringLiteral("blend")};
        break;
    case StageId::ExposureMask:
        fields += {QStringLiteral("kind"), QStringLiteral("centre"), QStringLiteral("size"),
                   QStringLiteral("ev"), QStringLiteral("feather")};
        break;
    case StageId::Texture:
        fields += {QStringLiteral("asset"), QStringLiteral("opacity"), QStringLiteral("blend"),
                   QStringLiteral("scale")};
        break;
    case StageId::Wand:
        fields += {QStringLiteral("pass")};
        break;
    }
    return fields;
}

Result<Stage> stageFromJson(const QJsonObject &object, const QString &where)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    StageId stageId = StageId::Exposure;
    if (!stageFromName(id, &stageId))
        return parseError(where, QStringLiteral("unknown stage id '%1'").arg(id));
    QSet<QString> fields = allowedFields(stageId);
    if (stageId == StageId::Wand) {
        const QString pass = object.value(QStringLiteral("pass")).toString();
        if (!wandPassFromName(pass, nullptr))
            return parseError(where, QStringLiteral("unknown MagickWand pass '%1'").arg(pass));
        fields += wandFieldsForPass(pass);
    }
    QString unknown;
    if (hasUnknown(object, fields, &unknown))
        return parseError(where, QStringLiteral("unknown field '%1' for '%2'")
                                    .arg(unknown, id));

    Stage stage;
    stage.id = stageId;
    stage.idName = id;
    stage.finish = object.value(QStringLiteral("finish")).toBool(false);

    switch (stageId) {
    case StageId::WhiteBalance:
        stage.temperature = clamped(object, QStringLiteral("temperature"), 0.0, -100.0, 100.0);
        stage.tint = clamped(object, QStringLiteral("tint"), 0.0, -100.0, 100.0);
        break;
    case StageId::Exposure:
        stage.ev = clamped(object, QStringLiteral("ev"), 0.0, -3.0, 3.0);
        break;
    case StageId::Gamma:
        stage.gamma = clamped(object, QStringLiteral("gamma"), 1.0, 0.2, 5.0);
        break;
    case StageId::Contrast:
        stage.amount = clamped(object, QStringLiteral("amount"), 0.0, -100.0, 100.0);
        stage.pivot = clamped(object, QStringLiteral("pivot"), 0.45, 0.0, 1.0);
        break;
    case StageId::Curves: {
        Error err;
        if (!readCurve(object, QStringLiteral("master"), &stage.curveMaster, &err, where)
            || !readCurve(object, QStringLiteral("r"), &stage.curveR, &err, where)
            || !readCurve(object, QStringLiteral("g"), &stage.curveG, &err, where)
            || !readCurve(object, QStringLiteral("b"), &stage.curveB, &err, where)) {
            return err;
        }
        if (stage.curveMaster.isEmpty() && stage.curveR.isEmpty() && stage.curveG.isEmpty()
            && stage.curveB.isEmpty())
            return parseError(where, QStringLiteral("'curves' needs at least one channel"));
        break;
    }
    case StageId::Levels:
        stage.black = clamped(object, QStringLiteral("black"), 0.0, 0.0, 1.0);
        stage.white = clamped(object, QStringLiteral("white"), 1.0, 0.0, 1.0);
        stage.gamma = clamped(object, QStringLiteral("gamma"), 1.0, 0.2, 5.0);
        if (stage.white <= stage.black)
            stage.white = std::min(1.0, stage.black + 1.0 / 1024.0);
        break;
    case StageId::Fade:
        stage.lift = clamped(object, QStringLiteral("lift"), 0.0, 0.0, 0.3);
        stage.toe = clamped(object, QStringLiteral("toe"), 0.6, 0.0, 1.0);
        stage.roll = clamped(object, QStringLiteral("roll"), 0.0, 0.0, 0.3);
        break;
    case StageId::Posterize: {
        const double levels = clamped(object, QStringLiteral("levels"), 2.0, 2.0, 32.0);
        stage.levels = int(std::lround(levels));
        stage.dither = object.value(QStringLiteral("dither")).toBool(false);
        // A dithered posterize cannot fold into a 1D LUT: the dither is a
        // spatial decision and gets its own segment.
        if (stage.dither) {
            stage.id = StageId::Posterize;
            stage.finish = false;
        }
        break;
    }
    case StageId::Invert:
        break;
    case StageId::ColorMatrix: {
        const QJsonArray matrix = object.value(QStringLiteral("matrix")).toArray();
        if (matrix.size() != 9)
            return parseError(where, QStringLiteral("'matrix' needs 9 coefficients"));
        for (int i = 0; i < 9; ++i) {
            if (!matrix.at(i).isDouble())
                return parseError(where, QStringLiteral("'matrix' coefficients must be numbers"));
            stage.matrix[static_cast<size_t>(i)] =
                std::clamp(matrix.at(i).toDouble(), -4.0, 4.0);
        }
        break;
    }
    case StageId::Saturation:
    case StageId::Vibrance:
        stage.amount = clamped(object, QStringLiteral("amount"), 0.0, -100.0, 100.0);
        break;
    case StageId::HueShift: {
        const double degrees = object.value(QStringLiteral("degrees")).toDouble(0.0);
        stage.amount = std::clamp(std::isfinite(degrees) ? degrees : 0.0, -180.0, 180.0);
        break;
    }
    case StageId::SplitTone:
        stage.shadow = colourValue(object.value(QStringLiteral("shadow")), QColor(128, 128, 128));
        stage.shadowStrength =
            clamped(object, QStringLiteral("shadowStrength"), 0.0, 0.0, 1.0);
        stage.highlight =
            colourValue(object.value(QStringLiteral("highlight")), QColor(128, 128, 128));
        stage.highlightStrength =
            clamped(object, QStringLiteral("highlightStrength"), 0.0, 0.0, 1.0);
        stage.balance = clamped(object, QStringLiteral("balance"), 0.5, 0.02, 0.98);
        break;
    case StageId::GradientMap: {
        Error err;
        if (!readStops(object, &stage.stops, &err, where))
            return err;
        if (stage.stops.size() < 2)
            return parseError(where, QStringLiteral("'gradientMap' needs 'stops'"));
        stage.mix = clamped(object, QStringLiteral("mix"), 1.0, 0.0, 1.0);
        break;
    }
    case StageId::Lut3d:
        stage.lut = object.value(QStringLiteral("lut")).toString();
        if (stage.lut.isEmpty())
            return parseError(where, QStringLiteral("'lut3d' needs a 'lut' name"));
        break;
    case StageId::Blur: {
        stage.radius = clamped(object, QStringLiteral("radius"), 0.0, 0.0, 200.0);
        stage.passes = int(std::lround(clamped(object, QStringLiteral("passes"), 3.0, 1.0, 3.0)));
        Error err;
        if (!readMask(object, &stage.mask, &err, where))
            return err;
        break;
    }
    case StageId::Unsharp:
        stage.radius = clamped(object, QStringLiteral("radius"), 1.0, 0.0, 50.0);
        stage.amount = clamped(object, QStringLiteral("amount"), 0.8, 0.0, 3.0);
        stage.threshold = clamped(object, QStringLiteral("threshold"), 0.0, 0.0, 0.1);
        break;
    case StageId::LocalContrast:
        stage.radius = clamped(object, QStringLiteral("radius"), 10.0, 0.0, 100.0);
        stage.amount = clamped(object, QStringLiteral("amount"), 0.3, 0.0, 2.0);
        break;
    case StageId::Bloom:
        stage.threshold = clamped(object, QStringLiteral("threshold"), 0.8, 0.0, 1.0);
        stage.radius = clamped(object, QStringLiteral("radius"), 40.0, 0.0, 200.0);
        stage.intensity = clamped(object, QStringLiteral("intensity"), 0.3, 0.0, 1.0);
        stage.tintColour = colourValue(object.value(QStringLiteral("tint")), QColor(255, 255, 255));
        break;
    case StageId::SoftFocus:
        stage.radius = clamped(object, QStringLiteral("radius"), 6.0, 0.0, 100.0);
        stage.amount = clamped(object, QStringLiteral("amount"), 0.2, 0.0, 1.0);
        if (!readBlend(object, BlendMode::Screen, &stage.blend,
                       {BlendMode::Screen, BlendMode::SoftLight, BlendMode::Multiply,
                        BlendMode::Overlay}))
            return parseError(where, QStringLiteral("unknown or unsupported softFocus blend"));
        break;
    case StageId::Grain:
        stage.amount = clamped(object, QStringLiteral("amount"), 0.03, 0.0, 0.2);
        stage.size = clamped(object, QStringLiteral("size"), 1.0, 1.0, 8.0);
        stage.seed = quint32(std::max<qint64>(
            0, object.value(QStringLiteral("seed")).toInteger(0)));
        stage.mono = object.value(QStringLiteral("mono")).toBool(true);
        break;
    case StageId::Vignette:
        stage.amount = clamped(object, QStringLiteral("amount"), 0.2, 0.0, 1.0);
        stage.midpoint = clamped(object, QStringLiteral("midpoint"), 0.6, 0.0, 1.0);
        stage.feather = clamped(object, QStringLiteral("feather"), 0.4, 0.0, 1.0);
        stage.roundness = clamped(object, QStringLiteral("roundness"), 0.0, -1.0, 1.0);
        break;
    case StageId::LightLeak: {
        stage.tintColour = colourValue(object.value(QStringLiteral("colour")),
                                       QColor(255, 168, 96));
        const QJsonArray centre = object.value(QStringLiteral("centre")).toArray();
        if (centre.size() == 2) {
            stage.centre = QPointF(std::clamp(centre.at(0).toDouble(), 0.0, 1.0),
                                   std::clamp(centre.at(1).toDouble(), 0.0, 1.0));
        }
        stage.radius = clamped(object, QStringLiteral("radius"), 0.6, 0.0, 2.0);
        stage.intensity = clamped(object, QStringLiteral("intensity"), 0.5, 0.0, 1.0);
        stage.softness = clamped(object, QStringLiteral("softness"), 0.5, 0.0, 1.0);
        if (!readBlend(object, BlendMode::Screen, &stage.blend,
                       {BlendMode::Screen, BlendMode::Add}))
            return parseError(where, QStringLiteral("unknown or unsupported lightLeak blend"));
        break;
    }
    case StageId::ExposureMask: {
        const QString kind = object.value(QStringLiteral("kind")).toString(QStringLiteral("radial"));
        if (kind == QLatin1String("linear"))
            stage.mask.kind = MaskKind::Linear;
        else if (kind == QLatin1String("radial"))
            stage.mask.kind = MaskKind::Radial;
        else
            return parseError(where, QStringLiteral("unknown exposureMask kind '%1'").arg(kind));
        const QJsonArray centre = object.value(QStringLiteral("centre")).toArray();
        if (centre.size() == 2) {
            stage.centre = QPointF(std::clamp(centre.at(0).toDouble(), 0.0, 1.0),
                                   std::clamp(centre.at(1).toDouble(), 0.0, 1.0));
        }
        stage.maskSize = clamped(object, QStringLiteral("size"), 0.5, 0.0, 2.0);
        stage.ev = clamped(object, QStringLiteral("ev"), 0.0, -3.0, 3.0);
        stage.feather = clamped(object, QStringLiteral("feather"), 0.5, 0.0, 1.0);
        break;
    }
    case StageId::Texture:
        stage.asset = object.value(QStringLiteral("asset")).toString();
        if (stage.asset.isEmpty())
            return parseError(where, QStringLiteral("'texture' needs an 'asset'"));
        stage.opacity = clamped(object, QStringLiteral("opacity"), 0.2, 0.0, 1.0);
        stage.scale = clamped(object, QStringLiteral("scale"), 1.0, 0.25, 4.0);
        if (!readBlend(object, BlendMode::Multiply, &stage.blend,
                       {BlendMode::Multiply, BlendMode::SoftLight, BlendMode::Overlay,
                        BlendMode::Screen}))
            return parseError(where, QStringLiteral("unknown or unsupported texture blend"));
        break;
    case StageId::Wand: {
        stage.wandPass = object.value(QStringLiteral("pass")).toString();
        Error err;
        const auto need = [&](const QString &field, double low, double high, double *out) {
            return requiredNumber(object, field, low, high, out, &err, where);
        };
        if (stage.wandPass == QLatin1String("swirl")) {
            if (!need(QStringLiteral("degrees"), -360.0, 360.0, &stage.wandDegrees))
                return err;
        } else if (stage.wandPass == QLatin1String("implode")) {
            if (!need(QStringLiteral("amount"), -1.0, 1.0, &stage.wandAmount))
                return err;
        } else if (stage.wandPass == QLatin1String("wave")) {
            if (!need(QStringLiteral("amplitude"), 0.0, 200.0, &stage.wandAmplitude)
                || !need(QStringLiteral("length"), 2.0, 1000.0, &stage.wandLength))
                return err;
        } else if (stage.wandPass == QLatin1String("rotationalBlur")) {
            if (!need(QStringLiteral("angle"), -180.0, 180.0, &stage.wandAngle))
                return err;
        } else if (stage.wandPass == QLatin1String("motionBlur")) {
            if (!need(QStringLiteral("radius"), 0.0, 200.0, &stage.wandRadius)
                || !need(QStringLiteral("sigma"), 0.0, 100.0, &stage.wandSigma))
                return err;
            stage.wandAngle = clamped(object, QStringLiteral("angle"), 0.0, -180.0, 180.0);
        } else if (stage.wandPass == QLatin1String("oilPaint")
                   || stage.wandPass == QLatin1String("edge")) {
            if (!need(QStringLiteral("radius"), 0.0, 100.0, &stage.wandRadius))
                return err;
        } else if (stage.wandPass == QLatin1String("sketch")
                   || stage.wandPass == QLatin1String("charcoal")
                   || stage.wandPass == QLatin1String("emboss")
                   || stage.wandPass == QLatin1String("kuwahara")) {
            if (!need(QStringLiteral("sigma"), 0.0, 100.0, &stage.wandSigma))
                return err;
        } else if (stage.wandPass == QLatin1String("halftone")) {
            stage.wandMap = object.value(QStringLiteral("map")).toString();
            if (!stage.wandMap.isEmpty() && !halftoneMaps().contains(stage.wandMap))
                return parseError(where, QStringLiteral("unknown halftone map '%1'")
                                            .arg(stage.wandMap));
            stage.wandSize = clamped(object, QStringLiteral("size"), 8.0, 2.0, 64.0);
            // A dot screen rendered at the grade raster and upscaled becomes
            // grey mush, so the pass is a finish stage whatever the flag says.
            stage.finish = true;
        } else if (stage.wandPass == QLatin1String("kaleido")) {
            const double segments =
                clamped(object, QStringLiteral("segments"), 6.0, 2.0, 12.0);
            stage.wandSegments = int(std::lround(segments));
        } else if (stage.wandPass == QLatin1String("stretch")) {
            // Below 1 the pass would have to pad the frame to crop back to it,
            // so a squeeze is refused rather than silently padded.
            if (!need(QStringLiteral("factor"), 1.0, 2.0, &stage.wandFactor))
                return err;
        } else if (stage.wandPass == QLatin1String("pixelate")) {
            if (!need(QStringLiteral("size"), 2.0, 100.0, &stage.wandSize))
                return err;
        }
        break;
    }
    }
    return stage;
}

Result<EffectRecipe> recipeFromJson(const QJsonObject &object, const QString &where)
{
    QString unknown;
    if (hasUnknown(object,
                   {QStringLiteral("key"), QStringLiteral("category"),
                    QStringLiteral("version"), QStringLiteral("name"),
                    QStringLiteral("description"), QStringLiteral("stages")},
                   &unknown)) {
        return parseError(where, QStringLiteral("unknown field '%1'").arg(unknown));
    }
    EffectRecipe recipe;
    recipe.key = object.value(QStringLiteral("key")).toString();
    recipe.category = object.value(QStringLiteral("category")).toString();
    recipe.version = object.value(QStringLiteral("version")).toInt(0);
    const QJsonValue name = object.value(QStringLiteral("name"));
    // One display name, the English product name. An object here would reintroduce
    // the per-language names the product decision removed.
    if (!name.isString() || name.toString().isEmpty())
        return parseError(where, QStringLiteral("'name' must be a single string"));
    recipe.name = name.toString();
    recipe.description = object.value(QStringLiteral("description")).toString();
    if (recipe.key.isEmpty() || !recipe.key.contains(QLatin1Char('.')))
        return parseError(where, QStringLiteral("'key' must be category.effect"));
    if (!kCategoryKeys.contains(recipe.category))
        return parseError(where, QStringLiteral("unknown category '%1'").arg(recipe.category));
    if (!recipe.key.startsWith(recipe.category + QLatin1Char('.')))
        return parseError(where, QStringLiteral("key does not match its category"));
    if (recipe.version < 1)
        return parseError(where, QStringLiteral("'version' must be at least 1"));

    const QJsonValue stages = object.value(QStringLiteral("stages"));
    if (!stages.isArray())
        return parseError(where, QStringLiteral("'stages' must be an array"));
    const QJsonArray array = stages.toArray();
    if (array.isEmpty() || array.size() > 12)
        return parseError(where, QStringLiteral("'stages' needs 1..12 entries"));
    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject())
            return parseError(where, QStringLiteral("stage %1 is not an object").arg(i));
        const auto stage =
            stageFromJson(array.at(i).toObject(), QStringLiteral("%1 stage %2").arg(where).arg(i));
        if (!stage)
            return stage.error();
        recipe.stages.append(stage.value());
    }
    // A dot screen destroys the grade, so nothing may follow it. This is a data
    // rule, not a compiler accident: the loader refuses the recipe outright.
    for (int i = 0; i + 1 < recipe.stages.size(); ++i) {
        const Stage &stage = recipe.stages.at(i);
        if (stage.id == StageId::Wand && stage.wandPass == QLatin1String("halftone"))
            return parseError(where, QStringLiteral("'halftone' must be the last stage"));
    }
    return recipe;
}

} // namespace

Result<EffectCatalogue> EffectCatalogue::fromJson(const QByteArray &json)
{
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse);
    if (document.isNull() || !document.isObject()) {
        return parseError(QStringLiteral("catalogue"),
                          QStringLiteral("not a JSON object: %1").arg(parse.errorString()));
    }
    const QJsonObject root = document.object();
    QString unknown;
    if (hasUnknown(root,
                   {QStringLiteral("schema"), QStringLiteral("version"),
                    QStringLiteral("categories"), QStringLiteral("recipes")},
                   &unknown)) {
        return parseError(QStringLiteral("catalogue"),
                          QStringLiteral("unknown top-level field '%1'").arg(unknown));
    }

    EffectCatalogue catalogue;
    const QJsonArray categories = root.value(QStringLiteral("categories")).toArray();
    for (const QJsonValue &value : categories) {
        const QJsonObject entry = value.toObject();
        const QString key = entry.value(QStringLiteral("key")).toString();
        const QString name = entry.value(QStringLiteral("name")).toString();
        if (!kCategoryKeys.contains(key) || name.isEmpty())
            return parseError(QStringLiteral("categories"), QStringLiteral("invalid category"));
        if (catalogue.m_categoryNames.contains(key))
            return parseError(QStringLiteral("categories"), QStringLiteral("duplicate category"));
        catalogue.m_categoryNames.insert(key, name);
    }
    if (catalogue.m_categoryNames.size() != kCategoryKeys.size())
        return parseError(QStringLiteral("categories"), QStringLiteral("all twelve categories are required"));
    catalogue.m_categoryKeys = kCategoryKeys;

    const QJsonArray recipes = root.value(QStringLiteral("recipes")).toArray();
    for (const QJsonValue &value : recipes) {
        if (!value.isObject())
            return parseError(QStringLiteral("recipes"), QStringLiteral("entry is not an object"));
        const auto recipe =
            recipeFromJson(value.toObject(), QStringLiteral("recipe '%1'")
                                                 .arg(value.toObject()
                                                          .value(QStringLiteral("key"))
                                                          .toString()));
        if (!recipe)
            return recipe.error();
        const EffectRecipe entry = recipe.value();
        if (catalogue.m_versions.value(entry.key).contains(entry.version))
            return parseError(entry.identity(), QStringLiteral("duplicate key and version"));
        catalogue.m_versions[entry.key].insert(entry.version, entry);
        // The catalogue exposes the newest version of every key, in file order.
        bool replaced = false;
        for (EffectRecipe &existing : catalogue.m_current) {
            if (existing.key == entry.key) {
                if (entry.version > existing.version)
                    existing = entry;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            catalogue.m_current.append(entry);
    }
    return catalogue;
}

Result<EffectRecipe> EffectCatalogue::resolve(const QString &keyWithVersion, int version) const
{
    QString key = keyWithVersion;
    int requested = version;
    const int at = key.lastIndexOf(QLatin1Char('@'));
    if (at > 0) {
        bool ok = false;
        const int parsed = key.mid(at + 1).toInt(&ok);
        if (ok) {
            requested = parsed;
            key = key.left(at);
        }
    }
    const auto versions = m_versions.constFind(key);
    if (versions == m_versions.constEnd())
        return error(QStringLiteral("effect-unknown"),
                     QStringLiteral("Unknown effect '%1'.").arg(keyWithVersion));
    if (requested > 0) {
        const auto exact = versions->constFind(requested);
        if (exact != versions->constEnd())
            return *exact;
        return error(QStringLiteral("effect-version"),
                     QStringLiteral("Effect '%1' has no version %2.")
                         .arg(key)
                         .arg(requested));
    }
    return versions->last();
}

const EffectCatalogue &EffectCatalogue::instance()
{
    ensureEffectResources();
    static const EffectCatalogue catalogue = [] {
        QFile file(QStringLiteral(":/effects/catalogue.json"));
        if (!file.open(QIODevice::ReadOnly)) {
            qCritical("EffectCatalogue: embedded catalogue is missing");
            return EffectCatalogue{};
        }
        auto parsed = EffectCatalogue::fromJson(file.readAll());
        if (!parsed) {
            qCritical("EffectCatalogue: %s", qPrintable(parsed.error().message));
            return EffectCatalogue{};
        }
        return parsed.value();
    }();
    return catalogue;
}

} // namespace pp
