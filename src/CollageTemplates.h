#pragma once

#include "Document.h"
#include "PaperGeometry.h"
#include "Result.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

namespace pp {

/// One slot of a collage template, normalised to the content box: 0..1 with the
/// origin at the top-left of the content box, x to the right, y down, exactly
/// like Frame in millimetres. The rectangle is a partition cell, not a print
/// rectangle: the gutter is cut at map time, so the cells still tile the unit
/// square without a gap of their own.
struct CollageSlot {
    QRectF rect{0.0, 0.0, 1.0, 1.0};
    /// The advisory aspect hint from the data, computed from the *normalised*
    /// rectangle only. It is not the aspect a photograph will be printed at: a
    /// square cell on a 10 × 15 cm sheet prints as 42.5 × 67.5 mm, because the
    /// content box is not square. The picker must call collageAspectMm() on the
    /// mapped millimetres instead. This field is kept only because the data
    /// records it and the loader checks it against the rectangle.
    QString normalizedAspect = QStringLiteral("any");
};

/// One entry of the shipped collage catalogue. Slot order is the fill order:
/// slot 0 is the feature slot and the first photograph goes there. Never sort
/// the slots by centre, because on a hero layout the hero may sit below a
/// satellite and a sort would put the first photograph in the wrong slot.
struct CollageTemplate {
    QString id;     ///< stable key, n plus the count and the layout name
    QString name;   ///< source-language reference name, used for accessibility
    QString nameEn; ///< English suggestion for the translation files
    QString family; ///< full, equal, hero, strip, mosaic, frame, bands, columns
    int photoCount = 0;
    /// The slots in fill order. The field is not called `slots` because Qt
    /// reserves that word as a macro keyword.
    QList<CollageSlot> slotList;
};

/// Margin between the paper edge and the photographs, and between two
/// neighbouring photographs. Five percent of the short side, capped at ten
/// millimetres, floored at four so the usual three millimetre hardware clip is
/// never crossed on a small sheet. A sheet that cannot describe a paper gives
/// zero; the mapper then refuses the template instead of dividing by nothing.
double collageMarginMm(const QSizeF &sheetMm);

/// Physical aspect class of a mapped slot: landscape when the millimetre ratio
/// is at least 1.5, portrait when at most 0.667, otherwise square. This is the
/// hint the picker must use, because the normalised hint in the data says
/// nothing about the sheet. Empty for a degenerate rectangle.
QString collageAspectMm(const QRectF &slotMm);

/// One apply() call's destructive side effects, so the dialog can warn honestly
/// instead of guessing. The engine changes nothing else about a frame.
struct CollageApplyReport {
    /// Frames whose crop centre and zoom were reset because the slot aspect is
    /// too far from the frame's current aspect.
    QStringList cropResetIds;
    /// Frames whose frame rotation (angleDeg) was non-zero and is set to zero.
    QStringList rotationResetIds;

    bool isEmpty() const { return cropResetIds.isEmpty() && rotationResetIds.isEmpty(); }
};

/// The shipped catalogue, loaded and validated from
/// `:/templates/collage.json`. The data file is the single source: nothing here
/// is generated into C++.
class CollageTemplates
{
public:
    /// The embedded catalogue. A malformed catalogue is a bug in the shipped
    /// data, so instance() reports it loudly and returns an empty catalogue
    /// rather than guessing at half a layout. loadError() then carries the
    /// reason, so a picker can tell broken data from a product without
    /// templates instead of showing an innocent empty list.
    static const CollageTemplates &instance();

    /// Empty when the embedded catalogue loaded, otherwise the failure the
    /// instance logged. A malformed count is data corruption, not an empty
    /// catalogue.
    static QString loadError();

    /// Strict parser, used by instance() and by the tests. Every rule the
    /// format states is enforced: the format key, the lattice, the entry count,
    /// unique ids, families, slot counts, lattice values, bounds, exact tiling
    /// and the advisory aspect hints. A malformed entry fails the whole
    /// catalogue, so a template that loads is always a complete layout.
    ///
    /// `landscapeSlots` is rejected rather than ignored: this build has one
    /// geometry for both orientations, and silently dropping an orientation
    /// override would print a different composition than the file describes.
    static Result<CollageTemplates> fromJson(const QByteArray &json);

    /// Every template, in catalogue order.
    const QList<CollageTemplate> &catalogue() const { return m_templates; }

    /// The templates for exactly this many photographs, in catalogue order.
    /// Empty outside 1..10 (and empty when the catalogue failed to load).
    QList<CollageTemplate> templatesForCount(int photoCount) const;

    /// Resolves an id. Null when the id is unknown; the pointer is owned by the
    /// catalogue and valid for as long as its instance lives.
    const CollageTemplate *find(const QString &id) const;

    bool isEmpty() const { return m_templates.isEmpty(); }

    /// Slot rectangles in page millimetres for the physical sheet of `geometry`.
    /// The content box is the sheet inset by half a margin and every slot gives
    /// up half a margin on every edge, so the outer margin and every gutter are
    /// the same value and two equal cells stay equal photographs. Fails with
    /// `template-too-small` when any slot would be under 15 mm on either axis.
    Result<QList<QRectF>> slotsMm(const CollageTemplate &t,
                                  const PaperGeometry &geometry) const;

    /// Convenience overload for callers that only have the paper.
    Result<QList<QRectF>> slotsMm(const CollageTemplate &t, const PaperSpec &paper,
                                  bool landscape) const;

    /// The physical aspect class of every slot, in fill order, on this paper.
    /// Same failure as slotsMm().
    Result<QStringList> aspectsMm(const CollageTemplate &t, const PaperSpec &paper,
                                  bool landscape) const;

    /// The frame list a template produces from `page.frames`, or an error.
    /// Exactly one frame per slot, in the same order and with the same ids.
    /// A template whose photoCount disagrees with its slot list is refused, so a
    /// hand-built template can never index past its slots.
    /// Geometry comes from the slot; angle becomes zero, fit becomes Fill and
    /// autoPlaced becomes false so a later added photograph never reflows the
    /// layout. Every other field is copied unchanged, including the source, its
    /// sha, the image rotation, the tone and the effect.
    ///
    /// Crop policy: the crop center and zoom survive when the slot aspect is
    /// within 15 % of the frame's current aspect (relative to the larger of the
    /// two), otherwise they reset to the centre and 1. What was reset is listed
    /// in `report`, so the page can say so before the user commits. The engine
    /// never applies the edit itself; the caller publishes the returned list as
    /// one SetFramesEdit, so one undo restores the layout.
    Result<QList<Frame>> apply(const CollageTemplate &t, const Page &page,
                               CollageApplyReport *report = nullptr) const;

    /// Overload for a page that is not being edited yet.
    Result<QList<Frame>> apply(const CollageTemplate &t, const PaperSpec &paper,
                               bool landscape, const QList<Frame> &frames,
                               CollageApplyReport *report = nullptr) const;

private:
    QList<CollageTemplate> m_templates;
    QHash<QString, int> m_indexById;
};

/// The picker label: the template's own name in the source language. All 55
/// names are unique, so two tiles never read the same and the picker translates
/// one string per template. Falls back to the English name, then the id, for a
/// template built by hand.
QString collageLabel(const CollageTemplate &t);

/// The accessible name for the tile. The catalogue names are unique, so the
/// label already identifies the template completely; this entry point exists so
/// the accessibility contract is named separately from the visible label.
QString collageReferenceName(const CollageTemplate &t);

} // namespace pp
