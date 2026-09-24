/*
 * Tab state and closed-tab history for thispc-view.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sessionmanager.h"

#include <QList>
#include <QUrl>

class TabController final
{
public:
    static constexpr int ClosedTabLimit = 20;

    struct CloseResult {
        bool accepted = false;
        bool wasActive = false;
        int nextActive = -1;
    };

    const QList<TabState> &tabs() const { return m_tabs; }
    QList<TabState> &tabs() { return m_tabs; }
    const QList<TabState> &closedTabs() const { return m_closedTabs; }
    QList<TabState> &closedTabs() { return m_closedTabs; }
    int activeIndex() const { return m_activeIndex; }
    int &activeIndexRef() { return m_activeIndex; }
    bool isValidIndex(int index) const;

    void syncActiveState(const TabState &state);
    int create(const QUrl &url, int sortKey, bool sortAscending);
    int duplicate(int index);
    int reopenClosed();
    bool switchTo(int index);
    CloseResult close(int index);
    bool closeOthers(int keepIndex);
    bool move(int from, int to);
    QUrl dropDirectory(int index, const QUrl &activeUrl) const;

    void clearActive() { m_activeIndex = -1; }
    void setActiveIndex(int index) { m_activeIndex = index; }
    void restore(const SessionSnapshot &snapshot);
    SessionSnapshot snapshot(bool splitPaneActive) const;

private:
    void rememberClosed(const TabState &state);

    QList<TabState> m_tabs;
    QList<TabState> m_closedTabs;
    int m_activeIndex = -1;
};
