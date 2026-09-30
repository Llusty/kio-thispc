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
class QProgressBar;
class QToolButton;

class SearchUiController final
{
public:
    struct Widgets {
        QLineEdit *edit = nullptr;
        QAction *stopAction = nullptr;
        QAction *scopeAction = nullptr;
        QToolButton *filterButton = nullptr;
        QActionGroup *scopeGroup = nullptr;
        QActionGroup *typeGroup = nullptr;
        QActionGroup *dateGroup = nullptr;
        QActionGroup *sizeGroup = nullptr;
        QFrame *progressFrame = nullptr;
        QProgressBar *progressBar = nullptr;
        QLabel *statusLabel = nullptr;
    };

    struct Presentation {
        std::function<QString(const char *, const char *)> translate;
        std::function<QString(const QUrl &)> displayName;
        std::function<QIcon()> scopeIcon;
        std::function<bool()> polish;
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
    void updateProgress(SearchController &backend) const;
    void updateStatus(SearchController &backend, int visibleCount, int scope) const;

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
