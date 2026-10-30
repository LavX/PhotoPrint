#pragma once

#include "Result.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QUuid>

class QFile;

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
    virtual ~JobJournal() = default;

    /// Appends one record and does not return success until it is on the
    /// device. A failed write or sync is rolled back to the length it had
    /// before, so the caller may delete the spool knowing no record survived.
    ///
    /// The rollback can itself fail. The result then carries the distinct code
    /// 'journal-uncertain' instead of 'journal-write': whether a record
    /// survived is unknown, so the caller must keep the spool as evidence and
    /// report an uncertain job rather than assume nothing is there.
    Result<void> appendIntent(const QUuid &id, const QString &queue, const QString &pdfSha256);
    /// Same error contract as appendIntent.
    Result<void> appendOutcome(const QUuid &id, const QString &state, qint64 jobId = -1,
                               const QString &detail = QString());

    /// Every readable record, oldest first. A missing file is a first run and
    /// returns an empty list; an existing but unreadable file is an error.
    /// Lines damaged by a crash are skipped instead of becoming junk records.
    Result<QList<JobRecord>> readAll() const;

    /// Convenience wrapper: an unreadable journal looks empty here. Use
    /// readAll() where "cannot read" must not mean "nothing happened".
    QList<JobRecord> entries() const;

    /// Jobs whose last state is still open (intent or uncertain).
    ///
    /// Returns -1 when the journal cannot be read: "cannot tell" is not the
    /// same as "nothing pending", and a caller that only compares against zero
    /// would silently drop the warning. Every caller must handle the negative
    /// case separately from 0.
    int uncertainCount() const;

    QString path() const { return m_path; }
    static QString defaultPath();

protected:
    /// Pushes the written bytes all the way to the device. A seam, because a
    /// failed sync cannot be provoked with a real file: the rollback in
    /// appendLine would otherwise stay untested. Nothing overrides it in the
    /// application.
    virtual Result<void> syncFile(QFile &file);

    /// Rewinds the journal to the length it had before a failed append, so an
    /// incomplete or undurable record does not outlive the error. Returns
    /// false when the file cannot be truncated. A seam for the same reason as
    /// syncFile: a healthy filesystem never refuses this, yet the refusal
    /// changes what the caller is allowed to delete.
    virtual bool truncateTo(QFile &file, qint64 size);

private:
    Result<void> appendLine(const JobRecord &record);
    /// Caller holds the journal lock: it reads, rewrites and replaces the file.
    Result<void> compactIfNeeded();
    QString m_path;
    /// Size the journal has to grow past before the next compaction. It is
    /// remembered per instance, so the owner must keep the journal for the
    /// session (JobController does): a fresh object per print would bound
    /// nothing.
    qint64 m_compactAfter = 0;
};

} // namespace pp
