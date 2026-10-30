#include "Document.h"
#include "ImageSource.h"
#include "ProjectStore.h"
#include "RecoveryStore.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QtTest>

using namespace pp;

namespace {

QImage solid(const QColor &colour)
{
    QImage image(120, 80, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
}

DocumentSnapshot documentWith(const SourceImage &source, double cx)
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    Frame frame;
    frame.id = QStringLiteral("frame");
    frame.sourcePath = source.path;
    frame.sourceSha256 = source.sha256;
    frame.cx = cx;
    frame.cy = 148.5;
    frame.width = 80.0;
    frame.height = 60.0;
    page.frames.append(frame);
    document.pages.append(page);
    return document;
}

} // namespace

class TestRecovery : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void checkpointsReplaceEachOtherAtomically();
    void corruptNewestSlotFallsBackToTheOlderSnapshot();
    void restartDoesNotOverwriteTheNewestSlot();
    void manifestCannotPointOutsideTheDirectory();
    void bothCorruptSlotsAreReported();
    void clearRemovesBothSlots();

private:
    QTemporaryDir m_dir;
    QString m_imagePath;
    QByteArray m_sha;
};

void TestRecovery::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_imagePath = m_dir.filePath(QStringLiteral("photo.png"));
    QImageWriter writer(m_imagePath, "png");
    QVERIFY(writer.write(solid(QColor(120, 60, 200))));
    m_sha = fileSha256(m_imagePath);
    QVERIFY(!m_sha.isEmpty());
}

void TestRecovery::checkpointsReplaceEachOtherAtomically()
{
    ImageStore store;
    const auto loaded = store.load(m_imagePath, ImportPolicy{});
    QVERIFY(loaded.ok());

    RecoveryStore recovery(m_dir.filePath(QStringLiteral("recovery")));
    DocumentSnapshot first = documentWith(loaded.value(), 40.0);
    QVERIFY(recovery.checkpoint(first).ok());
    DocumentSnapshot second = documentWith(loaded.value(), 120.0);
    QVERIFY(recovery.checkpoint(second).ok());

    const auto latest = recovery.loadLatest();
    if (!latest)
        QFAIL(qPrintable(latest.error().message));
    QCOMPARE(latest.value().document.pages.at(0).frames.at(0).cx, 120.0);
    QCOMPARE(fileSha256(m_imagePath), m_sha);
}

void TestRecovery::corruptNewestSlotFallsBackToTheOlderSnapshot()
{
    ImageStore store;
    const auto loaded = store.load(m_imagePath, ImportPolicy{});
    QVERIFY(loaded.ok());

    const QString directory = m_dir.filePath(QStringLiteral("recovery2"));
    RecoveryStore recovery(directory);
    QVERIFY(recovery.checkpoint(documentWith(loaded.value(), 50.0)).ok());
    QVERIFY(recovery.checkpoint(documentWith(loaded.value(), 150.0)).ok());

    const QStringList slotNames = {QDir(directory).filePath(QStringLiteral("recovery-a.json")),
                               QDir(directory).filePath(QStringLiteral("recovery-b.json"))};
    // The second checkpoint went to slot b; corrupt the newest complete file.
    QFile::remove(slotNames.at(1));
    QFile broken(slotNames.at(1));
    QVERIFY(broken.open(QIODevice::WriteOnly));
    broken.write("{ broken midway");
    broken.close();

    const auto latest = recovery.loadLatest();
    if (!latest)
        QFAIL(qPrintable(latest.error().message));
    QCOMPARE(latest.value().document.pages.at(0).frames.at(0).cx, 50.0);
}

void TestRecovery::restartDoesNotOverwriteTheNewestSlot()
{
    ImageStore store;
    const auto loaded = store.load(m_imagePath, ImportPolicy{});
    QVERIFY(loaded.ok());

    const QString directory = m_dir.filePath(QStringLiteral("recovery4"));
    {
        RecoveryStore recovery(directory);
        QVERIFY(recovery.checkpoint(documentWith(loaded.value(), 25.0)).ok());
        QVERIFY(recovery.checkpoint(documentWith(loaded.value(), 75.0)).ok());
    }

    // A fresh process must continue with slot a, not overwrite slot b, the
    // newest complete snapshot.
    RecoveryStore restarted(directory);
    QVERIFY(restarted.checkpoint(documentWith(loaded.value(), 95.0)).ok());

    QFile manifest(QDir(directory).filePath(QStringLiteral("latest.txt")));
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromLatin1(manifest.readAll()).trimmed(),
             QStringLiteral("recovery-a.json"));

    const auto latest = restarted.loadLatest();
    if (!latest)
        QFAIL(qPrintable(latest.error().message));
    QCOMPARE(latest.value().document.pages.at(0).frames.at(0).cx, 95.0);
}

void TestRecovery::manifestCannotPointOutsideTheDirectory()
{
    ImageStore store;
    const auto loaded = store.load(m_imagePath, ImportPolicy{});
    QVERIFY(loaded.ok());

    const QString directory = m_dir.filePath(QStringLiteral("recovery5"));
    QVERIFY(QDir().mkpath(directory));
    RecoveryStore recovery(directory);
    QVERIFY(recovery.checkpoint(documentWith(loaded.value(), 45.0)).ok());

    // A hostile manifest must not turn into a path component.
    const QString outside = m_dir.filePath(QStringLiteral("outside.ppj"));
    ProjectStore projects;
    QVERIFY(projects.save(outside, documentWith(loaded.value(), 999.0), SaveMode::Normal).ok());
    QFile manifest(QDir(directory).filePath(QStringLiteral("latest.txt")));
    QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
    manifest.write("../outside.ppj");
    manifest.close();

    const auto latest = recovery.loadLatest();
    if (!latest)
        QFAIL(qPrintable(latest.error().message));
    QCOMPARE(latest.value().document.pages.at(0).frames.at(0).cx, 45.0);
}

void TestRecovery::bothCorruptSlotsAreReported()
{
    const QString directory = m_dir.filePath(QStringLiteral("recovery6"));
    QVERIFY(QDir().mkpath(directory));
    for (const QString &name : {QStringLiteral("recovery-a.json"), QStringLiteral("recovery-b.json")}) {
        QFile file(QDir(directory).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{ not a project");
        file.close();
    }
    RecoveryStore recovery(directory);
    const auto latest = recovery.loadLatest();
    QVERIFY(!latest.ok());
    QCOMPARE(latest.error().code, QStringLiteral("recovery-corrupt"));
}

void TestRecovery::clearRemovesBothSlots()
{
    ImageStore store;
    const auto loaded = store.load(m_imagePath, ImportPolicy{});
    QVERIFY(loaded.ok());
    const QString directory = m_dir.filePath(QStringLiteral("recovery3"));
    RecoveryStore recovery(directory);
    QVERIFY(recovery.checkpoint(documentWith(loaded.value(), 30.0)).ok());
    QVERIFY(recovery.checkpoint(documentWith(loaded.value(), 60.0)).ok());
    QVERIFY(recovery.clear().ok());
    const auto latest = recovery.loadLatest();
    QVERIFY(!latest.ok());
    QCOMPARE(latest.error().code, QStringLiteral("recovery-empty"));
}

QTEST_GUILESS_MAIN(TestRecovery)
#include "tst_recovery.moc"
