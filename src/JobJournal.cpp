#include "JobJournal.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#elif defined(Q_OS_WIN)
// windows.h defines min/max as macros, which breaks the std:: algorithms used
// below. Ask for the clean headers before pulling it in.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <io.h>
#endif

#include <algorithm>

namespace pp {

namespace {

/// Above this size the journal is rewritten with only the last record of each
/// job. The file is a state log, not an audit trail, so older transitions of an
/// already finished job carry no information for the recovery checks.
constexpr qint64 kCompactBytes = 1024 * 1024;

/// A second instance may be appending at the same moment; wait rather than
/// fail, but not forever.
constexpr int kLockWaitMsecs = 2000;

Error writeError()
{
    return error(QStringLiteral("journal-write"),
                 QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem írható."));
}

/// The append failed and what it wrote could not be withdrawn. A distinct code,
/// because the caller's decision differs: 'journal-write' means no record
/// survived, this one means nobody can tell without reading the file.
Error uncertainError()
{
    return error(QStringLiteral("journal-uncertain"),
                 QCoreApplication::translate("PhotoPrint",
                                             "A napló írása nem sikerült, a félbehagyott "
                                             "bejegyzést pedig nem sikerült eltávolítani."));
}

Result<void> syncToDisk(QFile &file)
{
    if (!file.flush())
        return writeError();
#if defined(Q_OS_UNIX)
    // flush() only drains Qt's buffer; the intent must survive a power cut
    // before the CUPS call, so the data has to reach the device.
    if (::fdatasync(file.handle()) != 0)
        return writeError();
#elif defined(Q_OS_WIN)
    // QFile::handle() is a CRT file descriptor on Windows, not a Win32 HANDLE:
    // casting the descriptor straight to HANDLE hands FlushFileBuffers a small
    // integer that is not a handle, so every append failed here. _get_osfhandle
    // converts the descriptor, and -1 (errno EBADF) means there is no handle to
    // flush. A negative descriptor is refused before the conversion as well:
    // _get_osfhandle on one calls the CRT invalid-parameter handler, which
    // terminates the process instead of reporting a write error. Not exercised
    // by the test suite, which runs on Linux.
    const int descriptor = static_cast<int>(file.handle());
    if (descriptor < 0)
        return writeError();
    const intptr_t raw = ::_get_osfhandle(descriptor);
    if (raw == -1 || !::FlushFileBuffers(reinterpret_cast<HANDLE>(raw)))
        return writeError();
#endif
    return Result<void>();
}

/// Whether the journal still ends in a complete line. A read failure counts as
/// "no newline": the extra terminator only adds an empty line that readAll()
/// skips, while skipping it would fuse the next record onto the tail.
bool endsWithNewline(QFile &file, qint64 size)
{
    if (size <= 0)
        return true;
    if (!file.seek(size - 1))
        return false;
    return file.read(1) == QByteArrayLiteral("\n");
}

QString lockPathFor(const QString &path)
{
    return QFileInfo(path).absoluteFilePath() + QStringLiteral(".lock");
}

QByteArray lineFor(const JobRecord &record)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), record.id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("state"), record.state);
    object.insert(QStringLiteral("queue"), record.queue);
    object.insert(QStringLiteral("pdfSha256"), record.pdfSha256);
    object.insert(QStringLiteral("jobId"), record.jobId);
    object.insert(QStringLiteral("at"), record.at.toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("detail"), record.detail);
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

} // namespace

QString JobJournal::defaultPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("jobs.jsonl"));
}

JobJournal::JobJournal(QString path)
    : m_path(path.isEmpty() ? defaultPath() : std::move(path)), m_compactAfter(kCompactBytes)
{
}

Result<void> JobJournal::syncFile(QFile &file)
{
    return syncToDisk(file);
}

bool JobJournal::truncateTo(QFile &file, qint64 size)
{
    // Qt does not buffer writes, but flushing first keeps the truncation from
    // being reordered against what the failed append left behind.
    file.flush();
    if (file.resize(size))
        return true;
    qWarning("JobJournal: a napló visszavágása %lld bájtra nem sikerült: %s",
             static_cast<long long>(size), qPrintable(file.errorString()));
    return false;
}

Result<void> JobJournal::appendLine(const JobRecord &record)
{
    // The directory is created here rather than in the constructor so a
    // failure can be reported to the caller instead of being dropped.
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return writeError();

    // Append and compaction are a read-modify-write pair across processes: two
    // instances could otherwise both rewrite the file and one intent would be
    // lost without a trace. The lock covers both steps.
    QLockFile lock(lockPathFor(m_path));
    if (!lock.tryLock(kLockWaitMsecs)) {
        return error(QStringLiteral("journal-lock"),
                     QCoreApplication::translate("PhotoPrint", "A napló jelenleg használatban van."));
    }

    const auto compacted = compactIfNeeded();
    if (!compacted)
        return compacted.error();

    const qint64 before = QFileInfo(m_path).size();
    QFile file(m_path);
    // Read is needed to inspect the last byte; append keeps every write at the
    // end of the file, past the offset the check leaves behind.
    if (!file.open(QIODevice::ReadWrite | QIODevice::Append))
        return writeError();

    // A crash can leave the last record without its terminating newline.
    // Writing at that offset glues the new record onto the fragment, and
    // readAll() then drops the merged line: the newer record, the one whose
    // durability this call is about, disappears with it. Terminate the dangling
    // line first so every record owns a line of its own.
    QByteArray payload;
    if (!endsWithNewline(file, before))
        payload.append('\n');
    payload.append(lineFor(record));

    if (file.write(payload) != payload.size()) {
        // The trailing newline was written last, so a short write leaves a
        // fragment readAll() skips. Only a rollback that itself fails can leave
        // the file in a state the caller has to be warned about.
        if (!truncateTo(file, before))
            return uncertainError();
        return writeError();
    }
    const auto synced = syncFile(file);
    if (!synced) {
        // The bytes reached the file but not the device. Dropping them keeps
        // the file in step with the caller's decision to keep no evidence.
        if (!truncateTo(file, before))
            return uncertainError();
        return synced.error();
    }
    return Result<void>();
}

Result<void> JobJournal::compactIfNeeded()
{
    const QFileInfo info(m_path);
    // m_compactAfter starts at the threshold and moves to "compacted size plus
    // one more threshold" after each rewrite, so a compacted journal that is
    // still large cannot force a rewrite on every single append.
    if (!info.exists() || info.size() <= m_compactAfter)
        return Result<void>();

    const auto records = readAll();
    if (!records)
        return records.error();

    // Keep the last state of every job, in first-appearance order.
    QList<JobRecord> kept;
    QHash<QUuid, int> indexById;
    for (const JobRecord &record : records.value()) {
        const auto it = indexById.constFind(record.id);
        if (it == indexById.constEnd()) {
            indexById.insert(record.id, kept.size());
            kept.append(record);
        } else {
            kept[it.value()] = record;
        }
    }

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return writeError();
    for (const JobRecord &record : kept) {
        const QByteArray line = lineFor(record);
        if (file.write(line) != line.size()) {
            file.cancelWriting();
            return writeError();
        }
    }
    if (!file.commit())
        return writeError();
    m_compactAfter = std::max(kCompactBytes, QFileInfo(m_path).size() + kCompactBytes);
    return Result<void>();
}

Result<void> JobJournal::appendIntent(const QUuid &id, const QString &queue,
                                      const QString &pdfSha256)
{
    JobRecord record;
    record.id = id;
    record.state = QStringLiteral("intent");
    record.queue = queue;
    record.pdfSha256 = pdfSha256;
    record.at = QDateTime::currentDateTimeUtc();
    return appendLine(record);
}

Result<void> JobJournal::appendOutcome(const QUuid &id, const QString &state, qint64 jobId,
                                       const QString &detail)
{
    JobRecord record;
    record.id = id;
    record.state = state;
    record.jobId = jobId;
    record.at = QDateTime::currentDateTimeUtc();
    record.detail = detail;
    return appendLine(record);
}

Result<QList<JobRecord>> JobJournal::readAll() const
{
    QList<JobRecord> records;
    QFile file(m_path);
    if (!file.exists())
        return records; // first run
    if (!file.open(QIODevice::ReadOnly)) {
        return error(QStringLiteral("journal-read"),
                     QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem olvasható."));
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError parseError;
        const QJsonDocument parsed = QJsonDocument::fromJson(line, &parseError);
        // A crash can leave a half-written last line. Skip anything that is not
        // a complete record instead of turning it into a record with no id.
        if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
            continue;
        const QJsonObject object = parsed.object();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        // A damaged id would otherwise collapse every such line onto the null
        // uuid and merge unrelated jobs into one state.
        const QUuid parsedId(id);
        if (parsedId.isNull())
            continue;
        JobRecord record;
        record.id = parsedId;
        record.state = object.value(QStringLiteral("state")).toString();
        record.queue = object.value(QStringLiteral("queue")).toString();
        record.pdfSha256 = object.value(QStringLiteral("pdfSha256")).toString();
        record.jobId = object.value(QStringLiteral("jobId")).toInteger(-1);
        record.at = QDateTime::fromString(object.value(QStringLiteral("at")).toString(),
                                          Qt::ISODateWithMs);
        record.detail = object.value(QStringLiteral("detail")).toString();
        records.append(record);
    }
    return records;
}

QList<JobRecord> JobJournal::entries() const
{
    const auto records = readAll();
    return records ? records.value() : QList<JobRecord>{};
}

int JobJournal::uncertainCount() const
{
    const auto records = readAll();
    if (!records)
        return -1; // cannot tell; an empty list would read as "nothing pending"

    QHash<QUuid, QString> lastState;
    for (const JobRecord &record : records.value())
        lastState.insert(record.id, record.state);
    int count = 0;
    for (const QString &state : lastState) {
        if (state == QLatin1String("uncertain") || state == QLatin1String("intent"))
            ++count;
    }
    return count;
}

} // namespace pp
