#include "TrialSheetBuilder.h"

#include "LayoutEngine.h"

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
        return error(QStringLiteral("trial-frame"), QStringLiteral("Válassz egy fotót a próbalaphoz."));
    if (document.pages.isEmpty())
        return error(QStringLiteral("trial-page"), QStringLiteral("Nincs mit kipróbálni."));

    const Page &originalPage = document.pages.at(document.currentPage);
    TrialSheet sheet;
    sheet.context = context;

    const double corrections[] = {0.0, 0.06, -0.06, 0.12};
    QList<double> unique;
    for (double value : corrections) {
        value = std::clamp(value, -0.12, 0.12);
        if (!unique.contains(value))
            unique.append(value);
    }

    Page page;
    page.id = makeId(QStringLiteral("page"));
    page.paper = originalPage.paper;
    page.landscape = originalPage.landscape;
    const QSizeF size = pageSizeMm(page);
    const double margin = 10.0;
    const double gutter = 8.0;
    const double cellWidth = (size.width() - 2.0 * margin - gutter) / 2.0;
    const double cellHeight = (size.height() - 2.0 * margin - gutter) / 2.0;

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

        const double ratio = source->width > 0.0 ? source->height / source->width : 1.0;
        double width = cellWidth - 4.0;
        double height = width * ratio;
        if (height > cellHeight - 4.0) {
            height = cellHeight - 4.0;
            width = height / ratio;
        }
        frame.width = width;
        frame.height = height;
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
    return sheet;
}

} // namespace pp
