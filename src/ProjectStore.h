#pragma once

#include "Document.h"
#include "Result.h"

#include <QList>
#include <QString>

namespace pp {

enum class SaveMode { Normal, Portable };

struct SourceIssue {
    QString frameId;
    QString path;
    QByteArray expectedSha256;
    bool missing = false;
    bool changed = false;
};

struct LoadedProject {
    DocumentSnapshot document;
    QList<SourceIssue> issues;
    QString path;
};

/// Versioned JSON project. Saves atomically, keeps the original files
/// untouched, and never guesses a source's identity from a cached preview.
class ProjectStore
{
public:
    static constexpr int kFormatVersion = 1;

    Result<void> save(const QString &path, const DocumentSnapshot &document, SaveMode mode);
    Result<LoadedProject> load(const QString &path);

    static QByteArray serialize(const DocumentSnapshot &document, const QString &projectDir,
                                const QString &portableDir = QString());
    static Result<DocumentSnapshot> deserialize(const QByteArray &json);
};

} // namespace pp
