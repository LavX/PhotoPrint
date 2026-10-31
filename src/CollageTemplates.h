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

namespace pp {

/// One slot of a collage template, normalised to the content box: 0..1 with the
/// origin at the top-left of the content box, x to the right, y down, exactly
/// like Frame in millimetres. The rectangle is a partition cell, not a print
/// rectangle: the gutter is cut at map time, so the cells still tile the unit
/// square without a gap of their own.
struct CollageSlot {
    QRectF rect{0.0, 0.0, 1.0, 1.0};
    /// Advisory hint, derived from the normalised rectangle: any, portrait,
    /// landscape or square. The renderer ignores it; FitMode::Fill crops any
    /// photograph into any slot, so the hint never constrains the print.
    QString aspect = QStringLiteral("any");
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

/// The shipped catalogue, loaded and validated from
/// `:/templates/collage.json`. The data file is the single source: nothing here
/// is generated into C++.
class CollageTemplates
{
public:
    /// The embedded catalogue. A malformed catalogue is a bug in the shipped
    /// data, so instance() reports it loudly and returns an empty catalogue
    /// rather than guessing at half a layout.
    static const CollageTemplates &instance();

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

    /// The frame list a template produces from `page.frames`, or an error.
    /// Exactly one frame per slot, in the same order and with the same ids.
    /// Geometry comes from the slot; angle becomes zero, fit becomes Fill and
    /// autoPlaced becomes false so a later added photograph never reflows the
    /// layout. Every other field is copied unchanged, including the source, its
    /// sha, the image rotation, the tone and the effect.
    ///
    /// Crop policy: the crop center and zoom survive when the slot aspect is
    /// within 15 % of the frame's current aspect, otherwise they reset to the
    /// centre and 1. The engine never applies the edit itself; the caller
    /// publishes the returned list as one SetFramesEdit, so one undo restores
    /// the layout.
    Result<QList<Frame>> apply(const CollageTemplate &t, const Page &page) const;

    /// Overload for a page that is not being edited yet.
    Result<QList<Frame>> apply(const CollageTemplate &t, const PaperSpec &paper,
                               bool landscape, const QList<Frame> &frames) const;

private:
    QList<CollageTemplate> m_templates;
    QHash<QString, int> m_indexById;
};

/// Short, language-free picker label: the family key and the count, e.g.
/// "hero · 5". The UI translates the family and the counted pattern; the engine
/// keeps both untranslated.
QString collageLabel(const CollageTemplate &t);

/// The template's reference name for accessibility and disambiguation: the
/// source-language name from the catalogue, or the id for a template built by
/// hand without one.
QString collageReferenceName(const CollageTemplate &t);

} // namespace pp
