#include "CollageTemplates.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

// Defined at global scope by EffectCatalogue.cpp: Q_INIT_RESOURCE declares the
// initialiser at the scope of the call, and the generated symbol has no
// namespace. Keeping the declaration out of pp is what lets a static library
// pull the resource object in.
void ppInitPhotoPrintResources();

namespace pp {

namespace {

constexpr int kLattice = 1000;
constexpr int kMinSlotMm = 15;
constexpr int kExpectedTemplates = 55;
constexpr int kTemplatesPerCount = 6;
constexpr int kMaxPhotoCount = 10;

/// Height of the catalogue rounding contract: every coordinate is an integer
/// thousandth, so tiling is checked in the integer domain and needs no epsilon.
using LatticeRect = std::array<int, 4>;

void ensureCollageResources()
{
    static std::once_flag flag;
    std::call_once(flag, [] { ::ppInitPhotoPrintResources(); });
}

const QStringList &familyKeys()
{
    static const QStringList keys = {
        QStringLiteral("full"),   QStringLiteral("equal"),  QStringLiteral("hero"),
        QStringLiteral("strip"),  QStringLiteral("mosaic"), QStringLiteral("frame"),
        QStringLiteral("bands"),  QStringLiteral("columns")};
    return keys;
}

const QStringList &aspectKeys()
{
    static const QStringList keys = {QStringLiteral("any"), QStringLiteral("portrait"),
                                     QStringLiteral("landscape"), QStringLiteral("square")};
    return keys;
}

Error invalid(const QString &where, const QString &what)
{
    return error(QStringLiteral("collage-catalogue-invalid"),
                 QStringLiteral("%1: %2").arg(where, what));
}

/// One message for the whole 15 mm rule: it is the only reason a template is
/// refused on a paper, and the person needs to know which paper.
Error tooSmall(const QString &paper)
{
    return error(QStringLiteral("template-too-small"),
                 QStringLiteral("Ezen a papíron (%1) ez az elrendezés túl kicsi lenne. "
                                "Válassz nagyobb papírt.")
                     .arg(paper));
}

/// How the paper is named in a diagnostic: its everyday name, or the millimetre
/// size when it has none.
QString paperReference(const PaperSpec &paper, const QSizeF &sheet)
{
    if (!paper.name.isEmpty())
        return paper.name;
    return QStringLiteral("%1 × %2 mm").arg(sheet.width()).arg(sheet.height());
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

/// Reads a coordinate on the 1/1000 lattice. The scaling is done numerically
/// and rounded once, exactly like the verifier, so 0.7 is a lattice value even
/// though 0.7 is not exactly representable in binary floating point.
bool latticeInt(const QJsonValue &value, int *out)
{
    if (!value.isDouble())
        return false;
    const double raw = value.toDouble();
    if (!std::isfinite(raw))
        return false;
    const double scaled = raw * kLattice;
    const double rounded = std::round(scaled);
    if (std::abs(scaled - rounded) > 1e-6 || std::abs(rounded) > 1.0e6)
        return false;
    *out = int(rounded);
    return true;
}

bool readRect(const QJsonValue &value, LatticeRect *out, Error *err, const QString &where)
{
    if (!value.isArray()) {
        *err = invalid(where, QStringLiteral("'rect' must be an array of four numbers"));
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() != 4) {
        *err = invalid(where, QStringLiteral("'rect' must hold exactly four numbers"));
        return false;
    }
    LatticeRect rect{};
    for (int i = 0; i < 4; ++i) {
        if (!array.at(i).isDouble()) {
            *err = invalid(where, QStringLiteral("'rect' must hold four numbers"));
            return false;
        }
        if (!latticeInt(array.at(i), &rect[i])) {
            *err = invalid(where, QStringLiteral("'rect' values must be on the 1/1000 lattice"));
            return false;
        }
    }
    *out = rect;
    return true;
}

/// The advisory aspect of a slot, derived from the normalised rectangle exactly
/// as the data contract states it. Derived from the normalised ratio, so the
/// hint means the same thing on a portrait and on a landscape sheet.
QString derivedAspect(const LatticeRect &rect)
{
    const double ratio = double(rect[2]) / double(rect[3]);
    if (ratio >= 1.5 - 1e-9)
        return QStringLiteral("landscape");
    if (ratio <= 0.667 + 1e-9)
        return QStringLiteral("portrait");
    return QStringLiteral("square");
}

bool readName(const QJsonObject &object, const QString &field, const QString &where,
              QString *out, Error *err)
{
    const QJsonValue value = object.value(field);
    if (!value.isString()) {
        *err = invalid(where, QStringLiteral("'%1' must be a string").arg(field));
        return false;
    }
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > 64) {
        *err = invalid(where, QStringLiteral("'%1' must be 1..64 characters").arg(field));
        return false;
    }
    *out = text;
    return true;
}

/// Checks that the slot rectangles tile the unit square: their areas sum to one
/// and no pair overlaps. Rectangles that are inside the square, disjoint and
/// total one unit of area cover it, so coverage needs no separate sweep.
bool tilesUnitSquare(const QList<LatticeRect> &rects, Error *err, const QString &where)
{
    long long area = 0;
    for (const LatticeRect &rect : rects)
        area += 1LL * rect[2] * rect[3];
    if (area != 1LL * kLattice * kLattice) {
        *err = invalid(where, QStringLiteral("slots cover %1 of 1000000 area units, "
                                             "they must tile the unit square")
                                  .arg(area));
        return false;
    }
    for (int i = 0; i < rects.size(); ++i) {
        for (int j = i + 1; j < rects.size(); ++j) {
            const LatticeRect &a = rects.at(i);
            const LatticeRect &b = rects.at(j);
            const int overlapX = std::min(a[0] + a[2], b[0] + b[2]) - std::max(a[0], b[0]);
            const int overlapY = std::min(a[1] + a[3], b[1] + b[3]) - std::max(a[1], b[1]);
            if (overlapX > 0 && overlapY > 0) {
                *err = invalid(where, QStringLiteral("slots %1 and %2 overlap").arg(i).arg(j));
                return false;
            }
        }
    }
    return true;
}

bool parseSlot(const QJsonValue &value, int index, const QString &where, CollageSlot *out,
               LatticeRect *lattice, Error *err)
{
    const QString slotWhere = QStringLiteral("%1 slot %2").arg(where).arg(index);
    if (!value.isObject()) {
        *err = invalid(slotWhere, QStringLiteral("must be an object"));
        return false;
    }
    const QJsonObject object = value.toObject();
    QString unknown;
    if (hasUnknown(object, {QStringLiteral("rect"), QStringLiteral("aspect")}, &unknown)) {
        *err = invalid(slotWhere, QStringLiteral("unknown field '%1'").arg(unknown));
        return false;
    }

    LatticeRect rect{};
    if (!readRect(object.value(QStringLiteral("rect")), &rect, err, slotWhere))
        return false;
    if (rect[0] < 0 || rect[1] < 0 || rect[2] <= 0 || rect[3] <= 0) {
        *err = invalid(slotWhere, QStringLiteral("'rect' must be a positive rectangle"));
        return false;
    }
    if (rect[0] + rect[2] > kLattice || rect[1] + rect[3] > kLattice) {
        *err = invalid(slotWhere, QStringLiteral("'rect' must stay inside the unit square"));
        return false;
    }

    QString aspect = QStringLiteral("any");
    const QJsonValue aspectValue = object.value(QStringLiteral("aspect"));
    if (!aspectValue.isUndefined()) {
        if (!aspectValue.isString() || !aspectKeys().contains(aspectValue.toString())) {
            *err = invalid(slotWhere, QStringLiteral("'aspect' must be any, portrait, "
                                                     "landscape or square"));
            return false;
        }
        aspect = aspectValue.toString();
        if (aspect != QLatin1String("any") && aspect != derivedAspect(rect)) {
            *err = invalid(slotWhere,
                           QStringLiteral("'aspect' is '%1' but the rectangle is %2")
                               .arg(aspect, derivedAspect(rect)));
            return false;
        }
    }

    CollageSlot slot;
    slot.rect = QRectF(double(rect[0]) / kLattice, double(rect[1]) / kLattice,
                       double(rect[2]) / kLattice, double(rect[3]) / kLattice);
    slot.aspect = aspect;
    *out = slot;
    *lattice = rect;
    return true;
}

bool parseTemplate(const QJsonValue &value, int index, CollageTemplate *out, Error *err)
{
    const QString where = QStringLiteral("templates[%1]").arg(index);
    if (!value.isObject()) {
        *err = invalid(where, QStringLiteral("must be an object"));
        return false;
    }
    const QJsonObject object = value.toObject();
    // The format allows an orientation override, but this build has one
    // geometry for both orientations. Dropping it silently would print a
    // composition the file does not describe, so it is an error instead.
    if (object.contains(QStringLiteral("landscapeSlots"))) {
        *err = invalid(where, QStringLiteral("'landscapeSlots' is not supported: this "
                                             "build uses one geometry for both orientations"));
        return false;
    }
    QString unknown;
    if (hasUnknown(object,
                   {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("nameEn"),
                    QStringLiteral("photoCount"), QStringLiteral("family"),
                    QStringLiteral("slots")},
                   &unknown)) {
        *err = invalid(where, QStringLiteral("unknown field '%1'").arg(unknown));
        return false;
    }

    const QJsonValue idValue = object.value(QStringLiteral("id"));
    if (!idValue.isString()) {
        *err = invalid(where, QStringLiteral("'id' must be a string"));
        return false;
    }
    const QString id = idValue.toString();
    static const QRegularExpression idPattern(QStringLiteral("^n[0-9]{2}-[a-z0-9-]+$"));
    if (id.size() > 32 || !idPattern.match(id).hasMatch()) {
        *err = invalid(where, QStringLiteral("'id' must match ^n[0-9]{2}-[a-z0-9-]+$ and "
                                             "be at most 32 characters"));
        return false;
    }
    CollageTemplate t;
    t.id = id;
    if (!readName(object, QStringLiteral("name"), id, &t.name, err))
        return false;
    if (!readName(object, QStringLiteral("nameEn"), id, &t.nameEn, err))
        return false;

    const QJsonValue countValue = object.value(QStringLiteral("photoCount"));
    if (!countValue.isDouble()) {
        *err = invalid(id, QStringLiteral("'photoCount' must be an integer"));
        return false;
    }
    const double rawCount = countValue.toDouble();
    if (!std::isfinite(rawCount) || rawCount != std::floor(rawCount) || rawCount < 1.0
        || rawCount > double(kMaxPhotoCount)) {
        *err = invalid(id, QStringLiteral("'photoCount' must be an integer from 1 to 10"));
        return false;
    }
    t.photoCount = int(rawCount);

    const QJsonValue familyValue = object.value(QStringLiteral("family"));
    if (!familyValue.isString() || !familyKeys().contains(familyValue.toString())) {
        *err = invalid(id, QStringLiteral("'family' must be one of %1")
                               .arg(familyKeys().join(QStringLiteral(", "))));
        return false;
    }
    t.family = familyValue.toString();

    const QJsonValue slotsValue = object.value(QStringLiteral("slots"));
    if (!slotsValue.isArray()) {
        *err = invalid(id, QStringLiteral("'slots' must be an array"));
        return false;
    }
    const QJsonArray slotArray = slotsValue.toArray();
    if (slotArray.size() < 1 || slotArray.size() > kMaxPhotoCount) {
        *err = invalid(id, QStringLiteral("'slots' must hold 1..10 entries"));
        return false;
    }
    if (slotArray.size() != t.photoCount) {
        *err = invalid(id, QStringLiteral("'photoCount' is %1 but there are %2 slots")
                               .arg(t.photoCount)
                               .arg(slotArray.size()));
        return false;
    }

    QList<LatticeRect> lattice;
    lattice.reserve(slotArray.size());
    for (int i = 0; i < slotArray.size(); ++i) {
        CollageSlot slot;
        LatticeRect rect{};
        if (!parseSlot(slotArray.at(i), i, id, &slot, &rect, err))
            return false;
        t.slotList.append(slot);
        lattice.append(rect);
    }
    if (!tilesUnitSquare(lattice, err, id))
        return false;
    *out = t;
    return true;
}

} // namespace

double collageMarginMm(const QSizeF &sheetMm)
{
    const double shortSide = std::min(sheetMm.width(), sheetMm.height());
    if (!std::isfinite(shortSide) || shortSide <= 0.0)
        return 0.0;
    return std::max(4.0, std::min(10.0, 0.05 * shortSide));
}

const CollageTemplates &CollageTemplates::instance()
{
    ensureCollageResources();
    static const CollageTemplates catalogue = [] {
        QFile file(QStringLiteral(":/templates/collage.json"));
        if (!file.open(QIODevice::ReadOnly)) {
            qCritical("CollageTemplates: embedded catalogue is missing");
            return CollageTemplates{};
        }
        auto parsed = CollageTemplates::fromJson(file.readAll());
        if (!parsed) {
            qCritical("CollageTemplates: %s", qPrintable(parsed.error().message));
            return CollageTemplates{};
        }
        return parsed.value();
    }();
    return catalogue;
}

Result<CollageTemplates> CollageTemplates::fromJson(const QByteArray &json)
{
    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError) {
        return error(QStringLiteral("collage-catalogue-invalid"),
                     QStringLiteral("not valid JSON at offset %1: %2")
                         .arg(parse.offset)
                         .arg(parse.errorString()));
    }
    if (!document.isObject())
        return invalid(QStringLiteral("root"), QStringLiteral("must be an object"));
    const QJsonObject root = document.object();

    QString unknown;
    if (hasUnknown(root,
                   {QStringLiteral("format"), QStringLiteral("lattice"),
                    QStringLiteral("unit"), QStringLiteral("origin"),
                    QStringLiteral("templates")},
                   &unknown)) {
        return invalid(QStringLiteral("root"), QStringLiteral("unknown field '%1'").arg(unknown));
    }
    for (const QString &field : {QStringLiteral("unit"), QStringLiteral("origin")}) {
        if (root.contains(field) && !root.value(field).isString())
            return invalid(field, QStringLiteral("must be a string"));
    }
    if (root.value(QStringLiteral("format")).toString()
        != QLatin1String("photoprint.collage/1")) {
        return invalid(QStringLiteral("format"),
                       QStringLiteral("must be 'photoprint.collage/1'"));
    }
    const QJsonValue latticeValue = root.value(QStringLiteral("lattice"));
    if (!latticeValue.isDouble() || latticeValue.toDouble() != double(kLattice))
        return invalid(QStringLiteral("lattice"), QStringLiteral("must be 1000"));

    const QJsonValue templatesValue = root.value(QStringLiteral("templates"));
    if (!templatesValue.isArray())
        return invalid(QStringLiteral("templates"), QStringLiteral("must be an array"));
    const QJsonArray entries = templatesValue.toArray();

    CollageTemplates result;
    int perCount[kMaxPhotoCount + 1] = {};
    for (int i = 0; i < entries.size(); ++i) {
        CollageTemplate t;
        Error parseFailure;
        if (!parseTemplate(entries.at(i), i, &t, &parseFailure))
            return parseFailure;
        if (result.m_indexById.contains(t.id))
            return invalid(t.id, QStringLiteral("duplicate id"));
        result.m_indexById.insert(t.id, result.m_templates.size());
        result.m_templates.append(t);
        perCount[t.photoCount] += 1;
    }
    if (result.m_templates.size() != kExpectedTemplates) {
        return invalid(QStringLiteral("templates"),
                       QStringLiteral("must hold %1 templates, found %2")
                           .arg(kExpectedTemplates)
                           .arg(result.m_templates.size()));
    }
    if (perCount[1] != 1) {
        return invalid(QStringLiteral("templates"),
                       QStringLiteral("photoCount 1 must hold exactly one template, found %1")
                           .arg(perCount[1]));
    }
    for (int count = 2; count <= kMaxPhotoCount; ++count) {
        if (perCount[count] != kTemplatesPerCount) {
            return invalid(QStringLiteral("templates"),
                           QStringLiteral("photoCount %1 must hold %2 templates, found %3")
                               .arg(count)
                               .arg(kTemplatesPerCount)
                               .arg(perCount[count]));
        }
    }
    return result;
}

QList<CollageTemplate> CollageTemplates::templatesForCount(int photoCount) const
{
    QList<CollageTemplate> result;
    if (photoCount < 1 || photoCount > kMaxPhotoCount)
        return result;
    for (const CollageTemplate &t : m_templates) {
        if (t.photoCount == photoCount)
            result.append(t);
    }
    return result;
}

const CollageTemplate *CollageTemplates::find(const QString &id) const
{
    const auto it = m_indexById.constFind(id);
    if (it == m_indexById.constEnd())
        return nullptr;
    return &m_templates.at(it.value());
}

Result<QList<QRectF>> CollageTemplates::slotsMm(const CollageTemplate &t, const PaperSpec &paper,
                                                bool landscape) const
{
    return slotsMm(t, PaperGeometry::forPaper(paper, landscape));
}

Result<QList<QRectF>> CollageTemplates::slotsMm(const CollageTemplate &t,
                                                const PaperGeometry &geometry) const
{
    // The model carries no imageable rectangle a template may rely on, so the
    // physical sheet is the reference. Borderless printing applies its own
    // scale and offset later, and the margin keeps the content out of the bleed.
    const QSizeF sheet = geometry.physicalSizeMm();
    const QString reference = paperReference(geometry.paper, sheet);
    if (!(std::isfinite(sheet.width()) && std::isfinite(sheet.height()) && sheet.width() > 0.0
          && sheet.height() > 0.0)) {
        return tooSmall(reference);
    }

    const double margin = collageMarginMm(sheet);
    const double half = margin / 2.0;
    const double contentWidth = sheet.width() - margin;
    const double contentHeight = sheet.height() - margin;
    if (contentWidth <= 0.0 || contentHeight <= 0.0)
        return tooSmall(reference);

    QList<QRectF> rects;
    rects.reserve(t.slotList.size());
    for (const CollageSlot &slot : t.slotList) {
        // The content box is the sheet inset by half a margin and the slot
        // rectangle is stretched over it; every edge then gives up the other
        // half, so neighbours end up one gutter apart and the sheet edge keeps
        // a full margin.
        QRectF rect(half + slot.rect.x() * contentWidth,
                    half + slot.rect.y() * contentHeight, slot.rect.width() * contentWidth,
                    slot.rect.height() * contentHeight);
        rect.adjust(half, half, -half, -half);
        if (rect.width() < kMinSlotMm || rect.height() < kMinSlotMm)
            return tooSmall(reference);
        rects.append(rect);
    }
    return rects;
}

Result<QList<Frame>> CollageTemplates::apply(const CollageTemplate &t, const Page &page) const
{
    return apply(t, page.paper, page.landscape, page.frames);
}

Result<QList<Frame>> CollageTemplates::apply(const CollageTemplate &t, const PaperSpec &paper,
                                             bool landscape, const QList<Frame> &frames) const
{
    if (frames.size() != t.photoCount) {
        return error(QStringLiteral("template-count-mismatch"),
                     QStringLiteral("Ehhez a sablonhoz %1 fotó kell, a lapon most %2 van.")
                         .arg(t.photoCount)
                         .arg(frames.size()));
    }
    for (const Frame &frame : frames) {
        if (frame.sourcePath.isEmpty()) {
            return error(QStringLiteral("template-missing-source"),
                         QStringLiteral("Egy képnek nincs forrásfájlja, ezért a sablon nem "
                                        "alkalmazható."));
        }
    }

    const auto rects = slotsMm(t, paper, landscape);
    if (!rects)
        return rects.error();

    // Copy first, then change only the geometry the template owns. Identity,
    // the source and its sha, the image rotation, the label, the paper and tone
    // fields and the effect all survive untouched.
    QList<Frame> result = frames;
    for (int i = 0; i < result.size(); ++i) {
        Frame &frame = result[i];
        const QRectF &slot = rects.value().at(i);
        const double currentAspect = frame.height > 0.0 ? frame.width / frame.height : 0.0;
        const double slotAspect = slot.height() > 0.0 ? slot.width() / slot.height() : 0.0;
        const bool keepCrop = currentAspect > 0.0 && slotAspect > 0.0
            && std::abs(slotAspect - currentAspect) <= 0.15 * currentAspect;
        if (!keepCrop) {
            frame.cropCenter = QPointF(0.5, 0.5);
            frame.zoom = 1.0;
        }
        frame.cx = slot.center().x();
        frame.cy = slot.center().y();
        frame.width = slot.width();
        frame.height = slot.height();
        frame.angleDeg = 0.0;
        frame.fit = FitMode::Fill;
        // A template layout is hand work as far as the first-use grid is
        // concerned: a photograph added later must not reflow it.
        frame.autoPlaced = false;
    }
    return result;
}

QString collageLabel(const CollageTemplate &t)
{
    return QStringLiteral("%1 · %2").arg(t.family).arg(t.photoCount);
}

QString collageReferenceName(const CollageTemplate &t)
{
    return t.name.isEmpty() ? t.id : t.name;
}

} // namespace pp
