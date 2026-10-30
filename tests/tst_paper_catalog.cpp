#include "PaperCatalog.h"
#include "PaperPresetStore.h"
#include "PrintTicket.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <limits>

using namespace pp;

namespace {

QStringList paperNames(const QList<PaperSpec> &papers)
{
    QStringList names;
    for (const PaperSpec &paper : papers)
        names.append(paper.name);
    return names;
}

} // namespace

class TestPaperCatalog : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAreTheSixEverydayPapers();
    void labelsUseCentimetresOnly();
    void fixedPhotoSizesAreRecognised();
    void customPapersSurviveARestart();
    void customPaperWithTheSameNameIsReplaced();
    void customPaperKeepsEveryField();
    void namesAreTrimmedOnSave();
    void removingAnUnknownPaperFails();
    void corruptPresetFileIsNotOverwritten();
    void printContextToleranceIsShared();
};

void TestPaperCatalog::defaultsAreTheSixEverydayPapers()
{
    const QList<PaperSpec> papers = defaultPapers();
    QCOMPARE(papers.size(), 6);
    QCOMPARE(paperNames(papers),
             QStringList({QStringLiteral("A4"), QStringLiteral("A5"), QStringLiteral("A6"),
                          QStringLiteral("9 × 13 cm"), QStringLiteral("10 × 15 cm"),
                          QStringLiteral("13 × 18 cm")}));

    QCOMPARE(papers.at(0).type, PaperType::Plain);
    QVERIFY(!papers.at(0).isPhotoPaper());
    QCOMPARE(papers.at(1).physicalWidthMm, 148.0);
    QCOMPARE(papers.at(1).physicalHeightMm, 210.0);
    QCOMPARE(papers.at(2).physicalWidthMm, 105.0);
    QCOMPARE(papers.at(2).physicalHeightMm, 148.0);

    const PaperSpec nine = papers.at(3);
    QCOMPARE(nine.physicalWidthMm, 88.9);
    QCOMPARE(nine.physicalHeightMm, 127.0);
    QCOMPARE(nine.logicalWidthMm, 88.9);
    QCOMPARE(nine.logicalHeightMm, 127.0);

    const PaperSpec ten = papers.at(4);
    QCOMPARE(ten.physicalWidthMm, 100.0);
    QCOMPARE(ten.physicalHeightMm, 150.0);
    QCOMPARE(ten.logicalWidthMm, 101.6);
    QCOMPARE(ten.logicalHeightMm, 152.4);

    const PaperSpec thirteen = papers.at(5);
    QCOMPARE(thirteen.physicalWidthMm, 127.0);
    QCOMPARE(thirteen.physicalHeightMm, 177.8);

    for (const PaperSpec &photo : {nine, ten, thirteen}) {
        QCOMPARE(photo.type, PaperType::Glossy);
        QVERIFY(photo.isPhotoPaper());
        QVERIFY(!photo.custom);
    }
}

void TestPaperCatalog::labelsUseCentimetresOnly()
{
    const QList<PaperSpec> papers = defaultPapers();
    QCOMPARE(paperLabel(papers.at(0)), QStringLiteral("A4 (21 × 29,7 cm)"));
    QCOMPARE(paperLabel(papers.at(1)), QStringLiteral("A5 (14,8 × 21 cm)"));
    QCOMPARE(paperLabel(papers.at(2)), QStringLiteral("A6 (10,5 × 14,8 cm)"));
    QCOMPARE(paperLabel(papers.at(4)), QStringLiteral("10 × 15 cm"));
    QCOMPARE(sizeLabelCm(120.0, 180.0), QStringLiteral("12 × 18 cm"));

    for (const PaperSpec &paper : papers) {
        const QString label = paperLabel(paper);
        QVERIFY2(label.contains(QStringLiteral("cm")), qPrintable(label));
        QVERIFY2(!label.contains(QStringLiteral("inch"), Qt::CaseInsensitive), qPrintable(label));
    }

    QCOMPARE(paperTypeLabel(PaperType::Glossy), QStringLiteral("Fényes"));
    QCOMPARE(paperTypeLabel(PaperType::Matte), QStringLiteral("Matt"));
    QCOMPARE(paperTypeLabel(PaperType::Plain), QStringLiteral("Normál"));
}

void TestPaperCatalog::fixedPhotoSizesAreRecognised()
{
    const QList<PaperSpec> papers = defaultPapers();
    QVERIFY(!isFixedPhotoSize(papers.at(0))); // A4
    QVERIFY(isFixedPhotoSize(papers.at(3)));  // 9 × 13
    QVERIFY(isFixedPhotoSize(papers.at(4)));  // 10 × 15
    QVERIFY(isFixedPhotoSize(papers.at(5)));  // 13 × 18

    PaperSpec custom;
    custom.physicalWidthMm = 120.0;
    custom.physicalHeightMm = 180.0;
    QVERIFY(!isFixedPhotoSize(custom));

    PaperSpec rotated;
    rotated.physicalWidthMm = 150.0;
    rotated.physicalHeightMm = 100.0;
    QVERIFY(isFixedPhotoSize(rotated));
}

void TestPaperCatalog::customPapersSurviveARestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));

    PaperSpec custom;
    custom.name = QStringLiteral("12 × 18 cm (egyedi)");
    custom.physicalWidthMm = 120.0;
    custom.physicalHeightMm = 180.0;
    custom.type = PaperType::Matte;
    custom.custom = true;
    custom.borderlessSupported = false;
    {
        CustomPaperStore store(path);
        const auto saved = store.save(custom);
        if (!saved)
            QFAIL(qPrintable(saved.error().message));
        QCOMPARE(store.all().size(), 1);
    }
    {
        CustomPaperStore store(path);
        const QList<PaperSpec> papers = store.all();
        QCOMPARE(papers.size(), 1);
        const PaperSpec &restored = papers.first();
        QCOMPARE(restored.name, custom.name);
        QCOMPARE(restored.physicalWidthMm, 120.0);
        QCOMPARE(restored.physicalHeightMm, 180.0);
        QCOMPARE(restored.type, PaperType::Matte);
        QVERIFY(!restored.borderlessSupported);
        const auto removed = store.remove(custom.name);
        if (!removed)
            QFAIL(qPrintable(removed.error().message));
        QVERIFY(store.all().isEmpty());
    }
}

void TestPaperCatalog::customPaperWithTheSameNameIsReplaced()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
    CustomPaperStore store(path);

    PaperSpec first;
    first.name = QStringLiteral("Négyzet");
    first.physicalWidthMm = 120.0;
    first.physicalHeightMm = 120.0;
    first.type = PaperType::Plain;
    QVERIFY(store.save(first).ok());

    PaperSpec second = first;
    second.physicalHeightMm = 150.0;
    second.type = PaperType::Glossy;
    QVERIFY(store.save(second).ok());

    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().physicalHeightMm, 150.0);
    QCOMPARE(store.all().first().type, PaperType::Glossy);

    auto unnamed = second;
    unnamed.name.clear();
    QVERIFY(!store.save(unnamed).ok());
}

void TestPaperCatalog::customPaperKeepsEveryField()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));

    PaperSpec custom;
    custom.name = QStringLiteral("Egyedi");
    custom.physicalWidthMm = 120.0;
    custom.physicalHeightMm = 180.0;
    custom.logicalWidthMm = 121.5;
    custom.logicalHeightMm = 181.5;
    custom.borderlessSupported = true;
    custom.hasCheckedOffset = true;
    custom.offsetXMm = 1.5;
    custom.offsetYMm = -2.5;
    custom.type = PaperType::Matte;
    custom.custom = true;
    {
        CustomPaperStore store(path);
        QVERIFY(store.save(custom).ok());
    }
    CustomPaperStore store(path);
    const QList<PaperSpec> papers = store.all();
    QCOMPARE(papers.size(), 1);
    const PaperSpec &restored = papers.first();
    QCOMPARE(restored.name, custom.name);
    QCOMPARE(restored.physicalWidthMm, custom.physicalWidthMm);
    QCOMPARE(restored.physicalHeightMm, custom.physicalHeightMm);
    QCOMPARE(restored.logicalWidthMm, custom.logicalWidthMm);
    QCOMPARE(restored.logicalHeightMm, custom.logicalHeightMm);
    QCOMPARE(restored.borderlessSupported, custom.borderlessSupported);
    QCOMPARE(restored.hasCheckedOffset, custom.hasCheckedOffset);
    QCOMPARE(restored.offsetXMm, custom.offsetXMm);
    QCOMPARE(restored.offsetYMm, custom.offsetYMm);
    QCOMPARE(restored.type, custom.type);
}

void TestPaperCatalog::namesAreTrimmedOnSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
    CustomPaperStore store(path);

    PaperSpec custom;
    custom.name = QStringLiteral("  Négyzet  ");
    custom.physicalWidthMm = 120.0;
    custom.physicalHeightMm = 120.0;
    QVERIFY(store.save(custom).ok());
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().name, QStringLiteral("Négyzet"));

    custom.name = QStringLiteral("Négyzet ");
    custom.physicalHeightMm = 130.0;
    QVERIFY(store.save(custom).ok());
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().physicalHeightMm, 130.0);
}

void TestPaperCatalog::removingAnUnknownPaperFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
    CustomPaperStore store(path);

    PaperSpec custom;
    custom.name = QStringLiteral("Marad");
    custom.physicalWidthMm = 100.0;
    custom.physicalHeightMm = 150.0;
    QVERIFY(store.save(custom).ok());

    const auto removed = store.remove(QStringLiteral("Nincs ilyen"));
    QVERIFY(!removed.ok());
    QCOMPARE(removed.error().code, QStringLiteral("paper-missing"));
    QCOMPARE(store.all().size(), 1);
}

void TestPaperCatalog::corruptPresetFileIsNotOverwritten()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("presets.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ this is not a preset list");
    file.close();

    PaperPresetStore presets(path);
    const auto read = presets.readAll();
    QVERIFY(!read.ok());
    QCOMPARE(read.error().code, QStringLiteral("preset-corrupt"));

    PaperPreset preset;
    preset.id = QStringLiteral("a");
    preset.context.queueName = QStringLiteral("DCPT730DW");
    preset.paperEv = 0.05;
    preset.savedAt = QDateTime::currentDateTimeUtc();
    const auto saved = presets.save(preset);
    QVERIFY(!saved.ok());
    QCOMPARE(saved.error().code, QStringLiteral("preset-corrupt"));

    // The damaged file is left in place; a successful save must not be the
    // reason the user's old favourites disappear without a trace.
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("{ this is not a preset list"));
}

void TestPaperCatalog::printContextToleranceIsShared()
{
    // One predicate decides whether two print environments are the same: the
    // millimetre tolerance in sameMillimetres(), used by
    // PrintContext::operator== and forwarded by PaperPresetStore.
    PrintContext context;
    context.queueName = QStringLiteral("DCPT730DW");
    context.driverIdentity = QStringLiteral("brother-native");
    context.physicalWidthMm = 101.6;
    context.physicalHeightMm = 152.4;
    context.logicalWidthMm = 101.6;
    context.logicalHeightMm = 152.4;

    // A JSON round trip and PPD parsing both add noise far below 0.01 mm.
    QVERIFY(sameMillimetres(101.6, 101.6041));
    QVERIFY(sameMillimetres(0.0, 0.0099));
    QVERIFY(!sameMillimetres(101.6, 101.62));
    QVERIFY(!sameMillimetres(0.0, 0.01));

    PrintContext noisy = context;
    noisy.physicalWidthMm += 0.0041;
    noisy.logicalHeightMm -= 0.0041;
    QVERIFY(noisy == context);
    QVERIFY(samePrintContext(noisy, context));

    PrintContext moved = context;
    moved.logicalWidthMm += 0.02;
    QVERIFY(!(moved == context));
    QVERIFY(!samePrintContext(moved, context));

    // Every millimetre field takes part, not only the physical width.
    const auto perturbed = [&context](double PrintContext::*field) {
        PrintContext copy = context;
        copy.*field += 0.02;
        return copy;
    };
    for (double PrintContext::*field :
         {&PrintContext::physicalWidthMm, &PrintContext::physicalHeightMm,
          &PrintContext::logicalWidthMm, &PrintContext::logicalHeightMm}) {
        QVERIFY(!(perturbed(field) == context));
        QVERIFY(!samePrintContext(perturbed(field), context));
    }

    // A finite context is reflexive.
    QVERIFY(context == context);
    QVERIFY(samePrintContext(context, context));

    // Undeclared or corrupted measurements never match, not even themselves:
    // a preset must not be applied to a sheet that was never measured.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const double value : {nan, inf, -inf}) {
        QVERIFY(!sameMillimetres(value, value));
        PrintContext broken = context;
        broken.physicalWidthMm = value;
        QVERIFY(!(broken == broken));
        QVERIFY(!samePrintContext(broken, broken));
    }
}

QTEST_GUILESS_MAIN(TestPaperCatalog)
#include "tst_paper_catalog.moc"
