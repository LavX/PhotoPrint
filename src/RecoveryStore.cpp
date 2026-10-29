#include "RecoveryStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace pp {

QString RecoveryStore::defaultDirectory()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("recovery"));
}

RecoveryStore::RecoveryStore(QString directory)
    : m_directory(directory.isEmpty() ? defaultDirectory() : std::move(directory))
{
}

Result<void> RecoveryStore::checkpoint(const DocumentSnapshot &document)
{
    if (!QDir().mkpath(m_directory))
        return error(QStringLiteral("recovery-dir"),
                     QStringLiteral("A munkamenet mentése nem sikerült."));
    const QString slot = (m_nextSlot++ % 2 == 0) ? QStringLiteral("recovery-a.json")
                                                 : QStringLiteral("recovery-b.json");
    ProjectStore store;
    const auto saved = store.save(QDir(m_directory).filePath(slot), document, SaveMode::Normal);
    if (!saved)
        return saved.error();

    // A tiny manifest makes "the newest complete snapshot" deterministic even
    // when both slots share the same file-system timestamp.
    QSaveFile manifest(QDir(m_directory).filePath(QStringLiteral("latest.txt")));
    if (!manifest.open(QIODevice::WriteOnly))
        return error(QStringLiteral("recovery-manifest"),
                     QStringLiteral("A munkamenet mentése nem sikerült."));
    manifest.write(slot.toLatin1());
    if (!manifest.commit())
        return error(QStringLiteral("recovery-manifest"),
                     QStringLiteral("A munkamenet mentése nem sikerült."));
    return Result<void>();
}

Result<LoadedProject> RecoveryStore::loadLatest()
{
    QFile manifest(QDir(m_directory).filePath(QStringLiteral("latest.txt")));
    QString preferred;
    if (manifest.open(QIODevice::ReadOnly))
        preferred = QString::fromLatin1(manifest.readAll()).trimmed();
    const QString other = preferred == QLatin1String("recovery-a.json")
        ? QStringLiteral("recovery-b.json")
        : QStringLiteral("recovery-a.json");

    QList<QFileInfo> candidates;
    for (const QString &name : {preferred, other}) {
        if (name.isEmpty())
            continue;
        const QFileInfo info(QDir(m_directory).filePath(name));
        if (info.exists())
            candidates.append(info);
    }
    if (candidates.isEmpty())
        return error(QStringLiteral("recovery-empty"), QStringLiteral("Nincs folytatható munka."));

    ProjectStore store;
    for (const QFileInfo &candidate : candidates) {
        const auto loaded = store.load(candidate.absoluteFilePath());
        if (loaded)
            return loaded;
    }
    return error(QStringLiteral("recovery-corrupt"),
                 QStringLiteral("A korábbi munka nem nyitható meg."));
}

void RecoveryStore::clear()
{
    for (const QString &name : {QStringLiteral("recovery-a.json"), QStringLiteral("recovery-b.json"),
                                QStringLiteral("latest.txt")})
        QFile::remove(QDir(m_directory).filePath(name));
}

} // namespace pp
