#include "CanvasController.h"
#include "Document.h"
#include "ImageSource.h"
#include "LayoutEngine.h"
#include "MainWindow.h"
#include "PageRenderer.h"
#include "PhotoItem.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <QtMath>

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

QImage makeSplitImage()
{
    QImage image(400, 200, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, 200, 200), QColor(220, 30, 30));
    painter.fillRect(QRect(200, 0, 200, 200), QColor(30, 30, 220));
    painter.end();
    return image;
}

Frame baseFrame()
{
    Frame frame;
    frame.id = QStringLiteral("frame-a");
    frame.cx = 105.0;
    frame.cy = 148.5;
    frame.width = 60.0;
    frame.height = 40.0;
    frame.fit = FitMode::Fill;
    frame.cropCenter = QPointF(0.25, 0.5);
    return frame;
}

DocumentSnapshot documentWith(const Frame &frame)
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-a");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    page.frames.append(frame);
    document.pages.append(page);
    return document;
}

double distance(const QPointF &a, const QPointF &b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

QPointF pageToWidget(const PageView &view, const QPointF &mm)
{
    const QRectF rect = view.pageRect();
    const QSizeF page = view.pageMm();
    return QPointF(rect.left() + mm.x() / page.width() * rect.width(),
                   rect.top() + mm.y() / page.height() * rect.height());
}

bool isReddish(QRgb pixel)
{
    return qRed(pixel) > 180 && qGreen(pixel) < 80 && qBlue(pixel) < 80;
}

bool isBluish(QRgb pixel)
{
    return qBlue(pixel) > 180 && qRed(pixel) < 80 && qGreen(pixel) < 80;
}

bool isWhite(QRgb pixel)
{
    return qRed(pixel) > 248 && qGreen(pixel) > 248 && qBlue(pixel) > 248;
}

} // namespace

class TestCanvasEdits : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void moveIsOneUndoableStep();
    void cropSurvivesMoveAndRotation();
    void removeIsUndoable();
    void dragOnCanvasCommitsOnce();
    void keyboardNudgeIsPrecise();
    void orientationRotatesPageAndFramesOnce();
    void fitPageShrinksOverflowingFrames();
    void paperAndOrientationApplyToEveryPage();
    void viewZoomOnlyChangesTheView();
    void pdfCarriesTheRotatedCrop();

private:
    QTemporaryDir m_dir;
    QString m_imagePath;
};

void TestCanvasEdits::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_imagePath = m_dir.filePath(QStringLiteral("split.png"));
    QImageWriter writer(m_imagePath, "png");
    QVERIFY(writer.write(makeSplitImage()));
}

void TestCanvasEdits::moveIsOneUndoableStep()
{
    Frame frame = baseFrame();
    CanvasController controller;
    controller.setDocument(documentWith(frame));

    Frame moved = frame;
    moved.cx += 17.5;
    moved.cy -= 4.0;
    controller.apply(FrameChange{frame.id, moved});
    QCOMPARE(controller.document().pages.at(0).frames.at(0).cx, 122.5);
    QCOMPARE(controller.undoStack()->count(), 1);

    controller.undoStack()->undo();
    QCOMPARE(controller.document().pages.at(0).frames.at(0).cx, frame.cx);

    controller.undoStack()->redo();
    QCOMPARE(controller.document().pages.at(0).frames.at(0).cx, 122.5);
}

void TestCanvasEdits::cropSurvivesMoveAndRotation()
{
    Frame frame = baseFrame();
    frame.cropCenter = QPointF(0.31, 0.62);
    frame.zoom = 1.8;
    CanvasController controller;
    controller.setDocument(documentWith(frame));

    Frame moved = frame;
    moved.cx += 10.0;
    moved.angleDeg = 27.0;
    controller.apply(FrameChange{frame.id, moved});

    const Frame &stored = controller.document().pages.at(0).frames.at(0);
    QCOMPARE(stored.cropCenter, QPointF(0.31, 0.62));
    QCOMPARE(stored.zoom, 1.8);
    QCOMPARE(stored.angleDeg, 27.0);
}

void TestCanvasEdits::removeIsUndoable()
{
    const Frame frame = baseFrame();
    CanvasController controller;
    controller.setDocument(documentWith(frame));

    controller.apply(RemoveFrameEdit{frame.id});
    QCOMPARE(controller.document().pages.at(0).frames.size(), 0);
    controller.undoStack()->undo();
    QCOMPARE(controller.document().pages.at(0).frames.size(), 1);
    QCOMPARE(controller.document().pages.at(0).frames.at(0).id, frame.id);
}

void TestCanvasEdits::dragOnCanvasCommitsOnce()
{
    Frame frame = baseFrame();
    CanvasController controller;
    controller.setDocument(documentWith(frame));

    PageView view;
    view.resize(700, 900);
    view.setDocument(controller.document());
    view.setSelectedFrame(frame.id);
    view.show();
    QTest::qWait(50);

    int commits = 0;
    Frame committed;
    view.onFrameCommitted = [&](const Frame &value) {
        ++commits;
        committed = value;
        controller.apply(FrameChange{value.id, value});
    };

    const QPointF start = pageToWidget(view, QPointF(frame.cx, frame.cy));
    const QPointF end = start + QPointF(40.0, 15.0);
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start.toPoint());
    QTest::mouseMove(&view, end.toPoint());
    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, end.toPoint());

    QCOMPARE(commits, 1);
    QCOMPARE(controller.undoStack()->count(), 1);
    QVERIFY(std::abs(committed.cx - frame.cx) > 1.0);
    QVERIFY(std::abs(committed.cy - frame.cy) > 0.2);
}

void TestCanvasEdits::keyboardNudgeIsPrecise()
{
    Frame frame = baseFrame();
    CanvasController controller;
    controller.setDocument(documentWith(frame));

    PageView view;
    view.resize(700, 900);
    view.setDocument(controller.document());
    view.setSelectedFrame(frame.id);
    view.show();
    view.setFocus();
    QTest::qWait(50);

    Frame last;
    int commits = 0;
    view.onFrameCommitted = [&](const Frame &value) {
        last = value;
        ++commits;
    };

    QTest::keyClick(&view, Qt::Key_Right);
    QCOMPARE(commits, 1);
    QVERIFY(std::abs(last.cx - (frame.cx + 1.0)) < 1e-9);

    QTest::keyClick(&view, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(commits, 2);
    QVERIFY(std::abs(last.cx - (frame.cx + 1.1)) < 1e-9);

    QTest::keyClick(&view, Qt::Key_Down, Qt::ShiftModifier);
    QVERIFY(std::abs(last.cy - (frame.cy + 0.1)) < 1e-9);
}

void TestCanvasEdits::orientationRotatesPageAndFramesOnce()
{
    Frame frame = baseFrame();
    CanvasController controller;
    controller.setDocument(documentWith(frame));

    controller.apply(RotatePageEdit{0});
    const Page &landscape = controller.document().pages.at(0);
    QVERIFY(landscape.landscape);
    QCOMPARE(landscape.frames.size(), 1);
    QVERIFY(std::abs(landscape.frames.at(0).cx - (297.0 - frame.cy)) < 1e-9);
    QVERIFY(std::abs(landscape.frames.at(0).cy - frame.cx) < 1e-9);
    QCOMPARE(landscape.frames.at(0).angleDeg, 90.0);
    QCOMPARE(landscape.frames.at(0).width, frame.width);
    QCOMPARE(landscape.frames.at(0).height, frame.height);
    QCOMPARE(controller.undoStack()->count(), 1);

    controller.undoStack()->undo();
    QCOMPARE(controller.document(), documentWith(frame));
}

void TestCanvasEdits::fitPageShrinksOverflowingFrames()
{
    Frame frame = baseFrame();
    frame.width = 240.0;
    frame.height = 160.0;
    CanvasController controller;
    controller.setDocument(documentWith(frame));
    QVERIFY(frame.cx + frame.width / 2.0 > 210.0);

    controller.apply(FitPageEdit{0});
    const Frame &fitted = controller.document().pages.at(0).frames.at(0);
    QVERIFY(fitted.cx - fitted.width / 2.0 >= -0.01);
    QVERIFY(fitted.cx + fitted.width / 2.0 <= 210.0 + 0.01);
    QVERIFY(fitted.cy - fitted.height / 2.0 >= -0.01);
    QVERIFY(fitted.cy + fitted.height / 2.0 <= 297.0 + 0.01);
    QVERIFY(std::abs(fitted.width / fitted.height - frame.width / frame.height) < 1e-9);
    QCOMPARE(fitted.cropCenter, frame.cropCenter);

    controller.undoStack()->undo();
    QCOMPARE(controller.document(), documentWith(frame));
}

void TestCanvasEdits::paperAndOrientationApplyToEveryPage()
{
    DocumentSnapshot document = documentWith(baseFrame());
    Page second = document.pages.first();
    second.id = QStringLiteral("page-b");
    document.pages.append(second);
    CanvasController controller;
    controller.setDocument(document);

    PaperSpec photo;
    photo.name = QStringLiteral("10 × 15 cm");
    photo.physicalWidthMm = 100.0;
    photo.physicalHeightMm = 150.0;
    photo.logicalWidthMm = 101.6;
    photo.logicalHeightMm = 152.4;
    controller.apply(SetProjectPaperEdit{photo});
    QCOMPARE(controller.document().pages.at(0).paper, photo);
    QCOMPARE(controller.document().pages.at(1).paper, photo);

    controller.apply(SetProjectLandscapeEdit{true});
    QVERIFY(controller.document().pages.at(0).landscape);
    QVERIFY(controller.document().pages.at(1).landscape);
    for (const Page &page : controller.document().pages)
        QCOMPARE(page.frames.at(0).angleDeg, 90.0);

    controller.undoStack()->undo();
    controller.undoStack()->undo();
    QCOMPARE(controller.document(), document);
}

void TestCanvasEdits::viewZoomOnlyChangesTheView()
{
    Frame frame = baseFrame();
    CanvasController controller;
    controller.setDocument(documentWith(frame));

    PageView view;
    view.resize(900, 700);
    view.setDocument(controller.document());
    view.show();
    QTest::qWait(50);

    view.setViewZoom(0.5);
    const double half = view.pageRect().width();
    view.setViewZoom(1.0);
    const double full = view.pageRect().width();
    view.setViewZoom(2.0);
    const double doubled = view.pageRect().width();

    QVERIFY(half < full);
    QVERIFY(full < doubled);
    QVERIFY(std::abs(doubled / full - 2.0) < 0.01);
    QCOMPARE(controller.document(), documentWith(frame));
}

void TestCanvasEdits::pdfCarriesTheRotatedCrop()
{
    ImageStore store;
    const auto source = store.load(m_imagePath, ImportPolicy{});
    if (!source)
        QFAIL(qPrintable(source.error().message));

    Frame frame = baseFrame();
    frame.sourcePath = source.value().path;
    frame.sourceSha256 = source.value().sha256;
    frame.angleDeg = 20.0;

    DocumentSnapshot document = documentWith(frame);
    PageRenderer renderer(store);

    const double ppi = 200.0;
    const auto rendered = renderer.renderPage(document, 0, ppi, {}, RenderOptions{});
    if (!rendered)
        QFAIL(qPrintable(rendered.error().message));
    const QImage page = rendered.value();
    const double pxPerMm = ppi / 25.4;
    auto sampleMm = [&](const QPointF &mm) {
        return page.pixel(qRound(mm.x() * pxPerMm), qRound(mm.y() * pxPerMm));
    };

    const QPointF centre(frame.cx, frame.cy);
    QVERIFY2(isReddish(sampleMm(centre)), "cropped centre should show the red field");

    const QPointF rightLocal =
        centre + QPointF(std::cos(qDegreesToRadians(20.0)) * 25.0,
                         std::sin(qDegreesToRadians(20.0)) * 25.0);
    QVERIFY2(isBluish(sampleMm(rightLocal)), "right side of the crop should show the blue field");

    const QPointF outside =
        centre + QPointF(-std::sin(qDegreesToRadians(20.0)) * 45.0,
                         std::cos(qDegreesToRadians(20.0)) * 45.0);
    QVERIFY2(isWhite(sampleMm(outside)), "outside the rotated frame must stay paper white");

    const QString pdfPath = evidenceDir() + QStringLiteral("/slice2-frame.pdf");
    RenderOptions options;
    options.printPpi = ppi;
    const auto artifact = renderer.writePdf(document, pdfPath, {}, options);
    if (!artifact)
        QFAIL(qPrintable(artifact.error().message));
    QVERIFY(QFileInfo::exists(pdfPath));

    QProcess process;
    const QString base = evidenceDir() + QStringLiteral("/slice2-frame");
    process.start(QStringLiteral("pdftoppm"),
                  {QStringLiteral("-r"), QString::number(ppi), QStringLiteral("-png"),
                   QStringLiteral("-singlefile"), pdfPath, base});
    QVERIFY(process.waitForFinished(60000));
    QCOMPARE(process.exitCode(), 0);

    const QImage pdfPage(base + QStringLiteral(".png"));
    QVERIFY(!pdfPage.isNull());
    auto samplePdf = [&](const QPointF &mm) {
        return pdfPage.pixel(qRound(mm.x() * pxPerMm), qRound(mm.y() * pxPerMm));
    };
    QVERIFY2(isReddish(samplePdf(centre)), "PDF must carry the same crop");
    QVERIFY2(isBluish(samplePdf(rightLocal)), "PDF must carry the same rotation");
    QVERIFY2(isWhite(samplePdf(outside)), "PDF must not paint outside the frame");

    // No editor outline or handles in the export: a line of pixels just outside
    // the frame stays paper white instead of selection blue.
    for (int step = 0; step < 8; ++step) {
        const QPointF outsidePoint =
            centre + QPointF(-std::sin(qDegreesToRadians(20.0)) * (45.0 + step),
                             std::cos(qDegreesToRadians(20.0)) * (45.0 + step));
        QVERIFY2(isWhite(samplePdf(outsidePoint)), "editor marks leaked into the PDF");
    }
}

QTEST_MAIN(TestCanvasEdits)
#include "tst_canvas_edits.moc"
