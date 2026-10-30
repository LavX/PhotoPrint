#include "TrialSheetBuilder.h"

#include "LayoutEngine.h"

#include <QCoreApplication>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace pp {

Result<TrialSheet> TrialSheetBuilder::build(const DocumentSnapshot &document,
                                            const QString &frameId,
                                            const PrintContext &context)
{
    const Frame *source = findFrame(document, frameId);
    if (!source)
        return error(QStringLiteral("trial-frame"), QCoreApplication::translate("PhotoPrint", "Válassz egy fotót a próbalaphoz."));
    if (document.pages.isEmpty())
        return error(QStringLiteral("trial-page"), QCoreApplication::translate("PhotoPrint", "Nincs mit kipróbálni."));

    const Page &originalPage = document.pages.at(document.currentPage);
    TrialSheet sheet;
    sheet.context = context;

    const double corrections[] = {0.0, 0.06, -0.06, kTrialPaperEv};
    QList<double> unique;
    for (double value : corrections) {
        value = std::clamp(value, -kTrialPaperEv, kTrialPaperEv);
        if (!unique.contains(value))
            unique.append(value);
    }

    Page page;
    page.id = makeId(QStringLiteral("page"));
    page.paper = originalPage.paper;
    page.landscape = originalPage.landscape;
    const QSizeF size = pageSizeMm(page);
    // Fixed millimetre margins do not exist on tiny paper: they would drive the
    // cell size through zero. Scale them down with the short side, but keep the
    // familiar 10/8 mm layout on the photo papers this sheet is meant for.
    const double shortSide = std::min(size.width(), size.height());
    // Finite as well as positive: an infinite sheet would pass "greater than
    // zero" and turn every cell size below into infinity.
    if (!(shortSide > 0.0) || !std::isfinite(size.width()) || !std::isfinite(size.height()))
        return error(QStringLiteral("trial-paper"),
                     QCoreApplication::translate("PhotoPrint", "A papírméret érvénytelen."));
    const double margin = std::min(10.0, shortSide * 0.1);
    const double gutter = std::min(8.0, shortSide * 0.08);
    const double cellWidth = (size.width() - 2.0 * margin - gutter) / 2.0;
    const double cellHeight = (size.height() - 2.0 * margin - gutter) / 2.0;
    // Unreachable by construction: the margins are fractions of the short side,
    // so both expressions stay above 0.72 * side for any positive finite sheet.
    // The guard stays anyway, as defence against a future margin change, and it
    // is what keeps the translated message in the catalogue.
    if (!(cellWidth > 0.0) || !(cellHeight > 0.0))
        return error(QStringLiteral("trial-paper"),
                     QCoreApplication::translate("PhotoPrint",
                                                  "Ez a papír túl kicsi a próbalaphoz."));
    const double padding = std::min(4.0, std::min(cellWidth, cellHeight) * 0.15);

    for (int i = 0; i < unique.size(); ++i) {
        Frame frame = *source;
        frame.id = makeId(QStringLiteral("trial"));
        // Trial cells are small; 2x upscaling adds no visible detail and would
        // multiply memory by four for every variant.
        frame.upscale = false;
        frame.label = QString::number(i + 1);
        frame.paperOverride = true;
        frame.paperEv = unique.at(i);
        frame.angleDeg = 0.0;

        // A source with a zero dimension has no usable aspect ratio; 1:1 keeps
        // the arithmetic finite instead of dividing by zero below.
        const double ratio = source->width > 0.0 && source->height > 0.0
            ? source->height / source->width
            : 1.0;
        const double availableWidth = cellWidth - padding;
        const double availableHeight = cellHeight - padding;
        double width = availableWidth;
        double height = width * ratio;
        if (!(height > 0.0) || !std::isfinite(height) || height > availableHeight) {
            height = availableHeight;
            width = std::isfinite(ratio) && ratio > 0.0 ? height / ratio : availableWidth;
        }
        // The cell may shrink far below a millimetre on very small paper; it
        // must never grow past the slot it sits in. The floor is tiny on
        // purpose: a 1 mm floor used to stretch a 0.46 mm high cell up to 1 mm,
        // which broke the image's aspect ratio on the smallest sheets.
        frame.width = std::clamp(width, std::min(1e-3, availableWidth), availableWidth);
        frame.height = std::clamp(height, std::min(1e-3, availableHeight), availableHeight);
        const int column = i % 2;
        const int row = i / 2;
        frame.cx = margin + column * (cellWidth + gutter) + cellWidth / 2.0;
        frame.cy = margin + row * (cellHeight + gutter) + cellHeight / 2.0;

        page.frames.append(frame);
        sheet.variants.append({i + 1, frame.id, unique.at(i)});
    }
    sheet.document.pages.append(page);
    sheet.document.currentPage = 0;
    sheet.document.recipe = document.recipe;
    sheet.document.recipe.paperStrength = 0.0;
    // The sheet is rendered and printed with the same quality choice as the
    // document it was built from.
    sheet.document.fineQuality = document.fineQuality;
    return sheet;
}

} // namespace pp
