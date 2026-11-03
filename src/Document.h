#pragma once

#include <QByteArray>
#include <QColorSpace>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <optional>

namespace pp {

/// How a photograph is mapped into its frame rectangle.
enum class FitMode {
    Full, ///< the whole photograph is visible, empty bands keep the paper colour
    Fill  ///< the frame is filled, the overflow is cropped away
};

/// Versioned recipe for the image-side tone correction.
struct PhotoTone {
    double exposureEv = 0.0;
    double shadowLift = 0.0;
    double highlightCompress = 0.0;
    bool operator==(const PhotoTone &) const = default;
};

/// One photograph the page no longer shows but the library still keeps. The
/// pair is exactly the identity a frame carries, so a photograph that is back
/// on the page is the same source and the repair point drops the duplicate
/// entry instead of showing it twice.
struct PhotoSource {
    QString path;
    QByteArray sha256;
    bool operator==(const PhotoSource &) const = default;
};

/// One photograph placed on one page. All page geometry is millimetres,
/// origin at the top-left corner of the physical sheet, X to the right,
/// Y downwards. The frame is the visible cut-out rectangle on the paper.
struct Frame {
    QString id;
    QString sourcePath;      ///< absolute path of the original file
    QByteArray sourceSha256; ///< identity of the original file content
    double cx = 0.0;         ///< frame centre X in mm
    double cy = 0.0;         ///< frame centre Y in mm
    double width = 0.0;      ///< frame width in mm
    double height = 0.0;     ///< frame height in mm
    double angleDeg = 0.0;   ///< frame rotation, clockwise, about the centre
    double imageRotationDeg = 0.0; ///< photograph orientation inside the frame
    double zoom = 1.0;       ///< extra magnification above fit/fill
    QPointF cropCenter{0.5, 0.5}; ///< normalised centre of the visible crop
    FitMode fit = FitMode::Full;
    bool autoPlaced = true; ///< false after a manual move, resize or crop
    PhotoTone tone;         ///< image component suggested by the tone engine
    bool autoTone = true;
    double brightness = 0.0; ///< manual offset in EV, -0.5 .. +0.5
    bool paperOverride = false; ///< trial variants carry their own paper tone
    double paperEv = 0.0;
    QString label; ///< optional number on a trial sheet
    QString filter = QStringLiteral("original"); ///< PhotoFilters key or catalogue effect key
    int filterStrength = 100;                    ///< 0..100
    /// Catalogue recipe version. 0 follows the newest shipped version, which is
    /// what an old project or a freshly applied effect stores. A retune adds a
    /// version and old projects keep the one they were saved with. Both render
    /// paths read this field; the browser slice is what writes recipe.version
    /// when an effect is applied, so it is still 0 for every existing frame.
    int effectVersion = 0;
    bool upscale = false;                        ///< 2x Lanczos before print
    /// Empty for a free frame. Otherwise the slot on the page whose cell clips
    /// this frame. The slot is a clipping window, not a box that owns the
    /// photograph: the frame keeps its own centre, size and angle, so the photo
    /// may be smaller than the cell (paper shows around it) or larger (the
    /// overflow is cut away). A dangling id (a slot that no longer exists) is
    /// cleared by SlotLayoutEngine::repair, which turns the frame back into a
    /// free one instead of leaving an unusable reference behind.
    QString slotId;

    bool operator==(const Frame &) const = default;
};

/// One fixed area of a laid-out page. `unitRect` is the mapping source of truth:
/// 0..1 of the page's content box, origin top-left, x right, y down, the same
/// convention CollageSlot uses. The millimetre rectangle is a pure function of
/// this and the page's paper, so a paper or orientation change re-maps without
/// touching the stored layout. The cell is the clipping window of the
/// photograph assigned to it; the photograph's own rectangle lives on the frame.
struct Slot {
    QString id;                    ///< stable while the layout lives, unique per page
    QRectF unitRect{0.0, 0.0, 1.0, 1.0};
    bool operator==(const Slot &) const = default;
};

/// How a laid-out page is spaced. Every value has a neutral default, so a page
/// that never chose a spacing stores nothing and keeps today's geometry. A
/// missing margin means "automatic" (the catalogue's 5 %, 4..10 mm), not zero:
/// an explicit zero is a real choice the panel can make with automatic off.
struct SlotStyle {
    /// Missing means "automatic": 5 % of the short side, clamped 4..10 mm. An
    /// explicit zero is a real zero margin, so the panel can turn the automatic
    /// value off and still choose no margin at all.
    std::optional<double> marginMm;
    /// The white paper between two neighbouring photographs. Missing means
    /// "equal to the effective margin"; an explicit zero lets two cells touch.
    /// This is the border the panel exposes: the cells are the clipping windows
    /// and the gap is the paper between them.
    std::optional<double> gutterMm;
    double cornerRadiusMm = 0.0; ///< rounded photograph, 0 = square
    bool operator==(const SlotStyle &) const = default;
};

/// Bounds of the extra magnification of the photograph inside its frame. The
/// wheel gesture and the model share these, so neither can drift away from the
/// range the renderer clamps to.
constexpr double kMinFrameZoom = 0.2;
constexpr double kMaxFrameZoom = 8.0;

/// Bounds of the photograph's own rotation inside its frame. The inspector's
/// slider shows exactly this range, so a slider position is always a stored
/// value.
constexpr double kMinImageRotationDeg = -180.0;
constexpr double kMaxImageRotationDeg = 180.0;

/// Wraps any angle into (-180, 180]. Used by the gestures that rotate freely
/// (the quarter-turn buttons); the slider itself clamps instead, so a
/// deliberate -180 stays -180.
inline double normalizedImageRotationDeg(double degrees)
{
    if (!std::isfinite(degrees))
        return 0.0;
    double value = std::fmod(degrees, 360.0);
    if (value > 180.0)
        value -= 360.0;
    if (value <= -180.0)
        value += 360.0;
    return value;
}

/// Bounds of the manual brightness offset, in EV. The slider and the colour
/// pipeline share this range, so the stores clamp to it on the way in and the
/// tone path clamps again on the way out. The slider positions and the EV
/// values are converted only by the two helpers below, so the panel cannot
/// drift away from the range.
constexpr double kMinBrightnessEv = -0.5;
constexpr double kMaxBrightnessEv = 0.5;

inline double clampBrightnessEv(double ev)
{
    // NaN has no direction to saturate towards and would poison every pixel, so
    // it becomes the neutral offset. An infinity does have one: std::clamp
    // pulls it to the nearer bound instead of silently dropping the request.
    //
    // That is the opposite of sanitiseTone() below, and on purpose. Brightness
    // is a manual slider: a hand-edited +inf reads as "as bright as the slider
    // goes", a request somebody made, and it saturates to the top step. The
    // automatic tone is a measurement of the photograph, so an infinite
    // suggestion is not an extreme request but a broken number, and it becomes
    // neutral instead.
    if (std::isnan(ev))
        return 0.0;
    return std::clamp(ev, kMinBrightnessEv, kMaxBrightnessEv);
}

/// Sane bounds for the image tone. The tone engine suggests at most +0.35 EV
/// exposure, 0.12 shadow lift and 0.06 highlight compression; these bounds
/// leave room for a hand-edited recipe or a future stronger suggestion while
/// keeping the exposure two stops at most, far from the point where the
/// pipeline clips every frame to black or white.
constexpr double kMinToneExposureEv = -1.0;
constexpr double kMaxToneExposureEv = 1.0;
constexpr double kMaxToneShadowLift = 0.5;
constexpr double kMaxToneHighlightCompress = 0.35;

/// The tone the renderer may see. Values loaded from a project file are
/// untrusted: every finite field is clamped to its range, and a non-finite one
/// is rejected in favour of the neutral value rather than saturating, because
/// an infinity here is corruption, not a direction a person chose. The manual
/// brightness offset is the documented exception; see clampBrightnessEv().
PhotoTone sanitiseTone(const PhotoTone &tone);

/// Slider positions for the manual brightness control: whole steps, 0..100,
/// with the neutral position in the middle.
constexpr int kBrightnessSliderMin = 0;
constexpr int kBrightnessSliderNeutral = 50;
constexpr int kBrightnessSliderMax = 100;

/// Brightness offset for a slider position.
inline double sliderToBrightnessEv(int slider)
{
    return (std::clamp(slider, kBrightnessSliderMin, kBrightnessSliderMax)
            - kBrightnessSliderNeutral)
        / 100.0;
}

/// Slider position for a brightness offset; the inverse of the above.
inline int brightnessEvToSlider(double ev)
{
    return std::clamp(
        int(std::lround(kBrightnessSliderNeutral + clampBrightnessEv(ev) * 100.0)),
        kBrightnessSliderMin, kBrightnessSliderMax);
}

/// What the sheet is made of. The driver has no separate matt mode; matt
/// papers are sent as inkjet paper (see PrinterAdapter).
enum class PaperType {
    Plain,  ///< ordinary office paper
    Glossy, ///< photo paper with a glossy surface
    Matte   ///< photo paper with a matt surface
};

/// Physical sheet plus the logical size the printer driver expects.
/// Slice 1 uses physical size only; later slices fill the logical fields.
struct PaperSpec {
    QString name;            ///< everyday name, e.g. "A4" or "10 × 15 cm"
    double physicalWidthMm = 210.0;
    double physicalHeightMm = 297.0;
    double logicalWidthMm = 0.0;   ///< 0 means "same as physical"
    double logicalHeightMm = 0.0;
    bool borderlessSupported = false;
    bool hasCheckedOffset = false;
    double offsetXMm = 0.0;
    double offsetYMm = 0.0;
    PaperType type = PaperType::Plain;
    bool custom = false;     ///< added by the user, not one of the fixed papers

    /// Photo papers are exactly the non-plain types; there is no second flag
    /// that could drift out of sync.
    bool isPhotoPaper() const { return type != PaperType::Plain; }

    bool operator==(const PaperSpec &) const = default;
};

/// The cutting aid a page shows on its own frame edges. `None` is the default
/// and the only state a fresh page or an opened 1.0.1 project can get without
/// an explicit choice; a corrupt project also falls back to it, so a bad file
/// never puts marks on the paper.
enum class CutGuides {
    None,   ///< nothing on the screen and nothing on the print
    Screen, ///< a dashed overlay on the screen only
    Print   ///< a thin grey line on the screen and on the print
};

/// Whether the page prints borderless, overriding the printer's own choice.
/// `Auto` follows the printer and the paper; `Off` and `On` pin the decision
/// per page, so a later printer default cannot change a saved composition.
enum class BorderlessOverride { Auto, Off, On };

/// What is painted under the photographs. `None` is the white paper every
/// existing project, batch sheet and trial sheet already has, so a default
/// `PageBackground` leaves a page exactly as it was before the feature.
enum class BackgroundKind { None, Solid, Gradient, Image };

/// How a background picture is mapped onto the sheet. It is deliberately not
/// `FitMode`: a photograph's fit is about its frame, and adding `Tile` there
/// would let a frame ask for a mode the frame renderer cannot draw.
enum class BackgroundFit {
    Fill, ///< cover the sheet, the overflow is cropped away
    Fit,  ///< the whole picture is visible, paper shows in the bands
    Tile  ///< repeat at `tileMm`, anchored at the sheet's top-left corner
};

/// One colour stop of a gradient. The position is 0..1 along the gradient line
/// and the colour is sRGB with alpha forced opaque: transparency is the
/// background's own `opacity`, so there is exactly one place that decides how
/// see-through the layer is. A plain pair rather than a struct because the
/// effect catalogue already owns the name `GradientStop` for its own recipe.
using BackgroundStop = QPair<double, QRgb>;

/// The page's own background. Colours are stored as `QRgb` rather than `QColor`
/// because `QColor::operator==` compares the colour spec as well as the value:
/// a colour picked in the dialog's HSV tab and the same colour loaded from
/// `#rrggbb` would compare unequal, and the whole document's value comparison
/// (and its undo no-op guard) depends on an exact one.
///
/// Every field has a neutral default, so `PageBackground{}` is the white sheet
/// and a page that never chose a background writes nothing. The fields are kept
/// when the kind changes back to `None`, so a user who tries a picture and
/// returns to white does not lose the picture path.
struct PageBackground {
    BackgroundKind kind = BackgroundKind::None;
    QRgb solid = 0xffffffffu; ///< sRGB, alpha forced opaque
    /// Gradient stops, sorted by position on load. Empty means "white to
    /// white", which is what a freshly chosen gradient shows until a colour is
    /// picked. A list rather than two colours so a future build can add stops
    /// without a schema change.
    QList<BackgroundStop> stops;
    double angleDeg = 0.0; ///< clockwise, 0 = left to right
    QString imagePath;     ///< resolved absolute path, like Frame::sourcePath
    QByteArray imageSha256; ///< identity of the file content
    BackgroundFit fit = BackgroundFit::Fill;
    double tileMm = 0.0; ///< 0 = the source's natural size at kTileNaturalPpi
    double opacity = 100.0; ///< 0..100 over the white paper base
    bool operator==(const PageBackground &) const = default;
};

/// Bounds the renderer and the panel share, so neither can drift away from the
/// other.
constexpr int kMaxGradientStops = 8;
constexpr int kTileNaturalPpi = 300; ///< tileMm == 0 means this resolution
constexpr double kMaxTileMm = 1000.0;
constexpr int kMaxTilesPerAxis = 64; ///< tileMm is raised until the count fits

/// The background the renderer may see. Values loaded from a project file are
/// untrusted: every finite field is clamped to its range, a non-finite one
/// becomes the neutral value, an unknown enum becomes the default, and every
/// colour is forced opaque. Called from SlotLayoutEngine::repair, the single
/// invariant point every mutation and the loader pass through.
PageBackground sanitiseBackground(PageBackground background);

/// One decorative picture placed on a page, above every photograph. The box is
/// stored on both axes, like a Frame: the renderer draws into it, so a
/// hand-edited file that disagrees with the picture's aspect stretches it. The
/// editor never produces that, because every gesture keeps the pixel aspect.
/// `z` orders the stickers among themselves; photographs keep their own stack
/// underneath, so a sticker can never be lost behind a photo.
struct Sticker {
    QString id; ///< makeId("sticker"), unique on its page
    QString imagePath; ///< resolved absolute path, like Frame::sourcePath
    QByteArray imageSha256; ///< identity of the file content
    double cx = 0.0; ///< centre X in mm
    double cy = 0.0; ///< centre Y in mm
    double width = 0.0; ///< box width in mm
    double height = 0.0; ///< box height in mm
    double angleDeg = 0.0; ///< clockwise about the centre, (-180, 180]
    int opacity = 100; ///< 0..100 over the page beneath it
    int z = 0; ///< paint order among the page's stickers, ascending
    bool operator==(const Sticker &) const = default;
};

/// Bounds shared by the model, the renderer and the panel, so none of them can
/// drift away from the others. A page cap bounds a corrupt file; the minimum
/// size keeps every sticker big enough to be clicked again; the z range keeps a
/// hand-edited layer order representable.
constexpr int kMaxStickersPerPage = 64;
constexpr double kMinStickerMm = 2.0;
constexpr double kMaxStickerMm = 2000.0;
constexpr int kMinStickerZ = -10000;
constexpr int kMaxStickerZ = 10000;

/// The sticker the renderer and the editor may see. Finite geometry is clamped
/// to the paper and to the size range, a non-finite one becomes a drawable
/// value rather than vanishing, and an empty or duplicated id is dropped by the
/// caller. Called from SlotLayoutEngine::repair with the page's own size, the
/// same single repair point the background and the slot style use.
Sticker sanitiseSticker(Sticker sticker, const QSizeF &sheetMm);

/// A sticker box that fits the sheet without changing its aspect. The longest
/// side is bounded by the page diagonal (and by kMaxStickerMm), the shortest by
/// kMinStickerMm, and both bounds are applied as one uniform scale, so a tall
/// picture is made smaller instead of being squashed on one axis. Shared by the
/// sanitiser, the default placement and the relink picker.
QSizeF boundedStickerSizeMm(QSizeF size, const QSizeF &sheetMm);

/// The page's sticker list after the same rule: at most kMaxStickersPerPage
/// entries, each with a non-empty unique id, each sanitised against the sheet.
/// Document order is kept, because it is the tie-break for equal `z`.
QList<Sticker> sanitiseStickers(const QList<Sticker> &stickers, const QSizeF &sheetMm);

/// How the lines of a text block line up inside the block's own width. With a
/// single line the three values look identical, which is fine and needs no
/// explanation.
enum class TextAlign { Left, Center, Right };

/// One text block placed on a page, above every photograph. The block is not
/// stored: its size is a pure function of the text, the font and the size, so a
/// stored height could only go stale and clip glyphs. `cx`/`cy` is the centre of
/// the laid-out block, which is why adding a line grows the caption
/// symmetrically instead of pushing it down.
///
/// The colour is a normalised `QRgb` rather than a `QColor` for the same reason
/// the background is: `QColor::operator==` compares the colour spec as well as
/// the value, and the whole document's value comparison (and its undo no-op
/// guard) depends on an exact one. Alpha is not stored per colour; transparency
/// is the item's `opacity`.
struct TextItem {
    QString id; ///< makeId("text"), unique on its page
    QString text; ///< plain text, '\n' is a hard line break, never wrapped
    QString fontFamily; ///< empty = the application font
    double fontPt = 12.0; ///< 4..400
    QRgb color = 0xff000000u; ///< sRGB, alpha forced opaque
    bool bold = false;
    bool italic = false;
    bool underline = false;
    TextAlign align = TextAlign::Center;
    double lineSpacing = 1.0; ///< 0.8..2.5, a multiple of the font's line height
    double cx = 0.0; ///< centre of the block in mm
    double cy = 0.0;
    double angleDeg = 0.0; ///< clockwise about the centre, (-180, 180]
    int opacity = 100; ///< 0..100 over the page beneath it
    int z = 0; ///< paint order among the page's overlays, ascending
    bool operator==(const TextItem &) const = default;
};

/// Bounds shared by the model, the renderer and the panel. The page cap bounds a
/// corrupt file; the font range is what the size spin offers; the line spacing
/// range keeps a hand-edited file from collapsing the block or scattering it.
constexpr int kMaxTextsPerPage = 64;
constexpr double kMinFontPt = 4.0;
constexpr double kMaxFontPt = 400.0;
constexpr double kMinLineSpacing = 0.8;
constexpr double kMaxLineSpacing = 2.5;
/// The smallest block a text may have on either axis. A smaller one could not be
/// clicked again, so the sanitiser raises the font until the block reaches it.
constexpr double kMinTextBlockMm = 2.0;
constexpr int kMinTextZ = -10000;
constexpr int kMaxTextZ = 10000;

/// How many removed photographs one page's library keeps. The cap bounds a
/// corrupt or hand-edited file; the library is a working set, not an archive.
constexpr int kMaxUnusedPhotos = 256;

/// The text the renderer and the editor may see. `\r\n` and a lone `\r` become
/// `\n`, every finite field is clamped to its range, a non-finite one becomes the
/// neutral value, an unknown alignment becomes the default, the colour is forced
/// opaque, and the centre is clamped onto the paper so the block can always be
/// clicked. Called from SlotLayoutEngine::repair, the single repair point every
/// mutation and the loader pass through.
TextItem sanitiseText(TextItem text, const QSizeF &sheetMm);

/// The page's text list after the same rule: at most kMaxTextsPerPage entries,
/// each with a non-empty unique id, each sanitised against the sheet. A text
/// that is empty or only whitespace is dropped: the page never carries an
/// invisible layer, and committing an emptied editor deletes the item.
QList<TextItem> sanitiseTexts(const QList<TextItem> &texts, const QSizeF &sheetMm);

struct Page {
    QString id;
    PaperSpec paper;
    bool landscape = false;
    QList<Frame> frames;
    CutGuides cutGuides = CutGuides::None;
    BorderlessOverride borderlessOverride = BorderlessOverride::Auto;
    /// The slots of one laid-out page, in fill order. Empty means a free canvas:
    /// every existing project, every batch sheet and every trial sheet lands
    /// here, and a free-canvas page serialises exactly as it did before slots
    /// existed. The field is not called `slots` because Qt reserves that word as
    /// a macro in every translation unit that includes a QObject header.
    QList<Slot> slotList;
    SlotStyle slotStyle;
    /// Catalogue id of the applied layout. A label only: it is never resolved
    /// for rendering, so a renamed catalogue entry cannot break a project. Empty
    /// for a hand-made layout.
    QString layoutId;
    /// The sheet's own background, painted under the photographs. The default
    /// is the white paper, so a page that never chose one is unchanged.
    PageBackground background;
    /// The page's stickers, painted above every photograph in `z` order. The
    /// empty list is the default, so a page without stickers is unchanged and
    /// serialises exactly as it did before the feature.
    QList<Sticker> stickers;
    /// The page's text blocks. They share one `z` space with the stickers, so a
    /// caption can sit above or below a decoration; both are always above the
    /// photographs. The empty list is the default, so a page without text
    /// serialises exactly as it did before the feature.
    QList<TextItem> texts;
    /// Photographs that were taken off the page but stay in the library. Empty
    /// is the default, so every page written before the library kept removed
    /// photos is unchanged and loads exactly as it did. The repair point drops
    /// an entry a frame uses again, so undo needs no second bookkeeping.
    QList<PhotoSource> unusedPhotos;

    bool hasLayout() const { return !slotList.isEmpty(); }
    bool operator==(const Page &) const = default;
};

/// True when two sources are the same photograph for library purposes. The
/// path is the identity; the hash only distinguishes two different contents
/// stored under the same path. An empty hash is a wildcard, so a hand-edited
/// file or a frame with no recorded identity cannot make one path appear twice.
inline bool samePhotoSource(const PhotoSource &a, const PhotoSource &b)
{
    if (a.path != b.path)
        return false;
    return a.sha256.isEmpty() || b.sha256.isEmpty() || a.sha256 == b.sha256;
}

/// True when a frame on the page still shows this photograph. The library entry
/// and the frame carry the same pair, so this is the one rule that decides
/// whether a removed source is really unused.
inline bool pageUsesSource(const Page &page, const PhotoSource &source)
{
    for (const Frame &frame : page.frames) {
        if (samePhotoSource(PhotoSource{frame.sourcePath, frame.sourceSha256}, source))
            return true;
    }
    return false;
}

/// Project keys for the two optional page fields. A missing or unrecognised key
/// loads as the field's default, and the writer omits a default value, so an
/// older build sees a page it already understands.
QString cutGuidesKey(CutGuides value);
CutGuides cutGuidesFromKey(const QString &key);
QString borderlessOverrideKey(BorderlessOverride value);
BorderlessOverride borderlessOverrideFromKey(const QString &key);

/// Document-wide colour recipe. The image component is taken per frame from
/// `effectiveTone`; the paper component is added once at export.
struct ColorRecipe {
    int version = 1;
    bool autoEnhance = false;
    PhotoTone tone;
    double paperStrength = 0.0; ///< paper component, still zero in slices 1-6
    /// Output is rendered as neutral grey: luminance in linear space, before
    /// the sRGB conversion, so preview and PDF agree.
    bool grayscale = false;
    bool operator==(const ColorRecipe &) const = default;
    QByteArray digest() const;
};

/// Clamps the recipe values that can arrive from a project file. The renderer
/// applies the paper component as std::pow(2, paperStrength) to every pixel, so
/// nothing outside these bounds may pass. The preview clamps in MainWindow; the
/// renderer clamps again at its own boundary, because a crafted preset or a
/// caller that filled the value in directly would otherwise print a page the
/// preview never showed.
constexpr double kMaxPaperEv = 0.5;

/// Range of the trial sheet's paper steps. The builder offers these values and
/// the project loader clamps loaded frames to them, so one constant decides
/// both sides of that round trip.
constexpr double kTrialPaperEv = 0.12;

inline ColorRecipe sanitiseRecipe(ColorRecipe recipe)
{
    recipe.tone = sanitiseTone(recipe.tone);
    // Same rule as the tone above: a non-finite paper component is corruption,
    // not a direction to saturate towards, so it becomes neutral instead of the
    // strongest cast. A finite value is clamped, because an extreme one is a
    // request that has an obvious nearest allowed value.
    recipe.paperStrength = std::isfinite(recipe.paperStrength)
        ? std::clamp(recipe.paperStrength, -kMaxPaperEv, kMaxPaperEv)
        : 0.0;
    return recipe;
}

/// Immutable-by-convention document state. Editing code copies, changes and
/// publishes a new snapshot; the print path records one and never edits it.
struct DocumentSnapshot {
    QList<Page> pages;
    int currentPage = 0;
    ColorRecipe recipe;
    /// Print quality choice: true is the best setting the driver offers.
    bool fineQuality = true;

    bool operator==(const DocumentSnapshot &) const = default;
};

inline QString makeId(const QString &prefix)
{
    return QStringLiteral("%1-%2").arg(prefix, QUuid::createUuid().toString(QUuid::WithoutBraces));
}

/// Lookup helpers shared by the canvas, the renderer and the stores.
const Frame *findFrame(const DocumentSnapshot &snapshot, const QString &frameId, int *pageIndex = nullptr);
Frame *findFrame(DocumentSnapshot &snapshot, const QString &frameId, int *pageIndex = nullptr);
const Page *findPage(const DocumentSnapshot &snapshot, const QString &pageId, int *pageIndex = nullptr);
int pageIndexOf(const DocumentSnapshot &snapshot, const QString &pageId);
QSizeF pageSizeMm(const Page &page);

/// Image-side tone actually applied to a frame: the sanitised automatic
/// suggestion when enabled, plus the clamped manual brightness offset.
PhotoTone effectiveTone(const Frame &frame);

} // namespace pp
