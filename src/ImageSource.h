#pragma once

#include "Document.h"
#include "Result.h"

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QString>

#include <memory>
#include <optional>
#include <stop_token>

namespace pp {

/// Import decisions that never become a question on screen. The normal import
/// accepts the first valid interpretation and reports what it used in the
/// developer evidence.
struct ImportPolicy {
    bool allowSrgbFallback = true;
    bool allowEmbeddedProfile = true;
};

/// A decoded photograph: EXIF orientation already applied, pixels untouched in
/// their own colour space, plus the identity of the original file.
struct SourceImage {
    QString path;
    QByteArray sha256;
    QImage pixels;             ///< oriented pixels, source colour values
    QColorSpace sourceSpace;   ///< from the valid embedded ICC, else sRGB
    bool hasEmbeddedProfile = false;
    bool profileValid = true;
    bool displayP3 = false;
    bool srgbFallback = false;
    bool sdrAssumed = true;
    qint64 fileSize = 0;
    qint64 fileMtimeMs = 0;

    QSize size() const { return pixels.size(); }
};

/// Loads and caches decoded originals and derived sRGB previews. The original
/// file is only ever read.
class ImageStore
{
public:
    ImageStore();
    ~ImageStore();
    ImageStore(const ImageStore &) = delete;
    ImageStore &operator=(const ImageStore &) = delete;

    Result<SourceImage> load(const QString &path, const ImportPolicy &policy,
                             std::stop_token token = {});
    /// A copy of the cached entry, or nothing when the path was never loaded.
    /// The copy is taken under the lock, so it stays valid when another thread
    /// loads or clears the store; the pixels are shared copy-on-write.
    std::optional<SourceImage> cached(const QString &path) const;

    /// Re-reads the file and checks that the content still has the recorded
    /// identity. A changed or missing file never silently reuses a cache entry.
    Result<SourceImage> reloadVerified(const QString &path, const QByteArray &sha256,
                                       std::stop_token token = {});

    /// sRGB pixels for screen or print. `maxDimension` 0 keeps the full source
    /// resolution; a positive bound produces a smaller preview. Cached by file
    /// identity, recipe and bound.
    Result<QImage> srgb(const SourceImage &source, const ColorRecipe &recipe, int maxDimension,
                        std::stop_token token = {});

    /// Linear Rec.2020 premultiplied layer for page compositing.
    Result<QImage> linear(const SourceImage &source, const ColorRecipe &recipe, int maxDimension,
                          std::stop_token token = {});

    /// Linear layer with the frame's filter and optional 2x enlarging applied.
    /// Upscaling is skipped for bounded previews; the print path passes 0.
    Result<QImage> preparedLinear(const SourceImage &source, const ColorRecipe &recipe,
                                  const QString &filterKey, int filterStrength, bool upscale,
                                  int maxDimension, std::stop_token token = {});

    void clear();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

QByteArray fileSha256(const QString &path);

} // namespace pp
