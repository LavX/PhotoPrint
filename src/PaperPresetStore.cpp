#include "PaperPresetStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <cmath>

namespace pp {

namespace {

QJsonObject contextToJson(const PrintContext &context)
{
    QJsonObject object;
    object.insert(QStringLiteral("queue"), context.queueName);
    object.insert(QStringLiteral("driver"), context.driverIdentity);
    object.insert(QStringLiteral("ppd"), context.ppdFingerprint);
    object.insert(QStringLiteral("pageSize"), context.pageSizeOption);
    object.insert(QStringLiteral("media"), context.mediaTypeOption);
    object.insert(QStringLiteral("quality"), context.qualityOption);
    object.insert(QStringLiteral("color"), context.colorOption);
    object.insert(QStringLiteral("ink"), context.inkIdentity);
    object.insert(QStringLiteral("physicalWidthMm"), context.physicalWidthMm);
    object.insert(QStringLiteral("physicalHeightMm"), context.physicalHeightMm);
    object.insert(QStringLiteral("logicalWidthMm"), context.logicalWidthMm);
    object.insert(QStringLiteral("logicalHeightMm"), context.logicalHeightMm);
    object.insert(QStringLiteral("landscape"), context.landscape);
    object.insert(QStringLiteral("borderless"), context.borderless);
    return object;
}

PrintContext contextFromJson(const QJsonObject &object)
{
    PrintContext context;
    context.queueName = object.value(QStringLiteral("queue")).toString();
    context.driverIdentity = object.value(QStringLiteral("driver")).toString();
    context.ppdFingerprint = object.value(QStringLiteral("ppd")).toString();
    context.pageSizeOption = object.value(QStringLiteral("pageSize")).toString();
    context.mediaTypeOption = object.value(QStringLiteral("media")).toString();
    context.qualityOption = object.value(QStringLiteral("quality")).toString();
    context.colorOption = object.value(QStringLiteral("color")).toString();
    context.inkIdentity = object.value(QStringLiteral("ink")).toString(QStringLiteral("unknown"));
    context.physicalWidthMm = object.value(QStringLiteral("physicalWidthMm")).toDouble();
    context.physicalHeightMm = object.value(QStringLiteral("physicalHeightMm")).toDouble();
    context.logicalWidthMm = object.value(QStringLiteral("logicalWidthMm")).toDouble();
    context.logicalHeightMm = object.value(QStringLiteral("logicalHeightMm")).toDouble();
    context.landscape = object.value(QStringLiteral("landscape")).toBool();
    context.borderless = object.value(QStringLiteral("borderless")).toBool();
    return context;
}

/// A preset that may take part in matching. It needs an identity and a real
/// environment: an entry read as an empty context is a valid preset that would
/// match any caller whose context was never filled in, which is exactly the
/// correction a sheet with no measurements must not receive.
///
/// The paper tone must be a number too. match() hands it to MainWindow and into
/// ColorRecipe::paperStrength, which becomes an exposure through std::pow; a
/// non-finite value there would black out or blow out the cell.
bool isUsablePreset(const PaperPreset &preset)
{
    const PrintContext &context = preset.context;
    return !preset.id.isEmpty() && !context.queueName.isEmpty() && std::isfinite(preset.paperEv)
        && std::isfinite(context.physicalWidthMm) && context.physicalWidthMm > 0.0
        && std::isfinite(context.physicalHeightMm) && context.physicalHeightMm > 0.0
        && std::isfinite(context.logicalWidthMm) && context.logicalWidthMm >= 0.0
        && std::isfinite(context.logicalHeightMm) && context.logicalHeightMm >= 0.0;
}

Result<QList<PaperPreset>> corruptPresetFile()
{
    return error(QStringLiteral("preset-corrupt"),
                 QCoreApplication::translate("PhotoPrint", "A papírbeállítások fájlja sérült."));
}

} // namespace

QString PaperPresetStore::defaultPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("paper-presets.json"));
}

QString PaperPresetStore::newInkIdentity()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

PaperPresetStore::PaperPresetStore(QString path)
    : m_path(path.isEmpty() ? defaultPath() : std::move(path))
{
}

bool samePrintContext(const PrintContext &a, const PrintContext &b)
{
    // PrintContext::operator== owns the comparison; this name stays as the
    // store-side spelling so both paths cannot drift apart.
    return a == b;
}

QList<PaperPreset> PaperPresetStore::all() const
{
    const auto read = readAll();
    return read ? read.value() : QList<PaperPreset>{};
}

Result<QList<PaperPreset>> PaperPresetStore::readAll() const
{
    QList<PaperPreset> presets;
    QFile file(m_path);
    if (!file.exists())
        return presets;
    if (!file.open(QIODevice::ReadOnly))
        return error(QStringLiteral("preset-read"),
                     QCoreApplication::translate("PhotoPrint", "A papírbeállítások nem olvashatók."));
    const QByteArray data = file.readAll();
    if (data.trimmed().isEmpty())
        return presets;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
        return corruptPresetFile();
    for (const QJsonValue &value : document.array()) {
        if (!value.isObject())
            return corruptPresetFile();
        const QJsonObject object = value.toObject();
        const QJsonValue contextValue = object.value(QStringLiteral("context"));
        if (!contextValue.isObject())
            return corruptPresetFile();
        PaperPreset preset;
        preset.id = object.value(QStringLiteral("id")).toString();
        preset.context = contextFromJson(contextValue.toObject());
        preset.paperEv = object.value(QStringLiteral("paperEv")).toDouble();
        preset.label = object.value(QStringLiteral("label")).toString();
        preset.savedAt = QDateTime::fromString(object.value(QStringLiteral("savedAt")).toString(),
                                               Qt::ISODateWithMs);
        // Half-written entries are refused instead of being repaired into a
        // preset that matches things it was never measured against.
        if (!isUsablePreset(preset))
            return corruptPresetFile();
        presets.append(preset);
    }
    return presets;
}

std::optional<PaperPreset> PaperPresetStore::match(const PrintContext &context) const
{
    for (const PaperPreset &preset : all()) {
        if (samePrintContext(preset.context, context))
            return preset;
    }
    return std::nullopt;
}

Result<void> PaperPresetStore::save(const PaperPreset &preset)
{
    // What readAll() would refuse must not be written either: one empty context
    // on disk would make the whole file unreadable on the next start, so the
    // store would poison itself.
    if (!isUsablePreset(preset))
        return error(QStringLiteral("preset-context"),
                     QCoreApplication::translate("PhotoPrint", "A papírbeállítás nem menthető."));

    const QString directory = QFileInfo(m_path).absolutePath();
    if (!QDir().mkpath(directory))
        return error(QStringLiteral("preset-write"), QCoreApplication::translate("PhotoPrint", "A papírbeállítás nem menthető."));

    // The read-modify-write below must not interleave with another instance.
    QLockFile lock(QFileInfo(m_path).absoluteFilePath() + QStringLiteral(".lock"));
    // Called on the GUI thread: never freeze the window behind another
    // instance's lock. Failing fast with the real reason is better.
    if (!lock.tryLock(250))
        return error(QStringLiteral("preset-lock"), QCoreApplication::translate("PhotoPrint", "A papírbeállítás nem menthető."));

    const auto existing = readAll();
    if (!existing)
        return existing.error();
    QList<PaperPreset> presets = existing.value();
    for (int i = presets.size() - 1; i >= 0; --i) {
        if (samePrintContext(presets.at(i).context, preset.context))
            presets.removeAt(i);
    }
    presets.append(preset);

    QJsonArray array;
    for (const PaperPreset &entry : presets) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), entry.id);
        object.insert(QStringLiteral("context"), contextToJson(entry.context));
        object.insert(QStringLiteral("paperEv"), entry.paperEv);
        object.insert(QStringLiteral("label"), entry.label);
        object.insert(QStringLiteral("savedAt"), entry.savedAt.toString(Qt::ISODateWithMs));
        array.append(object);
    }

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return error(QStringLiteral("preset-write"), QCoreApplication::translate("PhotoPrint", "A papírbeállítás nem menthető."));
    const QByteArray data = QJsonDocument(array).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
        return error(QStringLiteral("preset-write"), QCoreApplication::translate("PhotoPrint", "A papírbeállítás nem menthető."));
    return Result<void>();
}

} // namespace pp
