#include "ToneEngine.h"

#include <QRgba64>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pp {

namespace {

struct Stats {
    double mean = 0.0;
    double p05 = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double p995 = 0.0;
};

Stats analyse(const QImage &image)
{
    Stats stats;
    const int bins = 1024;
    std::vector<double> histogram(bins, 0.0);
    double sum = 0.0;
    double count = 0.0;

    const QImage source = image.format() == QImage::Format_RGBA64_Premultiplied
        ? image
        : image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        const QRgba64 *line = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const quint16 alpha = line[x].alpha();
            if (alpha == 0)
                continue;
            const double scale = 65535.0 / alpha;
            const double red = std::min(1.0, line[x].red() * scale / 65535.0);
            const double green = std::min(1.0, line[x].green() * scale / 65535.0);
            const double blue = std::min(1.0, line[x].blue() * scale / 65535.0);
            const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
            sum += luminance;
            ++count;
            const int bin = std::clamp(static_cast<int>(luminance * (bins - 1)), 0, bins - 1);
            histogram[bin] += 1.0;
        }
    }
    if (count <= 0.0)
        return stats;
    stats.mean = sum / count;

    auto percentile = [&](double fraction) {
        const double target = fraction * count;
        double running = 0.0;
        for (int i = 0; i < bins; ++i) {
            running += histogram[i];
            if (running >= target)
                return static_cast<double>(i) / (bins - 1);
        }
        return 1.0;
    };
    stats.p05 = percentile(0.05);
    stats.p50 = percentile(0.50);
    stats.p95 = percentile(0.95);
    stats.p99 = percentile(0.99);
    stats.p995 = percentile(0.995);
    return stats;
}

} // namespace

PhotoTone ToneEngine::suggest(const QImage &linearSample)
{
    PhotoTone tone;
    if (linearSample.isNull())
        return tone;
    const Stats stats = analyse(linearSample);
    if (stats.mean <= 0.0)
        return tone;

    double exposure = 0.0;
    if (stats.mean < 0.16)
        exposure = std::min(0.35, std::log2(0.18 / stats.mean));

    // Protect the brightest content from clipping.
    if (stats.p995 > 0.0)
        exposure = std::min(exposure, std::max(0.0, std::log2(0.98 / stats.p995)));

    // A deliberately dark scene should stay dark: only a small lift.
    if (stats.p995 < 0.22)
        exposure = std::min(exposure, 0.15);

    // A bright or correctly exposed frame needs no help.
    if (stats.mean >= 0.45)
        exposure = 0.0;

    exposure = std::clamp(exposure, 0.0, 0.35);
    tone.exposureEv = exposure;

    if (exposure > 0.0)
        tone.shadowLift = std::clamp((0.16 - stats.p50) * 0.6, 0.0, 0.12);
    if (stats.p995 > 0.95)
        tone.highlightCompress = std::min(0.06, (stats.p995 - 0.95) * 1.2);
    return tone;
}

} // namespace pp
