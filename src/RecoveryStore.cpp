#include "RecoveryStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace pp {

namespace {

const QString kSlotA = QStringLiteral("recovery-a.json");
const QString kSlotB = QStringLiteral("recovery-b.json");
const QString kManifest = QStringLiteral("latest.txt");

bool isSlotName(const QString &name)
{
    return name == kSlotA || name == kSlotB;
}

} // namespace

QString RecoveryStore::defaultDirectory()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("recovery"));
}

RecoveryStore::RecoveryStore(QString directory)
    : m_directory(directory.isEmpty() ? defaultDirectory() : std::move(directory))
{
    m_nextSlot = nextSlotFromDisk();
}

QString RecoveryStore::slotName(int index) const
{
    return index % 2 == 0 ? kSlotA : kSlotB;
}

QString RecoveryStore::manifestName() const
{
    QFile manifest(QDir(m_directory).filePath(kManifest));
    if (!manifest.open(QIODevice::ReadOnly))
        return QString();
    const QString name = QString::fromLatin1(manifest.readAll()).trimmed();
    // The manifest is a file name, never a path: anything else is ignored.
    return isSlotName(name) ? name : QString();
}

int RecoveryStore::nextSlotFromDisk() const
{
    const QString latest = manifestName();
    if (latest == kSlotA)
        return 1;
    if (latest == kSlotB)
        return 0;

    // No usable manifest, so the slots themselves decide. A single slot is the
    // only complete snapshot there is: the next checkpoint goes to the empty
    // partner, never over that one. Only when both exist is the older slot the
    // sacrificial one, so the other complete snapshot stays readable.
    const QFileInfo a(QDir(m_directory).filePath(kSlotA));
    const QFileInfo b(QDir(m_directory).filePath(kSlotB));
    if (!a.exists())
        return 0;
    if (!b.exists())
        return 1;
    return a.lastModified() <= b.lastModified() ? 0 : 1;
}

Result<void> RecoveryStore::checkpoint(const DocumentSnapshot &document)
{
    if (!QDir().mkpath(m_directory)) {
        return error(QStringLiteral("recovery-dir"),
                     QCoreApplication::translate("PhotoPrint", "A munkamenet mentése nem sikerült."));
    }

    // nextSlotFromDisk(), checkpoint() and clear() keep m_nextSlot at 0 or 1.
    const int index = m_nextSlot;
    const QString slot = slotName(index);
    ProjectStore store;
    const auto saved = store.save(QDir(m_directory).filePath(slot), document, SaveMode::Normal);
    if (!saved)
        return saved.error();

    // The slot is complete before the manifest names it, so a crash in between
    // leaves the manifest pointing at the previous complete snapshot. The next
    // checkpoint picks that older slot's partner from the manifest, which is
    // exactly the slot that may have been left unannounced.
    QSaveFile manifest(QDir(m_directory).filePath(kManifest));
    if (!manifest.open(QIODevice::WriteOnly)) {
        return error(QStringLiteral("recovery-manifest"),
                     QCoreApplication::translate("PhotoPrint", "A munkamenet mentése nem sikerült."));
    }
    const QByteArray name = slot.toLatin1();
    if (manifest.write(name) != name.size()) {
        manifest.cancelWriting();
        return error(QStringLiteral("recovery-manifest"),
                     QCoreApplication::translate("PhotoPrint", "A munkamenet mentése nem sikerült."));
    }
    if (!manifest.commit()) {
        return error(QStringLiteral("recovery-manifest"),
                     QCoreApplication::translate("PhotoPrint", "A munkamenet mentése nem sikerült."));
    }

    m_nextSlot = (index + 1) % 2;
    return Result<void>();
}

Result<LoadedProject> RecoveryStore::loadLatest()
{
    struct Candidate {
        QFileInfo info;
        bool preferred = false;
    };

    const QString preferredName = manifestName();
    QList<Candidate> candidates;
    for (const QString &name : {kSlotA, kSlotB}) {
        const QFileInfo info(QDir(m_directory).filePath(name));
        if (info.exists() && info.isFile())
            candidates.append({info, name == preferredName});
    }
    if (candidates.isEmpty()) {
        return error(QStringLiteral("recovery-empty"),
                     QCoreApplication::translate("PhotoPrint", "Nincs folytatható munka."));
    }

    // Newest first: a checkpoint that reached the slot but not the manifest is
    // still the freshest complete snapshot. Equal timestamps (two checkpoints
    // in the same millisecond) fall back to the manifest, then to slot order.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &left, const Candidate &right) {
                         if (left.info.lastModified() != right.info.lastModified())
                             return left.info.lastModified() > right.info.lastModified();
                         if (left.preferred != right.preferred)
                             return left.preferred;
                         return left.info.fileName() < right.info.fileName();
                     });

    ProjectStore store;
    for (const Candidate &candidate : candidates) {
        const auto loaded = store.load(candidate.info.absoluteFilePath());
        if (loaded)
            return loaded;
    }
    return error(QStringLiteral("recovery-corrupt"),
                 QCoreApplication::translate("PhotoPrint", "A korábbi munka nem nyitható meg."));
}

Result<void> RecoveryStore::clear()
{
    for (const QString &name : {kSlotA, kSlotB, kManifest}) {
        const QString path = QDir(m_directory).filePath(name);
        if (!QFileInfo::exists(path))
            continue;
        if (!QFile::remove(path)) {
            return error(QStringLiteral("recovery-clear"),
                         QCoreApplication::translate("PhotoPrint",
                                                     "A munkamenet mentését nem sikerült törölni."));
        }
    }
    m_nextSlot = 0;
    return Result<void>();
}

} // namespace pp
