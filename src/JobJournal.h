#pragma once

#include "Result.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QUuid>

namespace pp {

/// Append-only record of print intent and outcome. The intent is durable before
/// the first side-effecting CUPS call, so a crash cannot silently resubmit.
struct JobRecord {
    QUuid id;
    QString state; ///< intent, queued, rejected, uncertain
    QString queue;
    QString pdfSha256;
    qint64 jobId = -1;
    QDateTime at;
    QString detail;
};

class JobJournal
{
public:
    explicit JobJournal(QString path = QString());

    Result<void> appendIntent(const QUuid &id, const QString &queue, const QString &pdfSha256);
    Result<void> appendOutcome(const QUuid &id, const QString &state, qint64 jobId = -1,
                               const QString &detail = QString());

    /// Every readable record, oldest first. A missing file is a first run and
    /// returns an empty list; an existing but unreadable file is an error.
    /// Lines damaged by a crash are skipped instead of becoming junk records.
    Result<QList<JobRecord>> readAll() const;

    /// Convenience wrapper: an unreadable journal looks empty here. Use
    /// readAll() where "cannot read" must not mean "nothing happened".
    QList<JobRecord> entries() const;
    int uncertainCount() const;
    QString path() const { return m_path; }
    static QString defaultPath();

private:
    Result<void> appendLine(const JobRecord &record);
    Result<void> compactIfNeeded();
    QString m_path;
};

} // namespace pp
