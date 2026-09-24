/*
 * Adapter for reading and routing the two concrete browser panes.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "panecontext.h"

#include <functional>

class DirectoryListWidget;
class DirectoryTreeWidget;

struct PaneBinding {
    std::function<bool()> available;
    std::function<QUrl()> directory;
    std::function<bool()> isDirectory;
    std::function<int()> viewMode;
    std::function<DirectoryListWidget *()> listView;
    std::function<DirectoryTreeWidget *()> detailsView;
    std::function<void(const QUrl &)> navigate;
    std::function<void()> refresh;
    std::function<void(const QUrl &)> openInOtherPane;
};

class PaneAdapter
{
public:
    PaneAdapter(PaneBinding primary, PaneBinding split);

    PaneId availablePane(PaneId requested) const;
    PaneContext context(PaneId requested) const;
    QList<QUrl> selectedUrls(PaneId requested) const;
    void navigate(PaneId pane, const QUrl &url) const;
    void refresh(PaneId pane) const;
    void openInOtherPane(PaneId pane, const QUrl &url) const;

private:
    const PaneBinding &binding(PaneId pane) const;

    PaneBinding m_primary;
    PaneBinding m_split;
};
