#include "ProjectStore.h"

#include "ImageSource.h"
#include "PaperCatalog.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QPair>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

/// The trial sheet offers only steps up to kTrialPaperEv, and this loader clamps
/// to the same range, so a crafted project cannot drive a cell to black or blown
/// out through the paper exposure.
double clampTrialPaperEv(double ev)
{
    // std::clamp passes NaN through, and a NaN here poisons every pixel.
    if (!std::isfinite(ev))
        return 0.0;
    return std::clamp(ev, -kTrialPaperEv, kTrialPaperEv);
}

QJsonObject paperToJson(const PaperSpec &paper)
{
    QJsonObject object;
    object.insert(QStringLiteral("name"), paper.name);
    object.insert(QStringLiteral("physicalWidthMm"), paper.physicalWidthMm);
    object.insert(QStringLiteral("physicalHeightMm"), paper.physicalHeightMm);
    object.insert(QStringLiteral("logicalWidthMm"), paper.logicalWidthMm);
    object.insert(QStringLiteral("logicalHeightMm"), paper.logicalHeightMm);
    object.insert(QStringLiteral("borderlessSupported"), paper.borderlessSupported);
    object.insert(QStringLiteral("hasCheckedOffset"), paper.hasCheckedOffset);
    object.insert(QStringLiteral("offsetXMm"), paper.offsetXMm);
    object.insert(QStringLiteral("offsetYMm"), paper.offsetYMm);
    object.insert(QStringLiteral("type"), paperTypeKey(paper.type));
    object.insert(QStringLiteral("custom"), paper.custom);
    return object;
}

PaperSpec paperFromJson(const QJsonObject &object)
{
    PaperSpec paper;
    paper.name = object.value(QStringLiteral("name")).toString();
    paper.physicalWidthMm = object.value(QStringLiteral("physicalWidthMm")).toDouble(210.0);
    paper.physicalHeightMm = object.value(QStringLiteral("physicalHeightMm")).toDouble(297.0);
    paper.logicalWidthMm = object.value(QStringLiteral("logicalWidthMm")).toDouble();
    paper.logicalHeightMm = object.value(QStringLiteral("logicalHeightMm")).toDouble();
    paper.borderlessSupported = object.value(QStringLiteral("borderlessSupported")).toBool();
    paper.hasCheckedOffset = object.value(QStringLiteral("hasCheckedOffset")).toBool();
    paper.offsetXMm = object.value(QStringLiteral("offsetXMm")).toDouble();
    paper.offsetYMm = object.value(QStringLiteral("offsetYMm")).toDouble();
    // Older projects only carried isPhotoPaper; it never meant matt.
    const bool legacyPhoto = object.value(QStringLiteral("isPhotoPaper")).toBool();
    paper.type = paperTypeFromKey(object.value(QStringLiteral("type")).toString(), legacyPhoto);
    paper.custom = object.value(QStringLiteral("custom")).toBool();
    return paper;
}

/// Lexical, symlink-free form of a path, always absolute.
QString lexicalPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isInsideDirectory(const QString &directory, const QString &path)
{
    if (directory.isEmpty() || path.isEmpty())
        return false;
    const QString root = lexicalPath(directory);
    const QString candidate = lexicalPath(path);
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return candidate.startsWith(prefix);
}

/// Resolves a project-relative `source` only when it stays inside the project
/// directory. An empty result means the stored path is not trusted.
QString resolveProjectSource(const QString &projectDir, const QString &stored)
{
    if (projectDir.isEmpty() || stored.isEmpty() || QDir::isAbsolutePath(stored))
        return QString();

    const QString joined = QDir::cleanPath(QDir(projectDir).absoluteFilePath(stored));
    if (!isInsideDirectory(projectDir, joined))
        return QString();

    // A symlink inside the project must not lead the read outside either.
    const QFileInfo info(joined);
    if (info.exists()) {
        const QString canonical = info.canonicalFilePath();
        const QString root = QFileInfo(projectDir).canonicalFilePath();
        if (!canonical.isEmpty() && !root.isEmpty() && !isInsideDirectory(root, canonical))
            return QString();
    }
    return joined;
}

/// A portable bundle must be a real directory of its own. The containment rule
/// is lexical, so a `.files` entry that is a symlink would send every copy into
/// a directory the project does not own. The canonical path is compared with
/// the canonical parent plus the entry name: a project inside a symlinked
/// folder still saves, while the `.files` entry itself may not be a link. An
/// empty result means the folder cannot be trusted.
QString canonicalBundleRoot(const QString &portableDir)
{
    const QFileInfo info(portableDir);
    if (!info.exists() || !info.isDir())
        return QString();
    const QString canonical = info.canonicalFilePath();
    const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    if (canonical.isEmpty() || parent.isEmpty())
        return QString();
    const QString expected = QDir::cleanPath(QDir(parent).filePath(info.fileName()));
    return QDir::cleanPath(canonical) == expected ? canonical : QString();
}

/// `original` is the legacy absolute path recorded for relinking. A relative
/// value is only honoured through the same containment rule as `source`.
///
/// An absolute value is the only way a 1.0.0 project reaches a photo that lives
/// outside the project folder, so it cannot simply be dropped. It arrives from
/// whoever wrote the file, though: adopted verbatim, it would let an untrusted
/// project name any local file for this app to stat, hash and render, and a
/// FIFO or device there would block the read. It is therefore followed only
/// when it still holds the recorded photo: a regular file whose content hash
/// equals the stored identity. A frame without a recorded identity cannot be
/// verified at all, so it is treated as missing too; the relink path handles
/// both cases.
QString resolveOriginalSource(const QString &projectDir, const QString &original,
                              const QByteArray &expectedSha256)
{
    if (original.isEmpty())
        return QString();
    if (!QDir::isAbsolutePath(original))
        return resolveProjectSource(projectDir, original);

    const QString candidate = QDir::cleanPath(original);
    const QFileInfo info(candidate);
    if (!info.exists() || !info.isFile() || expectedSha256.isEmpty())
        return QString();
    if (fileSha256(candidate) != expectedSha256)
        return QString();
    return candidate;
}

/// Deterministic file names for a portable copy. Unrelated sources with the
/// same base name get a short content hash, so two different photos can never
/// overwrite each other. Identical content is copied once and keeps the plain
/// name, which also keeps 1.0.0 portable copies stable.
///
/// Every plain base name is reserved before any generated name is handed out,
/// and each generated name is checked against all names already taken. A
/// generated `photo-1.png` therefore can never claim the file another source
/// stores under its own plain `photo-1.png`. Comparison folds case because the
/// bundle may be opened on a file system that does too.
QHash<QString, QString> portableFileNames(const DocumentSnapshot &document)
{
    QMap<QString, QString> baseBySource;
    QMap<QString, QSet<QString>> sourcesByBase;
    QMap<QString, QSet<QString>> hashesByBase;
    QMap<QString, QString> hashBySource;

    for (const Page &page : document.pages) {
        for (const Frame &frame : page.frames) {
            if (frame.sourcePath.isEmpty())
                continue;
            const QString source = lexicalPath(frame.sourcePath);
            if (baseBySource.contains(source))
                continue;
            const QString base = QFileInfo(source).fileName();
            const QString hash = QString::fromLatin1(frame.sourceSha256.toHex());
            baseBySource.insert(source, base);
            hashBySource.insert(source, hash);
            sourcesByBase[base].insert(source);
            hashesByBase[base].insert(hash);
        }
    }

    QSet<QString> used;
    QSet<QString> pending;
    QHash<QString, QString> names;

    // Plain base names first, in a stable order: they are what a bundle of
    // unique photos has always looked like.
    for (auto it = sourcesByBase.cbegin(); it != sourcesByBase.cend(); ++it) {
        const QString base = it.key();
        const QSet<QString> hashes = hashesByBase.value(base);
        const bool sameContent =
            it.value().size() > 1 && !hashes.contains(QString()) && hashes.size() == 1;
        const QString folded = base.toLower();
        if ((it.value().size() <= 1 || sameContent) && !used.contains(folded)) {
            used.insert(folded);
            for (const QString &source : it.value())
                names.insert(source, base);
            continue;
        }
        for (const QString &source : it.value())
            pending.insert(source);
    }

    QHash<QString, int> anonymousNext;
    for (auto it = baseBySource.cbegin(); it != baseBySource.cend(); ++it) {
        if (!pending.contains(it.key()))
            continue;
        const QString base = it.value();
        const QFileInfo info(it.key());
        QString stem = info.completeBaseName();
        if (stem.isEmpty())
            stem = base;
        const QString suffix = info.suffix();
        const QString hash = hashBySource.value(it.key());
        const auto withSuffix = [&suffix](const QString &core) {
            return suffix.isEmpty() ? core : core + QLatin1Char('.') + suffix;
        };

        QString name;
        if (hash.isEmpty()) {
            int &next = anonymousNext[stem];
            do {
                name = withSuffix(QStringLiteral("%1-%2").arg(stem).arg(next++));
            } while (!name.isEmpty() && used.contains(name.toLower()));
        } else {
            const QString hashed = QStringLiteral("%1-%2").arg(stem, hash.left(8));
            name = withSuffix(hashed);
            for (int bump = 2; !name.isEmpty() && used.contains(name.toLower()); ++bump)
                name = withSuffix(QStringLiteral("%1-%2").arg(hashed).arg(bump));
        }
        used.insert(name.toLower());
        names.insert(it.key(), name);
    }
    return names;
}

/// A border colour is written as #rrggbb when it is opaque and #aarrggbb
/// otherwise, the shortest form that round-trips. The loader accepts any colour
/// QColor understands, because the value is data, not a direction to trust.
QJsonValue colourToJson(const QColor &colour)
{
    if (colour.alpha() == 255)
        return colour.name(QColor::HexRgb);
    return colour.name(QColor::HexArgb);
}

QJsonObject slotStyleToJson(const SlotStyle &stored)
{
    const SlotStyle style = SlotGeometry::sanitiseStyle(stored);
    QJsonObject object;
    object.insert(QStringLiteral("marginMm"), style.marginMm);
    object.insert(QStringLiteral("gutterMm"), style.gutterMm);
    object.insert(QStringLiteral("borderMm"), style.borderMm);
    object.insert(QStringLiteral("borderColor"), colourToJson(style.borderColor));
    object.insert(QStringLiteral("cornerRadiusMm"), style.cornerRadiusMm);
    return object;
}

/// Reads one page's `layout` object. Every failure is local: a bad slot is
/// skipped, a slot list that cannot describe one is an empty layout, and an
/// empty layout drops its style, so a style with no slots never surprises a
/// later apply with a border the user never saw on a photograph.
struct ParsedLayout {
    QList<Slot> slotList;
    SlotStyle style;
    QString layoutId;
};

ParsedLayout layoutFromJson(const QJsonValue &value)
{
    ParsedLayout parsed;
    if (!value.isObject())
        return parsed;
    const QJsonObject object = value.toObject();

    QList<Slot> slotValues;
    QSet<QString> ids;
    const QJsonValue slotsValue = object.value(QStringLiteral("slots"));
    if (slotsValue.isArray()) {
        for (const QJsonValue &slotValue : slotsValue.toArray()) {
            if (slotValues.size() >= SlotGeometry::kMaxSlotsPerPage)
                break;
            if (!slotValue.isObject())
                continue;
            const QJsonObject slotObject = slotValue.toObject();
            Slot slot;
            slot.id = slotObject.value(QStringLiteral("id")).toString();
            if (slot.id.isEmpty() || ids.contains(slot.id))
                continue;
            const QJsonArray unit = slotObject.value(QStringLiteral("unit")).toArray();
            if (unit.size() != 4)
                continue;
            bool numbers = true;
            for (int i = 0; i < 4; ++i) {
                if (!unit.at(i).isDouble())
                    numbers = false;
            }
            if (!numbers)
                continue;
            slot.unitRect = QRectF(unit.at(0).toDouble(), unit.at(1).toDouble(),
                                   unit.at(2).toDouble(), unit.at(3).toDouble());
            if (!SlotGeometry::sanitiseUnitRect(&slot.unitRect))
                continue;
            ids.insert(slot.id);
            slotValues.append(slot);
        }
    }
    if (slotValues.isEmpty())
        return parsed; // style and template go with the slots they describe

    parsed.slotList = slotValues;
    parsed.layoutId = object.value(QStringLiteral("template")).toString();
    const QJsonValue styleValue = object.value(QStringLiteral("style"));
    if (styleValue.isObject()) {
        const QJsonObject styleObject = styleValue.toObject();
        SlotStyle style;
        style.marginMm = styleObject.value(QStringLiteral("marginMm")).toDouble(0.0);
        style.gutterMm = styleObject.value(QStringLiteral("gutterMm")).toDouble(0.0);
        style.borderMm = styleObject.value(QStringLiteral("borderMm")).toDouble(0.0);
        style.cornerRadiusMm =
            styleObject.value(QStringLiteral("cornerRadiusMm")).toDouble(0.0);
        const QString colour = styleObject.value(QStringLiteral("borderColor")).toString();
        if (!colour.isEmpty()) {
            const QColor parsedColour(colour);
            if (parsedColour.isValid())
                style.borderColor = parsedColour;
        }
        parsed.style = SlotGeometry::sanitiseStyle(style);
    }
    return parsed;
}

QJsonObject frameToJson(const Frame &frame, const QString &projectDir, const QString &portableDir,
                        const QHash<QString, QString> &portableNames)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), frame.id);

    if (!portableDir.isEmpty()) {
        // A portable copy carries its own images and nothing about the
        // author's disk layout.
        const QString source = lexicalPath(frame.sourcePath);
        const QString name = portableNames.value(source);
        if (name.isEmpty()) {
            object.insert(QStringLiteral("source"), QString());
        } else {
            const QString target = QDir(portableDir).filePath(name);
            object.insert(QStringLiteral("source"), QDir(projectDir).relativeFilePath(target));
        }
    } else {
        const QString relative = QDir(projectDir).relativeFilePath(frame.sourcePath);
        object.insert(QStringLiteral("source"), relative.isEmpty() ? frame.sourcePath : relative);
        object.insert(QStringLiteral("original"), frame.sourcePath);
    }

    object.insert(QStringLiteral("sha256"), QString::fromLatin1(frame.sourceSha256.toHex()));
    object.insert(QStringLiteral("cx"), frame.cx);
    object.insert(QStringLiteral("cy"), frame.cy);
    object.insert(QStringLiteral("width"), frame.width);
    object.insert(QStringLiteral("height"), frame.height);
    object.insert(QStringLiteral("angleDeg"), frame.angleDeg);
    object.insert(QStringLiteral("imageRotationDeg"), frame.imageRotationDeg);
    object.insert(QStringLiteral("zoom"), frame.zoom);
    object.insert(QStringLiteral("cropCenter"), QJsonArray{frame.cropCenter.x(), frame.cropCenter.y()});
    object.insert(QStringLiteral("fit"), frame.fit == FitMode::Fill ? QStringLiteral("fill")
                                                                    : QStringLiteral("full"));
    object.insert(QStringLiteral("autoPlaced"), frame.autoPlaced);
    // Optional and omitted when the frame is free, so a layout-free project is
    // byte-identical to a 1.0.1 file and an older build ignores the key.
    if (!frame.slotId.isEmpty())
        object.insert(QStringLiteral("slotId"), frame.slotId);
    object.insert(QStringLiteral("filter"), frame.filter);
    object.insert(QStringLiteral("filterStrength"), frame.filterStrength);
    object.insert(QStringLiteral("effectVersion"), frame.effectVersion);
    object.insert(QStringLiteral("upscale"), frame.upscale);
    object.insert(QStringLiteral("autoTone"), frame.autoTone);
    object.insert(QStringLiteral("brightness"), frame.brightness);
    // Trial sheets number their variants and carry their own paper tone; both
    // are part of the page the user sees, so both have to survive a save.
    object.insert(QStringLiteral("label"), frame.label);
    object.insert(QStringLiteral("paperOverride"), frame.paperOverride);
    object.insert(QStringLiteral("paperEv"), frame.paperEv);
    object.insert(QStringLiteral("tone"), QJsonObject{{QStringLiteral("exposureEv"), frame.tone.exposureEv},
                                                      {QStringLiteral("shadowLift"), frame.tone.shadowLift},
                                                      {QStringLiteral("highlightCompress"), frame.tone.highlightCompress}});
    return object;
}

Frame frameFromJson(const QJsonObject &object)
{
    Frame frame;
    frame.id = object.value(QStringLiteral("id")).toString();
    frame.sourceSha256 =
        QByteArray::fromHex(object.value(QStringLiteral("sha256")).toString().toLatin1());
    frame.cx = object.value(QStringLiteral("cx")).toDouble();
    frame.cy = object.value(QStringLiteral("cy")).toDouble();
    frame.width = object.value(QStringLiteral("width")).toDouble();
    frame.height = object.value(QStringLiteral("height")).toDouble();
    frame.angleDeg = object.value(QStringLiteral("angleDeg")).toDouble();
    frame.imageRotationDeg = object.value(QStringLiteral("imageRotationDeg")).toDouble();
    frame.zoom = object.value(QStringLiteral("zoom")).toDouble(1.0);
    const QJsonArray crop = object.value(QStringLiteral("cropCenter")).toArray();
    if (crop.size() == 2)
        frame.cropCenter = QPointF(crop.at(0).toDouble(), crop.at(1).toDouble());
    frame.fit = object.value(QStringLiteral("fit")).toString() == QLatin1String("fill")
        ? FitMode::Fill
        : FitMode::Full;
    frame.autoPlaced = object.value(QStringLiteral("autoPlaced")).toBool(true);
    frame.slotId = object.value(QStringLiteral("slotId")).toString();
    frame.filter = object.value(QStringLiteral("filter")).toString(QStringLiteral("original"));
    frame.filterStrength = object.value(QStringLiteral("filterStrength")).toInt(100);
    // Missing key means a 1.0.1 project: 0 follows the current recipe.
    frame.effectVersion =
        std::max(0, object.value(QStringLiteral("effectVersion")).toInt(0));
    frame.upscale = object.value(QStringLiteral("upscale")).toBool(false);
    frame.autoTone = object.value(QStringLiteral("autoTone")).toBool(true);
    frame.brightness = clampBrightnessEv(object.value(QStringLiteral("brightness")).toDouble());
    // Absent keys are 1.0.0 projects: plain frames without a trial label.
    frame.label = object.value(QStringLiteral("label")).toString();
    frame.paperOverride = object.value(QStringLiteral("paperOverride")).toBool(false);
    frame.paperEv = clampTrialPaperEv(object.value(QStringLiteral("paperEv")).toDouble());
    const QJsonObject tone = object.value(QStringLiteral("tone")).toObject();
    frame.tone.exposureEv = tone.value(QStringLiteral("exposureEv")).toDouble();
    frame.tone.shadowLift = tone.value(QStringLiteral("shadowLift")).toDouble();
    frame.tone.highlightCompress = tone.value(QStringLiteral("highlightCompress")).toDouble();
    return frame;
}

QJsonObject recipeToJson(const ColorRecipe &recipe)
{
    QJsonObject tone;
    tone.insert(QStringLiteral("exposureEv"), recipe.tone.exposureEv);
    tone.insert(QStringLiteral("shadowLift"), recipe.tone.shadowLift);
    tone.insert(QStringLiteral("highlightCompress"), recipe.tone.highlightCompress);
    QJsonObject object;
    object.insert(QStringLiteral("version"), recipe.version);
    object.insert(QStringLiteral("autoEnhance"), recipe.autoEnhance);
    object.insert(QStringLiteral("tone"), tone);
    object.insert(QStringLiteral("paperStrength"), recipe.paperStrength);
    object.insert(QStringLiteral("grayscale"), recipe.grayscale);
    return object;
}

ColorRecipe recipeFromJson(const QJsonObject &object)
{
    ColorRecipe recipe;
    recipe.version = object.value(QStringLiteral("version")).toInt(1);
    recipe.autoEnhance = object.value(QStringLiteral("autoEnhance")).toBool();
    const QJsonObject tone = object.value(QStringLiteral("tone")).toObject();
    recipe.tone.exposureEv = tone.value(QStringLiteral("exposureEv")).toDouble();
    recipe.tone.shadowLift = tone.value(QStringLiteral("shadowLift")).toDouble();
    recipe.tone.highlightCompress = tone.value(QStringLiteral("highlightCompress")).toDouble();
    recipe.paperStrength = object.value(QStringLiteral("paperStrength")).toDouble();
    recipe.grayscale = object.value(QStringLiteral("grayscale")).toBool();
    return sanitiseRecipe(recipe);
}

struct ParsedProject {
    DocumentSnapshot document;
    QList<SourceIssue> issues;
};

Result<ParsedProject> parseDocument(const QJsonObject &root, const QString &projectDir)
{
    ParsedProject parsed;
    parsed.document.recipe = recipeFromJson(root.value(QStringLiteral("recipe")).toObject());
    parsed.document.currentPage = root.value(QStringLiteral("currentPage")).toInt(0);
    parsed.document.fineQuality = root.value(QStringLiteral("fineQuality")).toBool(true);

    for (const QJsonValue &pageValue : root.value(QStringLiteral("pages")).toArray()) {
        const QJsonObject pageObject = pageValue.toObject();
        Page page;
        page.id = pageObject.value(QStringLiteral("id")).toString();
        page.paper = paperFromJson(pageObject.value(QStringLiteral("paper")).toObject());
        page.landscape = pageObject.value(QStringLiteral("landscape")).toBool();
        page.cutGuides =
            cutGuidesFromKey(pageObject.value(QStringLiteral("cutGuides")).toString());
        page.borderlessOverride = borderlessOverrideFromKey(
            pageObject.value(QStringLiteral("borderlessOverride")).toString());
        const ParsedLayout layout = layoutFromJson(pageObject.value(QStringLiteral("layout")));
        page.slotList = layout.slotList;
        page.slotStyle = layout.style;
        page.layoutId = layout.layoutId;
        for (const QJsonValue &frameValue : pageObject.value(QStringLiteral("frames")).toArray()) {
            const QJsonObject frameObject = frameValue.toObject();
            Frame frame = frameFromJson(frameObject);
            const QString stored = frameObject.value(QStringLiteral("source")).toString();
            const QString original = frameObject.value(QStringLiteral("original")).toString();

            bool blocked = false;
            QString contained;
            QString refused; // untrusted, kept only as a relink hint
            if (!stored.isEmpty()) {
                if (QDir::isAbsolutePath(stored)) {
                    // This application never writes an absolute `source`: a
                    // normal project stores the relative path plus an absolute
                    // `original`, a portable copy stores only a contained
                    // relative path. A recorded absolute value is therefore in
                    // the same trust class as an escape. It is not even
                    // stat()ed; only `original` can lead back to the photo.
                    refused = QDir::cleanPath(stored);
                    blocked = true;
                } else if (projectDir.isEmpty()) {
                    blocked = true;
                } else {
                    const QString joined =
                        QDir::cleanPath(QDir(projectDir).absoluteFilePath(stored));
                    contained = resolveProjectSource(projectDir, stored);
                    blocked = contained.isEmpty();
                    if (blocked)
                        refused = joined;
                }
            }

            if (!contained.isEmpty() && QFileInfo::exists(contained)) {
                frame.sourcePath = contained;
            } else {
                // The absolute `original` is only worth the stat and the full
                // content hash when the contained source cannot be used. A
                // healthy project resolves every frame from inside its own
                // folder, so hashing `original` eagerly read and hashed every
                // photo a second time for nothing.
                const QString fallback =
                    resolveOriginalSource(projectDir, original, frame.sourceSha256);
                // Absolute original: the normal way back to a photo that lives
                // outside the project folder on this machine. Without one, the
                // refused or missing contained path stays as the (empty) result.
                frame.sourcePath = fallback.isEmpty() ? contained : fallback;
            }

            SourceIssue issue;
            issue.frameId = frame.id;
            issue.path = frame.sourcePath;
            issue.refusedPath = refused;
            issue.expectedSha256 = frame.sourceSha256;
            if (!frame.sourcePath.isEmpty() && QFileInfo::exists(frame.sourcePath)) {
                const QByteArray current = fileSha256(frame.sourcePath);
                if (!issue.expectedSha256.isEmpty() && current != issue.expectedSha256) {
                    issue.changed = true;
                    parsed.issues.append(issue);
                }
            } else {
                issue.missing = true;
                issue.blocked = blocked;
                parsed.issues.append(issue);
            }
            page.frames.append(frame);
        }
        parsed.document.pages.append(page);
    }

    if (parsed.document.pages.isEmpty()) {
        return error(QStringLiteral("project-empty"),
                     QCoreApplication::translate("PhotoPrint", "Ezt a projektet most nem tudjuk megnyitni."));
    }
    parsed.document.currentPage =
        std::clamp(parsed.document.currentPage, 0, int(parsed.document.pages.size()) - 1);
    // The loader sanitises the slot invariant: dangling and duplicate
    // assignments become free frames and every remaining slotted frame takes its
    // slot's geometry. A corrupt file is therefore representable only as "these
    // photographs are free", never as a frame pointing at nothing.
    auto repaired = SlotLayoutEngine::repair(parsed.document);
    if (!repaired)
        return repaired.error();
    parsed.document = repaired.value();
    return parsed;
}

/// What a portable copy changed on disk, so a failed save can put the previous
/// bundle back instead of leaving it half rewritten.
struct PortableChanges {
    /// Files this save created; removed on rollback.
    QList<QString> added;
    /// backup -> target. `target` already existed with other content and was
    /// moved to `backup` first; rollback restores it, a commit drops it.
    QList<QPair<QString, QString>> replaced;
};

/// A free name in `directory` for a temporary backup of one file.
QString unusedBackupPath(const QString &directory)
{
    QTemporaryFile probe(QDir(directory).filePath(QStringLiteral(".ppj-previous-XXXXXX")));
    if (!probe.open())
        return QString();
    probe.setAutoRemove(false);
    const QString path = probe.fileName();
    probe.close();
    // QFile::rename() refuses an existing destination, and this empty probe
    // file is not the backup itself.
    QFile::remove(path);
    return path;
}

/// Copies every unique source into the portable folder, which the caller has
/// already checked to be a real directory and not a link (see
/// canonicalBundleRoot()). The caller rolls the listed changes back when any
/// later step fails.
Result<void> copyPortableSources(const DocumentSnapshot &document, const QString &portableDir,
                                 const QString &projectFile, PortableChanges *changes)
{
    const QHash<QString, QString> names = portableFileNames(document);
    const QString projectPath = lexicalPath(projectFile);
    QSet<QString> handled;

    for (const Page &page : document.pages) {
        for (const Frame &frame : page.frames) {
            if (frame.sourcePath.isEmpty()) {
                return error(QStringLiteral("source-missing"),
                             QCoreApplication::translate("PhotoPrint", "Ezt a fotót nem találjuk."));
            }
            const QString source = lexicalPath(frame.sourcePath);
            if (handled.contains(source))
                continue;
            handled.insert(source);

            const QFileInfo info(source);
            if (!info.exists() || !info.isFile()) {
                return error(QStringLiteral("source-missing"),
                             QCoreApplication::translate("PhotoPrint", "Ezt a fotót nem találjuk."));
            }
            if (source == projectPath) {
                return error(QStringLiteral("portable-source"),
                             QCoreApplication::translate("PhotoPrint", "A projektfájl nem másolható a fotók közé."));
            }

            const QString name = names.value(source);
            const QString target = QDir::cleanPath(QDir(portableDir).filePath(name));
            if (name.isEmpty() || !isInsideDirectory(portableDir, target) || target == projectPath) {
                return error(QStringLiteral("portable-path"),
                             QCoreApplication::translate("PhotoPrint", "A fotót nem sikerült a másolatba tenni."));
            }

            // A frame loaded from a portable bundle already points at the
            // carried file, so source and target are the same file. Nothing has
            // to be copied, and moving the target aside would destroy the only
            // copy this save could read. If its content no longer matches the
            // recorded hash, the next load reports it as changed.
            if (source == target)
                continue;

            if (QFileInfo::exists(target)) {
                if (fileSha256(target) == frame.sourceSha256)
                    continue; // already carried by an earlier save
                // The previous bundle already has a different photo under this
                // name. Move it aside rather than deleting it: a failure later
                // in this save has to be able to put it back.
                const QString backup = unusedBackupPath(portableDir);
                if (backup.isEmpty() || !QFile::rename(target, backup)) {
                    return error(QStringLiteral("portable-write"),
                                 QCoreApplication::translate("PhotoPrint", "A fotót nem sikerült a másolatba tenni."));
                }
                changes->replaced.append(qMakePair(backup, target));
            }
            if (!QFile::copy(source, target)) {
                return error(QStringLiteral("portable-copy"),
                             QCoreApplication::translate("PhotoPrint", "A fotót nem sikerült a másolatba tenni."));
            }
            changes->added.append(target);
            if (!frame.sourceSha256.isEmpty() && fileSha256(target) != frame.sourceSha256) {
                return error(QStringLiteral("portable-hash"),
                             QCoreApplication::translate("PhotoPrint", "A fotó másolata nem egyezik az eredetivel."));
            }
        }
    }
    return Result<void>();
}

} // namespace

QByteArray ProjectStore::serialize(const DocumentSnapshot &document, const QString &projectDir,
                                   const QString &portableDir)
{
    const QHash<QString, QString> portableNames = portableFileNames(document);

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("photoprint-project"));
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("recipe"), recipeToJson(document.recipe));
    root.insert(QStringLiteral("currentPage"), document.currentPage);
    root.insert(QStringLiteral("fineQuality"), document.fineQuality);

    QJsonArray pages;
    for (const Page &page : document.pages) {
        QJsonObject pageObject;
        pageObject.insert(QStringLiteral("id"), page.id);
        pageObject.insert(QStringLiteral("paper"), paperToJson(page.paper));
        pageObject.insert(QStringLiteral("landscape"), page.landscape);
        // Both keys are optional and written only when they say something. An
        // older build ignores them, and a page that never made the choice reads
        // back with the safe default.
        if (page.cutGuides != CutGuides::None)
            pageObject.insert(QStringLiteral("cutGuides"), cutGuidesKey(page.cutGuides));
        if (page.borderlessOverride != BorderlessOverride::Auto) {
            pageObject.insert(QStringLiteral("borderlessOverride"),
                              borderlessOverrideKey(page.borderlessOverride));
        }
        // One optional object per page. A page without slots writes nothing, so
        // a layout-free project stays byte-identical to a 1.0.1 file. Inside it,
        // the template and the style are omitted at their defaults too.
        if (page.hasLayout()) {
            QJsonObject layout;
            if (!page.layoutId.isEmpty())
                layout.insert(QStringLiteral("template"), page.layoutId);
            if (!(page.slotStyle == SlotStyle{}))
                layout.insert(QStringLiteral("style"), slotStyleToJson(page.slotStyle));
            QJsonArray slotArray;
            for (const Slot &slot : page.slotList) {
                slotArray.append(QJsonObject{
                    {QStringLiteral("id"), slot.id},
                    {QStringLiteral("unit"),
                     QJsonArray{slot.unitRect.x(), slot.unitRect.y(),
                                slot.unitRect.width(), slot.unitRect.height()}}});
            }
            layout.insert(QStringLiteral("slots"), slotArray);
            pageObject.insert(QStringLiteral("layout"), layout);
        }
        QJsonArray frames;
        for (const Frame &frame : page.frames)
            frames.append(frameToJson(frame, projectDir, portableDir, portableNames));
        pageObject.insert(QStringLiteral("frames"), frames);
        pages.append(pageObject);
    }
    root.insert(QStringLiteral("pages"), pages);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

Result<DocumentSnapshot> ProjectStore::deserialize(const QByteArray &json)
{
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
        return error(QStringLiteral("project-corrupt"),
                     QCoreApplication::translate("PhotoPrint", "Ezt a projektet most nem tudjuk megnyitni."));

    const QJsonObject root = parsed.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String("photoprint-project"))
        return error(QStringLiteral("project-format"),
                     QCoreApplication::translate("PhotoPrint", "Ezt a projektet most nem tudjuk megnyitni."));
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version != kFormatVersion) {
        return error(QStringLiteral("project-version"),
                     QCoreApplication::translate("PhotoPrint", "Ezt a projektet az alkalmazás egy újabb változata készítette."));
    }

    const auto document = parseDocument(root, QString());
    if (!document)
        return document.error();
    return document.value().document;
}

Result<void> ProjectStore::save(const QString &path, const DocumentSnapshot &document,
                                SaveMode mode)
{
    // An empty project cannot be opened again, so it is not written either.
    if (document.pages.isEmpty()) {
        return error(QStringLiteral("project-empty"),
                     QCoreApplication::translate("PhotoPrint", "Nincs mit elmenteni."));
    }
    if (path.isEmpty() || QFileInfo(path).fileName().isEmpty()) {
        return error(QStringLiteral("project-path"),
                     QCoreApplication::translate("PhotoPrint", "A projektet nem sikerült menteni."));
    }

    const QFileInfo info(path);
    const QDir projectDir(info.absolutePath());
    if (!projectDir.exists() && !QDir().mkpath(projectDir.absolutePath())) {
        return error(QStringLiteral("project-dir"),
                     QCoreApplication::translate("PhotoPrint", "A mappa nem hozható létre."));
    }

    QString portableDir;
    bool createdPortableDir = false;
    PortableChanges changes;
    auto rollback = [&] {
        // Remove the new copies first: a replaced target holds the half-written
        // copy at this point, and the backup has to land on a free name.
        for (const QString &file : changes.added)
            QFile::remove(file);
        for (const auto &entry : changes.replaced) {
            QFile::remove(entry.second);
            QFile::rename(entry.first, entry.second);
        }
        if (createdPortableDir) {
            QDir dir(portableDir);
            if (dir.exists() && dir.isEmpty())
                QDir().rmdir(portableDir);
        }
    };
    // The bundle is only rewritable until the project file itself is in place.
    auto discardBackups = [&] {
        for (const auto &entry : changes.replaced)
            QFile::remove(entry.first);
    };

    if (mode == SaveMode::Portable) {
        portableDir = info.absoluteFilePath() + QStringLiteral(".files");
        createdPortableDir = !QDir(portableDir).exists();
        if (!QDir().mkpath(portableDir)) {
            return error(QStringLiteral("portable-dir"),
                         QCoreApplication::translate("PhotoPrint", "A hordozható másolat mappája nem hozható létre."));
        }
        // The copies are only allowed inside the bundle. A `.files` entry that
        // is a symlink would pass the lexical containment check and still write
        // somewhere else, so the save stops before anything is copied.
        const QString bundleRoot = canonicalBundleRoot(portableDir);
        if (bundleRoot.isEmpty()) {
            return error(QStringLiteral("portable-dir"),
                         QCoreApplication::translate("PhotoPrint", "A hordozható másolat mappája nem hozható létre."));
        }
        const auto copied =
            copyPortableSources(document, bundleRoot, info.absoluteFilePath(), &changes);
        if (!copied) {
            rollback();
            return copied.error();
        }
    }

    const QByteArray json = serialize(document, projectDir.absolutePath(), portableDir);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        rollback();
        return error(QStringLiteral("project-write"),
                     QCoreApplication::translate("PhotoPrint", "A projektet nem sikerült menteni."));
    }
    if (file.write(json) != json.size()) {
        file.cancelWriting();
        rollback();
        return error(QStringLiteral("project-write"),
                     QCoreApplication::translate("PhotoPrint", "A projektet nem sikerült menteni."));
    }
    if (!file.commit()) {
        rollback();
        return error(QStringLiteral("project-write"),
                     QCoreApplication::translate("PhotoPrint", "A projektet nem sikerült menteni."));
    }
    discardBackups();
    return Result<void>();
}

Result<LoadedProject> ProjectStore::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return error(QStringLiteral("project-open"),
                     QCoreApplication::translate("PhotoPrint", "Ezt a projektet most nem tudjuk megnyitni."));
    const QByteArray json = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
        return error(QStringLiteral("project-corrupt"),
                     QCoreApplication::translate("PhotoPrint", "Ezt a projektet most nem tudjuk megnyitni."));
    const QJsonObject root = parsed.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String("photoprint-project"))
        return error(QStringLiteral("project-format"),
                     QCoreApplication::translate("PhotoPrint", "Ezt a projektet most nem tudjuk megnyitni."));
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version != kFormatVersion)
        return error(QStringLiteral("project-version"),
                     QCoreApplication::translate("PhotoPrint", "Ezt a projektet az alkalmazás egy újabb változata készítette."));

    const auto document = parseDocument(root, QFileInfo(path).absolutePath());
    if (!document)
        return document.error();

    LoadedProject loaded;
    loaded.path = QFileInfo(path).absoluteFilePath();
    loaded.document = document.value().document;
    loaded.issues = document.value().issues;
    return loaded;
}

} // namespace pp
