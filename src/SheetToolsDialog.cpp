#include "SheetToolsDialog.h"

#include "ColorPipeline.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QAccessible>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

QString uiText(const char *text, int count = -1)
{
    return QCoreApplication::translate("PhotoPrint", text, nullptr, count < 0 ? -1 : count);
}

constexpr int kBatchTab = 0;
constexpr int kAlignTab = 1;

/// One decimal, no trailing ".0": a margin of 3.5 mm reads "3.5", a cell of
/// 90.0 mm reads "90".
QString mmText(double value)
{
    // Locale-aware: a Hungarian interface writes 13,5 mm and an English one
    // 13.5 mm. The trailing zero group is removed in the same locale.
    QString text = QLocale().toString(value, 'f', 1);
    const QString zero = QStringLiteral("0");
    if (text.endsWith(QLocale().decimalPoint() + zero))
        text.chop(2);
    return text;
}

double clampSetting(double value, double fallback, double low, double high)
{
    if (!std::isfinite(value))
        return fallback;
    return std::clamp(value, low, high);
}

bool settingsEnabled()
{
    return !qEnvironmentVariableIsSet("PHOTOPRINT_NO_SETTINGS");
}

/// The default align margin is the collage rule: 5 % of the short side, kept
/// between 4 and 10 mm, so "auto align" and a template agree about a sane
/// margin.
double defaultAlignMargin(const SheetLayout::Sheet &sheet)
{
    if (!sheet.isValid())
        return 10.0;
    return std::clamp(0.05 * std::min(sheet.width, sheet.height), 4.0, 10.0);
}

QList<QLineF> rectEdges(const QRectF &rect)
{
    return {QLineF(rect.topLeft(), rect.topRight()), QLineF(rect.topRight(), rect.bottomRight()),
            QLineF(rect.bottomRight(), rect.bottomLeft()),
            QLineF(rect.bottomLeft(), rect.topLeft())};
}

/// Source rectangle for a cover draw: the largest centred part of the image with
/// the target's aspect. The preview has to look like the print, and a copy fills
/// its rectangle to the edge.
QRectF coverSource(const QImage &image, const QRectF &target)
{
    if (image.isNull() || target.isEmpty())
        return {};
    const double targetAspect = target.height() / target.width();
    const double imageAspect = double(image.height()) / double(image.width());
    QRectF source(0.0, 0.0, image.width(), image.height());
    if (imageAspect > targetAspect) {
        const double height = image.width() * targetAspect;
        source.setTop((image.height() - height) / 2.0);
        source.setHeight(height);
    } else {
        const double width = image.height() / targetAspect;
        source.setLeft((image.width() - width) / 2.0);
        source.setWidth(width);
    }
    return source;
}

} // namespace

// --- SheetPreview -----------------------------------------------------------

SheetPreview::SheetPreview(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("sheetPreview"));
    setMinimumWidth(240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SheetPreview::setSheetMm(const SheetLayout::Sheet &sheet)
{
    m_sheet = sheet;
    update();
}

void SheetPreview::setRects(const QList<QRectF> &rectsMm, const QList<QImage> &images)
{
    m_rects = rectsMm;
    m_images.clear();
    m_images.reserve(images.size());
    // The canvas preview cache holds linear Rec.2020 layers, which the page
    // painter converts at composition time. A dialog has no composition, so the
    // conversion happens once per distinct image here; otherwise every flat
    // colour and every photograph would show up dark and oversaturated.
    QHash<qint64, QImage> display;
    for (const QImage &image : images) {
        if (image.isNull()) {
            m_images.append(image);
            continue;
        }
        const qint64 key = image.cacheKey();
        if (!display.contains(key)) {
            display.insert(key, image.format() == QImage::Format_RGBA64_Premultiplied
                                   ? ColorPipeline::linearToSrgb(image)
                                   : image);
        }
        m_images.append(display.value(key));
    }
    setAccessibleDescription(description());
    update();
}

void SheetPreview::setGuideState(CutGuides guides)
{
    m_guides = guides;
    update();
}

QString SheetPreview::description() const
{
    return uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Előnézet")) + QStringLiteral(" · ") + QString::number(m_rects.size());
}

void SheetPreview::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    if (!m_sheet.isValid())
        return;

    const QRectF area = QRectF(rect()).adjusted(14.0, 14.0, -14.0, -14.0);
    if (area.width() <= 0.0 || area.height() <= 0.0)
        return;
    const double scale =
        std::min(area.width() / m_sheet.width, area.height() / m_sheet.height);
    const QSizeF pageSize(m_sheet.width * scale, m_sheet.height * scale);
    const QRectF page(area.center().x() - pageSize.width() / 2.0,
                      area.center().y() - pageSize.height() / 2.0, pageSize.width(),
                      pageSize.height());

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(page.translated(2.0, 3.0), QColor(0, 0, 0, 28));
    painter.fillRect(page, Qt::white);
    painter.setPen(QPen(QColor(0xc7, 0xc7, 0xcc), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(page);

    QTransform pageTransform;
    pageTransform.translate(page.left(), page.top());
    pageTransform.scale(scale, scale);

    for (int i = 0; i < m_rects.size(); ++i) {
        const QRectF target = pageTransform.mapRect(m_rects.at(i));
        if (target.width() < 0.5 || target.height() < 0.5)
            continue;
        const QImage image = i < m_images.size() ? m_images.at(i) : QImage();
        if (!image.isNull())
            painter.drawImage(target, image, coverSource(image, target));
        else
            painter.fillRect(target, QColor(0xe6, 0xe6, 0xea));
    }

    if (m_guides != CutGuides::None) {
        QPen pen = m_guides == CutGuides::Print
            ? QPen(QColor(0x7f, 0x7f, 0x7f), 1.0, Qt::SolidLine)
            : QPen(palette().color(QPalette::Highlight), 1.0, Qt::DashLine);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.setTransform(pageTransform);
        for (const QRectF &rectMm : m_rects) {
            for (const QLineF &line : rectEdges(rectMm))
                painter.drawLine(line);
        }
        painter.setTransform(QTransform());
    }
}

// --- SheetToolsDialog -------------------------------------------------------

SheetToolsDialog::SheetToolsDialog(Mode mode, DocumentSnapshot document, int pageIndex,
                                   std::optional<Frame> sourceFrame,
                                   QHash<QString, QImage> previews, bool sourceReadable,
                                   QWidget *parent)
    : QDialog(parent), m_document(std::move(document)), m_pageIndex(pageIndex),
      m_source(std::move(sourceFrame)), m_sourceReadable(sourceReadable),
      m_previews(std::move(previews))
{
    setObjectName(QStringLiteral("sheetToolsDialog"));
    setWindowTitle(mode == Mode::Batch ? uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Képek sokszorosítása")) : uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Fotók rendezése")));
    // The feedback panel below the tabs keeps Create, the warnings and the sheet
    // count in view at every system font size; the settings scroll on their own.
    setMinimumSize(760, 560);
    resize(1020, 720);

    auto *outer = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    body->setSpacing(16);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("sheetToolsTabs"));
    auto *previewColumn = new QVBoxLayout;
    auto *previewTitle = new QLabel(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Előnézet")), this);
    QFont titleFont = previewTitle->font();
    titleFont.setWeight(QFont::DemiBold);
    previewTitle->setFont(titleFont);
    previewColumn->addWidget(previewTitle);
    m_preview = new SheetPreview(this);
    m_preview->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Előnézet")));
    previewColumn->addWidget(m_preview, 1);
    m_caption = new QLabel(this);
    m_caption->setObjectName(QStringLiteral("previewCaption"));
    m_caption->setWordWrap(true);
    m_caption->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    previewColumn->addWidget(m_caption);

    buildBatchTab();
    buildAlignTab();
    body->addWidget(m_tabs, 1);
    body->addLayout(previewColumn, 1);
    outer->addLayout(body, 1);
    buildFeedback();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_createButton = buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
    m_createButton->setObjectName(QStringLiteral("sheetCreateButton"));
    m_createButton->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Mégsem")));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_createButton, &QPushButton::clicked, this, &SheetToolsDialog::accept);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        setWindowTitle(index == kBatchTab ? uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Képek sokszorosítása"))
                                          : uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Fotók rendezése")));
        setCreateText();
        // The create state belongs to the active tab: switching must re-evaluate
        // that tab's inputs, not leave the other tab's answer in place.
        if (index == kBatchTab)
            updateBatch();
        else
            updateAlign();
    });

    m_tabs->setCurrentIndex(mode == Mode::Batch ? kBatchTab : kAlignTab);
    if (m_tabs->count() == 2)
        m_tabs->setTabVisible(mode == Mode::Batch ? kAlignTab : kBatchTab, false);
    setCreateText();
    // Only the active mode fills the shared feedback line: running both would
    // let the hidden tab overwrite the visible one's summary.
    if (m_tabs->currentIndex() == kBatchTab)
        updateBatch();
    else
        updateAlign();
}

void SheetToolsDialog::buildBatchTab()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget(scroll);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(4, 8, 4, 8);
    scroll->setWidget(page);

    auto *intro = new QLabel(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Ugyanaz a fotó több példányban, egyenes rácsban.")), page);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *sourceLabel = new QLabel(page);
    sourceLabel->setObjectName(QStringLiteral("batchSourceLabel"));
    sourceLabel->setWordWrap(true);
    if (m_source) {
        const QString name = QFileInfo(m_source->sourcePath).fileName();
        const QFontMetrics metrics(sourceLabel->font());
        sourceLabel->setText(
            uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Fotó: %1")).arg(metrics.elidedText(name, Qt::ElideMiddle, 360)));
    } else {
        sourceLabel->setText(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Válassz ki egy fotót a sokszorosításhoz.")));
    }
    v->addWidget(sourceLabel);

    auto *placeGroup = new QGroupBox(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Hely")), page);
    auto *placeLayout = new QVBoxLayout(placeGroup);
    m_batchCurrent = new QRadioButton(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Az aktuális lapra")), placeGroup);
    m_batchCurrent->setObjectName(QStringLiteral("batchPlaceCurrent"));
    m_batchNew = new QRadioButton(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Új lapokra")), placeGroup);
    m_batchNew->setObjectName(QStringLiteral("batchPlaceNew"));
    m_batchPlaceNote = new QLabel(placeGroup);
    m_batchPlaceNote->setObjectName(QStringLiteral("batchPlaceNote"));
    m_batchPlaceNote->setWordWrap(true);
    m_batchPlaceNote->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    placeLayout->addWidget(m_batchCurrent);
    placeLayout->addWidget(m_batchPlaceNote);
    placeLayout->addWidget(m_batchNew);
    v->addWidget(placeGroup);


    auto *sizeGroup = new QGroupBox(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Méret")), page);
    auto *sizeForm = new QFormLayout(sizeGroup);
    m_batchPreset = new QComboBox(sizeGroup);
    m_batchPreset->setObjectName(QStringLiteral("batchPreset"));
    m_batchPreset->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Méret")));
    for (const SheetLayout::CopyPreset &preset : SheetLayout::copyPresets()) {
        m_batchPreset->addItem(
            QCoreApplication::translate("PhotoPrint", preset.label.toUtf8().constData()),
            preset.id);
    }
    m_batchPreset->addItem(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Egyedi méret")), QString());
    sizeForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Méret")), m_batchPreset);

    m_batchWidthCm = new QDoubleSpinBox(sizeGroup);
    m_batchWidthCm->setObjectName(QStringLiteral("batchWidthCm"));
    m_batchWidthCm->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Szélesség")));
    m_batchHeightCm = new QDoubleSpinBox(sizeGroup);
    m_batchHeightCm->setObjectName(QStringLiteral("batchHeightCm"));
    m_batchHeightCm->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Magasság")));
    for (QDoubleSpinBox *spin : {m_batchWidthCm, m_batchHeightCm}) {
        spin->setRange(SheetLayout::kMinCopyMm / 10.0, SheetLayout::kMaxCopyMm / 10.0);
        spin->setDecimals(1);
        spin->setSingleStep(0.1);
        spin->setSuffix(QStringLiteral(" cm"));
    }
    sizeForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Szélesség")), m_batchWidthCm);
    sizeForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Magasság")), m_batchHeightCm);
    m_batchSwap = new QPushButton(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Méret csere")), sizeGroup);
    m_batchSwap->setObjectName(QStringLiteral("batchSwap"));
    m_batchSwap->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Szélesség és magasság cseréje")));
    sizeForm->addRow(QString(), m_batchSwap);

    auto *sizeNote = new QLabel(
        uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A 35 × 45 mm az igazolványkép szokásos mérete. Ez az eszköz csak a helyét állítja "
           "be, azt nem ellenőrzi, hogy a fotó megfelel-e az előírásoknak.")),
        sizeGroup);
    sizeNote->setWordWrap(true);
    sizeNote->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    sizeForm->addRow(sizeNote);
    v->addWidget(sizeGroup);

    auto *layoutGroup = new QGroupBox(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Elrendezés")), page);
    auto *layoutForm = new QFormLayout(layoutGroup);
    m_batchGutter = new QDoubleSpinBox(layoutGroup);
    m_batchGutter->setObjectName(QStringLiteral("batchGutter"));
    m_batchGutter->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Térköz")));
    m_batchGutter->setRange(0.0, SheetLayout::kMaxGutterMm);
    m_batchGutter->setDecimals(1);
    m_batchGutter->setSingleStep(0.5);
    m_batchGutter->setSuffix(QStringLiteral(" mm"));
    layoutForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Térköz")), m_batchGutter);

    m_batchMargin = new QDoubleSpinBox(layoutGroup);
    m_batchMargin->setObjectName(QStringLiteral("batchMargin"));
    m_batchMargin->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Legkisebb margó")));
    m_batchMargin->setRange(SheetLayout::kMinMarginMm, SheetLayout::kMaxMarginMm);
    m_batchMargin->setDecimals(1);
    m_batchMargin->setSingleStep(0.5);
    m_batchMargin->setSuffix(QStringLiteral(" mm"));
    layoutForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Legkisebb margó")), m_batchMargin);

    auto *marginHint = new QLabel(
        uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A nyomtató a lap szélétől 3 mm-t nem nyomtat ki, ezért a margó legalább 3,5 mm.")),
        layoutGroup);
    marginHint->setWordWrap(true);
    marginHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    layoutForm->addRow(marginHint);

    auto *countRow = new QWidget(layoutGroup);
    auto *countLayout = new QHBoxLayout(countRow);
    countLayout->setContentsMargins(0, 0, 0, 0);
    m_batchCount = new QSpinBox(countRow);
    m_batchCount->setObjectName(QStringLiteral("batchCount"));
    m_batchCount->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Darabszám")));
    m_batchCount->setRange(1, SheetLayout::maxCount());
    m_batchCount->setSuffix(QStringLiteral(" ") + uiText(QT_TRANSLATE_NOOP("PhotoPrint", "db")));
    m_batchFill = new QCheckBox(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Amennyi befér")), countRow);
    m_batchFill->setObjectName(QStringLiteral("batchFill"));
    countLayout->addWidget(m_batchCount);
    countLayout->addWidget(m_batchFill);
    countLayout->addStretch(1);
    layoutForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Darabszám")), countRow);
    v->addWidget(layoutGroup);

    auto *guideGroup = new QGroupBox(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Vágójelek")), page);
    auto *guideForm = new QFormLayout(guideGroup);
    m_batchGuides = new QComboBox(guideGroup);
    m_batchGuides->setObjectName(QStringLiteral("batchGuides"));
    m_batchGuides->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Vágójelek")));
    m_batchGuides->addItem(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Nincs")), int(CutGuides::None));
    m_batchGuides->addItem(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Képernyőn")), int(CutGuides::Screen));
    m_batchGuides->addItem(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Nyomtatás")), int(CutGuides::Print));
    guideForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Vágójelek")), m_batchGuides);
    m_batchGuideHint = new QLabel(guideGroup);
    m_batchGuideHint->setObjectName(QStringLiteral("batchGuideHint"));
    m_batchGuideHint->setWordWrap(true);
    m_batchGuideHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    guideForm->addRow(m_batchGuideHint);
    v->addWidget(guideGroup);

    v->addStretch(1);

    // Defaults first, then the remembered values on top, clamped as they are
    // read. Remembers only the controls, never the photo or the page.
    m_batchGutter->setValue(3.0);
    m_batchMargin->setValue(SheetLayout::kMinMarginMm);
    m_batchCount->setValue(6);
    m_batchFill->setChecked(true);
    m_batchGuides->setCurrentIndex(0);
    m_batchCurrent->setChecked(true);
    if (const SheetLayout::CopyPreset *preset =
            SheetLayout::findCopyPreset(QStringLiteral("passport"))) {
        m_batchWidthCm->setValue(preset->widthMm / 10.0);
        m_batchHeightCm->setValue(preset->heightMm / 10.0);
    }
    m_batchPreset->setCurrentIndex(0);

    if (settingsEnabled()) {
        QSettings settings;
        const QString sizeId =
            settings.value(QStringLiteral("layout/batch/sizeId"), QStringLiteral("passport"))
                .toString();
        const double customW = clampSetting(
            settings.value(QStringLiteral("layout/batch/customWcm"), 3.5).toDouble(), 3.5,
            SheetLayout::kMinCopyMm / 10.0, SheetLayout::kMaxCopyMm / 10.0);
        const double customH = clampSetting(
            settings.value(QStringLiteral("layout/batch/customHcm"), 4.5).toDouble(), 4.5,
            SheetLayout::kMinCopyMm / 10.0, SheetLayout::kMaxCopyMm / 10.0);
        if (const SheetLayout::CopyPreset *saved = SheetLayout::findCopyPreset(sizeId)) {
            m_batchWidthCm->setValue(saved->widthMm / 10.0);
            m_batchHeightCm->setValue(saved->heightMm / 10.0);
            const QList<SheetLayout::CopyPreset> &presets = SheetLayout::copyPresets();
            for (int i = 0; i < presets.size(); ++i) {
                if (presets.at(i).id == saved->id)
                    m_batchPreset->setCurrentIndex(i);
            }
        } else {
            m_batchWidthCm->setValue(customW);
            m_batchHeightCm->setValue(customH);
            m_batchPreset->setCurrentIndex(m_batchPreset->count() - 1);
        }
        m_batchGutter->setValue(clampSetting(
            settings.value(QStringLiteral("layout/batch/gutterMm"), 3.0).toDouble(), 3.0, 0.0,
            SheetLayout::kMaxGutterMm));
        m_batchMargin->setValue(clampSetting(
            settings.value(QStringLiteral("layout/batch/marginMm"), 3.5).toDouble(), 3.5,
            SheetLayout::kMinMarginMm, SheetLayout::kMaxMarginMm));
        m_batchFill->setChecked(settings.value(QStringLiteral("layout/batch/fill"), true).toBool());
        m_batchCount->setValue(std::clamp(
            settings.value(QStringLiteral("layout/batch/count"), 6).toInt(), 1,
            SheetLayout::maxCount()));
        const QString guide =
            settings.value(QStringLiteral("layout/batch/guide"), QStringLiteral("none")).toString();
        m_batchGuides->setCurrentIndex(guide == QLatin1String("print")
                                           ? 2
                                           : (guide == QLatin1String("screen") ? 1 : 0));
        const QString place =
            settings.value(QStringLiteral("layout/batch/place"), QStringLiteral("current"))
                .toString();
        if (place == QLatin1String("new"))
            m_batchNew->setChecked(true);
    }
    m_batchCount->setEnabled(!m_batchFill->isChecked());

    connect(m_batchPreset, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (const SheetLayout::CopyPreset *chosen =
                SheetLayout::findCopyPreset(m_batchPreset->itemData(index).toString())) {
            m_batchWidthCm->setValue(chosen->widthMm / 10.0);
            m_batchHeightCm->setValue(chosen->heightMm / 10.0);
        }
        updateBatch();
    });
    const auto sizeEdited = [this] {
        const SheetLayout::CopyPreset *chosen =
            SheetLayout::findCopyPreset(m_batchPreset->currentData().toString());
        const bool matches = chosen
            && std::abs(m_batchWidthCm->value() - chosen->widthMm / 10.0) < 0.001
            && std::abs(m_batchHeightCm->value() - chosen->heightMm / 10.0) < 0.001;
        if (!matches) {
            const QSignalBlocker blocker(m_batchPreset);
            m_batchPreset->setCurrentIndex(m_batchPreset->count() - 1);
        }
        updateBatch();
    };
    connect(m_batchWidthCm, &QDoubleSpinBox::valueChanged, this,
            [sizeEdited](double) { sizeEdited(); });
    connect(m_batchHeightCm, &QDoubleSpinBox::valueChanged, this,
            [sizeEdited](double) { sizeEdited(); });
    connect(m_batchSwap, &QPushButton::clicked, this, [this] {
        const double width = m_batchWidthCm->value();
        m_batchWidthCm->setValue(m_batchHeightCm->value());
        m_batchHeightCm->setValue(width);
        updateBatch();
    });
    connect(m_batchGutter, &QDoubleSpinBox::valueChanged, this, [this](double) { updateBatch(); });
    connect(m_batchMargin, &QDoubleSpinBox::valueChanged, this, [this](double) { updateBatch(); });
    connect(m_batchCount, &QSpinBox::valueChanged, this, [this](int) { updateBatch(); });
    connect(m_batchFill, &QCheckBox::toggled, this, [this](bool) {
        // "Amennyi befér" owns the count: the spin follows the fit and is
        // disabled, so a typed number can never disagree with the preview.
        m_batchCount->setEnabled(!m_batchFill->isChecked());
        updateBatch();
    });
    connect(m_batchGuides, &QComboBox::currentIndexChanged, this, [this](int) { updateBatch(); });
    connect(m_batchCurrent, &QRadioButton::toggled, this, [this](bool) { updateBatch(); });

    m_tabs->addTab(scroll, uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Képek sokszorosítása")));
}

void SheetToolsDialog::buildAlignTab()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget(scroll);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(4, 8, 4, 8);
    scroll->setWidget(page);

    auto *intro = new QLabel(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A lapon lévő fotók elrendezése. A sorrend nem változik.")), page);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *modeRow = new QHBoxLayout;
    m_alignGridMode = new QRadioButton(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Rácsba")), page);
    m_alignGridMode->setObjectName(QStringLiteral("alignGridMode"));
    m_alignGapMode = new QRadioButton(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Térköz kiegyenlítése")), page);
    m_alignGapMode->setObjectName(QStringLiteral("alignGapMode"));
    m_alignGridMode->setChecked(true);
    modeRow->addWidget(m_alignGridMode);
    modeRow->addWidget(m_alignGapMode);
    modeRow->addStretch(1);
    v->addLayout(modeRow);

    m_alignStack = new QStackedWidget(page);
    m_alignStack->setObjectName(QStringLiteral("alignStack"));
    v->addWidget(m_alignStack);

    auto *gridPage = new QWidget(m_alignStack);
    auto *gridLayout = new QVBoxLayout(gridPage);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    auto *gridHint = new QLabel(
        uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A fotók egyforma cellákba kerülnek. A kép kitölti a cellát, a kilógó rész "
           "lemarad.")),
        gridPage);
    gridHint->setWordWrap(true);
    gridHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    gridLayout->addWidget(gridHint);

    auto *gridForm = new QFormLayout;
    m_alignMargin = new QDoubleSpinBox(gridPage);
    m_alignMargin->setObjectName(QStringLiteral("alignMargin"));
    m_alignMargin->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Külső margó")));
    m_alignMargin->setRange(SheetLayout::kMinMarginMm, SheetLayout::kMaxMarginMm);
    m_alignMargin->setDecimals(1);
    m_alignMargin->setSingleStep(0.5);
    m_alignMargin->setSuffix(QStringLiteral(" mm"));
    gridForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Külső margó")), m_alignMargin);

    m_alignGutter = new QDoubleSpinBox(gridPage);
    m_alignGutter->setObjectName(QStringLiteral("alignGutter"));
    m_alignGutter->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Térköz")));
    m_alignGutter->setRange(0.0, SheetLayout::kMaxGutterMm);
    m_alignGutter->setDecimals(1);
    m_alignGutter->setSingleStep(0.5);
    m_alignGutter->setSuffix(QStringLiteral(" mm"));
    gridForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Térköz")), m_alignGutter);

    m_alignColumns = new QComboBox(gridPage);
    m_alignColumns->setObjectName(QStringLiteral("alignColumns"));
    m_alignColumns->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Oszlopok")));
    gridForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Oszlopok")), m_alignColumns);
    gridLayout->addLayout(gridForm);

    m_alignFill = new QCheckBox(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A kép egésze látszódjon")), gridPage);
    m_alignFill->setObjectName(QStringLiteral("alignFill"));
    gridLayout->addWidget(m_alignFill);
    auto *fillHint = new QLabel(
        uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Bekapcsolva a kép nem tölti ki a cellát; a hiányzó rész fehér marad.")), gridPage);
    fillHint->setWordWrap(true);
    fillHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    gridLayout->addWidget(fillHint);
    gridLayout->addStretch(1);
    m_alignStack->addWidget(gridPage);

    auto *gapPage = new QWidget(m_alignStack);
    auto *gapLayout = new QVBoxLayout(gapPage);
    gapLayout->setContentsMargins(0, 0, 0, 0);
    auto *gapHint = new QLabel(
        uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A fotók megtartják a méretüket. A köztük lévő hely lesz egyenlő.")), gapPage);
    gapHint->setWordWrap(true);
    gapHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    gapLayout->addWidget(gapHint);

    auto *gapForm = new QFormLayout;
    m_alignAxis = new QComboBox(gapPage);
    m_alignAxis->setObjectName(QStringLiteral("alignAxis"));
    m_alignAxis->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Irány")));
    m_alignAxis->addItem(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Egymás mellett")), int(SheetLayout::DistributeAxis::Horizontal));
    m_alignAxis->addItem(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Egymás alatt")), int(SheetLayout::DistributeAxis::Vertical));
    gapForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Irány")), m_alignAxis);
    m_alignGap = new QDoubleSpinBox(gapPage);
    m_alignGap->setObjectName(QStringLiteral("alignGap"));
    m_alignGap->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Térköz")));
    m_alignGap->setRange(0.0, SheetLayout::kMaxGutterMm);
    m_alignGap->setDecimals(1);
    m_alignGap->setSingleStep(0.5);
    m_alignGap->setSuffix(QStringLiteral(" mm"));
    gapForm->addRow(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Térköz")), m_alignGap);
    gapLayout->addLayout(gapForm);
    auto *gapNote = new QLabel(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Ha nem fér el, a program csökkenti.")), gapPage);
    gapNote->setWordWrap(true);
    gapNote->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    gapLayout->addWidget(gapNote);
    gapLayout->addStretch(1);
    m_alignStack->addWidget(gapPage);

    v->addStretch(1);

    const SheetLayout::Sheet sheet = m_pageIndex >= 0 && m_pageIndex < m_document.pages.size()
        ? SheetLayout::sheetOf(m_document.pages.at(m_pageIndex))
        : SheetLayout::Sheet{};
    double margin = defaultAlignMargin(sheet);
    double gutter = margin;
    double gap = 5.0;
    int mode = 0;
    int axis = int(SheetLayout::DistributeAxis::Horizontal);
    bool fill = true;
    if (settingsEnabled()) {
        QSettings settings;
        const QString savedMode =
            settings.value(QStringLiteral("layout/align/mode"), QStringLiteral("grid")).toString();
        mode = savedMode == QLatin1String("gap") ? 1 : 0;
        margin = clampSetting(
            settings.value(QStringLiteral("layout/align/marginMm"), margin).toDouble(), margin,
            SheetLayout::kMinMarginMm, SheetLayout::kMaxMarginMm);
        gutter = clampSetting(
            settings.value(QStringLiteral("layout/align/gutterMm"), gutter).toDouble(), gutter,
            0.0, SheetLayout::kMaxGutterMm);
        fill = settings.value(QStringLiteral("layout/align/fill"), true).toBool();
        axis = settings.value(QStringLiteral("layout/align/axis"), QStringLiteral("h")).toString()
                == QLatin1String("v")
            ? int(SheetLayout::DistributeAxis::Vertical)
            : int(SheetLayout::DistributeAxis::Horizontal);
        gap = clampSetting(settings.value(QStringLiteral("layout/align/gapMm"), 5.0).toDouble(),
                           5.0, 0.0, SheetLayout::kMaxGutterMm);
    }
    m_alignMargin->setValue(margin);
    m_alignGutter->setValue(gutter);
    // The switch says "the whole photo is visible", the geometry stores Fill.
    m_alignFill->setChecked(!fill);
    m_alignGap->setValue(gap);
    m_alignAxis->setCurrentIndex(axis == int(SheetLayout::DistributeAxis::Vertical) ? 1 : 0);
    m_alignGapMode->setChecked(mode == 1);
    m_alignGridMode->setChecked(mode != 1);
    m_alignStack->setCurrentIndex(mode);

    connect(m_alignGridMode, &QRadioButton::toggled, this, [this](bool grid) {
        m_alignStack->setCurrentIndex(grid ? 0 : 1);
        updateAlign();
    });
    connect(m_alignMargin, &QDoubleSpinBox::valueChanged, this, [this](double) { updateAlign(); });
    connect(m_alignGutter, &QDoubleSpinBox::valueChanged, this, [this](double) { updateAlign(); });
    connect(m_alignColumns, &QComboBox::currentIndexChanged, this, [this](int) { updateAlign(); });
    connect(m_alignFill, &QCheckBox::toggled, this, [this](bool) { updateAlign(); });
    connect(m_alignAxis, &QComboBox::currentIndexChanged, this, [this](int) { updateAlign(); });
    connect(m_alignGap, &QDoubleSpinBox::valueChanged, this, [this](double) { updateAlign(); });

    m_tabs->addTab(scroll, uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Fotók rendezése")));
}

SheetLayout::BatchSpec SheetToolsDialog::batchSpec() const
{
    SheetLayout::BatchSpec spec;
    spec.copyWidthMm = SheetLayout::copyMm(m_batchWidthCm->value() * 10.0, 35.0);
    spec.copyHeightMm = SheetLayout::copyMm(m_batchHeightCm->value() * 10.0, 45.0);
    spec.gutterMm = SheetLayout::gutterMm(m_batchGutter->value());
    spec.marginMm = SheetLayout::marginMm(m_batchMargin->value());
    spec.fillPage = m_batchFill->isChecked();
    spec.count = m_batchCount->value();
    if (m_source) {
        const auto image = m_previews.constFind(m_source->id);
        if (image != m_previews.constEnd() && !image->isNull() && image->width() > 0) {
            double aspect = double(image->height()) / double(image->width());
            const double turns = std::fmod(std::abs(m_source->imageRotationDeg), 180.0);
            if (std::abs(turns - 90.0) < 1e-6)
                aspect = 1.0 / aspect;
            spec.sourceAspect = aspect;
        }
    }
    return spec;
}

SheetLayout::AlignSpec SheetToolsDialog::alignSpec() const
{
    SheetLayout::AlignSpec spec;
    spec.marginMm = m_alignMargin->value();
    spec.gutterMm = m_alignGutter->value();
    spec.columns = m_alignColumns->currentData().toInt();
    spec.fill = !m_alignFill->isChecked();
    return spec;
}

SheetLayout::DistributeSpec SheetToolsDialog::distributeSpec() const
{
    SheetLayout::DistributeSpec spec;
    spec.axis = m_alignAxis->currentData().toInt() == int(SheetLayout::DistributeAxis::Vertical)
        ? SheetLayout::DistributeAxis::Vertical
        : SheetLayout::DistributeAxis::Horizontal;
    spec.gapMm = m_alignGap->value();
    spec.marginMm = m_alignMargin->value();
    return spec;
}

QList<QSize> SheetToolsDialog::sourcePixels() const
{
    QList<QSize> sizes;
    if (m_pageIndex < 0 || m_pageIndex >= m_document.pages.size())
        return sizes;
    for (const Frame &frame : m_document.pages.at(m_pageIndex).frames) {
        const auto image = m_previews.constFind(frame.id);
        sizes.append(image != m_previews.constEnd() ? image->size() : QSize());
    }
    return sizes;
}

QList<Frame> SheetToolsDialog::freeFrames(const QList<Frame> &frames) const
{
    QList<Frame> result = frames;
    for (Frame &frame : result)
        frame.slotId.clear();
    return result;
}

void SheetToolsDialog::buildFeedback()
{
    m_feedback = new QWidget(this);
    m_feedback->setObjectName(QStringLiteral("sheetFeedback"));
    auto *feedbackLayout = new QVBoxLayout(m_feedback);
    feedbackLayout->setContentsMargins(0, 6, 0, 0);
    feedbackLayout->setSpacing(4);

    auto *summaryRow = new QHBoxLayout;
    m_summary = new QLabel(m_feedback);
    m_summary->setObjectName(QStringLiteral("sheetSummary"));
    m_summary->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Összegzés")));
    m_summary->setWordWrap(true);
    QFont summaryFont = m_summary->font();
    summaryFont.setWeight(QFont::DemiBold);
    m_summary->setFont(summaryFont);
    summaryRow->addWidget(m_summary, 1);

    m_prevPage = new QPushButton(QStringLiteral("‹"), m_feedback);
    m_prevPage->setObjectName(QStringLiteral("sheetPrevPage"));
    m_prevPage->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Előző lap")));
    m_prevPage->setFixedWidth(30);
    summaryRow->addWidget(m_prevPage);
    m_pageLabel = new QLabel(m_feedback);
    m_pageLabel->setObjectName(QStringLiteral("sheetPageLabel"));
    summaryRow->addWidget(m_pageLabel);
    m_nextPage = new QPushButton(QStringLiteral("›"), m_feedback);
    m_nextPage->setObjectName(QStringLiteral("sheetNextPage"));
    m_nextPage->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Következő lap")));
    m_nextPage->setFixedWidth(30);
    summaryRow->addWidget(m_nextPage);
    feedbackLayout->addLayout(summaryRow);

    m_warning = new QLabel(m_feedback);
    m_warning->setObjectName(QStringLiteral("sheetWarning"));
    m_warning->setAccessibleName(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Figyelmeztetés")));
    m_warning->setWordWrap(true);
    m_warning->setStyleSheet(QStringLiteral("color: #8a5a00;"));
    m_warning->setVisible(false);
    feedbackLayout->addWidget(m_warning);

    m_note = new QLabel(m_feedback);
    m_note->setObjectName(QStringLiteral("sheetLastSheetNote"));
    m_note->setWordWrap(true);
    m_note->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    m_note->setVisible(false);
    feedbackLayout->addWidget(m_note);

    m_exactNote = new QLabel(
        uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A példányok pontosan a választott méretben nyomtatódnak, nagyítás nélkül.")),
        m_feedback);
    m_exactNote->setObjectName(QStringLiteral("sheetExactSizeNote"));
    m_exactNote->setWordWrap(true);
    m_exactNote->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    feedbackLayout->addWidget(m_exactNote);

    layout()->addWidget(m_feedback);

    connect(m_prevPage, &QPushButton::clicked, this, [this] {
        if (!m_batchGrid)
            return;
        m_previewPage = std::max(0, m_previewPage - 1);
        updatePageNavigation();
        updatePreview();
        announce();
    });
    connect(m_nextPage, &QPushButton::clicked, this, [this] {
        if (!m_batchGrid)
            return;
        m_previewPage = std::min(m_batchGrid->pageCount() - 1, m_previewPage + 1);
        updatePageNavigation();
        updatePreview();
        announce();
    });
}

void SheetToolsDialog::applyFeedback()
{
    if (!m_summary)
        return;
    m_summary->setText(m_summaryText);
    m_warning->setText(m_warningText);
    m_warning->setVisible(!m_warningText.isEmpty());
}

void SheetToolsDialog::updatePageNavigation()
{
    if (!m_pageLabel)
        return;
    const bool multiple = m_tabs->currentIndex() == kBatchTab && m_batchGrid
        && m_batchGrid->pageCount() > 1;
    m_prevPage->setVisible(multiple);
    m_nextPage->setVisible(multiple);
    m_pageLabel->setVisible(multiple);
    if (!multiple)
        return;
    m_previewPage = std::clamp(m_previewPage, 0, m_batchGrid->pageCount() - 1);
    m_pageLabel->setText(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "%1/%2. lap"))
                             .arg(m_previewPage + 1)
                             .arg(m_batchGrid->pageCount()));
    m_prevPage->setEnabled(m_previewPage > 0);
    m_nextPage->setEnabled(m_previewPage + 1 < m_batchGrid->pageCount());
}

void SheetToolsDialog::announce()
{
    if (!m_preview)
        return;
    QString description = m_summaryText;
    if (!m_warningText.isEmpty())
        description += QLatin1Char('\n') + m_warningText;
    if (m_note && !m_note->isHidden() && !m_note->text().isEmpty())
        description += QLatin1Char('\n') + m_note->text();
    if (m_pageLabel && !m_pageLabel->isHidden())
        description += QLatin1Char('\n') + m_pageLabel->text();
    // The preview announces the current fit count and every warning, so a
    // screen reader hears what the sighted user reads below the tabs.
    m_preview->setAccessibleDescription(description);
    QAccessibleEvent event(m_summary, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}

void SheetToolsDialog::updateBatch()
{
    m_batchGrid.reset();
    m_summaryText.clear();
    m_warningText.clear();
    const bool onTab = m_tabs->currentIndex() == kBatchTab;

    const auto refuse = [this, onTab](const QString &message) {
        m_summaryText = message;
        m_warningText.clear();
        m_note->clear();
        m_note->setVisible(false);
        m_prevPage->setVisible(false);
        m_nextPage->setVisible(false);
        m_pageLabel->setVisible(false);
        m_exactNote->setVisible(false);
        if (onTab)
            m_createButton->setEnabled(false);
        applyFeedback();
        setCreateText();
        updatePreview();
        announce();
    };

    // The hint follows the choice instead of describing the default forever.
    const int guideValue = m_batchGuides->currentData().toInt();
    if (m_batchGuideHint) {
        m_batchGuideHint->setText(
            guideValue == int(CutGuides::Print)
                ? uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A nyomaton is megjelenik; a vágás a vonal közepén halad, nyomtatás "
                         "előtt megerősítést kér."))
                : (guideValue == int(CutGuides::Screen)
                       ? uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Csak a képernyőn látszik, a nyomatra nem kerül."))
                       : uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Nincs vágójel a képernyőn és a nyomaton."))));
    }

    if (m_pageIndex < 0 || m_pageIndex >= m_document.pages.size() || !m_source) {
        refuse(m_source ? uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Érvénytelen papírméret."))
                        : uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Válassz ki egy fotót a sokszorosításhoz.")));
        return;
    }
    if (!m_sourceReadable) {
        refuse(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A kiválasztott fotó nem olvasható.")));
        return;
    }

    const Page &page = m_document.pages.at(m_pageIndex);
    const SheetLayout::BatchSpec spec = batchSpec();
    const auto grid = SheetLayout::batchGrid(page, spec);
    if (!grid) {
        refuse(grid.error().message);
        return;
    }

    m_batchGrid = grid.value();
    const SheetLayout::BatchGrid &chosen = *m_batchGrid;
    if (m_batchFill->isChecked() && m_batchCount->value() != chosen.perPage) {
        const QSignalBlocker blocker(m_batchCount);
        m_batchCount->setValue(chosen.perPage);
    }

    QString summary = uiText(QT_TRANSLATE_NOOP("PhotoPrint", "%n példány fér el egy lapra."), chosen.perPage);
    if (chosen.pageCount() > 1)
        summary += QLatin1Char(' ') + uiText(QT_TRANSLATE_NOOP("PhotoPrint", "%n lapra kerül."), chosen.pageCount());
    // A full sheet of very small copies hits the per-page cap: "as many as fit"
    // then means the cap, and the summary has to say which number stopped it.
    // The shaped block of a single sheet is not the raw capacity, so count the
    // cells the paper would hold before the cap.
    const double rawWidth = chosen.sheet.width - 2.0 * chosen.marginMm;
    const double rawHeight = chosen.sheet.height - 2.0 * chosen.marginMm;
    const double cell = chosen.copyWidthMm + chosen.gutterMm;
    const double cellY = chosen.copyHeightMm + chosen.gutterMm;
    const qint64 rawCapacity = cell > 0.0 && cellY > 0.0
        ? qint64(std::floor((rawWidth + chosen.gutterMm) / cell))
            * qint64(std::floor((rawHeight + chosen.gutterMm) / cellY))
        : 0;
    if (spec.fillPage && rawCapacity > chosen.perPage) {
        summary += QLatin1Char(' ')
            + uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Egy lapra legfeljebb %1 példány kerül.")).arg(chosen.perPage);
    }
    if (!spec.fillPage && spec.count >= SheetLayout::maxCount()) {
        summary += QLatin1Char(' ')
            + uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Egyszerre legfeljebb %1 példány készíthető."))
                  .arg(SheetLayout::maxCount());
    }
    m_summaryText = summary;

    QStringList warnings;
    if (m_source && std::abs(m_source->angleDeg) > 1e-9) {
        warnings << uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A kiválasztott fotó el van forgatva. A példányok egyenesen kerülnek a "
                           "rácsba."));
    }
    if (chosen.minVisible < 0.6)
        warnings << uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A kiválasztott fotó szélei lemaradnak."));
    if (guideValue == int(CutGuides::Print) && chosen.marginMm < 5.0) {
        warnings << uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Ilyen szoros margónál a nyomtató a szélső vágójelet levághatja."));
    }
    if (!m_batchNew->isChecked() && page.hasLayout()) {
        warnings << uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A lap elrendezése megszűnik, a példányok szabadon kerülnek a lapra."));
    }
    const CutGuides guideChoice = static_cast<CutGuides>(guideValue);
    if (!m_batchNew->isChecked() && page.cutGuides != guideChoice) {
        // The chosen guide state is written to the target page as part of the
        // same edit. Say so before the user presses Create.
        warnings << uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A lap vágójel-beállítása is módosul: %1."))
                        .arg(m_batchGuides->currentText());
    }
    m_warningText = warnings.join(QLatin1Char('\n'));

    if (m_batchNew->isChecked())
        m_batchPlaceNote->clear();
    else if (!page.frames.isEmpty())
        m_batchPlaceNote->setText(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "%n fotó helyére"), page.frames.size()));
    else
        m_batchPlaceNote->clear();

    m_note->setText(chosen.pageCount() > 1
                        ? uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A maradék az első kockáktól töltődik, hogy a lapok együtt "
                                 "vághatók legyenek."))
                        : QString());
    m_note->setVisible(chosen.pageCount() > 1);
    m_exactNote->setVisible(true);
    m_previewPage = std::clamp(m_previewPage, 0, chosen.pageCount() - 1);
    if (onTab)
        m_createButton->setEnabled(true);
    applyFeedback();
    updatePageNavigation();
    setCreateText();
    updatePreview();
    announce();
}

void SheetToolsDialog::updateAlign()
{
    m_alignPlan.reset();
    m_distributePlan.reset();
    m_alignFrames.clear();
    m_alignValid = false;
    m_summaryText.clear();
    m_warningText.clear();
    const bool onTab = m_tabs->currentIndex() == kAlignTab;

    const auto refuse = [this, onTab](const QString &message) {
        m_summaryText = message;
        m_warningText.clear();
        m_note->clear();
        m_note->setVisible(false);
        m_prevPage->setVisible(false);
        m_nextPage->setVisible(false);
        m_pageLabel->setVisible(false);
        m_exactNote->setVisible(false);
        if (onTab)
            m_createButton->setEnabled(false);
        applyFeedback();
        setCreateText();
        updatePreview();
        announce();
    };

    if (m_pageIndex < 0 || m_pageIndex >= m_document.pages.size()) {
        refuse(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Ezen a lapon nincs fotó.")));
        return;
    }
    const Page &page = m_document.pages.at(m_pageIndex);
    const int count = int(page.frames.size());

    // The columns combo follows the page. Rebuilding it on every update would
    // reset a choice the user just made, so it is rebuilt only when the frame
    // count changes. Its data is the column count, 0 for automatic.
    if (m_alignColumns->count() != count + 1) {
        const int previous = m_alignColumns->currentData().toInt();
        const QSignalBlocker blocker(m_alignColumns);
        m_alignColumns->clear();
        m_alignColumns->addItem(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Automatikus")), 0);
        for (int columns = 1; columns <= count; ++columns)
            m_alignColumns->addItem(QString::number(columns), columns);
        const int index = m_alignColumns->findData(previous);
        m_alignColumns->setCurrentIndex(index >= 0 ? index : 0);
    }

    if (page.hasLayout()) {
        refuse(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Ehhez a laphoz elrendezés tartozik. Szüntesd meg az elrendezést az "
                      "igazításhoz.")));
        return;
    }
    if (count == 0) {
        refuse(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Ezen a lapon nincs fotó.")));
        return;
    }

    const QList<QSize> pixels = sourcePixels();
    if (m_alignGridMode->isChecked()) {
        const SheetLayout::AlignSpec spec = alignSpec();
        const auto plan = SheetLayout::alignGrid(page, spec, pixels);
        if (!plan) {
            refuse(plan.error().message);
            return;
        }
        m_alignPlan = plan.value();
        const auto frames = SheetLayout::alignFrames(page, spec, pixels);
        if (!frames) {
            refuse(frames.error().message);
            return;
        }
        m_alignFrames = frames.value();
        m_alignValid = true;
        if (count == 1) {
            m_summaryText =
                uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Egy fotó van a lapon. Csak középre igazítjuk, a mérete nem változik."));
        } else {
            m_summaryText =
                uiText(QT_TRANSLATE_NOOP("PhotoPrint", "%n fotó: %1 oszlop × %2 sor. Egy cella %3 × %4 mm."), count)
                    .arg(m_alignPlan->columns)
                    .arg(m_alignPlan->rows)
                    .arg(mmText(m_alignPlan->cellWidthMm), mmText(m_alignPlan->cellHeightMm));
            const bool strongCrop = spec.fill && m_alignPlan->minVisible < 0.5;
            if (strongCrop) {
                // Both alternatives are actions this dialog actually offers. The
                // old text suggested spreading the photos over more pages, which
                // this product cannot do.
                m_warningText =
                    uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A fotók egy része erősen lemarad. Kapcsold be a »A kép egésze "
                           "látszódjon« opciót, vagy válaszd a »Térköz kiegyenlítése« módot."));
            }
        }
    } else {
        const SheetLayout::DistributeSpec spec = distributeSpec();
        const auto plan = SheetLayout::distribute(page, spec);
        if (!plan) {
            refuse(plan.error().message);
            return;
        }
        m_distributePlan = plan.value();
        const auto frames = SheetLayout::distributeFrames(page, spec);
        if (!frames) {
            refuse(frames.error().message);
            return;
        }
        m_alignFrames = frames.value();
        m_alignValid = true;
        if (count == 1) {
            m_summaryText =
                uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Egy fotó van a lapon. Csak középre igazítjuk, a mérete nem változik."));
        } else {
            m_summaryText = uiText(QT_TRANSLATE_NOOP("PhotoPrint", "%n fotó, %1 mm térköz. A margó %2 mm."), count)
                                .arg(mmText(m_distributePlan->gapMm),
                                     mmText(m_distributePlan->startMm));
            if (m_distributePlan->gapMm < m_distributePlan->requestedGapMm) {
                m_warningText = uiText(QT_TRANSLATE_NOOP("PhotoPrint", "A térköz %1 mm lett, hogy a fotók elférjenek."))
                                    .arg(mmText(m_distributePlan->gapMm));
            }
        }
    }

    m_note->clear();
    m_note->setVisible(false);
    m_exactNote->setVisible(false);
    m_prevPage->setVisible(false);
    m_nextPage->setVisible(false);
    m_pageLabel->setVisible(false);
    if (onTab)
        m_createButton->setEnabled(true);
    applyFeedback();
    setCreateText();
    updatePreview();
    announce();
}

void SheetToolsDialog::updatePreview()
{
    if (!m_preview)
        return;
    if (m_tabs->currentIndex() == kBatchTab) {
        if (!m_batchGrid || m_batchGrid->pagesMm.isEmpty()) {
            m_preview->setSheetMm(m_pageIndex >= 0 && m_pageIndex < m_document.pages.size()
                                      ? SheetLayout::sheetOf(m_document.pages.at(m_pageIndex))
                                      : SheetLayout::Sheet{});
            m_preview->setRects({}, {});
            m_preview->setGuideState(CutGuides::None);
            m_caption->clear();
            return;
        }
        const SheetLayout::BatchGrid &grid = *m_batchGrid;
        m_previewPage = std::clamp(m_previewPage, 0, grid.pageCount() - 1);
        const QList<QRectF> &rects = grid.pagesMm.at(m_previewPage);
        QList<QImage> images;
        const QImage source = m_source ? m_previews.value(m_source->id) : QImage();
        for (int i = 0; i < rects.size(); ++i)
            images.append(source);
        m_preview->setSheetMm(grid.sheet);
        m_preview->setRects(rects, images);
        m_preview->setGuideState(static_cast<CutGuides>(m_batchGuides->currentData().toInt()));
        m_caption->setText(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Így kerül a lapra.")) + QLatin1Char('\n')
                           + uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Szélek: %1 mm oldalt, %2 mm felül és alul."))
                                 .arg(mmText(grid.leftMarginMm), mmText(grid.topMarginMm))
                           + QLatin1Char('\n') + uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Az előnézet hozzávetőleges")));
        return;
    }

    if (m_pageIndex < 0 || m_pageIndex >= m_document.pages.size()) {
        m_preview->setSheetMm(SheetLayout::Sheet{});
        m_preview->setRects({}, {});
        m_preview->setGuideState(CutGuides::None);
        m_caption->clear();
        return;
    }
    const Page &page = m_document.pages.at(m_pageIndex);
    QList<QRectF> rects;
    QList<QImage> images;
    for (const Frame &frame : m_alignFrames) {
        rects.append(QRectF(frame.cx - frame.width / 2.0, frame.cy - frame.height / 2.0,
                            frame.width, frame.height));
        images.append(m_previews.value(frame.id));
    }
    m_preview->setSheetMm(SheetLayout::sheetOf(page));
    m_preview->setRects(rects, images);
    m_preview->setGuideState(page.cutGuides);
    m_caption->setText(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Így kerül a lapra.")) + QLatin1Char('\n')
                       + uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Az előnézet hozzávetőleges")));
}

void SheetToolsDialog::setCreateText()
{
    if (!m_createButton)
        return;
    if (m_tabs->currentIndex() != kBatchTab) {
        m_createButton->setText(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Rendezés")));
        return;
    }
    if (m_batchGrid && m_batchGrid->pageCount() > 1) {
        // The action itself names how many sheets it will add or replace.
        m_createButton->setText(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Létrehozás (%n lap)"), m_batchGrid->pageCount()));
    } else {
        m_createButton->setText(uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Létrehozás")));
    }
}

bool SheetToolsDialog::buildBatchEdit(ApplyLayoutEdit *out)
{
    if (m_pageIndex < 0 || m_pageIndex >= m_document.pages.size() || !m_source)
        return false;
    const SheetLayout::BatchSpec spec = batchSpec();
    const auto pages = SheetLayout::batchPages(m_document.pages.at(m_pageIndex), spec, *m_source);
    if (!pages || pages.value().isEmpty())
        return false;

    const CutGuides guides = static_cast<CutGuides>(m_batchGuides->currentData().toInt());
    QList<Page> generated = pages.value();
    for (Page &page : generated) {
        page.frames = freeFrames(page.frames);
        page.cutGuides = guides;
        // A copy is a fixed-size print, so a batch sheet never inherits the
        // printer's bleed: borderless scales the content by the cover factor and
        // would print 35 × 45 as 35.6 × 45.7 on 10 × 15 cm. Off is the exact
        // size, which is the whole point of the tool.
        page.borderlessOverride = BorderlessOverride::Off;
    }

    ApplyLayoutEdit edit;
    edit.actionText = uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Képek sokszorosítása"));
    edit.insertIndex = m_pageIndex + 1;
    if (m_batchNew->isChecked()) {
        for (const Page &page : generated)
            edit.newPages.append(page);
        edit.selectPage = m_pageIndex + 1;
    } else {
        PageLayout change;
        change.pageIndex = m_pageIndex;
        change.frames = generated.first().frames;
        change.cutGuides = guides;
        change.borderlessOverride = BorderlessOverride::Off;
        // The page is a copy sheet now: a previous layout's cells would dangle
        // over free frames, so the replacement dissolves the layout in the same
        // undoable step.
        change.slotList = QList<Slot>{};
        change.layoutId = QString();
        edit.pages.append(change);
        for (int i = 1; i < generated.size(); ++i)
            edit.newPages.append(generated.at(i));
        edit.selectPage = m_pageIndex;
    }
    *out = edit;
    return true;
}

bool SheetToolsDialog::buildAlignEdit(ApplyLayoutEdit *out)
{
    if (!m_alignValid || m_pageIndex < 0 || m_pageIndex >= m_document.pages.size())
        return false;
    QList<Frame> frames;
    if (m_alignGridMode->isChecked()) {
        const auto aligned =
            SheetLayout::alignFrames(m_document.pages.at(m_pageIndex), alignSpec(), sourcePixels());
        if (!aligned)
            return false;
        frames = aligned.value();
    } else {
        const auto distributed = SheetLayout::distributeFrames(
            m_document.pages.at(m_pageIndex), distributeSpec());
        if (!distributed)
            return false;
        frames = distributed.value();
    }
    if (frames.isEmpty())
        return false;

    ApplyLayoutEdit edit;
    PageLayout change;
    change.pageIndex = m_pageIndex;
    change.frames = frames;
    edit.pages.append(change);
    edit.actionText = uiText(QT_TRANSLATE_NOOP("PhotoPrint", "Fotók rendezése"));
    edit.selectPage = m_pageIndex;
    *out = edit;
    return true;
}

void SheetToolsDialog::accept()
{
    // A disabled Create is a refusal, not a shortcut: nothing is built even if
    // a caller reaches accept() directly.
    if (!m_createButton || !m_createButton->isEnabled())
        return;
    ApplyLayoutEdit built;
    const bool batch = m_tabs->currentIndex() == kBatchTab;
    const bool ok = batch ? buildBatchEdit(&built) : buildAlignEdit(&built);
    if (!ok)
        return;
    m_edit = built;
    m_hasEdit = true;
    m_replacesCurrentPage = batch && m_batchNew && !m_batchNew->isChecked()
        && !m_edit.pages.isEmpty();
    saveSettings();
    QDialog::accept();
}

void SheetToolsDialog::saveSettings() const
{
    if (!settingsEnabled())
        return;
    QSettings settings;
    if (m_tabs->currentIndex() == kBatchTab) {
        const double copyWidth = m_batchWidthCm->value();
        const double copyHeight = m_batchHeightCm->value();
        const SheetLayout::CopyPreset *preset =
            SheetLayout::findCopyPreset(m_batchPreset->currentData().toString());
        const bool presetMatches = preset
            && std::abs(copyWidth - preset->widthMm / 10.0) < 0.001
            && std::abs(copyHeight - preset->heightMm / 10.0) < 0.001;
        settings.setValue(QStringLiteral("layout/batch/sizeId"),
                          presetMatches ? preset->id : QString());
        settings.setValue(QStringLiteral("layout/batch/customWcm"), copyWidth);
        settings.setValue(QStringLiteral("layout/batch/customHcm"), copyHeight);
        settings.setValue(QStringLiteral("layout/batch/gutterMm"), m_batchGutter->value());
        settings.setValue(QStringLiteral("layout/batch/marginMm"), m_batchMargin->value());
        settings.setValue(QStringLiteral("layout/batch/fill"), m_batchFill->isChecked());
        settings.setValue(QStringLiteral("layout/batch/count"), m_batchCount->value());
        const int guide = m_batchGuides->currentData().toInt();
        settings.setValue(QStringLiteral("layout/batch/guide"),
                          guide == int(CutGuides::Print)
                              ? QStringLiteral("print")
                              : (guide == int(CutGuides::Screen) ? QStringLiteral("screen")
                                                                 : QStringLiteral("none")));
        settings.setValue(QStringLiteral("layout/batch/place"),
                          m_batchNew->isChecked() ? QStringLiteral("new")
                                                  : QStringLiteral("current"));
    } else {
        const bool gridMode = m_alignGridMode->isChecked();
        settings.setValue(QStringLiteral("layout/align/mode"),
                          gridMode ? QStringLiteral("grid") : QStringLiteral("gap"));
        settings.setValue(QStringLiteral("layout/align/marginMm"), m_alignMargin->value());
        settings.setValue(QStringLiteral("layout/align/gutterMm"), m_alignGutter->value());
        settings.setValue(QStringLiteral("layout/align/fill"), !m_alignFill->isChecked());
        settings.setValue(QStringLiteral("layout/align/axis"),
                          m_alignAxis->currentData().toInt()
                                  == int(SheetLayout::DistributeAxis::Vertical)
                              ? QStringLiteral("v")
                              : QStringLiteral("h"));
        settings.setValue(QStringLiteral("layout/align/gapMm"), m_alignGap->value());
    }
}

bool SheetToolsDialog::replacesCurrentPage() const
{
    // Only a batch edit that wrote the current page's frame list replaces it.
    // An align edit also carries a PageLayout, but it keeps the page's identity
    // and its guides, so it must not trigger the replacement question.
    return m_hasEdit && m_replacesCurrentPage;
}

int SheetToolsDialog::currentTabIndexForTest() const
{
    return m_tabs->currentIndex();
}

std::optional<SheetLayout::BatchGrid> SheetToolsDialog::batchGridForTest() const
{
    return m_batchGrid;
}

std::optional<SheetLayout::AlignPlan> SheetToolsDialog::alignPlanForTest() const
{
    return m_alignPlan;
}

std::optional<SheetLayout::DistributePlan> SheetToolsDialog::distributePlanForTest() const
{
    return m_distributePlan;
}

QString SheetToolsDialog::summaryTextForTest() const
{
    return m_summary->text();
}

QString SheetToolsDialog::captionTextForTest() const
{
    return m_caption->text();
}

QString SheetToolsDialog::warningTextForTest() const
{
    return m_warning->text();
}

bool SheetToolsDialog::createEnabledForTest() const
{
    return m_createButton->isEnabled();
}

bool SheetToolsDialog::lastSheetNoteVisibleForTest() const
{
    // isHidden(), not isVisible(): a dialog a test drives without show() is not
    // visible, but setVisible(true) has already cleared the hidden flag.
    return !m_note->isHidden();
}

} // namespace pp
