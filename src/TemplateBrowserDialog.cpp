#include "TemplateBrowserDialog.h"

#include "PaperCatalog.h"
#include "PaperGeometry.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <functional>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

/// A preview raster is capped in device pixels, so a 200 % or 300 % display does
/// not build four or nine times the raster of a thumbnail nobody zooms into.
constexpr int kMaxPreviewPx = 260;
constexpr int kNumberBadge = 14;

/// The paper aspect a preview is drawn at. A paper that cannot describe a sheet
/// falls back to the 10 × 15 cm ratio instead of dividing by zero; the slots
/// themselves already fail with template-too-small on such a page.
QSizeF safePageMm(const QSizeF &pageMm)
{
    if (std::isfinite(pageMm.width()) && std::isfinite(pageMm.height())
        && pageMm.width() > 0.0 && pageMm.height() > 0.0)
        return pageMm;
    return QSizeF(100.0, 150.0);
}

/// One count chip. A checkable button gets focus, a click and a screen reader
/// for free; the arrows move the whole selection, which is what a chip row is
/// for.
class CountChip : public QPushButton
{
public:
    explicit CountChip(int count, QWidget *parent = nullptr)
        : QPushButton(QString::number(count), parent), m_count(count)
    {
        setObjectName(QStringLiteral("templateCountChip"));
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(38, 32);
        setAccessibleName(QCoreApplication::translate("PhotoPrint", "%n fotó", nullptr, count));
    }

    int count() const { return m_count; }

    std::function<void(int)> onStep;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        int delta = 0;
        if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Down)
            delta = 1;
        else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Up)
            delta = -1;
        if (delta != 0) {
            if (onStep)
                onStep(delta);
            event->accept();
            return;
        }
        QPushButton::keyPressEvent(event);
    }

private:
    int m_count;
};

} // namespace

TemplateBrowserDialog::TemplateBrowserDialog(const TemplateBrowserRequest &request, QWidget *parent)
    : QDialog(parent), m_request(request)
{
    setObjectName(QStringLiteral("templateBrowserDialog"));
    const QString title =
        QCoreApplication::translate("PhotoPrint", "Sablonok ehhez a laphoz");
    setWindowTitle(title);
    setAccessibleName(title);
    setModal(false);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumSize(500, 600);
    resize(540, 720);

    if (m_request.photos.size() < m_request.frames.size())
        m_request.photos.resize(m_request.frames.size());

    buildUi();
    fillChips();
    rebuildTiles();
    refreshStatus();
}

QString TemplateBrowserDialog::countLabel(int count) const
{
    return QCoreApplication::translate("PhotoPrint", "%n fotó", nullptr, count);
}

void TemplateBrowserDialog::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 14, 16, 14);
    outer->setSpacing(10);

    // The page the picker describes: its paper and its orientation, so it is
    // clear which sheet the layouts are mapped onto.
    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("templateSubtitle"));
    QPalette secondary = m_subtitle->palette();
    secondary.setColor(QPalette::WindowText, secondary.color(QPalette::PlaceholderText));
    m_subtitle->setPalette(secondary);
    outer->addWidget(m_subtitle);

    auto *chipHeader = new QLabel(QCoreApplication::translate("PhotoPrint", "Hány fotóhoz?"), this);
    chipHeader->setObjectName(QStringLiteral("templateChipHeader"));
    QFont headerFont = chipHeader->font();
    headerFont.setBold(true);
    chipHeader->setFont(headerFont);
    outer->addWidget(chipHeader);

    // Every count 2..10 is browsable, whatever the page holds: browsing is how
    // somebody with an empty page discovers what the feature offers.
    m_chipRow = new QWidget(this);
    m_chipRow->setObjectName(QStringLiteral("templateCountChips"));
    m_chipRow->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Fotószám"));
    m_chipLayout = new QHBoxLayout(m_chipRow);
    m_chipLayout->setContentsMargins(0, 0, 0, 0);
    m_chipLayout->setSpacing(6);
    m_chipGroup = new QButtonGroup(this);
    m_chipGroup->setExclusive(true);
    outer->addWidget(m_chipRow);

    m_grid = new QListWidget(this);
    m_grid->setObjectName(QStringLiteral("templateGrid"));
    m_grid->setAccessibleName(windowTitle());
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setSpacing(10);
    m_grid->setWordWrap(true);
    m_grid->setUniformItemSizes(true);
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(m_grid, 1);

    m_mismatchLabel = new QLabel(this);
    m_mismatchLabel->setObjectName(QStringLiteral("templateMismatch"));
    m_mismatchLabel->setWordWrap(true);
    m_mismatchLabel->setVisible(false);
    outer->addWidget(m_mismatchLabel);

    m_addPhotosButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Fotók hozzáadása"), this);
    m_addPhotosButton->setObjectName(QStringLiteral("templateAddPhotosButton"));
    m_addPhotosButton->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Fotók hozzáadása"));
    m_addPhotosButton->setAutoDefault(false);
    m_addPhotosButton->setVisible(false);
    outer->addWidget(m_addPhotosButton);

    m_capacityLabel = new QLabel(this);
    m_capacityLabel->setObjectName(QStringLiteral("templateCapacity"));
    m_capacityLabel->setWordWrap(true);
    m_capacityLabel->setVisible(false);
    outer->addWidget(m_capacityLabel);

    m_paperLabel = new QLabel(this);
    m_paperLabel->setObjectName(QStringLiteral("templatePaperMessage"));
    m_paperLabel->setWordWrap(true);
    m_paperLabel->setVisible(false);
    outer->addWidget(m_paperLabel);

    m_cropLabel = new QLabel(this);
    m_cropLabel->setObjectName(QStringLiteral("templateCropWarning"));
    m_cropLabel->setWordWrap(true);
    m_cropLabel->setVisible(false);
    outer->addWidget(m_cropLabel);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    m_applyButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Alkalmaz"), this);
    m_applyButton->setObjectName(QStringLiteral("templateApplyButton"));
    m_applyButton->setAutoDefault(false);
    m_cancelButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Mégse"), this);
    m_cancelButton->setObjectName(QStringLiteral("templateCancelButton"));
    m_cancelButton->setAutoDefault(false);
    buttonRow->addWidget(m_applyButton);
    buttonRow->addWidget(m_cancelButton);
    outer->addLayout(buttonRow);

    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_applyButton, &QPushButton::clicked, this, &TemplateBrowserDialog::chooseSelected);
    connect(m_addPhotosButton, &QPushButton::clicked, this,
            &TemplateBrowserDialog::addPhotosRequested);
    connect(m_chipGroup, &QButtonGroup::idClicked, this, [this](int count) {
        if (!m_building)
            selectCount(count);
    });
    connect(m_grid, &QListWidget::currentRowChanged, this, [this](int) {
        if (m_building)
            return;
        const int row = m_grid->currentRow();
        m_selectedId = row >= 0 && row < m_tiles.size() ? m_tiles.at(row).id : QString();
        refreshSelection();
        refreshStatus();
    });
    connect(m_grid, &QListWidget::itemActivated, this, [this](QListWidgetItem *) {
        chooseSelected();
    });
}

void TemplateBrowserDialog::fillChips()
{
    m_building = true;
    for (auto *button : std::as_const(m_chipButtons)) {
        m_chipGroup->removeButton(button);
        delete button;
    }
    m_chipButtons.clear();
    for (int count = 2; count <= 10; ++count) {
        auto *chip = new CountChip(count, m_chipRow);
        chip->setToolTip(countLabel(count));
        chip->onStep = [this](int delta) { stepCount(delta); };
        m_chipGroup->addButton(chip, count);
        m_chipLayout->addWidget(chip);
        m_chipButtons.append(chip);
    }
    m_chipLayout->addStretch(1);
    m_building = false;

    const int photos = m_request.frames.size();
    selectCount(std::clamp(photos, 2, 10));
}

int TemplateBrowserDialog::countChipCount() const
{
    return m_chipButtons.size();
}

int TemplateBrowserDialog::countChipValue(int index) const
{
    if (index < 0 || index >= m_chipButtons.size())
        return 0;
    return static_cast<CountChip *>(m_chipButtons.at(index))->count();
}

bool TemplateBrowserDialog::countChipEnabled(int count) const
{
    for (QPushButton *chip : m_chipButtons) {
        if (static_cast<CountChip *>(chip)->count() == count)
            return chip->isEnabled();
    }
    return false;
}

void TemplateBrowserDialog::selectCount(int count)
{
    if (count < 2 || count > 10)
        return;
    if (QAbstractButton *chip = m_chipGroup->button(count))
        chip->setChecked(true);
    if (!m_building)
        rebuildTiles();
}

void TemplateBrowserDialog::stepCount(int delta)
{
    const int next = std::clamp(selectedCount() + delta, 2, 10);
    selectCount(next);
    if (QAbstractButton *chip = m_chipGroup->button(next))
        chip->setFocus();
}

int TemplateBrowserDialog::selectedCount() const
{
    return currentChipCount();
}

int TemplateBrowserDialog::currentChipCount() const
{
    return m_chipGroup ? std::max(0, m_chipGroup->checkedId()) : 0;
}

QSizeF TemplateBrowserDialog::pageMm() const
{
    return safePageMm(PaperGeometry::forPaper(m_request.paper, m_request.landscape).physicalSizeMm());
}

QSize TemplateBrowserDialog::previewSize() const
{
    // The width is the requested logical width; the cap applies to the device
    // raster, so a HiDPI screen gets a sharper preview up to the same budget
    // instead of a raster that grows with the scale factor.
    const QSizeF mm = pageMm();
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
    const int logicalWidth = std::max(40, m_request.tileLogicalPx);
    const int deviceWidth = std::min(kMaxPreviewPx, int(std::lround(logicalWidth * dpr)));
    const int deviceHeight = std::clamp(int(std::lround(deviceWidth * mm.height() / mm.width())),
                                        60, kMaxPreviewPx * 2);
    return QSize(std::max(40, int(std::lround(deviceWidth / dpr))),
                 std::max(20, int(std::lround(deviceHeight / dpr))));
}

void TemplateBrowserDialog::rebuildTiles()
{
    const QSize preview = previewSize();
    m_grid->setIconSize(preview);
    m_grid->setGridSize(QSize(preview.width() + 20, preview.height() + 52));

    m_building = true;
    m_grid->clear();
    m_tiles.clear();

    const int count = currentChipCount();
    const auto &catalogue = CollageTemplates::instance();
    for (const CollageTemplate &t : catalogue.templatesForCount(count)) {
        TemplateTileState tile;
        tile.id = t.id;
        tile.nameEn = t.nameEn;
        tile.photoCount = t.photoCount;
        // `slots` is a Qt macro keyword, so the local carries the mapping.
        const auto mapping = catalogue.slotsMm(t, m_request.paper, m_request.landscape);
        tile.mapped = mapping.ok();
        if (tile.mapped)
            tile.slotRects = mapping.value();
        else
            tile.paperMessage = mapping.error().message;
        tile.preview = renderPreview(tile.slotRects, false);
        tile.marked = QPixmap();

        auto *item = new QListWidgetItem(m_grid);
        item->setIcon(QIcon(tile.preview));
        item->setText(t.nameEn);
        item->setTextAlignment(Qt::AlignHCenter);
        item->setData(Qt::UserRole, t.id);
        item->setData(Qt::AccessibleTextRole, templateAccessibleName(t.id));
        if (tile.mapped) {
            item->setToolTip(t.nameEn);
        } else {
            // A layout too small for this paper stays visible so the page can
            // explain why it cannot be used, but it cannot be chosen.
            item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
            item->setToolTip(tile.paperMessage);
        }
        m_tiles.append(tile);
    }

    // Keep the choice when the page changed under the dialog; otherwise start
    // on the first layout that can actually be applied.
    int keep = indexOfTemplate(m_selectedId);
    if (keep < 0 || !m_tiles.at(keep).mapped) {
        keep = -1;
        for (int i = 0; i < m_tiles.size(); ++i) {
            if (m_tiles.at(i).mapped) {
                keep = i;
                break;
            }
        }
    }
    m_grid->setCurrentRow(keep);
    m_selectedId = keep >= 0 ? m_tiles.at(keep).id : QString();
    m_building = false;

    refreshSelection();
    refreshStatus();
}

QPixmap TemplateBrowserDialog::renderPreview(const QList<QRectF> &slotRects, bool marked) const
{
    const QSize size = previewSize();
    const qreal dpr = devicePixelRatioF();
    QImage image(QSize(int(std::lround(size.width() * dpr)), int(std::lround(size.height() * dpr))),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF sheet(0.5, 0.5, size.width() - 1.0, size.height() - 1.0);
    painter.setPen(QPen(QColor(0, 0, 0, 70), 1.0));
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(sheet, 3.0, 3.0);

    const QSizeF mm = pageMm();
    const double scaleX = sheet.width() / mm.width();
    const double scaleY = sheet.height() / mm.height();

    if (slotRects.isEmpty()) {
        // No mappable layout on this paper: the page stays empty and a single
        // diagonal line says "nothing fits here" without claiming a layout.
        painter.setPen(QPen(QColor(0, 0, 0, 55), 1.0, Qt::DashLine));
        painter.drawLine(sheet.topLeft() + QPointF(4, 4), sheet.bottomRight() - QPointF(4, 4));
        painter.drawLine(sheet.bottomLeft() + QPointF(4, -4), sheet.topRight() + QPointF(-4, 4));
    }

    QFont numberFont = painter.font();
    numberFont.setPixelSize(9);
    numberFont.setBold(true);
    painter.setFont(numberFont);

    for (int i = 0; i < slotRects.size(); ++i) {
        const QRectF &slot = slotRects.at(i);
        const QRectF box(sheet.left() + slot.x() * scaleX, sheet.top() + slot.y() * scaleY,
                         slot.width() * scaleX, slot.height() * scaleY);
        if (box.width() < 2.0 || box.height() < 2.0)
            continue;
        QPainterPath clip;
        clip.addRoundedRect(box, 2.0, 2.0);
        painter.save();
        painter.setClipPath(clip);
        // Slot i is the i-th photograph of the page: the fill order is the
        // page's own order, so the preview never invents a new sequence.
        const QImage photo = m_request.photos.value(i);
        if (!photo.isNull()) {
            const QImage scaled = photo.scaled(box.size().toSize(), Qt::KeepAspectRatioByExpanding,
                                               Qt::SmoothTransformation);
            const int cropX = std::max(0, (scaled.width() - int(box.width())) / 2);
            const int cropY = std::max(0, (scaled.height() - int(box.height())) / 2);
            painter.drawImage(box, scaled,
                              QRectF(cropX, cropY, box.width(), box.height()));
        } else {
            painter.fillRect(box, QColor(0xd4, 0xd4, 0xda));
        }
        painter.restore();

        painter.setPen(QPen(QColor(0, 0, 0, 45), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box, 2.0, 2.0);

        // The number is the fill order, drawn even when a photo fills the slot.
        if (box.width() >= kNumberBadge + 3 && box.height() >= kNumberBadge + 3) {
            const QRectF badge(box.left() + 3.0, box.top() + 3.0, kNumberBadge, kNumberBadge);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 155));
            painter.drawEllipse(badge);
            painter.setPen(Qt::white);
            painter.drawText(badge, Qt::AlignCenter, QString::number(i + 1));
        }
    }

    if (marked) {
        // The chosen layout carries a check, so the choice does not rest on the
        // selection colour alone.
        const QRectF badge(sheet.right() - 17.0, sheet.top() + 5.0, 15.0, 15.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Highlight));
        painter.drawEllipse(badge);
        painter.setPen(palette().color(QPalette::HighlightedText));
        QFont checkFont = painter.font();
        checkFont.setPixelSize(10);
        checkFont.setBold(true);
        painter.setFont(checkFont);
        painter.drawText(badge, Qt::AlignCenter, QStringLiteral("\u2713"));
    }

    return QPixmap::fromImage(image);
}

void TemplateBrowserDialog::refreshSelection()
{
    for (int i = 0; i < m_tiles.size() && i < m_grid->count(); ++i) {
        TemplateTileState &tile = m_tiles[i];
        if (!tile.mapped)
            continue;
        const bool chosen = tile.id == m_selectedId;
        if (chosen && tile.marked.isNull())
            tile.marked = renderPreview(tile.slotRects, true);
        m_grid->item(i)->setIcon(QIcon(chosen && !tile.marked.isNull() ? tile.marked : tile.preview));
    }
}

int TemplateBrowserDialog::indexOfTemplate(const QString &id) const
{
    if (id.isEmpty())
        return -1;
    for (int i = 0; i < m_tiles.size(); ++i) {
        if (m_tiles.at(i).id == id)
            return i;
    }
    return -1;
}

const CollageTemplate *TemplateBrowserDialog::templateFor(const QString &id) const
{
    return CollageTemplates::instance().find(id);
}

int TemplateBrowserDialog::templateTileCount() const
{
    return m_tiles.size();
}

QStringList TemplateBrowserDialog::templateIds() const
{
    QStringList ids;
    ids.reserve(m_tiles.size());
    for (const TemplateTileState &tile : m_tiles)
        ids.append(tile.id);
    return ids;
}

void TemplateBrowserDialog::selectTemplate(const QString &id)
{
    const int index = indexOfTemplate(id);
    if (index < 0 || !m_tiles.at(index).mapped)
        return;
    m_grid->setCurrentRow(index);
}

QString TemplateBrowserDialog::selectedTemplateId() const
{
    return m_selectedId;
}

QString TemplateBrowserDialog::templateLabel(const QString &id) const
{
    const CollageTemplate *t = templateFor(id);
    return t ? t->nameEn : QString();
}

QString TemplateBrowserDialog::templateAccessibleName(const QString &id) const
{
    const CollageTemplate *t = templateFor(id);
    if (!t)
        return QString();
    // The count and the layout name, because the visible label is the single
    // English product name and nothing else.
    return QStringLiteral("%1 · %2").arg(countLabel(t->photoCount), t->nameEn);
}

QImage TemplateBrowserDialog::templateTileImage(const QString &id) const
{
    const int index = indexOfTemplate(id);
    if (index < 0)
        return QImage();
    const TemplateTileState &tile = m_tiles.at(index);
    const QPixmap &source = tile.id == m_selectedId && !tile.marked.isNull() ? tile.marked
                                                                            : tile.preview;
    return source.toImage();
}

bool TemplateBrowserDialog::templateMarkedChosen(const QString &id) const
{
    const int index = indexOfTemplate(id);
    if (index < 0)
        return false;
    return m_tiles.at(index).id == m_selectedId && m_tiles.at(index).mapped;
}

bool TemplateBrowserDialog::templateEnabled(const QString &id) const
{
    const int index = indexOfTemplate(id);
    return index >= 0 && m_tiles.at(index).mapped;
}

bool TemplateBrowserDialog::applyEnabled() const
{
    return m_applyButton && m_applyButton->isEnabled();
}

QString TemplateBrowserDialog::countMismatchText() const
{
    return m_mismatchLabel && m_mismatchLabel->isVisible() ? m_mismatchLabel->text() : QString();
}

QString TemplateBrowserDialog::capacityText() const
{
    return m_capacityLabel && m_capacityLabel->isVisible() ? m_capacityLabel->text() : QString();
}

QString TemplateBrowserDialog::paperMessageText() const
{
    return m_paperLabel && m_paperLabel->isVisible() ? m_paperLabel->text() : QString();
}

QString TemplateBrowserDialog::cropWarningText() const
{
    return m_cropLabel && m_cropLabel->isVisible() ? m_cropLabel->text() : QString();
}

bool TemplateBrowserDialog::addPhotosVisible() const
{
    return m_addPhotosButton && m_addPhotosButton->isVisible();
}

void TemplateBrowserDialog::refreshStatus()
{
    const int photos = m_request.frames.size();
    const int index = indexOfTemplate(m_selectedId);
    const TemplateTileState *tile = index >= 0 ? &m_tiles.at(index) : nullptr;
    const CollageTemplate *t = tile ? templateFor(tile->id) : nullptr;

    m_subtitle->setText(QStringLiteral("%1 · %2")
                            .arg(paperLabel(m_request.paper),
                                 m_request.landscape
                                     ? QCoreApplication::translate("PhotoPrint", "Fekvő")
                                     : QCoreApplication::translate("PhotoPrint", "Álló")));

    const bool tooMany = photos > 10;
    m_capacityLabel->setText(QCoreApplication::translate(
        "PhotoPrint", "Ehhez a laphoz legfeljebb 10 fotóhoz van sablon."));
    m_capacityLabel->setVisible(tooMany);

    const bool mismatch = !tooMany && t && t->photoCount != photos;
    if (mismatch) {
        // The same sentence the engine refuses with, so the line and the error
        // can never drift apart.
        m_mismatchLabel->setText(
            QCoreApplication::translate("PhotoPrint",
                                        "Ehhez a sablonhoz %1 fotó kell, a lapon most %2 van.")
                .arg(t->photoCount)
                .arg(photos));
    }
    m_mismatchLabel->setVisible(mismatch);
    m_addPhotosButton->setVisible(mismatch);

    QString paperMessage;
    if (tile && !tile->mapped)
        paperMessage = tile->paperMessage;
    if (paperMessage.isEmpty()) {
        for (const TemplateTileState &candidate : m_tiles) {
            if (!candidate.mapped) {
                paperMessage = candidate.paperMessage;
                break;
            }
        }
    }
    m_paperLabel->setText(paperMessage);
    m_paperLabel->setVisible(!paperMessage.isEmpty());

    // The crop warning is the apply() report itself, computed on the page copy.
    // Nothing is published here: the dialog only tells the truth about what the
    // engine would reset.
    QString cropWarning;
    if (tile && tile->mapped && t && tile->photoCount == photos && photos > 0) {
        Page page;
        page.paper = m_request.paper;
        page.landscape = m_request.landscape;
        page.frames = m_request.frames;
        CollageApplyReport report;
        const auto applied = CollageTemplates::instance().apply(*t, page, &report);
        if (applied && !report.isEmpty()) {
            cropWarning = QCoreApplication::translate(
                "PhotoPrint", "A kivágásokat az új elrendezéshez igazítjuk.");
        }
    }
    m_cropLabel->setText(cropWarning);
    m_cropLabel->setVisible(!cropWarning.isEmpty());

    m_applyButton->setEnabled(tile && tile->mapped && t && t->photoCount == photos && photos > 0);
}

void TemplateBrowserDialog::chooseSelected()
{
    if (!applyEnabled() || m_selectedId.isEmpty())
        return;
    emit templateChosen(m_selectedId);
    accept();
}

void TemplateBrowserDialog::setPage(const PaperSpec &paper, bool landscape,
                                    const QList<Frame> &frames, const QList<QImage> &photos)
{
    m_request.paper = paper;
    m_request.landscape = landscape;
    m_request.frames = frames;
    m_request.photos = photos;
    if (m_request.photos.size() < m_request.frames.size())
        m_request.photos.resize(m_request.frames.size());
    rebuildTiles();
}

} // namespace pp
