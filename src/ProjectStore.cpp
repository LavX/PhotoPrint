#include "ProjectStore.h"

#include "ImageSource.h"
#include "PaperCatalog.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"
#include "StickerAssets.h"

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

/// One stored asset path resolved through the same trust rules as a frame
/// source. `blocked` and `refused` are the relink hints: the path was not
/// trusted, so it is never opened, but the cleaned form can start a file
/// dialog next to it.
struct ResolvedSource {
    QString path;
    bool blocked = false;
    QString refused;
};

ResolvedSource resolveStoredSource(const QString &projectDir, const QString &stored,
                                   const QString &original, const QByteArray &expectedSha256)
{
    ResolvedSource resolved;
    QString contained;
    if (!stored.isEmpty()) {
        if (QDir::isAbsolutePath(stored)) {
            // This application never writes an absolute `source`: a normal
            // project stores the relative path plus an absolute `original`, a
            // portable copy stores only a contained relative path. A recorded
            // absolute value is in the same trust class as an escape.
            resolved.refused = QDir::cleanPath(stored);
            resolved.blocked = true;
        } else if (projectDir.isEmpty()) {
            resolved.blocked = true;
        } else {
            const QString joined =
                QDir::cleanPath(QDir(projectDir).absoluteFilePath(stored));
            contained = resolveProjectSource(projectDir, stored);
            resolved.blocked = contained.isEmpty();
            if (resolved.blocked)
                resolved.refused = joined;
        }
    }

    if (!contained.isEmpty() && QFileInfo::exists(contained)) {
        resolved.path = contained;
    } else {
        // The absolute `original` is only worth the stat and the full content
        // hash when the contained source cannot be used.
        const QString fallback =
            resolveOriginalSource(projectDir, original, expectedSha256);
        resolved.path = fallback.isEmpty() ? contained : fallback;
    }
    return resolved;
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

    const auto addSource = [&](const QString &path, const QByteArray &sha256) {
        // A shipped sticker is an alias, not a file: there is nothing to copy
        // and nothing to name in the bundle.
        if (path.isEmpty() || isBuiltinStickerPath(path))
            return;
        const QString source = lexicalPath(path);
        if (baseBySource.contains(source))
            return;
        const QString base = QFileInfo(source).fileName();
        const QString hash = QString::fromLatin1(sha256.toHex());
        baseBySource.insert(source, base);
        hashBySource.insert(source, hash);
        sourcesByBase[base].insert(source);
        hashesByBase[base].insert(hash);
    };

    for (const Page &page : document.pages) {
        for (const Frame &frame : page.frames)
            addSource(frame.sourcePath, frame.sourceSha256);
        // A background picture is carried by a portable copy exactly like a
        // photograph. Only a picture background has a file; a solid or gradient
        // page adds nothing.
        if (page.background.kind == BackgroundKind::Image)
            addSource(page.background.imagePath, page.background.imageSha256);
        // A sticker travels like a photograph: its own file, copied once.
        for (const Sticker &sticker : page.stickers)
            addSource(sticker.imagePath, sticker.imageSha256);
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

/// The mat width the old model allowed. A project saved with a mat could not
/// hold a larger value, because the loader clamped it to this, so migration
/// clamps a hand-edited file the same way before it becomes a gap.
constexpr double kLegacyMaxBorderMm = 20.0;

/// Turns a saved mat into the spacing it visibly was. The photograph used to
/// sit `border` inside its cell, so the white edge of the paper was
/// `margin + border` and two neighbours were `gutter + 2 * border` apart. The
/// new model has no inset, so the margin takes the border once and the gutter
/// takes it twice. A rounded corner started at the cell edge and the photo was
/// inset by the border, so the visible radius was `radius - border`; it keeps
/// that visible radius instead of growing back with the cell. Both automatic
/// values are resolved first, because the saved absence is not a number; the
/// migrated values are always explicit so the spacing cannot change again when
/// the paper does.
SlotStyle migrateLegacyMat(SlotStyle style, double borderMm, const QSizeF &sheetMm)
{
    if (!(borderMm > 0.0) || !std::isfinite(borderMm))
        return style;
    const double border = std::clamp(borderMm, 0.0, kLegacyMaxBorderMm);
    // Resolve both bases before writing either field: an automatic gutter
    // follows the old margin, not the migrated one.
    const double marginBase = SlotGeometry::effectiveMarginMm(style, sheetMm);
    const double gutterBase = SlotGeometry::effectiveGutterMm(style, sheetMm);
    style.marginMm =
        std::clamp(marginBase + border, 0.0, SlotGeometry::kMaxMarginMm);
    style.gutterMm =
        std::clamp(gutterBase + 2.0 * border, 0.0, SlotGeometry::kMaxGutterMm);
    style.cornerRadiusMm =
        std::max(0.0, SlotGeometry::sanitiseStyle(style).cornerRadiusMm - border);
    return style;
}

QJsonObject slotStyleToJson(const SlotStyle &stored)
{
    const SlotStyle style = SlotGeometry::sanitiseStyle(stored);
    QJsonObject object;
    // An unset margin or gutter means automatic, so the key is omitted and an
    // older build keeps its own default. An explicit zero is written: it is a
    // real choice, not the absence of one.
    if (style.marginMm)
        object.insert(QStringLiteral("marginMm"), *style.marginMm);
    if (style.gutterMm)
        object.insert(QStringLiteral("gutterMm"), *style.gutterMm);
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

ParsedLayout layoutFromJson(const QJsonValue &value, const QSizeF &sheetMm)
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
        // A missing key keeps the field unset, which is "automatic"; a present
        // number, including zero, is an explicit choice.
        const QJsonValue marginValue = styleObject.value(QStringLiteral("marginMm"));
        if (marginValue.isDouble())
            style.marginMm = marginValue.toDouble();
        const QJsonValue gutterValue = styleObject.value(QStringLiteral("gutterMm"));
        if (gutterValue.isDouble())
            style.gutterMm = gutterValue.toDouble();
        style.cornerRadiusMm =
            styleObject.value(QStringLiteral("cornerRadiusMm")).toDouble(0.0);
        // The old mat is migrated before sanitising, so its width becomes part
        // of the gutter instead of disappearing with the dropped colour.
        const double borderMm = styleObject.value(QStringLiteral("borderMm")).toDouble(0.0);
        parsed.style = SlotGeometry::sanitiseStyle(
            migrateLegacyMat(style, borderMm, sheetMm));
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
    // The inspector's ranges are model rules, not a display preference: a
    // hand-edited or older file never loads a zoom or an in-frame rotation the
    // sliders and the gestures cannot represent. A non-finite value is
    // corruption, so it becomes the neutral value instead of saturating.
    const double storedRotation =
        object.value(QStringLiteral("imageRotationDeg")).toDouble();
    frame.imageRotationDeg = std::isfinite(storedRotation)
        ? std::clamp(storedRotation, kMinImageRotationDeg, kMaxImageRotationDeg)
        : 0.0;
    const double storedZoom = object.value(QStringLiteral("zoom")).toDouble(1.0);
    frame.zoom = std::isfinite(storedZoom)
        ? std::clamp(storedZoom, kMinFrameZoom, kMaxFrameZoom)
        : 1.0;
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

QString backgroundKindKey(BackgroundKind kind)
{
    switch (kind) {
    case BackgroundKind::Solid:
        return QStringLiteral("solid");
    case BackgroundKind::Gradient:
        return QStringLiteral("gradient");
    case BackgroundKind::Image:
        return QStringLiteral("image");
    case BackgroundKind::None:
        break;
    }
    return QStringLiteral("none");
}

BackgroundKind backgroundKindFromKey(const QString &key)
{
    if (key == QLatin1String("solid"))
        return BackgroundKind::Solid;
    if (key == QLatin1String("gradient"))
        return BackgroundKind::Gradient;
    if (key == QLatin1String("image"))
        return BackgroundKind::Image;
    return BackgroundKind::None;
}

QString backgroundFitKey(BackgroundFit fit)
{
    switch (fit) {
    case BackgroundFit::Fit:
        return QStringLiteral("fit");
    case BackgroundFit::Tile:
        return QStringLiteral("tile");
    case BackgroundFit::Fill:
        break;
    }
    return QStringLiteral("fill");
}

BackgroundFit backgroundFitFromKey(const QString &key)
{
    if (key == QLatin1String("fit"))
        return BackgroundFit::Fit;
    if (key == QLatin1String("tile"))
        return BackgroundFit::Tile;
    return BackgroundFit::Fill;
}

QString colourKey(QRgb colour)
{
    return QStringLiteral("#%1").arg(
        QString::number(colour & 0x00ffffffu, 16).rightJustified(6, QLatin1Char('0')));
}

QRgb colourFromKey(const QString &key)
{
    const QColor colour(key);
    if (!colour.isValid())
        return 0xffffffffu;
    return 0xff000000u | (colour.rgb() & 0x00ffffffu);
}

QJsonObject backgroundToJson(const PageBackground &stored, const QString &projectDir,
                             const QString &portableDir,
                             const QHash<QString, QString> &portableNames)
{
    const PageBackground background = sanitiseBackground(stored);
    QJsonObject object;
    object.insert(QStringLiteral("kind"), backgroundKindKey(background.kind));
    object.insert(QStringLiteral("color"), colourKey(background.solid));
    QJsonArray stops;
    for (const BackgroundStop &stop : background.stops) {
        stops.append(QJsonObject{{QStringLiteral("pos"), stop.first},
                                 {QStringLiteral("color"), colourKey(stop.second)}});
    }
    object.insert(QStringLiteral("stops"), stops);
    object.insert(QStringLiteral("angleDeg"), background.angleDeg);
    object.insert(QStringLiteral("fit"), backgroundFitKey(background.fit));
    object.insert(QStringLiteral("tileMm"), background.tileMm);
    object.insert(QStringLiteral("opacity"), background.opacity);

    // The picture path follows the frame rule exactly: a normal project stores
    // the project-relative path plus the absolute original, a portable copy
    // stores only a contained relative path and no original.
    if (!portableDir.isEmpty()) {
        const QString source = lexicalPath(background.imagePath);
        const QString name = portableNames.value(source);
        if (name.isEmpty()) {
            object.insert(QStringLiteral("image"), QString());
        } else {
            const QString target = QDir(portableDir).filePath(name);
            object.insert(QStringLiteral("image"), QDir(projectDir).relativeFilePath(target));
        }
    } else if (!background.imagePath.isEmpty()) {
        const QString relative = QDir(projectDir).relativeFilePath(background.imagePath);
        object.insert(QStringLiteral("image"), relative.isEmpty() ? background.imagePath : relative);
        object.insert(QStringLiteral("original"), background.imagePath);
    }
    object.insert(QStringLiteral("sha256"),
                  QString::fromLatin1(background.imageSha256.toHex()));
    return object;
}

PageBackground backgroundFromJson(const QJsonObject &object, const QString &projectDir)
{
    PageBackground background;
    background.kind = backgroundKindFromKey(object.value(QStringLiteral("kind")).toString());
    background.solid = colourFromKey(object.value(QStringLiteral("color")).toString());
    const QJsonArray stops = object.value(QStringLiteral("stops")).toArray();
    for (const QJsonValue &value : stops) {
        if (!value.isObject())
            continue;
        const QJsonObject stopObject = value.toObject();
        background.stops.append(
            {stopObject.value(QStringLiteral("pos")).toDouble(),
             colourFromKey(stopObject.value(QStringLiteral("color")).toString())});
    }
    background.angleDeg = object.value(QStringLiteral("angleDeg")).toDouble();
    background.fit = backgroundFitFromKey(object.value(QStringLiteral("fit")).toString());
    background.tileMm = object.value(QStringLiteral("tileMm")).toDouble();
    background.opacity = object.value(QStringLiteral("opacity")).toDouble(100.0);
    background.imageSha256 =
        QByteArray::fromHex(object.value(QStringLiteral("sha256")).toString().toLatin1());

    const QString stored = object.value(QStringLiteral("image")).toString();
    const QString original = object.value(QStringLiteral("original")).toString();
    const ResolvedSource resolved =
        resolveStoredSource(projectDir, stored, original, background.imageSha256);
    background.imagePath = resolved.path;
    // A picture whose file cannot be resolved still loads as a picture with an
    // empty path: the panel shows the hint and the picker, and saving keeps the
    // rest of the page instead of silently turning it into a white sheet.
    return sanitiseBackground(background);
}

QJsonObject stickerToJson(const Sticker &sticker, const QString &projectDir,
                          const QString &portableDir,
                          const QHash<QString, QString> &portableNames)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), sticker.id);

    // The picture path follows the frame rule exactly: a normal project stores
    // the project-relative path plus the absolute original, a portable copy
    // stores only a contained relative path and no original. A shipped sticker
    // is the exception: it stores its pinned alias, so the project keeps
    // working when it moves and when the app is updated.
    if (isBuiltinStickerPath(sticker.imagePath)) {
        object.insert(QStringLiteral("image"), sticker.imagePath);
    } else if (!portableDir.isEmpty()) {
        const QString source = lexicalPath(sticker.imagePath);
        const QString name = portableNames.value(source);
        if (name.isEmpty()) {
            object.insert(QStringLiteral("image"), QString());
        } else {
            const QString target = QDir(portableDir).filePath(name);
            object.insert(QStringLiteral("image"), QDir(projectDir).relativeFilePath(target));
        }
    } else if (!sticker.imagePath.isEmpty()) {
        const QString relative = QDir(projectDir).relativeFilePath(sticker.imagePath);
        object.insert(QStringLiteral("image"), relative.isEmpty() ? sticker.imagePath : relative);
        object.insert(QStringLiteral("original"), sticker.imagePath);
    }
    object.insert(QStringLiteral("sha256"), QString::fromLatin1(sticker.imageSha256.toHex()));
    object.insert(QStringLiteral("cx"), sticker.cx);
    object.insert(QStringLiteral("cy"), sticker.cy);
    object.insert(QStringLiteral("width"), sticker.width);
    object.insert(QStringLiteral("height"), sticker.height);
    object.insert(QStringLiteral("angleDeg"), sticker.angleDeg);
    object.insert(QStringLiteral("opacity"), sticker.opacity);
    object.insert(QStringLiteral("z"), sticker.z);
    return object;
}

Sticker stickerFromJson(const QJsonObject &object, const QString &projectDir)
{
    Sticker sticker;
    sticker.id = object.value(QStringLiteral("id")).toString();
    sticker.imageSha256 =
        QByteArray::fromHex(object.value(QStringLiteral("sha256")).toString().toLatin1());
    sticker.cx = object.value(QStringLiteral("cx")).toDouble();
    sticker.cy = object.value(QStringLiteral("cy")).toDouble();
    sticker.width = object.value(QStringLiteral("width")).toDouble();
    sticker.height = object.value(QStringLiteral("height")).toDouble();
    sticker.angleDeg = object.value(QStringLiteral("angleDeg")).toDouble();
    sticker.opacity = object.value(QStringLiteral("opacity")).toInt(100);
    sticker.z = object.value(QStringLiteral("z")).toInt(0);

    const QString stored = object.value(QStringLiteral("image")).toString();
    const QString original = object.value(QStringLiteral("original")).toString();
    if (isBuiltinStickerPath(stored)) {
        // A shipped sticker keeps its alias: it is resolved against the
        // application's own resources, never against the project folder.
        sticker.imagePath = stored;
    } else {
        const ResolvedSource resolved =
            resolveStoredSource(projectDir, stored, original, sticker.imageSha256);
        sticker.imagePath = resolved.path;
    }
    // A sticker whose file cannot be resolved still loads, with an empty path:
    // the Elem panel shows the hint and the picker, the page paints the
    // placeholder, and saving keeps the rest of the page instead of silently
    // dropping the layer. Sanitising happens in the repair point.
    return sticker;
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
        const ParsedLayout layout =
            layoutFromJson(pageObject.value(QStringLiteral("layout")), pageSizeMm(page));
        page.slotList = layout.slotList;
        page.slotStyle = layout.style;
        page.layoutId = layout.layoutId;
        page.background = backgroundFromJson(
            pageObject.value(QStringLiteral("background")).toObject(), projectDir);
        // Stickers are optional and independent of the layout: a page without
        // them writes nothing, so every existing project loads as the page it
        // was. A malformed entry is dropped by the sanitiser, not the page.
        const QJsonValue stickersValue = pageObject.value(QStringLiteral("stickers"));
        if (stickersValue.isArray()) {
            for (const QJsonValue &stickerValue : stickersValue.toArray()) {
                if (page.stickers.size() >= kMaxStickersPerPage)
                    break;
                if (!stickerValue.isObject())
                    continue;
                page.stickers.append(
                    stickerFromJson(stickerValue.toObject(), projectDir));
            }
        }
        for (const QJsonValue &frameValue : pageObject.value(QStringLiteral("frames")).toArray()) {
            const QJsonObject frameObject = frameValue.toObject();
            Frame frame = frameFromJson(frameObject);
            const QString stored = frameObject.value(QStringLiteral("source")).toString();
            const QString original = frameObject.value(QStringLiteral("original")).toString();

            const ResolvedSource resolved =
                resolveStoredSource(projectDir, stored, original, frame.sourceSha256);
            frame.sourcePath = resolved.path;

            SourceIssue issue;
            issue.frameId = frame.id;
            issue.path = frame.sourcePath;
            issue.refusedPath = resolved.refused;
            issue.expectedSha256 = frame.sourceSha256;
            if (!frame.sourcePath.isEmpty() && QFileInfo::exists(frame.sourcePath)) {
                const QByteArray current = fileSha256(frame.sourcePath);
                if (!issue.expectedSha256.isEmpty() && current != issue.expectedSha256) {
                    issue.changed = true;
                    parsed.issues.append(issue);
                }
            } else {
                issue.missing = true;
                issue.blocked = resolved.blocked;
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

    // One copy path for photographs and the background picture. A photograph is
    // required: a missing one stops the save, exactly as it always did. A
    // background is decoration: a missing file is skipped, so a page whose
    // picture was moved still saves the rest of the project and reports the
    // missing picture in the panel.
    const auto copySource = [&](const QString &path, const QByteArray &sha256,
                                bool required) -> Result<void> {
        // A shipped sticker lives in the application, not on the author's disk.
        if (isBuiltinStickerPath(path))
            return Result<void>();
        if (path.isEmpty()) {
            if (!required)
                return Result<void>();
            return error(QStringLiteral("source-missing"),
                         QCoreApplication::translate("PhotoPrint", "Ezt a fotót nem találjuk."));
        }
        const QString source = lexicalPath(path);
        if (handled.contains(source))
            return Result<void>();
        handled.insert(source);

        const QFileInfo info(source);
        if (!info.exists() || !info.isFile()) {
            if (!required)
                return Result<void>();
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

        // A file loaded from a portable bundle already points at the carried
        // file, so source and target are the same file. Nothing has to be
        // copied, and moving the target aside would destroy the only copy this
        // save could read.
        if (source == target)
            return Result<void>();

        if (QFileInfo::exists(target)) {
            if (fileSha256(target) == sha256)
                return Result<void>(); // already carried by an earlier save
            // The previous bundle already has a different file under this name.
            // Move it aside rather than deleting it: a failure later in this
            // save has to be able to put it back.
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
        // A photograph's copy must still be the recorded photograph. A
        // background is decorative and is not verified: a changed file is
        // copied as it is, and the page still prints.
        if (required && !sha256.isEmpty() && fileSha256(target) != sha256) {
            return error(QStringLiteral("portable-hash"),
                         QCoreApplication::translate("PhotoPrint", "A fotó másolata nem egyezik az eredetivel."));
        }
        return Result<void>();
    };

    for (const Page &page : document.pages) {
        for (const Frame &frame : page.frames) {
            const auto copied = copySource(frame.sourcePath, frame.sourceSha256, true);
            if (!copied)
                return copied;
        }
        if (page.background.kind == BackgroundKind::Image) {
            const auto copied = copySource(page.background.imagePath,
                                           page.background.imageSha256, false);
            if (!copied)
                return copied;
        }
        // A sticker is decoration, like a background picture: a missing file is
        // skipped so the rest of the bundle still saves, and the page reports
        // the lost sticker in the panel.
        for (const Sticker &sticker : page.stickers) {
            const auto copied = copySource(sticker.imagePath, sticker.imageSha256, false);
            if (!copied)
                return copied;
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
        // The background is one optional object per page, omitted only when the
        // whole struct is the default. A page that set a colour and returned to
        // white still writes it, so the colour is not lost; an older build
        // ignores the key and a 1.0.1 project stays byte-identical.
        if (!(page.background == PageBackground{})) {
            pageObject.insert(QStringLiteral("background"),
                              backgroundToJson(page.background, projectDir, portableDir,
                                               portableNames));
        }
        // Optional array, omitted when empty, so a page that never carried a
        // sticker stays byte-identical to a 1.0.1 file and an older build
        // ignores the key.
        if (!page.stickers.isEmpty()) {
            QJsonArray stickerArray;
            for (const Sticker &sticker : page.stickers) {
                stickerArray.append(
                    stickerToJson(sticker, projectDir, portableDir, portableNames));
            }
            pageObject.insert(QStringLiteral("stickers"), stickerArray);
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
