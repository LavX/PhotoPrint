#include "PageRenderer.h"

#include "ColorPipeline.h"
#include "LayoutEngine.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QFont>
#include <QPainter>
#include <QSaveFile>
#include <QTransform>

#include <zlib.h>

#include <cmath>
#include <vector>

namespace pp {

namespace {

constexpr double kMmToPt = 72.0 / 25.4;

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

QByteArray rgbBytes(const QImage &image)
{
    const QImage rgb = image.format() == QImage::Format_RGB32 ? image
                                                              : image.convertToFormat(QImage::Format_RGB32);
    QByteArray bytes;
    bytes.resize(static_cast<int>(static_cast<qsizetype>(rgb.width()) * rgb.height() * 3));
    char *dst = bytes.data();
    for (int y = 0; y < rgb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            const QRgb pixel = line[x];
            *dst++ = static_cast<char>(qRed(pixel));
            *dst++ = static_cast<char>(qGreen(pixel));
            *dst++ = static_cast<char>(qBlue(pixel));
        }
    }
    return bytes;
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

class PdfWriter
{
public:
    PdfWriter() = default;

    void setPageSizeHint(const QSizeF &) {}

    bool write(const QString &path, const QList<QImage> &pages, const QList<QSizeF> &sizesMm,
               const QList<QSizeF> &mediaSizesMm, const QList<QPointF> &offsetsMm,
               const QList<std::vector<Mark>> &marks, double optionsScale, bool /*pdfA*/,
               QString *errorText)
    {
        if (pages.isEmpty() || pages.size() != sizesMm.size()) {
            *errorText = QStringLiteral("empty-document");
            return false;
        }
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

        const int pageCount = pages.size();
        const int iccNum = 3 + 3 * pageCount;
        const int metadataNum = iccNum + 1;
        const int infoNum = iccNum + 2;

        QList<QByteArray> objects;
        objects.resize(5 + 3 * pageCount);

        const QByteArray xmp = xmpPacket();
        const QByteArray xmpCompressed = flate(xmp);

        QString kids;
        for (int i = 0; i < pageCount; ++i)
            kids += QStringLiteral("%1 0 R ").arg(3 + 3 * i);

        objects[0] = QStringLiteral("<< /Type /Catalog /Pages 2 0 R /OutputIntents [ << /Type "
                                    "/OutputIntent /S /GTS_PDFA1 /OutputCondition (sRGB) "
                                    "/OutputConditionIdentifier (sRGB IEC61966-2.1) /Info (sRGB "
                                    "IEC61966-2.1) /DestOutputProfile %1 0 R >> ] /Metadata %2 0 "
                                    "R >>")
                         .arg(iccNum)
                         .arg(metadataNum)
                         .toLatin1();
        objects[1] = QStringLiteral("<< /Type /Pages /Count %1 /Kids [ %2] >>")
                         .arg(pageCount)
                         .arg(kids)
                         .toLatin1();

        for (int i = 0; i < pageCount; ++i) {
            const QSizeF size = sizesMm.at(i);
            const QSizeF media = i < mediaSizesMm.size() ? mediaSizesMm.at(i) : size;
            const QPointF offset = i < offsetsMm.size() ? offsetsMm.at(i) : QPointF();
            const double widthPt = media.width() * kMmToPt;
            const double heightPt = media.height() * kMmToPt;
            const double contentScale =
                optionsScale > 0.0 && std::isfinite(optionsScale) ? optionsScale : 1.0;
            const double contentWidthPt = size.width() * contentScale * kMmToPt;
            const double contentHeightPt = size.height() * contentScale * kMmToPt;
            const double offsetXPt = offset.x() * kMmToPt;
            const double offsetYPt =
                (media.height() - offset.y() - size.height() * contentScale) * kMmToPt;
            const int pageNum = 3 + 3 * i;
            const int contentNum = pageNum + 1;
            const int imageNum = pageNum + 2;

            QString content;
            content += QStringLiteral("q\n%1 0 0 %2 %3 %4 cm\n/Im0 Do\nQ\n")
                           .arg(number(contentWidthPt), number(contentHeightPt), number(offsetXPt),
                                number(offsetYPt));
            const std::vector<Mark> pageMarks = i < marks.size() ? marks.at(i) : std::vector<Mark>{};
            if (!pageMarks.empty()) {
                content += QStringLiteral("0 0 0 RG 0 0 0 rg\n");
                for (const Mark &mark : pageMarks) {
                    const double x1 = (offset.x() + mark.x1 * contentScale) * kMmToPt;
                    const double y1 = (media.height() - offset.y() - mark.y1 * contentScale) * kMmToPt;
                    const double x2 = (offset.x() + mark.x2 * contentScale) * kMmToPt;
                    const double y2 = (media.height() - offset.y() - mark.y2 * contentScale) * kMmToPt;
                    content += QStringLiteral("%1 w %2 %3 m %4 %5 l S\n")
                                   .arg(number(mark.widthMm * kMmToPt), number(x1), number(y1),
                                        number(x2), number(y2));
                }
            }

            const QByteArray imageData = flate(rgbBytes(pages.at(i)));
            const QImage &page = pages.at(i);

            objects[pageNum - 1] =
                QStringLiteral("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %1 %2] /Resources << "
                               "/XObject << /Im0 %3 0 R >> >> /Contents %4 0 R >>")
                    .arg(number(widthPt), number(heightPt))
                    .arg(imageNum)
                    .arg(contentNum)
                    .toLatin1();

            objects[contentNum - 1] = QByteArray("<< /Length ")
                + QByteArray::number(content.toLatin1().size()) + QByteArray(" >>\nstream\n")
                + content.toLatin1() + QByteArray("endstream");

            objects[imageNum - 1] =
                QByteArray("<< /Type /XObject /Subtype /Image /Width ")
                + QByteArray::number(page.width()) + QByteArray(" /Height ")
                + QByteArray::number(page.height()) + QByteArray(" /ColorSpace [ /ICCBased ")
                + QByteArray::number(iccNum) + QByteArray(" 0 R ] /BitsPerComponent 8 /Filter "
                                                          "/FlateDecode /Length ")
                + QByteArray::number(imageData.size()) + QByteArray(" >>\nstream\n") + imageData
                + QByteArray("\nendstream");
        }

        objects[iccNum - 1] = QByteArray("<< /N 3 /Alternate /DeviceRGB /Filter /FlateDecode "
                                         "/Length ")
            + QByteArray::number(iccCompressed.size()) + QByteArray(" >>\nstream\n") + iccCompressed
            + QByteArray("\nendstream");

        objects[metadataNum - 1] = QByteArray("<< /Type /Metadata /Subtype /XML /Filter "
                                              "/FlateDecode /Length ")
            + QByteArray::number(xmpCompressed.size()) + QByteArray(" >>\nstream\n") + xmpCompressed
            + QByteArray("\nendstream");

        objects[infoNum - 1] = QStringLiteral("<< /Title (PhotoPrint) /Producer (PhotoPrint) "
                                              "/Creator (PhotoPrint) >>")
                                   .toLatin1();

        QByteArray document;
        document += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
        QList<int> offsets;
        offsets.resize(objects.size());
        for (int i = 0; i < objects.size(); ++i) {
            offsets[i] = document.size();
            document += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
        }
        const int xrefOffset = document.size();
        document += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n";
        document += "0000000000 65535 f \n";
        for (int offset : offsets)
            document += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        const QByteArray id = QCryptographicHash::hash(document, QCryptographicHash::Md5).toHex();
        document += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R"
            + " /Info " + QByteArray::number(infoNum) + " 0 R /ID [<" + id + "><" + id + ">] >>\n";
        document += "startxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            *errorText = QStringLiteral("cannot-write");
            return false;
        }
        if (file.write(document) != document.size()) {
            *errorText = QStringLiteral("short-write");
            return false;
        }
        if (!file.commit()) {
            *errorText = QStringLiteral("commit-failed");
            return false;
        }
        return true;
    }

private:
    static QByteArray xmpPacket()
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
};

} // namespace

PageRenderer::PageRenderer(ImageStore &store) : m_store(store) {}

void paintPageContent(QPainter &painter, const DocumentSnapshot &snapshot, int pageIndex,
                      const QHash<QString, QImage> &images)
{
    if (pageIndex < 0 || pageIndex >= snapshot.pages.size())
        return;
    const Page &page = snapshot.pages.at(pageIndex);
    const QTransform pageTransform = painter.transform();

    for (const Frame &frame : page.frames) {
        const auto it = images.constFind(frame.id);
        if (it == images.constEnd() || it->isNull())
            continue;
        if (!LayoutEngine::validFrame(frame))
            continue;
        const QSize pixels = it->size();
        const QTransform imageTransform = LayoutEngine::imageToPage(frame, pixels);

        QTransform frameTransform;
        frameTransform.translate(frame.cx, frame.cy);
        frameTransform.rotate(frame.angleDeg);

        painter.save();
        painter.setTransform(frameTransform * pageTransform);
        painter.setClipRect(
            QRectF(-frame.width / 2.0, -frame.height / 2.0, frame.width, frame.height));
        painter.setTransform(imageTransform * pageTransform);
        painter.drawImage(QRectF(0, 0, pixels.width(), pixels.height()), *it);
        painter.restore();
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

Result<QImage> PageRenderer::renderPage(const DocumentSnapshot &snapshot, int pageIndex, double ppi,
                                        std::stop_token token, const RenderOptions &options) const
{
    if (pageIndex < 0 || pageIndex >= snapshot.pages.size())
        return error(QStringLiteral("page-index"), QCoreApplication::translate("PhotoPrint", "Ez az oldal nem található."));
    if (!(ppi > 0.0) || !std::isfinite(ppi))
        return error(QStringLiteral("ppi"), QCoreApplication::translate("PhotoPrint", "A felbontás érvénytelen."));

    const Page &page = snapshot.pages.at(pageIndex);
    const QSizeF pageSize = pageSizeMm(page);
    if (!(pageSize.width() > 0.0) || !(pageSize.height() > 0.0))
        return error(QStringLiteral("page-size"), QCoreApplication::translate("PhotoPrint", "A papírméret érvénytelen."));

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
        Result<SourceImage> source = options.verifySources
            ? m_store.reloadVerified(frame.sourcePath, frame.sourceSha256, token)
            : m_store.load(frame.sourcePath, ImportPolicy{}, token);
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
            // The placed frame is the print raster. Grading still happens at
            // the 2400 px engine bound; the finish stages run at this size.
            effectTarget.printSize =
                QSize(std::max(1, qRound(frame.width / 25.4 * ppi)),
                      std::max(1, qRound(frame.height / 25.4 * ppi)));
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

    const QImage srgb = ColorPipeline::linearToSrgb(canvas, snapshot.recipe.grayscale, token);
    if (srgb.isNull())
        return error(QStringLiteral("colour-conversion"),
                     QCoreApplication::translate("PhotoPrint", "A lapot most nem tudjuk elkészíteni. Próbáld újra."));
    return srgb;
}

Result<ExportArtifact> PageRenderer::writePdf(const DocumentSnapshot &snapshot, const QString &path,
                                              std::stop_token token,
                                              const RenderOptions &options) const
{
    if (snapshot.pages.isEmpty())
        return error(QStringLiteral("empty-document"), QCoreApplication::translate("PhotoPrint", "Nincs mit nyomtatni."));

    QList<QImage> pageImages;
    QList<QSizeF> sizes;
    QList<QSizeF> mediaSizes;
    QList<QPointF> offsets;
    QList<std::vector<Mark>> marks;
    const double ppi = options.printPpi > 0.0 && std::isfinite(options.printPpi)
        ? options.printPpi
        : 300.0;

    for (int i = 0; i < snapshot.pages.size(); ++i) {
        Result<QImage> rendered = renderPage(snapshot, i, ppi, token, options);
        if (!rendered)
            return rendered.error();
        pageImages.append(rendered.value());
        const QSizeF size = pageSizeMm(snapshot.pages.at(i));
        sizes.append(size);
        const QSizeF media = options.mediaSizeMm.width() > 0.0 && options.mediaSizeMm.height() > 0.0
            ? options.mediaSizeMm
            : size;
        mediaSizes.append(media);
        offsets.append(options.contentOffsetMm);
        marks.push_back(options.developerMarks ? developerMarksMm(size) : std::vector<Mark>{});
    }

    QString errorText;
    PdfWriter writer;
    if (!writer.write(path, pageImages, sizes, mediaSizes, offsets, marks,
                      options.contentScale, true, &errorText))
        return error(QStringLiteral("pdf-write"), QCoreApplication::translate("PhotoPrint", "A PDF mentése nem sikerült."));

    ExportArtifact artifact;
    artifact.path = path;
    artifact.sha256 = fileSha256(path);
    artifact.pageCount = pageImages.size();
    artifact.firstPageSizeMm = mediaSizes.first();
    if (artifact.sha256.isEmpty())
        return error(QStringLiteral("pdf-read-back"), QCoreApplication::translate("PhotoPrint", "A PDF mentése nem sikerült."));
    return artifact;
}

} // namespace pp
