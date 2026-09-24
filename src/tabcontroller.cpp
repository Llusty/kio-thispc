/*
 * Tab state and closed-tab history for thispc-view.
 * SPDX-License-Identifier: MIT
 */

#include "tabcontroller.h"

#include <KProtocolManager>

#include <algorithm>

bool TabController::isValidIndex(int index) const
{
    return index >= 0 && index < m_tabs.size();
}

void TabController::syncActiveState(const TabState &state)
{
    if (isValidIndex(m_activeIndex)) {
        m_tabs[m_activeIndex] = state;
    }
}

int TabController::create(
    const QUrl &url,
    int sortKey,
    bool sortAscending)
{
    TabState state;
    state.currentUrl = url;
    state.history = {url};
    state.historyIndex = 0;
    state.splitEnabled = false;
    state.splitUrl = url;
    state.splitViewMode = -1;
    state.splitSortKey = sortKey;
    state.splitSortAscending = sortAscending;
    m_tabs.push_back(state);
    return m_tabs.size() - 1;
}

int TabController::duplicate(int index)
{
    if (!isValidIndex(index)) {
        return -1;
    }
    m_tabs.push_back(m_tabs.at(index));
    return m_tabs.size() - 1;
}

int TabController::reopenClosed()
{
    if (m_closedTabs.isEmpty()) {
        return -1;
    }
    m_tabs.push_back(m_closedTabs.takeLast());
    return m_tabs.size() - 1;
}

bool TabController::switchTo(int index)
{
    if (!isValidIndex(index)) {
        return false;
    }
    m_activeIndex = index;
    return true;
}

TabController::CloseResult TabController::close(int index)
{
    CloseResult result;
    if (!isValidIndex(index) || m_tabs.size() == 1) {
        return result;
    }

    result.accepted = true;
    result.wasActive = index == m_activeIndex;
    result.nextActive = m_activeIndex;
    rememberClosed(m_tabs.at(index));

    if (index < m_activeIndex) {
        --result.nextActive;
    } else if (result.wasActive) {
        result.nextActive = std::min(
            index,
            static_cast<int>(m_tabs.size()) - 2);
    }

    m_tabs.removeAt(index);
    m_activeIndex = result.wasActive ? -1 : result.nextActive;
    return result;
}

bool TabController::closeOthers(int keepIndex)
{
    if (!isValidIndex(keepIndex)) {
        return false;
    }

    const TabState kept = m_tabs.at(keepIndex);
    for (int i = m_tabs.size() - 1; i >= 0; --i) {
        if (i != keepIndex) {
            rememberClosed(m_tabs.at(i));
        }
    }
    m_tabs = {kept};
    m_activeIndex = -1;
    return true;
}

bool TabController::move(int from, int to)
{
    if (!isValidIndex(from) || !isValidIndex(to)) {
        return false;
    }

    if (m_activeIndex == from) {
        m_activeIndex = to;
    } else if (from < m_activeIndex && m_activeIndex <= to) {
        --m_activeIndex;
    } else if (to <= m_activeIndex && m_activeIndex < from) {
        ++m_activeIndex;
    }
    m_tabs.move(from, to);
    return true;
}

QUrl TabController::dropDirectory(
    int index,
    const QUrl &activeUrl) const
{
    if (!isValidIndex(index)) {
        return {};
    }

    const QUrl url = index == m_activeIndex
        ? activeUrl
        : m_tabs.at(index).currentUrl;
    const QString scheme = url.scheme().toLower();
    // File copy/move accepts writable directories, not virtual roots or Trash
    // (which requires KIO::trash rather than KIO::move).
    if (!url.isValid() || scheme.isEmpty()
        || scheme == QStringLiteral("thispc")
        || scheme == QStringLiteral("trash")
        || scheme == QStringLiteral("remote")
        || scheme == QStringLiteral("thispcsearch")
        || scheme == QStringLiteral("filenamesearch")
        || !KProtocolManager::supportsListing(url)
        || !KProtocolManager::supportsWriting(url)) {
        return {};
    }
    return url;
}

void TabController::restore(const SessionSnapshot &snapshot)
{
    m_tabs = snapshot.tabs;
    m_closedTabs.clear();
    m_activeIndex = m_tabs.isEmpty() ? -1 : snapshot.activeTab;
}

SessionSnapshot TabController::snapshot(bool splitPaneActive) const
{
    SessionSnapshot result;
    result.tabs = m_tabs;
    result.activeTab = m_activeIndex;
    result.splitPaneActive = splitPaneActive;
    return result;
}

void TabController::rememberClosed(const TabState &state)
{
    m_closedTabs.push_back(state);
    while (m_closedTabs.size() > ClosedTabLimit) {
        m_closedTabs.removeFirst();
    }
}
