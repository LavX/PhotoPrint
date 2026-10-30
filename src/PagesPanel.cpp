#include "PagesPanel.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>

namespace pp {

PagesPanel::PagesPanel(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 6);
    layout->setSpacing(6);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("pageList"));
    m_list->setFlow(QListView::LeftToRight);
    m_list->setWrapping(false);
    m_list->setFixedHeight(40);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    layout->addWidget(m_list, 1);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_syncing && row >= 0 && onSelect)
            onSelect(row);
    });

    auto makeButton = [this, layout](const QString &text) {
        auto *button = new QPushButton(text, this);
        button->setMinimumHeight(28);
        layout->addWidget(button);
        return button;
    };
    m_add = makeButton(QCoreApplication::translate("PhotoPrint", "Új lap"));
    m_add->setToolTip(QCoreApplication::translate("PhotoPrint", "Új lap"));
    m_duplicate = makeButton(QCoreApplication::translate("PhotoPrint", "Másol"));
    m_duplicate->setToolTip(QCoreApplication::translate("PhotoPrint", "Lap másolása"));
    m_remove = makeButton(QCoreApplication::translate("PhotoPrint", "Törlés"));
    m_left = makeButton(QStringLiteral("‹"));
    m_left->setToolTip(QCoreApplication::translate("PhotoPrint", "Lap előrébb"));
    m_right = makeButton(QStringLiteral("›"));
    m_right->setToolTip(QCoreApplication::translate("PhotoPrint", "Lap hátrébb"));

    connect(m_add, &QPushButton::clicked, this, [this] {
        if (onAdd)
            onAdd();
    });
    connect(m_duplicate, &QPushButton::clicked, this, [this] {
        if (onDuplicate)
            onDuplicate();
    });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        if (onRemove)
            onRemove();
    });
    connect(m_left, &QPushButton::clicked, this, [this] {
        if (onMove)
            onMove(-1);
    });
    connect(m_right, &QPushButton::clicked, this, [this] {
        if (onMove)
            onMove(1);
    });
}

void PagesPanel::setPages(const QList<Page> &pages, int current)
{
    m_syncing = true;
    m_list->clear();
    for (int i = 0; i < pages.size(); ++i) {
        const Page &page = pages.at(i);
        auto *item = new QListWidgetItem(QStringLiteral("%1").arg(i + 1));
        item->setTextAlignment(Qt::AlignCenter);
        item->setSizeHint(QSize(34, 30));
        item->setToolTip(QCoreApplication::translate("PhotoPrint", "%1 · %2 fotó")
                             .arg(page.paper.name)
                             .arg(page.frames.size()));
        m_list->addItem(item);
    }
    if (current >= 0 && current < m_list->count())
        m_list->setCurrentRow(current);
    const bool many = pages.size() > 1;
    m_list->setVisible(many);
    m_duplicate->setVisible(many);
    m_remove->setVisible(many);
    m_left->setVisible(many);
    m_right->setVisible(many);
    m_remove->setEnabled(many);
    m_duplicate->setEnabled(!pages.isEmpty());
    m_left->setEnabled(current > 0);
    m_right->setEnabled(current >= 0 && current < pages.size() - 1);
    m_syncing = false;
}

} // namespace pp
