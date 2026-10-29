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

void TestRecovery::clearRemovesBothSlots()
{
    ImageStore store;
    const auto loaded = store.load(m_imagePath, ImportPolicy{});
    QVERIFY(loaded.ok());
    const QString directory = m_dir.filePath(QStringLiteral("recovery3"));
    RecoveryStore recovery(directory);
    QVERIFY(recovery.checkpoint(documentWith(loaded.value(), 30.0)).ok());
    QVERIFY(recovery.checkpoint(documentWith(loaded.value(), 60.0)).ok());
    recovery.clear();
    const auto latest = recovery.loadLatest();
    QVERIFY(!latest.ok());
    QCOMPARE(latest.error().code, QStringLiteral("recovery-empty"));
}

QTEST_GUILESS_MAIN(TestRecovery)
#include "tst_recovery.moc"
