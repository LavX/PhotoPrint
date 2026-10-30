#include "ProjectStore.h"

#include "ImageSource.h"
#include "PaperCatalog.h"

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
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace pp {

namespace {

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

/// `original` is the legacy absolute path recorded for relinking. A relative
/// value is only honoured through the same containment rule as `source`.
QString resolveOriginalSource(const QString &projectDir, const QString &original)
{
    if (original.isEmpty())
        return QString();
    if (QDir::isAbsolutePath(original))
        return QDir::cleanPath(original);
    return resolveProjectSource(projectDir, original);
}

/// Deterministic file names for a portable copy. Unrelated sources with the
/// same base name get a short content hash, so two different photos can never
/// overwrite each other. Identical content is copied once and keeps the plain
/// name, which also keeps 1.0.0 portable copies stable.
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
            const QString base = QFileInfo(source).fileName();
            if (baseBySource.contains(source))
                continue;
            const QString hash = QString::fromLatin1(frame.sourceSha256.toHex());
            baseBySource.insert(source, base);
            hashBySource.insert(source, hash);
            sourcesByBase[base].insert(source);
            hashesByBase[base].insert(hash);
        }
    }

    QHash<QString, QString> names;
    int anonymous = 1;
    for (auto it = baseBySource.cbegin(); it != baseBySource.cend(); ++it) {
        const QString base = it.value();
        const QSet<QString> hashes = hashesByBase.value(base);
        const bool sameContent =
            sourcesByBase.value(base).size() > 1 && !hashes.contains(QString()) && hashes.size() == 1;
        if (sourcesByBase.value(base).size() <= 1 || sameContent) {
            names.insert(it.key(), base);
            continue;
        }

        const QFileInfo info(it.key());
        QString stem = info.completeBaseName();
        if (stem.isEmpty())
            stem = base;
        const QString hash = hashBySource.value(it.key());
        const QString suffix = info.suffix();
        QString unique = hash.isEmpty()
            ? QStringLiteral("%1-%2").arg(stem).arg(anonymous++)
            : QStringLiteral("%1-%2").arg(stem, hash.left(8));
        if (!suffix.isEmpty())
            unique += QLatin1Char('.') + suffix;
        names.insert(it.key(), unique);
    }
    return names;
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
    object.insert(QStringLiteral("filter"), frame.filter);
    object.insert(QStringLiteral("filterStrength"), frame.filterStrength);
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
    frame.filter = object.value(QStringLiteral("filter")).toString(QStringLiteral("original"));
    frame.filterStrength = object.value(QStringLiteral("filterStrength")).toInt(100);
    frame.upscale = object.value(QStringLiteral("upscale")).toBool(false);
    frame.autoTone = object.value(QStringLiteral("autoTone")).toBool(true);
    frame.brightness = clampBrightnessEv(object.value(QStringLiteral("brightness")).toDouble());
    // Absent keys are 1.0.0 projects: plain frames without a trial label.
    frame.label = object.value(QStringLiteral("label")).toString();
    frame.paperOverride = object.value(QStringLiteral("paperOverride")).toBool(false);
    frame.paperEv = object.value(QStringLiteral("paperEv")).toDouble();
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
    return recipe;
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
        for (const QJsonValue &frameValue : pageObject.value(QStringLiteral("frames")).toArray()) {
            const QJsonObject frameObject = frameValue.toObject();
            Frame frame = frameFromJson(frameObject);
            const QString stored = frameObject.value(QStringLiteral("source")).toString();
            const QString original = frameObject.value(QStringLiteral("original")).toString();

            bool blocked = false;
            QString contained;
            if (!stored.isEmpty()) {
                if (QDir::isAbsolutePath(stored)) {
                    // 1.0.0 never wrote an absolute source, but a recorded one
                    // is in the same trust class as `original`.
                    contained = QDir::cleanPath(stored);
                } else if (projectDir.isEmpty()) {
                    blocked = true;
                } else {
                    contained = resolveProjectSource(projectDir, stored);
                    blocked = contained.isEmpty();
                }
            }

            const QString fallback = resolveOriginalSource(projectDir, original);
            if (!contained.isEmpty() && QFileInfo::exists(contained)) {
                frame.sourcePath = contained;
            } else if (!fallback.isEmpty()) {
                // Absolute original: the normal way back to a photo that lives
                // outside the project folder on this machine.
                frame.sourcePath = fallback;
            } else {
                frame.sourcePath = contained; // empty when the stored path was refused
            }

            SourceIssue issue;
            issue.frameId = frame.id;
            issue.path = frame.sourcePath;
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
    return parsed;
}

/// Copies every unique source into the portable folder. The caller rolls the
/// listed targets back when any later step fails.
Result<void> copyPortableSources(const DocumentSnapshot &document, const QString &portableDir,
                                 const QString &projectFile, QList<QString> *created)
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

            if (QFileInfo::exists(target)) {
                if (fileSha256(target) == frame.sourceSha256)
                    continue; // already carried by an earlier save
                if (!QFile::remove(target)) {
                    return error(QStringLiteral("portable-write"),
                                 QCoreApplication::translate("PhotoPrint", "A fotót nem sikerült a másolatba tenni."));
                }
            }
            if (!QFile::copy(source, target)) {
                return error(QStringLiteral("portable-copy"),
                             QCoreApplication::translate("PhotoPrint", "A fotót nem sikerült a másolatba tenni."));
            }
            created->append(target);
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
    QList<QString> created;
    auto rollback = [&] {
        for (const QString &file : created)
            QFile::remove(file);
        if (createdPortableDir) {
            QDir dir(portableDir);
            if (dir.exists() && dir.isEmpty())
                QDir().rmdir(portableDir);
        }
    };

    if (mode == SaveMode::Portable) {
        portableDir = info.absoluteFilePath() + QStringLiteral(".files");
        createdPortableDir = !QDir(portableDir).exists();
        if (!QDir().mkpath(portableDir)) {
            return error(QStringLiteral("portable-dir"),
                         QCoreApplication::translate("PhotoPrint", "A hordozható másolat mappája nem hozható létre."));
        }
        const auto copied =
            copyPortableSources(document, portableDir, info.absoluteFilePath(), &created);
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
