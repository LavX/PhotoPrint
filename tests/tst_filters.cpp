#include "PhotoFilters.h"

#include <QColorSpace>
#include <QImage>
#include <QtTest>

#include <cstring>

using namespace pp;

namespace {

QImage pattern(int width, int height, QImage::Format format = QImage::Format_RGB32)
{
    QImage image(width, height, format);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int r = (x * 37 + y * 11) % 256;
            const int g = (x * 17 + y * 53) % 256;
            const int b = (x * 71 + y * 7) % 256;
            const int a = (x + y) % 2 == 0 ? 255 : 96;
            image.setPixel(x, y, qRgba(r, g, b, format == QImage::Format_RGB32 ? 255 : a));
        }
    }
    return image;
}

QImage solid(const QColor &colour, int width, int height, QImage::Format format)
{
    QImage image(width, height, format);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    return image;
}

bool samePixels(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format())
        return false;
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<size_t>(a.width()) * 4)
            != 0)
            return false;
    }
    return true;
}

qint64 totalDistance(const QImage &a, const QImage &b)
{
    qint64 distance = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            distance += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q))
                        + std::abs(qBlue(p) - qBlue(q));
        }
    }
    return distance;
}

} // namespace

class TestFilters : public QObject
{
    Q_OBJECT

private slots:
    void zeroStrengthIsPixelIdentical();
    void originalPresetIsIdentity();
    void repeatedApplicationIsDeterministic();
    void alphaIsPreserved();
    void blackWhiteIsNeutral();
    void tinyFramesDoNotCrash();
    void upscaleDoublesDimensions();
    void upscaleOneIsIdentity();
    void upscalePreservesAlpha();
    void upscaleCapReturnsNull();
    void flatGreyStaysFlatUnderWarmAndCool();
    void vintageWarmsAndDarkensCorners();
};

void TestFilters::zeroStrengthIsPixelIdentical()
{
    const QImage source = pattern(17, 13);
    const QImage alphaSource = pattern(17, 13, QImage::Format_ARGB32);
    const FilterPreset presets[] = {
        FilterPreset::Vivid, FilterPreset::Warm,  FilterPreset::Cool,    FilterPreset::Sepia,
        FilterPreset::BlackWhite, FilterPreset::Vintage, FilterPreset::Crisp,
    };
    for (FilterPreset preset : presets) {
        QVERIFY(samePixels(PhotoFilters::apply(source, preset, 0), source));
        QVERIFY(samePixels(PhotoFilters::apply(alphaSource, preset, 0), alphaSource));
    }
    QVERIFY(samePixels(PhotoFilters::apply(source, FilterPreset::Vivid, -25), source));
}

void TestFilters::originalPresetIsIdentity()
{
    const QImage source = pattern(11, 9);
    QVERIFY(samePixels(PhotoFilters::apply(source, FilterPreset::Original, 100), source));
}

void TestFilters::repeatedApplicationIsDeterministic()
{
    const QImage source = pattern(64, 48);
    for (FilterPreset preset : {FilterPreset::Vivid, FilterPreset::Vintage, FilterPreset::Crisp}) {
        const QImage first = PhotoFilters::apply(source, preset, 75);
        const QImage second = PhotoFilters::apply(source, preset, 75);
        QVERIFY(!first.isNull());
        QVERIFY(samePixels(first, second));
    }
}

void TestFilters::alphaIsPreserved()
{
    QImage source(4, 1, QImage::Format_ARGB32);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    const QRgb pixels[4] = {
        qRgba(200, 60, 40, 0),
        qRgba(200, 60, 40, 64),
        qRgba(40, 160, 90, 128),
        qRgba(40, 90, 200, 255),
    };
    for (int x = 0; x < 4; ++x)
        source.setPixel(x, 0, pixels[x]);

    const QImage result = PhotoFilters::apply(source, FilterPreset::Vintage, 100);
    QVERIFY(!result.isNull());
    QCOMPARE(result.size(), source.size());
    for (int x = 0; x < 4; ++x)
        QCOMPARE(qAlpha(result.pixel(x, 0)), qAlpha(pixels[x]));
    QVERIFY(totalDistance(source, result) > 0);
}

void TestFilters::blackWhiteIsNeutral()
{
    const QImage source = solid(QColor(210, 120, 45), 12, 12, QImage::Format_RGB32);
    const QImage result = PhotoFilters::apply(source, FilterPreset::BlackWhite, 100);
    QVERIFY(!result.isNull());
    const QRgb pixel = result.pixel(6, 6);
    QVERIFY(std::abs(qRed(pixel) - qGreen(pixel)) <= 1);
    QVERIFY(std::abs(qGreen(pixel) - qBlue(pixel)) <= 1);
    QVERIFY(std::abs(qRed(pixel) - qBlue(pixel)) <= 1);
}

void TestFilters::tinyFramesDoNotCrash()
{
    for (const QSize &size : {QSize(1, 1), QSize(3, 2)}) {
        const QImage source = solid(QColor(90, 40, 170), size.width(), size.height(),
                                    QImage::Format_RGB32);
        for (FilterPreset preset : {FilterPreset::Vivid, FilterPreset::Crisp}) {
            const QImage result = PhotoFilters::apply(source, preset, 100);
            QVERIFY2(!result.isNull(), qPrintable(QStringLiteral("size %1x%2")
                                                     .arg(size.width())
                                                     .arg(size.height())));
            QCOMPARE(result.size(), size);
        }
    }
}

void TestFilters::upscaleDoublesDimensions()
{
    const QImage source = pattern(40, 30);
    const QImage result = PhotoFilters::upscale(source, 2);
    QVERIFY(!result.isNull());
    QCOMPARE(result.size(), QSize(80, 60));
}

void TestFilters::upscaleOneIsIdentity()
{
    const QImage source = pattern(9, 7);
    QVERIFY(samePixels(PhotoFilters::upscale(source, 1), source));
}

void TestFilters::upscalePreservesAlpha()
{
    QImage source = solid(QColor(120, 80, 40, 200), 20, 16, QImage::Format_ARGB32);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            const QRgb pixel = source.pixel(x, y);
            source.setPixel(x, y, qRgba(qRed(pixel), qGreen(pixel), qBlue(pixel),
                                        x < source.width() / 2 ? 255 : 64));
        }
    }

    const QImage result = PhotoFilters::upscale(source, 2);
    QVERIFY(!result.isNull());
    QCOMPARE(result.size(), QSize(40, 32));
    QCOMPARE(qAlpha(result.pixel(2, 2)), 255);
    QCOMPARE(qAlpha(result.pixel(37, 30)), 64);
}

void TestFilters::upscaleCapReturnsNull()
{
    const QImage huge(2000, 2000, QImage::Format_RGB32);
    QVERIFY(PhotoFilters::upscale(huge, 8).isNull());
    QVERIFY(PhotoFilters::upscale(huge, 0).isNull());
    QVERIFY(PhotoFilters::upscale(huge, 9).isNull());
    QVERIFY(PhotoFilters::upscale(QImage(), 2).isNull());
}

void TestFilters::flatGreyStaysFlatUnderWarmAndCool()
{
    const QImage source = solid(QColor(128, 128, 128), 16, 16, QImage::Format_RGB32);
    for (FilterPreset preset : {FilterPreset::Warm, FilterPreset::Cool}) {
        const QImage result = PhotoFilters::apply(source, preset, 100);
        QVERIFY(!result.isNull());
        for (int y = 0; y < result.height(); ++y) {
            for (int x = 0; x < result.width(); ++x) {
                const QRgb pixel = result.pixel(x, y);
                QVERIFY2(std::abs(qRed(pixel) - 128) <= 12
                             && std::abs(qGreen(pixel) - 128) <= 12
                             && std::abs(qBlue(pixel) - 128) <= 12,
                         qPrintable(QStringLiteral("rgb(%1,%2,%3)")
                                        .arg(qRed(pixel))
                                        .arg(qGreen(pixel))
                                        .arg(qBlue(pixel))));
            }
        }
    }
}

void TestFilters::vintageWarmsAndDarkensCorners()
{
    const QImage source = solid(QColor(150, 150, 150), 200, 150, QImage::Format_RGB32);
    const QImage result = PhotoFilters::apply(source, FilterPreset::Vintage, 100);
    QVERIFY(!result.isNull());

    const QRgb centre = result.pixel(100, 75);
    QVERIFY(qRed(centre) > qBlue(centre));
    const int centreLevel = qGray(centre);
    const int cornerLevel = qGray(result.pixel(0, 0));
    QVERIFY2(cornerLevel < centreLevel,
             qPrintable(QStringLiteral("corner %1 >= centre %2").arg(cornerLevel).arg(centreLevel)));
}

QTEST_GUILESS_MAIN(TestFilters)

#include "tst_filters.moc"
