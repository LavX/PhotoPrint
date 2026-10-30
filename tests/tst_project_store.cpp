#include "Document.h"
#include "ImageSource.h"
#include "PageRenderer.h"
#include "ProjectStore.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace pp;

namespace {

QImage solid(const QColor &colour)
{
    QImage image(240, 160, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
}

bool writeImage(const QString &path, const QColor &colour)
{
    QImageWriter writer(path, "png");
    return writer.write(solid(colour));
}

Frame sampleFrame(const SourceImage &source, const QString &id)
{
    Frame frame;
    frame.id = id;
    frame.sourcePath = source.path;
    frame.sourceSha256 = source.sha256;
    frame.cx = 55.25;
    frame.cy = 77.5;
    frame.width = 60.5;
    frame.height = 40.25;
    frame.angleDeg = 17.0;
    frame.imageRotationDeg = 90.0;
    frame.zoom = 1.7;
    frame.cropCenter = QPointF(0.3, 0.7);
    frame.fit = FitMode::Fill;
    frame.autoPlaced = false;
    frame.tone.exposureEv = 0.21;
    frame.tone.shadowLift = 0.04;
    frame.tone.highlightCompress = 0.02;
    frame.autoTone = true;
    frame.brightness = -0.05;
    frame.filter = QStringLiteral("vintage");
    frame.filterStrength = 64;
    frame.upscale = true;
    return frame;
}

DocumentSnapshot sampleDocument(const SourceImage &red, const SourceImage &blue)
{
    DocumentSnapshot document;
    document.recipe.autoEnhance = true;
    document.recipe.tone.exposureEv = 0.12;
    document.recipe.tone.shadowLift = 0.2;
    document.recipe.tone.highlightCompress = 0.05;
    document.recipe.paperStrength = 0.1;
    document.recipe.grayscale = true;
    document.fineQuality = false;

    Page one;
    one.id = QStringLiteral("page-one");
    one.paper.name = QStringLiteral("10 × 15 cm");
    one.paper.physicalWidthMm = 100.0;
    one.paper.physicalHeightMm = 150.0;
    one.paper.logicalWidthMm = 101.6;
    one.paper.logicalHeightMm = 152.4;
    one.paper.type = PaperType::Matte;
    one.frames.append(sampleFrame(red, QStringLiteral("frame-red")));
    document.pages.append(one);

    Page two = one;
    two.id = QStringLiteral("page-two");
    two.landscape = true;
    two.paper.name = QStringLiteral("12 × 18 cm (egyedi)");
    two.paper.physicalWidthMm = 120.0;
    two.paper.physicalHeightMm = 180.0;
    two.paper.logicalWidthMm = 0.0;
    two.paper.logicalHeightMm = 0.0;
    two.paper.type = PaperType::Glossy;
    two.paper.custom = true;
    two.frames = {sampleFrame(blue, QStringLiteral("frame-blue"))};
    document.pages.append(two);
    document.currentPage = 1;
    return document;
}

DocumentSnapshot singleFrameDocument(const SourceImage &source, const QString &id)
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-single");
    page.frames.append(sampleFrame(source, id));
    document.pages.append(page);
    return document;
}

bool closeFrame(const Frame &a, const Frame &b)
{
    return a.id == b.id && a.sourcePath == b.sourcePath && a.sourceSha256 == b.sourceSha256
        && std::abs(a.cx - b.cx) <= 0.001 && std::abs(a.cy - b.cy) <= 0.001
        && std::abs(a.width - b.width) <= 0.001 && std::abs(a.height - b.height) <= 0.001
        && std::abs(a.angleDeg - b.angleDeg) <= 0.001
        && std::abs(a.imageRotationDeg - b.imageRotationDeg) <= 0.001
        && std::abs(a.zoom - b.zoom) <= 0.0001
        && std::abs(a.cropCenter.x() - b.cropCenter.x()) <= 0.0001
        && std::abs(a.cropCenter.y() - b.cropCenter.y()) <= 0.0001 && a.fit == b.fit
        && a.autoPlaced == b.autoPlaced && a.tone == b.tone && a.autoTone == b.autoTone
        && std::abs(a.brightness - b.brightness) <= 1e-6 && a.filter == b.filter
        && a.filterStrength == b.filterStrength && a.upscale == b.upscale
        && a.label == b.label && a.paperOverride == b.paperOverride
        && std::abs(a.paperEv - b.paperEv) <= 1e-9;
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeJson(const QString &path, const QJsonObject &root)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray data = QJsonDocument(root).toJson();
    return file.write(data) == data.size();
}

/// Points the first frame of the first page at another stored path, exactly
/// what a hand-edited or hostile project file can do.
bool patchFirstFrame(const QString &path, const QString &source, bool keepOriginal)
{
    QJsonObject root = readJson(path);
    QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    QJsonObject page = pages.at(0).toObject();
    QJsonArray frames = page.value(QStringLiteral("frames")).toArray();
    QJsonObject frame = frames.at(0).toObject();
    frame.insert(QStringLiteral("source"), source);
    if (!keepOriginal)
        frame.remove(QStringLiteral("original"));
    frames.replace(0, frame);
    page.insert(QStringLiteral("frames"), frames);
    pages.replace(0, page);
    root.insert(QStringLiteral("pages"), pages);
    return writeJson(path, root);
}

/// Rewrites the recorded identity of the first frame, or removes it when the
/// hash is empty: a project without an identity cannot be verified at all.
bool patchFirstFrameSha(const QString &path, const QByteArray &sha256)
{
    QJsonObject root = readJson(path);
    QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    QJsonObject page = pages.at(0).toObject();
    QJsonArray frames = page.value(QStringLiteral("frames")).toArray();
    QJsonObject frame = frames.at(0).toObject();
    if (sha256.isEmpty())
        frame.remove(QStringLiteral("sha256"));
    else
        frame.insert(QStringLiteral("sha256"), QString::fromLatin1(sha256.toHex()));
    frames.replace(0, frame);
    page.insert(QStringLiteral("frames"), frames);
    pages.replace(0, page);
    root.insert(QStringLiteral("pages"), pages);
    return writeJson(path, root);
}

/// Rewrites the recorded absolute `original` of the first frame, the other
/// half of what a hand-edited project file can do.
bool patchFirstFrameOriginal(const QString &path, const QString &original)
{
    QJsonObject root = readJson(path);
    QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    QJsonObject page = pages.at(0).toObject();
    QJsonArray frames = page.value(QStringLiteral("frames")).toArray();
    QJsonObject frame = frames.at(0).toObject();
    if (original.isEmpty())
        frame.remove(QStringLiteral("original"));
    else
        frame.insert(QStringLiteral("original"), original);
    frames.replace(0, frame);
    page.insert(QStringLiteral("frames"), frames);
    pages.replace(0, page);
    root.insert(QStringLiteral("pages"), pages);
    return writeJson(path, root);
}

} // namespace

class TestProjectStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void roundTripKeepsEveryTransform();
    void emptyDocumentIsNotSaved();
    void zeroPageProjectIsRejected();
    void escapingSourceIsNotRead();
    void blockedRelativeSourceKeepsRelinkInformation();
    void absoluteSourceIsRefused();
    void absoluteOriginalKeepsNormalProjectsWorking();
    void absoluteOriginalMustMatchTheRecordedPhoto();
    void portableCopyDoesNotStoreAbsolutePaths();
    void portableBundleSymlinkIsRefused();
    void portableCopyKeepsSameNamedSourcesApart();
    void portableCopyRollsBackOnFailure();
    void portableCopyReplacementRollsBack();
    void portableCopyReplacementLeavesNoBackup();
    void portableGeneratedNamesNeverShadowPlainNames();
    void portableAnonymousNamesAvoidPlainNames();
    void portableBundleCanBeResaved();
    void craftedBrightnessCannotReachTheRenderer();
    void craftedPaperToneIsClamped();
    void craftedRecipeCannotReachThePrintPath();
    void versionOneProjectWithoutNewKeysStillLoads();
    void missingAndChangedSourcesKeepTheLayout();
    void portableCopyWorksAfterMoving();
    void corruptAndUnknownProjectsAreRejected();
    void changedSourceStopsPrintPreparation();

private:
    QTemporaryDir m_dir;
    QString m_redPath;
    QString m_bluePath;
    QByteArray m_redSha;
    QByteArray m_blueSha;
    SourceImage m_red;
    SourceImage m_blue;
};

void TestProjectStore::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_redPath = m_dir.filePath(QStringLiteral("red.png"));
    m_bluePath = m_dir.filePath(QStringLiteral("blue.png"));
    QImageWriter redWriter(m_redPath, "png");
    QVERIFY(redWriter.write(solid(QColor(200, 50, 40))));
    QImageWriter blueWriter(m_bluePath, "png");
    QVERIFY(blueWriter.write(solid(QColor(30, 60, 200))));
    m_redSha = fileSha256(m_redPath);
    m_blueSha = fileSha256(m_bluePath);
    QVERIFY(QFile::copy(m_redPath, m_dir.filePath(QStringLiteral("red-original.png"))));
    QVERIFY(QFile::copy(m_bluePath, m_dir.filePath(QStringLiteral("blue-original.png"))));

    ImageStore store;
    const auto red = store.load(m_redPath, ImportPolicy{});
    const auto blue = store.load(m_bluePath, ImportPolicy{});
    QVERIFY(red.ok());
    QVERIFY(blue.ok());
    m_red = red.value();
    m_blue = blue.value();
}

void TestProjectStore::roundTripKeepsEveryTransform()
{
    DocumentSnapshot original = sampleDocument(m_red, m_blue);
    // Trial-sheet fields: a numbered variant with its own paper tone.
    original.pages[0].frames[0].label = QStringLiteral("3");
    original.pages[0].frames[0].paperOverride = true;
    original.pages[0].frames[0].paperEv = 0.07;
    original.pages[1].frames[0].label = QStringLiteral("A");
    original.pages[1].frames[0].paperEv = -0.03;
    const QString path = m_dir.filePath(QStringLiteral("project.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, original, SaveMode::Normal);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    const DocumentSnapshot restored = loaded.value().document;

    QCOMPARE(restored.pages.size(), original.pages.size());
    QCOMPARE(restored.currentPage, original.currentPage);
    QCOMPARE(restored.recipe, original.recipe);
    QVERIFY(restored.recipe.grayscale);
    QCOMPARE(restored.fineQuality, original.fineQuality);
    QVERIFY(!restored.fineQuality);
    QCOMPARE(restored.pages.at(0).paper.type, PaperType::Matte);
    QCOMPARE(restored.pages.at(1).paper.type, PaperType::Glossy);
    QVERIFY(!restored.pages.at(0).paper.custom);
    QVERIFY(restored.pages.at(1).paper.custom);
    QCOMPARE(restored.pages.at(1).paper.name, QStringLiteral("12 × 18 cm (egyedi)"));
    for (int p = 0; p < original.pages.size(); ++p) {
        QCOMPARE(restored.pages.at(p).id, original.pages.at(p).id);
        QCOMPARE(restored.pages.at(p).paper, original.pages.at(p).paper);
        QCOMPARE(restored.pages.at(p).landscape, original.pages.at(p).landscape);
        QCOMPARE(restored.pages.at(p).frames.size(), original.pages.at(p).frames.size());
        for (int f = 0; f < original.pages.at(p).frames.size(); ++f) {
            QVERIFY(closeFrame(restored.pages.at(p).frames.at(f),
                               original.pages.at(p).frames.at(f)));
        }
    }
    QCOMPARE(fileSha256(m_redPath), m_redSha);
    QCOMPARE(fileSha256(m_bluePath), m_blueSha);
}

void TestProjectStore::emptyDocumentIsNotSaved()
{
    const QString path = m_dir.filePath(QStringLiteral("empty.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, DocumentSnapshot{}, SaveMode::Normal);
    QVERIFY(!saved.ok());
    QCOMPARE(saved.error().code, QStringLiteral("project-empty"));
    QVERIFY(!QFileInfo::exists(path));
}

void TestProjectStore::zeroPageProjectIsRejected()
{
    const QString path = m_dir.filePath(QStringLiteral("zero.ppj"));
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("photoprint-project"));
    root.insert(QStringLiteral("version"), ProjectStore::kFormatVersion);
    root.insert(QStringLiteral("pages"), QJsonArray());
    QVERIFY(writeJson(path, root));

    ProjectStore store;
    const auto loaded = store.load(path);
    QVERIFY(!loaded.ok());
    QCOMPARE(loaded.error().code, QStringLiteral("project-empty"));
}

void TestProjectStore::escapingSourceIsNotRead()
{
    // A secret outside the project folder, with a perfectly valid image in the
    // project so the document itself is not the reason for any failure.
    const QString secret = m_dir.filePath(QStringLiteral("secret.txt"));
    QFile secretFile(secret);
    QVERIFY(secretFile.open(QIODevice::WriteOnly));
    secretFile.write("not a photo");
    secretFile.close();

    const QString projectDir = m_dir.filePath(QStringLiteral("traversal"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("t.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Normal).ok());

    const QString escaping = QDir(projectDir).relativeFilePath(secret);
    QVERIFY(escaping.startsWith(QStringLiteral("..")));
    QVERIFY(patchFirstFrame(path, escaping, false));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    const Frame frame = loaded.value().document.pages.at(0).frames.at(0);
    QVERIFY2(frame.sourcePath.isEmpty(), qPrintable(frame.sourcePath));
    QCOMPARE(loaded.value().issues.size(), 1);
    QVERIFY(loaded.value().issues.first().missing);
    QVERIFY(loaded.value().issues.first().blocked);
    QVERIFY(!loaded.value().issues.first().path.contains(QStringLiteral("secret")));
}

void TestProjectStore::blockedRelativeSourceKeepsRelinkInformation()
{
    // A frame whose stored path escapes and whose recorded `original` is gone
    // keeps no usable path. The layout is still restored, so the refusal has to
    // leave something a relink dialog can start from, and the photo must not be
    // read through the refused path.
    const QString projectDir = m_dir.filePath(QStringLiteral("relink"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("r.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Normal).ok());

    const QString elsewhere = m_dir.filePath(QStringLiteral("relink-elsewhere.png"));
    QVERIFY(QFile::copy(m_redPath, elsewhere));
    const QString escaping = QDir(projectDir).relativeFilePath(elsewhere);
    QVERIFY(escaping.startsWith(QStringLiteral("..")));
    QVERIFY(patchFirstFrame(path, escaping, false));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    const Frame frame = loaded.value().document.pages.at(0).frames.at(0);
    QVERIFY(frame.sourcePath.isEmpty());
    QCOMPARE(loaded.value().issues.size(), 1);

    const SourceIssue issue = loaded.value().issues.first();
    QCOMPARE(issue.frameId, QStringLiteral("frame-red"));
    QVERIFY(issue.missing);
    QVERIFY(issue.blocked);
    QVERIFY(issue.path.isEmpty());
    // The hint is the cleaned stored path, not a resolved or verified file.
    QCOMPARE(issue.refusedPath, QDir::cleanPath(m_dir.filePath(QStringLiteral("relink-elsewhere.png"))));
}

void TestProjectStore::absoluteSourceIsRefused()
{
    // An absolute `source` is never written by this application, so a project
    // carrying one is hand-edited or hostile. It may name any file on the
    // machine, so it must not be opened, hashed or rendered.
    const QString projectDir = m_dir.filePath(QStringLiteral("absolute-source"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("a.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Normal).ok());

    const QString absolute = QFileInfo(m_redPath).absoluteFilePath();
    QVERIFY(QDir::isAbsolutePath(absolute));
    QVERIFY(patchFirstFrame(path, absolute, false));

    {
        const auto loaded = store.load(path);
        if (!loaded)
            QFAIL(qPrintable(loaded.error().message));
        QCOMPARE(loaded.value().document.pages.at(0).frames.at(0).sourcePath, QString());
        QCOMPARE(loaded.value().issues.size(), 1);
        const SourceIssue issue = loaded.value().issues.first();
        QCOMPARE(issue.frameId, QStringLiteral("frame-red"));
        QVERIFY(issue.missing);
        QVERIFY(issue.blocked);
        QVERIFY(issue.path.isEmpty());
        // The refusal is still usable as a relink hint, but it is only text.
        QCOMPARE(issue.refusedPath, QDir::cleanPath(absolute));
    }

    // The legitimate way to reach a photo outside the folder is `original`,
    // and only that: the absolute `source` must not win even when it names an
    // existing, valid photo of its own.
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Normal).ok());
    QVERIFY(patchFirstFrame(path, QFileInfo(m_bluePath).absoluteFilePath(), true));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    const Frame frame = loaded.value().document.pages.at(0).frames.at(0);
    QCOMPARE(frame.sourcePath, QFileInfo(m_redPath).absoluteFilePath());
    QCOMPARE(frame.sourceSha256, m_redSha);
    QVERIFY(frame.sourcePath != QFileInfo(m_bluePath).absoluteFilePath());
    // Resolved through `original`: this is the 1.0.0 layout and it is healthy.
    QVERIFY(loaded.value().issues.isEmpty());
}

void TestProjectStore::absoluteOriginalKeepsNormalProjectsWorking()
{
    // 1.0.0 wrote the relative path even when the photo lived outside the
    // project folder. That layout must keep opening on the author's machine.
    const QString secret = m_dir.filePath(QStringLiteral("secret2.txt"));
    QFile secretFile(secret);
    QVERIFY(secretFile.open(QIODevice::WriteOnly));
    secretFile.write("not a photo either");
    secretFile.close();

    const QString projectDir = m_dir.filePath(QStringLiteral("normal"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("n.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Normal).ok());

    const QString escaping = QDir(projectDir).relativeFilePath(secret);
    QVERIFY(patchFirstFrame(path, escaping, true));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    const Frame frame = loaded.value().document.pages.at(0).frames.at(0);
    QCOMPARE(frame.sourcePath, QFileInfo(m_redPath).absoluteFilePath());
    QVERIFY(loaded.value().issues.isEmpty());
}

void TestProjectStore::absoluteOriginalMustMatchTheRecordedPhoto()
{
    // `original` is the legacy way back to a photo outside the project folder,
    // but it arrives from whoever wrote the file. Adopted verbatim, an
    // untrusted project could name any local file for this app to stat, hash
    // and render. It is followed only while the file is still the recorded
    // photo: a regular file whose hash equals the frame's stored identity.
    const QString projectDir = m_dir.filePath(QStringLiteral("original-guard"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("g.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Normal).ok());

    // A 1.0.0 shape: the relative source does not resolve, so the absolute
    // original decides. The frame's recorded identity is the red photo.
    const QString secret = m_dir.filePath(QStringLiteral("secret4.txt"));
    QFile secretFile(secret);
    QVERIFY(secretFile.open(QIODevice::WriteOnly));
    secretFile.write("still not a photo");
    secretFile.close();
    const QString escaping = QDir(projectDir).relativeFilePath(secret);
    QVERIFY(escaping.startsWith(QStringLiteral("..")));
    QVERIFY(patchFirstFrame(path, escaping, false));

    // Another real photo at the recorded path is not the recorded photo.
    QVERIFY(patchFirstFrameOriginal(path, QFileInfo(m_bluePath).absoluteFilePath()));
    {
        const auto loaded = store.load(path);
        if (!loaded)
            QFAIL(qPrintable(loaded.error().message));
        const Frame frame = loaded.value().document.pages.at(0).frames.at(0);
        QVERIFY2(frame.sourcePath.isEmpty(), "an unverified local file must not be rendered");
        QCOMPARE(loaded.value().issues.size(), 1);
        QVERIFY(loaded.value().issues.first().missing);
    }

    // Neither is a path that no longer exists.
    QVERIFY(patchFirstFrameOriginal(path, m_dir.filePath(QStringLiteral("gone.png"))));
    {
        const auto loaded = store.load(path);
        if (!loaded)
            QFAIL(qPrintable(loaded.error().message));
        QVERIFY(loaded.value().document.pages.at(0).frames.at(0).sourcePath.isEmpty());
        QCOMPARE(loaded.value().issues.size(), 1);
        QVERIFY(loaded.value().issues.first().missing);
    }

    // Nor a device: not a regular file, and reading it could block the load.
    if (QFileInfo::exists(QStringLiteral("/dev/null"))) {
        QVERIFY(patchFirstFrameOriginal(path, QStringLiteral("/dev/null")));
        const auto loaded = store.load(path);
        if (!loaded)
            QFAIL(qPrintable(loaded.error().message));
        QVERIFY(loaded.value().document.pages.at(0).frames.at(0).sourcePath.isEmpty());
    }

    // A frame with no recorded identity cannot be verified at all, so not even
    // the recorded photo at the recorded path is adopted: it is missing, and
    // the relink path handles it. The file is never read or hashed.
    QVERIFY(patchFirstFrameSha(path, QByteArray()));
    QVERIFY(patchFirstFrameOriginal(path, QFileInfo(m_redPath).absoluteFilePath()));
    {
        const auto loaded = store.load(path);
        if (!loaded)
            QFAIL(qPrintable(loaded.error().message));
        const Frame frame = loaded.value().document.pages.at(0).frames.at(0);
        QVERIFY2(frame.sourcePath.isEmpty(), "an unverifiable original must not be adopted");
        QCOMPARE(loaded.value().issues.size(), 1);
        QVERIFY(loaded.value().issues.first().missing);
        QVERIFY(!loaded.value().issues.first().changed);
    }

    // The recorded photo at the recorded path still opens: that is the layout
    // 1.0.0 wrote and it must keep working.
    QVERIFY(patchFirstFrameSha(path, m_redSha));
    QVERIFY(patchFirstFrameOriginal(path, QFileInfo(m_redPath).absoluteFilePath()));
    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    const Frame frame = loaded.value().document.pages.at(0).frames.at(0);
    QCOMPARE(frame.sourcePath, QFileInfo(m_redPath).absoluteFilePath());
    QVERIFY(loaded.value().issues.isEmpty());
}

void TestProjectStore::portableCopyDoesNotStoreAbsolutePaths()
{
    const QString projectDir = m_dir.filePath(QStringLiteral("privacy"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("portable.ppj"));

    ProjectStore store;
    const auto saved = store.save(path, sampleDocument(m_red, m_blue), SaveMode::Portable);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    // Nothing may reveal where the author keeps the photos.
    QVERIFY(!text.contains(m_dir.path()));
    QVERIFY(!text.contains(m_redPath));
    QVERIFY(!text.contains(QStringLiteral("\"original\"")));

    const QJsonObject root = readJson(path);
    for (const QJsonValue &pageValue : root.value(QStringLiteral("pages")).toArray()) {
        for (const QJsonValue &frameValue : pageValue.toObject().value(QStringLiteral("frames")).toArray()) {
            const QString stored = frameValue.toObject().value(QStringLiteral("source")).toString();
            QVERIFY2(stored.contains(QStringLiteral(".files/")), qPrintable(stored));
            QVERIFY(!QDir::isAbsolutePath(stored));
        }
    }
}

void TestProjectStore::portableBundleSymlinkIsRefused()
{
    // The containment rule for the copies is lexical, so a `.files` entry that
    // is a symlink passes it and still writes into a directory the project
    // does not own. The bundle root is resolved canonically before anything is
    // copied, and a link is refused.
    const QString projectDir = m_dir.filePath(QStringLiteral("symlink-bundle"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("s.ppj"));
    const QString outside = m_dir.filePath(QStringLiteral("outside-target"));
    QVERIFY(QDir().mkpath(outside));

    const QString bundle = path + QStringLiteral(".files");
    // QFile::link() makes a symbolic link on Unix and a .lnk shortcut on
    // Windows, and a file system may refuse the link altogether. The test is
    // about the link, so there is nothing to check when it cannot be made.
    if (!QFile::link(outside, bundle) || !QFileInfo(bundle).isSymLink())
        QSKIP("symbolic links are not available here");
    QVERIFY(QFileInfo(bundle).isSymLink());

    ProjectStore store;
    const auto saved = store.save(path, sampleDocument(m_red, m_blue), SaveMode::Portable);
    QVERIFY2(!saved.ok(), "a symlinked bundle folder would receive the photo copies");
    QCOMPARE(saved.error().code, QStringLiteral("portable-dir"));

    // Nothing was copied through the link, the link is untouched and no
    // half-written project was left behind.
    const QStringList leaked =
        QDir(outside).entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                                | QDir::NoDotAndDotDot);
    QVERIFY2(leaked.isEmpty(), qPrintable(leaked.join(QLatin1Char(','))));
    QVERIFY(QFileInfo(bundle).isSymLink());
    QVERIFY(!QFileInfo::exists(path));

    // A real bundle folder in the same place still works.
    QVERIFY(QFile::remove(bundle));
    QVERIFY(QDir().mkpath(bundle));
    const auto again = store.save(path, sampleDocument(m_red, m_blue), SaveMode::Portable);
    if (!again)
        QFAIL(qPrintable(again.error().message));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(!QDir(bundle)
                 .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
                 .isEmpty());
}

void TestProjectStore::portableCopyKeepsSameNamedSourcesApart()
{
    const QString oneDir = m_dir.filePath(QStringLiteral("one"));
    const QString twoDir = m_dir.filePath(QStringLiteral("two"));
    QVERIFY(QDir().mkpath(oneDir));
    QVERIFY(QDir().mkpath(twoDir));
    const QString onePath = QDir(oneDir).filePath(QStringLiteral("photo.png"));
    const QString twoPath = QDir(twoDir).filePath(QStringLiteral("photo.png"));
    QVERIFY(QFile::copy(m_redPath, onePath));
    QVERIFY(QFile::copy(m_bluePath, twoPath));

    ImageStore images;
    const auto one = images.load(onePath, ImportPolicy{});
    const auto two = images.load(twoPath, ImportPolicy{});
    QVERIFY(one.ok());
    QVERIFY(two.ok());
    QVERIFY(one.value().sha256 != two.value().sha256);

    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page");
    page.frames.append(sampleFrame(one.value(), QStringLiteral("frame-one")));
    page.frames.append(sampleFrame(two.value(), QStringLiteral("frame-two")));
    document.pages.append(page);

    const QString originDir = m_dir.filePath(QStringLiteral("collision"));
    QVERIFY(QDir().mkpath(originDir));
    const QString path = QDir(originDir).filePath(QStringLiteral("portable.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, document, SaveMode::Portable);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));

    const QString filesDir = path + QStringLiteral(".files");
    QCOMPARE(QDir(filesDir).entryList(QDir::Files).size(), 2);

    const QString movedDir = m_dir.filePath(QStringLiteral("collision-moved"));
    QVERIFY(QDir().rename(originDir, movedDir));
    const auto loaded = store.load(QDir(movedDir).filePath(QStringLiteral("portable.ppj")));
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QVERIFY(loaded.value().issues.isEmpty());

    const auto &frames = loaded.value().document.pages.at(0).frames;
    QCOMPARE(frames.size(), 2);
    QVERIFY(QFileInfo::exists(frames.at(0).sourcePath));
    QVERIFY(QFileInfo::exists(frames.at(1).sourcePath));
    QVERIFY(frames.at(0).sourcePath != frames.at(1).sourcePath);
    QCOMPARE(fileSha256(frames.at(0).sourcePath), one.value().sha256);
    QCOMPARE(fileSha256(frames.at(1).sourcePath), two.value().sha256);
}

void TestProjectStore::portableCopyRollsBackOnFailure()
{
    DocumentSnapshot document = sampleDocument(m_red, m_blue);
    Frame broken = document.pages.at(1).frames.at(0);
    broken.id = QStringLiteral("frame-broken");
    broken.sourcePath = m_dir.filePath(QStringLiteral("no-such-photo.png"));
    document.pages[1].frames.append(broken);

    const QString projectDir = m_dir.filePath(QStringLiteral("rollback"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("r.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, document, SaveMode::Portable);
    QVERIFY(!saved.ok());
    QCOMPARE(saved.error().code, QStringLiteral("source-missing"));
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(!QDir(path + QStringLiteral(".files")).exists());
}

void TestProjectStore::portableCopyReplacementRollsBack()
{
    const QString projectDir = m_dir.filePath(QStringLiteral("replace-rollback"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("p.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Portable).ok());

    const QString filesDir = path + QStringLiteral(".files");
    const QString carried = QDir(filesDir).filePath(QStringLiteral("red.png"));
    QCOMPARE(fileSha256(carried), m_redSha);

    // Another photo under the same base name would replace the carried copy,
    // and a later frame fails. Both the previous bundle and the previous
    // project file have to survive that.
    const QString otherDir = m_dir.filePath(QStringLiteral("replace-rollback-other"));
    QVERIFY(QDir().mkpath(otherDir));
    const QString otherPath = QDir(otherDir).filePath(QStringLiteral("red.png"));
    QVERIFY(QFile::copy(m_bluePath, otherPath));
    ImageStore images;
    const auto other = images.load(otherPath, ImportPolicy{});
    QVERIFY(other.ok());
    QVERIFY(other.value().sha256 != m_redSha);

    DocumentSnapshot document =
        singleFrameDocument(other.value(), QStringLiteral("frame-replaced"));
    Frame broken = sampleFrame(other.value(), QStringLiteral("frame-broken"));
    broken.sourcePath = m_dir.filePath(QStringLiteral("no-such-photo.png"));
    document.pages[0].frames.append(broken);

    const auto saved = store.save(path, document, SaveMode::Portable);
    QVERIFY(!saved.ok());
    QCOMPARE(saved.error().code, QStringLiteral("source-missing"));

    QCOMPARE(fileSha256(carried), m_redSha);
    // Nothing half-written is left behind, not even a hidden backup.
    QCOMPARE(QDir(filesDir).entryList(QDir::Files | QDir::Hidden | QDir::System).size(), 2);

    const auto reloaded = store.load(path);
    if (!reloaded)
        QFAIL(qPrintable(reloaded.error().message));
    QVERIFY(reloaded.value().issues.isEmpty());
    QCOMPARE(reloaded.value().document.pages.size(), 2);
}

void TestProjectStore::portableCopyReplacementLeavesNoBackup()
{
    const QString projectDir = m_dir.filePath(QStringLiteral("replace-commit"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("p.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Portable).ok());

    const QString filesDir = path + QStringLiteral(".files");
    const QString carried = QDir(filesDir).filePath(QStringLiteral("red.png"));
    QCOMPARE(fileSha256(carried), m_redSha);

    const QString otherDir = m_dir.filePath(QStringLiteral("replace-commit-other"));
    QVERIFY(QDir().mkpath(otherDir));
    const QString otherPath = QDir(otherDir).filePath(QStringLiteral("red.png"));
    QVERIFY(QFile::copy(m_bluePath, otherPath));
    ImageStore images;
    const auto other = images.load(otherPath, ImportPolicy{});
    QVERIFY(other.ok());

    const auto saved = store.save(path, singleFrameDocument(other.value(), QStringLiteral("frame-replaced")),
                                  SaveMode::Portable);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));

    // The replacement landed and the file it displaced is not kept around.
    QCOMPARE(fileSha256(carried), other.value().sha256);
    QCOMPARE(QDir(filesDir).entryList(QDir::Files | QDir::Hidden | QDir::System).size(), 2);

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QVERIFY(loaded.value().issues.isEmpty());
    const Frame frame = loaded.value().document.pages.at(0).frames.at(0);
    QCOMPARE(fileSha256(frame.sourcePath), other.value().sha256);
}

void TestProjectStore::portableGeneratedNamesNeverShadowPlainNames()
{
    // Two different photos share the base name `photo.png`, so both need a
    // generated name. The third source already stores its photo under exactly
    // the hashed name the first one would get. A generated name must never take
    // a name another source already owns, or the second copy would silently
    // replace the first.
    const QString oneDir = m_dir.filePath(QStringLiteral("shadow-a"));
    const QString twoDir = m_dir.filePath(QStringLiteral("shadow-b"));
    const QString shadowDir = m_dir.filePath(QStringLiteral("shadow-c"));
    QVERIFY(QDir().mkpath(oneDir));
    QVERIFY(QDir().mkpath(twoDir));
    QVERIFY(QDir().mkpath(shadowDir));

    const QString onePath = QDir(oneDir).filePath(QStringLiteral("photo.png"));
    const QString twoPath = QDir(twoDir).filePath(QStringLiteral("photo.png"));
    QVERIFY(QFile::copy(m_redPath, onePath));
    QVERIFY(QFile::copy(m_bluePath, twoPath));

    ImageStore images;
    const auto one = images.load(onePath, ImportPolicy{});
    const auto two = images.load(twoPath, ImportPolicy{});
    QVERIFY(one.ok());
    QVERIFY(two.ok());
    QVERIFY(one.value().sha256 != two.value().sha256);

    const QString green = m_dir.filePath(QStringLiteral("shadow-green.png"));
    QVERIFY(writeImage(green, QColor(40, 160, 70)));
    const QString shadowPath =
        QDir(shadowDir).filePath(QStringLiteral("photo-%1.png")
                                     .arg(QString::fromLatin1(one.value().sha256.toHex()).left(8)));
    QVERIFY(QFile::copy(green, shadowPath));
    const auto shadow = images.load(shadowPath, ImportPolicy{});
    QVERIFY(shadow.ok());

    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page");
    page.frames.append(sampleFrame(one.value(), QStringLiteral("frame-one")));
    page.frames.append(sampleFrame(two.value(), QStringLiteral("frame-two")));
    page.frames.append(sampleFrame(shadow.value(), QStringLiteral("frame-shadow")));
    document.pages.append(page);

    const QString originDir = m_dir.filePath(QStringLiteral("shadow-bundle"));
    QVERIFY(QDir().mkpath(originDir));
    const QString path = QDir(originDir).filePath(QStringLiteral("bundle.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, document, SaveMode::Portable);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));

    // Three different photos, three different carried files.
    QStringList stored;
    for (const QJsonValue &pageValue : readJson(path).value(QStringLiteral("pages")).toArray()) {
        for (const QJsonValue &frameValue :
             pageValue.toObject().value(QStringLiteral("frames")).toArray()) {
            stored.append(frameValue.toObject().value(QStringLiteral("source")).toString());
        }
    }
    QCOMPARE(stored.size(), 3);
    QSet<QString> unique;
    for (const QString &name : stored)
        unique.insert(name);
    QCOMPARE(unique.size(), 3);
    QCOMPARE(QDir(path + QStringLiteral(".files")).entryList(QDir::Files).size(), 3);

    const QString movedDir = m_dir.filePath(QStringLiteral("shadow-bundle-moved"));
    QVERIFY(QDir().rename(originDir, movedDir));
    const auto loaded = store.load(QDir(movedDir).filePath(QStringLiteral("bundle.ppj")));
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QVERIFY(loaded.value().issues.isEmpty());

    const auto &frames = loaded.value().document.pages.at(0).frames;
    QCOMPARE(frames.size(), 3);
    QCOMPARE(fileSha256(frames.at(0).sourcePath), one.value().sha256);
    QCOMPARE(fileSha256(frames.at(1).sourcePath), two.value().sha256);
    QCOMPARE(fileSha256(frames.at(2).sourcePath), shadow.value().sha256);
    QVERIFY(frames.at(0).sourcePath != frames.at(1).sourcePath);
    QVERIFY(frames.at(0).sourcePath != frames.at(2).sourcePath);
    QVERIFY(frames.at(1).sourcePath != frames.at(2).sourcePath);
}

void TestProjectStore::portableAnonymousNamesAvoidPlainNames()
{
    // Frames without a recorded hash get a numbered name. Two of those plus one
    // source whose plain base name is already `photo-1.png` must still end up
    // on three files.
    const QString oneDir = m_dir.filePath(QStringLiteral("anonymous-a"));
    const QString twoDir = m_dir.filePath(QStringLiteral("anonymous-b"));
    const QString plainDir = m_dir.filePath(QStringLiteral("anonymous-plain"));
    QVERIFY(QDir().mkpath(oneDir));
    QVERIFY(QDir().mkpath(twoDir));
    QVERIFY(QDir().mkpath(plainDir));

    const QString onePath = QDir(oneDir).filePath(QStringLiteral("photo.png"));
    const QString twoPath = QDir(twoDir).filePath(QStringLiteral("photo.png"));
    const QString plainPath = QDir(plainDir).filePath(QStringLiteral("photo-1.png"));
    QVERIFY(QFile::copy(m_redPath, onePath));
    QVERIFY(writeImage(twoPath, QColor(40, 160, 70)));
    QVERIFY(QFile::copy(m_bluePath, plainPath));

    ImageStore images;
    const auto one = images.load(onePath, ImportPolicy{});
    const auto two = images.load(twoPath, ImportPolicy{});
    const auto plain = images.load(plainPath, ImportPolicy{});
    QVERIFY(one.ok());
    QVERIFY(two.ok());
    QVERIFY(plain.ok());
    QVERIFY(one.value().sha256 != two.value().sha256);

    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page");
    Frame hashlessOne = sampleFrame(one.value(), QStringLiteral("frame-hashless-one"));
    hashlessOne.sourceSha256.clear(); // a project may carry no identity at all
    Frame hashlessTwo = sampleFrame(two.value(), QStringLiteral("frame-hashless-two"));
    hashlessTwo.sourceSha256.clear();
    page.frames.append(hashlessOne);
    page.frames.append(hashlessTwo);
    page.frames.append(sampleFrame(plain.value(), QStringLiteral("frame-plain")));
    document.pages.append(page);

    const QString originDir = m_dir.filePath(QStringLiteral("anonymous-bundle"));
    QVERIFY(QDir().mkpath(originDir));
    const QString path = QDir(originDir).filePath(QStringLiteral("bundle.ppj"));
    ProjectStore store;
    const auto saved = store.save(path, document, SaveMode::Portable);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));

    QStringList stored;
    for (const QJsonValue &pageValue : readJson(path).value(QStringLiteral("pages")).toArray()) {
        for (const QJsonValue &frameValue :
             pageValue.toObject().value(QStringLiteral("frames")).toArray()) {
            stored.append(frameValue.toObject().value(QStringLiteral("source")).toString());
        }
    }
    QCOMPARE(stored.size(), 3);
    QSet<QString> unique;
    for (const QString &name : stored)
        unique.insert(name);
    QCOMPARE(unique.size(), 3);
    // The plain name stays with the source that owns it.
    QVERIFY2(QFileInfo(stored.at(2)).fileName() == QStringLiteral("photo-1.png"),
             qPrintable(stored.at(2)));
    QCOMPARE(QDir(path + QStringLiteral(".files")).entryList(QDir::Files).size(), 3);

    const QString movedDir = m_dir.filePath(QStringLiteral("anonymous-bundle-moved"));
    QVERIFY(QDir().rename(originDir, movedDir));
    const auto loaded = store.load(QDir(movedDir).filePath(QStringLiteral("bundle.ppj")));
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));

    const auto &frames = loaded.value().document.pages.at(0).frames;
    QCOMPARE(frames.size(), 3);
    QCOMPARE(fileSha256(frames.at(0).sourcePath), one.value().sha256);
    QCOMPARE(fileSha256(frames.at(1).sourcePath), two.value().sha256);
    QCOMPARE(fileSha256(frames.at(2).sourcePath), plain.value().sha256);
}

void TestProjectStore::portableBundleCanBeResaved()
{
    const QString projectDir = m_dir.filePath(QStringLiteral("resave"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("r.ppj"));
    const QString filesDir = path + QStringLiteral(".files");
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Portable).ok());

    // Loading a bundle points every frame at the carried file, so a plain
    // re-save copies each file onto itself and must stay a no-op.
    const auto first = store.load(path);
    if (!first)
        QFAIL(qPrintable(first.error().message));
    QVERIFY(first.value().issues.isEmpty());
    DocumentSnapshot document = first.value().document;
    for (const Page &page : document.pages) {
        for (const Frame &frame : page.frames)
            QVERIFY2(frame.sourcePath.contains(QStringLiteral(".files")), qPrintable(frame.sourcePath));
    }
    QVERIFY(store.save(path, document, SaveMode::Portable).ok());
    QCOMPARE(QDir(filesDir).entryList(QDir::Files | QDir::Hidden | QDir::System).size(), 2);

    // The carried copy no longer matches the hash the project recorded. The
    // save used to move that file aside and then copy the very file it had just
    // moved, which could never succeed and made the bundle unsavable.
    const QString carried = QDir(filesDir).filePath(QStringLiteral("red.png"));
    QCOMPARE(fileSha256(carried), m_redSha);
    {
        const QByteArray extra = QByteArrayLiteral("edited after the last save");
        QFile file(carried);
        QVERIFY(file.open(QIODevice::Append));
        QCOMPARE(file.write(extra), qint64(extra.size()));
    }
    const auto second = store.load(path);
    if (!second)
        QFAIL(qPrintable(second.error().message));
    QCOMPARE(second.value().issues.size(), 1);
    QVERIFY(second.value().issues.first().changed);

    QVERIFY(store.save(path, second.value().document, SaveMode::Portable).ok());
    QVERIFY(QFileInfo::exists(carried));
    QVERIFY(QFileInfo(carried).size() > QFileInfo(m_redPath).size());
    QCOMPARE(QDir(filesDir).entryList(QDir::Files | QDir::Hidden | QDir::System).size(), 2);

    // A relinked source is the real repair: it replaces the stale carried file.
    const QString replacementDir = m_dir.filePath(QStringLiteral("resave-replacement"));
    QVERIFY(QDir().mkpath(replacementDir));
    const QString replacementPath = QDir(replacementDir).filePath(QStringLiteral("red.png"));
    QVERIFY(writeImage(replacementPath, QColor(40, 160, 70)));
    ImageStore images;
    const auto replacement = images.load(replacementPath, ImportPolicy{});
    QVERIFY(replacement.ok());
    QVERIFY(replacement.value().sha256 != m_redSha);

    DocumentSnapshot relinked = second.value().document;
    relinked.pages[0].frames[0].sourcePath = replacement.value().path;
    relinked.pages[0].frames[0].sourceSha256 = replacement.value().sha256;
    const auto resaved = store.save(path, relinked, SaveMode::Portable);
    if (!resaved)
        QFAIL(qPrintable(resaved.error().message));
    QCOMPARE(fileSha256(carried), replacement.value().sha256);
    QCOMPARE(QDir(filesDir).entryList(QDir::Files | QDir::Hidden | QDir::System).size(), 2);

    const auto third = store.load(path);
    if (!third)
        QFAIL(qPrintable(third.error().message));
    QVERIFY(third.value().issues.isEmpty());
    QCOMPARE(fileSha256(third.value().document.pages.at(0).frames.at(0).sourcePath),
             replacement.value().sha256);
}

void TestProjectStore::craftedBrightnessCannotReachTheRenderer()
{
    const QString projectDir = m_dir.filePath(QStringLiteral("brightness"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("b.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Normal).ok());

    // A hand-edited project can carry any number. One page gets a white-out
    // positive value, the other a black-out negative one.
    QJsonObject root = readJson(path);
    QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    for (int p = 0; p < pages.size(); ++p) {
        QJsonObject page = pages.at(p).toObject();
        QJsonArray frames = page.value(QStringLiteral("frames")).toArray();
        for (int f = 0; f < frames.size(); ++f) {
            QJsonObject frame = frames.at(f).toObject();
            frame.insert(QStringLiteral("brightness"), p == 0 ? 20.0 : -20.0);
            frames.replace(f, frame);
        }
        page.insert(QStringLiteral("frames"), frames);
        pages.replace(p, page);
    }
    root.insert(QStringLiteral("pages"), pages);
    QVERIFY(writeJson(path, root));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    const Frame high = loaded.value().document.pages.at(0).frames.at(0);
    const Frame low = loaded.value().document.pages.at(1).frames.at(0);
    QCOMPARE(high.brightness, kMaxBrightnessEv);
    QCOMPARE(low.brightness, kMinBrightnessEv);
    QCOMPARE(effectiveTone(high).exposureEv, high.tone.exposureEv + kMaxBrightnessEv);
    QCOMPARE(effectiveTone(low).exposureEv, low.tone.exposureEv + kMinBrightnessEv);

    // The tone path is guarded on its own, not only through the store.
    Frame inMemory;
    inMemory.autoTone = false;
    inMemory.brightness = 20.0;
    QCOMPARE(effectiveTone(inMemory).exposureEv, kMaxBrightnessEv);
    inMemory.brightness = -20.0;
    QCOMPARE(effectiveTone(inMemory).exposureEv, kMinBrightnessEv);
}

void TestProjectStore::craftedPaperToneIsClamped()
{
    const QString projectDir = m_dir.filePath(QStringLiteral("paper-tone"));
    QVERIFY(QDir().mkpath(projectDir));
    const QString path = QDir(projectDir).filePath(QStringLiteral("t.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Normal).ok());

    // A hand-edited project can carry any number, and the value becomes the
    // paper exposure through std::pow: a huge positive value blows the cell
    // out, a huge negative one blacks it out. The trial sheet only ever offers
    // -kTrialPaperEv .. kTrialPaperEv, so the load clamps to that range.
    QJsonObject root = readJson(path);
    QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    // The fixture below is two pages, one per sign. Pinning the count keeps a
    // grown fixture from indexing past `crafted` below.
    QCOMPARE(pages.size(), 2);
    const double crafted[] = {20.0, -20.0};
    for (int p = 0; p < pages.size(); ++p) {
        QJsonObject page = pages.at(p).toObject();
        QJsonArray frames = page.value(QStringLiteral("frames")).toArray();
        for (int f = 0; f < frames.size(); ++f) {
            QJsonObject frame = frames.at(f).toObject();
            frame.insert(QStringLiteral("paperEv"), crafted[p]);
            frame.insert(QStringLiteral("paperOverride"), true);
            frames.replace(f, frame);
        }
        page.insert(QStringLiteral("frames"), frames);
        pages.replace(p, page);
    }
    root.insert(QStringLiteral("pages"), pages);
    QVERIFY(writeJson(path, root));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    const DocumentSnapshot &document = loaded.value().document;
    QCOMPARE(document.pages.at(0).frames.at(0).paperEv, kTrialPaperEv);
    QCOMPARE(document.pages.at(1).frames.at(0).paperEv, -kTrialPaperEv);
}

void TestProjectStore::craftedRecipeCannotReachThePrintPath()
{
    // MainWindow clamps the recipe it hands to the preview and the thumbnail
    // paths. The renderer is the last boundary before the printed pixels, so a
    // snapshot that still carries a huge paper strength, or a crafted per-frame
    // trial override, has to produce exactly what the clamped recipe produces.
    // Otherwise a crafted preset would print a page the preview never showed.
    DocumentSnapshot crafted = singleFrameDocument(m_red, QStringLiteral("frame-crafted"));
    crafted.pages[0].frames[0].paperOverride = true;
    crafted.recipe.paperStrength = 100.0;
    crafted.pages[0].frames[0].paperEv = 100.0;

    DocumentSnapshot sanitised = crafted;
    sanitised.recipe = sanitiseRecipe(sanitised.recipe);
    sanitised.pages[0].frames[0].paperEv = kMaxPaperEv;

    ImageStore images;
    PageRenderer renderer(images);
    const auto printed = renderer.renderPage(crafted, 0, 72.0);
    const auto preview = renderer.renderPage(sanitised, 0, 72.0);
    if (!printed)
        QFAIL(qPrintable(printed.error().message));
    if (!preview)
        QFAIL(qPrintable(preview.error().message));
    QCOMPARE(printed.value(), preview.value());

    // The clamp really is the recipe bound, not a no-op: the same frame through
    // a neutral recipe differs, so the equality above is not trivially true.
    DocumentSnapshot neutral = sanitised;
    neutral.recipe.paperStrength = 0.0;
    neutral.pages[0].frames[0].paperOverride = false;
    const auto withoutPaper = renderer.renderPage(neutral, 0, 72.0);
    QVERIFY(withoutPaper.ok());
    QVERIFY(withoutPaper.value() != preview.value());
}

void TestProjectStore::versionOneProjectWithoutNewKeysStillLoads()
{
    const QString path = m_dir.filePath(QStringLiteral("legacy.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, sampleDocument(m_red, m_blue), SaveMode::Normal).ok());

    // A 1.0.0 file has no trial-sheet keys; they must default to "no label"
    // instead of making the frame or the project unreadable.
    QJsonObject root = readJson(path);
    QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    for (int p = 0; p < pages.size(); ++p) {
        QJsonObject page = pages.at(p).toObject();
        QJsonArray frames = page.value(QStringLiteral("frames")).toArray();
        for (int f = 0; f < frames.size(); ++f) {
            QJsonObject frame = frames.at(f).toObject();
            frame.remove(QStringLiteral("label"));
            frame.remove(QStringLiteral("paperOverride"));
            frame.remove(QStringLiteral("paperEv"));
            frames.replace(f, frame);
        }
        page.insert(QStringLiteral("frames"), frames);
        pages.replace(p, page);
    }
    root.insert(QStringLiteral("pages"), pages);
    QVERIFY(writeJson(path, root));

    const auto loaded = store.load(path);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    for (const Page &page : loaded.value().document.pages) {
        for (const Frame &frame : page.frames) {
            QVERIFY(frame.label.isEmpty());
            QVERIFY(!frame.paperOverride);
            QCOMPARE(frame.paperEv, 0.0);
        }
    }
    QCOMPARE(loaded.value().document.pages.at(1).paper.name,
             QStringLiteral("12 × 18 cm (egyedi)"));
}

void TestProjectStore::missingAndChangedSourcesKeepTheLayout()
{
    const DocumentSnapshot original = sampleDocument(m_red, m_blue);
    const QString path = m_dir.filePath(QStringLiteral("issues.ppj"));
    ProjectStore store;
    QVERIFY(store.save(path, original, SaveMode::Normal).ok());

    const QString redBackup = m_dir.filePath(QStringLiteral("red-backup.png"));
    QVERIFY(QFile::rename(m_redPath, redBackup));

    {
        const auto loaded = store.load(path);
        if (!loaded)
            QFAIL(qPrintable(loaded.error().message));
        QCOMPARE(loaded.value().issues.size(), 1);
        QCOMPARE(loaded.value().issues.first().frameId, QStringLiteral("frame-red"));
        QVERIFY(loaded.value().issues.first().missing);
        QCOMPARE(loaded.value().document.pages.at(0).frames.at(0).cx,
                 original.pages.at(0).frames.at(0).cx);
    }

    QVERIFY(QFile::rename(redBackup, m_redPath));

    QFile blue(m_bluePath);
    QVERIFY(blue.open(QIODevice::Append));
    blue.write("changed");
    blue.close();

    {
        const auto loaded = store.load(path);
        if (!loaded)
            QFAIL(qPrintable(loaded.error().message));
        QCOMPARE(loaded.value().issues.size(), 1);
        QCOMPARE(loaded.value().issues.first().frameId, QStringLiteral("frame-blue"));
        QVERIFY(loaded.value().issues.first().changed);
        QCOMPARE(loaded.value().document.pages.at(1).frames.at(0).width,
                 original.pages.at(1).frames.at(0).width);
    }
}

void TestProjectStore::portableCopyWorksAfterMoving()
{
    // The changed-source test appended bytes to the blue file; restore it so
    // the recorded identity matches again.
    QFile::remove(m_bluePath);
    QVERIFY(QFile::copy(m_dir.filePath(QStringLiteral("blue-original.png")), m_bluePath));
    QCOMPARE(fileSha256(m_bluePath), m_blueSha);

    const DocumentSnapshot original = sampleDocument(m_red, m_blue);
    const QString originDir = m_dir.filePath(QStringLiteral("origin"));
    QVERIFY(QDir().mkpath(originDir));
    const QString path = QDir(originDir).filePath(QStringLiteral("portable.ppj"));

    ProjectStore store;
    const auto saved = store.save(path, original, SaveMode::Portable);
    if (!saved)
        QFAIL(qPrintable(saved.error().message));
    QVERIFY(QDir(path + QStringLiteral(".files")).exists());

    const QString movedDir = m_dir.filePath(QStringLiteral("moved"));
    QVERIFY(QDir().rename(originDir, movedDir));
    const QString movedPath = QDir(movedDir).filePath(QStringLiteral("portable.ppj"));

    const auto loaded = store.load(movedPath);
    if (!loaded)
        QFAIL(qPrintable(loaded.error().message));
    QVERIFY(loaded.value().issues.isEmpty());
    const DocumentSnapshot restored = loaded.value().document;
    for (const Page &page : restored.pages) {
        for (const Frame &frame : page.frames)
            QVERIFY(QFileInfo::exists(frame.sourcePath));
    }

    ImageStore images;
    PageRenderer originalRenderer(images);
    PageRenderer restoredRenderer(images);
    const auto originalPage = originalRenderer.renderPage(original, 0, 120.0);
    const auto restoredPage = restoredRenderer.renderPage(restored, 0, 120.0);
    QVERIFY(originalPage.ok());
    QVERIFY(restoredPage.ok());
    QCOMPARE(originalPage.value(), restoredPage.value());

    QCOMPARE(fileSha256(m_redPath), m_redSha);
    QCOMPARE(fileSha256(m_bluePath), m_blueSha);
}

void TestProjectStore::corruptAndUnknownProjectsAreRejected()
{
    ProjectStore store;
    const QString corrupt = m_dir.filePath(QStringLiteral("corrupt.ppj"));
    QFile file(corrupt);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ this is not json");
    file.close();
    const auto corruptResult = store.load(corrupt);
    QVERIFY(!corruptResult.ok());
    QCOMPARE(corruptResult.error().code, QStringLiteral("project-corrupt"));

    const DocumentSnapshot original = sampleDocument(m_red, m_blue);
    const QString path = m_dir.filePath(QStringLiteral("future.ppj"));
    QVERIFY(store.save(path, original, SaveMode::Normal).ok());
    QFile future(path);
    QVERIFY(future.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(future.readAll()).object();
    future.close();
    root.insert(QStringLiteral("version"), 99);
    QVERIFY(future.open(QIODevice::WriteOnly | QIODevice::Truncate));
    future.write(QJsonDocument(root).toJson());
    future.close();

    const auto futureResult = store.load(path);
    QVERIFY(!futureResult.ok());
    QCOMPARE(futureResult.error().code, QStringLiteral("project-version"));
}

void TestProjectStore::changedSourceStopsPrintPreparation()
{
    QFile::remove(m_bluePath);
    QVERIFY(QFile::copy(m_dir.filePath(QStringLiteral("blue-original.png")), m_bluePath));
    const DocumentSnapshot original = sampleDocument(m_red, m_blue);

    QFile blue(m_bluePath);
    QVERIFY(blue.open(QIODevice::Append));
    blue.write("changed after saving");
    blue.close();

    ImageStore images;
    PageRenderer renderer(images);
    RenderOptions options;
    options.verifySources = true;
    const auto rendered = renderer.renderPage(original, 1, 120.0, {}, options);
    QVERIFY(!rendered.ok());
    QCOMPARE(rendered.error().code, QStringLiteral("source-changed"));
}

QTEST_GUILESS_MAIN(TestProjectStore)
#include "tst_project_store.moc"
