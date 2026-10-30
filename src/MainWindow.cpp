#include "MainWindow.h"

#include "AboutDialog.h"
#include "ColorPipeline.h"
#include "LayoutEngine.h"
#include "PhotoFilters.h"
#include "PhotoItem.h"
#include "JobJournal.h"
#include "Translation.h"
#include "TrialSheetBuilder.h"
#include "ToneEngine.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPixmap>
#include <QGuiApplication>
#include <QStyleHints>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QDragEnterEvent>
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
QCheckBox { spacing: 7px; color: @text; }
QStatusBar { background: @window; color: @secondary; }
QDialog, QMessageBox { background: @panel; color: @text; }
QMessageBox QLabel { color: @text; }
QMessageBox QPushButton { min-width: 80px; }
QToolTip { background: @panel; color: @text; border: 1px solid @border; padding: 4px 6px; border-radius: 5px; }
QLabel { color: @text; }
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

PageView::PageView(QWidget *parent) : QWidget(parent)
{
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(180, 140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PageView::setDocument(const DocumentSnapshot &document)
{
    m_document = document;
    if (!m_selectedId.isEmpty() && !findFrame(m_document, m_selectedId))
        m_selectedId.clear();
    invalidateComposition();
    update();
}

void PageView::invalidateComposition()
{
    m_composedDirty = true;
    m_composed = QImage();
}

void PageView::ensureComposed() const
{
    const QRectF page = pageRect();
    if (page.isEmpty())
        return;
    const double dpr = devicePixelRatioF();
    const int width = std::max(1, qRound(page.width() * dpr));
    const int height = std::max(1, qRound(page.height() * dpr));
    if (!m_composedDirty && m_composedPixels == QSize(width, height) && !m_composed.isNull())
        return;

    QImage linear(width, height, QImage::Format_RGBA64_Premultiplied);
    linear.fill(Qt::white);
    QPainter painter(&linear);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QTransform scale;
    scale.scale(static_cast<double>(width) / pageMm().width(),
                static_cast<double>(height) / pageMm().height());
    painter.setTransform(scale);
    paintPageContent(painter, m_document, m_document.currentPage, m_images);
    painter.end();

    m_composed = ColorPipeline::linearToSrgb(linear, m_document.recipe.grayscale);
    m_composedPixels = QSize(width, height);
    m_composedDirty = false;
}

void PageView::setFrameImages(const QHash<QString, QImage> &images)
{
    m_images = images;
    invalidateComposition();
    update();
}

void PageView::setViewZoom(double zoom)
{
    m_viewZoom = std::clamp(zoom, 0.25, 3.0);
    if (m_viewZoom <= 1.0)
        m_viewPan = QPointF();
    invalidateComposition();
    update();
}

void PageView::setSelectedFrame(const QString &frameId)
{
    if (m_selectedId == frameId)
        return;
    m_selectedId = frameId;
    update();
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

QSizeF PageView::pageMm() const
{
    if (m_document.pages.isEmpty() || m_document.currentPage < 0
        || m_document.currentPage >= m_document.pages.size())
        return {};
    return pageSizeMm(m_document.pages.at(m_document.currentPage));
}

QImage PageView::composedImage() const
{
    ensureComposed();
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

int PageView::hitHandle(const QPointF &widgetPoint) const
{
    const Frame *frame = selectedFramePtr();
    if (!frame)
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
    const auto it = m_images.constFind(frame.id);
    m_drag.pixels = it == m_images.constEnd() ? QSize() : it->size();
    setCursor(kind == DragKind::CropPan ? Qt::ClosedHandCursor : Qt::ArrowCursor);
}

void PageView::updateLiveFrame(const Frame &frame)
{
    Frame *target = findFrame(m_document, frame.id);
    if (!target)
        return;
    *target = frame;
    target->autoPlaced = false;
    m_drag.changed = true;
    invalidateComposition();
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
    painter.fillRect(rect(), m_canvasColor);
    const QRectF page = pageRect();
    if (page.isEmpty())
        return;

    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    painter.fillRect(page.adjusted(2, 4, 2, 6), m_pageShadowColor);
    painter.fillRect(page, Qt::white);

    ensureComposed();
    if (!m_composed.isNull())
        painter.drawImage(page, m_composed);

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

    const Frame *frame = selectedFramePtr();
    if (!frame)
        return;

    const QPolygonF quad = frameQuadWidget(*frame);
    QPen outline(palette().color(QPalette::Highlight));
    outline.setWidthF(1.6);
    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(quad);

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

void PageView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    invalidateComposition();
    update();
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

    const int index = hitFrame(event->position());
    if (index >= 0 && !m_document.pages.isEmpty()) {
        const Frame frame = m_document.pages.at(m_document.currentPage).frames.at(index);
        setSelectedFrame(frame.id);
        if (onSelectionChanged)
            onSelectionChanged(frame.id);
        beginDrag(DragKind::Move, -1, pagePoint, frame);
        event->accept();
        return;
    }

    if (!m_selectedId.isEmpty()) {
        m_selectedId.clear();
        update();
        if (onSelectionChanged)
            onSelectionChanged(QString());
    }
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        m_viewPan += event->position() - m_panStart;
        m_panStart = event->position();
        invalidateComposition();
        update();
        return;
    }
    if (m_drag.kind == DragKind::None) {
        const Frame *frame = selectedFramePtr();
        if (frame && m_cropMode
            && frameQuadWidget(*frame).containsPoint(event->position(), Qt::OddEvenFill))
            setCursor(Qt::OpenHandCursor);
        else
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
}

void PageView::wheelEvent(QWheelEvent *event)
{
    const Frame *frame = selectedFramePtr();
    if (m_cropMode && frame) {
        const double factor = std::pow(1.0015, event->angleDelta().y());
        const QSize pixels = m_images.contains(frame->id) ? m_images.value(frame->id).size()
                                                          : QSize();
        Frame updated = PhotoItem::zoomAt(*frame, widgetToPage(event->position()), factor, pixels);
        if (PhotoItem::clampCrop(updated, pixels).cropCenter != frame->cropCenter
            || updated.zoom != frame->zoom) {
            updateLiveFrame(updated);
            if (onFrameCommitted)
                onFrameCommitted(updated);
        }
        event->accept();
        return;
    }
    const double factor = std::pow(1.0015, event->angleDelta().y());
    setViewZoom(m_viewZoom * factor);
    if (onViewZoomChanged)
        onViewZoomChanged(m_viewZoom);
    event->accept();
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    const Frame *frame = selectedFramePtr();
    if (!frame) {
        QWidget::keyPressEvent(event);
        return;
    }
    const bool fine = event->modifiers().testFlag(Qt::ShiftModifier);
    const double step = fine ? 0.1 : 1.0;
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
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (onDeleteRequested)
            onDeleteRequested(frame->id);
        event->accept();
        return;
    case Qt::Key_Escape:
        if (m_cropMode) {
            m_cropMode = false;
            if (onCropModeExited)
                onCropModeExited();
            update();
        } else {
            m_selectedId.clear();
            update();
            if (onSelectionChanged)
                onSelectionChanged(QString());
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
        nudged = false;
        break;
    }
    if (nudged) {
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
    const int index = hitFrame(event->position());
    if (index >= 0 && !m_document.pages.isEmpty()) {
        const Frame frame = m_document.pages.at(m_document.currentPage).frames.at(index);
        setSelectedFrame(frame.id);
        if (onSelectionChanged)
            onSelectionChanged(frame.id);
        m_cropMode = true;
        if (onCropModeChanged)
            onCropModeChanged(true);
        update();
    }
}

void PageView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void PageView::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    if (!paths.isEmpty() && onFilesDropped)
        onFilesDropped(paths);
    event->acceptProposedAction();
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), m_renderer(m_store)
{
    buildUi();
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
    if (JobJournal(jobConfig.journalPath).uncertainCount() > 0) {
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
            if (answer == QMessageBox::Yes)
                applyLoadedProject(latest.value(), true);
            else
                m_recovery.clear();
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
    side->setMaximumWidth(320);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(12, 12, 12, 12);
    auto addSection = [side, sideLayout](const QString &text) {
        auto *label = new QLabel(text, side);
        label->setObjectName(QStringLiteral("sectionHeader"));
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        label->setStyleSheet(QStringLiteral("margin-top: 8px;"));
        sideLayout->addWidget(label);
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

    m_photoList = new QListWidget(library);
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

    addSection(QCoreApplication::translate("PhotoPrint", "Kiválasztott fotó"));
    auto *fitRow = new QHBoxLayout;
    m_fitButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Teljes kép"), side);
    m_fillButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Kép kitöltése"), side);
    fitRow->addWidget(m_fitButton);
    fitRow->addWidget(m_fillButton);
    sideLayout->addLayout(fitRow);
    connect(m_fitButton, &QPushButton::clicked, this, [this] { applyFit(FitMode::Full); });
    connect(m_fillButton, &QPushButton::clicked, this, [this] { applyFit(FitMode::Fill); });

    auto *actionRow = new QHBoxLayout;
    m_cropButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Kivágás"), side);
    m_cropButton->setCheckable(true);
    m_deleteButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Eltávolítás"), side);
    actionRow->addWidget(m_cropButton);
    actionRow->addWidget(m_deleteButton);
    sideLayout->addLayout(actionRow);
    connect(m_cropButton, &QPushButton::toggled, this, &MainWindow::toggleCrop);
    connect(m_deleteButton, &QPushButton::clicked, this, &MainWindow::deleteSelected);

    auto *layerRow = new QHBoxLayout;
    m_forwardButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Előtérbe"), side);
    m_backwardButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Háttérbe"), side);
    layerRow->addWidget(m_forwardButton);
    layerRow->addWidget(m_backwardButton);
    sideLayout->addLayout(layerRow);
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
    m_autoHint->setStyleSheet(QStringLiteral("color: #6e6e73; font-size: 11px;"));
    m_autoHint->setWordWrap(true);
    toneText->addWidget(toneTitle);
    toneText->addWidget(m_autoHint);
    m_originalButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Eredeti"), side);
    m_originalButton->setToolTip(QCoreApplication::translate("PhotoPrint", "Tartsd nyomva az eredeti képhez."));
    toneRow->addWidget(m_autoCheck);
    toneRow->addLayout(toneText);
    toneRow->addStretch(1);
    toneRow->addWidget(m_originalButton);
    sideLayout->addLayout(toneRow);
    connect(m_autoCheck, &QAbstractButton::toggled, this, &MainWindow::toggleAutoTone);
    connect(m_originalButton, &QPushButton::pressed, this, [this] { showOriginalPressed(true); });
    connect(m_originalButton, &QPushButton::released, this, [this] { showOriginalPressed(false); });

    m_brightnessSlider = new QSlider(Qt::Horizontal, side);
    m_brightnessSlider->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Világosítás"));
    m_brightnessSlider->setRange(0, 100);
    m_brightnessSlider->setValue(50);
    m_brightnessSlider->setTracking(false);
    sideLayout->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Világosítás"), side));
    sideLayout->addWidget(m_brightnessSlider);
    connect(m_brightnessSlider, &QSlider::valueChanged, this, &MainWindow::brightnessChanged);

    addSection(QCoreApplication::translate("PhotoPrint", "Képhangulat"));
    m_filterCombo = new QComboBox(side);
    for (FilterPreset preset : {FilterPreset::Original, FilterPreset::Vivid, FilterPreset::Warm,
                                FilterPreset::Cool, FilterPreset::Sepia,
                                FilterPreset::BlackWhite, FilterPreset::Vintage,
                                FilterPreset::Crisp}) {
        m_filterCombo->addItem(filterLabel(preset), filterKey(preset));
    }
    m_filterCombo->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Szűrő"));
    sideLayout->addWidget(m_filterCombo);
    connect(m_filterCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::applyFilterSettings);

    sideLayout->addWidget(new QLabel(QCoreApplication::translate("PhotoPrint", "Hatás"), side));
    m_filterStrength = new QSlider(Qt::Horizontal, side);
    m_filterStrength->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Hatás erőssége"));
    m_filterStrength->setRange(0, 100);
    m_filterStrength->setValue(100);
    m_filterStrength->setTracking(false);
    sideLayout->addWidget(m_filterStrength);
    connect(m_filterStrength, &QSlider::valueChanged, this, &MainWindow::applyFilterSettings);

    m_upscaleCheck = new QCheckBox(QCoreApplication::translate("PhotoPrint", "Kép nagyítása 2×"), side);
    m_upscaleCheck->setToolTip(
        QCoreApplication::translate("PhotoPrint", "A nyomtatási felbontást növeli; az előnézet nem változik."));
    sideLayout->addWidget(m_upscaleCheck);
    auto *upscaleHint = new QLabel(QCoreApplication::translate("PhotoPrint", "A nyomtatási felbontást növeli."), side);
    upscaleHint->setStyleSheet(QStringLiteral("color: #6e6e73; font-size: 11px;"));
    upscaleHint->setWordWrap(true);
    sideLayout->addWidget(upscaleHint);
    connect(m_upscaleCheck, &QCheckBox::toggled, this, &MainWindow::applyFilterSettings);

    m_trialButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Próbanyomat"), side);
    sideLayout->addWidget(m_trialButton);
    connect(m_trialButton, &QPushButton::clicked, this, &MainWindow::showTrialSheet);

    m_advancedToggle = new QPushButton(QCoreApplication::translate("PhotoPrint", "További beállítások"), side);
    m_advancedToggle->setCheckable(true);
    sideLayout->addWidget(m_advancedToggle);
    auto *advancedContent = new QWidget(side);
    auto *grid = new QGridLayout(advancedContent);
    grid->setContentsMargins(0, 0, 0, 0);
    m_widthSpin = new QDoubleSpinBox(advancedContent);
    m_heightSpin = new QDoubleSpinBox(advancedContent);
    m_angleSpin = new QDoubleSpinBox(advancedContent);
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
    sideLayout->addWidget(advancedContent);
    for (QDoubleSpinBox *spin : {m_widthSpin, m_heightSpin, m_angleSpin})
        connect(spin, &QDoubleSpinBox::editingFinished, this, &MainWindow::applySizeFromControls);

    m_resolutionHint = new QLabel(side);
    m_resolutionHint->setWordWrap(true);
    m_resolutionHint->setStyleSheet(QStringLiteral("color: #8a5a00;"));
    sideLayout->addWidget(m_resolutionHint);

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
    m_fitPageButton->setVisible(false);
    noticeLayout->addWidget(m_fitPageButton);
    noticeLayout->addStretch(1);
    connect(m_fitPageButton, &QPushButton::clicked, this, &MainWindow::fitPage);
    m_noticeBar->setVisible(false);

    m_pageView = new PageView(central);
    m_pageView->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Papírlap"));
    m_pageView->setAccessibleDescription(
        QCoreApplication::translate("PhotoPrint", "Nagyításnál a középső egérgombbal mozgatható a lap."));
    m_pageView->onFilesDropped = [this](const QStringList &paths) { addDroppedFiles(paths); };
    m_pageView->onSelectionChanged = [this](const QString &frameId) {
        selectFrame(frameId, false);
    };
    m_pageView->onFrameCommitted = [this](const Frame &frame) {
        m_controller.apply(FrameChange{frame.id, frame});
    };
    m_pageView->onDeleteRequested = [this](const QString &frameId) {
        if (!frameId.isEmpty())
            m_controller.apply(RemoveFrameEdit{frameId});
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
    sideScroll->setMinimumWidth(240);
    sideScroll->setMaximumWidth(330);
    sideScroll->setMinimumHeight(0);
    side->setMinimumHeight(0);
    side->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    sideScroll->setWidget(side);
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
        addDroppedFiles(paths);
}

void MainWindow::addDroppedFiles(const QStringList &paths)
{
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile())
            continue;
        addPhoto(info.absoluteFilePath());
    }
}

void MainWindow::addPhoto(const QString &path)
{
    Result<SourceImage> loaded = m_store.load(path, ImportPolicy{});
    if (!loaded) {
        showProblem(loaded.error().message);
        return;
    }
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    Frame frame = makeInitialFrame(loaded.value(), page);
    frame.autoPlaced = true;
    const QImage toneSample = ColorPipeline::toLinear(loaded.value(), ColorRecipe{}, 1000);
    frame.tone = ToneEngine::suggest(toneSample);
    frame.autoTone = true;

    QHash<QString, QSize> pixelSizes;
    for (const Frame &existing : page.frames) {
        if (const std::optional<SourceImage> source = m_store.cached(existing.sourcePath))
            pixelSizes.insert(existing.id, source->size());
    }
    pixelSizes.insert(frame.id, loaded.value().size());

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
    m_selectedId = frame.id;
    syncListSelection();
    m_pageView->setSelectedFrame(frame.id);
    syncFrameControls();
}

void MainWindow::onDocumentChanged()
{
    if (!m_restoringProject) {
        m_dirty = true;
        if (m_checkpointTimer)
            m_checkpointTimer->start();
    }
    updateTitle();
    rebuildPreviewImages();
    refreshPhotoList();
    const DocumentSnapshot document = m_controller.document();
    m_pageView->setDocument(document);
    m_pageView->setFrameImages(m_previewImages);
    if (!m_selectedId.isEmpty() && !findFrame(document, m_selectedId)) {
        m_selectedId.clear();
        m_pageView->setSelectedFrame(QString());
    }
    m_pageView->setEmptyHint(
        document.pages.isEmpty() || document.pages.at(document.currentPage).frames.isEmpty()
            ? QCoreApplication::translate("PhotoPrint", "Húzd ide a fotókat, vagy válaszd a Fotók hozzáadása gombot.")
            : QString());
    const bool overflows = pageOverflows();
    m_overflowLabel->setVisible(overflows);
    m_fitPageButton->setVisible(overflows);
    updatePrintButton();
    updateNoticeBar();
    if (m_pagesPanel)
        m_pagesPanel->setPages(document.pages, document.currentPage);
    syncFrameControls();
    syncPrintControls();
}

void MainWindow::rebuildPreviewImages()
{
    const DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const Page &page = document.pages.at(document.currentPage);
    QHash<QString, QImage> images;
    for (const Frame &frame : page.frames) {
        Result<SourceImage> source = m_store.load(frame.sourcePath, ImportPolicy{});
        if (!source)
            continue;
        const bool showOriginal = m_showOriginal && frame.id == m_selectedId;
        ColorRecipe recipe = document.recipe;
        recipe.tone = showOriginal ? PhotoTone{} : effectiveTone(frame);
        const QString filter = showOriginal ? QStringLiteral("original") : frame.filter;
        const int filterStrength = showOriginal ? 0 : frame.filterStrength;
        const bool upscale = showOriginal ? false : frame.upscale;
        Result<QImage> converted =
            m_store.preparedLinear(source.value(), recipe, filter, filterStrength, upscale, 1600);
        if (!converted)
            continue;
        images.insert(frame.id, converted.value());
    }
    m_previewImages = images;
}

void MainWindow::refreshPhotoList()
{
    m_syncing = true;
    m_photoList->clear();
    const DocumentSnapshot document = m_controller.document();
    if (!document.pages.isEmpty()) {
        const Page &page = document.pages.at(document.currentPage);
        for (const Frame &frame : page.frames) {
            auto *item = new QListWidgetItem(QString());
            item->setToolTip(QFileInfo(frame.sourcePath).fileName());
            item->setData(Qt::AccessibleTextRole,
                          QCoreApplication::translate("PhotoPrint", "%n. fotó", nullptr,
                                                      page.frames.indexOf(frame) + 1));
            item->setData(Qt::UserRole, frame.id);
            if (const std::optional<SourceImage> source = m_store.cached(frame.sourcePath)) {
                const Result<QImage> thumb = m_store.srgb(*source, document.recipe, 96);
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
        if (page.paper.isPhotoPaper() && !wantBorderless(page.paper)) {
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
    if (m_selectedId == frameId)
        return;
    m_selectedId = frameId;
    m_pageView->setSelectedFrame(frameId);
    if (frameId.isEmpty() && m_cropButton->isChecked()) {
        m_cropButton->setChecked(false);
        m_pageView->setCropMode(false);
    }
    syncListSelection();
    syncFrameControls();
}

std::optional<Frame> MainWindow::selectedFrame()
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
    const bool hasSelection = selectedFrame().has_value();
    if (m_inspectorPanel)
        m_inspectorPanel->setVisible(hasSelection);
    m_fitButton->setEnabled(hasSelection);
    m_fillButton->setEnabled(hasSelection);
    m_cropButton->setEnabled(hasSelection);
    m_deleteButton->setEnabled(hasSelection);
    m_forwardButton->setEnabled(hasSelection);
    m_backwardButton->setEnabled(hasSelection);
    m_autoCheck->setEnabled(hasSelection);
    m_brightnessSlider->setEnabled(hasSelection);
    m_filterCombo->setEnabled(hasSelection);
    const std::optional<Frame> current = selectedFrame();
    m_filterStrength->setEnabled(hasSelection && current
                                 && current->filter != QLatin1String("original"));
    m_upscaleCheck->setEnabled(hasSelection);
    m_originalButton->setEnabled(hasSelection);
    m_advancedToggle->setEnabled(hasSelection);

    m_syncing = true;
    m_resolutionHint->clear();
    if (const std::optional<Frame> frame = selectedFrame()) {
        m_widthSpin->setValue(frame->width / 10.0);
        m_heightSpin->setValue(frame->height / 10.0);
        m_angleSpin->setValue(frame->angleDeg);
        m_autoCheck->setChecked(frame->autoTone);
        if (m_autoHint) {
            if (!frame->autoTone)
                m_autoHint->setText(QCoreApplication::translate("PhotoPrint", "Kikapcsolva"));
            else if (frame->tone == PhotoTone{})
                m_autoHint->setText(QCoreApplication::translate("PhotoPrint", "Erre a képre most nincs szükség."));
            else
                m_autoHint->setText(QCoreApplication::translate("PhotoPrint", "Finom fény- és színjavítás"));
        }
        m_brightnessSlider->setValue(
            std::clamp(qRound(50.0 + frame->brightness * 100.0), 0, 100));
        int filterIndex = 0;
        for (int i = 0; i < m_filterCombo->count(); ++i) {
            if (m_filterCombo->itemData(i).toString() == frame->filter)
                filterIndex = i;
        }
        m_filterCombo->setCurrentIndex(filterIndex);
        m_filterStrength->setValue(std::clamp(frame->filterStrength, 0, 100));
        m_upscaleCheck->setChecked(frame->upscale);
        const auto image = m_previewImages.constFind(frame->id);
        if (image != m_previewImages.constEnd()
            && LayoutEngine::resolutionHint(*frame, image->size())
                == LayoutEngine::ResolutionHint::Low) {
            m_resolutionHint->setText(
                QCoreApplication::translate("PhotoPrint", "Ekkora méretben a kép életlen lehet."));
        }
    }
    m_syncing = false;
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
    if (m_selectedId.isEmpty())
        return;
    m_controller.apply(RemoveFrameEdit{m_selectedId});
}

void MainWindow::applySizeFromControls()
{
    if (m_syncing)
        return;
    const std::optional<Frame> frame = selectedFrame();
    if (!frame)
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

bool MainWindow::wantBorderless(const PaperSpec &paper) const
{
    if (!paper.isPhotoPaper() || !isFixedPhotoSize(paper))
        return false;
    if (paper.custom && !paper.borderlessSupported)
        return false;
    const PrinterCapabilities *capabilities = printerByQueue(m_queue);
    return capabilities && capabilities->borderlessSupported();
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
    m_selectedId.clear();
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
}

void MainWindow::duplicatePage()
{
    m_controller.apply(DuplicatePageEdit{m_controller.document().currentPage});
    m_selectedId.clear();
}

void MainWindow::removePage()
{
    m_controller.apply(RemovePageEdit{m_controller.document().currentPage});
    m_selectedId.clear();
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
            showProblem(QStringLiteral(
                "A nyomtató most nem érhető el. Ellenőrizd, hogy be van-e kapcsolva."));
            return;
        }
        const Page &page = document.pages.at(document.currentPage);
        PrintSelection selection;
        selection.queueName = m_queue;
        selection.paper = page.paper;
        selection.landscape = page.landscape;
        selection.borderless = wantBorderless(page.paper);
        selection.fineQuality = document.fineQuality;
        selection.color = !document.recipe.grayscale;
        const auto ticket = PrinterAdapter::resolve(*capabilities, selection);
        if (!ticket) {
            showProblem(ticket.error().message);
            return;
        }
        options.mediaSizeMm = ticket.value().geometry.logicalSizeMm();
        options.contentOffsetMm = ticket.value().geometry.physicalOriginInLogicalMm;
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
    updated.filter = m_filterCombo->currentData().toString();
    updated.filterStrength = m_filterStrength->value();
    updated.upscale = m_upscaleCheck->isChecked();
    if (updated.filter == frame->filter && updated.filterStrength == frame->filterStrength
        && updated.upscale == frame->upscale)
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
    updated.brightness = (value - 50) / 100.0;
    m_controller.apply(FrameChange{updated.id, updated});
}

void MainWindow::showOriginalPressed(bool pressed)
{
    m_showOriginal = pressed;
    rebuildPreviewImages();
    m_pageView->setFrameImages(m_previewImages);
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
    if (m_queue.isEmpty()) {
        showProblem(QCoreApplication::translate("PhotoPrint", "Válassz nyomtatót."));
        return;
    }
    DocumentSnapshot document = m_controller.document();
    if (document.pages.isEmpty())
        return;
    const PrinterCapabilities *capabilities = printerByQueue(m_queue);
    if (!capabilities) {
        showProblem(QStringLiteral(
            "A nyomtató most nem érhető el. Ellenőrizd, hogy be van-e kapcsolva."));
        return;
    }
    const Page &page = document.pages.at(document.currentPage);
    PrintSelection selection;
    selection.queueName = m_queue;
    selection.paper = page.paper;
    selection.landscape = page.landscape;
    selection.borderless = wantBorderless(page.paper);
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
        showProblem(QStringLiteral(
            "A nyomtató most nem érhető el. Ellenőrizd, hogy be van-e kapcsolva."));
        return;
    }
    const DocumentSnapshot document = m_controller.document();
    const Page &page = document.pages.at(document.currentPage);
    PrintSelection selection;
    selection.queueName = m_queue;
    selection.paper = page.paper;
    selection.landscape = page.landscape;
    selection.borderless = wantBorderless(page.paper);
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
    const int pages = m_controller.document().pages.size();
    m_printButton->setText(pages > 1
                               ? QCoreApplication::translate("PhotoPrint", "%n oldal nyomtatása", nullptr, pages)
                               : QCoreApplication::translate("PhotoPrint", "Nyomtatás"));
}

void MainWindow::updateNoticeBar()
{
    if (!m_noticeBar)
        return;
    const bool printer = m_printerStatus && !m_printerStatus->text().isEmpty();
    const bool paper = m_paperHint && !m_paperHint->text().isEmpty();
    const bool overflow = m_overflowLabel && m_overflowLabel->isVisible();
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
    m_recovery.clear();
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
    m_selectedId.clear();
    m_pageView->setSelectedFrame(QString());
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
        m_recovery.clear();
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
    } else {
        m_recovery.clear();
    }
    event->accept();
}

void MainWindow::showProblem(const QString &message)
{
    statusBar()->showMessage(message, 8000);
}

} // namespace pp
