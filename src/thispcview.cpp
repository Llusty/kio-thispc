/*
 * thispc-view - a lightweight KDE/Qt file browser with a Windows-like
 * "This PC" home page, backed by KIO.
 *
 * Version 0.32.0
 * SPDX-License-Identifier: MIT
 */

#include "archive-creation.h"
#include "archive-detection.h"
#include "archive-extraction.h"
#include "actionstatecontroller.h"
#include "applicationstyle.h"
#include "appwidgets.h"
#include "drivehomecoordinator.h"
#include "locationpresentation.h"
#include "navigationhistory.h"
#include "paneadapter.h"
#include "panemenucontroller.h"
#include "primarybrowserpane.h"
#include "previewcoordinator.h"
#include "selectionmenucontroller.h"
#include "tabcontroller.h"
#include <KIO/CopyJob>
#include <KIO/Global>
#include <KIO/JobUiDelegateFactory>
#include <KIO/ChmodJob>
#include <KIO/StoredTransferJob>
#include <KIO/ListJob>
#include <KIO/MkdirJob>
#include <KIO/SimpleJob>
#include <KIO/StatJob>
#include <KIO/UDSEntry>
#include <KJob>
#include <KJobUiDelegate>
#include <KFileItem>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <KProtocolInfo>
#include <KProtocolManager>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFocusEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QMimeData>
#include <QMenu>
#include <QMimeDatabase>
#include <QMimeType>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QPalette>
#include <QPointer>
#include <QProcess>
#include <QPixmap>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStatusBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QTabWidget>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextLayout>
#include <QtConcurrent>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidget>
#include <QWidgetAction>

#include "browsercommon.h"
#include "directoryview.h"
#include "directoryviewsettings.h"
#include "fileactions.h"
#include "templatemenu.h"
#include "operationmanager.h"
#include "propertiesdialog.h"
#include "previewpane.h"
#include "quicklook.h"
#include "undocontroller.h"
#include "sidebar.h"
#include "sessionmanager.h"
#include "searchcontroller.h"
#include "searchuicontroller.h"
#include "splitbrowserpane.h"
#include "splitcomparedialog.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <memory>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{

bool adminProtocolAvailable()
{
    return KProtocolInfo::isKnownProtocol(
        QStringLiteral("admin"));
}

QUrl ordinaryFileUrlForAdmin(const QUrl &url)
{
    if (!isAdminUrl(url)) {
        return url;
    }

    return QUrl::fromLocalFile(
        QDir::cleanPath(url.path()));
}

bool isWithinLocation(const QUrl &childRaw, const QUrl &baseRaw)
{
    const QUrl child = normalizedUrl(childRaw);
    const QUrl base = normalizedUrl(baseRaw);

    if (!child.isValid() || !base.isValid()) {
        return false;
    }

    if (sameLocation(child, base)) {
        return true;
    }

    if (child.scheme() != base.scheme()) {
        return false;
    }

    if (child.isLocalFile() && base.isLocalFile()) {
        const QString childPath = QDir::cleanPath(child.toLocalFile());
        const QString basePath = QDir::cleanPath(base.toLocalFile());

        if (basePath == QStringLiteral("/")) {
            return childPath.startsWith(QLatin1Char('/'));
        }

        return childPath.startsWith(basePath + QDir::separator());
    }

    QString childPath = child.path();
    QString basePath = base.path();

    if (!basePath.endsWith(QLatin1Char('/'))) {
        basePath += QLatin1Char('/');
    }

    return childPath.startsWith(basePath);
}

bool dropWouldCreateCycle(
    const QList<QUrl> &urls,
    const QUrl &destination);

namespace {

bool localPathIsWithin(
    const QString &childPath,
    const QString &basePath)
{
    const QString child = QFileInfo(childPath).canonicalFilePath();
    const QString base = QFileInfo(basePath).canonicalFilePath();
    if (child.isEmpty() || base.isEmpty()) {
        return false;
    }
    const QString relative = QDir(base).relativeFilePath(child);
    return relative.isEmpty()
        || (relative != QStringLiteral("..")
            && !relative.startsWith(QStringLiteral("..%1")
                .arg(QDir::separator()))
            && !QDir::isAbsolutePath(relative));
}

}

bool dropWouldCreateCycle(
    const QList<QUrl> &urls,
    const QUrl &destination)
{
    for (const QUrl &url : urls) {
        if (url.isLocalFile() && destination.isLocalFile()) {
            const QFileInfo sourceInfo(url.toLocalFile());
            if (sourceInfo.isDir()
                && localPathIsWithin(
                    destination.toLocalFile(),
                    sourceInfo.absoluteFilePath())) {
                return true;
            }
        } else if (sameLocation(url, destination)
                   || isWithinLocation(destination, url)) {
            return true;
        }
    }
    return false;
}

void openInDolphin(const QUrl &url)
{
    if (!url.isValid()) {
        return;
    }

    const QString argument =
        url.isLocalFile() ? url.toLocalFile() : url.toString();

    if (!QProcess::startDetached(QStringLiteral("dolphin"), {argument})) {
        QDesktopServices::openUrl(url);
    }
}

bool openTerminalAt(const QUrl &url)
{
    if (!url.isLocalFile()) {
        return false;
    }

    const QString path = url.toLocalFile();

    if (QProcess::startDetached(
            QStringLiteral("konsole"),
            {QStringLiteral("--workdir"), path})) {
        return true;
    }

    return QProcess::startDetached(
        QStringLiteral("x-terminal-emulator"),
        {},
        path);
}



} // namespace

class ThisPcWindow : public QMainWindow
{
    Q_OBJECT

#ifdef THISPC_TEST_HARNESS
public:
#endif
    using PaneId = ::PaneId;
    using PaneItem = ::PaneItem;
    using PaneContext = ::PaneContext;
    PaneId m_activePane = PaneId::Primary;
    const PaneContext *m_operationContext = nullptr;
    std::unique_ptr<PaneAdapter> m_paneAdapter;

    PaneContext paneContext() const
    {
        if (m_operationContext) {
            return *m_operationContext;
        }
        return m_paneAdapter ? m_paneAdapter->context(m_activePane) : PaneContext{};
    }

    void setActivePane(PaneId pane)
    {
        m_activePane = m_paneAdapter ? m_paneAdapter->availablePane(pane) : PaneId::Primary;
        for (QWidget *widget : {static_cast<QWidget *>(m_primaryPane),
                                static_cast<QWidget *>(m_splitPane)}) {
            if (!widget) continue;
            const bool active = widget == (m_activePane == PaneId::Split
                ? static_cast<QWidget *>(m_splitPane) : m_primaryPane);
            if (widget->property("active").toBool() != active) {
                widget->setProperty("active", active);
                widget->style()->unpolish(widget);
                widget->style()->polish(widget);
                widget->update();
            }
        }
        if (m_breadcrumbFrame) {
            const bool active = m_activePane == PaneId::Primary;
            m_breadcrumbFrame->setProperty("active", active);
            m_breadcrumbFrame->style()->unpolish(m_breadcrumbFrame);
            m_breadcrumbFrame->style()->polish(m_breadcrumbFrame);
        }
        if (m_splitPane) {
            m_splitPane->setAddressActive(m_activePane == PaneId::Split);
        }
        updateSidebarCurrent();
        updateSearchControls();
        updateFileActionStates();
        updatePreviewAndQuickLook();
    }

    void setQuickLookVisible(bool visible)
    {
        if (m_previewCoordinator) m_previewCoordinator->setQuickLookVisible(visible);
    }

    void updatePreviewAndQuickLook()
    {
        if (m_previewCoordinator) m_previewCoordinator->selectionChanged();
    }

    void positionQuickLook()
    {
        if (m_previewCoordinator) m_previewCoordinator->repositionQuickLook();
    }

#ifdef THISPC_TEST_HARNESS
public:
#else
protected:
#endif
    void resizeEvent(QResizeEvent *event) override
    {
        QMainWindow::resizeEvent(event);
        positionQuickLook();
    }

#ifdef THISPC_TEST_HARNESS
public:
#else
private:
#endif

    void navigatePane(PaneId pane, const QUrl &url)
    {
        if (m_paneAdapter) m_paneAdapter->navigate(pane, url);
    }

    void refreshPane(PaneId pane)
    {
        if (m_paneAdapter) m_paneAdapter->refresh(pane);
    }

    void openInOtherPane(PaneId pane, const QUrl &url)
    {
        if (m_paneAdapter) m_paneAdapter->openInOtherPane(pane, url);
    }

    void showSelectedProperties()
    {
        if (!mutationAllowedByRecoveryGate()) return;
        const auto context = paneContext();
        if (context.items.size() != 1) return;
        const auto item = context.items.first();
        showPropertiesDialog(item.name, item.url, item.isDir,
                             item.type, item.size, item.modified);
    }


public:
    explicit ThisPcWindow(
        const QUrl &initialUrl = kThisPcUrl,
        bool allowSessionRestore = true)
    {
        setWindowTitle(trLocal("Ten komputer", "This PC"));
        setWindowIcon(QIcon::fromTheme(QStringLiteral("computer")));

        m_dateGroupingTimer.setSingleShot(true);
        connect(&m_dateGroupingTimer, &QTimer::timeout, this, [this] {
            if (m_contentStack && m_contentStack->currentWidget() == m_directoryPage) {
                renderDirectoryItems(true);
            }
            scheduleDateGroupingRefresh();
        });

        QSettings settings;
        const QByteArray geometry =
            settings.value(QStringLiteral("window/geometry")).toByteArray();

        if (!geometry.isEmpty()) {
            restoreGeometry(geometry);
        } else {
            resize(1180, 760);
        }

        // Explorer-like layout: command bar on the first row, navigation /
        // address / search directly below it.
        buildFileActions();

        m_undoController = new UndoController(
            this,
            [this](bool preserveStatusMessage) {
                refreshCurrent(preserveStatusMessage);
                if (m_splitPane && m_splitPane->isVisible()) {
                    m_splitPane->refresh();
                }
            },
            [this] { updateFileActionStates(); },
            [this](const QString &message, int timeout) {
                statusBar()->showMessage(message, timeout);
            },
            this);
        m_undoController->setActions(
            m_undoAction,
            m_redoAction);

        m_fileActions = new FileActions(this, m_undoController,
            [this](KJob *job, const QString &message, bool clearClipboard, const QString &title,
                   const FileActions::RefreshViews &refreshViews) {
                watchFileOperation(job, message, clearClipboard, title, refreshViews);
            }, [this](const QString &message) { statusBar()->showMessage(message, 0); });

        auto &recoveryGate = BatchRenameRecoveryGate::instance();
        const bool startsRecoveryScan = recoveryGate.beginStartupScan();
        m_recoveryStatusLabel = new QLabel(statusBar());
        m_recoveryStatusLabel->setObjectName(QStringLiteral("recoveryFenceStatus"));
        m_recoveryStatusLabel->setText(recoveryGate.message());
        m_recoveryStatusLabel->setToolTip(recoveryGate.message());
        m_recoveryStatusLabel->setVisible(recoveryGate.mutationsBlocked());
        statusBar()->addPermanentWidget(m_recoveryStatusLabel, 1);
        if (recoveryGate.mutationsBlocked()) {
            statusBar()->showMessage(recoveryGate.message(), 0);
        } else {
            m_recoverySuccessShown = true;
            statusBar()->showMessage(
                trLocal(
                    "Sprawdzanie odzyskiwania po awarii zakończone. Historia Cofnij/Ponów sprzed ponownego uruchomienia nie jest przywracana.",
                    "Crash recovery check completed. Undo/Redo history from before restart is not restored."),
                6000);
        }
        recordRecoveryTestPresentation();
        if (startsRecoveryScan) {
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
            if (qgetenv("THISPC_RECOVERY_STARTUP_SYNC") == QByteArrayLiteral("1")) {
                recoveryGate.completeStartupScan();
                updateRecoveryFencePresentation();
            } else {
#endif
            m_recoveryStartupWatcher = new QFutureWatcher<void>(this);
            connect(m_recoveryStartupWatcher, &QFutureWatcher<void>::finished, this, [this] {
                updateRecoveryFencePresentation();
                updateFileActionStates();
            });
            m_recoveryStartupWatcher->setFuture(QtConcurrent::run([&recoveryGate] {
                recoveryGate.completeStartupScan();
            }));
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
            }
#endif
        }
        QTimer *recoveryFenceTimer = new QTimer(this);
        recoveryFenceTimer->setInterval(1000);
        connect(recoveryFenceTimer, &QTimer::timeout, this, [this] {
            updateRecoveryFencePresentation();
            updateFileActionStates();
        });
        recoveryFenceTimer->start();

        m_searchController = new SearchController(this);
        connect(m_searchController, &SearchController::resultsChanged, this, [this] {
            if (isSearchLocation(m_navigation.currentUrl())) {
                m_primaryPane->setFiles(m_searchController->files());
                renderDirectoryItems();
            }
        });
        connect(m_searchController, &SearchController::progressChanged, this, [this] {
            updateSearchProgress();
            updateSearchStatusLabel();
        });
        connect(m_searchController, &SearchController::finished, this, [this] {
            m_searchProgressFrame->hide();
            updateSearchControls();
            if (m_activePane == PaneId::Primary)
                statusBar()->showMessage(trLocal("Wyszukiwanie zakończone", "Search completed"), 4000);
        });

        buildToolbar();
        buildCentralUi();

        m_searchUiController = std::make_unique<SearchUiController>(
            SearchUiController::Widgets{m_searchEdit, m_stopSearchAction,
                m_searchScopeAction, m_searchFilterButton, m_searchScopeGroup,
                m_searchTypeGroup, m_searchDateGroup, m_searchSizeGroup,
                m_searchProgressFrame, m_searchProgressBar, m_directoryStatus},
            SearchUiController::Presentation{
                [](const char *pl, const char *en) { return trLocal(pl, en); },
                [this](const QUrl &url) { return displayNameForLocation(url); },
                [this] { return searchScopeMenuIcon(); },
                [] { return isPolish(); }});

        connect(
            QApplication::clipboard(),
            &QClipboard::dataChanged,
            this,
            &ThisPcWindow::updateFileActionStates);

        connect(&m_driveHomeCoordinator, &DriveHomeCoordinator::loadingStarted, this, [this] {
            m_homeStatus->setText(trLocal("Odświeżanie…", "Refreshing…"));
            m_splitHomeStatus->setText(m_homeStatus->text());
        });
        connect(&m_driveHomeCoordinator, &DriveHomeCoordinator::drivesChanged, this,
            [this](const QList<DriveInfo> &) {
                rebuildDriveGrid();
                if (m_sidebar) m_sidebar->setDrives(m_driveHomeCoordinator.drives());
                rebuildBreadcrumbs();
                updateSidebarCurrent();
            });
        connect(&m_driveHomeCoordinator, &DriveHomeCoordinator::error, this, [this](const QString &) {
            m_homeStatus->setText(trLocal("Nie udało się odczytać thispc:/.", "Could not read thispc:/."));
            m_splitHomeStatus->setText(m_homeStatus->text());
        });

        m_refreshTimer.setInterval(15000);
        connect(
            &m_refreshTimer,
            &QTimer::timeout,
            this,
            &ThisPcWindow::reloadDrives);
        m_refreshTimer.start();

        reloadDrives();

        const bool restored =
            allowSessionRestore
            && m_restorePreviousSession
            && restoreSessionState();
        if (!restored) {
            createNewTab(
                initialUrl.isValid() ? initialUrl : kThisPcUrl,
                true);
        }
    }

#ifdef THISPC_TEST_HARNESS
public:
#else
protected:
#endif
    void closeEvent(QCloseEvent *event) override
    {
        if (BatchRenameRecoveryGate::instance().unjournaledSwapRunning()) {
            statusBar()->showMessage(trLocal(
                "Trwa zamiana nazw bez produkcyjnego dziennika recovery. Poczekaj na jej zakończenie przed zamknięciem okna.",
                "A name exchange without production crash recovery is in progress. Wait for completion before closing."), 0);
            event->ignore();
            return;
        }
        const int activeOperations =
            m_operationManager ? m_operationManager->activeCount() : 0;
        if (activeOperations > 0) {
            const QString question =
                isPolish()
                    ? QStringLiteral(
                        "Trwa %1 operacji plikowych. Zamknięcie aplikacji przerwie aktywne zadania.\n\nCzy na pewno zamknąć?")
                        .arg(activeOperations)
                    : QStringLiteral(
                        "%1 file operations are still running. Closing the application will stop active tasks.\n\nClose anyway?")
                        .arg(activeOperations);

            if (QMessageBox::question(
                    this,
                    trLocal("Aktywne operacje", "Active operations"),
                    question,
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No)
                != QMessageBox::Yes) {
                event->ignore();
                return;
            }

            if (m_operationManager) {
                m_operationManager->cancelAll();
            }
        }

        saveSessionState();

        QSettings settings;
        settings.setValue(
            QStringLiteral("window/geometry"),
            saveGeometry());
        if (m_contentSplitter) {
            settings.setValue(
                QStringLiteral("split/state"),
                m_splitPane->isHidden() ? m_splitterState : m_contentSplitter->saveState());
        }

        QMainWindow::closeEvent(event);
    }

#ifdef THISPC_TEST_HARNESS
public:
#else
private:
#endif
    void buildToolbar()
    {
        // The command bar is built first. Force navigation/address/search onto
        // the row below, matching Windows 11 Explorer.
        addToolBarBreak(Qt::TopToolBarArea);

        auto *toolbar = addToolBar(trLocal("Nawigacja", "Navigation"));
        toolbar->setObjectName(QStringLiteral("navigationToolbar"));
        toolbar->setMovable(false);
        toolbar->setFloatable(false);
        toolbar->setIconSize(QSize(20, 20));
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

        m_backAction = toolbar->addAction(
            themedIcon(QStringLiteral("go-previous")),
            trLocal("Wstecz", "Back"));
        m_backAction->setShortcut(QKeySequence::Back);
        connect(m_backAction, &QAction::triggered, this, [this] { if (paneContext().id == PaneId::Split) m_splitPane->navigateBack(); else goBack(); });

        m_forwardAction = toolbar->addAction(
            themedIcon(QStringLiteral("go-next")),
            trLocal("Dalej", "Forward"));
        m_forwardAction->setShortcut(QKeySequence::Forward);
        connect(
            m_forwardAction,
            &QAction::triggered,
            this,
            [this] { if (paneContext().id == PaneId::Split) m_splitPane->navigateForward(); else goForward(); });

        m_upAction = toolbar->addAction(
            themedIcon(QStringLiteral("go-up")),
            trLocal("W górę", "Up"));
        m_upAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
        connect(m_upAction, &QAction::triggered, this, [this] { if (paneContext().id == PaneId::Split) m_splitPane->navigateUp(); else goUp(); });

        m_refreshAction = toolbar->addAction(
            themedIcon(QStringLiteral("view-refresh")),
            trLocal("Odśwież", "Refresh"));
        m_refreshAction->setShortcut(QKeySequence::Refresh);
        connect(
            m_refreshAction,
            &QAction::triggered,
            this,
            [this] { refreshPane(paneContext().id); });

        m_openDolphinAction = toolbar->addAction(
            themedIcon(QStringLiteral("system-file-manager")),
            trLocal("Otwórz w Dolphinie", "Open in Dolphin"));
        connect(
            m_openDolphinAction,
            &QAction::triggered,
            this,
            [this] {
                openInDolphin(paneContext().directory);
            });

        m_addressStack = new QStackedWidget(toolbar);
        m_addressStack->setSizePolicy(
            QSizePolicy::Ignored,
            QSizePolicy::Preferred);

        m_breadcrumbFrame = new BreadcrumbFrame(m_addressStack);
        m_breadcrumbFrame->setObjectName(QStringLiteral("breadcrumbFrame"));
        m_breadcrumbFrame->setCursor(Qt::IBeamCursor);
        m_breadcrumbFrame->setToolTip(
            trLocal(
                "Kliknij bieżący segment ścieżki lub użyj Ctrl+L, aby wpisać adres",
                "Click the current path segment or press Ctrl+L to type an address"));

        connect(
            m_breadcrumbFrame,
            &BreadcrumbFrame::clicked,
            this,
            [this] {
                setActivePane(PaneId::Primary);
                beginAddressEdit(PaneId::Primary);
            });
        m_breadcrumbLayout = new QHBoxLayout(m_breadcrumbFrame->contentsWidget());
        m_breadcrumbLayout->setContentsMargins(0, 0, 0, 0);
        m_breadcrumbLayout->setSpacing(1);
        m_breadcrumbLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);

        m_addressEdit = new AddressLineEdit(m_addressStack);
        m_addressEdit->setClearButtonEnabled(true);
        m_addressEdit->setPlaceholderText(
            trLocal("Wpisz ścieżkę lub adres", "Enter path or address"));
        m_addressEdit->setToolTip(
            trLocal("Ctrl+L — edytuj bieżący adres", "Ctrl+L — edit current address"));

        m_addressStack->addWidget(m_breadcrumbFrame);
        m_addressStack->addWidget(m_addressEdit);
        // The address control is created here with the navigation actions but
        // is placed above the primary pane in buildCentralUi(). In Split View
        // each pane therefore owns an equally wide part of the address row.

        auto *navigationSpacer = new QWidget(toolbar);
        navigationSpacer->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Preferred);
        toolbar->addWidget(navigationSpacer);

        m_searchEdit = new QLineEdit(toolbar);
        m_searchEdit->setObjectName(QStringLiteral("searchEdit"));
        m_searchEdit->setClearButtonEnabled(true);
        m_searchEdit->setPlaceholderText(
            trLocal("Szukaj w tym folderze", "Search this folder"));
        m_searchEdit->setFixedWidth(255);

        // 0.13.1: the search scope lives under the magnifier inside the
        // search box instead of consuming space between address and search.
        m_searchScopeMenu = new QMenu(m_searchEdit);
        m_searchScopeMenu->addSection(
            trLocal(
                "Zakres wyszukiwania",
                "Search scope"));

        m_searchScopeGroup =
            new QActionGroup(m_searchScopeMenu);
        m_searchScopeGroup->setExclusive(true);

        struct SearchScopeDef {
            int value;
            const char *pl;
            const char *en;
            const char *icon;
        };

        const SearchScopeDef scopes[] = {
            {0, "Bieżący folder", "Current folder", "folder"},
            {1, "Bieżący dysk", "Current drive", "drive-harddisk"},
            {2, "Ten komputer", "This PC", "computer"},
        };

        for (const SearchScopeDef &def : scopes) {
            QAction *action = m_searchScopeMenu->addAction(
                themedIcon(QString::fromLatin1(def.icon)),
                trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.value);
            m_searchScopeGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, value = def.value] {
                    activeSearchState().scope = value;
                    updateSearchControls();
                });
        }

        m_searchScopeAction =
            m_searchEdit->addAction(
                searchScopeMenuIcon(),
                QLineEdit::LeadingPosition);

        connect(
            m_searchScopeAction,
            &QAction::triggered,
            this,
            [this] {
                if (!m_searchScopeMenu
                    || !m_searchScopeAction->isEnabled()) {
                    return;
                }

                const QPoint popupPoint =
                    m_searchEdit->mapToGlobal(
                        QPoint(0, m_searchEdit->height()));
                m_searchScopeMenu->popup(popupPoint);
            });

        m_stopSearchAction =
            m_searchEdit->addAction(
                themedIcon(QStringLiteral("process-stop")),
                QLineEdit::TrailingPosition);
        m_stopSearchAction->setToolTip(
            trLocal(
                "Przerwij wyszukiwanie",
                "Stop search"));
        m_stopSearchAction->setVisible(false);

        connect(
            m_stopSearchAction,
            &QAction::triggered,
            this,
            [this] {
                cancelSearch(m_activePane, true);
            });

        toolbar->addWidget(m_searchEdit);

        m_searchFilterButton = new QToolButton(toolbar);
        m_searchFilterButton->setObjectName(
            QStringLiteral("searchFilterButton"));
        m_searchFilterButton->setPopupMode(
            QToolButton::InstantPopup);
        m_searchFilterButton->setToolButtonStyle(
            Qt::ToolButtonIconOnly);
        m_searchFilterButton->setIcon(
            themedIcon(QStringLiteral("view-filter")));
        m_searchFilterButton->setToolTip(
            trLocal("Filtry wyszukiwania", "Search filters"));

        auto *filterMenu = new QMenu(m_searchFilterButton);

        QMenu *typeMenu =
            filterMenu->addMenu(
                trLocal("Typ", "Type"));
        m_searchTypeGroup = new QActionGroup(typeMenu);
        m_searchTypeGroup->setExclusive(true);

        struct SearchFilterDef {
            int value;
            const char *pl;
            const char *en;
        };

        const SearchFilterDef types[] = {
            {0, "Wszystkie", "All"},
            {1, "Foldery", "Folders"},
            {2, "Obrazy", "Images"},
            {3, "Dokumenty", "Documents"},
            {4, "Muzyka / audio", "Music / audio"},
            {5, "Wideo", "Video"},
            {6, "Archiwa", "Archives"},
        };

        for (const SearchFilterDef &def : types) {
            QAction *action =
                typeMenu->addAction(
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.value);
            m_searchTypeGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, value = def.value] {
                    activeSearchState().type = value;
                    syncCurrentSearchFilters();
                    updateSearchControls();
                    renderActiveSearchItems();
                });
        }

        QMenu *dateMenu =
            filterMenu->addMenu(
                trLocal("Data modyfikacji", "Date modified"));
        m_searchDateGroup = new QActionGroup(dateMenu);
        m_searchDateGroup->setExclusive(true);

        const SearchFilterDef dates[] = {
            {0, "Dowolna", "Any time"},
            {1, "Ostatnie 24 godziny", "Last 24 hours"},
            {2, "Ostatnie 7 dni", "Last 7 days"},
            {3, "Ostatnie 30 dni", "Last 30 days"},
            {4, "Ostatni rok", "Last year"},
        };

        for (const SearchFilterDef &def : dates) {
            QAction *action =
                dateMenu->addAction(
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.value);
            m_searchDateGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, value = def.value] {
                    activeSearchState().date = value;
                    syncCurrentSearchFilters();
                    updateSearchControls();
                    renderActiveSearchItems();
                });
        }

        QMenu *sizeMenu =
            filterMenu->addMenu(
                trLocal("Rozmiar", "Size"));
        m_searchSizeGroup = new QActionGroup(sizeMenu);
        m_searchSizeGroup->setExclusive(true);

        const SearchFilterDef sizes[] = {
            {0, "Dowolny", "Any size"},
            {1, "Mniejszy niż 1 MiB", "Smaller than 1 MiB"},
            {2, "1–100 MiB", "1–100 MiB"},
            {3, "100 MiB–1 GiB", "100 MiB–1 GiB"},
            {4, "Większy niż 1 GiB", "Larger than 1 GiB"},
        };

        for (const SearchFilterDef &def : sizes) {
            QAction *action =
                sizeMenu->addAction(
                    trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setData(def.value);
            m_searchSizeGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, value = def.value] {
                    activeSearchState().size = value;
                    syncCurrentSearchFilters();
                    updateSearchControls();
                    renderActiveSearchItems();
                });
        }

        filterMenu->addSeparator();

        QAction *clearFilters =
            filterMenu->addAction(
                themedIcon(QStringLiteral("edit-clear")),
                trLocal("Wyczyść filtry", "Clear filters"));

        connect(
            clearFilters,
            &QAction::triggered,
            this,
            [this] {
                activeSearchState().type = 0;
                activeSearchState().date = 0;
                activeSearchState().size = 0;
                syncCurrentSearchFilters();
                updateSearchControls();
                renderActiveSearchItems();
            });

        m_searchFilterButton->setMenu(filterMenu);
        toolbar->addWidget(m_searchFilterButton);

        connect(
            m_searchEdit,
            &QLineEdit::textChanged,
            this,
            [this](const QString &text) {
                activeSearchState().text = text;
                if (!isSearchLocation(activeLocation())) renderActiveSearchItems();
            });

        connect(
            m_searchEdit,
            &QLineEdit::returnPressed,
            this,
            &ThisPcWindow::startSearchFromUi);

        auto *findAction = new QAction(this);
        findAction->setShortcut(QKeySequence::Find);
        addAction(findAction);
        connect(
            findAction,
            &QAction::triggered,
            this,
            [this] {
                if (!m_searchEdit) {
                    return;
                }

                m_searchEdit->setFocus(
                    Qt::ShortcutFocusReason);
                m_searchEdit->selectAll();
            });

        auto *locationAction = new QAction(this);
        locationAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
        addAction(locationAction);

        connect(
            locationAction,
            &QAction::triggered,
            this,
            [this] { beginAddressEdit(m_activePane); });

        connect(
            m_addressEdit,
            &QLineEdit::returnPressed,
            this,
            [this] {
                const QUrl target =
                    urlFromUserText(m_addressEdit->text());

                m_addressStack->setCurrentWidget(m_breadcrumbFrame);

                if (target.isValid()) {
                    navigatePane(PaneId::Primary, target);
                }
            });

        connect(
            m_addressEdit,
            &AddressLineEdit::canceled,
            this,
            [this] {
                m_addressStack->setCurrentWidget(m_breadcrumbFrame);
                setFocus();
            });

        connect(
            m_addressEdit,
            &AddressLineEdit::focusLeft,
            this,
            [this] {
                // Defer until Qt has completed the focus transition. This
                // avoids fighting with clicks on the search box or toolbar.
                QTimer::singleShot(
                    0,
                    this,
                    [this] {
                        if (m_addressEdit
                            && !m_addressEdit->hasFocus()
                            && m_addressStack->currentWidget()
                                == m_addressEdit) {
                            m_addressStack->setCurrentWidget(
                                m_breadcrumbFrame);
                        }
                    });
            });

        updateSearchControls();
    }

    void beginAddressEdit(PaneId pane)
    {
        if (pane == PaneId::Split
            && m_splitPane
            && m_splitPane->isVisible()) {
            setActivePane(PaneId::Split);
            m_splitPane->beginAddressEdit();
            return;
        }

        if (!m_addressEdit || !m_addressStack) {
            return;
        }

        setActivePane(PaneId::Primary);
        m_addressEdit->setText(
            urlForDisplay(m_navigation.currentUrl()));
        m_addressStack->setCurrentWidget(
            m_addressEdit);
        m_addressEdit->setFocus(
            Qt::MouseFocusReason);
        m_addressEdit->selectAll();
    }

    void buildFileActions()
    {
        // First toolbar row: file commands. Navigation/address/search are
        // created afterwards on the row below.
        auto *toolbar =
            addToolBar(trLocal("Pliki", "Files"));

        toolbar->setObjectName(
            QStringLiteral("fileCommandToolbar"));
        addToolBar(Qt::TopToolBarArea, toolbar);
        toolbar->setMovable(false);
        toolbar->setFloatable(false);
        toolbar->setAllowedAreas(Qt::TopToolBarArea);
        toolbar->setIconSize(QSize(18, 18));
        toolbar->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);

        m_newButton = new QToolButton(toolbar);
        m_newButton->setText(trLocal("Nowy", "New"));
        m_newButton->setIcon(
            themedIcon(QStringLiteral("list-add"), QStringLiteral("folder-new")));
        m_newButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_newButton->setPopupMode(
            QToolButton::InstantPopup);

        auto *newMenu = new QMenu(m_newButton);

        m_newFolderAction = newMenu->addAction(
            themedIcon(QStringLiteral("folder-new")),
            trLocal("Folder", "Folder"));
        connect(
            m_newFolderAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::createNewFolder);

        m_newTextFileAction = newMenu->addAction(
            themedIcon(QStringLiteral("text-plain"), QStringLiteral("text-x-generic")),
            trLocal("Dokument tekstowy", "Text document"));
        connect(
            m_newTextFileAction,
            &QAction::triggered,
            this,
            [this] {
                createNewFile(
                    trLocal("Nowy dokument.txt", "New document.txt"),
                    QByteArray());
            });

        m_newMarkdownAction = newMenu->addAction(
            themedIcon(QStringLiteral("text-markdown"), QStringLiteral("text-x-generic")),
            trLocal("Dokument Markdown", "Markdown document"));
        connect(
            m_newMarkdownAction,
            &QAction::triggered,
            this,
            [this] {
                createNewFile(
                    trLocal("Nowy dokument.md", "New document.md"),
                    QByteArray());
            });

        m_newEmptyFileAction = newMenu->addAction(
            themedIcon(QStringLiteral("document-new")),
            trLocal("Pusty plik…", "Empty file…"));
        connect(
            m_newEmptyFileAction,
            &QAction::triggered,
            this,
            [this] {
                createNewFile(QString(), QByteArray());
            });

        newMenu->addSeparator();
        m_templateMenu = new TemplateMenu(newMenu);
        newMenu->addMenu(m_templateMenu);
        connect(m_templateMenu, &TemplateMenu::templateSelected,
                this, &ThisPcWindow::createFromTemplate);

        m_newButton->setMenu(newMenu);
        toolbar->addWidget(m_newButton);

        toolbar->addSeparator();

        m_cutAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-cut")),
            trLocal("Wytnij", "Cut"));
        connect(
            m_cutAction,
            &QAction::triggered,
            this,
            [this] {
                putSelectionOnClipboard(true);
            });

        m_copyAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-copy")),
            trLocal("Kopiuj", "Copy"));
        connect(
            m_copyAction,
            &QAction::triggered,
            this,
            [this] {
                putSelectionOnClipboard(false);
            });

        m_pasteAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-paste")),
            trLocal("Wklej", "Paste"));
        connect(
            m_pasteAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::pasteClipboard);

        for (QAction *action : {m_cutAction, m_copyAction, m_pasteAction}) {
            if (auto *button =
                    qobject_cast<QToolButton *>(toolbar->widgetForAction(action))) {
                button->setToolButtonStyle(Qt::ToolButtonIconOnly);
                button->setToolTip(action->text());
            }
        }

        m_trashAction = toolbar->addAction(
            themedIcon(QStringLiteral("user-trash")),
            trLocal("Do Kosza", "Trash"));
        connect(
            m_trashAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::trashSelected);

        if (auto *button =
                qobject_cast<QToolButton *>(
                    toolbar->widgetForAction(m_trashAction))) {
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setToolTip(m_trashAction->text());
        }

        m_emptyTrashAction = toolbar->addAction(
            themedIcon(QStringLiteral("trash-empty"), QStringLiteral("user-trash")),
            trLocal("Opróżnij kosz", "Empty Trash"));
        connect(
            m_emptyTrashAction,
            &QAction::triggered,
            this,
            [this] { emptyTrashAt(paneContext().directory); });

        if (auto *button =
                qobject_cast<QToolButton *>(
                    toolbar->widgetForAction(m_emptyTrashAction))) {
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setToolTip(m_emptyTrashAction->text());
        }

        toolbar->addSeparator();

        m_renameAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-rename")),
            trLocal("Zmień nazwę", "Rename"));
        connect(
            m_renameAction,
            &QAction::triggered,
            this,
            &ThisPcWindow::renameSelected);

        m_batchRenameAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-rename")),
            trLocal("Zmień nazwy zbiorczo…", "Batch Rename…"));
        connect(m_batchRenameAction, &QAction::triggered,
                this, &ThisPcWindow::batchRenameSelected);

        toolbar->addSeparator();

        m_propertiesAction = toolbar->addAction(
            themedIcon(QStringLiteral("document-properties")),
            trLocal("Właściwości", "Properties"));
        connect(m_propertiesAction, &QAction::triggered,
                this, &ThisPcWindow::showSelectedProperties);

        m_undoAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-undo")),
            trLocal("Cofnij", "Undo"));
        m_undoAction->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Z")));
        connect(
            m_undoAction,
            &QAction::triggered,
            this,
            [this] {
                if (m_undoController) {
                    m_undoController->undo();
                }
            });

        m_redoAction = toolbar->addAction(
            themedIcon(QStringLiteral("edit-redo")),
            trLocal("Ponów", "Redo"));
        m_redoAction->setShortcuts(
            {QKeySequence(QStringLiteral("Ctrl+Y")),
             QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
        connect(
            m_redoAction,
            &QAction::triggered,
            this,
            [this] {
                if (m_undoController) {
                    m_undoController->redo();
                }
            });

        for (QAction *action : {m_undoAction, m_redoAction}) {
            if (auto *button =
                    qobject_cast<QToolButton *>(toolbar->widgetForAction(action))) {
                button->setToolButtonStyle(Qt::ToolButtonIconOnly);
                button->setToolTip(action->text());
            }
        }

        toolbar->addSeparator();

        QSettings settings;
        m_directoryViewMode =
            std::clamp(
                settings.value(
                    QStringLiteral("directory/viewMode"),
                    0).toInt(),
                0,
                3);

        m_directoryIconSizeMode = std::clamp(
            settings.value(
                QStringLiteral("directory/iconSizeMode"),
                DirectoryViewSettings::DefaultIconSizeMode).toInt(),
            0,
            3);

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

        m_alwaysShowFullNames =
            settings.value(
                QStringLiteral("directory/alwaysShowFullNames"),
                false).toBool();

        m_restorePreviousSession =
            SessionManager::restorePreviousSessionEnabled();

        m_viewButton = new QToolButton(toolbar);
        m_viewButton->setText(trLocal("Widok", "View"));
        m_viewButton->setIcon(
            themedIcon(QStringLiteral("view-list-icons")));
        m_viewButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_viewButton->setPopupMode(
            QToolButton::InstantPopup);

        auto *viewMenu = new QMenu(m_viewButton);
        viewMenu->setObjectName(QStringLiteral("viewMenu"));
        auto *viewGroup = new QActionGroup(viewMenu);
        viewGroup->setExclusive(true);

        struct ViewDef {
            int mode;
            const char *pl;
            const char *en;
            const char *icon;
        };

        const ViewDef views[] = {
            {0, "Ikony", "Icons", "view-list-icons"},
            {1, "Lista", "List", "view-list-text"},
            {2, "Szczegóły", "Details", "view-list-details"},
            {3, "Kompaktowy", "Compact", "view-list-tree"},
        };

        for (const ViewDef &def : views) {
            QAction *action = viewMenu->addAction(
                themedIcon(QString::fromLatin1(def.icon)),
                trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setChecked(def.mode == m_directoryViewMode);
            action->setData(def.mode);
            action->setObjectName(
                QStringLiteral("viewModeAction%1").arg(def.mode));
            viewGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, mode = def.mode] {
                    if (paneContext().id == PaneId::Split) m_splitPane->setViewMode(mode);
                    else setDirectoryViewMode(mode);
                });
        }

        QMenu *iconSizeMenu = viewMenu->addMenu(
            themedIcon(QStringLiteral("transform-scale"), QStringLiteral("view-list-icons")),
            trLocal("Rozmiar ikon", "Icon size"));
        iconSizeMenu->setObjectName(QStringLiteral("viewIconSizeMenu"));
        auto *iconSizeGroup = new QActionGroup(iconSizeMenu);
        iconSizeGroup->setExclusive(true);
        struct IconSizeDef { int mode; const char *pl; const char *en; };
        const IconSizeDef iconSizes[] = {
            {0, "Bardzo duże", "Very large"},
            {1, "Duże", "Large"},
            {2, "Średnie", "Medium"},
            {3, "Małe", "Small"},
        };
        for (const IconSizeDef &def : iconSizes) {
            QAction *action = iconSizeMenu->addAction(trLocal(def.pl, def.en));
            action->setObjectName(QStringLiteral("iconSizeAction%1").arg(def.mode));
            action->setCheckable(true);
            action->setChecked(def.mode == m_directoryIconSizeMode);
            action->setProperty("iconSizeMode", def.mode);
            iconSizeGroup->addAction(action);
            connect(action, &QAction::triggered, this, [this, mode = def.mode] {
                if (paneContext().id == PaneId::Split) m_splitPane->setIconSizeMode(mode);
                else setDirectoryIconSizeMode(mode);
            });
        }

        viewMenu->addSeparator();

        QMenu *showMenu = viewMenu->addMenu(
            themedIcon(QStringLiteral("view-visible"), QStringLiteral("view-preview")),
            trLocal("Pokaż", "Show"));
        showMenu->setObjectName(QStringLiteral("viewShowMenu"));

        m_showHiddenAction = showMenu->addAction(
            themedIcon(QStringLiteral("view-hidden")),
            trLocal("Ukryte elementy", "Hidden items"));
        m_showHiddenAction->setCheckable(true);
        m_showHiddenAction->setChecked(m_showHiddenFiles);
        m_showHiddenAction->setShortcut(
            QKeySequence(Qt::CTRL | Qt::Key_H));
        addAction(m_showHiddenAction);
        connect(
            m_showHiddenAction,
            &QAction::toggled,
            this,
            &ThisPcWindow::setShowHiddenFiles);

        m_thumbnailsAction = showMenu->addAction(
            themedIcon(QStringLiteral("view-preview")),
            trLocal("Miniatury obrazów", "Image thumbnails"));
        m_thumbnailsAction->setCheckable(true);
        m_thumbnailsAction->setChecked(m_thumbnailsEnabled);
        connect(
            m_thumbnailsAction,
            &QAction::toggled,
            this,
            &ThisPcWindow::setThumbnailsEnabled);

        m_previewAction = showMenu->addAction(
            themedIcon(QStringLiteral("document-preview"), QStringLiteral("view-preview")),
            trLocal("Panel podglądu", "Preview pane"));
        m_previewAction->setCheckable(true);
        m_previewAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_P));
        addAction(m_previewAction);
        connect(m_previewAction, &QAction::toggled, this, [this](bool enabled) {
            if (m_previewCoordinator) m_previewCoordinator->setPreviewVisible(enabled);
        });

        m_fullNamesAction = showMenu->addAction(
            themedIcon(
                QStringLiteral("format-text-underline"),
                QStringLiteral("view-list-text")),
            trLocal("Pełne nazwy", "Full names"));
        m_fullNamesAction->setCheckable(true);
        m_fullNamesAction->setChecked(
            m_alwaysShowFullNames);
        m_fullNamesAction->setToolTip(
            trLocal(
                "Zawsze pokazuj pełne nazwy plików i folderów. Bez tej opcji pełna nazwa rozwija się po zaznaczeniu.",
                "Always show full file and folder names. When disabled, a selected item expands to show its full name."));
        connect(
            m_fullNamesAction,
            &QAction::toggled,
            this,
            &ThisPcWindow::setAlwaysShowFullNames);

        viewMenu->addSeparator();

        m_restoreSessionAction = viewMenu->addAction(
            themedIcon(QStringLiteral("document-open-recent")),
            trLocal(
                "Przywracaj poprzednią sesję",
                "Restore previous session"));
        m_restoreSessionAction->setCheckable(true);
        m_restoreSessionAction->setChecked(
            m_restorePreviousSession);
        connect(
            m_restoreSessionAction,
            &QAction::toggled,
            this,
            [this](bool enabled) {
                m_restorePreviousSession = enabled;
                SessionManager::setRestorePreviousSessionEnabled(enabled);
            });

        m_viewButton->setMenu(viewMenu);
        toolbar->addWidget(m_viewButton);

        m_sortButton = new QToolButton(toolbar);
        m_sortButton->setText(trLocal("Sortuj", "Sort"));
        m_sortButton->setIcon(
            themedIcon(QStringLiteral("view-sort-ascending")));
        m_sortButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_sortButton->setPopupMode(
            QToolButton::InstantPopup);

        auto *sortMenu = new QMenu(m_sortButton);
        auto *sortGroup = new QActionGroup(sortMenu);
        sortGroup->setExclusive(true);

        struct SortDef {
            int key;
            const char *pl;
            const char *en;
        };

        const SortDef sorts[] = {
            {0, "Nazwa", "Name"},
            {1, "Typ", "Type"},
            {2, "Rozmiar", "Size"},
            {3, "Data modyfikacji", "Date modified"},
        };

        for (const SortDef &def : sorts) {
            QAction *action = sortMenu->addAction(
                trLocal(def.pl, def.en));
            action->setCheckable(true);
            action->setChecked(def.key == m_sortKey);
            action->setData(def.key);
            sortGroup->addAction(action);

            connect(
                action,
                &QAction::triggered,
                this,
                [this, key = def.key] {
                    if (paneContext().id == PaneId::Split) m_splitPane->setSortState(key, m_splitPane->sortAscending());
                    else setSortKey(key);
                });
        }

        sortMenu->addSeparator();

        auto *directionGroup =
            new QActionGroup(sortMenu);
        directionGroup->setExclusive(true);

        QAction *ascending =
            sortMenu->addAction(
                themedIcon(QStringLiteral("view-sort-ascending")),
                trLocal("Rosnąco", "Ascending"));
        ascending->setCheckable(true);
        ascending->setChecked(m_sortAscending);
        directionGroup->addAction(ascending);

        QAction *descending =
            sortMenu->addAction(
                themedIcon(QStringLiteral("view-sort-descending")),
                trLocal("Malejąco", "Descending"));
        descending->setCheckable(true);
        descending->setChecked(!m_sortAscending);
        directionGroup->addAction(descending);

        connect(
            ascending,
            &QAction::triggered,
            this,
            [this] {
                if (paneContext().id == PaneId::Split) m_splitPane->setSortState(m_splitPane->sortKey(), true);
                else setSortAscending(true);
            });

        connect(
            descending,
            &QAction::triggered,
            this,
            [this] {
                if (paneContext().id == PaneId::Split) m_splitPane->setSortState(m_splitPane->sortKey(), false);
                else setSortAscending(false);
            });

        sortMenu->addSeparator();
        QMenu *groupMenu = sortMenu->addMenu(trLocal("Grupuj według", "Group by"));
        auto *groupActions = new QActionGroup(groupMenu);
        groupActions->setExclusive(true);
        for (const auto &entry : std::initializer_list<std::pair<int, QString>>{
                 {DirectoryViewSettings::NoGrouping, trLocal("Brak", "None")},
                 {DirectoryViewSettings::GroupByType, trLocal("Typ", "Type")},
                 {DirectoryViewSettings::GroupByDate, trLocal("Data modyfikacji", "Date modified")},
                 {DirectoryViewSettings::GroupBySize, trLocal("Rozmiar", "Size")}}) {
            QAction *action = groupMenu->addAction(entry.second);
            action->setCheckable(true);
            action->setData(entry.first);
            groupActions->addAction(action);
            connect(action, &QAction::triggered, this, [this, mode = entry.first] {
                if (paneContext().id == PaneId::Split) m_splitPane->setGroupMode(mode);
                else setGroupMode(mode);
            });
        }
        connect(groupMenu, &QMenu::aboutToShow, this, [this, groupActions] {
            const int mode = paneContext().id == PaneId::Split
                ? m_splitPane->groupMode() : m_groupMode;
            for (QAction *action : groupActions->actions())
                action->setChecked(action->data().toInt() == mode);
        });

        m_sortButton->setMenu(sortMenu);
        toolbar->addWidget(m_sortButton);

        toolbar->addSeparator();

        toolbar->addAction(m_fullNamesAction);

        toolbar->addSeparator();

        m_splitViewAction = toolbar->addAction(
            themedIcon(QStringLiteral("view-split-left-right"), QStringLiteral("view-list-details")),
            trLocal("Podziel widok", "Split view"));
        m_splitViewAction->setCheckable(true);
        m_splitViewAction->setShortcut(QKeySequence(Qt::Key_F3));
        m_splitViewAction->setToolTip(
            trLocal("Podziel widok na dwa panele (F3)", "Split the view into two panes (F3)"));
        connect(
            m_splitViewAction,
            &QAction::toggled,
            this,
            &ThisPcWindow::setSplitViewEnabled);

        m_swapPanesAction = toolbar->addAction(
            themedIcon(QStringLiteral("object-flip-horizontal"),
                       QStringLiteral("transform-move")),
            trLocal("Zamień panele", "Swap panes"));
        m_swapPanesAction->setToolTip(trLocal(
            "Zamień lokalizacje lewego i prawego panelu",
            "Swap the locations of the left and right panes"));
        m_swapPanesAction->setVisible(false);
        connect(m_swapPanesAction, &QAction::triggered,
                this, &ThisPcWindow::swapSplitPanes);

        m_comparePanesAction = toolbar->addAction(
            themedIcon(QStringLiteral("view-split-left-right"),
                       QStringLiteral("document-compare")),
            trLocal("Porównaj panele", "Compare panels"));
        m_comparePanesAction->setToolTip(trLocal(
            "Porównaj zawartość lewego i prawego panelu",
            "Compare contents of the left and right panes"));
        m_comparePanesAction->setVisible(false);
        connect(m_comparePanesAction, &QAction::triggered,
                this, &ThisPcWindow::compareSplitPanes);

        // 0.15.1: compact operation history lives at the far-right edge of
        // the command bar and opens as a Brave-like popup.
        buildOperationManager(toolbar);

        // Keyboard shortcuts are scoped to the directory page so Ctrl+C/X/V,
        // F2 and Delete do not steal keystrokes while editing Ctrl+L.
        updateFileActionStates();
    }

    void buildCentralUi()
    {
        auto *central = new QWidget(this);
        auto *centralLayout = new QHBoxLayout(central);
        centralLayout->setContentsMargins(0, 0, 0, 0);
        centralLayout->setSpacing(0);
        setCentralWidget(central);

        m_quickLook = new QuickLookOverlay(central);
        connect(m_quickLook->closeButton(), &QToolButton::clicked,
                this, [this] { setQuickLookVisible(false); });

        m_sidebarSplitter = new QSplitter(Qt::Horizontal, central);
        m_sidebarSplitter->setObjectName(QStringLiteral("sidebarSplitter"));
        m_sidebarSplitter->setChildrenCollapsible(false);
        m_sidebarSplitter->setHandleWidth(5);

        m_sidebarScrollArea = new QScrollArea(m_sidebarSplitter);
        m_sidebarScrollArea->setObjectName(QStringLiteral("sidebarScrollArea"));
        m_sidebarScrollArea->setWidgetResizable(true);
        m_sidebarScrollArea->setFrameShape(QFrame::NoFrame);
        m_sidebarScrollArea->setSizeAdjustPolicy(
            QAbstractScrollArea::AdjustIgnored);
        m_sidebarScrollArea->setHorizontalScrollBarPolicy(
            Qt::ScrollBarAlwaysOff);
        m_sidebarScrollArea->setVerticalScrollBarPolicy(
            Qt::ScrollBarAsNeeded);
        m_sidebarScrollArea->setMinimumWidth(205);
        m_sidebarScrollArea->setMaximumWidth(480);
        m_sidebarScrollArea->setSizePolicy(
            QSizePolicy::Preferred, QSizePolicy::Expanding);

        m_sidebar = new SidebarPanel(m_sidebarScrollArea);
        m_sidebarScrollArea->setWidget(m_sidebar);
        connect(
            m_sidebar,
            &SidebarPanel::activated,
            this,
            [this](const QUrl &url) {
                navigatePane(m_activePane, url);
            });
        connect(
            m_sidebar,
            &SidebarPanel::openInNewTabRequested,
            this,
            [this](const QUrl &url, bool makeCurrent) {
                createNewTab(url, makeCurrent);
            });
        connect(
            m_sidebar,
            &SidebarPanel::openInNewWindowRequested,
            this,
            &ThisPcWindow::openInNewWindow);
        connect(
            m_sidebar,
            &SidebarPanel::openInSplitPaneRequested,
            this,
            [this](const QUrl &url) {
                openInOtherPane(m_activePane, url);
            });
        connect(
            m_sidebar,
            &SidebarPanel::openInDolphinRequested,
            this,
            [](const QUrl &url) {
                openInDolphin(url);
            });
        connect(
            m_sidebar,
            &SidebarPanel::statusMessageRequested,
            this,
            [this](const QString &message, int timeoutMs) {
                statusBar()->showMessage(message, timeoutMs);
            });
        connect(m_sidebar, &SidebarPanel::urlsDropped,
                this, &ThisPcWindow::handleDroppedUrls);
        connect(m_sidebarScrollArea->verticalScrollBar(),
                &QScrollBar::valueChanged,
                m_sidebar, &SidebarPanel::clearDropFeedback);

        m_sidebarSplitter->addWidget(m_sidebarScrollArea);

        auto *rightPane = new QWidget(central);
        auto *rightLayout = new QVBoxLayout(rightPane);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);

        m_tabStrip = new QFrame(rightPane);
        m_tabStrip->setObjectName(QStringLiteral("tabStrip"));
        auto *tabLayout = new QHBoxLayout(m_tabStrip);
        tabLayout->setContentsMargins(4, 0, 2, 0);
        tabLayout->setSpacing(0);

        m_tabBar = new ExplorerTabBar(m_tabStrip);
        m_tabBar->setDropDirectoryResolver([this](int index) {
            return tabDropDirectory(index);
        });
        connect(m_tabBar, &ExplorerTabBar::urlsDropped,
                this, &ThisPcWindow::handleDroppedUrls);
        m_tabBar->setObjectName(QStringLiteral("explorerTabs"));
        m_tabBar->setTabsClosable(true);
        m_tabBar->setMovable(true);
        m_tabBar->setDocumentMode(true);
        m_tabBar->setElideMode(Qt::ElideRight);
        m_tabBar->setExpanding(false);
        m_tabBar->setUsesScrollButtons(true);
        m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
        tabLayout->addWidget(m_tabBar, 1);

        m_newTabButton = new QToolButton(m_tabStrip);
        m_newTabButton->setObjectName(QStringLiteral("newTabButton"));
        m_newTabButton->setIcon(themedIcon(QStringLiteral("list-add")));
        m_newTabButton->setToolTip(trLocal(
            "Nowa karta (Ctrl+T)",
            "New tab (Ctrl+T)"));
        tabLayout->addWidget(m_newTabButton, 0, Qt::AlignTop);

        connect(
            m_newTabButton,
            &QToolButton::clicked,
            this,
            [this] { createNewTab(kThisPcUrl, true); });

        connect(
            m_tabBar,
            &QTabBar::currentChanged,
            this,
            [this](int index) {
                if (!m_tabChangeInProgress) {
                    switchToTab(index);
                }
            });

        connect(
            m_tabBar,
            &QTabBar::tabCloseRequested,
            this,
            &ThisPcWindow::closeTab);

        connect(
            m_tabBar,
            &QTabBar::tabMoved,
            this,
            [this](int from, int to) {
                if (from < 0 || to < 0
                    || from >= m_tabs.size()
                    || to >= m_tabs.size()) {
                    return;
                }

                syncActiveTabState();
                m_tabController.move(from, to);
            });

        connect(
            m_tabBar,
            &QWidget::customContextMenuRequested,
            this,
            &ThisPcWindow::showTabContextMenu);

        m_contentSplitter = new QSplitter(Qt::Horizontal, rightPane);
        m_contentSplitter->setObjectName(QStringLiteral("contentSplitter"));
        m_contentSplitter->setChildrenCollapsible(false);
        m_contentSplitter->setHandleWidth(1);

        m_primaryPane = new PrimaryBrowserPane(m_contentSplitter);
        auto *primaryLayout = qobject_cast<QVBoxLayout *>(m_primaryPane->layout());

        auto *primaryAddressHeader = new QFrame(m_primaryPane);
        primaryAddressHeader->setObjectName(
            QStringLiteral("primaryPaneHeader"));
        auto *primaryAddressLayout = new QHBoxLayout(primaryAddressHeader);
        primaryAddressLayout->setContentsMargins(8, 6, 8, 6);
        primaryAddressLayout->setSpacing(4);
        m_addressStack->setParent(primaryAddressHeader);
        primaryAddressLayout->addWidget(m_addressStack, 1);
        primaryLayout->addWidget(primaryAddressHeader);

        m_contentStack = m_primaryPane->contentStack();
        primaryLayout->addWidget(m_contentStack, 1);
        m_contentSplitter->addWidget(m_primaryPane);

        m_splitPane = new SplitBrowserPane(m_contentSplitter);
        m_splitPane->setAlwaysShowFullNames(
            m_alwaysShowFullNames);
        m_contentSplitter->addWidget(m_splitPane);
        m_splitPane->hide();

        m_paneAdapter = std::make_unique<PaneAdapter>(
            PaneBinding{
                [] { return true; },
                [this] { return m_primaryPane->currentUrl(); },
                [this] { return m_contentStack && m_contentStack->currentWidget() == m_directoryPage; },
                [this] { return m_directoryViewMode; },
                [this] { return m_directoryList; },
                [this] { return m_directoryDetails; },
                [this](const QUrl &url) { navigateTo(url, true); },
                [this] { refreshCurrent(); },
                [this](const QUrl &url) { openInSplitPane(url); }},
            PaneBinding{
                [this] { return m_splitPane && m_splitPane->isVisible(); },
                [this] { return m_splitPane ? m_splitPane->currentUrl() : QUrl(); },
                [this] { return m_splitPane && !sameLocation(m_splitPane->currentUrl(), kThisPcUrl); },
                [this] { return m_splitPane ? m_splitPane->viewMode() : 0; },
                [this] { return m_splitPane ? m_splitPane->listView() : nullptr; },
                [this] { return m_splitPane ? m_splitPane->detailsView() : nullptr; },
                [this](const QUrl &url) { if (m_splitPane) m_splitPane->setCurrentUrl(url, true); },
                [this] { if (m_splitPane) m_splitPane->refresh(); },
                [this](const QUrl &url) { navigateTo(url, true); }});

        QSettings splitSettings;
        const QByteArray storedSplitState =
            splitSettings.value(
                QStringLiteral("split/state"))
                .toByteArray();
        if (storedSplitState.isEmpty()
            || !m_contentSplitter->restoreState(storedSplitState)) {
            m_contentSplitter->setSizes({650, 450});
        }
        m_splitterState = m_contentSplitter->saveState();

        connect(
            m_contentSplitter,
            &QSplitter::splitterMoved,
            this,
            [this] {
                m_splitterState = m_contentSplitter->saveState();
                QSettings settings;
                settings.setValue(
                    QStringLiteral("split/state"),
                    m_splitterState);
            });

        m_splitPane->setSearchRootsProvider([this] { return wholeComputerSearchRoots(); });
        connect(m_splitPane, &SplitBrowserPane::homeRefreshRequested,
                this, &ThisPcWindow::reloadDrives);
        connect(m_splitPane, &SplitBrowserPane::searchUiChanged, this, [this] {
            if (m_activePane == PaneId::Split) updateSearchControls();
        });
        connect(m_splitPane, &SplitBrowserPane::searchCanceled, this, [this] {
            statusBar()->showMessage(trLocal("Wyszukiwanie anulowane", "Search canceled"), 4000);
        });
        connect(m_splitPane->searchController(), &SearchController::finished, this, [this] {
            if (m_activePane == PaneId::Split)
                statusBar()->showMessage(trLocal("Wyszukiwanie zakończone", "Search completed"), 4000);
        });
        connect(
            m_splitPane,
            &SplitBrowserPane::closeRequested,
            this,
            [this] { setSplitViewEnabled(false); });
        connect(
            m_splitPane,
            &SplitBrowserPane::swapRequested,
            this,
            &ThisPcWindow::swapSplitPanes);
        connect(
            m_splitPane,
            &SplitBrowserPane::openInPrimaryRequested,
            this,
            [this](const QUrl &url) { navigateTo(url, true); });
        connect(
            m_splitPane,
            &SplitBrowserPane::openInNewTabRequested,
            this,
            [this](const QUrl &url) { createNewTab(url, true); });
        connect(
            m_splitPane,
            &SplitBrowserPane::openInNewWindowRequested,
            this,
            &ThisPcWindow::openInNewWindow);
        connect(
            m_splitPane,
            &SplitBrowserPane::locationChanged,
            this,
            [this](const QUrl &url) {
                if (!m_tabRestoreInProgress) {
                    recordRecentLocation(url);
                }
            });
        connect(
            m_splitPane,
            &SplitBrowserPane::urlsDropped,
            this,
            &ThisPcWindow::handleDroppedUrls);

        connect(
            m_splitPane,
            &SplitBrowserPane::stateChanged,
            this,
            [this] {
                if (!m_tabRestoreInProgress) {
                    syncActiveTabState();
                }
                if (m_activePane == PaneId::Split) {
                    updateSidebarCurrent();
                    updateSearchControls();
                }
                updateFileActionStates();
            });

        connect(m_splitPane, &SplitBrowserPane::selectionChanged,
                this, [this] { updateFileActionStates(); updatePreviewAndQuickLook(); });
        connect(m_splitPane, &SplitBrowserPane::activated,
                this, [this] { setActivePane(PaneId::Split); });
        connect(m_splitPane, &SplitBrowserPane::contextMenuRequested,
                this, [this](bool details, const QPoint &pos) {
                    showPaneContextMenu(PaneId::Split, details, pos);
                });

        connect(
            qApp,
            &QApplication::focusChanged,
            this,
            [this](QWidget *, QWidget *now) {
                if (!now
                    || !m_primaryPane
                    || !m_splitPane) {
                    return;
                }

                const bool splitActive =
                    m_splitPane->isVisible()
                    && (now == m_splitPane
                        || m_splitPane->isAncestorOf(now));

                const bool primaryActive =
                    now == m_primaryPane
                    || m_primaryPane->isAncestorOf(now);

                if (!splitActive && !primaryActive) {
                    return;
                }

                setActivePane(splitActive ? PaneId::Split : PaneId::Primary);
            });

        rightLayout->addWidget(m_tabStrip);
        m_previewSplitter = new QSplitter(Qt::Horizontal, rightPane);
        m_previewSplitter->setObjectName(QStringLiteral("previewSplitter"));
        m_previewSplitter->setChildrenCollapsible(false);
        m_previewSplitter->setHandleWidth(1);
        m_contentSplitter->setParent(m_previewSplitter);
        m_previewSplitter->addWidget(m_contentSplitter);
        m_previewPane = new PreviewPane(m_previewSplitter);
        m_previewSplitter->addWidget(m_previewPane);
        m_previewPane->hide();
        m_previewCoordinator = new PreviewCoordinator(
            {this, centralWidget(), m_previewPane, m_quickLook},
            [this] { return paneContext(); }, this);
        m_previewSplitter->setStretchFactor(0, 1);
        m_previewSplitter->setStretchFactor(1, 0);
        m_previewSplitter->setSizes({850, 320});
        rightLayout->addWidget(m_previewSplitter, 1);
        m_sidebarSplitter->addWidget(rightPane);
        m_sidebarSplitter->setStretchFactor(0, 0);
        m_sidebarSplitter->setStretchFactor(1, 1);

        QSettings sidebarSettings;
        m_preferredSidebarWidth = std::clamp(
            sidebarSettings.value(QStringLiteral("sidebar/width"), 235).toInt(),
            205,
            480);
        m_sidebarSplitter->setSizes({m_preferredSidebarWidth, 945});
        connect(m_sidebarSplitter, &QSplitter::splitterMoved,
                this, [this](int, int) {
            if (!m_sidebarScrollArea) {
                return;
            }
            m_preferredSidebarWidth = std::clamp(
                m_sidebarScrollArea->width(), 205, 480);
            QSettings settings;
            settings.setValue(
                QStringLiteral("sidebar/width"),
                m_preferredSidebarWidth);
        });
        centralLayout->addWidget(m_sidebarSplitter, 1);

        buildHomePage(PaneId::Primary, m_contentStack, m_homePage, m_homeStatus, m_drivesGrid);
        buildHomePage(PaneId::Split, m_splitPane->contentStack(), m_splitHomePage,
                      m_splitHomeStatus, m_splitDrivesGrid);
        m_splitPane->setHomePage(m_splitHomePage);
        buildDirectoryPage();
        buildTabShortcuts();
        buildSplitShortcuts();

        statusBar()->setSizeGripEnabled(true);

        m_versionLabel = new QLabel(
            QStringLiteral("v0.32.0"),
            this);
        m_versionLabel->setObjectName(
            QStringLiteral("versionLabel"));
        m_versionLabel->setToolTip(
            trLocal(
                "Wersja thispc-view 0.32.0",
                "thispc-view version 0.32.0"));
        statusBar()->addPermanentWidget(m_versionLabel);
    }

    void buildOperationManager(QToolBar *toolbar)
    {
        if (!toolbar || m_operationManager) {
            return;
        }

        m_operationManager =
            new OperationManager(this, toolbar, this);
    }

    void buildHomePage(PaneId pane, QStackedWidget *stack, QWidget *&homePage,
                       QLabel *&homeStatus, QGridLayout *&drivesGrid)
    {
        auto *scroll = new QScrollArea(stack);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        auto *page = new QWidget(scroll);
        scroll->setWidget(page);

        auto *mainLayout = new QVBoxLayout(page);
        mainLayout->setContentsMargins(20, 18, 20, 22);
        mainLayout->setSpacing(10);

        auto *header =
            new QLabel(trLocal("Ten komputer", "This PC"), page);

        QFont headerFont = header->font();
        headerFont.setPointSize(headerFont.pointSize() + 5);
        headerFont.setBold(true);
        header->setFont(headerFont);
        mainLayout->addWidget(header);

        auto *foldersTitle =
            new QLabel(trLocal("Foldery", "Folders"), page);

        QFont sectionFont = foldersTitle->font();
        sectionFont.setPointSize(sectionFont.pointSize() + 1);
        sectionFont.setBold(true);
        foldersTitle->setFont(sectionFont);
        mainLayout->addWidget(foldersTitle);

        auto *foldersGrid = new QGridLayout;
        foldersGrid->setContentsMargins(0, 0, 0, 2);
        foldersGrid->setHorizontalSpacing(10);
        foldersGrid->setVerticalSpacing(7);

        struct FolderDef {
            QStandardPaths::StandardLocation location;
            const char *pl;
            const char *en;
            const char *icon;
        };

        const FolderDef folders[] = {
            {QStandardPaths::DesktopLocation,
             "Pulpit", "Desktop", "folder"},
            {QStandardPaths::DocumentsLocation,
             "Dokumenty", "Documents", "folder-documents"},
            {QStandardPaths::DownloadLocation,
             "Pobrane", "Downloads", "folder-download"},
            {QStandardPaths::PicturesLocation,
             "Obrazy", "Pictures", "folder-pictures"},
            {QStandardPaths::MusicLocation,
             "Muzyka", "Music", "folder-music"},
            {QStandardPaths::MoviesLocation,
             "Filmy", "Videos", "folder-videos"},
        };

        int folderIndex = 0;

        for (const FolderDef &def : folders) {
            const QString path =
                QStandardPaths::writableLocation(def.location);

            if (path.isEmpty()) {
                continue;
            }

            ClickableFrame *card = makeFolderCard(
                trLocal(def.pl, def.en),
                path,
                QString::fromLatin1(def.icon),
                page);

            connect(
                card,
                &ClickableFrame::activated,
                this,
                [this, pane](const QUrl &url) {
                    setActivePane(pane);
                    navigatePane(pane, url);
                });

            foldersGrid->addWidget(
                card,
                folderIndex / 3,
                folderIndex % 3);

            ++folderIndex;
        }

        for (int col = 0; col < 3; ++col) {
            foldersGrid->setColumnStretch(col, 1);
        }

        mainLayout->addLayout(foldersGrid);

        auto *drivesTitle = new QLabel(
            trLocal("Urządzenia i dyski", "Devices and drives"),
            page);
        drivesTitle->setFont(sectionFont);
        mainLayout->addWidget(drivesTitle);

        homeStatus =
            new QLabel(trLocal("Wczytywanie…", "Loading…"), page);
        homeStatus->setForegroundRole(QPalette::PlaceholderText);
        mainLayout->addWidget(homeStatus);

        drivesGrid = new QGridLayout;
        drivesGrid->setContentsMargins(0, 0, 0, 0);
        drivesGrid->setHorizontalSpacing(10);
        drivesGrid->setVerticalSpacing(7);
        drivesGrid->setColumnStretch(0, 1);
        drivesGrid->setColumnStretch(1, 1);
        mainLayout->addLayout(drivesGrid);

        mainLayout->addStretch(1);

        homePage = scroll;
        stack->addWidget(homePage);
    }

    void buildDirectoryPage()
    {
        auto *page = new QWidget(m_contentStack);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 14, 18, 18);
        layout->setSpacing(8);

        m_adminBanner = new QFrame(page);
        m_adminBanner->setObjectName(
            QStringLiteral("adminBanner"));

        auto *adminBannerLayout =
            new QHBoxLayout(m_adminBanner);
        adminBannerLayout->setContentsMargins(
            8, 5, 8, 5);
        adminBannerLayout->setSpacing(8);

        auto *adminIcon =
            new QLabel(m_adminBanner);
        adminIcon->setPixmap(
            themedIcon(
                QStringLiteral("security-high"))
                .pixmap(20, 20));

        auto *adminText =
            new QLabel(
                trLocal(
                    "TRYB ADMINISTRATORA — operacje w tej lokalizacji mogą modyfikować chronione pliki systemowe.",
                    "ADMINISTRATOR MODE — operations in this location may modify protected system files."),
                m_adminBanner);
        adminText->setObjectName(
            QStringLiteral("adminBannerText"));
        adminText->setWordWrap(true);

        auto *leaveAdmin =
            new QPushButton(
                themedIcon(
                    QStringLiteral("system-log-out")),
                trLocal(
                    "Wyjdź z trybu administratora",
                    "Leave administrator mode"),
                m_adminBanner);
        leaveAdmin->setFlat(true);

        connect(
            leaveAdmin,
            &QPushButton::clicked,
            this,
            [this] {
                if (!isAdminUrl(m_navigation.currentUrl())) {
                    return;
                }

                navigateTo(
                    ordinaryFileUrlForAdmin(
                        m_navigation.currentUrl()),
                    true);
            });

        adminBannerLayout->addWidget(adminIcon);
        adminBannerLayout->addWidget(adminText, 1);
        adminBannerLayout->addWidget(leaveAdmin);

        m_adminBanner->hide();
        layout->addWidget(m_adminBanner);

        m_directoryTitle = new ElidedPathLabel(page);

        QFont titleFont = m_directoryTitle->font();
        titleFont.setPointSize(titleFont.pointSize() + 3);
        titleFont.setBold(true);
        m_directoryTitle->setFont(titleFont);

        layout->addWidget(m_directoryTitle);

        m_directoryStatus = new ElidedPathLabel(page);
        m_directoryStatus->setForegroundRole(
            QPalette::PlaceholderText);
        layout->addWidget(m_directoryStatus);

        m_searchProgressFrame = new QFrame(page);
        m_searchProgressFrame->setObjectName(
            QStringLiteral("searchProgressFrame"));

        auto *searchProgressLayout =
            new QHBoxLayout(m_searchProgressFrame);
        searchProgressLayout->setContentsMargins(0, 0, 0, 0);
        searchProgressLayout->setSpacing(8);

        m_searchProgressBar =
            new QProgressBar(m_searchProgressFrame);
        m_searchProgressBar->setRange(0, 100);
        m_searchProgressBar->setValue(0);
        m_searchProgressBar->setTextVisible(false);
        m_searchProgressBar->setMaximumHeight(7);

        m_cancelSearchButton =
            new QPushButton(
                themedIcon(QStringLiteral("process-stop")),
                trLocal("Stop", "Stop"),
                m_searchProgressFrame);
        m_cancelSearchButton->setFlat(true);
        m_cancelSearchButton->setToolTip(
            trLocal(
                "Przerwij wyszukiwanie",
                "Stop search"));

        connect(
            m_cancelSearchButton,
            &QPushButton::clicked,
            this,
            [this] {
                cancelSearch(PaneId::Primary, true);
            });

        searchProgressLayout->addWidget(
            m_searchProgressBar,
            1);
        searchProgressLayout->addWidget(
            m_cancelSearchButton);

        m_searchProgressFrame->hide();
        layout->addWidget(m_searchProgressFrame);

        m_directoryViewStack = new QStackedWidget(page);

        m_directoryList = new DirectoryListWidget(m_directoryViewStack);
        m_directoryList->setObjectName(
            QStringLiteral("directoryList"));
        m_directoryList->setResizeMode(QListView::Adjust);
        m_directoryList->setMovement(QListView::Static);
        m_directoryList->setSelectionMode(
            QAbstractItemView::ExtendedSelection);
        m_directoryList->setContextMenuPolicy(
            Qt::CustomContextMenu);
        m_directoryList->setAlwaysShowFullNames(
            m_alwaysShowFullNames);

        m_directoryDetails =
            new DirectoryTreeWidget(m_directoryViewStack);
        m_directoryDetails->setObjectName(
            QStringLiteral("directoryDetails"));
        m_directoryDetails->setColumnCount(5);
        m_directoryDetails->setHeaderLabels(
            {
                trLocal("Nazwa", "Name"),
                trLocal("Typ", "Type"),
                trLocal("Rozmiar", "Size"),
                trLocal("Zmodyfikowano", "Date modified"),
                trLocal("Lokalizacja", "Location")
            });
        m_directoryDetails->setRootIsDecorated(false);
        m_directoryDetails->setUniformRowHeights(true);
        m_directoryDetails->setAllColumnsShowFocus(true);
        m_directoryDetails->setSelectionMode(
            QAbstractItemView::ExtendedSelection);
        m_directoryDetails->setContextMenuPolicy(
            Qt::CustomContextMenu);
        m_directoryDetails->setIconSize(QSize(22, 22));

        QHeaderView *header =
            m_directoryDetails->header();
        header->setStretchLastSection(false);
        header->setSectionsMovable(true);
        header->setSectionResizeMode(
            0,
            QHeaderView::Stretch);
        header->setSectionResizeMode(
            1,
            QHeaderView::Interactive);
        header->setSectionResizeMode(
            2,
            QHeaderView::ResizeToContents);
        header->setSectionResizeMode(
            3,
            QHeaderView::ResizeToContents);
        header->setSectionResizeMode(
            4,
            QHeaderView::Interactive);
        m_directoryDetails->setColumnWidth(1, 220);
        m_directoryDetails->setColumnWidth(4, 320);
        m_directoryDetails->setColumnHidden(4, true);

        m_directoryViewStack->addWidget(m_directoryList);
        m_directoryViewStack->addWidget(m_directoryDetails);

        layout->addWidget(m_directoryViewStack, 1);

        connect(
            m_directoryList,
            &DirectoryListWidget::itemDoubleClicked,
            this,
            [this](const QModelIndex &item) {
                activateDirectoryItem(item);
            });

        connect(
            m_directoryList,
            &QListWidget::customContextMenuRequested,
            this,
            [this](const QPoint &pos) {
                showDirectoryContextMenu(pos);
            });

        connect(
            m_directoryList,
            &DirectoryListWidget::itemSelectionChanged,
            this,
            [this] { updateFileActionStates(); updatePreviewAndQuickLook(); });

        connect(
            m_directoryList,
            &DirectoryListWidget::urlsDropped,
            this,
            &ThisPcWindow::handleDroppedUrls);

        connect(
            m_directoryDetails,
            &QTreeWidget::itemDoubleClicked,
            this,
            [this](QTreeWidgetItem *item, int) {
                activateDetailsItem(item);
            });

        connect(
            m_directoryDetails,
            &QTreeWidget::customContextMenuRequested,
            this,
            [this](const QPoint &pos) {
                showDirectoryDetailsContextMenu(pos);
            });

        connect(
            m_directoryDetails,
            &QTreeWidget::itemSelectionChanged,
            this,
            [this] { updateFileActionStates(); updatePreviewAndQuickLook(); });

        connect(
            m_directoryDetails,
            &DirectoryTreeWidget::urlsDropped,
            this,
            &ThisPcWindow::handleDroppedUrls);

        installPaneShortcuts(m_directoryViewStack, PaneId::Primary);
        installPaneShortcuts(m_splitPane->shortcutScope(), PaneId::Split);

        m_directoryPage = page;
        m_contentStack->addWidget(m_directoryPage);
        m_primaryPane->bindDirectoryViews(
            m_directoryList, m_directoryDetails, m_directoryTitle, m_directoryStatus);

        applyDirectoryViewMode(false);
    }

    void installPaneShortcuts(QWidget *scope, PaneId pane)
    {
        auto addViewShortcut =
            [this, scope, pane](const QKeySequence &sequence,
                   auto callback) {
            auto *shortcut =
                new QShortcut(sequence, scope);
            shortcut->setContext(
                Qt::WidgetWithChildrenShortcut);
            connect(
                shortcut,
                &QShortcut::activated,
                this,
                [this, pane, callback] {
                    setActivePane(pane);
                    callback();
                });
        };

        addViewShortcut(
            QKeySequence::Copy,
            [this] { putSelectionOnClipboard(false); });

        addViewShortcut(
            QKeySequence::Cut,
            [this] { putSelectionOnClipboard(true); });

        addViewShortcut(
            QKeySequence::Paste,
            [this] { pasteClipboard(); });

        addViewShortcut(
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N),
            [this] { createNewFolder(); });

        addViewShortcut(
            QKeySequence(Qt::Key_F2),
            [this] { renameSelected(); });

        addViewShortcut(
            QKeySequence(Qt::Key_Delete),
            [this] { trashSelected(); });

        addViewShortcut(
            QKeySequence::SelectAll,
            [this] { selectAllDirectoryItems(); });

        addViewShortcut(QKeySequence(Qt::ALT | Qt::Key_Return),
                        [this] { showSelectedProperties(); });
    }

    bool canQuickAccessLocation(const QUrl &url) const
    {
        return m_sidebar
            && m_sidebar->canQuickAccessLocation(url);
    }

    bool isQuickAccessPinned(const QUrl &url) const
    {
        return m_sidebar
            && m_sidebar->isQuickAccessPinned(url);
    }

    void toggleQuickAccessLocation(const QUrl &url)
    {
        if (m_sidebar) {
            m_sidebar->toggleQuickAccessLocation(url);
        }
    }

    void recordRecentLocation(const QUrl &url)
    {
        if (m_sidebar) {
            m_sidebar->recordRecentLocation(url);
        }
    }

    QIcon searchScopeMenuIcon() const
    {
        const QSize size(24, 20);
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        themedIcon(QStringLiteral("edit-find")).paint(
            &painter,
            QRect(0, 1, 17, 17),
            Qt::AlignCenter,
            QIcon::Normal);

        const QIcon arrow = style()->standardIcon(QStyle::SP_ArrowDown);
        arrow.paint(
            &painter,
            QRect(15, 9, 8, 8),
            Qt::AlignCenter,
            QIcon::Normal);

        return QIcon(pixmap);
    }

    QUrl activeLocation() const
    {
        return m_activePane == PaneId::Split && m_splitPane && m_splitPane->isVisible()
            ? m_splitPane->currentUrl() : m_navigation.currentUrl();
    }

    PaneSearchState &activeSearchState()
    {
        return m_activePane == PaneId::Split && m_splitPane && m_splitPane->isVisible()
            ? m_splitPane->searchState() : m_primarySearch;
    }

    void renderActiveSearchItems()
    {
        if (sameLocation(activeLocation(), kThisPcUrl)) return;
        if (m_activePane == PaneId::Split) m_splitPane->renderSearchItems();
        else renderDirectoryItems();
    }

    void updateSearchControls()
    {
        if (!m_searchUiController) return;
        SearchController *backend = m_activePane == PaneId::Split && m_splitPane
            ? m_splitPane->searchController() : m_searchController;
        m_searchUiController->updateControls(
            {activeLocation(), &activeSearchState(), backend});
    }

    QList<QUrl> searchDriveRoots() const
    {
        QList<QUrl> roots;
        for (const DriveInfo &drive : m_driveHomeCoordinator.drives()) roots.push_back(drive.targetUrl);
        return roots;
    }

    QList<QUrl> wholeComputerSearchRoots() const
    {
        return SearchUiController::wholeComputerSearchRoots(searchDriveRoots());
    }

    void startSearchFromUi()
    {
        if (!m_searchUiController) return;
        const auto request = m_searchUiController->requestFromUi(
            {activeLocation(), &activeSearchState(), nullptr}, searchDriveRoots(),
            QUrl::fromLocalFile(QDir::homePath()));
        if (request.isValid()) navigatePane(m_activePane,
            request.location(activeSearchState()));
    }

    void syncCurrentSearchFilters()
    {
        if (!isSearchLocation(activeLocation())) {
            return;
        }

        const QUrl updated = m_searchUiController->locationWithSyncedFilters(
            {activeLocation(), &activeSearchState(), nullptr});

        if (m_activePane == PaneId::Split) {
            m_splitPane->updateSearchFilters(updated);
            return;
        }
        m_navigation.updateCurrent(updated, true);
        m_primarySearch.location = updated;

        rebuildBreadcrumbs();
        syncActiveTabState();
        updateActiveTabPresentation();
    }

    void updateSearchProgress()
    {
        if (m_searchUiController) m_searchUiController->updateProgress(*m_searchController);
    }

    void updateSearchStatusLabel()
    {
        if (!m_directoryStatus
            || !isSearchLocation(m_navigation.currentUrl())) {
            return;
        }

        m_searchUiController->updateStatus(*m_searchController,
            m_searchVisibleCount, m_primarySearch.scope);
    }

    void cancelSearch(PaneId pane, bool userRequested)
    {
        if (pane == PaneId::Split) {
            m_splitPane->cancelSearch(userRequested);
            updateSearchControls();
            return;
        }
        if (!m_searchController->cancel()) return;
        if (m_searchProgressFrame) m_searchProgressFrame->hide();
        updateSearchControls();
        if (userRequested) {
            m_primaryPane->setFiles(m_searchController->files());
            renderDirectoryItems();
            statusBar()->showMessage(trLocal("Wyszukiwanie anulowane", "Search canceled"), 4000);
        }
    }

    void loadSearchLocation(const QUrl &url)
    {
        cancelSearch(PaneId::Primary, false);

        m_primaryPane->mutableFiles().clear();
        m_directoryList->clear();
        m_directoryDetails->clear();
        m_directoryList->setDropDirectory(QUrl());
        m_directoryDetails->setDropDirectory(QUrl());

        const QString query =
            searchQueryFromUrl(url);

        m_primarySearch.loadLocation(url);

        m_directoryDetails->setColumnHidden(
            4,
            false);

        m_directoryTitle->setText(
            isPolish()
                ? QStringLiteral("Wyniki wyszukiwania: %1")
                    .arg(query)
                : QStringLiteral("Search results: %1")
                    .arg(query));

        const QList<QUrl> roots = m_searchUiController->rootsForLocation(url,
            m_primarySearch, searchDriveRoots(), QUrl::fromLocalFile(QDir::homePath()));

        m_searchVisibleCount = 0;
        m_searchProgressBar->setRange(0, 100);
        m_searchProgressBar->setValue(0);
        m_searchProgressFrame->show();
        m_searchController->start(query, roots, m_showHiddenFiles);
        updateSearchControls();
    }

    void setSplitViewEnabled(bool enabled)
    {
        if (!m_splitPane || !m_contentSplitter) {
            return;
        }

        if (m_splitViewAction
            && m_splitViewAction->isChecked() != enabled) {
            QSignalBlocker blocker(m_splitViewAction);
            m_splitViewAction->setChecked(enabled);
        }
        if (m_swapPanesAction) {
            m_swapPanesAction->setVisible(enabled);
            m_swapPanesAction->setEnabled(enabled);
        }
        if (m_comparePanesAction) {
            m_comparePanesAction->setVisible(enabled);
            m_comparePanesAction->setEnabled(enabled);
        }

        if (enabled) {
            QUrl target = m_navigation.currentUrl();

            if (m_activeTab >= 0
                && m_activeTab < m_tabs.size()
                && m_tabs.at(m_activeTab).splitUrl.isValid()) {
                target = m_tabs.at(m_activeTab).splitUrl;
            }

            if (!target.isValid()) {
                target = kThisPcUrl;
            }

            const bool wasHidden = m_splitPane->isHidden();
            m_splitPane->show();

            int splitViewMode =
                m_directoryViewMode;
            int splitSortKey =
                m_sortKey;
            bool splitSortAscending =
                m_sortAscending;

            if (m_activeTab >= 0
                && m_activeTab < m_tabs.size()) {
                const TabState &state =
                    m_tabs.at(m_activeTab);

                if (state.splitViewMode >= 0) {
                    splitViewMode =
                        state.splitViewMode;
                    splitSortKey =
                        state.splitSortKey;
                    splitSortAscending =
                        state.splitSortAscending;
                }
            }

            // The tab snapshot is only a transition fallback. The target
            // location's persisted preference is applied by setCurrentUrl().
            m_splitPane->setViewMode(splitViewMode, false);
            m_splitPane->setSortState(
                splitSortKey,
                splitSortAscending);
            m_splitPane->setCurrentUrl(target, true);

            if (wasHidden) m_contentSplitter->restoreState(m_splitterState);
        } else {
            // Hiding a splitter child gives it zero visible width. Preserve
            // the user's two-pane allocation before that layout takes place.
            if (!m_splitPane->isHidden()) m_splitterState = m_contentSplitter->saveState();
            m_splitPane->cancelSearch(false);
            m_splitPane->hide();
            setActivePane(PaneId::Primary);
        }

        if (!m_tabRestoreInProgress) {
            syncActiveTabState();
        }
    }

    void openInSplitPane(const QUrl &rawUrl)
    {
        const QUrl url = normalizedUrl(
            rawUrl.isValid() ? rawUrl : m_navigation.currentUrl());

        setSplitViewEnabled(true);
        m_splitPane->setCurrentUrl(url, true);
        syncActiveTabState();
    }

    void swapSplitPanes()
    {
        if (!m_splitPane || !m_splitPane->isVisible()) {
            return;
        }

        const QUrl primary = m_navigation.currentUrl();
        const QUrl secondary = m_splitPane->currentUrl();

        if (secondary.isValid()) {
            navigateTo(secondary, true);
        }
        if (primary.isValid()) {
            m_splitPane->setCurrentUrl(primary, true);
        }
        syncActiveTabState();
    }

    void compareSplitPanes()
    {
        if (!m_splitPane || !m_splitPane->isVisible()) {
            return;
        }

        const QUrl left = m_navigation.currentUrl();
        const QUrl right = m_splitPane->currentUrl();

        if (sameLocation(left, kThisPcUrl) || sameLocation(right, kThisPcUrl)) {
            QMessageBox::information(
                this,
                trLocal("Porównanie paneli", "Compare Panels"),
                trLocal(
                    "Porównanie wymaga otwarcia konkretnego folderu w obu panelach (nie widoku „Ten komputer”).",
                    "Comparison requires a specific folder to be open in both panes (not the \"This PC\" root)."));
            return;
        }

        if (isSearchLocation(left) || isSearchLocation(right)) {
            QMessageBox::information(
                this,
                trLocal("Porównanie paneli", "Compare Panels"),
                trLocal(
                    "Porównywanie wyników wyszukiwania nie jest obsługiwane. Otwórz zwykłe foldery w obu panelach.",
                    "Comparing search result sets is not supported. Please open standard folder locations in both panes."));
            return;
        }

        SplitCompareDialog dialog(left, right, m_showHiddenFiles, this);
        dialog.exec();
    }

    void focusPrimaryPane()
    {
        if (m_contentStack->currentWidget() == m_homePage) {
            m_homePage->setFocus(Qt::ShortcutFocusReason);
            return;
        }

        if (m_directoryViewMode == 2 && m_directoryDetails) {
            m_directoryDetails->setFocus(Qt::ShortcutFocusReason);
        } else if (m_directoryList) {
            m_directoryList->setFocus(Qt::ShortcutFocusReason);
        }
    }

    void buildSplitShortcuts()
    {
        auto *focusOther = new QAction(this);
        focusOther->setShortcut(QKeySequence(Qt::Key_F6));
        focusOther->setShortcutContext(Qt::WindowShortcut);
        addAction(focusOther);

        connect(
            focusOther,
            &QAction::triggered,
            this,
            [this] {
                if (!m_splitPane || !m_splitPane->isVisible()) {
                    return;
                }

                if (m_activePane == PaneId::Split) {
                    setActivePane(PaneId::Primary);
                    focusPrimaryPane();
                } else {
                    setActivePane(PaneId::Split);
                    m_splitPane->focusView();
                }
            });
    }

    QIcon tabIconForUrl(const QUrl &url) const
    {
        if (sameLocation(url, kThisPcUrl)) {
            return themedIcon(QStringLiteral("computer"));
        }

        if (isSearchLocation(url)) {
            return themedIcon(QStringLiteral("system-search"));
        }

        if (isAdminUrl(url)) {
            return themedIcon(QStringLiteral("security-high"));
        }

        if (url.scheme() == QStringLiteral("trash")) {
            return themedIcon(QStringLiteral("user-trash"));
        }

        if (url.scheme() == QStringLiteral("remote")) {
            return themedIcon(QStringLiteral("network-workgroup"));
        }

        return themedIcon(QStringLiteral("folder"));
    }

    QString tabTitleForUrl(const QUrl &url) const
    {
        QString title = displayNameForLocation(url);

        if (title.isEmpty()) {
            title = trLocal("Nowa karta", "New tab");
        }

        return title;
    }

    void syncActiveTabState()
    {
        if (m_activeTab < 0
            || m_activeTab >= m_tabs.size()) {
            return;
        }

        if (m_tabRestoreInProgress) {
            return;
        }

        TabState state;
        const auto navigation = m_navigation.snapshot();
        state.currentUrl = navigation.currentUrl;
        state.history = navigation.history;
        state.historyIndex = navigation.historyIndex;
        state.splitEnabled =
            m_splitPane
            && m_splitPane->isVisible();

        if (m_splitPane) {
            if (m_splitPane->currentUrl().isValid()) {
                state.splitUrl =
                    m_splitPane->currentUrl();
            }

            state.splitViewMode =
                m_splitPane->viewMode();
            state.splitSortKey =
                m_splitPane->sortKey();
            state.splitSortAscending =
                m_splitPane->sortAscending();
        }
        m_tabController.syncActiveState(state);
    }

    void saveSessionState()
    {
        syncActiveTabState();

        const SessionSnapshot snapshot = m_tabController.snapshot(
            m_activePane == PaneId::Split);
        SessionManager::save(snapshot);
    }

    bool restoreSessionState()
    {
        if (!m_tabBar || !m_tabs.isEmpty()) {
            return false;
        }

        const SessionSnapshot snapshot =
            SessionManager::load(
                m_sortKey,
                m_sortAscending);
        if (snapshot.tabs.isEmpty()) {
            return false;
        }

        m_tabController.restore(snapshot);
        m_tabChangeInProgress = true;
        for (int i = 0; i < m_tabs.size(); ++i) {
            const TabState &state = m_tabs.at(i);
            m_tabBar->addTab(
                tabIconForUrl(state.currentUrl),
                tabTitleForUrl(state.currentUrl));
            m_tabBar->setTabToolTip(
                i,
                urlForDisplay(state.currentUrl));
        }

        m_tabBar->setCurrentIndex(snapshot.activeTab);
        m_tabChangeInProgress = false;

        m_tabController.clearActive();
        switchToTab(snapshot.activeTab);

        setActivePane(
            snapshot.splitPaneActive
                && m_splitPane
                && m_splitPane->isVisible()
                ? PaneId::Split
                : PaneId::Primary);

        refreshRestoredSessionGeometry();
        return true;
    }

    void refreshRestoredSessionGeometry()
    {
        // Session restore happens before the window has completed its first
        // layout pass.  Re-run the tab-strip/splitter geometry after the event
        // loop starts so the active-pane top border is aligned with the final
        // tab-strip height immediately, rather than only after switching tabs.
        QTimer::singleShot(0, this, [this] {
            if (m_tabBar) {
                m_tabBar->updateGeometry();
                m_tabBar->update();
            }
            if (m_tabStrip) {
                if (m_tabStrip->layout()) {
                    m_tabStrip->layout()->invalidate();
                    m_tabStrip->layout()->activate();
                }
                m_tabStrip->updateGeometry();
                m_tabStrip->update();
            }
            if (m_contentSplitter) {
                m_contentSplitter->updateGeometry();
                m_contentSplitter->update();
            }
            if (m_primaryPane) {
                m_primaryPane->updateGeometry();
                m_primaryPane->update();
            }
            if (m_splitPane && m_splitPane->isVisible()) {
                m_splitPane->updateGeometry();
                m_splitPane->update();
            }
            if (centralWidget() && centralWidget()->layout()) {
                centralWidget()->layout()->invalidate();
                centralWidget()->layout()->activate();
            }
        });
    }

    void updateActiveTabPresentation()
    {
        if (!m_tabBar
            || m_activeTab < 0
            || m_activeTab >= m_tabs.size()) {
            return;
        }

        const QUrl url = m_navigation.currentUrl();
        const QString title = tabTitleForUrl(url);

        m_tabBar->setTabText(m_activeTab, title);
        m_tabBar->setTabIcon(m_activeTab, tabIconForUrl(url));
        m_tabBar->setTabToolTip(
            m_activeTab,
            urlForDisplay(url));

        setWindowTitle(
            isPolish()
                ? QStringLiteral("%1 — Ten komputer").arg(title)
                : QStringLiteral("%1 — This PC").arg(title));
    }

    void openInNewWindow(const QUrl &rawUrl)
    {
        const QUrl url = normalizedUrl(
            rawUrl.isValid() ? rawUrl : kThisPcUrl);

        const QString executable =
            QCoreApplication::applicationFilePath();

        if (executable.isEmpty()
            || !QProcess::startDetached(
                executable,
                {url.toString(QUrl::FullyEncoded)})) {
            QMessageBox::warning(
                this,
                trLocal(
                    "Nowe okno",
                    "New window"),
                trLocal(
                    "Nie udało się otworzyć nowego okna.",
                    "Could not open a new window."));
        }
    }

    void createNewTab(
        const QUrl &rawUrl = kThisPcUrl,
        bool makeCurrent = true)
    {
        const QUrl url = normalizedUrl(
            rawUrl.isValid() ? rawUrl : kThisPcUrl);

        const int index = m_tabController.create(
            url,
            m_sortKey,
            m_sortAscending);

        m_tabChangeInProgress = true;
        m_tabBar->addTab(
            tabIconForUrl(url),
            tabTitleForUrl(url));
        m_tabBar->setTabToolTip(
            index,
            urlForDisplay(url));

        if (makeCurrent) {
            m_tabBar->setCurrentIndex(index);
        }
        m_tabChangeInProgress = false;

        if (makeCurrent) {
            switchToTab(index);
        }
    }

    void duplicateTab(int index)
    {
        if (index < 0 || index >= m_tabs.size()) {
            return;
        }

        if (index == m_activeTab) {
            syncActiveTabState();
        }

        const int newIndex = m_tabController.duplicate(index);
        const TabState source = m_tabs.at(newIndex);

        m_tabChangeInProgress = true;
        m_tabBar->addTab(
            tabIconForUrl(source.currentUrl),
            tabTitleForUrl(source.currentUrl));
        m_tabBar->setTabToolTip(
            newIndex,
            urlForDisplay(source.currentUrl));
        m_tabBar->setCurrentIndex(newIndex);
        m_tabChangeInProgress = false;

        switchToTab(newIndex);
    }

    void reopenClosedTab()
    {
        if (m_closedTabs.isEmpty()) {
            return;
        }

        const int index = m_tabController.reopenClosed();
        const TabState state = m_tabs.at(index);

        m_tabChangeInProgress = true;
        m_tabBar->addTab(
            tabIconForUrl(state.currentUrl),
            tabTitleForUrl(state.currentUrl));
        m_tabBar->setTabToolTip(
            index,
            urlForDisplay(state.currentUrl));
        m_tabBar->setCurrentIndex(index);
        m_tabChangeInProgress = false;

        switchToTab(index);
    }

    QUrl tabDropDirectory(int index) const
    {
        return m_tabController.dropDirectory(index, m_navigation.currentUrl());
    }

    void switchToTab(int index)
    {
        if (index < 0 || index >= m_tabs.size()) {
            return;
        }

        if (index == m_activeTab) {
            updateActiveTabPresentation();
            return;
        }

        syncActiveTabState();

        m_tabController.switchTo(index);
        const TabState state = m_tabs.at(index);
        const QUrl previousPrimaryUrl = m_navigation.currentUrl();
        m_navigation.restore({state.currentUrl, state.history, state.historyIndex});

        m_tabChangeInProgress = true;
        if (m_tabBar->currentIndex() != index) {
            m_tabBar->setCurrentIndex(index);
        }
        m_tabChangeInProgress = false;

        m_tabRestoreInProgress = true;
        if (m_splitViewAction) {
            QSignalBlocker blocker(m_splitViewAction);
            m_splitViewAction->setChecked(state.splitEnabled);
        }
        if (m_swapPanesAction) {
            m_swapPanesAction->setVisible(state.splitEnabled);
            m_swapPanesAction->setEnabled(state.splitEnabled);
        }
        if (m_comparePanesAction) {
            m_comparePanesAction->setVisible(state.splitEnabled);
            m_comparePanesAction->setEnabled(state.splitEnabled);
        }
        if (state.splitEnabled) {
            m_splitPane->show();
            m_splitPane->setViewMode(
                state.splitViewMode >= 0
                    ? state.splitViewMode
                    : m_directoryViewMode,
                false);
            m_splitPane->setSortState(
                state.splitSortKey,
                state.splitSortAscending);
            m_splitPane->setCurrentUrl(
                state.splitUrl.isValid()
                    ? state.splitUrl
                    : state.currentUrl,
                true);
        } else {
            m_splitPane->cancelSearch(false);
            m_splitPane->hide();
            setActivePane(PaneId::Primary);
        }
        loadLocation(state.currentUrl, !sameLocation(previousPrimaryUrl, state.currentUrl));
        m_tabRestoreInProgress = false;
        syncActiveTabState();
    }

    void closeTab(int index)
    {
        if (index < 0 || index >= m_tabs.size()) {
            return;
        }

        if (m_tabs.size() == 1) {
            // Keep one usable tab instead of leaving the window without a view.
            navigateTo(kThisPcUrl, true);
            return;
        }

        if (index == m_activeTab) {
            syncActiveTabState();
        }

        const TabController::CloseResult result =
            m_tabController.close(index);

        m_tabChangeInProgress = true;
        m_tabBar->removeTab(index);
        m_tabChangeInProgress = false;

        if (result.wasActive) {
            switchToTab(result.nextActive);
        } else {
            m_tabChangeInProgress = true;
            m_tabBar->setCurrentIndex(m_activeTab);
            m_tabChangeInProgress = false;
            updateActiveTabPresentation();
        }
    }

    void closeOtherTabs(int keepIndex)
    {
        if (keepIndex < 0 || keepIndex >= m_tabs.size()) {
            return;
        }

        if (keepIndex == m_activeTab) {
            syncActiveTabState();
        }

        const TabState kept = m_tabs.at(keepIndex);
        m_tabController.closeOthers(keepIndex);

        m_tabChangeInProgress = true;
        while (m_tabBar->count() > 0) {
            m_tabBar->removeTab(0);
        }
        m_tabBar->addTab(
            tabIconForUrl(kept.currentUrl),
            tabTitleForUrl(kept.currentUrl));
        m_tabBar->setTabToolTip(
            0,
            urlForDisplay(kept.currentUrl));
        m_tabBar->setCurrentIndex(0);
        m_tabChangeInProgress = false;

        switchToTab(0);
    }

    void showTabContextMenu(const QPoint &pos)
    {
        const int index = m_tabBar->tabAt(pos);
        QMenu menu(this);

        QAction *newTab = menu.addAction(
            themedIcon(QStringLiteral("tab-new")),
            trLocal("Nowa karta", "New tab"));
        newTab->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));

        QAction *duplicate = nullptr;
        QAction *close = nullptr;
        QAction *closeOthers = nullptr;

        if (index >= 0) {
            duplicate = menu.addAction(
                themedIcon(QStringLiteral("edit-copy")),
                trLocal("Duplikuj kartę", "Duplicate tab"));

            menu.addSeparator();

            close = menu.addAction(
                themedIcon(QStringLiteral("window-close")),
                trLocal("Zamknij kartę", "Close tab"));
            close->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));

            closeOthers = menu.addAction(
                trLocal(
                    "Zamknij pozostałe karty",
                    "Close other tabs"));
            closeOthers->setEnabled(m_tabs.size() > 1);
        }

        menu.addSeparator();

        QAction *reopen = menu.addAction(
            themedIcon(QStringLiteral("edit-undo")),
            trLocal(
                "Otwórz ponownie zamkniętą kartę",
                "Reopen closed tab"));
        reopen->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Shift+T")));
        reopen->setEnabled(!m_closedTabs.isEmpty());

        QAction *chosen = menu.exec(
            m_tabBar->mapToGlobal(pos));

        if (chosen == newTab) {
            createNewTab(kThisPcUrl, true);
        } else if (duplicate && chosen == duplicate) {
            duplicateTab(index);
        } else if (close && chosen == close) {
            closeTab(index);
        } else if (closeOthers && chosen == closeOthers) {
            closeOtherTabs(index);
        } else if (chosen == reopen) {
            reopenClosedTab();
        }
    }

    void buildTabShortcuts()
    {
        auto *newWindow = new QAction(this);
        newWindow->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+N")));
        newWindow->setShortcutContext(Qt::WindowShortcut);
        addAction(newWindow);
        connect(
            newWindow,
            &QAction::triggered,
            this,
            [this] {
                openInNewWindow(m_navigation.currentUrl());
            });

        auto *newTab = new QAction(this);
        newTab->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+T")));
        newTab->setShortcutContext(Qt::WindowShortcut);
        addAction(newTab);
        connect(
            newTab,
            &QAction::triggered,
            this,
            [this] { createNewTab(kThisPcUrl, true); });

        auto *closeCurrent = new QAction(this);
        closeCurrent->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+W")));
        closeCurrent->setShortcutContext(Qt::WindowShortcut);
        addAction(closeCurrent);
        connect(
            closeCurrent,
            &QAction::triggered,
            this,
            [this] { closeTab(m_activeTab); });

        auto *reopen = new QAction(this);
        reopen->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Shift+T")));
        reopen->setShortcutContext(Qt::WindowShortcut);
        addAction(reopen);
        connect(
            reopen,
            &QAction::triggered,
            this,
            &ThisPcWindow::reopenClosedTab);

        auto *nextTab = new QAction(this);
        nextTab->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Tab")));
        nextTab->setShortcutContext(Qt::WindowShortcut);
        addAction(nextTab);
        connect(
            nextTab,
            &QAction::triggered,
            this,
            [this] {
                if (m_tabBar->count() < 2) {
                    return;
                }
                const int next =
                    (m_tabBar->currentIndex() + 1)
                    % m_tabBar->count();
                m_tabBar->setCurrentIndex(next);
            });

        auto *previousTab = new QAction(this);
        previousTab->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Shift+Tab")));
        previousTab->setShortcutContext(Qt::WindowShortcut);
        addAction(previousTab);
        connect(
            previousTab,
            &QAction::triggered,
            this,
            [this] {
                if (m_tabBar->count() < 2) {
                    return;
                }
                const int previous =
                    (m_tabBar->currentIndex()
                     + m_tabBar->count() - 1)
                    % m_tabBar->count();
                m_tabBar->setCurrentIndex(previous);
            });
    }

    void navigateTo(const QUrl &rawUrl, bool addHistory)
    {
        const QUrl previousUrl = m_navigation.currentUrl();
        if (!rawUrl.isValid()) {
            return;
        }

        if (!m_navigation.navigate(rawUrl, addHistory)) return;
        const QUrl url = m_navigation.currentUrl();
        recordRecentLocation(url);
        loadLocation(url, !sameLocation(previousUrl, url));
    }

    void loadLocation(const QUrl &url, bool locationChangedBeforeModel = false)
    {
        const bool changedLocation =
            locationChangedBeforeModel || !sameLocation(m_navigation.currentUrl(), url);

        if (changedLocation && m_searchController->isRunning()) {
            cancelSearch(PaneId::Primary, false);
        }

        m_navigation.updateCurrent(url);
        m_primaryPane->setCurrentUrl(url);
        m_directoryViewMode = DirectoryViewSettings::viewMode(
            url,
            m_directoryViewMode);
        m_directoryIconSizeMode = DirectoryViewSettings::iconSizeMode(
            url,
            m_directoryIconSizeMode);
        m_groupMode = DirectoryViewSettings::groupMode(
            url,
            m_groupMode);
        scheduleDateGroupingRefresh();
        applyDirectoryViewMode(false);

        if (m_adminBanner) {
            m_adminBanner->setVisible(
                isAdminUrl(url));
        }

        m_primarySearch.loadLocation(url);

        m_primaryPane->cancelListing();

        if (sameLocation(url, kThisPcUrl)) {
            m_contentStack->setCurrentWidget(m_homePage);
            if (m_directoryDetails) {
                m_directoryDetails->setColumnHidden(
                    4,
                    true);
            }
            statusBar()->showMessage(
                trLocal("Ten komputer", "This PC"));
        } else {
            m_contentStack->setCurrentWidget(m_directoryPage);

            if (isSearchLocation(url)) {
                loadSearchLocation(url);
            } else {
                m_directoryDetails->setColumnHidden(
                    4,
                    true);
                loadDirectory(url);
            }
        }

        updateSearchControls();
        rebuildBreadcrumbs();
        updateNavigationActions();
        updateSidebarCurrent();
        updateFileActionStates();
        syncActiveTabState();
        updateActiveTabPresentation();
    }

    void loadDirectory(const QUrl &url, bool preserveStatusMessage = false)
    {
        m_primaryPane->loadDirectory(
            url, preserveStatusMessage, m_showHiddenFiles,
            m_directoryViewMode == 2,
            [this](bool preserve) { renderDirectoryItems(preserve); },
            [this](const QString &error) { statusBar()->showMessage(error, 6000); });
    }

    bool fileMatchesSearch(const FileInfo &file) const
    {
        if (isSearchLocation(m_navigation.currentUrl()) || m_primarySearch.text.trimmed().isEmpty()) {
            return true;
        }

        return file.name.contains(
                m_primarySearch.text.trimmed(),
                Qt::CaseInsensitive)
            || file.mimeType.contains(
                m_primarySearch.text.trimmed(),
                Qt::CaseInsensitive);
    }

    QIcon iconForFile(
        const FileInfo &file,
        QMimeDatabase &mimeDatabase)
    {
        return m_primaryPane->iconForFile(file, mimeDatabase, m_thumbnailsEnabled);
    }

    void renderDirectoryItems(bool preserveStatusMessage = false)
    {
        PrimaryBrowserPane::RenderOptions options;
        options.sortKey = m_sortKey;
        options.sortAscending = m_sortAscending;
        options.groupMode = m_groupMode;
        options.thumbnailsEnabled = m_thumbnailsEnabled;
        options.detailsActive = m_directoryViewMode == 2;
        options.acceptsFile = [this](const FileInfo &file, QMimeDatabase &mimeDb) {
            if (!fileMatchesSearch(file)) return false;
            return !isSearchLocation(m_navigation.currentUrl())
                || SearchController::matchesFile(
                    file, mimeDb,
                    {m_primarySearch.type, m_primarySearch.date, m_primarySearch.size});
        };
        options.updateStatus = [this](int visibleCount, int count) {
            if (isSearchLocation(m_navigation.currentUrl())) {
                m_searchVisibleCount = visibleCount;
                updateSearchStatusLabel();
            } else if (m_primarySearch.text.trimmed().isEmpty()) {
                m_directoryStatus->setText(isPolish()
                    ? QStringLiteral("%1 elementów").arg(count)
                    : QStringLiteral("%1 items").arg(count));
            } else {
                m_directoryStatus->setText(isPolish()
                    ? QStringLiteral("%1 z %2 elementów").arg(visibleCount).arg(count)
                    : QStringLiteral("%1 of %2 items").arg(visibleCount).arg(count));
            }
        };
        m_primaryPane->renderDirectoryItems(options);
        if (m_activePane == PaneId::Primary && !preserveStatusMessage) {
            statusBar()->showMessage(isSearchLocation(m_navigation.currentUrl())
                ? displayNameForLocation(m_navigation.currentUrl()) : urlForDisplay(m_navigation.currentUrl()));
        }
        updateFileActionStates();
    }

    void activateDirectoryItem(const QModelIndex &item)
    {
        if (!item.isValid()) {
            return;
        }

        const QUrl url(
            item.data(directory_view_detail::UrlRole).toString());

        const bool isDir =
            item.data(directory_view_detail::DirectoryRole).toBool();

        if (isDir) {
            navigateTo(url, true);
        } else {
            QDesktopServices::openUrl(url);
        }
    }


    void activateDetailsItem(QTreeWidgetItem *item)
    {
        if (!item || !item->data(0, directory_view_detail::FileItemRole).toBool()) {
            return;
        }

        const QUrl url(
            item->data(0, Qt::UserRole).toString());

        const bool isDir =
            item->data(
                0,
                Qt::UserRole + 1).toBool();

        if (isDir) {
            navigateTo(url, true);
        } else {
            QDesktopServices::openUrl(url);
        }
    }


    void copySelectionToDirectory(const QList<QUrl> &urls, const QString &directory, const QString &successMessage)
    {
        m_fileActions->copySelectionToDirectory(urls, directory, successMessage);
    }

    void createArchiveFromSelection(const QList<QUrl> &urls, ThisPcArchiveFormat format)
    {
        if (!mutationAllowedByRecoveryGate()) return;
        QString parent;
        if (!m_selectionMenuController.selectionHasCommonParent(urls, &parent)) {
            QMessageBox::information(this, trLocal("Tworzenie archiwum", "Create archive"),
                trLocal("Wybierz pliki i katalogi z jednego katalogu lokalnego.",
                        "Select files and folders from the same local directory."));
            return;
        }
        const QString suffix = thispcArchiveSuffix(format);
        const QString proposed = QDir(parent).filePath(trLocal("Archiwum", "Archive") + suffix);
        const QString label = format == ThisPcArchiveFormat::Zip
            ? trLocal("Archiwa ZIP (*.zip)", "ZIP archives (*.zip)")
            : format == ThisPcArchiveFormat::SevenZip
                ? trLocal("Archiwa 7z (*.7z)", "7z archives (*.7z)")
                : trLocal("Archiwa tar.gz (*.tar.gz)", "tar.gz archives (*.tar.gz)");
        QFileDialog dialog(this, trLocal("Utwórz archiwum — bez nadpisywania", "Create archive — no overwrite"),
                           proposed, label);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setOption(QFileDialog::DontConfirmOverwrite, true);
        dialog.setDefaultSuffix(suffix.mid(1));
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
        QString archivePath = dialog.selectedFiles().first();
        if (!archivePath.endsWith(suffix, Qt::CaseInsensitive)) archivePath += suffix;
        // QFileDialog's overwrite prompt is intentionally disabled; job publication
        // uses RENAME_NOREPLACE as the final protection against races and symlinks.
        if (QFileInfo::exists(archivePath)) {
            QMessageBox::warning(this, trLocal("Tworzenie archiwum", "Create archive"),
                trLocal("Archiwum już istnieje. Wybierz inną nazwę — niczego nie nadpisano.",
                        "The archive exists. Choose a different name — nothing was overwritten."));
            return;
        }
        if (!mutationAllowedByRecoveryGate()) return;
        const QUrl destination = QUrl::fromLocalFile(archivePath);
        auto *job = new ArchiveCreationJob(urls, destination, format, this);
        connect(job, &QObject::destroyed, this, [this, destination] {
            refreshArchiveViews(QUrl::fromLocalFile(QFileInfo(destination.toLocalFile()).absolutePath()));
        });
        watchFileOperation(job,
            trLocal("Archiwum zostało utworzone.", "Archive created."), false,
            trLocal("Tworzenie archiwum", "Archive creation"), [] {});
        job->start();
    }

    // Keep the historic Send to -> ZIP action and its selection semantics.
    void createZipFromSelection(const QList<QUrl> &urls)
    {
        createArchiveFromSelection(urls, ThisPcArchiveFormat::Zip);
    }

    void refreshArchiveViews(const QUrl &destination)
    {
        // Compare the locations visible NOW, not the pane focused at dispatch.
        const QString target = QFileInfo(destination.toLocalFile()).canonicalFilePath();
        if (target.isEmpty()) return;
        const auto affected = [&](const QUrl &url) {
            if (!url.isLocalFile()) return false;
            const QString path = QFileInfo(url.toLocalFile()).canonicalFilePath();
            const QString prefix = target.endsWith(QLatin1Char('/')) ? target : target + QLatin1Char('/');
            return path == target || (!path.isEmpty() && path.startsWith(prefix));
        };
        if (affected(m_navigation.currentUrl())) refreshPane(PaneId::Primary);
        if (m_splitPane && m_splitPane->isVisible() && affected(m_splitPane->currentUrl()))
            refreshPane(PaneId::Split);
    }

    void extractArchiveWithArk(const QUrl &archiveUrl, bool showDialog)
    {
        if (!mutationAllowedByRecoveryGate()) return;
        const QString executable = QStandardPaths::findExecutable(QStringLiteral("ark"));
        if (executable.isEmpty() || !thispcIsArchiveCandidate(archiveUrl, false)) {
            QMessageBox::warning(this, trLocal("Wypakowywanie", "Extraction"),
                executable.isEmpty()
                    ? trLocal("Program Ark nie jest zainstalowany lub jest niedostępny.", "Ark is not installed or is unavailable.")
                    : trLocal("Ten plik nie jest obsługiwanym archiwum.", "This file is not a supported archive."));
            return;
        }
        const QString identity = thispcArchiveIdentity(archiveUrl);
        if (identity.isEmpty()) return;
        if (m_runningArchivePaths.contains(identity)) {
            statusBar()->showMessage(trLocal("To archiwum jest już przetwarzane.",
                                            "This archive is already being processed."), 4000);
            return;
        }
        m_runningArchivePaths.insert(identity); // Includes the destination dialog and hardlink aliases.
        QString destination = QFileInfo(archiveUrl.toLocalFile()).absolutePath();
        if (showDialog) {
            destination = QFileDialog::getExistingDirectory(this,
                trLocal("Wypakuj do — istniejące pliki nie będą nadpisywane",
                        "Extract To — existing files will not be overwritten"), destination);
            if (destination.isEmpty()) { m_runningArchivePaths.remove(identity); return; }
        }
        if (!mutationAllowedByRecoveryGate()) {
            m_runningArchivePaths.remove(identity);
            return;
        }
        const QUrl destinationUrl = QUrl::fromLocalFile(destination);
        auto *job = new ArchiveExtractionJob(archiveUrl, destinationUrl, executable, this);
        // Keep the busy identity until destruction has joined the worker and reaped Ark.
        connect(job, &QObject::destroyed, this, [this, identity, destinationUrl] {
            m_runningArchivePaths.remove(identity);
            refreshArchiveViews(destinationUrl);
        });
        watchFileOperation(job, trLocal("Wypakowywanie zakończone i sprawdzone.", "Extraction completed and verified."),
            false, trLocal("Wypakowywanie", "Extraction"), [] {});
        job->start();
    }

    void selectAllDirectoryItems()
    {
        const auto context = paneContext();
        if (context.isDirectory && context.view) context.view->selectAll();
        updateFileActionStates();
    }


    void pasteClipboardInto(const QUrl &destination)
    {
        m_fileActions->pasteClipboardInto(destination);
    }

    bool ensureAdminProtocol()
    {
        if (adminProtocolAvailable()) {
            return true;
        }

        QMessageBox::information(
            this,
            trLocal(
                "Tryb administratora",
                "Administrator mode"),
            trLocal(
                "Nie znaleziono workera KDE „admin://”. Zainstaluj pakiet „kio-admin”, a następnie uruchom ponownie aplikację.",
                "The KDE “admin://” worker is not installed. Install the “kio-admin” package and restart the application."));
        return false;
    }

    void openAsAdministrator(
        const QUrl &url,
        bool isDirectory)
    {
        const PaneId pane = paneContext().id;
        if (!ensureAdminProtocol()) {
            return;
        }

        QUrl target = url;

        if (!isDirectory) {
            target =
                containingDirectoryForResult(url);
        }

        const QString localPath =
            localPathForFileOrAdmin(target);

        if (localPath.isEmpty()) {
            QMessageBox::information(
                this,
                trLocal(
                    "Tryb administratora",
                    "Administrator mode"),
                trLocal(
                    "Tryb administratora jest dostępny tylko dla lokalnych ścieżek systemu plików.",
                    "Administrator mode is available only for local filesystem paths."));
            return;
        }

        navigatePane(pane, adminUrlForLocalPath(localPath));
    }

    void showPropertiesDialog(
        const QString &name,
        const QUrl &url,
        bool isDir,
        const QString &typeText,
        const QString &sizeText,
        const QString &modifiedText)
    {
        PropertiesDialog::show(
            this,
            name,
            url,
            isDir,
            typeText,
            sizeText,
            modifiedText,
            [this] { return ensureAdminProtocol(); },
            [this](KIO::CopyJob *job) {
                if (m_undoController) {
                    m_undoController->recordCopyJob(job);
                }
            },
            [this](const QString &message, int timeout) {
                statusBar()->showMessage(message, timeout);
            },
            [this] { refreshCurrent(); });
    }

    QUrl containingDirectoryForResult(
        const QUrl &url) const
    {
        if (!url.isValid()) {
            return {};
        }

        QUrl parent = url;

        QString path = parent.path();
        while (path.size() > 1
               && path.endsWith(QLatin1Char('/'))) {
            path.chop(1);
        }

        const int slash =
            path.lastIndexOf(QLatin1Char('/'));

        if (slash < 0) {
            return {};
        }

        path =
            slash == 0
                ? QStringLiteral("/")
                : path.left(slash);

        parent.setPath(path);
        parent.setQuery(QString());
        parent.setFragment(QString());

        return parent;
    }

    void openResultLocation(
        const QUrl &url)
    {
        const QUrl parent =
            containingDirectoryForResult(url);

        if (!parent.isValid()) {
            statusBar()->showMessage(
                trLocal(
                    "Nie udało się ustalić lokalizacji elementu.",
                    "Could not determine the item's location."),
                4000);
            return;
        }

        navigateTo(parent, true);
    }

    void showDirectoryContextMenu(const QPoint &pos)
    {
        showPaneContextMenu(PaneId::Primary, false, pos);
    }

    void showDirectoryDetailsContextMenu(const QPoint &pos)
    {
        showPaneContextMenu(PaneId::Primary, true, pos);
    }

    void showPaneContextMenu(PaneId pane, bool details, const QPoint &pos)
    {
        auto *list = pane == PaneId::Split ? m_splitPane->listView() : m_directoryList;
        auto *tree = pane == PaneId::Split ? m_splitPane->detailsView() : m_directoryDetails;
        QAbstractItemView *view = details ? static_cast<QAbstractItemView *>(tree) : list;
        const QPersistentModelIndex clickedIndex(view->indexAt(pos));
        setActivePane(pane);
        view->setFocus(Qt::MouseFocusReason);
        PaneItem clicked;
        bool hasItem = false;
        if (details) {
            if (auto *item = tree->itemFromIndex(clickedIndex);
                item && item->data(0, directory_view_detail::FileItemRole).toBool()) {
                hasItem = true;
                if (!item->isSelected()) {
                    tree->clearSelection();
                    tree->setCurrentItem(item);
                    item->setSelected(true);
                }
                clicked = {QUrl(item->data(0, Qt::UserRole).toString()),
                    item->text(0), item->text(1), item->text(2), item->text(3),
                    item->data(0, Qt::UserRole + 1).toBool()};
            }
        } else if (const QModelIndex item = list->itemFromIndex(clickedIndex); item.isValid()) {
            hasItem = true;
            if (!list->selectionModel()->isSelected(item)) {
                list->clearSelection();
                list->selectionModel()->setCurrentIndex(
                    item, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
            }
            clicked = {QUrl(item.data(directory_view_detail::UrlRole).toString()),
                item.data(Qt::DisplayRole).toString(),
                item.data(directory_view_detail::TypeTextRole).toString(),
                item.data(directory_view_detail::SizeTextRole).toString(),
                item.data(directory_view_detail::ModifiedTextRole).toString(),
                item.data(directory_view_detail::DirectoryRole).toBool()};
        }
        // Snapshot before QMenu::exec(): focus and directory contents may change.
        const PaneContext context = paneContext();
        const QPoint globalPosition = view->viewport()->mapToGlobal(pos);
        {
            QScopedValueRollback<const PaneContext *> operation(m_operationContext, &context);
            showPaneMenu(context, clicked, hasItem, globalPosition);
        }
        updateFileActionStates();
    }

    void showPaneMenu(const PaneContext &context, const PaneItem &clicked,
                      bool hasItem, const QPoint &globalPosition)
    {
        const ActionAvailability availability =
            ActionStateController::compute(actionStateInput());

        if (!hasItem) {
            QMenu backgroundMenu(this);
            const bool split = context.id == PaneId::Split;
            const SelectionMenuController::ViewState viewState{
                context.id,
                split ? m_splitPane->viewMode() : m_directoryViewMode,
                split ? m_splitPane->iconSizeMode() : m_directoryIconSizeMode,
                split ? m_splitPane->sortKey() : m_sortKey,
                split ? m_splitPane->sortAscending() : m_sortAscending,
                split ? m_splitPane->groupMode() : m_groupMode,
                m_showHiddenFiles,
                m_thumbnailsEnabled,
                m_previewAction,
                m_fullNamesAction};
            const SelectionMenuController::ViewCallbacks viewCallbacks{
                [this, split](int mode) { split ? m_splitPane->setViewMode(mode) : setDirectoryViewMode(mode); },
                [this, split](int mode) { split ? m_splitPane->setIconSizeMode(mode) : setDirectoryIconSizeMode(mode); },
                [this, split](int key) { split ? m_splitPane->setSortState(key, m_splitPane->sortAscending()) : setSortKey(key); },
                [this, split](bool ascending) { split ? m_splitPane->setSortState(m_splitPane->sortKey(), ascending) : setSortAscending(ascending); },
                [this, split](int mode) { split ? m_splitPane->setGroupMode(mode) : setGroupMode(mode); },
                [this](bool value) { setShowHiddenFiles(value); },
                [this](bool value) { setThumbnailsEnabled(value); }};
            const bool quickAccess = canQuickAccessLocation(context.directory);
            m_paneMenuController.buildBackgroundMenu(backgroundMenu,
                {context.directory, availability, viewState, quickAccess,
                 quickAccess && isQuickAccessPinned(context.directory),
                 context.directory.isLocalFile(), context.directory.isLocalFile()},
                {viewCallbacks,
                 [this, pane = context.id] { refreshPane(pane); },
                 [this] { selectAllDirectoryItems(); },
                 [this] { createNewFolder(); },
                 [this] { pasteClipboard(); },
                 [this, directory = context.directory] { emptyTrashAt(directory); },
                 [this, directory = context.directory] { openInDolphin(directory); },
                 [this, directory = context.directory] { createNewTab(directory, true); },
                 [this, directory = context.directory] { openInNewWindow(directory); },
                 [this, pane = context.id, directory = context.directory] {
                     openInOtherPane(pane, directory);
                 },
                 [this, directory = context.directory] { toggleQuickAccessLocation(directory); },
                 [this, directory = context.directory] { openTerminalAt(directory); },
                 [this, directory = context.directory] { openAsAdministrator(directory, true); }});
            backgroundMenu.exec(globalPosition);
            return;
        }

        const QUrl url = clicked.url;
        const bool isDir = clicked.isDir;
        const QList<QUrl> selected = selectedUrls();
        const bool single = selected.size() == 1;
        QMenu menu(this);
        const bool quickAccess = single && isDir && canQuickAccessLocation(url);
        const QMimeData *mime = QApplication::clipboard()->mimeData();
        m_paneMenuController.buildItemMenu(menu,
            {url, selected, availability, isDir, isSearchLocation(context.directory),
             quickAccess, quickAccess && isQuickAccessPinned(url),
             single && thispcCanExtractArchive(url, isDir),
             m_selectionMenuController.canPrintUrl(url, isDir),
             m_selectionMenuController.isLocalImageUrl(url, isDir),
             m_selectionMenuController.canSetWallpaper(url, isDir),
             isDir && mime && mime->hasUrls(), isDir && url.isLocalFile(),
             single && url.isLocalFile()},
            {[this, pane = context.id, url, isDir] {
                 if (isDir)
                     navigatePane(pane, url);
                 else
                     QDesktopServices::openUrl(url);
             },
             [this, url] { createNewTab(url, true); },
             [this, url] { openInNewWindow(url); },
             [this, pane = context.id, url] { openInOtherPane(pane, url); },
             [this, url] { toggleQuickAccessLocation(url); },
             [this, url] { openInDolphin(url); },
             [this, url] { openResultLocation(url); },
             [this, url] { extractArchiveWithArk(url, false); },
             [this, url] { extractArchiveWithArk(url, true); },
             [this, url] { m_selectionMenuController.printUrl(url); },
             [this, url] {
                 m_selectionMenuController.setAsDesktopWallpaper(url,
                     [this](const QString &message) { statusBar()->showMessage(message, 4000); });
             },
             {[this](const QList<QUrl> &urls, const QString &directory, const QString &message) {
                  copySelectionToDirectory(urls, directory, message);
              },
              [this](const QList<QUrl> &urls) { createZipFromSelection(urls); },
              [this](const QList<QUrl> &urls) { createArchiveFromSelection(urls, ThisPcArchiveFormat::SevenZip); },
              [this](const QList<QUrl> &urls) { createArchiveFromSelection(urls, ThisPcArchiveFormat::TarGzip); }},
             [this, url] { openTerminalAt(url); },
             [this, url, isDir] { openAsAdministrator(url, isDir); },
             [this] { putSelectionOnClipboard(true); },
             [this] { putSelectionOnClipboard(false); },
             [this] { renameSelected(); },
             [this] { batchRenameSelected(); },
             [this] { trashSelected(); },
             [this, url] { pasteClipboardInto(url); },
             [url] { QGuiApplication::clipboard()->setText(urlForDisplay(url)); },
             [this, clicked] {
                 showPropertiesDialog(clicked.name, clicked.url, clicked.isDir,
                                      clicked.type, clicked.size, clicked.modified);
             }});
        menu.exec(globalPosition);
        updateFileActionStates();
    }


    QList<QUrl> selectedUrls() const
    {
        return m_operationContext
            ? [&] {
                QList<QUrl> urls;
                for (const PaneItem &item : m_operationContext->items)
                    if (item.url.isValid()) urls.push_back(item.url);
                return urls;
            }()
            : (m_paneAdapter ? m_paneAdapter->selectedUrls(m_activePane) : QList<QUrl>{});
    }

    bool canModifyCurrentDirectory() const
    {
        return ActionStateController::compute(actionStateInput(false)).canModifyCurrentDirectory;
    }

    bool canPasteHere() const
    {
        return ActionStateController::compute(actionStateInput(false)).canPasteHere;
    }

    ActionStateInput actionStateInput(bool includeRuntimeCapabilities = true) const
    {
        const PaneContext context = paneContext();
        const QMimeData *mime = QApplication::clipboard()->mimeData();
        const bool clipboardHasUrls = mime
            && mime->hasUrls()
            && !mime->urls().isEmpty();
        const bool trashRoot = FileActions::isTrashRoot(context.directory);

        ActionStateInput input;
        input.directory = context.directory;
        input.selection = selectedUrls();
        input.isDirectory = context.isDirectory;
        input.isThisPcLocation = sameLocation(context.directory, kThisPcUrl);
        input.isSearchLocation = ::isSearchLocation(context.directory);
        input.isTrashLocation = context.directory.scheme() == QStringLiteral("trash");
        input.isTrashRoot = trashRoot;
        input.clipboardHasUrls = clipboardHasUrls;
        if (includeRuntimeCapabilities) {
            auto &recoveryGate = BatchRenameRecoveryGate::instance();
            recoveryGate.refresh();
            input.recoverySafe = !recoveryGate.mutationsBlocked();
            input.emptyTrashAvailable = trashRoot
                && m_fileActions
                && m_fileActions->canEmptyTrash(context.directory);
        }
        return input;
    }

    void updateFileActionStates()
    {
        if (!m_copyAction) {
            return;
        }

        if (m_backAction && m_contentStack) updateNavigationActions();
        const bool split = paneContext().id == PaneId::Split;
        const int mode = split ? m_splitPane->viewMode() : m_directoryViewMode;
        const int iconSizeMode = split
            ? m_splitPane->iconSizeMode()
            : m_directoryIconSizeMode;
        const int sort = split ? m_splitPane->sortKey() : m_sortKey;
        const bool ascending = split
            ? m_splitPane->sortAscending()
            : m_sortAscending;
        if (m_viewButton) {
            static const QStringList icons = {
                QStringLiteral("view-list-icons"),
                QStringLiteral("view-list-text"),
                QStringLiteral("view-list-details"),
                QStringLiteral("view-list-tree")
            };
            m_viewButton->setIcon(themedIcon(icons.at(mode)));
        }
        if (m_sortButton) {
            m_sortButton->setIcon(themedIcon(
                ascending
                    ? QStringLiteral("view-sort-ascending")
                    : QStringLiteral("view-sort-descending")));
        }
        if (m_viewButton && m_viewButton->menu()) {
            for (auto *action : m_viewButton->menu()->actions()) {
                if (action->data().isValid()) {
                    QSignalBlocker blocker(action);
                    action->setChecked(action->data().toInt() == mode);
                }
            }
            if (QMenu *iconSizeMenu = m_viewButton->menu()->findChild<QMenu *>(
                    QStringLiteral("viewIconSizeMenu"))) {
                for (QAction *action : iconSizeMenu->actions()) {
                    QSignalBlocker blocker(action);
                    action->setChecked(
                        action->property("iconSizeMode").toInt() == iconSizeMode);
                }
            }
        }
        if (m_sortButton && m_sortButton->menu()) {
            for (auto *action : m_sortButton->menu()->actions()) {
                if (action->data().isValid()) {
                    QSignalBlocker blocker(action);
                    action->setChecked(action->data().toInt() == sort);
                }
            }
        }

        const ActionAvailability state =
            ActionStateController::compute(actionStateInput());
        updateRecoveryFencePresentation();

        m_copyAction->setEnabled(state.copyEnabled);
        m_cutAction->setEnabled(state.cutEnabled);
        m_renameAction->setEnabled(state.renameEnabled);
        m_batchRenameAction->setEnabled(state.batchRenameEnabled);
        if (m_propertiesAction) m_propertiesAction->setEnabled(state.propertiesEnabled);
        m_trashAction->setEnabled(state.trashEnabled);

        if (m_emptyTrashAction) {
            m_emptyTrashAction->setVisible(state.emptyTrashVisible);
            m_emptyTrashAction->setEnabled(state.emptyTrashEnabled);
        }

        m_newFolderAction->setEnabled(state.createEnabled);
        if (m_newTextFileAction) {
            m_newTextFileAction->setEnabled(state.createEnabled);
        }
        if (m_newMarkdownAction) {
            m_newMarkdownAction->setEnabled(state.createEnabled);
        }
        if (m_newEmptyFileAction) {
            m_newEmptyFileAction->setEnabled(state.createEnabled);
        }
        if (m_templateMenu) {
            m_templateMenu->menuAction()->setEnabled(state.createEnabled);
        }
        if (m_newButton) {
            m_newButton->setEnabled(state.createEnabled);
        }
        m_pasteAction->setEnabled(state.pasteEnabled);

        if (m_viewButton) {
            m_viewButton->setEnabled(state.viewControlsEnabled);
        }

        if (m_sortButton) {
            m_sortButton->setEnabled(state.viewControlsEnabled);
        }

        if (m_openDolphinAction) {
            m_openDolphinAction->setEnabled(state.openDolphinEnabled);
        }
    }

    void putSelectionOnClipboard(bool cut)
    {
        const QList<QUrl> urls = selectedUrls();

        if (urls.isEmpty()) {
            return;
        }

        auto *mimeData = new QMimeData;
        mimeData->setUrls(urls);

        // This is the KDE convention understood by file managers such as
        // Dolphin. "1" means cut/move, "0" means normal copy.
        mimeData->setData(
            QStringLiteral("application/x-kde-cutselection"),
            cut ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));

        QApplication::clipboard()->setMimeData(mimeData);

        statusBar()->showMessage(
            cut
                ? (isPolish()
                    ? QStringLiteral("Wycięto do schowka: %1 elementów")
                        .arg(urls.size())
                    : QStringLiteral("Cut to clipboard: %1 items")
                        .arg(urls.size()))
                : (isPolish()
                    ? QStringLiteral("Skopiowano do schowka: %1 elementów")
                        .arg(urls.size())
                    : QStringLiteral("Copied to clipboard: %1 items")
                        .arg(urls.size())),
            4000);

        updateFileActionStates();
    }

    void pasteClipboard()
    {
        const QUrl destination = paneContext().directory;
        if (canPasteHere()) pasteClipboardInto(destination);
    }

    void createNewFile(const QString &suggestedName, const QByteArray &contents)
    {
        if (canModifyCurrentDirectory())
            m_fileActions->createNewFile(paneContext().directory, suggestedName, contents);
    }

    void createFromTemplate(const QUrl &source)
    {
        if (canModifyCurrentDirectory())
            m_fileActions->createFromTemplate(paneContext().directory, source);
    }

    void createNewFolder()
    {
        if (canModifyCurrentDirectory())
            m_fileActions->createNewFolder(paneContext().directory);
    }

    void renameSelected()
    {
        const auto context = paneContext();
        m_fileActions->renameSelected(selectedUrls(), context.items.isEmpty() ? QString() : context.items.first().name);
    }

    void batchRenameSelected()
    {
        const PaneContext context = paneContext();
        QList<QUrl> urls;
        for (const PaneItem &item : context.items) urls.push_back(item.url);
        if (m_fileActions) m_fileActions->batchRenameSelected(urls);
    }

    void trashSelected()
    {
        m_fileActions->trashSelected(selectedUrls());
    }

    void emptyTrashAt(const QUrl &directory)
    {
        if (m_emptyTrashAction) m_emptyTrashAction->setEnabled(false);
        m_fileActions->emptyTrash(directory, [this] { refreshTrashViews(); });
        updateFileActionStates();
    }

    void refreshTrashViews()
    {
        // The modal dialog and the asynchronous job can both outlive a pane
        // switch or navigation. Refresh only views still displaying Trash.
        if (m_navigation.currentUrl().scheme() == QStringLiteral("trash")) {
            refreshPane(PaneId::Primary);
            if (m_primaryPane->listingJob()) {
                // The listing completes later; keep the operation result
                // visible instead of immediately replacing it with trash:/.
                m_primaryPane->listingJob()->setProperty("thispcPreserveStatusMessage", true);
            }
        }
        if (m_splitPane && m_splitPane->isVisible()
            && m_splitPane->currentUrl().scheme() == QStringLiteral("trash")) {
            refreshPane(PaneId::Split);
        }
    }

    void watchFileOperation(
        KJob *job,
        const QString &successMessage,
        bool clearClipboardOnSuccess = false,
        const QString &operationTitle = QString(),
        FileActions::RefreshViews refreshViews = {})
    {
        if (!job) {
            return;
        }

        if (!refreshViews) {
            refreshViews = [this] {
                refreshCurrent();
                if (m_splitPane && m_splitPane->isVisible()) m_splitPane->refresh();
            };
        }

        if (m_operationManager) {
            m_operationManager->track(
                job,
                operationTitle.isEmpty()
                    ? trLocal("Operacja plikowa", "File operation")
                    : operationTitle);
        }

        statusBar()->showMessage(
            trLocal("Trwa operacja…", "Operation in progress…"));

        connect(
            job,
            &KJob::result,
            this,
            [this, job, successMessage, clearClipboardOnSuccess, refreshViews](KJob *) {
            const bool cancelled =
                (m_operationManager
                    && m_operationManager->cancelRequested(job))
                || job->error() == KJob::KilledJobError
                || job->error() == KIO::ERR_USER_CANCELED;

            const QString operationError =
                job->error() ? job->errorString() : QString();

            if (m_operationManager) {
                m_operationManager->finish(
                    job,
                    job->error() == 0,
                    cancelled,
                    operationError);
            }

            if (cancelled) {
                statusBar()->showMessage(
                    trLocal("Operacja anulowana", "Operation cancelled"),
                    4000);

                refreshViews();
                updateFileActionStates();
                return;
            }

            if (job->error()) {
                statusBar()->showMessage(
                    job->errorString(),
                    7000);

                QMessageBox::warning(
                    this,
                    trLocal("Operacja nie powiodła się", "Operation failed"),
                    job->errorString());

                refreshViews();
                updateFileActionStates();
                return;
            }

            if (clearClipboardOnSuccess) {
                QApplication::clipboard()->clear();
            }

            statusBar()->showMessage(
                successMessage,
                4000);

            refreshViews();
            updateFileActionStates();
        });
    }

    void setShowHiddenFiles(bool show)
    {
        if (m_showHiddenFiles == show) {
            return;
        }

        m_showHiddenFiles = show;
        m_searchController->setShowHiddenFiles(show);

        if (m_showHiddenAction
            && m_showHiddenAction->isChecked() != show) {
            m_showHiddenAction->blockSignals(true);
            m_showHiddenAction->setChecked(show);
            m_showHiddenAction->blockSignals(false);
        }

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/showHidden"),
            show);

        if (m_contentStack
            && m_contentStack->currentWidget()
                == m_directoryPage) {
            if (isSearchLocation(m_navigation.currentUrl())) loadSearchLocation(m_navigation.currentUrl());
            else loadDirectory(m_navigation.currentUrl());
        }
        if (m_splitPane) m_splitPane->setDisplayOptions(m_showHiddenFiles, m_thumbnailsEnabled);
    }

    void setThumbnailsEnabled(bool enabled)
    {
        if (m_thumbnailsEnabled == enabled) {
            return;
        }

        m_thumbnailsEnabled = enabled;

        if (m_thumbnailsAction
            && m_thumbnailsAction->isChecked() != enabled) {
            m_thumbnailsAction->blockSignals(true);
            m_thumbnailsAction->setChecked(enabled);
            m_thumbnailsAction->blockSignals(false);
        }

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/thumbnails"),
            enabled);

        if (!enabled) {
            m_primaryPane->clearThumbnailCache();
        }

        if (m_contentStack
            && m_contentStack->currentWidget()
                == m_directoryPage) {
            renderDirectoryItems();
        }
        if (m_splitPane) m_splitPane->setDisplayOptions(m_showHiddenFiles, m_thumbnailsEnabled);
    }

    void setAlwaysShowFullNames(bool enabled)
    {
        m_alwaysShowFullNames = enabled;

        if (m_fullNamesAction
            && m_fullNamesAction->isChecked() != enabled) {
            QSignalBlocker blocker(m_fullNamesAction);
            m_fullNamesAction->setChecked(enabled);
        }

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/alwaysShowFullNames"),
            enabled);

        if (m_directoryList) {
            m_directoryList->setAlwaysShowFullNames(enabled);
        }
        if (m_splitPane) {
            m_splitPane->setAlwaysShowFullNames(enabled);
        }
    }

    void handleDroppedUrls(
        const QList<QUrl> &urls,
        const QUrl &destination,
        const QPoint &globalPosition,
        Qt::KeyboardModifiers modifiers)
    {
        if (urls.isEmpty() || !destination.isValid()) {
            return;
        }

        if (dropWouldCreateCycle(urls, destination)) {
            QMessageBox::warning(
                this,
                trLocal(
                    "Nieprawidłowe miejsce docelowe",
                    "Invalid destination"),
                trLocal(
                    "Nie można skopiować ani przenieść folderu do niego samego lub do jednego z jego podfolderów.",
                    "A folder cannot be copied or moved into itself or one of its subfolders."));
            return;
        }

        const bool forcedAction =
            modifiers.testFlag(Qt::ControlModifier)
            || modifiers.testFlag(Qt::ShiftModifier);

        Qt::DropAction action =
            dropActionForUrls(urls, destination, modifiers);

        if (!forcedAction) {
            QMenu menu(this);

            QAction *copyHere = menu.addAction(
                themedIcon(QStringLiteral("edit-copy")),
                trLocal("Kopiuj tutaj", "Copy here"));

            QAction *moveHere = menu.addAction(
                themedIcon(QStringLiteral("go-jump")),
                trLocal("Przenieś tutaj", "Move here"));

            menu.setDefaultAction(
                action == Qt::MoveAction
                    ? moveHere
                    : copyHere);

            menu.addSeparator();
            QAction *cancel = menu.addAction(
                trLocal("Anuluj", "Cancel"));

            QAction *chosen = menu.exec(globalPosition);
            if (!chosen || chosen == cancel) {
                return;
            }

            action = chosen == moveHere
                ? Qt::MoveAction
                : Qt::CopyAction;
        }

        m_fileActions->transfer(urls, destination, action);
    }

    void setDirectoryViewMode(int mode)
    {
        const DirectorySelectionSnapshot selection =
            captureDirectorySelection(
                m_directoryList, m_directoryDetails, m_directoryViewMode == 2);
        m_directoryViewMode =
            std::clamp(mode, 0, 3);

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/viewMode"),
            m_directoryViewMode);
        DirectoryViewSettings::setViewMode(
            m_navigation.currentUrl(),
            m_directoryViewMode);

        applyDirectoryViewMode(false);
        restoreDirectorySelection(m_directoryList, m_directoryDetails, selection);
    }

    void setDirectoryIconSizeMode(int mode)
    {
        m_directoryIconSizeMode = std::clamp(mode, 0, 3);
        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/iconSizeMode"),
            m_directoryIconSizeMode);
        DirectoryViewSettings::setIconSizeMode(
            m_navigation.currentUrl(),
            m_directoryIconSizeMode);
        applyDirectoryViewMode(false);
    }

    void setGroupMode(int mode)
    {
        m_groupMode = std::clamp(
            mode, DirectoryViewSettings::NoGrouping, DirectoryViewSettings::GroupBySize);
        DirectoryViewSettings::setGroupMode(m_navigation.currentUrl(), m_groupMode);
        if (m_contentStack && m_contentStack->currentWidget() == m_directoryPage) {
            renderDirectoryItems();
        }
        scheduleDateGroupingRefresh();
    }

    void scheduleDateGroupingRefresh()
    {
        m_dateGroupingTimer.stop();
        if (m_groupMode != DirectoryViewSettings::GroupByDate) return;
        m_dateGroupingTimer.start(
            directory_view_detail::millisecondsUntilNextLocalDay());
    }

    void applyDirectoryViewMode(bool saveSetting)
    {
        if (!m_directoryList
            || !m_directoryDetails
            || !m_directoryViewStack) {
            return;
        }

        if (saveSetting) {
            QSettings settings;
            settings.setValue(
                QStringLiteral("directory/viewMode"),
                m_directoryViewMode);
        }

        applyDirectoryViewLayout(
            m_directoryList,
            m_directoryDetails,
            m_directoryViewStack,
            m_viewButton,
            m_directoryViewMode,
            m_directoryIconSizeMode);

        updateFileActionStates();
    }

    void setSortKey(int key)
    {
        m_sortKey = std::clamp(key, 0, 3);

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/sortKey"),
            m_sortKey);

        if (m_contentStack
            && m_contentStack->currentWidget()
                == m_directoryPage) {
            renderDirectoryItems();
        }
    }

    void setSortAscending(bool ascending)
    {
        m_sortAscending = ascending;

        QSettings settings;
        settings.setValue(
            QStringLiteral("directory/sortAscending"),
            m_sortAscending);

        if (m_sortButton) {
            m_sortButton->setIcon(
                themedIcon(
                    m_sortAscending
                        ? QStringLiteral("view-sort-ascending")
                        : QStringLiteral("view-sort-descending")));
        }

        if (m_contentStack
            && m_contentStack->currentWidget()
                == m_directoryPage) {
            renderDirectoryItems();
        }
    }

    QString displayNameForLocation(const QUrl &url) const
    {
        return LocationPresentation::primaryTitle(url, m_driveHomeCoordinator.drives());
    }

    void rebuildBreadcrumbs()
    {
        clearLayout(m_breadcrumbLayout);
        m_breadcrumbFrame->setToolTip(urlForDisplay(m_navigation.currentUrl()));

        auto addCrumb =
            [this](const QString &text,
                   const QIcon &icon,
                   const QUrl &url) {
            auto *button =
                new QToolButton(m_breadcrumbFrame->contentsWidget());

            button->setObjectName(
                QStringLiteral("crumbButton"));
            button->setCursor(Qt::PointingHandCursor);
            button->setText(text);
            button->setToolTip(urlForDisplay(url));
            button->setIcon(icon);
            button->setToolButtonStyle(
                icon.isNull()
                    ? Qt::ToolButtonTextOnly
                    : Qt::ToolButtonTextBesideIcon);

            connect(
                button,
                &QToolButton::clicked,
                this,
                [this, url] {
                    setActivePane(PaneId::Primary);
                    if (sameLocation(url, m_navigation.currentUrl())) {
                        beginAddressEdit(PaneId::Primary);
                    } else {
                        navigateTo(url, true);
                    }
                });

            m_breadcrumbLayout->addWidget(button);
        };

        auto addSeparator =
            [this] {
            auto *sep =
                new QLabel(QStringLiteral("›"), m_breadcrumbFrame->contentsWidget());
            sep->setForegroundRole(
                QPalette::PlaceholderText);
            m_breadcrumbLayout->addWidget(sep);
        };

        if (sameLocation(m_navigation.currentUrl(), kThisPcUrl)) {
            addCrumb(
                trLocal("Ten komputer", "This PC"),
                themedIcon(QStringLiteral("computer")),
                kThisPcUrl);

            m_breadcrumbLayout->addStretch(1);
            return;
        }

        if (m_navigation.currentUrl().isLocalFile()) {
            const auto segments = LocationPresentation::localPathSegments(
                m_navigation.currentUrl(),
                m_driveHomeCoordinator.drives());
            for (qsizetype index = 0; index < segments.size(); ++index) {
                if (index > 0) addSeparator();
                const auto &segment = segments.at(index);
                addCrumb(
                    segment.text,
                    segment.iconName.isEmpty()
                        ? QIcon()
                        : themedIcon(segment.iconName),
                    segment.url);
            }
        } else {
            QString rootLabel =
                m_navigation.currentUrl().scheme();

            if (isSearchLocation(m_navigation.currentUrl())) {
                addCrumb(
                    trLocal(
                        "Ten komputer",
                        "This PC"),
                    themedIcon(QStringLiteral("computer")),
                    kThisPcUrl);
                addSeparator();
                addCrumb(
                    displayNameForLocation(m_navigation.currentUrl()),
                    themedIcon(QStringLiteral("edit-find")),
                    m_navigation.currentUrl());
                m_breadcrumbLayout->addStretch(1);
                return;
            }

            if (m_navigation.currentUrl().scheme()
                == QStringLiteral("trash")) {
                rootLabel = trLocal("Kosz", "Trash");
            } else if (
                m_navigation.currentUrl().scheme()
                == QStringLiteral("remote")) {
                rootLabel = trLocal("Sieć", "Network");
            }

            QUrl rootUrl = m_navigation.currentUrl();
            rootUrl.setPath(QStringLiteral("/"));

            addCrumb(
                rootLabel,
                themedIcon(QStringLiteral("folder")),
                rootUrl);

            QString cumulativePath;

            const QStringList parts =
                m_navigation.currentUrl().path().split(
                    QLatin1Char('/'),
                    Qt::SkipEmptyParts);

            for (const QString &part : parts) {
                addSeparator();

                cumulativePath +=
                    QLatin1Char('/') + part;

                QUrl crumbUrl = m_navigation.currentUrl();
                crumbUrl.setPath(cumulativePath);

                addCrumb(
                    part,
                    QIcon(),
                    crumbUrl);
            }
        }

        m_breadcrumbLayout->addStretch(1);
    }

    void updateNavigationActions()
    {
        const bool split = paneContext().id == PaneId::Split;
        m_backAction->setEnabled(split ? m_splitPane->canGoBack() : m_navigation.canGoBack());
        m_forwardAction->setEnabled(split ? m_splitPane->canGoForward()
            : m_navigation.canGoForward());
        m_upAction->setEnabled(split ? m_splitPane->canGoUp()
            : m_navigation.canGoUp());
    }

    void updateSidebarCurrent()
    {
        if (!m_sidebar) {
            return;
        }

        const bool split = m_activePane == PaneId::Split
            && m_splitPane && m_splitPane->isVisible();
        QUrl effectiveLocation = split
            ? m_splitPane->currentUrl()
            : m_navigation.currentUrl();
        if (isSearchLocation(effectiveLocation)) {
            if (searchIntParameter(effectiveLocation, QStringLiteral("scope"), 2) == 2) {
                effectiveLocation = kThisPcUrl;
            } else {
                const QUrl base = searchBaseFromUrl(effectiveLocation);
                if (base.isValid()) {
                    effectiveLocation = base;
                }
            }
        }

        m_sidebar->setCurrentLocation(effectiveLocation);
    }

private Q_SLOTS:
    void goBack()
    {
        const QUrl previousUrl = m_navigation.currentUrl();
        const QUrl url = m_navigation.back();
        if (!url.isValid()) return;
        recordRecentLocation(url);
        loadLocation(url, !sameLocation(previousUrl, url));
    }

    void goForward()
    {
        const QUrl previousUrl = m_navigation.currentUrl();
        const QUrl url = m_navigation.forward();
        if (!url.isValid()) return;
        recordRecentLocation(url);
        loadLocation(url, !sameLocation(previousUrl, url));
    }

    void goUp()
    {
        const QUrl previousUrl = m_navigation.currentUrl();
        const QUrl parent = m_navigation.up(m_driveHomeCoordinator.drives());

        if (parent.isValid()) {
            recordRecentLocation(parent);
            loadLocation(parent, !sameLocation(previousUrl, parent));
        }
    }

    void refreshCurrent(bool preserveStatusMessage = false)
    {
        reloadDrives();

        if (sameLocation(
                m_navigation.currentUrl(),
                kThisPcUrl)) {
            return;
        }

        if (isSearchLocation(m_navigation.currentUrl())) {
            loadSearchLocation(m_navigation.currentUrl());
        } else {
            loadDirectory(m_navigation.currentUrl(), preserveStatusMessage);
        }
    }

    void reloadDrives()
    {
        m_driveHomeCoordinator.refresh();
    }

#ifdef THISPC_TEST_HARNESS
public:
#else
private:
#endif
    void recordRecoveryTestPresentation()
    {
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
        const QString path = QString::fromLocal8Bit(qgetenv("THISPC_RECOVERY_UI_TRACE"));
        if (path.isEmpty() || !m_recoveryStatusLabel) return;
        const QByteArray line = m_recoveryStatusLabel->text().toUtf8() + '\n';
        QFile trace(path);
        if (trace.open(QIODevice::WriteOnly | QIODevice::Append)) trace.write(line);
#endif
    }

    void updateRecoveryFencePresentation()
    {
        auto &gate = BatchRenameRecoveryGate::instance();
        const QString previousRecoveryMessage = m_recoveryStatusLabel
            ? m_recoveryStatusLabel->text()
            : QString();
        gate.refresh();
        if (!m_recoveryStatusLabel) return;
        m_recoveryStatusLabel->setText(gate.message());
        m_recoveryStatusLabel->setToolTip(gate.message());
        m_recoveryStatusLabel->setVisible(gate.mutationsBlocked());
        recordRecoveryTestPresentation();
        if (gate.mutationsBlocked()) {
            m_recoverySuccessShown = false;
            statusBar()->showMessage(gate.message(), 0);
        } else if (!m_recoverySuccessShown) {
            // Consume this completion exactly once even when a newer status is
            // currently visible. Otherwise the one-second refresh timer can
            // resurrect the success notice after its timeout or clobber a
            // later operation message.
            m_recoverySuccessShown = true;
            if (statusBar()->currentMessage().isEmpty()
                || statusBar()->currentMessage() == previousRecoveryMessage
                || statusBar()->currentMessage().contains(
                    QStringLiteral("Checking crash recovery"), Qt::CaseInsensitive)) {
                statusBar()->showMessage(
                    trLocal(
                        "Sprawdzanie odzyskiwania po awarii zakończone. Historia Cofnij/Ponów sprzed ponownego uruchomienia nie jest przywracana.",
                        "Crash recovery check completed. Undo/Redo history from before restart is not restored."),
                    6000);
            }
        }
    }

    bool mutationAllowedByRecoveryGate()
    {
        auto &gate = BatchRenameRecoveryGate::instance();
        gate.refresh();
        if (!gate.mutationsBlocked()) {
            updateRecoveryFencePresentation();
            return true;
        }
        if (m_recoveryStatusLabel) {
            m_recoveryStatusLabel->setText(gate.message());
            m_recoveryStatusLabel->setToolTip(gate.message());
            m_recoveryStatusLabel->show();
        }
        statusBar()->showMessage(gate.message(), 0);
        return false;
    }

    void rebuildDriveGrid()
    {
        rebuildDriveGrid(PaneId::Primary, m_homePage, m_homeStatus, m_drivesGrid);
        rebuildDriveGrid(PaneId::Split, m_splitHomePage, m_splitHomeStatus, m_splitDrivesGrid);
        m_splitPane->setDrives(m_driveHomeCoordinator.drives());
    }

    void rebuildDriveGrid(PaneId pane, QWidget *homePage, QLabel *homeStatus, QGridLayout *drivesGrid)
    {
        clearLayout(drivesGrid);

        const QList<DriveInfo> &drives = m_driveHomeCoordinator.drives();
        if (drives.isEmpty()) {
            homeStatus->setText(
                trLocal(
                    "Nie znaleziono dysków.",
                    "No drives found."));
            return;
        }

        homeStatus->clear();

        for (int i = 0;
             i < drives.size();
             ++i) {
            DriveFrame *card =
                makeDriveCard(
                    drives.at(i),
                    homePage);

            connect(
                card,
                &ClickableFrame::activated,
                this,
                [this, pane](const QUrl &url) {
                    setActivePane(pane);
                    navigatePane(pane, url);
                });

            drivesGrid->addWidget(
                card,
                i / 2,
                i % 2);
        }
    }


#ifdef THISPC_TEST_HARNESS
public:
#else
private:
#endif
    QAction *m_backAction = nullptr;
    QAction *m_forwardAction = nullptr;
    QAction *m_upAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_openDolphinAction = nullptr;

    QToolButton *m_newButton = nullptr;
    QAction *m_newFolderAction = nullptr;
    QAction *m_newTextFileAction = nullptr;
    QAction *m_newMarkdownAction = nullptr;
    QAction *m_newEmptyFileAction = nullptr;
    TemplateMenu *m_templateMenu = nullptr;
    QAction *m_cutAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_renameAction = nullptr;
    QAction *m_batchRenameAction = nullptr;
    QAction *m_propertiesAction = nullptr;
    QAction *m_trashAction = nullptr;
    QAction *m_emptyTrashAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    UndoController *m_undoController = nullptr;

    QToolButton *m_viewButton = nullptr;
    QToolButton *m_sortButton = nullptr;
    QAction *m_splitViewAction = nullptr;
    QAction *m_swapPanesAction = nullptr;
    QAction *m_comparePanesAction = nullptr;
    QAction *m_showHiddenAction = nullptr;
    QAction *m_thumbnailsAction = nullptr;
    QAction *m_previewAction = nullptr;
    QAction *m_fullNamesAction = nullptr;
    QAction *m_restoreSessionAction = nullptr;
    int m_directoryViewMode = 0;
    int m_directoryIconSizeMode = DirectoryViewSettings::DefaultIconSizeMode;
    int m_sortKey = 0;
    bool m_sortAscending = true;
    int m_groupMode = DirectoryViewSettings::NoGrouping;
    QTimer m_dateGroupingTimer;
    bool m_showHiddenFiles = false;
    bool m_thumbnailsEnabled = true;
    bool m_alwaysShowFullNames = false;
    bool m_restorePreviousSession = true;

    QStackedWidget *m_addressStack = nullptr;
    BreadcrumbFrame *m_breadcrumbFrame = nullptr;
    QHBoxLayout *m_breadcrumbLayout = nullptr;
    AddressLineEdit *m_addressEdit = nullptr;
    QAction *m_searchScopeAction = nullptr;
    QMenu *m_searchScopeMenu = nullptr;
    QToolButton *m_searchFilterButton = nullptr;
    QActionGroup *m_searchScopeGroup = nullptr;
    QActionGroup *m_searchTypeGroup = nullptr;
    QActionGroup *m_searchDateGroup = nullptr;
    QActionGroup *m_searchSizeGroup = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QAction *m_stopSearchAction = nullptr;
    PaneSearchState m_primarySearch;

    SidebarPanel *m_sidebar = nullptr;
    QSplitter *m_sidebarSplitter = nullptr;
    QScrollArea *m_sidebarScrollArea = nullptr;
    int m_preferredSidebarWidth = 235;

    QFrame *m_tabStrip = nullptr;
    ExplorerTabBar *m_tabBar = nullptr;
    QToolButton *m_newTabButton = nullptr;
    TabController m_tabController;
    // Transitional aliases keep the composition-root integration small while
    // TabController owns all tab/session state and state transitions.
    QList<TabState> &m_tabs = m_tabController.tabs();
    QList<TabState> &m_closedTabs = m_tabController.closedTabs();
    int &m_activeTab = m_tabController.activeIndexRef();
    bool m_tabChangeInProgress = false;
    bool m_tabRestoreInProgress = false;

    QSplitter *m_contentSplitter = nullptr;
    QSplitter *m_previewSplitter = nullptr;
    PreviewPane *m_previewPane = nullptr;
    QuickLookOverlay *m_quickLook = nullptr;
    PreviewCoordinator *m_previewCoordinator = nullptr;
    QByteArray m_splitterState;
    PrimaryBrowserPane *m_primaryPane = nullptr;
    SplitBrowserPane *m_splitPane = nullptr;
    QStackedWidget *m_contentStack = nullptr;

    OperationManager *m_operationManager = nullptr;
    QLabel *m_versionLabel = nullptr;
    QLabel *m_recoveryStatusLabel = nullptr;
    QFutureWatcher<void> *m_recoveryStartupWatcher = nullptr;
    bool m_recoverySuccessShown = false;

    QWidget *m_splitHomePage = nullptr;
    QLabel *m_splitHomeStatus = nullptr;
    QGridLayout *m_splitDrivesGrid = nullptr;
    QWidget *m_homePage = nullptr;
    QLabel *m_homeStatus = nullptr;
    QGridLayout *m_drivesGrid = nullptr;

    QWidget *m_directoryPage = nullptr;
    QFrame *m_adminBanner = nullptr;
    QLabel *m_directoryTitle = nullptr;
    QLabel *m_directoryStatus = nullptr;
    QFrame *m_searchProgressFrame = nullptr;
    QProgressBar *m_searchProgressBar = nullptr;
    QPushButton *m_cancelSearchButton = nullptr;
    QStackedWidget *m_directoryViewStack = nullptr;
    DirectoryListWidget *m_directoryList = nullptr;
    DirectoryTreeWidget *m_directoryDetails = nullptr;

    QSet<QString> m_runningArchivePaths;
    NavigationHistory m_navigation;

    QTimer m_refreshTimer;
    DriveHomeCoordinator m_driveHomeCoordinator{this};

    FileActions *m_fileActions = nullptr;
    SelectionMenuController m_selectionMenuController{this};
    PaneMenuController m_paneMenuController{&m_selectionMenuController};
    SearchController *m_searchController = nullptr;
    std::unique_ptr<SearchUiController> m_searchUiController;
    int m_searchVisibleCount = 0;
};

#ifndef THISPC_TEST_HARNESS
int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(
        QStringLiteral("kio-thispc"));
    QCoreApplication::setOrganizationDomain(
        QStringLiteral("local"));
    QCoreApplication::setApplicationName(
        QStringLiteral("thispc-view"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral("0.32.0"));

    app.setApplicationDisplayName(
        isPolish()
            ? QStringLiteral("Ten komputer")
            : QStringLiteral("This PC"));

    app.setWindowIcon(
        QIcon::fromTheme(
            QStringLiteral("computer")));

    app.setStyleSheet(
        applicationStyleSheet());

    QUrl initialUrl = kThisPcUrl;
    bool hasExplicitInitialUrl = false;

    if (argc > 1) {
        const QString argument =
            QString::fromLocal8Bit(argv[1]).trimmed();

        if (!argument.isEmpty()) {
            hasExplicitInitialUrl = true;
            const QUrl explicitUrl(argument);

            if (explicitUrl.isValid()
                && !explicitUrl.scheme().isEmpty()) {
                initialUrl = explicitUrl;
            } else {
                initialUrl = QUrl::fromUserInput(
                    argument,
                    QDir::currentPath(),
                    QUrl::AssumeLocalFile);
            }
        }
    }

    ThisPcWindow window(
        initialUrl,
        !hasExplicitInitialUrl);
    window.show();

#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
    QTimer recoveryTestHeartbeat;
    const QString heartbeatPath = QString::fromLocal8Bit(
        qgetenv("THISPC_RECOVERY_UI_HEARTBEAT"));
    if (!heartbeatPath.isEmpty()) {
        recoveryTestHeartbeat.setInterval(25);
        QObject::connect(&recoveryTestHeartbeat, &QTimer::timeout, [&heartbeatPath] {
            QFile heartbeat(heartbeatPath);
            if (heartbeat.open(QIODevice::WriteOnly | QIODevice::Append)) heartbeat.write("tick\n");
        });
        recoveryTestHeartbeat.start();
    }
    QTimer recoveryTestExit;
    bool recoveryTestExitScheduled = false;
    if (qgetenv("THISPC_RECOVERY_TEST_EXIT") == QByteArrayLiteral("1")) {
        recoveryTestExit.setInterval(25);
        QObject::connect(&recoveryTestExit, &QTimer::timeout,
                         [&app, &recoveryTestExitScheduled] {
            if (!recoveryTestExitScheduled
                && !BatchRenameRecoveryGate::instance().startupScanPending()) {
                recoveryTestExitScheduled = true;
                QTimer::singleShot(100, &app, &QCoreApplication::quit);
            }
        });
        recoveryTestExit.start();
    }
#endif

    return app.exec();
}
#endif

#include "thispcview.moc"
