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
    qInfo("PASS: %d assertions, Icons/List/Details, both panes; KIO dispatch intercepted", checks);
}
