#include "JobJournal.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QStandardPaths>

namespace pp {

QString JobJournal::defaultPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("jobs.jsonl"));
}

JobJournal::JobJournal(QString path) : m_path(path.isEmpty() ? defaultPath() : std::move(path))
{
    QDir().mkpath(QFileInfo(m_path).absolutePath());
}

Result<void> JobJournal::appendLine(const JobRecord &record)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), record.id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("state"), record.state);
    object.insert(QStringLiteral("queue"), record.queue);
    object.insert(QStringLiteral("pdfSha256"), record.pdfSha256);
    object.insert(QStringLiteral("jobId"), record.jobId);
    object.insert(QStringLiteral("at"), record.at.toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("detail"), record.detail);

    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return error(QStringLiteral("journal-write"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.write("\n");
    if (!file.flush())
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

QList<JobRecord> JobJournal::entries() const
{
    QList<JobRecord> records;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return records;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QJsonObject object = QJsonDocument::fromJson(line).object();
        JobRecord record;
        record.id = QUuid(object.value(QStringLiteral("id")).toString());
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
