#include "JobController.h"

#include "JobJournal.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace pp {

namespace {

QString stateFor(SubmitStatus status)
{
    switch (status) {
    case SubmitStatus::KnownJob:
        return QStringLiteral("queued");
    case SubmitStatus::Rejected:
        return QStringLiteral("rejected");
    case SubmitStatus::Uncertain:
        return QStringLiteral("uncertain");
    }
    return QStringLiteral("uncertain");
}

QString messageFor(SubmitStatus status)
{
    switch (status) {
    case SubmitStatus::KnownJob:
        return QCoreApplication::translate("PhotoPrint", "Elküldve a nyomtatónak.");
    case SubmitStatus::Rejected:
        return QCoreApplication::translate("PhotoPrint", "A nyomtató nem fogadta el a feladatot.");
    case SubmitStatus::Uncertain:
        return QCoreApplication::translate("PhotoPrint",
                                        "Nem tudjuk, elindult-e a nyomtatás. Nézd meg a nyomtatót, "
                                        "mielőtt újraindítod.");
    }
    return QCoreApplication::translate("PhotoPrint", "Nem tudjuk, elindult-e a nyomtatás.");
}

/// Releases the single-flight gate on every exit path, including the early
/// errors, so a failed job never leaves the controller permanently busy.
class BusyGuard
{
public:
    explicit BusyGuard(std::atomic<bool> &flag) : m_flag(flag) {}
    ~BusyGuard() { m_flag.store(false, std::memory_order_release); }
    BusyGuard(const BusyGuard &) = delete;
    BusyGuard &operator=(const BusyGuard &) = delete;

private:
    std::atomic<bool> &m_flag;
};

void removeFile(const QString &path)
{
    if (!path.isEmpty())
        QFile::remove(path);
}

} // namespace

BorderlessPlacement borderlessPlacement(const QSizeF &physicalMm, const QSizeF &logicalMm,
                                        const QPointF &originMm)
{
    BorderlessPlacement placement;
    placement.offsetMm = originMm;
    if (!(physicalMm.width() > 0.0) || !(physicalMm.height() > 0.0))
        return placement;

    // Just enough enlargement for the physical composition to reach every
    // edge of the logical page.
    const double cover =
        std::max(logicalMm.width() / physicalMm.width(), logicalMm.height() / physicalMm.height());
    if (!(cover > 1.0 + 1e-9))
        return placement;

    // Full coverage requires the content to start at or before 0 and to end at
    // or after the logical edge, so the feasible top-left range is [min, 0].
    const double minX = logicalMm.width() - physicalMm.width() * cover;
    const double minY = logicalMm.height() - physicalMm.height() * cover;
    // Keep the declared origin inside that range: it survives when the
    // overshoot leaves room for it, otherwise it moves only as far as the page
    // edge requires. It is never replaced by a constant. The outer min() also
    // absorbs the rounding case where the ratio leaves minX a hair above zero.
    const auto keepCovered = [](double value, double minimum) {
        return std::min(0.0, std::max(value, minimum));
    };
    placement.scale = cover;
    placement.offsetMm = QPointF(keepCovered(originMm.x(), minX), keepCovered(originMm.y(), minY));
    return placement;
}

JobController::JobController(ImageStore &store, JobConfig config, CupsBackend *backend)
    : m_store(store), m_config(std::move(config)), m_backend(backend), m_renderer(store)
{
}

QString JobController::lastManifestPath() const
{
    QMutexLocker locker(&m_artifactMutex);
    return m_lastManifest;
}

QString JobController::lastPdfPath() const
{
    QMutexLocker locker(&m_artifactMutex);
    return m_lastPdf;
}

Result<JobResult> JobController::start(const DocumentSnapshot &document,
                                       const PrintTicket &ticket, std::stop_token token)
{
    if (!ticket.valid || ticket.queueName.isEmpty())
        return error(QStringLiteral("job-ticket"), QCoreApplication::translate("PhotoPrint", "Válassz nyomtatót és papírt."));
    if (document.pages.isEmpty())
        return error(QStringLiteral("job-empty"), QCoreApplication::translate("PhotoPrint", "Nincs mit nyomtatni."));

    // The gate is a compare-exchange, not a read followed by a write: two
    // concurrent calls can both see an idle controller, but only one can take
    // the slot, so one user action can never submit twice.
    bool idle = false;
    if (!m_busy.compare_exchange_strong(idle, true))
        return error(QStringLiteral("job-busy"), QCoreApplication::translate("PhotoPrint", "A nyomtatás már folyamatban van."));
    BusyGuard gate(m_busy);

    const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastStart = m_lastStartMsecs.load(std::memory_order_acquire);
    if (lastStart > 0 && startedAt - lastStart < 1200) {
        return error(QStringLiteral("job-double"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatás már elindult. Várd meg az eredményét."));
    }

    const QUuid id = QUuid::createUuid();
    if (!QDir().mkpath(m_config.workDir))
        return error(QStringLiteral("job-dir"), QCoreApplication::translate("PhotoPrint", "A nyomtatás előkészítése nem sikerült."));
    const QString base = QDir(m_config.workDir).filePath(id.toString(QUuid::WithoutBraces));
    const QString pdfPath = base + QStringLiteral(".pdf");
    const QString manifestPath = base + QStringLiteral(".json");

    RenderOptions options;
    options.verifySources = true;
    const bool fine =
        ticket.qualityOption.compare(QLatin1String("Fine"), Qt::CaseInsensitive) == 0
        || ticket.qualityOption.compare(QLatin1String("High"), Qt::CaseInsensitive) == 0;
    // The queue's resolution decides. Highest follows it up to 1200 ppi
    // (a 1200 dpi queue really gets 1200 ppi), normal stays at 300 ppi even
    // when the queue only offers something larger.
    double ppi = fine ? 1200.0 : 300.0;
    if (ticket.printResolutionDpi > 0)
        ppi = std::min(ppi, static_cast<double>(ticket.printResolutionDpi));
    else if (fine)
        ppi = 600.0;  // no resolution advertised: the safe default
    options.printPpi = std::clamp(ppi, 150.0, 1200.0);
    options.mediaSizeMm = ticket.geometry.logicalSizeMm();
    options.contentOffsetMm = ticket.geometry.physicalOriginInLogicalMm;
    if (ticket.geometry.borderless) {
        // A bleed leaves no margin: the physical composition is enlarged just
        // enough to cover the logical page, keeping its declared origin where
        // that is still compatible with full coverage.
        const BorderlessPlacement placement =
            borderlessPlacement(ticket.geometry.physicalSizeMm(),
                                ticket.geometry.logicalSizeMm(),
                                ticket.geometry.physicalOriginInLogicalMm);
        options.contentScale = placement.scale;
        options.contentOffsetMm = placement.offsetMm;
    }
    const auto artifact = m_renderer.writePdf(document, pdfPath, token, options);
    if (!artifact) {
        removeFile(pdfPath);  // a failed write must not leave a half spool behind
        return artifact.error();
    }

    // Cancellation is honoured until the journal intent is durable. After that
    // the single submission must run to completion; stopping between the
    // intent and the CUPS call would leave a job nobody can ever settle.
    if (token.stop_requested()) {
        removeFile(pdfPath);
        return error(QStringLiteral("cancelled"),
                     QCoreApplication::translate("PhotoPrint", "A művelet megszakadt."));
    }

    const auto manifest = writeManifest(manifestPath, id, artifact.value().sha256, ticket,
                                        document);
    if (!manifest) {
        // No job exists yet, so the spool is not evidence: drop it.
        removeFile(pdfPath);
        return manifest.error();
    }
    {
        QMutexLocker locker(&m_artifactMutex);
        m_lastManifest = manifestPath;
    }

    if (token.stop_requested()) {
        removeFile(pdfPath);
        removeFile(manifestPath);
        QMutexLocker locker(&m_artifactMutex);
        m_lastManifest.clear();
        return error(QStringLiteral("cancelled"),
                     QCoreApplication::translate("PhotoPrint", "A művelet megszakadt."));
    }

    m_lastStartMsecs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_release);
    JobJournal journal(m_config.journalPath);
    const auto intent = journal.appendIntent(id, ticket.queueName,
                                             QString::fromLatin1(artifact.value().sha256.toHex()));
    if (!intent) {
        // The intent never became durable, so nothing was submitted and no
        // evidence has to be kept.
        removeFile(pdfPath);
        removeFile(manifestPath);
        QMutexLocker locker(&m_artifactMutex);
        m_lastManifest.clear();
        return intent.error();
    }

    const SubmitOutcome outcome = m_backend->submit(ticket.queueName, artifact.value(), ticket);

    // A submission whose outcome cannot be written down is not a finished job:
    // the journal already holds the intent, and a restart treats an intent as
    // uncertain. Record that explicitly and report it as uncertain instead of
    // pretending the outcome is known.
    SubmitStatus status = outcome.status;
    const auto recorded =
        journal.appendOutcome(id, stateFor(outcome.status), outcome.jobId, outcome.detail);
    if (!recorded) {
        journal.appendOutcome(id, QStringLiteral("uncertain"), outcome.jobId,
                              outcome.detail + QStringLiteral(" journal=")
                                  + recorded.error().code);
        status = SubmitStatus::Uncertain;
    }

    // Keep the spool only when the outcome is uncertain: then it is evidence,
    // not something a restart may silently resubmit. Normal closures drop it.
    // The manifest stays for every submitted job as the record of what was
    // sent; pre-submit failures remove it with the spool.
    const bool keepSpool = status == SubmitStatus::Uncertain;
    if (!keepSpool)
        removeFile(pdfPath);

    JobResult result;
    result.id = id;
    result.status = status;
    result.message = messageFor(status);
    result.pdfPath = keepSpool ? pdfPath : QString();
    {
        QMutexLocker locker(&m_artifactMutex);
        m_lastPdf = result.pdfPath;
    }
    return result;
}

Result<void> JobController::writeManifest(const QString &path, const QUuid &id,
                                          const QByteArray &pdfSha, const PrintTicket &ticket,
                                          const DocumentSnapshot &document)
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("id"), id.toString(QUuid::WithoutBraces));
    manifest.insert(QStringLiteral("createdAt"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    manifest.insert(QStringLiteral("program"), QStringLiteral("PhotoPrint"));
    manifest.insert(QStringLiteral("queue"), ticket.queueName);
    manifest.insert(QStringLiteral("driverIdentity"), ticket.driverIdentity);
    manifest.insert(QStringLiteral("ppdFingerprint"), ticket.ppdFingerprint);
    manifest.insert(QStringLiteral("pageSizeOption"), ticket.pageSizeOption);
    manifest.insert(QStringLiteral("mediaTypeOption"), ticket.mediaTypeOption);
    manifest.insert(QStringLiteral("qualityOption"), ticket.qualityOption);
    manifest.insert(QStringLiteral("printResolutionDpi"), ticket.printResolutionDpi);
    manifest.insert(QStringLiteral("colorOption"), ticket.colorOption);
    manifest.insert(QStringLiteral("duplexOption"), ticket.duplexOption);
    manifest.insert(QStringLiteral("copies"), ticket.copies);
    manifest.insert(QStringLiteral("logicalWidthMm"), ticket.geometry.logicalSizeMm().width());
    manifest.insert(QStringLiteral("logicalHeightMm"), ticket.geometry.logicalSizeMm().height());
    manifest.insert(QStringLiteral("pdfSha256"), QString::fromLatin1(pdfSha.toHex()));
    manifest.insert(QStringLiteral("recipeDigest"),
                    QString::fromLatin1(document.recipe.digest().toHex()));

    QJsonArray pages;
    for (const Page &page : document.pages) {
        QJsonArray frames;
        for (const Frame &frame : page.frames) {
            frames.append(QJsonObject{
                {QStringLiteral("sha256"), QString::fromLatin1(frame.sourceSha256.toHex())},
                {QStringLiteral("sourcePixels"), QJsonValue::Null}});
        }
        pages.append(QJsonObject{{QStringLiteral("paper"), page.paper.name},
                                 {QStringLiteral("landscape"), page.landscape},
                                 {QStringLiteral("frames"), frames}});
    }
    manifest.insert(QStringLiteral("pages"), pages);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return error(QStringLiteral("manifest-write"), QCoreApplication::translate("PhotoPrint", "A nyomtatási jegyzék nem írható."));
    const QByteArray data = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
        return error(QStringLiteral("manifest-write"), QCoreApplication::translate("PhotoPrint", "A nyomtatási jegyzék nem írható."));
    return Result<void>();
}

} // namespace pp
