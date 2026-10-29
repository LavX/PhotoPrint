#include "ImageSource.h"

#include "ColorPipeline.h"
#include "PhotoFilters.h"

#include <QCryptographicHash>
#include <QCache>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <utility>

namespace pp {

namespace {

QByteArray markerScan(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray head = file.read(1024 * 1024);
    return head;
}

QString cacheKey(const QString &mode, const QString &path, const QByteArray &sha,
                 const ColorRecipe &recipe, int maxDimension)
{
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(mode, path, QString::fromLatin1(sha.toHex()),
             QString::fromLatin1(recipe.digest().toHex()))
        .arg(maxDimension);
}

} // namespace

QByteArray fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return hash.result();
}

struct ImageStore::Private {
    QHash<QString, SourceImage> originals;
    QCache<QString, QImage> previews;
    QMutex mutex;

    Private() : previews(320 * 1024) {} // cost unit is 1 KiB
};

ImageStore::ImageStore() : d(std::make_unique<Private>()) {}

ImageStore::~ImageStore() = default;

Result<SourceImage> ImageStore::load(const QString &path, const ImportPolicy &policy,
                                     std::stop_token token)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                                 : info.canonicalFilePath();
    {
        QMutexLocker locker(&d->mutex);
        auto it = d->originals.constFind(canonical);
        if (it != d->originals.constEnd()) {
            const QFileInfo current(canonical);
            if (current.exists()
                && current.lastModified().toMSecsSinceEpoch() == it->fileMtimeMs
                && current.size() == it->fileSize)
                return *it;
            d->originals.remove(canonical);
        }
    }
    if (token.stop_requested())
        return error(QStringLiteral("cancelled"), QStringLiteral("A művelet megszakadt."));

    QFile file(canonical);
    if (!file.exists())
        return error(QStringLiteral("source-missing"), QStringLiteral("Ezt a fotót nem találjuk."));

    const QByteArray sha = fileSha256(canonical);
    if (sha.isEmpty())
        return error(QStringLiteral("source-unreadable"), QStringLiteral("Ezt a fotót nem találjuk."));

    const QByteArray head = markerScan(canonical);
    const bool claimsProfile = head.contains("ICC_PROFILE") || head.contains("iCCP");

    QImageReader reader(canonical);
    reader.setAutoTransform(true);
    QImage pixels = reader.read();
    if (pixels.isNull()) {
        return error(QStringLiteral("unsupported-image"),
                     QStringLiteral("Ezt a képet most nem tudjuk megnyitni. Próbálj JPG- vagy "
                                    "PNG-másolatot."));
    }

    SourceImage source;
    source.path = canonical;
    source.sha256 = sha;
    source.hasEmbeddedProfile = claimsProfile || pixels.colorSpace().isValid();
    const QColorSpace embedded = pixels.colorSpace();
    if (embedded.isValid()) {
        source.sourceSpace = embedded;
        source.profileValid = true;
    } else {
        source.profileValid = false;
        source.srgbFallback = true;
        source.sourceSpace = QColorSpace(QColorSpace::SRgb);
        if (!policy.allowSrgbFallback && claimsProfile) {
            return error(QStringLiteral("profile-unreadable"),
                         QStringLiteral("Ezt a képet most nem tudjuk megnyitni. Próbálj JPG- vagy "
                                        "PNG-másolatot."));
        }
    }
    if (!policy.allowEmbeddedProfile && source.hasEmbeddedProfile) {
        source.profileValid = false;
        source.srgbFallback = true;
        source.sourceSpace = QColorSpace(QColorSpace::SRgb);
    }
    const QFileInfo stamp(canonical);
    source.fileSize = stamp.size();
    source.fileMtimeMs = stamp.lastModified().toMSecsSinceEpoch();
    source.displayP3 = source.profileValid
        && source.sourceSpace.primaries() == QColorSpace::Primaries::DciP3D65;
    source.pixels = pixels;

    {
        QMutexLocker locker(&d->mutex);
        d->originals.insert(canonical, source);
    }
    return source;
}

const SourceImage *ImageStore::cached(const QString &path) const
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                                 : info.canonicalFilePath();
    QMutexLocker locker(&d->mutex);
    auto it = d->originals.constFind(canonical);
    return it == d->originals.constEnd() ? nullptr : &it.value();
}

Result<SourceImage> ImageStore::reloadVerified(const QString &path, const QByteArray &sha256,
                                               std::stop_token token)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                                 : info.canonicalFilePath();
    if (!QFileInfo::exists(canonical))
        return error(QStringLiteral("source-missing"), QStringLiteral("Ezt a fotót nem találjuk."));
    const QByteArray current = fileSha256(canonical);
    if (current.isEmpty())
        return error(QStringLiteral("source-unreadable"), QStringLiteral("Ezt a fotót nem találjuk."));
    if (current != sha256) {
        return error(QStringLiteral("source-changed"),
                     QStringLiteral("Ez a fotó megváltozott. Add meg újra, ha ezt szeretnéd "
                                    "nyomtatni."));
    }
    ImportPolicy policy;
    return load(canonical, policy, token);
}

Result<QImage> ImageStore::srgb(const SourceImage &source, const ColorRecipe &recipe, int maxDimension,
                                std::stop_token token)
{
    const QString key = cacheKey(QStringLiteral("s"), source.path, source.sha256, recipe, maxDimension);
    {
        QMutexLocker locker(&d->mutex);
        if (QImage *hit = d->previews.object(key)) {
            QImage copy = *hit;
            return copy;
        }
    }
    QImage converted = ColorPipeline::toSrgb(source, recipe, maxDimension, token);
    if (converted.isNull())
        return error(QStringLiteral("colour-conversion"),
                     QStringLiteral("Ezt a képet most nem tudjuk feldolgozni. Próbáld újra."));
    {
        QMutexLocker locker(&d->mutex);
        d->previews.insert(key, new QImage(converted),
                           std::max(1, static_cast<int>(converted.sizeInBytes() / 1024)));
    }
    return converted;
}

Result<QImage> ImageStore::preparedLinear(const SourceImage &source, const ColorRecipe &recipe,
                                          const QString &filterKey, int filterStrength,
                                          bool upscale, int maxDimension, std::stop_token token)
{
    const FilterPreset preset = filterFromKey(filterKey);
    if (preset == FilterPreset::Original && !upscale)
        return linear(source, recipe, maxDimension, token);

    const QString mode = QStringLiteral("f|%1|%2|%3")
                             .arg(filterKey)
                             .arg(filterStrength)
                             .arg(upscale ? 1 : 0);
    const QString key = cacheKey(mode, source.path, source.sha256, recipe, maxDimension);
    {
        QMutexLocker locker(&d->mutex);
        if (QImage *hit = d->previews.object(key)) {
            QImage copy = *hit;
            return copy;
        }
    }

    Result<QImage> base = linear(source, recipe, maxDimension, token);
    if (!base)
        return base.error();
    QImage srgb = ColorPipeline::linearToSrgbWithAlpha(base.value(), token);
    if (srgb.isNull())
        return error(QStringLiteral("colour-conversion"),
                     QStringLiteral("Ezt a képet most nem tudjuk feldolgozni. Próbáld újra."));
    if (upscale && maxDimension == 0) {
        const QImage enlarged = PhotoFilters::upscale(srgb, 2);
        if (!enlarged.isNull())
            srgb = enlarged;
    }
    if (preset != FilterPreset::Original) {
        const QImage filtered = PhotoFilters::apply(srgb, preset, filterStrength);
        if (!filtered.isNull())
            srgb = filtered;
    }
    const QImage linearResult = ColorPipeline::srgbToLinear(srgb, token);
    if (linearResult.isNull())
        return error(QStringLiteral("colour-conversion"),
                     QStringLiteral("Ezt a képet most nem tudjuk feldolgozni. Próbáld újra."));
    {
        QMutexLocker locker(&d->mutex);
        d->previews.insert(key, new QImage(linearResult),
                           std::max(1, static_cast<int>(linearResult.sizeInBytes() / 1024)));
    }
    return linearResult;
}

Result<QImage> ImageStore::linear(const SourceImage &source, const ColorRecipe &recipe,
                                  int maxDimension, std::stop_token token)
{
    const QString key = cacheKey(QStringLiteral("l"), source.path, source.sha256, recipe, maxDimension);
    {
        QMutexLocker locker(&d->mutex);
        if (QImage *hit = d->previews.object(key)) {
            QImage copy = *hit;
            return copy;
        }
    }
    QImage converted = ColorPipeline::toLinear(source, recipe, maxDimension, token);
    if (converted.isNull())
        return error(QStringLiteral("colour-conversion"),
                     QStringLiteral("Ezt a képet most nem tudjuk feldolgozni. Próbáld újra."));
    {
        QMutexLocker locker(&d->mutex);
        d->previews.insert(key, new QImage(converted),
                           std::max(1, static_cast<int>(converted.sizeInBytes() / 1024)));
    }
    return converted;
}

void ImageStore::clear()
{
    QMutexLocker locker(&d->mutex);
    d->originals.clear();
    d->previews.clear();
}

} // namespace pp
