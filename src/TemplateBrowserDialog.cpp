#include "TemplateBrowserDialog.h"

#include "PaperCatalog.h"
#include "PaperGeometry.h"
#include "SlotLayoutEngine.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAccessible>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace pp {

namespace {

/// A preview raster is capped in device pixels, so a 200 % or 300 % display does
/// not build four or nine times the raster of a thumbnail nobody zooms into.
constexpr int kMaxPreviewPx = 260;
constexpr int kNumberBadge = 14;
/// One rebuild per burst of edits. Adding a photo, nudging a frame or changing
/// the paper all publish document changes; six previews are not re-rendered for
/// every one of them.
constexpr int kSyncDelayMs = 120;

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
    orderByDisplay();

    m_syncTimer = new QTimer(this);
    m_syncTimer->setSingleShot(true);
    m_syncTimer->setInterval(kSyncDelayMs);
    connect(m_syncTimer, &QTimer::timeout, this, &TemplateBrowserDialog::runPendingSync);

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
    m_grid->setAccessibleName(gridAccessibleName());
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

    // The action that helps, and only when it helps: fewer photos than the
    // layout needs can be fixed by adding them; more photos cannot, so that case
    // explains itself instead of offering a button that makes it worse.
    m_directionLabel = new QLabel(this);
    m_directionLabel->setObjectName(QStringLiteral("templateDirection"));
    m_directionLabel->setWordWrap(true);
    m_directionLabel->setVisible(false);
    outer->addWidget(m_directionLabel);

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

    m_noticeLabel = new QLabel(this);
    m_noticeLabel->setObjectName(QStringLiteral("templatePositionNotice"));
    m_noticeLabel->setWordWrap(true);
    m_noticeLabel->setVisible(false);
    outer->addWidget(m_noticeLabel);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setObjectName(QStringLiteral("templateApplyError"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setVisible(false);
    outer->addWidget(m_errorLabel);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    m_applyButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Alkalmaz"), this);
    m_applyButton->setObjectName(QStringLiteral("templateApplyButton"));
    m_applyButton->setAutoDefault(false);
    m_cancelButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Mégsem"), this);
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
        // A new choice is a new question: the previous refusal does not apply.
        m_applyError.clear();
        refreshSelection();
        refreshStatus();
    });
    connect(m_grid, &QListWidget::itemActivated, this, [this](QListWidgetItem *) {
        // Activation arrives from inside the view's own event handling. Applying
        // publishes an edit, and the sync may rebuild this grid; clearing the
        // item under the emitting view is not safe, so the choice runs after the
        // emission has returned.
        QMetaObject::invokeMethod(this, [this] { chooseSelected(); }, Qt::QueuedConnection);
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

    selectCount(std::clamp(int(m_request.frames.size()), 2, 10));
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

QString TemplateBrowserDialog::signature() const
{
    // Everything a preview depends on. The frames' geometry is deliberately
    // absent: moving a photo does not change any thumbnail, and rebuilding six
    // of them for a nudge is exactly the cost this guards.
    QStringList parts;
    parts << m_request.paper.name
          << QString::number(m_request.paper.physicalWidthMm, 'f', 3)
          << QString::number(m_request.paper.physicalHeightMm, 'f', 3)
          << (m_request.landscape ? QStringLiteral("landscape") : QStringLiteral("portrait"))
          << QString::number(currentChipCount());
    for (const QImage &photo : m_request.photos)
        parts << QString::number(photo.isNull() ? 0 : photo.cacheKey());
    return parts.join(QLatin1Char('|'));
}

void TemplateBrowserDialog::scheduleSync()
{
    m_syncTimer->start();
}

void TemplateBrowserDialog::runPendingSync()
{
    if (signature() == m_builtSignature) {
        // Only the photographs' positions changed, so nothing a preview shows is
        // stale; the guidance can still change, because the crop report reads
        // the frames.
        refreshStatus();
        return;
    }
    rebuildTiles();
}

bool TemplateBrowserDialog::waitForPendingSync(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (m_syncTimer->isActive() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return !m_syncTimer->isActive();
}

void TemplateBrowserDialog::setPage(const PaperSpec &paper, bool landscape,
                                    const QList<Frame> &frames, const QList<QImage> &photos,
                                    const QList<Slot> &slotList)
{
    m_request.paper = paper;
    m_request.landscape = landscape;
    m_request.frames = frames;
    m_request.slotList = slotList;
    m_request.photos = photos;
    if (m_request.photos.size() < m_request.frames.size())
        m_request.photos.resize(m_request.frames.size());
    orderByDisplay();

    // Follow the page: the chip is the page's photo count, clamped to the
    // catalogue. A count the page cannot have is not offered as if it were
    // current.
    const int target = std::clamp(int(m_request.frames.size()), 2, 10);
    if (target != currentChipCount()) {
        m_building = true;
        if (QAbstractButton *chip = m_chipGroup->button(target))
            chip->setChecked(true);
        m_building = false;
    }
    scheduleSync();
}

void TemplateBrowserDialog::orderByDisplay()
{
    if (!m_request.frames.isEmpty() && m_request.photos.size() < m_request.frames.size())
        m_request.photos.resize(m_request.frames.size());
    if (m_request.frames.size() < 2 || m_request.slotList.isEmpty())
        return;

    // The page's own fill order, not the z-order the frames happen to be stored
    // in: a slotted page lists its photographs by slot index first. Both lists
    // are parallel, so they are reordered with the same indices.
    Page page;
    page.paper = m_request.paper;
    page.landscape = m_request.landscape;
    page.frames = m_request.frames;
    page.slotList = m_request.slotList;
    const QList<int> order = SlotLayoutEngine::frameIndicesInDisplayOrder(page);
    if (order.size() != m_request.frames.size())
        return;

    QList<Frame> frames;
    QList<QImage> photos;
    frames.reserve(order.size());
    photos.reserve(order.size());
    for (int index : order) {
        frames.append(m_request.frames.at(index));
        photos.append(m_request.photos.value(index));
    }
    m_request.frames = frames;
    m_request.photos = photos;
}

void TemplateBrowserDialog::rebuildTiles()
{
    const QSize preview = previewSize();
    m_grid->setIconSize(preview);
    m_grid->setGridSize(QSize(preview.width() + 20, preview.height() + 52));
    m_grid->setAccessibleName(gridAccessibleName());

    m_building = true;
    m_grid->clear();
    m_tiles.clear();
    m_applyError.clear();

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
        // A layout that does not fit this paper stays selectable: the page has
        // to be able to say why it cannot be used, and Apply stays off for it.
        item->setToolTip(tile.mapped ? t.nameEn : tile.paperMessage);
        if (!tile.mapped)
            item->setForeground(palette().color(QPalette::Disabled, QPalette::WindowText));
        m_tiles.append(tile);
    }

    // Keep the choice only while it still maps. A layout that stopped fitting
    // after a paper or orientation change is dropped, never swapped for another:
    // silently choosing a second layout would apply something nobody picked.
    int keep = indexOfTemplate(m_selectedId);
    if (keep >= 0 && !m_tiles.at(keep).mapped)
        keep = -1;
    m_grid->setCurrentRow(keep);
    m_selectedId = keep >= 0 ? m_tiles.at(keep).id : QString();
    m_building = false;

    m_builtSignature = signature();
    ++m_rebuilds;
    refreshSelection();
    refreshStatus();
}

QPixmap TemplateBrowserDialog::renderPreview(const QList<QRectF> &slotRects, bool marked) const
{
    const QSize size = previewSize();
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
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
            // Crop at the device raster, not at the logical box, so a HiDPI
            // preview shows the crop it will print instead of an upscale of a
            // screen-sized one.
            const QSize target((box.size() * dpr).toSize());
            const QImage scaled = photo.scaled(target, Qt::KeepAspectRatioByExpanding,
                                               Qt::SmoothTransformation);
            const int cropX = std::max(0, (scaled.width() - target.width()) / 2);
            const int cropY = std::max(0, (scaled.height() - target.height()) / 2);
            painter.drawImage(box, scaled, QRectF(cropX, cropY, target.width(), target.height()));
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
        // selection colour alone. It sits in the bottom-right corner: slot
        // numbers are drawn in the top-left corner of their slot, so the badge
        // can never cover one.
        const QRectF badge(sheet.right() - 20.0, sheet.bottom() - 20.0, 15.0, 15.0);
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
    const QString chosenLabel = QCoreApplication::translate("PhotoPrint", "Kiválasztott");
    for (int i = 0; i < m_tiles.size() && i < m_grid->count(); ++i) {
        TemplateTileState &tile = m_tiles[i];
        const bool chosen = tile.id == m_selectedId && tile.mapped;
        if (chosen && tile.marked.isNull())
            tile.marked = renderPreview(tile.slotRects, true);
        QListWidgetItem *item = m_grid->item(i);
        item->setIcon(QIcon(chosen && !tile.marked.isNull() ? tile.marked : tile.preview));
        // The selection state is exposed to a screen reader as a word, not only
        // as the painted check.
        item->setData(Qt::AccessibleDescriptionRole, chosen ? chosenLabel : QString());
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
    if (index >= 0)
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

QString TemplateBrowserDialog::templateAccessibleDescription(const QString &id) const
{
    const int index = indexOfTemplate(id);
    if (index < 0 || index >= m_grid->count())
        return QString();
    return m_grid->item(index)->data(Qt::AccessibleDescriptionRole).toString();
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
    return index >= 0 && m_tiles.at(index).id == m_selectedId && m_tiles.at(index).mapped;
}

bool TemplateBrowserDialog::templateMapped(const QString &id) const
{
    const int index = indexOfTemplate(id);
    return index >= 0 && m_tiles.at(index).mapped;
}

QString TemplateBrowserDialog::gridAccessibleName() const
{
    return QStringLiteral("%1 · %2").arg(windowTitle(), paperLabel(m_request.paper));
}

bool TemplateBrowserDialog::applyEnabled() const
{
    return m_applyButton && m_applyButton->isEnabled();
}

QString TemplateBrowserDialog::countMismatchText() const
{
    return m_mismatchLabel && m_mismatchLabel->isVisible() ? m_mismatchLabel->text() : QString();
}

QString TemplateBrowserDialog::directionHintText() const
{
    return m_directionLabel && m_directionLabel->isVisible() ? m_directionLabel->text() : QString();
}

QString TemplateBrowserDialog::capacityText() const
{
    return m_capacityLabel && m_capacityLabel->isVisible() ? m_capacityLabel->text() : QString();
}

QString TemplateBrowserDialog::paperMessageText() const
{
    return m_paperLabel && m_paperLabel->isVisible() ? m_paperLabel->text() : QString();
}

QString TemplateBrowserDialog::positionNoticeText() const
{
    return m_noticeLabel && m_noticeLabel->isVisible() ? m_noticeLabel->text() : QString();
}

QString TemplateBrowserDialog::applyErrorText() const
{
    return m_errorLabel && m_errorLabel->isVisible() ? m_errorLabel->text() : QString();
}

bool TemplateBrowserDialog::addPhotosVisible() const
{
    return m_addPhotosButton && m_addPhotosButton->isVisible();
}

void TemplateBrowserDialog::refreshStatus()
{
    const int photos = m_request.frames.size();
    const int chip = currentChipCount();
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

    // A count difference belongs to the chip, not to the chosen tile: the line
    // appears as soon as the chip is clicked, with or without a choice.
    const bool mismatch = !tooMany && chip >= 2 && photos != chip;
    if (mismatch) {
        // The same sentence the engine refuses with, so the line and the error
        // can never drift apart.
        m_mismatchLabel->setText(
            QCoreApplication::translate("PhotoPrint",
                                        "Ehhez a sablonhoz %1 fotó kell, a lapon most %2 van.")
                .arg(chip)
                .arg(photos));
        m_mismatchLabel->setAccessibleName(m_mismatchLabel->text());
    }
    m_mismatchLabel->setVisible(mismatch);

    // Only the direction that can be fixed by adding photos offers the button.
    QString direction;
    if (mismatch && photos > chip) {
        const int surplus = photos - chip;
        direction = surplus == 1
            ? QCoreApplication::translate(
                  "PhotoPrint", "Válassz %1 fotós sablont, vagy távolíts el egy fotót.")
                  .arg(photos)
            : QCoreApplication::translate(
                  "PhotoPrint", "Válassz %1 fotós sablont, vagy távolíts el %n fotót.", nullptr,
                  surplus)
                  .arg(photos);
        m_directionLabel->setAccessibleName(direction);
    }
    m_directionLabel->setText(direction);
    m_directionLabel->setVisible(!direction.isEmpty());
    m_addPhotosButton->setVisible(mismatch && photos < chip);

    // A paper message belongs to the chosen layout. With nothing chosen it still
    // explains a count that cannot be used at all, and it can never sit next to
    // an enabled Apply, because Apply needs a mapped layout.
    QString paperMessage;
    if (tile && !tile->mapped) {
        paperMessage = tile->paperMessage;
    } else if (!tile) {
        for (const TemplateTileState &candidate : m_tiles) {
            if (!candidate.mapped) {
                paperMessage = candidate.paperMessage;
                break;
            }
        }
    }
    m_paperLabel->setText(paperMessage);
    m_paperLabel->setAccessibleName(paperMessage);
    m_paperLabel->setVisible(!paperMessage.isEmpty());

    // What Apply would do, from the engine's own report: the layout moves every
    // photo into its slot, and any crop or rotation it resets is named in the
    // same sentence instead of staying invisible.
    QString notice;
    const bool applicable = tile && tile->mapped && t && tile->photoCount == photos && photos > 0;
    if (applicable) {
        Page page;
        page.paper = m_request.paper;
        page.landscape = m_request.landscape;
        page.frames = m_request.frames;
        CollageApplyReport report;
        const auto applied = CollageTemplates::instance().apply(*t, page, &report);
        if (applied) {
            const bool crops = !report.cropResetIds.isEmpty();
            const bool rotations = !report.rotationResetIds.isEmpty();
            if (crops && rotations) {
                notice = QCoreApplication::translate(
                    "PhotoPrint",
                    "Az új elrendezés minden fotót a saját helyére tesz, a kivágásokat és az "
                    "elforgatást is ehhez igazítjuk.");
            } else if (crops) {
                notice = QCoreApplication::translate(
                    "PhotoPrint",
                    "Az új elrendezés minden fotót a saját helyére tesz, a kivágásokat ehhez "
                    "igazítjuk.");
            } else if (rotations) {
                notice = QCoreApplication::translate(
                    "PhotoPrint",
                    "Az új elrendezés minden fotót a saját helyére tesz, az elforgatást nullára "
                    "állítjuk.");
            } else {
                notice = QCoreApplication::translate(
                    "PhotoPrint", "Az új elrendezés minden fotót a saját helyére tesz.");
            }
        }
    }
    m_noticeLabel->setText(notice);
    m_noticeLabel->setAccessibleName(notice);
    m_noticeLabel->setVisible(!notice.isEmpty());

    m_errorLabel->setText(m_applyError);
    m_errorLabel->setAccessibleName(m_applyError);
    m_errorLabel->setVisible(!m_applyError.isEmpty());

    m_applyButton->setEnabled(applicable);

    announce();
}

void TemplateBrowserDialog::announce()
{
    // Everything the footer currently tells the person, in reading order. A
    // change is announced once instead of being left for a screen reader to
    // discover on its next pass.
    QStringList lines;
    for (const QLabel *label : {m_mismatchLabel, m_directionLabel, m_capacityLabel, m_paperLabel,
                                m_noticeLabel, m_errorLabel}) {
        if (label && label->isVisible() && !label->text().isEmpty())
            lines.append(label->text());
    }
    const QString guidance = lines.join(QLatin1Char(' '));
    if (guidance == m_announced)
        return;
    const bool hadGuidance = !m_announced.isEmpty();
    m_announced = guidance;
    setAccessibleDescription(guidance);
    if (hadGuidance && !guidance.isEmpty() && isVisible())
        QAccessible::updateAccessibility(new QAccessibleEvent(this, QAccessible::Alert));
}

void TemplateBrowserDialog::reportApplyFailure(const QString &message)
{
    m_applyError = message;
    refreshStatus();
}

void TemplateBrowserDialog::chooseSelected()
{
    const int index = indexOfTemplate(m_selectedId);
    if (index < 0 || !m_tiles.at(index).mapped || !applyEnabled())
        return;
    // The dialog never applies and never closes itself: the receiver publishes
    // the edit, and a refusal comes back through reportApplyFailure().
    emit templateChosen(m_selectedId);
}

void TemplateBrowserDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // A deliberate start: the chosen layout when there is one, otherwise the
    // chip row, because nothing is selected and Apply is off.
    if (m_grid->currentRow() >= 0)
        m_grid->setFocus();
    else if (QAbstractButton *chip = m_chipGroup->checkedButton())
        chip->setFocus();
    else
        m_grid->setFocus();
}

} // namespace pp
