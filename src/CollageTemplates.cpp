#include "CollageTemplates.h"

#include "SlotGeometry.h"

#include <QCoreApplication>
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
/// refused on a paper, and the person needs to know which paper. The sentence is
/// translated, so it lives in the catalogues like every other user-facing text.
Error tooSmall(const QString &paper)
{
    const QString text = QCoreApplication::translate(
        "PhotoPrint",
        "Ezen a papíron (%1) ez az elrendezés túl kicsi lenne. Válassz nagyobb papírt.");
    return error(QStringLiteral("template-too-small"), text.arg(paper));
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

    // The data key is "aspect" for format compatibility; the field is called
    // normalizedAspect so no caller mistakes it for the printed aspect.
    QString normalizedAspect = QStringLiteral("any");
    const QJsonValue aspectValue = object.value(QStringLiteral("aspect"));
    if (!aspectValue.isUndefined()) {
        if (!aspectValue.isString() || !aspectKeys().contains(aspectValue.toString())) {
            *err = invalid(slotWhere, QStringLiteral("'aspect' must be any, portrait, "
                                                     "landscape or square"));
            return false;
        }
        normalizedAspect = aspectValue.toString();
        if (normalizedAspect != QLatin1String("any")
            && normalizedAspect != derivedAspect(rect)) {
            *err = invalid(slotWhere,
                           QStringLiteral("'aspect' is '%1' but the rectangle is %2")
                               .arg(normalizedAspect, derivedAspect(rect)));
            return false;
        }
    }

    CollageSlot slot;
    slot.rect = QRectF(double(rect[0]) / kLattice, double(rect[1]) / kLattice,
                       double(rect[2]) / kLattice, double(rect[3]) / kLattice);
    slot.normalizedAspect = normalizedAspect;
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
    // The id carries the count in its first two digits. Requiring them to agree
    // is a structural sentinel: a data edit that changes one and forgets the
    // other fails here with a precise message, not somewhere down the pipeline.
    const int idCount = id.mid(1, 2).toInt();
    if (idCount != t.photoCount) {
        *err = invalid(id, QStringLiteral("the id prefix n%1 contradicts photoCount %2")
                               .arg(idCount, 2, 10, QLatin1Char('0'))
                               .arg(t.photoCount));
        return false;
    }

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
    // One constant lives in SlotGeometry: the catalogue's margin and a page's
    // automatic slot margin must never drift apart.
    return SlotGeometry::automaticMarginMm(sheetMm);
}

namespace {

/// The catalogue and the failure that produced it, so the two are read from one
/// initialisation and a broken data file never looks like a product without
/// templates.
struct CatalogueLoad {
    CollageTemplates catalogue;
    QString error;
};

const CatalogueLoad &catalogueLoad()
{
    ensureCollageResources();
    static const CatalogueLoad load = [] {
        QFile file(QStringLiteral(":/templates/collage.json"));
        if (!file.open(QIODevice::ReadOnly)) {
            qCritical("CollageTemplates: embedded catalogue is missing");
            return CatalogueLoad{CollageTemplates{},
                                 QStringLiteral("the embedded catalogue is missing")};
        }
        auto parsed = CollageTemplates::fromJson(file.readAll());
        if (!parsed) {
            qCritical("CollageTemplates: %s", qPrintable(parsed.error().message));
            return CatalogueLoad{CollageTemplates{}, parsed.error().message};
        }
        return CatalogueLoad{parsed.value(), QString()};
    }();
    return load;
}

} // namespace

const CollageTemplates &CollageTemplates::instance()
{
    return catalogueLoad().catalogue;
}

QString CollageTemplates::loadError()
{
    return catalogueLoad().error;
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
    const double latticeNumber = latticeValue.toDouble();
    if (!latticeValue.isDouble() || !std::isfinite(latticeNumber)
        || latticeNumber != std::floor(latticeNumber)
        || latticeNumber != double(kLattice)) {
        return invalid(QStringLiteral("lattice"), QStringLiteral("must be the integer 1000"));
    }

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
    // Per-count checks, not a total: a truncated file says exactly which photo
    // count lost an entry instead of only "55 expected". The catalogue contract
    // is one fallback for one photo and six layouts for every count up to ten.
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

Result<QStringList> CollageTemplates::aspectsMm(const CollageTemplate &t, const PaperSpec &paper,
                                                bool landscape) const
{
    const auto rects = slotsMm(t, paper, landscape);
    if (!rects)
        return rects.error();
    QStringList aspects;
    aspects.reserve(rects.value().size());
    for (const QRectF &rect : rects.value())
        aspects.append(collageAspectMm(rect));
    return aspects;
}

Result<QList<Frame>> CollageTemplates::apply(const CollageTemplate &t, const Page &page,
                                             CollageApplyReport *report) const
{
    return apply(t, page.paper, page.landscape, page.frames, report);
}

Result<QList<Frame>> CollageTemplates::apply(const CollageTemplate &t, const PaperSpec &paper,
                                             bool landscape, const QList<Frame> &frames,
                                             CollageApplyReport *report) const
{
    if (report)
        *report = CollageApplyReport{};

    // A template built by hand can disagree with itself. Check the slot list
    // against the declared count before anything is indexed, so a bad template
    // is an error instead of a crash or a frame with garbage geometry.
    if (t.slotList.isEmpty() || t.slotList.size() != t.photoCount) {
        return error(QStringLiteral("template-invalid"),
                     QStringLiteral("a template must have exactly one slot per photograph"));
    }
    if (frames.size() != t.slotList.size()) {
        return error(
            QStringLiteral("template-count-mismatch"),
            QCoreApplication::translate("PhotoPrint",
                                        "Ehhez a sablonhoz %1 fotó kell, a lapon most %2 van.")
                .arg(t.photoCount)
                .arg(frames.size()));
    }
    for (const Frame &frame : frames) {
        if (frame.sourcePath.isEmpty()) {
            return error(
                QStringLiteral("template-missing-source"),
                QCoreApplication::translate(
                    "PhotoPrint",
                    "Egy képnek nincs forrásfájlja, ezért a sablon nem alkalmazható."));
        }
    }

    const auto rects = slotsMm(t, paper, landscape);
    if (!rects)
        return rects.error();
    if (rects.value().size() != t.slotList.size()) {
        return error(QStringLiteral("template-invalid"),
                     QStringLiteral("the mapper did not produce one slot per photograph"));
    }

    // Copy first, then change only the geometry the template owns. Identity,
    // the source and its sha, the image rotation, the label, the paper and tone
    // fields and the effect all survive untouched.
    QList<Frame> result = frames;
    for (int i = 0; i < result.size(); ++i) {
        if (i >= t.slotList.size() || i >= rects.value().size()) {
            return error(QStringLiteral("template-invalid"),
                         QStringLiteral("slot index %1 is outside the template").arg(i));
        }
        Frame &frame = result[i];
        const QRectF &slot = rects.value().at(i);
        // One rule for the picker, the engine and the drop handler.
        const bool keepCrop = SlotGeometry::keepCropAcrossAspect(
            frame.width, frame.height, slot.width(), slot.height());
        if (!keepCrop) {
            frame.cropCenter = QPointF(0.5, 0.5);
            frame.zoom = 1.0;
            if (report)
                report->cropResetIds.append(frame.id);
        }
        if (frame.angleDeg != 0.0 && report)
            report->rotationResetIds.append(frame.id);
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

QString collageAspectMm(const QRectF &slotMm)
{
    if (!std::isfinite(slotMm.width()) || !std::isfinite(slotMm.height())
        || slotMm.width() <= 0.0 || slotMm.height() <= 0.0) {
        return {};
    }
    const double ratio = slotMm.width() / slotMm.height();
    if (ratio >= 1.5)
        return QStringLiteral("landscape");
    if (ratio <= 0.667)
        return QStringLiteral("portrait");
    return QStringLiteral("square");
}

QString collageLabel(const CollageTemplate &t)
{
    if (!t.name.isEmpty())
        return t.name;
    if (!t.nameEn.isEmpty())
        return t.nameEn;
    return t.id;
}

QString collageReferenceName(const CollageTemplate &t)
{
    return collageLabel(t);
}

} // namespace pp
