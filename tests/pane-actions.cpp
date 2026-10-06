/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

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
    const QString templates = files.filePath("templates");
    verify(QDir().mkpath(templates), "temporary templates directory");
    QFile templateFile(templates + "/A&B #ż.md");
    verify(templateFile.open(QIODevice::WriteOnly) && templateFile.write("template") == 8,
           "pane template fixture");
    templateFile.close();
    const QUrl templateSource = QUrl::fromLocalFile(templateFile.fileName());
    const QString configPath = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"));
    verify(!configPath.isEmpty() && QDir().mkpath(configPath), "isolated pane template config");
    QFile userDirs(configPath + "/user-dirs.dirs");
    verify(userDirs.open(QIODevice::WriteOnly), "open pane template config");
    userDirs.write("XDG_TEMPLATES_DIR=\"" + templates.toUtf8() + "\"\n");
    userDirs.close();

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
    verify(tooltipList.item(0).data(Qt::ToolTipRole).toString().contains(formatFileSize(tooltipFile.size, false))
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
    verify(qobject_cast<PrimaryBrowserPane *>(window.m_primaryPane) != nullptr
               && window.m_primaryPane->objectName() == QStringLiteral("primaryBrowserPane"),
           "PrimaryBrowserPane is created with the stable style hook");
    verify(window.m_primaryPane->contentStack() == window.m_contentStack
               && window.m_primaryPane->listView() == window.m_directoryList
               && window.m_primaryPane->detailsView() == window.m_directoryDetails,
           "PrimaryBrowserPane owns the primary content and bound directory views");
    verify(window.m_primaryPane->currentUrl() == left,
           "PrimaryBrowserPane initializes with the requested current URL");
    verify(window.m_paneAdapter->context(PaneId::Primary).directory == left,
           "PaneAdapter reads the PrimaryBrowserPane URL contract");
    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(right);
    const auto newActions = window.m_newButton->menu()->actions();
    verify(newActions.size() == 6
               && newActions.at(0) == window.m_newFolderAction
               && newActions.at(1) == window.m_newTextFileAction
               && newActions.at(2) == window.m_newMarkdownAction
               && newActions.at(3) == window.m_newEmptyFileAction
               && newActions.at(4)->isSeparator()
               && newActions.at(5)->menu() == window.m_templateMenu,
           "New retains all four built-ins in order and appends Templates after a separator");
    window.m_templateMenu->aboutToShow();
    verify(QTest::qWaitFor([&] { return !window.m_templateMenu->m_job; }, 5000),
           "window discovers XDG templates");
    verify(window.m_templateMenu->actions().size() == 1
               && window.m_templateMenu->actions().first()->data().toUrl() == templateSource,
           "window populates the template action from native XDG");
    QTest::qWait(200);
    window.m_refreshTimer.stop();
    window.m_primaryPane->cancelListing();
    window.m_splitPane->cancelListing();

    // 0.30 Stage 1: the mode belongs to the normalized location rather than
    // to a pane. Exercise both panes, navigation, and the shared persistence.
    window.setDirectoryViewMode(1);
    window.navigateTo(right, false);
    window.m_primaryPane->cancelListing();
    window.setDirectoryViewMode(2);
    window.navigateTo(left, false);
    window.m_primaryPane->cancelListing();
    verify(window.m_directoryViewMode == 1,
           "primary navigation restores the folder view mode");
    window.m_splitPane->setCurrentUrl(right, false);
    window.m_splitPane->cancelListing();
    verify(window.m_splitPane->viewMode() == 2,
           "split pane restores the same persisted folder mode");
    window.m_splitPane->setViewMode(0);
    window.m_splitPane->setCurrentUrl(left, false);
    window.m_splitPane->cancelListing();
    verify(window.m_splitPane->viewMode() == 1,
           "per-folder mode is shared across primary and split panes");

    window.setDirectoryViewMode(0);
    window.m_splitPane->setViewMode(0);
    window.m_splitPane->setCurrentUrl(right, false);
    window.m_splitPane->cancelListing();
    QMenu *viewMenu = window.m_viewButton->menu();
    QMenu *showMenu = viewMenu
        ? viewMenu->findChild<QMenu *>(QStringLiteral("viewShowMenu"))
        : nullptr;
    verify(viewMenu && showMenu, "View contains the Show submenu");
    const auto viewActions = viewMenu->actions();
    QMenu *iconSizeMenu = viewMenu
        ? viewMenu->findChild<QMenu *>(QStringLiteral("viewIconSizeMenu"))
        : nullptr;
    QAction *applyViewToSubfolders = viewMenu->findChild<QAction *>(
        QStringLiteral("pane.applyViewToSubfolders"));
    QAction *removeViewFromSubfolders = viewMenu->findChild<QAction *>(
        QStringLiteral("pane.removeViewFromSubfolders"));
    verify(viewActions.size() == 12
               && viewActions.at(0)->objectName() == QStringLiteral("viewModeAction0")
               && viewActions.at(1)->objectName() == QStringLiteral("viewModeAction1")
               && viewActions.at(2)->objectName() == QStringLiteral("viewModeAction2")
               && viewActions.at(3)->objectName() == QStringLiteral("viewModeAction3")
               && viewActions.at(4)->menu() == iconSizeMenu
               && viewActions.at(5)->isSeparator()
               && viewActions.at(6)->menu() == showMenu
               && viewActions.at(7)->isSeparator()
               && viewActions.at(8) == window.m_restoreSessionAction
               && viewActions.at(9)->isSeparator()
               && viewActions.at(10) == applyViewToSubfolders
               && viewActions.at(11) == removeViewFromSubfolders,
           "View groups modes, display commands, session, and folder profile commands");
    verify(applyViewToSubfolders && !applyViewToSubfolders->isCheckable(),
           "top-level View Apply-to-subfolders is a one-shot command");
    verify(iconSizeMenu && iconSizeMenu->actions().size() == 3
               && iconSizeMenu->findChild<QAction *>(QStringLiteral("iconSizeSmaller"))
               && iconSizeMenu->findChild<QAction *>(QStringLiteral("iconSizeCurrent"))
               && iconSizeMenu->findChild<QAction *>(QStringLiteral("iconSizeLarger")),
           "Icon size exposes one compact smaller/current/larger step control");
    const auto showActions = showMenu->actions();
    verify(showActions.size() == 4
               && showActions.at(0) == window.m_showHiddenAction
               && showActions.at(1) == window.m_thumbnailsAction
               && showActions.at(2) == window.m_previewAction
               && showActions.at(3) == window.m_fullNamesAction,
           "Show contains only the four implemented display commands");
    verify(window.m_showHiddenAction->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_H)
               && window.m_previewAction->shortcut() == QKeySequence(Qt::ALT | Qt::Key_P),
           "Show preserves Hidden items and Preview pane shortcuts");

    QAction *listModeAction = viewMenu->findChild<QAction *>(QStringLiteral("viewModeAction1"));
    QAction *detailsModeAction = viewMenu->findChild<QAction *>(QStringLiteral("viewModeAction2"));
    QAction *compactModeAction = viewMenu->findChild<QAction *>(QStringLiteral("viewModeAction3"));
    verify(listModeAction && detailsModeAction && compactModeAction,
           "View mode actions remain addressable as one radio group");
    window.setActivePane(ThisPcWindow::PaneId::Primary);
    listModeAction->trigger();
    verify(window.m_directoryViewMode == 1 && window.m_splitPane->viewMode() == 0,
           "View mode routes to the active primary pane");
    window.setActivePane(ThisPcWindow::PaneId::Split);
    viewMenu->aboutToShow();
    verify(applyViewToSubfolders->isEnabled() && !removeViewFromSubfolders->isVisible(),
           "top-level View profile commands reflect the active Split location");
    applyViewToSubfolders->trigger();
    verify(DirectoryViewSettings::hasInheritedRule(right),
           "top-level View Apply routes to the active Split pane");
    viewMenu->aboutToShow();
    verify(removeViewFromSubfolders->isVisible() && removeViewFromSubfolders->isEnabled(),
           "top-level View reveals Remove after Apply creates a rule");
    removeViewFromSubfolders->trigger();
    verify(!DirectoryViewSettings::hasInheritedRule(right)
               && DirectoryViewSettings::hasExplicitProfile(right),
           "top-level View Remove deletes only the Split ancestor rule");
    detailsModeAction->trigger();
    verify(window.m_directoryViewMode == 1 && window.m_splitPane->viewMode() == 2,
           "View mode routes to the active split pane");
    window.updateFileActionStates();
    verify(detailsModeAction->isChecked() && !listModeAction->isChecked(),
           "View radio state follows the active pane");
    compactModeAction->trigger();
    verify(window.m_directoryViewMode == 1 && window.m_splitPane->viewMode() == 3
               && window.m_splitPane->listView()->compactMode()
               && window.m_splitPane->listView()->flow() == QListView::TopToBottom
               && window.m_splitPane->listView()->isWrapping(),
           "Compact routes to the active split pane and applies a real column layout");

    QAction *smallerAction = iconSizeMenu->findChild<QAction *>(QStringLiteral("iconSizeSmaller"));
    QAction *largerAction = iconSizeMenu->findChild<QAction *>(QStringLiteral("iconSizeLarger"));
    verify(smallerAction && largerAction,
           "icon size step actions remain addressable");
    window.setDirectoryViewMode(0);
    window.m_splitPane->setViewMode(0);
    window.setDirectoryIconSizeStep(5);
    window.m_splitPane->setIconSizeStep(3);
    window.setActivePane(ThisPcWindow::PaneId::Primary);
    largerAction->trigger();
    verify(window.m_directoryIconSizeStep == 6
               && window.m_directoryList->iconSize() == QSize(80, 80)
               && window.m_splitPane->iconSizeStep() == 3,
           "icon size routes to active primary pane");
    window.setActivePane(ThisPcWindow::PaneId::Split);
    smallerAction->trigger();
    verify(window.m_splitPane->iconSizeStep() == 2
               && window.m_splitPane->listView()->iconSize() == QSize(40, 40)
               && window.m_directoryIconSizeStep == 6,
           "icon size routes to active split pane");
    window.updateFileActionStates();
    verify(iconSizeMenu->isEnabled()
               && iconSizeMenu->findChild<QAction *>(QStringLiteral("iconSizeCurrent"))
                      ->text().contains(QStringLiteral("40")),
           "icon-size current step follows the active pane");
    window.m_splitPane->setViewMode(1);
    window.updateFileActionStates();
    verify(!iconSizeMenu->isEnabled(),
           "icon-size control is disabled outside Icons mode");

    window.setDirectoryViewMode(0);
    window.setDirectoryIconSizeStep(4);
    window.m_directoryList->setFocus();
    QTest::keyClick(window.m_directoryList, Qt::Key_Plus, Qt::ControlModifier);
    verify(window.m_directoryIconSizeStep == 5,
           "Ctrl Plus advances the active primary pane icon-size step");
    QTest::keyClick(window.m_directoryList, Qt::Key_Equal, Qt::ControlModifier);
    verify(window.m_directoryIconSizeStep == 6,
           "Ctrl Equal advances the active primary pane icon-size step");
    QTest::keyClick(window.m_directoryList, Qt::Key_Minus, Qt::ControlModifier);
    verify(window.m_directoryIconSizeStep == 5,
           "Ctrl Minus reduces the active primary pane icon-size step");
    verify(DirectoryViewSettings::resolveProfile(left).iconSizeStep == 5,
           "primary icon-size shortcut persists the exact folder profile step");

    window.m_splitPane->setViewMode(0);
    window.m_splitPane->setIconSizeStep(3);
    window.m_splitPane->listView()->setFocus();
    QTest::keyClick(window.m_splitPane->listView(), Qt::Key_Equal,
                    Qt::ControlModifier);
    verify(window.m_splitPane->iconSizeStep() == 4
               && window.m_directoryIconSizeStep == 5,
           "icon-size shortcut routes to the focused split pane");
    verify(DirectoryViewSettings::resolveProfile(right).iconSizeStep == 4,
           "split icon-size shortcut persists the exact folder profile step");

    window.m_splitPane->setIconSizeStep(0);
    QTest::keyClick(window.m_splitPane->listView(), Qt::Key_Minus,
                    Qt::ControlModifier);
    verify(window.m_splitPane->iconSizeStep() == 0,
           "icon-size shortcut minimum boundary is a no-op");
    window.m_splitPane->setIconSizeStep(DirectoryViewSettings::iconSizeStepCount() - 1);
    QTest::keyClick(window.m_splitPane->listView(), Qt::Key_Plus,
                    Qt::ControlModifier);
    verify(window.m_splitPane->iconSizeStep()
               == DirectoryViewSettings::iconSizeStepCount() - 1,
           "icon-size shortcut maximum boundary is a no-op");

    window.m_splitPane->setViewMode(1);
    const int listStep = window.m_splitPane->iconSizeStep();
    QTest::keyClick(window.m_splitPane->listView(), Qt::Key_Minus,
                    Qt::ControlModifier);
    verify(window.m_splitPane->iconSizeStep() == listStep,
           "icon-size shortcut is disabled outside Icons mode");
    window.m_splitPane->setViewMode(0);
    const int plainStep = window.m_splitPane->iconSizeStep();
    QTest::keyClick(window.m_splitPane->listView(), Qt::Key_Plus);
    QTest::keyClick(window.m_splitPane->listView(), Qt::Key_Minus);
    QTest::keyClick(window.m_splitPane->listView(), Qt::Key_Equal);
    verify(window.m_splitPane->iconSizeStep() == plainStep,
           "plain Plus, Minus, and Equal do not change icon size");

    window.m_searchEdit->setFocus();
    QTest::keyClick(window.m_searchEdit, Qt::Key_Plus, Qt::ControlModifier);
    verify(window.m_splitPane->iconSizeStep() == plainStep,
           "Search editor focus blocks icon-size shortcuts");
    window.m_addressStack->setCurrentWidget(window.m_addressEdit);
    window.m_addressEdit->setFocus();
    QTest::keyClick(window.m_addressEdit, Qt::Key_Minus, Qt::ControlModifier);
    verify(window.m_splitPane->iconSizeStep() == plainStep,
           "address editor focus blocks icon-size shortcuts");
    window.m_addressStack->setCurrentWidget(window.m_breadcrumbFrame);

    window.m_splitPane->setCurrentUrl(kThisPcUrl, false);
    const int virtualStep = window.m_splitPane->iconSizeStep();
    window.adjustActiveIconSizeStep(1);
    verify(window.m_splitPane->iconSizeStep() == virtualStep,
           "This PC virtual root rejects icon-size step changes");
    window.m_splitPane->setCurrentUrl(QUrl(QStringLiteral("thispcsearch:/query")), false);
    const int searchStep = window.m_splitPane->iconSizeStep();
    window.adjustActiveIconSizeStep(-1);
    verify(window.m_splitPane->iconSizeStep() == searchStep,
           "This PC search results reject icon-size step changes");
    window.m_splitPane->setCurrentUrl(right, false);
    window.m_splitPane->cancelListing();

    window.setDirectoryViewMode(0);
    window.m_splitPane->setViewMode(0);
    window.setActivePane(ThisPcWindow::PaneId::Primary);
    auto fill = [](DirectoryListWidget *list, DirectoryTreeWidget *tree, const QUrl &directory) {
        list->clear(); tree->clear();
        for (const auto &name : {QStringLiteral("one"), QStringLiteral("two")}) {
            const QUrl url = childUrlWithName(directory, name);
            FileInfo file{name, QString(), QString(), url, false, 0, 0};
            list->addFileItem(file, QIcon(), QStringLiteral("File"),
                              QStringLiteral("0"), QStringLiteral("Today"), QString());
            auto *ti = new QTreeWidgetItem(tree, QStringList{name, "File", "0", "Today"});
            ti->setFlags(ti->flags() | Qt::ItemIsEditable);
            ti->setData(0, Qt::UserRole, url.toString());
            ti->setData(0, Qt::UserRole + 1, false);
            ti->setData(0, directory_view_detail::FileItemRole, true);
        }
    };
    fill(window.m_directoryList, window.m_directoryDetails, left);
    fill(window.m_splitPane->listView(), window.m_splitPane->detailsView(), right);

    // 0.32 Stage 4: the neutral pane contract reads and routes both concrete panes.
    window.setActivePane(ThisPcWindow::PaneId::Primary);
    auto primaryContext = window.paneContext();
    verify(primaryContext.id == ThisPcWindow::PaneId::Primary
               && primaryContext.directory == left
               && primaryContext.view == window.m_directoryList
               && primaryContext.isDirectory,
           "PaneContext describes the primary directory pane");
    verify(primaryContext.items.isEmpty() && window.selectedUrls().isEmpty(),
           "primary PaneContext and selectedUrls are empty without selection");
    window.m_directoryList->selectionModel()->select(
        window.m_directoryList->item(0), QItemSelectionModel::ClearAndSelect);
    primaryContext = window.paneContext();
    verify(primaryContext.items.size() == 1
               && primaryContext.items.first().url == childUrlWithName(left, "one")
               && window.selectedUrls() == QList<QUrl>{childUrlWithName(left, "one")},
           "primary PaneContext and selectedUrls expose the primary selection");
    window.m_directoryList->clearSelection();

    window.m_directoryList->setFocus(Qt::OtherFocusReason);
    QWidget *focusBeforeActivation = QApplication::focusWidget();
    window.setActivePane(ThisPcWindow::PaneId::Split);
    auto splitContext = window.paneContext();
    verify(splitContext.id == ThisPcWindow::PaneId::Split
               && splitContext.directory == right
               && splitContext.view == window.m_splitPane->listView()
               && splitContext.isDirectory,
           "PaneContext describes the Split directory pane");
    verify(QApplication::focusWidget() == focusBeforeActivation
               && window.m_splitPane->property("active").toBool()
               && !window.m_primaryPane->property("active").toBool(),
           "setActivePane updates pane semantics without stealing focus");
    verify(splitContext.items.isEmpty() && window.selectedUrls().isEmpty(),
           "Split PaneContext and selectedUrls are empty without selection");
    window.m_splitPane->listView()->selectionModel()->select(
        window.m_splitPane->listView()->item(1), QItemSelectionModel::ClearAndSelect);
    splitContext = window.paneContext();
    verify(splitContext.items.size() == 1
               && splitContext.items.first().url == childUrlWithName(right, "two")
               && window.selectedUrls() == QList<QUrl>{childUrlWithName(right, "two")},
           "Split PaneContext and selectedUrls expose the Split selection");
    window.m_splitPane->listView()->clearSelection();

    const QUrl primaryRoute = QUrl::fromLocalFile(files.path() + "/primary-route");
    const QUrl splitRoute = QUrl::fromLocalFile(files.path() + "/split-route");
    QDir().mkpath(primaryRoute.toLocalFile());
    QDir().mkpath(splitRoute.toLocalFile());
    window.navigatePane(ThisPcWindow::PaneId::Primary, primaryRoute);
    window.navigatePane(ThisPcWindow::PaneId::Split, splitRoute);
    verify(window.m_navigation.currentUrl() == primaryRoute && window.m_splitPane->currentUrl() == splitRoute,
           "navigatePane routes Primary and Split independently");
    window.m_primaryPane->cancelListing();
    window.m_splitPane->cancelListing();
    window.navigateTo(left, false);
    window.m_splitPane->setCurrentUrl(right, false);
    window.m_primaryPane->cancelListing();
    window.m_splitPane->cancelListing();

    interceptPaneRefreshes = true;
    refreshedPanes.clear();
    window.refreshPane(ThisPcWindow::PaneId::Primary);
    window.refreshPane(ThisPcWindow::PaneId::Split);
    interceptPaneRefreshes = false;
    verify(refreshedPanes == QList<int>({0, 1}),
           "refreshPane routes Primary and Split independently");

    window.openInOtherPane(ThisPcWindow::PaneId::Primary, splitRoute);
    verify(window.m_navigation.currentUrl() == left && window.m_splitPane->currentUrl() == splitRoute,
           "openInOtherPane routes Primary to Split");
    window.openInOtherPane(ThisPcWindow::PaneId::Split, primaryRoute);
    verify(window.m_navigation.currentUrl() == primaryRoute && window.m_splitPane->currentUrl() == splitRoute,
           "openInOtherPane routes Split to Primary");
    window.m_primaryPane->cancelListing();
    window.m_splitPane->cancelListing();
    window.navigateTo(left, false);
    window.m_splitPane->setCurrentUrl(right, false);
    window.m_primaryPane->cancelListing();
    window.m_splitPane->cancelListing();

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
    verify(primaryHeader->height() == splitHeader->height(),
           "Primary and Split address bar headers share identical runtime height");
    verify(window.m_addressStack && window.m_splitPane->m_locationStack
               && window.m_addressStack->height() == window.m_splitPane->m_locationStack->height(),
           "Primary and Split address stacks share identical runtime height");

    const auto globalTop = [](const QWidget *widget) {
        return widget->mapToGlobal(QPoint(0, 0)).y();
    };
    const auto verifyVerticalParity = [&](int mode, ThisPcWindow::PaneId activePane) {
        window.setActivePane(ThisPcWindow::PaneId::Primary);
        window.setDirectoryViewMode(mode);
        window.m_splitPane->setViewMode(mode);
        window.setActivePane(activePane);
        app.processEvents();

        auto *primaryView = mode == 2
            ? static_cast<QAbstractItemView *>(window.m_directoryDetails)
            : static_cast<QAbstractItemView *>(window.m_directoryList);
        auto *splitView = mode == 2
            ? static_cast<QAbstractItemView *>(window.m_splitPane->m_details)
            : static_cast<QAbstractItemView *>(window.m_splitPane->m_list);
        auto *primaryContentHeader = window.m_directoryTitle->parentWidget();
        auto *splitContentHeader = window.m_splitPane->m_title->parentWidget();
        auto *primaryCrumb = window.m_breadcrumbLayout->count() > 0
            ? window.m_breadcrumbLayout->itemAt(0)->widget() : nullptr;
        auto *splitCrumb = window.m_splitPane->m_breadcrumbButton;

        verify(globalTop(primaryHeader) == globalTop(splitHeader)
                   && primaryHeader->height() == splitHeader->height()
                   && primaryHeader->sizeHint().height() == splitHeader->sizeHint().height()
                   && primaryHeader->minimumHeight() == splitHeader->minimumHeight()
                   && primaryHeader->contentsRect().height() == splitHeader->contentsRect().height(),
               "Primary/Split pane headers retain identical Y and height metrics");
        verify(globalTop(window.m_breadcrumbFrame) == globalTop(window.m_splitPane->m_breadcrumbFrame)
                   && window.m_breadcrumbFrame->height() == window.m_splitPane->m_breadcrumbFrame->height()
                   && window.m_breadcrumbFrame->minimumHeight()
                       == window.m_splitPane->m_breadcrumbFrame->minimumHeight()
                   && window.m_breadcrumbFrame->contentsMargins()
                       == window.m_splitPane->m_breadcrumbFrame->contentsMargins(),
               "Primary/Split breadcrumb frames retain identical Y and height metrics");
        verify(primaryCrumb && splitCrumb
                   && globalTop(primaryCrumb) == globalTop(splitCrumb),
               "Primary/Split rendered breadcrumb rows retain identical top Y");
        verify(window.m_breadcrumbFrame->m_scroll
                   && globalTop(window.m_breadcrumbFrame->m_scroll->viewport())
                       == globalTop(window.m_splitPane->m_breadcrumbScroll->viewport()),
               "Primary/Split breadcrumb viewports retain identical top Y");
        verify(globalTop(primaryContentHeader) == globalTop(splitContentHeader),
               "Primary/Split content headers retain identical top Y");
        verify(globalTop(primaryView->viewport()) == globalTop(splitView->viewport()),
               "Primary/Split listing viewports retain identical top Y");
    };

    for (int mode : {0, 1, 2, 3}) {
        verifyVerticalParity(mode, ThisPcWindow::PaneId::Primary);
        verifyVerticalParity(mode, ThisPcWindow::PaneId::Split);
    }

    window.m_addressStack->setCurrentWidget(window.m_addressEdit);
    window.m_splitPane->m_locationStack->setCurrentWidget(window.m_splitPane->m_addressEdit);
    app.processEvents();
    verify(globalTop(window.m_addressEdit) == globalTop(window.m_splitPane->m_addressEdit)
               && window.m_addressEdit->height() == window.m_splitPane->m_addressEdit->height(),
           "Primary/Split active address editors retain identical Y and height");
    window.m_addressStack->setCurrentWidget(window.m_breadcrumbFrame);
    window.m_splitPane->m_locationStack->setCurrentWidget(window.m_splitPane->m_breadcrumbFrame);
    app.processEvents();
    verify(primaryHeader->mapToGlobal(QPoint()).x() == window.m_primaryPane->mapToGlobal(QPoint()).x()
               && splitHeader->mapToGlobal(QPoint()).x() == window.m_splitPane->mapToGlobal(QPoint()).x(),
           "address sections align with their pane edges");
    auto *primaryContentHeader = window.m_directoryTitle->parentWidget();
    auto *splitContentHeader = window.m_splitPane->m_title->parentWidget();
    auto *primaryContentLayout = qobject_cast<QVBoxLayout *>(primaryContentHeader->layout());
    auto *splitContentLayout = qobject_cast<QVBoxLayout *>(splitContentHeader->layout());
    verify(primaryContentLayout && splitContentLayout
               && primaryContentLayout->contentsMargins() == splitContentLayout->contentsMargins()
               && primaryContentLayout->spacing() == splitContentLayout->spacing()
               && window.m_directoryTitle->font() == window.m_splitPane->m_title->font()
               && window.m_directoryStatus->foregroundRole()
                   == window.m_splitPane->m_status->foregroundRole(),
           "Primary and Split content headers share margins typography spacing and color");
    window.m_splitPane->setCurrentUrl(left, false);
    window.m_splitPane->cancelListing();
    app.processEvents();
    verify(window.m_directoryTitle->text() == window.m_splitPane->m_title->text()
               && window.m_directoryTitle->text() == LocationPresentation::contentHeaderText(left),
           "same local URL has identical full-path content header presentation");

    // Per-URL view settings symmetric persistence:
    window.navigateTo(left, false);
    window.m_splitPane->setCurrentUrl(left, false);
    window.m_primaryPane->cancelListing();
    window.m_splitPane->cancelListing();
    app.processEvents();

    window.setDirectoryViewMode(2);
    window.setDirectoryIconSizeStep(6);
    window.setSortKey(1);
    window.setSortAscending(false);
    window.setGroupMode(DirectoryViewSettings::GroupByType);
    app.processEvents();

    verify(window.m_splitPane->viewMode() != 2
               || window.m_splitPane->iconSizeStep() != 6
               || window.m_splitPane->sortKey() != 1
               || window.m_splitPane->sortAscending()
               || window.m_splitPane->groupMode() != DirectoryViewSettings::GroupByType,
           "Split pane does not live-sync view profile before refresh");

    window.m_splitPane->refresh();
    window.m_splitPane->cancelListing();
    app.processEvents();
    verify(window.m_splitPane->viewMode() == 2
               && window.m_splitPane->iconSizeStep() == 6
               && window.m_splitPane->sortKey() == 1
               && !window.m_splitPane->sortAscending()
               && window.m_splitPane->groupMode() == DirectoryViewSettings::GroupByType,
           "Primary view profile changes propagate to Split on refresh");

    window.m_splitPane->setViewMode(1);
    window.m_splitPane->setIconSizeStep(2);
    window.m_splitPane->setSortState(2, true);
    window.m_splitPane->setGroupMode(DirectoryViewSettings::GroupByDate);
    app.processEvents();

    verify(window.m_directoryViewMode != 1
               || window.m_directoryIconSizeStep != 2
               || window.m_sortKey != 2
               || !window.m_sortAscending
               || window.m_groupMode != DirectoryViewSettings::GroupByDate,
           "Primary pane does not live-sync view profile before refresh");

    window.refreshPane(ThisPcWindow::PaneId::Primary);
    window.m_primaryPane->cancelListing();
    app.processEvents();
    verify(window.m_directoryViewMode == 1
               && window.m_directoryIconSizeStep == 2
               && window.m_sortKey == 2
               && window.m_sortAscending
               && window.m_groupMode == DirectoryViewSettings::GroupByDate,
           "Split view profile changes propagate to Primary on refresh");

    window.m_splitPane->setCurrentUrl(right, false);
    window.m_splitPane->cancelListing();
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
               && window.m_navigation.currentUrl() == left,
           "sidebar navigates right active pane and preserves pane selection");
    const QUrl otherPaneTarget = QUrl::fromLocalFile(files.path() + "/other-pane-target");
    QDir().mkpath(otherPaneTarget.toLocalFile());
    window.m_sidebar->openInSplitPaneRequested(otherPaneTarget);
    verify(window.m_activePane == ThisPcWindow::PaneId::Split
               && window.m_navigation.currentUrl() == otherPaneTarget
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
    window.m_primaryPane->cancelListing();
    window.m_splitPane->cancelListing();
    fill(window.m_directoryList, window.m_directoryDetails, left);
    fill(window.m_splitPane->listView(), window.m_splitPane->detailsView(), right);

    auto appendThird = [](DirectoryListWidget *list, DirectoryTreeWidget *tree,
                          const QUrl &directory) {
        const QString name = QStringLiteral("three");
        const QUrl url = childUrlWithName(directory, name);
        FileInfo file{name, QString(), QString(), url, false, 0, 0};
        addDirectoryFileItems(list, tree, file, QIcon(), QStringLiteral("File"),
                              QStringLiteral("0"), QStringLiteral("Today"));
    };
    appendThird(window.m_directoryList, window.m_directoryDetails, left);
    appendThird(window.m_splitPane->listView(), window.m_splitPane->detailsView(), right);

    auto selectedUrls = [](QAbstractItemView *view) {
        QSet<QString> urls;
        for (const QModelIndex &index : view->selectionModel()->selectedRows()) {
            const QString url = qobject_cast<DirectoryTreeWidget *>(view)
                ? index.siblingAtColumn(0).data(Qt::UserRole).toString()
                : index.data(directory_view_detail::UrlRole).toString();
            if (!url.isEmpty()) urls.insert(url);
        }
        return urls;
    };
    auto verifyExactSelection = [&](QAbstractItemView *view,
                                    const QSet<QString> &expected,
                                    const char *description) {
        const QSet<QString> actual = selectedUrls(view);
        verify(actual == expected
                   && view->currentIndex().isValid()
                   && view->selectionModel()->isSelected(view->currentIndex())
                   && expected.contains(qobject_cast<DirectoryTreeWidget *>(view)
                       ? view->currentIndex().siblingAtColumn(0).data(Qt::UserRole).toString()
                       : view->currentIndex().data(directory_view_detail::UrlRole).toString()),
               description);
    };

    // GUI/offscreen regression for the intermittent Icons -> List -> Details
    // expansion.  Repeat the real pane paths so queued layout/current events
    // are drained between every transition.
    for (int iteration = 0; iteration < 50; ++iteration) {
        for (bool split : {false, true}) {
            DirectoryListWidget *list = split
                ? window.m_splitPane->listView() : window.m_directoryList;
            DirectoryTreeWidget *details = split
                ? window.m_splitPane->detailsView() : window.m_directoryDetails;
            const QUrl directory = split ? right : left;
            const auto setMode = [&](int mode) {
                if (split) window.m_splitPane->setViewMode(mode);
                else window.setDirectoryViewMode(mode);
                app.processEvents();
            };
            const QString middle = childUrlWithName(directory, QStringLiteral("two")).toString();
            const QString last = childUrlWithName(directory, QStringLiteral("three")).toString();

            setMode(0);
            list->selectionModel()->setCurrentIndex(
                list->item(1), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
            const QSet<QString> one{middle};
            setMode(1);
            verifyExactSelection(list, one,
                "Icons -> List preserves exactly the single selected URL");
            setMode(2);
            verifyExactSelection(details, one,
                "Icons -> List -> Details preserves exactly the single selected URL");
            app.processEvents();
            verifyExactSelection(details, one,
                "queued events do not expand Details selection after restore");
            setMode(3);
            verifyExactSelection(list, one,
                "Details -> Compact preserves the exact single selection");
            setMode(0);
            verifyExactSelection(list, one,
                "Compact -> Icons preserves the exact single selection");

            list->selectionModel()->setCurrentIndex(
                list->item(1), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
            list->selectionModel()->select(list->item(2), QItemSelectionModel::Select);
            const QSet<QString> two{middle, last};
            for (int mode : {1, 2, 3, 0}) {
                setMode(mode);
                QAbstractItemView *active = mode == 2
                    ? static_cast<QAbstractItemView *>(details)
                    : static_cast<QAbstractItemView *>(list);
                verifyExactSelection(active, two,
                    "all view modes preserve exactly two of three selected URLs");
            }
        }
    }

    // Restore the original two-item fixture expected by the remaining pane
    // action matrix.
    fill(window.m_directoryList, window.m_directoryDetails, left);
    fill(window.m_splitPane->listView(), window.m_splitPane->detailsView(), right);

    for (QAbstractItemView *view : {
             static_cast<QAbstractItemView *>(window.m_directoryList),
             static_cast<QAbstractItemView *>(window.m_splitPane->listView())}) {
        view->selectionModel()->setCurrentIndex(
            view->model()->index(0, 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        view->selectionModel()->select(
            view->model()->index(1, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    for (int mode : {2, 3, 1, 0}) {
        window.setDirectoryViewMode(mode);
        window.m_splitPane->setViewMode(mode);
        QAbstractItemView *primary = mode == 2
            ? static_cast<QAbstractItemView *>(window.m_directoryDetails)
            : static_cast<QAbstractItemView *>(window.m_directoryList);
        QAbstractItemView *split = mode == 2
            ? static_cast<QAbstractItemView *>(window.m_splitPane->detailsView())
            : static_cast<QAbstractItemView *>(window.m_splitPane->listView());
        verify(primary->selectionModel()->selectedRows().size() == 2
                   && primary->currentIndex().isValid()
                   && primary->selectionModel()->isSelected(primary->currentIndex()),
               "Primary Icons/List/Details/Compact switch preserves multi-selection and current");
        verify(split->selectionModel()->selectedRows().size() == 2
                   && split->currentIndex().isValid()
                   && split->selectionModel()->isSelected(split->currentIndex()),
               "Split Icons/List/Details/Compact switch preserves multi-selection and current");
    }

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
    for (int mode : {0, 1, 2, 3}) {
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
            QTest::keyClick(view, Qt::Key_F2);
            QWidget *renameEditor = nullptr;
            verify(QTest::qWaitFor([&] {
                       renameEditor = view->findChild<QLineEdit *>();
                       if (!renameEditor) {
                           for (QPlainTextEdit *candidate : view->findChildren<QPlainTextEdit *>()) {
                               if (candidate->isVisible()) { renameEditor = candidate; break; }
                           }
                       }
                       return renameEditor && renameEditor->isVisible();
                   }, 250),
                   "F2 opens the inline rename editor");
            if (auto *line = qobject_cast<QLineEdit *>(renameEditor)) line->selectAll();
            if (auto *plain = qobject_cast<QPlainTextEdit *>(renameEditor)) plain->selectAll();
            QTest::keyClicks(renameEditor, "renamed");
            QTest::keyClick(renameEditor, Qt::Key_Return);
            app.processEvents();
            verify(dispatch.kind == "rename" && dispatch.sources == QList<QUrl>{file}
                && dispatch.destination == childUrlWithName(directory, "renamed"), "F2 keeps source and parent");
            dispatch = {};
            window.renameSelected();
            QWidget *menuRenameEditor = nullptr;
            verify(QTest::qWaitFor([&] {
                       menuRenameEditor = view->findChild<QLineEdit *>();
                       if (!menuRenameEditor) {
                           for (QPlainTextEdit *candidate : view->findChildren<QPlainTextEdit *>()) {
                               if (candidate->isVisible()) { menuRenameEditor = candidate; break; }
                           }
                       }
                       return menuRenameEditor && menuRenameEditor->isVisible();
                   }, 250),
                   "single-item Rename action opens the same inline editor");
            QTest::keyClick(menuRenameEditor, Qt::Key_Escape);
            verify(dispatch.kind.isEmpty(),
                   "canceling Rename action inline editor starts no backend operation");
            focus(view); dispatch = {};
            QTest::keyClick(view, Qt::Key_Return, Qt::AltModifier);
            verify(dispatch.kind == "properties" && dispatch.sources == QList<QUrl>{file}, "Alt+Enter properties");
            QTest::keyClick(view, Qt::Key_A, Qt::ControlModifier);
            verify(window.selectedUrls().size() == 2, "Ctrl+A active pane");
            verify(other->selectionModel()->selectedRows().size() == 1, "Ctrl+A leaves other selection alone");
            verify(window.m_batchRenameAction->isEnabled(),
                   "Batch Rename is enabled for multiple selection in the active pane");
            QTimer::singleShot(0, &window, [&, split, directory] {
                auto *batch = dynamic_cast<BatchRenameDialog *>(QApplication::activeModalWidget());
                verify(batch != nullptr, "Batch Rename dialog opened");
                window.setActivePane(split ? ThisPcWindow::PaneId::Primary
                                           : ThisPcWindow::PaneId::Split);
                verify(batch->m_urls.size() == 2
                           && QFileInfo(batch->m_urls.first().toLocalFile()).absolutePath()
                               == directory.toLocalFile(),
                       "Batch Rename retains the initiating pane snapshot after focus changes");
                batch->reject();
            });
            window.batchRenameSelected();
            focus(view);
            selectOne(view);
            dialog(true, true, other);
            QTest::keyClick(view, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
            verify(dispatch.kind == "mkdir" && dispatch.destination == childUrlWithName(directory, "renamed"), "create folder captures directory");
            focus(view);
            QFile occupied(directory.toLocalFile() + "/new.txt");
            if (!occupied.exists()) {
                verify(occupied.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                       "occupied default fixture is created per pane");
                occupied.close();
            }
            QTimer::singleShot(0, &window, [&, other] {
                auto *box = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                verify(box && box->textValue() == "new (1).txt",
                       "active pane collision is suggested before input");
                window.setActivePane(other == window.m_directoryList || other == window.m_directoryDetails
                    ? ThisPcWindow::PaneId::Primary : ThisPcWindow::PaneId::Split);
                box->setTextValue("renamed");
                box->accept();
            });
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

            focus(view);
            verify(window.m_templateMenu->menuAction()->isEnabled(), "Templates enabled in active directory");
            QTimer::singleShot(0, &window, [&] {
                auto *nameDialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                verify(nameDialog && nameDialog->textValue() == templateSource.fileName(),
                       "template dialog suggests the exact source name and extension");
                window.setActivePane(split ? ThisPcWindow::PaneId::Primary : ThisPcWindow::PaneId::Split);
                nameDialog->setTextValue("  from template.md  ");
                nameDialog->accept();
            });
            dispatch = {};
            window.m_templateMenu->actions().first()->trigger();
            verify(dispatch.kind == "template" && dispatch.sources == QList<QUrl>{templateSource}
                       && dispatch.destination == childUrlWithName(directory, "from template.md"),
                   "template action keeps initiating pane through modal focus change and trims target name");
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
    verify(window.m_splitPane->currentUrl() == kThisPcUrl && window.m_navigation.currentUrl() == left
               && window.m_activePane == Pane::Split,
           "sidebar This PC targets the active right pane");
    verify(window.m_splitPane->contentStack()->currentWidget() == window.m_splitHomePage
               && window.m_splitHomePage->isVisible() && window.m_splitPane->listingJob() == nullptr,
           "right This PC presents the card page without a directory-list job");
    verify(!window.paneContext().isDirectory && !window.m_upAction->isEnabled()
               && !window.m_viewButton->isEnabled() && !window.m_sortButton->isEnabled()
               && !window.m_copyAction->isEnabled() && !window.m_newButton->isEnabled(),
           "right This PC shared controls match left virtual-page policy");
    dispatch = {};
    verify(!window.m_templateMenu->menuAction()->isEnabled(), "Templates disabled on right This PC");
    window.createFromTemplate(templateSource);
    verify(dispatch.kind.isEmpty(), "right This PC cannot dispatch template creation");
    verify(window.m_splitPane->m_breadcrumbButton->text() == "This PC"
               && window.m_searchEdit->placeholderText() == "Search this computer"
               && window.m_searchScopeGroup->checkedAction()->data().toInt() == 2,
           "right This PC has friendly breadcrumb and whole-computer Search scope");
    window.m_driveHomeCoordinator.cancel();
    DriveInfo fixtureDrive;
    fixtureDrive.name = "Disposable drive";
    fixtureDrive.targetUrl = right;
    fixtureDrive.usedPercent = 37;
    fixtureDrive.capacityText = "100 GiB";
    fixtureDrive.freeText = "63 GiB";
    window.m_driveHomeCoordinator.setSnapshotForTesting({fixtureDrive});
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
    verify(window.m_splitPane->currentUrl() == right && window.m_navigation.currentUrl() == left
               && window.m_activePane == Pane::Split,
           "right drive card keyboard activation opens its real target in right pane");
    window.m_upAction->trigger();
    verify(window.m_splitPane->currentUrl() == kThisPcUrl,
           "right drive-root Up returns to This PC instead of the filesystem parent");
    window.m_backAction->trigger();
    verify(window.m_splitPane->currentUrl() == right, "right Back restores drive after This PC");
    window.m_forwardAction->trigger();
    verify(window.m_splitPane->currentUrl() == kThisPcUrl, "right Forward restores This PC card page");
    window.m_driveHomeCoordinator.cancel();
    window.m_refreshAction->trigger();
    verify(window.m_driveHomeCoordinator.isLoading() && !window.m_splitPane->listingJob() && window.m_navigation.currentUrl() == left,
           "right This PC Refresh uses the existing shared drive backend");
    window.m_driveHomeCoordinator.cancel();
    window.navigateTo(kThisPcUrl, true);
    window.setActivePane(Pane::Primary);
    dispatch = {};
    verify(!window.m_templateMenu->menuAction()->isEnabled(), "Templates disabled on left This PC");
    window.createFromTemplate(templateSource);
    verify(dispatch.kind.isEmpty(), "left This PC cannot dispatch template creation");
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
    verify(window.m_splitPane->currentUrl() == right && window.m_navigation.currentUrl() == kThisPcUrl,
           "right folder card double-click affects only its owning pane");
    window.setActivePane(Pane::Split);
    leftDrive->setFocus();
    QTest::keyClick(leftDrive, Qt::Key_Return);
    verify(window.m_navigation.currentUrl() == right && window.m_splitPane->currentUrl() == right
               && window.m_activePane == Pane::Primary,
           "left drive card retains its accepted pane-local routing");
    window.setActivePane(Pane::Split);
    window.beginAddressEdit(Pane::Split);
    window.m_splitPane->m_addressEdit->setText("thispc:/");
    QTest::keyClick(window.m_splitPane->m_addressEdit, Qt::Key_Return);
    verify(window.m_splitPane->currentUrl() == kThisPcUrl && window.m_navigation.currentUrl() == right
               && window.m_splitPane->m_locationStack->currentWidget() == window.m_splitPane->m_breadcrumbFrame,
           "right address submission opens This PC and restores its breadcrumb");
    window.swapSplitPanes();
    verify(window.m_navigation.currentUrl() == kThisPcUrl && window.m_splitPane->currentUrl() == right
               && window.m_contentStack->currentWidget() == window.m_homePage,
           "pane swap preserves This PC virtual-page semantics");

    // Launch activation and dispatch seam verification:
    // 1. Direct launch of local file in primary pane
    dispatch = {};
    const QUrl testExe = QUrl::fromLocalFile(files.path() + "/test.exe");
    window.launchFile(testExe);
    verify(dispatch.kind == "launch" && dispatch.sources.value(0) == testExe,
           "primary pane launchFile dispatches through launch seam with local URL");

    // 2. Launch of thispc drive file resolves to mounted targetUrl
    DriveInfo testDrive;
    testDrive.id = QStringLiteral("drive-test");
    testDrive.targetUrl = left;
    testDrive.isMounted = true;
    window.m_driveHomeCoordinator.setSnapshotForTesting({testDrive});

    dispatch = {};
    const QUrl virtualExe(QStringLiteral("thispc:/drive-test/app.exe"));
    const QUrl resolvedExe = QUrl::fromLocalFile(left.toLocalFile() + "/app.exe");
    window.launchFile(virtualExe);
    verify(dispatch.kind == "launch" && dispatch.sources.value(0) == resolvedExe,
           "primary pane launchFile resolves thispc drive URL to local mount path");

    // 3. Split pane launchItem delegates through the same window launch seam
    dispatch = {};
    window.m_splitPane->setDrives({testDrive});
    window.m_splitPane->launchItem(virtualExe);
    verify(dispatch.kind == "launch" && dispatch.sources.value(0) == resolvedExe,
           "split pane launchItem resolves and delegates to window launch seam");

    qInfo("PASS: %d assertions, Icons/List/Details/Compact, both panes; KIO dispatch intercepted", checks);
}
