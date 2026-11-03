#include "MainWindow.h"

#include "AboutDialog.h"
#include "BackgroundGeometry.h"
#include "CollageTemplates.h"
#include "ColorPipeline.h"
#include "EffectBrowserDialog.h"
#include "LayoutEngine.h"
#include "OverlayGeometry.h"
#include "PhotoItem.h"
#include "JobJournal.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"
#include "SheetLayout.h"
#include "SheetToolsDialog.h"
#include "StickerAssets.h"
#include "TemplateBrowserDialog.h"
#include "Translation.h"
#include "TrialSheetBuilder.h"
#include "ToneEngine.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QColorDialog>
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
#include <QScrollBar>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
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
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QThread>
#include <QToolBar>
#include <QToolButton>
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
// A layout style edit follows the spin live and lands as one edit once the user
// stops: short enough to feel immediate, long enough that a held arrow is one
// undo step.
constexpr int kStyleDebounceMs = 180;
// The preview's background layer is bounded like a photograph's: the canvas is
// at most a couple of thousand pixels wide, and a 100 MP source would otherwise
// be converted in full for every document change.
constexpr int kBackgroundPreviewBound = 1600;
// A sticker is drawn at a few centimetres, so the preview bound follows its
// drawn size at about print resolution and never exceeds this.
constexpr int kStickerPreviewBound = 1600;
// Choosing "Szín" from a white page would otherwise look like nothing happened.
// The swatch starts on a soft blue-grey, which is visible on the paper and is
// one click away from white if the user wants it.
constexpr QRgb kNeutralBackgroundSolid = 0xffcfd8e3u;
// A raster is kept while it is between 0.98x and 2x the view target: a slightly
// larger raster is free and sharper than rebuilding a smaller one, and a 2x
// surplus is still worth keeping. Outside that band the settle rebuilds.
constexpr double kRasterMinFactor = 0.98;
constexpr double kRasterMaxFactor = 2.0;

/// A 24 x 24 colour swatch for the background buttons. The colour is the label,
/// so the button carries no text; its accessible name says what it changes.
QIcon swatchIcon(QRgb colour)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
    painter.setBrush(QColor(colour));
    painter.drawRoundedRect(QRectF(0.5, 0.5, 23.0, 23.0), 4.0, 4.0);
    painter.end();
    return QIcon(pixmap);
}

/// Draws a ring that stays visible on white paper and on a dark or busy
/// background: a light underlay and the coloured line on top. The underlay is
/// invisible on white and provides the contrast on a dark page, so the same
/// furniture works on every background without a luminance guess.
void drawTwoToneRing(QPainter &painter, const QRectF &rect, const QColor &colour,
                     double radius, double width, Qt::PenStyle style,
                     const QList<qreal> &dash = {})
{
    QPen underlay(QColor(255, 255, 255, 220), width + 2.0, style);
    if (!dash.isEmpty())
        underlay.setDashPattern(dash);
    painter.setPen(underlay);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect, radius, radius);

    QPen inner(colour, width, style);
    if (!dash.isEmpty())
        inner.setDashPattern(dash);
    painter.setPen(inner);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect, radius, radius);
}

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

/// The same sticker list with one entry replaced by id. A missing id leaves the
/// list untouched, so a commit that outlived its sticker is a no-op instead of an
/// accidental append.
QList<Sticker> withSticker(const QList<Sticker> &stickers, const Sticker &sticker)
{
    QList<Sticker> updated = stickers;
    for (Sticker &entry : updated) {
        if (entry.id == sticker.id) {
            entry = sticker;
            break;
        }
    }
    return updated;
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
    QColor disabledText;
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
        // A disabled tab still has to be readable: about 3.9:1 on the panel,
        // where the old border colour sat at 1.4:1.
        colors.disabledText = QColor(0x8e, 0x8e, 0x93);
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
        // About 3.4:1 on the white panel, so a disabled tab is muted but still
        // legible. The old border colour was 1.5:1.
        colors.disabledText = QColor(0x8a, 0x8a, 0x8f);
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
QTabWidget#inspectorTabs::pane { border: none; }
QTabWidget#inspectorTabs QTabBar::tab { background: transparent; color: @secondary; padding: 5px 9px; margin-right: 2px; border: none; border-bottom: 2px solid transparent; }
QTabWidget#inspectorTabs QTabBar::tab:selected { color: @accent; border-bottom: 2px solid @accent; font-weight: 600; }
QTabWidget#inspectorTabs QTabBar::tab:hover:!selected { color: @text; }
QTabWidget#inspectorTabs QTabBar::tab:disabled { color: @disabled; }
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
    replace("@disabled", c.disabledText);
    replace("@border", c.border);
    replace("@input", c.input);
    replace("@hover", c.hover);
    replace("@accent", c.accent);
    replace("@selected", c.selected);
    return qss;
}

/// The inspector's tab strip. The stylesheet owns the tab painting, so the
/// platform style's own focus rectangle never reaches the screen. This bar
/// draws one around the open tab instead, in the palette's highlight colour, so
/// a keyboard user can always see where the focus is.
class InspectorTabBar : public QTabBar
{
public:
    explicit InspectorTabBar(QWidget *parent = nullptr) : QTabBar(parent) {}

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QTabBar::paintEvent(event);
        if (!hasFocus() || currentIndex() < 0)
            return;
        const QRect tab = tabRect(currentIndex());
        if (tab.isEmpty())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(palette().color(QPalette::Highlight));
        pen.setWidth(2);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(tab).adjusted(2.5, 2.5, -2.5, -2.5), 4.0, 4.0);
    }
};

/// A tab widget that installs the painting tab bar. QTabWidget::setTabBar is
/// protected, so the strip is installed from a subclass instead of from the
/// window.
class InspectorTabs : public QTabWidget
{
public:
    explicit InspectorTabs(QWidget *parent = nullptr) : QTabWidget(parent)
    {
        setTabBar(new InspectorTabBar(this));
    }
};

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

    // An arrow-key nudge on a sticker is one gesture too: the overlay follows
    // every key and a single edit lands when the keys stop.
    m_stickerNudgeTimer = new QTimer(this);
    m_stickerNudgeTimer->setSingleShot(true);
    m_stickerNudgeTimer->setInterval(kEditPulseMs);
    connect(m_stickerNudgeTimer, &QTimer::timeout, this, [this] { flushStickerNudge(); });

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
        // A sticker that left the document (an undo, a page switch, another
        // page's edit) cannot stay selected: the Elem tab falls back to its
        // empty state instead of showing a layer that is not there.
        if (!m_selectedStickerId.isEmpty()) {
            bool found = false;
            if (page) {
                for (const Sticker &sticker : page->stickers) {
                    if (sticker.id == m_selectedStickerId) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found)
                m_selectedStickerId.clear();
        }
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
        const QString stickerBefore = m_selectedStickerId;
        pruneSelection();
        updateAccessibleDescription();
        if (frameBefore != m_selectedId || slotBefore != m_selectedSlotId
            || stickerBefore != m_selectedStickerId)
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
    // The same for a sticker nudge: the new document already carries whatever
    // position it has, so a pending commit would fight it.
    if (!m_nudgeStickerId.isEmpty()) {
        m_nudgeStickerId.clear();
        if (m_stickerNudgeTimer)
            m_stickerNudgeTimer->stop();
    }

    pruneSelection();
    updateAccessibleDescription();
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

void PageView::setPageImages(const PageImages &images)
{
    // ImageStore hands out shared copies, so an unchanged set compares cheaply
    // and must not throw the composed page away.
    if (images == m_images)
        return;
    m_images = images;
    contentChanged(true);
}

void PageView::setFrameImages(const QHash<QString, QImage> &images)
{
    PageImages next = m_images;
    next.frames = images;
    setPageImages(next);
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
            if (drag.kind == DragKind::MoveSticker || drag.kind == DragKind::ResizeSticker
                || drag.kind == DragKind::RotateSticker) {
                const std::optional<Sticker> sticker = liveSticker(drag.startSticker.id);
                if (sticker && onStickerCommitted) {
                    const StickerAction action = drag.kind == DragKind::ResizeSticker
                        ? StickerAction::Resize
                        : (drag.kind == DragKind::RotateSticker ? StickerAction::Rotate
                                                                : StickerAction::Move);
                    onStickerCommitted(*sticker, action);
                }
            } else if (const Frame *frame = selectedFramePtr(); frame && onFrameCommitted) {
                onFrameCommitted(*frame);
            }
        } else if (drag.kind == DragKind::MoveSticker || drag.kind == DragKind::ResizeSticker
                   || drag.kind == DragKind::RotateSticker) {
            // The widget is going away: leave the document as the user had it.
            updateLiveSticker(drag.startSticker, false);
        } else if (Frame *frame = findFrame(m_document, drag.startFrame.id)) {
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
    updateAccessibleDescription();
    update();
}

void PageView::setSelectedSlot(const QString &slotId)
{
    if (m_selectedSlotId == slotId)
        return;
    m_selectedSlotId = slotId;
    updateAccessibleDescription();
    update();
}

void PageView::setSelectedSticker(const QString &stickerId)
{
    if (m_selectedStickerId == stickerId)
        return;
    m_selectedStickerId = stickerId;
    updateAccessibleDescription();
    update();
}

QPolygonF PageView::stickerQuad(const QString &stickerId) const
{
    if (stickerId.isEmpty())
        return {};
    const Page *page = currentPage();
    if (!page)
        return {};
    for (const Sticker &sticker : page->stickers) {
        if (sticker.id == stickerId)
            return stickerQuadWidgetFor(sticker);
    }
    return {};
}

void PageView::previewSticker(const Sticker &sticker)
{
    const Sticker *selected = selectedStickerPtr();
    if (!selected || selected->id != sticker.id)
        return;
    updateLiveSticker(sticker, false);
}

std::optional<Sticker> PageView::liveSticker(const QString &stickerId) const
{
    const Page *page = currentPage();
    if (!page || stickerId.isEmpty())
        return std::nullopt;
    for (const Sticker &sticker : page->stickers) {
        if (sticker.id == stickerId)
            return sticker;
    }
    return std::nullopt;
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

void PageView::setMissingStickers(const QSet<QString> &stickerIds)
{
    if (stickerIds == m_missingStickers)
        return;
    m_missingStickers = stickerIds;
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
    if (m_images.frames.contains(frame->id) && !m_images.frames.value(frame->id).isNull())
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

const Sticker *PageView::selectedStickerPtr() const
{
    const Page *page = currentPage();
    if (!page || m_selectedStickerId.isEmpty())
        return nullptr;
    for (const Sticker &sticker : page->stickers) {
        if (sticker.id == m_selectedStickerId)
            return &sticker;
    }
    return nullptr;
}

QPolygonF PageView::stickerQuadWidgetFor(const Sticker &sticker) const
{
    return pageToWidget().map(OverlayGeometry::stickerQuadMm(sticker));
}

int PageView::hitSticker(const QPointF &widgetPoint) const
{
    const Page *page = currentPage();
    if (!page || page->stickers.isEmpty())
        return -1;
    // The model owns the rule (the topmost rotated box wins); the view only
    // converts the pointer to page millimetres.
    const Sticker *hit = OverlayGeometry::stickerAt(*page, widgetToPage(widgetPoint));
    if (!hit)
        return -1;
    for (int i = 0; i < page->stickers.size(); ++i) {
        if (page->stickers.at(i).id == hit->id)
            return i;
    }
    return -1;
}

int PageView::hitStickerHandle(const QPointF &widgetPoint) const
{
    const Sticker *sticker = selectedStickerPtr();
    if (!sticker)
        return -1;
    const QPolygonF quad = stickerQuadWidgetFor(*sticker);
    for (int i = 0; i < 4; ++i) {
        if (QLineF(quad.at(i), widgetPoint).length() <= kHandleHalfPx + 4.0)
            return i;
    }
    const QPointF center = pageToWidget().map(QPointF(sticker->cx, sticker->cy));
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

QPolygonF PageView::frameQuadWidget(const Frame &frame) const
{
    return pageToWidget().map(PhotoItem::frameQuadMm(frame));
}

int PageView::hitFrame(const QPointF &widgetPoint) const
{
    if (m_document.pages.isEmpty())
        return -1;
    const Page &page = m_document.pages.at(m_document.currentPage);
    const QPointF pagePoint = widgetToPage(widgetPoint);
    for (int i = page.frames.size() - 1; i >= 0; --i) {
        const Frame &frame = page.frames.at(i);
        if (!frameQuadWidget(frame).containsPoint(widgetPoint, Qt::OddEvenFill))
            continue;
        // A slotted photograph is only visible inside its cell, so a click on
        // the clipped-away part of a larger frame belongs to whatever is under
        // it, not to a photograph the user cannot see. A dangling slot id is
        // drawn as a free frame by the renderer, so it is hit as one too.
        if (!frame.slotId.isEmpty()) {
            const Slot *slot = SlotGeometry::slotById(page, frame.slotId);
            if (slot && !SlotGeometry::slotCellMm(page, *slot).contains(pagePoint))
                continue;
        }
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
    // A slotted frame keeps the original handles: the slot clips the photograph,
    // it does not forbid resizing or turning it inside the cell.
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
    const auto it = m_images.frames.constFind(frame.id);
    m_drag.pixels = it == m_images.frames.constEnd() ? QSize() : it->size();
    setCursor(kind == DragKind::CropPan ? Qt::ClosedHandCursor : Qt::ArrowCursor);
}

void PageView::beginStickerDrag(DragKind kind, int handle, const QPointF &pagePoint,
                                const Sticker &sticker)
{
    m_drag = DragState{};
    m_drag.kind = kind;
    m_drag.handle = handle;
    m_drag.startPage = pagePoint;
    m_drag.startSticker = sticker;
    m_drag.changed = false;
    m_cropZoomPulse.invalidate();
    m_nudgePulse.invalidate();
    setCursor(Qt::ArrowCursor);
}

void PageView::updateLiveSticker(const Sticker &sticker, bool markDragChanged)
{
    if (m_document.pages.isEmpty())
        return;
    const int index = std::clamp(m_document.currentPage, 0, int(m_document.pages.size()) - 1);
    Page *page = &m_document.pages[index];
    bool found = false;
    for (Sticker &target : page->stickers) {
        if (target.id == sticker.id) {
            target = sticker;
            found = true;
            break;
        }
    }
    if (!found)
        return;
    if (markDragChanged)
        m_drag.changed = true;
    // One composition behind the pointer, like a frame drag: the painter
    // follows without starving every build.
    contentChanged(false);
}

Sticker PageView::draggedSticker(const QPointF &pagePoint, bool fine) const
{
    const Sticker start = m_drag.startSticker;
    const QSizeF sheet = pageMm();
    switch (m_drag.kind) {
    case DragKind::MoveSticker:
        return OverlayGeometry::movedSticker(start, pagePoint - m_drag.startPage, sheet);
    case DragKind::ResizeSticker:
        return OverlayGeometry::resizedSticker(start, pagePoint, sheet);
    case DragKind::RotateSticker:
        // Shift snaps to 15 degrees, exactly like a frame's rotate handle.
        return OverlayGeometry::rotatedSticker(start, pagePoint, fine);
    default:
        break;
    }
    return start;
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
    updateAccessibleDescription();
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

void PageView::flushWheelBurst()
{
    flushWheelZoom();
}

void PageView::flushStickerBurst()
{
    flushStickerNudge();
}

void PageView::flushStickerNudge()
{
    if (m_stickerNudgeTimer)
        m_stickerNudgeTimer->stop();
    if (m_nudgeStickerId.isEmpty())
        return;
    const QString stickerId = m_nudgeStickerId;
    const Sticker start = m_nudgeStartSticker;
    m_nudgeStickerId.clear();
    const std::optional<Sticker> live = liveSticker(stickerId);
    // One edit for the whole burst, and only when it moved: a key that hit the
    // paper edge must not push a dead undo step.
    if (live && onStickerCommitted && !(*live == start))
        onStickerCommitted(*live, StickerAction::Move);
    scheduleSettle();
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
    const bool hadSelection = !m_selectedId.isEmpty() || !m_selectedSlotId.isEmpty()
        || !m_selectedStickerId.isEmpty();
    m_selectedId.clear();
    m_selectedSlotId.clear();
    m_selectedStickerId.clear();
    if (!hadSelection)
        return;
    updateAccessibleDescription();
    update();
    if (onSelectionChanged)
        onSelectionChanged(QString());
    if (onSlotSelectionChanged)
        onSlotSelectionChanged(QString());
    if (onStickerSelectionChanged)
        onStickerSelectionChanged(QString());
}

void PageView::updateAccessibleDescription()
{
    QStringList parts;
    const Page *page = currentPage();
    if (page) {
        if (const Frame *frame = selectedFramePtr()) {
            if (SlotGeometry::frameIsClipped(*page, *frame)) {
                parts << QCoreApplication::translate(
                    "PhotoPrint", "A kép egy része le van vágva.");
            }
        } else if (selectedStickerPtr()) {
            // A sticker is a layer without a text, so the reader is told what
            // is selected before the panel reads its controls.
            parts << QCoreApplication::translate("PhotoPrint",
                                                 "A matrica ki van jelölve.");
        }
        // The background is a page-level property with its own panel, so a
        // screen reader should hear it the same way it hears the clipped-photo
        // warning.
        const PageBackground &background = page->background;
        if (background.kind != BackgroundKind::None) {
            QString kind;
            switch (background.kind) {
            case BackgroundKind::Solid:
                kind = QCoreApplication::translate("PhotoPrint", "Szín");
                break;
            case BackgroundKind::Gradient:
                kind = QCoreApplication::translate("PhotoPrint", "Színátmenet");
                break;
            case BackgroundKind::Image:
                kind = QCoreApplication::translate("PhotoPrint", "Kép");
                break;
            case BackgroundKind::None:
                break;
            }
            parts << QCoreApplication::translate("PhotoPrint", "A lap háttere: %1")
                         .arg(kind);
        }
    }
    const QString description = parts.join(QLatin1Char(' '));
    if (m_accessibleBase == description)
        return;
    m_accessibleBase = description;
    if (m_dropCue.isEmpty())
        setAccessibleDescription(description);
}

QColor PageView::handleOutlineColor() const
{
    // The page may now carry a background, so a light theme colour would
    // vanish on a light one; the handles also get a light underlay.
    QColor colour = palette().color(QPalette::Text);
    if (colour.lightness() > 140)
        colour = QColor(0x2c, 0x2c, 0x30);
    return colour;
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
        setAccessibleDescription(m_accessibleBase);
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
        break;
    }
    case DragKind::None:
    case DragKind::MoveSticker:
    case DragKind::ResizeSticker:
    case DragKind::RotateSticker:
        break;
    }
    // A move, resize or turn can never take a slotted photograph out of its
    // window: the frame is pulled back until a visible part stays inside.
    if (m_drag.kind != DragKind::CropPan && m_drag.kind != DragKind::None) {
        if (const Page *page = currentPage())
            frame = SlotGeometry::clampFrameToCell(*page, frame);
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

    // A light line just outside the page keeps the sheet edge visible when the
    // page and the canvas are both dark; on a light canvas it is invisible and
    // the theme border does the work.
    painter.setPen(QPen(QColor(255, 255, 255, 120), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(page.adjusted(-1, -1, 1, 1));
    painter.setPen(QPen(m_pageBorderColor, 1));
    painter.drawRect(page);

    if (!m_emptyHint.isEmpty()) {
        QFont font = painter.font();
        font.setPointSizeF(font.pointSizeF() * 1.1);
        painter.setFont(font);
        const QRectF hintArea = page.adjusted(page.width() * 0.12, page.height() * 0.42,
                                              -page.width() * 0.12, -page.height() * 0.42);
        // The page is no longer always white, so the hint sits on a translucent
        // plate: a fixed grey would disappear on a dark or busy background.
        const QRectF textRect = painter.boundingRect(
            hintArea, Qt::AlignCenter | Qt::TextWordWrap, m_emptyHint);
        painter.fillRect(textRect.adjusted(-8, -4, 8, 4), QColor(255, 255, 255, 210));
        painter.setPen(QColor(0x55, 0x58, 0x5d));
        painter.drawText(hintArea, Qt::AlignCenter | Qt::TextWordWrap, m_emptyHint);
    }

    // Placeholders and the cell selection are editor furniture: they live here,
    // never in paintPageContent, so no raster, PDF or printer can ever see them.
    if (const Page *current = currentPage()) {
        paintSlotOverlays(painter, *current);
        paintStickerPlaceholders(painter, *current);
    }

    // The focus ring is drawn for every state, so a keyboard user can always see
    // where the canvas has focus. A helper because the frame and the sticker
    // branches both end with it.
    const auto drawFocusRing = [this, &painter] {
        if (!hasFocus())
            return;
        QPen focus(QColor(0x2c, 0x6c, 0xd6));
        focus.setWidth(2);
        focus.setStyle(Qt::DashLine);
        painter.setPen(focus);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    };

    const Frame *frame = selectedFramePtr();
    const Sticker *layer = selectedStickerPtr();
    if (!frame && !layer) {
        drawFocusRing();
        return;
    }

    // A sticker's furniture: the same two-tone mark the photographs use, so the
    // selection is visible on white paper and on a dark or busy page alike.
    if (layer) {
        const QPolygonF stickerOutline = stickerQuadWidgetFor(*layer);
        QPen underlay(QColor(255, 255, 255, 220));
        underlay.setWidthF(3.6);
        painter.setPen(underlay);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(stickerOutline);
        QPen accent(palette().color(QPalette::Highlight));
        accent.setWidthF(1.6);
        painter.setPen(accent);
        painter.drawPolygon(stickerOutline);

        const QColor handleFill = palette().color(QPalette::Base);
        const QColor handleOutline = handleOutlineColor();
        painter.setPen(QPen(handleOutline, 1.2));
        painter.setBrush(handleFill);
        for (int i = 0; i < 4; ++i) {
            const QPointF point = stickerOutline.at(i);
            painter.drawRect(QRectF(point.x() - kHandleHalfPx, point.y() - kHandleHalfPx,
                                    kHandleHalfPx * 2.0, kHandleHalfPx * 2.0));
        }
        const QPointF stickerCenter = pageToWidget().map(QPointF(layer->cx, layer->cy));
        const QPointF topMid = (stickerOutline.at(0) + stickerOutline.at(1)) / 2.0;
        QPointF direction = topMid - stickerCenter;
        const double directionLength = std::hypot(direction.x(), direction.y());
        if (directionLength > 0.0)
            direction /= directionLength;
        const QPointF rotate = topMid + direction * kRotateOffsetPx;
        painter.setPen(QPen(handleOutline, 1.2));
        painter.drawLine(topMid, rotate);
        painter.setBrush(handleFill);
        painter.drawEllipse(rotate, 5.0, 5.0);
        drawFocusRing();
        return;
    }

    const QPolygonF quad = frameQuadWidget(*frame);
    // A light underlay keeps the selection visible on a dark background; the
    // accent line on top keeps it visible on white paper.
    QPen outlineUnderlay(QColor(255, 255, 255, 220));
    outlineUnderlay.setWidthF(3.6);
    painter.setPen(outlineUnderlay);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(quad);
    QPen outline(palette().color(QPalette::Highlight));
    outline.setWidthF(1.6);
    painter.setPen(outline);
    painter.drawPolygon(quad);

    // Every frame keeps the original handles, slotted or free: the section's
    // cell clips the photograph but does not take the frame's rectangle away
    // from the user. The fill and the outline are theme-aware, and the outline
    // is forced dark enough to stay visible on a light page; the light
    // underlay keeps it visible on a dark one.
    const QColor handleFill = palette().color(QPalette::Base);
    const QColor handleOutline = handleOutlineColor();
    painter.setPen(QPen(handleOutline, 1.2));
    painter.setBrush(handleFill);
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
    painter.setPen(QPen(handleOutline, 1.2));
    painter.drawLine(topMid, rotate);
    painter.setBrush(handleFill);
    painter.drawEllipse(rotate, 5.0, 5.0);

    if (m_cropMode) {
        // The crop overlay marks the visible window, not the frame: a slotted
        // photograph is clipped to its cell, so the dim and the dashed outline
        // follow the intersection of the frame and the cell.
        QPainterPath visible;
        if (!frame->slotId.isEmpty()) {
            if (const Page *page = currentPage()) {
                if (const Slot *slot = SlotGeometry::slotById(*page, frame->slotId)) {
                    QPainterPath framePath;
                    framePath.addPolygon(quad);
                    framePath.closeSubpath();
                    QPainterPath cellPath;
                    cellPath.addRect(pageToWidget().mapRect(
                        SlotGeometry::slotCellMm(*page, *slot)));
                    visible = framePath.intersected(cellPath);
                }
            }
        }
        if (visible.isEmpty()) {
            visible.addPolygon(quad);
            visible.closeSubpath();
        }
        QPainterPath dim;
        dim.setFillRule(Qt::OddEvenFill);
        dim.addRect(page);
        dim.addPath(visible);
        painter.fillPath(dim, QColor(255, 255, 255, 170));
        QPen cropPen(QColor(0x00, 0x7a, 0x5e), 1.4, Qt::DashLine);
        painter.setPen(cropPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(visible);
    }

    drawFocusRing();
}

void PageView::paintSlotOverlays(QPainter &painter, const Page &page) const
{
    if (page.slotList.isEmpty())
        return;
    const QColor accent = palette().color(QPalette::Highlight);
    // The section outline is furniture, not selection: a muted grey that stays
    // visible on the white page whether the cell is empty or filled. The accent
    // is reserved for the selected cell.
    const QColor muted(0x8a, 0x8a, 0x92);
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
        const Frame *occupant = SlotGeometry::frameInSlot(page, slot.id);
        const bool clipped = occupant && SlotGeometry::frameIsClipped(page, *occupant);

        QColor border;
        QColor fill;
        QString text;
        switch (state) {
        case SlotState::Filled:
            break;
        case SlotState::Empty:
            border = muted;
            fill = QColor(muted.red(), muted.green(), muted.blue(), 18);
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

        // The clip window is always visible, filled or not: without it a
        // photograph that fills its cell would look like a free frame. The
        // two-tone ring keeps it readable on a coloured page.
        const Qt::PenStyle sectionStyle =
            state == SlotState::Filled ? Qt::SolidLine : Qt::DashLine;
        const QList<qreal> sectionDash =
            state == SlotState::Filled ? QList<qreal>{} : QList<qreal>{5.0, 4.0};
        drawTwoToneRing(painter, ring, muted, 3.0, 1.0, sectionStyle, sectionDash);

        if (state != SlotState::Filled) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(ring, 3.0, 3.0);
            if (!text.isEmpty() && ring.width() > 54.0 && ring.height() > 22.0) {
                QFont font = baseFont;
                font.setPointSizeF(std::max(7.0, baseFont.pointSizeF() * 0.85));
                painter.setFont(font);
                const QString elided =
                    painter.fontMetrics().elidedText(text, Qt::ElideRight,
                                                     qRound(ring.width() - 10.0));
                const QRectF textRect = painter.boundingRect(
                    ring.adjusted(4, 2, -4, -2), Qt::AlignCenter, elided);
                // The cue sits on a plate for the same reason the empty-page
                // hint does: a fixed grey disappears on a dark background.
                painter.fillRect(textRect.adjusted(-4, -2, 4, 2),
                                 QColor(255, 255, 255, 210));
                painter.setPen(state == SlotState::Missing ? border.darker(115) : border);
                painter.drawText(ring.adjusted(4, 2, -4, -2), Qt::AlignCenter, elided);
            }
        }

        if (clipped) {
            // Part of the photograph is outside the window. A dashed orange
            // ring and a short badge say so before the user wonders where the
            // rest of the picture went; the inspector carries the full hint.
            drawTwoToneRing(painter, ring.adjusted(2.0, 2.0, -2.0, -2.0),
                            QColor(0xc4, 0x55, 0x00), 3.0, 1.6, Qt::DashLine,
                            {3.0, 3.0});
            const QString label = QCoreApplication::translate("PhotoPrint", "Levágva");
            QFont font = baseFont;
            font.setBold(true);
            font.setPointSizeF(std::max(7.0, baseFont.pointSizeF() * 0.85));
            const QFontMetrics metrics(font);
            const int textWidth = metrics.horizontalAdvance(label) + 12;
            const int textHeight = metrics.height() + 4;
            if (cell.width() >= textWidth + 8 && cell.height() >= textHeight + 8) {
                const QRectF badge(cell.left() + 5.0, cell.top() + 5.0, textWidth, textHeight);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0xc4, 0x55, 0x00));
                painter.drawRoundedRect(badge, 3.0, 3.0);
                painter.setFont(font);
                painter.setPen(Qt::white);
                painter.drawText(badge, Qt::AlignCenter, label);
            }
        }

        if (selected) {
            // A selected cell reads as one object even when the cell is filled:
            // the photograph's outline and this ring are the same rectangle.
            drawTwoToneRing(painter, ring, accent, 3.0, 2.0, Qt::SolidLine);
        }

        if (hovered) {
            // A filled cell is replaced, so its hover cue is not the accent drop
            // target: a distinct colour and a `Csere` badge say what the release
            // will do before the photo disappears.
            const bool occupied = SlotGeometry::frameInSlot(page, slot.id) != nullptr;
            const QColor cue = occupied ? QColor(0xc4, 0x55, 0x00) : accent;
            drawTwoToneRing(painter, cell.adjusted(1.5, 1.5, -1.5, -1.5), cue, 4.0,
                            2.4, Qt::SolidLine);
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

void PageView::paintStickerPlaceholders(QPainter &painter, const Page &page) const
{
    if (m_missingStickers.isEmpty())
        return;
    const QTransform transform = pageToWidget();
    const QList<Sticker> stickers = sanitiseStickers(page.stickers, pageSizeMm(page));
    for (const Sticker &sticker : stickers) {
        if (!m_missingStickers.contains(sticker.id))
            continue;
        const QPolygonF quad = transform.map(OverlayGeometry::stickerQuadMm(sticker));
        if (quad.size() != 4)
            continue;
        // The same two-tone mark the rest of the editor furniture uses, so the
        // box stays visible on white paper and on a dark or busy page.
        QPen underlay(QColor(255, 255, 255, 220));
        underlay.setWidthF(3.2);
        painter.setPen(underlay);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(quad);
        painter.drawLine(quad.at(0), quad.at(2));
        painter.drawLine(quad.at(1), quad.at(3));
        QPen pen(QColor(0xb3, 0x26, 0x1e), 1.4, Qt::DashLine);
        pen.setDashPattern({4.0, 3.0});
        painter.setPen(pen);
        painter.drawPolygon(quad);
        painter.drawLine(quad.at(0), quad.at(2));
        painter.drawLine(quad.at(1), quad.at(3));
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
    flushStickerNudge();
    cancelInteraction(false);
}

void PageView::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    // A drag whose release went to another window must not keep moving frames.
    // A wheel burst that lost focus still made its change: keep it, as one edit.
    flushWheelZoom();
    flushStickerNudge();
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
    // A new gesture ends a nudge and a wheel burst: their pending commits land
    // first, so the undo stack keeps one step per gesture.
    flushStickerNudge();
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

    // An overlay handle belongs to the selected sticker, exactly like a frame's.
    const int stickerHandle = hitStickerHandle(event->position());
    if (stickerHandle >= 0) {
        const Sticker *sticker = selectedStickerPtr();
        if (sticker) {
            beginStickerDrag(stickerHandle == 4 ? DragKind::RotateSticker
                                                : DragKind::ResizeSticker,
                             stickerHandle, pagePoint, *sticker);
            event->accept();
            return;
        }
    }

    const Page *page = currentPage();
    // Stickers are painted above the photographs, so they are hit first: a
    // click on a sticker over a photo must select the sticker. The Fotók list
    // is the way back to the photograph underneath.
    const int stickerIndex = hitSticker(event->position());
    if (stickerIndex >= 0 && page) {
        const Sticker sticker = page->stickers.at(stickerIndex);
        m_selectedId.clear();
        m_selectedSlotId.clear();
        setSelectedFrame(QString());
        setSelectedSlot(QString());
        setSelectedSticker(sticker.id);
        if (onStickerSelectionChanged)
            onStickerSelectionChanged(sticker.id);
        beginStickerDrag(DragKind::MoveSticker, -1, pagePoint, sticker);
        event->accept();
        return;
    }

    const int index = hitFrame(event->position());
    if (index >= 0 && page) {
        const Frame frame = page->frames.at(index);
        setSelectedSticker(QString());
        if (onStickerSelectionChanged)
            onStickerSelectionChanged(QString());
        setSelectedFrame(frame.id);
        setSelectedSlot(frame.slotId);
        if (onSelectionChanged)
            onSelectionChanged(frame.id);
        // A drag moves the photograph's frame. Inside a cell the picture may be
        // smaller or larger than the window, and a drag moves the window with
        // it; crop mode is what pans the image inside the frame.
        beginDrag(DragKind::Move, -1, pagePoint, frame);
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
            setSelectedSticker(QString());
            if (onStickerSelectionChanged)
                onStickerSelectionChanged(QString());
            setSelectedFrame(frame.id);
            setSelectedSlot(slot.id);
            if (onSelectionChanged)
                onSelectionChanged(frame.id);
            beginDrag(DragKind::Move, -1, pagePoint, frame);
        } else {
            m_selectedId.clear();
            m_selectedSlotId = slot.id;
            m_selectedStickerId.clear();
            setSelectedFrame(QString());
            setSelectedSlot(slot.id);
            setSelectedSticker(QString());
            update();
            if (onSelectionChanged)
                onSelectionChanged(QString());
            if (onStickerSelectionChanged)
                onStickerSelectionChanged(QString());
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
        setCursor(Qt::ArrowCursor);
        return;
    }
    const QPointF pagePoint = widgetToPage(event->position());
    const bool fine = event->modifiers().testFlag(Qt::ShiftModifier)
        || event->modifiers().testFlag(Qt::AltModifier);
    if (m_drag.kind == DragKind::MoveSticker || m_drag.kind == DragKind::ResizeSticker
        || m_drag.kind == DragKind::RotateSticker) {
        updateLiveSticker(draggedSticker(pagePoint, fine));
        return;
    }
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
    const DragKind kind = m_drag.kind;
    const Sticker stickerBefore = m_drag.startSticker;
    m_drag = DragState{};
    setCursor(Qt::ArrowCursor);
    if (changed) {
        if (kind == DragKind::MoveSticker || kind == DragKind::ResizeSticker
            || kind == DragKind::RotateSticker) {
            // One commit per gesture, with the kind so the window can name the
            // undo step. The live sticker is what the user last saw.
            if (const std::optional<Sticker> sticker = liveSticker(stickerBefore.id)) {
                if (onStickerCommitted) {
                    const StickerAction action = kind == DragKind::ResizeSticker
                        ? StickerAction::Resize
                        : (kind == DragKind::RotateSticker ? StickerAction::Rotate
                                                           : StickerAction::Move);
                    onStickerCommitted(*sticker, action);
                }
            }
        } else {
            const Frame *frame = selectedFramePtr();
            if (frame && onFrameCommitted)
                onFrameCommitted(*frame);
        }
    }
    // The gesture could only afford a coarse raster: schedule the full-scale
    // rebuild instead of composing inside the event that ends the drag.
    scheduleSettle();
}

void PageView::wheelEvent(QWheelEvent *event)
{
    const QPointF position = event->position();
    const double factor = std::pow(1.0015, event->angleDelta().y());

    // Which photograph, if any, is under the wheel? Only crop mode acts on a
    // frame; outside it the page zooms, even over a laid-out page. The frame is
    // copied out of the document first: flushWheelZoom() commits synchronously
    // and the window may publish a new document in response, so no pointer into
    // m_document may outlive it.
    Frame target;
    bool hasTarget = false;
    if (m_cropMode) {
        if (const Frame *selected = selectedFramePtr()) {
            target = *selected;
            hasTarget = true;
        }
    }
    const QString targetId = hasTarget ? target.id : QString();

    if (m_cropMode && hasTarget) {
        // Crop mode on a photograph: the wheel magnifies the image inside its
        // frame, slotted or free. The burst now commits once instead of once
        // per notch.
        const QSize pixels =
            m_images.frames.contains(targetId) ? m_images.frames.value(targetId).size() : QSize();
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

    // Outside crop mode the wheel is the page view's own zoom, exactly as it
    // was before slots existed. Ctrl is kept as an equivalent spelling.
    setViewZoom(m_viewZoom * factor);
    if (onViewZoomChanged)
        onViewZoomChanged(m_viewZoom);
    event->accept();
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    const Frame *frame = selectedFramePtr();
    const Sticker *sticker = selectedStickerPtr();

    // Delete and Escape act on a selected cell even when the cell is empty, so
    // a layout can be inspected without a photograph being selected.
    switch (event->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (sticker && onStickerDeleteRequested) {
            flushStickerNudge();
            onStickerDeleteRequested(sticker->id);
            event->accept();
            return;
        }
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
        flushStickerNudge();
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

    if (!frame && !sticker) {
        QWidget::keyPressEvent(event);
        return;
    }
    const bool fine = event->modifiers().testFlag(Qt::ShiftModifier);
    const double step = fine ? 0.1 : 1.0;

    if (sticker) {
        // An overlay nudge is a burst like a frame's: the outline follows every
        // key and the page is composed once the keys stop. One commit per press
        // would flood the undo stack with a held arrow.
        QPointF delta;
        switch (event->key()) {
        case Qt::Key_Left:
            delta.setX(-step);
            break;
        case Qt::Key_Right:
            delta.setX(step);
            break;
        case Qt::Key_Up:
            delta.setY(-step);
            break;
        case Qt::Key_Down:
            delta.setY(step);
            break;
        default:
            QWidget::keyPressEvent(event);
            return;
        }
        const Sticker moved = OverlayGeometry::movedSticker(*sticker, delta, pageMm());
        // A burst: the overlay follows every key and one edit lands when the
        // keys stop, so a held arrow is one undo step.
        if (m_nudgeStickerId != sticker->id) {
            m_nudgeStartSticker = *sticker;
            m_nudgeStickerId = sticker->id;
        }
        m_nudgePulse.start();
        scheduleSettle();
        updateLiveSticker(moved);
        if (m_stickerNudgeTimer)
            m_stickerNudgeTimer->start();
        event->accept();
        return;
    }

    Frame moved = *frame;

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
        // A nudge can never take a slotted photograph out of its window.
        if (const Page *page = currentPage())
            moved = SlotGeometry::clampFrameToCell(*page, moved);
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
    // A double click on a sticker is a second click on the same overlay, not a
    // request to crop the photograph underneath it.
    if (hitSticker(event->position()) >= 0) {
        event->accept();
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
    // A section header for one of the inspector's tabs. The parent follows the
    // layout it is added to, so a header inside the Fotó tab never lives in the
    // column behind it.
    const auto addSectionTo = [](QVBoxLayout *into, const QString &text) {
        QWidget *parent = into->parentWidget();
        auto *label = new QLabel(text, parent);
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

    // --- A jobb oldali vizsgáló: three contextual tabs -----------------------
    // The inspector used to be one long stack of layout and photo controls, most
    // of them irrelevant to whatever was selected. It is now three pages, one per
    // context: the selected photograph (Fotó), the selected section (Hely, which
    // opens by itself for an empty one) and the page itself (Lap). Only the
    // active context's controls are on screen. The indices are fixed, so a tab
    // without content is disabled rather than removed and the tab order never
    // jumps around.
    //
    // The tab strip sits above three independent scroll areas: it never scrolls
    // away with the page, and switching tabs lands at the top of the new page
    // instead of keeping the previous page's offset.
    m_inspectorTabs = new InspectorTabs(side);
    m_inspectorTabs->setObjectName(QStringLiteral("inspectorTabs"));
    m_inspectorTabs->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Beállítások"));
    m_inspectorTabs->setDocumentMode(true);
    m_inspectorTabs->tabBar()->setExpanding(false);
    m_inspectorTabs->tabBar()->setDrawBase(false);
    m_inspectorTabs->tabBar()->setFocusPolicy(Qt::StrongFocus);
    connect(m_inspectorTabs, &QTabWidget::currentChanged, this, [this](int index) {
        // A page opens at the top, whatever it was last left at, and a switch
        // never inherits the previous page's scroll offset.
        if (index >= 0) {
            if (auto *scroll = qobject_cast<QScrollArea *>(m_inspectorTabs->widget(index)))
                scroll->verticalScrollBar()->setValue(0);
        }
        if (m_settingInspectorTab || index < 0)
            return;
        // A click, Ctrl+Tab or an arrow key: the user's choice holds until the
        // selection context changes, so a slider never flips the panel away.
        m_inspectorTabChosen = true;
    });

    const auto makeScrollPage = [this](const QString &scrollName, const QString &contentName,
                                       const QString &accessibleName) {
        auto *scroll = new QScrollArea(m_inspectorTabs);
        scroll->setObjectName(scrollName);
        scroll->setAccessibleName(accessibleName);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget(scroll);
        content->setObjectName(contentName);
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 8, 0, 8);
        scroll->setWidget(content);
        return scroll;
    };

    auto *photoScroll = makeScrollPage(QStringLiteral("photoScroll"),
                                       QStringLiteral("photoPanel"),
                                       QCoreApplication::translate("PhotoPrint", "Fotó"));
    auto *photoTab = photoScroll->widget();
    auto *photoLayout = qobject_cast<QVBoxLayout *>(photoTab->layout());

    auto *slotScroll = makeScrollPage(QStringLiteral("slotScroll"),
                                      QStringLiteral("slotPanel"),
                                      QCoreApplication::translate("PhotoPrint", "Hely"));
    auto *slotTab = slotScroll->widget();
    auto *slotLayout = qobject_cast<QVBoxLayout *>(slotTab->layout());

    auto *pageScroll = makeScrollPage(QStringLiteral("pageScroll"),
                                      QStringLiteral("pagePanel"),
                                      QCoreApplication::translate("PhotoPrint", "Lap"));
    auto *pageTab = pageScroll->widget();
    auto *pageTabLayout = qobject_cast<QVBoxLayout *>(pageTab->layout());

    m_inspectorTabs->addTab(photoScroll, QCoreApplication::translate("PhotoPrint", "Fotó"));
    m_inspectorTabs->addTab(slotScroll, QCoreApplication::translate("PhotoPrint", "Hely"));
    m_inspectorTabs->addTab(pageScroll, QCoreApplication::translate("PhotoPrint", "Lap"));
    // The fourth tab is the overlay's own inspector. It is always enabled: with
    // nothing selected it is where a sticker is added from the built-in set, so
    // the page can be decorated without selecting something first. It opens by
    // itself when a sticker is selected.
    auto *elemScroll = makeScrollPage(QStringLiteral("elemScroll"),
                                      QStringLiteral("elemPanel"),
                                      QCoreApplication::translate("PhotoPrint", "Elem"));
    auto *elemTab = elemScroll->widget();
    auto *elemLayout = qobject_cast<QVBoxLayout *>(elemTab->layout());
    m_inspectorTabs->addTab(elemScroll, QCoreApplication::translate("PhotoPrint", "Elem"));

    // --- Lap: the page's own layout -----------------------------------------
    // A laid-out page is dressed as a whole, so these controls belong to the
    // page and not to one photograph. They live in the Lap tab, never next to
    // the photo controls: the layout is invisible while a photo is being edited.
    auto *layoutPanel = new QWidget(pageTab);
    layoutPanel->setObjectName(QStringLiteral("layoutPanel"));
    auto *layoutLayout = new QVBoxLayout(layoutPanel);
    layoutLayout->setContentsMargins(0, 0, 0, 0);
    m_layoutPanel = layoutPanel;

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
    const auto addTitle = [layoutPanel, layoutLayout](const QString &text, const QString &name) {
        auto *title = new QLabel(text, layoutPanel);
        title->setObjectName(name);
        title->setWordWrap(true);
        layoutLayout->addWidget(title);
    };
    const auto addLength = [layoutPanel, layoutLayout](const QString &spinName,
                                                       const QString &accessibleName,
                                                       double maximum, double step) {
        auto *spin = new QDoubleSpinBox(layoutPanel);
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
    const auto addAutomatic = [layoutPanel, layoutLayout](const QString &text,
                                                          const QString &name) {
        auto *check = new QCheckBox(text, layoutPanel);
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
        layoutPanel);
    m_gutterHint->setObjectName(QStringLiteral("gutterHint"));
    m_gutterHint->setWordWrap(true);
    markSecondaryText(m_gutterHint);
    layoutLayout->addWidget(m_gutterHint);

    addTitle(QCoreApplication::translate("PhotoPrint", "Sarok lekerekítése"),
             QStringLiteral("radiusTitle"));
    m_radiusSpin = addLength(QStringLiteral("radiusSpin"),
                             QCoreApplication::translate("PhotoPrint", "Sarok lekerekítése"),
                             SlotGeometry::kMaxCornerRadiusMm, 1.0);

    // Adding a section is a layout action and belongs here. Removing one needs a
    // selected section, so that action lives in Hely instead of sitting greyed
    // out in this panel.
    m_addSlotButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Hely hozzáadása"), layoutPanel);
    m_addSlotButton->setObjectName(QStringLiteral("addSlotButton"));
    layoutLayout->addWidget(m_addSlotButton);
    connect(m_addSlotButton, &QPushButton::clicked, this, &MainWindow::addSlotToPage);

    m_dissolveLayoutButton = new QPushButton(
        QCoreApplication::translate("PhotoPrint", "Elrendezés megszüntetése"), layoutPanel);
    m_dissolveLayoutButton->setObjectName(QStringLiteral("dissolveLayoutButton"));
    m_dissolveLayoutButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "A fotók megmaradnak a helyükön."));
    layoutLayout->addWidget(m_dissolveLayoutButton);
    connect(m_dissolveLayoutButton, &QPushButton::clicked, this,
            &MainWindow::dissolvePageLayout);

    // The template picker is already in the library column, always visible, and
    // it always acts on the current page. A second entry here would be the same
    // action twice.

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
    pageTabLayout->addWidget(layoutPanel);

    // --- Lap: the page summary for a page without a layout -------------------
    // A free page has no dress to edit, so it shows one line saying so rather
    // than a column of controls that could only clamp to zero. A brand-new,
    // still empty document has nothing to explain and stays quiet.
    m_pageSummaryPanel = new QWidget(pageTab);
    m_pageSummaryPanel->setObjectName(QStringLiteral("pageSummaryPanel"));
    auto *summaryLayout = new QVBoxLayout(m_pageSummaryPanel);
    summaryLayout->setContentsMargins(0, 0, 0, 0);
    auto *pageLayoutHint = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Ehhez a laphoz még nincs elrendezés."),
        m_pageSummaryPanel);
    pageLayoutHint->setObjectName(QStringLiteral("pageLayoutHint"));
    pageLayoutHint->setWordWrap(true);
    markSecondaryText(pageLayoutHint);
    summaryLayout->addWidget(pageLayoutHint);
    pageTabLayout->addWidget(m_pageSummaryPanel);

    // --- Lap: the page background -------------------------------------------
    // Below the layout controls: the layout is the primary page action, the
    // background is the sheet's dress. The group is always visible, because a
    // free page has a background too and it is the one page property that does
    // not depend on a layout. Only the controls of the selected kind are shown,
    // so the panel never offers a fit combo for a solid page.
    auto *backgroundPanel = new QWidget(pageTab);
    backgroundPanel->setObjectName(QStringLiteral("backgroundPanel"));
    auto *backgroundLayout = new QVBoxLayout(backgroundPanel);
    backgroundLayout->setContentsMargins(0, 0, 0, 0);
    m_backgroundPanel = backgroundPanel;

    auto *backgroundTitle = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Háttér"), backgroundPanel);
    backgroundTitle->setObjectName(QStringLiteral("backgroundTitle"));
    backgroundTitle->setWordWrap(true);
    QFont backgroundTitleFont = backgroundTitle->font();
    backgroundTitleFont.setWeight(QFont::DemiBold);
    backgroundTitle->setFont(backgroundTitleFont);
    backgroundLayout->addWidget(backgroundTitle);

    m_backgroundKindCombo = new QComboBox(backgroundPanel);
    m_backgroundKindCombo->setObjectName(QStringLiteral("backgroundKindCombo"));
    m_backgroundKindCombo->setAccessibleName(
        QCoreApplication::translate("PhotoPrint", "Háttér típusa"));
    m_backgroundKindCombo->addItem(QCoreApplication::translate("PhotoPrint", "Nincs"),
                                   int(BackgroundKind::None));
    m_backgroundKindCombo->addItem(QCoreApplication::translate("PhotoPrint", "Szín"),
                                   int(BackgroundKind::Solid));
    m_backgroundKindCombo->addItem(QCoreApplication::translate("PhotoPrint", "Színátmenet"),
                                   int(BackgroundKind::Gradient));
    m_backgroundKindCombo->addItem(QCoreApplication::translate("PhotoPrint", "Kép"),
                                   int(BackgroundKind::Image));
    backgroundLayout->addWidget(m_backgroundKindCombo);
    connect(m_backgroundKindCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::backgroundKindChanged);

    // The swatch is a button with a 24 x 24 icon and no text: the colour is the
    // label. Its accessible name carries the word, so a screen reader still
    // hears what it changes.
    const auto makeSwatch = [backgroundPanel](const QString &name, const QString &accessible) {
        auto *button = new QPushButton(backgroundPanel);
        button->setObjectName(name);
        button->setAccessibleName(accessible);
        button->setIconSize(QSize(24, 24));
        button->setMinimumWidth(48);
        return button;
    };

    m_backgroundSolidRow = new QWidget(backgroundPanel);
    m_backgroundSolidRow->setObjectName(QStringLiteral("backgroundSolidRow"));
    auto *solidLayout = new QHBoxLayout(m_backgroundSolidRow);
    solidLayout->setContentsMargins(0, 0, 0, 0);
    m_backgroundColorButton = makeSwatch(
        QStringLiteral("backgroundColorButton"),
        QCoreApplication::translate("PhotoPrint", "Háttér színe"));
    m_backgroundColorButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Válassz színt a háttérnek."));
    solidLayout->addWidget(m_backgroundColorButton);
    auto *solidLabel = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Szín"), m_backgroundSolidRow);
    solidLabel->setWordWrap(true);
    solidLayout->addWidget(solidLabel, 1);
    backgroundLayout->addWidget(m_backgroundSolidRow);
    connect(m_backgroundColorButton, &QPushButton::clicked, this,
            &MainWindow::chooseBackgroundColor);

    m_backgroundGradientRow = new QWidget(backgroundPanel);
    m_backgroundGradientRow->setObjectName(QStringLiteral("backgroundGradientRow"));
    auto *gradientLayout = new QVBoxLayout(m_backgroundGradientRow);
    gradientLayout->setContentsMargins(0, 0, 0, 0);
    auto *startRow = new QHBoxLayout;
    m_gradientStartButton = makeSwatch(
        QStringLiteral("gradientStartButton"),
        QCoreApplication::translate("PhotoPrint", "Kezdő szín"));
    startRow->addWidget(m_gradientStartButton);
    auto *startLabel = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Kezdő szín"), m_backgroundGradientRow);
    startLabel->setWordWrap(true);
    startRow->addWidget(startLabel, 1);
    gradientLayout->addLayout(startRow);
    connect(m_gradientStartButton, &QPushButton::clicked, this,
            &MainWindow::chooseGradientStart);

    auto *endRow = new QHBoxLayout;
    m_gradientEndButton = makeSwatch(
        QStringLiteral("gradientEndButton"),
        QCoreApplication::translate("PhotoPrint", "Záró szín"));
    endRow->addWidget(m_gradientEndButton);
    auto *endLabel = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Záró szín"), m_backgroundGradientRow);
    endLabel->setWordWrap(true);
    endRow->addWidget(endLabel, 1);
    gradientLayout->addLayout(endRow);
    connect(m_gradientEndButton, &QPushButton::clicked, this,
            &MainWindow::chooseGradientEnd);

    auto *angleLabel = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Irány"), m_backgroundGradientRow);
    angleLabel->setWordWrap(true);
    gradientLayout->addWidget(angleLabel);
    m_gradientAngleCombo = new QComboBox(m_backgroundGradientRow);
    m_gradientAngleCombo->setObjectName(QStringLiteral("gradientAngleCombo"));
    m_gradientAngleCombo->setAccessibleName(angleLabel->text());
    m_gradientAngleCombo->addItem(
        QCoreApplication::translate("PhotoPrint", "Balról jobbra"), 0);
    m_gradientAngleCombo->addItem(
        QCoreApplication::translate("PhotoPrint", "Felülről lefelé"), 90);
    m_gradientAngleCombo->addItem(
        QCoreApplication::translate("PhotoPrint", "Átlósan jobbra le"), 45);
    m_gradientAngleCombo->addItem(
        QCoreApplication::translate("PhotoPrint", "Átlósan balra le"), 135);
    gradientLayout->addWidget(m_gradientAngleCombo);
    connect(m_gradientAngleCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::gradientAngleChanged);
    m_gradientHint = new QLabel(
        QCoreApplication::translate(
            "PhotoPrint", "Két szín és négy irány közül választhatsz."),
        m_backgroundGradientRow);
    m_gradientHint->setObjectName(QStringLiteral("gradientHint"));
    m_gradientHint->setWordWrap(true);
    markSecondaryText(m_gradientHint);
    gradientLayout->addWidget(m_gradientHint);
    backgroundLayout->addWidget(m_backgroundGradientRow);

    m_backgroundImageRow = new QWidget(backgroundPanel);
    m_backgroundImageRow->setObjectName(QStringLiteral("backgroundImageRow"));
    auto *imageLayout = new QVBoxLayout(m_backgroundImageRow);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    m_backgroundImageButton = new QPushButton(
        QCoreApplication::translate("PhotoPrint", "Háttérkép kiválasztása…"),
        m_backgroundImageRow);
    m_backgroundImageButton->setObjectName(QStringLiteral("backgroundImageButton"));
    imageLayout->addWidget(m_backgroundImageButton);
    connect(m_backgroundImageButton, &QPushButton::clicked, this,
            &MainWindow::chooseBackgroundImage);
    m_backgroundFileLabel = new QLabel(m_backgroundImageRow);
    m_backgroundFileLabel->setObjectName(QStringLiteral("backgroundFileLabel"));
    m_backgroundFileLabel->setWordWrap(true);
    markSecondaryText(m_backgroundFileLabel);
    imageLayout->addWidget(m_backgroundFileLabel);

    auto *fitLabel = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Kép illesztése"), m_backgroundImageRow);
    fitLabel->setWordWrap(true);
    imageLayout->addWidget(fitLabel);
    m_backgroundFitCombo = new QComboBox(m_backgroundImageRow);
    m_backgroundFitCombo->setObjectName(QStringLiteral("backgroundFitCombo"));
    m_backgroundFitCombo->setAccessibleName(fitLabel->text());
    m_backgroundFitCombo->addItem(QCoreApplication::translate("PhotoPrint", "Kitöltés"),
                                  int(BackgroundFit::Fill));
    m_backgroundFitCombo->addItem(QCoreApplication::translate("PhotoPrint", "Befoglalás"),
                                  int(BackgroundFit::Fit));
    m_backgroundFitCombo->addItem(QCoreApplication::translate("PhotoPrint", "Csempézés"),
                                  int(BackgroundFit::Tile));
    imageLayout->addWidget(m_backgroundFitCombo);
    connect(m_backgroundFitCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::backgroundFitChanged);

    auto *tileLabel = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Csempe mérete"), m_backgroundImageRow);
    tileLabel->setWordWrap(true);
    imageLayout->addWidget(tileLabel);
    m_backgroundTileLabel = tileLabel;
    m_backgroundTileSpin = new QDoubleSpinBox(m_backgroundImageRow);
    m_backgroundTileSpin->setObjectName(QStringLiteral("backgroundTileSpin"));
    m_backgroundTileSpin->setAccessibleName(tileLabel->text());
    m_backgroundTileSpin->setDecimals(1);
    m_backgroundTileSpin->setRange(0.0, kMaxTileMm / 10.0);
    m_backgroundTileSpin->setSingleStep(0.5);
    m_backgroundTileSpin->setSuffix(QStringLiteral(" cm"));
    m_backgroundTileSpin->setSpecialValueText(
        QCoreApplication::translate("PhotoPrint", "Automatikus"));
    imageLayout->addWidget(m_backgroundTileSpin);
    connect(m_backgroundTileSpin, &QDoubleSpinBox::valueChanged, this,
            &MainWindow::backgroundTileChanged);
    connect(m_backgroundTileSpin, &QDoubleSpinBox::editingFinished, this,
            &MainWindow::flushBackground);
    backgroundLayout->addWidget(m_backgroundImageRow);

    m_backgroundMissingHint = new QLabel(
        QCoreApplication::translate("PhotoPrint", "A háttérkép nem található."),
        backgroundPanel);
    m_backgroundMissingHint->setObjectName(QStringLiteral("backgroundMissingHint"));
    m_backgroundMissingHint->setWordWrap(true);
    markSecondaryText(m_backgroundMissingHint);
    backgroundLayout->addWidget(m_backgroundMissingHint);
    m_backgroundRelinkButton = new QPushButton(
        QCoreApplication::translate("PhotoPrint", "Kép kiválasztása"), backgroundPanel);
    m_backgroundRelinkButton->setObjectName(QStringLiteral("backgroundRelinkButton"));
    backgroundLayout->addWidget(m_backgroundRelinkButton);
    connect(m_backgroundRelinkButton, &QPushButton::clicked, this,
            &MainWindow::chooseBackgroundImage);

    auto *opacityLabel = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Fedettség"), backgroundPanel);
    opacityLabel->setWordWrap(true);
    backgroundLayout->addWidget(opacityLabel);
    m_backgroundOpacityLabel = opacityLabel;
    m_backgroundOpacitySlider = new QSlider(Qt::Horizontal, backgroundPanel);
    m_backgroundOpacitySlider->setObjectName(QStringLiteral("backgroundOpacitySlider"));
    m_backgroundOpacitySlider->setAccessibleName(opacityLabel->text());
    m_backgroundOpacitySlider->setRange(0, 100);
    m_backgroundOpacitySlider->setValue(100);
    backgroundLayout->addWidget(m_backgroundOpacitySlider);
    m_backgroundOpacityValue = new QLabel(QStringLiteral("100 %"), backgroundPanel);
    m_backgroundOpacityValue->setObjectName(QStringLiteral("backgroundOpacityValue"));
    markSecondaryText(m_backgroundOpacityValue);
    backgroundLayout->addWidget(m_backgroundOpacityValue);
    connect(m_backgroundOpacitySlider, &QSlider::valueChanged, this,
            &MainWindow::backgroundOpacityChanged);
    connect(m_backgroundOpacitySlider, &QSlider::sliderReleased, this,
            &MainWindow::flushBackground);

    m_backgroundResetButton = new QPushButton(
        QCoreApplication::translate("PhotoPrint", "Vissza fehérre"), backgroundPanel);
    m_backgroundResetButton->setObjectName(QStringLiteral("backgroundResetButton"));
    backgroundLayout->addWidget(m_backgroundResetButton);
    connect(m_backgroundResetButton, &QPushButton::clicked, this,
            &MainWindow::resetBackground);

    m_backgroundHint = new QLabel(
        QCoreApplication::translate(
            "PhotoPrint",
            "A háttér a fotók mögött látszik, a résekben és az üres helyeken is."),
        backgroundPanel);
    m_backgroundHint->setObjectName(QStringLiteral("backgroundHint"));
    m_backgroundHint->setWordWrap(true);
    markSecondaryText(m_backgroundHint);
    backgroundLayout->addWidget(m_backgroundHint);
    // The background is the page's dress, and the layout panel is long: the
    // group sits directly under the page info so it is not below the fold on a
    // small window or at a larger font.
    pageTabLayout->insertWidget(0, backgroundPanel);

    if (!m_backgroundTimer) {
        m_backgroundTimer = new QTimer(this);
        m_backgroundTimer->setSingleShot(true);
        m_backgroundTimer->setInterval(kStyleDebounceMs);
        connect(m_backgroundTimer, &QTimer::timeout, this, &MainWindow::flushBackground);
    }
    m_pendingBackground.reset();
    m_pendingBackgroundField.reset();

    // --- Lap: tools that belong to the sheet, not to one photograph ---------
    // Always in the Lap tab: a page with no layout and no selection can still be
    // tidied, and the cut guide state is a page property.
    auto *pageTools = new QWidget(pageTab);
    pageTools->setObjectName(QStringLiteral("pageToolsPanel"));
    auto *pageToolsLayout = new QVBoxLayout(pageTools);
    pageToolsLayout->setContentsMargins(0, 0, 0, 0);

    m_alignButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Fotók rendezése…"), pageTools);
    m_alignButton->setObjectName(QStringLiteral("alignPhotosButton"));
    m_alignButton->setToolTip(QCoreApplication::translate(
        "PhotoPrint", "A lapon lévő fotók elrendezése. A sorrend nem változik."));
    pageToolsLayout->addWidget(m_alignButton);
    connect(m_alignButton, &QPushButton::clicked, this, &MainWindow::openAlignPhotos);

    auto *guideTitle = new QLabel(QCoreApplication::translate("PhotoPrint", "Vágójelek"), pageTools);
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
    m_guidesHint = new QLabel(pageTools);
    m_guidesHint->setObjectName(QStringLiteral("cutGuidesHint"));
    m_guidesHint->setWordWrap(true);
    markSecondaryText(m_guidesHint);
    pageToolsLayout->addWidget(m_guidesHint);

    auto *borderlessTitle =
        new QLabel(QCoreApplication::translate("PhotoPrint", "Szélig nyomtatás"), pageTools);
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
                   pageTools);
    m_borderlessHint->setWordWrap(true);
    markSecondaryText(m_borderlessHint);
    pageToolsLayout->addWidget(m_borderlessHint);
    pageToolsLayout->addStretch(1);
    pageTabLayout->addWidget(pageTools);

    // The page's own summary line: paper and size. It heads the Lap tab, so the
    // page is identified before either the layout or the no-layout state talks
    // about it.
    m_pageInfo = new QLabel(pageTab);
    m_pageInfo->setWordWrap(true);
    m_pageInfo->setObjectName(QStringLiteral("pageInfoLabel"));
    pageTabLayout->insertWidget(0, m_pageInfo);

    // --- Elem: the sticker palette and the selected sticker ------------------
    // The palette is the tab's way in and stays visible while a sticker is
    // selected: adding a second one must not need a deselect first. The grid is
    // icons only, and every icon carries its name as the accessible text, so a
    // screen reader can tell them apart.
    m_stickerAddPanel = new QWidget(elemTab);
    m_stickerAddPanel->setObjectName(QStringLiteral("stickerAddPanel"));
    auto *stickerAddLayout = new QVBoxLayout(m_stickerAddPanel);
    stickerAddLayout->setContentsMargins(0, 0, 0, 0);

    m_addStickerButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Matrica hozzáadása…"),
                        m_stickerAddPanel);
    m_addStickerButton->setObjectName(QStringLiteral("addStickerButton"));
    m_addStickerButton->setAccessibleName(
        QCoreApplication::translate("PhotoPrint", "Matrica hozzáadása…"));
    m_addStickerButton->setMinimumHeight(32);
    stickerAddLayout->addWidget(m_addStickerButton);
    connect(m_addStickerButton, &QPushButton::clicked, this, &MainWindow::addSticker);

    auto *stickerSetTitle =
        new QLabel(QCoreApplication::translate("PhotoPrint", "Matricakészlet"),
                   m_stickerAddPanel);
    stickerSetTitle->setObjectName(QStringLiteral("stickerSetTitle"));
    stickerSetTitle->setWordWrap(true);
    QFont stickerSetFont = stickerSetTitle->font();
    stickerSetFont.setWeight(QFont::DemiBold);
    stickerSetTitle->setFont(stickerSetFont);
    stickerAddLayout->addWidget(stickerSetTitle);

    m_stickerSetGrid = new QWidget(m_stickerAddPanel);
    m_stickerSetGrid->setObjectName(QStringLiteral("stickerSetGrid"));
    m_stickerSetGrid->setAccessibleName(
        QCoreApplication::translate("PhotoPrint", "Matricakészlet"));
    auto *stickerSetLayout = new QGridLayout(m_stickerSetGrid);
    stickerSetLayout->setContentsMargins(0, 0, 0, 0);
    stickerSetLayout->setSpacing(4);
    {
        // The shipped shapes. The name is the label a person reads and the alias
        // is what the model stores in the project, so the two can never drift
        // apart. The icon comes from the same versioned resource the placed
        // sticker will resolve to.
        const struct {
            const char *alias;
            const char *label;
        } builtIns[] = {
            {"star", QT_TRANSLATE_NOOP("PhotoPrint", "Csillag")},
            {"heart", QT_TRANSLATE_NOOP("PhotoPrint", "Szív")},
            {"arrow", QT_TRANSLATE_NOOP("PhotoPrint", "Nyíl")},
            {"speech", QT_TRANSLATE_NOOP("PhotoPrint", "Beszédbuborék")},
            {"sun", QT_TRANSLATE_NOOP("PhotoPrint", "Nap")},
            {"flower", QT_TRANSLATE_NOOP("PhotoPrint", "Virág")},
            {"confetti", QT_TRANSLATE_NOOP("PhotoPrint", "Konfetti")},
            {"frame", QT_TRANSLATE_NOOP("PhotoPrint", "Keret")},
            {"ring", QT_TRANSLATE_NOOP("PhotoPrint", "Kör")},
            {"sparkle", QT_TRANSLATE_NOOP("PhotoPrint", "Szikra")},
            {"ribbon", QT_TRANSLATE_NOOP("PhotoPrint", "Szalag")},
            {"badge", QT_TRANSLATE_NOOP("PhotoPrint", "Jelvény")},
        };
        int index = 0;
        for (const auto &builtIn : builtIns) {
            const QString alias = QString::fromLatin1(builtIn.alias);
            const QString label = QCoreApplication::translate("PhotoPrint", builtIn.label);
            auto *button = new QToolButton(m_stickerSetGrid);
            button->setObjectName(QStringLiteral("stickerSet_") + alias);
            button->setIcon(QIcon(builtinStickerResource(builtinStickerPath(alias))));
            button->setIconSize(QSize(30, 30));
            button->setFixedSize(40, 40);
            button->setAutoRaise(true);
            button->setToolTip(label);
            button->setAccessibleName(label);
            button->setProperty("stickerAlias", alias);
            connect(button, &QToolButton::clicked, this,
                    [this, alias] { addBuiltInSticker(alias); });
            // A 6 x 2 palette: one row less than 4 x 3 leaves room for the
            // properties and the actions at the contract window size.
            stickerSetLayout->addWidget(button, index / 6, index % 6);
            ++index;
        }
        // One spacer column so the buttons do not stretch across the panel.
        stickerSetLayout->setColumnStretch(6, 1);
    }
    stickerAddLayout->addWidget(m_stickerSetGrid);
    elemLayout->addWidget(m_stickerAddPanel);

    // The one line that explains the tab before anything is on the page. It
    // disappears as soon as a sticker is selected, and the palette above stays.
    m_stickerEmptyPanel = new QWidget(elemTab);
    m_stickerEmptyPanel->setObjectName(QStringLiteral("stickerEmptyPanel"));
    auto *stickerEmptyLayout = new QVBoxLayout(m_stickerEmptyPanel);
    stickerEmptyLayout->setContentsMargins(0, 0, 0, 0);
    m_stickerEmptyHint = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Még nincs matrica a lapon."),
        m_stickerEmptyPanel);
    m_stickerEmptyHint->setObjectName(QStringLiteral("stickerEmptyHint"));
    m_stickerEmptyHint->setWordWrap(true);
    markSecondaryText(m_stickerEmptyHint);
    stickerEmptyLayout->addWidget(m_stickerEmptyHint);
    elemLayout->addWidget(m_stickerEmptyPanel);

    // The selected sticker's own controls. Each one writes the whole page as one
    // edit, so one action is one undo step, exactly like the background group.
    // The fields share one grid so the labels sit beside their controls: the
    // panel has to keep delete and the order buttons on screen at 1280 x 720
    // with a large system font, under the palette.
    m_stickerPanel = new QWidget(elemTab);
    m_stickerPanel->setObjectName(QStringLiteral("stickerPanel"));
    auto *stickerLayout = new QVBoxLayout(m_stickerPanel);
    stickerLayout->setContentsMargins(0, 0, 0, 0);
    auto *stickerFields = new QGridLayout;
    stickerFields->setContentsMargins(0, 0, 0, 0);
    stickerFields->setHorizontalSpacing(8);
    stickerFields->setVerticalSpacing(6);

    m_stickerPreview = new QLabel(m_stickerPanel);
    m_stickerPreview->setObjectName(QStringLiteral("stickerPreview"));
    m_stickerPreview->setFixedSize(56, 56);
    m_stickerPreview->setAlignment(Qt::AlignCenter);
    m_stickerPreview->setAccessibleName(
        QCoreApplication::translate("PhotoPrint", "Matrica"));
    // A plate behind the picture, so a pale or transparent sticker is visible
    // against the panel as well.
    m_stickerPreview->setStyleSheet(QStringLiteral(
        "border: 1px solid #d2d2d7; border-radius: 6px; background: #f5f5f7;"));
    stickerFields->addWidget(m_stickerPreview, 0, 0, 1, 2, Qt::AlignLeft);

    auto *stickerSizeLabel =
        new QLabel(QCoreApplication::translate("PhotoPrint", "Méret"), m_stickerPanel);
    stickerSizeLabel->setWordWrap(false);
    stickerFields->addWidget(stickerSizeLabel, 1, 0);
    m_stickerSizeSpin = new QDoubleSpinBox(m_stickerPanel);
    m_stickerSizeSpin->setObjectName(QStringLiteral("stickerSizeSpin"));
    m_stickerSizeSpin->setAccessibleName(stickerSizeLabel->text());
    // The model is millimetres, the contract is centimetres: the spin is the
    // only place the conversion happens.
    m_stickerSizeSpin->setDecimals(2);
    m_stickerSizeSpin->setRange(kMinStickerMm / 10.0, kMaxStickerMm / 10.0);
    m_stickerSizeSpin->setSingleStep(0.5);
    m_stickerSizeSpin->setSuffix(QStringLiteral(" cm"));
    stickerFields->addWidget(m_stickerSizeSpin, 1, 1);
    connect(m_stickerSizeSpin, &QDoubleSpinBox::editingFinished, this,
            &MainWindow::stickerSizeChanged);

    auto *stickerAngleLabel =
        new QLabel(QCoreApplication::translate("PhotoPrint", "Elforgatás"), m_stickerPanel);
    stickerAngleLabel->setWordWrap(false);
    stickerFields->addWidget(stickerAngleLabel, 2, 0);
    m_stickerAngleSpin = new QDoubleSpinBox(m_stickerPanel);
    m_stickerAngleSpin->setObjectName(QStringLiteral("stickerAngleSpin"));
    m_stickerAngleSpin->setAccessibleName(stickerAngleLabel->text());
    m_stickerAngleSpin->setDecimals(1);
    m_stickerAngleSpin->setRange(-180.0, 180.0);
    m_stickerAngleSpin->setSingleStep(5.0);
    m_stickerAngleSpin->setSuffix(QStringLiteral("°"));
    stickerFields->addWidget(m_stickerAngleSpin, 2, 1);
    connect(m_stickerAngleSpin, &QDoubleSpinBox::editingFinished, this,
            &MainWindow::stickerAngleChanged);

    auto *stickerOpacityLabel =
        new QLabel(QCoreApplication::translate("PhotoPrint", "Fedettség"), m_stickerPanel);
    stickerOpacityLabel->setWordWrap(false);
    stickerFields->addWidget(stickerOpacityLabel, 3, 0);
    auto *stickerOpacityRow = new QHBoxLayout;
    m_stickerOpacitySlider = new QSlider(Qt::Horizontal, m_stickerPanel);
    m_stickerOpacitySlider->setObjectName(QStringLiteral("stickerOpacitySlider"));
    m_stickerOpacitySlider->setAccessibleName(stickerOpacityLabel->text());
    m_stickerOpacitySlider->setRange(0, 100);
    // Tracking on, so the canvas follows the drag; the burst timer lands the
    // whole drag as one edit.
    m_stickerOpacitySlider->setTracking(true);
    m_stickerOpacityValue = new QLabel(QStringLiteral("100 %"), m_stickerPanel);
    m_stickerOpacityValue->setObjectName(QStringLiteral("stickerOpacityValue"));
    m_stickerOpacityValue->setMinimumWidth(38);
    markSecondaryText(m_stickerOpacityValue);
    stickerOpacityRow->addWidget(m_stickerOpacitySlider, 1);
    stickerOpacityRow->addWidget(m_stickerOpacityValue);
    stickerFields->addLayout(stickerOpacityRow, 3, 1);
    connect(m_stickerOpacitySlider, &QSlider::valueChanged, this,
            &MainWindow::stickerOpacityChanged);
    connect(m_stickerOpacitySlider, &QSlider::sliderReleased, this,
            &MainWindow::flushStickerOpacity);
    if (!m_stickerOpacityTimer) {
        m_stickerOpacityTimer = new QTimer(this);
        m_stickerOpacityTimer->setSingleShot(true);
        m_stickerOpacityTimer->setInterval(kStyleDebounceMs);
        connect(m_stickerOpacityTimer, &QTimer::timeout, this,
                &MainWindow::flushStickerOpacity);
    }
    m_pendingStickerOpacity.reset();

    stickerLayout->addLayout(stickerFields);

    auto *stickerOrderRow = new QHBoxLayout;
    m_stickerRaiseButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Előtérbe"), m_stickerPanel);
    m_stickerRaiseButton->setObjectName(QStringLiteral("stickerRaiseButton"));
    m_stickerRaiseButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Egy lépéssel előrébb."));
    m_stickerLowerButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Háttérbe"), m_stickerPanel);
    m_stickerLowerButton->setObjectName(QStringLiteral("stickerLowerButton"));
    m_stickerLowerButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Egy lépéssel hátrébb."));
    stickerOrderRow->addWidget(m_stickerRaiseButton);
    stickerOrderRow->addWidget(m_stickerLowerButton);
    stickerLayout->addLayout(stickerOrderRow);
    connect(m_stickerRaiseButton, &QPushButton::clicked, this,
            [this] { reorderStickerLayer(1); });
    connect(m_stickerLowerButton, &QPushButton::clicked, this,
            [this] { reorderStickerLayer(-1); });

    m_stickerDeleteButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Eltávolítás"),
                        m_stickerPanel);
    m_stickerDeleteButton->setObjectName(QStringLiteral("stickerDeleteButton"));
    stickerLayout->addWidget(m_stickerDeleteButton);
    connect(m_stickerDeleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteSelectedSticker);

    m_stickerMissingHint = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Ezt a matricát nem találjuk."),
        m_stickerPanel);
    m_stickerMissingHint->setObjectName(QStringLiteral("stickerMissingHint"));
    m_stickerMissingHint->setWordWrap(true);
    markSecondaryText(m_stickerMissingHint);
    stickerLayout->addWidget(m_stickerMissingHint);
    m_stickerRelinkButton = new QPushButton(
        QCoreApplication::translate("PhotoPrint", "Matrica kiválasztása"), m_stickerPanel);
    m_stickerRelinkButton->setObjectName(QStringLiteral("stickerRelinkButton"));
    stickerLayout->addWidget(m_stickerRelinkButton);
    connect(m_stickerRelinkButton, &QPushButton::clicked, this,
            &MainWindow::chooseStickerImage);

    elemLayout->addWidget(m_stickerPanel);

    // The keyboard-only route to an existing overlay: cycling the paint order,
    // with the position and the count between the two buttons so the user knows
    // where in the stack they are.
    auto *stickerNavRow = new QHBoxLayout;
    m_previousStickerButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Előző elem"), elemTab);
    m_previousStickerButton->setObjectName(QStringLiteral("previousStickerButton"));
    m_nextStickerButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Következő elem"), elemTab);
    m_nextStickerButton->setObjectName(QStringLiteral("nextStickerButton"));
    m_stickerPositionLabel = new QLabel(elemTab);
    m_stickerPositionLabel->setObjectName(QStringLiteral("stickerPositionLabel"));
    markSecondaryText(m_stickerPositionLabel);
    m_stickerPositionLabel->setAlignment(Qt::AlignCenter);
    stickerNavRow->addWidget(m_previousStickerButton);
    stickerNavRow->addWidget(m_stickerPositionLabel, 1);
    stickerNavRow->addWidget(m_nextStickerButton);
    elemLayout->addLayout(stickerNavRow);
    connect(m_previousStickerButton, &QPushButton::clicked, this,
            [this] { cycleSticker(-1); });
    connect(m_nextStickerButton, &QPushButton::clicked, this, [this] { cycleSticker(1); });

    auto *stickerHint = new QLabel(
        QCoreApplication::translate("PhotoPrint", "A matrica a fotók fölött látszik."),
        elemTab);
    stickerHint->setObjectName(QStringLiteral("stickerHint"));
    stickerHint->setWordWrap(true);
    markSecondaryText(stickerHint);
    elemLayout->addWidget(stickerHint);
    elemLayout->addStretch(1);

    // --- Hely: the selected empty section ------------------------------------
    // An empty section is a selection of its own, so this tab earns its open:
    // the section's facts, the action that fills this exact cell, and the action
    // that removes it. No layout numbers, no photo controls, no page tools and no
    // drop hint, because a panel is not a drop target.
    auto *slotExplanation = new QLabel(
        QCoreApplication::translate(
            "PhotoPrint",
            "Az üres hely a lap egy kivágott része; ide egy fotó kerül."),
        slotTab);
    slotExplanation->setObjectName(QStringLiteral("slotExplanation"));
    slotExplanation->setWordWrap(true);
    markSecondaryText(slotExplanation);
    slotLayout->addWidget(slotExplanation);

    auto *facts = new QGridLayout;
    facts->setContentsMargins(0, 4, 0, 6);
    facts->setHorizontalSpacing(10);
    facts->setVerticalSpacing(4);
    const auto addFact = [slotTab, facts](const QString &labelText, const QString &valueName) {
        auto *label = new QLabel(labelText, slotTab);
        markSecondaryText(label);
        auto *value = new QLabel(slotTab);
        value->setObjectName(valueName);
        value->setWordWrap(true);
        const int row = facts->rowCount();
        facts->addWidget(label, row, 0);
        facts->addWidget(value, row, 1);
        return value;
    };
    m_slotSizeValue = addFact(QCoreApplication::translate("PhotoPrint", "Méret"),
                              QStringLiteral("slotSizeValue"));
    m_slotPositionValue = addFact(QCoreApplication::translate("PhotoPrint", "Hely"),
                                  QStringLiteral("slotPositionValue"));
    m_slotStateValue = addFact(QCoreApplication::translate("PhotoPrint", "Állapot"),
                               QStringLiteral("slotStateValue"));
    m_slotClipValue = addFact(QCoreApplication::translate("PhotoPrint", "Levágás"),
                              QStringLiteral("slotClipValue"));
    slotLayout->addLayout(facts);

    // "Fotó beillesztése" fills this cell, not the first empty one: the panel is
    // talking about this section, so the file must land here.
    m_slotFillButton = new QPushButton(
        QCoreApplication::translate("PhotoPrint", "Fotó beillesztése"), slotTab);
    m_slotFillButton->setObjectName(QStringLiteral("slotAddPhotosButton"));
    slotLayout->addWidget(m_slotFillButton);
    connect(m_slotFillButton, &QPushButton::clicked, this,
            &MainWindow::addPhotosToSelectedSlot);

    // Removing a section needs a selected section, so this is the only place the
    // action lives. The Lap tab adds sections; it does not repeat this one.
    m_slotRemoveButton = new QPushButton(
        QCoreApplication::translate("PhotoPrint", "Hely törlése"), slotTab);
    m_slotRemoveButton->setObjectName(QStringLiteral("slotRemoveButton"));
    slotLayout->addWidget(m_slotRemoveButton);
    connect(m_slotRemoveButton, &QPushButton::clicked, this, &MainWindow::removeSlotFromPage);
    slotLayout->addStretch(1);

    // --- Fotó: the selected photograph ---------------------------------------
    // One tab, one context. The old "Kiválasztott fotó" header is gone: the tab
    // is named Fotó, and the controls are ordered the way a photograph is worked
    // on, with the size and angle spins visible instead of collapsed away.
    auto *fitRow = new QHBoxLayout;
    m_fitButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Teljes kép"), photoTab);
    m_fitButton->setObjectName(QStringLiteral("fitButton"));
    m_fillButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Kép kitöltése"), photoTab);
    m_fillButton->setObjectName(QStringLiteral("fillButton"));
    fitRow->addWidget(m_fitButton);
    fitRow->addWidget(m_fillButton);
    photoLayout->addLayout(fitRow);
    connect(m_fitButton, &QPushButton::clicked, this, [this] { applyFit(FitMode::Full); });
    connect(m_fillButton, &QPushButton::clicked, this, [this] { applyFit(FitMode::Fill); });

    auto *actionRow = new QHBoxLayout;
    m_cropButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Kivágás"), photoTab);
    m_cropButton->setObjectName(QStringLiteral("cropButton"));
    m_cropButton->setCheckable(true);
    m_deleteButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Eltávolítás"), photoTab);
    m_deleteButton->setObjectName(QStringLiteral("deleteButton"));
    actionRow->addWidget(m_cropButton);
    actionRow->addWidget(m_deleteButton);
    photoLayout->addLayout(actionRow);
    connect(m_cropButton, &QPushButton::toggled, this, &MainWindow::toggleCrop);
    connect(m_deleteButton, &QPushButton::clicked, this, &MainWindow::deleteSelected);

    auto *layerRow = new QHBoxLayout;
    m_forwardButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Előtérbe"), photoTab);
    m_forwardButton->setObjectName(QStringLiteral("forwardButton"));
    m_backwardButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Háttérbe"), photoTab);
    m_backwardButton->setObjectName(QStringLiteral("backwardButton"));
    layerRow->addWidget(m_forwardButton);
    layerRow->addWidget(m_backwardButton);
    photoLayout->addLayout(layerRow);
    connect(m_forwardButton, &QPushButton::clicked, this, [this] { reorderFrame(1); });
    connect(m_backwardButton, &QPushButton::clicked, this, [this] { reorderFrame(-1); });

    auto *toneRow = new QHBoxLayout;
    m_autoCheck = new ToggleSwitch(photoTab);
    m_autoCheck->setObjectName(QStringLiteral("autoToneSwitch"));
    m_autoCheck->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Automatikus javítás"));
    m_autoCheck->setToolTip(
        QCoreApplication::translate("PhotoPrint", "Finoman igazítja a fényt és a színt a nyomtatáshoz."));
    auto *toneText = new QVBoxLayout;
    auto *toneTitle = new QLabel(QCoreApplication::translate("PhotoPrint", "Automatikus javítás"), photoTab);
    QFont toneFont = toneTitle->font();
    toneFont.setWeight(QFont::DemiBold);
    toneTitle->setFont(toneFont);
    m_autoHint = new QLabel(QCoreApplication::translate("PhotoPrint", "Finom fény- és színjavítás"), photoTab);
    markSecondaryText(m_autoHint);
    m_autoHint->setWordWrap(true);
    toneText->addWidget(toneTitle);
    toneText->addWidget(m_autoHint);
    m_originalButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Eredeti"), photoTab);
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

    m_brightnessSlider = new QSlider(Qt::Horizontal, photoTab);
    m_brightnessSlider->setObjectName(QStringLiteral("brightnessSlider"));
    m_brightnessSlider->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Világosítás"));
    m_brightnessSlider->setRange(kBrightnessSliderMin, kBrightnessSliderMax);
    m_brightnessSlider->setValue(kBrightnessSliderNeutral);
    m_brightnessSlider->setTracking(false);
    photoLayout->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Világosítás"), photoTab));
    photoLayout->addWidget(m_brightnessSlider);
    connect(m_brightnessSlider, &QSlider::valueChanged, this, &MainWindow::brightnessChanged);

    addSectionTo(photoLayout, QCoreApplication::translate("PhotoPrint", "Effekt"));
    m_effectButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Eredeti"), photoTab);
    m_effectButton->setObjectName(QStringLiteral("effectButton"));
    m_effectButton->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Effekt"));
    m_effectButton->setToolTip(QCoreApplication::translate("PhotoPrint", "Effekt kiválasztása"));
    photoLayout->addWidget(m_effectButton);
    m_legacyHint = new QLabel(QCoreApplication::translate("PhotoPrint", "Új effekt választása"), photoTab);
    m_legacyHint->setObjectName(QStringLiteral("legacyEffectHint"));
    m_legacyHint->setWordWrap(true);
    markSecondaryText(m_legacyHint);
    m_legacyHint->setVisible(false);
    photoLayout->addWidget(m_legacyHint);
    connect(m_effectButton, &QPushButton::clicked, this, &MainWindow::openEffectBrowser);

    m_strengthLabel = new QLabel(QCoreApplication::translate("PhotoPrint", "Effekt erőssége"), photoTab);
    m_strengthLabel->setObjectName(QStringLiteral("effectStrengthLabel"));
    photoLayout->addWidget(m_strengthLabel);
    m_filterStrength = new QSlider(Qt::Horizontal, photoTab);
    m_filterStrength->setObjectName(QStringLiteral("effectStrength"));
    m_filterStrength->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Effekt erőssége"));
    m_filterStrength->setRange(0, 100);
    m_filterStrength->setValue(100);
    m_filterStrength->setTracking(false);
    photoLayout->addWidget(m_filterStrength);
    connect(m_filterStrength, &QSlider::valueChanged, this, &MainWindow::applyFilterSettings);

    m_strengthHint = new QLabel(photoTab);
    m_strengthHint->setObjectName(QStringLiteral("effectStrengthHint"));
    m_strengthHint->setWordWrap(true);
    markSecondaryText(m_strengthHint);
    m_strengthHint->setVisible(false);
    photoLayout->addWidget(m_strengthHint);

    m_upscaleCheck = new QCheckBox(QCoreApplication::translate("PhotoPrint", "Kép nagyítása 2×"), photoTab);
    m_upscaleCheck->setObjectName(QStringLiteral("upscaleCheck"));
    m_upscaleCheck->setToolTip(
        QCoreApplication::translate("PhotoPrint", "A nyomtatási felbontást növeli; az előnézet nem változik."));
    photoLayout->addWidget(m_upscaleCheck);
    auto *upscaleHint = new QLabel(QCoreApplication::translate("PhotoPrint", "A nyomtatási felbontást növeli."), photoTab);
    markSecondaryText(upscaleHint);
    upscaleHint->setWordWrap(true);
    photoLayout->addWidget(upscaleHint);
    connect(m_upscaleCheck, &QCheckBox::toggled, this, &MainWindow::applyFilterSettings);

    // --- Kép forgatása: the photograph's own rotation inside its frame --------
    // The frame angle below turns the window; this turns the picture inside it.
    // One slider, the two quarter turns and a reset, each one undo step.
    addSectionTo(photoLayout, QCoreApplication::translate("PhotoPrint", "Kép forgatása"));
    auto *imageRotationRow = new QHBoxLayout;
    m_imageRotationSlider = new QSlider(Qt::Horizontal, photoTab);
    m_imageRotationSlider->setObjectName(QStringLiteral("imageRotationSlider"));
    m_imageRotationSlider->setAccessibleName(
        QCoreApplication::translate("PhotoPrint", "Kép forgatása"));
    m_imageRotationSlider->setRange(int(kMinImageRotationDeg), int(kMaxImageRotationDeg));
    m_imageRotationSlider->setSingleStep(1);
    m_imageRotationSlider->setPageStep(15);
    // Tracking off: the release is the action, so one drag is one undo step.
    m_imageRotationSlider->setTracking(false);
    m_imageRotationValue = new QLabel(QStringLiteral("0°"), photoTab);
    m_imageRotationValue->setObjectName(QStringLiteral("imageRotationValue"));
    m_imageRotationValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    markSecondaryText(m_imageRotationValue);
    imageRotationRow->addWidget(m_imageRotationSlider, 1);
    imageRotationRow->addWidget(m_imageRotationValue);
    photoLayout->addLayout(imageRotationRow);
    connect(m_imageRotationSlider, &QSlider::valueChanged, this,
            &MainWindow::imageRotationChanged);

    auto *imageTurnRow = new QHBoxLayout;
    m_imageRotateLeftButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Balra 90°"), photoTab);
    m_imageRotateLeftButton->setObjectName(QStringLiteral("imageRotateLeft"));
    m_imageRotateRightButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Jobbra 90°"), photoTab);
    m_imageRotateRightButton->setObjectName(QStringLiteral("imageRotateRight"));
    m_imageResetButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Alaphelyzet"), photoTab);
    m_imageResetButton->setObjectName(QStringLiteral("imageRotationReset"));
    m_imageResetButton->setToolTip(QCoreApplication::translate(
        "PhotoPrint", "Visszaállítja a kép nagyítását, helyét és forgatását."));
    imageTurnRow->addWidget(m_imageRotateLeftButton);
    imageTurnRow->addWidget(m_imageRotateRightButton);
    imageTurnRow->addWidget(m_imageResetButton);
    photoLayout->addLayout(imageTurnRow);
    connect(m_imageRotateLeftButton, &QPushButton::clicked, this,
            [this] { rotateImageBy(-90.0); });
    connect(m_imageRotateRightButton, &QPushButton::clicked, this,
            [this] { rotateImageBy(90.0); });
    connect(m_imageResetButton, &QPushButton::clicked, this,
            &MainWindow::resetImageInsideFrame);

    // --- Méret: visible, not folded under a toggle ---------------------------
    addSectionTo(photoLayout, QCoreApplication::translate("PhotoPrint", "Méret"));
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    m_widthSpin = new QDoubleSpinBox(photoTab);
    m_heightSpin = new QDoubleSpinBox(photoTab);
    m_angleSpin = new QDoubleSpinBox(photoTab);
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
    grid->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Szélesség"), photoTab), 0, 0);
    grid->addWidget(m_widthSpin, 0, 1);
    grid->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Magasság"), photoTab), 1, 0);
    grid->addWidget(m_heightSpin, 1, 1);
    grid->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Szög"), photoTab), 2, 0);
    grid->addWidget(m_angleSpin, 2, 1);
    photoLayout->addLayout(grid);
    for (QDoubleSpinBox *spin : {m_widthSpin, m_heightSpin, m_angleSpin})
        connect(spin, &QDoubleSpinBox::editingFinished, this, &MainWindow::applySizeFromControls);

    // A slotted frame can be moved, resized or turned out of its window. The
    // reset puts it back on the cell; the hint says when part of the picture is
    // clipped away.
    m_replaceInSlotButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Kép visszahelyezése"), photoTab);
    m_replaceInSlotButton->setObjectName(QStringLiteral("replaceInSlotButton"));
    m_replaceInSlotButton->setToolTip(
        QCoreApplication::translate("PhotoPrint", "A fotót a helyére igazítja."));
    m_replaceInSlotButton->setVisible(false);
    photoLayout->addWidget(m_replaceInSlotButton);
    connect(m_replaceInSlotButton, &QPushButton::clicked, this,
            &MainWindow::replaceFrameInSlot);

    m_clippedHint = new QLabel(
        QCoreApplication::translate("PhotoPrint", "A kép egy része le van vágva."), photoTab);
    m_clippedHint->setObjectName(QStringLiteral("clippedHint"));
    m_clippedHint->setWordWrap(true);
    markSecondaryText(m_clippedHint);
    m_clippedHint->setVisible(false);
    photoLayout->addWidget(m_clippedHint);

    m_resolutionHint = new QLabel(photoTab);
    m_resolutionHint->setWordWrap(true);
    m_resolutionHint->setStyleSheet(QStringLiteral("color: #8a5a00;"));
    photoLayout->addWidget(m_resolutionHint);

    m_trialButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Próbanyomat"), photoTab);
    m_trialButton->setObjectName(QStringLiteral("trialSheetButton"));
    photoLayout->addWidget(m_trialButton);
    connect(m_trialButton, &QPushButton::clicked, this, &MainWindow::showTrialSheet);

    // Many identical copies of the selected photograph. It sits under the test
    // sheet because both answer "how will this photo come out of the printer".
    m_batchButton =
        new QPushButton(QCoreApplication::translate("PhotoPrint", "Sokszorosítás…"), photoTab);
    m_batchButton->setObjectName(QStringLiteral("batchCopiesButton"));
    m_batchButton->setToolTip(QCoreApplication::translate(
        "PhotoPrint", "Ugyanaz a fotó több példányban, egyenes rácsban."));
    photoLayout->addWidget(m_batchButton);
    connect(m_batchButton, &QPushButton::clicked, this, &MainWindow::openBatchCopies);
    photoLayout->addStretch(1);

    sideLayout->addWidget(m_inspectorTabs);

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
    m_backgroundNotice = new QLabel(m_noticeBar);
    m_backgroundNotice->setObjectName(QStringLiteral("backgroundNotice"));
    m_backgroundNotice->setWordWrap(true);
    m_backgroundNotice->setVisible(false);
    noticeLayout->addWidget(m_backgroundNotice);
    // The same one-line notice for a sticker that cannot be shown: the page
    // carries its placeholder and this says why.
    m_stickerNotice = new QLabel(m_noticeBar);
    m_stickerNotice->setObjectName(QStringLiteral("stickerNotice"));
    m_stickerNotice->setWordWrap(true);
    m_stickerNotice->setVisible(false);
    noticeLayout->addWidget(m_stickerNotice);
    // The sentence alone is a dead end: the one repair lives three clicks deep
    // in the panel. This is the way in, and it selects the sticker so the
    // relink picker is already on screen.
    m_stickerNoticeButton = new QPushButton(
        QCoreApplication::translate("PhotoPrint", "Matrica kiválasztása"), m_noticeBar);
    m_stickerNoticeButton->setObjectName(QStringLiteral("stickerNoticeButton"));
    m_stickerNoticeButton->setVisible(false);
    noticeLayout->addWidget(m_stickerNoticeButton);
    connect(m_stickerNoticeButton, &QPushButton::clicked, this, [this] {
        const DocumentSnapshot document = m_controller.document();
        if (document.pages.isEmpty())
            return;
        for (const Sticker &sticker :
             OverlayGeometry::stickersInPaintOrder(document.pages.at(document.currentPage))) {
            if (stickerProblem(sticker) != StickerProblem::None) {
                selectSticker(sticker.id);
                break;
            }
        }
    });
    m_overflowLabel = new QLabel(QCoreApplication::translate("PhotoPrint", "A kép egy része lemarad."), m_noticeBar);
    m_overflowLabel->setObjectName(QStringLiteral("overflowLabel"));
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
    m_pageView->onStickerSelectionChanged = [this](const QString &stickerId) {
        selectSticker(stickerId);
    };
    m_pageView->onStickerCommitted = [this](const Sticker &sticker,
                                            PageView::StickerAction action) {
        // The gesture names the undo step; the page is still the source of what
        // is on it, so the list is taken from the document and one entry is
        // replaced.
        const QString text = action == PageView::StickerAction::Resize
            ? QCoreApplication::translate("PhotoPrint", "Matrica átméretezése")
            : (action == PageView::StickerAction::Rotate
                   ? QCoreApplication::translate("PhotoPrint", "Matrica forgatása")
                   : QCoreApplication::translate("PhotoPrint", "Matrica mozgatása"));
        const DocumentSnapshot document = m_controller.document();
        if (document.pages.isEmpty())
            return;
        const Page &page = document.pages.at(document.currentPage);
        publishStickers(withSticker(page.stickers, sticker), text);
    };
    m_pageView->onStickerDeleteRequested = [this](const QString &stickerId) {
        Q_UNUSED(stickerId)
        deleteSelectedSticker();
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

    // The tab bar is the top of this column and never scrolls away. Each tab page
    // owns its scroll area instead, so one page's offset can never be another
    // page's starting position.
    side->setMinimumHeight(0);
    sideLayout->activate();
    int contentWidth = std::max(240, side->minimumSizeHint().width());
    if (m_inspectorTabs) {
        contentWidth = std::max(contentWidth, m_inspectorTabs->minimumSizeHint().width());
        for (int index = 0; index < m_inspectorTabs->count(); ++index) {
            auto *page = qobject_cast<QScrollArea *>(m_inspectorTabs->widget(index));
            if (!page || !page->widget())
                continue;
            if (QLayout *pageLayout = page->widget()->layout())
                pageLayout->activate();
            // The page's content, plus the per-page scroll bar and the tab frame
            // around it: a horizontal scroll bar is never offered, so the column
            // has to be wide enough for the widest page on its own.
            const int scrollBarWidth = page->verticalScrollBar()->sizeHint().width();
            contentWidth = std::max(contentWidth,
                                    page->widget()->minimumSizeHint().width()
                                        + scrollBarWidth + 24);
        }
    }
    // The column is never narrower than its own content. At the default font
    // that is the old 240 px; at a larger system font the buttons and labels need
    // more, and no horizontal scroll bar is offered, so without this they would
    // be clipped instead of laid out.
    side->setMinimumWidth(std::max(240, contentWidth));
    side->setMaximumWidth(std::max(330, contentWidth + 40));
    m_inspectorPanel = side;
    layout->addWidget(library);
    layout->addWidget(m_pageView, 1);
    layout->addWidget(side);

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
    QAction *remove = editMenu->addAction(QCoreApplication::translate("PhotoPrint", "Kiválasztott elem eltávolítása"));
    remove->setShortcut(QKeySequence::Delete);
    connect(remove, &QAction::triggered, this, &MainWindow::deleteSelected);

    editMenu->addSeparator();
    QAction *templates =
        editMenu->addAction(QCoreApplication::translate("PhotoPrint", "Sablonok…"));
    templates->setObjectName(QStringLiteral("templatesAction"));
    connect(templates, &QAction::triggered, this, &MainWindow::openTemplateBrowser);

    // Inserting a decorative layer is an action on the page, so it sits in the
    // menu bar and not only in a tab: it is reachable without switching tabs.
    QMenu *insertMenu = menuBar()->addMenu(QCoreApplication::translate("PhotoPrint", "Beszúrás"));
    insertMenu->setObjectName(QStringLiteral("insertMenu"));
    QAction *stickerAction =
        insertMenu->addAction(QCoreApplication::translate("PhotoPrint", "Matrica…"));
    stickerAction->setObjectName(QStringLiteral("insertStickerAction"));
    connect(stickerAction, &QAction::triggered, this, &MainWindow::addSticker);

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
    // Everything the user was looking at survives the language change, including
    // which inspector tab was open and whether it was a manual choice.
    const QString selected = m_selectedId;
    const QString selectedSticker = m_selectedSticker;
    const bool dirty = m_dirty;
    const int copies = m_copiesSpin ? m_copiesSpin->value() : 1;
    const double zoom = m_pageView ? m_pageView->viewZoom() : 1.0;
    const bool landscape = m_landscape;
    const int inspectorTab = m_inspectorTabs ? m_inspectorTabs->currentIndex() : -1;
    const bool inspectorTabChosen = m_inspectorTabChosen;
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
    m_inspectorTabs = nullptr;
    m_layoutPanel = nullptr;
    m_pageSummaryPanel = nullptr;
    m_pageInfo = nullptr;
    m_stickerAddPanel = nullptr;
    m_stickerPanel = nullptr;
    m_stickerEmptyPanel = nullptr;
    m_stickerEmptyHint = nullptr;
    m_stickerPreview = nullptr;
    m_stickerSizeSpin = nullptr;
    m_stickerAngleSpin = nullptr;
    m_stickerOpacitySlider = nullptr;
    m_stickerOpacityValue = nullptr;
    m_stickerRaiseButton = nullptr;
    m_stickerLowerButton = nullptr;
    m_stickerDeleteButton = nullptr;
    m_addStickerButton = nullptr;
    m_stickerSetGrid = nullptr;
    m_stickerPositionLabel = nullptr;
    m_previousStickerButton = nullptr;
    m_nextStickerButton = nullptr;
    m_stickerMissingHint = nullptr;
    m_stickerRelinkButton = nullptr;
    m_stickerNotice = nullptr;
    m_stickerNoticeButton = nullptr;
    m_stickerPreviewPath.clear();
    m_slotSizeValue = nullptr;
    m_slotPositionValue = nullptr;
    m_slotStateValue = nullptr;
    m_slotClipValue = nullptr;
    m_slotRemoveButton = nullptr;
    m_inspectorContext.clear();
    m_inspectorTabChosen = false;
    m_settingInspectorTab = false;
    m_printButton = nullptr;
    m_printerCombo = nullptr;

    m_selectedId = selected;
    m_selectedSticker = selectedSticker;
    buildUi();
    buildMenus();

    refreshPrinters();

    const bool wasRestoring = m_restoringProject;
    m_restoringProject = true;
    onDocumentChanged();
    m_restoringProject = wasRestoring;
    // The rebuilt tabs start on the auto-selected page. Put the one the user was
    // reading back, when it still has content; a manual choice survives the
    // language change just like it survives a slider drag.
    if (m_inspectorTabs && inspectorTab >= 0
        && m_inspectorTabs->isTabEnabled(inspectorTab)) {
        setInspectorTab(inspectorTab);
        m_inspectorTabChosen = inspectorTabChosen;
    }
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

void MainWindow::addPhotosToSelectedSlot()
{
    if (m_selectedSlotId.isEmpty())
        return;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QCoreApplication::translate("PhotoPrint", "Fotók hozzáadása"), QString(),
        QCoreApplication::translate("PhotoPrint", "Fotók (*.jpg *.jpeg *.png *.JPG *.JPEG *.PNG)"));
    if (!paths.isEmpty())
        addFilesToSelectedSlot(paths);
}

void MainWindow::addFilesToSelectedSlot(const QStringList &paths)
{
    // The Hely tab is about one section. A file picked there lands in that
    // section, not in whichever empty cell happens to come first.
    if (m_selectedSlotId.isEmpty())
        return;
    addFilesAt(paths, m_selectedSlotId);
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
    cancelPendingBackground();
    cancelPendingStickerOpacity();
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
    m_pageView->setPageImages(displayedPageImages());

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
    // A sticker that is no longer on this page cannot stay selected.
    if (!m_selectedSticker.isEmpty()) {
        bool found = false;
        if (page) {
            for (const Sticker &sticker : page->stickers) {
                if (sticker.id == m_selectedSticker) {
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            m_selectedSticker.clear();
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
    m_pageView->setSelectedSticker(m_selectedSticker);
    m_pageView->setUnavailableFrames(m_unavailableFrames);
    m_pageView->setMissingStickers(m_missingStickers);

    // A laid-out page carries its own drop hints on the cells; the page-wide
    // line would draw a second instruction over them. A page that already has
    // a sticker is not empty either: the plate would cover the layer and tell
    // the user to add photographs on top of it.
    m_pageView->setEmptyHint(
        !page || hasLayout
            ? QString()
            : (page->frames.isEmpty() && page->stickers.isEmpty()
                   ? QCoreApplication::translate(
                         "PhotoPrint",
                         "Húzd ide a fotókat, vagy válaszd a Fotók hozzáadása gombot.")
                   : QString()));
    const bool overflows = pageOverflows();
    m_overflowLabel->setVisible(overflows);
    // "Igazítás a laphoz" scales free photographs so they land on the paper. On
    // a laid-out page the photographs are clipped to cells that already fit the
    // sheet, and fitting would only break the composition inside a section, so
    // the entry stays hidden.
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
        // The background is loaded by the same job, so the canvas and the print
        // cannot disagree about whether the page has one. A missing or
        // unreadable file leaves the layer null and the page white; the panel
        // and the notice bar say why.
        const PageBackground &background = page.background;
        if (background.kind == BackgroundKind::Image && !background.imagePath.isEmpty()
            && !token.stop_requested()) {
            QImageReader probe(background.imagePath);
            probe.setDecideFormatFromContent(true);
            if (probe.canRead()
                && !BackgroundGeometry::backgroundSourceTooLarge(probe.size())) {
                Result<SourceImage> source =
                    store->load(background.imagePath, ImportPolicy{}, token);
                if (source) {
                    ColorRecipe recipe = sanitiseRecipe(job->document.recipe);
                    recipe.tone = PhotoTone{};
                    Result<QImage> converted = store->linear(
                        source.value(), recipe, kBackgroundPreviewBound, token);
                    if (converted) {
                        result.background = converted.value();
                        result.backgroundPixels = source.value().size();
                        result.backgroundPath = background.imagePath;
                    }
                }
            }
        }
        // The stickers are loaded by the same helper the print renderer uses, so
        // the canvas and the print cannot disagree about what is on the page. A
        // missing or unreadable picture is remembered, and the canvas draws its
        // own placeholder for it.
        const QList<Sticker> stickers = sanitiseStickers(page.stickers, pageSizeMm(page));
        for (const Sticker &sticker : stickers) {
            if (token.stop_requested())
                break;
            int bound = qRound(std::max(sticker.width, sticker.height) / 25.4 * 300.0);
            bound = std::clamp(bound, 1, kStickerPreviewBound);
            const StickerLayer layer = loadStickerLayer(*store, sticker,
                                                        job->document.recipe, bound, token);
            if (layer.missing)
                result.missingStickers.insert(sticker.id);
            else
                result.stickers.insert(sticker.id, layer.image);
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
    m_previewBackground = result.background;
    m_previewBackgroundPixels = result.backgroundPixels;
    m_previewBackgroundPath = result.backgroundPath;
    m_previewStickers = result.stickers;
    m_missingStickers = result.missingStickers;
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
        m_pageView->setMissingStickers(m_missingStickers);
        m_pageView->setPageImages(displayedPageImages());
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
        m_pageView->setPageImages(displayedPageImages());
}

PageImages MainWindow::displayedPageImages() const
{
    PageImages images;
    images.frames = m_previewImages;
    // Sticker layers are keyed by id, so a layer for a sticker on another page
    // is simply never looked up. The canvas holds its own missing set and draws
    // the placeholder; the page painter never does.
    images.stickers = m_previewStickers;
    const DocumentSnapshot document = m_controller.document();
    if (!document.pages.isEmpty()) {
        const Page &page = document.pages.at(document.currentPage);
        // The background is not keyed by an id, so a layer loaded for another
        // page must not be stretched over this one while the new job runs.
        if (page.background.kind == BackgroundKind::Image
            && !page.background.imagePath.isEmpty()
            && page.background.imagePath == m_previewBackgroundPath) {
            images.background = m_previewBackground;
            images.backgroundPixels = m_previewBackgroundPixels;
        }
    }
    if (!m_showOriginal || m_selectedId.isEmpty())
        return images;
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return images;
    const auto original = m_originalPreviews.constFind(sourceKeyFor(*frame));
    if (original == m_originalPreviews.constEnd())
        return images;
    images.frames.insert(m_selectedId, original.value());
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
    const bool stickerChanged = !m_selectedSticker.isEmpty();
    m_selectedSticker.clear();
    if (m_selectedId == frameId && m_selectedSlotId == slotId && !stickerChanged)
        return;
    m_selectedId = frameId;
    m_selectedSlotId = slotId;
    if (m_pageView) {
        m_pageView->setSelectedSticker(QString());
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
    // A cell and a sticker are exclusive selections: the cell panel only talks
    // about a section that is really the current one.
    const bool stickerChanged = !m_selectedSticker.isEmpty();
    m_selectedSticker.clear();
    if (slotId.isEmpty()) {
        if (m_selectedSlotId.isEmpty() && !stickerChanged)
            return;
        m_selectedSlotId.clear();
        if (m_pageView) {
            m_pageView->setSelectedSticker(QString());
            m_pageView->setSelectedSlot(QString());
        }
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
    if (m_selectedSlotId == slotId && m_selectedId.isEmpty() && !stickerChanged)
        return;
    m_selectedSlotId = slotId;
    m_selectedId.clear();
    if (m_pageView) {
        m_pageView->setSelectedSticker(QString());
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

std::optional<Sticker> MainWindow::selectedSticker() const
{
    if (m_selectedSticker.isEmpty())
        return std::nullopt;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return std::nullopt;
    const Page &page = document.pages.at(document.currentPage);
    for (const Sticker &sticker : page.stickers) {
        if (sticker.id == m_selectedSticker)
            return sticker;
    }
    return std::nullopt;
}

void MainWindow::selectSticker(const QString &stickerId)
{
    const bool sameSticker = m_selectedSticker == stickerId;
    const bool otherSelection = !m_selectedId.isEmpty() || !m_selectedSlotId.isEmpty();
    m_selectedSticker = stickerId;
    if (!stickerId.isEmpty()) {
        // An overlay and a photograph are exclusive selections: the picture's
        // crop controls must never act on the photo under the sticker.
        m_selectedId.clear();
        m_selectedSlotId.clear();
    }
    if (m_pageView) {
        if (!stickerId.isEmpty()) {
            m_pageView->setSelectedFrame(QString());
            m_pageView->setSelectedSlot(QString());
        }
        m_pageView->setSelectedSticker(stickerId);
        if (!stickerId.isEmpty() && m_cropButton->isChecked()) {
            m_cropButton->blockSignals(true);
            m_cropButton->setChecked(false);
            m_cropButton->blockSignals(false);
            m_pageView->setCropMode(false);
            m_cropButton->setText(QCoreApplication::translate("PhotoPrint", "Kivágás"));
        }
    }
    if (sameSticker && !otherSelection)
        return;
    syncListSelection();
    syncFrameControls();
}

void MainWindow::flushPendingStickerEdit()
{
    if (m_pageView)
        m_pageView->flushStickerBurst();
}

bool MainWindow::publishStickers(const QList<Sticker> &stickers, const QString &actionText,
                                 const QString &selectId)
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return false;
    Page page = document.pages.at(document.currentPage);
    if (page.stickers == stickers && selectId.isEmpty())
        return true;
    page.stickers = stickers;
    const auto published = m_controller.publishPage(page, actionText);
    if (!published) {
        showProblem(published.error().message);
        syncStickerPanel();
        return false;
    }
    if (!selectId.isEmpty())
        selectSticker(selectId);
    return true;
}

void MainWindow::syncFrameControls()
{
    // One snapshot for the whole sync: document() returns by value, so each
    // extra selectedFrame() call copies every page and frame again.
    const DocumentSnapshot document = m_controller.document();
    const Frame *frame =
        m_selectedId.isEmpty() ? nullptr : findFrame(document, m_selectedId);
    const bool hasSelection = frame != nullptr;
    const Page *currentPage =
        document.pages.isEmpty() ? nullptr : &document.pages.at(document.currentPage);
    // The selected section: a photo also selects the cell it sits in, so Hely can
    // describe that cell when the user opens it. `emptySection` is what makes the
    // tab open by itself; a full cell keeps Fotó in front.
    const QString selectedSlotId =
        (currentPage && !m_selectedSlotId.isEmpty()
         && SlotGeometry::slotById(*currentPage, m_selectedSlotId))
            ? m_selectedSlotId
            : QString();
    const bool emptySection = !hasSelection && !selectedSlotId.isEmpty();
    if (m_inspectorPanel)
        m_inspectorPanel->setVisible(!document.pages.isEmpty());
    updateInspectorTabs(hasSelection ? frame->id : QString(), selectedSlotId, emptySection,
                        m_selectedSticker);
    if (currentPage)
        syncSlotFacts(*currentPage, selectedSlotId);
    if (m_slotRemoveButton)
        m_slotRemoveButton->setEnabled(!selectedSlotId.isEmpty());
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
    if (m_batchButton)
        m_batchButton->setEnabled(hasSelection);
    m_widthSpin->setEnabled(hasSelection);
    m_heightSpin->setEnabled(hasSelection);
    m_angleSpin->setEnabled(hasSelection);
    if (m_imageRotationSlider)
        m_imageRotationSlider->setEnabled(hasSelection);
    if (m_imageRotateLeftButton)
        m_imageRotateLeftButton->setEnabled(hasSelection);
    if (m_imageRotateRightButton)
        m_imageRotateRightButton->setEnabled(hasSelection);
    if (m_imageResetButton)
        m_imageResetButton->setEnabled(hasSelection);

    m_syncing = true;
    m_resolutionHint->clear();
    if (frame) {
        m_widthSpin->setValue(frame->width / 10.0);
        m_heightSpin->setValue(frame->height / 10.0);
        m_angleSpin->setValue(frame->angleDeg);
        syncImageRotationControls(*frame);
        const bool slotted = !frame->slotId.isEmpty();
        if (m_replaceInSlotButton)
            m_replaceInSlotButton->setVisible(slotted);
        if (m_clippedHint) {
            const bool clipped = slotted && currentPage
                && SlotGeometry::frameIsClipped(*currentPage, *frame);
            m_clippedHint->setVisible(clipped);
        }
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
        if (m_replaceInSlotButton)
            m_replaceInSlotButton->setVisible(false);
        if (m_clippedHint)
            m_clippedHint->setVisible(false);
    }
    m_syncing = false;
    syncLayoutPanel();
    syncBackgroundPanel();
    syncStickerPanel();
}

void MainWindow::syncSlotFacts(const Page &page, const QString &slotId)
{
    if (!m_slotSizeValue)
        return;
    const Slot *slot = slotId.isEmpty() ? nullptr : SlotGeometry::slotById(page, slotId);
    if (!slot) {
        m_slotSizeValue->clear();
        m_slotPositionValue->clear();
        m_slotStateValue->clear();
        m_slotClipValue->clear();
        if (m_slotFillButton)
            m_slotFillButton->setVisible(false);
        return;
    }
    const QRectF cell = SlotGeometry::slotCellMm(page, *slot);
    // A millimetre pair is digits and a unit: the same technical shape in every
    // language, which is why it is not a catalogue entry. The comma matches the
    // paper sizes the rest of the interface prints.
    const auto millimetres = [](double value) {
        QString text = QString::number(value, 'f', 1);
        text.replace(QLatin1Char('.'), QLatin1Char(','));
        return text;
    };
    m_slotSizeValue->setText(QStringLiteral("%1 × %2 mm")
                                 .arg(millimetres(cell.width()),
                                      millimetres(cell.height())));
    int position = 0;
    for (int index = 0; index < page.slotList.size(); ++index) {
        if (page.slotList.at(index).id == slotId) {
            position = index + 1;
            break;
        }
    }
    m_slotPositionValue->setText(
        QStringLiteral("%1 / %2").arg(position).arg(page.slotList.size()));
    const Frame *occupant = SlotGeometry::frameInSlot(page, slotId);
    m_slotStateValue->setText(occupant
                                  ? QCoreApplication::translate("PhotoPrint", "Foglalt")
                                  : QCoreApplication::translate("PhotoPrint", "Üres"));
    const bool clipped = occupant && SlotGeometry::frameIsClipped(page, *occupant);
    m_slotClipValue->setText(clipped
                                 ? QCoreApplication::translate("PhotoPrint", "Levágva")
                                 : QCoreApplication::translate("PhotoPrint", "Nincs"));
    // Replacing a filled cell's picture is the canvas drop or the Fotó tab, not a
    // picker here. Hiding the button keeps the tab honest for a full cell.
    if (m_slotFillButton)
        m_slotFillButton->setVisible(occupant == nullptr);
}

void MainWindow::setInspectorTab(int index)
{
    if (!m_inspectorTabs || index < 0 || index >= m_inspectorTabs->count())
        return;
    if (!m_inspectorTabs->isTabEnabled(index))
        return;
    if (m_inspectorTabs->currentIndex() == index)
        return;
    // The signal below is what normally records a manual choice; this switch is
    // the programmatic one, so it must not be mistaken for the user.
    const bool wasSetting = m_settingInspectorTab;
    m_settingInspectorTab = true;
    m_inspectorTabs->setCurrentIndex(index);
    m_settingInspectorTab = wasSetting;
}

void MainWindow::updateInspectorTabs(const QString &frameId, const QString &slotId,
                                     bool emptySection, const QString &stickerId)
{
    if (!m_inspectorTabs)
        return;

    const bool hasPhoto = !frameId.isEmpty();
    const bool hasSection = !slotId.isEmpty();
    const bool hasOverlay = !stickerId.isEmpty();
    // The context is the thing that changes the meaning of the panel: the same
    // photograph after a slider edit is not a new context, another photograph or
    // another section is. Without this a value change would drag the panel back
    // from Lap while the user is reading it.
    const QString context = hasOverlay
        ? QStringLiteral("sticker:") + stickerId
        : (hasPhoto ? QStringLiteral("photo:") + frameId
                    : (hasSection ? QStringLiteral("section:") + slotId
                                  : QStringLiteral("page")));
    const QString previousContext = m_inspectorContext;
    const bool contextChanged = context != m_inspectorContext;
    m_inspectorContext = context;

    // A tab with no content is disabled, never removed: the indices stay fixed
    // and Lap is always reachable. Elem is the exception: it is always enabled,
    // because with nothing selected it is where a sticker is added from. Hely
    // follows the selected section, which for a photograph is the cell it sits
    // in, so the section's facts and its remove action stay reachable even when
    // the cell is full.
    const int previous = m_inspectorTabs->currentIndex();
    m_inspectorTabs->setTabEnabled(0, hasPhoto);
    m_inspectorTabs->setTabEnabled(1, hasSection);
    m_inspectorTabs->setTabEnabled(2, true);
    m_inspectorTabs->setTabEnabled(3, true);

    if (contextChanged)
        m_inspectorTabChosen = false;

    int wanted = previous;
    const bool previousUsable =
        previous >= 0 && m_inspectorTabs->isTabEnabled(previous);
    // Deselecting a sticker by clicking empty paper is part of decorating, not
    // a request to leave the palette: a stray click must not throw the panel
    // back to Lap mid-flow. A deselect from a photograph still goes to Lap.
    const bool keepElem = context == QLatin1String("page")
        && previousContext.startsWith(QStringLiteral("sticker:"))
        && previous == 3;
    if (keepElem) {
        wanted = 3;
    } else if (!previousUsable || (!m_inspectorTabChosen)) {
        // The automatic page, in the order a document is worked on: the overlay
        // that was picked, the photo, the empty section, the sheet itself.
        wanted = hasOverlay ? 3 : (hasPhoto ? 0 : (emptySection ? 1 : 2));
    }
    if (wanted != m_inspectorTabs->currentIndex())
        setInspectorTab(wanted);
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
    // A page without a layout shows one line saying so instead of a column of
    // controls that could only clamp to zero. A brand-new document is empty on
    // purpose: there is nothing to explain yet, so the line stays hidden.
    if (m_pageSummaryPanel)
        m_pageSummaryPanel->setVisible(page && !hasLayout && !page->frames.isEmpty());
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
    // The catalogue name is English by design, the frame around it is the
    // interface language: "Elrendezés: Four equal photos in a 2x2 grid".
    m_layoutNameLabel->setText(
        QCoreApplication::translate("PhotoPrint", "Elrendezés: %1")
            .arg(layoutDisplayName(*page)));
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
    m_syncing = wasSyncing;
}

void MainWindow::syncPageTools()
{
    if (!m_guidesCombo || !m_alignButton)
        return;
    const DocumentSnapshot document = m_controller.document();
    const Page *page = document.pages.isEmpty() ? nullptr
                                                : &document.pages.at(document.currentPage);
    const bool canArrange = page && !page->frames.isEmpty() && !page->hasLayout();
    // Arranging free photographs has no meaning on a laid-out page, and a greyed
    // button that can never be used is bulk. Hide it instead of deadening it.
    m_alignButton->setVisible(canArrange);
    m_alignButton->setEnabled(canArrange);
    m_alignButton->setToolTip(QCoreApplication::translate(
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
        // A slotted photograph is clipped to its cell, which is inside the
        // sheet by construction, so its own rectangle can never overflow the
        // paper even when the user made it larger than the window.
        if (!frame.slotId.isEmpty())
            continue;
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
    // A sticker is deliberately allowed to bleed over the edge: that is a
    // decoration, not a photograph the user meant to keep whole. Counting it
    // here would warn about "a part of the picture is cut" and offer a fit that
    // shrinks perfectly placed photos to chase a decoration.
    return false;
}

void MainWindow::deleteSelected()
{
    // The canvas and the Edit menu share one delete: whatever is selected goes.
    // A sticker is an overlay above the photographs, so it wins the selection.
    if (!m_selectedSticker.isEmpty()) {
        deleteSelectedSticker();
        return;
    }
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
    // A slotted frame owns its rectangle exactly like a free one: the slot is a
    // clipping window, so the user may make the photograph smaller, larger,
    // move it or turn it inside the cell.
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
    if (!m_lastStickerDir.isEmpty())
        settings.setValue(QStringLiteral("ui/lastStickerDir"), m_lastStickerDir);
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
    m_lastStickerDir = settings.value(QStringLiteral("ui/lastStickerDir")).toString();

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
    m_selectedSticker.clear();
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
    m_selectedSticker.clear();
}

void MainWindow::duplicatePage()
{
    m_controller.apply(DuplicatePageEdit{m_controller.document().currentPage});
    m_selectedId.clear();
    m_selectedSlotId.clear();
    m_selectedSticker.clear();
}

void MainWindow::removePage()
{
    m_controller.apply(RemovePageEdit{m_controller.document().currentPage});
    m_selectedId.clear();
    m_selectedSlotId.clear();
    m_selectedSticker.clear();
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

void MainWindow::syncImageRotationControls(const Frame &frame)
{
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    const int degrees = int(std::lround(std::clamp(frame.imageRotationDeg,
                                                   kMinImageRotationDeg,
                                                   kMaxImageRotationDeg)));
    if (m_imageRotationSlider && !m_imageRotationSlider->isSliderDown())
        m_imageRotationSlider->setValue(degrees);
    if (m_imageRotationValue)
        m_imageRotationValue->setText(QStringLiteral("%1°").arg(degrees));
    m_syncing = wasSyncing;
}

void MainWindow::imageRotationChanged(int degrees)
{
    if (m_syncing)
        return;
    // A wheel burst that has not settled yet becomes its own edit first, so
    // the undo stack keeps one step per gesture.
    if (m_pageView)
        m_pageView->flushWheelBurst();
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    const QSize pixels =
        m_previewImages.contains(frame->id) ? m_previewImages.value(frame->id).size() : QSize();
    const Frame updated = PhotoItem::rotateImageTo(*frame, degrees, pixels);
    if (updated != *frame)
        m_controller.apply(FrameChange{updated.id, updated});
    syncImageRotationControls(updated);
}

void MainWindow::rotateImageBy(double degrees)
{
    if (m_syncing)
        return;
    // A wheel burst that has not settled yet becomes its own edit first, so
    // the undo stack keeps one step per gesture.
    if (m_pageView)
        m_pageView->flushWheelBurst();
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    const QSize pixels =
        m_previewImages.contains(frame->id) ? m_previewImages.value(frame->id).size() : QSize();
    const Frame updated = PhotoItem::rotateImageBy(*frame, degrees, pixels);
    if (updated != *frame)
        m_controller.apply(FrameChange{updated.id, updated});
    syncImageRotationControls(updated);
}

void MainWindow::resetImageInsideFrame()
{
    if (m_syncing)
        return;
    // A wheel burst that has not settled yet becomes its own edit first, so
    // the undo stack keeps one step per gesture.
    if (m_pageView)
        m_pageView->flushWheelBurst();
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
        return;
    const Frame updated = PhotoItem::resetInsideFrame(*frame);
    if (updated != *frame)
        m_controller.apply(FrameChange{updated.id, updated});
    syncImageRotationControls(updated);
}

void MainWindow::replaceFrameInSlot()
{
    if (m_syncing)
        return;
    // A wheel burst that has not settled yet becomes its own edit first, so
    // the undo stack keeps one step per gesture.
    if (m_pageView)
        m_pageView->flushWheelBurst();
    const std::optional<Frame> frame = selectedFrame();
    if (!frame || frame->slotId.isEmpty())
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    const Slot *slot = SlotGeometry::slotById(page, frame->slotId);
    if (!slot)
        return;
    Frame updated = *frame;
    SlotGeometry::initialiseFrameInSlot(page, *slot, updated);
    updated.autoPlaced = false;
    if (updated != *frame)
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
    m_pageView->setPageImages(displayedPageImages());
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

PageBackground MainWindow::currentBackgroundFor(BackgroundField field)
{
    // A burst belongs to the control it was started in. Moving to another
    // control lands the burst first, so the new edit builds on the value the
    // user just set instead of overwriting it with a stale copy of the page.
    // This is the background counterpart of queueSlotStyle's field guard.
    if (m_pendingBackgroundField && *m_pendingBackgroundField != field)
        flushBackground();
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return {};
    return document.pages.at(document.currentPage).background;
}

void MainWindow::queueBackground(const PageBackground &background, BackgroundField field)
{
    if (m_syncing)
        return;
    if (m_pendingBackgroundField && *m_pendingBackgroundField != field)
        flushBackground();
    m_pendingBackground = sanitiseBackground(background);
    m_pendingBackgroundField = field;
    if (m_backgroundTimer)
        m_backgroundTimer->start();
}

void MainWindow::flushBackground()
{
    if (m_backgroundTimer)
        m_backgroundTimer->stop();
    if (!m_pendingBackground)
        return;
    const PageBackground background = *m_pendingBackground;
    m_pendingBackground.reset();
    m_pendingBackgroundField.reset();
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    if (page.background == background)
        return;
    Page updated = page;
    updated.background = background;
    const auto published = m_controller.publishPage(
        updated, QCoreApplication::translate("PhotoPrint", "Háttér"));
    if (!published) {
        showProblem(published.error().message);
        syncBackgroundPanel();
    }
}

void MainWindow::cancelPendingBackground()
{
    if (m_backgroundTimer)
        m_backgroundTimer->stop();
    m_pendingBackground.reset();
    m_pendingBackgroundField.reset();
}

void MainWindow::backgroundKindChanged(int index)
{
    if (m_syncing || index < 0)
        return;
    if (m_controller.document().pages.isEmpty())
        return;
    PageBackground background = currentBackgroundFor(BackgroundField::Kind);
    background.kind =
        static_cast<BackgroundKind>(m_backgroundKindCombo->itemData(index).toInt());
    // A new solid starts on a visible neutral instead of the default white, so
    // choosing "Szín" changes the page at once. A gradient with no stops starts
    // as white to white; the two swatches are the next step.
    if (background.kind == BackgroundKind::Solid && background.solid == 0xffffffffu)
        background.solid = kNeutralBackgroundSolid;
    if (background.kind == BackgroundKind::Gradient && background.stops.size() < 2) {
        background.stops = {BackgroundStop{0.0, 0xffffffffu},
                            BackgroundStop{1.0, 0xffffffffu}};
    }
    queueBackground(background, BackgroundField::Kind);
    flushBackground();
}

void MainWindow::chooseBackgroundColor()
{
    if (m_controller.document().pages.isEmpty())
        return;
    PageBackground background = currentBackgroundFor(BackgroundField::Solid);
    const QColor chosen = QColorDialog::getColor(
        QColor(background.solid), this,
        QCoreApplication::translate("PhotoPrint", "Háttér színe"));
    if (!chosen.isValid())
        return;
    background.solid = 0xff000000u | (chosen.rgb() & 0x00ffffffu);
    queueBackground(background, BackgroundField::Solid);
    flushBackground();
}

void MainWindow::chooseGradientStart()
{
    chooseGradientStop(0);
}

void MainWindow::chooseGradientEnd()
{
    chooseGradientStop(1);
}

void MainWindow::chooseGradientStop(int which)
{
    if (m_controller.document().pages.isEmpty())
        return;
    PageBackground background = currentBackgroundFor(BackgroundField::Stops);
    if (background.stops.size() < 2) {
        background.stops = {BackgroundStop{0.0, 0xffffffffu},
                            BackgroundStop{1.0, 0xffffffffu}};
    }
    const int index = which == 0 ? 0 : int(background.stops.size()) - 1;
    const QString title = which == 0
        ? QCoreApplication::translate("PhotoPrint", "Kezdő szín")
        : QCoreApplication::translate("PhotoPrint", "Záró szín");
    const QColor chosen =
        QColorDialog::getColor(QColor(background.stops.at(index).second), this, title);
    if (!chosen.isValid())
        return;
    background.stops[index].second = 0xff000000u | (chosen.rgb() & 0x00ffffffu);
    background.kind = BackgroundKind::Gradient;
    queueBackground(background, BackgroundField::Stops);
    flushBackground();
}

void MainWindow::gradientAngleChanged(int index)
{
    if (m_syncing || index < 0)
        return;
    if (m_controller.document().pages.isEmpty())
        return;
    PageBackground background = currentBackgroundFor(BackgroundField::Angle);
    background.angleDeg = m_gradientAngleCombo->itemData(index).toDouble();
    background.kind = BackgroundKind::Gradient;
    queueBackground(background, BackgroundField::Angle);
    flushBackground();
}

void MainWindow::chooseBackgroundImage()
{
    if (m_controller.document().pages.isEmpty())
        return;
    PageBackground background = currentBackgroundFor(BackgroundField::Image);
    const QString startDir = background.imagePath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
        : QFileInfo(background.imagePath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, QCoreApplication::translate("PhotoPrint", "Háttérkép kiválasztása…"),
        startDir,
        QCoreApplication::translate("PhotoPrint",
                                    "Képek (*.jpg *.jpeg *.png *.webp *.bmp)"));
    if (path.isEmpty())
        return;
    const auto loaded = m_store.load(path, ImportPolicy{});
    if (!loaded) {
        showProblem(loaded.error().message);
        return;
    }
    background.kind = BackgroundKind::Image;
    background.imagePath = loaded.value().path;
    background.imageSha256 = loaded.value().sha256;
    queueBackground(background, BackgroundField::Image);
    flushBackground();
}

void MainWindow::backgroundFitChanged(int index)
{
    if (m_syncing || index < 0)
        return;
    if (m_controller.document().pages.isEmpty())
        return;
    PageBackground background = currentBackgroundFor(BackgroundField::Fit);
    background.fit =
        static_cast<BackgroundFit>(m_backgroundFitCombo->itemData(index).toInt());
    background.kind = BackgroundKind::Image;
    queueBackground(background, BackgroundField::Fit);
    flushBackground();
}

void MainWindow::backgroundTileChanged(double value)
{
    if (m_syncing)
        return;
    if (m_controller.document().pages.isEmpty())
        return;
    PageBackground background = currentBackgroundFor(BackgroundField::Tile);
    // The panel is in centimetres, the model in millimetres. Zero is the
    // special value: the picture's natural size at 300 ppi.
    background.tileMm = value * 10.0;
    queueBackground(background, BackgroundField::Tile);
}

void MainWindow::backgroundOpacityChanged(int value)
{
    if (m_syncing)
        return;
    if (m_controller.document().pages.isEmpty())
        return;
    PageBackground background = currentBackgroundFor(BackgroundField::Opacity);
    background.opacity = value;
    if (m_backgroundOpacityValue)
        m_backgroundOpacityValue->setText(QStringLiteral("%1 %").arg(value));
    queueBackground(background, BackgroundField::Opacity);
}

void MainWindow::resetBackground()
{
    if (m_controller.document().pages.isEmpty())
        return;
    PageBackground background = currentBackgroundFor(BackgroundField::Kind);
    // Back to the white sheet. The colour, the stops and the picture path are
    // kept, so switching back restores what the user had; the presentation
    // fields are reset so the next background starts at full strength and the
    // default fit instead of inheriting a half-transparent tile.
    background.kind = BackgroundKind::None;
    background.opacity = 100.0;
    background.fit = BackgroundFit::Fill;
    background.tileMm = 0.0;
    queueBackground(background, BackgroundField::Kind);
    flushBackground();
}

void MainWindow::addSticker()
{
    if (m_controller.document().pages.isEmpty())
        return;
    const QString startDir = m_lastStickerDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
        : m_lastStickerDir;
    const QString path = QFileDialog::getOpenFileName(
        this, QCoreApplication::translate("PhotoPrint", "Matrica hozzáadása…"), startDir,
        QCoreApplication::translate("PhotoPrint", "Képek (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty())
        return;
    m_lastStickerDir = QFileInfo(path).absolutePath();
    addStickerFromPath(path);
}

void MainWindow::addStickerFromPath(const QString &path)
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty() || path.isEmpty())
        return;
    // A picture too large to decode is refused before it reaches the store: the
    // decode would fail anyway, and the panel can say why.
    QImageReader probe(path);
    probe.setDecideFormatFromContent(true);
    if (!probe.canRead() || OverlayGeometry::stickerSourceTooLarge(probe.size())) {
        showProblem(QCoreApplication::translate(
            "PhotoPrint", "A matrica túl nagy vagy nem olvasható."));
        return;
    }
    const auto loaded = m_store.load(path, ImportPolicy{});
    if (!loaded) {
        showProblem(loaded.error().message);
        return;
    }
    m_lastStickerDir = QFileInfo(loaded.value().path).absolutePath();
    addStickerWithSource(loaded.value().path, loaded.value().sha256, loaded.value().size());
}

void MainWindow::addBuiltInSticker(const QString &alias)
{
    if (alias.isEmpty())
        return;
    // The shipped shapes live in the application and are stored as a pinned
    // alias: nothing is copied to the user's disk, so moving the project,
    // wiping the app's data folder or updating the app cannot break them.
    const QString stored = builtinStickerPath(alias);
    const QString resource = builtinStickerResource(stored);
    if (resource.isEmpty()) {
        showProblem(QCoreApplication::translate("PhotoPrint",
                                                "Ezt a matricát nem találjuk."));
        return;
    }
    QImageReader probe(resource);
    probe.setDecideFormatFromContent(true);
    if (!probe.canRead()) {
        showProblem(QCoreApplication::translate(
            "PhotoPrint", "A matrica túl nagy vagy nem olvasható."));
        return;
    }
    addStickerWithSource(stored, fileSha256(resource), probe.size());
}

bool MainWindow::addStickerWithSource(const QString &storedPath, const QByteArray &sha256,
                                      const QSize &pixels)
{
    flushPendingStickerEdit();
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty() || storedPath.isEmpty())
        return false;
    const Page &page = document.pages.at(document.currentPage);
    if (page.stickers.size() >= kMaxStickersPerPage) {
        showProblem(QCoreApplication::translate(
                        "PhotoPrint", "Egy lapon legfeljebb %1 matrica lehet.")
                        .arg(kMaxStickersPerPage));
        return false;
    }

    // Default placement: a quarter of the short side wide, the height from the
    // picture's own aspect (bounded so a tall picture is smaller instead of
    // squashed), on top of the overlays already there. Each new layer steps
    // away from the centre so a second click on the same shape is visibly a
    // second object. One edit, one undo step, and the new sticker is selected.
    const QSizeF sheet = pageSizeMm(page);
    const QSizeF size = OverlayGeometry::defaultStickerSizeMm(sheet, pixels);
    const QPointF centre = OverlayGeometry::cascadedStickerCentreMm(page, sheet, size);
    Sticker sticker;
    sticker.id = makeId(QStringLiteral("sticker"));
    sticker.imagePath = storedPath;
    sticker.imageSha256 = sha256;
    sticker.cx = centre.x();
    sticker.cy = centre.y();
    sticker.width = size.width();
    sticker.height = size.height();
    int top = 0;
    for (const Sticker &existing : page.stickers)
        top = std::max(top, existing.z);
    sticker.z = top + 1;

    QList<Sticker> stickers = page.stickers;
    stickers.append(sticker);
    return publishStickers(stickers,
                           QCoreApplication::translate("PhotoPrint", "Matrica hozzáadása"),
                           sticker.id);
}

void MainWindow::chooseStickerImage()
{
    flushPendingStickerEdit();
    const std::optional<Sticker> sticker = selectedSticker();
    if (!sticker)
        return;
    QString startDir = stickerPickerDirectory(*sticker, m_lastStickerDir);
    if (startDir.isEmpty())
        startDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, QCoreApplication::translate("PhotoPrint", "Matrica kiválasztása"), startDir,
        QCoreApplication::translate("PhotoPrint", "Képek (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty())
        return;
    relinkStickerFromPath(path);
}

void MainWindow::relinkStickerFromPath(const QString &path)
{
    flushPendingStickerEdit();
    const std::optional<Sticker> sticker = selectedSticker();
    if (!sticker || path.isEmpty())
        return;
    QImageReader probe(path);
    probe.setDecideFormatFromContent(true);
    if (!probe.canRead() || OverlayGeometry::stickerSourceTooLarge(probe.size())) {
        showProblem(QCoreApplication::translate(
            "PhotoPrint", "A matrica túl nagy vagy nem olvasható."));
        return;
    }
    const auto loaded = m_store.load(path, ImportPolicy{});
    if (!loaded) {
        showProblem(loaded.error().message);
        return;
    }
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    Sticker updated = *sticker;
    updated.imagePath = loaded.value().path;
    updated.imageSha256 = loaded.value().sha256;
    // A different picture usually has a different aspect: the box is reset from
    // the new pixels at the width the layer already had, and bounded as one
    // uniform scale. Stretching the old box would print a distorted sticker.
    const QSize pixels = loaded.value().size();
    const double aspect = pixels.width() > 0 && pixels.height() > 0
        ? double(pixels.height()) / double(pixels.width())
        : 1.0;
    const QSizeF box = boundedStickerSizeMm(
        QSizeF(sticker->width, sticker->width * aspect), pageSizeMm(page));
    updated.width = box.width();
    updated.height = box.height();
    m_lastStickerDir = QFileInfo(loaded.value().path).absolutePath();
    publishStickers(withSticker(page.stickers, updated),
                    QCoreApplication::translate("PhotoPrint", "Matrica kiválasztása"),
                    updated.id);
}

void MainWindow::stickerSizeChanged()
{
    if (m_syncing)
        return;
    flushPendingStickerEdit();
    const std::optional<Sticker> sticker = selectedSticker();
    if (!sticker)
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    // The panel is in centimetres, the model in millimetres. The height follows
    // the width through the stored aspect, and the pair goes through the same
    // uniform bound as every other gesture: at the spin minimum a 1:40 sticker
    // stays 1:40 instead of becoming a square because each axis hit the minimum
    // first.
    const double aspect =
        sticker->width > 0.0 ? sticker->height / sticker->width : 1.0;
    const double width = std::max(m_stickerSizeSpin->value() * 10.0, kMinStickerMm);
    const QSizeF box = boundedStickerSizeMm(QSizeF(width, width * aspect),
                                            pageSizeMm(page));
    Sticker updated = *sticker;
    updated.width = box.width();
    updated.height = box.height();
    if (updated.width == sticker->width && updated.height == sticker->height)
        return;
    publishStickers(withSticker(page.stickers, updated),
                    QCoreApplication::translate("PhotoPrint", "Matrica átméretezése"));
}

void MainWindow::stickerAngleChanged()
{
    flushPendingStickerEdit();
    if (m_syncing)
        return;
    const std::optional<Sticker> sticker = selectedSticker();
    if (!sticker)
        return;
    Sticker updated = *sticker;
    updated.angleDeg = normalizedImageRotationDeg(m_stickerAngleSpin->value());
    if (updated.angleDeg == sticker->angleDeg)
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    publishStickers(withSticker(page.stickers, updated),
                    QCoreApplication::translate("PhotoPrint", "Matrica forgatása"));
}

void MainWindow::stickerOpacityChanged(int value)
{
    if (m_syncing)
        return;
    flushPendingStickerEdit();
    if (m_stickerOpacityValue)
        m_stickerOpacityValue->setText(QStringLiteral("%1 %").arg(value));
    const std::optional<Sticker> sticker = selectedSticker();
    if (!sticker)
        return;
    Sticker updated = *sticker;
    updated.opacity = std::clamp(value, 0, 100);
    if (updated.opacity == sticker->opacity && !m_pendingStickerOpacity)
        return;
    // The canvas follows the drag immediately; the document is not touched
    // until the burst ends, so the whole gesture is one undo step.
    if (m_pageView)
        m_pageView->previewSticker(updated);
    m_pendingStickerOpacity = updated;
    if (m_stickerOpacityTimer)
        m_stickerOpacityTimer->start();
}

void MainWindow::flushStickerOpacity()
{
    flushPendingStickerEdit();
    if (m_stickerOpacityTimer)
        m_stickerOpacityTimer->stop();
    if (!m_pendingStickerOpacity)
        return;
    const Sticker updated = *m_pendingStickerOpacity;
    m_pendingStickerOpacity.reset();
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    bool changed = false;
    for (const Sticker &sticker : page.stickers) {
        if (sticker.id == updated.id)
            changed = sticker != updated;
    }
    if (!changed)
        return;
    publishStickers(withSticker(page.stickers, updated),
                    QCoreApplication::translate("PhotoPrint", "Matrica fedettsége"));
}

void MainWindow::cancelPendingStickerOpacity()
{
    if (m_stickerOpacityTimer)
        m_stickerOpacityTimer->stop();
    m_pendingStickerOpacity.reset();
}

void MainWindow::reorderStickerLayer(int delta)
{
    flushPendingStickerEdit();
    const std::optional<Sticker> sticker = selectedSticker();
    if (!sticker)
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    QList<Sticker> order = OverlayGeometry::stickersInPaintOrder(page);
    // The helper renumbers every z to its position, so one press always changes
    // the visible order even when several layers share a z.
    if (!OverlayGeometry::reorderSticker(order, sticker->id, delta))
        return;
    publishStickers(order, QCoreApplication::translate("PhotoPrint", "Rétegsorrend"),
                    sticker->id);
}

void MainWindow::deleteSelectedSticker()
{
    flushPendingStickerEdit();
    const std::optional<Sticker> sticker = selectedSticker();
    if (!sticker)
        return;
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    QList<Sticker> stickers = document.pages.at(document.currentPage).stickers;
    for (int i = stickers.size() - 1; i >= 0; --i) {
        if (stickers.at(i).id == sticker->id)
            stickers.removeAt(i);
    }
    m_selectedSticker.clear();
    publishStickers(stickers, QCoreApplication::translate("PhotoPrint", "Matrica törlése"));
    syncStickerPanel();
}

void MainWindow::cycleSticker(int delta)
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const QList<Sticker> order =
        OverlayGeometry::stickersInPaintOrder(document.pages.at(document.currentPage));
    if (order.isEmpty())
        return;
    int index = -1;
    for (int i = 0; i < order.size(); ++i) {
        if (order.at(i).id == m_selectedSticker) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        index = delta >= 0 ? 0 : int(order.size()) - 1;
    } else {
        const int count = int(order.size());
        index = ((index + delta) % count + count) % count;
    }
    selectSticker(order.at(index).id);
}

MainWindow::BackgroundProblem MainWindow::backgroundProblem() const
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return BackgroundProblem::None;
    const PageBackground &background = document.pages.at(document.currentPage).background;
    if (background.kind != BackgroundKind::Image)
        return BackgroundProblem::None;
    if (background.imagePath.isEmpty()) {
        // An empty path with a recorded identity is a picture that was chosen
        // and lost. An empty path with no identity is simply "no picture yet".
        return background.imageSha256.isEmpty() ? BackgroundProblem::None
                                                : BackgroundProblem::Missing;
    }
    const QFileInfo info(background.imagePath);
    if (!info.exists() || !info.isFile())
        return BackgroundProblem::Missing;
    QImageReader reader(background.imagePath);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead())
        return BackgroundProblem::Unreadable;
    if (BackgroundGeometry::backgroundSourceTooLarge(reader.size()))
        return BackgroundProblem::TooLarge;
    return BackgroundProblem::None;
}

QString MainWindow::backgroundProblemText(MainWindow::BackgroundProblem problem) const
{
    switch (problem) {
    case BackgroundProblem::Missing:
        return QCoreApplication::translate("PhotoPrint", "A háttérkép nem található.");
    case BackgroundProblem::Unreadable:
    case BackgroundProblem::TooLarge:
        return QCoreApplication::translate(
            "PhotoPrint", "A háttérkép túl nagy vagy nem olvasható.");
    case BackgroundProblem::None:
        break;
    }
    return QString();
}

void MainWindow::syncBackgroundPanel()
{
    if (!m_backgroundPanel)
        return;
    const DocumentSnapshot document = m_controller.document();
    const Page *page = document.pages.isEmpty()
        ? nullptr
        : &document.pages.at(document.currentPage);
    m_backgroundPanel->setVisible(page != nullptr);
    if (!page)
        return;

    const PageBackground background = sanitiseBackground(page->background);
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    const int kindIndex = m_backgroundKindCombo->findData(int(background.kind));
    m_backgroundKindCombo->setCurrentIndex(kindIndex < 0 ? 0 : kindIndex);
    m_backgroundColorButton->setIcon(swatchIcon(background.solid));
    const QRgb start =
        background.stops.isEmpty() ? 0xffffffffu : background.stops.first().second;
    const QRgb end =
        background.stops.isEmpty() ? 0xffffffffu : background.stops.last().second;
    m_gradientStartButton->setIcon(swatchIcon(start));
    m_gradientEndButton->setIcon(swatchIcon(end));

    // The combo shows the nearest of the four directions, so a loaded angle
    // that is not one of them still has a consistent entry.
    int angleIndex = 0;
    double best = 1e9;
    for (int i = 0; i < m_gradientAngleCombo->count(); ++i) {
        const double candidate = m_gradientAngleCombo->itemData(i).toDouble();
        double difference = std::abs(candidate - background.angleDeg);
        difference = std::min(difference, 360.0 - difference);
        if (difference < best) {
            best = difference;
            angleIndex = i;
        }
    }
    m_gradientAngleCombo->setCurrentIndex(angleIndex);

    const int fitIndex = m_backgroundFitCombo->findData(int(background.fit));
    m_backgroundFitCombo->setCurrentIndex(fitIndex < 0 ? 0 : fitIndex);
    m_backgroundTileSpin->setValue(background.tileMm / 10.0);
    const int opacity = int(std::lround(background.opacity));
    m_backgroundOpacitySlider->setValue(opacity);
    m_backgroundOpacityValue->setText(QStringLiteral("%1 %").arg(opacity));
    m_syncing = wasSyncing;

    const bool solid = background.kind == BackgroundKind::Solid;
    const bool gradient = background.kind == BackgroundKind::Gradient;
    const bool image = background.kind == BackgroundKind::Image;
    m_backgroundSolidRow->setVisible(solid);
    m_backgroundGradientRow->setVisible(gradient);
    m_backgroundImageRow->setVisible(image);
    m_backgroundTileLabel->setVisible(image && background.fit == BackgroundFit::Tile);
    m_backgroundTileSpin->setVisible(image && background.fit == BackgroundFit::Tile);
    // Opacity has no meaning on the white sheet, so it is hidden rather than
    // shown as a dead control.
    m_backgroundOpacityLabel->setVisible(background.kind != BackgroundKind::None);
    m_backgroundOpacitySlider->setVisible(background.kind != BackgroundKind::None);
    m_backgroundOpacityValue->setVisible(background.kind != BackgroundKind::None);
    m_backgroundResetButton->setEnabled(background.kind != BackgroundKind::None);

    // The picture row names the file it is using, so the user can confirm the
    // choice without reading the page. The name is user data, not a label.
    if (m_backgroundFileLabel) {
        const QString name = background.imagePath.isEmpty()
            ? QString()
            : QFileInfo(background.imagePath).fileName();
        m_backgroundFileLabel->setText(name);
        m_backgroundFileLabel->setVisible(image && !name.isEmpty());
    }
    if (m_backgroundImageButton) {
        m_backgroundImageButton->setText(
            background.imagePath.isEmpty()
                ? QCoreApplication::translate("PhotoPrint", "Háttérkép kiválasztása…")
                : QCoreApplication::translate("PhotoPrint", "Háttérkép cseréje…"));
    }

    const BackgroundProblem problem = backgroundProblem();
    const QString problemText = backgroundProblemText(problem);
    m_backgroundMissingHint->setText(problemText);
    m_backgroundMissingHint->setVisible(problem != BackgroundProblem::None);
    m_backgroundRelinkButton->setVisible(problem != BackgroundProblem::None);
}

QPixmap MainWindow::stickerThumbnail(const Sticker &sticker, int size) const
{
    const QString source = stickerSourcePath(sticker);
    if (source.isEmpty())
        return {};
    QImageReader reader(source);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead())
        return {};
    // Ask the reader for the final size instead of decoding a large source in
    // full on the GUI thread: a sticker preview is a few dozen pixels.
    const QSize original = reader.size();
    if (original.isValid() && !original.isEmpty())
        reader.setScaledSize(original.scaled(size, size, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull())
        return {};
    return QPixmap::fromImage(
        image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

MainWindow::StickerProblem MainWindow::stickerProblem(const Sticker &sticker) const
{
    // An alias that no longer resolves is missing in exactly the same way as a
    // file that was moved away: the panel offers the picker either way.
    const QString source = stickerSourcePath(sticker);
    if (source.isEmpty())
        return StickerProblem::Missing;
    const QFileInfo info(source);
    if (!info.exists() || !info.isFile())
        return StickerProblem::Missing;
    QImageReader reader(source);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead())
        return StickerProblem::Unreadable;
    if (OverlayGeometry::stickerSourceTooLarge(reader.size()))
        return StickerProblem::TooLarge;
    // A shipped sticker is pinned twice: by the version in its alias and by the
    // content hash recorded when it was placed. If the file under that version
    // no longer hashes to the recorded identity, the artwork changed under the
    // project, so say so instead of printing different pixels without a word.
    if (isBuiltinStickerPath(sticker.imagePath) && !sticker.imageSha256.isEmpty()
        && fileSha256(source) != sticker.imageSha256) {
        return StickerProblem::Changed;
    }
    return StickerProblem::None;
}

QString MainWindow::stickerProblemText(MainWindow::StickerProblem problem) const
{
    switch (problem) {
    case StickerProblem::Missing:
        return QCoreApplication::translate("PhotoPrint", "Ezt a matricát nem találjuk.");
    case StickerProblem::Unreadable:
    case StickerProblem::TooLarge:
        return QCoreApplication::translate(
            "PhotoPrint", "A matrica túl nagy vagy nem olvasható.");
    case StickerProblem::Changed:
        return QCoreApplication::translate("PhotoPrint",
                                           "A matrica képe megváltozott.");
    case StickerProblem::None:
        break;
    }
    return QString();
}

MainWindow::StickerProblem MainWindow::pageStickerProblem() const
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return StickerProblem::None;
    for (const Sticker &sticker : document.pages.at(document.currentPage).stickers) {
        const StickerProblem problem = stickerProblem(sticker);
        if (problem != StickerProblem::None)
            return problem;
    }
    return StickerProblem::None;
}

void MainWindow::syncStickerPanel()
{
    if (!m_stickerPanel || !m_stickerEmptyPanel)
        return;
    const DocumentSnapshot document = m_controller.document();
    const Page *page = document.pages.isEmpty()
        ? nullptr
        : &document.pages.at(document.currentPage);
    const std::optional<Sticker> sticker = selectedSticker();

    // The palette is always there while the page exists; the hint only
    // explains the empty state, and the properties only show a selected layer.
    if (m_stickerAddPanel)
        m_stickerAddPanel->setVisible(page != nullptr);
    m_stickerEmptyPanel->setVisible(page && !sticker);
    m_stickerPanel->setVisible(sticker.has_value());
    if (!page) {
        if (m_previousStickerButton)
            m_previousStickerButton->setEnabled(false);
        if (m_nextStickerButton)
            m_nextStickerButton->setEnabled(false);
        return;
    }

    const bool canAdd = page->stickers.size() < kMaxStickersPerPage;
    if (m_addStickerButton)
        m_addStickerButton->setEnabled(canAdd);
    if (m_stickerSetGrid)
        m_stickerSetGrid->setEnabled(canAdd);
    // The hint names the state the page is really in: a page that already has a
    // sticker but none selected is not told "there is none".
    if (m_stickerEmptyHint) {
        m_stickerEmptyHint->setText(
            page->stickers.isEmpty()
                ? QCoreApplication::translate("PhotoPrint", "Még nincs matrica a lapon.")
                : QCoreApplication::translate(
                      "PhotoPrint", "Válassz egy matricát a lapon vagy a készletből."));
    }
    const bool hasStickers = !page->stickers.isEmpty();
    if (m_previousStickerButton)
        m_previousStickerButton->setEnabled(hasStickers);
    if (m_nextStickerButton)
        m_nextStickerButton->setEnabled(hasStickers);

    // The position in the stack: one place, used by the caption and by the
    // order buttons below, so the two cannot disagree.
    const QList<Sticker> order = OverlayGeometry::stickersInPaintOrder(*page);
    int index = -1;
    if (sticker) {
        for (int i = 0; i < order.size(); ++i) {
            if (order.at(i).id == sticker->id) {
                index = i;
                break;
            }
        }
    }
    if (m_stickerPositionLabel) {
        if (index >= 0) {
            m_stickerPositionLabel->setText(
                QStringLiteral("%1 / %2").arg(index + 1).arg(order.size()));
            m_stickerPositionLabel->setAccessibleName(
                QCoreApplication::translate("PhotoPrint", "Matrica: %1 / %2")
                    .arg(index + 1)
                    .arg(order.size()));
        } else {
            m_stickerPositionLabel->setText(
                hasStickers ? QStringLiteral("– / %1").arg(order.size()) : QString());
            m_stickerPositionLabel->setAccessibleName(QString());
        }
    }

    if (!sticker)
        return;

    const bool wasSyncing = m_syncing;
    m_syncing = true;
    if (m_stickerSizeSpin) {
        // The spin can never offer a size the model would clamp away: the page
        // diagonal is the largest box a sticker can have.
        const QSizeF sheet = pageSizeMm(*page);
        const double diagonal = sheet.width() > 0.0 && sheet.height() > 0.0
            ? std::hypot(sheet.width(), sheet.height())
            : kMaxStickerMm;
        const double limit = std::min(kMaxStickerMm, std::max(kMinStickerMm, diagonal));
        m_stickerSizeSpin->setMaximum(limit / 10.0);
        m_stickerSizeSpin->setValue(sticker->width / 10.0);
    }
    if (m_stickerAngleSpin)
        m_stickerAngleSpin->setValue(sticker->angleDeg);
    if (m_stickerOpacitySlider)
        m_stickerOpacitySlider->setValue(std::clamp(sticker->opacity, 0, 100));
    if (m_stickerOpacityValue)
        m_stickerOpacityValue->setText(
            QStringLiteral("%1 %").arg(std::clamp(sticker->opacity, 0, 100)));
    m_syncing = wasSyncing;

    // A move at either end would change nothing, so the button says so.
    if (m_stickerRaiseButton)
        m_stickerRaiseButton->setEnabled(index >= 0 && index < order.size() - 1);
    if (m_stickerLowerButton)
        m_stickerLowerButton->setEnabled(index > 0);

    const StickerProblem problem = stickerProblem(*sticker);
    if (m_stickerMissingHint) {
        m_stickerMissingHint->setText(stickerProblemText(problem));
        m_stickerMissingHint->setVisible(problem != StickerProblem::None);
    }
    if (m_stickerRelinkButton)
        m_stickerRelinkButton->setVisible(problem != StickerProblem::None);

    if (m_stickerPreview) {
        if (sticker->imagePath != m_stickerPreviewPath) {
            m_stickerPreviewPath = sticker->imagePath;
            m_stickerPreviewPixmap = stickerThumbnail(*sticker, 64);
        }
        m_stickerPreview->setPixmap(m_stickerPreviewPixmap);
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
    // A background picture that cannot be shown is a page problem the user has
    // to see without opening the Lap tab: the page prints white and the notice
    // says why. The label carries the message, not just the bar's visibility.
    const BackgroundProblem problem = backgroundProblem();
    if (m_backgroundNotice) {
        m_backgroundNotice->setText(backgroundProblemText(problem));
        m_backgroundNotice->setVisible(problem != BackgroundProblem::None);
    }
    // A sticker whose picture is lost is the same kind of page problem: the
    // placeholder is on the page, and this line says why.
    const StickerProblem stickerIssue = pageStickerProblem();
    if (m_stickerNotice) {
        m_stickerNotice->setText(stickerProblemText(stickerIssue));
        m_stickerNotice->setVisible(stickerIssue != StickerProblem::None);
    }
    if (m_stickerNoticeButton)
        m_stickerNoticeButton->setVisible(stickerIssue != StickerProblem::None);
    // The overflow state is the page's own, not the label's effective
    // visibility: a child of a hidden bar reports itself hidden, which would
    // keep the bar hidden and the warning could never appear.
    const bool overflow = pageOverflows();
    m_noticeBar->setVisible(printer || paper || problem != BackgroundProblem::None
                            || stickerIssue != StickerProblem::None || overflow);
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
    m_selectedSticker.clear();
    m_pageView->setSelectedFrame(QString());
    m_pageView->setSelectedSlot(QString());
    m_pageView->setSelectedSticker(QString());
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
