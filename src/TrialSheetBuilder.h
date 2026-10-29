#pragma once

#include "Document.h"
#include "PrintTicket.h"
#include "Result.h"

#include <QList>
#include <QString>

namespace pp {

struct TrialVariant {
    int number = 0;
    QString frameId;
    double paperEv = 0.0;
};

struct TrialSheet {
    DocumentSnapshot document;
    QList<TrialVariant> variants;
    PrintContext context;
};

/// Builds one numbered sheet. Variant 1 keeps the fixed image recipe and adds
/// zero paper correction; the other variants change only the paper component.
class TrialSheetBuilder
{
public:
    static Result<TrialSheet> build(const DocumentSnapshot &document, const QString &frameId,
                                    const PrintContext &context);
};

} // namespace pp
