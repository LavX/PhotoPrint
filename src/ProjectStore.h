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
    /// The stored path pointed outside the project directory and was not used.
    bool blocked = false;
};

struct LoadedProject {
    DocumentSnapshot document;
    QList<SourceIssue> issues;
    QString path;
};

/// Versioned JSON project. Saves atomically, keeps the original files
/// untouched, and never guesses a source's identity from a cached preview.
///
/// Path policy. The file is data, not code, and it may arrive from somebody
/// else:
///  * `source` is project-relative by contract. It is resolved only when it
///    stays inside the project directory (the portable `.files` folder is
///    inside it). `..` escapes and symlinks leading out are rejected, never
///    opened, and reported as a blocked issue. The layout is still restored so
///    the user can relink the photo.
///  * `original` is the absolute path recorded for relinking. 1.0.0 projects
///    rely on it because their photos normally live outside the project
///    folder, so it keeps working on the author's machine. It is never written
///    into a portable copy: those carry only contained relative paths.
///  * A portable copy only writes inside its own `.files` folder and only
///    copies regular, readable files; a failed copy rolls the whole bundle
///    back.
class ProjectStore
{
public:
    static constexpr int kFormatVersion = 1;

    Result<void> save(const QString &path, const DocumentSnapshot &document, SaveMode mode);
    Result<LoadedProject> load(const QString &path);

    /// `portableDir` empty serialises a normal project. A non-empty value is
    /// the `.files` folder of a portable copy; the stored paths are relative to
    /// it and no absolute `original` is written.
    static QByteArray serialize(const DocumentSnapshot &document, const QString &projectDir,
                                const QString &portableDir = QString());
    /// Parses without a project directory, so only absolute recorded paths can
    /// be resolved. Use load() when relative sources should be followed.
    static Result<DocumentSnapshot> deserialize(const QByteArray &json);
};

} // namespace pp
