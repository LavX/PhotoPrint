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

    // The mark's own 48x48 geometry, scaled to the requested size.
    const double unit = size / 48.0;
    painter.setPen(Qt::NoPen);
    QLinearGradient tile(QPointF(2 * unit, 2 * unit), QPointF(46 * unit, 46 * unit));
    tile.setColorAt(0, QColor(0x0a, 0x84, 0xff));
    tile.setColorAt(1, QColor(0x00, 0x40, 0xb3));
    painter.setBrush(tile);
    painter.drawRoundedRect(QRectF(2 * unit, 2 * unit, 44 * unit, 44 * unit),
                            11 * unit, 11 * unit);
    painter.setBrush(Qt::white);
    // The six stripes, in the untransformed coordinates the SVG lists them
    // in; the mark places them with translate(7.75, 11.8) scale(0.78).
    static const double kStripes[][8] = {
        {12, 0, 30, 0, 33.2, 3.2, 15.2, 3.2},
        {14.6, 5.6, 32.6, 5.6, 35.8, 8.8, 17.8, 8.8},
        {17.2, 11.2, 35.2, 11.2, 38.4, 14.4, 20.4, 14.4},
        {3.2, 16.8, 21.2, 16.8, 24.4, 20, 6.4, 20},
        {5.8, 22.4, 23.8, 22.4, 27, 25.6, 9, 25.6},
        {8.4, 28, 26.4, 28, 29.6, 31.2, 11.6, 31.2},
    };
    for (const auto &stripe : kStripes) {
        QPolygonF polygon;
        for (int i = 0; i < 4; ++i) {
            polygon.append(QPointF(stripe[i * 2] * 0.78 * unit + 7.75 * unit,
                                   stripe[i * 2 + 1] * 0.78 * unit + 11.8 * unit));
        }
        painter.drawPolygon(polygon);
    }
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
    return QStringLiteral(PHOTOPRINT_VERSION);
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
    root->addWidget(buttons);

    m_closeButton->setFocus();
}

} // namespace pp
