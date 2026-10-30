#include "CupsBackend.h"

#include <cups/cups.h>

#include <QByteArray>

namespace pp {

RealCupsBackend::RealCupsBackend() : m_owned(new RealCupsCall), m_call(m_owned.get()) {}

// m_call is deliberately not owned: an injected CupsCall (tests) must outlive
// the backend, so the caller keeps it. m_owned only keeps the default call
// alive for the constructor above.
RealCupsBackend::RealCupsBackend(CupsCall *call) : m_call(call)
{
    if (!m_call) {
        m_owned = std::make_unique<RealCupsCall>();
        m_call = m_owned.get();
    }
}

CupsCallResult RealCupsCall::printFile(const QString &queue, const QString &pdfPath,
                                       const QStringList &options)
{
    cups_option_t *cupsOptions = nullptr;
    int count = 0;
    for (const QString &option : options) {
        const int equals = option.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QByteArray key = option.left(equals).toUtf8();
        const QByteArray value = option.mid(equals + 1).toUtf8();
        count = cupsAddOption(key.constData(), value.constData(), count, &cupsOptions);
    }

    const QByteArray queueName = queue.toUtf8();
    const QByteArray path = pdfPath.toUtf8();
    const int jobId = cupsPrintFile(queueName.constData(), path.constData(), "PhotoPrint", count,
                                    cupsOptions);
    cupsFreeOptions(count, cupsOptions);

    CupsCallResult result;
    result.jobId = jobId;
    result.ippStatus = static_cast<int>(cupsLastError());
    result.detail = QStringLiteral("cupsLastError=%1 %2")
                        .arg(result.ippStatus)
                        .arg(QString::fromLocal8Bit(cupsLastErrorString()));
    return result;
}

SubmitOutcome RealCupsBackend::submit(const QString &queue, const ExportArtifact &artifact,
                                      const PrintTicket &ticket)
{
    QStringList options;
    auto add = [&options](const QString &key, const QString &value) {
        if (!value.isEmpty())
            options.append(key + QLatin1Char('=') + value);
    };

    add(QStringLiteral("media"), ticket.pageSizeOption);
    add(QStringLiteral("copies"), QString::number(ticket.copies));
    // The rendered PDF page is already the logical media size, so fit-to-page
    // neither adds nor removes scaling; sending the resolved choice keeps the
    // job attributes honest instead of silently forcing one value.
    add(QStringLiteral("fit-to-page"),
        ticket.scaleToFit ? QStringLiteral("true") : QStringLiteral("false"));
    add(QStringLiteral("job-name"), QStringLiteral("PhotoPrint"));
    add(QStringLiteral("document-format"), QStringLiteral("application/pdf"));
    if (ticket.lastPage >= ticket.firstPage)
        add(QStringLiteral("page-ranges"),
            QStringLiteral("%1-%2").arg(ticket.firstPage).arg(ticket.lastPage));

    if (ticket.driverIdentity == QLatin1String("brother-native")) {
        add(QStringLiteral("BRMediaType"), ticket.mediaTypeOption);
        add(QStringLiteral("BRResolution"), ticket.qualityOption);
        add(QStringLiteral("BRMonoColor"), ticket.colorOption);
    } else {
        add(QStringLiteral("media-type"), ticket.mediaTypeOption);
        add(QStringLiteral("cupsPrintQuality"), ticket.qualityOption);
        add(QStringLiteral("ColorModel"), ticket.colorOption);
        if (!ticket.resolutionOption.isEmpty() && !ticket.resolutionValue.isEmpty())
            add(ticket.resolutionOption, ticket.resolutionValue);
    }
    // The adapter already resolved the single-sided option for this queue.
    // An empty value means the queue exposes no Duplex group at all; then the
    // driver default applies and sending a made-up keyword would be wrong.
    add(QStringLiteral("Duplex"), ticket.duplexOption);

    const CupsCallResult call = m_call->printFile(queue, artifact.path, options);
    SubmitOutcome outcome;
    outcome.detail = call.detail;
    if (call.jobId > 0) {
        outcome.status = SubmitStatus::KnownJob;
        outcome.jobId = call.jobId;
        return outcome;
    }
    const int status = call.ippStatus;
    if (status == IPP_STATUS_ERROR_TIMEOUT || status == IPP_STATUS_ERROR_SERVICE_UNAVAILABLE
        || status == IPP_STATUS_ERROR_INTERNAL) {
        outcome.status = SubmitStatus::Uncertain;
    } else {
        outcome.status = SubmitStatus::Rejected;
    }
    return outcome;
}

} // namespace pp
