#include "FakeCupsBackend.h"
#include "ImageSource.h"
#include "JobJournal.h"
#include "PaperGeometry.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace pp;

class TestJobRecovery : public QObject
{
    Q_OBJECT

private slots:
    void uncertainIntentSurvivesRestartWithoutResubmit();
    void journalKeepsOrderAndDetail();
    void orphanedIntentCountsAsUncertain();
    void corruptLinesAreSkipped();
    void unreadableJournalIsNotAFirstRun();
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
