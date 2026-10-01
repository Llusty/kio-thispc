/*
 * Widget-free primary navigation history.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <QList>
#include <QUrl>
#include <QVector>
struct DriveInfo;
class NavigationHistory final
{
public:
    struct Snapshot { QUrl currentUrl; QList<QUrl> history; int historyIndex = -1; };
    const QUrl &currentUrl() const { return m_currentUrl; }
    const QList<QUrl> &history() const { return m_history; }
    int historyIndex() const { return m_historyIndex; }
    bool canGoBack() const { return m_historyIndex > 0; }
    bool canGoForward() const;
    bool canGoUp() const;
    bool navigate(const QUrl &url, bool addHistory = true);
    QUrl back();
    QUrl forward();
    QUrl parentUrl(const QVector<DriveInfo> &drives) const;
    QUrl up(const QVector<DriveInfo> &drives);
    void updateCurrent(const QUrl &url, bool updateHistoryEntry = false);
    Snapshot snapshot() const;
    void restore(const Snapshot &snapshot);
    static Snapshot safeRestoreBaseline(const Snapshot &snapshot);
private:
    QUrl m_currentUrl = QUrl(QStringLiteral("thispc:/"));
    QList<QUrl> m_history;
    int m_historyIndex = -1;
};
