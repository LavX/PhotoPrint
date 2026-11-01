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

/// Paints one page from prepared sRGB images. The painter is expected to map
/// page millimetres to device pixels; nothing editor-only is drawn here.
void paintPageContent(QPainter &painter, const DocumentSnapshot &snapshot, int pageIndex,
                      const QHash<QString, QImage> &images);

/// Numbered labels for trial sheets, drawn in device pixels after the page.
void paintFrameLabels(QPainter &painter, const DocumentSnapshot &snapshot, int pageIndex,
                      const QTransform &pageTransform, double ppi);

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
    /// test counts instead of trusting the wall clock.
    int verificationCount() const { return m_verificationCount; }
    void resetVerificationCount() { m_verificationCount = 0; }

private:
    /// The shared implementation. `verified` is the job-wide set of
    /// `path + sha` pairs already checked; a null set gives this call its own,
    /// so even a direct multi-copy render verifies each source once.
    Result<QImage> renderPageUsing(const DocumentSnapshot &snapshot, int pageIndex, double ppi,
                                   std::stop_token token, const RenderOptions &options,
                                   QSet<QString> *verified) const;

    ImageStore &m_store;
    mutable int m_verificationCount = 0;
};

} // namespace pp
