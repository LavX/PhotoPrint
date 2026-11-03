#pragma once

#include "Document.h"
#include "ImageSource.h"
#include "Result.h"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QTransform>

#include <atomic>
#include <functional>
#include <stop_token>

class QPainter;

namespace pp {

struct RenderOptions {
    /// Full source resolution by default; a positive bound limits memory for previews.
    int sourceMaxDimension = 0;
    /// Resolution of the raster embedded in the PDF.
    double printPpi = 300.0;
    /// Resolved logical page for the printer. Empty keeps the physical size.
    QSizeF mediaSizeMm;
    /// Physical content origin inside the logical page, top-left coordinates.
    QPointF contentOffsetMm;
    /// Uniform enlargement of the physical composition on the logical page.
    /// Used only for borderless media whose logical area is slightly larger.
    double contentScale = 1.0;
    /// Developer marks belong to tests only. They never appear in normal exports.
    bool developerMarks = false;
    /// Verify the recorded source identity before using pixels.
    bool verifySources = false;
};

struct ExportArtifact {
    QString path;
    QByteArray sha256;
    int pageCount = 0;
    QSizeF firstPageSizeMm;
};

/// The prepared layers one page needs. The background is the converted source
/// at a bounded size, not a page-sized raster: the painter places it, so memory
/// stays bounded by the source and the geometry is testable without pixels.
struct PageImages {
    QHash<QString, QImage> frames; ///< frame id -> linear premultiplied layer
    QImage background;             ///< linear premultiplied, null when none
    /// The source's pixel size, kept for the natural tile size. The converted
    /// layer may be smaller than the source, and a tile must not shrink with it.
    QSize backgroundPixels;
    /// Sticker id -> linear premultiplied layer, already colour converted.
    QHash<QString, QImage> stickers;
    bool operator==(const PageImages &) const = default;
};

/// The longest side a background layer is converted at. A decorative picture
/// does not need 1200 ppi, and a page-sized layer at that resolution would be
/// over a gigabyte; the painter upscales the bounded layer instead.
constexpr int kBackgroundMaxDimension = 4096;

/// The longest side a sticker layer is converted at. A sticker is drawn at a
/// few centimetres, so this is already far more than print needs; it bounds the
/// cost of one decorative PNG on a 1200 ppi page.
constexpr int kStickerMaxLayer = 2048;

/// Paints one page from prepared linear layers. The painter is expected to map
/// page millimetres to device pixels; nothing editor-only is drawn here. The
/// background is painted first, before the slot branch, so gaps, margins, empty
/// cells and fit bands show it on a laid-out page and a free-canvas page alike.
void paintPageContent(QPainter &painter, const DocumentSnapshot &snapshot, int pageIndex,
                      const PageImages &images);

/// Numbered labels for trial sheets, drawn in device pixels after the page.
void paintFrameLabels(QPainter &painter, const DocumentSnapshot &snapshot, int pageIndex,
                      const QTransform &pageTransform, double ppi);

/// Draws the glyphs of one text block in the painter's current coordinate
/// system, centred on the origin, at @p ppi. The caller sets the transform, the
/// opacity and the pen colour. Shared by the page raster and the editor's live
/// preview, so the preview cannot drift from the print.
void paintTextBlock(QPainter &painter, const TextItem &item, double ppi);

/// Whether any page carries cut guides that reach the print. A page set to
/// `Screen` shows the overlay only, so it never asks for a confirmation; both
/// the print path and the PDF export call this before they start.
bool requiresCutGuideConfirmation(const DocumentSnapshot &document);

/// The pages whose printed guides reach paper, 1-based, in page order. The
/// confirmation names them so a multi-page job says which sheets carry marks.
QList<int> pagesPrintingCutGuides(const DocumentSnapshot &document);

/// Draws the printed cut-guide ink onto an already converted sRGB page. A 0.2 mm
/// solid 50 % grey line at the frame edges: solid beats dashed at print
/// resolution, and drawing after the conversion is what makes #7F7F7F print as
/// #7F7F7F instead of a linear-space approximation of it.
void paintCutGuideInk(QImage &srgbPage, const Page &page, double ppi);

/// Draws the documented millimetre model into pixels. Screen previews and the
/// PDF use the same page recipe; the PDF raster is never a screen capture.
class PageRenderer
{
public:
    explicit PageRenderer(ImageStore &store);

    Result<QImage> renderPage(const DocumentSnapshot &snapshot, int pageIndex, double ppi,
                              std::stop_token token = {},
                              const RenderOptions &options = {}) const;

    Result<ExportArtifact> writePdf(const DocumentSnapshot &snapshot, const QString &path,
                                    std::stop_token token = {},
                                    const RenderOptions &options = {}) const;

    /// Pixel size of the effect layer for one frame at @p ppi. The source
    /// aspect is preserved through LayoutEngine::imageSizeMm, and the layer is
    /// capped at EffectEngine::kFinishMax so a 1200 ppi page never asks the
    /// effect engine for a 100+ megapixel frame. The page painter scales the
    /// capped layer into place.
    static QSize effectLayerSize(const Frame &frame, const QSize &sourceSize, double ppi);

    /// How many sources this renderer verified in total. A batch job verifies
    /// one source once however many copies share it, and this is the seam a
    /// test counts instead of trusting the wall clock. Atomic because the
    /// counter is incremented on whichever worker runs the job.
    int verificationCount() const { return m_verificationCount.load(std::memory_order_relaxed); }
    void resetVerificationCount() { m_verificationCount.store(0, std::memory_order_relaxed); }

    /// Test seam: called with the page index after that page has been written.
    /// It lets a test change a source file halfway through a job without a
    /// second writer or a fake store.
    void setAfterPageHookForTest(std::function<void(int)> hook)
    {
        m_afterPage = std::move(hook);
    }

private:
    /// The shared implementation. `verified` is the job-wide map of
    /// `path + sha` identities to the decoded, verified snapshot. Every copy in
    /// the job reuses that one snapshot, so a file replaced halfway through a
    /// job cannot put two versions of it into one document. A null map gives
    /// this call its own, so even a direct multi-copy render verifies once.
    Result<QImage> renderPageUsing(const DocumentSnapshot &snapshot, int pageIndex, double ppi,
                                   std::stop_token token, const RenderOptions &options,
                                   QHash<QString, SourceImage> *verified) const;

    ImageStore &m_store;
    mutable std::atomic<int> m_verificationCount{0};
    std::function<void(int)> m_afterPage;
};

} // namespace pp
