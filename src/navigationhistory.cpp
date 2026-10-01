/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#include "navigationhistory.h"
#include "browsercommon.h"
#include "locationpresentation.h"

bool NavigationHistory::canGoForward() const
{ return m_historyIndex >= 0 && m_historyIndex < m_history.size() - 1; }
bool NavigationHistory::canGoUp() const
{ return !sameLocation(m_currentUrl, kThisPcUrl); }
bool NavigationHistory::navigate(const QUrl &rawUrl, bool addHistory)
{
    if (!rawUrl.isValid()) return false;
    const QUrl url = normalizedUrl(rawUrl);
    if (addHistory && (m_historyIndex < 0 || m_historyIndex >= m_history.size()
                       || !sameLocation(m_history.at(m_historyIndex), url))) {
        while (m_history.size() > m_historyIndex + 1) m_history.removeLast();
        m_history.push_back(url);
        m_historyIndex = m_history.size() - 1;
    }
    m_currentUrl = url;
    return true;
}
QUrl NavigationHistory::back()
{
    if (!canGoBack()) return {};
    m_currentUrl = m_history.at(--m_historyIndex);
    return m_currentUrl;
}
QUrl NavigationHistory::forward()
{
    if (!canGoForward()) return {};
    m_currentUrl = m_history.at(++m_historyIndex);
    return m_currentUrl;
}
QUrl NavigationHistory::parentUrl(const QVector<DriveInfo> &drives) const
{
    return LocationPresentation::parentUrl(
        m_currentUrl, drives, LocationPresentation::ParentProfile::Primary);
}
QUrl NavigationHistory::up(const QVector<DriveInfo> &drives)
{
    const QUrl parent = parentUrl(drives);
    if (parent.isValid()) navigate(parent, true);
    return parent;
}
void NavigationHistory::updateCurrent(const QUrl &url, bool updateHistoryEntry)
{
    m_currentUrl = normalizedUrl(url);
    if (updateHistoryEntry && m_historyIndex >= 0 && m_historyIndex < m_history.size())
        m_history[m_historyIndex] = m_currentUrl;
}
NavigationHistory::Snapshot NavigationHistory::snapshot() const
{ return {m_currentUrl, m_history, m_historyIndex}; }
void NavigationHistory::restore(const Snapshot &snapshot)
{
    m_currentUrl = normalizedUrl(snapshot.currentUrl);
    m_history.clear();
    for (const QUrl &url : snapshot.history) m_history.push_back(normalizedUrl(url));
    m_historyIndex = snapshot.historyIndex;
    if (m_history.isEmpty()) { m_history = {m_currentUrl}; m_historyIndex = 0; }
    if (m_historyIndex < 0 || m_historyIndex >= m_history.size())
        m_historyIndex = m_history.size() - 1;
}

NavigationHistory::Snapshot NavigationHistory::safeRestoreBaseline(const Snapshot &snapshot)
{
    // An unverified restored remote must not become the active/history entry.
    NavigationHistory normalized;
    normalized.restore(snapshot);
    Snapshot safe = normalized.snapshot();
    const int index = std::clamp(snapshot.historyIndex, 0,
                               std::max(0, static_cast<int>(safe.history.size()) - 1));
    if (!safe.history.isEmpty() && sameLocation(safe.history.at(index), safe.currentUrl))
        safe.history.removeAt(index);
    int fallback = std::min(index - 1, static_cast<int>(safe.history.size()) - 1);
    while (fallback >= 0 && RemoteUrlHelper::isRemoteUrl(safe.history.at(fallback))) --fallback;
    if (fallback >= 0) {
        safe.currentUrl = safe.history.at(fallback);
        safe.historyIndex = fallback;
    } else {
        safe.currentUrl = kThisPcUrl;
        safe.history.prepend(kThisPcUrl);
        safe.historyIndex = 0;
    }
    return safe;
}
