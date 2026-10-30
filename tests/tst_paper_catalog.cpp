#include "PaperCatalog.h"
#include "PaperPresetStore.h"
#include "PrintTicket.h"

#include <QDir>
#include <QFile>
#include <QLockFile>
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
    void storedPapersAreAlwaysCustom();
    void damagedPaperFileIsNotOverwritten();
    void unparsablePaperFileKeepsItsReadablePaper();
    void corruptPaperEntryIsNotDropped();
    void nonFiniteSizesAreRejected();
    void settingsFailureIsNotSticky();
    void aConcurrentWriterIsRefused();
    void corruptPresetFileIsNotOverwritten();
    void corruptPresetEntriesAreRefused();
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

void TestPaperCatalog::storedPapersAreAlwaysCustom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
    CustomPaperStore store(path);

    // This store only holds sheets the user made, so the flag follows from the
    // store: a paper handed in as a built-in comes back as custom, and nothing
    // that is always true is written to the file.
    PaperSpec paper;
    paper.name = QStringLiteral("Saját négyzet");
    paper.physicalWidthMm = 90.0;
    paper.physicalHeightMm = 90.0;
    paper.custom = false;
    QVERIFY(store.save(paper).ok());

    const auto read = store.readAll();
    QVERIFY(read.ok());
    QCOMPARE(read.value().size(), 1);
    QVERIFY(read.value().first().custom);

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY2(!file.readAll().contains("custom="), "the custom flag is not stored");
}

void TestPaperCatalog::damagedPaperFileIsNotOverwritten()
{
    // A truncated write whose array count key was lost. QSettings reads it as
    // "no papers" without any error at all, so the key heuristic is the only
    // signal that the file is damaged.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
    const QByteArray content =
        QByteArray("[customPapers]\n1\\name=Regi\n1\\widthMm=100\n1\\heightMm=150\n");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content), qint64(content.size()));
    }

    CustomPaperStore store(path);
    const auto read = store.readAll();
    QVERIFY2(!read.ok(), "a damaged file must not look like an empty store");
    QCOMPARE(read.error().code, QStringLiteral("paper-corrupt"));
    // The plain accessor still answers, so the read-only callers keep working.
    QVERIFY(store.all().isEmpty());

    PaperSpec paper;
    paper.name = QStringLiteral("Új");
    paper.physicalWidthMm = 100.0;
    paper.physicalHeightMm = 150.0;
    const auto saved = store.save(paper);
    QVERIFY2(!saved.ok(), "saving over a damaged file would lose the old papers");
    QCOMPARE(saved.error().code, QStringLiteral("paper-corrupt"));

    const auto removed = store.remove(QStringLiteral("Regi"));
    QVERIFY2(!removed.ok(), "removing from a damaged file would rewrite it too");
    QCOMPARE(removed.error().code, QStringLiteral("paper-corrupt"));

    // The damage stays on disk, so the user or a later repair can still get
    // the papers back.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), content);
}

void TestPaperCatalog::unparsablePaperFileKeepsItsReadablePaper()
{
    // A file the INI parser rejects outright (a section header that was never
    // closed). The first read in a process reports the damage. QSettings keeps
    // the parse in memory afterwards, so a later read of the same file can see
    // the complete paper it did manage to read; refusing that paper, or writing
    // it away, is what must never happen.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[customPapers\nsize=1\n1\\name=Regi\n1\\widthMm=100\n1\\heightMm=150\n");
    }

    CustomPaperStore store(path);
    const auto read = store.readAll();
    QVERIFY(!read.ok());
    QCOMPARE(read.error().code, QStringLiteral("paper-corrupt"));

    PaperSpec paper;
    paper.name = QStringLiteral("Új");
    paper.physicalWidthMm = 100.0;
    paper.physicalHeightMm = 150.0;
    // Either the save refuses the file or it repairs it; both keep what the file
    // still holds in full.
    const auto saved = store.save(paper);
    if (!saved.ok())
        QCOMPARE(saved.error().code, QStringLiteral("paper-corrupt"));

    const QStringList names = paperNames(store.all());
    QVERIFY2(names.contains(QStringLiteral("Regi")), "the readable paper must survive");
    if (saved.ok())
        QVERIFY(names.contains(QStringLiteral("Új")));
}

void TestPaperCatalog::corruptPaperEntryIsNotDropped()
{
    // Both files still claim an entry the app would never write: a truncated
    // second paper that lost its name, and a zero width. Skipping either one and
    // returning success would let the next save() rewrite the file from the
    // shorter list, and the paper would be gone without a word.
    const QList<QByteArray> damaged = {
        QByteArray("[customPapers]\nsize=2\n1\\name=Regi\n1\\widthMm=100\n1\\heightMm=150\n"
                   "2\\widthMm=100\n2\\heightMm=150\n"),
        QByteArray("[customPapers]\nsize=1\n1\\name=Regi\n1\\widthMm=0\n1\\heightMm=150\n"),
        QByteArray("[customPapers]\nsize=1\n1\\name=Regi\n1\\widthMm=100\n"),
    };

    for (const QByteArray &content : damaged) {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(content), qint64(content.size()));
        }

        CustomPaperStore store(path);
        const auto read = store.readAll();
        QVERIFY2(!read.ok(), "a half-written entry is damage, not an empty slot");
        QCOMPARE(read.error().code, QStringLiteral("paper-corrupt"));
        QVERIFY(store.all().isEmpty());

        PaperSpec paper;
        paper.name = QStringLiteral("Új");
        paper.physicalWidthMm = 100.0;
        paper.physicalHeightMm = 150.0;
        const auto saved = store.save(paper);
        QVERIFY2(!saved.ok(), "saving over an entry the app cannot read loses it");
        QCOMPARE(saved.error().code, QStringLiteral("paper-corrupt"));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), content);
    }
}

void TestPaperCatalog::nonFiniteSizesAreRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
    CustomPaperStore store(path);

    PaperSpec paper;
    paper.name = QStringLiteral("Végtelen");
    paper.physicalWidthMm = std::numeric_limits<double>::infinity();
    paper.physicalHeightMm = 150.0;
    const auto infiniteWidth = store.save(paper);
    QVERIFY2(!infiniteWidth.ok(), "+inf passes a bare > 0 test, so it needs its own refusal");
    QCOMPARE(infiniteWidth.error().code, QStringLiteral("paper-size"));

    paper.physicalWidthMm = 100.0;
    paper.physicalHeightMm = std::numeric_limits<double>::quiet_NaN();
    const auto nanHeight = store.save(paper);
    QVERIFY(!nanHeight.ok());
    QCOMPARE(nanHeight.error().code, QStringLiteral("paper-size"));

    paper.physicalHeightMm = 150.0;
    paper.logicalWidthMm = std::numeric_limits<double>::infinity();
    paper.logicalHeightMm = 152.0;
    const auto infiniteLogical = store.save(paper);
    QVERIFY(!infiniteLogical.ok());
    QCOMPARE(infiniteLogical.error().code, QStringLiteral("paper-size"));

    paper.logicalWidthMm = 101.6;
    paper.logicalHeightMm = -1.0;
    const auto negativeLogical = store.save(paper);
    QVERIFY(!negativeLogical.ok());
    QCOMPARE(negativeLogical.error().code, QStringLiteral("paper-size"));

    QVERIFY(store.all().isEmpty());

    // The same refusal on the read side: a stored infinite size is not a paper.
    const QString corruptPath = QDir(dir.path()).filePath(QStringLiteral("corrupt.ini"));
    QFile file(corruptPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("[customPapers]\nsize=1\n1\\name=Regi\n1\\widthMm=inf\n1\\heightMm=150\n");
    file.close();

    CustomPaperStore corrupted(corruptPath);
    const auto read = corrupted.readAll();
    QVERIFY(!read.ok());
    QCOMPARE(read.error().code, QStringLiteral("paper-corrupt"));
}

void TestPaperCatalog::settingsFailureIsNotSticky()
{
    // QSettings::status() stays set for the lifetime of the object, and the app
    // keeps one store for its whole run, so a single transient failure would
    // otherwise block every later save until a restart. A directory in the
    // file's place makes the settings object fail without any permission games.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
    QVERIFY(QDir().mkpath(path));

    CustomPaperStore store(path);
    PaperSpec paper;
    paper.name = QStringLiteral("Újra");
    paper.physicalWidthMm = 120.0;
    paper.physicalHeightMm = 180.0;

    const auto blocked = store.save(paper);
    QVERIFY(!blocked.ok());
    QCOMPARE(blocked.error().code, QStringLiteral("paper-read"));

    QVERIFY(QDir(path).removeRecursively());
    const auto recovered = store.save(paper);
    QVERIFY2(recovered.ok(), "the store must recover once the file can be written");
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().name, paper.name);
}

void TestPaperCatalog::aConcurrentWriterIsRefused()
{
    // save() and remove() rewrite the whole array from a fresh read. Two writers
    // doing that at once lose one of the papers, so a held lock must be reported
    // instead of being ignored.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("papers.ini"));
    CustomPaperStore store(path);

    PaperSpec paper;
    paper.name = QStringLiteral("Verseny");
    paper.physicalWidthMm = 100.0;
    paper.physicalHeightMm = 150.0;

    QLockFile lock(path + QStringLiteral(".papers.lock"));
    QVERIFY(lock.tryLock(1000));

    const auto blockedSave = store.save(paper);
    QVERIFY(!blockedSave.ok());
    QCOMPARE(blockedSave.error().code, QStringLiteral("paper-lock"));

    const auto blockedRemove = store.remove(paper.name);
    QVERIFY(!blockedRemove.ok());
    QCOMPARE(blockedRemove.error().code, QStringLiteral("paper-lock"));
    QVERIFY(store.all().isEmpty());

    lock.unlock();
    QVERIFY(store.save(paper).ok());
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
    preset.context.physicalWidthMm = 100.0;
    preset.context.physicalHeightMm = 150.0;
    preset.context.logicalWidthMm = 101.6;
    preset.context.logicalHeightMm = 152.4;
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

void TestPaperCatalog::corruptPresetEntriesAreRefused()
{
    // Each of these entries would have been read as a preset with an empty
    // context, and an empty context matches a caller whose context was never
    // filled in. That is a paper correction applied to a sheet that was never
    // measured, so the entry is damage and the whole file is refused.
    const QList<QByteArray> damaged = {
        QByteArray(R"([{"paperEv":0.05}])"),
        QByteArray(R"([{"id":"a"}])"),
        QByteArray(R"([{"id":"a","context":[]}])"),
        QByteArray(R"([{"id":"","context":{"queue":"DCPT730DW","physicalWidthMm":100,)"
                   R"("physicalHeightMm":150,"logicalWidthMm":101.6,"logicalHeightMm":152.4}}])"),
        QByteArray(R"([{"id":"a","context":{"physicalWidthMm":100,)"
                   R"("physicalHeightMm":150,"logicalWidthMm":101.6,"logicalHeightMm":152.4}}])"),
        QByteArray(R"([{"id":"a","context":{"queue":"DCPT730DW","physicalWidthMm":100,)"
                   R"("physicalHeightMm":150,"logicalWidthMm":101.6,"logicalHeightMm":152.4}},null])"),
    };

    for (const QByteArray &content : damaged) {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("presets.json"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(content), qint64(content.size()));
        }

        PaperPresetStore presets(path);
        const auto read = presets.readAll();
        QVERIFY2(!read.ok(), content.constData());
        QCOMPARE(read.error().code, QStringLiteral("preset-corrupt"));
        // The plain accessor answers with nothing, so an unusable preset can
        // never be matched against an unfilled context.
        QVERIFY(presets.all().isEmpty());
        QVERIFY(!presets.match(PrintContext{}).has_value());

        PaperPreset preset;
        preset.id = QStringLiteral("b");
        preset.context.queueName = QStringLiteral("DCPT730DW");
        preset.context.physicalWidthMm = 100.0;
        preset.context.physicalHeightMm = 150.0;
        preset.context.logicalWidthMm = 101.6;
        preset.context.logicalHeightMm = 152.4;
        const auto saved = presets.save(preset);
        QVERIFY2(!saved.ok(), "a valid preset must not repair a damaged file");
        QCOMPARE(saved.error().code, QStringLiteral("preset-corrupt"));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), content);
    }

    // A well-formed entry is still read and matched, so the refusal is about the
    // damage and not about refusing presets in general.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("presets.json"));
    PaperPresetStore presets(path);

    // And a preset without an environment is refused on the way in: writing it
    // would make the store read its own file as damaged on the next start.
    PaperPreset unusable;
    unusable.id = QStringLiteral("d");
    const auto refused = presets.save(unusable);
    QVERIFY(!refused.ok());
    QCOMPARE(refused.error().code, QStringLiteral("preset-context"));
    QVERIFY(presets.readAll().ok());

    PrintContext context;
    context.queueName = QStringLiteral("DCPT730DW");
    context.driverIdentity = QStringLiteral("brother-native");
    context.physicalWidthMm = 100.0;
    context.physicalHeightMm = 150.0;
    context.logicalWidthMm = 101.6;
    context.logicalHeightMm = 152.4;
    PaperPreset good;
    good.id = QStringLiteral("c");
    good.context = context;
    good.paperEv = 0.06;
    QVERIFY(presets.save(good).ok());
    const auto match = presets.match(context);
    QVERIFY(match.has_value());
    QCOMPARE(match->paperEv, 0.06);
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
