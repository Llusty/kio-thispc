// Appended to a temporary, instrumented copy by run-pane-actions.py.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-pane-test");
    QCoreApplication::setApplicationName("pane-test");
    QTemporaryDir files;
    verify(files.isValid(), "temporary directory");
    const QUrl left = QUrl::fromLocalFile(files.path() + "/left");
    const QUrl right = QUrl::fromLocalFile(files.path() + "/right");
    QDir().mkpath(left.toLocalFile());
    QDir().mkpath(right.toLocalFile());

    DirectoryListWidget tooltipList;
    DirectoryTreeWidget tooltipDetails;
    tooltipDetails.setColumnCount(4);
    FileInfo tooltipFile;
    tooltipFile.name = QStringLiteral("sample.bin");
    tooltipFile.url = childUrlWithName(left, tooltipFile.name);
    tooltipFile.isDir = false;
    tooltipFile.size = 32LL * 1024 * 1024 * 1024;
    addDirectoryFileItems(
        &tooltipList,
        &tooltipDetails,
        tooltipFile,
        QIcon(),
        QStringLiteral("File"),
        formatFileSize(tooltipFile.size, false),
        QStringLiteral("Today"));
    verify(tooltipList.item(0)->toolTip().contains(formatFileSize(tooltipFile.size, false))
               && tooltipDetails.topLevelItem(0)->toolTip(0).contains(formatFileSize(tooltipFile.size, false)),
           "file tooltips include the formatted file size in list and details views");

    FileInfo tooltipDirectory = tooltipFile;
    tooltipDirectory.name = QStringLiteral("folder");
    tooltipDirectory.url = childUrlWithName(left, tooltipDirectory.name);
    tooltipDirectory.isDir = true;
    tooltipDirectory.size = -1;
    verify(!directoryItemToolTip(
                tooltipDirectory,
                QStringLiteral("Folder"),
                QStringLiteral("—"),
                QStringLiteral("Today"))
                .contains(trLocal("Rozmiar", "Size") + QStringLiteral(":")),
           "directory tooltips do not trigger or advertise recursive size calculation");

    ThisPcWindow window(left);
    window.show();
    window.activateWindow();
    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(right);
    QTest::qWait(200);
    window.m_refreshTimer.stop();
    if (window.m_directoryJob) {
        window.m_directoryJob->kill();
        window.m_directoryJob = nullptr;
    }
    if (window.m_splitPane->m_job) {
        window.m_splitPane->m_job->kill();
        window.m_splitPane->m_job = nullptr;
    }
    auto fill = [](DirectoryListWidget *list, DirectoryTreeWidget *tree, const QUrl &directory) {
        list->clear(); tree->clear();
        for (const auto &name : {QStringLiteral("one"), QStringLiteral("two")}) {
            const QUrl url = childUrlWithName(directory, name);
            auto *li = new QListWidgetItem(name, list);
            li->setData(Qt::UserRole, url.toString());
            li->setData(Qt::UserRole + 1, false);
            auto *ti = new QTreeWidgetItem(tree, QStringList{name, "File", "0", "Today"});
            ti->setData(0, Qt::UserRole, url.toString());
            ti->setData(0, Qt::UserRole + 1, false);
        }
    };
    fill(window.m_directoryList, window.m_directoryDetails, left);
    fill(window.m_splitPane->listView(), window.m_splitPane->detailsView(), right);

    // Stage 1: both panes own one equally aligned address section while all
    // shared controls keep operating on the pane selected by the user.
    app.processEvents();
    auto *primaryHeader = window.findChild<QFrame *>(QStringLiteral("primaryPaneHeader"));
    auto *splitHeader = window.m_splitPane->findChild<QFrame *>(QStringLiteral("splitPaneHeader"));
    verify(primaryHeader && splitHeader, "both pane address headers exist");
    verify(primaryHeader->isVisible() && splitHeader->isVisible(), "both pane addresses stay visible in Split View");
    verify(primaryHeader->width() == window.m_primaryPane->width()
               && splitHeader->width() == window.m_splitPane->width(),
           "address sections exactly match their pane widths");
    verify(primaryHeader->mapToGlobal(QPoint()).x() == window.m_primaryPane->mapToGlobal(QPoint()).x()
               && splitHeader->mapToGlobal(QPoint()).x() == window.m_splitPane->mapToGlobal(QPoint()).x(),
           "address sections align with their pane edges");
    window.m_contentSplitter->setSizes({430, 670});
    app.processEvents();
    verify(primaryHeader->width() == window.m_primaryPane->width()
               && splitHeader->width() == window.m_splitPane->width(),
           "address sections continue matching panes after divider resize");
    window.setActivePane(ThisPcWindow::PaneId::Split);
    window.m_sidebarSplitter->setSizes({360, 820});
    app.processEvents();
    verify(primaryHeader->width() == window.m_primaryPane->width()
               && splitHeader->width() == window.m_splitPane->width(),
           "pane addresses remain aligned after sidebar divider resize");
    verify(window.m_activePane == ThisPcWindow::PaneId::Split,
           "sidebar divider resize preserves the active pane");
    verify(!window.m_splitPane->m_backButton->isVisible()
               && !window.m_splitPane->m_forwardButton->isVisible()
               && !window.m_splitPane->m_upButton->isVisible()
               && !window.m_splitPane->m_viewButton->isVisible()
               && !window.m_splitPane->m_sortButton->isVisible()
               && !window.m_splitPane->m_swapButton->isVisible()
               && !window.m_splitPane->m_closeButton->isVisible(),
           "asymmetric split mini-toolbar is removed");
    verify(window.m_swapPanesAction->isVisible(), "swap remains available in the shared toolbar");

    window.setActivePane(ThisPcWindow::PaneId::Primary);
    QTest::mouseClick(window.m_splitPane->m_breadcrumbButton, Qt::LeftButton);
    app.processEvents();
    verify(window.m_activePane == ThisPcWindow::PaneId::Split,
           "clicking right breadcrumb activates right pane");
    QTest::keyClick(window.m_splitPane->m_addressEdit, Qt::Key_Escape);
    window.m_breadcrumbFrame->clicked();
    app.processEvents();
    verify(window.m_activePane == ThisPcWindow::PaneId::Primary,
           "clicking left breadcrumb activates left pane");
    QTest::keyClick(window.m_addressEdit, Qt::Key_Escape);

    window.setActivePane(ThisPcWindow::PaneId::Split);
    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    app.processEvents();
    verify(window.m_activePane == ThisPcWindow::PaneId::Split
               && window.m_splitPane->m_locationStack->currentWidget() == window.m_splitPane->m_addressEdit,
           "Ctrl+L edits the active right address without switching panes");
    QTest::keyClick(window.m_splitPane->m_addressEdit, Qt::Key_Escape);

    const QUrl sidebarTarget = QUrl::fromLocalFile(files.path() + "/sidebar-target");
    QDir().mkpath(sidebarTarget.toLocalFile());
    window.m_sidebar->activated(sidebarTarget);
    verify(window.m_activePane == ThisPcWindow::PaneId::Split
               && window.m_splitPane->currentUrl() == sidebarTarget
               && window.m_currentUrl == left,
           "sidebar navigates right active pane and preserves pane selection");
    const QUrl otherPaneTarget = QUrl::fromLocalFile(files.path() + "/other-pane-target");
    QDir().mkpath(otherPaneTarget.toLocalFile());
    window.m_sidebar->openInSplitPaneRequested(otherPaneTarget);
    verify(window.m_activePane == ThisPcWindow::PaneId::Split
               && window.m_currentUrl == otherPaneTarget
               && window.m_splitPane->currentUrl() == sidebarTarget,
           "sidebar Open in other pane targets left from active right pane");
    window.navigateTo(left, false);
    window.m_splitPane->setCurrentUrl(right, true);

    const QUrl nested = QUrl::fromLocalFile(right.toLocalFile() + "/nested");
    QDir().mkpath(nested.toLocalFile());
    window.m_splitPane->setCurrentUrl(nested, true);
    window.m_backAction->trigger();
    verify(window.m_activePane == ThisPcWindow::PaneId::Split
               && window.m_splitPane->currentUrl() == right,
           "shared Back operates on right pane");
    window.m_forwardAction->trigger();
    verify(window.m_splitPane->currentUrl() == nested, "shared Forward operates on right pane");
    window.m_upAction->trigger();
    verify(window.m_splitPane->currentUrl() == right, "shared Up operates on right pane");
    window.m_refreshAction->trigger();
    verify(window.m_activePane == ThisPcWindow::PaneId::Split,
           "shared Refresh preserves active pane");

    for (auto *action : window.m_viewButton->menu()->actions()) {
        if (action->data().toInt() == 2) action->trigger();
    }
    verify(window.m_splitPane->viewMode() == 2
               && window.m_activePane == ThisPcWindow::PaneId::Split,
           "shared View changes right pane without switching it");
    for (auto *action : window.m_sortButton->menu()->actions()) {
        if (action->data().toInt() == 3) action->trigger();
    }
    verify(window.m_splitPane->sortKey() == 3
               && window.m_activePane == ThisPcWindow::PaneId::Split,
           "shared Sort changes right pane without switching it");

    window.m_searchEdit->setFocus();
    QTest::keyClick(&window, Qt::Key_F6);
    app.processEvents();
    verify(window.m_activePane == ThisPcWindow::PaneId::Primary,
           "F6 moves from right to left even when toolbar has focus");
    QTest::keyClick(&window, Qt::Key_F6);
    app.processEvents();
    verify(window.m_activePane == ThisPcWindow::PaneId::Split,
           "F6 moves from left to right symmetrically");

    // Restore the fixture state used by the existing parity matrix.
    window.setDirectoryViewMode(0);
    window.m_splitPane->setViewMode(0);
    window.m_splitPane->setSortState(0, true);
    window.m_splitPane->setCurrentUrl(right, false);
    if (window.m_directoryJob) {
        window.m_directoryJob->kill();
        window.m_directoryJob = nullptr;
    }
    if (window.m_splitPane->m_job) {
        window.m_splitPane->m_job->kill();
        window.m_splitPane->m_job = nullptr;
    }
    fill(window.m_directoryList, window.m_directoryDetails, left);
    fill(window.m_splitPane->listView(), window.m_splitPane->detailsView(), right);
    auto focus = [&](QAbstractItemView *view) {
        window.activateWindow(); view->setFocus(); app.processEvents();
    };
    auto selectOne = [](QAbstractItemView *view) {
        view->clearSelection();
        view->selectionModel()->setCurrentIndex(view->model()->index(0, 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    };
    auto dialog = [&](bool rename, bool changeFocus, QAbstractItemView *other) {
        QTimer::singleShot(0, &window, [&, rename, changeFocus, other] {
            if (changeFocus) window.setActivePane(other == window.m_directoryList || other == window.m_directoryDetails
                ? ThisPcWindow::PaneId::Primary : ThisPcWindow::PaneId::Split);
            if (rename) {
                auto *box = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                verify(box != nullptr, "input dialog opened");
                box->setTextValue("renamed"); box->accept();
            } else {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                verify(box != nullptr, "Trash confirmation opened");
                box->button(QMessageBox::Yes)->click();
            }
        });
    };
    for (int mode : {0, 1, 2}) {
        window.setDirectoryViewMode(mode);
        window.m_splitPane->setViewMode(mode);
        app.processEvents();
        QAbstractItemView *lv = mode == 2 ? static_cast<QAbstractItemView *>(window.m_directoryDetails) : window.m_directoryList;
        QAbstractItemView *rv = mode == 2 ? static_cast<QAbstractItemView *>(window.m_splitPane->detailsView()) : window.m_splitPane->listView();
        for (bool split : {false, true}) {
            auto *view = split ? rv : lv;
            auto *other = split ? lv : rv;
            const QUrl directory = split ? right : left;
            const QUrl file = childUrlWithName(directory, "one");
            selectOne(lv); selectOne(rv); focus(view);
            verify(window.selectedUrls() == QList<QUrl>{file}, "focus chooses selection with both panes selected");
            verify(window.m_copyAction->isEnabled(), "toolbar selection state");
            dispatch = {};
            dialog(false, true, other);
            QTest::keyClick(view, Qt::Key_Delete);
            verify(dispatch.kind == "trash" && dispatch.sources == QList<QUrl>{file}, "Delete keeps initiating pane despite focus change");
            focus(view);
            QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);
            verify(app.clipboard()->mimeData()->urls() == QList<QUrl>{file}, "Ctrl+C source");
            verify(app.clipboard()->mimeData()->data("application/x-kde-cutselection") == "0", "KDE copy marker");
            QTest::keyClick(view, Qt::Key_X, Qt::ControlModifier);
            verify(app.clipboard()->mimeData()->urls() == QList<QUrl>{file}, "Ctrl+X source");
            verify(app.clipboard()->mimeData()->data("application/x-kde-cutselection") == "1", "KDE cut marker");
            focus(other);
            dispatch = {};
            QTest::keyClick(other, Qt::Key_V, Qt::ControlModifier);
            verify(dispatch.kind == "move" && dispatch.sources == QList<QUrl>{file}
                && dispatch.destination == (split ? left : right), "Ctrl+V destination in opposite pane");
            focus(view);
            QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);
            focus(other);
            QTest::keyClick(other, Qt::Key_V, Qt::ControlModifier);
            verify(dispatch.kind == "copy" && dispatch.destination == (split ? left : right), "copy paste destination");
            focus(view); dispatch = {};
            dialog(true, true, other);
            QTest::keyClick(view, Qt::Key_F2);
            verify(dispatch.kind == "rename" && dispatch.sources == QList<QUrl>{file}
                && dispatch.destination == childUrlWithName(directory, "renamed"), "F2 keeps source and parent");
            focus(view); dispatch = {};
            QTest::keyClick(view, Qt::Key_Return, Qt::AltModifier);
            verify(dispatch.kind == "properties" && dispatch.sources == QList<QUrl>{file}, "Alt+Enter properties");
            QTest::keyClick(view, Qt::Key_A, Qt::ControlModifier);
            verify(window.selectedUrls().size() == 2, "Ctrl+A active pane");
            verify(other->selectionModel()->selectedRows().size() == 1, "Ctrl+A leaves other selection alone");
            selectOne(view);
            dialog(true, true, other);
            QTest::keyClick(view, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
            verify(dispatch.kind == "mkdir" && dispatch.destination == childUrlWithName(directory, "renamed"), "create folder captures directory");
            focus(view);
            dialog(true, true, other);
            window.createNewFile("new.txt", {});
            verify(dispatch.kind == "create" && dispatch.destination == childUrlWithName(directory, "renamed"), "create file captures directory");

            focus(view);
            verify(window.m_newMarkdownAction != nullptr, "Markdown New action exists");
            QTimer::singleShot(0, &window, [&] {
                auto *nameDialog =
                    qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                verify(nameDialog != nullptr, "Markdown name dialog opened");
                verify(nameDialog->textValue().endsWith(".md"),
                       "Markdown action suggests .md");
                nameDialog->setTextValue("markdown.md");
                nameDialog->accept();
            });
            dispatch = {};
            window.m_newMarkdownAction->trigger();
            verify(dispatch.kind == "create"
                       && dispatch.destination
                           == childUrlWithName(directory, "markdown.md"),
                   "Markdown action creates in active pane");
        }
        selectOne(lv); selectOne(rv); focus(rv);
        QToolButton *copyButton = nullptr;
        for (auto *button : window.findChildren<QToolButton *>())
            if (button->defaultAction() == window.m_copyAction) copyButton = button;
        verify(copyButton != nullptr, "copy toolbar button exists");
        QTest::mouseClick(copyButton, Qt::LeftButton);
        verify(app.clipboard()->mimeData()->urls() == QList<QUrl>{childUrlWithName(right, "one")}, "toolbar preserves right pane");
        focus(rv);
        rv->clearSelection();
        verify(!window.m_copyAction->isEnabled(), "right selection change updates toolbar despite left selection");
        focus(lv);
        QTimer::singleShot(0, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            verify(menu != nullptr, "right context menu opened");
            QStringList labels;
            for (auto *action : menu->actions()) labels << action->text();
            for (const auto &text : {"Open", "Cut", "Copy", "Rename", "Trash", "Properties"})
                verify(labels.contains(QString::fromLatin1(text)), "shared item menu action");
            verify(window.paneContext().id == ThisPcWindow::PaneId::Split, "PPM activates right without left click");
            menu->close();
        });
        const QPoint point = mode == 2 ? window.m_splitPane->detailsView()->visualItemRect(window.m_splitPane->detailsView()->topLevelItem(0)).center()
            : window.m_splitPane->listView()->visualItemRect(window.m_splitPane->listView()->item(0)).center();
        // Invoke the actual customContextMenuRequested signal, as Qt does for PPM.
        QMetaObject::invokeMethod(rv, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point));
        verify(window.m_activePane == ThisPcWindow::PaneId::Split, "right remains active after menu closes");
    }
    // Both background menus use the same generator and available operations.
    QStringList primaryBackground;
    for (bool split : {false, true}) {
        QTimer::singleShot(0, &window, [&, split] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            verify(menu != nullptr, "background menu opens");
            QStringList labels;
            for (auto *action : menu->actions()) labels << action->text();
            for (const auto &label : {"View", "Sort", "Refresh", "Select all", "New folder", "Paste"})
                verify(labels.contains(QString::fromLatin1(label)), "background operation available");
            if (!split) primaryBackground = labels;
            else verify(labels == primaryBackground, "same background menu in both panes");
            menu->close();
        });
        auto *view = split ? window.m_splitPane->detailsView() : window.m_directoryDetails;
        const QPoint background(view->viewport()->width()-5, view->viewport()->height()-5);
        QMetaObject::invokeMethod(view, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, background));
    }
    // Choosing Copy after a focus change inside the popup keeps the menu's selection.
    window.focusPrimaryPane();
    QTimer::singleShot(0, &window, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        verify(menu != nullptr, "snapshot menu opened");
        QAction *copy = nullptr;
        for (auto *action : menu->actions()) if (action->text() == "Copy") copy = action;
        verify(copy != nullptr, "menu Copy exists");
        window.setActivePane(ThisPcWindow::PaneId::Primary);
        menu->setActiveAction(copy);
        QTest::keyClick(menu, Qt::Key_Return);
    });
    const QPoint itemPosition = window.m_splitPane->detailsView()->visualItemRect(window.m_splitPane->detailsView()->topLevelItem(0)).center();
    QMetaObject::invokeMethod(window.m_splitPane->detailsView(), "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, itemPosition));
    verify(app.clipboard()->mimeData()->urls() == QList<QUrl>{childUrlWithName(right, "one")}, "menu snapshot survives focus change");
    window.m_splitPane->focusView();
    window.setActivePane(ThisPcWindow::PaneId::Split);
    window.setSplitViewEnabled(false);
    verify(window.m_activePane == ThisPcWindow::PaneId::Primary, "closing split resets active pane");
    verify(window.paneContext().directory == left, "closing split restores primary destination");
    // Stage 4: This PC is the same virtual card page on either side.
    using Pane = ThisPcWindow::PaneId;
    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(right);
    window.setActivePane(Pane::Split);
    window.m_sidebar->activated(kThisPcUrl);
    verify(window.m_splitPane->currentUrl() == kThisPcUrl && window.m_currentUrl == left
               && window.m_activePane == Pane::Split,
           "sidebar This PC targets the active right pane");
    verify(window.m_splitPane->contentStack()->currentWidget() == window.m_splitHomePage
               && window.m_splitHomePage->isVisible() && window.m_splitPane->m_job == nullptr,
           "right This PC presents the card page without a directory-list job");
    verify(!window.paneContext().isDirectory && !window.m_upAction->isEnabled()
               && !window.m_viewButton->isEnabled() && !window.m_sortButton->isEnabled()
               && !window.m_copyAction->isEnabled() && !window.m_newButton->isEnabled(),
           "right This PC shared controls match left virtual-page policy");
    verify(window.m_splitPane->m_breadcrumbButton->text() == "This PC"
               && window.m_searchEdit->placeholderText() == "Search this computer"
               && window.m_searchScopeGroup->checkedAction()->data().toInt() == 2,
           "right This PC has friendly breadcrumb and whole-computer Search scope");
    if (window.m_driveJob) { window.m_driveJob->kill(); window.m_driveJob = nullptr; }
    DriveInfo fixtureDrive;
    fixtureDrive.name = "Disposable drive";
    fixtureDrive.targetUrl = right;
    fixtureDrive.usedPercent = 37;
    fixtureDrive.capacityText = "100 GiB";
    fixtureDrive.freeText = "63 GiB";
    window.m_drives = {fixtureDrive};
    window.rebuildDriveGrid();
    verify(window.m_drivesGrid->count() == 1 && window.m_splitDrivesGrid->count() == 1,
           "shared drive inventory renders a card in each This PC page");
    auto *leftDrive = qobject_cast<DriveFrame *>(window.m_drivesGrid->itemAt(0)->widget());
    auto *rightDrive = qobject_cast<DriveFrame *>(window.m_splitDrivesGrid->itemAt(0)->widget());
    verify(leftDrive && rightDrive && leftDrive->accessibleName() == rightDrive->accessibleName()
               && leftDrive->toolTip() == rightDrive->toolTip(),
           "both This PC pages use identical drive labels and capacity tooltips");
    verify(window.m_homePage->findChildren<ClickableFrame *>().size()
               == window.m_splitHomePage->findChildren<ClickableFrame *>().size(),
           "both This PC pages expose the same folders and drives");
    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    verify(window.m_splitPane->m_addressEdit->text() == urlForDisplay(kThisPcUrl),
           "right This PC Ctrl+L exposes the virtual address");
    QTest::keyClick(window.m_splitPane->m_addressEdit, Qt::Key_Escape);
    rightDrive->setFocus();
    QTest::keyClick(rightDrive, Qt::Key_Return);
    verify(window.m_splitPane->currentUrl() == right && window.m_currentUrl == left
               && window.m_activePane == Pane::Split,
           "right drive card keyboard activation opens its real target in right pane");
    window.m_upAction->trigger();
    verify(window.m_splitPane->currentUrl() == kThisPcUrl,
           "right drive-root Up returns to This PC instead of the filesystem parent");
    window.m_backAction->trigger();
    verify(window.m_splitPane->currentUrl() == right, "right Back restores drive after This PC");
    window.m_forwardAction->trigger();
    verify(window.m_splitPane->currentUrl() == kThisPcUrl, "right Forward restores This PC card page");
    if (window.m_driveJob) { window.m_driveJob->kill(); window.m_driveJob = nullptr; }
    window.m_refreshAction->trigger();
    verify(window.m_driveJob && !window.m_splitPane->m_job && window.m_currentUrl == left,
           "right This PC Refresh uses the existing shared drive backend");
    window.m_driveJob->kill(); window.m_driveJob = nullptr;
    window.navigateTo(kThisPcUrl, true);
    window.setActivePane(Pane::Split);
    window.m_searchEdit->setFocus();
    QAction *focusOtherAction = nullptr;
    for (QAction *action : window.actions()) {
        if (action->shortcut() == QKeySequence(Qt::Key_F6)) {
            focusOtherAction = action;
            break;
        }
    }
    verify(focusOtherAction != nullptr, "shared F6 action exists");
    focusOtherAction->trigger();
    verify(window.m_activePane == Pane::Primary, "F6 focuses left This PC from shared Search");
    focusOtherAction->trigger();
    verify(window.m_activePane == Pane::Split, "F6 focuses right This PC symmetrically");
    auto folderCards = window.m_splitHomePage->findChildren<ClickableFrame *>();
    ClickableFrame *folderCard = nullptr;
    for (auto *card : folderCards) {
        if (!qobject_cast<DriveFrame *>(card)) { folderCard = card; break; }
    }
    verify(folderCard != nullptr, "right This PC includes user folder cards");
    // Point the real card interaction at a disposable folder for this test.
    folderCard->m_url = right;
    QTest::mouseDClick(folderCard, Qt::LeftButton);
    verify(window.m_splitPane->currentUrl() == right && window.m_currentUrl == kThisPcUrl,
           "right folder card double-click affects only its owning pane");
    window.setActivePane(Pane::Split);
    leftDrive->setFocus();
    QTest::keyClick(leftDrive, Qt::Key_Return);
    verify(window.m_currentUrl == right && window.m_splitPane->currentUrl() == right
               && window.m_activePane == Pane::Primary,
           "left drive card retains its accepted pane-local routing");
    window.setActivePane(Pane::Split);
    window.beginAddressEdit(Pane::Split);
    window.m_splitPane->m_addressEdit->setText("thispc:/");
    QTest::keyClick(window.m_splitPane->m_addressEdit, Qt::Key_Return);
    verify(window.m_splitPane->currentUrl() == kThisPcUrl && window.m_currentUrl == right
               && window.m_splitPane->m_locationStack->currentWidget() == window.m_splitPane->m_breadcrumbFrame,
           "right address submission opens This PC and restores its breadcrumb");
    window.swapSplitPanes();
    verify(window.m_currentUrl == kThisPcUrl && window.m_splitPane->currentUrl() == right
               && window.m_contentStack->currentWidget() == window.m_homePage,
           "pane swap preserves This PC virtual-page semantics");
    qInfo("PASS: %d assertions, Icons/List/Details, both panes; KIO dispatch intercepted", checks);
}
