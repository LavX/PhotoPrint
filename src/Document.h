#pragma once

#include <QByteArray>
#include <QColorSpace>
#include <QList>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QUuid>

#include <algorithm>
#include <cmath>

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
    QString filter = QStringLiteral("original"); ///< PhotoFilters key
    int filterStrength = 100;                    ///< 0..100
    bool upscale = false;                        ///< 2x Lanczos before print

    bool operator==(const Frame &) const = default;
};

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
/// an infinity here is corruption, not a direction a person chose.
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

struct Page {
    QString id;
    PaperSpec paper;
    bool landscape = false;
    QList<Frame> frames;

    bool operator==(const Page &) const = default;
};

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
/// applies the exposure directly, so nothing outside these bounds may pass.
constexpr double kMaxPaperEv = 0.5;
inline ColorRecipe sanitiseRecipe(ColorRecipe recipe)
{
    recipe.tone = sanitiseTone(recipe.tone);
    if (std::isnan(recipe.paperStrength))
        recipe.paperStrength = 0.0;
    recipe.paperStrength = std::clamp(recipe.paperStrength, -kMaxPaperEv, kMaxPaperEv);
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
