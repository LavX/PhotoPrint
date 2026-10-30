#include "AboutDialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace pp {

namespace {

constexpr int kIconSize = 72;

/// Drawn only when the SVG image plugin is missing. The shape mirrors
/// resources/photoprint.svg so the dialog is never iconless.
QPixmap drawnIcon(int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x2c, 0x6c, 0xd6));
    painter.drawRoundedRect(QRectF(0, 0, size, size), size * 0.16, size * 0.16);
    painter.setBrush(Qt::white);
    const double inset = size * 0.19;
    painter.drawRoundedRect(QRectF(inset, inset, size - 2 * inset, size - 2 * inset), 2, 2);
    painter.setBrush(QColor(0x8f, 0xb7, 0xf0));
    painter.drawRect(
        QRectF(inset * 1.25, inset * 1.25, size - 3 * inset, (size - 3 * inset) * 0.62));
    painter.setBrush(QColor(0xf2, 0xc1, 0x4e));
    painter.drawEllipse(QPointF(size * 0.38, size * 0.41), size * 0.06, size * 0.06);
    return pixmap;
}

/// Muted text that stays readable on both the light and the dark theme.
void setSecondary(QLabel *label)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);
}

QLabel *addLabel(QVBoxLayout *layout, QWidget *parent, const QString &objectName,
                 const QString &text, bool bold = false)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setAccessibleName(text);
    label->setWordWrap(true);
    if (bold) {
        QFont font = label->font();
        font.setWeight(QFont::DemiBold);
        label->setFont(font);
    }
    layout->addWidget(label);
    return label;
}

} // namespace

QString AboutDialog::versionString()
{
#ifdef PHOTOPRINT_VERSION
    return QStringLiteral(PHOTOPRINT_VERSION);
#else
    return QStringLiteral("0.0.0");
#endif
}

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("aboutDialog"));
    setWindowTitle(QCoreApplication::translate("PhotoPrint", "Névjegy"));
    setAccessibleName(QCoreApplication::translate("PhotoPrint", "Névjegy"));
    setModal(true);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(440);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 18);
    root->setSpacing(10);

    auto *header = new QHBoxLayout;
    header->setSpacing(16);

    auto *icon = new QLabel(this);
    icon->setObjectName(QStringLiteral("aboutIcon"));
    icon->setAccessibleName(QStringLiteral("PhotoPrint"));
    icon->setFixedSize(kIconSize, kIconSize);
    QPixmap pixmap = QIcon(QStringLiteral(":/icons/photoprint.svg"))
                         .pixmap(QSize(kIconSize, kIconSize), devicePixelRatioF());
    if (pixmap.isNull())
        pixmap = drawnIcon(kIconSize);
    icon->setPixmap(pixmap);
    header->addWidget(icon, 0, Qt::AlignTop);

    auto *names = new QVBoxLayout;
    names->setSpacing(2);

    auto *title = new QLabel(QStringLiteral("PhotoPrint"), this);
    title->setObjectName(QStringLiteral("aboutTitle"));
    title->setAccessibleName(QStringLiteral("PhotoPrint"));
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.6);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    names->addWidget(title);

    auto *version = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Verzió %1").arg(versionString()), this);
    version->setObjectName(QStringLiteral("aboutVersion"));
    version->setAccessibleName(version->text());
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    names->addWidget(version);

    names->addSpacing(4);
    auto *description = new QLabel(
        QCoreApplication::translate("PhotoPrint", "Fotók rendezése, vágása és nyomtatása papírra."),
        this);
    description->setObjectName(QStringLiteral("aboutDescription"));
    description->setAccessibleName(description->text());
    description->setWordWrap(true);
    names->addWidget(description);
    header->addLayout(names, 1);
    root->addLayout(header);

    auto *rule = new QFrame(this);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    root->addWidget(rule);

    auto *copyright = new QLabel(QStringLiteral("© 2026 Laszlo Toth"), this);
    copyright->setObjectName(QStringLiteral("aboutCopyright"));
    copyright->setAccessibleName(copyright->text());
    root->addWidget(copyright);

    addLabel(root, this, QStringLiteral("aboutLicenseTitle"), QStringLiteral("MIT License"),
             true);
    setSecondary(addLabel(
        root, this, QStringLiteral("aboutLicense"),
        QCoreApplication::translate("PhotoPrint",
                                    "A program MIT licenc alatt érhető el. A licenc teljes szövege "
                                    "a LICENSE fájlban található.")));

    root->addSpacing(4);
    addLabel(root, this, QStringLiteral("aboutCreditsTitle"),
             QCoreApplication::translate("PhotoPrint", "Készítők"), true);
    setSecondary(addLabel(root, this, QStringLiteral("aboutCredits"),
                          QStringLiteral("Qt 6 · LittleCMS2 · ImageMagick · CUPS")));

    root->addSpacing(8);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_closeButton = buttons->button(QDialogButtonBox::Close);
    m_closeButton->setObjectName(QStringLiteral("aboutCloseButton"));
    m_closeButton->setText(QCoreApplication::translate("PhotoPrint", "Bezárás"));
    m_closeButton->setAccessibleName(QCoreApplication::translate("PhotoPrint", "Bezárás"));
    m_closeButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
    root->addWidget(buttons);

    m_closeButton->setFocus();
}

} // namespace pp
