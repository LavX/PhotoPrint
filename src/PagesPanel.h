#pragma once

#include "Document.h"

#include <QList>
#include <QWidget>

#include <functional>

class QListWidget;
class QPushButton;

namespace pp {

/// Page strip with plain actions: new page, copy, delete and order.
class PagesPanel : public QWidget
{
public:
    explicit PagesPanel(QWidget *parent = nullptr);

    void setPages(const QList<Page> &pages, int current);

    std::function<void(int)> onSelect;
    std::function<void()> onAdd;
    std::function<void()> onDuplicate;
    std::function<void()> onRemove;
    std::function<void(int)> onMove;

private:
    QListWidget *m_list = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_duplicate = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_left = nullptr;
    QPushButton *m_right = nullptr;
    bool m_syncing = false;
};

} // namespace pp
