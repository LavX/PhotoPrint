#include "PageRenderer.h"

#include "ColorPipeline.h"
#include "EffectEngine.h"
#include "LayoutEngine.h"
#include "SheetLayout.h"
#include "SlotGeometry.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QSet>
#include <QTransform>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pp {

namespace {

constexpr double kMmToPt = 72.0 / 25.4;

/// zlib stream for a small buffer (the ICC profile and the XMP packet).
QByteArray flate(const QByteArray &input)
{
    uLongf bound = compressBound(static_cast<uLong>(input.size()));
    QByteArray output(static_cast<int>(bound), Qt::Uninitialized);
    if (compress2(reinterpret_cast<Bytef *>(output.data()), &bound,
                  reinterpret_cast<const Bytef *>(input.constData()),
                  static_cast<uLong>(input.size()), 6)
        != Z_OK)
        return {};
    output.resize(static_cast<int>(bound));
    return output;
}

/// Flate-compresses the page raster one row at a time. A whole-page RGB copy
/// plus its compressed form would double the peak for every sheet; the row
/// buffer is the only extra allocation this needs.
QByteArray flateImage(const QImage &image)
{
    if (image.isNull())
        return {};
    const QImage rgb = image.format() == QImage::Format_RGB32
        ? image
        : image.convertToFormat(QImage::Format_RGB32);
    if (rgb.isNull() || rgb.width() <= 0 || rgb.height() <= 0)
        return {};

    z_stream stream{};
    if (deflateInit(&stream, 6) != Z_OK)
        return {};

    QByteArray output;
    output.reserve(std::max(1024, rgb.width() * rgb.height() / 2));
    QByteArray row(rgb.width() * 3, Qt::Uninitialized);
    QByteArray chunk(1 << 16, Qt::Uninitialized);
    bool ok = true;

    const auto drain = [&](int flush) {
        int result = Z_OK;
        do {
            stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
            stream.avail_out = static_cast<uInt>(chunk.size());
            result = deflate(&stream, flush);
            if (result != Z_OK && result != Z_STREAM_END) {
                ok = false;
                return;
            }
            output.append(chunk.constData(), int(chunk.size() - stream.avail_out));
        } while (stream.avail_out == 0);
        if (flush == Z_FINISH && result != Z_STREAM_END)
            ok = false;
    };

    for (int y = 0; y < rgb.height() && ok; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        char *dst = row.data();
        for (int x = 0; x < rgb.width(); ++x) {
            const QRgb pixel = line[x];
            *dst++ = static_cast<char>(qRed(pixel));
            *dst++ = static_cast<char>(qGreen(pixel));
            *dst++ = static_cast<char>(qBlue(pixel));
        }
        stream.next_in = reinterpret_cast<Bytef *>(row.data());
        stream.avail_in = static_cast<uInt>(row.size());
        while (stream.avail_in > 0 && ok)
            drain(Z_NO_FLUSH);
    }
    if (ok) {
        stream.next_in = nullptr;
        stream.avail_in = 0;
        drain(Z_FINISH);
    }
    deflateEnd(&stream);
    return ok ? output : QByteArray();
}

QString number(double value)
{
    if (!std::isfinite(value))
        value = 0.0;
    QString text = QString::number(value, 'f', 4);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    if (text == QLatin1String("-0"))
        text = QStringLiteral("0");
    return text;
}

struct Mark {
    double x1 = 0; // mm from the top-left corner, Y downwards
    double y1 = 0;
    double x2 = 0;
    double y2 = 0;
    double widthMm = 0.25;
};

/// Developer measurement marks: corner marks and one 50 mm ruler element.
std::vector<Mark> developerMarksMm(const QSizeF &page)
{
    std::vector<Mark> marks;
    const double inset = 5.0;
    const double length = 10.0;
    const double w = page.width();
    const double h = page.height();
    marks.push_back({inset, inset, inset + length, inset, 0.25});
    marks.push_back({inset, inset, inset, inset + length, 0.25});
    marks.push_back({w - inset - length, inset, w - inset, inset, 0.25});
    marks.push_back({w - inset, inset, w - inset, inset + length, 0.25});
    marks.push_back({inset, h - inset, inset + length, h - inset, 0.25});
    marks.push_back({inset, h - inset - length, inset, h - inset, 0.25});
    marks.push_back({w - inset - length, h - inset, w - inset, h - inset, 0.25});
    marks.push_back({w - inset, h - inset - length, w - inset, h - inset, 0.25});

    // 50 mm ruler with end ticks, near the bottom edge.
    const double rulerY = h - 12.0;
    const double startX = 20.0;
    marks.push_back({startX, rulerY, startX + 50.0, rulerY, 0.25});
    marks.push_back({startX, rulerY - 2.5, startX, rulerY + 2.5, 0.25});
    marks.push_back({startX + 50.0, rulerY - 2.5, startX + 50.0, rulerY + 2.5, 0.25});
    return marks;
}

QByteArray xmpPacket()
{
    return QByteArrayLiteral(
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\" xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
        "xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\">\n"
        "   <pdfaid:part>1</pdfaid:part>\n"
        "   <pdfaid:conformance>B</pdfaid:conformance>\n"
        "   <xmp:CreatorTool>PhotoPrint</xmp:CreatorTool>\n"
        "   <pdf:Producer>PhotoPrint</pdf:Producer>\n"
        "   <dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">PhotoPrint</rdf:li></rdf:Alt>"
        "</dc:title>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>");
}

/// Draws one photograph clipped to its frame rectangle, or to a rounded path
/// when the slot asks for a corner radius. The clip is set in frame-local
/// coordinates, before the image transform, so the crop and zoom gestures keep
/// working exactly as they did.
void drawFramePhoto(QPainter &painter, const Frame &frame, const QImage &image,
                    const QTransform &pageTransform, double cornerRadiusMm)
{
    const QSize pixels = image.size();
    const QTransform imageTransform = LayoutEngine::imageToPage(frame, pixels);

    QTransform frameTransform;
    frameTransform.translate(frame.cx, frame.cy);
    frameTransform.rotate(frame.angleDeg);

    const QRectF photo(-frame.width / 2.0, -frame.height / 2.0, frame.width, frame.height);

    painter.save();
    painter.setTransform(frameTransform * pageTransform);
    if (cornerRadiusMm > 0.0) {
        QPainterPath clip;
        clip.addRoundedRect(photo, cornerRadiusMm, cornerRadiusMm);
        painter.setClipPath(clip, Qt::IntersectClip);
    } else {
        painter.setClipRect(photo, Qt::IntersectClip);
    }
    painter.setTransform(imageTransform * pageTransform);
    painter.drawImage(QRectF(0, 0, pixels.width(), pixels.height()), image);
    painter.restore();
}

/// Draws one slotted photograph: the cell is the clipping window, and inside it
/// the photograph is drawn at the frame's own rectangle, size and angle. A photo
/// smaller than the cell leaves paper around it; a larger one is cut away at the
/// cell edge. The optional corner radius rounds the window, not the photograph,
/// so a smaller photo still gets the section's rounded shape.
void drawSlottedFramePhoto(QPainter &painter, const Frame &frame, const QImage &image,
                           const QTransform &pageTransform, const QRectF &cell,
                           double cornerRadiusMm)
{
    if (cell.width() <= 0.0 || cell.height() <= 0.0)
        return;
    painter.save();
    painter.setTransform(pageTransform);
    if (cornerRadiusMm > 0.0) {
        QPainterPath clip;
        clip.addRoundedRect(cell, cornerRadiusMm, cornerRadiusMm);
        painter.setClipPath(clip, Qt::IntersectClip);
    } else {
        painter.setClipRect(cell, Qt::IntersectClip);
    }
    drawFramePhoto(painter, frame, image, pageTransform, 0.0);
    painter.restore();
}

} // namespace

PageRenderer::PageRenderer(ImageStore &store) : m_store(store) {}

QSize PageRenderer::effectLayerSize(const Frame &frame, const QSize &sourceSize, double ppi)
{
    if (!(ppi > 0.0) || !std::isfinite(ppi) || !LayoutEngine::validFrame(frame)
        || sourceSize.width() <= 0 || sourceSize.height() <= 0)
        return {};
    // imageSizeMm preserves the photograph's aspect and includes fit, zoom and
    // crop. Using the frame rectangle here stretched a 4:3 photo to 2:3.
    const QSizeF imageMm = LayoutEngine::imageSizeMm(frame, sourceSize);
    if (imageMm.width() <= 0.0 || imageMm.height() <= 0.0)
        return {};
    QSize size(std::max(1, qRound(imageMm.width() / 25.4 * ppi)),
               std::max(1, qRound(imageMm.height() / 25.4 * ppi)));
    const int longest = std::max(size.width(), size.height());
    if (longest > EffectEngine::kFinishMax) {
        const double factor = double(EffectEngine::kFinishMax) / double(longest);
        size = QSize(std::max(1, int(std::lround(size.width() * factor))),
                     std::max(1, int(std::lround(size.height() * factor))));
    }
    return size;
}

void paintPageContent(QPainter &painter, const DocumentSnapshot &snapshot, int pageIndex,
                      const QHash<QString, QImage> &images)
{
    if (pageIndex < 0 || pageIndex >= snapshot.pages.size())
        return;
    const Page &page = snapshot.pages.at(pageIndex);
    const QTransform pageTransform = painter.transform();

    // A page without slots keeps the exact code path it had before the feature
    // existed, so a 1.0.1 project paints and prints byte-identically.
    if (!page.hasLayout()) {
        for (const Frame &frame : page.frames) {
            const auto it = images.constFind(frame.id);
            if (it == images.constEnd() || it->isNull())
                continue;
            if (!LayoutEngine::validFrame(frame))
                continue;
            drawFramePhoto(painter, frame, *it, pageTransform, 0.0);
        }
        return;
    }

    QSet<QString> painted;
    for (const Slot &slot : page.slotList) {
        const QRectF cell = SlotGeometry::slotCellMm(page, slot);
        const double radius = SlotGeometry::cornerRadiusMm(page.slotStyle, cell);

        if (const Frame *frame = SlotGeometry::frameInSlot(page, slot.id)) {
            painted.insert(frame->id);
            const auto it = images.constFind(frame->id);
            if (it != images.constEnd() && !it->isNull()
                && LayoutEngine::validFrame(*frame)) {
                // The cell is the section's clipping window. The photograph is
                // drawn at the frame's own geometry inside it: smaller leaves
                // paper around it, larger is clipped. The optional radius rounds
                // the window, with no coloured ring around it.
                drawSlottedFramePhoto(painter, *frame, *it, pageTransform, cell, radius);
            }
        }
        // An empty cell prints paper and nothing else. Placeholders are painted
        // by the canvas widget, never here, so they cannot reach a raster, a
        // PDF or a printer.
    }

    // Free frames paint on top of the layout, exactly as they did before slots.
    // A frame whose slot id dangles in a caller-built snapshot also lands here,
    // at its stored geometry, instead of disappearing.
    for (const Frame &frame : page.frames) {
        if (painted.contains(frame.id))
            continue;
        const auto it = images.constFind(frame.id);
        if (it == images.constEnd() || it->isNull())
            continue;
        if (!LayoutEngine::validFrame(frame))
            continue;
        drawFramePhoto(painter, frame, *it, pageTransform, 0.0);
    }
}

void paintFrameLabels(QPainter &painter, const DocumentSnapshot &snapshot, int pageIndex,
                      const QTransform &pageTransform, double ppi)
{
    if (pageIndex < 0 || pageIndex >= snapshot.pages.size())
        return;
    const Page &page = snapshot.pages.at(pageIndex);
    painter.save();
    painter.setTransform(QTransform());
    for (const Frame &frame : page.frames) {
        if (frame.label.isEmpty())
            continue;
        const QPointF topLeft =
            pageTransform.map(QPointF(frame.cx - frame.width / 2.0, frame.cy - frame.height / 2.0));
        const int pixelSize = std::max(10, qRound(7.0 * ppi / 25.4));
        QFont font;
        font.setPixelSize(pixelSize);
        font.setBold(true);
        painter.setFont(font);
        const QRectF box(topLeft.x() + 6, topLeft.y() + 6, pixelSize * 1.6, pixelSize * 1.35);
        painter.fillRect(box, QColor(255, 255, 255, 225));
        painter.setPen(Qt::black);
        painter.drawText(box, Qt::AlignCenter, frame.label);
    }
    painter.restore();
}

bool requiresCutGuideConfirmation(const DocumentSnapshot &document)
{
    for (const Page &page : document.pages) {
        if (page.cutGuides == CutGuides::Print)
            return true;
    }
    return false;
}

QList<int> pagesPrintingCutGuides(const DocumentSnapshot &document)
{
    QList<int> pages;
    for (int i = 0; i < document.pages.size(); ++i) {
        if (document.pages.at(i).cutGuides == CutGuides::Print)
            pages.append(i + 1);
    }
    return pages;
}

void paintCutGuideInk(QImage &srgbPage, const Page &page, double ppi)
{
    if (page.cutGuides != CutGuides::Print || srgbPage.isNull())
        return;
    if (!(ppi > 0.0) || !std::isfinite(ppi))
        return;
    const QList<QLineF> lines = SheetLayout::cutGuideLinesMm(page);
    if (lines.isEmpty())
        return;

    QPainter painter(&srgbPage);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QTransform pageScale;
    pageScale.scale(ppi / 25.4, ppi / 25.4);
    painter.setTransform(pageScale);
    // 0.2 mm at 300 ppi is 2.4 px: thin enough not to eat the photograph and
    // thick enough for the printer to render. Flat caps keep the corner joins
    // from growing a blob, and the miter join closes the rectangle cleanly.
    // The line is centred on the trim on purpose: half of it sits on the
    // photograph, half in the gutter, so a cut along the line's centre removes
    // 0.1 mm from the copy. That is 0.2 % of a 45 mm side, well inside any print
    // and cutting tolerance, and it keeps the mark readable on both sides of
    // the cut. Offsetting it inward would instead leave a grey hairline on the
    // finished piece.
    QPen pen(QColor(127, 127, 127), 0.2, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    for (const QLineF &line : lines)
        painter.drawLine(line);
}

namespace {

/// The `path + sha` identity one verification covers. Copies share both, so a
/// hundred of them produce one key and one file read.
QString verificationKey(const Frame &frame)
{
    return frame.sourcePath + QLatin1Char('\n')
        + QString::fromLatin1(frame.sourceSha256.toHex());
}

} // namespace

Result<QImage> PageRenderer::renderPage(const DocumentSnapshot &snapshot, int pageIndex, double ppi,
                                        std::stop_token token, const RenderOptions &options) const
{
    // A single call still verifies a source once: a page of copies is one job
    // as far as the identity check is concerned.
    QHash<QString, SourceImage> verified;
    return renderPageUsing(snapshot, pageIndex, ppi, token, options, &verified);
}

Result<QImage> PageRenderer::renderPageUsing(const DocumentSnapshot &snapshot, int pageIndex,
                                             double ppi, std::stop_token token,
                                             const RenderOptions &options,
                                             QHash<QString, SourceImage> *verified) const
{
    if (pageIndex < 0 || pageIndex >= snapshot.pages.size())
        return error(QStringLiteral("page-index"), QCoreApplication::translate("PhotoPrint", "Ez az oldal nem található."));
    if (!(ppi > 0.0) || !std::isfinite(ppi))
        return error(QStringLiteral("ppi"), QCoreApplication::translate("PhotoPrint", "A felbontás érvénytelen."));

    const Page &page = snapshot.pages.at(pageIndex);
    const QSizeF pageSize = pageSizeMm(page);
    if (!(pageSize.width() > 0.0) || !(pageSize.height() > 0.0))
        return error(QStringLiteral("page-size"), QCoreApplication::translate("PhotoPrint", "A papírméret érvénytelen."));

    QHash<QString, SourceImage> localVerified;
    QHash<QString, SourceImage> *alreadyVerified = verified ? verified : &localVerified;

    QHash<QString, QImage> images;
    // The snapshot may come straight from a project file or from a caller that
    // filled paperStrength from a preset. MainWindow clamps it for the preview,
    // so the renderer clamps the same recipe here: otherwise a crafted value
    // would print a page the preview never showed.
    const ColorRecipe baseRecipe = sanitiseRecipe(snapshot.recipe);
    for (const Frame &frame : page.frames) {
        if (token.stop_requested())
            return error(QStringLiteral("cancelled"), QCoreApplication::translate("PhotoPrint", "A művelet megszakadt."));
        if (!LayoutEngine::validFrame(frame))
            continue;
        // The file is hashed once per identity per job, not once per copy: a
        // hundred 5 MB JPEGs would otherwise read and hash half a gigabyte
        // before the first page is drawn.
        const auto loadSource = [&]() -> Result<SourceImage> {
            if (!options.verifySources)
                return m_store.load(frame.sourcePath, ImportPolicy{}, token);
            const QString key = verificationKey(frame);
            // The verified snapshot is the job's own copy of the photograph.
            // Reusing it means every copy of a source is identical even if the
            // file is replaced between page 1 and page 2, and it removes the
            // store's mtime+size shortcut from the decision entirely.
            const auto known = alreadyVerified->constFind(key);
            if (known != alreadyVerified->constEnd())
                return *known;
            Result<SourceImage> checked =
                m_store.reloadVerified(frame.sourcePath, frame.sourceSha256, token);
            if (checked) {
                alreadyVerified->insert(key, checked.value());
                m_verificationCount.fetch_add(1, std::memory_order_relaxed);
            }
            return checked;
        };
        Result<SourceImage> source = loadSource();
        if (!source)
            return source.error();
        ColorRecipe recipe = baseRecipe;
        recipe.tone = effectiveTone(frame);
        if (frame.paperOverride)
            recipe.paperStrength = frame.paperEv;
        // The per-frame paper override comes from the same untrusted frame, so
        // it goes through the recipe clamp as well instead of reaching std::pow
        // unbounded.
        recipe = sanitiseRecipe(recipe);
        EffectRenderTarget effectTarget;
        effectTarget.recipeVersion = frame.effectVersion;
        if (options.sourceMaxDimension == 0) {
            // The layer follows the photograph's drawn size, not the frame
            // rectangle: the page painter already crops/fits the image to the
            // frame. The size is capped at the finish bound, and the painter
            // performs the final scale.
            effectTarget.printSize = effectLayerSize(frame, source.value().size(), ppi);
        }
        Result<QImage> converted =
            m_store.preparedLinear(source.value(), recipe, frame.filter, frame.filterStrength,
                                   frame.upscale, options.sourceMaxDimension, token, effectTarget);
        if (!converted)
            return converted.error();
        images.insert(frame.id, converted.value());
    }

    const int pixelWidth = std::max(1, qRound(pageSize.width() / 25.4 * ppi));
    const int pixelHeight = std::max(1, qRound(pageSize.height() / 25.4 * ppi));
    QImage canvas(pixelWidth, pixelHeight, QImage::Format_RGBA64_Premultiplied);
    if (canvas.isNull())
        return error(QStringLiteral("render-size"), QCoreApplication::translate("PhotoPrint", "A lap nem rajzolható meg."));
    canvas.fill(Qt::white);

    QPainter painter(&canvas);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QTransform pageScale;
    pageScale.scale(ppi / 25.4, ppi / 25.4);
    painter.setTransform(pageScale);
    paintPageContent(painter, snapshot, pageIndex, images);
    paintFrameLabels(painter, snapshot, pageIndex, pageScale, ppi);
    painter.end();

    QImage srgb = ColorPipeline::linearToSrgb(canvas, snapshot.recipe.grayscale, token);
    if (srgb.isNull())
        return error(QStringLiteral("colour-conversion"),
                     QCoreApplication::translate("PhotoPrint", "A lapot most nem tudjuk elkészíteni. Próbáld újra."));
    // The ink goes on after the conversion on purpose: writing #7F7F7F into the
    // linear working space would print it at about 188. A screen overlay never
    // reaches this function, so `Screen` leaves the raster untouched.
    paintCutGuideInk(srgb, page, ppi);
    return srgb;
}


/// Streams the PDF object by object. The page raster is compressed and written
/// as soon as it is rendered, so the job holds one sheet at a time instead of
/// the whole document. The object layout is the one the file always had: one
/// page object, one content stream and one image XObject per sheet, then the
/// shared sRGB profile, the XMP packet and the document info.
class StreamingPdfWriter
{
public:
    bool open(const QString &path, int pageCount, QString *errorText)
    {
        if (pageCount < 1) {
            *errorText = QStringLiteral("empty-document");
            return false;
        }
        m_pageCount = pageCount;
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::WriteOnly)) {
            *errorText = QStringLiteral("cannot-write");
            return false;
        }
        if (!write(QByteArrayLiteral("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n"), errorText))
            return false;

        const int iccNum = 3 + 3 * pageCount;
        const int metadataNum = iccNum + 1;
        QString kids;
        for (int i = 0; i < pageCount; ++i)
            kids += QStringLiteral("%1 0 R ").arg(3 + 3 * i);
        const QByteArray catalog =
            QStringLiteral("<< /Type /Catalog /Pages 2 0 R /OutputIntents [ << /Type "
                           "/OutputIntent /S /GTS_PDFA1 /OutputCondition (sRGB) "
                           "/OutputConditionIdentifier (sRGB IEC61966-2.1) /Info (sRGB "
                           "IEC61966-2.1) /DestOutputProfile %1 0 R >> ] /Metadata %2 0 "
                           "R >>")
                .arg(iccNum)
                .arg(metadataNum)
                .toLatin1();
        if (!writeObject(catalog, errorText))
            return false;
        const QByteArray pages = QStringLiteral("<< /Type /Pages /Count %1 /Kids [ %2] >>")
                                     .arg(pageCount)
                                     .arg(kids)
                                     .toLatin1();
        return writeObject(pages, errorText);
    }

    bool addPage(const QImage &page, const QSizeF &sizeMm, const QSizeF &mediaMm,
                 const QPointF &offsetMm, double contentScale, const std::vector<Mark> &marks,
                 QString *errorText)
    {
        if (m_writtenPages >= m_pageCount) {
            *errorText = QStringLiteral("too-many-pages");
            return false;
        }
        const int pageNum = 3 + 3 * m_writtenPages;
        const int contentNum = pageNum + 1;
        const int imageNum = pageNum + 2;
        const int iccNum = 3 + 3 * m_pageCount;

        const double widthPt = mediaMm.width() * kMmToPt;
        const double heightPt = mediaMm.height() * kMmToPt;
        const double contentWidthPt = sizeMm.width() * contentScale * kMmToPt;
        const double contentHeightPt = sizeMm.height() * contentScale * kMmToPt;
        const double offsetXPt = offsetMm.x() * kMmToPt;
        // PDF user space has its origin at the bottom-left corner, the model's at
        // the top-left.
        const double offsetYPt =
            (mediaMm.height() - offsetMm.y() - sizeMm.height() * contentScale) * kMmToPt;

        QString content;
        content += QStringLiteral("q\n%1 0 0 %2 %3 %4 cm\n/Im0 Do\nQ\n")
                       .arg(number(contentWidthPt), number(contentHeightPt), number(offsetXPt),
                            number(offsetYPt));
        if (!marks.empty()) {
            content += QStringLiteral("0 0 0 RG 0 0 0 rg\n");
            for (const Mark &mark : marks) {
                const double x1 = (offsetMm.x() + mark.x1 * contentScale) * kMmToPt;
                const double y1 =
                    (mediaMm.height() - offsetMm.y() - mark.y1 * contentScale) * kMmToPt;
                const double x2 = (offsetMm.x() + mark.x2 * contentScale) * kMmToPt;
                const double y2 =
                    (mediaMm.height() - offsetMm.y() - mark.y2 * contentScale) * kMmToPt;
                content += QStringLiteral("%1 w %2 %3 m %4 %5 l S\n")
                               .arg(number(mark.widthMm * kMmToPt), number(x1), number(y1),
                                    number(x2), number(y2));
            }
        }

        const QByteArray imageData = flateImage(page);
        if (imageData.isEmpty()) {
            *errorText = QStringLiteral("image-compression");
            return false;
        }

        const QByteArray pageObject =
            QStringLiteral("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %1 %2] /Resources << "
                           "/XObject << /Im0 %3 0 R >> >> /Contents %4 0 R >>")
                .arg(number(widthPt), number(heightPt))
                .arg(imageNum)
                .arg(contentNum)
                .toLatin1();
        if (!writeObject(pageObject, errorText))
            return false;

        const QByteArray contentBytes = content.toLatin1();
        const QByteArray contentObject = QByteArray("<< /Length ")
            + QByteArray::number(contentBytes.size()) + QByteArray(" >>\nstream\n")
            + contentBytes + QByteArray("endstream");
        if (!writeObject(contentObject, errorText))
            return false;

        const QByteArray imageObject =
            QByteArray("<< /Type /XObject /Subtype /Image /Width ")
            + QByteArray::number(page.width()) + QByteArray(" /Height ")
            + QByteArray::number(page.height()) + QByteArray(" /ColorSpace [ /ICCBased ")
            + QByteArray::number(iccNum) + QByteArray(" 0 R ] /BitsPerComponent 8 /Filter "
                                                      "/FlateDecode /Length ")
            + QByteArray::number(imageData.size()) + QByteArray(" >>\nstream\n") + imageData
            + QByteArray("\nendstream");
        if (!writeObject(imageObject, errorText))
            return false;

        ++m_writtenPages;
        return true;
    }

    bool finish(QString *errorText)
    {
        if (m_writtenPages != m_pageCount) {
            *errorText = QStringLiteral("missing-pages");
            return false;
        }
        const int iccNum = 3 + 3 * m_pageCount;
        const int infoNum = iccNum + 2;

        const QByteArray icc = ColorPipeline::srgbProfileBytes();
        if (icc.isEmpty()) {
            *errorText = QStringLiteral("missing-srgb-profile");
            return false;
        }
        const QByteArray iccCompressed = flate(icc);
        if (iccCompressed.isEmpty()) {
            *errorText = QStringLiteral("icc-compression");
            return false;
        }
        const QByteArray iccObject =
            QByteArray("<< /N 3 /Alternate /DeviceRGB /Filter /FlateDecode /Length ")
            + QByteArray::number(iccCompressed.size()) + QByteArray(" >>\nstream\n")
            + iccCompressed + QByteArray("\nendstream");
        if (!writeObject(iccObject, errorText))
            return false;

        const QByteArray xmpCompressed = flate(xmpPacket());
        if (xmpCompressed.isEmpty()) {
            *errorText = QStringLiteral("xmp-compression");
            return false;
        }
        const QByteArray metadataObject =
            QByteArray("<< /Type /Metadata /Subtype /XML /Filter /FlateDecode /Length ")
            + QByteArray::number(xmpCompressed.size()) + QByteArray(" >>\nstream\n")
            + xmpCompressed + QByteArray("\nendstream");
        if (!writeObject(metadataObject, errorText))
            return false;

        const QByteArray infoObject =
            QStringLiteral("<< /Title (PhotoPrint) /Producer (PhotoPrint) "
                           "/Creator (PhotoPrint) >>")
                .toLatin1();
        if (!writeObject(infoObject, errorText))
            return false;

        const qint64 xrefOffset = m_file.pos();
        const int objectCount = 5 + 3 * m_pageCount;
        QByteArray xref = "xref\n0 " + QByteArray::number(objectCount + 1) + "\n";
        xref += "0000000000 65535 f \n";
        for (const qint64 offset : m_offsets)
            xref += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        if (!write(xref, errorText))
            return false;

        // The file ID is the MD5 of everything up to the xref, exactly as the
        // in-memory writer computed it.
        const QByteArray id = m_hash.result().toHex();
        QByteArray trailer = "trailer\n<< /Size " + QByteArray::number(objectCount + 1)
            + " /Root 1 0 R" + " /Info " + QByteArray::number(infoNum) + " 0 R /ID [<" + id
            + "><" + id + ">] >>\n";
        trailer += "startxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
        // The trailer is not part of the hashed prefix; writing it through the
        // hashing path would feed a finalised hash.
        if (m_file.write(trailer) != trailer.size()) {
            *errorText = QStringLiteral("short-write");
            return false;
        }
        if (!m_file.commit()) {
            *errorText = QStringLiteral("commit-failed");
            return false;
        }
        return true;
    }

private:
    bool write(const QByteArray &data, QString *errorText)
    {
        if (m_file.write(data) != data.size()) {
            *errorText = QStringLiteral("short-write");
            return false;
        }
        m_hash.addData(data);
        return true;
    }

    bool writeObject(const QByteArray &body, QString *errorText)
    {
        m_offsets.append(m_file.pos());
        if (!write(QByteArray::number(m_offsets.size()) + QByteArray(" 0 obj\n"), errorText))
            return false;
        if (!write(body, errorText))
            return false;
        return write(QByteArray("\nendobj\n"), errorText);
    }

    QSaveFile m_file;
    QCryptographicHash m_hash{QCryptographicHash::Md5};
    QList<qint64> m_offsets;
    int m_pageCount = 0;
    int m_writtenPages = 0;
};

Result<ExportArtifact> PageRenderer::writePdf(const DocumentSnapshot &snapshot, const QString &path,
                                              std::stop_token token,
                                              const RenderOptions &options) const
{
    if (snapshot.pages.isEmpty())
        return error(QStringLiteral("empty-document"), QCoreApplication::translate("PhotoPrint", "Nincs mit nyomtatni."));

    const double ppi = options.printPpi > 0.0 && std::isfinite(options.printPpi)
        ? options.printPpi
        : 300.0;
    const double contentScale =
        options.contentScale > 0.0 && std::isfinite(options.contentScale) ? options.contentScale
                                                                          : 1.0;

    QString errorText;
    StreamingPdfWriter writer;
    if (!writer.open(path, int(snapshot.pages.size()), &errorText))
        return error(QStringLiteral("pdf-write"), QCoreApplication::translate("PhotoPrint", "A PDF mentése nem sikerült."));

    // One job-wide identity cache: a four-page batch of one photograph verifies
    // the file on page one and reuses that verified snapshot on every later copy.
    QHash<QString, SourceImage> verified;
    for (int i = 0; i < snapshot.pages.size(); ++i) {
        if (token.stop_requested())
            return error(QStringLiteral("cancelled"), QCoreApplication::translate("PhotoPrint", "A művelet megszakadt."));

        const Page &page = snapshot.pages.at(i);
        const QSizeF size = pageSizeMm(page);
        if (!(size.width() > 0.0) || !(size.height() > 0.0))
            return error(QStringLiteral("page-size"), QCoreApplication::translate("PhotoPrint", "A papírméret érvénytelen."));
        const QSizeF media =
            options.mediaSizeMm.width() > 0.0 && options.mediaSizeMm.height() > 0.0
            ? options.mediaSizeMm
            : size;

        // Render, paint, release: the page raster lives for one iteration. The
        // writer compresses it and drops it, so the job's peak is one sheet and
        // not the sum of every sheet.
        Result<QImage> rendered = renderPageUsing(snapshot, i, ppi, token, options, &verified);
        if (!rendered)
            return rendered.error();
        const std::vector<Mark> marks =
            options.developerMarks ? developerMarksMm(size) : std::vector<Mark>{};
        if (!writer.addPage(rendered.value(), size, media, options.contentOffsetMm, contentScale,
                            marks, &errorText)) {
            return error(QStringLiteral("pdf-write"),
                         QCoreApplication::translate("PhotoPrint", "A PDF mentése nem sikerült."));
        }
        if (m_afterPage)
            m_afterPage(i);
    }

    if (!writer.finish(&errorText))
        return error(QStringLiteral("pdf-write"), QCoreApplication::translate("PhotoPrint", "A PDF mentése nem sikerült."));

    ExportArtifact artifact;
    artifact.path = path;
    artifact.sha256 = fileSha256(path);
    artifact.pageCount = int(snapshot.pages.size());
    artifact.firstPageSizeMm =
        options.mediaSizeMm.width() > 0.0 && options.mediaSizeMm.height() > 0.0
        ? options.mediaSizeMm
        : pageSizeMm(snapshot.pages.first());
    if (artifact.sha256.isEmpty())
        return error(QStringLiteral("pdf-read-back"), QCoreApplication::translate("PhotoPrint", "A PDF mentése nem sikerült."));
    return artifact;
}

} // namespace pp
