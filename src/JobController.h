#pragma once

#include "CupsBackend.h"
#include "Document.h"
#include "PageRenderer.h"
#include "PrintTicket.h"
#include "Result.h"

#include <QDateTime>
#include <QString>
#include <QUuid>

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

/// One logical print action: immutable snapshot, one PDF, one manifest, one
/// journal intent, exactly one submit call.
class JobController
{
public:
    JobController(ImageStore &store, JobConfig config, CupsBackend *backend);

    Result<JobResult> start(const DocumentSnapshot &document, const PrintTicket &ticket,
                            std::stop_token token = {});

    bool busy() const { return m_busy; }
    QString lastManifestPath() const { return m_lastManifest; }
    QString lastPdfPath() const { return m_lastPdf; }

private:
    Result<void> writeManifest(const QUuid &id, const QByteArray &pdfSha,
                               const PrintTicket &ticket, const DocumentSnapshot &document);

    ImageStore &m_store;
    JobConfig m_config;
    CupsBackend *m_backend;
    PageRenderer m_renderer;
    bool m_busy = false;
    QDateTime m_lastStart;
    QString m_lastManifest;
    QString m_lastPdf;
};

} // namespace pp
