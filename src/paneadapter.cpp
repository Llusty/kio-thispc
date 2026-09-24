/* SPDX-License-Identifier: MIT */

#include "paneadapter.h"

#include "directoryview.h"

#include <QAbstractItemView>
#include <QTreeWidgetItem>

#include <utility>

PaneAdapter::PaneAdapter(PaneBinding primary, PaneBinding split)
    : m_primary(std::move(primary))
    , m_split(std::move(split))
{
}

const PaneBinding &PaneAdapter::binding(PaneId pane) const
{
    return pane == PaneId::Split ? m_split : m_primary;
}

PaneId PaneAdapter::availablePane(PaneId requested) const
{
    return requested == PaneId::Split && m_split.available && m_split.available()
        ? PaneId::Split : PaneId::Primary;
}

PaneContext PaneAdapter::context(PaneId requested) const
{
    PaneContext context;
    context.id = availablePane(requested);
    const PaneBinding &pane = binding(context.id);
    context.directory = pane.directory ? pane.directory() : QUrl();
    context.isDirectory = pane.isDirectory && pane.isDirectory();
    const int mode = pane.viewMode ? pane.viewMode() : 0;
    DirectoryListWidget *list = pane.listView ? pane.listView() : nullptr;
    DirectoryTreeWidget *tree = pane.detailsView ? pane.detailsView() : nullptr;
    context.view = mode == 2 ? static_cast<QAbstractItemView *>(tree) : list;
    if (!context.isDirectory || !context.view) {
        return context;
    }
    if (mode == 2 && tree) {
        for (auto *item : tree->selectedItems()) {
            context.items.push_back({QUrl(item->data(0, Qt::UserRole).toString()),
                item->text(0), item->text(1), item->text(2), item->text(3),
                item->data(0, Qt::UserRole + 1).toBool()});
        }
    } else if (list) {
        for (const QModelIndex &item : list->selectedItems()) {
            context.items.push_back({QUrl(item.data(directory_view_detail::UrlRole).toString()),
                item.data(Qt::DisplayRole).toString(),
                item.data(directory_view_detail::TypeTextRole).toString(),
                item.data(directory_view_detail::SizeTextRole).toString(),
                item.data(directory_view_detail::ModifiedTextRole).toString(),
                item.data(directory_view_detail::DirectoryRole).toBool()});
        }
    }
    return context;
}

QList<QUrl> PaneAdapter::selectedUrls(PaneId requested) const
{
    QList<QUrl> urls;
    for (const PaneItem &item : context(requested).items) {
        if (item.url.isValid()) urls.push_back(item.url);
    }
    return urls;
}

void PaneAdapter::navigate(PaneId pane, const QUrl &url) const
{
    const PaneBinding &target = binding(pane);
    if (target.navigate) target.navigate(url);
}

void PaneAdapter::refresh(PaneId pane) const
{
    const PaneBinding &target = binding(pane);
    if (target.refresh) target.refresh();
}

void PaneAdapter::openInOtherPane(PaneId pane, const QUrl &url) const
{
    const PaneBinding &source = binding(pane);
    if (source.openInOtherPane) source.openInOtherPane(url);
}
