#include "ProjectStore.h"

#include "ImageSource.h"
#include "PaperCatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

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

QJsonObject frameToJson(const Frame &frame, const QString &projectDir, const QString &portableDir)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), frame.id);

    QString stored = frame.sourcePath;
    if (!portableDir.isEmpty()) {
        const QString target = QDir(portableDir).filePath(QFileInfo(frame.sourcePath).fileName());
        stored = QDir(projectDir).relativeFilePath(target);
    } else {
        const QString relative = QDir(projectDir).relativeFilePath(frame.sourcePath);
        if (!relative.isEmpty())
            stored = relative;
    }
    object.insert(QStringLiteral("source"), stored);
    object.insert(QStringLiteral("original"), frame.sourcePath);
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
    object.insert(QStringLiteral("tone"), QJsonObject{{QStringLiteral("exposureEv"), frame.tone.exposureEv},
                                                      {QStringLiteral("shadowLift"), frame.tone.shadowLift},
                                                      {QStringLiteral("highlightCompress"), frame.tone.highlightCompress}});
    return object;
}

Frame frameFromJson(const QJsonObject &object, const QString &projectDir)
{
    Frame frame;
    frame.id = object.value(QStringLiteral("id")).toString();
    const QString stored = object.value(QStringLiteral("source")).toString();
    const QString original = object.value(QStringLiteral("original")).toString();
    const QFileInfo storedInfo(QDir(projectDir).filePath(stored));
    frame.sourcePath = storedInfo.exists() ? storedInfo.absoluteFilePath()
                                           : QFileInfo(original).absoluteFilePath();
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
    frame.brightness = object.value(QStringLiteral("brightness")).toDouble();
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

} // namespace

QByteArray ProjectStore::serialize(const DocumentSnapshot &document, const QString &projectDir,
                                   const QString &portableDir)
{
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
            frames.append(frameToJson(frame, projectDir, portableDir));
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
                     QStringLiteral("Ezt a projektet most nem tudjuk megnyitni."));

    const QJsonObject root = parsed.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String("photoprint-project"))
        return error(QStringLiteral("project-format"),
                     QStringLiteral("Ezt a projektet most nem tudjuk megnyitni."));
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version != kFormatVersion) {
        return error(QStringLiteral("project-version"),
                     QStringLiteral("Ezt a projektet az alkalmazás egy újabb változata készítette."));
    }

    DocumentSnapshot document;
    document.recipe = recipeFromJson(root.value(QStringLiteral("recipe")).toObject());
    document.currentPage = root.value(QStringLiteral("currentPage")).toInt(0);
    document.fineQuality = root.value(QStringLiteral("fineQuality")).toBool(true);
    for (const QJsonValue &pageValue : root.value(QStringLiteral("pages")).toArray()) {
        const QJsonObject pageObject = pageValue.toObject();
        Page page;
        page.id = pageObject.value(QStringLiteral("id")).toString();
        page.paper = paperFromJson(pageObject.value(QStringLiteral("paper")).toObject());
        page.landscape = pageObject.value(QStringLiteral("landscape")).toBool();
        for (const QJsonValue &frameValue : pageObject.value(QStringLiteral("frames")).toArray())
            page.frames.append(frameFromJson(frameValue.toObject(), QDir::currentPath()));
        document.pages.append(page);
    }
    if (document.pages.isEmpty())
        return error(QStringLiteral("project-empty"),
                     QStringLiteral("Ezt a projektet most nem tudjuk megnyitni."));
    document.currentPage = std::clamp(document.currentPage, 0, int(document.pages.size()) - 1);
    return document;
}

Result<void> ProjectStore::save(const QString &path, const DocumentSnapshot &document,
                                SaveMode mode)
{
    const QFileInfo info(path);
    const QDir projectDir(info.absolutePath());
    if (!projectDir.exists() && !QDir().mkpath(projectDir.absolutePath()))
        return error(QStringLiteral("project-dir"), QStringLiteral("A mappa nem hozható létre."));

    QString portableDir;
    if (mode == SaveMode::Portable) {
        portableDir = info.absoluteFilePath() + QStringLiteral(".files");
        if (!QDir().mkpath(portableDir)) {
            return error(QStringLiteral("portable-dir"),
                         QStringLiteral("A hordozható másolat mappája nem hozható létre."));
        }
        for (const Page &page : document.pages) {
            for (const Frame &frame : page.frames) {
                const QFileInfo sourceInfo(frame.sourcePath);
                if (!sourceInfo.exists())
                    return error(QStringLiteral("source-missing"),
                                 QStringLiteral("Ezt a fotót nem találjuk."));
                const QString target = QDir(portableDir).filePath(sourceInfo.fileName());
                if (QFileInfo::exists(target)) {
                    if (fileSha256(target) != frame.sourceSha256)
                        return error(QStringLiteral("portable-conflict"),
                                     QStringLiteral("Ugyanilyen nevű fotó már van a másolatban."));
                    continue;
                }
                if (!QFile::copy(frame.sourcePath, target))
                    return error(QStringLiteral("portable-copy"),
                                 QStringLiteral("A fotót nem sikerült a másolatba tenni."));
                if (fileSha256(target) != frame.sourceSha256) {
                    QFile::remove(target);
                    return error(QStringLiteral("portable-hash"),
                                 QStringLiteral("A fotó másolata nem egyezik az eredetivel."));
                }
            }
        }
    }

    const QByteArray json = serialize(document, projectDir.absolutePath(), portableDir);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return error(QStringLiteral("project-write"), QStringLiteral("A projektet nem sikerült menteni."));
    if (file.write(json) != json.size())
        return error(QStringLiteral("project-write"), QStringLiteral("A projektet nem sikerült menteni."));
    if (!file.commit())
        return error(QStringLiteral("project-write"), QStringLiteral("A projektet nem sikerült menteni."));
    return Result<void>();
}

Result<LoadedProject> ProjectStore::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return error(QStringLiteral("project-open"),
                     QStringLiteral("Ezt a projektet most nem tudjuk megnyitni."));
    const QByteArray json = file.readAll();
    file.close();

    // Source resolution needs the project directory, so parse manually first.
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
        return error(QStringLiteral("project-corrupt"),
                     QStringLiteral("Ezt a projektet most nem tudjuk megnyitni."));
    const QJsonObject root = parsed.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String("photoprint-project"))
        return error(QStringLiteral("project-format"),
                     QStringLiteral("Ezt a projektet most nem tudjuk megnyitni."));
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version != kFormatVersion)
        return error(QStringLiteral("project-version"),
                     QStringLiteral("Ezt a projektet az alkalmazás egy újabb változata készítette."));

    LoadedProject loaded;
    loaded.path = QFileInfo(path).absoluteFilePath();
    const QDir projectDir(QFileInfo(path).absolutePath());
    loaded.document.recipe = recipeFromJson(root.value(QStringLiteral("recipe")).toObject());
    loaded.document.currentPage = root.value(QStringLiteral("currentPage")).toInt(0);
    loaded.document.fineQuality = root.value(QStringLiteral("fineQuality")).toBool(true);
    for (const QJsonValue &pageValue : root.value(QStringLiteral("pages")).toArray()) {
        const QJsonObject pageObject = pageValue.toObject();
        Page page;
        page.id = pageObject.value(QStringLiteral("id")).toString();
        page.paper = paperFromJson(pageObject.value(QStringLiteral("paper")).toObject());
        page.landscape = pageObject.value(QStringLiteral("landscape")).toBool();
        for (const QJsonValue &frameValue : pageObject.value(QStringLiteral("frames")).toArray()) {
            const QJsonObject frameObject = frameValue.toObject();
            Frame frame = frameFromJson(frameObject, projectDir.absolutePath());
            const QString stored = frameObject.value(QStringLiteral("source")).toString();
            const QString original = frameObject.value(QStringLiteral("original")).toString();
            if (!QFileInfo::exists(frame.sourcePath) && QFileInfo::exists(original))
                frame.sourcePath = QFileInfo(original).absoluteFilePath();

            SourceIssue issue;
            issue.frameId = frame.id;
            issue.path = frame.sourcePath;
            issue.expectedSha256 = frame.sourceSha256;
            if (!QFileInfo::exists(frame.sourcePath)) {
                issue.missing = true;
                loaded.issues.append(issue);
            } else {
                const QByteArray current = fileSha256(frame.sourcePath);
                if (!issue.expectedSha256.isEmpty() && current != issue.expectedSha256) {
                    issue.changed = true;
                    loaded.issues.append(issue);
                }
            }
            Q_UNUSED(stored)
            page.frames.append(frame);
        }
        loaded.document.pages.append(page);
    }
    if (loaded.document.pages.isEmpty())
        return error(QStringLiteral("project-empty"),
                     QStringLiteral("Ezt a projektet most nem tudjuk megnyitni."));
    loaded.document.currentPage =
        std::clamp(loaded.document.currentPage, 0, int(loaded.document.pages.size()) - 1);
    return loaded;
}

} // namespace pp
