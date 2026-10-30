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

#include <atomic>
#include <cmath>
#include <thread>

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
    void resolvedTicketReachesTheBackend();
    void rejectedSubmitIsJournaled();
    void uncertainSubmitIsJournaledWithoutRetry();
    void doubleClickDoesNotSubmitTwice();
    void concurrentStartsSubmitOnce();
    void missingOrChangedSourceStopsBeforeSubmit();
    void cancelledStartNeverSubmits();
    void failedJournalIntentLeavesNoSpoolOrManifest();
    void realAdapterClassifiesUncertainWithoutRetry();
    void realAdapterSendsTheResolvedOptions();
    void borderlessPlacementCoversThePage();
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
    QVERIFY(result.value().pdfPath.isEmpty()); // normal closure drops the spool
    QVERIFY(QFileInfo::exists(controller.lastManifestPath()));
    QVERIFY(controller.lastPdfPath().isEmpty());

    JobJournal journal(m_config.journalPath);
    const QList<JobRecord> entries = journal.entries();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).state, QStringLiteral("intent"));
    QCOMPARE(entries.at(0).queue, QStringLiteral("FakeQueue"));
    QCOMPARE(entries.at(1).state, QStringLiteral("queued"));
    QCOMPARE(entries.at(1).jobId, 42);
    QVERIFY(QFile::remove(m_config.journalPath));
}

void TestJobController::resolvedTicketReachesTheBackend()
{
    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::KnownJob;
    backend.outcome.jobId = 11;
    ImageStore store;
    JobController controller(store, m_config, &backend);

    PrintTicket ticket = ticketFor(a4());
    ticket.duplexOption = QStringLiteral("DuplexNoTumble");
    ticket.qualityOption = QStringLiteral("High");
    ticket.colorOption = QStringLiteral("RGB");
    ticket.mediaTypeOption = QStringLiteral("PhotographicGlossy");
    ticket.pageSizeOption = QStringLiteral("4x6");
    ticket.scaleToFit = true;
    ticket.copies = 2;

    const auto result = controller.start(documentWith(m_source), ticket);
    QVERIFY(result.ok());
    QCOMPARE(backend.submits, 1);
    // The adapter's resolved ticket is what the backend saw; nothing is
    // re-derived or dropped on the way.
    QCOMPARE(backend.lastTicket.duplexOption, QStringLiteral("DuplexNoTumble"));
    QCOMPARE(backend.lastTicket.qualityOption, QStringLiteral("High"));
    QCOMPARE(backend.lastTicket.colorOption, QStringLiteral("RGB"));
    QCOMPARE(backend.lastTicket.mediaTypeOption, QStringLiteral("PhotographicGlossy"));
    QCOMPARE(backend.lastTicket.pageSizeOption, QStringLiteral("4x6"));
    QVERIFY(backend.lastTicket.scaleToFit);
    QCOMPARE(backend.lastTicket.copies, 2);
    QVERIFY(!backend.lastArtifact.path.isEmpty());
    QCOMPARE(backend.queues, QStringList{QStringLiteral("FakeQueue")});
    QFile::remove(m_config.journalPath);
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
    // Evidence: both the spool and the manifest survive an uncertain job, and
    // the reported path is the file that really is on disk.
    QVERIFY(QFileInfo::exists(result.value().pdfPath));
    QVERIFY(QFileInfo::exists(controller.lastManifestPath()));
    QCOMPARE(controller.lastPdfPath(), result.value().pdfPath);
    QFile::remove(result.value().pdfPath);
    QFile::remove(controller.lastManifestPath());

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

void TestJobController::concurrentStartsSubmitOnce()
{
    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::KnownJob;
    backend.outcome.jobId = 21;
    ImageStore store;
    JobController controller(store, m_config, &backend);

    std::atomic<bool> go{false};
    std::atomic<int> accepted{0};
    const DocumentSnapshot document = documentWith(m_source);
    const PrintTicket ticket = ticketFor(a4());
    auto worker = [&] {
        while (!go.load())
            std::this_thread::yield();
        const auto result = controller.start(document, ticket);
        if (result)
            ++accepted;
    };
    std::thread first(worker);
    std::thread second(worker);
    go.store(true);
    first.join();
    second.join();

    // Whichever gate stops the second call (busy or the 1200 ms window), one
    // user action still yields exactly one CUPS submission.
    QCOMPARE(backend.submits, 1);
    QCOMPARE(accepted.load(), 1);
    QVERIFY(!controller.busy());
    QFile::remove(m_config.journalPath);
}

void TestJobController::cancelledStartNeverSubmits()
{
    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::KnownJob;
    ImageStore store;

    std::stop_source stop;
    stop.request_stop();
    JobConfig config;
    config.workDir = m_dir.filePath(QStringLiteral("cancelled-jobs"));
    config.journalPath = m_dir.filePath(QStringLiteral("cancelled-jobs.jsonl"));
    JobController controller(store, config, &backend);

    const auto result = controller.start(documentWith(m_source), ticketFor(a4()), stop.get_token());
    QVERIFY(!result.ok());
    QCOMPARE(result.error().code, QStringLiteral("cancelled"));
    QCOMPARE(backend.submits, 0);
    QVERIFY(!QFileInfo::exists(config.journalPath));
    QVERIFY(!controller.busy());
    const QDir workDir(config.workDir);
    QVERIFY2(workDir.entryList(QDir::Files).isEmpty(),
             qPrintable(workDir.entryList(QDir::Files).join(QLatin1Char(' '))));
}

void TestJobController::failedJournalIntentLeavesNoSpoolOrManifest()
{
    FakeCupsBackend backend;
    backend.outcome.status = SubmitStatus::KnownJob;
    ImageStore store;
    JobConfig config;
    config.workDir = m_dir.filePath(QStringLiteral("failed-jobs"));
    config.journalPath = m_dir.path(); // a directory: the journal cannot open it
    JobController controller(store, config, &backend);

    const auto result = controller.start(documentWith(m_source), ticketFor(a4()));
    QVERIFY(!result.ok());
    QCOMPARE(result.error().code, QStringLiteral("journal-write"));
    QCOMPARE(backend.submits, 0);
    // The PDF was already rendered, but without a durable intent no job exists
    // and nothing may be left behind.
    const QDir workDir(config.workDir);
    QVERIFY2(workDir.entryList(QDir::Files).isEmpty(),
             qPrintable(workDir.entryList(QDir::Files).join(QLatin1Char(' '))));
    QVERIFY(!controller.busy());
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
    QStringList options;
    CupsCallResult printFile(const QString &, const QString &, const QStringList &sent) override
    {
        ++calls;
        options = sent;
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

void TestJobController::realAdapterSendsTheResolvedOptions()
{
    StubCupsCall call;
    call.result.jobId = 5;
    RealCupsBackend backend(&call);

    PrintTicket ticket = ticketFor(a4());
    ticket.duplexOption = QStringLiteral("DuplexNoTumble");
    ticket.scaleToFit = true;
    ExportArtifact artifact;
    artifact.path = m_imagePath;
    const SubmitOutcome outcome = backend.submit(ticket.queueName, artifact, ticket);
    QCOMPARE(outcome.status, SubmitStatus::KnownJob);

    // Both resolved values reach CUPS instead of being replaced by constants.
    QVERIFY2(call.options.contains(QStringLiteral("Duplex=DuplexNoTumble")),
             qPrintable(call.options.join(QLatin1Char(' '))));
    QVERIFY2(call.options.contains(QStringLiteral("fit-to-page=true")),
             qPrintable(call.options.join(QLatin1Char(' '))));
    // The old code sent this invented option; it is not a CUPS keyword.
    for (const QString &option : call.options)
        QVERIFY(!option.startsWith(QStringLiteral("cupsScaleToFit")));

    StubCupsCall plain;
    plain.result.jobId = 6;
    RealCupsBackend plainBackend(&plain);
    PrintTicket withoutDuplex = ticketFor(a4());
    withoutDuplex.duplexOption.clear();
    withoutDuplex.scaleToFit = false;
    QCOMPARE(plainBackend.submit(withoutDuplex.queueName, artifact, withoutDuplex).status,
             SubmitStatus::KnownJob);
    // No Duplex group on the queue: leave the driver default alone.
    for (const QString &option : plain.options)
        QVERIFY(!option.startsWith(QStringLiteral("Duplex=")));
    QVERIFY(plain.options.contains(QStringLiteral("fit-to-page=false")));
}

void TestJobController::borderlessPlacementCoversThePage()
{
    // The common borderless case: a 10 × 15 cm sheet inside a slightly larger
    // logical page. The physical origin (0,0) already covers, so it stays and
    // the enlargement is exactly the logical/physical ratio.
    const BorderlessPlacement plain =
        borderlessPlacement(QSizeF(100.0, 150.0), QSizeF(101.6, 152.4), QPointF(0.0, 0.0));
    QVERIFY(std::abs(plain.scale - 1.016) < 1e-9);
    QCOMPARE(plain.offsetMm, QPointF(0.0, 0.0));

    // A declared origin that still covers (negative, i.e. already bleeding off
    // the left edge) is kept on that axis instead of being replaced by a
    // constant; the axis the ratio fills exactly stays at the page edge.
    const BorderlessPlacement kept =
        borderlessPlacement(QSizeF(100.0, 150.0), QSizeF(101.6, 154.0), QPointF(-0.5, -0.5));
    QVERIFY(std::abs(kept.scale - 154.0 / 150.0) < 1e-9);
    QCOMPARE(kept.offsetMm, QPointF(-0.5, 0.0));

    // An origin with a positive margin would leave a white edge, so it moves
    // just far enough to cover, and no further.
    const BorderlessPlacement moved =
        borderlessPlacement(QSizeF(100.0, 150.0), QSizeF(101.6, 154.0), QPointF(0.5, 0.5));
    QCOMPARE(moved.offsetMm, QPointF(0.0, 0.0));

    // The enlarged content must cover [0, logical] in both directions.
    const QSizeF physical(100.0, 150.0);
    const QSizeF logical(101.6, 154.0);
    for (const QPointF &origin : {QPointF(0.0, 0.0), QPointF(-0.5, -0.5), QPointF(0.5, 0.5),
                                  QPointF(-9.0, 9.0)}) {
        const BorderlessPlacement placement = borderlessPlacement(physical, logical, origin);
        QVERIFY(placement.scale >= 1.0);
        QVERIFY(placement.offsetMm.x() <= 0.0);
        QVERIFY(placement.offsetMm.y() <= 0.0);
        QVERIFY(placement.offsetMm.x() + physical.width() * placement.scale
                >= logical.width() - 1e-9);
        QVERIFY(placement.offsetMm.y() + physical.height() * placement.scale
                >= logical.height() - 1e-9);
    }

    // Degenerate geometry is left untouched instead of dividing by zero.
    const BorderlessPlacement square =
        borderlessPlacement(QSizeF(0.0, 0.0), QSizeF(101.6, 152.4), QPointF(0.0, 0.0));
    QCOMPARE(square.scale, 1.0);
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
