// The runner substitutes only KIO::emptyTrash() with a controllable KJob.
// Confirmation, dispatch, the operation manager and completion UI stay real.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-empty-trash-test");
    QCoreApplication::setApplicationName("empty-trash-test");
    interceptPaneRefreshes = true;
    const QUrl trash(QStringLiteral("trash:/"));
    QTemporaryDir files;
    verify(files.isValid(), "temporary local directory");
    const QUrl local = QUrl::fromLocalFile(files.path());
    ThisPcWindow window(local, false);
    window.show();
    window.setSplitViewEnabled(true);
    app.processEvents();
    window.m_primaryPane->cancelListing();
    window.m_splitPane->cancelListing();
    if (window.m_driveJob) { window.m_driveJob->kill(); window.m_driveJob = nullptr; }
    using Pane = ThisPcWindow::PaneId;
    auto *actions = window.m_fileActions;
    auto *manager = window.m_operationManager;
    auto place = [&](const QUrl &left, const QUrl &right) {
        // Seed view locations without listing any real trash contents.
        window.m_navigation.updateCurrent(left);
        window.m_primaryPane->setCurrentUrl(left);
        window.m_splitPane->m_currentUrl = right;
        window.m_contentStack->setCurrentWidget(window.m_directoryPage);
        window.m_splitPane->m_contentStack->setCurrentWidget(window.m_splitPane->m_directoryPage);
        window.m_directoryList->clear();
        window.m_directoryDetails->clear();
        window.m_splitPane->listView()->clear();
        window.m_splitPane->detailsView()->clear();
    };
    auto confirm = [&](int answer, const std::function<void()> &steer) {
        QTimer::singleShot(0, &window, [&, answer, steer] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            verify(box && box->windowTitle() == "Empty Trash", "explicit Empty Trash confirmation");
            verify(box->text().contains("all items") && box->text().contains("cannot be undone"),
                   "confirmation states global scope and irreversible deletion");
            verify(box->defaultButton() == box->button(QMessageBox::No), "No is the safe default");
            verify(!actions->canEmptyTrash(trash),
                   "reentry is blocked while confirmation is open");
            const int before = emptyTrashDispatches;
            actions->emptyTrash(trash, {});
            verify(emptyTrashDispatches == before, "nested invocation does not dispatch");
            if (steer) steer();
            if (answer < 0) QTest::keyClick(box, Qt::Key_Escape);
            else box->button(answer ? QMessageBox::Yes : QMessageBox::No)->click();
        });
    };
    auto background = [&](Pane pane, bool present, bool enabled, bool choose = false,
                          int answer = 0, const std::function<void()> &steer = {}) {
        QTimer::singleShot(0, &window, [&, pane, present, enabled, choose, answer, steer] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            verify(menu != nullptr, "real pane background menu opens");
            QAction *empty = nullptr;
            for (auto *action : menu->actions()) if (action->text() == "Empty Trash") empty = action;
            verify(bool(empty) == present, "Empty Trash visibility matches root location");
            if (empty) verify(empty->isEnabled() == enabled, "Empty Trash enabled state");
            verify(window.paneContext().id == pane, "background menu captures initiating pane");
            if (choose) {
                // Change active pane both in QMenu::exec and in confirmation.
                window.setActivePane(pane == Pane::Primary ? Pane::Split : Pane::Primary);
                confirm(answer, steer);
                menu->setActiveAction(empty);
                QTest::keyClick(menu, Qt::Key_Return);
            } else menu->close();
        });
        QAbstractItemView *view = pane == Pane::Primary
            ? (window.m_directoryViewMode == 2 ? static_cast<QAbstractItemView *>(window.m_directoryDetails) : window.m_directoryList)
            : (window.m_splitPane->viewMode() == 2 ? static_cast<QAbstractItemView *>(window.m_splitPane->detailsView()) : window.m_splitPane->listView());
        const QPoint point(view->viewport()->width() - 5, view->viewport()->height() - 5);
        QMetaObject::invokeMethod(view, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point));
    };

    const QList<QUrl> forbidden{local, kThisPcUrl, QUrl("thispc:/Search?q=test"),
        makeSearchLocation("test", 0, local, 0, 0, 0), filenameSearchUrl("test", local),
        QUrl("sftp://example.test/folder"), QUrl("trash:/folder"),
        QUrl("trash://example.test/"), QUrl("trash:/?q=test"), QUrl("trash:/#item"), QUrl()};

    verify(window.m_emptyTrashAction != nullptr, "Empty Trash toolbar action exists");
    verify(!window.m_emptyTrashAction->icon().isNull(), "Empty Trash toolbar action has a themed icon");
    for (Pane pane : {Pane::Primary, Pane::Split}) {
        for (const QUrl &url : forbidden) {
            place(pane == Pane::Primary ? url : trash,
                  pane == Pane::Split ? url : trash);
            window.setActivePane(pane);
            window.updateFileActionStates();
            verify(!window.m_emptyTrashAction->isVisible(),
                   "toolbar action is hidden outside the active pane Trash root");
            verify(!window.m_emptyTrashAction->isEnabled(),
                   "toolbar action is disabled outside the active pane Trash root");
        }
        place(pane == Pane::Primary ? trash : local,
              pane == Pane::Split ? trash : local);
        window.setActivePane(pane);
        window.updateFileActionStates();
        verify(window.m_emptyTrashAction->isVisible() && window.m_emptyTrashAction->isEnabled(),
               "toolbar action follows the active pane at the Trash root");
        window.setActivePane(pane == Pane::Primary ? Pane::Split : Pane::Primary);
        verify(!window.m_emptyTrashAction->isVisible() && !window.m_emptyTrashAction->isEnabled(),
               "toolbar action updates when the active pane changes");
    }

    // The toolbar routes through the same confirmation and FileActions path as
    // the background menu.
    place(trash, local);
    window.setActivePane(Pane::Primary);
    const int toolbarBefore = emptyTrashDispatches;
    confirm(0, {});
    window.m_emptyTrashAction->trigger();
    verify(emptyTrashDispatches == toolbarBefore
               && window.m_emptyTrashAction->isVisible()
               && window.m_emptyTrashAction->isEnabled(),
           "toolbar cancellation dispatches no job and restores its state");
    confirm(1, {});
    window.m_emptyTrashAction->trigger();
    verify(emptyTrashDispatches == toolbarBefore + 1
               && window.m_emptyTrashAction->isVisible()
               && !window.m_emptyTrashAction->isEnabled(),
           "toolbar confirmation dispatches once and stays disabled while busy");
    testEmptyTrashJob->complete();
    verify(window.m_emptyTrashAction->isEnabled(),
           "toolbar action is enabled again after completion");
    const int afterToolbarDispatches = emptyTrashDispatches;

    for (const QUrl &url : forbidden) {
        verify(!actions->canEmptyTrash(url), "FileActions rejects non-root destinations");
        actions->emptyTrash(url, {});
    }
    verify(emptyTrashDispatches == afterToolbarDispatches,
           "invalid locations dispatch no jobs");
    verify(actions->canEmptyTrash(trash) && actions->canEmptyTrash(QUrl("trash:")),
           "root URL spellings are supported");
    for (int mode : {0, 1, 2, 3}) {
        window.setDirectoryViewMode(mode);
        window.m_splitPane->setViewMode(mode);
        for (Pane pane : {Pane::Primary, Pane::Split}) {
            for (const QUrl &url : forbidden + QList<QUrl>{trash}) {
                place(pane == Pane::Primary ? url : local, pane == Pane::Split ? url : local);
                background(pane, url == trash, url == trash);
            }
            place(pane == Pane::Primary ? trash : local, pane == Pane::Split ? trash : local);
            verify(!window.m_newMarkdownAction->isEnabled() && !window.m_templateMenu->menuAction()->isEnabled(),
                   "Markdown and templates remain disabled in Trash");
            for (int answer : {0, -1}) {
                const int before = emptyTrashDispatches;
                background(pane, true, true, true, answer);
                verify(emptyTrashDispatches == before && actions->canEmptyTrash(trash),
                       "No and Escape preserve Trash and release confirmation guard");
            }
            const int before = emptyTrashDispatches;
            refreshedPanes.clear();
            background(pane, true, true, true, 1, [&] {
                window.setActivePane(pane == Pane::Primary ? Pane::Split : Pane::Primary);
            });
            verify(emptyTrashDispatches == before + 1 && manager->activeCount() == 1,
                   "one confirmed operation is tracked despite modal focus changes");
            verify(testEmptyTrashJob && testEmptyTrashJob->uiDelegate() == nullptr,
                   "automatic KIO error dialog is disabled");
            verify(!actions->canEmptyTrash(trash), "repeat dispatch blocked during operation");
            background(pane, true, false);
            actions->emptyTrash(trash, {});
            verify(emptyTrashDispatches == before + 1, "direct repeat dispatch is also blocked");
            verify(refreshedPanes.isEmpty(), "refresh waits for completion");
            testEmptyTrashJob->complete();
            verify(refreshedPanes == QList<int>{pane == Pane::Split ? 1 : 0},
                   "completion refreshes the initiating Trash pane despite focus switch");
            verify(manager->activeCount() == 0 && actions->canEmptyTrash(trash),
                   "completion releases tracked operation and busy guard");
            verify(window.statusBar()->currentMessage() == "Trash emptied", "success message is visible");
        }
    }

    // Completion uses current locations, including after navigating away,
    // opening Trash in the other pane, closing Split View, or partial failure.
    for (int error : {0, int(KIO::ERR_ACCESS_DENIED), int(KJob::KilledJobError), int(KIO::ERR_USER_CANCELED)}) {
        for (int destination : {0, 1, 2, 3, 4}) {
            place(trash, local);
            background(Pane::Primary, true, true, true, 1);
            const auto operationId = manager->operations().last().id;
            const bool leftTrash = destination == 0 || destination == 2;
            const bool rightTrash = destination == 1 || destination == 2 || destination == 4;
            place(leftTrash ? trash : local, rightTrash ? trash : local);
            if (destination == 4) window.m_splitPane->hide();
            refreshedPanes.clear();
            if (error == KIO::ERR_ACCESS_DENIED) {
                QTimer::singleShot(0, &window, [&] {
                    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                    verify(box && box->windowTitle() == "Operation failed", "common operation error dialog");
                    box->accept();
                });
            }
            testEmptyTrashJob->complete(error);
            QList<int> expected;
            if (leftTrash) expected << 0;
            if (rightTrash && destination != 4) expected << 1;
            verify(refreshedPanes == expected, "only currently visible Trash panes refresh for each result");
            const auto expectedState = error == 0 ? OperationManager::State::Completed
                : error == KIO::ERR_ACCESS_DENIED ? OperationManager::State::Failed : OperationManager::State::Cancelled;
            bool found = false;
            for (const auto &op : manager->operations()) {
                if (op.id != operationId) continue;
                found = true;
                verify(op.state == expectedState && op.title == "Emptying Trash", "operation history has correct title and state");
            }
            verify(found && manager->activeCount() == 0, "finished operation stays in history");
            verify(actions->canEmptyTrash(trash), "success, failure and cancellation release busy state");
            if (error == KJob::KilledJobError || error == KIO::ERR_USER_CANCELED)
                verify(window.statusBar()->currentMessage() == "Operation cancelled", "cancellation uses existing status UI");
            window.m_splitPane->show();
        }
    }
    place(trash, trash);
    auto *mime = new QMimeData;
    mime->setUrls({local});
    mime->setData("application/x-kde-cutselection", "1");
    app.clipboard()->setMimeData(mime);
    background(Pane::Split, true, true, true, 1);
    refreshedPanes.clear();
    manager->cancelAll();
    verify(manager->activeCount() == 0 && refreshedPanes == QList<int>({0, 1}),
           "real manager Cancel finishes job and refreshes both Trash views");
    verify(app.clipboard()->mimeData()->urls() == QList<QUrl>{local}, "Empty Trash preserves clipboard");
    verify(!window.m_undoAction->isEnabled(), "Empty Trash adds no Undo command");

    // Exercise a real asynchronous listing using our empty local fixture.
    // Route its completion as a Trash refresh without accessing live Trash.
    place(trash, local);
    window.setActivePane(Pane::Primary);
    window.loadDirectory(local);
    window.statusBar()->showMessage("Trash emptied", 4000);
    window.refreshTrashViews();
    verify(QTest::qWaitFor([&] { return !window.m_primaryPane->listingJob(); }, 10000),
           "real disposable directory listing completes");
    verify(window.statusBar()->currentMessage() == "Trash emptied",
           "asynchronous refresh preserves the operation result message");
    app.processEvents();
    qInfo("PASS: %d Empty Trash assertions; visibility, confirmation, routing, simulated job lifecycle", checks);
}
