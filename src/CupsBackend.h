#pragma once

#include "PageRenderer.h"
#include "PrintTicket.h"

#include <QString>

#include <memory>

namespace pp {

enum class SubmitStatus { KnownJob, Rejected, Uncertain };

struct SubmitOutcome {
    SubmitStatus status = SubmitStatus::Uncertain;
    qint64 jobId = -1;
    QString detail; ///< developer detail only; never shown as the user message
};

/// The only place that talks to the existing CUPS queue. One call per print
/// action; no retry lives below this boundary.
class CupsBackend
{
public:
    virtual ~CupsBackend() = default;
    virtual SubmitOutcome submit(const QString &queue, const ExportArtifact &artifact,
                                 const PrintTicket &ticket) = 0;
};

/// Injected boundary around the single libcups call, so the real adapter's
/// classification can be tested without a printer.
struct CupsCallResult {
    int jobId = 0;
    int ippStatus = 0;
    QString detail;
};

class CupsCall
{
public:
    virtual ~CupsCall() = default;
    virtual CupsCallResult printFile(const QString &queue, const QString &pdfPath,
                                     const QStringList &options) = 0;
};

class RealCupsCall : public CupsCall
{
public:
    CupsCallResult printFile(const QString &queue, const QString &pdfPath,
                             const QStringList &options) override;
};

class RealCupsBackend : public CupsBackend
{
public:
    RealCupsBackend();
    explicit RealCupsBackend(CupsCall *call);
    SubmitOutcome submit(const QString &queue, const ExportArtifact &artifact,
                         const PrintTicket &ticket) override;

private:
    std::unique_ptr<CupsCall> m_owned;
    CupsCall *m_call;
};

} // namespace pp
