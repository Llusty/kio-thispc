/*
 * Search toolbar binding and search-request translation.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "searchcontroller.h"

#include <QIcon>
#include <QList>
#include <QString>
#include <QUrl>

#include <functional>

class QAction;
class QActionGroup;
class QFrame;
class QLabel;
class QLineEdit;
class SearchProgressBar;
class QToolButton;

class SearchUiController final
{
public:
    struct Widgets {
        QLineEdit *edit = nullptr;
        QAction *stopAction = nullptr;
        QAction *scopeAction = nullptr;
        QToolButton *scopeButton = nullptr;
        QToolButton *filterButton = nullptr;
        QAction *filterIndicatorAction = nullptr;
        QActionGroup *scopeGroup = nullptr;
        QActionGroup *typeGroup = nullptr;
        QAction *typeInfoAction = nullptr;
        QActionGroup *dateGroup = nullptr;
        QAction *dateInfoAction = nullptr;
        QActionGroup *sizeGroup = nullptr;
        QAction *sizeInfoAction = nullptr;
        QFrame *progressFrame = nullptr;
        SearchProgressBar *progressBar = nullptr;
        QLabel *statusLabel = nullptr;
        QToolButton *nameModeButton = nullptr;
        QActionGroup *nameModeGroup = nullptr;
        QAction *clearAction = nullptr;
    };

    struct Presentation {
        std::function<QString(const char *, const char *)> translate;
        std::function<QString(const QUrl &)> displayName;
        std::function<QIcon()> scopeIcon;
        std::function<bool()> polish;
        std::function<QIcon(int)> filterBadgeIcon;
    };

    struct Context {
        QUrl location;
        PaneSearchState *state = nullptr;
        SearchController *backend = nullptr;
    };

    struct Request {
        QString query;
        int scope = 0;
        QUrl base;
        QList<QUrl> roots;

        bool isValid() const { return !query.isEmpty(); }
        QUrl location(const PaneSearchState &state) const;
    };

    SearchUiController(Widgets widgets, Presentation presentation);

    void updateControls(const Context &context) const;
    void updateProgress(const SearchController &backend) const;
    void updateStatus(const SearchController &backend, int visibleCount, int scope, bool recursive = true) const;

    Request requestFromUi(const Context &context,
                          const QList<QUrl> &driveRoots,
                          const QUrl &home) const;
    QUrl locationWithSyncedFilters(const Context &context) const;
    QList<QUrl> rootsForLocation(const QUrl &location,
                                 const PaneSearchState &state,
                                 const QList<QUrl> &driveRoots,
                                 const QUrl &home) const;

    static QUrl bestDriveRootForUrl(const QUrl &url,
                                    const QList<QUrl> &driveRoots);
    static QList<QUrl> wholeComputerSearchRoots(const QList<QUrl> &driveRoots);
    static QUrl searchContextUrl(const QUrl &location, const QUrl &home);

private:
    QString tr(const char *pl, const char *en) const;
    Widgets m_widgets;
    Presentation m_presentation;
};
