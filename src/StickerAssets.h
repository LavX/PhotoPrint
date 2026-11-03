#pragma once

#include "Document.h"
#include "ImageSource.h"

#include <QImage>
#include <QString>

#include <stop_token>

namespace pp {

/// The prefix a shipped sticker's stored path carries instead of a file path.
/// A project stores the alias, so moving the project, wiping the app's data
/// folder or updating the app cannot break the layer.
inline constexpr char kBuiltinStickerPrefix[] = "builtin:";

/// The artwork version a new sticker is pinned to. A future artwork change
/// bumps this, ships the new file as `<alias>-v<new>.png`, and keeps the old
/// one, so a project saved at an older version still resolves to the pixels it
/// was saved with. The resolver accepts any version whose file ships; the
/// version is compared with the stored sha, not with this constant.
constexpr int kBuiltinStickerVersion = 1;

/// True when the stored path is a shipped-sticker alias rather than a file.
bool isBuiltinStickerPath(const QString &path);

/// The alias a new sticker of a shipped shape is stored with, e.g.
/// `builtin:star@1`.
QString builtinStickerPath(const QString &alias);

/// The resource a stored alias resolves to, or an empty string when the alias
/// or its pinned version is unknown. The version is part of the answer on
/// purpose: a build that ships new artwork must add a new versioned file, and
/// an old project keeps resolving to the file it was saved with.
QString builtinStickerResource(const QString &path);

/// The file a sticker's pixels come from: the shipped resource for an alias,
/// the stored path for an imported picture. Empty when an alias cannot be
/// resolved, which the caller reports as a missing picture.
QString stickerSourcePath(const Sticker &sticker);

/// The folder a relink dialog should open in. A shipped sticker lives in the
/// resource tree, which no file dialog can show, so it falls back to the last
/// folder the user picked from; an imported picture starts beside its file.
QString stickerPickerDirectory(const Sticker &sticker, const QString &lastDir);

/// One decoded sticker layer, or the reason there is none.
struct StickerLayer {
    QImage image; ///< linear premultiplied, null when missing
    bool missing = false; ///< the source is absent, unreadable or too large
};

/// Decodes, verifies and colour converts one sticker. Shared by the print
/// renderer and the preview job, so the two cannot drift apart: the same
/// source, the same working space, the same missing rule. `maxDimension` is
/// the layer bound the caller computed for its own resolution.
StickerLayer loadStickerLayer(ImageStore &store, const Sticker &sticker,
                              const ColorRecipe &recipe, int maxDimension,
                              std::stop_token token = {});

} // namespace pp
