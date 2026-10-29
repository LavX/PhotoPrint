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

    SubmitOutcome submit(const QString &queue, const ExportArtifact &artifact,
                         const PrintTicket &) override
    {
        ++submits;
        queues.append(queue);
        lastArtifact = artifact;
        return outcome;
    }
};

} // namespace pp
