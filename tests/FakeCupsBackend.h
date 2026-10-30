#pragma once

#include "CupsBackend.h"

#include <QList>

namespace pp {

class FakeCupsBackend : public CupsBackend
{
public:
    SubmitOutcome outcome;
    int submits = 0;
    QStringList queues;
    ExportArtifact lastArtifact;
    /// The resolved ticket actually handed to the backend, so tests can check
    /// that the adapter's decisions survive the trip.
    PrintTicket lastTicket;

    SubmitOutcome submit(const QString &queue, const ExportArtifact &artifact,
                         const PrintTicket &ticket) override
    {
        ++submits;
        queues.append(queue);
        lastArtifact = artifact;
        lastTicket = ticket;
        return outcome;
    }
};

} // namespace pp
