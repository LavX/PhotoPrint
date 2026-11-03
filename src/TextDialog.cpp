#include "TextDialog.h"

#include "OverlayGeometry.h"
#include "PageRenderer.h"

#include <QApplication>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

/// The resolution the preview glyphs are rasterised at. It is a screen preview,
/// so 150 ppi is already sharper than the label can show; the block geometry is
/// the same 1200 ppi reference the page uses.
constexpr double kPreviewPpi = 150.0;

QIcon colourSwatch(QRgb colour)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(QColor::fromRgba(colour));
    QPainter painter(&pixmap);
    painter.setPen(QColor(0x60, 0x60, 0x66));
    painter.drawRect(0, 0, 23, 23);
    painter.end();
    return QIcon(pixmap);
}

} // namespace

TextDialog::TextDialog(const TextItem &item, QWidget *parent)
    : QDialog(parent), m_item(item)
{
    setObjectName(QStringLiteral("textDialog"));
    setWindowTitle(QCoreApplication::translate("PhotoPrint", "Szöveg a lapra"));
    setModal(true);
    // The height follows the content, so a 1.35x system font grows the dialog
    // instead of squeezing the preview.
    setMinimumWidth(460);

    auto *layout = new QVBoxLayout(this);

    auto *textLabel = new QLabel(QCoreApplication::translate("PhotoPrint", "A szöveg"), this);
    textLabel->setWordWrap(true);
    layout->addWidget(textLabel);

    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName(QStringLiteral("textDialogEditor"));
    m_editor->setAccessibleName(textLabel->text());
    m_editor->setPlainText(item.text);
    m_editor->setMinimumHeight(96);
    // Tab moves between the dialog's controls instead of inserting a tab: the
    // editor is a caption field, not a code editor.
    m_editor->setTabChangesFocus(true);
    layout->addWidget(m_editor);

    auto *textHint = new QLabel(
        QCoreApplication::translate("PhotoPrint",
                                    "Írd be a szöveget. A sortörést az Enter adja."),
        this);
    textHint->setWordWrap(true);
    textHint->setObjectName(QStringLiteral("textDialogHint"));
    QPalette hintPalette = textHint->palette();
    hintPalette.setColor(QPalette::WindowText, hintPalette.color(QPalette::Disabled, QPalette::WindowText));
    textHint->setPalette(hintPalette);
    layout->addWidget(textHint);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_fontCombo = new QFontComboBox(this);
    m_fontCombo->setObjectName(QStringLiteral("textDialogFontCombo"));
    m_fontCombo->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Betűtípus"));
    // An empty family is the application font. The combo shows it as the app
    // font's own family, so the panel never reads as "no font chosen".
    const QString family = item.fontFamily.isEmpty() ? QApplication::font().family()
                                                     : item.fontFamily;
    m_fontCombo->setCurrentFont(QFont(family));
    form->addRow(QCoreApplication::translate("PhotoPrint", "Betűtípus"), m_fontCombo);

    m_sizeSpin = new QDoubleSpinBox(this);
    m_sizeSpin->setObjectName(QStringLiteral("textDialogSizeSpin"));
    m_sizeSpin->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Betűméret"));
    m_sizeSpin->setDecimals(1);
    m_sizeSpin->setRange(kMinFontPt, kMaxFontPt);
    m_sizeSpin->setSingleStep(1.0);
    m_sizeSpin->setSuffix(QStringLiteral(" pt"));
    m_sizeSpin->setValue(std::clamp(item.fontPt, kMinFontPt, kMaxFontPt));
    form->addRow(QCoreApplication::translate("PhotoPrint", "Betűméret"), m_sizeSpin);

    m_spacingSpin = new QDoubleSpinBox(this);
    m_spacingSpin->setObjectName(QStringLiteral("textDialogSpacingSpin"));
    m_spacingSpin->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Sormagasság"));
    m_spacingSpin->setDecimals(0);
    m_spacingSpin->setRange(kMinLineSpacing * 100.0, kMaxLineSpacing * 100.0);
    m_spacingSpin->setSingleStep(5.0);
    m_spacingSpin->setSuffix(QStringLiteral(" %"));
    m_spacingSpin->setValue(std::clamp(item.lineSpacing, kMinLineSpacing, kMaxLineSpacing)
                            * 100.0);
    form->addRow(QCoreApplication::translate("PhotoPrint", "Sormagasság"), m_spacingSpin);

    layout->addLayout(form);

    // The style toggles and the three alignments share one row: they are the
    // same kind of choice and the dialog must stay short at a large font.
    auto *styleRow = new QHBoxLayout;
    const auto makeToggle = [this](const QString &text, const QString &name,
                                   const QString &accessible) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setText(text);
        button->setCheckable(true);
        button->setAccessibleName(accessible);
        button->setToolTip(accessible);
        button->setMinimumSize(32, 28);
        return button;
    };
    m_boldButton = makeToggle(QStringLiteral("B"), QStringLiteral("textDialogBoldButton"),
                              QCoreApplication::translate("PhotoPrint", "Félkövér"));
    m_italicButton = makeToggle(QStringLiteral("I"), QStringLiteral("textDialogItalicButton"),
                                QCoreApplication::translate("PhotoPrint", "Dőlt"));
    m_underlineButton = makeToggle(QStringLiteral("U"), QStringLiteral("textDialogUnderlineButton"),
                                   QCoreApplication::translate("PhotoPrint", "Aláhúzott"));
    m_boldButton->setChecked(item.bold);
    m_italicButton->setChecked(item.italic);
    m_underlineButton->setChecked(item.underline);
    styleRow->addWidget(m_boldButton);
    styleRow->addWidget(m_italicButton);
    styleRow->addWidget(m_underlineButton);
    styleRow->addSpacing(12);

    m_alignLeftButton = makeToggle(QStringLiteral("‹"), QStringLiteral("textDialogAlignLeftButton"),
                                   QCoreApplication::translate("PhotoPrint", "Balra igazítás"));
    m_alignCenterButton = makeToggle(QStringLiteral("≡"), QStringLiteral("textDialogAlignCenterButton"),
                                     QCoreApplication::translate("PhotoPrint", "Középre igazítás"));
    m_alignRightButton = makeToggle(QStringLiteral("›"), QStringLiteral("textDialogAlignRightButton"),
                                    QCoreApplication::translate("PhotoPrint", "Jobbra igazítás"));
    styleRow->addWidget(m_alignLeftButton);
    styleRow->addWidget(m_alignCenterButton);
    styleRow->addWidget(m_alignRightButton);
    styleRow->addStretch(1);

    m_colorButton = new QPushButton(this);
    m_colorButton->setObjectName(QStringLiteral("textDialogColorButton"));
    m_colorButton->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Szín"));
    m_colorButton->setToolTip(QCoreApplication::translate("PhotoPrint", "Válassz színt a szövegnek."));
    m_colorButton->setIcon(colourSwatch(item.color));
    m_colorButton->setFixedSize(40, 30);
    styleRow->addWidget(m_colorButton);
    layout->addLayout(styleRow);

    auto *previewLabel = new QLabel(QCoreApplication::translate("PhotoPrint", "Előnézet"), this);
    previewLabel->setWordWrap(true);
    layout->addWidget(previewLabel);
    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("textDialogPreview"));
    m_preview->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Előnézet"));
    m_preview->setFixedSize(400, 120);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFrameShape(QFrame::StyledPanel);
    layout->addWidget(m_preview);

    m_fontHint = new QLabel(this);
    m_fontHint->setObjectName(QStringLiteral("textDialogFontHint"));
    m_fontHint->setWordWrap(true);
    m_fontHint->setText(QCoreApplication::translate(
        "PhotoPrint",
        "Ez a betűtípus ezen a gépen nincs meg, helyette a rendszer betűtípusa látszik."));
    layout->addWidget(m_fontHint);

    m_emptyHint = new QLabel(this);
    m_emptyHint->setObjectName(QStringLiteral("textDialogEmptyHint"));
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setText(QCoreApplication::translate(
        "PhotoPrint", "Írj be legalább egy látható karaktert."));
    layout->addWidget(m_emptyHint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QCoreApplication::translate("PhotoPrint", "Kész"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("PhotoPrint", "Mégsem"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &TextDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &TextDialog::reject);

    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        m_emptyHint->setVisible(false);
        updatePreview();
    });
    connect(m_fontCombo, &QFontComboBox::currentFontChanged, this, [this] {
        updateFontHint();
        updatePreview();
    });
    connect(m_sizeSpin, &QDoubleSpinBox::valueChanged, this, [this] { updatePreview(); });
    connect(m_spacingSpin, &QDoubleSpinBox::valueChanged, this, [this] { updatePreview(); });
    connect(m_boldButton, &QToolButton::toggled, this, [this] { updatePreview(); });
    connect(m_italicButton, &QToolButton::toggled, this, [this] { updatePreview(); });
    connect(m_underlineButton, &QToolButton::toggled, this, [this] { updatePreview(); });
    connect(m_alignLeftButton, &QToolButton::clicked, this, [this] {
        m_item.align = TextAlign::Left;
        syncStyleButtons();
        updatePreview();
    });
    connect(m_alignCenterButton, &QToolButton::clicked, this, [this] {
        m_item.align = TextAlign::Center;
        syncStyleButtons();
        updatePreview();
    });
    connect(m_alignRightButton, &QToolButton::clicked, this, [this] {
        m_item.align = TextAlign::Right;
        syncStyleButtons();
        updatePreview();
    });
    connect(m_colorButton, &QPushButton::clicked, this, &TextDialog::chooseColor);

    syncStyleButtons();
    updateFontHint();
    m_emptyHint->setVisible(false);
    updatePreview();
}

TextItem TextDialog::item() const
{
    TextItem result = m_item;
    result.text = m_editor->toPlainText();
    result.text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    result.text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    // An empty family is the application font. The combo always shows a real
    // family, so the app font's own name is stored as empty again: a project
    // written on one machine must not pin a family the next may lack.
    const QString family = m_fontCombo->currentFont().family();
    result.fontFamily = family == QApplication::font().family() ? QString() : family;
    result.fontPt = m_sizeSpin->value();
    result.bold = m_boldButton->isChecked();
    result.italic = m_italicButton->isChecked();
    result.underline = m_underlineButton->isChecked();
    result.lineSpacing = m_spacingSpin->value() / 100.0;
    return result;
}

bool TextDialog::textIsEmpty() const
{
    return m_editor->toPlainText().trimmed().isEmpty();
}

void TextDialog::accept()
{
    if (textIsEmpty()) {
        // The dialog stays open: an empty text is not a layer, and closing here
        // would silently add nothing.
        m_emptyHint->setVisible(true);
        m_editor->setFocus();
        return;
    }
    QDialog::accept();
}

void TextDialog::syncStyleButtons()
{
    m_alignLeftButton->setChecked(m_item.align == TextAlign::Left);
    m_alignCenterButton->setChecked(m_item.align == TextAlign::Center);
    m_alignRightButton->setChecked(m_item.align == TextAlign::Right);
}

void TextDialog::chooseColor()
{
    const QColor picked = QColorDialog::getColor(QColor::fromRgba(m_item.color), this,
                                                 QCoreApplication::translate("PhotoPrint", "Szín"));
    if (!picked.isValid())
        return;
    m_item.color = 0xff000000u | (picked.rgb() & 0x00ffffffu);
    m_colorButton->setIcon(colourSwatch(m_item.color));
    updatePreview();
}

void TextDialog::updateFontHint()
{
    if (!m_fontHint)
        return;
    const QString family = m_fontCombo->currentFont().family();
    const bool installed = family.isEmpty()
        || QFontDatabase::families().contains(family, Qt::CaseInsensitive);
    m_fontHint->setVisible(!installed);
}

void TextDialog::updatePreview()
{
    if (!m_preview)
        return;
    const QSize size = m_preview->size();
    if (size.isEmpty())
        return;
    QPixmap pixmap(size);
    pixmap.fill(Qt::white);
    QPainter painter(&pixmap);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                           | QPainter::SmoothPixmapTransform);

    const TextItem current = item();
    const QRectF block = OverlayGeometry::textBlockMm(current);
    if (!block.isEmpty()) {
        const double pxPerMm = kPreviewPpi / 25.4;
        const double blockWidth = block.width() * pxPerMm;
        const double blockHeight = block.height() * pxPerMm;
        // A long caption is scaled down to fit the plate instead of being cut
        // off: the preview is about the look, not the exact millimetres.
        const double fit = std::min(
            {1.0, (size.width() - 12.0) / std::max(1.0, blockWidth),
             (size.height() - 12.0) / std::max(1.0, blockHeight)});
        painter.translate(size.width() / 2.0, size.height() / 2.0);
        painter.scale(fit, fit);
        painter.setPen(QColor::fromRgba(current.color));
        paintTextBlock(painter, current, kPreviewPpi);
    }
    painter.end();
    m_preview->setPixmap(pixmap);
}

} // namespace pp
