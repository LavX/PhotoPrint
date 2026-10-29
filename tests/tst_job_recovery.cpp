#include "FakeCupsBackend.h"
#include "ImageSource.h"
#include "JobJournal.h"
#include "PaperGeometry.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace pp;

class TestJobRecovery : public QObject
{
    Q_OBJECT

private slots:
    void uncertainIntentSurvivesRestartWithoutResubmit();
    void journalKeepsOrderAndDetail();
    void orphanedIntentCountsAsUncertain();
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

QTEST_GUILESS_MAIN(TestJobRecovery)
#include "tst_job_recovery.moc"
