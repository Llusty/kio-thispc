/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Menus and destination dialog are real; only the final job dispatch is captured.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-archive-menu-test");
    QCoreApplication::setApplicationName("archive-menu-test");
    QTemporaryDir root;
    verify(root.isValid(), "disposable menu root");
    const QString sourceDir = root.filePath(QStringLiteral("real source ż"));
    const QString otherDir = root.filePath("other");
    const QString targetDir = root.filePath(QStringLiteral("selected destination ę"));
    for (const QString &path : {sourceDir, otherDir, targetDir}) verify(QDir().mkpath(path), "menu fixture directory");
    const QString archive = sourceDir + QStringLiteral("/źródło #.zip");
    QFile text(sourceDir + "/file.txt");
    verify(text.open(QIODevice::WriteOnly) && text.write("payload") == 7, "archive input fixture");
    text.close();
    QProcess zip;
    zip.setWorkingDirectory(sourceDir);
    zip.start(QStandardPaths::findExecutable("zip"), {"-q", archive, "file.txt"});
    verify(zip.waitForFinished(10000) && zip.exitCode() == 0, "create ZIP menu fixture");
    const QUrl source = QUrl::fromLocalFile(archive);
    const QUrl local = QUrl::fromLocalFile(sourceDir);
    const QUrl other = QUrl::fromLocalFile(otherDir);
    const QUrl target = QUrl::fromLocalFile(targetDir);
    const QUrl search = makeSearchLocation("zip", 0, local, 0, 0, 0);
    const QUrl fake = QUrl::fromLocalFile(sourceDir + "/file.txt");
    ThisPcWindow window(local, false);
    window.show();
    window.setSplitViewEnabled(true);
    app.processEvents();
    window.m_refreshTimer.stop();
    auto stopListings = [&] {
        window.m_primaryPane->cancelListing();
        window.m_splitPane->cancelListing();
        window.m_driveHomeCoordinator.cancel();
    };
    stopListings();
    interceptArchiveJobs = true;
    interceptPaneRefreshes = true;
    using Pane = ThisPcWindow::PaneId;
    auto fill = [&](DirectoryListWidget *list, DirectoryTreeWidget *tree, const QUrl &url) {
        list->clear(); tree->clear();
        for (const auto &value : {url, fake}) {
            FileInfo file{value.fileName(), QString(), QString(), value, false, 7, 0};
            list->addFileItem(file, QIcon(), QStringLiteral("File"),
                              QStringLiteral("7"), QStringLiteral("Today"), QString());
            auto *ti = new QTreeWidgetItem(tree, QStringList{value.fileName(), "File", "7", "Today"});
            ti->setData(0, Qt::UserRole, value.toString());
            ti->setData(0, Qt::UserRole + 1, false);
            ti->setData(0, directory_view_detail::FileItemRole, true);
        }
    };
    auto place = [&](Pane pane, bool searching, const QUrl &clicked = QUrl()) {
        window.m_navigation.updateCurrent(pane == Pane::Primary ? (searching ? search : local) : other);
        window.m_primaryPane->setCurrentUrl(window.m_navigation.currentUrl());
        window.m_splitPane->m_currentUrl = pane == Pane::Split ? (searching ? search : local) : other;
        window.m_contentStack->setCurrentWidget(window.m_directoryPage);
        window.m_splitPane->m_contentStack->setCurrentWidget(window.m_splitPane->m_directoryPage);
        fill(window.m_directoryList, window.m_directoryDetails, pane == Pane::Primary ? (clicked.isEmpty() ? source : clicked) : fake);
        fill(window.m_splitPane->listView(), window.m_splitPane->detailsView(), pane == Pane::Split ? (clicked.isEmpty() ? source : clicked) : fake);
    };
    auto menu = [&](Pane pane, int mode, bool expected, int choice, bool multiple = false) {
        auto *list = pane == Pane::Primary ? window.m_directoryList : window.m_splitPane->listView();
        auto *tree = pane == Pane::Primary ? window.m_directoryDetails : window.m_splitPane->detailsView();
        auto *view = mode == 2 ? static_cast<QAbstractItemView *>(tree) : list;
        if (multiple) {
            if (mode == 2) { tree->topLevelItem(0)->setSelected(true); tree->topLevelItem(1)->setSelected(true); }
            else { list->setRowSelected(0, true); list->setRowSelected(1, true); }
        }
        window.setActivePane(pane == Pane::Primary ? Pane::Split : Pane::Primary);
        dispatch = {};
        QTimer::singleShot(0, &window, [&, pane, mode, expected, choice] {
            auto *rootMenu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            verify(rootMenu, "real item menu opened");
            QMenu *extract = nullptr;
            bool sendTo = false;
            for (auto *action : rootMenu->actions()) {
                if (action->text() == "Extract") extract = action->menu();
                if (action->text() == "Send to") {
                    sendTo = true;
                    QMenu *sendMenu = action->menu();
                    verify(sendMenu != nullptr, "Send to submenu exists");
                    QStringList formats;
                    for (const QAction *entry : sendMenu->actions())
                        formats.append(entry->text());
                    verify(formats.contains("Compressed ZIP file…"), "existing ZIP menu action retained");
                    verify(formats.contains("Compressed 7z file…"), "7z creation menu action present");
                    verify(formats.contains("Compressed tar.gz file…"), "tar.gz creation menu action present");
                }
            }
            verify(bool(extract) == expected, "archive menu visibility");
            if (!sendTo) qFatal("FAIL: item menu became background menu (mode=%d pane=%d)",
                                mode, pane == Pane::Primary ? 0 : 1);
            ++checks;
            verify(window.paneContext().id == pane, "right click captures initiating pane");
            if (choice < 0) { rootMenu->close(); return; }
            verify(extract && extract->actions().size() == 2, "two extraction actions");
            verify(extract->actions()[0]->text() == "Extract Here"
                   && extract->actions()[1]->text() == "Extract To…", "extraction action labels");
            // The nested menu must preserve the snapshot despite focus/navigation.
            window.setActivePane(pane == Pane::Primary ? Pane::Split : Pane::Primary);
            window.m_navigation.updateCurrent(other);
            window.m_primaryPane->setCurrentUrl(other);
            window.m_splitPane->m_currentUrl = other;
            rootMenu->setActiveAction(extract->menuAction());
            QTest::keyClick(rootMenu, Qt::Key_Right);
            QTimer::singleShot(0, &window, [&, extract, choice] {
                if (choice == 1) QTimer::singleShot(0, &window, [&] {
                    auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                    verify(dialog, "Extract To owns its destination dialog");
                    verify(dialog->directory().canonicalPath() == QFileInfo(sourceDir).canonicalFilePath(),
                           "Search destination starts at the archive real directory");
                    dialog->setDirectory(targetDir);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                });
                extract->setActiveAction(extract->actions()[choice]);
                QTest::keyClick(extract, Qt::Key_Return);
            });
        });
        const QPoint point = mode == 2 ? tree->visualItemRect(tree->topLevelItem(0)).center()
                                       : list->visualItemRect(list->item(0)).center();
        verify(mode == 2 ? tree->itemAt(point) != nullptr : list->itemAt(point).isValid(),
               "item point resolves after layout and scroll offsets");
        QMetaObject::invokeMethod(view, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point));
        if (choice >= 0) {
            verify(dispatch.sources == QList<QUrl>{source}, "dispatch uses captured clicked archive, including Search");
            verify(dispatch.kind == (choice ? "extract-to" : "extract-here"), "correct extraction mode");
            verify(dispatch.destination == (choice ? target : local), "destination remains independent of focus/navigation");
            verify(window.m_runningArchivePaths.isEmpty(), "dispatch releases test busy guard");
        } else verify(dispatch.kind.isEmpty(), "dismissed or ineligible menu dispatches nothing");
    };
    auto backgroundMenu = [&](Pane pane, int mode) {
        auto *list = pane == Pane::Primary ? window.m_directoryList : window.m_splitPane->listView();
        auto *tree = pane == Pane::Primary ? window.m_directoryDetails : window.m_splitPane->detailsView();
        auto *view = mode == 2 ? static_cast<QAbstractItemView *>(tree) : list;
        view->doItemsLayout();
        app.processEvents();
        QPoint point = view->viewport()->rect().bottomRight() - QPoint(2, 2);
        verify(mode == 2 ? tree->itemAt(point) == nullptr : !list->itemAt(point).isValid(),
               "background point does not resolve to an item");
        QTimer::singleShot(0, &window, [&, pane] {
            auto *rootMenu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            verify(rootMenu, "real background menu opened");
            QStringList labels;
            for (const QAction *action : rootMenu->actions()) labels.append(action->text());
            verify(labels.contains("Refresh") && !labels.contains("Send to"),
                   "background menu is distinct from item menu");
            verify(window.paneContext().id == pane, "background right click captures initiating pane");
            rootMenu->close();
        });
        QMetaObject::invokeMethod(view, "customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, point));
    };
    for (int mode : {0, 1, 2, 3}) {
        window.setDirectoryViewMode(mode);
        window.m_splitPane->setViewMode(mode);
        stopListings();
        for (Pane pane : {Pane::Primary, Pane::Split}) {
            for (bool searching : {false, true}) {
                for (int choice : {0, 1}) {
                    place(pane, searching);
                    menu(pane, mode, true, choice);
                }
                place(pane, searching); menu(pane, mode, false, -1, true);
                place(pane, searching, fake); menu(pane, mode, false, -1);
                place(pane, searching, QUrl("sftp://example.test/file.zip")); menu(pane, mode, false, -1);
            }
        }
    }
    for (int mode : {0, 1, 2, 3}) {
        window.setDirectoryViewMode(mode);
        window.m_splitPane->setViewMode(mode);
        stopListings();
        for (Pane pane : {Pane::Primary, Pane::Split}) {
            place(pane, false);
            backgroundMenu(pane, mode);
        }
    }
    const QByteArray originalPath = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(root.filePath("no-executables")));
    window.setDirectoryViewMode(2);
    window.m_splitPane->setViewMode(2);
    place(Pane::Primary, false); menu(Pane::Primary, 2, false, -1);
    qputenv("PATH", originalPath);

    // Cancellation and duplicate identity checks execute the real entry point.
    QString alias = sourceDir + "/alias.zip";
    verify(QFile::link(archive, alias), "source symlink alias");
    QString hard = sourceDir + "/hard.zip";
    verify(::link(QFile::encodeName(archive).constData(), QFile::encodeName(hard).constData()) == 0, "source hardlink alias");
    const QString identity = thispcArchiveIdentity(source);
    verify(thispcArchiveIdentity(QUrl::fromLocalFile(alias)) == identity
           && thispcArchiveIdentity(QUrl::fromLocalFile(hard)) == identity, "duplicate identity follows symlinks and hardlinks");
    window.m_runningArchivePaths.insert(identity);
    dispatch = {};
    window.extractArchiveWithArk(QUrl::fromLocalFile(alias), false);
    window.extractArchiveWithArk(QUrl::fromLocalFile(hard), true);
    verify(dispatch.kind.isEmpty(), "duplicate requests cannot open dialog or dispatch");
    window.m_runningArchivePaths.clear();
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        verify(dialog, "destination dialog opens for cancellation");
        window.extractArchiveWithArk(source, false);
        verify(dispatch.kind.isEmpty(), "duplicate guarded while destination dialog is open");
        dialog->reject();
    });
    window.extractArchiveWithArk(source, true);
    verify(dispatch.kind.isEmpty() && window.m_runningArchivePaths.isEmpty(), "cancelled dialog releases identity and creates no job");

    const QString linkDir = root.filePath("directory-alias");
    verify(QFile::link(sourceDir, linkDir), "visible directory alias fixture");
    for (int combination = 0; combination < 5; ++combination) {
        window.m_navigation.updateCurrent(combination == 0 || combination == 2 ? local : other);
        window.m_primaryPane->setCurrentUrl(window.m_navigation.currentUrl());
        window.m_splitPane->m_currentUrl = combination == 1 || combination == 2
            ? QUrl::fromLocalFile(linkDir) : other;
        if (combination == 3) { window.m_navigation.updateCurrent(search); window.m_primaryPane->setCurrentUrl(search); }
        if (combination == 4) { window.m_splitPane->m_currentUrl = local; window.m_splitPane->hide(); }
        refreshedPanes.clear();
        window.refreshArchiveViews(local);
        const QList<int> expected = combination == 0 ? QList<int>{0} : combination == 1 ? QList<int>{1}
            : combination == 2 ? QList<int>{0, 1} : QList<int>{};
        verify(refreshedPanes == expected, "refresh only currently visible affected directories, including aliases");
    }
    // Integration: the real window starts real Ark, tracks the KJob, and only
    // refreshes after the worker/process/staging have been disposed.
    window.m_splitPane->show();
    window.m_navigation.updateCurrent(search);
    window.m_primaryPane->setCurrentUrl(search);
    window.m_splitPane->m_currentUrl = other;
    interceptArchiveJobs = false;
    refreshedPanes.clear();
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        verify(dialog, "real extraction destination dialog");
        dialog->setDirectory(targetDir);
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    window.extractArchiveWithArk(source, true);
    verify(!window.m_runningArchivePaths.isEmpty() && window.m_operationManager->activeCount() == 1,
           "real archive job is tracked and guarded");
    window.m_navigation.updateCurrent(other);
    window.m_primaryPane->setCurrentUrl(other);
    window.m_splitPane->m_currentUrl = target;
    window.setActivePane(Pane::Primary);
    verify(QTest::qWaitFor([&] { return window.m_runningArchivePaths.isEmpty(); }, 20000),
           "window extraction completes and releases its process identity");
    verify(window.m_operationManager->activeCount() == 0, "finished extraction leaves no active operation");
    QFile extracted(targetDir + "/file.txt");
    verify(extracted.open(QIODevice::ReadOnly) && extracted.readAll() == "payload",
           "window action produces verified real Ark output");
    verify(refreshedPanes == QList<int>{1}, "completion refreshes new visible target despite source focus/navigation");
    verify(QDir(targetDir).entryList({".thispc-extract-*"}, QDir::Dirs | QDir::Hidden).isEmpty(),
           "window refresh occurs after staging cleanup");

    qInfo("PASS: %d archive menu assertions; both panes, all views, Search, dialogs, duplicates and refresh", checks);
}
