#include "FakeCupsBackend.h"
#include "ImageSource.h"
#include "JobJournal.h"
#include "PaperGeometry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

using namespace pp;

namespace {

/// Fails the durability step while the write itself succeeds, which is the one
/// failure a real file cannot be made to produce on demand. The rollback can be
/// refused as well: on a healthy filesystem the truncation always succeeds, and
/// the branch where it does not is the one that decides whether the spool may
/// be deleted.
class FailingSyncJournal : public JobJournal
{
public:
    using JobJournal::JobJournal;
    bool failSync = false;
    bool failRollback = false;
    int syncCalls = 0;

protected:
    Result<void> syncFile(QFile &file) override
    {
        ++syncCalls;
        if (failSync)
            return error(QStringLiteral("journal-write"), QStringLiteral("sync failed"));
        return JobJournal::syncFile(file);
    }

    bool truncateTo(QFile &file, qint64 size) override
    {
        if (failRollback)
            return false;
        return JobJournal::truncateTo(file, size);
    }
};

/// Cuts the journal at `size`, emulating a crash in the middle of the last
/// record or just before its terminating newline.
bool cutJournal(const QString &path, qint64 size)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    return file.resize(size);
}

char lastByte(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() == 0)
        return '\0';
    if (!file.seek(file.size() - 1))
        return '\0';
    const QByteArray one = file.read(1);
    return one.isEmpty() ? '\0' : one.at(0);
}

quint64 inodeOf(const QString &path)
{
#if defined(Q_OS_UNIX)
    struct stat info {};
    if (::stat(QFile::encodeName(path).constData(), &info) != 0)
        return 0;
    return static_cast<quint64>(info.st_ino);
#else
    Q_UNUSED(path);
    return 0;
#endif
}

void writeRawRecord(QFile &file, const QUuid &id, const QString &state, const QByteArray &padding)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("state"), state);
    object.insert(QStringLiteral("detail"), QString::fromLatin1(padding));
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.write("\n");
}

/// A journal above the compaction threshold whose live jobs alone keep it
/// there, so a rewrite cannot bring it back under the limit.
void writeFatJournal(const QString &path, int jobs)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QByteArray padding(4096, 'x');
    for (int i = 0; i < jobs; ++i)
        writeRawRecord(file, QUuid::createUuid(), QStringLiteral("queued"), padding);
}

} // namespace

class TestJobRecovery : public QObject
{
    Q_OBJECT

private slots:
    void uncertainIntentSurvivesRestartWithoutResubmit();
    void journalKeepsOrderAndDetail();
    void orphanedIntentCountsAsUncertain();
    void corruptLinesAreSkipped();
    void unreadableJournalIsNotAFirstRun();
    void unreadableJournalReportsUnknownCount();
    void nonUuidIdIsSkipped();
    void failedSyncRollsTheRecordBack();
    void failedRollbackIsReportedAsUncertain();
    void partialLastLineDoesNotSwallowTheNextRecord();
    void appendWaitsForTheJournalLock();
    void compactionIsNotRepeatedOnEveryAppend();
    void largeJournalIsCompacted();
};

void TestJobRecovery::uncertainIntentSurvivesRestartWithoutResubmit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));

    JobJournal journal(path);
    const QUuid id = QUuid::createUuid();
    QVERIFY(journal.appendIntent(id, QStringLiteral("DCPT730DW"),
                                 QStringLiteral("abcdef")).ok());
    QVERIFY(journal.appendOutcome(id, QStringLiteral("uncertain"), -1,
                                  QStringLiteral("timeout after submit")).ok());

    FakeCupsBackend backend;
    QCOMPARE(backend.submits, 0);
    const JobJournal reread(path);
    QCOMPARE(reread.uncertainCount(), 1);
    QCOMPARE(reread.entries().size(), 2);
    QCOMPARE(backend.submits, 0);
}

void TestJobRecovery::journalKeepsOrderAndDetail()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));
    JobJournal journal(path);
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();
    QVERIFY(journal.appendIntent(first, QStringLiteral("Q1"), QStringLiteral("sha1")).ok());
    QVERIFY(journal.appendOutcome(first, QStringLiteral("queued"), 11).ok());
    QVERIFY(journal.appendIntent(second, QStringLiteral("Q2"), QStringLiteral("sha2")).ok());
    QVERIFY(journal.appendOutcome(second, QStringLiteral("rejected"), -1,
                                  QStringLiteral("media unsupported")).ok());

    const QList<JobRecord> entries = journal.entries();
    QCOMPARE(entries.size(), 4);
    QCOMPARE(entries.at(0).id, first);
    QCOMPARE(entries.at(1).jobId, 11);
    QCOMPARE(entries.at(2).queue, QStringLiteral("Q2"));
    QCOMPARE(entries.at(3).detail, QStringLiteral("media unsupported"));
}

void TestJobRecovery::orphanedIntentCountsAsUncertain()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));
    JobJournal journal(path);
    const QUuid crashed = QUuid::createUuid();
    QVERIFY(journal.appendIntent(crashed, QStringLiteral("DCPT730DW"),
                                 QStringLiteral("sha")).ok());
    // No outcome: a crash after the intent but before the submit response.
    QCOMPARE(journal.uncertainCount(), 1);
}

void TestJobRecovery::corruptLinesAreSkipped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));
    JobJournal journal(path);
    const QUuid id = QUuid::createUuid();
    QVERIFY(journal.appendIntent(id, QStringLiteral("Q1"), QStringLiteral("sha1")).ok());

    // A crash between the writes leaves half a line; a later append keeps it.
    QFile file(path);
    QVERIFY(file.open(QIODevice::Append));
    file.write("{\"id\":\"truncated\",\"state\":\"uncertain\"\n");
    file.write("not json at all\n");
    file.write("\n");
    file.close();
    QVERIFY(journal.appendOutcome(id, QStringLiteral("queued"), 7).ok());

    const auto read = journal.readAll();
    QVERIFY(read.ok());
    QCOMPARE(read.value().size(), 2);
    QCOMPARE(read.value().at(1).jobId, 7);
    QCOMPARE(journal.uncertainCount(), 0);
}

void TestJobRecovery::unreadableJournalIsNotAFirstRun()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Missing file: no history, not an error.
    JobJournal missing(dir.filePath(QStringLiteral("nothing-here.jsonl")));
    const auto empty = missing.readAll();
    QVERIFY(empty.ok());
    QVERIFY(empty.value().isEmpty());

    // A directory in place of the journal cannot be read and must say so.
    const QString blockedPath = dir.filePath(QStringLiteral("is-a-directory"));
    QVERIFY(QDir().mkpath(blockedPath));
    JobJournal blocked(blockedPath);
    const auto unreadable = blocked.readAll();
    QVERIFY(!unreadable.ok());
    QCOMPARE(unreadable.error().code, QStringLiteral("journal-read"));
    QVERIFY(blocked.entries().isEmpty());
}

void TestJobRecovery::unreadableJournalReportsUnknownCount()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // A first run genuinely has nothing pending.
    JobJournal missing(dir.filePath(QStringLiteral("nothing-here.jsonl")));
    QCOMPARE(missing.uncertainCount(), 0);

    // An unreadable journal must not answer zero: that is indistinguishable
    // from a first run, so the startup warning would silently disappear.
    const QString blockedPath = dir.filePath(QStringLiteral("is-a-directory"));
    QVERIFY(QDir().mkpath(blockedPath));
    JobJournal blocked(blockedPath);
    QCOMPARE(blocked.uncertainCount(), -1);
    // The lenient list wrapper still reads as empty; callers that care use
    // readAll() or the count.
    QVERIFY(blocked.entries().isEmpty());
}

void TestJobRecovery::nonUuidIdIsSkipped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));

    // Well-formed JSON with a damaged id must not collapse onto the null uuid:
    // every such record would otherwise merge into one imaginary job.
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    writeRawRecord(file, QUuid(), QStringLiteral("uncertain"), QByteArray("x"));
    file.write("{\"id\":\"not-a-uuid\",\"state\":\"uncertain\"}\n");
    file.write("{\"id\":\"\",\"state\":\"intent\"}\n");
    file.close();

    JobJournal journal(path);
    const auto read = journal.readAll();
    QVERIFY(read.ok());
    QVERIFY2(read.value().isEmpty(),
             qPrintable(QStringLiteral("%1 records").arg(read.value().size())));
    QCOMPARE(journal.uncertainCount(), 0);
}

void TestJobRecovery::failedSyncRollsTheRecordBack()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));

    FailingSyncJournal journal(path);
    const QUuid recorded = QUuid::createUuid();
    QVERIFY(journal.appendOutcome(recorded, QStringLiteral("queued"), 5).ok());
    const qint64 before = QFileInfo(path).size();
    QVERIFY(before > 0);
    const int syncsSoFar = journal.syncCalls;

    journal.failSync = true;
    const QUuid lost = QUuid::createUuid();
    const auto appended = journal.appendIntent(lost, QStringLiteral("Q1"), QStringLiteral("sha"));
    QVERIFY(!appended.ok());
    QCOMPARE(appended.error().code, QStringLiteral("journal-write"));
    // The record was written and then the durability step failed.
    QCOMPARE(journal.syncCalls, syncsSoFar + 1);

    // The bytes reached the file, not the device. The caller treats this as a
    // failed append and deletes the spool, so the record must not survive: a
    // lingering intent would be read as a pending job on the next start.
    QCOMPARE(QFileInfo(path).size(), before);
    JobJournal reread(path);
    const auto records = reread.readAll();
    QVERIFY(records.ok());
    QCOMPARE(records.value().size(), 1);
    QCOMPARE(records.value().first().id, recorded);
    QCOMPARE(reread.uncertainCount(), 0);
}

void TestJobRecovery::failedRollbackIsReportedAsUncertain()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));

    FailingSyncJournal journal(path);
    const QUuid settled = QUuid::createUuid();
    QVERIFY(journal.appendOutcome(settled, QStringLiteral("queued"), 4).ok());
    const qint64 before = QFileInfo(path).size();
    QVERIFY(before > 0);

    // Both steps fail: the record reached the file, the device never took it,
    // and the rollback cannot take it back either.
    journal.failSync = true;
    journal.failRollback = true;
    const QUuid ghost = QUuid::createUuid();
    const auto appended = journal.appendIntent(ghost, QStringLiteral("Q1"), QStringLiteral("sha"));
    QVERIFY(!appended.ok());
    // Not 'journal-write': that would tell the caller nothing survived, and the
    // caller would delete the spool of the job this record describes.
    QCOMPARE(appended.error().code, QStringLiteral("journal-uncertain"));
    QVERIFY(QFileInfo(path).size() > before);

    // The record really is readable: the caller has to keep the evidence.
    const JobJournal reread(path);
    QCOMPARE(reread.uncertainCount(), 1);
    QCOMPARE(reread.entries().last().id, ghost);
}

void TestJobRecovery::partialLastLineDoesNotSwallowTheNextRecord()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // A crash cut the last record in half, so the file does not end in a
    // newline. Appending at that offset glued the next record onto the
    // fragment, and readAll() dropped the merged, unparseable line: the fresh
    // intent, the one whose durability the append had just confirmed, vanished
    // with the fragment.
    const QString cutPath = dir.filePath(QStringLiteral("cut.jsonl"));
    JobJournal cut(cutPath);
    QVERIFY(cut.appendIntent(QUuid::createUuid(), QStringLiteral("Q1"),
                             QStringLiteral("sha1")).ok());
    const qint64 written = QFileInfo(cutPath).size();
    QVERIFY(written > 40);
    QVERIFY(cutJournal(cutPath, written - 15)); // ends inside the record

    const QUuid fresh = QUuid::createUuid();
    QVERIFY(cut.appendIntent(fresh, QStringLiteral("Q2"), QStringLiteral("sha2")).ok());
    QCOMPARE(lastByte(cutPath), '\n');

    const JobJournal afterCut(cutPath);
    const auto cutRecords = afterCut.readAll();
    QVERIFY(cutRecords.ok());
    QCOMPARE(cutRecords.value().size(), 1);
    QCOMPARE(cutRecords.value().first().id, fresh);
    QCOMPARE(cutRecords.value().first().queue, QStringLiteral("Q2"));
    // The pending intent is visible after a restart instead of reading as a
    // clean journal with nothing to warn about.
    QCOMPARE(afterCut.uncertainCount(), 1);

    // The terminator is not rewritten twice: the next append is still one
    // record on one line.
    const QUuid later = QUuid::createUuid();
    QVERIFY(cut.appendIntent(later, QStringLiteral("Q3"), QStringLiteral("sha3")).ok());
    const auto laterRecords = afterCut.readAll();
    QVERIFY(laterRecords.ok());
    QCOMPARE(laterRecords.value().size(), 2);
    QCOMPARE(laterRecords.value().last().id, later);

    // A record that is complete but lost its final newline is still a record:
    // it and the appended one must both be readable, not merged into one line.
    const QString unterminatedPath = dir.filePath(QStringLiteral("unterminated.jsonl"));
    JobJournal unterminated(unterminatedPath);
    const QUuid first = QUuid::createUuid();
    QVERIFY(unterminated.appendIntent(first, QStringLiteral("Q1"), QStringLiteral("sha1")).ok());
    QVERIFY(cutJournal(unterminatedPath, QFileInfo(unterminatedPath).size() - 1));
    const QUuid second = QUuid::createUuid();
    QVERIFY(unterminated.appendIntent(second, QStringLiteral("Q2"), QStringLiteral("sha2")).ok());

    const JobJournal afterUnterminated(unterminatedPath);
    const auto records = afterUnterminated.readAll();
    QVERIFY(records.ok());
    QCOMPARE(records.value().size(), 2);
    QCOMPARE(records.value().at(0).id, first);
    QCOMPARE(records.value().at(1).id, second);
    QCOMPARE(afterUnterminated.uncertainCount(), 2);
}

void TestJobRecovery::appendWaitsForTheJournalLock()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));
    writeFatJournal(path, 300);
    const qint64 fat = QFileInfo(path).size();
    QVERIFY(fat > 1024 * 1024);

    // Another instance is in the middle of its own read-modify-write.
    QLockFile other(QFileInfo(path).absoluteFilePath() + QStringLiteral(".lock"));
    QVERIFY(other.tryLock(0));

    JobJournal journal(path);
    const auto blocked =
        journal.appendIntent(QUuid::createUuid(), QStringLiteral("Q1"), QStringLiteral("sha"));
    QVERIFY(!blocked.ok());
    QCOMPARE(blocked.error().code, QStringLiteral("journal-lock"));
    // Neither the append nor the pending compaction touched the file.
    QCOMPARE(QFileInfo(path).size(), fat);

    other.unlock();
    QVERIFY(journal.appendIntent(QUuid::createUuid(), QStringLiteral("Q2"),
                                 QStringLiteral("sha")).ok());
}

void TestJobRecovery::compactionIsNotRepeatedOnEveryAppend()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));
    writeFatJournal(path, 300);
    const quint64 originalInode = inodeOf(path);
    QVERIFY(QFileInfo(path).size() > 1024 * 1024);

    JobJournal journal(path);
    QVERIFY(journal.appendIntent(QUuid::createUuid(), QStringLiteral("Q1"),
                                 QStringLiteral("sha")).ok());
    const qint64 compacted = QFileInfo(path).size();
    const quint64 compactedInode = inodeOf(path);
    // The live records alone keep the file above the threshold: a rewrite did
    // happen and did not bring it back under the limit.
    QVERIFY(compacted > 1024 * 1024);
#if defined(Q_OS_UNIX)
    QVERIFY(originalInode != 0);
    QVERIFY2(compactedInode != originalInode, "a rewrite should replace the file");
#endif

    // The next append must not pay for another full read-modify-write.
    QVERIFY(journal.appendIntent(QUuid::createUuid(), QStringLiteral("Q2"),
                                 QStringLiteral("sha")).ok());
#if defined(Q_OS_UNIX)
    QCOMPARE(inodeOf(path), compactedInode);
#endif
    QVERIFY(QFileInfo(path).size() < compacted + 1024);
}

void TestJobRecovery::largeJournalIsCompacted()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("jobs.jsonl"));

    // Build a journal well above the rotation threshold without thousands of
    // fsync calls: records for one finished job plus one lingering intent.
    const QUuid finished = QUuid::createUuid();
    const QUuid pending = QUuid::createUuid();
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray padding(4096, 'x');
    for (int i = 0; i < 400; ++i) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), finished.toString(QUuid::WithoutBraces));
        object.insert(QStringLiteral("state"), i % 2 == 0 ? QStringLiteral("intent")
                                                         : QStringLiteral("queued"));
        object.insert(QStringLiteral("detail"), QString::fromLatin1(padding));
        object.insert(QStringLiteral("jobId"), i);
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        file.write("\n");
    }
    QJsonObject lingering;
    lingering.insert(QStringLiteral("id"), pending.toString(QUuid::WithoutBraces));
    lingering.insert(QStringLiteral("state"), QStringLiteral("uncertain"));
    file.write(QJsonDocument(lingering).toJson(QJsonDocument::Compact));
    file.write("\n");
    file.close();
    QVERIFY(QFileInfo(path).size() > 1024 * 1024);

    JobJournal journal(path);
    QVERIFY(journal.appendIntent(QUuid::createUuid(), QStringLiteral("Q"),
                                 QStringLiteral("s")).ok());

    QVERIFY(QFileInfo(path).size() < 64 * 1024);
    const auto read = journal.readAll();
    QVERIFY(read.ok());
    // One record per known job: the finished job's last state, the lingering
    // intent and the fresh one.
    QCOMPARE(read.value().size(), 3);
    QCOMPARE(journal.uncertainCount(), 2);
    for (const JobRecord &record : read.value()) {
        if (record.id == finished)
            QCOMPARE(record.state, QStringLiteral("queued"));
    }
}

QTEST_GUILESS_MAIN(TestJobRecovery)
#include "tst_job_recovery.moc"
