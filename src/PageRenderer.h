#pragma once

#include "Document.h"
#include "ImageSource.h"
#include "Result.h"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QSizeF>
#include <QString>

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

private:
    ImageStore &m_store;
};

} // namespace pp
