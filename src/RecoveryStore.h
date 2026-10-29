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
    void clear();

    QString directory() const { return m_directory; }
    static QString defaultDirectory();

private:
    QString m_directory;
    int m_nextSlot = 0;
};

} // namespace pp
