#include "JobController.h"

#include "JobJournal.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
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

} // namespace

JobController::JobController(ImageStore &store, JobConfig config, CupsBackend *backend)
    : m_store(store), m_config(std::move(config)), m_backend(backend), m_renderer(store)
{
}

Result<JobResult> JobController::start(const DocumentSnapshot &document,
                                       const PrintTicket &ticket, std::stop_token token)
{
    if (m_busy)
        return error(QStringLiteral("job-busy"), QCoreApplication::translate("PhotoPrint", "A nyomtatás már folyamatban van."));
    if (m_lastStart.isValid() && m_lastStart.msecsTo(QDateTime::currentDateTime()) < 1200) {
        return error(QStringLiteral("job-double"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatás már elindult. Várd meg az eredményét."));
    }
    if (!ticket.valid || ticket.queueName.isEmpty())
        return error(QStringLiteral("job-ticket"), QCoreApplication::translate("PhotoPrint", "Válassz nyomtatót és papírt."));
    if (document.pages.isEmpty())
        return error(QStringLiteral("job-empty"), QCoreApplication::translate("PhotoPrint", "Nincs mit nyomtatni."));

    m_busy = true;
    auto release = [this] { m_busy = false; };

    const QUuid id = QUuid::createUuid();
    if (!QDir().mkpath(m_config.workDir)) {
        release();
        return error(QStringLiteral("job-dir"), QCoreApplication::translate("PhotoPrint", "A nyomtatás előkészítése nem sikerült."));
    }
    const QString pdfPath = QDir(m_config.workDir).filePath(id.toString(QUuid::WithoutBraces)
                                                           + QStringLiteral(".pdf"));
    RenderOptions options;
    options.verifySources = true;
    options.printPpi = (ticket.qualityOption.compare(QLatin1String("Fine"), Qt::CaseInsensitive) == 0
                        || ticket.qualityOption.compare(QLatin1String("High"), Qt::CaseInsensitive) == 0)
        ? 600.0
        : 300.0;
    options.mediaSizeMm = ticket.geometry.logicalSizeMm();
    options.contentOffsetMm = ticket.geometry.physicalOriginInLogicalMm;
    const QSizeF physical = ticket.geometry.physicalSizeMm();
    const QSizeF logical = ticket.geometry.logicalSizeMm();
    if (ticket.geometry.borderless && physical.width() > 0.0 && physical.height() > 0.0) {
        const double cover =
            std::max(logical.width() / physical.width(), logical.height() / physical.height());
        if (cover > 1.0 + 1e-9) {
            options.contentScale = cover;
            options.contentOffsetMm =
                QPointF((logical.width() - physical.width() * cover) / 2.0,
                        (logical.height() - physical.height() * cover) / 2.0);
        }
    }
    const auto artifact = m_renderer.writePdf(document, pdfPath, token, options);
    if (!artifact) {
        release();
        return artifact.error();
    }
    m_lastPdf = pdfPath;

    const auto manifest = writeManifest(id, artifact.value().sha256, ticket, document);
    if (!manifest) {
        release();
        return manifest.error();
    }

    m_lastStart = QDateTime::currentDateTime();
    JobJournal journal(m_config.journalPath);
    const auto intent = journal.appendIntent(id, ticket.queueName,
                                             QString::fromLatin1(artifact.value().sha256.toHex()));
    if (!intent) {
        release();
        return intent.error();
    }

    const SubmitOutcome outcome = m_backend->submit(ticket.queueName, artifact.value(), ticket);
    journal.appendOutcome(id, stateFor(outcome.status), outcome.jobId, outcome.detail);

    // Keep the spool only when the outcome is uncertain: then it is evidence,
    // not something a restart may silently resubmit. Normal closures drop it.
    if (outcome.status != SubmitStatus::Uncertain)
        QFile::remove(pdfPath);

    JobResult result;
    result.id = id;
    result.status = outcome.status;
    result.message = messageFor(outcome.status);
    result.pdfPath = pdfPath;
    release();
    return result;
}

Result<void> JobController::writeManifest(const QUuid &id, const QByteArray &pdfSha,
                                          const PrintTicket &ticket,
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

    const QString path = QDir(m_config.workDir).filePath(id.toString(QUuid::WithoutBraces)
                                                         + QStringLiteral(".json"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return error(QStringLiteral("manifest-write"), QCoreApplication::translate("PhotoPrint", "A nyomtatási jegyzék nem írható."));
    const QByteArray data = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
        return error(QStringLiteral("manifest-write"), QCoreApplication::translate("PhotoPrint", "A nyomtatási jegyzék nem írható."));
    m_lastManifest = path;
    return Result<void>();
}

} // namespace pp
