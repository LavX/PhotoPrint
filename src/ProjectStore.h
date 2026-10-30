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
    /// Cleaned form of the `source` this load refused, whether it was a
    /// project-relative path escaping the folder or an absolute one. Purely
    /// informational: a relink dialog can start next to it. It is never
    /// resolved, opened or written back, so it must not be treated as trusted.
    /// Empty when nothing was refused or there was no project directory.
    QString refusedPath;
    QByteArray expectedSha256;
    bool missing = false;
    bool changed = false;
    /// The stored `source` was not a path inside the project directory: a `..`
    /// escape, a symlink leading out, or an absolute value. It was not used.
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
///    inside it). `..` escapes, symlinks leading out and absolute values are
///    rejected, never opened, and reported as a blocked issue; an absolute
///    value is kept only as a relink hint. The layout is still restored so the
///    user can relink the photo.
///  * `original` is the absolute path recorded for relinking, and the only
///    thing a refused `source` may resolve through. 1.0.0 projects rely on it
///    because their photos normally live outside the project folder, and they
///    store a relative `source` next to it, so they keep working on the
///    author's machine. It is never written into a portable copy: those carry
///    only contained relative paths.
///    It is not trusted blindly. The path is followed only when it is a
///    regular file and its SHA-256 still equals the frame's recorded identity.
///    A project from somebody else therefore cannot name an arbitrary local
///    file to be hashed and rendered, and a FIFO or device there cannot block
///    the read. A frame without a recorded identity cannot be verified, so its
///    absolute `original` is treated as missing; the user relinks it. The
///    accepted cost is that a 1.0.0 photo whose content changed in place at the
///    same absolute path also reports as missing instead of being shown
///    unverified. The healthy 1.0.0 layout (same photo, same path) is
///    unaffected.
///  * A portable copy only writes inside its own `.files` folder and only
///    copies regular, readable files. The bundle folder itself must be a real
///    directory: a `.files` symlink is refused before anything is copied, so
///    the lexical containment rule cannot be redirected. A failed copy removes
///    what it added and puts back any file it replaced, so the previous bundle
///    stays usable.
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
    /// Parses without a project directory, so no relative `source` can be
    /// followed; only an absolute `original` is resolved. Use load() when
    /// relative sources should be followed.
    static Result<DocumentSnapshot> deserialize(const QByteArray &json);
};

} // namespace pp
