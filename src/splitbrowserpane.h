/*
 * Secondary browser pane used by Split View.
 *
 * This is a structural extraction from thispcview.cpp for 0.21.0. Behavior
 * intentionally remains unchanged.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "directoryview.h"
#include "searchcontroller.h"

#include <KIO/ListJob>
#include <KIO/UDSEntry>

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPalette>
#include <QPixmap>
#include <QPoint>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <utility>

class SplitBrowserPane : public QFrame
{
    Q_OBJECT

public:
    explicit SplitBrowserPane(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("splitBrowserPane"));
        setMinimumWidth(330);
        setProperty("active", false);

        QSettings settings;
        m_viewMode =
            std::clamp(
                settings.value(
                    QStringLiteral("directory/viewMode"),
                    0).toInt(),
                0,
                2);
        m_sortKey =
            std::clamp(
                settings.value(
                    QStringLiteral("directory/sortKey"),
                    0).toInt(),
                0,
                3);
        m_sortAscending =
            settings.value(
                QStringLiteral("directory/sortAscending"),
                true).toBool();
        m_showHiddenFiles =
            settings.value(
                QStringLiteral("directory/showHidden"),
                false).toBool();
        m_thumbnailsEnabled =
            settings.value(
                QStringLiteral("directory/thumbnails"),
                true).toBool();

        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        // --------------------------------------------------------------
        // Navigation row
        // --------------------------------------------------------------
        auto *header = new QFrame(this);
        header->setObjectName(QStringLiteral("splitPaneHeader"));

        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(8, 6, 8, 6);
        headerLayout->setSpacing(4);

        m_backButton = new QToolButton(header);
        m_backButton->setIcon(
            themedIcon(QStringLiteral("go-previous")));
        m_backButton->setToolTip(
            trLocal("Wstecz", "Back"));
        m_backButton->setAutoRaise(true);
        headerLayout->addWidget(m_backButton);

        m_forwardButton = new QToolButton(header);
        m_forwardButton->setIcon(
            themedIcon(QStringLiteral("go-next")));
        m_forwardButton->setToolTip(
            trLocal("Dalej", "Forward"));
        m_forwardButton->setAutoRaise(true);
        headerLayout->addWidget(m_forwardButton);

        m_upButton = new QToolButton(header);
        m_upButton->setIcon(
            themedIcon(QStringLiteral("go-up")));
        m_upButton->setToolTip(
            trLocal("W górę", "Up"));
        m_upButton->setAutoRaise(true);
        headerLayout->addWidget(m_upButton);

        // Pretty breadcrumb by default; click to edit the real address.
        m_locationStack = new QStackedWidget(header);

        m_breadcrumbFrame = new QFrame(m_locationStack);
        m_breadcrumbFrame->setObjectName(
            QStringLiteral("splitBreadcrumbFrame"));

        auto *breadcrumbLayout =
            new QHBoxLayout(m_breadcrumbFrame);
        breadcrumbLayout->setContentsMargins(7, 2, 7, 2);
        breadcrumbLayout->setSpacing(5);

        m_breadcrumbIcon =
            new QLabel(m_breadcrumbFrame);
        m_breadcrumbIcon->setFixedSize(18, 18);
        m_breadcrumbIcon->setAlignment(Qt::AlignCenter);
        m_breadcrumbIcon->hide();

        m_thisPcCrumb = new QToolButton(m_breadcrumbFrame);
        m_thisPcCrumb->setObjectName(QStringLiteral("crumbButton"));
        m_thisPcCrumb->setText(trLocal("Ten komputer", "This PC"));
        m_thisPcCrumb->setIcon(themedIcon(QStringLiteral("computer")));
        m_thisPcCrumb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_thisPcCrumb->hide();
        breadcrumbLayout->addWidget(m_thisPcCrumb);
        connect(m_thisPcCrumb, &QToolButton::clicked, this, [this] {
            Q_EMIT activated();
            navigateTo(kThisPcUrl, true);
        });

        m_breadcrumbButton =
            new QToolButton(m_breadcrumbFrame);
        m_breadcrumbButton->setObjectName(
            QStringLiteral("splitBreadcrumbButton"));
        m_breadcrumbButton->setAutoRaise(true);
        m_breadcrumbButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_breadcrumbButton->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Preferred);
        breadcrumbLayout->addWidget(
            m_breadcrumbButton,
            1);

        m_addressEdit =
            new QLineEdit(m_locationStack);
        m_addressEdit->setObjectName(
            QStringLiteral("splitAddressEdit"));
        m_addressEdit->setClearButtonEnabled(true);
        m_addressEdit->setPlaceholderText(
            trLocal(
                "Wpisz ścieżkę",
                "Enter a path"));

        m_locationStack->addWidget(
            m_breadcrumbFrame);
        m_locationStack->addWidget(
            m_addressEdit);
        m_locationStack->setCurrentWidget(
            m_breadcrumbFrame);

        headerLayout->addWidget(
            m_locationStack,
            1);

        m_viewButton = new QToolButton(header);
        m_viewButton->setAutoRaise(true);
        m_viewButton->setPopupMode(
            QToolButton::InstantPopup);
        m_viewButton->setToolTip(
            trLocal("Widok", "View"));

        auto *viewMenu =
            new QMenu(m_viewButton);
        auto *viewGroup =
            new QActionGroup(viewMenu);
        viewGroup->setExclusive(true);

        struct ViewDef {
            int mode;
            const char *pl;
            const char *en;
            const char *icon;
        };

        const ViewDef viewDefs[] = {
            {0, "Ikony", "Icons", "view-list-icons"},
            {1, "Lista", "List", "view-list-text"},
            {2, "Szczegóły", "Details", "view-list-details"}
        };

        for (const ViewDef &def : viewDefs) {
            QAction *action =
                viewMenu->addAction(
                    themedIcon(
                        QString::fromLatin1(
                            def.icon)),
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.mode);
            action->setChecked(
                def.mode == m_viewMode);
            viewGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, mode = def.mode] {
                    setViewMode(mode);
                });
        }

        m_viewButton->setMenu(viewMenu);
        headerLayout->addWidget(m_viewButton);

        m_sortButton = new QToolButton(header);
        m_sortButton->setAutoRaise(true);
        m_sortButton->setPopupMode(
            QToolButton::InstantPopup);
        m_sortButton->setToolTip(
            trLocal("Sortuj", "Sort"));

        auto *sortMenu =
            new QMenu(m_sortButton);
        auto *sortGroup =
            new QActionGroup(sortMenu);
        sortGroup->setExclusive(true);

        struct SortDef {
            int key;
            const char *pl;
            const char *en;
        };

        const SortDef sortDefs[] = {
            {0, "Nazwa", "Name"},
            {1, "Typ", "Type"},
            {2, "Rozmiar", "Size"},
            {3, "Data modyfikacji", "Date modified"}
        };

        for (const SortDef &def : sortDefs) {
            QAction *action =
                sortMenu->addAction(
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setChecked(
                def.key == m_sortKey);
            sortGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, key = def.key] {
                    m_sortKey = key;
                    renderItems();
                    updateSortIcon();
                    Q_EMIT stateChanged();
                });
        }

        sortMenu->addSeparator();

        m_sortAscendingAction =
            sortMenu->addAction(
                themedIcon(
                    QStringLiteral(
                        "view-sort-ascending")),
                trLocal(
                    "Rosnąco",
                    "Ascending"));
        m_sortAscendingAction->setCheckable(true);
        m_sortAscendingAction->setChecked(
            m_sortAscending);

        connect(
            m_sortAscendingAction,
            &QAction::toggled,
            this,
            [this](bool checked) {
                m_sortAscending = checked;
                renderItems();
                updateSortIcon();
                Q_EMIT stateChanged();
            });

        m_sortButton->setMenu(sortMenu);
        headerLayout->addWidget(m_sortButton);

        m_swapButton = new QToolButton(header);
        m_swapButton->setIcon(
            themedIcon(
                QStringLiteral(
                    "object-flip-horizontal"),
                QStringLiteral(
                    "transform-move")));
        m_swapButton->setToolTip(
            trLocal(
                "Zamień lokalizacje paneli",
                "Swap pane locations"));
        m_swapButton->setAutoRaise(true);
        headerLayout->addWidget(m_swapButton);

        m_closeButton = new QToolButton(header);
        m_closeButton->setIcon(
            themedIcon(
                QStringLiteral("window-close")));
        m_closeButton->setToolTip(
            trLocal(
                "Wyłącz widok dzielony (F3)",
                "Close split view (F3)"));
        m_closeButton->setAutoRaise(true);
        headerLayout->addWidget(m_closeButton);

        // Split View uses the window's single command/navigation toolbar.
        // Keep these objects temporarily for the existing internal state
        // plumbing, but remove the asymmetric controls from presentation.
        for (QWidget *control : {
                 static_cast<QWidget *>(m_backButton),
                 static_cast<QWidget *>(m_forwardButton),
                 static_cast<QWidget *>(m_upButton),
                 static_cast<QWidget *>(m_viewButton),
                 static_cast<QWidget *>(m_sortButton),
                 static_cast<QWidget *>(m_swapButton),
                 static_cast<QWidget *>(m_closeButton)}) {
            control->hide();
        }

        outer->addWidget(header);

        m_contentStack = new QStackedWidget(this);
        outer->addWidget(m_contentStack, 1);
        m_directoryPage = new QWidget(m_contentStack);
        auto *directoryLayout = new QVBoxLayout(m_directoryPage);
        directoryLayout->setContentsMargins(0, 0, 0, 0);
        directoryLayout->setSpacing(0);
        m_contentStack->addWidget(m_directoryPage);

        // --------------------------------------------------------------
        // Same heading/count layout as the primary pane
        // --------------------------------------------------------------
        auto *contentHeader =
            new QWidget(this);
        auto *contentHeaderLayout =
            new QVBoxLayout(contentHeader);
        contentHeaderLayout->setContentsMargins(
            16, 12, 16, 7);
        contentHeaderLayout->setSpacing(3);

        m_title = new QLabel(contentHeader);
        QFont titleFont = m_title->font();
        titleFont.setPointSize(
            titleFont.pointSize() + 2);
        titleFont.setBold(true);
        m_title->setFont(titleFont);
        contentHeaderLayout->addWidget(m_title);

        m_status = new QLabel(contentHeader);
        m_status->setObjectName(
            QStringLiteral("splitPaneStatus"));
        m_status->setForegroundRole(
            QPalette::PlaceholderText);
        contentHeaderLayout->addWidget(m_status);

        directoryLayout->addWidget(contentHeader);
        m_searchProgressFrame = new QFrame(m_directoryPage);
        m_searchProgressFrame->setObjectName(QStringLiteral("searchProgressFrame"));
        auto *progressLayout = new QHBoxLayout(m_searchProgressFrame);
        progressLayout->setContentsMargins(16, 0, 16, 8);
        m_searchProgressBar = new QProgressBar(m_searchProgressFrame);
        m_searchProgressBar->setRange(0, 100);
        m_searchProgressBar->setTextVisible(false);
        m_searchProgressBar->setMaximumHeight(7);
        m_cancelSearchButton = new QPushButton(themedIcon(QStringLiteral("process-stop")),
            trLocal("Stop", "Stop"), m_searchProgressFrame);
        m_cancelSearchButton->setFlat(true);
        m_cancelSearchButton->setToolTip(trLocal("Przerwij wyszukiwanie", "Stop search"));
        progressLayout->addWidget(m_searchProgressBar, 1);
        progressLayout->addWidget(m_cancelSearchButton);
        directoryLayout->addWidget(m_searchProgressFrame);
        m_searchProgressFrame->hide();
        m_searchController = new SearchController(this);
        connect(m_cancelSearchButton, &QPushButton::clicked, this, [this] { cancelSearch(true); });
        connect(m_searchController, &SearchController::resultsChanged, this, [this] {
            if (!isSearchLocation(m_currentUrl)) return;
            m_pending = m_searchController->files();
            renderItems();
        });
        connect(m_searchController, &SearchController::progressChanged, this, [this] {
            updateSearchProgress();
        });
        connect(m_searchController, &SearchController::finished, this, [this] {
            updateSearchProgress();
        });

        // --------------------------------------------------------------
        // Icons/List + Details, matching the main pane
        // --------------------------------------------------------------
        m_viewStack = new QStackedWidget(this);

        m_list =
            new DirectoryListWidget(m_viewStack);
        m_list->setObjectName(
            QStringLiteral("splitDirectoryList"));
        m_list->setResizeMode(QListView::Adjust);
        m_list->setMovement(QListView::Static);
        m_list->setSelectionMode(
            QAbstractItemView::ExtendedSelection);
        m_list->setContextMenuPolicy(
            Qt::CustomContextMenu);

        m_details =
            new DirectoryTreeWidget(m_viewStack);
        m_details->setObjectName(
            QStringLiteral("splitDirectoryDetails"));
        m_details->setColumnCount(5);
        m_details->setHeaderLabels({
            trLocal("Nazwa", "Name"),
            trLocal("Typ", "Type"),
            trLocal("Rozmiar", "Size"),
            trLocal(
                "Zmodyfikowano",
                "Date modified"),
            trLocal("Lokalizacja", "Location")
        });
        m_details->setRootIsDecorated(false);
        m_details->setUniformRowHeights(true);
        m_details->setAllColumnsShowFocus(true);
        m_details->setSelectionMode(
            QAbstractItemView::ExtendedSelection);
        m_details->setContextMenuPolicy(
            Qt::CustomContextMenu);
        m_details->setIconSize(QSize(22, 22));

        QHeaderView *detailsHeader =
            m_details->header();
        detailsHeader->setStretchLastSection(false);
        detailsHeader->setSectionsMovable(true);
        detailsHeader->setSectionResizeMode(
            0,
            QHeaderView::Stretch);
        detailsHeader->setSectionResizeMode(
            1,
            QHeaderView::Interactive);
        detailsHeader->setSectionResizeMode(
            2,
            QHeaderView::ResizeToContents);
        detailsHeader->setSectionResizeMode(
            3,
            QHeaderView::ResizeToContents);
        m_details->setColumnWidth(1, 190);
        detailsHeader->setSectionResizeMode(4, QHeaderView::Interactive);
        m_details->setColumnWidth(4, 320);
        m_details->setColumnHidden(4, true);

        m_viewStack->addWidget(m_list);
        m_viewStack->addWidget(m_details);
        directoryLayout->addWidget(m_viewStack, 1);

        connect(
            m_backButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::goBack);
        connect(
            m_forwardButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::goForward);
        connect(
            m_upButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::goUp);
        connect(
            m_closeButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::closeRequested);
        connect(
            m_swapButton,
            &QToolButton::clicked,
            this,
            &SplitBrowserPane::swapRequested);

        connect(
            m_breadcrumbButton,
            &QToolButton::clicked,
            this,
            [this] {
                Q_EMIT activated();
                m_addressEdit->setText(
                    urlForDisplay(
                        m_currentUrl));
                m_locationStack->setCurrentWidget(
                    m_addressEdit);
                m_addressEdit->setFocus(
                    Qt::ShortcutFocusReason);
                m_addressEdit->selectAll();
            });

        connect(
            m_addressEdit,
            &QLineEdit::returnPressed,
            this,
            [this] {
                Q_EMIT activated();
                const QUrl target =
                    urlFromUserText(
                        m_addressEdit->text());
                m_locationStack->setCurrentWidget(
                    m_breadcrumbFrame);
                if (target.isValid()) {
                    navigateTo(target, true);
                }
            });

        auto *escapeAddress =
            new QShortcut(
                QKeySequence(Qt::Key_Escape),
                m_addressEdit);
        connect(
            escapeAddress,
            &QShortcut::activated,
            this,
            [this] {
                m_locationStack->setCurrentWidget(
                    m_breadcrumbFrame);
                updateLocationPresentation();
            });

        connect(
            m_list,
            &QListWidget::itemDoubleClicked,
            this,
            [this](QListWidgetItem *item) {
                activateListItem(item);
            });
        connect(
            m_details,
            &QTreeWidget::itemDoubleClicked,
            this,
            [this](
                QTreeWidgetItem *item,
                int) {
                activateDetailsItem(item);
            });

        connect(
            m_list,
            &QWidget::customContextMenuRequested,
            this,
            &SplitBrowserPane::showListContextMenu);
        connect(
            m_details,
            &QWidget::customContextMenuRequested,
            this,
            &SplitBrowserPane::showDetailsContextMenu);

        connect(
            m_list,
            &DirectoryListWidget::urlsDropped,
            this,
            &SplitBrowserPane::urlsDropped);
        connect(
            m_details,
            &DirectoryTreeWidget::urlsDropped,
            this,
            &SplitBrowserPane::urlsDropped);

        connect(m_list, &QListWidget::itemSelectionChanged,
                this, &SplitBrowserPane::selectionChanged);
        connect(m_details, &QTreeWidget::itemSelectionChanged,
                this, &SplitBrowserPane::selectionChanged);

        applyViewMode();
        updateSortIcon();
        updateNavigationButtons();
    }

    QStackedWidget *contentStack() const { return m_contentStack; }
    void setHomePage(QWidget *page) { m_homePage = page; }
    PaneSearchState &searchState() { return m_searchState; }
    SearchController *searchController() const { return m_searchController; }
    void setSearchRootsProvider(std::function<QList<QUrl>()> provider)
    {
        m_searchRootsProvider = std::move(provider);
    }
    void setDrives(const QList<DriveInfo> &drives)
    {
        m_drives = drives;
        updateNavigationButtons();
    }
    void renderSearchItems() { renderItems(); }
    void updateSearchFilters(const QUrl &url)
    {
        m_currentUrl = url;
        m_searchState.location = url;
        if (m_historyIndex >= 0 && m_historyIndex < m_history.size())
            m_history[m_historyIndex] = url;
        updateLocationPresentation();
        renderItems();
        Q_EMIT stateChanged();
    }
    void cancelSearch(bool userRequested)
    {
        if (!m_searchController->cancel()) return;
        if (userRequested) {
            m_pending = m_searchController->files();
            renderItems();
        }
        updateSearchProgress();
        if (userRequested) Q_EMIT searchCanceled();
    }

    DirectoryListWidget *listView() const { return m_list; }
    DirectoryTreeWidget *detailsView() const { return m_details; }
    QWidget *shortcutScope() const { return m_viewStack; }
    bool canGoBack() const { return m_historyIndex > 0; }
    bool canGoForward() const { return m_historyIndex >= 0 && m_historyIndex + 1 < m_history.size(); }
    bool canGoUp() const { return parentUrl().isValid(); }
    void setDisplayOptions(bool hidden, bool thumbnails)
    {
        const bool hiddenChanged = hidden != m_showHiddenFiles;
        m_showHiddenFiles = hidden;
        m_thumbnailsEnabled = thumbnails;
        if (!hiddenChanged && isSearchLocation(m_currentUrl)) {
            renderItems();
            return;
        }
        refresh();
    }

    void setAlwaysShowFullNames(bool enabled)
    {
        if (m_list) {
            m_list->setAlwaysShowFullNames(enabled);
        }
    }
    void navigateBack() { goBack(); }
    void navigateForward() { goForward(); }
    void navigateUp() { goUp(); }

    void beginAddressEdit()
    {
        Q_EMIT activated();
        m_addressEdit->setText(urlForDisplay(m_currentUrl));
        m_locationStack->setCurrentWidget(m_addressEdit);
        m_addressEdit->setFocus(Qt::ShortcutFocusReason);
        m_addressEdit->selectAll();
    }

    void setAddressActive(bool active)
    {
        if (!m_breadcrumbFrame
            || m_breadcrumbFrame->property("active").toBool() == active) {
            return;
        }
        m_breadcrumbFrame->setProperty("active", active);
        m_breadcrumbFrame->style()->unpolish(m_breadcrumbFrame);
        m_breadcrumbFrame->style()->polish(m_breadcrumbFrame);
        m_breadcrumbFrame->update();
    }

    QUrl currentUrl() const
    {
        return m_currentUrl;
    }

    int viewMode() const
    {
        return m_viewMode;
    }

    int sortKey() const
    {
        return m_sortKey;
    }

    bool sortAscending() const
    {
        return m_sortAscending;
    }

    void setViewMode(int mode)
    {
        m_viewMode =
            std::clamp(mode, 0, 2);
        applyViewMode();
        Q_EMIT stateChanged();
    }

    void setSortState(
        int key,
        bool ascending)
    {
        m_sortKey =
            std::clamp(key, 0, 3);
        m_sortAscending = ascending;

        if (m_sortAscendingAction) {
            QSignalBlocker blocker(
                m_sortAscendingAction);
            m_sortAscendingAction->setChecked(
                ascending);
        }

        if (m_sortButton
            && m_sortButton->menu()) {
            const QList<QAction *> actions =
                m_sortButton->menu()->actions();
            int index = 0;
            for (QAction *action : actions) {
                if (!action->isCheckable()
                    || action
                        == m_sortAscendingAction) {
                    continue;
                }
                QSignalBlocker blocker(action);
                action->setChecked(
                    index == m_sortKey);
                ++index;
                if (index >= 4) {
                    break;
                }
            }
        }

        updateSortIcon();
        renderItems();
    }

    void setCurrentUrl(
        const QUrl &url,
        bool addHistory = true)
    {
        navigateTo(url, addHistory);
    }

    void refresh()
    {
        loadDirectory(m_currentUrl);
    }

    void focusView()
    {
        if (sameLocation(m_currentUrl, kThisPcUrl) && m_homePage) {
            m_homePage->setFocus(Qt::ShortcutFocusReason);
            return;
        }
        if (m_viewMode == 2) {
            m_details->setFocus(
                Qt::ShortcutFocusReason);
        } else {
            m_list->setFocus(
                Qt::ShortcutFocusReason);
        }
    }

    bool viewHasFocus() const
    {
        QWidget *focus =
            QApplication::focusWidget();

        return focus
            && (focus == m_list
                || m_list->isAncestorOf(focus)
                || focus == m_details
                || m_details->isAncestorOf(focus));
    }

Q_SIGNALS:
    void homeRefreshRequested();
    void searchUiChanged();
    void searchCanceled();
    void activated();
    void selectionChanged();
    void contextMenuRequested(bool details, const QPoint &position);
    void closeRequested();
    void swapRequested();
    void openInPrimaryRequested(
        const QUrl &url);
    void openInNewTabRequested(
        const QUrl &url);
    void openInNewWindowRequested(
        const QUrl &url);
    void locationChanged(const QUrl &url);
    void stateChanged();
    void urlsDropped(
        const QList<QUrl> &urls,
        const QUrl &destination,
        const QPoint &globalPosition,
        Qt::KeyboardModifiers modifiers);

private:
    QString friendlyLocationText(
        const QUrl &url) const
    {
        if (sameLocation(url, kThisPcUrl)) {
            return trLocal(
                "Ten komputer",
                "This PC");
        }

        if (isSearchLocation(url)) {
            return (isPolish() ? QStringLiteral("Wyniki dla: %1") : QStringLiteral("Results for: %1"))
                .arg(searchQueryFromUrl(url));
        }

        if (url.isLocalFile()) {
            const QString path =
                QDir::cleanPath(
                    url.toLocalFile());

            QStorageInfo storage(path);
            QString root =
                QDir::cleanPath(
                    storage.rootPath());
            QString rootName =
                storage.displayName();

            if (rootName.trimmed().isEmpty()) {
                rootName =
                    QFileInfo(root).fileName();
            }

            if (rootName.trimmed().isEmpty()
                || root == QStringLiteral("/")) {
                rootName =
                    trLocal(
                        "System",
                        "System");
            }

            QString result =
                trLocal(
                    "Ten komputer",
                    "This PC")
                + QStringLiteral("  ›  ")
                + rootName;

            QString relative =
                QDir(root).relativeFilePath(path);

            if (!relative.isEmpty()
                && relative != QStringLiteral(".")) {
                const QStringList parts =
                    relative.split(
                        QLatin1Char('/'),
                        Qt::SkipEmptyParts);

                for (const QString &part : parts) {
                    result +=
                        QStringLiteral("  ›  ")
                        + part;
                }
            }

            return result;
        }

        if (isAdminUrl(url)) {
            QString result =
                trLocal(
                    "Administrator",
                    "Administrator");

            const QStringList parts =
                url.path().split(
                    QLatin1Char('/'),
                    Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                result +=
                    QStringLiteral("  ›  ")
                    + part;
            }
            return result;
        }

        return urlForDisplay(url);
    }

    QString friendlyTitle(
        const QUrl &url) const
    {
        if (sameLocation(url, kThisPcUrl)) {
            return trLocal(
                "Ten komputer",
                "This PC");
        }

        if (isSearchLocation(url)) {
            return (isPolish() ? QStringLiteral("Wyniki dla: %1") : QStringLiteral("Results for: %1"))
                .arg(searchQueryFromUrl(url));
        }

        if (url.isLocalFile()) {
            const QString path =
                QDir::cleanPath(
                    url.toLocalFile());
            const QFileInfo info(path);

            QStorageInfo storage(path);
            const QString root =
                QDir::cleanPath(
                    storage.rootPath());

            if (path == root) {
                QString label =
                    storage.displayName();
                if (!label.trimmed().isEmpty()) {
                    return label;
                }
            }

            if (!info.fileName().isEmpty()) {
                return info.fileName();
            }
        }

        const QString path =
            url.path();
        const QString name =
            QFileInfo(path).fileName();

        if (!name.isEmpty()) {
            return name;
        }

        return urlForDisplay(url);
    }

    QIcon locationIcon(
        const QUrl &url) const
    {
        if (sameLocation(url, kThisPcUrl)) {
            return themedIcon(
                QStringLiteral("computer"));
        }

        if (isSearchLocation(url)) return themedIcon(QStringLiteral("system-search"));

        if (isAdminUrl(url)) {
            return themedIcon(
                QStringLiteral("security-high"));
        }

        if (url.isLocalFile()) {
            const QString path =
                QDir::cleanPath(
                    url.toLocalFile());
            QStorageInfo storage(path);

            if (QDir::cleanPath(
                    storage.rootPath())
                == path) {
                return themedIcon(
                    QStringLiteral(
                        "drive-harddisk"));
            }
        }

        return themedIcon(
            QStringLiteral("folder"));
    }

    void updateLocationPresentation()
    {
        if (!m_addressEdit->hasFocus()) m_addressEdit->setText(urlForDisplay(m_currentUrl));
        m_thisPcCrumb->setVisible(isSearchLocation(m_currentUrl));
        m_breadcrumbButton->setText(
            friendlyLocationText(
                m_currentUrl));
        m_breadcrumbButton->setToolTip(
            urlForDisplay(
                m_currentUrl));

        m_breadcrumbButton->setIcon(
            locationIcon(m_currentUrl));

        m_title->setText(
            friendlyTitle(
                m_currentUrl));
    }

    QUrl parentUrl() const
    {
        if (!m_currentUrl.isValid()
            || sameLocation(
                m_currentUrl,
                kThisPcUrl)) {
            return {};
        }

        if (isSearchLocation(m_currentUrl)) {
            const QUrl base = searchBaseFromUrl(m_currentUrl);
            return base.isValid() ? base : kThisPcUrl;
        }
        for (const DriveInfo &drive : m_drives) {
            if (sameLocation(drive.targetUrl, m_currentUrl)) return kThisPcUrl;
        }

        QUrl parent = m_currentUrl;
        QString path = parent.path();

        if (path.isEmpty()
            || path == QStringLiteral("/")) {
            return kThisPcUrl;
        }

        while (path.size() > 1
               && path.endsWith(
                   QLatin1Char('/'))) {
            path.chop(1);
        }

        const int slash =
            path.lastIndexOf(
                QLatin1Char('/'));

        path =
            slash <= 0
                ? QStringLiteral("/")
                : path.left(slash);

        parent.setPath(path);
        parent.setQuery(QString());

        if (parent.isLocalFile()
            && QFileInfo(
                parent.toLocalFile())
                   .absoluteFilePath()
                == QFileInfo(
                    m_currentUrl.toLocalFile())
                       .absoluteFilePath()) {
            return kThisPcUrl;
        }

        return normalizedUrl(parent);
    }

    void navigateTo(
        const QUrl &rawUrl,
        bool addHistory)
    {
        if (!rawUrl.isValid()) {
            return;
        }

        const QUrl url =
            normalizedUrl(rawUrl);

        if (addHistory) {
            if (m_historyIndex >= 0
                && m_historyIndex
                    < m_history.size()
                && sameLocation(
                    m_history.at(
                        m_historyIndex),
                    url)) {
                loadDirectory(url);
                return;
            }

            while (m_history.size()
                   > m_historyIndex + 1) {
                m_history.removeLast();
            }

            m_history.push_back(url);
            m_historyIndex =
                m_history.size() - 1;
        }

        m_currentUrl = url;
        loadDirectory(url);
        updateNavigationButtons();
        Q_EMIT locationChanged(m_currentUrl);
        Q_EMIT stateChanged();
    }

    void goBack()
    {
        if (m_historyIndex <= 0) {
            return;
        }

        --m_historyIndex;
        m_currentUrl =
            m_history.at(m_historyIndex);
        loadDirectory(m_currentUrl);
        updateNavigationButtons();
        Q_EMIT locationChanged(m_currentUrl);
        Q_EMIT stateChanged();
    }

    void goForward()
    {
        if (m_historyIndex < 0
            || m_historyIndex + 1
                >= m_history.size()) {
            return;
        }

        ++m_historyIndex;
        m_currentUrl =
            m_history.at(m_historyIndex);
        loadDirectory(m_currentUrl);
        updateNavigationButtons();
        Q_EMIT locationChanged(m_currentUrl);
        Q_EMIT stateChanged();
    }

    void goUp()
    {
        const QUrl parent =
            parentUrl();

        if (parent.isValid()) {
            navigateTo(parent, true);
        }
    }

    void updateNavigationButtons()
    {
        m_backButton->setEnabled(
            m_historyIndex > 0);

        m_forwardButton->setEnabled(
            m_historyIndex >= 0
            && m_historyIndex + 1
                < m_history.size());

        m_upButton->setEnabled(
            parentUrl().isValid());

        updateLocationPresentation();
    }

    void loadDirectory(
        const QUrl &url)
    {
        if (m_job) {
            m_job->kill();
            m_job = nullptr;
        }

        cancelSearch(false);
        m_searchState.loadLocation(url);
        m_pending.clear();
        m_list->clear();
        m_details->clear();
        m_currentUrl = url;
        const bool search = isSearchLocation(url);
        const bool home = sameLocation(url, kThisPcUrl);
        m_list->setDropDirectory(search || home ? QUrl() : url);
        m_details->setDropDirectory(search || home ? QUrl() : url);
        m_details->setColumnHidden(4, !search);
        updateLocationPresentation();
        m_contentStack->setCurrentWidget(home && m_homePage ? m_homePage : m_directoryPage);
        if (home) {
            Q_EMIT homeRefreshRequested();
            Q_EMIT searchUiChanged();
            return;
        }
        if (search) {
            QList<QUrl> roots;
            if (m_searchState.scope == 2 && m_searchRootsProvider) roots = m_searchRootsProvider();
            else {
                const QUrl base = searchBaseFromUrl(url);
                roots.push_back(base.isValid() ? base : QUrl::fromLocalFile(QDir::homePath()));
            }
            m_searchController->start(searchQueryFromUrl(url), roots, m_showHiddenFiles);
            updateSearchProgress();
            return;
        }
        Q_EMIT searchUiChanged();
        m_status->setText(trLocal("Wczytywanie…", "Loading…"));

        KIO::ListJob *job =
            KIO::listDir(
                url,
                KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        m_job = job;

        connect(
            job,
            &KIO::ListJob::entries,
            this,
            [this, url, job](
                KIO::Job *,
                const KIO::UDSEntryList &entries) {
            if (m_job != job) {
                return;
            }

            for (const KIO::UDSEntry &entry :
                 entries) {
                const QString rawName =
                    entry.stringValue(
                        KIO::UDSEntry::UDS_NAME);

                if (rawName.isEmpty()
                    || rawName
                        == QStringLiteral(".")
                    || rawName
                        == QStringLiteral("..")
                    || (!m_showHiddenFiles
                        && rawName.startsWith(
                            QLatin1Char('.')))) {
                    continue;
                }

                m_pending.push_back(
                    fileInfoForEntry(
                        url,
                        entry));
            }
        });

        connect(
            job,
            &KJob::result,
            this,
            [this, job](KJob *) {
            if (m_job != job) {
                return;
            }

            m_job = nullptr;

            if (job->error()) {
                m_status->setText(
                    job->errorString());
                return;
            }

            renderItems();
        });
    }

    QIcon iconForSplitFile(
        const FileInfo &file,
        QMimeDatabase &database)
    {
        const QMimeType mime =
            resolvedMimeType(
                file,
                database);

        QString iconName =
            file.iconName;

        if (iconName.isEmpty()
            || iconName
                == QStringLiteral(
                    "text-x-generic")) {
            if (mime.isValid()
                && !mime.isDefault()) {
                iconName =
                    mime.iconName();

                if (iconName.isEmpty()) {
                    iconName =
                        mime.genericIconName();
                }
            }
        }

        if (iconName.isEmpty()) {
            iconName =
                file.isDir
                    ? QStringLiteral("folder")
                    : QStringLiteral(
                        "text-x-generic");
        }

        const QIcon fallback =
            themedIcon(
                iconName,
                file.isDir
                    ? QStringLiteral("folder")
                    : QStringLiteral(
                        "text-x-generic"));

        const QString mimeName =
            mime.isValid()
                ? mime.name()
                : QString();

        if (!m_thumbnailsEnabled
            || file.isDir
            || !file.url.isLocalFile()
            || !mimeName.startsWith(
                QStringLiteral("image/"))
            || file.size
                > 64LL * 1024LL * 1024LL) {
            return fallback;
        }

        const QString path =
            file.url.toLocalFile();

        QImageReader reader(path);
        reader.setAutoTransform(true);

        const QSize source =
            reader.size();
        const QSize target(128, 128);

        if (source.isValid()
            && (source.width()
                    > target.width()
                || source.height()
                    > target.height())) {
            reader.setScaledSize(
                source.scaled(
                    target,
                    Qt::KeepAspectRatio));
        }

        const QImage image =
            reader.read();

        if (image.isNull()) {
            return fallback;
        }

        QPixmap pixmap =
            QPixmap::fromImage(image)
                .scaled(
                    target,
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation);

        return QIcon(pixmap);
    }

    void renderItems()
    {
        QMimeDatabase database;

        auto typeFor =
            [&database](
                const FileInfo &file) {
            return fileTypeLabel(
                file,
                database);
        };

        sortDirectoryFiles(
            m_pending,
            m_sortKey,
            m_sortAscending,
            typeFor);

        m_list->clear();
        m_details->clear();

        int visibleCount = 0;
        for (const FileInfo &file :
             std::as_const(m_pending)) {
            if (isSearchLocation(m_currentUrl)) {
                if (!SearchController::matchesFile(file, database,
                    {m_searchState.type, m_searchState.date, m_searchState.size})) continue;
            } else {
                const QString query = m_searchState.text.trimmed();
                if (!query.isEmpty() && !file.name.contains(query, Qt::CaseInsensitive)
                    && !file.mimeType.contains(query, Qt::CaseInsensitive)) continue;
            }
            ++visibleCount;
            const QIcon icon =
                iconForSplitFile(
                    file,
                    database);

            const QString typeText =
                fileTypeLabel(
                    file,
                    database);

            const QString sizeText =
                formatFileSize(
                    file.size,
                    file.isDir);

            const QString modifiedText =
                formatModificationTime(
                    file.modificationTime);

            addDirectoryFileItems(
                m_list,
                m_details,
                file,
                icon,
                typeText,
                sizeText,
                modifiedText,
                {parentLocationForDisplay(file.url)});
        }

        if (isSearchLocation(m_currentUrl)) {
            m_status->setText(m_searchController->statusText(visibleCount, m_searchState.scope));
        } else if (m_searchState.text.trimmed().isEmpty()) {
            m_status->setText((isPolish() ? QStringLiteral("%1 elementów") : QStringLiteral("%1 items"))
                .arg(m_pending.size()));
        } else {
            m_status->setText((isPolish() ? QStringLiteral("%1 z %2 elementów") : QStringLiteral("%1 of %2 items"))
                .arg(visibleCount).arg(m_pending.size()));
        }
    }

    void updateSearchProgress()
    {
        const bool running = m_searchController->isRunning();
        m_searchProgressFrame->setVisible(running);
        m_searchProgressBar->setValue(m_searchController->progressPercent());
        if (isSearchLocation(m_currentUrl))
            m_status->setText(m_searchController->statusText(m_list->count(), m_searchState.scope));
        Q_EMIT searchUiChanged();
    }

    void applyViewMode()
    {
        applyDirectoryViewLayout(
            m_list,
            m_details,
            m_viewStack,
            m_viewButton,
            m_viewMode);

        if (m_viewButton
            && m_viewButton->menu()) {
            for (QAction *action :
                 m_viewButton->menu()
                     ->actions()) {
                if (!action->isCheckable()) {
                    continue;
                }

                QSignalBlocker blocker(action);
                action->setChecked(
                    action->data().toInt()
                    == m_viewMode);
            }
        }
    }

    void updateSortIcon()
    {
        if (!m_sortButton) {
            return;
        }

        m_sortButton->setIcon(
            themedIcon(
                m_sortAscending
                    ? QStringLiteral(
                        "view-sort-ascending")
                    : QStringLiteral(
                        "view-sort-descending")));
    }

    void activateListItem(
        QListWidgetItem *item)
    {
        if (!item) {
            return;
        }

        const QUrl url(
            item->data(
                Qt::UserRole).toString());

        const bool isDir =
            item->data(
                Qt::UserRole + 1).toBool();

        if (isDir) {
            navigateTo(url, true);
        } else if (url.isValid()) {
            QDesktopServices::openUrl(url);
        }
    }

    void activateDetailsItem(
        QTreeWidgetItem *item)
    {
        if (!item) {
            return;
        }

        const QUrl url(
            item->data(
                0,
                Qt::UserRole).toString());

        const bool isDir =
            item->data(
                0,
                Qt::UserRole + 1).toBool();

        if (isDir) {
            navigateTo(url, true);
        } else if (url.isValid()) {
            QDesktopServices::openUrl(url);
        }
    }

    void showListContextMenu(const QPoint &pos)
    {
        Q_EMIT contextMenuRequested(false, pos);
    }

    void showDetailsContextMenu(const QPoint &pos)
    {
        Q_EMIT contextMenuRequested(true, pos);
    }

    QStackedWidget *m_contentStack = nullptr;
    QWidget *m_directoryPage = nullptr;
    QWidget *m_homePage = nullptr;
    QToolButton *m_thisPcCrumb = nullptr;
    PaneSearchState m_searchState;
    SearchController *m_searchController = nullptr;
    QFrame *m_searchProgressFrame = nullptr;
    QProgressBar *m_searchProgressBar = nullptr;
    QPushButton *m_cancelSearchButton = nullptr;
    std::function<QList<QUrl>()> m_searchRootsProvider;
    QList<DriveInfo> m_drives;

    QToolButton *m_backButton = nullptr;
    QToolButton *m_forwardButton = nullptr;
    QToolButton *m_upButton = nullptr;
    QToolButton *m_viewButton = nullptr;
    QToolButton *m_sortButton = nullptr;
    QToolButton *m_swapButton = nullptr;
    QToolButton *m_closeButton = nullptr;
    QAction *m_sortAscendingAction = nullptr;

    QStackedWidget *m_locationStack = nullptr;
    QFrame *m_breadcrumbFrame = nullptr;
    QLabel *m_breadcrumbIcon = nullptr;
    QToolButton *m_breadcrumbButton = nullptr;
    QLineEdit *m_addressEdit = nullptr;

    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;

    QStackedWidget *m_viewStack = nullptr;
    DirectoryListWidget *m_list = nullptr;
    DirectoryTreeWidget *m_details = nullptr;

    QUrl m_currentUrl = kThisPcUrl;
    QList<QUrl> m_history;
    int m_historyIndex = -1;

    int m_viewMode = 0;
    int m_sortKey = 0;
    bool m_sortAscending = true;
    bool m_showHiddenFiles = false;
    bool m_thumbnailsEnabled = true;

    QList<FileInfo> m_pending;
    QPointer<KIO::ListJob> m_job;
};
