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
    /// Empty for a free frame. Otherwise the slot on the page that owns this
    /// frame's rectangle: the slot's photo rectangle is the frame's geometry and
    /// the frame angle stays zero. A dangling id (a slot that no longer exists)
    /// is cleared by SlotLayoutEngine::repair, which turns the frame back into a
    /// free one instead of leaving an unusable reference behind.
    QString slotId;

    bool operator==(const Frame &) const = default;
};

/// One fixed area of a laid-out page. `unitRect` is the mapping source of truth:
/// 0..1 of the page's content box, origin top-left, x right, y down, the same
/// convention CollageSlot uses. The millimetre rectangle is a pure function of
/// this and the page's paper, so a paper or orientation change re-maps without
/// touching the stored layout.
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
    /// This is the border the panel exposes: the photograph fills its cell, so
    /// the gap is the only frame around it.
    std::optional<double> gutterMm;
    double cornerRadiusMm = 0.0; ///< rounded photograph, 0 = square
    bool operator==(const SlotStyle &) const = default;
};

/// Bounds of the extra magnification of the photograph inside its frame. The
/// wheel gesture and the in-slot inspector share these, so neither can drift
/// away from the model the renderer clamps to.
constexpr double kMinFrameZoom = 0.2;
constexpr double kMaxFrameZoom = 8.0;

/// Bounds of the photograph's own rotation inside its frame. The inspector
/// shows exactly this range, so a slider position is always a stored value.
constexpr double kMinImageRotationDeg = -180.0;
constexpr double kMaxImageRotationDeg = 180.0;

/// Wraps any angle into (-180, 180]. Used by the gestures that rotate freely
/// (the wheel and the quarter-turn buttons); the slider itself clamps instead,
/// so a deliberate -180 stays -180.
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

    bool hasLayout() const { return !slotList.isEmpty(); }
    bool operator==(const Page &) const = default;
};

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
