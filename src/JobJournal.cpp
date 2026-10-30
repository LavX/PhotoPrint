#include "JobJournal.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace pp {

namespace {

/// Above this size the journal is rewritten with only the last record of each
/// job. The file is a state log, not an audit trail, so older transitions of an
/// already finished job carry no information for the recovery checks.
constexpr qint64 kCompactBytes = 1024 * 1024;

Result<void> syncToDisk(QFile &file)
{
    if (!file.flush())
        return error(QStringLiteral("journal-write"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
#if defined(Q_OS_UNIX)
    // flush() only drains Qt's buffer; the intent must survive a power cut
    // before the CUPS call, so the data has to reach the device.
    if (::fdatasync(file.handle()) != 0) {
        return error(QStringLiteral("journal-write"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
    }
#endif
    return Result<void>();
}

QByteArray lineFor(const JobRecord &record)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), record.id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("state"), record.state);
    object.insert(QStringLiteral("queue"), record.queue);
    object.insert(QStringLiteral("pdfSha256"), record.pdfSha256);
    object.insert(QStringLiteral("jobId"), record.jobId);
    object.insert(QStringLiteral("at"), record.at.toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("detail"), record.detail);
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

} // namespace

QString JobJournal::defaultPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("jobs.jsonl"));
}

JobJournal::JobJournal(QString path) : m_path(path.isEmpty() ? defaultPath() : std::move(path))
{
}

Result<void> JobJournal::appendLine(const JobRecord &record)
{
    // The directory is created here rather than in the constructor so a
    // failure can be reported to the caller instead of being dropped.
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        return error(QStringLiteral("journal-write"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
    }
    const auto compacted = compactIfNeeded();
    if (!compacted)
        return compacted.error();

    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return error(QStringLiteral("journal-write"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
    }
    const QByteArray line = lineFor(record);
    if (file.write(line) != line.size())
        return error(QStringLiteral("journal-write"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
    return syncToDisk(file);
}

Result<void> JobJournal::compactIfNeeded()
{
    const QFileInfo info(m_path);
    if (!info.exists() || info.size() <= kCompactBytes)
        return Result<void>();

    const auto records = readAll();
    if (!records)
        return records.error();

    // Keep the last state of every job, in first-appearance order.
    QList<JobRecord> kept;
    QHash<QUuid, int> indexById;
    for (const JobRecord &record : records.value()) {
        const auto it = indexById.constFind(record.id);
        if (it == indexById.constEnd()) {
            indexById.insert(record.id, kept.size());
            kept.append(record);
        } else {
            kept[it.value()] = record;
        }
    }

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return error(QStringLiteral("journal-write"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
    for (const JobRecord &record : kept) {
        const QByteArray line = lineFor(record);
        if (file.write(line) != line.size())
            return error(QStringLiteral("journal-write"),
                         QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
    }
    if (!file.commit())
        return error(QStringLiteral("journal-write"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
    return Result<void>();
}

Result<void> JobJournal::appendIntent(const QUuid &id, const QString &queue,
                                      const QString &pdfSha256)
{
    JobRecord record;
    record.id = id;
    record.state = QStringLiteral("intent");
    record.queue = queue;
    record.pdfSha256 = pdfSha256;
    record.at = QDateTime::currentDateTimeUtc();
    return appendLine(record);
}

Result<void> JobJournal::appendOutcome(const QUuid &id, const QString &state, qint64 jobId,
                                       const QString &detail)
{
    JobRecord record;
    record.id = id;
    record.state = state;
    record.jobId = jobId;
    record.at = QDateTime::currentDateTimeUtc();
    record.detail = detail;
    return appendLine(record);
}

Result<QList<JobRecord>> JobJournal::readAll() const
{
    QList<JobRecord> records;
    QFile file(m_path);
    if (!file.exists())
        return records; // first run
    if (!file.open(QIODevice::ReadOnly)) {
        return error(QStringLiteral("journal-read"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem olvasható."));
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError parseError;
        const QJsonDocument parsed = QJsonDocument::fromJson(line, &parseError);
        // A crash can leave a half-written last line. Skip anything that is not
        // a complete record instead of turning it into a record with no id.
        if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
            continue;
        const QJsonObject object = parsed.object();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        JobRecord record;
        record.id = QUuid(id);
        record.state = object.value(QStringLiteral("state")).toString();
        record.queue = object.value(QStringLiteral("queue")).toString();
        record.pdfSha256 = object.value(QStringLiteral("pdfSha256")).toString();
        record.jobId = object.value(QStringLiteral("jobId")).toInteger(-1);
        record.at = QDateTime::fromString(object.value(QStringLiteral("at")).toString(),
                                          Qt::ISODateWithMs);
        record.detail = object.value(QStringLiteral("detail")).toString();
        records.append(record);
    }
    return records;
}

QList<JobRecord> JobJournal::entries() const
{
    const auto records = readAll();
    return records ? records.value() : QList<JobRecord>{};
}

int JobJournal::uncertainCount() const
{
    QHash<QUuid, QString> lastState;
    for (const JobRecord &record : entries())
        lastState.insert(record.id, record.state);
    int count = 0;
    for (const QString &state : lastState) {
        if (state == QLatin1String("uncertain") || state == QLatin1String("intent"))
            ++count;
    }
    return count;
}

} // namespace pp
