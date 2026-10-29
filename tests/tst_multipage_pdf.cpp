#include "CanvasController.h"
#include "Document.h"
#include "ImageSource.h"
#include "LayoutEngine.h"
#include "PageRenderer.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

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

QImage solid(int width, int height, const QColor &colour)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
}

int channelDistance(QRgb a, QRgb b)
{
    return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                     std::abs(qBlue(a) - qBlue(b))});
}

QString runTool(const QString &program, const QStringList &arguments, int *exitCode)
{
    QProcess process;
    process.start(program, arguments);
    process.waitForFinished(60000);
    *exitCode = process.exitCode();
    return QString::fromLocal8Bit(process.readAllStandardOutput())
        + QString::fromLocal8Bit(process.readAllStandardError());
}

} // namespace

class TestMultipagePdf : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void allPagesKeepTheirOrder();
    void currentPageSubsetExportsOnePage();
    void pageOperationsAreUndoable();

private:
    QTemporaryDir m_dir;
    QString m_redPath;
    QString m_bluePath;
    QByteArray m_redSha;
    QByteArray m_blueSha;
};

void TestMultipagePdf::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_redPath = m_dir.filePath(QStringLiteral("red.png"));
    m_bluePath = m_dir.filePath(QStringLiteral("blue.png"));
    QImageWriter redWriter(m_redPath, "png");
    QVERIFY(redWriter.write(solid(200, 200, QColor(210, 40, 40))));
    QImageWriter blueWriter(m_bluePath, "png");
    QVERIFY(blueWriter.write(solid(200, 200, QColor(40, 60, 210))));
    m_redSha = fileSha256(m_redPath);
    m_blueSha = fileSha256(m_bluePath);
    QVERIFY(!m_redSha.isEmpty());
    QVERIFY(!m_blueSha.isEmpty());
}

void TestMultipagePdf::allPagesKeepTheirOrder()
{
    Page pageOne;
    pageOne.id = QStringLiteral("page-1");
    pageOne.paper.name = QStringLiteral("A4");
    pageOne.paper.physicalWidthMm = 210.0;
    pageOne.paper.physicalHeightMm = 297.0;
    Frame red;
    red.id = QStringLiteral("red");
    red.sourcePath = m_redPath;
    red.sourceSha256 = m_redSha;
    red.cx = 105.0;
    red.cy = 148.5;
    red.width = 100.0;
    red.height = 100.0;
    red.fit = FitMode::Fill;
    pageOne.frames.append(red);

    Page pageTwo = pageOne;
    pageTwo.id = QStringLiteral("page-2");
    Frame blue = red;
    blue.id = QStringLiteral("blue");
    blue.sourcePath = m_bluePath;
    blue.sourceSha256 = m_blueSha;
    pageTwo.frames = {blue};

    DocumentSnapshot document;
    document.pages = {pageOne, pageTwo};

    ImageStore store;
    PageRenderer renderer(store);
    const QString pdfPath = evidenceDir() + QStringLiteral("/multipage.pdf");
    const auto artifact = renderer.writePdf(document, pdfPath, {}, RenderOptions{});
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));
    QCOMPARE(artifact.value().pageCount, 2);

    int exitCode = -1;
    const QString info = runTool(QStringLiteral("pdfinfo"), {pdfPath}, &exitCode);
    QCOMPARE(exitCode, 0);
    QVERIFY(info.contains(QStringLiteral("Pages:           2")));

    const double ppi = 150.0;
    const double pxPerMm = ppi / 25.4;
    for (int pageNumber = 1; pageNumber <= 2; ++pageNumber) {
        const QString base = evidenceDir() + QStringLiteral("/multipage-%1").arg(pageNumber);
        const QString output = runTool(
            QStringLiteral("pdftoppm"),
            {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-f"),
             QString::number(pageNumber), QStringLiteral("-l"), QString::number(pageNumber),
             QStringLiteral("-png"), QStringLiteral("-singlefile"), pdfPath, base},
            &exitCode);
        QCOMPARE(exitCode, 0);
        const QImage raster(base + QStringLiteral(".png"));
        QVERIFY2(!raster.isNull(), qPrintable(output));
        const QRgb pixel = raster.pixel(qRound(105.0 * pxPerMm), qRound(148.5 * pxPerMm));
        const QColor wanted =
            pageNumber == 1 ? QColor(210, 40, 40) : QColor(40, 60, 210);
        QVERIFY2(channelDistance(pixel, wanted.rgb()) <= 4,
                 qPrintable(QStringLiteral("page %1 colour %2/%3/%4")
                                .arg(pageNumber)
                                .arg(qRed(pixel))
                                .arg(qGreen(pixel))
                                .arg(qBlue(pixel))));
    }
}

void TestMultipagePdf::currentPageSubsetExportsOnePage()
{
    Page pageOne;
    pageOne.id = QStringLiteral("page-1");
    pageOne.paper.name = QStringLiteral("A4");
    pageOne.paper.physicalWidthMm = 210.0;
    pageOne.paper.physicalHeightMm = 297.0;
    Frame red;
    red.id = QStringLiteral("red");
    red.sourcePath = m_redPath;
    red.sourceSha256 = m_redSha;
    red.cx = 105.0;
    red.cy = 148.5;
    red.width = 100.0;
    red.height = 100.0;
    red.fit = FitMode::Fill;
    pageOne.frames.append(red);

    Page pageTwo = pageOne;
    pageTwo.id = QStringLiteral("page-2");
    pageTwo.frames.first().id = QStringLiteral("red-2");

    DocumentSnapshot onePage;
    onePage.pages = {pageTwo};

    ImageStore store;
    PageRenderer renderer(store);
    const QString path = evidenceDir() + QStringLiteral("/multipage-single.pdf");
    const auto artifact = renderer.writePdf(onePage, path, {}, RenderOptions{});
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));
    QCOMPARE(artifact.value().pageCount, 1);

    int exitCode = -1;
    const QString info = runTool(QStringLiteral("pdfinfo"), {path}, &exitCode);
    QCOMPARE(exitCode, 0);
    QVERIFY(info.contains(QStringLiteral("Pages:           1")));
}

void TestMultipagePdf::pageOperationsAreUndoable()
{
    CanvasController controller;
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-1");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    document.pages.append(page);
    controller.setDocument(document);

    controller.apply(AddPageEdit{0});
    QCOMPARE(controller.document().pages.size(), 2);
    QCOMPARE(controller.document().currentPage, 1);
    QVERIFY(controller.document().pages.at(0).id != controller.document().pages.at(1).id);

    controller.apply(DuplicatePageEdit{1});
    QCOMPARE(controller.document().pages.size(), 3);
    QCOMPARE(controller.document().currentPage, 2);

    controller.apply(MovePageEdit{2, 0});
    QCOMPARE(controller.document().currentPage, 0);
    const int pagesAfterMove = controller.document().pages.size();
    QCOMPARE(pagesAfterMove, 3);

    controller.apply(RemovePageEdit{0});
    QCOMPARE(controller.document().pages.size(), 2);
    QCOMPARE(controller.document().currentPage, 0);

    controller.undoStack()->undo();
    QCOMPARE(controller.document().pages.size(), 3);
    controller.undoStack()->undo();
    QCOMPARE(controller.document().currentPage, 2);
    controller.undoStack()->undo();
    QCOMPARE(controller.document().pages.size(), 2);
    controller.undoStack()->undo();
    QCOMPARE(controller.document().pages.size(), 1);
    QCOMPARE(controller.document().pages.at(0).id, QStringLiteral("page-1"));
}

QTEST_MAIN(TestMultipagePdf)
#include "tst_multipage_pdf.moc"
