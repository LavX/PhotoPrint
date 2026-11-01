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
    QString text = QString::number(value, 'f', 1);
    if (text.endsWith(QLatin1String(".0")))
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
    return uiText("Előnézet") + QStringLiteral(" · ") + QString::number(m_rects.size());
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
                                   QHash<QString, QImage> previews, QWidget *parent)
    : QDialog(parent), m_document(std::move(document)), m_pageIndex(pageIndex),
      m_source(std::move(sourceFrame)), m_previews(std::move(previews))
{
    setObjectName(QStringLiteral("sheetToolsDialog"));
    setWindowTitle(mode == Mode::Batch ? uiText("Képek sokszorosítása") : uiText("Fotók rendezése"));
    resize(960, 660);

    auto *outer = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    body->setSpacing(16);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("sheetToolsTabs"));
    auto *previewColumn = new QVBoxLayout;
    auto *previewTitle = new QLabel(uiText("Előnézet"), this);
    QFont titleFont = previewTitle->font();
    titleFont.setWeight(QFont::DemiBold);
    previewTitle->setFont(titleFont);
    previewColumn->addWidget(previewTitle);
    m_preview = new SheetPreview(this);
    m_preview->setAccessibleName(uiText("Előnézet"));
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

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_createButton = buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
    m_createButton->setObjectName(QStringLiteral("sheetCreateButton"));
    m_createButton->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText(uiText("Mégsem"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_createButton, &QPushButton::clicked, this, &SheetToolsDialog::accept);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        setWindowTitle(index == kBatchTab ? uiText("Képek sokszorosítása")
                                          : uiText("Fotók rendezése"));
        setCreateText();
        // The create state belongs to the active tab: switching must re-evaluate
        // that tab's inputs, not leave the other tab's answer in place.
        if (index == kBatchTab)
            updateBatch();
        else
            updateAlign();
    });

    m_tabs->setCurrentIndex(mode == Mode::Batch ? kBatchTab : kAlignTab);
    setCreateText();
    updateBatch();
    updateAlign();
    updatePreview();
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

    auto *intro = new QLabel(uiText("Ugyanaz a fotó több példányban, egyenes rácsban."), page);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *sourceLabel = new QLabel(page);
    sourceLabel->setObjectName(QStringLiteral("batchSourceLabel"));
    sourceLabel->setWordWrap(true);
    if (m_source) {
        const QString name = QFileInfo(m_source->sourcePath).fileName();
        const QFontMetrics metrics(sourceLabel->font());
        sourceLabel->setText(
            uiText("Fotó: %1").arg(metrics.elidedText(name, Qt::ElideMiddle, 360)));
    } else {
        sourceLabel->setText(uiText("Válassz ki egy fotót a sokszorosításhoz."));
    }
    v->addWidget(sourceLabel);

    auto *sizeGroup = new QGroupBox(uiText("Méret"), page);
    auto *sizeForm = new QFormLayout(sizeGroup);
    m_batchPreset = new QComboBox(sizeGroup);
    m_batchPreset->setObjectName(QStringLiteral("batchPreset"));
    m_batchPreset->setAccessibleName(uiText("Méret"));
    for (const SheetLayout::CopyPreset &preset : SheetLayout::copyPresets()) {
        m_batchPreset->addItem(
            QCoreApplication::translate("PhotoPrint", preset.label.toUtf8().constData()),
            preset.id);
    }
    m_batchPreset->addItem(uiText("Egyedi méret"), QString());
    sizeForm->addRow(uiText("Méret"), m_batchPreset);

    m_batchWidthCm = new QDoubleSpinBox(sizeGroup);
    m_batchWidthCm->setObjectName(QStringLiteral("batchWidthCm"));
    m_batchWidthCm->setAccessibleName(uiText("Szélesség"));
    m_batchHeightCm = new QDoubleSpinBox(sizeGroup);
    m_batchHeightCm->setObjectName(QStringLiteral("batchHeightCm"));
    m_batchHeightCm->setAccessibleName(uiText("Magasság"));
    for (QDoubleSpinBox *spin : {m_batchWidthCm, m_batchHeightCm}) {
        spin->setRange(SheetLayout::kMinCopyMm / 10.0, SheetLayout::kMaxCopyMm / 10.0);
        spin->setDecimals(1);
        spin->setSingleStep(0.1);
        spin->setSuffix(QStringLiteral(" cm"));
    }
    sizeForm->addRow(uiText("Szélesség"), m_batchWidthCm);
    sizeForm->addRow(uiText("Magasság"), m_batchHeightCm);
    m_batchSwap = new QPushButton(uiText("Csere"), sizeGroup);
    m_batchSwap->setObjectName(QStringLiteral("batchSwap"));
    m_batchSwap->setAccessibleName(uiText("Szélesség és magasság cseréje"));
    sizeForm->addRow(QString(), m_batchSwap);

    auto *sizeNote = new QLabel(
        uiText("A 35 × 45 mm az igazolványkép szokásos mérete. Ez az eszköz csak a helyét állítja "
           "be, azt nem ellenőrzi, hogy a fotó megfelel-e az előírásoknak."),
        sizeGroup);
    sizeNote->setWordWrap(true);
    sizeNote->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    sizeForm->addRow(sizeNote);
    v->addWidget(sizeGroup);

    auto *layoutGroup = new QGroupBox(uiText("Elrendezés"), page);
    auto *layoutForm = new QFormLayout(layoutGroup);
    m_batchGutter = new QDoubleSpinBox(layoutGroup);
    m_batchGutter->setObjectName(QStringLiteral("batchGutter"));
    m_batchGutter->setAccessibleName(uiText("Térköz"));
    m_batchGutter->setRange(0.0, SheetLayout::kMaxGutterMm);
    m_batchGutter->setDecimals(1);
    m_batchGutter->setSingleStep(0.5);
    m_batchGutter->setSuffix(QStringLiteral(" mm"));
    layoutForm->addRow(uiText("Térköz"), m_batchGutter);

    m_batchMargin = new QDoubleSpinBox(layoutGroup);
    m_batchMargin->setObjectName(QStringLiteral("batchMargin"));
    m_batchMargin->setAccessibleName(uiText("Legkisebb margó"));
    m_batchMargin->setRange(SheetLayout::kMinMarginMm, SheetLayout::kMaxMarginMm);
    m_batchMargin->setDecimals(1);
    m_batchMargin->setSingleStep(0.5);
    m_batchMargin->setSuffix(QStringLiteral(" mm"));
    layoutForm->addRow(uiText("Legkisebb margó"), m_batchMargin);

    auto *marginHint = new QLabel(
        uiText("A nyomtató a lap szélétől 3 mm-t nem nyomtat ki, ezért a margó legalább 3,5 mm."),
        layoutGroup);
    marginHint->setWordWrap(true);
    marginHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    layoutForm->addRow(marginHint);

    auto *countRow = new QWidget(layoutGroup);
    auto *countLayout = new QHBoxLayout(countRow);
    countLayout->setContentsMargins(0, 0, 0, 0);
    m_batchCount = new QSpinBox(countRow);
    m_batchCount->setObjectName(QStringLiteral("batchCount"));
    m_batchCount->setAccessibleName(uiText("Darabszám"));
    m_batchCount->setRange(1, SheetLayout::maxCount());
    m_batchCount->setSuffix(QStringLiteral(" db"));
    m_batchFill = new QCheckBox(uiText("Amennyi befér"), countRow);
    m_batchFill->setObjectName(QStringLiteral("batchFill"));
    countLayout->addWidget(m_batchCount);
    countLayout->addWidget(m_batchFill);
    countLayout->addStretch(1);
    layoutForm->addRow(uiText("Darabszám"), countRow);
    v->addWidget(layoutGroup);

    auto *guideGroup = new QGroupBox(uiText("Vágójelek"), page);
    auto *guideForm = new QFormLayout(guideGroup);
    m_batchGuides = new QComboBox(guideGroup);
    m_batchGuides->setObjectName(QStringLiteral("batchGuides"));
    m_batchGuides->setAccessibleName(uiText("Vágójelek"));
    m_batchGuides->addItem(uiText("Nincs"), int(CutGuides::None));
    m_batchGuides->addItem(uiText("Képernyőn"), int(CutGuides::Screen));
    m_batchGuides->addItem(uiText("Nyomtatás"), int(CutGuides::Print));
    guideForm->addRow(uiText("Vágójelek"), m_batchGuides);
    auto *guideHint = new QLabel(
        uiText("A nyomtatott jel vékony szürke vonal a példányok szélén. Alapból nincs a "
           "nyomaton."),
        guideGroup);
    guideHint->setWordWrap(true);
    guideHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    guideForm->addRow(guideHint);
    v->addWidget(guideGroup);

    auto *placeGroup = new QGroupBox(uiText("Hely"), page);
    auto *placeLayout = new QVBoxLayout(placeGroup);
    m_batchCurrent = new QRadioButton(uiText("Az aktuális lapra"), placeGroup);
    m_batchCurrent->setObjectName(QStringLiteral("batchPlaceCurrent"));
    m_batchNew = new QRadioButton(uiText("Új lapokra"), placeGroup);
    m_batchNew->setObjectName(QStringLiteral("batchPlaceNew"));
    m_batchPlaceNote = new QLabel(placeGroup);
    m_batchPlaceNote->setObjectName(QStringLiteral("batchPlaceNote"));
    m_batchPlaceNote->setWordWrap(true);
    m_batchPlaceNote->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    placeLayout->addWidget(m_batchCurrent);
    placeLayout->addWidget(m_batchPlaceNote);
    placeLayout->addWidget(m_batchNew);
    v->addWidget(placeGroup);

    m_batchLastSheet = new QLabel(
        uiText("A maradék az első kockáktól töltődik, hogy a lapok együtt vághatók legyenek."), page);
    m_batchLastSheet->setObjectName(QStringLiteral("batchLastSheet"));
    m_batchLastSheet->setWordWrap(true);
    m_batchLastSheet->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    v->addWidget(m_batchLastSheet);

    m_batchSummary = new QLabel(page);
    m_batchSummary->setObjectName(QStringLiteral("batchSummary"));
    m_batchSummary->setWordWrap(true);
    m_batchSummary->setAccessibleName(uiText("Összegzés"));
    v->addWidget(m_batchSummary);

    m_batchWarning = new QLabel(page);
    m_batchWarning->setObjectName(QStringLiteral("batchWarning"));
    m_batchWarning->setWordWrap(true);
    m_batchWarning->setStyleSheet(QStringLiteral("color: #8a5a00;"));
    v->addWidget(m_batchWarning);
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

    m_tabs->addTab(scroll, uiText("Képek sokszorosítása"));
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

    auto *intro = new QLabel(uiText("A lapon lévő fotók elrendezése. A sorrend nem változik."), page);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *modeRow = new QHBoxLayout;
    m_alignGridMode = new QRadioButton(uiText("Rácsba"), page);
    m_alignGridMode->setObjectName(QStringLiteral("alignGridMode"));
    m_alignGapMode = new QRadioButton(uiText("Térköz kiegyenlítése"), page);
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
        uiText("A fotók egyforma cellákba kerülnek. A kép kitölti a cellát, a kilógó rész "
           "lemarad."),
        gridPage);
    gridHint->setWordWrap(true);
    gridHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    gridLayout->addWidget(gridHint);

    auto *gridForm = new QFormLayout;
    m_alignMargin = new QDoubleSpinBox(gridPage);
    m_alignMargin->setObjectName(QStringLiteral("alignMargin"));
    m_alignMargin->setAccessibleName(uiText("Külső margó"));
    m_alignMargin->setRange(SheetLayout::kMinMarginMm, SheetLayout::kMaxMarginMm);
    m_alignMargin->setDecimals(1);
    m_alignMargin->setSingleStep(0.5);
    m_alignMargin->setSuffix(QStringLiteral(" mm"));
    gridForm->addRow(uiText("Külső margó"), m_alignMargin);

    m_alignGutter = new QDoubleSpinBox(gridPage);
    m_alignGutter->setObjectName(QStringLiteral("alignGutter"));
    m_alignGutter->setAccessibleName(uiText("Térköz"));
    m_alignGutter->setRange(0.0, SheetLayout::kMaxGutterMm);
    m_alignGutter->setDecimals(1);
    m_alignGutter->setSingleStep(0.5);
    m_alignGutter->setSuffix(QStringLiteral(" mm"));
    gridForm->addRow(uiText("Térköz"), m_alignGutter);

    m_alignColumns = new QComboBox(gridPage);
    m_alignColumns->setObjectName(QStringLiteral("alignColumns"));
    m_alignColumns->setAccessibleName(uiText("Oszlopok"));
    gridForm->addRow(uiText("Oszlopok"), m_alignColumns);
    gridLayout->addLayout(gridForm);

    m_alignFill = new QCheckBox(uiText("A kép egésze látszódjon"), gridPage);
    m_alignFill->setObjectName(QStringLiteral("alignFill"));
    gridLayout->addWidget(m_alignFill);
    auto *fillHint = new QLabel(
        uiText("Bekapcsolva a kép nem tölti ki a cellát; a hiányzó rész fehér marad."), gridPage);
    fillHint->setWordWrap(true);
    fillHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    gridLayout->addWidget(fillHint);
    gridLayout->addStretch(1);
    m_alignStack->addWidget(gridPage);

    auto *gapPage = new QWidget(m_alignStack);
    auto *gapLayout = new QVBoxLayout(gapPage);
    gapLayout->setContentsMargins(0, 0, 0, 0);
    auto *gapHint = new QLabel(
        uiText("A fotók megtartják a méretüket. A köztük lévő hely lesz egyenlő."), gapPage);
    gapHint->setWordWrap(true);
    gapHint->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    gapLayout->addWidget(gapHint);

    auto *gapForm = new QFormLayout;
    m_alignAxis = new QComboBox(gapPage);
    m_alignAxis->setObjectName(QStringLiteral("alignAxis"));
    m_alignAxis->setAccessibleName(uiText("Irány"));
    m_alignAxis->addItem(uiText("Egymás mellett"), int(SheetLayout::DistributeAxis::Horizontal));
    m_alignAxis->addItem(uiText("Egymás alatt"), int(SheetLayout::DistributeAxis::Vertical));
    gapForm->addRow(uiText("Irány"), m_alignAxis);
    m_alignGap = new QDoubleSpinBox(gapPage);
    m_alignGap->setObjectName(QStringLiteral("alignGap"));
    m_alignGap->setAccessibleName(uiText("Térköz"));
    m_alignGap->setRange(0.0, SheetLayout::kMaxGutterMm);
    m_alignGap->setDecimals(1);
    m_alignGap->setSingleStep(0.5);
    m_alignGap->setSuffix(QStringLiteral(" mm"));
    gapForm->addRow(uiText("Térköz"), m_alignGap);
    gapLayout->addLayout(gapForm);
    auto *gapNote = new QLabel(uiText("Ha nem fér el, a program csökkenti."), gapPage);
    gapNote->setWordWrap(true);
    gapNote->setStyleSheet(QStringLiteral("color: #6e6e73;"));
    gapLayout->addWidget(gapNote);
    gapLayout->addStretch(1);
    m_alignStack->addWidget(gapPage);

    m_alignSummary = new QLabel(page);
    m_alignSummary->setObjectName(QStringLiteral("alignSummary"));
    m_alignSummary->setWordWrap(true);
    v->addWidget(m_alignSummary);
    m_alignWarning = new QLabel(page);
    m_alignWarning->setObjectName(QStringLiteral("alignWarning"));
    m_alignWarning->setWordWrap(true);
    m_alignWarning->setStyleSheet(QStringLiteral("color: #8a5a00;"));
    v->addWidget(m_alignWarning);
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

    m_tabs->addTab(scroll, uiText("Fotók rendezése"));
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

void SheetToolsDialog::updateBatch()
{
    m_batchGrid.reset();
    const bool onTab = m_tabs->currentIndex() == kBatchTab;

    if (m_pageIndex < 0 || m_pageIndex >= m_document.pages.size() || !m_source) {
        m_batchSummary->setText(m_source ? uiText("Érvénytelen papírméret.")
                                         : uiText("Válassz ki egy fotót a sokszorosításhoz."));
        m_batchWarning->clear();
        m_batchLastSheet->setVisible(false);
        m_batchPlaceNote->clear();
        if (onTab)
            m_createButton->setEnabled(false);
        updatePreview();
        return;
    }

    const Page &page = m_document.pages.at(m_pageIndex);
    const SheetLayout::BatchSpec spec = batchSpec();
    const auto grid = SheetLayout::batchGrid(page, spec);
    if (!grid) {
        m_batchSummary->setText(grid.error().message);
        m_batchWarning->clear();
        m_batchLastSheet->setVisible(false);
        if (onTab)
            m_createButton->setEnabled(false);
        updatePreview();
        return;
    }

    m_batchGrid = grid.value();
    const SheetLayout::BatchGrid &chosen = *m_batchGrid;
    if (m_batchFill->isChecked() && m_batchCount->value() != chosen.perPage) {
        const QSignalBlocker blocker(m_batchCount);
        m_batchCount->setValue(chosen.perPage);
    }

    QString summary = uiText("%n példány fér el egy lapra.", chosen.perPage);
    if (chosen.pageCount() > 1)
        summary += QLatin1Char(' ') + uiText("%n lapra kerül.", chosen.pageCount());
    if (!spec.fillPage && spec.count >= SheetLayout::maxCount()) {
        // The spin cannot go past the cap, so say why the number stopped there
        // instead of letting the maximum look like an arbitrary limit.
        summary += QLatin1Char(' ')
            + uiText("Egyszerre legfeljebb %1 példány készíthető.")
                  .arg(SheetLayout::maxCount());
    }
    m_batchSummary->setText(summary);

    QStringList warnings;
    if (m_source && std::abs(m_source->angleDeg) > 1e-9) {
        warnings << uiText("A kiválasztott fotó el van forgatva. A példányok egyenesen kerülnek a "
                       "rácsba.");
    }
    if (chosen.minVisible < 0.6)
        warnings << uiText("A kiválasztott fotó szélei lemaradnak.");
    if (m_batchGuides->currentData().toInt() == int(CutGuides::Print) && chosen.marginMm < 5.0) {
        warnings << uiText("Ilyen szoros margónál a nyomtató a szélső vágójelet levághatja.");
    }
    if (!m_batchNew->isChecked() && page.hasLayout()) {
        warnings << uiText("A lap elrendezése megszűnik, a példányok szabadon kerülnek a lapra.");
    }
    m_batchWarning->setText(warnings.join(QLatin1Char('\n')));
    m_batchLastSheet->setVisible(chosen.pageCount() > 1);

    if (m_batchNew->isChecked())
        m_batchPlaceNote->clear();
    else if (!page.frames.isEmpty())
        m_batchPlaceNote->setText(uiText("%n fotó helyére", page.frames.size()));
    else
        m_batchPlaceNote->clear();

    if (onTab)
        m_createButton->setEnabled(true);
    updatePreview();
}

void SheetToolsDialog::updateAlign()
{
    m_alignPlan.reset();
    m_distributePlan.reset();
    m_alignFrames.clear();
    m_alignValid = false;
    const bool onTab = m_tabs->currentIndex() == kAlignTab;

    if (m_pageIndex < 0 || m_pageIndex >= m_document.pages.size()) {
        m_alignSummary->setText(uiText("Ezen a lapon nincs fotó."));
        m_alignWarning->clear();
        if (onTab)
            m_createButton->setEnabled(false);
        updatePreview();
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
        m_alignColumns->addItem(uiText("Automatikus"), 0);
        for (int columns = 1; columns <= count; ++columns)
            m_alignColumns->addItem(QString::number(columns), columns);
        const int index = m_alignColumns->findData(previous);
        m_alignColumns->setCurrentIndex(index >= 0 ? index : 0);
    }

    if (page.hasLayout()) {
        m_alignSummary->setText(
            uiText("Ehhez a laphoz elrendezés tartozik. Szüntesd meg az elrendezést az "
               "igazításhoz."));
        m_alignWarning->clear();
        if (onTab)
            m_createButton->setEnabled(false);
        updatePreview();
        return;
    }
    if (count == 0) {
        m_alignSummary->setText(uiText("Ezen a lapon nincs fotó."));
        m_alignWarning->clear();
        if (onTab)
            m_createButton->setEnabled(false);
        updatePreview();
        return;
    }

    const QList<QSize> pixels = sourcePixels();
    QList<Frame> previewFrames;

    if (m_alignGridMode->isChecked()) {
        const SheetLayout::AlignSpec spec = alignSpec();
        const auto plan = SheetLayout::alignGrid(page, spec, pixels);
        if (!plan) {
            m_alignSummary->setText(plan.error().message);
            m_alignWarning->clear();
            if (onTab)
                m_createButton->setEnabled(false);
            updatePreview();
            return;
        }
        m_alignPlan = plan.value();
        const auto frames = SheetLayout::alignFrames(page, spec, pixels);
        if (!frames) {
            m_alignSummary->setText(frames.error().message);
            m_alignWarning->clear();
            if (onTab)
                m_createButton->setEnabled(false);
            updatePreview();
            return;
        }
        m_alignFrames = frames.value();
        m_alignValid = true;
        if (count == 1) {
            m_alignSummary->setText(
                uiText("Egy fotó van a lapon. Csak középre igazítjuk, a mérete nem változik."));
            m_alignWarning->clear();
        } else {
            m_alignSummary->setText(
                uiText("%n fotó: %1 oszlop × %2 sor. Egy cella %3 × %4 mm.", count)
                    .arg(m_alignPlan->columns)
                    .arg(m_alignPlan->rows)
                    .arg(mmText(m_alignPlan->cellWidthMm), mmText(m_alignPlan->cellHeightMm)));
            const bool strongCrop = spec.fill && m_alignPlan->minVisible < 0.5;
            m_alignWarning->setText(
                strongCrop ? uiText("A fotók egy része erősen lemarad. Kapcsold be a »A kép egésze "
                                "látszódjon« opciót, vagy oszd el őket több lapon.")
                           : QString());
        }
    } else {
        const SheetLayout::DistributeSpec spec = distributeSpec();
        const auto plan = SheetLayout::distribute(page, spec);
        if (!plan) {
            m_alignSummary->setText(plan.error().message);
            m_alignWarning->clear();
            if (onTab)
                m_createButton->setEnabled(false);
            updatePreview();
            return;
        }
        m_distributePlan = plan.value();
        const auto frames = SheetLayout::distributeFrames(page, spec);
        if (!frames) {
            m_alignSummary->setText(frames.error().message);
            m_alignWarning->clear();
            if (onTab)
                m_createButton->setEnabled(false);
            updatePreview();
            return;
        }
        m_alignFrames = frames.value();
        m_alignValid = true;
        if (count == 1) {
            m_alignSummary->setText(
                uiText("Egy fotó van a lapon. Csak középre igazítjuk, a mérete nem változik."));
            m_alignWarning->clear();
        } else {
            m_alignSummary->setText(
                uiText("%n fotó, %1 mm térköz. A margó %2 mm.", count)
                    .arg(mmText(m_distributePlan->gapMm),
                         mmText(m_distributePlan->startMm)));
            m_alignWarning->setText(m_distributePlan->gapMm < m_distributePlan->requestedGapMm
                                        ? uiText("A térköz %1 mm lett, hogy a fotók elférjenek.")
                                              .arg(mmText(m_distributePlan->gapMm))
                                        : QString());
        }
    }

    if (onTab)
        m_createButton->setEnabled(true);
    updatePreview();
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
        QList<QImage> images;
        const QImage source = m_source ? m_previews.value(m_source->id) : QImage();
        for (int i = 0; i < grid.pagesMm.first().size(); ++i)
            images.append(source);
        m_preview->setSheetMm(grid.sheet);
        m_preview->setRects(grid.pagesMm.first(), images);
        m_preview->setGuideState(static_cast<CutGuides>(m_batchGuides->currentData().toInt()));
        m_caption->setText(uiText("Így kerül a lapra.") + QLatin1Char('\n')
                           + uiText("Szélek: %1 mm oldalt, %2 mm felül és alul.")
                                 .arg(mmText(grid.leftMarginMm), mmText(grid.topMarginMm)));
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
    m_caption->setText(uiText("Így kerül a lapra."));
}

void SheetToolsDialog::setCreateText()
{
    if (!m_createButton)
        return;
    m_createButton->setText(m_tabs->currentIndex() == kBatchTab ? uiText("Létrehozás")
                                                               : uiText("Rendezés"));
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
    edit.actionText = uiText("Képek sokszorosítása");
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
    edit.actionText = uiText("Fotók rendezése");
    edit.selectPage = m_pageIndex;
    *out = edit;
    return true;
}

void SheetToolsDialog::accept()
{
    ApplyLayoutEdit built;
    const bool ok = m_tabs->currentIndex() == kBatchTab ? buildBatchEdit(&built)
                                                        : buildAlignEdit(&built);
    if (!ok)
        return;
    m_edit = built;
    m_hasEdit = true;
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
    return m_hasEdit && !m_edit.pages.isEmpty();
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
    return m_tabs->currentIndex() == kBatchTab ? m_batchSummary->text() : m_alignSummary->text();
}

QString SheetToolsDialog::captionTextForTest() const
{
    return m_caption->text();
}

QString SheetToolsDialog::warningTextForTest() const
{
    return m_tabs->currentIndex() == kBatchTab ? m_batchWarning->text() : m_alignWarning->text();
}

bool SheetToolsDialog::createEnabledForTest() const
{
    return m_createButton->isEnabled();
}

bool SheetToolsDialog::lastSheetNoteVisibleForTest() const
{
    // isHidden(), not isVisible(): a dialog a test drives without show() is not
    // visible, but setVisible(true) has already cleared the hidden flag.
    return !m_batchLastSheet->isHidden();
}

} // namespace pp
