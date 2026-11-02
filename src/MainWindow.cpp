#include "MainWindow.h"

#include "AboutDialog.h"
#include "CollageTemplates.h"
#include "ColorPipeline.h"
#include "EffectBrowserDialog.h"
#include "LayoutEngine.h"
#include "PhotoItem.h"
#include "JobJournal.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"
#include "SheetLayout.h"
#include "SheetToolsDialog.h"
#include "TemplateBrowserDialog.h"
#include "Translation.h"
#include "TrialSheetBuilder.h"
#include "ToneEngine.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPixmap>
#include <QGuiApplication>
#include <QStyleHints>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPlainTextEdit>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QGuiApplication>
#include <QStyleHints>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

constexpr double kHandleHalfPx = 6.0;
constexpr double kRotateOffsetPx = 30.0;

// Canvas raster policy. The composed page is a page-local bitmap: the view
// transform (pan) never touches it; only the page content and the resolution
// do. A zoom or a resize paints the existing bitmap scaled and sharpens once
// the view stops moving, on a worker thread.
constexpr int kSettleDelayMs = 120;
constexpr qint64 kMaxRasterPixels = 8000000; // retained raster ceiling (8 MP)
constexpr int kEditPulseMs = 200;            // crop zoom or arrow nudge burst
// How far one 120-unit wheel notch turns the photograph inside its slot when
// Shift is held. Small enough to feel continuous, big enough to see per notch.
constexpr double kImageRotationWheelStepDeg = 3.0;
// A layout style edit follows the spin live and lands as one edit once the user
// stops: short enough to feel immediate, long enough that a held arrow is one
// undo step.
constexpr int kStyleDebounceMs = 180;
// A raster is kept while it is between 0.98x and 2x the view target: a slightly
// larger raster is free and sharper than rebuilding a smaller one, and a 2x
// surplus is still worth keeping. Outside that band the settle rebuilds.
constexpr double kRasterMinFactor = 0.98;
constexpr double kRasterMaxFactor = 2.0;

/// Marks a label as secondary text. The theme stylesheet owns the colour, so
/// the label follows light and dark mode and keeps the normal label size
/// instead of a hardcoded grey at the smallest size.
void markSecondaryText(QLabel *label)
{
    if (label)
        label->setProperty("secondary", true);
}

Page emptyPage(const PaperSpec &paper)
{
    Page page;
    page.id = makeId(QStringLiteral("page"));
    page.paper = paper;
    page.landscape = false;
    return page;
}

} // namespace

namespace {

struct ThemeColors {
    bool dark = false;
    QColor window;
    QColor panel;
    QColor canvas;
    QColor text;
    QColor secondary;
    QColor border;
    QColor input;
    QColor hover;
    QColor accent;
    QColor selected;
    QColor pageBorder;
    QColor shadow;
};

ThemeColors themeColors()
{
    ThemeColors colors;
    const QString override = qEnvironmentVariable("PHOTOPRINT_THEME");
    colors.dark = override == QLatin1String("dark");
    if (override.isEmpty())
        colors.dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;

    if (colors.dark) {
        colors.window = QColor(0x1c, 0x1c, 0x1e);
        colors.panel = QColor(0x2c, 0x2c, 0x2e);
        colors.canvas = QColor(0x10, 0x10, 0x12);
        colors.text = QColor(0xf5, 0xf5, 0xf7);
        colors.secondary = QColor(0xa1, 0xa1, 0xa6);
        colors.border = QColor(0x3a, 0x3a, 0x3c);
        colors.input = QColor(0x2c, 0x2c, 0x2e);
        colors.hover = QColor(0x3a, 0x3a, 0x3c);
        colors.accent = QColor(0x0a, 0x84, 0xff);
        colors.selected = QColor(0x0a, 0x84, 0xff, 48);
        colors.pageBorder = QColor(0x48, 0x48, 0x4a);
        colors.shadow = QColor(0, 0, 0, 150);
    } else {
        colors.window = QColor(0xf5, 0xf5, 0xf7);
        colors.panel = QColor(0xff, 0xff, 0xff);
        colors.canvas = QColor(0xd9, 0xd9, 0xde);
        colors.text = QColor(0x1d, 0x1d, 0x1f);
        colors.secondary = QColor(0x6e, 0x6e, 0x73);
        colors.border = QColor(0xd2, 0xd2, 0xd7);
        colors.input = QColor(0xff, 0xff, 0xff);
        colors.hover = QColor(0xf5, 0xf5, 0xf7);
        colors.accent = QColor(0x00, 0x7a, 0xff);
        colors.selected = QColor(0x00, 0x7a, 0xff, 28);
        colors.pageBorder = QColor(0xc7, 0xc7, 0xcc);
        colors.shadow = QColor(0, 0, 0, 38);
    }
    return colors;
}

QPalette themePalette(const ThemeColors &colors)
{
    QPalette palette;
    palette.setColor(QPalette::Window, colors.window);
    palette.setColor(QPalette::WindowText, colors.text);
    palette.setColor(QPalette::Base, colors.input);
    palette.setColor(QPalette::AlternateBase, colors.hover);
    palette.setColor(QPalette::ToolTipBase, colors.panel);
    palette.setColor(QPalette::ToolTipText, colors.text);
    palette.setColor(QPalette::Text, colors.text);
    palette.setColor(QPalette::Button, colors.input);
    palette.setColor(QPalette::ButtonText, colors.text);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Link, colors.accent);
    palette.setColor(QPalette::Highlight, colors.accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, colors.secondary);
    palette.setColor(QPalette::Mid, colors.border);
    palette.setColor(QPalette::Disabled, QPalette::Text, colors.secondary);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, colors.secondary);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, colors.secondary);
    return palette;
}

QString themedStyleSheet(const ThemeColors &c)
{
    QString qss = QStringLiteral(R"(
QMainWindow { background: @window; }
QWidget#library { background: @panel; border-right: 1px solid @border; }
QWidget#inspector { background: @panel; border-left: 1px solid @border; }
QToolBar { background: @window; border: none; border-bottom: 1px solid @border; spacing: 7px; padding: 3px 10px; }
QToolBar QLabel { color: @secondary; font-size: 11px; }
QMenuBar { background: @window; color: @text; }
QMenuBar::item { background: transparent; padding: 4px 9px; }
QMenuBar::item:selected { background: @hover; border-radius: 5px; }
QMenu { background: @panel; color: @text; border: 1px solid @border; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 10px; border-radius: 5px; }
QMenu::item:selected { background: @accent; color: #ffffff; }
QComboBox, QSpinBox, QDoubleSpinBox { background: @input; border: 1px solid @border; border-radius: 7px; padding: 3px 8px; min-height: 24px; color: @text; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { border: 1px solid @border; background: @panel; color: @text; selection-background-color: @accent; selection-color: #ffffff; outline: 0; }
QComboBox QAbstractItemView::item { color: @text; min-height: 24px; padding: 3px 8px; }
QComboBox QAbstractItemView::item:selected { background: @accent; color: #ffffff; }
QPushButton { background: @input; border: 1px solid @border; border-radius: 7px; padding: 5px 10px; color: @text; }
QPushButton:hover { background: @hover; }
QPushButton:pressed { background: @border; }
QPushButton:checked { background: @selected; border-color: @accent; color: @accent; }
QPushButton#printButton { background: @accent; color: #ffffff; border: none; font-weight: 600; border-radius: 9px; padding: 8px 18px; }
QPushButton#printButton:hover { background: @accent; }
QPushButton#addButton { background: @accent; color: #ffffff; border: none; border-radius: 8px; padding: 8px 12px; font-weight: 600; }
QPushButton#addButton:hover { background: @accent; }
QLabel#sectionHeader { color: @secondary; font-size: 11px; font-weight: 600; margin-top: 10px; }
QListWidget { border: none; background: transparent; outline: none; color: @text; }
QListWidget::item { border-radius: 9px; padding: 4px; color: @text; }
QListWidget::item:selected { background: @selected; color: @text; }
QListWidget#pageList { background: transparent; color: @secondary; }
QListWidget#pageList::item { color: @secondary; border-radius: 8px; }
QListWidget#pageList::item:selected { background: @selected; color: @accent; font-weight: 600; }
QSlider::groove:horizontal { height: 4px; background: @border; border-radius: 2px; }
QSlider::handle:horizontal { width: 16px; height: 16px; margin: -6px 0; border-radius: 8px; background: #ffffff; border: 1px solid @border; }
QSlider::groove:horizontal:disabled { background: @border; }
QSlider::sub-page:horizontal:disabled { background: @border; }
QSlider::handle:horizontal:disabled { background: @hover; border-color: @border; }
QLabel:disabled { color: @secondary; }
QPushButton:disabled { color: @secondary; background: @window; border-color: @border; }
QCheckBox:disabled { color: @secondary; }
QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: @secondary; background: @window; }
QWidget#previewBusyOverlay { background: @panel; border: 1px solid @border; border-radius: 8px; }
QWidget#previewBusyOverlay QLabel { color: @text; }
QWidget#previewBusyOverlay QPushButton { padding: 2px 8px; color: @accent; border: none; background: transparent; }
QCheckBox { spacing: 7px; color: @text; }
QStatusBar { background: @window; color: @secondary; }
QDialog, QMessageBox { background: @panel; color: @text; }
QMessageBox QLabel { color: @text; }
QMessageBox QPushButton { min-width: 80px; }
QToolTip { background: @panel; color: @text; border: 1px solid @border; padding: 4px 6px; border-radius: 5px; }
QLabel { color: @text; }
QLabel[secondary="true"] { color: @secondary; }
)");

    auto replace = [&qss](const char *token, const QColor &color) {
        qss.replace(QLatin1String(token), color.name(QColor::HexArgb));
    };
    replace("@window", c.window);
    replace("@panel", c.panel);
    replace("@text", c.text);
    replace("@secondary", c.secondary);
    replace("@border", c.border);
    replace("@input", c.input);
    replace("@hover", c.hover);
    replace("@accent", c.accent);
    replace("@selected", c.selected);
    return qss;
}

/// Every library entry gets the same square tile, so the grid stays even even
/// when the photos have different shapes.
QPixmap libraryThumbnail(const QImage &image, int size)
{
    QPixmap tile(size, size);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setPen(QPen(QColor(128, 128, 128, 70), 1));
    painter.setBrush(QColor(128, 128, 128, 22));
    painter.drawRoundedRect(QRectF(0.5, 0.5, size - 1.0, size - 1.0), 6, 6);
    const int inset = 5;
    const QImage scaled =
        image.scaled(size - inset * 2, size - inset * 2, Qt::KeepAspectRatio,
                     Qt::SmoothTransformation);
    painter.drawImage(QPoint((size - scaled.width()) / 2, (size - scaled.height()) / 2), scaled);
    return tile;
}

} // namespace

class ToggleSwitch : public QAbstractButton
{
public:
    explicit ToggleSwitch(QWidget *parent = nullptr) : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    QSize sizeHint() const override { return QSize(42, 24); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF track = QRectF(rect()).adjusted(1, 3, -1, -3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(isChecked() ? palette().color(QPalette::Highlight)
                                     : palette().color(QPalette::Mid));
        painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);
        const double diameter = track.height() - 4.0;
        const QRectF knob(isChecked() ? track.right() - diameter - 2.0 : track.left() + 2.0,
                          track.top() + 2.0, diameter, diameter);
        painter.setBrush(Qt::white);
        painter.setPen(QPen(QColor(0, 0, 0, 35), 0.5));
        painter.drawEllipse(knob);
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75), 7, 7);
        }
    }
};

PhotoListWidget::PhotoListWidget(QWidget *parent) : QListWidget(parent)
{
    // The list is the page's frames, so a drag out of it is a move. The canvas
    // is the only drop target; the list itself must not reorder by dragging.
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDefaultDropAction(Qt::CopyAction);
}

QMimeData *PhotoListWidget::mimeData(const QList<QListWidgetItem *> &items) const
{
    if (items.isEmpty())
        return nullptr;
    const QString frameId = items.first()->data(Qt::UserRole).toString();
    if (frameId.isEmpty())
        return nullptr;
    auto *data = new QMimeData;
    data->setData(QString::fromLatin1(kFrameMimeType), frameId.toUtf8());
    return data;
}

PageView::PageView(QWidget *parent) : QWidget(parent)
{
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(180, 140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // paintEvent() fills the whole viewport, so Qt must not clear it first.
    setAttribute(Qt::WA_OpaquePaintEvent);

    // Zoom and resize draw the existing raster immediately and rebuild it once
    // the view has been still for a moment.
    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    m_settleTimer->setInterval(kSettleDelayMs);
    connect(m_settleTimer, &QTimer::timeout, this, [this] { settleRaster(); });

    // A wheel burst on a photograph is one gesture: the picture follows every
    // notch live, and a single edit lands once the wheel has stopped.
    m_zoomTimer = new QTimer(this);
    m_zoomTimer->setSingleShot(true);
    m_zoomTimer->setInterval(kEditPulseMs);
    connect(m_zoomTimer, &QTimer::timeout, this, [this] { flushWheelZoom(); });

    // Page rasters are built off the GUI thread. The watcher lives on this
    // object, so a result arriving after destruction is simply dropped.
    m_composeWatcher = new QFutureWatcher<ComposeResult>(this);
    connect(m_composeWatcher, &QFutureWatcher<ComposeResult>::finished, this,
            [this] { handleComposeFinished(); });

    // Preview generations can take a moment on a page of effects. The overlay
    // is a small child in the corner: it never covers the canvas and it is the
    // only thing that catches the pointer while it is visible.
    m_busyOverlay = new QWidget(this);
    m_busyOverlay->setObjectName(QStringLiteral("previewBusyOverlay"));
    m_busyOverlay->setAttribute(Qt::WA_StyledBackground, true);
    auto *busyLayout = new QHBoxLayout(m_busyOverlay);
    busyLayout->setContentsMargins(10, 5, 6, 5);
    busyLayout->setSpacing(8);
    m_busyLabel = new QLabel(QCoreApplication::translate("PhotoPrint", "Feldolgozás…"),
                             m_busyOverlay);
    m_busyLabel->setObjectName(QStringLiteral("previewBusyLabel"));
    m_busyCancel = new QPushButton(QCoreApplication::translate("PhotoPrint", "Mégsem"),
                                   m_busyOverlay);
    m_busyCancel->setObjectName(QStringLiteral("busyCancelButton"));
    m_busyCancel->setAutoDefault(false);
    m_busyCancel->setFlat(true);
    busyLayout->addWidget(m_busyLabel);
    busyLayout->addWidget(m_busyCancel);
    connect(m_busyCancel, &QPushButton::clicked, this, [this] {
        if (onCancelBusy)
            onCancelBusy();
    });
    m_busyOverlay->hide();
}

PageView::~PageView()
{
    // The worker owns copies of everything it needs, so it must not keep
    // rendering a page whose view is gone: stop it and let it unwind.
    if (m_activeStop)
        m_activeStop->request_stop();
}

void PageView::setDocument(const DocumentSnapshot &document)
{
    const auto pruneSelection = [this] {
        // A selection survives a document change for as long as the thing it
        // names still exists. A removed frame clears the frame selection; a
        // removed slot clears the cell selection; a selected photo keeps its
        // cell selected.
        if (!m_selectedId.isEmpty() && !findFrame(m_document, m_selectedId))
            m_selectedId.clear();
        const Page *page = currentPage();
        if (!m_selectedSlotId.isEmpty()
            && (!page || !SlotGeometry::slotById(*page, m_selectedSlotId)))
            m_selectedSlotId.clear();
        if (const Frame *frame = selectedFramePtr()) {
            m_selectedSlotId = frame->slotId;
        } else if (!m_selectedSlotId.isEmpty() && page) {
            // The selected cell gained a photograph (an undo, a drop, a top-up):
            // the photo is what the user wants to act on now.
            if (const Frame *occupant =
                    SlotGeometry::frameInSlot(*page, m_selectedSlotId))
                m_selectedId = occupant->id;
        }
    };

    if (document == m_document) {
        // Same document: no page pixel changed, so the composed raster stays.
        // The only visible state that can still be stale is a selection that
        // points at a frame or a slot which no longer exists.
        const QString frameBefore = m_selectedId;
        const QString slotBefore = m_selectedSlotId;
        pruneSelection();
        if (frameBefore != m_selectedId || slotBefore != m_selectedSlotId)
            update();
        return;
    }
    m_document = document;

    // A document replaced from outside (an undo, a project load, another edit)
    // makes a pending wheel burst stale: committing it would re-apply a zoom the
    // new document no longer has. Drop the burst instead of landing it.
    if (!m_zoomFrameId.isEmpty()) {
        m_zoomFrameId.clear();
        if (m_zoomTimer)
            m_zoomTimer->stop();
        m_cropZoomPulse.invalidate();
    }

    pruneSelection();
    contentChanged(true);
}

void PageView::contentChanged(bool supersede)
{
    m_contentDirty = true;
    ++m_contentRevision;
    m_composeFailed = false; // a real change earns one more attempt
    m_blankRetries = 0;
    if (supersede && m_activeStop && m_composeInFlight)
        m_activeStop->request_stop();
    update();
}

void PageView::setFrameImages(const QHash<QString, QImage> &images)
{
    // ImageStore hands out shared copies, so an unchanged set compares cheaply
    // and must not throw the composed page away.
    if (images == m_images)
        return;
    m_images = images;
    contentChanged(true);
}

QSize PageView::capRasterSize(QSize pixels, qint64 maxPixels)
{
    if (pixels.isEmpty() || maxPixels <= 0)
        return pixels;
    const qint64 area = static_cast<qint64>(pixels.width()) * pixels.height();
    if (area <= maxPixels)
        return pixels;
    const double factor = std::sqrt(static_cast<double>(maxPixels) / static_cast<double>(area));
    return QSize(std::max(1, static_cast<int>(pixels.width() * factor)),
                 std::max(1, static_cast<int>(pixels.height() * factor)));
}

QSize PageView::targetRasterSize() const
{
    const QRectF page = pageRect();
    if (page.isEmpty())
        return {};
    const double dpr = devicePixelRatioF();
    const QSize pixels(std::max(1, qRound(page.width() * dpr)),
                       std::max(1, qRound(page.height() * dpr)));
    // A 300% viewport on a 200% display would be 23 MP; the cap keeps the
    // retained raster bounded and lets the painter stretch it instead.
    return capRasterSize(pixels, kMaxRasterPixels);
}

QSize PageView::gestureRasterSize() const
{
    const QSize target = targetRasterSize();
    if (target.isEmpty())
        return target;
    // A drag composes the page at the fitted scale: the painter stretches it for
    // the duration of the gesture and the release rebuilds at full scale. That
    // is the documented "1x coarse raster", so it follows the display scale
    // instead of hiding an extra cap.
    if (m_viewZoom <= 1.0)
        return target;
    const double factor = 1.0 / m_viewZoom;
    return QSize(std::max(1, qRound(target.width() * factor)),
                 std::max(1, qRound(target.height() * factor)));
}

bool PageView::dragActive() const
{
    return m_drag.kind != DragKind::None;
}

bool PageView::cropZoomPulseActive() const
{
    return m_cropZoomPulse.isValid() && m_cropZoomPulse.elapsed() < kEditPulseMs;
}

bool PageView::nudgePulseActive() const
{
    return m_nudgePulse.isValid() && m_nudgePulse.elapsed() < kEditPulseMs;
}

bool PageView::interactionPulseActive() const
{
    return cropZoomPulseActive() || nudgePulseActive();
}

bool PageView::interactionActive() const
{
    return m_panning || dragActive() || interactionPulseActive();
}

bool PageView::buildAllowed() const
{
    // A held pan never builds while the button is down. A crop zoom is a
    // gesture, so it builds at the fitted scale like a drag; an arrow-nudge
    // burst only lets the frame outline move and composes once it ends.
    return !m_panning && !nudgePulseActive();
}

bool PageView::rasterMatchesTarget() const
{
    if (m_composed.isNull() || m_composedPageMm != pageMm())
        return false;
    const QSize target = targetRasterSize();
    if (target.isEmpty())
        return true;
    const double width = m_composed.width();
    const double height = m_composed.height();
    return width >= target.width() * kRasterMinFactor && width <= target.width() * kRasterMaxFactor
        && height >= target.height() * kRasterMinFactor
        && height <= target.height() * kRasterMaxFactor;
}

void PageView::scheduleSettle()
{
    if (!m_settleTimer || m_document.pages.isEmpty() || pageMm().isEmpty())
        return;
    m_settleTimer->start();
}

void PageView::settleRaster()
{
    if (!isVisible() || m_document.pages.isEmpty() || pageMm().isEmpty())
        return;
    if (m_panning || dragActive())
        return; // the release settles: never compose under a held button
    if (interactionPulseActive()) {
        scheduleSettle();
        return;
    }
    if (!m_composeWatcher || m_composeInFlight) {
        scheduleSettle(); // try again once the running build has landed
        return;
    }
    if (m_composeFailed)
        return; // a failed build waits for a real change or an explicit retry
    if (!m_contentDirty && rasterMatchesTarget())
        return;

    // A zoom-out leaves a raster with more pixels than the view needs. The
    // painter would scale it down anyway, so replace it with the scaled copy
    // here: that releases the surplus (32 MiB at the cap) before the worker
    // allocates the replacement, and the interim picture is unchanged.
    const QSize target = targetRasterSize();
    if (!m_composed.isNull() && !m_contentDirty && target.width() > 0
        && m_composed.width() > target.width() * 1.02) {
        m_composed =
            m_composed.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        update();
    }
    startCompose();
}

void PageView::startCompose() const
{
    if (!m_composeWatcher || m_composeInFlight)
        return;
    const QSizeF page = pageMm();
    if (page.isEmpty() || m_document.pages.isEmpty())
        return;
    // A drag and a crop zoom change the picture under the pointer: both compose
    // the fitted page and let the release (or the burst end) sharpen it.
    const bool coarse = dragActive() || cropZoomPulseActive();
    const QSize pixels = coarse ? gestureRasterSize() : targetRasterSize();
    if (pixels.isEmpty())
        return;

    ComposeJob job;
    job.generation = ++m_composeGeneration;
    job.revision = m_contentRevision;
    job.document = m_document;
    job.images = m_images;
    job.pageMm = page;
    job.pixels = pixels;
    job.delayMs = m_composeDelayMs;
    job.forceFailure = m_failCompositions;
    job.stop = std::make_shared<std::stop_source>();
    m_activeStop = job.stop;
    m_composeInFlight = true;
    ++m_composeRequests;
    m_composeWatcher->setFuture(QtConcurrent::run([job] { return composePage(job); }));
}

PageView::ComposeResult PageView::composePage(const ComposeJob &job)
{
    ComposeResult result;
    result.generation = job.generation;
    result.revision = job.revision;
    result.pageMm = job.pageMm;
    if (job.delayMs > 0)
        QThread::msleep(static_cast<unsigned long>(job.delayMs));
    if (job.stop->stop_requested()) {
        result.cancelled = true;
        return result;
    }
    if (job.forceFailure)
        return result; // an empty raster, the way a failed allocation looks

    QImage linear(job.pixels, QImage::Format_RGBA64_Premultiplied);
    if (linear.isNull())
        return result;
    linear.fill(Qt::white);
    QPainter painter(&linear);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QTransform scale;
    scale.scale(static_cast<double>(job.pixels.width()) / job.pageMm.width(),
                static_cast<double>(job.pixels.height()) / job.pageMm.height());
    painter.setTransform(scale);
    paintPageContent(painter, job.document, job.document.currentPage, job.images);
    painter.end();

    if (job.stop->stop_requested()) {
        result.cancelled = true;
        return result;
    }
    result.image =
        ColorPipeline::linearToSrgb(linear, job.document.recipe.grayscale, job.stop->get_token());
    if (result.image.isNull() && job.stop->stop_requested())
        result.cancelled = true;
    return result;
}

void PageView::handleComposeFinished()
{
    if (!m_composeWatcher || !m_composeWatcher->isFinished())
        return;
    const ComposeResult result = m_composeWatcher->result();
    if (result.generation == 0 || result.generation == m_handledGeneration)
        return; // the queued signal and a direct wait can both land here
    m_handledGeneration = result.generation;
    m_composeInFlight = false;
    m_activeStop.reset();

    if (result.cancelled) {
        // Superseded by a newer request: drop it without touching the display.
    } else if (result.pageMm != pageMm()) {
        // The paper changed while the worker ran: those pixels belong to another
        // page and must not be stretched over this one.
        m_contentDirty = true;
    } else if (result.image.isNull()) {
        // Keep the current raster and the dirty flag. Retrying on every paint
        // would spin on a permanent failure, so the guard waits for a change.
        m_composeFailed = true;
        if (m_composed.isNull() && m_blankRetries == 0) {
            // Nothing to fall back on: a blank canvas gets exactly one retry,
            // then the guard applies like anywhere else.
            ++m_blankRetries;
            m_composeFailed = false;
        }
    } else {
        installCompose(result);
    }

    if (m_composeFailed || !isVisible())
        return;
    if (m_contentDirty && buildAllowed())
        startCompose(); // a newer document arrived while this one was building
    else if (!rasterMatchesTarget() && !interactionActive())
        scheduleSettle(); // the settle that armed this build was consumed
}

void PageView::installCompose(const ComposeResult &result)
{
    m_composed = result.image;
    m_composedPageMm = result.pageMm;
    m_contentDirty = result.revision != m_contentRevision;
    m_composeFailed = false;
    ++m_compositions;
    update();
}

void PageView::ensureComposed() const
{
    const QSizeF page = pageMm();
    if (page.isEmpty() || pageRect().isEmpty() || m_document.pages.isEmpty()) {
        m_composed = QImage();
        m_composedPageMm = QSizeF();
        m_contentDirty = true;
        return;
    }
    if (!m_composed.isNull() && m_composedPageMm != page) {
        // Another paper or orientation: the old raster has the wrong aspect and
        // cannot stand in for the new page.
        m_composed = QImage();
        m_composedPageMm = QSizeF();
        m_contentDirty = true;
    }
    if (!m_contentDirty || m_composeFailed || !buildAllowed())
        return;
    startCompose(); // no-op while a build is already in flight
}

bool PageView::compositionPending() const
{
    return m_composeInFlight;
}

void PageView::retryComposition()
{
    m_composeFailed = false;
    m_blankRetries = 0;
    if (m_contentDirty || !rasterMatchesTarget()) {
        if (!m_composeInFlight)
            startCompose();
        scheduleSettle();
    }
}

void PageView::cancelInteraction(bool commitEdit)
{
    const DragState drag = m_drag;
    const bool wasActive = m_panning || dragActive() || interactionPulseActive();
    m_panning = false;
    m_drag = DragState{};
    m_cropZoomPulse.invalidate();
    m_nudgePulse.invalidate();
    setCursor(Qt::ArrowCursor);
    if (drag.kind != DragKind::None && drag.changed) {
        if (commitEdit) {
            // Losing the mouse-up is not an undo request: keep the edit.
            if (const Frame *frame = selectedFramePtr(); frame && onFrameCommitted)
                onFrameCommitted(*frame);
        } else if (Frame *frame = findFrame(m_document, drag.startFrame.id)) {
            // The widget is going away: leave the document as the user had it.
            *frame = drag.startFrame;
            ++m_contentRevision;
            m_contentDirty = true;
        }
    }
    if (wasActive)
        scheduleSettle();
}

void PageView::setViewZoom(double zoom)
{
    m_viewZoom = std::clamp(zoom, 0.25, 3.0);
    if (m_viewZoom <= 1.0)
        m_viewPan = QPointF();
    // The raster is page-local: the new zoom only changes how it is drawn. The
    // higher resolution arrives once the zooming stops.
    m_composeFailed = false;
    scheduleSettle();
    update();
}

void PageView::setSelectedFrame(const QString &frameId)
{
    if (m_selectedId == frameId)
        return;
    m_selectedId = frameId;
    if (const Frame *frame = selectedFramePtr())
        m_selectedSlotId = frame->slotId;
    update();
}

void PageView::setSelectedSlot(const QString &slotId)
{
    if (m_selectedSlotId == slotId)
        return;
    m_selectedSlotId = slotId;
    update();
}

const Page *PageView::currentPage() const
{
    if (m_document.pages.isEmpty() || m_document.currentPage < 0
        || m_document.currentPage >= m_document.pages.size())
        return nullptr;
    return &m_document.pages.at(m_document.currentPage);
}

void PageView::setUnavailableFrames(const QSet<QString> &frameIds)
{
    if (frameIds == m_unavailable)
        return;
    m_unavailable = frameIds;
    update();
}

PageView::SlotState PageView::slotStateFor(const Slot &slot) const
{
    const Page *page = currentPage();
    if (!page)
        return SlotState::Empty;
    const Frame *frame = SlotGeometry::frameInSlot(*page, slot.id);
    if (!frame)
        return SlotState::Empty;
    if (m_images.contains(frame->id) && !m_images.value(frame->id).isNull())
        return SlotState::Filled;
    // Not decoded yet is "loading", never "missing": a fresh drop goes through
    // the asynchronous preview and must not flash "not found" on the way.
    if (m_unavailable.contains(frame->id) || frame->sourcePath.isEmpty())
        return SlotState::Missing;
    return SlotState::Loading;
}

PageView::SlotState PageView::slotState(const QString &slotId) const
{
    const Page *page = currentPage();
    if (!page)
        return SlotState::Empty;
    const Slot *slot = SlotGeometry::slotById(*page, slotId);
    if (!slot)
        return SlotState::Empty;
    return slotStateFor(*slot);
}

QRectF PageView::slotRect(const QString &slotId) const
{
    const Page *page = currentPage();
    if (!page)
        return {};
    const Slot *slot = SlotGeometry::slotById(*page, slotId);
    if (!slot)
        return {};
    const QRectF cell = SlotGeometry::slotCellMm(*page, *slot);
    if (cell.isEmpty())
        return {};
    return pageToWidget().mapRect(cell);
}

QString PageView::slotAtWidget(const QPointF &widgetPoint) const
{
    const int index = hitSlot(widgetPoint);
    const Page *page = currentPage();
    if (index < 0 || !page)
        return {};
    return page->slotList.at(index).id;
}

void PageView::setCropMode(bool enabled)
{
    m_cropMode = enabled && !m_selectedId.isEmpty();
    update();
}

void PageView::setEmptyHint(const QString &text)
{
    m_emptyHint = text;
    update();
}

void PageView::setThemeColors(const QColor &canvas, const QColor &border, const QColor &shadow)
{
    m_canvasColor = canvas;
    m_pageBorderColor = border;
    m_pageShadowColor = shadow;
    update();
}

void PageView::setBusyOverlay(bool busy)
{
    if (!m_busyOverlay)
        return;
    m_busyOverlay->setVisible(busy);
    if (busy) {
        m_busyOverlay->adjustSize();
        positionBusyOverlay();
        m_busyOverlay->raise();
    }
}

bool PageView::busyOverlayVisible() const
{
    return m_busyOverlay && m_busyOverlay->isVisible();
}

void PageView::positionBusyOverlay()
{
    if (!m_busyOverlay)
        return;
    const QSize hint = m_busyOverlay->sizeHint();
    m_busyOverlay->resize(hint);
    m_busyOverlay->move(width() - hint.width() - 16, height() - hint.height() - 16);
}

QSizeF PageView::pageMm() const
{
    if (m_document.pages.isEmpty() || m_document.currentPage < 0
        || m_document.currentPage >= m_document.pages.size())
        return {};
    return pageSizeMm(m_document.pages.at(m_document.currentPage));
}

QImage PageView::composedImage() const
{
    // A test and utility accessor, never the paint path: it waits for the build
    // that matches the current document so the caller can read pixels.
    for (int pass = 0; pass < 8; ++pass) {
        if (!m_composeWatcher)
            break;
        if (!m_composeInFlight) {
            if (m_composeFailed || !m_contentDirty || m_document.pages.isEmpty()
                || pageMm().isEmpty())
                break;
            startCompose();
            if (!m_composeInFlight)
                break;
        }
        m_composeWatcher->waitForFinished();
        const_cast<PageView *>(this)->handleComposeFinished();
    }
    return m_composed;
}

QRectF PageView::pageRect() const
{
    const QSizeF page = pageMm();
    if (!(page.width() > 0.0) || !(page.height() > 0.0))
        return {};
    const QRectF area = QRectF(rect()).adjusted(24, 24, -24, -24);
    if (area.width() <= 0 || area.height() <= 0)
        return {};
    const double scale =
        std::min(area.width() / page.width(), area.height() / page.height()) * m_viewZoom;
    const QSizeF size(page.width() * scale, page.height() * scale);
    return QRectF(area.center().x() - size.width() / 2.0 + m_viewPan.x(),
                  area.center().y() - size.height() / 2.0 + m_viewPan.y(), size.width(),
                  size.height());
}

QTransform PageView::pageToWidget() const
{
    const QRectF page = pageRect();
    const QSizeF pageSize = pageMm();
    QTransform transform;
    if (page.isEmpty() || pageSize.isEmpty())
        return transform;
    transform.translate(page.left(), page.top());
    transform.scale(page.width() / pageSize.width(), page.height() / pageSize.height());
    return transform;
}

QPointF PageView::widgetToPage(const QPointF &point) const
{
    bool ok = false;
    const QTransform inverse = pageToWidget().inverted(&ok);
    return ok ? inverse.map(point) : QPointF();
}

const Frame *PageView::selectedFramePtr() const
{
    if (m_selectedId.isEmpty())
        return nullptr;
    return findFrame(m_document, m_selectedId);
}

QPolygonF PageView::frameQuadWidget(const Frame &frame) const
{
    return pageToWidget().map(PhotoItem::frameQuadMm(frame));
}

int PageView::hitFrame(const QPointF &widgetPoint) const
{
    if (m_document.pages.isEmpty())
        return -1;
    const Page &page = m_document.pages.at(m_document.currentPage);
    for (int i = page.frames.size() - 1; i >= 0; --i) {
        if (frameQuadWidget(page.frames.at(i)).containsPoint(widgetPoint, Qt::OddEvenFill))
            return i;
    }
    return -1;
}

int PageView::hitSlot(const QPointF &widgetPoint) const
{
    const Page *page = currentPage();
    if (!page || page->slotList.isEmpty())
        return -1;
    // The model owns the hit rule (cell rectangle, later slots win); the view
    // only converts the pointer to page millimetres.
    const Slot *slot = SlotGeometry::slotAt(*page, widgetToPage(widgetPoint));
    if (!slot)
        return -1;
    return SlotGeometry::slotIndexOf(*page, slot->id);
}

int PageView::hitHandle(const QPointF &widgetPoint) const
{
    const Frame *frame = selectedFramePtr();
    if (!frame)
        return -1;
    // A slot owns its rectangle: there is nothing to resize or rotate, and a
    // dragged handle would break the invariant.
    if (!frame->slotId.isEmpty())
        return -1;
    const QPolygonF quad = frameQuadWidget(*frame);
    for (int i = 0; i < 4; ++i) {
        if (QLineF(quad.at(i), widgetPoint).length() <= kHandleHalfPx + 4.0)
            return i;
    }
    const QPointF center = pageToWidget().map(QPointF(frame->cx, frame->cy));
    const QPointF topMid = (quad.at(0) + quad.at(1)) / 2.0;
    QPointF direction = topMid - center;
    const double length = std::hypot(direction.x(), direction.y());
    if (length > 0.0)
        direction /= length;
    const QPointF rotate = topMid + direction * kRotateOffsetPx;
    if (QLineF(rotate, widgetPoint).length() <= 8.0)
        return 4;
    return -1;
}

void PageView::beginDrag(DragKind kind, int handle, const QPointF &pagePoint, const Frame &frame)
{
    m_drag.kind = kind;
    m_drag.handle = handle;
    m_drag.startPage = pagePoint;
    m_drag.startFrame = frame;
    m_drag.changed = false;
    m_cropZoomPulse.invalidate(); // a drag supersedes a crop or nudge burst
    m_nudgePulse.invalidate();
    const auto it = m_images.constFind(frame.id);
    m_drag.pixels = it == m_images.constEnd() ? QSize() : it->size();
    setCursor(kind == DragKind::CropPan ? Qt::ClosedHandCursor : Qt::ArrowCursor);
}

void PageView::updateLiveFrame(const Frame &frame, bool markDragChanged)
{
    Frame *target = findFrame(m_document, frame.id);
    if (!target)
        return;
    *target = frame;
    target->autoPlaced = false;
    if (markDragChanged)
        m_drag.changed = true;
    // A move inside a drag keeps the running build: the frame preview advances
    // one composition behind the pointer instead of starving every build.
    contentChanged(false);
}

std::optional<Frame> PageView::liveFrame(const QString &frameId) const
{
    if (frameId.isEmpty())
        return std::nullopt;
    if (const Frame *frame = findFrame(m_document, frameId))
        return *frame;
    return std::nullopt;
}

void PageView::previewFrame(const Frame &frame)
{
    if (!findFrame(m_document, frame.id))
        return;
    updateLiveFrame(frame, false);
}

void PageView::takeOverWheelBurst(const QString &frameId)
{
    if (frameId.isEmpty() || m_zoomFrameId != frameId)
        return;
    m_zoomFrameId.clear();
    if (m_zoomTimer)
        m_zoomTimer->stop();
}

void PageView::flushWheelBurst()
{
    flushWheelZoom();
}

void PageView::flushWheelZoom()
{
    if (m_zoomFrameId.isEmpty())
        return;
    const QString frameId = m_zoomFrameId;
    const Frame finalFrame = m_zoomCurrentFrame;
    const Frame startFrame = m_zoomStartFrame;
    m_zoomFrameId.clear();
    if (m_zoomTimer)
        m_zoomTimer->stop();
    const bool changed = finalFrame.cropCenter != startFrame.cropCenter
        || finalFrame.zoom != startFrame.zoom || finalFrame.fit != startFrame.fit
        || finalFrame.imageRotationDeg != startFrame.imageRotationDeg;
    if (changed && !frameId.isEmpty() && onFrameCommitted)
        onFrameCommitted(finalFrame);
    // The burst is over: sharpen the raster it left behind.
    scheduleSettle();
}

void PageView::clearSelection()
{
    const bool hadSelection = !m_selectedId.isEmpty() || !m_selectedSlotId.isEmpty();
    m_selectedId.clear();
    m_selectedSlotId.clear();
    if (!hadSelection)
        return;
    update();
    if (onSelectionChanged)
        onSelectionChanged(QString());
    if (onSlotSelectionChanged)
        onSlotSelectionChanged(QString());
}

void PageView::updateDropCue()
{
    QString cue;
    const Page *page = currentPage();
    if (!m_dropSlotId.isEmpty() && page) {
        // A filled cell is replaced, not filled: the cue says so before the
        // button is released, so a photo never disappears without warning.
        if (SlotGeometry::frameInSlot(*page, m_dropSlotId))
            cue = QCoreApplication::translate("PhotoPrint", "Csere");
    }
    if (cue == m_dropCue)
        return;
    m_dropCue = cue;
    // The cue is what a screen reader hears while the drag hovers the cell; the
    // base description returns as soon as the drag leaves.
    if (!cue.isEmpty())
        setAccessibleDescription(cue);
    else
        setAccessibleDescription(m_accessibleBeforeDrag);
    update();
}

Frame PageView::draggedFrame(const QPointF &pagePoint, bool fine) const
{
    Frame frame = m_drag.startFrame;
    switch (m_drag.kind) {
    case DragKind::Move:
        frame.cx += pagePoint.x() - m_drag.startPage.x();
        frame.cy += pagePoint.y() - m_drag.startPage.y();
        break;
    case DragKind::Resize:
        frame = PhotoItem::resizedFromCorner(frame, m_drag.handle, pagePoint, !fine);
        frame = PhotoItem::clampCrop(frame, m_drag.pixels);
        break;
    case DragKind::Rotate: {
        const double dx = pagePoint.x() - frame.cx;
        const double dy = pagePoint.y() - frame.cy;
        double angle = qRadiansToDegrees(std::atan2(dy, dx)) + 90.0;
        if (fine)
            angle = std::round(angle / 15.0) * 15.0;
        while (angle > 180.0)
            angle -= 360.0;
        while (angle <= -180.0)
            angle += 360.0;
        frame.angleDeg = angle;
        break;
    }
    case DragKind::CropPan: {
        const QPointF delta = pagePoint - m_drag.startPage;
        const QSizeF image = LayoutEngine::imageSizeMm(frame, m_drag.pixels);
        if (image.width() > 0.0 && image.height() > 0.0) {
            const QPointF local = PhotoItem::toFrameLocalMm(frame, pagePoint)
                - PhotoItem::toFrameLocalMm(frame, m_drag.startPage);
            const QPointF inImage(local.x() * std::cos(qDegreesToRadians(-frame.imageRotationDeg))
                                      - local.y() * std::sin(qDegreesToRadians(-frame.imageRotationDeg)),
                                  local.x() * std::sin(qDegreesToRadians(-frame.imageRotationDeg))
                                      + local.y() * std::cos(qDegreesToRadians(-frame.imageRotationDeg)));
            frame.cropCenter.setX(frame.cropCenter.x() - inImage.x() / image.width());
            frame.cropCenter.setY(frame.cropCenter.y() - inImage.y() / image.height());
            frame = PhotoItem::clampCrop(frame, m_drag.pixels);
        }
        Q_UNUSED(delta)
        break;
    }
    case DragKind::None:
        break;
    }
    return frame;
}

void PageView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    // WA_OpaquePaintEvent: the whole viewport is painted here, every time.
    painter.fillRect(rect(), m_canvasColor);
    const QRectF page = pageRect();
    if (page.isEmpty()) {
        m_composed = QImage();
        m_composedPageMm = QSizeF();
        m_contentDirty = true;
        return;
    }

    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    painter.fillRect(page.adjusted(2, 4, 2, 6), m_pageShadowColor);
    painter.fillRect(page, Qt::white);

    // The composed raster is page-local, so drawing it is a pure transform of
    // the view: a pan repaints one blit and never a recomposition. A build that
    // is still running simply leaves the previous raster on screen.
    ensureComposed();
    if (!m_composed.isNull())
        painter.drawImage(page, m_composed);

    // Cut guides are not part of the page composition: the raster, the PDF and
    // the printer share one painter that never draws them, and the screen adds
    // its own copy here. Both sides take the geometry from
    // SheetLayout::cutGuideLinesMm, so they cannot drift apart.
    if (const Page *cutPage = currentPage()) {
        if (cutPage->cutGuides != CutGuides::None) {
            // The screen-only mark is orange, deliberately not the selection
            // accent: a guide and the selection can be on screen at the same
            // time, and the two must not look like one thicker outline.
            QPen guidePen = cutPage->cutGuides == CutGuides::Print
                ? QPen(QColor(0x7f, 0x7f, 0x7f), 1.0, Qt::SolidLine)
                : QPen(QColor(0xc4, 0x55, 0x00), 1.0, Qt::DashLine);
            // 1 device pixel at every zoom: the marks are furniture, not a
            // hairline the zoom is supposed to thicken.
            guidePen.setCosmetic(true);
            painter.setPen(guidePen);
            painter.setBrush(Qt::NoBrush);
            painter.setTransform(pageToWidget());
            for (const QLineF &line : SheetLayout::cutGuideLinesMm(*cutPage))
                painter.drawLine(line);
            painter.resetTransform();
        }
    }

    painter.setPen(QPen(m_pageBorderColor, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(page);

    if (!m_emptyHint.isEmpty()) {
        painter.setPen(QColor(0x55, 0x58, 0x5d));
        QFont font = painter.font();
        font.setPointSizeF(font.pointSizeF() * 1.1);
        painter.setFont(font);
        const QRectF hintArea = page.adjusted(page.width() * 0.12, page.height() * 0.42,
                                              -page.width() * 0.12, -page.height() * 0.42);
        painter.drawText(hintArea, Qt::AlignCenter | Qt::TextWordWrap, m_emptyHint);
    }

    // Placeholders and the cell selection are editor furniture: they live here,
    // never in paintPageContent, so no raster, PDF or printer can ever see them.
    if (const Page *current = currentPage())
        paintSlotOverlays(painter, *current);

    const Frame *frame = selectedFramePtr();
    if (!frame) {
        if (hasFocus()) {
            QPen focus(QColor(0x2c, 0x6c, 0xd6));
            focus.setWidth(2);
            focus.setStyle(Qt::DashLine);
            painter.setPen(focus);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect().adjusted(1, 1, -2, -2));
        }
        return;
    }

    const QPolygonF quad = frameQuadWidget(*frame);
    QPen outline(palette().color(QPalette::Highlight));
    outline.setWidthF(1.6);
    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(quad);

    // A slotted frame's rectangle is the slot's: no resize or rotate handles,
    // because a slot is fixed by definition.
    if (frame->slotId.isEmpty()) {
        painter.setBrush(Qt::white);
        for (int i = 0; i < 4; ++i) {
            const QPointF point = quad.at(i);
            painter.drawRect(QRectF(point.x() - kHandleHalfPx, point.y() - kHandleHalfPx,
                                    kHandleHalfPx * 2.0, kHandleHalfPx * 2.0));
        }

        const QPointF center = pageToWidget().map(QPointF(frame->cx, frame->cy));
        const QPointF topMid = (quad.at(0) + quad.at(1)) / 2.0;
        QPointF direction = topMid - center;
        const double length = std::hypot(direction.x(), direction.y());
        if (length > 0.0)
            direction /= length;
        const QPointF rotate = topMid + direction * kRotateOffsetPx;
        painter.setPen(QPen(palette().color(QPalette::Highlight), 1.2));
        painter.drawLine(topMid, rotate);
        painter.setBrush(Qt::white);
        painter.drawEllipse(rotate, 5.0, 5.0);
    }

    if (m_cropMode) {
        QPainterPath dim;
        dim.setFillRule(Qt::OddEvenFill);
        dim.addRect(page);
        dim.addPolygon(quad);
        painter.fillPath(dim, QColor(255, 255, 255, 170));
        QPen cropPen(QColor(0x00, 0x7a, 0x5e), 1.4, Qt::DashLine);
        painter.setPen(cropPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(quad);
    }

    if (hasFocus()) {
        QPen focus(QColor(0x2c, 0x6c, 0xd6));
        focus.setWidth(2);
        focus.setStyle(Qt::DashLine);
        painter.setPen(focus);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
}

void PageView::paintSlotOverlays(QPainter &painter, const Page &page) const
{
    if (page.slotList.isEmpty())
        return;
    const QColor accent = palette().color(QPalette::Highlight);
    const Slot *firstEmpty = SlotGeometry::firstEmptySlot(page);
    const QTransform transform = pageToWidget();
    const QFont baseFont = painter.font();

    for (const Slot &slot : page.slotList) {
        const QRectF cell = transform.mapRect(SlotGeometry::slotCellMm(page, slot));
        if (cell.width() < 4.0 || cell.height() < 4.0)
            continue;
        const SlotState state = slotStateFor(slot);
        const bool hovered = m_dropSlotId == slot.id;
        const bool selected = m_selectedSlotId == slot.id;
        const QRectF ring = cell.adjusted(1.0, 1.0, -1.0, -1.0);

        QColor border;
        QColor fill;
        QString text;
        switch (state) {
        case SlotState::Filled:
            break;
        case SlotState::Empty:
            border = accent;
            fill = QColor(accent.red(), accent.green(), accent.blue(), 18);
            if (firstEmpty && firstEmpty->id == slot.id)
                text = QCoreApplication::translate("PhotoPrint", "Húzd ide a fotót");
            break;
        case SlotState::Loading:
            border = QColor(0x9a, 0x9a, 0xa0);
            fill = QColor(0x9a, 0x9a, 0xa0, 14);
            break;
        case SlotState::Missing:
            border = QColor(0xb3, 0x26, 0x1e);
            fill = QColor(0xb3, 0x26, 0x1e, 14);
            text = QCoreApplication::translate("PhotoPrint", "Ezt a fotót nem találjuk.");
            break;
        }

        if (state != SlotState::Filled) {
            QPen pen(border, hovered ? 2.0 : 1.2, Qt::DashLine);
            pen.setDashPattern({5.0, 4.0});
            painter.setPen(pen);
            painter.setBrush(fill);
            painter.drawRoundedRect(ring, 3.0, 3.0);
            if (!text.isEmpty() && ring.width() > 54.0 && ring.height() > 22.0) {
                QFont font = baseFont;
                font.setPointSizeF(std::max(7.0, baseFont.pointSizeF() * 0.85));
                painter.setFont(font);
                painter.setPen(state == SlotState::Missing ? border.darker(115) : border);
                const QString elided =
                    painter.fontMetrics().elidedText(text, Qt::ElideRight,
                                                     qRound(ring.width() - 10.0));
                painter.drawText(ring.adjusted(4, 2, -4, -2), Qt::AlignCenter, elided);
            }
        }

        if (selected) {
            // A selected cell reads as one object even when the cell is filled:
            // the photograph's outline and this ring are the same rectangle.
            QPen pen(accent, 2.0, Qt::SolidLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(ring, 3.0, 3.0);
        }

        if (hovered) {
            // A filled cell is replaced, so its hover cue is not the accent drop
            // target: a distinct colour and a `Csere` badge say what the release
            // will do before the photo disappears.
            const bool occupied = SlotGeometry::frameInSlot(page, slot.id) != nullptr;
            const QColor cue = occupied ? QColor(0xc4, 0x55, 0x00) : accent;
            QPen pen(cue, 2.4, Qt::SolidLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(cell.adjusted(1.5, 1.5, -1.5, -1.5), 4.0, 4.0);
            if (occupied) {
                const QString label = QCoreApplication::translate("PhotoPrint", "Csere");
                QFont font = baseFont;
                font.setBold(true);
                font.setPointSizeF(std::max(7.0, baseFont.pointSizeF() * 0.9));
                const QFontMetrics metrics(font);
                const int textWidth = metrics.horizontalAdvance(label) + 14;
                const int textHeight = metrics.height() + 6;
                if (cell.width() >= textWidth + 8 && cell.height() >= textHeight + 8) {
                    const QRectF badge(cell.center().x() - textWidth / 2.0, cell.top() + 6.0,
                                       textWidth, textHeight);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(cue);
                    painter.drawRoundedRect(badge, 4.0, 4.0);
                    painter.setFont(font);
                    painter.setPen(Qt::white);
                    painter.drawText(badge, Qt::AlignCenter, label);
                }
            }
        }
    }
}

void PageView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The page rect changed size: draw the raster we have, scaled, and rebuild
    // it at the new scale once the resizing stops.
    m_composeFailed = false;
    positionBusyOverlay();
    scheduleSettle();
    update();
}

void PageView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // The view may have been resized or moved to another screen while hidden.
    scheduleSettle();
}

void PageView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // No mouse-up will arrive for a gesture that was running when we went away.
    flushWheelZoom();
    cancelInteraction(false);
}

void PageView::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    // A drag whose release went to another window must not keep moving frames.
    // A wheel burst that lost focus still made its change: keep it, as one edit.
    flushWheelZoom();
    cancelInteraction(true);
}

bool PageView::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
        // The raster is in the device pixels of the old screen: keep drawing it
        // scaled and rebuild once the window has landed on its new screen.
        m_composeFailed = false;
        scheduleSettle();
        update();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void PageView::changeEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::ActivationChange:
    case QEvent::WindowDeactivate:
    case QEvent::ApplicationDeactivate:
        // Alt-Tab in the middle of a drag: the mouse-up will never arrive, and
        // no gesture can be running while the window is being activated.
        if (dragActive() || m_panning) {
            cancelInteraction(true);
            update();
        }
        break;
    default:
        break;
    }
    QWidget::changeEvent(event);
}

void PageView::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panStart = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    // A new gesture ends a wheel burst: its pending commit lands as one edit
    // before the drag starts, so the undo stack keeps one step per gesture.
    flushWheelZoom();
    const QPointF pagePoint = widgetToPage(event->position());

    if (m_cropMode) {
        const Frame *frame = selectedFramePtr();
        if (frame && frameQuadWidget(*frame).containsPoint(event->position(), Qt::OddEvenFill)) {
            beginDrag(DragKind::CropPan, -1, pagePoint, *frame);
            event->accept();
            return;
        }
        m_cropMode = false;
        if (onCropModeExited)
            onCropModeExited();
        update();
        return;
    }

    const int handle = hitHandle(event->position());
    if (handle >= 0) {
        const Frame *frame = selectedFramePtr();
        if (frame) {
            beginDrag(handle == 4 ? DragKind::Rotate : DragKind::Resize, handle, pagePoint, *frame);
            event->accept();
            return;
        }
    }

    const Page *page = currentPage();
    const int index = hitFrame(event->position());
    if (index >= 0 && page) {
        const Frame frame = page->frames.at(index);
        setSelectedFrame(frame.id);
        setSelectedSlot(frame.slotId);
        if (onSelectionChanged)
            onSelectionChanged(frame.id);
        // A slotted photograph cannot move: a drag inside it pans the photo in
        // its cell, exactly like crop mode. A free frame keeps the move gesture.
        beginDrag(frame.slotId.isEmpty() ? DragKind::Move : DragKind::CropPan, -1, pagePoint,
                  frame);
        event->accept();
        return;
    }

    // The whole cell is a hit, so clicking a photograph or an empty cell
    // selects the cell rather than falling through to the paper.
    const int slotIndex = hitSlot(event->position());
    if (slotIndex >= 0 && page) {
        const Slot &slot = page->slotList.at(slotIndex);
        if (const Frame *occupant = SlotGeometry::frameInSlot(*page, slot.id)) {
            const Frame frame = *occupant;
            setSelectedFrame(frame.id);
            setSelectedSlot(slot.id);
            if (onSelectionChanged)
                onSelectionChanged(frame.id);
            beginDrag(DragKind::CropPan, -1, pagePoint, frame);
        } else {
            m_selectedId.clear();
            m_selectedSlotId = slot.id;
            setSelectedFrame(QString());
            setSelectedSlot(slot.id);
            update();
            if (onSelectionChanged)
                onSelectionChanged(QString());
            if (onSlotSelectionChanged)
                onSlotSelectionChanged(slot.id);
        }
        event->accept();
        return;
    }

    clearSelection();
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        m_viewPan += event->position() - m_panStart;
        m_panStart = event->position();
        // Panning moves the page, it does not change a single page pixel.
        update();
        return;
    }
    if (m_drag.kind == DragKind::None) {
        const Frame *frame = selectedFramePtr();
        if (frame && m_cropMode
            && frameQuadWidget(*frame).containsPoint(event->position(), Qt::OddEvenFill)) {
            setCursor(Qt::OpenHandCursor);
            return;
        }
        // A photograph in a cell is panned, not moved: the open hand says so.
        const Page *page = currentPage();
        const int index = hitFrame(event->position());
        if (index >= 0 && page && !page->frames.at(index).slotId.isEmpty()) {
            setCursor(m_panning ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
            return;
        }
        setCursor(Qt::ArrowCursor);
        return;
    }
    const QPointF pagePoint = widgetToPage(event->position());
    const bool fine = event->modifiers().testFlag(Qt::ShiftModifier)
        || event->modifiers().testFlag(Qt::AltModifier);
    const Frame frame = draggedFrame(pagePoint, fine);
    if (LayoutEngine::validFrame(frame))
        updateLiveFrame(frame);
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = false;
        setCursor(Qt::ArrowCursor);
        // The pan itself changed no pixel, but a zoom or an edit that happened
        // before or during it may still be waiting for its sharpening pass.
        scheduleSettle();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool changed = m_drag.changed;
    m_drag = DragState{};
    setCursor(Qt::ArrowCursor);
    if (changed) {
        const Frame *frame = selectedFramePtr();
        if (frame && onFrameCommitted)
            onFrameCommitted(*frame);
    }
    // The gesture could only afford a coarse raster: schedule the full-scale
    // rebuild instead of composing inside the event that ends the drag.
    scheduleSettle();
}

void PageView::wheelEvent(QWheelEvent *event)
{
    const QPointF position = event->position();
    const bool pageZoomRequested = event->modifiers().testFlag(Qt::ControlModifier);
    const double factor = std::pow(1.0015, event->angleDelta().y());

    // Which photograph, if any, is under the wheel? Crop mode keeps acting on
    // the selected frame; otherwise the photo under the cursor wins, which is
    // what "wheel over a photo" means. The frame is copied out of the document
    // first: flushWheelZoom() commits synchronously and the window may publish a
    // new document in response, so no pointer into m_document may outlive it.
    Frame target;
    bool hasTarget = false;
    const Page *page = currentPage();
    if (m_cropMode) {
        if (const Frame *selected = selectedFramePtr()) {
            target = *selected;
            hasTarget = true;
        }
    } else if (page) {
        const int index = hitFrame(position);
        if (index >= 0) {
            target = page->frames.at(index);
            hasTarget = true;
        }
    }
    const QString targetId = hasTarget ? target.id : QString();

    if (!pageZoomRequested && hasTarget && !target.slotId.isEmpty()) {
        // The page view does not move: the photograph zooms inside its cell and
        // the existing clip keeps everything outside the cell invisible. Shift
        // turns the photograph instead, in small steps, without touching the
        // cell rectangle.
        const QSize pixels =
            m_images.contains(targetId) ? m_images.value(targetId).size() : QSize();
        // Both edits need the image's pixel size: the crop bounds and the Fill
        // cover minimum are meaningless without it. With no pixels the event
        // falls through instead of being swallowed for nothing.
        const bool rotate = !pixels.isEmpty()
            && event->modifiers().testFlag(Qt::ShiftModifier)
            && event->angleDelta().y() != 0;
        Frame updated = target;
        bool handled = true;
        if (rotate) {
            const double notches = event->angleDelta().y() / 120.0;
            updated = PhotoItem::rotateImageBy(target,
                                               kImageRotationWheelStepDeg * notches, pixels);
        } else if (!pixels.isEmpty()) {
            updated = PhotoItem::zoomAt(target, widgetToPage(position), factor, pixels);
        } else {
            handled = false;
        }
        if (handled) {
            updated = PhotoItem::clampCrop(updated, pixels);
            if (updated != target) {
                if (m_zoomFrameId != targetId) {
                    const Frame start = target;
                    flushWheelZoom();
                    m_zoomStartFrame = start;
                }
                m_zoomFrameId = targetId;
                m_zoomCurrentFrame = updated;
                // A crop zoom is a gesture: compose the fitted page per notch so
                // the picture follows the wheel, and sharpen when it stops.
                m_cropZoomPulse.start();
                scheduleSettle();
                updateLiveFrame(updated, false);
                m_zoomTimer->start();
            }
            event->accept();
            return;
        }
    }

    if (m_cropMode && hasTarget && !pageZoomRequested) {
        // Crop mode on a free frame: unchanged from the shipped behaviour, but
        // the burst now commits once instead of once per notch.
        const QSize pixels =
            m_images.contains(targetId) ? m_images.value(targetId).size() : QSize();
        const QPointF pagePoint = widgetToPage(position);
        Frame updated = PhotoItem::zoomAt(target, pagePoint, factor, pixels);
        if (PhotoItem::clampCrop(updated, pixels).cropCenter != target.cropCenter
            || updated.zoom != target.zoom) {
            if (m_zoomFrameId != targetId) {
                const Frame start = target;
                flushWheelZoom();
                m_zoomStartFrame = start;
            }
            m_zoomFrameId = targetId;
            m_zoomCurrentFrame = updated;
            m_cropZoomPulse.start();
            scheduleSettle();
            updateLiveFrame(updated, false);
            m_zoomTimer->start();
        }
        event->accept();
        return;
    }

    setViewZoom(m_viewZoom * factor);
    if (onViewZoomChanged)
        onViewZoomChanged(m_viewZoom);
    event->accept();
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    const Frame *frame = selectedFramePtr();

    // Delete and Escape act on a selected cell even when the cell is empty, so
    // a layout can be inspected without a photograph being selected.
    switch (event->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (frame && onDeleteRequested) {
            onDeleteRequested(frame->id);
            event->accept();
            return;
        }
        if (!m_selectedSlotId.isEmpty() && onEmptySlotRequested) {
            onEmptySlotRequested(m_selectedSlotId);
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
        return;
    case Qt::Key_Escape:
        if (m_cropMode) {
            m_cropMode = false;
            if (onCropModeExited)
                onCropModeExited();
            update();
        } else {
            clearSelection();
        }
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_cropMode) {
            m_cropMode = false;
            if (onCropModeExited)
                onCropModeExited();
            update();
        }
        event->accept();
        return;
    default:
        break;
    }

    if (!frame) {
        QWidget::keyPressEvent(event);
        return;
    }
    const bool fine = event->modifiers().testFlag(Qt::ShiftModifier);
    const double step = fine ? 0.1 : 1.0;
    Frame moved = *frame;

    if (!frame->slotId.isEmpty()) {
        // A slotted frame cannot move. The arrow keys pan the photo inside its
        // cell, through the same clamped crop the pointer gesture uses.
        const QSize pixels = m_images.contains(frame->id) ? m_images.value(frame->id).size()
                                                          : QSize();
        const QSizeF image = LayoutEngine::imageSizeMm(*frame, pixels);
        if (pixels.isEmpty() || image.width() <= 0.0 || image.height() <= 0.0) {
            QWidget::keyPressEvent(event);
            return;
        }
        bool panned = true;
        switch (event->key()) {
        case Qt::Key_Left:
            moved.cropCenter.rx() += step / image.width();
            break;
        case Qt::Key_Right:
            moved.cropCenter.rx() -= step / image.width();
            break;
        case Qt::Key_Up:
            moved.cropCenter.ry() += step / image.height();
            break;
        case Qt::Key_Down:
            moved.cropCenter.ry() -= step / image.height();
            break;
        default:
            panned = false;
            break;
        }
        if (!panned) {
            QWidget::keyPressEvent(event);
            return;
        }
        moved = PhotoItem::clampCrop(moved, pixels);
        if (moved.cropCenter == frame->cropCenter) {
            event->accept();
            return;
        }
        m_nudgePulse.start();
        scheduleSettle();
        updateLiveFrame(moved);
        if (onFrameCommitted)
            onFrameCommitted(moved);
        event->accept();
        return;
    }

    bool nudged = true;
    switch (event->key()) {
    case Qt::Key_Left:
        moved.cx -= step;
        break;
    case Qt::Key_Right:
        moved.cx += step;
        break;
    case Qt::Key_Up:
        moved.cy -= step;
        break;
    case Qt::Key_Down:
        moved.cy += step;
        break;
    default:
        nudged = false;
        break;
    }
    if (nudged) {
        // Repeated arrow keys are a burst: the outline moves live and the
        // photo is composed once when the keys stop.
        m_nudgePulse.start();
        scheduleSettle();
        updateLiveFrame(moved);
        if (onFrameCommitted)
            onFrameCommitted(moved);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PageView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    if (m_cropMode) {
        m_cropMode = false;
        if (onCropModeExited)
            onCropModeExited();
        update();
        return;
    }
    const Page *page = currentPage();
    if (!page) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const int index = hitFrame(event->position());
    const int slotIndex = index >= 0 ? -1 : hitSlot(event->position());
    const Frame *target = nullptr;
    Frame copy;
    if (index >= 0) {
        copy = page->frames.at(index);
        target = &copy;
    } else if (slotIndex >= 0) {
        target = SlotGeometry::frameInSlot(*page, page->slotList.at(slotIndex).id);
    }
    if (target) {
        setSelectedFrame(target->id);
        setSelectedSlot(target->slotId);
        if (onSelectionChanged)
            onSelectionChanged(target->id);
        m_cropMode = true;
        if (onCropModeChanged)
            onCropModeChanged(true);
        update();
    }
}

void PageView::dragEnterEvent(QDragEnterEvent *event)
{
    // A drop is a new gesture: a wheel burst that is still waiting for its
    // commit lands first, so the drop cannot be overwritten by it.
    flushWheelZoom();
    const bool frames = event->mimeData()->hasFormat(QString::fromLatin1(kFrameMimeType));
    if (event->mimeData()->hasUrls() || frames) {
        m_accessibleBeforeDrag = accessibleDescription();
        m_dropSlotId = slotAtWidget(event->position());
        updateDropCue();
        update();
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void PageView::dragMoveEvent(QDragMoveEvent *event)
{
    m_dropSlotId = slotAtWidget(event->position());
    updateDropCue();
    update();
    event->acceptProposedAction();
}

void PageView::dragLeaveEvent(QDragLeaveEvent *event)
{
    Q_UNUSED(event)
    m_dropSlotId.clear();
    updateDropCue();
    update();
}

void PageView::dropEvent(QDropEvent *event)
{
    flushWheelZoom();
    const QString slotId = slotAtWidget(event->position());
    const QPointF pagePoint = widgetToPage(event->position());
    m_dropSlotId.clear();
    updateDropCue();
    update();

    const QString frameFormat = QString::fromLatin1(kFrameMimeType);
    if (event->mimeData()->hasFormat(frameFormat)) {
        const QString frameId = QString::fromUtf8(event->mimeData()->data(frameFormat));
        if (!frameId.isEmpty() && onFrameDroppedAt)
            onFrameDroppedAt(frameId, slotId, pagePoint);
        event->acceptProposedAction();
        return;
    }

    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    if (!paths.isEmpty() && onFilesDroppedAt)
        onFilesDroppedAt(paths, slotId);
    event->acceptProposedAction();
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), m_renderer(m_store)
{
    buildUi();
    m_previewWatcher = new QFutureWatcher<PreviewResult>(this);
    connect(m_previewWatcher, &QFutureWatcher<PreviewResult>::finished, this,
            &MainWindow::handlePreviewFinished);
    m_originalWatcher = new QFutureWatcher<OriginalResult>(this);
    connect(m_originalWatcher, &QFutureWatcher<OriginalResult>::finished, this,
            &MainWindow::handleOriginalPreviewFinished);
    applyTheme();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) {
                if (!qEnvironmentVariableIsSet("PHOTOPRINT_THEME"))
                    applyTheme();
            });
    buildMenus();
    statusBar()->show(); // reserve the error line before the first error
    connect(&m_controller, &CanvasController::documentChanged, this,
            &MainWindow::onDocumentChanged);

    JobConfig jobConfig;
    jobConfig.workDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("jobs"));
    jobConfig.journalPath = JobJournal::defaultPath();
    m_jobController = std::make_unique<JobController>(m_printStore, jobConfig, &m_cups);

    restoreSettings();
    m_recoveryEnabled = !qEnvironmentVariableIsSet("PHOTOPRINT_NO_RECOVERY");
    m_checkpointTimer = new QTimer(this);
    m_checkpointTimer->setSingleShot(true);
    m_checkpointTimer->setInterval(2000);
    connect(m_checkpointTimer, &QTimer::timeout, this, &MainWindow::checkpointNow);

    DocumentSnapshot document;
    Page page = emptyPage(currentPaper());
    page.landscape = m_landscape;
    document.pages.append(page);
    m_controller.setDocument(document);
    m_dirty = false;
    updateTitle();
    refreshPrinters();
    const int pendingPrints = JobJournal(jobConfig.journalPath).uncertainCount();
    if (pendingPrints < 0) {
        // The journal exists but cannot be read: do not claim there is nothing
        // to check, because a print may be waiting for an answer.
        statusBar()->showMessage(
            QCoreApplication::translate("PhotoPrint", "A nyomtatási napló nem olvasható.")
                + QLatin1Char(' ')
                + QCoreApplication::translate(
                    "PhotoPrint", "Korábbi nyomtatás eredménye bizonytalan. Nézd meg a nyomtatót."),
            0);
    } else if (pendingPrints > 0) {
        statusBar()->showMessage(
            QCoreApplication::translate("PhotoPrint", "Korábbi nyomtatás eredménye bizonytalan. Nézd meg a nyomtatót."),
            0);
    }

    if (m_recoveryEnabled) {
        const auto latest = m_recovery.loadLatest();
        if (latest && !latest.value().document.pages.isEmpty()) {
            const auto answer = QMessageBox::question(
                this, QStringLiteral("PhotoPrint"),
                QCoreApplication::translate("PhotoPrint", "Korábbi, nem mentett munka található. Folytatod?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (answer == QMessageBox::Yes) {
                applyLoadedProject(latest.value(), true);
            } else if (const auto cleared = m_recovery.clear(); !cleared) {
                showProblem(cleared.error().message);
            }
        }
    }
}

const PrinterCapabilities *MainWindow::printerByQueue(const QString &queue) const
{
    for (const PrinterCapabilities &capabilities : m_printers) {
        if (capabilities.queueName == queue)
            return &capabilities;
    }
    return nullptr;
}

MainWindow::~MainWindow()
{
    // A running CUPS call must not outlive the window that owns its watcher.
    if (m_printWatcher)
        m_printWatcher->waitForFinished();
    // The preview worker holds a copy of the document and a pointer to the
    // image store. Stop it and wait so the store is still alive when it exits.
    if (m_previewStop)
        m_previewStop->request_stop();
    if (m_previewWatcher && m_previewWatcher->isRunning())
        m_previewWatcher->waitForFinished();
    if (m_originalStop)
        m_originalStop->request_stop();
    if (m_originalWatcher && m_originalWatcher->isRunning())
        m_originalWatcher->waitForFinished();
    saveSettings();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("PhotoPrint"));
    resize(1280, 800);

    auto *toolbar = addToolBar(QCoreApplication::translate("PhotoPrint", "Fő"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    toolbar->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Nyomtató:"), this));
    m_printerCombo = new QComboBox(this);
    m_printerCombo->setMinimumContentsLength(22);
    m_printerCombo->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Nyomtató"));
    toolbar->addWidget(m_printerCombo);
    connect(m_printerCombo, &QComboBox::currentIndexChanged, this, &MainWindow::printerChanged);

    toolbar->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Papír:"), this));
    m_paperCombo = new QComboBox(this);
    toolbar->addWidget(m_paperCombo);
    m_customPaperButton = new QPushButton(QStringLiteral("+"), this);
    m_customPaperButton->setFixedWidth(28);
    m_customPaperButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Egyedi papírméret hozzáadása vagy törlése."));
    toolbar->addWidget(m_customPaperButton);
    connect(m_customPaperButton, &QPushButton::clicked, this, &MainWindow::manageCustomPapers);
    rebuildPaperCombo(m_paperName);
    connect(m_paperCombo, &QComboBox::currentIndexChanged, this, &MainWindow::paperChanged);

    toolbar->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Tájolás:"), this));
    m_orientationCombo = new QComboBox(this);
    m_orientationCombo->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Tájolás"));
    m_orientationCombo->addItem(QCoreApplication::translate("PhotoPrint", "Álló"));
    m_orientationCombo->addItem(QCoreApplication::translate("PhotoPrint", "Fekvő"));
    toolbar->addWidget(m_orientationCombo);
    connect(m_orientationCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::orientationChanged);

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Nézet:"), this));
    m_zoomCombo = new QComboBox(this);
    m_zoomCombo->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Nézet nagyítása"));
    m_zoomCombo->addItem(QStringLiteral("50%"), 0.5);
    m_zoomCombo->addItem(QStringLiteral("100%"), 1.0);
    m_zoomCombo->addItem(QStringLiteral("200%"), 2.0);
    m_zoomCombo->setCurrentIndex(1);
    toolbar->addWidget(m_zoomCombo);
    connect(m_zoomCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!m_pageView)
            return;
        m_pageView->setViewZoom(m_zoomCombo->itemData(index).toDouble());
    });

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *side = new QWidget(central);
    side->setObjectName(QStringLiteral("inspector"));
    side->setMinimumWidth(240);
    // Wide enough for the longest label at a larger system font; the width the
    // column actually gets is settled below from its own content.
    side->setMaximumWidth(560);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(12, 12, 12, 12);
    // A section header for one of the side column's two containers: the page's
    // own inspector and the selected-photograph inspector. They are separate
    // widgets so the layout panel can stay on screen with nothing selected.
    const auto addSectionTo = [side](QVBoxLayout *into, const QString &text) {
        auto *label = new QLabel(text, side);
        label->setObjectName(QStringLiteral("sectionHeader"));
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        label->setStyleSheet(QStringLiteral("margin-top: 8px;"));
        into->addWidget(label);
    };

    auto *library = new QWidget(central);
    library->setObjectName(QStringLiteral("library"));
    library->setMinimumWidth(210);
    library->setMaximumWidth(250);
    auto *libraryLayout = new QVBoxLayout(library);
    libraryLayout->setContentsMargins(12, 12, 12, 12);

    auto *libraryTitle = new QLabel(QCoreApplication::translate("PhotoPrint", "Fotók"), library);
    libraryTitle->setObjectName(QStringLiteral("sectionHeader"));
    libraryLayout->addWidget(libraryTitle);

    m_addButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Fotók hozzáadása"), library);
    m_addButton->setObjectName(QStringLiteral("addButton"));
    m_addButton->setMinimumHeight(36);
    m_addButton->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Fotók hozzáadása"));
    libraryLayout->addWidget(m_addButton);
    connect(m_addButton, &QPushButton::clicked, this, &MainWindow::addPhotos);

    // Templates sit next to the photo library, because a layout is chosen for
    // the page the photographs are already on.
    auto *templatesButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Sablonok"), library);
    templatesButton->setObjectName(QStringLiteral("templatesButton"));
    templatesButton->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Sablonok"));
    templatesButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Sablonok ehhez a laphoz"));
    templatesButton->setMinimumHeight(30);
    libraryLayout->addWidget(templatesButton);
    connect(templatesButton, &QPushButton::clicked, this, &MainWindow::openTemplateBrowser);

    m_photoList = new PhotoListWidget(library);
    m_photoList->setViewMode(QListView::IconMode);
    m_photoList->setResizeMode(QListView::Adjust);
    m_photoList->setMovement(QListView::Static);
    m_photoList->setGridSize(QSize(72, 72));
    m_photoList->setIconSize(QSize(64, 64));
    m_photoList->setSpacing(4);
    m_photoList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_photoList->setWordWrap(false);
    m_photoList->setUniformItemSizes(true);
    m_photoList->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Fotók"));
    libraryLayout->addWidget(m_photoList, 1);
    connect(m_photoList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
                if (m_syncing || !current)
                    return;
                selectFrame(current->data(Qt::UserRole).toString(), true);
            });

    // --- Elrendezés: the page's own inspector -------------------------------
    // A laid-out page is dressed as a whole, so these controls belong to the
    // page and not to one photograph. The block is visible whenever the current
    // page has slots, even with nothing selected, and it never hides the
    // selected-photograph sections below it.
    auto *layoutPanel = new QWidget(side);
    layoutPanel->setObjectName(QStringLiteral("layoutPanel"));
    auto *layoutLayout = new QVBoxLayout(layoutPanel);
    layoutLayout->setContentsMargins(0, 0, 0, 0);
    m_layoutPanel = layoutPanel;
    addSectionTo(layoutLayout, QCoreApplication::translate("PhotoPrint", "Elrendezés"));

    // The layout's name and how many cells it has. The name is the catalogue's
    // English suggestion, so it reads the same in every language, exactly as the
    // picker names its tiles.
    m_layoutNameLabel = new QLabel(layoutPanel);
    m_layoutNameLabel->setObjectName(QStringLiteral("layoutNameLabel"));
    m_layoutNameLabel->setWordWrap(true);
    QFont layoutNameFont = m_layoutNameLabel->font();
    layoutNameFont.setWeight(QFont::DemiBold);
    m_layoutNameLabel->setFont(layoutNameFont);
    layoutLayout->addWidget(m_layoutNameLabel);
    m_layoutCountLabel = new QLabel(layoutPanel);
    m_layoutCountLabel->setObjectName(QStringLiteral("layoutCountLabel"));
    markSecondaryText(m_layoutCountLabel);
    layoutLayout->addWidget(m_layoutCountLabel);

    // A length in millimetres. The spin gets the full width of the column, which
    // is what a "10.0 mm" value needs next to its own arrows. The two automatic
    // fields share the same shape: a named group title, an "Automatikus" switch
    // and the value below it. The title names the field, so the switch never has
    // to repeat it.
    const auto addTitle = [side, layoutLayout](const QString &text, const QString &name) {
        auto *title = new QLabel(text, side);
        title->setObjectName(name);
        title->setWordWrap(true);
        layoutLayout->addWidget(title);
    };
    const auto addLength = [side, layoutLayout](const QString &spinName,
                                                const QString &accessibleName, double maximum,
                                                double step) {
        auto *spin = new QDoubleSpinBox(side);
        spin->setObjectName(spinName);
        spin->setAccessibleName(accessibleName);
        spin->setDecimals(1);
        spin->setRange(0.0, maximum);
        spin->setSingleStep(step);
        spin->setSuffix(QStringLiteral(" mm"));
        spin->setMinimumWidth(96);
        layoutLayout->addWidget(spin);
        return spin;
    };
    const auto addAutomatic = [side, layoutLayout](const QString &text, const QString &name) {
        auto *check = new QCheckBox(text, side);
        check->setObjectName(name);
        layoutLayout->addWidget(check);
        return check;
    };

    addTitle(QCoreApplication::translate("PhotoPrint", "Külső margó"),
             QStringLiteral("marginTitle"));
    m_marginAutoCheck = addAutomatic(
        QCoreApplication::translate("PhotoPrint", "Automatikus"),
        QStringLiteral("marginAutoCheck"));
    m_marginSpin = addLength(QStringLiteral("marginSpin"),
                             QCoreApplication::translate("PhotoPrint", "Külső margó"),
                             SlotGeometry::kMaxMarginMm, 0.5);

    // The gap is the border: the white paper between two photographs. It has
    // no width and no colour of its own, only the distance.
    addTitle(QCoreApplication::translate("PhotoPrint", "Szegély"),
             QStringLiteral("gutterTitle"));
    m_gutterAutoCheck = addAutomatic(
        QCoreApplication::translate("PhotoPrint", "Automatikus"),
        QStringLiteral("gutterAutoCheck"));
    m_gutterSpin = addLength(QStringLiteral("gutterSpin"),
                             QCoreApplication::translate("PhotoPrint", "Szegély"),
                             SlotGeometry::kMaxGutterMm, 0.5);
    m_gutterHint = new QLabel(
        QCoreApplication::translate(
            "PhotoPrint",
            "Automatikus esetén a szegély a margót követi. A 0 mm-es szegélynél "
            "a fotók összeérnek."),
        side);
    m_gutterHint->setObjectName(QStringLiteral("gutterHint"));
    m_gutterHint->setWordWrap(true);
    markSecondaryText(m_gutterHint);
    layoutLayout->addWidget(m_gutterHint);

    addTitle(QCoreApplication::translate("PhotoPrint", "Sarok lekerekítése"),
             QStringLiteral("radiusTitle"));
    m_radiusSpin = addLength(QStringLiteral("radiusSpin"),
                             QCoreApplication::translate("PhotoPrint", "Sarok lekerekítése"),
                             SlotGeometry::kMaxCornerRadiusMm, 1.0);

    // The two cell actions sit on their own rows: at the narrowest side column a
    // pair of buttons would have to clip its own label.
    m_addSlotButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Hely hozzáadása"), side);
    m_addSlotButton->setObjectName(QStringLiteral("addSlotButton"));
    layoutLayout->addWidget(m_addSlotButton);
    connect(m_addSlotButton, &QPushButton::clicked, this, &MainWindow::addSlotToPage);

    m_removeSlotButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Hely törlése"), side);
    m_removeSlotButton->setObjectName(QStringLiteral("removeSlotButton"));
    m_removeSlotButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Válaszd ki a helyet a lapon."));
    layoutLayout->addWidget(m_removeSlotButton);
    connect(m_removeSlotButton, &QPushButton::clicked, this, &MainWindow::removeSlotFromPage);

    m_dissolveLayoutButton = new QPushButton(
        QCoreApplication::translate("PhotoPrint", "Elrendezés megszüntetése"), side);
    m_dissolveLayoutButton->setObjectName(QStringLiteral("dissolveLayoutButton"));
    m_dissolveLayoutButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "A fotók megmaradnak a helyükön."));
    layoutLayout->addWidget(m_dissolveLayoutButton);
    connect(m_dissolveLayoutButton, &QPushButton::clicked, this,
            &MainWindow::dissolvePageLayout);

    m_layoutTemplatesButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Sablonok…"), side);
    m_layoutTemplatesButton->setObjectName(QStringLiteral("layoutTemplatesButton"));
    m_layoutTemplatesButton->setAccessibleName(
        QCoreApplication::translate("PhotoPrint", "Sablonok"));
    m_layoutTemplatesButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Sablonok ehhez a laphoz"));
    layoutLayout->addWidget(m_layoutTemplatesButton);
    connect(m_layoutTemplatesButton, &QPushButton::clicked, this,
            &MainWindow::openTemplateBrowser);

    // One burst timer for the four spins: a value change restarts it, so the
    // page follows live and the whole burst lands as one undo step.
    if (!m_styleTimer) {
        m_styleTimer = new QTimer(this);
        m_styleTimer->setSingleShot(true);
        m_styleTimer->setInterval(kStyleDebounceMs);
        connect(m_styleTimer, &QTimer::timeout, this, &MainWindow::flushSlotStyle);
    }
    m_pendingStyleField.reset();
    const struct {
        QDoubleSpinBox *spin;
        SlotStyleField field;
    } styleSpins[] = {
        {m_marginSpin, SlotStyleField::Margin},
        {m_gutterSpin, SlotStyleField::Gutter},
        {m_radiusSpin, SlotStyleField::Radius},
    };
    for (const auto &entry : styleSpins) {
        connect(entry.spin, &QDoubleSpinBox::valueChanged, this,
                [this, field = entry.field](double) { queueSlotStyle(field); });
        connect(entry.spin, &QDoubleSpinBox::editingFinished, this,
                [this, field = entry.field] {
                    if (m_pendingStyleField && *m_pendingStyleField == field)
                        flushSlotStyle();
                });
    }
    connect(m_marginAutoCheck, &QCheckBox::toggled, this, [this](bool automatic) {
        // A switch is one action, not a burst: it lands at once, and its spin
        // follows it.
        m_marginSpin->setEnabled(!automatic);
        cancelPendingStyle();
        applySlotStyleField(SlotStyleField::Margin);
    });
    connect(m_gutterAutoCheck, &QCheckBox::toggled, this, [this](bool automatic) {
        m_gutterSpin->setEnabled(!automatic);
        cancelPendingStyle();
        applySlotStyleField(SlotStyleField::Gutter);
    });
    layoutPanel->setVisible(false);
    sideLayout->addWidget(layoutPanel);

    // --- Lap: tools that belong to the sheet, not to one photograph ---------
    // Always visible: a page with no layout and no selection can still be
    // tidied, and the cut guide state is a page property.
    auto *pageTools = new QWidget(side);
    pageTools->setObjectName(QStringLiteral("pageToolsPanel"));
    auto *pageToolsLayout = new QVBoxLayout(pageTools);
    pageToolsLayout->setContentsMargins(0, 0, 0, 0);
    addSectionTo(pageToolsLayout, QCoreApplication::translate("PhotoPrint", "Lap"));

    m_alignButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Fotók rendezése…"), side);
    m_alignButton->setObjectName(QStringLiteral("alignPhotosButton"));
    m_alignButton->setToolTip(QCoreApplication::translate(
        "PhotoPrint", "A lapon lévő fotók elrendezése. A sorrend nem változik."));
    pageToolsLayout->addWidget(m_alignButton);
    connect(m_alignButton, &QPushButton::clicked, this, &MainWindow::openAlignPhotos);

    auto *guideTitle = new QLabel(QCoreApplication::translate("PhotoPrint", "Vágójelek"), side);
    guideTitle->setWordWrap(true);
    pageToolsLayout->addWidget(guideTitle);
    m_guidesCombo = new QComboBox(pageTools);
    m_guidesCombo->setObjectName(QStringLiteral("cutGuidesCombo"));
    m_guidesCombo->setAccessibleName(guideTitle->text());
    m_guidesCombo->addItem(QCoreApplication::translate("PhotoPrint", "Nincs"),
                           int(CutGuides::None));
    m_guidesCombo->addItem(QCoreApplication::translate("PhotoPrint", "Képernyőn"),
                           int(CutGuides::Screen));
    m_guidesCombo->addItem(QCoreApplication::translate("PhotoPrint", "Nyomtatás"),
                           int(CutGuides::Print));
    pageToolsLayout->addWidget(m_guidesCombo);
    connect(m_guidesCombo, &QComboBox::currentIndexChanged, this, &MainWindow::guidesChanged);
    m_guidesHint = new QLabel(side);
    m_guidesHint->setObjectName(QStringLiteral("cutGuidesHint"));
    m_guidesHint->setWordWrap(true);
    markSecondaryText(m_guidesHint);
    pageToolsLayout->addWidget(m_guidesHint);

    auto *borderlessTitle =
        new QLabel(QCoreApplication::translate("PhotoPrint", "Szélig nyomtatás"), side);
    borderlessTitle->setWordWrap(true);
    pageToolsLayout->addWidget(borderlessTitle);
    m_borderlessCombo = new QComboBox(pageTools);
    m_borderlessCombo->setObjectName(QStringLiteral("borderlessCombo"));
    m_borderlessCombo->setAccessibleName(borderlessTitle->text());
    m_borderlessCombo->addItem(QCoreApplication::translate("PhotoPrint", "Automatikus"),
                               int(BorderlessOverride::Auto));
    m_borderlessCombo->addItem(QCoreApplication::translate("PhotoPrint", "Kikapcsolva"),
                               int(BorderlessOverride::Off));
    m_borderlessCombo->addItem(QCoreApplication::translate("PhotoPrint", "Be"),
                               int(BorderlessOverride::On));
    pageToolsLayout->addWidget(m_borderlessCombo);
    connect(m_borderlessCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::borderlessChanged);
    m_borderlessHint =
        new QLabel(QCoreApplication::translate(
                       "PhotoPrint",
                       "A példányok pontosan a választott méretben nyomtatódnak, nagyítás "
                       "nélkül."),
                   side);
    m_borderlessHint->setWordWrap(true);
    markSecondaryText(m_borderlessHint);
    pageToolsLayout->addWidget(m_borderlessHint);
    sideLayout->addWidget(pageTools);

    // --- Kiválasztott fotó and Effekt: the photograph's own inspector --------
    // One container, hidden as a whole when no photograph is selected, so a
    // laid-out page with an empty selection shows the layout panel alone.
    m_photoPanel = new QWidget(side);
    m_photoPanel->setObjectName(QStringLiteral("photoPanel"));
    auto *photoLayout = new QVBoxLayout(m_photoPanel);
    photoLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->addWidget(m_photoPanel);

    addSectionTo(photoLayout, QCoreApplication::translate("PhotoPrint", "Kiválasztott fotó"));
    auto *fitRow = new QHBoxLayout;
    m_fitButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Teljes kép"), side);
    m_fillButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Kép kitöltése"), side);
    fitRow->addWidget(m_fitButton);
    fitRow->addWidget(m_fillButton);
    photoLayout->addLayout(fitRow);
    connect(m_fitButton, &QPushButton::clicked, this, [this] { applyFit(FitMode::Full); });
    connect(m_fillButton, &QPushButton::clicked, this, [this] { applyFit(FitMode::Fill); });

    auto *actionRow = new QHBoxLayout;
    m_cropButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Kivágás"), side);
    m_cropButton->setCheckable(true);
    m_deleteButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Eltávolítás"), side);
    actionRow->addWidget(m_cropButton);
    actionRow->addWidget(m_deleteButton);
    photoLayout->addLayout(actionRow);
    connect(m_cropButton, &QPushButton::toggled, this, &MainWindow::toggleCrop);
    connect(m_deleteButton, &QPushButton::clicked, this, &MainWindow::deleteSelected);

    auto *layerRow = new QHBoxLayout;
    m_forwardButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Előtérbe"), side);
    m_backwardButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Háttérbe"), side);
    layerRow->addWidget(m_forwardButton);
    layerRow->addWidget(m_backwardButton);
    photoLayout->addLayout(layerRow);
    connect(m_forwardButton, &QPushButton::clicked, this, [this] { reorderFrame(1); });
    connect(m_backwardButton, &QPushButton::clicked, this, [this] { reorderFrame(-1); });

    auto *toneRow = new QHBoxLayout;
    m_autoCheck = new ToggleSwitch(side);
    m_autoCheck->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Automatikus javítás"));
    m_autoCheck->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Finoman igazítja a fényt és a színt a nyomtatáshoz."));
    auto *toneText = new QVBoxLayout;
    auto *toneTitle = new QLabel(QCoreApplication::translate("PhotoPrint", "Automatikus javítás"), side);
    QFont toneFont = toneTitle->font();
    toneFont.setWeight(QFont::DemiBold);
    toneTitle->setFont(toneFont);
    m_autoHint = new QLabel(QCoreApplication::translate("PhotoPrint", "Finom fény- és színjavítás"), side);
    markSecondaryText(m_autoHint);
    m_autoHint->setWordWrap(true);
    toneText->addWidget(toneTitle);
    toneText->addWidget(m_autoHint);
    m_originalButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Eredeti"), side);
    m_originalButton->setObjectName(QStringLiteral("originalButton"));
    m_originalButton->setToolTip(QCoreApplication::translate("PhotoPrint", "Tartsd nyomva az eredeti képhez."));
    toneRow->addWidget(m_autoCheck);
    toneRow->addLayout(toneText);
    toneRow->addStretch(1);
    toneRow->addWidget(m_originalButton);
    photoLayout->addLayout(toneRow);
    connect(m_autoCheck, &QAbstractButton::toggled, this, &MainWindow::toggleAutoTone);
    connect(m_originalButton, &QPushButton::pressed, this, [this] { showOriginalPressed(true); });
    connect(m_originalButton, &QPushButton::released, this, [this] { showOriginalPressed(false); });

    m_brightnessSlider = new QSlider(Qt::Horizontal, side);
    m_brightnessSlider->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Világosítás"));
    m_brightnessSlider->setRange(kBrightnessSliderMin, kBrightnessSliderMax);
    m_brightnessSlider->setValue(kBrightnessSliderNeutral);
    m_brightnessSlider->setTracking(false);
    photoLayout->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Világosítás"), side));
    photoLayout->addWidget(m_brightnessSlider);
    connect(m_brightnessSlider, &QSlider::valueChanged, this, &MainWindow::brightnessChanged);

    addSectionTo(photoLayout, QCoreApplication::translate("PhotoPrint", "Effekt"));
    m_effectButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Eredeti"), side);
    m_effectButton->setObjectName(QStringLiteral("effectButton"));
    m_effectButton->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Effekt"));
    m_effectButton->setToolTip(QCoreApplication::translate("PhotoPrint", "Effekt kiválasztása"));
    photoLayout->addWidget(m_effectButton);
    m_legacyHint = new QLabel(QCoreApplication::translate("PhotoPrint", "Új effekt választása"), side);
    m_legacyHint->setObjectName(QStringLiteral("legacyEffectHint"));
    m_legacyHint->setWordWrap(true);
    markSecondaryText(m_legacyHint);
    m_legacyHint->setVisible(false);
    photoLayout->addWidget(m_legacyHint);
    connect(m_effectButton, &QPushButton::clicked, this, &MainWindow::openEffectBrowser);

    m_strengthLabel = new QLabel(QCoreApplication::translate("PhotoPrint", "Effekt erőssége"), side);
    m_strengthLabel->setObjectName(QStringLiteral("effectStrengthLabel"));
    photoLayout->addWidget(m_strengthLabel);
    m_filterStrength = new QSlider(Qt::Horizontal, side);    m_filterStrength->setObjectName(QStringLiteral("effectStrength"));
    m_filterStrength->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Effekt erőssége"));
    m_filterStrength->setRange(0, 100);
    m_filterStrength->setValue(100);
    m_filterStrength->setTracking(false);
    photoLayout->addWidget(m_filterStrength);
    connect(m_filterStrength, &QSlider::valueChanged, this, &MainWindow::applyFilterSettings);

    m_strengthHint = new QLabel(side);
    m_strengthHint->setObjectName(QStringLiteral("effectStrengthHint"));
    m_strengthHint->setWordWrap(true);
    markSecondaryText(m_strengthHint);
    m_strengthHint->setVisible(false);
    photoLayout->addWidget(m_strengthHint);

    m_upscaleCheck = new QCheckBox(QCoreApplication::translate("PhotoPrint", "Kép nagyítása 2×"), side);
    m_upscaleCheck->setToolTip(
        QCoreApplication::translate("PhotoPrint", "A nyomtatási felbontást növeli; az előnézet nem változik."));
    photoLayout->addWidget(m_upscaleCheck);
    auto *upscaleHint = new QLabel(QCoreApplication::translate("PhotoPrint", "A nyomtatási felbontást növeli."), side);
    markSecondaryText(upscaleHint);
    upscaleHint->setWordWrap(true);
    photoLayout->addWidget(upscaleHint);
    connect(m_upscaleCheck, &QCheckBox::toggled, this, &MainWindow::applyFilterSettings);

    m_trialButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Próbanyomat"), side);
    photoLayout->addWidget(m_trialButton);
    connect(m_trialButton, &QPushButton::clicked, this, &MainWindow::showTrialSheet);

    // Many identical copies of the selected photograph. It sits under the test
    // sheet because both answer "how will this photo come out of the printer".
    m_batchButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Sokszorosítás…"), side);
    m_batchButton->setObjectName(QStringLiteral("batchCopiesButton"));
    m_batchButton->setToolTip(QCoreApplication::translate(
        "PhotoPrint", "Ugyanaz a fotó több példányban, egyenes rácsban."));
    photoLayout->addWidget(m_batchButton);
    connect(m_batchButton, &QPushButton::clicked, this, &MainWindow::openBatchCopies);

    m_advancedToggle = new QPushButton(QCoreApplication::translate("PhotoPrint", "További beállítások"), side);
    m_advancedToggle->setCheckable(true);
    photoLayout->addWidget(m_advancedToggle);
    auto *advancedContent = new QWidget(side);
    auto *grid = new QGridLayout(advancedContent);
    grid->setContentsMargins(0, 0, 0, 0);
    m_widthSpin = new QDoubleSpinBox(advancedContent);
    m_heightSpin = new QDoubleSpinBox(advancedContent);
    m_angleSpin = new QDoubleSpinBox(advancedContent);
    m_widthSpin->setObjectName(QStringLiteral("widthSpin"));
    m_heightSpin->setObjectName(QStringLiteral("heightSpin"));
    m_angleSpin->setObjectName(QStringLiteral("angleSpin"));
    for (QDoubleSpinBox *spin : {m_widthSpin, m_heightSpin}) {
        spin->setRange(0.5, 100.0);
        spin->setDecimals(2);
        spin->setSuffix(QStringLiteral(" cm"));
        spin->setSingleStep(0.1);
    }
    m_angleSpin->setRange(-180.0, 180.0);
    m_angleSpin->setDecimals(1);
    m_angleSpin->setSuffix(QStringLiteral("°"));
    grid->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Szélesség")), 0, 0);
    grid->addWidget(m_widthSpin, 0, 1);
    grid->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Magasság")), 1, 0);
    grid->addWidget(m_heightSpin, 1, 1);
    grid->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Szög")), 2, 0);
    grid->addWidget(m_angleSpin, 2, 1);
    advancedContent->setVisible(false);
    connect(m_advancedToggle, &QPushButton::toggled, advancedContent, &QWidget::setVisible);
    photoLayout->addWidget(advancedContent);
    for (QDoubleSpinBox *spin : {m_widthSpin, m_heightSpin, m_angleSpin})
        connect(spin, &QDoubleSpinBox::editingFinished, this, &MainWindow::applySizeFromControls);

    m_resolutionHint = new QLabel(side);
    m_resolutionHint->setWordWrap(true);
    m_resolutionHint->setStyleSheet(QStringLiteral("color: #8a5a00;"));
    photoLayout->addWidget(m_resolutionHint);

    // --- Kép a helyén: the photograph inside its cell ------------------------
    // A slot owns the rectangle, so the only edits left are the image's own:
    // its magnification and its rotation. The whole block hides for a free
    // frame. It sits after the existing photo controls on purpose, so turning
    // it on never pushes Effekt, the trial sheet or the batch entry down.
    m_slotImagePanel = new QWidget(side);
    m_slotImagePanel->setObjectName(QStringLiteral("slotImagePanel"));
    auto *slotImageLayout = new QVBoxLayout(m_slotImagePanel);
    slotImageLayout->setContentsMargins(0, 0, 0, 0);
    photoLayout->addWidget(m_slotImagePanel);
    {
        auto *label = new QLabel(QCoreApplication::translate("PhotoPrint", "Kép a helyén"),
                                 m_slotImagePanel);
        label->setObjectName(QStringLiteral("sectionHeader"));
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        label->setStyleSheet(QStringLiteral("margin-top: 8px;"));
        slotImageLayout->addWidget(label);
    }

    auto *slotGrid = new QGridLayout;
    slotGrid->setContentsMargins(0, 0, 0, 0);
    slotGrid->setColumnStretch(1, 1);

    auto *slotZoomLabel =
        new QLabel(QCoreApplication::translate("PhotoPrint", "Nagyítás"), m_slotImagePanel);
    m_slotZoomSlider = new QSlider(Qt::Horizontal, m_slotImagePanel);
    m_slotZoomSlider->setObjectName(QStringLiteral("slotZoomSlider"));
    m_slotZoomSlider->setAccessibleName(
        QCoreApplication::translate("PhotoPrint", "Nagyítás"));
    m_slotZoomSlider->setToolTip(QCoreApplication::translate(
        "PhotoPrint", "A kerék nagyít, a Ctrl+kerék a lapot nagyítja."));
    // The range is the model's own, so the thumb never lies after a wheel
    // gesture. A Fill cell can raise the effective zoom above the requested
    // step; the readout then shows the value the frame actually holds.
    m_slotZoomSlider->setRange(int(kMinFrameZoom * 100.0), int(kMaxFrameZoom * 100.0));
    m_slotZoomSlider->setSingleStep(5);
    m_slotZoomSlider->setPageStep(25);
    m_slotZoomSlider->setValue(100);
    // Tracking stays on: the canvas follows the thumb, and the release commits
    // the whole drag as one edit.
    m_slotZoomSlider->setTracking(true);
    slotZoomLabel->setBuddy(m_slotZoomSlider);
    m_slotZoomValue = new QLabel(QStringLiteral("100%"), m_slotImagePanel);
    m_slotZoomValue->setObjectName(QStringLiteral("slotZoomValue"));
    m_slotZoomValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    markSecondaryText(m_slotZoomValue);
    slotGrid->addWidget(slotZoomLabel, 0, 0);
    slotGrid->addWidget(m_slotZoomSlider, 0, 1);
    slotGrid->addWidget(m_slotZoomValue, 0, 2);
    connect(m_slotZoomSlider, &QSlider::valueChanged, this, &MainWindow::slotZoomChanged);
    connect(m_slotZoomSlider, &QSlider::sliderPressed, this,
            &MainWindow::slotImageEditStarted);
    connect(m_slotZoomSlider, &QSlider::sliderReleased, this,
            &MainWindow::slotImageEditFinished);

    auto *slotRotationLabel =
        new QLabel(QCoreApplication::translate("PhotoPrint", "Forgatás"), m_slotImagePanel);
    m_slotRotationSlider = new QSlider(Qt::Horizontal, m_slotImagePanel);
    m_slotRotationSlider->setObjectName(QStringLiteral("slotRotationSlider"));
    m_slotRotationSlider->setAccessibleName(
        QCoreApplication::translate("PhotoPrint", "Forgatás"));
    m_slotRotationSlider->setToolTip(
        QCoreApplication::translate("PhotoPrint", "A Shift+kerék forgatja a fotót."));
    m_slotRotationSlider->setRange(int(kMinImageRotationDeg), int(kMaxImageRotationDeg));
    m_slotRotationSlider->setSingleStep(1);
    m_slotRotationSlider->setPageStep(15);
    m_slotRotationSlider->setTracking(true);
    slotRotationLabel->setBuddy(m_slotRotationSlider);
    m_slotRotationValue = new QLabel(QStringLiteral("0°"), m_slotImagePanel);
    m_slotRotationValue->setObjectName(QStringLiteral("slotRotationValue"));
    m_slotRotationValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    markSecondaryText(m_slotRotationValue);
    slotGrid->addWidget(slotRotationLabel, 1, 0);
    slotGrid->addWidget(m_slotRotationSlider, 1, 1);
    slotGrid->addWidget(m_slotRotationValue, 1, 2);
    connect(m_slotRotationSlider, &QSlider::valueChanged, this,
            &MainWindow::slotRotationChanged);
    connect(m_slotRotationSlider, &QSlider::sliderPressed, this,
            &MainWindow::slotImageEditStarted);
    connect(m_slotRotationSlider, &QSlider::sliderReleased, this,
            &MainWindow::slotImageEditFinished);
    slotImageLayout->addLayout(slotGrid);

    auto *slotTurnRow = new QHBoxLayout;
    m_slotRotateLeftButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Balra 90°"),
                        m_slotImagePanel);
    m_slotRotateLeftButton->setObjectName(QStringLiteral("slotRotateLeft"));
    m_slotRotateRightButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Jobbra 90°"),
                        m_slotImagePanel);
    m_slotRotateRightButton->setObjectName(QStringLiteral("slotRotateRight"));
    slotTurnRow->addWidget(m_slotRotateLeftButton);
    slotTurnRow->addWidget(m_slotRotateRightButton);
    slotImageLayout->addLayout(slotTurnRow);
    connect(m_slotRotateLeftButton, &QPushButton::clicked, this,
            [this] { rotateSlotImageBy(-90.0); });
    connect(m_slotRotateRightButton, &QPushButton::clicked, this,
            [this] { rotateSlotImageBy(90.0); });

    m_slotResetButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Alaphelyzet"),
                        m_slotImagePanel);
    m_slotResetButton->setObjectName(QStringLiteral("slotImageReset"));
    m_slotResetButton->setToolTip(QCoreApplication::translate(
        "PhotoPrint", "Visszaállítja a kép nagyítását, helyét és forgatását."));
    slotImageLayout->addWidget(m_slotResetButton);
    connect(m_slotResetButton, &QPushButton::clicked, this, &MainWindow::resetSlotImage);

    auto *slotImageHint = new QLabel(
        QCoreApplication::translate(
            "PhotoPrint",
            "Húzd a fotót a mozgatáshoz. A kerék nagyít, a Shift+kerék forgat, a "
            "Ctrl+kerék a lapot nagyítja."),
        m_slotImagePanel);
    slotImageHint->setObjectName(QStringLiteral("slotImageHint"));
    slotImageHint->setWordWrap(true);
    markSecondaryText(slotImageHint);
    slotImageLayout->addWidget(slotImageHint);
    m_slotImagePanel->setVisible(false);

    m_pageInfo = new QLabel(side);
    m_pageInfo->setWordWrap(true);
    sideLayout->addWidget(m_pageInfo);

    m_noticeBar = new QWidget(central);
    auto *noticeLayout = new QHBoxLayout(m_noticeBar);
    noticeLayout->setContentsMargins(12, 2, 12, 2);
    m_printerStatus = new QLabel(m_noticeBar);
    m_printerStatus->setWordWrap(true);
    noticeLayout->addWidget(m_printerStatus);
    m_recheckButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Újraellenőrzés"), m_noticeBar);
    m_recheckButton->setVisible(false);
    noticeLayout->addWidget(m_recheckButton);
    connect(m_recheckButton, &QPushButton::clicked, this, &MainWindow::refreshPrinters);
    m_paperHint = new QLabel(m_noticeBar);
    m_paperHint->setWordWrap(true);
    noticeLayout->addWidget(m_paperHint);
    m_overflowLabel = new QLabel(QCoreApplication::translate("PhotoPrint", "A kép egy része lemarad."), m_noticeBar);
    m_overflowLabel->setWordWrap(true);
    m_overflowLabel->setVisible(false);
    noticeLayout->addWidget(m_overflowLabel);
    m_fitPageButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Igazítás a laphoz"), m_noticeBar);
    m_fitPageButton->setObjectName(QStringLiteral("fitPageButton"));
    m_fitPageButton->setVisible(false);
    noticeLayout->addWidget(m_fitPageButton);
    noticeLayout->addStretch(1);
    connect(m_fitPageButton, &QPushButton::clicked, this, &MainWindow::fitPage);
    m_noticeBar->setVisible(false);

    m_pageView = new PageView(central);
    m_pageView->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Papírlap"));
    m_pageView->setAccessibleDescription(
        QCoreApplication::translate("PhotoPrint", "Nagyításnál a középső egérgombbal mozgatható a lap."));
    m_pageView->onFilesDroppedAt = [this](const QStringList &paths, const QString &slotId) {
        addFilesAt(paths, slotId);
    };
    m_pageView->onFrameDroppedAt =
        [this](const QString &frameId, const QString &slotId, const QPointF &pagePoint) {
            dropFrameAt(frameId, slotId, pagePoint);
        };
    m_pageView->onCancelBusy = [this] { cancelPreviewWork(); };
    m_pageView->onSelectionChanged = [this](const QString &frameId) {
        selectFrame(frameId, false);
    };
    m_pageView->onSlotSelectionChanged = [this](const QString &slotId) {
        selectSlot(slotId);
    };
    m_pageView->onFrameCommitted = [this](const Frame &frame) {
        m_controller.apply(FrameChange{frame.id, frame});
    };
    m_pageView->onDeleteRequested = [this](const QString &frameId) {
        if (!frameId.isEmpty())
            m_controller.apply(RemoveFrameEdit{frameId});
    };
    m_pageView->onEmptySlotRequested = [this](const QString &slotId) {
        clearSlot(slotId);
    };
    m_pageView->onCropModeExited = [this] {
        if (m_cropButton->isChecked()) {
            m_cropButton->blockSignals(true);
            m_cropButton->setChecked(false);
            m_cropButton->blockSignals(false);
        }
        m_pageView->setCropMode(false);
        m_cropButton->setText(QCoreApplication::translate("PhotoPrint", "Kivágás"));
    };
    m_pageView->onCropModeChanged = [this](bool enabled) {
        if (m_cropButton->isChecked() != enabled) {
            m_cropButton->blockSignals(true);
            m_cropButton->setChecked(enabled);
            m_cropButton->blockSignals(false);
        }
        m_pageView->setCropMode(enabled);
        m_cropButton->setText(enabled ? QCoreApplication::translate("PhotoPrint", "Kész") : QCoreApplication::translate("PhotoPrint", "Kivágás"));
        if (enabled)
            m_pageView->setFocus();
    };
    m_pageView->onViewZoomChanged = [this](double zoom) {
        int best = -1;
        for (int i = 0; i < m_zoomCombo->count(); ++i) {
            if (std::abs(m_zoomCombo->itemData(i).toDouble() - zoom) < 0.01)
                best = i;
        }
        m_zoomCombo->blockSignals(true);
        if (best >= 0) {
            m_zoomCombo->setCurrentIndex(best);
        } else {
            m_zoomCombo->setEditable(true);
            m_zoomCombo->setCurrentIndex(-1);
            m_zoomCombo->setEditText(QStringLiteral("%1%").arg(qRound(zoom * 100.0)));
        }
        m_zoomCombo->blockSignals(false);
    };

    auto *sideScroll = new QScrollArea(central);
    sideScroll->setWidgetResizable(true);
    sideScroll->setFrameShape(QFrame::NoFrame);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setMinimumHeight(0);
    side->setMinimumHeight(0);
    side->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    sideScroll->setWidget(side);
    // The column is never narrower than its own content. At the default font
    // that is the old 240 px; at a larger system font the buttons and labels need
    // more, and a horizontal scroll bar is switched off, so without this they
    // would be clipped instead of laid out.
    sideLayout->activate();
    const int contentWidth = side->minimumSizeHint().width();
    sideScroll->setMinimumWidth(std::max(240, contentWidth));
    sideScroll->setMaximumWidth(std::max(330, contentWidth + 40));
    m_inspectorPanel = sideScroll;
    layout->addWidget(library);
    layout->addWidget(m_pageView, 1);
    layout->addWidget(sideScroll);

    auto *printBar = new QWidget(central);
    auto *printLayout = new QHBoxLayout(printBar);
    printLayout->setContentsMargins(12, 4, 12, 4);
    printLayout->addStretch(1);
    printLayout->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Minőség"), printBar));
    m_qualityCombo = new QComboBox(printBar);
    m_qualityCombo->addItem(QCoreApplication::translate("PhotoPrint", "Kiváló"), true);
    m_qualityCombo->addItem(QCoreApplication::translate("PhotoPrint", "Normál"), false);
    m_qualityCombo->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Nyomtatási minőség"));
    printLayout->addWidget(m_qualityCombo);
    connect(m_qualityCombo, &QComboBox::currentIndexChanged, this, &MainWindow::qualityChanged);

    printLayout->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Szín"), printBar));
    m_colorCombo = new QComboBox(printBar);
    m_colorCombo->addItem(QCoreApplication::translate("PhotoPrint", "Színes"), false);
    m_colorCombo->addItem(QCoreApplication::translate("PhotoPrint", "Szürkeárnyalatos"), true);
    m_colorCombo->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Színek"));
    printLayout->addWidget(m_colorCombo);
    connect(m_colorCombo, &QComboBox::currentIndexChanged, this, &MainWindow::colorChanged);

    printLayout->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Példány"), printBar));
    m_copiesSpin = new QSpinBox(printBar);
    m_copiesSpin->setRange(1, 99);
    m_copiesSpin->setValue(1);
    m_copiesSpin->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Példányszám"));
    printLayout->addWidget(m_copiesSpin);
    m_printButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Nyomtatás"), printBar);
    m_printButton->setMinimumHeight(36);
    m_printButton->setMinimumWidth(180);
    m_printButton->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Nyomtatás"));
    m_printButton->setObjectName(QStringLiteral("printButton"));
    printLayout->addWidget(m_printButton);
    connect(m_printButton, &QPushButton::clicked, this, &MainWindow::printNow);

    m_pagesPanel = new PagesPanel(printBar);
    m_pagesPanel->setObjectName(QStringLiteral("pagesPanel"));
    printLayout->insertWidget(0, m_pagesPanel, 0);
    // The same picker from the page strip: the layout belongs to a page, and
    // this is where pages are handled.
    auto *pageTemplatesButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Sablonok"), printBar);
    pageTemplatesButton->setObjectName(QStringLiteral("pageStripTemplatesButton"));
    pageTemplatesButton->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Sablonok"));
    pageTemplatesButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Sablonok ehhez a laphoz"));
    printLayout->insertWidget(1, pageTemplatesButton, 0);
    connect(pageTemplatesButton, &QPushButton::clicked, this, &MainWindow::openTemplateBrowser);
    m_pagesPanel->onSelect = [this](int index) { pageSelected(index); };
    m_pagesPanel->onAdd = [this] { addPage(); };
    m_pagesPanel->onDuplicate = [this] { duplicatePage(); };
    m_pagesPanel->onRemove = [this] { removePage(); };
    m_pagesPanel->onMove = [this](int delta) { movePage(delta); };

    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addLayout(layout, 1);
    centralLayout->addWidget(m_noticeBar);
    centralLayout->addWidget(printBar);
    setCentralWidget(central);

    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(80);

    applyTheme();
}


void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(QCoreApplication::translate("PhotoPrint", "Fájl"));

    QAction *open = fileMenu->addAction(QCoreApplication::translate("PhotoPrint", "Projekt megnyitása…"));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::openProject);

    QAction *saveProjectAction = fileMenu->addAction(QCoreApplication::translate("PhotoPrint", "Mentés"));
    saveProjectAction->setShortcut(QKeySequence::Save);
    connect(saveProjectAction, &QAction::triggered, this, &MainWindow::saveProject);

    QAction *saveAs = fileMenu->addAction(QCoreApplication::translate("PhotoPrint", "Mentés másként…"));
    saveAs->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(saveAs, &QAction::triggered, this, &MainWindow::saveProjectAs);

    QAction *portable = fileMenu->addAction(QCoreApplication::translate("PhotoPrint", "Hordozható másolat…"));
    connect(portable, &QAction::triggered, this, &MainWindow::savePortableCopy);

    fileMenu->addSeparator();
    QAction *add = fileMenu->addAction(QCoreApplication::translate("PhotoPrint", "Fotók hozzáadása…"));
    add->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    connect(add, &QAction::triggered, this, &MainWindow::addPhotos);

    QAction *save = fileMenu->addAction(QCoreApplication::translate("PhotoPrint", "Mentés PDF-ként…"));
    connect(save, &QAction::triggered, this, &MainWindow::savePdfAll);

    QAction *saveCurrent = fileMenu->addAction(QCoreApplication::translate("PhotoPrint", "Aktuális lap mentése PDF-ként…"));
    connect(saveCurrent, &QAction::triggered, this, &MainWindow::savePdfCurrent);

    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(QCoreApplication::translate("PhotoPrint", "Kilépés"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(QCoreApplication::translate("PhotoPrint", "Szerkesztés"));
    m_undoAction = m_controller.undoStack()->createUndoAction(this, QCoreApplication::translate("PhotoPrint", "Vissza"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = m_controller.undoStack()->createRedoAction(this, QCoreApplication::translate("PhotoPrint", "Újra"));
    m_redoAction->setShortcut(QKeySequence::Redo);
    editMenu->addAction(m_undoAction);
    editMenu->addAction(m_redoAction);
    editMenu->addSeparator();
    QAction *remove = editMenu->addAction(QCoreApplication::translate("PhotoPrint", "Kiválasztott fotó eltávolítása"));
    remove->setShortcut(QKeySequence::Delete);
    connect(remove, &QAction::triggered, this, &MainWindow::deleteSelected);

    editMenu->addSeparator();
    QAction *templates =
        editMenu->addAction(QCoreApplication::translate("PhotoPrint", "Sablonok…"));
    templates->setObjectName(QStringLiteral("templatesAction"));
    connect(templates, &QAction::triggered, this, &MainWindow::openTemplateBrowser);

    buildLanguageMenu();

    QMenu *helpMenu = menuBar()->addMenu(QCoreApplication::translate("PhotoPrint", "Súgó"));
    QAction *diagnostics = helpMenu->addAction(QCoreApplication::translate("PhotoPrint", "Hibakeresési adatok"));
    connect(diagnostics, &QAction::triggered, this, &MainWindow::showDiagnostics);

    helpMenu->addSeparator();

    QAction *about = helpMenu->addAction(QCoreApplication::translate("PhotoPrint", "Névjegy"));
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::buildLanguageMenu()
{
    QMenu *languageMenu = menuBar()->addMenu(QCoreApplication::translate("PhotoPrint", "Nyelv"));
    languageMenu->setObjectName(QStringLiteral("languageMenu"));

    // The language names stay in their own script; the current one is checked.
    const QString selected = pp::i18n::selectedLanguage();
    auto *group = new QActionGroup(languageMenu);
    group->setExclusive(true);

    auto addLanguage = [languageMenu, group, selected](const QString &code) {
        QAction *action = languageMenu->addAction(pp::i18n::nativeName(code));
        action->setCheckable(true);
        action->setChecked(pp::i18n::resolveLocale(code) == selected);
        action->setData(code);
        group->addAction(action);
        return action;
    };

    for (const pp::i18n::Language &language : pp::i18n::targetLanguages())
        connect(addLanguage(language.code), &QAction::triggered, this,
                [this, code = language.code] { setLanguage(code); });
    connect(addLanguage(QString()), &QAction::triggered, this, [this] { setLanguage(QString()); });
}

void MainWindow::setLanguage(const QString &code)
{
    if (pp::i18n::resolveLocale(code) == pp::i18n::selectedLanguage())
        return;
    pp::i18n::saveLanguage(code);
    pp::i18n::installLanguage(pp::i18n::selectedLanguage());
    // The menu is still running its own event loop while the action fires, so
    // the rebuild waits until the click is fully delivered.
    QTimer::singleShot(0, this, &MainWindow::rebuildUi);
}

void MainWindow::rebuildUi()
{
    // The browser labels are translated as well; close it rather than leaving
    // a dialog in the previous language.
    if (m_effectDialog)
        m_effectDialog->close();
    if (m_templateDialog) {
        m_templateDialog->close();
        m_templateDialog = nullptr;
    }
    // Everything the user was looking at survives the language change.
    const QString selected = m_selectedId;
    const bool dirty = m_dirty;
    const int copies = m_copiesSpin ? m_copiesSpin->value() : 1;
    const double zoom = m_pageView ? m_pageView->viewZoom() : 1.0;
    const bool landscape = m_landscape;
    if (!currentPaper().name.isEmpty())
        m_paperName = currentPaper().name;

    if (m_resizeTimer) {
        m_resizeTimer->stop();
        delete m_resizeTimer;
        m_resizeTimer = nullptr;
    }
    if (m_undoAction) {
        delete m_undoAction;
        m_undoAction = nullptr;
    }
    if (m_redoAction) {
        delete m_redoAction;
        m_redoAction = nullptr;
    }
    const QList<QAction *> menuActions = menuBar()->actions();
    for (QAction *action : menuActions) {
        menuBar()->removeAction(action);
        delete action->menu();
    }
    const QList<QToolBar *> toolbars = findChildren<QToolBar *>();
    for (QToolBar *bar : toolbars) {
        removeToolBar(bar);
        delete bar;
    }
    if (m_checkpointTimer)
        m_checkpointTimer->stop();
    if (QWidget *central = takeCentralWidget())
        delete central;

    m_pageView = nullptr;
    m_photoList = nullptr;
    m_pagesPanel = nullptr;
    m_noticeBar = nullptr;
    m_inspectorPanel = nullptr;
    m_layoutPanel = nullptr;
    m_photoPanel = nullptr;
    m_printButton = nullptr;
    m_printerCombo = nullptr;

    m_selectedId = selected;
    buildUi();
    buildMenus();

    refreshPrinters();

    const bool wasRestoring = m_restoringProject;
    m_restoringProject = true;
    onDocumentChanged();
    m_restoringProject = wasRestoring;
    m_dirty = dirty;
    updateBusyOverlay();

    m_orientationCombo->blockSignals(true);
    m_orientationCombo->setCurrentIndex(landscape ? 1 : 0);
    m_orientationCombo->blockSignals(false);
    m_copiesSpin->setValue(copies);
    if (m_pageView) {
        m_pageView->setViewZoom(zoom);
        if (m_pageView->onViewZoomChanged)
            m_pageView->onViewZoomChanged(zoom);
    }
    m_printButton->setEnabled(!m_printInProgress);
    updateTitle();
    if (m_checkpointTimer && dirty)
        m_checkpointTimer->start();
}

void MainWindow::showAbout()
{
    AboutDialog dialog(this);
    dialog.exec();
}

PaperSpec MainWindow::currentPaper() const
{
    if (m_papers.isEmpty())
        return PaperSpec{};
    const int index = m_paperCombo ? m_paperCombo->currentIndex() : 0;
    return m_papers.at(std::clamp(index, 0, static_cast<int>(m_papers.size()) - 1));
}

void MainWindow::rebuildPaperCombo(const QString &selectedName)
{
    if (!m_paperCombo)
        return;
    m_papers = defaultPapers();
    if (!qEnvironmentVariableIsSet("PHOTOPRINT_NO_SETTINGS"))
        m_papers.append(m_customPaperStore.all());

    int selected = 0;
    for (int i = 0; i < m_papers.size(); ++i) {
        if (!selectedName.isEmpty() && m_papers.at(i).name == selectedName)
            selected = i;
    }
    m_paperName = m_papers.at(selected).name;

    const bool wasSyncing = m_syncing;
    m_syncing = true;
    m_paperCombo->clear();
    for (const PaperSpec &paper : m_papers)
        m_paperCombo->addItem(paperLabel(paper), paper.name);
    m_paperCombo->setCurrentIndex(selected);
    m_syncing = wasSyncing;
}

void MainWindow::ensurePaperInCombo(const PaperSpec &paper)
{
    if (!m_paperCombo || paper.name.isEmpty())
        return;
    for (int i = 0; i < m_papers.size(); ++i) {
        if (m_papers.at(i).name == paper.name) {
            m_paperCombo->blockSignals(true);
            m_paperCombo->setCurrentIndex(i);
            m_paperCombo->blockSignals(false);
            return;
        }
    }
    // A project may carry a paper that is not in the saved list any more.
    m_papers.append(paper);
    m_paperCombo->blockSignals(true);
    m_paperCombo->addItem(paperLabel(paper), paper.name);
    m_paperCombo->setCurrentIndex(m_papers.size() - 1);
    m_paperCombo->blockSignals(false);
}

Frame MainWindow::makeInitialFrame(const SourceImage &source, const Page &page) const
{
    const QSizeF pageSize = pageSizeMm(page);
    const QSize pixels = source.size();
    const double maxWidth = pageSize.width() * 0.7;
    const double maxHeight = pageSize.height() * 0.7;
    const double ratio =
        pixels.width() > 0 ? static_cast<double>(pixels.height()) / pixels.width() : 1.0;
    double width = maxWidth;
    double height = width * ratio;
    if (height > maxHeight) {
        height = maxHeight;
        width = height / ratio;
    }

    Frame frame;
    frame.id = makeId(QStringLiteral("frame"));
    frame.sourcePath = source.path;
    frame.sourceSha256 = source.sha256;
    frame.cx = pageSize.width() / 2.0;
    frame.cy = pageSize.height() / 2.0;
    frame.width = width;
    frame.height = height;
    frame.fit = FitMode::Full;
    return frame;
}

void MainWindow::addPhotos()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QCoreApplication::translate("PhotoPrint", "Fotók hozzáadása"), QString(),
        QCoreApplication::translate("PhotoPrint", "Fotók (*.jpg *.jpeg *.png *.JPG *.JPEG *.PNG)"));
    if (!paths.isEmpty())
        addFilesAt(paths, QString());
}

void MainWindow::addDroppedFiles(const QStringList &paths)
{
    addFilesAt(paths, QString());
}

std::optional<Frame> MainWindow::photoPayload(const QString &path, const Page &page)
{
    Result<SourceImage> loaded = m_store.load(path, ImportPolicy{});
    if (!loaded) {
        showProblem(loaded.error().message);
        return std::nullopt;
    }
    Frame frame = makeInitialFrame(loaded.value(), page);
    const QImage toneSample = ColorPipeline::toLinear(loaded.value(), ColorRecipe{}, 1000);
    frame.tone = ToneEngine::suggest(toneSample);
    frame.autoTone = true;
    return frame;
}

void MainWindow::addFilesAt(const QStringList &paths, const QString &slotId)
{
    QStringList files;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.exists() && info.isFile())
            files.append(info.absoluteFilePath());
    }
    if (files.isEmpty())
        return;

    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page page = document.pages.at(document.currentPage);

    if (!page.hasLayout()) {
        if (files.size() == 1) {
            addPhoto(files.first());
            return;
        }
        // A free canvas has no cells: build the same placement the sequential
        // path makes, but publish it once so a multi-file drop is one undo.
        Page built = page;
        QString lastId;
        for (const QString &path : files) {
            const std::optional<Frame> payload = photoPayload(path, built);
            if (!payload)
                continue;
            Frame frame = *payload;
            frame.autoPlaced = true;
            QHash<QString, QSize> pixelSizes;
            for (const Frame &existing : built.frames) {
                if (const std::optional<SourceImage> source =
                        m_store.cached(existing.sourcePath))
                    pixelSizes.insert(existing.id, source->size());
            }
            if (const std::optional<SourceImage> source = m_store.cached(frame.sourcePath))
                pixelSizes.insert(frame.id, source->size());
            bool allAuto = !built.frames.isEmpty();
            for (const Frame &existing : built.frames) {
                if (!existing.autoPlaced) {
                    allAuto = false;
                    break;
                }
            }
            if (allAuto) {
                built.frames = LayoutEngine::appendPhoto(built, built.frames, frame, pixelSizes);
            } else {
                const int count = built.frames.size();
                frame.cx += (count % 5) * 6.0;
                frame.cy += (count % 5) * 6.0;
                built.frames.append(frame);
            }
            lastId = frame.id;
        }
        if (lastId.isEmpty() || built == page)
            return;
        const auto published = m_controller.publishPage(
            built, QCoreApplication::translate("PhotoPrint", "Fotók hozzáadása"));
        if (!published) {
            showProblem(published.error().message);
            return;
        }
        selectFrame(lastId, false);
        return;
    }

    QList<Frame> payloads;
    payloads.reserve(files.size());
    for (const QString &path : files) {
        if (const std::optional<Frame> payload = photoPayload(path, page))
            payloads.append(*payload);
    }
    if (payloads.isEmpty())
        return;

    // An empty target id is the page background: the first empty cell takes the
    // first file, exactly as a drop on a cell would. No empty cell means the
    // photographs fall through to free placement below.
    QString target = slotId;
    if (target.isEmpty()) {
        if (const Slot *first = SlotGeometry::firstEmptySlot(page))
            target = first->id;
    }

    SlotLayoutEngine::Report report;
    auto placed = SlotLayoutEngine::placePhotos(page, target, payloads, &report);
    if (!placed) {
        showProblem(placed.error().message);
        return;
    }
    Page finished = placed.value();

    // More files than empty cells: the surplus goes to new pages with the same
    // layout, never onto the sheet as free frames across the sections. All of it
    // is one edit and one undo.
    QList<Frame> remaining;
    for (const Frame &payload : payloads) {
        if (report.leftFreeIds.contains(payload.id))
            remaining.append(payload);
    }
    QList<Page> extraPages;
    while (!remaining.isEmpty()) {
        Page extra;
        extra.id = makeId(QStringLiteral("page"));
        extra.paper = page.paper;
        extra.landscape = page.landscape;
        extra.slotList = page.slotList;
        extra.slotStyle = page.slotStyle;
        extra.layoutId = page.layoutId;
        SlotLayoutEngine::Report extraReport;
        auto filled = SlotLayoutEngine::placePhotos(extra, QString(), remaining, &extraReport);
        if (!filled) {
            showProblem(filled.error().message);
            return;
        }
        QList<Frame> stillFree;
        for (const Frame &frame : remaining) {
            if (extraReport.leftFreeIds.contains(frame.id))
                stillFree.append(frame);
        }
        extraPages.append(filled.value());
        if (stillFree.size() == remaining.size())
            break; // no usable cell on this layout: stop instead of looping
        remaining = stillFree;
    }

    ApplyLayoutEdit edit;
    PageLayout change;
    change.pageIndex = document.currentPage;
    change.frames = finished.frames;
    change.slotList = finished.slotList;
    change.slotStyle = finished.slotStyle;
    change.layoutId = finished.layoutId;
    change.cutGuides = finished.cutGuides;
    change.borderlessOverride = finished.borderlessOverride;
    edit.pages.append(change);
    edit.newPages = extraPages;
    edit.insertIndex = document.currentPage + 1;
    edit.selectPage = document.currentPage;
    edit.actionText = QCoreApplication::translate("PhotoPrint", "Fotó beillesztése");
    const auto published = m_controller.apply(edit);
    if (!published) {
        showProblem(published.error().message);
        return;
    }
    if (!extraPages.isEmpty()) {
        statusBar()->showMessage(
            QCoreApplication::translate("PhotoPrint", "A többi fotó új lapra került."), 6000);
    }

    // The dropped photograph (or the first file of a multi-drop) is selected, so
    // the inspector and the library show what just arrived. A file that landed
    // only on a new page is not selected on this one.
    QString placedId;
    if (!target.isEmpty()) {
        if (const Frame *frame = SlotGeometry::frameInSlot(finished, target))
            placedId = frame->id;
    }
    if (placedId.isEmpty()) {
        for (const Frame &payload : payloads) {
            for (const Frame &frame : finished.frames) {
                if (frame.id == payload.id) {
                    placedId = payload.id;
                    break;
                }
            }
            if (!placedId.isEmpty())
                break;
        }
    }
    if (!placedId.isEmpty())
        selectFrame(placedId, false);
}

void MainWindow::addPhoto(const QString &path)
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    if (page.hasLayout()) {
        // A laid-out page fills its first empty cell; appending a free frame on
        // top of the layout would ignore the cells the user is looking at.
        addFilesAt(QStringList{path}, QString());
        return;
    }

    const std::optional<Frame> payload = photoPayload(path, page);
    if (!payload)
        return;
    Frame frame = *payload;
    frame.autoPlaced = true;

    QHash<QString, QSize> pixelSizes;
    for (const Frame &existing : page.frames) {
        if (const std::optional<SourceImage> source = m_store.cached(existing.sourcePath))
            pixelSizes.insert(existing.id, source->size());
    }
    if (const std::optional<SourceImage> source = m_store.cached(frame.sourcePath))
        pixelSizes.insert(frame.id, source->size());

    bool allAuto = !page.frames.isEmpty();
    for (const Frame &existing : page.frames) {
        if (!existing.autoPlaced) {
            allAuto = false;
            break;
        }
    }
    if (allAuto) {
        const QList<Frame> frames =
            LayoutEngine::appendPhoto(page, page.frames, frame, pixelSizes);
        m_controller.apply(SetFramesEdit{document.currentPage, frames});
    } else {
        const int count = page.frames.size();
        frame.cx += (count % 5) * 6.0;
        frame.cy += (count % 5) * 6.0;
        m_controller.apply(AddFrameEdit{frame, -1});
    }
    selectFrame(frame.id, false);
}

void MainWindow::dropFrameAt(const QString &frameId, const QString &slotId, const QPointF &pagePoint)
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page page = document.pages.at(document.currentPage);

    const Frame *source = nullptr;
    for (const Frame &frame : page.frames) {
        if (frame.id == frameId) {
            source = &frame;
            break;
        }
    }
    if (!source)
        return;

    if (!page.hasLayout()) {
        // A free canvas has no cells to drop into: the drag moves the photo to
        // the drop point, as one edit, instead of being silently ignored.
        const QSizeF sheet = pageSizeMm(page);
        if (sheet.width() <= 0.0 || sheet.height() <= 0.0)
            return;
        Page placed = page;
        for (Frame &frame : placed.frames) {
            if (frame.id != frameId)
                continue;
            frame.cx = std::clamp(pagePoint.x(), 0.0, sheet.width());
            frame.cy = std::clamp(pagePoint.y(), 0.0, sheet.height());
            frame.autoPlaced = false;
            break;
        }
        if (placed == page) {
            selectFrame(frameId, false);
            return;
        }
        m_controller.publishPage(placed,
                                 QCoreApplication::translate("PhotoPrint",
                                                             "Fotó áthelyezése"));
        selectFrame(frameId, false);
        return;
    }

    if (slotId.isEmpty()) {
        // Dropped outside every cell: the photograph leaves the layout and
        // becomes a free frame where it stands. One undo puts it back.
        if (source->slotId.isEmpty())
            return;
        Page detached = page;
        for (Frame &frame : detached.frames) {
            if (frame.id != frameId)
                continue;
            frame.slotId.clear();
            frame.autoPlaced = false;
            break;
        }
        m_controller.publishPage(detached,
                                 QCoreApplication::translate("PhotoPrint",
                                                             "Fotó leválasztása"));
        selectFrame(frameId, false);
        selectSlot(QString());
        return;
    }

    const Frame *occupant = SlotGeometry::frameInSlot(page, slotId);
    if (occupant && occupant->id == frameId) {
        selectFrame(frameId, false); // dropped on its own cell: a selection
        return;
    }

    SlotLayoutEngine::Report report;
    auto moved = occupant
        ? SlotLayoutEngine::swapPhotos(page, frameId, slotId, &report)
        : SlotLayoutEngine::movePhoto(page, frameId, slotId, &report);
    if (!moved) {
        showProblem(moved.error().message);
        return;
    }
    const QString action = occupant
        ? QCoreApplication::translate("PhotoPrint", "Képek cseréje")
        : QCoreApplication::translate("PhotoPrint", "Fotó áthelyezése");
    m_controller.publishPage(moved.value(), action);
    selectFrame(frameId, false);
}

void MainWindow::clearSlot(const QString &slotId)
{
    if (slotId.isEmpty())
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page page = document.pages.at(document.currentPage);
    if (!page.hasLayout() || !SlotGeometry::slotById(page, slotId))
        return;
    if (!SlotGeometry::frameInSlot(page, slotId))
        return; // an empty cell has nothing to clear, and no edit to make

    auto emptied = SlotLayoutEngine::emptySlot(page, slotId);
    if (!emptied) {
        showProblem(emptied.error().message);
        return;
    }
    m_controller.publishPage(emptied.value(),
                             QCoreApplication::translate("PhotoPrint", "Hely ürítése"));
    // The cell stays selected; its photograph is gone, not the cell.
    m_selectedId.clear();
    m_selectedSlotId = slotId;
    if (m_pageView) {
        m_pageView->setSelectedFrame(QString());
        m_pageView->setSelectedSlot(slotId);
    }
    syncListSelection();
    syncFrameControls();
}

void MainWindow::onDocumentChanged()
{
    // A change from outside the panel (an undo, a project load, another page)
    // makes a pending style burst stale: landing it afterwards would write a
    // value into a document the user is no longer looking at.
    cancelPendingStyle();
    if (!m_restoringProject) {
        m_dirty = true;
        if (m_checkpointTimer)
            m_checkpointTimer->start();
    }
    updateTitle();
    rebuildPreviewImages();
    refreshPhotoList();
    const DocumentSnapshot document = m_controller.document();
    const Page *page = document.pages.isEmpty()
        ? nullptr
        : &document.pages.at(document.currentPage);
    const bool hasLayout = page && page->hasLayout();
    m_pageView->setDocument(document);
    m_pageView->setFrameImages(m_previewImages);

    // Selection survives a document change while its frame or cell still
    // exists. A selected photograph also selects the cell that owns it.
    if (!m_selectedId.isEmpty() && !findFrame(document, m_selectedId))
        m_selectedId.clear();
    if (!m_selectedSlotId.isEmpty()
        && (!page || !SlotGeometry::slotById(*page, m_selectedSlotId)))
        m_selectedSlotId.clear();
    if (const Frame *selected =
            m_selectedId.isEmpty() ? nullptr : findFrame(document, m_selectedId)) {
        m_selectedSlotId = selected->slotId;
    } else if (!m_selectedSlotId.isEmpty() && page) {
        // The selected empty cell gained a photograph (an undo, a drop, a
        // top-up): the photo becomes the selection so the next action acts on it.
        if (const Frame *occupant = SlotGeometry::frameInSlot(*page, m_selectedSlotId))
            m_selectedId = occupant->id;
    }

    // A frame that left the document cannot be "not found" any more.
    for (auto it = m_unavailableFrames.begin(); it != m_unavailableFrames.end();) {
        if (!findFrame(document, *it))
            it = m_unavailableFrames.erase(it);
        else
            ++it;
    }
    m_pageView->setSelectedFrame(m_selectedId);
    m_pageView->setSelectedSlot(m_selectedSlotId);
    m_pageView->setUnavailableFrames(m_unavailableFrames);

    // A laid-out page carries its own drop hints on the cells; the page-wide
    // line would draw a second instruction over them.
    m_pageView->setEmptyHint(
        !page || hasLayout
            ? QString()
            : (page->frames.isEmpty()
                   ? QCoreApplication::translate(
                         "PhotoPrint",
                         "Húzd ide a fotókat, vagy válaszd a Fotók hozzáadása gombot.")
                   : QString()));
    const bool overflows = pageOverflows();
    m_overflowLabel->setVisible(overflows);
    // "Igazítás a laphoz" is meaningless on a laid-out page: the cells already
    // fit the paper by construction, and scaling them would break the slots.
    m_fitPageButton->setVisible(overflows && !hasLayout);
    m_fitPageButton->setEnabled(!hasLayout);
    updatePrintButton();
    updateNoticeBar();
    if (m_pagesPanel)
        m_pagesPanel->setPages(document.pages, document.currentPage);
    syncFrameControls();
    syncPageTools();
    syncPrintControls();
    if (m_templateDialog && !document.pages.isEmpty()) {
        const Page &page = document.pages.at(document.currentPage);
        m_templateDialog->setPage(page.paper, page.landscape, page.frames,
                                  templatePhotoThumbnails(page), page.slotList,
                                  page.slotStyle, page.layoutId);
    }
}

void MainWindow::rebuildPreviewImages()
{
    // Every document change bumps the generation. The GUI thread never renders
    // a page preview; the previous image stays on screen until the matching
    // generation arrives and stale results are discarded.
    ++m_previewGeneration;
    m_previewDirty = true;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty()) {
        m_previewDirty = false;
        m_previewImages.clear();
        m_originalPreviews.clear();
        m_previewJobActive = false;
        updateBusyOverlay();
        return;
    }
    if (m_previewWatcher && m_previewWatcher->isRunning()) {
        if (m_previewStop)
            m_previewStop->request_stop();
        updateBusyOverlay();
        return; // handlePreviewFinished starts the newest generation
    }
    startPreviewJob();
}

void MainWindow::startPreviewJob()
{
    if (!m_previewDirty || !m_previewWatcher)
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty()) {
        m_previewDirty = false;
        return;
    }
    auto job = std::make_shared<PreviewJob>();
    job->generation = m_previewGeneration;
    job->document = document;
    job->pageIndex = document.currentPage;
    job->stop = std::make_shared<std::stop_source>();
    m_previewStop = job->stop;
    m_previewDirty = false;
    m_previewJobActive = true;
    updateBusyOverlay();

    // The job owns a copy of the document. The store is a member of the
    // window, and ~MainWindow waits for this future, so the pointer cannot
    // outlive the store.
    ImageStore *store = &m_store;
    m_previewWatcher->setFuture(QtConcurrent::run(effectPreviewPool(), [job, store] {
        PreviewResult result;
        result.generation = job->generation;
        result.pageIndex = job->pageIndex;
        const std::stop_token token = job->stop->get_token();
        if (job->pageIndex < 0 || job->pageIndex >= job->document.pages.size())
            return result;
        const Page &page = job->document.pages.at(job->pageIndex);
        for (const Frame &frame : page.frames) {
            if (token.stop_requested())
                break;
            Result<SourceImage> source = store->load(frame.sourcePath, ImportPolicy{}, token);
            if (!source)
                continue;
            ColorRecipe recipe = sanitiseRecipe(job->document.recipe);
            recipe.tone = effectiveTone(frame);
            EffectRenderTarget target;
            target.recipeVersion = frame.effectVersion;
            Result<QImage> converted = store->preparedLinear(
                source.value(), recipe, frame.filter, frame.filterStrength, frame.upscale, 1600,
                token, target);
            if (converted)
                result.images.insert(frame.id, converted.value());
        }
        return result;
    }));
}

void MainWindow::handlePreviewFinished()
{
    if (!m_previewWatcher || !m_previewWatcher->isFinished())
        return;
    if (m_previewDirty) {
        startPreviewJob();
        return;
    }
    const PreviewResult result = m_previewWatcher->result();
    if (result.generation != m_previewGeneration)
        return;
    m_previewJobActive = false;
    updateBusyOverlay();
    const DocumentSnapshot document = m_controller.document();
    if (result.pageIndex != document.currentPage)
        return;
    m_previewImages = result.images;
    // What the pass could not decode is exactly the set that may say "not
    // found". Everything else that is merely absent is still loading.
    const Page &page = document.pages.at(document.currentPage);
    QSet<QString> unavailable;
    for (const Frame &frame : page.frames) {
        if (frame.sourcePath.isEmpty() || result.images.contains(frame.id))
            continue;
        unavailable.insert(frame.id);
    }
    m_unavailableFrames = unavailable;
    if (m_pageView) {
        m_pageView->setUnavailableFrames(unavailable);
        m_pageView->setFrameImages(displayedPreviewImages());
    }
    updateResolutionHint();
}

QString MainWindow::sourceKeyFor(const Frame &frame) const
{
    // A frame's untouched layer depends on the file content and the document
    // recipe (grayscale, paper). Two frames of the same photo share one entry.
    const QString identity = frame.sourceSha256.isEmpty()
        ? QStringLiteral("path:") + frame.sourcePath
        : QString::fromLatin1(frame.sourceSha256.toHex());
    return identity + QLatin1Char('|')
        + QString::fromLatin1(sanitiseRecipe(m_controller.document().recipe).digest().toHex());
}

void MainWindow::startOriginalPreview(const Frame &frame)
{
    // The untouched layer is lazy: it is rendered the first time Eredeti is
    // pressed and then cached by source identity. Preparing it for every frame
    // of every preview batch doubled the work on a page of effects.
    const QString sourceKey = sourceKeyFor(frame);
    if (m_originalPreviews.contains(sourceKey))
        return;
    if (m_originalJobActive && m_originalJobSourceKey == sourceKey)
        return;
    ++m_originalGeneration;
    if (m_originalStop)
        m_originalStop->request_stop();

    auto job = std::make_shared<OriginalJob>();
    job->generation = m_originalGeneration;
    job->sourceKey = sourceKey;
    job->sourcePath = frame.sourcePath;
    job->sha256 = frame.sourceSha256;
    job->recipe = sanitiseRecipe(m_controller.document().recipe);
    job->recipe.tone = PhotoTone{};
    job->stop = std::make_shared<std::stop_source>();
    m_originalStop = job->stop;
    m_originalJobSourceKey = sourceKey;
    m_originalJobActive = true;
    ++m_originalRenderCount;
    updateBusyOverlay();

    ImageStore *store = &m_store;
    m_originalWatcher->setFuture(QtConcurrent::run(effectPreviewPool(), [job, store] {
        OriginalResult result;
        result.generation = job->generation;
        result.sourceKey = job->sourceKey;
        const std::stop_token token = job->stop->get_token();
        Result<SourceImage> source = store->load(job->sourcePath, ImportPolicy{}, token);
        if (!source)
            return result;
        Result<QImage> converted = store->preparedLinear(
            source.value(), job->recipe, QStringLiteral("original"), 100, false, 1600, token,
            EffectRenderTarget{});
        if (converted)
            result.image = converted.value();
        return result;
    }));
}

void MainWindow::handleOriginalPreviewFinished()
{
    if (!m_originalWatcher || !m_originalWatcher->isFinished())
        return;
    const OriginalResult result = m_originalWatcher->result();
    if (result.generation != m_originalGeneration)
        return;
    m_originalJobActive = false;
    if (!result.image.isNull()) {
        if (m_originalPreviews.size() > 64)
            m_originalPreviews.clear();
        m_originalPreviews.insert(result.sourceKey, result.image);
    }
    updateBusyOverlay();
    if (!m_showOriginal || !m_pageView)
        return;
    const std::optional<Frame> frame = selectedFrame();
    if (frame && sourceKeyFor(*frame) == result.sourceKey)
        m_pageView->setFrameImages(displayedPreviewImages());
}

QHash<QString, QImage> MainWindow::displayedPreviewImages() const
{
    if (!m_showOriginal || m_selectedId.isEmpty())
        return m_previewImages;
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return m_previewImages;
    const auto original = m_originalPreviews.constFind(sourceKeyFor(*frame));
    if (original == m_originalPreviews.constEnd())
        return m_previewImages;
    QHash<QString, QImage> images = m_previewImages;
    images.insert(m_selectedId, original.value());
    return images;
}

void MainWindow::cancelPreviewWork()
{
    // The user asked to stop waiting. Bump both generations, so whatever the
    // workers still finish is dropped instead of replacing the visible page.
    ++m_previewGeneration;
    if (m_previewStop)
        m_previewStop->request_stop();
    m_previewDirty = false;
    m_previewJobActive = false;
    ++m_originalGeneration;
    if (m_originalStop)
        m_originalStop->request_stop();
    m_originalJobActive = false;
    updateBusyOverlay();
}

void MainWindow::updateBusyOverlay()
{
    if (m_pageView)
        m_pageView->setBusyOverlay(m_previewJobActive || m_originalJobActive);
}

void MainWindow::updateResolutionHint()
{
    if (!m_resolutionHint)
        return;
    m_resolutionHint->clear();
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    const auto image = m_previewImages.constFind(frame->id);
    if (image != m_previewImages.constEnd()
        && LayoutEngine::resolutionHint(*frame, image->size())
            == LayoutEngine::ResolutionHint::Low) {
        m_resolutionHint->setText(
            QCoreApplication::translate("PhotoPrint", "Ekkora méretben a kép életlen lehet."));
    }
}

bool MainWindow::previewPending() const
{
    return m_previewDirty || m_previewJobActive;
}

bool MainWindow::waitForPreview(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (!previewPending())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
    return !previewPending();
}

bool MainWindow::waitForOriginalPreview(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (!m_originalJobActive)
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
    return !m_originalJobActive;
}

void MainWindow::openEffectBrowser()
{
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;

    EffectBrowserRequest request;
    request.currentKey = frame->filter;
    request.currentVersion = frame->effectVersion;
    request.currentStrength = frame->filterStrength;
    if (const std::optional<SourceImage> source = m_store.cached(frame->sourcePath)) {
        request.sampleHash = QString::fromLatin1(source->sha256.toHex().left(16));
        if (const Result<QImage> sample =
                m_store.srgb(source.value(), ColorRecipe{}, 480)) {
            request.sample = sample.value();
        }
    } else if (const Result<SourceImage> loaded =
                   m_store.load(frame->sourcePath, ImportPolicy{})) {
        request.sampleHash = QString::fromLatin1(loaded.value().sha256.toHex().left(16));
        if (const Result<QImage> sample =
                m_store.srgb(loaded.value(), ColorRecipe{}, 480)) {
            request.sample = sample.value();
        }
    }
    if (request.sample.isNull()) {
        request.sample = QImage(QStringLiteral(":/samples/effect-sample.jpg"));
        request.sampleHash = QStringLiteral("bundled-sample");
    }

    auto *dialog = new EffectBrowserDialog(request, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &EffectBrowserDialog::effectChosen, this, &MainWindow::applyEffectChoice);
    m_effectDialog = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::applyEffectChoice(const QString &key, int version, int strength)
{
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    Frame updated = *frame;
    updated.filter = key;
    updated.effectVersion = key == QLatin1String("original") ? 0 : version;
    // The render boundary applies the same rule, so the stored value is what
    // the preview and the print actually use.
    updated.filterStrength = effectiveEffectStrength(key, strength);
    if (updated.filter == frame->filter && updated.effectVersion == frame->effectVersion
        && updated.filterStrength == frame->filterStrength)
        return;
    // Key, version and strength travel as one FrameChange, so one undo puts the
    // whole previous look back.
    m_controller.apply(FrameChange{updated.id, updated});
}

void MainWindow::openTemplateBrowser()
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    const QList<QImage> photos = templatePhotoThumbnails(page);

    if (m_templateDialog) {
        // The entry points are three doors to one picker; a second click brings
        // the open one forward with the page it shows up to date.
        m_templateDialog->setPage(page.paper, page.landscape, page.frames, photos,
                                  page.slotList, page.slotStyle, page.layoutId);
        m_templateDialog->raise();
        m_templateDialog->activateWindow();
        return;
    }

    TemplateBrowserRequest request;
    request.paper = page.paper;
    request.landscape = page.landscape;
    request.frames = page.frames;
    request.slotList = page.slotList;
    request.slotStyle = page.slotStyle;
    request.layoutId = page.layoutId;
    request.photos = photos;

    auto *dialog = new TemplateBrowserDialog(request, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &TemplateBrowserDialog::templateChosen, this, &MainWindow::applyTemplate);
    m_templateDialog = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

bool MainWindow::applyTemplate(const QString &templateId)
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const CollageTemplate *chosen = catalogue.find(templateId);
    if (!chosen)
        return false;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return false;
    const Page &page = document.pages.at(document.currentPage);

    // One engine for every page. The page's own layout is topped up: its cells
    // and assignments stay, and its empty cells take the free photographs. Any
    // other page is laid out from scratch, assigning the photographs in display
    // order. Fewer photographs than cells is legal, and more than cells stay
    // free; the picker names both before Apply, from the same engine report.
    auto applied = page.layoutId == chosen->id
        ? SlotLayoutEngine::applyTemplateKeeping(page, *chosen)
        : SlotLayoutEngine::applyTemplate(page, *chosen);
    if (!applied) {
        // A refusal belongs where the choice was made. The picker stays open so
        // the person can see which layout was refused and why.
        if (m_templateDialog)
            m_templateDialog->reportApplyFailure(applied.error().message);
        else
            showProblem(applied.error().message);
        return false;
    }

    // The same layout on the same page changes nothing: no second undo entry,
    // and a saved project is not marked dirty by a click that applies nothing.
    if (applied.value() == page) {
        if (m_templateDialog)
            m_templateDialog->accept();
        return true;
    }

    // The slots, the assignments and the derived geometry land as one page
    // edit, so one undo restores the previous arrangement. Frame ids survive,
    // so the selection and the cached previews stay valid.
    const auto published = m_controller.publishPage(
        applied.value(), QCoreApplication::translate("PhotoPrint", "Sablon alkalmazása"));
    if (!published) {
        showProblem(published.error().message);
        return false;
    }
    if (m_templateDialog)
        m_templateDialog->accept();
    return true;
}

QList<QImage> MainWindow::templatePhotoThumbnails(const Page &page)
{
    // The picker follows the page on every document change, and a change arrives
    // for a nudge and for a selection as well. The pixels only depend on which
    // photographs are on the page, so the decode is cached by their identity.
    const ColorRecipe recipe = sanitiseRecipe(m_controller.document().recipe);
    QStringList keys;
    keys.reserve(page.frames.size() + 1);
    // The recipe is part of the identity: a grayscale page shows grey thumbnails.
    keys.append(QString::fromLatin1(recipe.digest().toHex()));
    for (const Frame &frame : page.frames) {
        keys.append(frame.id + QLatin1Char('|')
                    + QString::fromLatin1(frame.sourceSha256.toHex())
                    + QLatin1Char('|') + frame.sourcePath);
    }
    if (keys == m_templatePhotoKeys && m_templatePhotos.size() == page.frames.size())
        return m_templatePhotos;

    // Only what the preview job already decoded: opening the picker must not
    // block the window on ten photographs. A slot without a thumbnail keeps its
    // number, so the fill order is still readable.
    QList<QImage> photos;
    photos.reserve(page.frames.size());
    for (const Frame &frame : page.frames) {
        QImage image;
        if (const std::optional<SourceImage> source = m_store.cached(frame.sourcePath)) {
            if (const Result<QImage> small = m_store.srgb(*source, recipe, 96))
                image = small.value();
        }
        photos.append(image);
    }
    m_templatePhotoKeys = keys;
    m_templatePhotos = photos;
    return photos;
}

void MainWindow::refreshPhotoList()
{
    m_syncing = true;
    m_photoList->clear();
    const DocumentSnapshot document = m_controller.document();
    if (!document.pages.isEmpty()) {
        const Page &page = document.pages.at(document.currentPage);
        // Display order: slotted photographs by cell, then free photographs in
        // z-order. "1. fotó" therefore means the first cell, the same order the
        // drop handler fills.
        const QList<int> order = SlotLayoutEngine::frameIndicesInDisplayOrder(page);
        for (int position = 0; position < order.size(); ++position) {
            const Frame &frame = page.frames.at(order.at(position));
            auto *item = new QListWidgetItem(QString());
            item->setToolTip(QFileInfo(frame.sourcePath).fileName());
            // The position of the frame is an index, not a quantity: it has no
            // plural forms, so it stays a plain %1 placeholder.
            item->setData(Qt::AccessibleTextRole,
                          QCoreApplication::translate("PhotoPrint", "%1. fotó")
                              .arg(position + 1));
            item->setData(Qt::UserRole, frame.id);
            if (const std::optional<SourceImage> source = m_store.cached(frame.sourcePath)) {
                const Result<QImage> thumb =
                    m_store.srgb(*source, sanitiseRecipe(document.recipe), 96);
                if (thumb)
                    item->setIcon(QIcon(libraryThumbnail(thumb.value(), 64)));
            }
            m_photoList->addItem(item);
        }
        const QSizeF size = pageSizeMm(page);
        m_pageInfo->setText(QStringLiteral("%1 · %2")
                                .arg(page.paper.name)
                                .arg(sizeLabelCm(size.width(), size.height())));
        QString hint;
        if (page.paper.type == PaperType::Matte) {
            hint = QCoreApplication::translate(
                "PhotoPrint", "A matt papírt a nyomtató tintasugaras papírként kezeli.");
        }
        if (page.paper.isPhotoPaper() && !wantBorderless(page)) {
            if (!hint.isEmpty())
                hint += QLatin1Char(' ');
            hint += QCoreApplication::translate("PhotoPrint", "Ennél a papírnál keskeny fehér szél maradhat.");
        }
        m_paperHint->setText(hint);
    }
    m_syncing = false;
    syncListSelection();
}

void MainWindow::syncListSelection()
{
    if (m_syncing)
        return;
    m_syncing = true;
    m_photoList->clearSelection();
    for (int i = 0; i < m_photoList->count(); ++i) {
        QListWidgetItem *item = m_photoList->item(i);
        if (item->data(Qt::UserRole).toString() == m_selectedId) {
            m_photoList->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
            break;
        }
    }
    m_syncing = false;
}

void MainWindow::selectFrame(const QString &frameId, bool fromList)
{
    Q_UNUSED(fromList)
    const DocumentSnapshot document = m_controller.document();
    const Frame *frame = frameId.isEmpty() ? nullptr : findFrame(document, frameId);
    const QString slotId = frame ? frame->slotId : QString();
    if (m_selectedId == frameId && m_selectedSlotId == slotId)
        return;
    m_selectedId = frameId;
    m_selectedSlotId = slotId;
    if (m_pageView) {
        m_pageView->setSelectedFrame(frameId);
        m_pageView->setSelectedSlot(slotId);
    }
    if (frameId.isEmpty() && m_cropButton->isChecked()) {
        m_cropButton->setChecked(false);
        m_pageView->setCropMode(false);
    }
    syncListSelection();
    syncFrameControls();
}

void MainWindow::selectSlot(const QString &slotId)
{
    if (slotId.isEmpty()) {
        if (m_selectedSlotId.isEmpty())
            return;
        m_selectedSlotId.clear();
        if (m_pageView)
            m_pageView->setSelectedSlot(QString());
        syncFrameControls();
        return;
    }
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    if (!SlotGeometry::slotById(page, slotId))
        return;
    // A filled cell is a photograph: selecting the cell selects the photo in it.
    if (const Frame *occupant = SlotGeometry::frameInSlot(page, slotId)) {
        selectFrame(occupant->id, false);
        return;
    }
    if (m_selectedSlotId == slotId && m_selectedId.isEmpty())
        return;
    m_selectedSlotId = slotId;
    m_selectedId.clear();
    if (m_pageView) {
        m_pageView->setSelectedFrame(QString());
        m_pageView->setSelectedSlot(slotId);
    }
    if (m_cropButton->isChecked()) {
        m_cropButton->blockSignals(true);
        m_cropButton->setChecked(false);
        m_cropButton->blockSignals(false);
        m_pageView->setCropMode(false);
        m_cropButton->setText(QCoreApplication::translate("PhotoPrint", "Kivágás"));
    }
    syncListSelection();
    syncFrameControls();
}

std::optional<Frame> MainWindow::selectedFrame() const
{
    if (m_selectedId.isEmpty())
        return std::nullopt;
    // The snapshot is returned by value, so copy the frame out of it here
    // instead of handing back a pointer into storage that is about to die.
    const DocumentSnapshot document = m_controller.document();
    const Frame *frame = findFrame(document, m_selectedId);
    if (!frame)
        return std::nullopt;
    return *frame;
}

void MainWindow::syncFrameControls()
{
    // One snapshot for the whole sync: document() returns by value, so each
    // extra selectedFrame() call copies every page and frame again.
    const DocumentSnapshot document = m_controller.document();
    const Frame *frame =
        m_selectedId.isEmpty() ? nullptr : findFrame(document, m_selectedId);
    const bool hasSelection = frame != nullptr;
    // The side column carries two inspectors: the page's layout panel (visible
    // whenever the page has slots) and the selected photograph's. Neither hides
    // the other, and a laid-out page with an empty selection is a real state,
    // not a reason to hide the column.
    const Page *currentPage =
        document.pages.isEmpty() ? nullptr : &document.pages.at(document.currentPage);
    const bool pageHasLayout = currentPage && currentPage->hasLayout();
    if (m_photoPanel)
        m_photoPanel->setVisible(hasSelection);
    if (m_inspectorPanel)
        m_inspectorPanel->setVisible(hasSelection || pageHasLayout || !document.pages.isEmpty());
    m_fitButton->setEnabled(hasSelection);
    m_fillButton->setEnabled(hasSelection);
    m_cropButton->setEnabled(hasSelection);
    m_deleteButton->setEnabled(hasSelection);
    m_forwardButton->setEnabled(hasSelection);
    m_backwardButton->setEnabled(hasSelection);
    m_autoCheck->setEnabled(hasSelection);
    m_brightnessSlider->setEnabled(hasSelection);
    m_effectButton->setEnabled(hasSelection);
    const bool original = frame && frame->filter == QLatin1String("original");
    const bool dither = frame && effectIgnoresStrength(frame->filter);
    m_filterStrength->setEnabled(hasSelection && !original && !dither);
    m_strengthLabel->setEnabled(hasSelection && !original && !dither);
    m_upscaleCheck->setEnabled(hasSelection);
    m_originalButton->setEnabled(hasSelection);
    m_advancedToggle->setEnabled(hasSelection);
    if (m_batchButton)
        m_batchButton->setEnabled(hasSelection);
    // A slot owns the rectangle: width, height and angle are not the user's to
    // change on a laid-out page. Moving the photo is a crop pan instead.
    const bool slotted = frame && !frame->slotId.isEmpty();
    m_widthSpin->setEnabled(hasSelection && !slotted);
    m_heightSpin->setEnabled(hasSelection && !slotted);
    m_angleSpin->setEnabled(hasSelection && !slotted);
    // The in-slot controls describe a photograph the slot has fixed in place;
    // a free frame keeps the size and angle controls instead.
    if (m_slotImagePanel)
        m_slotImagePanel->setVisible(slotted);

    m_syncing = true;
    m_resolutionHint->clear();
    if (frame) {
        m_widthSpin->setValue(frame->width / 10.0);
        m_heightSpin->setValue(frame->height / 10.0);
        m_angleSpin->setValue(frame->angleDeg);
        // The wheel and the slider both write the same frame fields, so the
        // slider always shows the value the model holds. Its range is the
        // model's own 20..800 %, so the thumb never lies after a wheel gesture;
        // m_syncing keeps this write from looking like a user action.
        syncSlotImageControls(*frame);
        m_autoCheck->setChecked(frame->autoTone);
        if (m_autoHint) {
            if (!frame->autoTone)
                m_autoHint->setText(QCoreApplication::translate("PhotoPrint", "Kikapcsolva"));
            else if (frame->tone == PhotoTone{})
                m_autoHint->setText(QCoreApplication::translate("PhotoPrint", "Erre a képre most nincs szükség."));
            else
                m_autoHint->setText(QCoreApplication::translate("PhotoPrint", "Finom fény- és színjavítás"));
        }
        m_brightnessSlider->setValue(brightnessEvToSlider(frame->brightness));
        const QString label = effectButtonLabel(frame->filter, frame->effectVersion);
        m_effectButton->setText(label);
        m_effectButton->setAccessibleDescription(label);
        const bool legacy =
            isLegacyPresetKey(frame->filter) && frame->filter != QLatin1String("original");
        m_legacyHint->setVisible(legacy);
        m_strengthHint->setText(dither ? effectStrengthReason() : QString());
        m_strengthHint->setVisible(dither);
        m_filterStrength->setValue(dither ? 100 : std::clamp(frame->filterStrength, 0, 100));
        m_upscaleCheck->setChecked(frame->upscale);
        const auto image = m_previewImages.constFind(frame->id);
        if (image != m_previewImages.constEnd()
            && LayoutEngine::resolutionHint(*frame, image->size())
                == LayoutEngine::ResolutionHint::Low) {
            m_resolutionHint->setText(
                QCoreApplication::translate("PhotoPrint", "Ekkora méretben a kép életlen lehet."));
        }
    } else {
        m_effectButton->setText(QCoreApplication::translate("PhotoPrint", "Eredeti"));
        m_legacyHint->setVisible(false);
        m_strengthHint->setVisible(false);
    }
    m_syncing = false;
    syncLayoutPanel();
}

QString MainWindow::layoutDisplayName(const Page &page) const
{
    if (page.layoutId.isEmpty())
        return QCoreApplication::translate("PhotoPrint", "Saját elrendezés");
    if (const CollageTemplate *chosen = CollageTemplates::instance().find(page.layoutId))
        return chosen->nameEn.isEmpty() ? chosen->name : chosen->nameEn;
    // The label is kept verbatim: an unknown id is still what the page was saved
    // with, and renaming a catalogue entry must not rename a project.
    return page.layoutId;
}

void MainWindow::syncLayoutPanel()
{
    if (!m_layoutPanel)
        return;
    const DocumentSnapshot document = m_controller.document();
    const Page *page = document.pages.isEmpty() ? nullptr
                                                : &document.pages.at(document.currentPage);
    const bool hasLayout = page && page->hasLayout();
    m_layoutPanel->setVisible(hasLayout);
    if (!hasLayout)
        return;

    // Every control shows the value the page can actually use: a value that was
    // too large for this paper is displayed clamped, not silently kept.
    const QSizeF sheet = pageSizeMm(*page);
    const double shortSide = std::min(sheet.width(), sheet.height());
    const double marginLimit = std::max(
        0.0, std::min(SlotGeometry::kMaxMarginMm, shortSide / 2.0 - 2.0));
    // The gutter has its own range, so a future change to one constant cannot
    // silently move the other.
    const double gutterLimit = std::max(
        0.0, std::min(SlotGeometry::kMaxGutterMm, shortSide / 2.0 - 2.0));
    // The radius is clamped per cell, so the smallest cell on the page is the
    // one that limits it.
    double shortestCell = 0.0;
    for (const QRectF &cell : SlotGeometry::cellsMm(*page)) {
        const double side = std::min(cell.width(), cell.height());
        if (!(side > 0.0))
            continue;
        shortestCell = shortestCell <= 0.0 ? side : std::min(shortestCell, side);
    }
    const double radiusLimit =
        std::max(0.0, std::min(SlotGeometry::kMaxCornerRadiusMm, shortestCell / 2.0));

    const SlotStyle style = SlotGeometry::sanitiseStyle(page->slotStyle);
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    m_layoutNameLabel->setText(layoutDisplayName(*page));
    m_layoutCountLabel->setText(QCoreApplication::translate("PhotoPrint", "%n hely", nullptr,
                                                            page->slotList.size()));

    const bool automaticMargin = !style.marginMm.has_value();
    m_marginAutoCheck->setChecked(automaticMargin);
    m_marginSpin->setEnabled(!automaticMargin);
    m_marginSpin->setMaximum(marginLimit);
    m_marginSpin->setValue(
        style.marginMm.value_or(SlotGeometry::effectiveMarginMm(style, sheet)));

    const bool automaticGutter = !style.gutterMm.has_value();
    m_gutterAutoCheck->setChecked(automaticGutter);
    m_gutterSpin->setEnabled(!automaticGutter);
    m_gutterSpin->setMaximum(gutterLimit);
    m_gutterSpin->setValue(
        style.gutterMm.value_or(SlotGeometry::effectiveGutterMm(style, sheet)));

    m_radiusSpin->setMaximum(radiusLimit);
    m_radiusSpin->setValue(style.cornerRadiusMm);

    m_addSlotButton->setEnabled(page->slotList.size() < SlotGeometry::kMaxSlotsPerPage);
    m_removeSlotButton->setEnabled(
        !m_selectedSlotId.isEmpty() && SlotGeometry::slotById(*page, m_selectedSlotId));
    m_syncing = wasSyncing;
}

void MainWindow::syncPageTools()
{
    if (!m_guidesCombo || !m_alignButton)
        return;
    const DocumentSnapshot document = m_controller.document();
    const Page *page = document.pages.isEmpty() ? nullptr
                                                : &document.pages.at(document.currentPage);
    m_alignButton->setEnabled(page && !page->frames.isEmpty() && !page->hasLayout());
    m_alignButton->setToolTip(
        page && page->hasLayout()
            ? QCoreApplication::translate(
                  "PhotoPrint",
                  "Ehhez a laphoz elrendezés tartozik. Szüntesd meg az elrendezést az "
                  "igazításhoz.")
            : QCoreApplication::translate(
                  "PhotoPrint", "A lapon lévő fotók elrendezése. A sorrend nem változik."));

    const int index = page ? (page->cutGuides == CutGuides::Print
                                  ? 2
                                  : (page->cutGuides == CutGuides::Screen ? 1 : 0))
                           : 0;
    if (m_guidesHint) {
        m_guidesHint->setText(
            index == 2
                ? QCoreApplication::translate(
                      "PhotoPrint",
                      "A nyomaton is megjelenik; a vágás a vonal közepén halad, nyomtatás előtt "
                      "megerősítést kér.")
                : (index == 1
                       ? QCoreApplication::translate(
                             "PhotoPrint", "Csak a képernyőn látszik, a nyomatra nem kerül.")
                       : QCoreApplication::translate(
                             "PhotoPrint", "Nincs vágójel a képernyőn és a nyomaton.")));
    }

    int borderlessIndex = 0;
    if (page) {
        borderlessIndex = page->borderlessOverride == BorderlessOverride::Off
            ? 1
            : (page->borderlessOverride == BorderlessOverride::On ? 2 : 0);
    }
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    m_guidesCombo->setCurrentIndex(index);
    m_guidesCombo->setEnabled(page != nullptr);
    if (m_borderlessCombo) {
        m_borderlessCombo->setCurrentIndex(borderlessIndex);
        m_borderlessCombo->setEnabled(page != nullptr);
    }
    m_syncing = wasSyncing;
}

void MainWindow::queueSlotStyle(SlotStyleField field)
{
    if (m_syncing || !m_styleTimer)
        return;
    // A burst belongs to the control it was started in. Moving straight from one
    // spin to another lands the first one instead of rewriting the burst, so no
    // accepted value is ever dropped.
    if (m_pendingStyleField && *m_pendingStyleField != field)
        flushSlotStyle();
    // One spin keeps one undo step no matter how many notches the user clicks.
    m_pendingStyleField = field;
    m_styleTimer->start();
}

void MainWindow::cancelPendingStyle()
{
    if (m_styleTimer)
        m_styleTimer->stop();
    m_pendingStyleField.reset();
}

void MainWindow::flushSlotStyle()
{
    if (!m_pendingStyleField)
        return;
    const SlotStyleField field = *m_pendingStyleField;
    cancelPendingStyle();
    applySlotStyleField(field);
}

void MainWindow::applySlotStyleField(SlotStyleField field)
{
    if (m_syncing)
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    if (!page.hasLayout())
        return;

    // Only the edited field comes from the panel. The other three stay exactly
    // as the page stores them, so a value this paper clamps on screen (a 20 mm
    // border on a sheet whose cells only allow 19.1 mm) is never written back
    // over the stored one because some other control was touched.
    SlotStyle style = page.slotStyle;
    switch (field) {
    case SlotStyleField::Margin:
        // An unchecked switch pins the value in the spin, so 0 mm is a real
        // choice and "automatic" is the absence of one.
        style.marginMm = m_marginAutoCheck->isChecked()
            ? std::nullopt
            : std::optional<double>(m_marginSpin->value());
        break;
    case SlotStyleField::Gutter:
        style.gutterMm = m_gutterAutoCheck->isChecked()
            ? std::nullopt
            : std::optional<double>(m_gutterSpin->value());
        break;
    case SlotStyleField::Radius:
        style.cornerRadiusMm = m_radiusSpin->value();
        break;
    }
    if (style == page.slotStyle) {
        // The field already holds this value, so there is nothing to undo and no
        // undo entry is pushed.
        syncLayoutPanel();
        return;
    }
    auto restyled = SlotLayoutEngine::setStyle(page, style);
    if (!restyled) {
        showProblem(restyled.error().message);
        syncLayoutPanel();
        return;
    }
    const QString label = field == SlotStyleField::Margin
        ? QCoreApplication::translate("PhotoPrint", "Margó beállítása")
        : (field == SlotStyleField::Gutter
               ? QCoreApplication::translate("PhotoPrint", "Szegély beállítása")
               : QCoreApplication::translate("PhotoPrint", "Sarok lekerekítése"));
    const auto published = m_controller.publishPage(restyled.value(), label);
    if (!published) {
        showProblem(published.error().message);
        syncLayoutPanel();
    }
}

void MainWindow::addSlotToPage()
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    if (!page.hasLayout())
        return;
    // The selected cell is split; without a selection the largest cell is. The
    // engine owns the 15 mm floor and the 12-cell cap, so the refusal is the same
    // sentence wherever it is attempted.
    auto added = SlotLayoutEngine::addSlot(page, m_selectedSlotId);
    if (!added) {
        showProblem(added.error().message);
        return;
    }
    const auto published = m_controller.publishPage(
        added.value(), QCoreApplication::translate("PhotoPrint", "Hely hozzáadása"));
    if (!published)
        showProblem(published.error().message);
}

void MainWindow::removeSlotFromPage()
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    Page page = document.pages.at(document.currentPage);
    const QString slotId = m_selectedSlotId;
    if (!page.hasLayout() || slotId.isEmpty() || !SlotGeometry::slotById(page, slotId))
        return;

    // The photograph is not deleted with its cell. It comes off the layout first
    // and stays on the page as a free frame, exactly where it was printed, so
    // removing a cell can never destroy a picture. One edit carries both halves.
    bool detached = false;
    if (const Frame *occupant = SlotGeometry::frameInSlot(page, slotId)) {
        for (Frame &frame : page.frames) {
            if (frame.id != occupant->id)
                continue;
            frame.slotId.clear();
            frame.autoPlaced = false;
            detached = true;
            break;
        }
    }

    auto removed = SlotLayoutEngine::removeSlot(page, slotId);
    if (!removed) {
        showProblem(removed.error().message);
        return;
    }
    const auto published = m_controller.publishPage(
        removed.value(), QCoreApplication::translate("PhotoPrint", "Hely törlése"));
    if (!published) {
        showProblem(published.error().message);
        return;
    }
    // Say what the page does not show by itself. The neighbours keep their
    // rectangles, so the space a removed cell leaves stays empty; and the last
    // cell takes the layout with it, which must not be reported as a gap.
    const bool dissolved = removed.value().slotList.isEmpty();
    QString message;
    if (dissolved) {
        message = detached
            ? QCoreApplication::translate(
                  "PhotoPrint", "Az elrendezés megszűnt, a fotó a lapon marad.")
            : QCoreApplication::translate("PhotoPrint", "Az elrendezés megszűnt.");
    } else if (detached) {
        message = QCoreApplication::translate(
            "PhotoPrint", "A fotó szabadon marad a lapon, a többi hely a helyén marad.");
    } else {
        message = QCoreApplication::translate("PhotoPrint", "A többi hely a helyén marad.");
    }
    statusBar()->showMessage(message, 6000);
}

void MainWindow::dissolvePageLayout()
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    if (!page.hasLayout())
        return;
    const auto answer = QMessageBox::question(
        this, QCoreApplication::translate("PhotoPrint", "Elrendezés megszüntetése"),
        QCoreApplication::translate(
            "PhotoPrint", "Az elrendezés megszűnik, a fotók szabadon maradnak a helyükön."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    // The photographs stay exactly where they are and become free frames again.
    const auto published = m_controller.publishPage(
        SlotLayoutEngine::dissolve(page),
        QCoreApplication::translate("PhotoPrint", "Elrendezés megszüntetése"));
    if (!published)
        showProblem(published.error().message);
}

void MainWindow::applyFit(FitMode fit)
{
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    Frame updated = *frame;
    updated.fit = fit;
    updated.zoom = 1.0;
    updated.cropCenter = QPointF(0.5, 0.5);
    updated.autoPlaced = false;
    m_controller.apply(FrameChange{updated.id, updated});
}

void MainWindow::toggleCrop(bool enabled)
{
    if (enabled && !selectedFrame()) {
        m_cropButton->blockSignals(true);
        m_cropButton->setChecked(false);
        m_cropButton->blockSignals(false);
        return;
    }
    m_pageView->setCropMode(enabled);
    m_cropButton->setText(m_pageView->cropMode() ? QCoreApplication::translate("PhotoPrint", "Kész")
                                                 : QCoreApplication::translate("PhotoPrint", "Kivágás"));
    if (m_pageView->cropMode())
        m_pageView->setFocus();
}

void MainWindow::fitPage()
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    // A laid-out page already fits: the cells are derived from the paper, so
    // there is nothing to scale and scaling would break the slot invariant.
    if (document.pages.at(document.currentPage).hasLayout())
        return;
    m_controller.apply(FitPageEdit{document.currentPage});
}

bool MainWindow::pageOverflows() const
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return false;
    const Page &page = document.pages.at(document.currentPage);
    const QSizeF size = pageSizeMm(page);
    if (size.width() <= 0.0 || size.height() <= 0.0)
        return false;
    for (const Frame &frame : page.frames) {
        if (!LayoutEngine::validFrame(frame))
            continue;
        const double rad = qDegreesToRadians(frame.angleDeg);
        const double width =
            std::abs(frame.width * std::cos(rad)) + std::abs(frame.height * std::sin(rad));
        const double height =
            std::abs(frame.width * std::sin(rad)) + std::abs(frame.height * std::cos(rad));
        if (frame.cx - width / 2.0 < -0.05 || frame.cy - height / 2.0 < -0.05
            || frame.cx + width / 2.0 > size.width() + 0.05
            || frame.cy + height / 2.0 > size.height() + 0.05)
            return true;
    }
    return false;
}

void MainWindow::deleteSelected()
{
    if (!m_selectedId.isEmpty()) {
        m_controller.apply(RemoveFrameEdit{m_selectedId});
        return;
    }
    // No photograph selected but a cell is: clearing an occupied cell removes
    // its photo; an empty cell has nothing to clear and makes no edit.
    if (!m_selectedSlotId.isEmpty())
        clearSlot(m_selectedSlotId);
}

void MainWindow::applySizeFromControls()
{
    if (m_syncing)
        return;
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    // The spin boxes are disabled for a slotted frame; this guard keeps a
    // stray editingFinished from writing geometry the slot owns.
    if (!frame->slotId.isEmpty())
        return;
    Frame updated = *frame;
    updated.width = m_widthSpin->value() * 10.0;
    updated.height = m_heightSpin->value() * 10.0;
    updated.angleDeg = m_angleSpin->value();
    updated.autoPlaced = false;
    if (!LayoutEngine::validFrame(updated))
        return;
    m_controller.apply(FrameChange{updated.id, updated});
}

void MainWindow::paperChanged(int index)
{
    Q_UNUSED(index)
    if (m_restoring || m_syncing)
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    flushSlotStyle();
    m_paperName = currentPaper().name;
    m_controller.apply(SetProjectPaperEdit{currentPaper()});
    saveSettings();
    refreshPhotoList();
}

void MainWindow::qualityChanged(int index)
{
    if (m_restoring || m_syncing || index < 0)
        return;
    m_controller.apply(SetPrintQualityEdit{m_qualityCombo->itemData(index).toBool()});
}

void MainWindow::colorChanged(int index)
{
    if (m_restoring || m_syncing || index < 0)
        return;
    ColorRecipe recipe = m_controller.document().recipe;
    recipe.grayscale = m_colorCombo->itemData(index).toBool();
    m_controller.apply(SetRecipeEdit{recipe});
}

void MainWindow::syncPrintControls()
{
    if (!m_qualityCombo || !m_colorCombo)
        return;
    const DocumentSnapshot document = m_controller.document();
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    m_qualityCombo->setCurrentIndex(document.fineQuality ? 0 : 1);
    m_colorCombo->setCurrentIndex(document.recipe.grayscale ? 1 : 0);
    m_syncing = wasSyncing;
}

bool resolvedBorderless(const Page &page, bool printerBorderless)
{
    switch (page.borderlessOverride) {
    case BorderlessOverride::Off:
        return false;
    case BorderlessOverride::On:
        return true;
    case BorderlessOverride::Auto:
        break;
    }
    return printerBorderless;
}

bool MainWindow::wantBorderless(const Page &page) const
{
    const PaperSpec &paper = page.paper;
    bool printerBorderless = false;
    if (paper.isPhotoPaper() && isFixedPhotoSize(paper)
        && (!paper.custom || paper.borderlessSupported)) {
        const PrinterCapabilities *capabilities = printerByQueue(m_queue);
        printerBorderless = capabilities && capabilities->borderlessSupported();
    }
    // The page's own choice decides; the printer only supplies the automatic
    // answer. A batch sheet pins Off, so a 35 × 45 copy prints exactly 35 × 45
    // instead of the 1.6 % larger bleed composition.
    return resolvedBorderless(page, printerBorderless);
}

void MainWindow::manageCustomPapers()
{
    const bool persisted = !qEnvironmentVariableIsSet("PHOTOPRINT_NO_SETTINGS");
    QDialog dialog(this);
    dialog.setWindowTitle(QCoreApplication::translate("PhotoPrint", "Egyedi papír"));
    auto *layout = new QVBoxLayout(&dialog);

    auto *list = new QListWidget(&dialog);
    list->setMinimumHeight(110);
    layout->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Mentett egyedi papírok:"), &dialog));
    layout->addWidget(list);

    auto *form = new QGridLayout;
    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText(QCoreApplication::translate("PhotoPrint", "Például: Négyzetes 12 × 12"));
    auto *widthSpin = new QDoubleSpinBox(&dialog);
    auto *heightSpin = new QDoubleSpinBox(&dialog);
    for (QDoubleSpinBox *spin : {widthSpin, heightSpin}) {
        spin->setRange(5.0, 50.0); // 50-500 mm, 0.1 mm steps
        spin->setDecimals(2);
        spin->setSingleStep(0.1);
        spin->setSuffix(QStringLiteral(" cm"));
    }
    widthSpin->setValue(12.0);
    heightSpin->setValue(18.0);
    nameEdit->setText(QCoreApplication::translate("PhotoPrint", "%1 (egyedi)")
                          .arg(sizeLabelCm(120.0, 180.0)));
    auto *typeCombo = new QComboBox(&dialog);
    typeCombo->addItem(paperTypeLabel(PaperType::Glossy), int(PaperType::Glossy));
    typeCombo->addItem(paperTypeLabel(PaperType::Matte), int(PaperType::Matte));
    typeCombo->addItem(paperTypeLabel(PaperType::Plain), int(PaperType::Plain));
    auto *borderlessCheck = new QCheckBox(QCoreApplication::translate("PhotoPrint", "Szegély nélkül"), &dialog);
    auto *matteNote = new QLabel(
        QCoreApplication::translate("PhotoPrint", "A matt papírt a nyomtató tintasugaras papírként kezeli."), &dialog);
    matteNote->setWordWrap(true);
    matteNote->setVisible(false);

    form->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Név")), 0, 0);
    form->addWidget(nameEdit, 0, 1);
    form->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Szélesség")), 1, 0);
    form->addWidget(widthSpin, 1, 1);
    form->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Magasság")), 2, 0);
    form->addWidget(heightSpin, 2, 1);
    form->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Papír")), 3, 0);
    form->addWidget(typeCombo, 3, 1);
    layout->addLayout(form);
    layout->addWidget(matteNote);
    layout->addWidget(borderlessCheck);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QPushButton *addButton =
        buttons->addButton(QCoreApplication::translate("PhotoPrint", "Hozzáadás"), QDialogButtonBox::ActionRole);
    QPushButton *removeButton =
        buttons->addButton(QCoreApplication::translate("PhotoPrint", "Törlés"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);

    QList<PaperSpec> savedPapers =
        persisted ? m_customPaperStore.all() : QList<PaperSpec>{};
    auto refreshList = [list, &savedPapers] {
        list->clear();
        for (const PaperSpec &paper : savedPapers) {
            auto *item = new QListWidgetItem(
                QStringLiteral("%1 · %2").arg(paper.name, paperTypeLabel(paper.type)));
            list->addItem(item);
        }
    };
    refreshList();

    auto updateBorderless = [widthSpin, heightSpin, typeCombo, borderlessCheck] {
        PaperSpec probe;
        probe.physicalWidthMm = widthSpin->value() * 10.0;
        probe.physicalHeightMm = heightSpin->value() * 10.0;
        const bool photo = typeCombo->currentData().toInt() != int(PaperType::Plain);
        const bool allowed = photo && isFixedPhotoSize(probe);
        borderlessCheck->setEnabled(allowed);
        if (!allowed)
            borderlessCheck->setChecked(false);
    };
    connect(widthSpin, &QDoubleSpinBox::valueChanged, &dialog,
            [updateBorderless](double) { updateBorderless(); });
    connect(heightSpin, &QDoubleSpinBox::valueChanged, &dialog,
            [updateBorderless](double) { updateBorderless(); });
    connect(typeCombo, &QComboBox::currentIndexChanged, &dialog,
            [updateBorderless, matteNote, typeCombo](int) {
                matteNote->setVisible(typeCombo->currentData().toInt()
                                      == int(PaperType::Matte));
                updateBorderless();
            });
    updateBorderless();

    connect(list, &QListWidget::currentRowChanged, &dialog,
            [nameEdit, widthSpin, heightSpin, typeCombo, borderlessCheck, &savedPapers](int row) {
                if (row < 0 || row >= savedPapers.size())
                    return;
                const PaperSpec &paper = savedPapers.at(row);
                nameEdit->setText(paper.name);
                widthSpin->setValue(paper.physicalWidthMm / 10.0);
                heightSpin->setValue(paper.physicalHeightMm / 10.0);
                for (int i = 0; i < typeCombo->count(); ++i) {
                    if (typeCombo->itemData(i).toInt() == int(paper.type))
                        typeCombo->setCurrentIndex(i);
                }
                borderlessCheck->setChecked(paper.borderlessSupported
                                            && borderlessCheck->isEnabled());
            });

    connect(addButton, &QPushButton::clicked, &dialog, [&] {
        PaperSpec paper;
        paper.custom = true;
        paper.physicalWidthMm = widthSpin->value() * 10.0;
        paper.physicalHeightMm = heightSpin->value() * 10.0;
        paper.type = static_cast<PaperType>(typeCombo->currentData().toInt());
        paper.borderlessSupported = borderlessCheck->isEnabled() && borderlessCheck->isChecked();
        paper.name = nameEdit->text().trimmed();
        if (paper.name.isEmpty())
            paper.name = sizeLabelCm(paper.physicalWidthMm, paper.physicalHeightMm);
        for (const PaperSpec &builtin : defaultPapers()) {
            if (paper.name.compare(builtin.name, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(
                    &dialog, QStringLiteral("PhotoPrint"),
                    QCoreApplication::translate("PhotoPrint", "Ez a név már egy beépített papíré. Adj másik nevet."));
                return;
            }
        }
        if (!PrinterAdapter::customSizeInRange(paper.physicalWidthMm, paper.physicalHeightMm)) {
            QMessageBox::warning(
                &dialog, QStringLiteral("PhotoPrint"),
                QCoreApplication::translate(
                    "PhotoPrint", "Ez a papírméret nem nyomtatható. A nyomtató 8,9–21,6 × "
                                  "12,7–35,6 cm közötti egyedi méretet fogad el."));
            return;
        }
        if (persisted) {
            const auto saved = m_customPaperStore.save(paper);
            if (!saved) {
                QMessageBox::warning(&dialog, QStringLiteral("PhotoPrint"),
                                     saved.error().message);
                return;
            }
            savedPapers = m_customPaperStore.all();
        } else {
            savedPapers.append(paper);
        }
        refreshList();
        m_restoring = true;
        rebuildPaperCombo(paper.name);
        m_restoring = false;
        m_paperName = paper.name;
        if (!m_controller.document().pages.isEmpty())
            m_controller.apply(SetProjectPaperEdit{currentPaper()});
        saveSettings();
        refreshPhotoList();
    });

    connect(removeButton, &QPushButton::clicked, &dialog, [&] {
        const int row = list->currentRow();
        if (row < 0 || row >= savedPapers.size())
            return;
        const QString name = savedPapers.at(row).name;
        if (persisted) {
            const auto removed = m_customPaperStore.remove(name);
            if (!removed) {
                QMessageBox::warning(&dialog, QStringLiteral("PhotoPrint"),
                                     removed.error().message);
                return;
            }
            savedPapers = m_customPaperStore.all();
        } else {
            for (int i = savedPapers.size() - 1; i >= 0; --i) {
                if (savedPapers.at(i).name == name)
                    savedPapers.removeAt(i);
            }
        }
        refreshList();
        if (m_paperName == name)
            m_paperName = QStringLiteral("A4");
        m_restoring = true;
        rebuildPaperCombo(m_paperName);
        m_restoring = false;
        if (!m_controller.document().pages.isEmpty())
            m_controller.apply(SetProjectPaperEdit{currentPaper()});
        saveSettings();
        refreshPhotoList();
    });

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::orientationChanged(int index)
{
    if (m_restoring)
        return;
    const bool landscape = index == 1;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    flushSlotStyle();
    m_landscape = landscape;
    m_controller.apply(SetProjectLandscapeEdit{landscape});
    saveSettings();
}

void MainWindow::printerChanged(int index)
{
    if (m_restoring || index < 0)
        return;
    m_queue = m_printerCombo->itemData(index).toString();
    saveSettings();
    if (m_queue.isEmpty()) {
        m_printerStatus->clear();
        updateNoticeBar();
        return;
    }
    const PrinterCapabilities *capabilities = printerByQueue(m_queue);
    if (!capabilities) {
        m_printerStatus->setText(
            QCoreApplication::translate("PhotoPrint", "A nyomtató most nem érhető el. Ellenőrizd, hogy be van-e kapcsolva."));
        m_recheckButton->setVisible(true);
        updateNoticeBar();
        return;
    }
    if (!capabilities->online) {
        m_printerStatus->setText(
            QCoreApplication::translate("PhotoPrint", "A nyomtató most nem érhető el. Ellenőrizd, hogy be van-e kapcsolva."));
        m_recheckButton->setVisible(true);
    } else {
        m_printerStatus->clear();
        m_recheckButton->setVisible(false);
    }
    refreshPhotoList();
    updateNoticeBar();
}

void MainWindow::refreshPrinters()
{
    m_recheckButton->setVisible(false);
    m_printerStatus->clear();
    if (qEnvironmentVariableIsSet("PHOTOPRINT_NO_CUPS")) {
        m_printerCombo->blockSignals(true);
        m_printerCombo->clear();
        m_printerCombo->addItem(QCoreApplication::translate("PhotoPrint", "Nincs nyomtató"));
        m_printerCombo->setEnabled(false);
        m_printerCombo->blockSignals(false);
        return;
    }
    const auto discovered = m_catalog.refresh();
    if (!discovered) {
        m_printers.clear();
        m_printerCombo->blockSignals(true);
        m_printerCombo->clear();
        m_printerCombo->addItem(QCoreApplication::translate("PhotoPrint", "Nincs nyomtató"));
        m_printerCombo->setEnabled(false);
        m_printerCombo->blockSignals(false);
        m_printerStatus->setText(discovered.error().message);
        m_recheckButton->setVisible(true);
        return;
    }

    m_printers = discovered.value();
    auto rank = [](const PrinterCapabilities &capabilities) {
        int score = capabilities.driverIdentity == QLatin1String("brother-native") ? 3
            : capabilities.driverIdentity == QLatin1String("ipp-everywhere")    ? 2
                                                                               : 1;
        if (capabilities.online)
            score += 10;
        return score;
    };
    QList<const PrinterCapabilities *> shown;
    for (const PrinterCapabilities &capabilities : m_printers) {
        const PrinterCapabilities *existing = nullptr;
        for (const PrinterCapabilities *candidate : shown) {
            if (candidate->displayName == capabilities.displayName) {
                existing = candidate;
                break;
            }
        }
        if (!existing)
            shown.append(&capabilities);
        else if (rank(capabilities) > rank(*existing))
            shown.replace(shown.indexOf(existing), &capabilities);
    }

    m_printerCombo->blockSignals(true);
    m_printerCombo->clear();
    m_printerCombo->setEnabled(true);
    for (const PrinterCapabilities *capabilities : shown)
        m_printerCombo->addItem(capabilities->displayName, capabilities->queueName);
    int selected = -1;
    if (!m_queue.isEmpty()) {
        for (int i = 0; i < m_printerCombo->count(); ++i) {
            if (m_printerCombo->itemData(i).toString() == m_queue) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && m_queue.isEmpty() && m_printerCombo->count() > 0) {
        m_queue = m_printerCombo->itemData(0).toString();
        selected = 0;
    }
    m_printerCombo->setCurrentIndex(selected);
    m_printerCombo->blockSignals(false);

    if (!m_queue.isEmpty() && !printerByQueue(m_queue)) {
        // A remembered printer that disappeared is never swapped silently.
        m_printerStatus->setText(
            QCoreApplication::translate("PhotoPrint", "A nyomtató most nem érhető el. Ellenőrizd, hogy be van-e kapcsolva."));
        m_recheckButton->setVisible(true);
        updateNoticeBar();
    } else if (selected >= 0) {
        printerChanged(selected);
    }
}

void MainWindow::saveSettings()
{
    if (qEnvironmentVariableIsSet("PHOTOPRINT_NO_SETTINGS"))
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("printer/queue"), m_queue);
    settings.setValue(QStringLiteral("paper/name"), m_paperName);
    settings.setValue(QStringLiteral("page/landscape"), m_landscape);
}

void MainWindow::restoreSettings()
{
    m_restoring = true;
    if (qEnvironmentVariableIsSet("PHOTOPRINT_NO_SETTINGS")) {
        m_restoring = false;
        return;
    }
    QSettings settings;
    m_queue = settings.value(QStringLiteral("printer/queue")).toString();
    m_paperName = settings.value(QStringLiteral("paper/name"), QStringLiteral("A4")).toString();
    m_landscape = settings.value(QStringLiteral("page/landscape"), false).toBool();

    rebuildPaperCombo(m_paperName);
    m_orientationCombo->blockSignals(true);
    m_orientationCombo->setCurrentIndex(m_landscape ? 1 : 0);
    m_orientationCombo->blockSignals(false);
    m_restoring = false;
}

void MainWindow::savePdfCurrent()
{
    exportPdf(false);
}

void MainWindow::savePdfAll()
{
    exportPdf(true);
}

void MainWindow::pageSelected(int index)
{
    flushSlotStyle();
    m_selectedId.clear();
    m_selectedSlotId.clear();
    m_controller.setCurrentPage(index);
    const DocumentSnapshot document = m_controller.document();
    if (index >= 0 && index < document.pages.size()) {
        m_restoring = true;
        m_landscape = document.pages.at(index).landscape;
        m_orientationCombo->setCurrentIndex(m_landscape ? 1 : 0);
        m_restoring = false;
    }
}

void MainWindow::addPage()
{
    m_controller.apply(AddPageEdit{m_controller.document().currentPage});
    m_selectedId.clear();
    m_selectedSlotId.clear();
}

void MainWindow::duplicatePage()
{
    m_controller.apply(DuplicatePageEdit{m_controller.document().currentPage});
    m_selectedId.clear();
    m_selectedSlotId.clear();
}

void MainWindow::removePage()
{
    m_controller.apply(RemovePageEdit{m_controller.document().currentPage});
    m_selectedId.clear();
    m_selectedSlotId.clear();
}

void MainWindow::movePage(int delta)
{
    const int current = m_controller.document().currentPage;
    m_controller.apply(MovePageEdit{current, current + delta});
}

void MainWindow::reorderFrame(int delta)
{
    if (m_selectedId.isEmpty())
        return;
    m_controller.apply(ReorderFrameEdit{m_selectedId, delta});
}

void MainWindow::exportPdf(bool allPages)
{
    DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    if (!projectPrintGeometryConsistent(document)) {
        showProblem(QCoreApplication::translate("PhotoPrint", "A lapok papírja vagy tájolása eltér. Állítsd egységesre."));
        return;
    }
    if (!projectBorderlessConsistent(document)) {
        showProblem(QCoreApplication::translate("PhotoPrint", "A lapok szélig nyomtatása eltér. Állítsd egységesre."));
        return;
    }
    // The exported page numbers are the document's own, also for a single-page
    // export: page 3 stays page 3, it is not renumbered to 1 by the narrowing.
    QList<int> guardedPages;
    if (allPages) {
        guardedPages = pagesPrintingCutGuides(document);
    } else if (document.currentPage >= 0
               && document.currentPage < document.pages.size()
               && document.pages.at(document.currentPage).cutGuides == CutGuides::Print) {
        guardedPages = {document.currentPage + 1};
    }
    // A PDF is a print proof: printed guides reach it exactly as they reach
    // paper, so it asks the same question, naming the sheets in this file.
    if (!confirmCutGuides(document, guardedPages))
        return;
    if (!allPages) {
        const int index = document.currentPage;
        DocumentSnapshot single;
        single.recipe = document.recipe;
        single.fineQuality = document.fineQuality;
        single.pages.append(document.pages.at(index));
        single.currentPage = 0;
        document = single;
    }
    const QString path = QFileDialog::getSaveFileName(this, QCoreApplication::translate("PhotoPrint", "Mentés PDF-ként"),
                                                     QStringLiteral("photoprint.pdf"),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (path.isEmpty())
        return;
    RenderOptions options;
    options.verifySources = true;
    options.printPpi = document.fineQuality ? 600.0 : 300.0;
    if (!m_queue.isEmpty()) {
        const PrinterCapabilities *capabilities = printerByQueue(m_queue);
        if (!capabilities) {
            showProblem(QCoreApplication::translate(
                "PhotoPrint",
                "A nyomtató most nem érhető el. Ellenőrizd, hogy be van-e kapcsolva."));
            return;
        }
        const Page &page = document.pages.at(document.currentPage);
        PrintSelection selection;
        selection.queueName = m_queue;
        selection.paper = page.paper;
        selection.landscape = page.landscape;
        selection.borderless = wantBorderless(page);
        selection.fineQuality = document.fineQuality;
        selection.color = !document.recipe.grayscale;
        const auto ticket = PrinterAdapter::resolve(*capabilities, selection);
        if (!ticket) {
            showProblem(ticket.error().message);
            return;
        }
        options.mediaSizeMm = ticket.value().geometry.logicalSizeMm();
        options.contentOffsetMm = ticket.value().geometry.physicalOriginInLogicalMm;
        if (ticket.value().geometry.borderless) {
            // The exported PDF has to match what the print path sends, bleed
            // included, otherwise the paper proof differs from the print.
            const BorderlessPlacement placement = borderlessPlacement(
                ticket.value().geometry.physicalSizeMm(),
                ticket.value().geometry.logicalSizeMm(),
                ticket.value().geometry.physicalOriginInLogicalMm);
            options.contentScale = placement.scale;
            options.contentOffsetMm = placement.offsetMm;
        }
        const PrintContext context = PrinterAdapter::contextFor(ticket.value());
        if (const auto preset = m_presets.match(context))
            document.recipe.paperStrength = preset->paperEv;
    }
    Result<ExportArtifact> artifact = m_renderer.writePdf(document, path, {}, options);
    if (!artifact) {
        showProblem(artifact.error().message);
        return;
    }
    statusBar()->showMessage(QCoreApplication::translate("PhotoPrint", "A PDF elkészült: %1").arg(QFileInfo(path).fileName()),
                             6000);
}

void MainWindow::showDiagnostics()
{
    const DocumentSnapshot document = m_controller.document();
    QString text;
    if (!document.pages.isEmpty()) {
        const Page &page = document.pages.at(document.currentPage);
        const QSizeF size = pageSizeMm(page);
        text += QStringLiteral("Page %1/%2  %3 x %4 mm  landscape=%5\n")
                    .arg(document.currentPage + 1)
                    .arg(document.pages.size())
                    .arg(size.width(), 0, 'f', 2)
                    .arg(size.height(), 0, 'f', 2)
                    .arg(page.landscape ? QStringLiteral("yes") : QStringLiteral("no"));
        text += QStringLiteral("Frames: %1\n\n").arg(page.frames.size());
    }
    if (const std::optional<Frame> frame = selectedFrame()) {
        text += QStringLiteral("Frame %1\n").arg(frame->id);
        text += QStringLiteral("  centre  %1 / %2 mm\n")
                    .arg(frame->cx, 0, 'f', 3)
                    .arg(frame->cy, 0, 'f', 3);
        text += QStringLiteral("  size    %1 x %2 mm\n")
                    .arg(frame->width, 0, 'f', 3)
                    .arg(frame->height, 0, 'f', 3);
        text += QStringLiteral("  angle   %1 deg\n").arg(frame->angleDeg, 0, 'f', 2);
        text += QStringLiteral("  fit     %1  zoom %2\n")
                    .arg(frame->fit == FitMode::Full ? QStringLiteral("full")
                                                     : QStringLiteral("fill"))
                    .arg(frame->zoom, 0, 'f', 3);
        text += QStringLiteral("  crop    %1 / %2\n")
                    .arg(frame->cropCenter.x(), 0, 'f', 4)
                    .arg(frame->cropCenter.y(), 0, 'f', 4);
        const auto image = m_previewImages.constFind(frame->id);
        if (image != m_previewImages.constEnd()) {
            text += QStringLiteral("  effective ppi  %1\n")
                        .arg(LayoutEngine::effectivePpi(*frame, image->size()), 0, 'f', 1);
            text += QStringLiteral("  source pixels  %1 x %2\n")
                        .arg(image->width())
                        .arg(image->height());
        }
        if (const std::optional<SourceImage> source = m_store.cached(frame->sourcePath)) {
            text += QStringLiteral("  source  %1\n").arg(source->path);
            text += QStringLiteral("  sha256  %1\n").arg(QString::fromLatin1(source->sha256.toHex()));
            text += QStringLiteral("  embedded icc  %1  valid  %2  displayP3  %3\n")
                        .arg(source->hasEmbeddedProfile ? QStringLiteral("yes")
                                                        : QStringLiteral("no"))
                        .arg(source->profileValid ? QStringLiteral("yes") : QStringLiteral("no"))
                        .arg(source->displayP3 ? QStringLiteral("yes") : QStringLiteral("no"));
            text += QStringLiteral("  srgb fallback  %1  sdr  %2\n")
                        .arg(source->srgbFallback ? QStringLiteral("yes") : QStringLiteral("no"))
                        .arg(source->sdrAssumed ? QStringLiteral("yes") : QStringLiteral("no"));
        }
    } else {
        text += QStringLiteral("No frame selected.\n");
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QCoreApplication::translate("PhotoPrint", "Hibakeresési adatok"));
    dialog.resize(640, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *edit = new QPlainTextEdit(&dialog);
    edit->setReadOnly(true);
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    edit->setPlainText(text);
    layout->addWidget(edit);
    dialog.exec();
}

void MainWindow::applyFilterSettings()
{
    if (m_syncing)
        return;
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    Frame updated = *frame;
    updated.filterStrength = effectiveEffectStrength(frame->filter, m_filterStrength->value());
    updated.upscale = m_upscaleCheck->isChecked();
    if (updated.filterStrength == frame->filterStrength && updated.upscale == frame->upscale)
        return;
    m_controller.apply(FrameChange{updated.id, updated});
}

void MainWindow::toggleAutoTone(bool enabled)
{
    if (m_syncing)
        return;
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    Frame updated = *frame;
    updated.autoTone = enabled;
    if (enabled) {
        if (const std::optional<SourceImage> source = m_store.cached(frame->sourcePath)) {
            const QImage sample = ColorPipeline::toLinear(*source, ColorRecipe{}, 1000);
            updated.tone = ToneEngine::suggest(sample);
        }
    }
    m_controller.apply(FrameChange{updated.id, updated});
}

std::optional<Frame> MainWindow::liveSelectedFrame() const
{
    if (m_selectedId.isEmpty())
        return std::nullopt;
    if (m_pageView) {
        if (const std::optional<Frame> live = m_pageView->liveFrame(m_selectedId))
            return live;
    }
    return selectedFrame();
}

void MainWindow::showInSlotReadouts(const Frame &frame)
{
    if (m_slotZoomValue) {
        const int percent = int(std::lround(
            std::clamp(frame.zoom, kMinFrameZoom, kMaxFrameZoom) * 100.0));
        m_slotZoomValue->setText(QStringLiteral("%1%").arg(percent));
    }
    if (m_slotRotationValue) {
        const int degrees = int(std::lround(std::clamp(frame.imageRotationDeg,
                                                       kMinImageRotationDeg,
                                                       kMaxImageRotationDeg)));
        m_slotRotationValue->setText(QStringLiteral("%1°").arg(degrees));
    }
}

void MainWindow::syncSlotImageControls(const Frame &frame)
{
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    if (m_slotZoomSlider) {
        const int percent = int(std::lround(
            std::clamp(frame.zoom, kMinFrameZoom, kMaxFrameZoom) * 100.0));
        // The thumb under the pointer is the user's; the label carries the
        // effective value until the release aligns them.
        if (!m_slotZoomSlider->isSliderDown())
            m_slotZoomSlider->setValue(percent);
    }
    if (m_slotRotationSlider) {
        const int degrees = int(std::lround(
            std::clamp(frame.imageRotationDeg, kMinImageRotationDeg, kMaxImageRotationDeg)));
        if (!m_slotRotationSlider->isSliderDown())
            m_slotRotationSlider->setValue(degrees);
    }
    m_syncing = wasSyncing;
    showInSlotReadouts(frame);
}

void MainWindow::editSlotImage(const std::function<Frame(Frame, const QSize &)> &edit, bool live)
{
    if (m_syncing)
        return;
    const std::optional<Frame> frame = liveSelectedFrame();
    if (!frame || frame->slotId.isEmpty())
        return;
    const QSize pixels =
        m_previewImages.contains(frame->id) ? m_previewImages.value(frame->id).size() : QSize();
    const Frame updated = edit(*frame, pixels);

    if (live) {
        if (m_slotEditFrameId != frame->id) {
            // The drag takes over any uncommitted wheel burst instead of
            // letting its timer overwrite the newer state.
            if (m_pageView)
                m_pageView->takeOverWheelBurst(frame->id);
            m_slotEditFrameId = frame->id;
        }
        if (updated != *frame && m_pageView)
            m_pageView->previewFrame(updated);
        // The readout follows the effective frame, so a Fill cover minimum is
        // visible even when the thumb asked for a smaller zoom.
        syncSlotImageControls(updated);
        return;
    }

    if (m_slotEditFrameId == frame->id)
        m_slotEditFrameId.clear();
    if (updated != *frame)
        m_controller.apply(FrameChange{updated.id, updated});
    // A request the model or the Fill cover rule clamped snaps the thumb back
    // to what the frame actually holds.
    syncSlotImageControls(updated);
}

void MainWindow::slotZoomChanged(int percent)
{
    if (m_syncing)
        return;
    const bool live = m_slotZoomSlider && m_slotZoomSlider->isSliderDown();
    editSlotImage(
        [percent](Frame frame, const QSize &pixels) {
            return PhotoItem::setZoom(frame, percent / 100.0, pixels);
        },
        live);
}

void MainWindow::slotRotationChanged(int degrees)
{
    if (m_syncing)
        return;
    const bool live = m_slotRotationSlider && m_slotRotationSlider->isSliderDown();
    editSlotImage(
        [degrees](Frame frame, const QSize &pixels) {
            return PhotoItem::rotateImageTo(frame, degrees, pixels);
        },
        live);
}

void MainWindow::slotImageEditStarted()
{
    // A wheel burst that has not settled yet becomes its own edit before the
    // drag starts, so the undo stack keeps one step per gesture.
    if (m_pageView)
        m_pageView->flushWheelBurst();
    m_slotEditFrameId.clear();
}

void MainWindow::slotImageEditFinished()
{
    if (m_slotEditFrameId.isEmpty() || !m_pageView)
        return;
    const QString frameId = m_slotEditFrameId;
    m_slotEditFrameId.clear();
    const std::optional<Frame> live = m_pageView->liveFrame(frameId);
    if (!live)
        return;
    const DocumentSnapshot document = m_controller.document();
    const Frame *committed = findFrame(document, frameId);
    if (committed && *committed == *live)
        return;
    m_controller.apply(FrameChange{frameId, *live});
}

void MainWindow::rotateSlotImageBy(double degrees)
{
    editSlotImage(
        [degrees](Frame frame, const QSize &pixels) {
            return PhotoItem::rotateImageBy(frame, degrees, pixels);
        },
        false);
}

void MainWindow::resetSlotImage()
{
    editSlotImage(
        [](Frame frame, const QSize &) { return PhotoItem::resetInsideFrame(frame); }, false);
}

void MainWindow::brightnessChanged(int value)
{
    if (m_syncing)
        return;
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    Frame updated = *frame;
    updated.brightness = sliderToBrightnessEv(value);
    m_controller.apply(FrameChange{updated.id, updated});
}

void MainWindow::showOriginalPressed(bool pressed)
{
    m_showOriginal = pressed;
    if (pressed) {
        if (const std::optional<Frame> frame = selectedFrame()) {
            if (!m_originalPreviews.contains(sourceKeyFor(*frame)))
                startOriginalPreview(*frame);
        }
    }
    // A cached untouched layer swaps instantly. When it is not ready yet, the
    // current image stays and the busy overlay explains the wait; the worker
    // installs the original as soon as it exists.
    m_pageView->setFrameImages(displayedPreviewImages());
}

bool MainWindow::projectBorderlessConsistent(const DocumentSnapshot &document) const
{
    if (document.pages.isEmpty())
        return true;
    const bool first = wantBorderless(document.pages.first());
    for (const Page &page : document.pages) {
        if (wantBorderless(page) != first)
            return false;
    }
    return true;
}

bool MainWindow::projectPrintGeometryConsistent(const DocumentSnapshot &document) const
{
    if (document.pages.isEmpty())
        return true;
    const Page &first = document.pages.first();
    for (const Page &page : document.pages) {
        if (page.paper != first.paper || page.landscape != first.landscape)
            return false;
    }
    return true;
}

void MainWindow::printNow()
{
    if (m_printInProgress)
        return;
    if (!projectPrintGeometryConsistent(m_controller.document())) {
        showProblem(QCoreApplication::translate("PhotoPrint", "A lapok papírja vagy tájolása eltér. Állítsd egységesre."));
        return;
    }
    if (!projectBorderlessConsistent(m_controller.document())) {
        showProblem(QCoreApplication::translate("PhotoPrint", "A lapok szélig nyomtatása eltér. Állítsd egységesre."));
        return;
    }
    if (m_queue.isEmpty()) {
        showProblem(QCoreApplication::translate("PhotoPrint", "Válassz nyomtatót."));
        return;
    }
    DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const PrinterCapabilities *capabilities = printerByQueue(m_queue);
    if (!capabilities) {
        showProblem(QCoreApplication::translate(
            "PhotoPrint",
            "A nyomtató most nem érhető el. Ellenőrizd, hogy be van-e kapcsolva."));
        return;
    }
    const Page &page = document.pages.at(document.currentPage);
    PrintSelection selection;
    selection.queueName = m_queue;
    selection.paper = page.paper;
    selection.landscape = page.landscape;
    selection.borderless = wantBorderless(page);
    selection.fineQuality = document.fineQuality;
    selection.color = !document.recipe.grayscale;
    selection.copies = m_copiesSpin->value();
    const auto ticket = PrinterAdapter::resolve(*capabilities, selection);
    if (!ticket) {
        showProblem(ticket.error().message);
        return;
    }

    PrintTicket value = ticket.value();
    const PrintContext context = PrinterAdapter::contextFor(value);
    if (const auto preset = m_presets.match(context))
        document.recipe.paperStrength = preset->paperEv;
    // Ask before the spool is written and before CUPS sees anything: a No must
    // leave no trace, and the printed marks are the one thing a person may not
    // expect from a layout tool.
    if (!confirmCutGuides(document))
        return;
    submitPrint(document, value);
}

void MainWindow::submitPrint(const DocumentSnapshot &document, const PrintTicket &ticket)
{
    if (m_printInProgress) {
        showProblem(QCoreApplication::translate("PhotoPrint", "A nyomtatás már folyamatban van."));
        return;
    }
    m_printInProgress = true;
    m_printButton->setEnabled(false);
    statusBar()->showMessage(QCoreApplication::translate("PhotoPrint", "Nyomtatásra vár."));

    m_printWatcher = new QFutureWatcher<Result<JobResult>>(this);
    auto *watcher = m_printWatcher;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const auto result = watcher->result();
        m_printInProgress = false;
        m_printButton->setEnabled(true);
        if (!result) {
            showProblem(result.error().message);
        } else {
            const bool uncertain = result.value().status == SubmitStatus::Uncertain;
            statusBar()->showMessage(result.value().message, uncertain ? 0 : 15000);
        }
        m_printWatcher = nullptr;
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([this, document, ticket] {
        return m_jobController->start(document, ticket);
    }));
}

void MainWindow::showTrialSheet()
{
    if (!projectPrintGeometryConsistent(m_controller.document())) {
        showProblem(QCoreApplication::translate("PhotoPrint", "A lapok papírja vagy tájolása eltér. Állítsd egységesre."));
        return;
    }
    if (!projectBorderlessConsistent(m_controller.document())) {
        showProblem(QCoreApplication::translate("PhotoPrint", "A lapok szélig nyomtatása eltér. Állítsd egységesre."));
        return;
    }
    const std::optional<Frame> frame = selectedFrame();
    if (!frame) {
        showProblem(QCoreApplication::translate("PhotoPrint", "Válassz egy fotót a próbalaphoz."));
        return;
    }
    if (m_queue.isEmpty()) {
        showProblem(QCoreApplication::translate("PhotoPrint", "Válassz nyomtatót."));
        return;
    }
    const PrinterCapabilities *capabilities = printerByQueue(m_queue);
    if (!capabilities) {
        showProblem(QCoreApplication::translate(
            "PhotoPrint",
            "A nyomtató most nem érhető el. Ellenőrizd, hogy be van-e kapcsolva."));
        return;
    }
    const DocumentSnapshot document = m_controller.document();
    const Page &page = document.pages.at(document.currentPage);
    PrintSelection selection;
    selection.queueName = m_queue;
    selection.paper = page.paper;
    selection.landscape = page.landscape;
    selection.borderless = wantBorderless(page);
    selection.fineQuality = document.fineQuality;
    selection.color = !document.recipe.grayscale;
    const auto ticket = PrinterAdapter::resolve(*capabilities, selection);
    if (!ticket) {
        showProblem(ticket.error().message);
        return;
    }
    PrintContext context = PrinterAdapter::contextFor(ticket.value());
    context.inkIdentity = m_inkIdentity;
    const auto built = TrialSheetBuilder::build(document, frame->id, context);
    if (!built) {
        showProblem(built.error().message);
        return;
    }
    const TrialSheet sheet = built.value();

    ImageStore store;
    PageRenderer renderer(store);
    RenderOptions trialOptions;
    trialOptions.sourceMaxDimension = 1200;
    const auto rendered = renderer.renderPage(sheet.document, 0, 110.0, {}, trialOptions);
    if (!rendered) {
        showProblem(rendered.error().message);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QCoreApplication::translate("PhotoPrint", "Próbanyomat"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Válaszd ki, melyik tetszik legjobban."), &dialog));
    auto *image = new QLabel(&dialog);
    image->setPixmap(QPixmap::fromImage(rendered.value().scaledToWidth(
        640, Qt::SmoothTransformation)));
    layout->addWidget(image);
    auto *choice = new QComboBox(&dialog);
    for (const TrialVariant &variant : sheet.variants)
        choice->addItem(QCoreApplication::translate("PhotoPrint", "%1. változat").arg(variant.number), variant.paperEv);
    layout->addWidget(choice);

    auto *feedback = new QLabel(&dialog);
    feedback->setWordWrap(true);
    layout->addWidget(feedback);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QPushButton *printTrial = buttons->addButton(QCoreApplication::translate("PhotoPrint", "Próbanyomat nyomtatása"),
                                                 QDialogButtonBox::ActionRole);
    QPushButton *otherInk = buttons->addButton(QCoreApplication::translate("PhotoPrint", "Más tintát használok"),
                                               QDialogButtonBox::ActionRole);
    connect(printTrial, &QPushButton::clicked, &dialog, [this, &dialog, &feedback, &sheet, &ticket] {
        PrintTicket trialTicket = ticket.value();
        trialTicket.copies = 1;
        trialTicket.firstPage = 1;
        trialTicket.lastPage = 1;
        submitPrint(sheet.document, trialTicket);
        feedback->setText(QCoreApplication::translate(
            "PhotoPrint",
            "A próbalap elküldve. Nézd meg a nyomatot, majd válaszd ki, amelyik tetszik."));
    });
    connect(otherInk, &QPushButton::clicked, &dialog, [this, &context] {
        m_inkIdentity = PaperPresetStore::newInkIdentity();
        context.inkIdentity = m_inkIdentity;
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const double chosenEv = choice->currentData().toDouble();
    PaperPreset preset;
    preset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    preset.context = context;
    preset.paperEv = chosenEv;
    preset.label = choice->currentText();
    preset.savedAt = QDateTime::currentDateTimeUtc();
    const auto saved = m_presets.save(preset);
    if (!saved) {
        showProblem(saved.error().message);
        return;
    }
    ColorRecipe recipe = m_controller.document().recipe;
    recipe.paperStrength = chosenEv;
    m_controller.apply(SetRecipeEdit{recipe});
    statusBar()->showMessage(QCoreApplication::translate("PhotoPrint", "A papírbeállítás elmentve."), 5000);
}

void MainWindow::updatePrintButton()
{
    if (!m_printButton)
        return;
    const int pages = m_controller.pageCount();
    m_printButton->setText(pages > 1
                               ? QCoreApplication::translate("PhotoPrint", "%n oldal nyomtatása", nullptr, pages)
                               : QCoreApplication::translate("PhotoPrint", "Nyomtatás"));
}

Result<void> MainWindow::applySheetEdit(const ApplyLayoutEdit &edit)
{
    return m_controller.apply(edit);
}

void MainWindow::openBatchCopies()
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const std::optional<Frame> source = selectedFrame();
    if (!source) {
        showProblem(QCoreApplication::translate("PhotoPrint",
                                                "Válassz ki egy fotót a sokszorosításhoz."));
        return;
    }

    // A photo the preview pass could not decode must not produce blank
    // copies: the dialog disables Create and says why.
    const bool sourceReadable = source && !m_unavailableFrames.contains(source->id);
    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, document.currentPage,
                            source, m_previewImages, sourceReadable, this);
    if (dialog.exec() != QDialog::Accepted || !dialog.hasEdit())
        return;

    // Replacing the page's photographs is destructive in one step even though it
    // is undoable, so the count is named before anything changes. New pages
    // leave the current one alone and need no question.
    if (dialog.replacesCurrentPage()) {
        const int frames = document.pages.at(document.currentPage).frames.size();
        if (frames > 0) {
            const auto answer = QMessageBox::question(
                this, QCoreApplication::translate("PhotoPrint", "Fotók cseréje"),
                QCoreApplication::translate(
                    "PhotoPrint",
                    "A lapon most %n fotó van. A példányok ezek helyére kerülnek. Egy "
                    "lépésben visszavonható.",
                    nullptr, frames),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes)
                return;
        }
    }

    const auto applied = applySheetEdit(dialog.edit());
    if (!applied) {
        showProblem(applied.error().message);
        return;
    }
    // The source frame is gone after a replacement, so the selection would fall
    // back to nothing. The first copy is what the next action should act on.
    const DocumentSnapshot after = m_controller.document();
    if (!after.pages.isEmpty()) {
        const Page &page = after.pages.at(after.currentPage);
        if (!page.frames.isEmpty())
            selectFrame(page.frames.first().id, false);
    }
}

void MainWindow::openAlignPhotos()
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, document.currentPage,
                            std::nullopt, m_previewImages, true, this);
    if (dialog.exec() != QDialog::Accepted || !dialog.hasEdit())
        return;
    const auto applied = applySheetEdit(dialog.edit());
    if (!applied)
        showProblem(applied.error().message);
}

void MainWindow::guidesChanged(int index)
{
    if (m_syncing || index < 0)
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const CutGuides guides =
        static_cast<CutGuides>(m_guidesCombo->itemData(index).toInt());
    if (document.pages.at(document.currentPage).cutGuides == guides)
        return;

    ApplyLayoutEdit edit;
    PageLayout change;
    change.pageIndex = document.currentPage;
    change.cutGuides = guides;
    edit.pages.append(change);
    edit.selectPage = document.currentPage;
    edit.actionText = QCoreApplication::translate("PhotoPrint", "Vágójelek");
    const auto applied = applySheetEdit(edit);
    if (!applied) {
        showProblem(applied.error().message);
        syncPageTools();
    }
}

void MainWindow::borderlessChanged(int index)
{
    if (m_syncing || index < 0)
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const BorderlessOverride choice =
        static_cast<BorderlessOverride>(m_borderlessCombo->itemData(index).toInt());
    const Page &page = document.pages.at(document.currentPage);
    if (page.borderlessOverride == choice)
        return;

    ApplyLayoutEdit edit;
    PageLayout change;
    change.pageIndex = document.currentPage;
    change.borderlessOverride = choice;
    edit.pages.append(change);
    edit.selectPage = document.currentPage;
    edit.actionText = QCoreApplication::translate("PhotoPrint", "Szélig nyomtatás");
    const auto applied = applySheetEdit(edit);
    if (!applied) {
        showProblem(applied.error().message);
        syncPageTools();
    }
}

bool MainWindow::confirmCutGuides()
{
    return confirmCutGuides(m_controller.document());
}

bool MainWindow::confirmCutGuides(const DocumentSnapshot &document)
{
    return confirmCutGuides(document, pagesPrintingCutGuides(document));
}

bool MainWindow::confirmCutGuides(const DocumentSnapshot &document,
                                  const QList<int> &pageNumbers)
{
    if (!requiresCutGuideConfirmation(document) || pageNumbers.isEmpty())
        return true;
    QStringList numbers;
    for (const int page : pageNumbers)
        numbers << QString::number(page);
    const auto answer = QMessageBox::question(
        this, QCoreApplication::translate("PhotoPrint", "Vágójelek a nyomaton"),
        QCoreApplication::translate(
            "PhotoPrint",
            "A nyomatra vágójelek kerülnek a következő lapokon: %1. Folytatod?")
            .arg(numbers.join(QStringLiteral(", "))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer == QMessageBox::Yes;
}

void MainWindow::updateNoticeBar()
{
    if (!m_noticeBar)
        return;
    const bool printer = m_printerStatus && !m_printerStatus->text().isEmpty();
    const bool paper = m_paperHint && !m_paperHint->text().isEmpty();
    // The overflow state is the page's own, not the label's effective
    // visibility: a child of a hidden bar reports itself hidden, which would
    // keep the bar hidden and the warning could never appear.
    const bool overflow = pageOverflows();
    m_noticeBar->setVisible(printer || paper || overflow);
}

void MainWindow::applyTheme()
{
    const ThemeColors colors = themeColors();
    if (qApp)
        qApp->setPalette(themePalette(colors));
    setStyleSheet(themedStyleSheet(colors));
    if (m_pageView)
        m_pageView->setThemeColors(colors.canvas, colors.pageBorder, colors.shadow);
}

void MainWindow::updateTitle()
{
    QString title = QStringLiteral("PhotoPrint");
    if (!m_projectPath.isEmpty()) {
        const QString projectName = QFileInfo(m_projectPath).completeBaseName();
        if (!projectName.isEmpty()
            && projectName.compare(QStringLiteral("PhotoPrint"), Qt::CaseInsensitive) != 0)
            title += QStringLiteral(" — ") + projectName;
    }
    if (m_dirty)
        title += QStringLiteral(" *");
    setWindowTitle(title);
}

void MainWindow::saveProject()
{
    if (m_projectPath.isEmpty()) {
        saveProjectAs();
        return;
    }
    ProjectStore store;
    const auto saved = store.save(m_projectPath, m_controller.document(), SaveMode::Normal);
    if (!saved) {
        showProblem(saved.error().message);
        return;
    }
    m_dirty = false;
    if (const auto cleared = m_recovery.clear(); !cleared) {
        // The project is saved, but the recovery copy could not be removed, so
        // it may be offered again on the next start. That failure is the one
        // the user has to see: the success message would overwrite it on the
        // status bar before it could be read.
        showProblem(cleared.error().message);
        updateTitle();
        return;
    }
    updateTitle();
    statusBar()->showMessage(QCoreApplication::translate("PhotoPrint", "A projekt elmentve."), 4000);
}

void MainWindow::saveProjectAs()
{
    QString path = QFileDialog::getSaveFileName(this, QCoreApplication::translate("PhotoPrint", "Projekt mentése"),
                                                QStringLiteral("photoprint.ppj"),
                                                QCoreApplication::translate("PhotoPrint", "PhotoPrint projekt (*.ppj)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".ppj"), Qt::CaseInsensitive))
        path += QStringLiteral(".ppj");
    m_projectPath = path;
    saveProject();
}

void MainWindow::savePortableCopy()
{
    QString path = QFileDialog::getSaveFileName(this, QCoreApplication::translate("PhotoPrint", "Hordozható másolat"),
                                                QStringLiteral("photoprint.ppj"),
                                                QCoreApplication::translate("PhotoPrint", "PhotoPrint projekt (*.ppj)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".ppj"), Qt::CaseInsensitive))
        path += QStringLiteral(".ppj");
    ProjectStore store;
    const auto saved = store.save(path, m_controller.document(), SaveMode::Portable);
    if (!saved) {
        showProblem(saved.error().message);
        return;
    }
    statusBar()->showMessage(QCoreApplication::translate("PhotoPrint", "A hordozható másolat elkészült."), 5000);
}

void MainWindow::openProject()
{
    if (m_dirty) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("PhotoPrint"),
            QCoreApplication::translate("PhotoPrint", "A jelenlegi munka nincs elmentve. Megnyitod a másikat?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return;
        if (answer == QMessageBox::Save) {
            saveProject();
            if (m_dirty)
                return;
        }
    }
    const QString path = QFileDialog::getOpenFileName(this, QCoreApplication::translate("PhotoPrint", "Projekt megnyitása"),
                                                      QString(),
                                                      QCoreApplication::translate("PhotoPrint", "PhotoPrint projekt (*.ppj)"));
    if (path.isEmpty())
        return;
    ProjectStore store;
    const auto loaded = store.load(path);
    if (!loaded) {
        showProblem(loaded.error().message);
        return;
    }
    m_projectPath = path;
    applyLoadedProject(loaded.value());
}

void MainWindow::applyLoadedProject(const LoadedProject &loaded, bool recovery)
{
    m_restoringProject = true;
    m_controller.setDocument(loaded.document);
    m_restoringProject = false;
    m_sourceIssues = loaded.issues;
    m_selectedId.clear();
    m_selectedSlotId.clear();
    m_pageView->setSelectedFrame(QString());
    m_pageView->setSelectedSlot(QString());
    if (!loaded.document.pages.isEmpty()) {
        const Page &page = loaded.document.pages.first();
        m_restoring = true;
        m_paperName = page.paper.name;
        ensurePaperInCombo(page.paper);
        m_landscape = page.landscape;
        m_orientationCombo->setCurrentIndex(m_landscape ? 1 : 0);
        m_restoring = false;
    }
    m_recoveryEnabled = !qEnvironmentVariableIsSet("PHOTOPRINT_NO_RECOVERY");
    if (recovery) {
        m_projectPath.clear();
        m_dirty = true;
        m_checkpointTimer->start();
    } else {
        m_dirty = false;
        if (const auto cleared = m_recovery.clear(); !cleared)
            showProblem(cleared.error().message);
    }
    updateTitle();
    promptMissingSources();
}

void MainWindow::promptMissingSources()
{
    // Source issues were computed while loading; re-read them from a fresh load
    // would lose the current document, so the frames themselves are checked.
    DocumentSnapshot document = m_controller.document();
    for (int p = 0; p < document.pages.size(); ++p) {
        for (int f = 0; f < document.pages.at(p).frames.size(); ++f) {
            const Frame frame = document.pages.at(p).frames.at(f);
            if (frame.sourcePath.isEmpty())
                continue;
            const bool exists = QFileInfo::exists(frame.sourcePath);
            const QByteArray current = exists ? fileSha256(frame.sourcePath) : QByteArray();
            const bool changed = exists && !frame.sourceSha256.isEmpty()
                && current != frame.sourceSha256;
            if (exists && !changed)
                continue;

            QMessageBox box(this);
            box.setWindowTitle(QStringLiteral("PhotoPrint"));
            box.setText(exists
                            ? QCoreApplication::translate("PhotoPrint",
                                                          "Ez a fotó megváltozott. Add meg újra, "
                                                          "ha ezt szeretnéd nyomtatni.")
                            : QCoreApplication::translate("PhotoPrint", "Ezt a fotót nem találjuk."));
            QPushButton *find = box.addButton(QCoreApplication::translate("PhotoPrint", "Fotó megkeresése"),
                                              QMessageBox::ActionRole);
            box.addButton(QCoreApplication::translate("PhotoPrint", "Kihagyás"), QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() != find)
                continue;

            const QString replacement = QFileDialog::getOpenFileName(
                this, QCoreApplication::translate("PhotoPrint", "Fotó megkeresése"), QFileInfo(frame.sourcePath).absolutePath(),
                QCoreApplication::translate("PhotoPrint", "Fotók (*.jpg *.jpeg *.png *.JPG *.JPEG *.PNG)"));
            if (replacement.isEmpty())
                continue;
            const auto loaded = m_store.load(replacement, ImportPolicy{});
            if (!loaded) {
                showProblem(loaded.error().message);
                continue;
            }
            QList<Frame> frames = document.pages.at(p).frames;
            frames[f].sourcePath = loaded.value().path;
            frames[f].sourceSha256 = loaded.value().sha256;
            m_controller.apply(SetFramesEdit{p, frames});
            document = m_controller.document();
        }
    }

    // A blocked project-relative source has no path the frame can point at, so
    // the loop above never sees it. Offer the same relink dialog starting next
    // to the cleaned stored path, which is only used as a directory hint.
    const QList<SourceIssue> issues = m_sourceIssues;
    for (const SourceIssue &issue : issues) {
        if (!issue.blocked || issue.refusedPath.isEmpty())
            continue;
        int pageIndex = -1;
        const Frame *frame = findFrame(document, issue.frameId, &pageIndex);
        if (!frame || !frame->sourcePath.isEmpty() || pageIndex < 0)
            continue;

        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("PhotoPrint"));
        box.setText(QCoreApplication::translate("PhotoPrint", "Ezt a fotót nem találjuk."));
        QPushButton *find = box.addButton(QCoreApplication::translate("PhotoPrint", "Fotó megkeresése"),
                                          QMessageBox::ActionRole);
        box.addButton(QCoreApplication::translate("PhotoPrint", "Kihagyás"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != find)
            continue;

        const QString replacement = QFileDialog::getOpenFileName(
            this, QCoreApplication::translate("PhotoPrint", "Fotó megkeresése"),
            QFileInfo(issue.refusedPath).absolutePath(),
            QCoreApplication::translate("PhotoPrint", "Fotók (*.jpg *.jpeg *.png *.JPG *.JPEG *.PNG)"));
        if (replacement.isEmpty())
            continue;
        const auto loaded = m_store.load(replacement, ImportPolicy{});
        if (!loaded) {
            showProblem(loaded.error().message);
            continue;
        }
        QList<Frame> frames = document.pages.at(pageIndex).frames;
        int frameIndex = -1;
        for (int i = 0; i < frames.size(); ++i) {
            if (frames.at(i).id == issue.frameId) {
                frameIndex = i;
                break;
            }
        }
        if (frameIndex < 0)
            continue;
        frames[frameIndex].sourcePath = loaded.value().path;
        frames[frameIndex].sourceSha256 = loaded.value().sha256;
        m_controller.apply(SetFramesEdit{pageIndex, frames});
        document = m_controller.document();
    }
}

void MainWindow::checkpointNow()
{
    if (!m_recoveryEnabled || !m_dirty)
        return;
    const auto saved = m_recovery.checkpoint(m_controller.document());
    Q_UNUSED(saved)
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_dirty) {
        event->accept();
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("PhotoPrint"),
        QCoreApplication::translate("PhotoPrint", "Mentetted a munkát kilépés előtt?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) {
        event->ignore();
        return;
    }
    if (answer == QMessageBox::Save) {
        saveProject();
        if (m_dirty) {
            event->ignore();
            return;
        }
    } else if (const auto cleared = m_recovery.clear(); !cleared) {
        qWarning() << "recovery cleanup failed:" << cleared.error().message;
    }
    event->accept();
}

void MainWindow::showProblem(const QString &message)
{
    statusBar()->showMessage(message, 8000);
}

} // namespace pp
