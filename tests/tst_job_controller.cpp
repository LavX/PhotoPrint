#include "Document.h"
#include "FakeCupsBackend.h"
#include "ImageSource.h"
#include "JobController.h"
#include "JobJournal.h"
#include "PaperGeometry.h"

#include <cups/ipp.h>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace pp;

namespace {

QImage solid(const QColor &colour)
{
    QImage image(160, 120, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
}

PaperSpec a4()
{
    PaperSpec paper;
    paper.name = QStringLiteral("A4");
    paper.physicalWidthMm = 210.0;
    paper.physicalHeightMm = 297.0;
    return paper;
}

PrintTicket ticketFor(const PaperSpec &paper)
{
    PrintTicket ticket;
    ticket.valid = true;
    ticket.queueName = QStringLiteral("FakeQueue");
    ticket.driverIdentity = QStringLiteral("brother-native");
    ticket.ppdFingerprint = QStringLiteral("fingerprint");
    ticket.geometry = PaperGeometry::forPaper(paper, false);
    ticket.pageSizeOption = QStringLiteral("A4");
    ticket.mediaTypeOption = QStringLiteral("Glossy");
    ticket.qualityOption = QStringLiteral("Fine");
    ticket.colorOption = QStringLiteral("FullColor");
    ticket.duplexOption = QStringLiteral("None");
    ticket.copies = 1;
    return ticket;
}

DocumentSnapshot documentWith(const SourceImage &source)
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page");
    page.paper = a4();
    Frame frame;
    frame.id = QStringLiteral("frame");
    frame.sourcePath = source.path;
    frame.sourceSha256 = source.sha256;
    frame.cx = 105.0;
    frame.cy = 148.5;
    frame.width = 100.0;
    frame.height = 80.0;
    page.frames.append(frame);
    document.pages.append(page);
    return document;
}

} // namespace

class TestJobController : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void knownJobIsSubmittedExactlyOnce();
    void rejectedSubmitIsJournaled();
    void uncertainSubmitIsJournaledWithoutRetry();
    void doubleClickDoesNotSubmitTwice();
    void missingOrChangedSourceStopsBeforeSubmit();
    void realAdapterClassifiesUncertainWithoutRetry();
    void borderlessTicketProducesFullBleedPdf();

private:
    QTemporaryDir m_dir;
    QString m_imagePath;
    QByteArray m_sha;
    SourceImage m_source;
    JobConfig m_config;
};

void TestJobController::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_imagePath = m_dir.filePath(QStringLiteral("photo.png"));
    QImageWriter writer(m_imagePath, "png");
    QVERIFY(writer.write(solid(QColor(120, 160, 90))));
    m_sha = fileSha256(m_imagePath);
    ImageStore store;
    const auto loaded = store.load(m_imagePath, ImportPolicy{});
    QVERIFY(loaded.ok());
    m_source = loaded.value();
    m_config.workDir = m_dir.filePath(QStringLiteral("jobs"));
    m_config.journalPath = m_dir.filePath(QStringLiteral("jobs.jsonl"));
}

void TestJobController::knownJobIsSubmittedExactlyOnce()
{
    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::KnownJob;
    backend.outcome.jobId = 42;
    ImageStore store;
    JobController controller(store, m_config, &backend);

    const auto result = controller.start(documentWith(m_source), ticketFor(a4()));
    if (!result)
        QFAIL(qPrintable(result.error().message));
    QCOMPARE(backend.submits, 1);
    QCOMPARE(result.value().status, SubmitStatus::KnownJob);
    QCOMPARE(result.value().message, QStringLiteral("Elküldve a nyomtatónak."));
    QVERIFY(!QFileInfo::exists(result.value().pdfPath)); // normal closure drops the spool
    QVERIFY(QFileInfo::exists(controller.lastManifestPath()));

    JobJournal journal(m_config.journalPath);
    const QList<JobRecord> entries = journal.entries();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).state, QStringLiteral("intent"));
    QCOMPARE(entries.at(0).queue, QStringLiteral("FakeQueue"));
    QCOMPARE(entries.at(1).state, QStringLiteral("queued"));
    QCOMPARE(entries.at(1).jobId, 42);
    QVERIFY(QFile::remove(m_config.journalPath));
}

void TestJobController::rejectedSubmitIsJournaled()
{
    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::Rejected;
    ImageStore store;
    JobController controller(store, m_config, &backend);
    const auto result = controller.start(documentWith(m_source), ticketFor(a4()));
    if (!result)
        QFAIL(qPrintable(result.error().message));
    QCOMPARE(backend.submits, 1);
    QCOMPARE(result.value().status, SubmitStatus::Rejected);
    QVERIFY(result.value().message.contains(QStringLiteral("nem fogadta el")));

    JobJournal journal(m_config.journalPath);
    const QList<JobRecord> entries = journal.entries();
    QCOMPARE(entries.last().state, QStringLiteral("rejected"));
    QVERIFY(QFile::remove(m_config.journalPath));
}

void TestJobController::uncertainSubmitIsJournaledWithoutRetry()
{
    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::Uncertain;
    ImageStore store;
    JobController controller(store, m_config, &backend);
    const auto result = controller.start(documentWith(m_source), ticketFor(a4()));
    if (!result)
        QFAIL(qPrintable(result.error().message));
    QCOMPARE(backend.submits, 1);
    QCOMPARE(result.value().status, SubmitStatus::Uncertain);
    QVERIFY(result.value().message.contains(QStringLiteral("Nem tudjuk")));

    JobJournal journal(m_config.journalPath);
    QCOMPARE(journal.uncertainCount(), 1);

    // A later controller only reads the journal; it never resubmits.
    FakeCupsBackend secondBackend;
    JobController second(store, m_config, &secondBackend);
    QCOMPARE(secondBackend.submits, 0);
    QCOMPARE(JobJournal(m_config.journalPath).uncertainCount(), 1);
    QVERIFY(QFile::remove(m_config.journalPath));
}

void TestJobController::doubleClickDoesNotSubmitTwice()
{
    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::KnownJob;
    backend.outcome.jobId = 7;
    ImageStore store;
    JobController controller(store, m_config, &backend);

    const auto first = controller.start(documentWith(m_source), ticketFor(a4()));
    QVERIFY(first.ok());
    const auto second = controller.start(documentWith(m_source), ticketFor(a4()));
    QVERIFY(!second.ok());
    QCOMPARE(second.error().code, QStringLiteral("job-double"));
    QCOMPARE(backend.submits, 1);
    QVERIFY(QFile::remove(m_config.journalPath));
}

void TestJobController::missingOrChangedSourceStopsBeforeSubmit()
{
    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::KnownJob;
    ImageStore store;
    JobController controller(store, m_config, &backend);

    DocumentSnapshot missing = documentWith(m_source);
    missing.pages[0].frames[0].sourcePath = m_dir.filePath(QStringLiteral("missing.png"));
    const auto missingResult = controller.start(missing, ticketFor(a4()));
    QVERIFY(!missingResult.ok());
    QCOMPARE(backend.submits, 0);

    DocumentSnapshot changed = documentWith(m_source);
    changed.pages[0].frames[0].sourceSha256 = QByteArrayLiteral("deadbeef");
    const auto changedResult = controller.start(changed, ticketFor(a4()));
    QVERIFY(!changedResult.ok());
    QCOMPARE(changedResult.error().code, QStringLiteral("source-changed"));
    QCOMPARE(backend.submits, 0);
    QVERIFY(!QFileInfo::exists(m_config.journalPath));
}

namespace {
class StubCupsCall : public CupsCall
{
public:
    CupsCallResult result;
    int calls = 0;
    CupsCallResult printFile(const QString &, const QString &, const QStringList &) override
    {
        ++calls;
        return result;
    }
};
} // namespace

void TestJobController::realAdapterClassifiesUncertainWithoutRetry()
{
    StubCupsCall call;
    call.result.jobId = 0;
    call.result.ippStatus = IPP_STATUS_ERROR_TIMEOUT;
    call.result.detail = QStringLiteral("timeout");
    RealCupsBackend backend(&call);

    PrintTicket ticket = ticketFor(a4());
    ExportArtifact artifact;
    artifact.path = m_imagePath;
    const SubmitOutcome outcome = backend.submit(ticket.queueName, artifact, ticket);
    QCOMPARE(call.calls, 1);
    QCOMPARE(outcome.status, SubmitStatus::Uncertain);

    StubCupsCall known;
    known.result.jobId = 9;
    RealCupsBackend knownBackend(&known);
    const SubmitOutcome knownOutcome = knownBackend.submit(ticket.queueName, artifact, ticket);
    QCOMPARE(known.calls, 1);
    QCOMPARE(knownOutcome.status, SubmitStatus::KnownJob);
    QCOMPARE(knownOutcome.jobId, 9);
}

void TestJobController::borderlessTicketProducesFullBleedPdf()
{
    PaperSpec photo;
    photo.name = QStringLiteral("10 × 15 cm");
    photo.physicalWidthMm = 100.0;
    photo.physicalHeightMm = 150.0;
    photo.logicalWidthMm = 101.6;
    photo.logicalHeightMm = 152.4;

    DocumentSnapshot document = documentWith(m_source);
    document.pages[0].paper = photo;
    Frame &frame = document.pages[0].frames[0];
    frame.cx = 50.0;
    frame.cy = 75.0;
    frame.width = 100.0;
    frame.height = 150.0;
    frame.fit = FitMode::Fill;

    PrintTicket ticket = ticketFor(photo);
    ticket.geometry = PaperGeometry::forPaper(photo, false);
    ticket.geometry.borderless = true;
    ticket.pageSizeOption = QStringLiteral("BrPostC4x6_B");
    ticket.mediaTypeOption = QStringLiteral("Glossy");

    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::Uncertain; // keeps the spool for inspection
    ImageStore store;
    JobController controller(store, m_config, &backend);
    const auto result = controller.start(document, ticket);
    if (!result)
        QFAIL(qPrintable(result.error().message));
    QVERIFY(QFileInfo::exists(result.value().pdfPath));
    QProcess process;
    process.start(QStringLiteral("pdfinfo"), {result.value().pdfPath});
    QVERIFY(process.waitForFinished(30000));
    const QString info = QString::fromLocal8Bit(process.readAllStandardOutput());
    QVERIFY2(info.contains(QStringLiteral("288 x 432 pts")), qPrintable(info));

    QProcess images;
    images.start(QStringLiteral("pdfimages"), {QStringLiteral("-list"), result.value().pdfPath});
    QVERIFY(images.waitForFinished(30000));
    const QString imageList = QString::fromLocal8Bit(images.readAllStandardOutput());
    QVERIFY2(imageList.contains(QStringLiteral("590")) || imageList.contains(QStringLiteral("600")),
             qPrintable(imageList));

    const QString base = m_dir.filePath(QStringLiteral("borderless-job"));
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QStringLiteral("150"), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), result.value().pdfPath, base});
    QVERIFY(process.waitForFinished(30000));
    QCOMPARE(process.exitCode(), 0);
    const QImage page(base + QStringLiteral(".png"));
    QVERIFY(!page.isNull());
    const double pxPerMm = 150.0 / 25.4;
    const QRgb corner = page.pixel(qRound(0.8 * pxPerMm), qRound(0.8 * pxPerMm));
    QVERIFY2(qGreen(corner) > 140 && qBlue(corner) < 120,
             qPrintable(QStringLiteral("corner %1/%2/%3")
                            .arg(qRed(corner))
                            .arg(qGreen(corner))
                            .arg(qBlue(corner))));
    QFile::remove(result.value().pdfPath); // uncertain evidence, removed by this test
    QFile::remove(m_config.journalPath);
}

QTEST_GUILESS_MAIN(TestJobController)
#include "tst_job_controller.moc"
