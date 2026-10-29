#include "ColorPipeline.h"
#include "ImageSource.h"

#include <QColor>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

using namespace pp;

namespace {

QImage solidImage(int width, int height, const QColorSpace &space, QRgb colour)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(colour);
    image.setColorSpace(space);
    return image;
}

QImage makePatchImage(const QColorSpace &space)
{
    QImage image(400, 200, QImage::Format_RGB32);
    image.setColorSpace(space);
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, 200, 100), QColor(230, 40, 40));
    painter.fillRect(QRect(200, 0, 200, 100), QColor(40, 200, 60));
    painter.fillRect(QRect(0, 100, 200, 100), QColor(40, 60, 220));
    painter.fillRect(QRect(200, 100, 200, 100), QColor(128, 128, 128));
    painter.end();
    return image;
}

bool savePng(const QImage &image, const QString &path)
{
    QImageWriter writer(path, "png");
    if (!writer.write(image))
        return false;
    return true;
}

QImage readImage(const QString &path)
{
    return QImage(path);
}

int channelDistance(QRgb a, QRgb b)
{
    return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                     std::abs(qBlue(a) - qBlue(b))});
}

} // namespace

class TestImportColor : public QObject
{
    Q_OBJECT

private slots:
    void realPhotoKeepsItsProfile();
    void p3FieldsMatchIndependentQtConversion();
    void missingProfileFallsBackToSrgb();
    void brokenProfileStillOpens();
    void exifOrientationIsApplied();
    void sourceFileIsNotModified();
    void replacedFileIsReloaded();
};

void TestImportColor::realPhotoKeepsItsProfile()
{
    const QString path = QStringLiteral("/home/lavx/Downloads/IMG20260830150231.jpg");
    QVERIFY2(QFileInfo::exists(path), "The agreed local sample photo is missing.");

    ImageStore store;
    const auto loaded = store.load(path, ImportPolicy{});
    if (!loaded) QFAIL(qPrintable(loaded.error().message));
    const SourceImage &source = loaded.value();

    QCOMPARE(source.size(), QSize(3072, 4096));
    QVERIFY(source.hasEmbeddedProfile);
    QVERIFY(source.profileValid);
    QVERIFY(source.displayP3);
    QVERIFY(source.sdrAssumed);
    QVERIFY(!source.sha256.isEmpty());
    QVERIFY(!source.pixels.isNull());
}

void TestImportColor::p3FieldsMatchIndependentQtConversion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage p3 = makePatchImage(QColorSpace(QColorSpace::DisplayP3));
    const QString path = dir.filePath(QStringLiteral("p3-fields.png"));
    QVERIFY(savePng(p3, path));

    ImageStore store;
    const auto loaded = store.load(path, ImportPolicy{});
    if (!loaded) QFAIL(qPrintable(loaded.error().message));
    QVERIFY(loaded.value().displayP3);

    const QImage ours = ColorPipeline::toSrgb(loaded.value(), ColorRecipe{});
    QVERIFY(!ours.isNull());
    QImage qtReference = p3;
    qtReference.convertToColorSpace(QColorSpace(QColorSpace::SRgb));

    const QList<QPoint> probes = {QPoint(100, 50), QPoint(300, 50), QPoint(100, 150), QPoint(300, 150)};
    for (const QPoint &point : probes) {
        const QRgb a = ours.pixel(point);
        const QRgb b = qtReference.pixel(point);
        const int distance = channelDistance(a, b);
        QVERIFY2(distance <= 8,
                 qPrintable(QStringLiteral("P3 -> sRGB differs by %1 at %2,%3: ours %4/%5/%6, "
                                           "Qt %7/%8/%9")
                                .arg(distance)
                                .arg(point.x())
                                .arg(point.y())
                                .arg(qRed(a))
                                .arg(qGreen(a))
                                .arg(qBlue(a))
                                .arg(qRed(b))
                                .arg(qGreen(b))
                                .arg(qBlue(b))));
    }

    // The conversion must be real: P3 primaries cannot equal the raw values.
    const QRgb raw = p3.pixel(100, 50);
    const QRgb converted = ours.pixel(100, 50);
    QVERIFY(channelDistance(raw, converted) > 0);
}

void TestImportColor::missingProfileFallsBackToSrgb()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage plain(64, 64, QImage::Format_RGB32);
    plain.fill(QColor(120, 80, 40));
    const QString path = dir.filePath(QStringLiteral("plain.png"));
    QVERIFY(savePng(plain, path));

    ImageStore store;
    const auto loaded = store.load(path, ImportPolicy{});
    if (!loaded) QFAIL(qPrintable(loaded.error().message));
    QVERIFY(!loaded.value().hasEmbeddedProfile);
    QVERIFY(loaded.value().srgbFallback);
    QVERIFY(loaded.value().sourceSpace.isValid());
    QCOMPARE(loaded.value().size(), QSize(64, 64));
}

void TestImportColor::brokenProfileStillOpens()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage plain(48, 32, QImage::Format_RGB32);
    plain.fill(QColor(10, 200, 130));
    const QString plainPath = dir.filePath(QStringLiteral("base.jpg"));
    QImageWriter writer(plainPath, "jpeg");
    QVERIFY(writer.write(plain));

    QFile file(plainPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray bytes = file.readAll();
    file.close();

    // Append a syntactically present but unparsable ICC APP2 segment.
    const QByteArray payload = QByteArrayLiteral("ICC_PROFILE\0") + QByteArray(2, '\x01')
        + QByteArrayLiteral("this-is-not-an-icc-profile");
    QByteArray segment;
    segment.append('\xFF');
    segment.append('\xE2');
    const int length = payload.size() + 2;
    segment.append(static_cast<char>((length >> 8) & 0xFF));
    segment.append(static_cast<char>(length & 0xFF));
    segment.append(payload);
    QByteArray broken = bytes.left(2) + segment + bytes.mid(2);

    const QString brokenPath = dir.filePath(QStringLiteral("broken-profile.jpg"));
    QFile out(brokenPath);
    QVERIFY(out.open(QIODevice::WriteOnly));
    QCOMPARE(out.write(broken), static_cast<qint64>(broken.size()));
    out.close();

    ImageStore store;
    const auto loaded = store.load(brokenPath, ImportPolicy{});
    if (!loaded) QFAIL(qPrintable(loaded.error().message));
    QVERIFY(loaded.value().hasEmbeddedProfile);
    QVERIFY(!loaded.value().profileValid);
    QVERIFY(loaded.value().srgbFallback);
    QCOMPARE(loaded.value().size(), QSize(48, 32));
}

void TestImportColor::exifOrientationIsApplied()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage landscape(40, 20, QImage::Format_RGB32);
    landscape.fill(QColor(200, 120, 40));
    const QString path = dir.filePath(QStringLiteral("rotated.jpg"));
    QImageWriter writer(path, "jpeg");
    writer.setTransformation(QImageIOHandler::TransformationRotate90);
    QVERIFY(writer.write(landscape));

    ImageStore store;
    const auto loaded = store.load(path, ImportPolicy{});
    if (!loaded) QFAIL(qPrintable(loaded.error().message));
    QCOMPARE(loaded.value().size(), QSize(20, 40));
}

void TestImportColor::sourceFileIsNotModified()
{
    const QString path = QStringLiteral("/home/lavx/Downloads/IMG20260830150231.jpg");
    QVERIFY(QFileInfo::exists(path));
    const QByteArray before = fileSha256(path);
    ImageStore store;
    const auto loaded = store.load(path, ImportPolicy{});
    QVERIFY(loaded.ok());
    const QImage converted = ColorPipeline::toSrgb(loaded.value(), ColorRecipe{}, 800);
    QVERIFY(!converted.isNull());
    QCOMPARE(fileSha256(path), before);
}

void TestImportColor::replacedFileIsReloaded()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("replace.png"));
    QVERIFY(savePng(solidImage(64, 64, QColorSpace(QColorSpace::SRgb), qRgb(200, 30, 30)), path));

    ImageStore store;
    const auto first = store.load(path, ImportPolicy{});
    QVERIFY(first.ok());
    const QByteArray firstSha = first.value().sha256;
    QCOMPARE(qRed(first.value().pixels.pixel(10, 10)), 200);

    QVERIFY(savePng(solidImage(80, 64, QColorSpace(QColorSpace::SRgb), qRgb(30, 30, 200)), path));
    const auto second = store.load(path, ImportPolicy{});
    QVERIFY(second.ok());
    QVERIFY(second.value().sha256 != firstSha);
    QCOMPARE(second.value().size(), QSize(80, 64));
    QCOMPARE(qBlue(second.value().pixels.pixel(10, 10)), 200);
}

QTEST_GUILESS_MAIN(TestImportColor)
#include "tst_import_color.moc"
