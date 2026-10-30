#pragma once

#include "CupsBackend.h"
#include "Document.h"
#include "JobJournal.h"
#include "PageRenderer.h"
#include "PrintTicket.h"
#include "Result.h"

#include <QMutex>
#include <QString>
#include <QUuid>

#include <atomic>
#include <memory>
#include <stop_token>

namespace pp {

struct JobConfig {
    QString workDir;
    QString journalPath;
};

struct JobResult {
    QUuid id;
    SubmitStatus status = SubmitStatus::Uncertain;
    QString message;
    QString pdfPath;
};

/// Where the rendered physical sheet goes on the logical page for a borderless
/// print: the smallest uniform enlargement that covers the page, with the
/// declared physical origin kept whenever it already covers it. A default (0,0)
/// origin is not a corner anchor: the overshoot is split evenly, as it always
/// was, so the composition is cropped equally on both sides.
struct BorderlessPlacement {
    double scale = 1.0;
    QPointF offsetMm;
};

BorderlessPlacement borderlessPlacement(const QSizeF &physicalMm, const QSizeF &logicalMm,
                                        const QPointF &originMm);

/// One logical print action: immutable snapshot, one PDF, one manifest, one
/// journal intent, exactly one submit call.
class JobController
{
public:
    /// The journal is injectable so a test can fail the durability or the
    /// rollback step, which a healthy filesystem never does. The controller
    /// takes ownership; nullptr means the ordinary on-disk journal at
    /// config.journalPath.
    JobController(ImageStore &store, JobConfig config, CupsBackend *backend,
                  std::unique_ptr<JobJournal> journal = {});

    /// On failure nothing was submitted, with one exception: a
    /// 'journal-uncertain' result means a journal record may have survived
    /// although it could not be withdrawn. The spool and manifest are then
    /// kept, and lastPdfPath()/lastManifestPath() point at them, because an
    /// intent without its spool is a phantom job nobody can settle.
    Result<JobResult> start(const DocumentSnapshot &document, const PrintTicket &ticket,
                            std::stop_token token = {});

    bool busy() const { return m_busy.load(std::memory_order_acquire); }
    QString lastManifestPath() const;
    QString lastPdfPath() const;

private:
    Result<void> writeManifest(const QString &path, const QUuid &id, const QByteArray &pdfSha,
                               const PrintTicket &ticket, const DocumentSnapshot &document) const;
    /// Drops the manifest and spool paths together: they describe one job, so
    /// half of a pair would point at a file that no longer belongs to a job.
    void clearArtifacts();

    ImageStore &m_store;
    JobConfig m_config;
    CupsBackend *m_backend;
    PageRenderer m_renderer;
    /// Lives as long as the controller rather than as long as one start(): the
    /// journal remembers when it last compacted, so a per-print object would
    /// rewrite a journal that its own live records keep above the threshold on
    /// every single print.
    std::unique_ptr<JobJournal> m_journal;
    /// Single-flight gate. Acquired with a compare-exchange so two concurrent
    /// start() calls cannot both pass the check and submit twice.
    std::atomic<bool> m_busy{false};
    /// Start of the last submission, in epoch milliseconds (0 = never).
    std::atomic<qint64> m_lastStartMsecs{0};
    mutable QMutex m_artifactMutex;
    QString m_lastManifest;
    QString m_lastPdf;
};

} // namespace pp
