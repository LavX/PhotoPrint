#include "PaperPresetStore.h"
#include "PrintTicket.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace pp;

namespace {

PrintContext baseContext()
{
    PrintContext context;
    context.queueName = QStringLiteral("DCPT730DW");
    context.driverIdentity = QStringLiteral("brother-native");
    context.ppdFingerprint = QStringLiteral("fingerprint");
    context.pageSizeOption = QStringLiteral("BrPostC4x6_S");
    context.mediaTypeOption = QStringLiteral("Glossy");
    context.qualityOption = QStringLiteral("Fine");
    context.colorOption = QStringLiteral("FullColor");
    context.physicalWidthMm = 100.0;
    context.physicalHeightMm = 150.0;
    context.logicalWidthMm = 101.6;
    context.logicalHeightMm = 152.4;
    context.inkIdentity = QStringLiteral("unknown");
    return context;
}

PaperPreset presetFor(const PrintContext &context, double ev, const QString &id)
{
    PaperPreset preset;
    preset.id = id;
    preset.context = context;
    preset.paperEv = ev;
    preset.label = QStringLiteral("Próba %1").arg(id);
    preset.savedAt = QDateTime::currentDateTimeUtc();
    return preset;
}

} // namespace

class TestPaperPresets : public QObject
{
    Q_OBJECT

private slots:
    void matchRequiresTheWholeEnvironment();
    void newFavouriteReplacesTheOldOne();
    void differentInkInvalidatesTheMatch();
    void presetsSurviveARestart();
};

void TestPaperPresets::matchRequiresTheWholeEnvironment()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PaperPresetStore store(dir.filePath(QStringLiteral("presets.json")));
    const PrintContext context = baseContext();
    QVERIFY(store.save(presetFor(context, 0.04, QStringLiteral("a"))).ok());

    const auto match = store.match(context);
    QVERIFY(match.has_value());
    QCOMPARE(match->paperEv, 0.04);

    QList<PrintContext> changed;
    PrintContext queue = context;
    queue.queueName = QStringLiteral("other");
    changed.append(queue);
    PrintContext driver = context;
    driver.driverIdentity = QStringLiteral("ipp-everywhere");
    changed.append(driver);
    PrintContext ppd = context;
    ppd.ppdFingerprint = QStringLiteral("different");
    changed.append(ppd);
    PrintContext media = context;
    media.mediaTypeOption = QStringLiteral("Plain");
    changed.append(media);
    PrintContext quality = context;
    quality.qualityOption = QStringLiteral("Normal");
    changed.append(quality);
    PrintContext paper = context;
    paper.pageSizeOption = QStringLiteral("A4");
    changed.append(paper);
    PrintContext orientation = context;
    orientation.landscape = true;
    changed.append(orientation);

    for (const PrintContext &candidate : changed)
        QVERIFY2(!store.match(candidate).has_value(), "a changed environment must not match");
}

void TestPaperPresets::newFavouriteReplacesTheOldOne()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PaperPresetStore store(dir.filePath(QStringLiteral("presets.json")));
    const PrintContext context = baseContext();
    QVERIFY(store.save(presetFor(context, 0.02, QStringLiteral("a"))).ok());
    QVERIFY(store.save(presetFor(context, 0.06, QStringLiteral("b"))).ok());

    QCOMPARE(store.all().size(), 1);
    const auto match = store.match(context);
    QVERIFY(match.has_value());
    QCOMPARE(match->paperEv, 0.06);
    QCOMPARE(match->id, QStringLiteral("b"));
}

void TestPaperPresets::differentInkInvalidatesTheMatch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PaperPresetStore store(dir.filePath(QStringLiteral("presets.json")));
    const PrintContext context = baseContext();
    QVERIFY(store.save(presetFor(context, 0.03, QStringLiteral("a"))).ok());

    PrintContext otherInk = context;
    otherInk.inkIdentity = PaperPresetStore::newInkIdentity();
    QVERIFY(!store.match(otherInk).has_value());
    QVERIFY(store.match(context).has_value());
}

void TestPaperPresets::presetsSurviveARestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("presets.json"));
    const PrintContext context = baseContext();
    {
        PaperPresetStore store(path);
        QVERIFY(store.save(presetFor(context, 0.05, QStringLiteral("a"))).ok());
    }
    {
        PaperPresetStore store(path);
        const auto match = store.match(context);
        QVERIFY(match.has_value());
        QCOMPARE(match->paperEv, 0.05);
    }
}

QTEST_GUILESS_MAIN(TestPaperPresets)
#include "tst_paper_presets.moc"
