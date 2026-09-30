/*
 * Session persistence for tabs and per-tab split-pane state.
 *
 * Extracted during the 0.21.0 architecture refactor. Keep this module focused
 * on serializing/restoring session data; UI presentation stays in ThisPcWindow.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <QList>
#include <QSettings>
#include <QStringList>
#include <QUrl>

#include <algorithm>

struct TabState
{
    QUrl currentUrl = kThisPcUrl;
    QList<QUrl> history;
    int historyIndex = -1;
    bool splitEnabled = false;
    QUrl splitUrl = kThisPcUrl;
    int splitViewMode = -1;
    int splitSortKey = 0;
    bool splitSortAscending = true;
};

struct SessionSnapshot
{
    QList<TabState> tabs;
    int activeTab = 0;
    bool splitPaneActive = false;
};

class SessionManager final
{
public:
    static bool restorePreviousSessionEnabled()
    {
        QSettings settings;
        return settings.value(
            QStringLiteral("session/restorePrevious"),
            true).toBool();
    }

    static void setRestorePreviousSessionEnabled(bool enabled)
    {
        QSettings settings;
        settings.setValue(
            QStringLiteral("session/restorePrevious"),
            enabled);
    }

    static void save(const SessionSnapshot &snapshot)
    {
        QSettings settings;
        settings.setValue(
            QStringLiteral("session/activeTab"),
            snapshot.activeTab);
        settings.setValue(
            QStringLiteral("session/activePane"),
            snapshot.splitPaneActive ? 1 : 0);

        settings.beginWriteArray(
            QStringLiteral("session/tabs"),
            static_cast<int>(snapshot.tabs.size()));
        for (int i = 0; i < snapshot.tabs.size(); ++i) {
            settings.setArrayIndex(i);
            const TabState &state = snapshot.tabs.at(i);

            settings.setValue(
                QStringLiteral("currentUrl"),
                state.currentUrl.toString(QUrl::FullyEncoded));

            QStringList history;
            history.reserve(state.history.size());
            for (const QUrl &url : state.history) {
                history.push_back(
                    url.toString(QUrl::FullyEncoded));
            }
            settings.setValue(
                QStringLiteral("history"),
                history);
            settings.setValue(
                QStringLiteral("historyIndex"),
                state.historyIndex);
            settings.setValue(
                QStringLiteral("splitEnabled"),
                state.splitEnabled);
            settings.setValue(
                QStringLiteral("splitUrl"),
                state.splitUrl.toString(QUrl::FullyEncoded));
            settings.setValue(
                QStringLiteral("splitViewMode"),
                state.splitViewMode);
            settings.setValue(
                QStringLiteral("splitSortKey"),
                state.splitSortKey);
            settings.setValue(
                QStringLiteral("splitSortAscending"),
                state.splitSortAscending);
        }
        settings.endArray();
        settings.sync();
    }

    static SessionSnapshot load(
        int defaultSortKey,
        bool defaultSortAscending)
    {
        QSettings settings;
        SessionSnapshot snapshot;

        const int count = settings.beginReadArray(
            QStringLiteral("session/tabs"));
        snapshot.tabs.reserve(count);

        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);

            TabState state;
            state.currentUrl = normalizedUrl(
                QUrl(settings.value(
                    QStringLiteral("currentUrl")).toString()));
            if (!state.currentUrl.isValid()) {
                continue;
            }

            const QStringList historyValues =
                settings.value(
                    QStringLiteral("history")).toStringList();
            for (const QString &value : historyValues) {
                const QUrl url = normalizedUrl(QUrl(value));
                if (url.isValid()) {
                    state.history.push_back(url);
                }
            }
            if (state.history.isEmpty()) {
                state.history = {state.currentUrl};
            }
            state.historyIndex = std::clamp(
                settings.value(
                    QStringLiteral("historyIndex"),
                    static_cast<int>(state.history.size()) - 1).toInt(),
                0,
                static_cast<int>(state.history.size()) - 1);

            state.splitEnabled = settings.value(
                QStringLiteral("splitEnabled"),
                false).toBool();
            state.splitUrl = normalizedUrl(
                QUrl(settings.value(
                    QStringLiteral("splitUrl")).toString()));
            if (!state.splitUrl.isValid()) {
                state.splitUrl = state.currentUrl;
            }
            state.splitViewMode = settings.value(
                QStringLiteral("splitViewMode"),
                -1).toInt();
            state.splitSortKey = std::clamp(
                settings.value(
                    QStringLiteral("splitSortKey"),
                    defaultSortKey).toInt(),
                0,
                3);
            state.splitSortAscending = settings.value(
                QStringLiteral("splitSortAscending"),
                defaultSortAscending).toBool();

            snapshot.tabs.push_back(state);
        }
        settings.endArray();

        snapshot.activeTab = settings.value(
            QStringLiteral("session/activeTab"),
            0).toInt();
        snapshot.splitPaneActive = settings.value(
            QStringLiteral("session/activePane"),
            0).toInt() == 1;

        if (!snapshot.tabs.isEmpty()) {
            snapshot.activeTab = std::clamp(
                snapshot.activeTab,
                0,
                static_cast<int>(snapshot.tabs.size()) - 1);
        } else {
            snapshot.activeTab = 0;
        }

        return snapshot;
    }
};
