#include "Document.h"
#include "ImageSource.h"
#include "PageRenderer.h"
#include "PaperGeometry.h"
#include "TrialSheetBuilder.h"

#include <QColor>
#include <QDir>
#include <QImage>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace pp;

namespace {

QImage grayImage()
{
    QImage image(200, 200, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(128, 128, 128));
    return image;
}

PrintContext contextFor(const PaperSpec &paper)
{
    PrintContext context;
    context.queueName = QStringLiteral("DCPT730DW");
    context.driverIdentity = QStringLiteral("brother-native");
    context.ppdFingerprint = QStringLiteral("abc");
    context.pageSizeOption = QStringLiteral("BrPostC4x6_S");
    context.mediaTypeOption = QStringLiteral("Glossy");
    context.qualityOption = QStringLiteral("Fine");
    context.colorOption = QStringLiteral("FullColor");
    context.physicalWidthMm = paper.physicalWidthMm;
    context.physicalHeightMm = paper.physicalHeightMm;
    context.logicalWidthMm = paper.logicalWidthMm > 0 ? paper.logicalWidthMm
                                                      : paper.physicalWidthMm;
    context.logicalHeightMm = paper.logicalHeightMm > 0 ? paper.logicalHeightMm
                                                        : paper.physicalHeightMm;
    return context;
}

} // namespace

class TestTrialSheet : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void variantsKeepTheImageRecipeAndVaryOnlyPaper();
    void smallPaperStillGetsPositiveCells();
    void sourceWithoutHeightDoesNotDivideByZero();
    void renderedVariantsGetBrighterAndDarker();

private:
    QTemporaryDir m_dir;
    SourceImage m_source;
    PaperSpec m_paper;
    DocumentSnapshot m_document;
};

void TestTrialSheet::initTestCase()
{
    QVERIFY(m_dir.isValid());
    const QString path = m_dir.filePath(QStringLiteral("gray.png"));
    QImageWriter writer(path, "png");
    QVERIFY(writer.write(grayImage()));
    ImageStore store;
    const auto loaded = store.load(path, ImportPolicy{});
    QVERIFY(loaded.ok());
    m_source = loaded.value();

    m_paper.name = QStringLiteral("10 × 15 cm");
    m_paper.physicalWidthMm = 100.0;
    m_paper.physicalHeightMm = 150.0;
    m_paper.logicalWidthMm = 101.6;
    m_paper.logicalHeightMm = 152.4;

    Page page;
    page.id = QStringLiteral("page");
    page.paper = m_paper;
    Frame frame;
    frame.id = QStringLiteral("frame");
    frame.sourcePath = m_source.path;
    frame.sourceSha256 = m_source.sha256;
    frame.cx = 50.0;
    frame.cy = 75.0;
    frame.width = 60.0;
    frame.height = 45.0;
    frame.fit = FitMode::Fill;
    frame.angleDeg = 17.0;
    frame.cropCenter = QPointF(0.4, 0.6);
    frame.tone.exposureEv = 0.1;
    frame.tone.shadowLift = 0.02;
    frame.autoTone = true;
    frame.upscale = true;
    page.frames.append(frame);
    m_document.pages.append(page);
    m_document.recipe.paperStrength = 0.05;
    m_document.fineQuality = false;
}

void TestTrialSheet::variantsKeepTheImageRecipeAndVaryOnlyPaper()
{
    const auto built = TrialSheetBuilder::build(m_document, QStringLiteral("frame"),
                                                contextFor(m_paper));
    if (!built)
        QFAIL(qPrintable(built.error().message));
    const TrialSheet &sheet = built.value();
    QCOMPARE(sheet.variants.size(), 4);
    QCOMPARE(sheet.document.pages.size(), 1);
    QCOMPARE(sheet.document.recipe.paperStrength, 0.0);
    // The sheet is printed with the same quality choice as its document.
    QCOMPARE(sheet.document.fineQuality, m_document.fineQuality);

    const Frame *original = findFrame(m_document, QStringLiteral("frame"));
    QVERIFY(original);
    QSet<double> seen;
    for (int i = 0; i < sheet.variants.size(); ++i) {
        const TrialVariant &variant = sheet.variants.at(i);
        QCOMPARE(variant.number, i + 1);
        const Frame *frame = findFrame(sheet.document, variant.frameId);
        QVERIFY(frame);
        QVERIFY(frame->paperOverride);
        QCOMPARE(frame->paperEv, variant.paperEv);
        QCOMPARE(frame->label, QString::number(i + 1));
        QCOMPARE(frame->sourcePath, original->sourcePath);
        QCOMPARE(frame->sourceSha256, original->sourceSha256);
        QCOMPARE(frame->cropCenter, original->cropCenter);
        QCOMPARE(frame->zoom, original->zoom);
        QCOMPARE(frame->fit, original->fit);
        QCOMPARE(frame->tone, original->tone);
        QCOMPARE(frame->autoTone, original->autoTone);
        QVERIFY2(!frame->upscale, "trial variants must not upscale four times");
        seen.insert(variant.paperEv);
    }
    QCOMPARE(seen.size(), 4);
    QCOMPARE(sheet.variants.first().paperEv, 0.0);
}

void TestTrialSheet::smallPaperStillGetsPositiveCells()
{
    DocumentSnapshot document = m_document;
    // 20 mm is narrower than the old fixed 10 mm margins plus 8 mm gutter, so
    // the old cell width was negative.
    PaperSpec small;
    small.name = QStringLiteral("20 × 30 mm");
    small.physicalWidthMm = 20.0;
    small.physicalHeightMm = 30.0;
    document.pages[0].paper = small;

    const auto built = TrialSheetBuilder::build(document, QStringLiteral("frame"),
                                                contextFor(small));
    if (!built)
        QFAIL(qPrintable(built.error().message));
    const TrialSheet &sheet = built.value();
    QCOMPARE(sheet.document.pages.size(), 1);
    QCOMPARE(sheet.document.pages.at(0).frames.size(), sheet.variants.size());
    for (const Frame &frame : sheet.document.pages.at(0).frames) {
        QVERIFY2(frame.width > 0.0 && frame.height > 0.0, "a trial cell must not be empty");
        QVERIFY2(frame.cx - frame.width / 2.0 >= 0.0 && frame.cy - frame.height / 2.0 >= 0.0,
                 "a trial cell must stay on the paper");
        QVERIFY2(frame.cx + frame.width / 2.0 <= 20.0 && frame.cy + frame.height / 2.0 <= 30.0,
                 "a trial cell must stay on the paper");
    }
}

void TestTrialSheet::sourceWithoutHeightDoesNotDivideByZero()
{
    DocumentSnapshot document = m_document;
    Frame *frame = findFrame(document, QStringLiteral("frame"));
    QVERIFY(frame);
    // A zero-height frame used to make the aspect ratio zero and the width
    // computation divide by it.
    frame->height = 0.0;

    const auto built = TrialSheetBuilder::build(document, QStringLiteral("frame"),
                                                contextFor(m_paper));
    if (!built)
        QFAIL(qPrintable(built.error().message));
    const QList<Frame> &cells = built.value().document.pages.at(0).frames;
    QCOMPARE(cells.size(), 4);
    for (const Frame &cell : cells) {
        QVERIFY(std::isfinite(cell.width) && std::isfinite(cell.height));
        QVERIFY(cell.width > 0.0 && cell.height > 0.0);
    }
}

void TestTrialSheet::renderedVariantsGetBrighterAndDarker()
{
    const auto built = TrialSheetBuilder::build(m_document, QStringLiteral("frame"),
                                                contextFor(m_paper));
    if (!built)
        QFAIL(qPrintable(built.error().message));
    const TrialSheet &sheet = built.value();
    ImageStore store;
    PageRenderer renderer(store);
    const double ppi = 150.0;
    const auto rendered = renderer.renderPage(sheet.document, 0, ppi);
    if (!rendered)
        QFAIL(qPrintable(rendered.error().message));
    const QImage page = rendered.value();
    const double pxPerMm = ppi / 25.4;

    auto meanAt = [&](const Frame &frame) {
        const int cx = qRound(frame.cx * pxPerMm);
        const int cy = qRound(frame.cy * pxPerMm);
        const int radius = qRound(4.0 * pxPerMm);
        double sum = 0.0;
        int count = 0;
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                sum += qGreen(page.pixel(x, y));
                ++count;
            }
        }
        return sum / count;
    };

    const Frame *neutral = findFrame(sheet.document, sheet.variants.at(0).frameId);
    const Frame *brighter = findFrame(sheet.document, sheet.variants.at(1).frameId);
    const Frame *darker = findFrame(sheet.document, sheet.variants.at(2).frameId);
    QVERIFY(neutral && brighter && darker);
    const double neutralMean = meanAt(*neutral);
    QVERIFY2(meanAt(*brighter) > neutralMean + 1.0, "positive paper correction should be brighter");
    QVERIFY2(meanAt(*darker) < neutralMean - 1.0, "negative paper correction should be darker");
}

QTEST_MAIN(TestTrialSheet)
#include "tst_trial_sheet.moc"
