#include "EffectBrowserDialog.h"

#include "EffectEngine.h"
#include "ImageSource.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

constexpr int kTileBorderReserve = 30; // label strip below the picture
constexpr int kSlowBadgeHeight = 16;
constexpr int kStrengthDebounceMs = 70;

QString categoryLabel(const QString &key)
{
    // The twelve labels are navigation, so they are translated. The effect
    // names themselves are product names and stay single. The table keeps the
    // source strings in one place; QT_TRANSLATE_NOOP is what lupdate reads.
    struct Entry {
        const char *key;
        const char *source;
    };
    static constexpr Entry entries[] = {
        {"basic", QT_TRANSLATE_NOOP("PhotoPrint", "Alap")},
        {"mono", QT_TRANSLATE_NOOP("PhotoPrint", "Fekete-fehér")},
        {"retro", QT_TRANSLATE_NOOP("PhotoPrint", "Retró")},
        {"color", QT_TRANSLATE_NOOP("PhotoPrint", "Szín")},
        {"blur", QT_TRANSLATE_NOOP("PhotoPrint", "Elmosás")},
        {"detail", QT_TRANSLATE_NOOP("PhotoPrint", "Élesség")},
        {"art", QT_TRANSLATE_NOOP("PhotoPrint", "Művészi")},
        {"warp", QT_TRANSLATE_NOOP("PhotoPrint", "Torzítás")},
        {"light", QT_TRANSLATE_NOOP("PhotoPrint", "Fény")},
        {"grain", QT_TRANSLATE_NOOP("PhotoPrint", "Szemcse")},
        {"print", QT_TRANSLATE_NOOP("PhotoPrint", "Nyomat")},
        {"glow", QT_TRANSLATE_NOOP("PhotoPrint", "Ragyogás")},
    };
    for (const Entry &entry : entries) {
        if (key == QLatin1String(entry.key))
            return QCoreApplication::translate("PhotoPrint", entry.source);
    }
    return key;
}

/// Case- and accent-insensitive form used by the search. `Művészi` has to be
/// found by typing `muveszi` as well.
QString folded(const QString &text)
{
    QString form = text.normalized(QString::NormalizationForm_D);
    form.removeIf([](QChar character) { return character.category() == QChar::Mark_NonSpacing; });
    return form.toCaseFolded();
}

QColor secondaryColour(const QWidget *widget)
{
    return widget->palette().color(QPalette::PlaceholderText);
}

} // namespace

QString effectButtonLabel(const QString &key, int version)
{
    if (key.isEmpty() || key == QLatin1String("original"))
        return QCoreApplication::translate("PhotoPrint", "Eredeti");
    if (isLegacyPresetKey(key))
        return legacyEffectLabel();
    const auto recipe = EffectCatalogue::instance().resolve(key, version);
    if (recipe)
        return recipe.value().name;
    // An unknown key renders as the untouched frame, so the honest label is
    // the same as the identity path.
    return QCoreApplication::translate("PhotoPrint", "Eredeti");
}

QString legacyEffectLabel()
{
    return QCoreApplication::translate("PhotoPrint", "Korábbi megjelenés");
}

QString newEffectAffordance()
{
    return QCoreApplication::translate("PhotoPrint", "Új effekt választása");
}

QString effectStrengthReason()
{
    return QCoreApplication::translate("PhotoPrint", "Ez az effekt teljes erősséggel jelenik meg.");
}

int appliedEffectVersion(const QString &key, int tileVersion, const QString &currentKey,
                         int currentVersion)
{
    if (key.isEmpty() || key == QLatin1String("original"))
        return 0;
    // Re-applying the effect a pinned frame already uses keeps that version;
    // only a different tile is a new choice and may take its recipe version.
    if (key == currentKey && currentVersion > 0)
        return currentVersion;
    return tileVersion;
}

QThreadPool *effectPreviewPool()
{
    static QThreadPool *pool = [] {
        auto *created = new QThreadPool;
        created->setObjectName(QStringLiteral("effects-preview"));
        // Plan 5.4 bounds preview work at one to two threads. The engine keeps
        // its wand passes atomic and has no per-stage OpenMP, so two jobs are
        // the ceiling, not a starting point to raise.
        created->setMaxThreadCount(2);
        return created;
    }();
    return pool;
}

/// One grid tile. It is a plain QAbstractButton so it is focusable, clickable
/// and reachable by a screen reader without a second accessibility layer.
class EffectTile : public QAbstractButton
{
public:
    explicit EffectTile(QWidget *parent = nullptr) : QAbstractButton(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
    }

    void setEffectKey(const QString &key) { m_key = key; }
    QString effectKey() const { return m_key; }

    void setLabel(const QString &label)
    {
        m_label = label;
        setText(label);
        setAccessibleName(label);
    }

    void setContextText(const QString &text) { setAccessibleDescription(text); }

    void setEffectImage(const QImage &image)
    {
        m_image = image;
        update();
    }
    bool hasEffectImage() const { return !m_image.isNull(); }
    QImage effectImage() const { return m_image; }

    void setSlow(bool slow, const QString &badge)
    {
        m_slow = slow;
        m_slowText = badge;
        setProperty("slow", slow);
        update();
    }

    void setMarkedCurrent(bool current)
    {
        m_current = current;
        setProperty("current", current);
        update();
    }

    void setMarkedSelected(bool selected)
    {
        m_selected = selected;
        setProperty("selected", selected);
        update();
    }

    /// Double click applies at once; the dialog installs the same action on
    /// Apply and Enter.
    std::function<void()> onActivate;

    QSize sizeHint() const override
    {
        const int side = property("tileSide").toInt();
        return QSize(side > 0 ? side : 150, (side > 0 ? side : 150) + kTileBorderReserve);
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const QRectF box = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
        const double radius = 9.0;
        QColor base = palette().color(QPalette::Base);
        if (m_selected)
            base = base.lighter(102);
        else if (underMouse())
            base = palette().color(QPalette::AlternateBase);

        painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
        painter.setBrush(base);
        painter.drawRoundedRect(box, radius, radius);

        const QRectF imageBox(box.left() + 5.0, box.top() + 5.0, box.width() - 10.0,
                              box.height() - 10.0 - kTileBorderReserve + 6.0);
        if (!m_image.isNull() && imageBox.width() > 1.0 && imageBox.height() > 1.0) {
            QPainterPath clip;
            clip.addRoundedRect(imageBox, 6.0, 6.0);
            painter.save();
            painter.setClipPath(clip);
            const QImage scaled = m_image.scaled(imageBox.size().toSize(),
                                                 Qt::KeepAspectRatioByExpanding,
                                                 Qt::SmoothTransformation);
            const int cropX = std::max(0, (scaled.width() - int(imageBox.width())) / 2);
            const int cropY = std::max(0, (scaled.height() - int(imageBox.height())) / 2);
            painter.drawImage(imageBox, scaled,
                              QRectF(cropX, cropY, imageBox.width(), imageBox.height()));
            painter.restore();
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette().color(QPalette::Window));
            painter.drawRoundedRect(imageBox, 6.0, 6.0);
        }

        // The name is drawn bold when it is the effect the photo uses. The
        // check mark above it carries the same information without relying on
        // colour.
        QFont nameFont = font();
        nameFont.setWeight(m_current ? QFont::Bold : QFont::Normal);
        painter.setFont(nameFont);
        painter.setPen(palette().color(QPalette::WindowText));
        const QRectF nameBox(box.left() + 6.0, box.bottom() - kTileBorderReserve + 5.0,
                             box.width() - 12.0 - (m_current ? 18.0 : 0.0), 20.0);
        const QFontMetrics metrics(nameFont);
        painter.drawText(nameBox, Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(m_label, Qt::ElideRight, int(nameBox.width())));

        if (m_current) {
            const QRectF badge(nameBox.right() + 2.0, nameBox.center().y() - 8.0, 16.0, 16.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette().color(QPalette::Highlight));
            painter.drawEllipse(badge);
            painter.setPen(palette().color(QPalette::HighlightedText));
            QFont checkFont = font();
            checkFont.setWeight(QFont::Bold);
            checkFont.setPixelSize(11);
            painter.setFont(checkFont);
            painter.drawText(badge, Qt::AlignCenter, QStringLiteral("\u2713"));
        }

        if (m_slow) {
            QFont badgeFont = font();
            badgeFont.setPixelSize(9);
            painter.setFont(badgeFont);
            const QFontMetrics badgeMetrics(badgeFont);
            const int width = badgeMetrics.horizontalAdvance(m_slowText) + 10;
            const QRectF badge(box.left() + 7.0, box.top() + 7.0, width, kSlowBadgeHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 150));
            painter.drawRoundedRect(badge, 4.0, 4.0);
            painter.setPen(Qt::white);
            painter.drawText(badge, Qt::AlignCenter, m_slowText);
        }

        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(palette().color(QPalette::Highlight), 2.0));
            painter.drawRoundedRect(box.adjusted(1.5, 1.5, -1.5, -1.5), radius, radius);
        } else if (m_selected) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1.0));
            painter.drawRoundedRect(box.adjusted(1.5, 1.5, -1.5, -1.5), radius, radius);
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        event->accept();
        if (onActivate)
            onActivate();
    }

    void enterEvent(QEnterEvent *event) override
    {
        QAbstractButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QAbstractButton::leaveEvent(event);
        update();
    }

private:
    QString m_key;
    QString m_label;
    QString m_slowText;
    QImage m_image;
    bool m_slow = false;
    bool m_current = false;
    bool m_selected = false;
};

EffectBrowserDialog::EffectBrowserDialog(const EffectBrowserRequest &request, QWidget *parent)
    : QDialog(parent), m_request(request)
{
    setObjectName(QStringLiteral("effectBrowserDialog"));
    setWindowTitle(QCoreApplication::translate("PhotoPrint", "Effektek"));
    setAccessibleName(QCoreApplication::translate("PhotoPrint", "Effektek"));
    setModal(true);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumSize(860, 560);
    resize(1060, 720);

    m_recipes = EffectCatalogue::instance().recipes();
    for (const EffectRecipe &recipe : m_recipes)
        m_byKey.insert(recipe.key, recipe);

    m_sample = request.sample;
    if (m_sample.isNull()) {
        // A dialog with no photo still has to show something; the bundled
        // sample is the same one the product ships for an empty document.
        QImage bundled(QStringLiteral(":/samples/effect-sample.jpg"));
        if (!bundled.isNull())
            m_sample = bundled;
    }
    if (m_sample.isNull()) {
        m_sample = QImage(480, 320, QImage::Format_RGB32);
        m_sample.fill(QColor(128, 128, 128));
        m_sample.setColorSpace(QColorSpace(QColorSpace::SRgb));
    }
    const qreal dpr = devicePixelRatioF();
    m_rasterPx = std::min(240, int(std::ceil(request.tileLogicalPx * dpr)));
    m_tileSize = m_sample.size().scaled(m_rasterPx, m_rasterPx, Qt::KeepAspectRatio);

    m_currentKey = request.currentKey;
    m_currentStrength = std::clamp(request.currentStrength, 0, 100);

    m_watcher = new QFutureWatcher<RenderResult>(this);
    connect(m_watcher, &QFutureWatcher<RenderResult>::finished, this,
            &EffectBrowserDialog::handleRenderFinished);
    m_selectedWatcher = new QFutureWatcher<RenderResult>(this);
    connect(m_selectedWatcher, &QFutureWatcher<RenderResult>::finished, this,
            &EffectBrowserDialog::handleSelectedRenderFinished);

    buildUi();
    fillCategories();

    // Select the applied effect's category, or the first category for the
    // untouched frame. No tile is rendered until showEvent schedules the
    // worker, so the dialog is on screen before any effect runs.
    QString startCategory;
    if (const auto applied = m_byKey.constFind(m_currentKey);
        applied != m_byKey.constEnd()) {
        startCategory = applied->category;
    }
    if (startCategory.isEmpty() && m_categories->count() > 0)
        startCategory = m_categories->item(0)->data(Qt::UserRole).toString();
    for (int row = 0; row < m_categories->count(); ++row) {
        if (m_categories->item(row)->data(Qt::UserRole).toString() == startCategory) {
            m_categories->setCurrentRow(row);
            break;
        }
    }
    rebuildGrid();
}

EffectBrowserDialog::~EffectBrowserDialog()
{
    if (m_activeJob && m_activeJob->stop)
        m_activeJob->stop->request_stop();
    if (m_selectedJob && m_selectedJob->stop)
        m_selectedJob->stop->request_stop();
    if (m_watcher && m_watcher->isRunning())
        m_watcher->waitForFinished();
    if (m_selectedWatcher && m_selectedWatcher->isRunning())
        m_selectedWatcher->waitForFinished();
}

int EffectBrowserDialog::categoryCount() const
{
    return m_categories ? m_categories->count() : 0;
}

int EffectBrowserDialog::visibleTileCount() const
{
    return m_visibleKeys.size();
}

QStringList EffectBrowserDialog::visibleTileKeys() const
{
    return m_visibleKeys;
}

QString EffectBrowserDialog::tileLabelFor(const QString &key) const
{
    const auto tile = m_tiles.constFind(key);
    return tile == m_tiles.constEnd() ? QString() : (*tile)->text();
}

QImage EffectBrowserDialog::tileImage(const QString &key) const
{
    const auto tile = m_tiles.constFind(key);
    return tile == m_tiles.constEnd() ? QImage() : (*tile)->effectImage();
}

bool EffectBrowserDialog::tileHasImage(const QString &key) const
{
    const auto tile = m_tiles.constFind(key);
    return tile != m_tiles.constEnd() && (*tile)->hasEffectImage();
}

int EffectBrowserDialog::renderedTileCount() const
{
    int count = 0;
    for (const QString &key : m_visibleKeys) {
        if (tileHasImage(key))
            ++count;
    }
    return count;
}

bool EffectBrowserDialog::strengthEnabled() const
{
    return m_strength && m_strength->isEnabled();
}

QString EffectBrowserDialog::strengthHintText() const
{
    return m_strengthHint && m_strengthHint->isVisible() ? m_strengthHint->text() : QString();
}

QString EffectBrowserDialog::adjacentHintText() const
{
    return m_adjacentHint && m_adjacentHint->isVisible() ? m_adjacentHint->text() : QString();
}

QString EffectBrowserDialog::currentKey() const
{
    return m_currentKey;
}

QString EffectBrowserDialog::selectedKey() const
{
    return m_selectedKey;
}

bool EffectBrowserDialog::tileMarkedCurrent(const QString &key) const
{
    const auto tile = m_tiles.constFind(key);
    return tile != m_tiles.constEnd() && (*tile)->property("current").toBool();
}

bool EffectBrowserDialog::applyEnabled() const
{
    return m_applyButton && m_applyButton->isEnabled();
}

bool EffectBrowserDialog::hasLegacyMarker() const
{
    return m_legacyLabel && m_legacyLabel->isVisible();
}

QString EffectBrowserDialog::legacyMarkerText() const
{
    return m_legacyLabel ? m_legacyLabel->text() : QString();
}

int EffectBrowserDialog::strengthValue() const
{
    return m_strength ? m_strength->value() : 100;
}

bool EffectBrowserDialog::waitForRendering(int timeoutMs)
{
    if (!m_started) {
        m_started = true;
        scheduleRender();
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!m_rendering) {
            bool complete = true;
            for (const QString &key : m_visibleKeys) {
                if (!cachedImage(key, 100).isNull()) {
                    continue;
                }
                complete = false;
                break;
            }
            if (complete)
                return true;
        }
    }
    return false;
}

bool EffectBrowserDialog::waitForSelectedRender(int timeoutMs)
{
    if (m_selectedKey.isEmpty())
        return true;
    QElapsedTimer timer;
    timer.start();
    const int strength = selectedStrength();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!cachedImage(m_selectedKey, strength).isNull() && !m_selectedRendering)
            return true;
    }
    return false;
}

void EffectBrowserDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    relayoutGrid();
    if (!m_started) {
        m_started = true;
        scheduleRender();
        scheduleSelectedRender();
    }
}

int EffectBrowserDialog::columnCount() const
{
    if (!m_scroll)
        return m_columns;
    const int spacing = m_grid->horizontalSpacing();
    const int step = m_request.tileLogicalPx + spacing;
    if (step <= 0)
        return 1;
    const int available = m_scroll->viewport()->width() - 4;
    return std::clamp((available + spacing) / step, 1, 5);
}

void EffectBrowserDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    relayoutGrid();
}

void EffectBrowserDialog::relayoutGrid()
{
    const int columns = columnCount();
    if (columns == m_columns || m_visibleKeys.isEmpty())
        return;
    m_columns = columns;
    // Re-add the existing tiles; taking a layout item does not delete its
    // widget, so the rendered images and the focus survive a resize.
    while (QLayoutItem *item = m_grid->takeAt(0))
        delete item;
    for (int index = 0; index < m_visibleKeys.size(); ++index) {
        EffectTile *tile = m_tiles.value(m_visibleKeys.at(index));
        if (tile)
            m_grid->addWidget(tile, index / m_columns, index % m_columns);
    }
}

void EffectBrowserDialog::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 14, 14, 14);
    outer->setSpacing(10);

    auto *searchRow = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("searchField"));
    m_search->setPlaceholderText(
        QCoreApplication::translate("PhotoPrint", "Keresés név vagy kategória szerint"));
    m_search->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Keresés"));
    m_search->setClearButtonEnabled(true);
    searchRow->addWidget(m_search, 1);
    m_countLabel = new QLabel(this);
    m_countLabel->setObjectName(QStringLiteral("resultCount"));
    m_countLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QPalette countPalette = m_countLabel->palette();
    countPalette.setColor(QPalette::WindowText, secondaryColour(this));
    m_countLabel->setPalette(countPalette);
    searchRow->addWidget(m_countLabel);
    outer->addLayout(searchRow);

    m_legacyLabel = new QLabel(legacyEffectLabel(), this);
    m_legacyLabel->setObjectName(QStringLiteral("legacyMarker"));
    m_legacyLabel->setPalette(countPalette);
    m_legacyLabel->setVisible(false);
    outer->addWidget(m_legacyLabel);

    auto *body = new QHBoxLayout;
    body->setSpacing(12);
    m_categories = new QListWidget(this);
    m_categories->setObjectName(QStringLiteral("categoryList"));
    m_categories->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Kategóriák"));
    m_categories->setFixedWidth(178);
    m_categories->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_categories->setUniformItemSizes(true);
    body->addWidget(m_categories);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("tileScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Effektek"));
    m_gridHost = new QWidget(m_scroll);
    m_gridHost->setObjectName(QStringLiteral("tileGridHost"));
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(2, 2, 2, 2);
    m_grid->setHorizontalSpacing(10);
    m_grid->setVerticalSpacing(10);
    m_grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_scroll->setWidget(m_gridHost);
    body->addWidget(m_scroll, 1);
    outer->addLayout(body, 1);

    auto *strengthRow = new QHBoxLayout;
    m_strengthLabel =
        new QLabel(QCoreApplication::translate("PhotoPrint", "Effekt erőssége"), this);
    m_strengthLabel->setObjectName(QStringLiteral("strengthLabel"));
    strengthRow->addWidget(m_strengthLabel);
    m_strength = new QSlider(Qt::Horizontal, this);
    m_strength->setObjectName(QStringLiteral("strengthSlider"));
    m_strength->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Effekt erőssége"));
    m_strength->setRange(0, 100);
    m_strength->setValue(m_currentStrength);
    m_strength->setTracking(true);
    strengthRow->addWidget(m_strength, 1);
    m_strengthValue = new QLabel(QStringLiteral("%1%").arg(m_strength->value()), this);
    m_strengthValue->setObjectName(QStringLiteral("strengthValue"));
    m_strengthValue->setMinimumWidth(46);
    m_strengthValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    strengthRow->addWidget(m_strengthValue);
    outer->addLayout(strengthRow);

    m_strengthHint = new QLabel(this);
    m_strengthHint->setObjectName(QStringLiteral("strengthHint"));
    m_strengthHint->setWordWrap(true);
    QPalette hintPalette = m_strengthHint->palette();
    hintPalette.setColor(QPalette::WindowText, secondaryColour(this));
    m_strengthHint->setPalette(hintPalette);
    m_strengthHint->setVisible(false);
    outer->addWidget(m_strengthHint);

    m_adjacentHint = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Az előnézet hozzávetőleges"), this);
    m_adjacentHint->setObjectName(QStringLiteral("approximateHint"));
    m_adjacentHint->setWordWrap(true);
    m_adjacentHint->setPalette(hintPalette);
    m_adjacentHint->setVisible(false);
    outer->addWidget(m_adjacentHint);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    m_applyButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Alkalmaz"), this);
    m_applyButton->setObjectName(QStringLiteral("applyButton"));
    m_applyButton->setAutoDefault(false);
    m_closeButton = new QPushButton(QCoreApplication::translate("PhotoPrint", "Bezárás"), this);
    m_closeButton->setObjectName(QStringLiteral("closeButton"));
    m_closeButton->setAutoDefault(false);
    buttonRow->addWidget(m_applyButton);
    buttonRow->addWidget(m_closeButton);
    outer->addLayout(buttonRow);

    m_strengthTimer = new QTimer(this);
    m_strengthTimer->setSingleShot(true);
    m_strengthTimer->setInterval(kStrengthDebounceMs);
    connect(m_strengthTimer, &QTimer::timeout, this,
            &EffectBrowserDialog::scheduleSelectedRender);

    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_applyButton, &QPushButton::clicked, this, &EffectBrowserDialog::activateCurrent);
    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuildGrid(); });
    connect(m_search, &QLineEdit::returnPressed, this, &EffectBrowserDialog::applyFirstMatch);
    connect(m_categories, &QListWidget::currentRowChanged, this,
            [this](int) { categoryActivated(); });
    connect(m_categories, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *) { categoryActivated(); });
    connect(m_strength, &QSlider::valueChanged, this, [this](int value) {
        m_strengthValue->setText(QStringLiteral("%1%").arg(value));
        if (m_strength->isEnabled())
            m_strengthTimer->start();
    });
}

void EffectBrowserDialog::fillCategories()
{
    m_categories->clear();
    for (const QString &key : EffectCatalogue::instance().categoryKeys()) {
        auto *item = new QListWidgetItem(categoryLabel(key), m_categories);
        item->setData(Qt::AccessibleTextRole, categoryLabel(key));
        item->setData(Qt::UserRole, key);
    }
}

void EffectBrowserDialog::categoryActivated()
{
    // A category is a direct navigation: leave the search and show the ten
    // effects of the clicked category. The itemClicked path makes this work
    // even when the clicked row was already current.
    if (m_search && !m_search->text().isEmpty()) {
        const QSignalBlocker blocker(m_search);
        m_search->clear();
    }
    rebuildGrid();
}

QString EffectBrowserDialog::scaledSampleHash() const
{
    return m_request.sampleHash.isEmpty() ? QStringLiteral("bundled-sample")
                                          : m_request.sampleHash;
}

EffectRecipe EffectBrowserDialog::recipeForKey(const QString &key) const
{
    // A pinned frame shows the recipe it was saved with. Only a new choice
    // renders the newest one.
    if (key == m_currentKey && m_request.currentVersion > 0) {
        if (const auto pinned =
                EffectCatalogue::instance().resolve(key, m_request.currentVersion)) {
            return pinned.value();
        }
    }
    return m_byKey.value(key);
}

QString EffectBrowserDialog::cacheKeyFor(const QString &key, int strength) const
{
    const EffectRecipe recipe = recipeForKey(key);
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(key)
        .arg(recipe.version)
        .arg(scaledSampleHash())
        .arg(strength)
        .arg(m_rasterPx);
}

QImage EffectBrowserDialog::cachedImage(const QString &key, int strength) const
{
    return m_images.value(cacheKeyFor(key, strength));
}

void EffectBrowserDialog::storeImage(const QString &key, int strength, const QImage &image)
{
    m_images.insert(cacheKeyFor(key, strength), image);
    // One complete 120-effect set at 240 px is about 8 MB. A generous cap
    // keeps a strength sweep bounded without evicting the current category.
    if (m_images.size() > 480)
        m_images.clear();
}

void EffectBrowserDialog::rebuildGrid()
{
    while (QLayoutItem *item = m_grid->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            m_tiles.remove(widget->property("effectKey").toString());
            // Delete now, not deleteLater: a deferred tile would still be a
            // child with the same object name and screen readers and tests
            // would find the dead one before the live grid.
            delete widget;
        }
        delete item;
    }
    m_tiles.clear();
    m_visibleKeys.clear();

    QString currentCategory;
    if (m_categories->currentItem())
        currentCategory = m_categories->currentItem()->data(Qt::UserRole).toString();
    const QString query = m_search ? folded(m_search->text().trimmed()) : QString();

    const bool legacyCurrent = !m_currentKey.isEmpty()
        && m_currentKey != QLatin1String("original")
        && !m_byKey.contains(m_currentKey);
    m_legacyLabel->setVisible(legacyCurrent);

    // Eredeti is always available, even when the search matches nothing: it is
    // the way back and it must never disappear from the grid.
    {
        auto *tile = new EffectTile(m_gridHost);
        tile->setProperty("tileSide", m_request.tileLogicalPx);
        tile->setObjectName(QStringLiteral("effectTile_original"));
        tile->setProperty("effectKey", QStringLiteral("original"));
        tile->setEffectKey(QStringLiteral("original"));
        tile->setLabel(QCoreApplication::translate("PhotoPrint", "Eredeti"));
        tile->setContextText(
            QCoreApplication::translate("PhotoPrint", "Az eredeti kép effekt nélkül."));
        const QImage original = m_sample.scaled(m_tileSize, Qt::KeepAspectRatioByExpanding,
                                                 Qt::SmoothTransformation);
        storeImage(QStringLiteral("original"), 100, original);
        tile->setEffectImage(original);
        connect(tile, &QAbstractButton::clicked, this,
                [this] { selectTile(QStringLiteral("original")); });
        tile->onActivate = [this, key = QStringLiteral("original")] {
            selectTile(key);
            activateCurrent();
        };
        tile->installEventFilter(this);
        m_tiles.insert(QStringLiteral("original"), tile);
        m_visibleKeys.append(QStringLiteral("original"));
    }

    for (const EffectRecipe &recipe : m_recipes) {
        if (query.isEmpty()) {
            if (recipe.category != currentCategory)
                continue;
        } else {
            const QString haystack =
                folded(recipe.name) + QLatin1Char('\n') + folded(recipe.key)
                + QLatin1Char('\n') + folded(categoryLabel(recipe.category));
            if (!haystack.contains(query))
                continue;
        }
        auto *tile = new EffectTile(m_gridHost);
        tile->setProperty("tileSide", m_request.tileLogicalPx);
        tile->setObjectName(QStringLiteral("effectTile_%1").arg(recipe.key));
        tile->setProperty("effectKey", recipe.key);
        tile->setEffectKey(recipe.key);
        tile->setLabel(recipe.name);
        // The effect name is a product name and stays single; the accessible
        // description is the translated navigation label, never English prose.
        tile->setContextText(categoryLabel(recipe.category));
        tile->setSlow(recipe.slow, QCoreApplication::translate("PhotoPrint", "lassú"));
        if (const QImage image = cachedImage(recipe.key, 100); !image.isNull())
            tile->setEffectImage(image);
        connect(tile, &QAbstractButton::clicked, this,
                [this, key = recipe.key] { selectTile(key); });
        tile->onActivate = [this, key = recipe.key] {
            selectTile(key);
            activateCurrent();
        };
        tile->installEventFilter(this);
        m_tiles.insert(recipe.key, tile);
        m_visibleKeys.append(recipe.key);
    }

    m_columns = columnCount();
    for (int index = 0; index < m_visibleKeys.size(); ++index) {
        const QString key = m_visibleKeys.at(index);
        EffectTile *tile = m_tiles.value(key);
        m_grid->addWidget(tile, index / m_columns, index % m_columns);
        tile->setMarkedCurrent(key == m_currentKey);
        tile->setMarkedSelected(key == m_selectedKey);
    }

    // Tab order: search -> categories -> grid -> strength -> Apply/Close. The
    // tiles keep their own order inside the grid so a screen reader can walk
    // them; the explicit links put the grid between the categories and the
    // strength slider.
    setTabOrder(m_search, m_categories);
    if (!m_visibleKeys.isEmpty())
        setTabOrder(m_categories, m_tiles.value(m_visibleKeys.first()));
    for (const QString &key : m_visibleKeys)
        setTabOrder(m_tiles.value(key), m_strength);
    setTabOrder(m_strength, m_applyButton);
    setTabOrder(m_applyButton, m_closeButton);

    // Only the effect the frame already uses is preselected. A legacy or
    // unknown key leaves the grid empty: Apply must never silently replace the
    // frozen look. The same rule applies when browsing a category that does
    // not contain the current effect.
    if (m_selectedKey.isEmpty() || !m_tiles.contains(m_selectedKey)) {
        m_selectedKey = m_tiles.contains(m_currentKey) ? m_currentKey : QString();
    }
    if (EffectTile *tile = m_tiles.value(m_selectedKey)) {
        tile->setMarkedSelected(true);
        resetStrengthForSelection();
    } else {
        m_selectedKey.clear();
        const QSignalBlocker blocker(m_strength);
        m_strength->setValue(m_currentStrength);
        m_strengthValue->setText(QStringLiteral("%1%").arg(m_currentStrength));
    }

    updateCountLabel();
    syncStrengthUi();
    m_applyButton->setEnabled(!m_selectedKey.isEmpty());
    scheduleRender();
    scheduleSelectedRender();
}

void EffectBrowserDialog::updateCountLabel()
{
    int count = 0;
    for (const QString &key : m_visibleKeys) {
        if (key != QLatin1String("original"))
            ++count;
    }
    if (m_search && !m_search->text().trimmed().isEmpty() && count == 0) {
        m_countLabel->setText(QCoreApplication::translate("PhotoPrint", "Nincs találat."));
    } else {
        m_countLabel->setText(
            QCoreApplication::translate("PhotoPrint", "%n effekt", nullptr, count));
    }
}

void EffectBrowserDialog::resetStrengthForSelection()
{
    if (m_selectedKey.isEmpty())
        return;
    int value = 100;
    if (m_selectedKey == m_currentKey)
        value = m_currentStrength;
    if (m_selectedKey == QLatin1String("original") || effectIgnoresStrength(m_selectedKey))
        value = 100;
    const QSignalBlocker blocker(m_strength);
    m_strength->setValue(value);
    m_strengthValue->setText(QStringLiteral("%1%").arg(value));
}

void EffectBrowserDialog::syncStrengthUi()
{
    const bool noSelection = m_selectedKey.isEmpty();
    const bool original = m_selectedKey == QLatin1String("original");
    const bool ignores = !noSelection && effectIgnoresStrength(m_selectedKey);
    const bool enabled = !noSelection && !original && !ignores;

    const QSignalBlocker blocker(m_strength);
    m_strength->setEnabled(enabled);
    if (original || ignores) {
        m_strength->setValue(100);
        m_strengthValue->setText(QStringLiteral("100%"));
    }
    m_strengthLabel->setEnabled(enabled);
    m_strengthValue->setEnabled(enabled);

    if (ignores) {
        m_strengthHint->setText(effectStrengthReason());
        m_strengthHint->setVisible(true);
    } else {
        m_strengthHint->clear();
        m_strengthHint->setVisible(false);
    }

    const EffectRecipe recipe = recipeForKey(m_selectedKey);
    m_adjacentHint->setVisible(recipe.approximate);
}

int EffectBrowserDialog::selectedStrength() const
{
    if (m_selectedKey.isEmpty())
        return 100;
    return effectiveEffectStrength(m_selectedKey, m_strength->value());
}

void EffectBrowserDialog::selectTile(const QString &key)
{
    if (!m_tiles.contains(key))
        return;
    m_selectedKey = key;
    for (auto it = m_tiles.constBegin(); it != m_tiles.constEnd(); ++it)
        it.value()->setMarkedSelected(it.key() == key);
    resetStrengthForSelection();
    syncStrengthUi();
    m_applyButton->setEnabled(true);
    scheduleSelectedRender();
    if (EffectTile *tile = m_tiles.value(key))
        tile->setFocus(Qt::OtherFocusReason);
}

void EffectBrowserDialog::activateCurrent()
{
    if (m_selectedKey.isEmpty())
        return;
    m_strengthTimer->stop();
    const QString key = m_selectedKey;
    const int strength = selectedStrength();
    const int tileVersion = key == QLatin1String("original") ? 0 : recipeForKey(key).version;
    const int version = appliedEffectVersion(key, tileVersion, m_currentKey,
                                             m_request.currentVersion);
    emit effectChosen(key, version, strength);
    accept();
}

void EffectBrowserDialog::applyFirstMatch()
{
    // Return is a search action. With an empty field it must not apply the
    // first tile by accident; the user has not chosen anything yet.
    const QString query = m_search->text().trimmed();
    if (query.isEmpty())
        return;
    // Return applies the first real match. If the search only kept Eredeti, it
    // applies only when the query actually names it, never as a blind fallback.
    QString target;
    for (const QString &key : m_visibleKeys) {
        if (key != QLatin1String("original")) {
            target = key;
            break;
        }
    }
    if (target.isEmpty()) {
        const QString foldedQuery = folded(query);
        if (folded(QCoreApplication::translate("PhotoPrint", "Eredeti"))
                .contains(foldedQuery)
            || QStringLiteral("original").contains(foldedQuery)) {
            target = QStringLiteral("original");
        }
    }
    if (target.isEmpty())
        return;
    selectTile(target);
    activateCurrent();
}

void EffectBrowserDialog::scheduleRender()
{
    if (!m_started || m_tiles.isEmpty())
        return;

    ++m_generation;
    if (m_activeJob && m_activeJob->stop)
        m_activeJob->stop->request_stop();

    auto job = std::make_shared<RenderJob>();
    job->generation = m_generation;
    job->sample = m_sample;
    job->tileSize = m_tileSize;
    job->rasterPx = m_rasterPx;
    job->strength = 100;
    job->sampleHash = scaledSampleHash();
    job->stop = std::make_shared<std::stop_source>();
    for (const QString &key : m_visibleKeys) {
        if (key == QLatin1String("original"))
            continue;
        if (!cachedImage(key, 100).isNull())
            continue;
        job->recipes.append(recipeForKey(key));
    }
    m_activeJob = job;
    if (job->recipes.isEmpty()) {
        m_rendering = false;
        return;
    }
    m_rendering = true;
    m_watcher->setFuture(
        QtConcurrent::run(effectPreviewPool(), [job] { return renderTiles(*job); }));
}

void EffectBrowserDialog::scheduleSelectedRender()
{
    if (!m_started || m_selectedKey.isEmpty())
        return;
    const QString key = m_selectedKey;
    if (key == QLatin1String("original") || effectIgnoresStrength(key))
        return;
    const int strength = effectiveEffectStrength(key, m_strength->value());
    if (const QImage cached = cachedImage(key, strength); !cached.isNull()) {
        if (EffectTile *tile = m_tiles.value(key))
            tile->setEffectImage(cached);
        return;
    }
    // The batch is already producing the 100% version of every tile; do not
    // start a duplicate for the highlighted one.
    if (strength == 100 && (m_rendering || tileHasImage(key)))
        return;

    ++m_selectedGeneration;
    if (m_selectedJob && m_selectedJob->stop)
        m_selectedJob->stop->request_stop();
    auto job = std::make_shared<RenderJob>();
    job->generation = m_selectedGeneration;
    job->sample = m_sample;
    job->tileSize = m_tileSize;
    job->rasterPx = m_rasterPx;
    job->strength = strength;
    job->sampleHash = scaledSampleHash();
    job->stop = std::make_shared<std::stop_source>();
    job->recipes.append(recipeForKey(key));
    job->pinnedKey = key;
    m_selectedJob = job;
    m_selectedRendering = true;
    m_selectedWatcher->setFuture(
        QtConcurrent::run(effectPreviewPool(), [job] { return renderTiles(*job); }));
}

EffectBrowserDialog::RenderResult EffectBrowserDialog::renderTiles(RenderJob job)
{
    RenderResult result;
    result.generation = job.generation;
    result.key = job.pinnedKey;
    result.strength = job.strength;
    const std::stop_token token = job.stop->get_token();
    for (const EffectRecipe &recipe : job.recipes) {
        if (token.stop_requested())
            break;
        // The tile raster is capped at 240 px, never the grade raster: that cap
        // is what makes all 120 effects interactive.
        Result<QImage> image = EffectEngine::instance().render(
            job.sample, recipe, job.strength, job.tileSize, false, token);
        if (image)
            result.images.insert(recipe.key, image.value());
    }
    return result;
}

void EffectBrowserDialog::handleRenderFinished()
{
    if (!m_watcher || !m_watcher->isFinished())
        return;
    m_rendering = false;
    const RenderResult result = m_watcher->result();
    if (result.generation != m_generation)
        return;
    for (auto it = result.images.constBegin(); it != result.images.constEnd(); ++it) {
        storeImage(it.key(), 100, it.value());
        if (it.key() == m_selectedKey && selectedStrength() != 100)
            continue;
        if (EffectTile *tile = m_tiles.value(it.key()))
            tile->setEffectImage(it.value());
    }
    if (!m_selectedKey.isEmpty() && selectedStrength() != 100)
        scheduleSelectedRender();
}

void EffectBrowserDialog::handleSelectedRenderFinished()
{
    if (!m_selectedWatcher || !m_selectedWatcher->isFinished())
        return;
    m_selectedRendering = false;
    const RenderResult result = m_selectedWatcher->result();
    if (result.generation != m_selectedGeneration)
        return;
    if (result.key != m_selectedKey || result.strength != selectedStrength())
        return;
    for (auto it = result.images.constBegin(); it != result.images.constEnd(); ++it) {
        storeImage(it.key(), result.strength, it.value());
        if (EffectTile *tile = m_tiles.value(it.key()))
            tile->setEffectImage(it.value());
    }
}

bool EffectBrowserDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress)
        return QDialog::eventFilter(watched, event);
    auto *tile = dynamic_cast<EffectTile *>(watched);
    if (!tile)
        return QDialog::eventFilter(watched, event);
    const int index = m_visibleKeys.indexOf(tile->effectKey());
    if (index < 0)
        return QDialog::eventFilter(watched, event);

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    switch (keyEvent->key()) {
    case Qt::Key_Left:
        moveFocus(index - 1);
        return true;
    case Qt::Key_Right:
        moveFocus(index + 1);
        return true;
    case Qt::Key_Up:
        moveFocus(index - m_columns);
        return true;
    case Qt::Key_Down:
        moveFocus(index + m_columns);
        return true;
    case Qt::Key_Home:
        moveFocus(0);
        return true;
    case Qt::Key_End:
        moveFocus(m_visibleKeys.size() - 1);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        selectTile(tile->effectKey());
        activateCurrent();
        return true;
    default:
        break;
    }
    return QDialog::eventFilter(watched, event);
}

void EffectBrowserDialog::moveFocus(int index)
{
    if (m_visibleKeys.isEmpty())
        return;
    index = std::clamp(index, 0, int(m_visibleKeys.size()) - 1);
    const QString key = m_visibleKeys.at(index);
    selectTile(key);
    if (EffectTile *tile = m_tiles.value(key))
        m_scroll->ensureWidgetVisible(tile, 12, 12);
}

} // namespace pp
