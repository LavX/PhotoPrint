#include "Document.h"
#include "ImageSource.h"
#include "PageRenderer.h"
#include "SheetLayout.h"

#include <QColorSpace>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace pp;

namespace {

QString evidenceDir()
{
#ifdef PHOTOPRINT_EVIDENCE_DIR
    const QString dir = QStringLiteral(PHOTOPRINT_EVIDENCE_DIR);
#else
    const QString dir = QDir::currentPath() + QStringLiteral("/build/evidence");
#endif
    QDir().mkpath(dir);
    return dir;
}

QString makePhoto(const QString &directory, const QString &name, const QColor &colour, QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    const QString path = QDir(directory).filePath(name);
    QImageWriter writer(path, "png");
    writer.write(image);
    return path;
}

/// Incompressible pixels, so the source file is large enough that hashing and
/// decoding costs what a real photograph costs.
QString makeNoisyPhoto(const QString &directory, const QString &name, QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QRandomGenerator *random = QRandomGenerator::global();
    for (int y = 0; y < size.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            line[x] = random->generate() | 0xff000000u;
    }
    const QString path = QDir(directory).filePath(name);
    QImageWriter writer(path, "png");
    writer.write(image);
    return path;
}

PaperSpec tenByFifteen()
{
    PaperSpec paper;
    paper.name = QStringLiteral("10 × 15 cm");
    paper.physicalWidthMm = 100.0;
    paper.physicalHeightMm = 150.0;
    paper.type = PaperType::Glossy;
    paper.logicalWidthMm = 101.6;
    paper.logicalHeightMm = 152.4;
    return paper;
}

Frame frameAt(const QString &path, const QByteArray &sha, double cx, double cy, double width,
              double height)
{
    Frame frame;
    frame.id = makeId(QStringLiteral("frame"));
    frame.sourcePath = path;
    frame.sourceSha256 = sha;
    frame.cx = cx;
    frame.cy = cy;
    frame.width = width;
    frame.height = height;
    frame.fit = FitMode::Fill;
    frame.autoPlaced = false;
    return frame;
}

/// Peak resident set size of this process, in KiB. Linux's high-water mark is
/// the right number for "did the job retain every page": current RSS would drop
/// again once the last raster was freed.
qint64 peakRssKiB()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    // readAll(), not an atEnd() loop: /proc files report a size of zero, so
    // QFile::atEnd() is true before the first read.
    const QByteArray all = status.readAll();
    const int start = all.indexOf("VmHWM:");
    if (start < 0)
        return -1;
    const int end = all.indexOf('\n', start);
    const QByteArray line = all.mid(start, end < 0 ? -1 : end - start);
    const QList<QByteArray> parts = line.simplified().split(' ');
    return parts.size() >= 2 ? parts.at(1).toLongLong() : -1;
}

template <typename T>
QString why(const Result<T> &result)
{
    return result.ok() ? QString() : result.error().message;
}

DocumentSnapshot batchDocument(const QString &photo, const QByteArray &sha, int copies)
{
    SheetLayout::BatchSpec spec;
    spec.copyWidthMm = 35.0;
    spec.copyHeightMm = 45.0;
    spec.gutterMm = 3.0;
    spec.marginMm = SheetLayout::kMinMarginMm;
    spec.fillPage = false;
    spec.count = copies;
    Page target;
    target.id = makeId(QStringLiteral("page"));
    target.paper = tenByFifteen();
    const Frame source = frameAt(photo, sha, 31.0, 27.0, 35.0, 45.0);
    const auto built = SheetLayout::batchPages(target, spec, source);
    DocumentSnapshot document;
    if (built)
        document.pages = built.value();
    document.currentPage = 0;
    return document;
}

} // namespace

class TestPdfStreaming : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void hundredCopiesAtPrintResolutionStayBounded();
    void changedSourceMidJobUsesOneVerifiedSnapshot();
};

void TestPdfStreaming::initTestCase()
{
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintPdfStreamingTest"));
    QSettings().clear();
    qputenv("PHOTOPRINT_NO_RECOVERY", "1");
    qputenv("PHOTOPRINT_NO_SETTINGS", "1");
    qputenv("PHOTOPRINT_NO_CUPS", "1");
}

void TestPdfStreaming::hundredCopiesAtPrintResolutionStayBounded()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo =
        makeNoisyPhoto(dir.path(), QStringLiteral("stream-100.png"), QSize(1200, 900));
    const QByteArray sha = fileSha256(photo);
    QVERIFY(!sha.isEmpty());

    // 100 copied 35 × 45 photos on 10 × 15 cm is 17 sheets: the largest page
    // count this slice allows, and the worst case for retained rasters.
    DocumentSnapshot document = batchDocument(photo, sha, 100);
    QCOMPARE(document.pages.size(), 17);
    int copies = 0;
    for (const Page &page : document.pages)
        copies += page.frames.size();
    QCOMPARE(copies, 100);

    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    options.verifySources = true;
    // The real print resolution: Fine on a queue that advertises 600 dpi.
    options.printPpi = 600.0;

    const qint64 before = peakRssKiB();
    QVERIFY(before > 0);
    const QString path = dir.filePath(QStringLiteral("stream-100.pdf"));
    QElapsedTimer timer;
    timer.start();
    const auto artifact = renderer.writePdf(document, path, {}, options);
    const qint64 elapsed = timer.elapsed();
    const qint64 after = peakRssKiB();
    QVERIFY(after > 0);

    QVERIFY2(artifact.ok(), qPrintable(why(artifact)));
    QCOMPARE(artifact.value().pageCount, 17);
    QCOMPARE(renderer.verificationCount(), 1);
    QVERIFY(artifact.value().sha256.size() > 0);

    const double pageMiB = (100.0 / 25.4 * 600.0) * (150.0 / 25.4 * 600.0) * 4.0
        / (1024.0 * 1024.0);
    const double retainedMiB = pageMiB * 17.0;
    const double growthMiB = double(after - before) / 1024.0;

    // A retained-page writer would grow by about 570 MiB for these 17 sheets.
    // One page of raster is about 34 MiB, so the streamed job must stay far
    // below the sum; 400 MiB leaves room for the encoder, the store and the
    // allocator while still failing a writer that keeps every page.
    QVERIFY2(growthMiB < 400.0,
             qPrintable(QStringLiteral("peak grew %1 MiB for %2 pages, one page is %3 MiB")
                            .arg(growthMiB)
                            .arg(17)
                            .arg(pageMiB)));

    QFile report(evidenceDir() + QStringLiteral("/pdf-streaming.txt"));
    QVERIFY(report.open(QIODevice::WriteOnly | QIODevice::Text));
    report.write(QStringLiteral("copies=100 pages=17 ppi=600 resolution=2362x3543\n").toUtf8());
    report.write(QStringLiteral("baseline_peak_mib=%1\n").arg(before / 1024.0).toUtf8());
    report.write(QStringLiteral("job_peak_mib=%1\n").arg(after / 1024.0).toUtf8());
    report.write(QStringLiteral("peak_growth_mib=%1\n").arg(growthMiB).toUtf8());
    report.write(QStringLiteral("one_page_raster_mib=%1\n").arg(pageMiB).toUtf8());
    report.write(QStringLiteral("retained_all_pages_mib=%1\n").arg(retainedMiB).toUtf8());
    report.write(QStringLiteral("elapsed_ms=%1\n").arg(elapsed).toUtf8());
    report.write(QStringLiteral("pdf_bytes=%1\n").arg(QFileInfo(path).size()).toUtf8());
    report.close();

    // The file is a real multi-page PDF, not a truncated stream.
    QProcess pdfinfo;
    pdfinfo.start(QStringLiteral("pdfinfo"), {path});
    QVERIFY(pdfinfo.waitForFinished(15000));
    QCOMPARE(pdfinfo.exitCode(), 0);
    const QString text = QString::fromLatin1(pdfinfo.readAllStandardOutput());
    QVERIFY2(text.contains(QStringLiteral("Pages:          17"))
                 || text.contains(QStringLiteral("Pages:           17")),
             qPrintable(text));
}

void TestPdfStreaming::changedSourceMidJobUsesOneVerifiedSnapshot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QColor oldColour(200, 40, 40);
    const QColor newColour(40, 40, 200);
    const QString photo =
        makePhoto(dir.path(), QStringLiteral("mid-job.png"), oldColour, QSize(800, 1000));
    const QByteArray sha = fileSha256(photo);
    QVERIFY(!sha.isEmpty());

    DocumentSnapshot document;
    for (int i = 0; i < 2; ++i) {
        Page page;
        page.id = makeId(QStringLiteral("page"));
        page.paper = tenByFifteen();
        page.frames = {frameAt(photo, sha, 50.0, 75.0, 35.0, 45.0)};
        document.pages.append(page);
    }

    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions options;
    options.verifySources = true;
    options.printPpi = 72.0;

    int writes = 0;
    renderer.setAfterPageHookForTest([&](int pageIndex) {
        if (pageIndex != 0)
            return;
        // The file is replaced between page 1 and page 2. The job must keep the
        // snapshot it verified, or two versions of one photograph end up in one
        // document. The store's mtime+size check is never consulted again.
        QImage replacement(QSize(800, 1000), QImage::Format_RGB32);
        replacement.setColorSpace(QColorSpace(QColorSpace::SRgb));
        replacement.fill(newColour);
        QImageWriter writer(photo, "png");
        ++writes;
        writer.write(replacement);
    });

    const QString path = dir.filePath(QStringLiteral("mid-job.pdf"));
    const auto artifact = renderer.writePdf(document, path, {}, options);
    QVERIFY2(artifact.ok(), qPrintable(why(artifact)));
    QCOMPARE(artifact.value().pageCount, 2);
    QCOMPARE(renderer.verificationCount(), 1);
    QCOMPARE(writes, 1);
    QVERIFY(fileSha256(photo) != sha);

    // Rasterise both pages: both must show the colour that was verified at the
    // start of the job, not the replacement.
    const QString base = dir.filePath(QStringLiteral("mid-job"));
    QProcess poppler;
    poppler.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QStringLiteral("72"), QStringLiteral("-png"), path, base});
    QVERIFY(poppler.waitForFinished(15000));
    QCOMPARE(poppler.exitCode(), 0);
    const QImage first(base + QStringLiteral("-1.png"));
    const QImage second(base + QStringLiteral("-2.png"));
    QVERIFY(!first.isNull());
    QVERIFY(!second.isNull());
    const QPoint sample(qRound(50.0 / 25.4 * 72.0), qRound(75.0 / 25.4 * 72.0));
    for (const QImage &page : {first, second}) {
        const QColor pixel = page.pixelColor(sample);
        QVERIFY2(std::abs(pixel.red() - oldColour.red()) < 12
                     && std::abs(pixel.blue() - oldColour.blue()) < 12,
                 qPrintable(QStringLiteral("page sampled %1,%2,%3")
                                .arg(pixel.red())
                                .arg(pixel.green())
                                .arg(pixel.blue())));
    }
}

QTEST_MAIN(TestPdfStreaming)
#include "tst_pdf_streaming.moc"
