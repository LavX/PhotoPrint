#pragma once

#include "ProjectStore.h"
#include "Result.h"

#include <QString>

namespace pp {

/// Two alternating recovery slots. Each write is atomic, so a crash during a
/// checkpoint leaves the previous complete snapshot readable.
class RecoveryStore
{
public:
    explicit RecoveryStore(QString directory = QString());

    Result<void> checkpoint(const DocumentSnapshot &document);
    Result<LoadedProject> loadLatest();

    /// Removes both slots and the manifest. Reports a failure to remove an
    /// existing file instead of pretending the recovery data is gone.
    [[nodiscard]] Result<void> clear();

    QString directory() const { return m_directory; }
    static QString defaultDirectory();

private:
    QString slotName(int index) const;
    /// The manifest slot name, or an empty string when the file is missing or
    /// does not name one of the two slots.
    QString manifestName() const;
    /// The slot the next checkpoint should overwrite, derived from disk so a
    /// restart does not clobber the newest snapshot.
    int nextSlotFromDisk() const;

    QString m_directory;
    /// Always 0 or 1; every writer of this field keeps that invariant.
    int m_nextSlot = 0;
};

} // namespace pp
