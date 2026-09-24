// Appended to the instrumented application by run-pane-actions.py --tabs.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static bool enter(QWidget *widget, const QPoint &pos, const QMimeData &mime)
{
    QDragEnterEvent event(pos, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    return event.isAccepted();
}

static bool moveDrag(QWidget *widget, const QPoint &pos, const QMimeData &mime)
{
    QDragMoveEvent event(pos, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    return event.isAccepted();
}

static void leave(QWidget *widget)
{
    QDragLeaveEvent event;
    QApplication::sendEvent(widget, &event);
}

static TabState tabState(
    const QUrl &left,
    const QUrl &right,
    bool split,
    int viewMode)
{
    TabState state;
    state.currentUrl = left;
    state.history = {kThisPcUrl, left};
    state.historyIndex = 1;
    state.splitEnabled = split;
    state.splitUrl = right;
    state.splitViewMode = viewMode;
    state.splitSortKey = 2;
    state.splitSortAscending = false;
    return state;
}

static void testTabControllerState()
{
    const QUrl a = QUrl::fromLocalFile(QStringLiteral("/tmp/tab-state-a"));
    const QUrl b = QUrl::fromLocalFile(QStringLiteral("/tmp/tab-state-b"));
    const QUrl right = QUrl::fromLocalFile(QStringLiteral("/tmp/tab-state-right"));
    const QUrl search(QStringLiteral(
        "thispcsearch:/?query=needle&base=file%3A%2F%2F%2Ftmp%2Ftab-state-a&type=1"));

    TabController controller;
    verify(controller.create(a, 1, true) == 0, "controller creates first tab");
    verify(controller.create(b, 3, false) == 1, "controller appends second tab");
    verify(controller.tabs().at(1).splitSortKey == 3
               && !controller.tabs().at(1).splitSortAscending,
           "new tab keeps current view sort defaults");
    verify(controller.switchTo(0) && controller.activeIndex() == 0,
           "controller switches active tab");
    controller.syncActiveState(tabState(search, right, true, 2));
    verify(controller.tabs().at(0).currentUrl == search
               && controller.tabs().at(0).historyIndex == 1,
           "active tab sync preserves Search URL and history");
    verify(controller.tabs().at(0).splitEnabled
               && controller.tabs().at(0).splitUrl == right
               && controller.tabs().at(0).splitViewMode == 2,
           "active tab sync preserves asymmetric Split View and view mode");

    const int duplicate = controller.duplicate(0);
    verify(duplicate == 2 && controller.tabs().at(duplicate).currentUrl == search,
           "duplicate copies complete tab state");
    verify(controller.tabs().at(duplicate).splitUrl == right
               && controller.tabs().at(duplicate).splitSortKey == 2,
           "duplicate copies split location and settings");
    controller.switchTo(duplicate);
    const auto closed = controller.close(duplicate);
    verify(closed.accepted && closed.wasActive && closed.nextActive == 1,
           "closing active last tab chooses preceding tab");
    verify(controller.closedTabs().size() == 1 && controller.activeIndex() == -1,
           "close records history before UI restores next tab");
    verify(controller.reopenClosed() == 2
               && controller.tabs().at(2).currentUrl == search,
           "reopen restores the most recently closed tab");
    verify(controller.closedTabs().isEmpty(), "reopen consumes closed history entry");

    controller.switchTo(1);
    verify(controller.closeOthers(1) && controller.tabs().size() == 1,
           "close others retains exactly the selected tab");
    verify(controller.tabs().first().currentUrl == b
               && controller.closedTabs().size() == 2,
           "close others records every removed tab");
    verify(!controller.close(0).accepted && controller.tabs().size() == 1,
           "controller refuses to remove the final usable tab");

    for (int i = 0; i < 25; ++i) {
        controller.create(QUrl::fromLocalFile(QStringLiteral("/tmp/closed-%1").arg(i)), 0, true);
        controller.close(controller.tabs().size() - 1);
    }
    verify(controller.closedTabs().size() == TabController::ClosedTabLimit,
           "closed-tab history is capped at 20");
    verify(controller.closedTabs().first().currentUrl.toLocalFile().endsWith("closed-5")
               && controller.closedTabs().last().currentUrl.toLocalFile().endsWith("closed-24"),
           "closed-tab limit discards oldest entries only");

    TabController persisted;
    persisted.create(a, 0, true);
    persisted.switchTo(0);
    persisted.syncActiveState(tabState(search, right, true, 3));
    persisted.create(b, 1, false);
    persisted.switchTo(1);
    const SessionSnapshot saved = persisted.snapshot(true);
    verify(saved.tabs.size() == 2 && saved.activeTab == 1 && saved.splitPaneActive,
           "session snapshot preserves multiple tabs and active pane");
    SessionManager::save(saved);
    const SessionSnapshot loaded = SessionManager::load(0, true);
    verify(loaded.tabs.size() == 2 && loaded.activeTab == 1 && loaded.splitPaneActive,
           "QSettings restores tab count, active tab and active pane");
    verify(loaded.tabs.at(0).currentUrl == search
               && loaded.tabs.at(0).splitUrl == right
               && loaded.tabs.at(0).splitEnabled,
           "QSettings restores Search and asymmetric Split View locations");
    verify(loaded.tabs.at(0).splitViewMode == 3
               && loaded.tabs.at(0).splitSortKey == 2
               && !loaded.tabs.at(0).splitSortAscending,
           "QSettings restores per-tab view settings");
    verify(loaded.tabs.at(0).history.size() == 2
               && loaded.tabs.at(0).historyIndex == 1,
           "QSettings restores per-tab navigation history");
    TabController restored;
    restored.restore(loaded);
    verify(restored.tabs().size() == 2 && restored.activeIndex() == 1,
           "controller accepts restart session state");
    QSettings().clear();
}

static bool drop(QWidget *widget, const QPoint &pos, const QMimeData &mime)
{
    QDropEvent event(pos, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    return event.isAccepted();
}

static void testTabEvents()
{
    ExplorerTabBar bar;
    bar.resize(500, 40);
    bar.setExpanding(false);
    bar.setDropDirectoryResolver([](int index) {
        return QUrl::fromLocalFile(QStringLiteral("/tmp/tab-test-%1").arg(index));
    });
    auto reset = [&] {
        while (bar.count()) bar.removeTab(0);
        bar.addTab("A"); bar.addTab("B"); bar.addTab("C");
        bar.setCurrentIndex(0);
        bar.show(); QApplication::processEvents();
    };
    reset();
    QMimeData urls;
    urls.setUrls({QUrl::fromLocalFile("/tmp/tab-source")});
    QMimeData text; text.setText("not a file drag");
    verify(bar.acceptDrops(), "bar accepts drops");
    verify(!enter(&bar, bar.tabRect(1).center(), text), "non-URL MIME rejected");
    verify(!bar.m_hoverTimer.isActive(), "non-URL drag has no timer");
    verify(!enter(&bar, QPoint(490, 20), urls), "empty tab strip rejected");
    verify(enter(&bar, bar.tabRect(0).center(), urls), "current tab enter accepted");
    verify(!bar.m_hoverTimer.isActive(), "current tab does not start timer");
    leave(&bar);
    QSignalSpy changed(&bar, &QTabBar::currentChanged);
    verify(enter(&bar, bar.tabRect(1).center(), urls), "URL dragEnter accepted");
    verify(bar.m_hoverTimer.isSingleShot() && bar.m_hoverTimer.interval() == 650, "single-shot 650 ms");
    QTest::qWait(250);
    const int remaining = bar.m_hoverTimer.remainingTime();
    verify(moveDrag(&bar, bar.tabRect(1).center(), urls), "URL dragMove accepted");
    verify(bar.m_hoverTimer.remainingTime() <= remaining, "same target does not restart timer");
    QTest::qWait(470);
    verify(bar.currentIndex() == 1 && changed.count() == 1, "timer switches through currentChanged");
    verify(!bar.m_hoverTimer.isActive(), "timer stops after activation");
    leave(&bar); bar.setCurrentIndex(0);
    enter(&bar, bar.tabRect(1).center(), urls);
    QTest::qWait(250);
    moveDrag(&bar, bar.tabRect(2).center(), urls);
    verify(bar.m_hoverTab == 2 && bar.m_hoverTimer.remainingTime() > 500, "new target restarts timer");
    QTest::qWait(430);
    verify(bar.currentIndex() == 0, "old timer cannot activate previous target");
    QTest::qWait(300);
    verify(bar.currentIndex() == 2, "new target activates");
    leave(&bar); bar.setCurrentIndex(0);
    enter(&bar, bar.tabRect(1).center(), urls); leave(&bar);
    verify(!bar.m_hoverTimer.isActive(), "dragLeave cancels timer");
    QTest::qWait(720);
    verify(bar.currentIndex() == 0, "no activation after leave");
    enter(&bar, bar.tabRect(1).center(), urls);
    bar.removeTab(1);
    verify(!bar.m_hoverTimer.isActive(), "removal cancels timer");
    QTest::qWait(720);
    verify(bar.currentIndex() == 0, "removed index cannot activate replacement");
    reset(); enter(&bar, bar.tabRect(2).center(), urls);
    bar.moveTab(2, 1);
    verify(!bar.m_hoverTimer.isActive(), "reordering cancels timer");
    QTest::qWait(720);
    verify(bar.currentIndex() == 0, "reordering does not activate stale index");
    enter(&bar, bar.tabRect(1).center(), urls);
    bar.insertTab(0, "Inserted");
    verify(!bar.m_hoverTimer.isActive(), "insertion cancels timer");
    reset();
    bar.setTabEnabled(1, false);
    verify(!enter(&bar, bar.tabRect(1).center(), urls), "disabled tab rejected");
    bar.setTabEnabled(1, true);
    QSignalSpy dropped(&bar, &ExplorerTabBar::urlsDropped);
    enter(&bar, bar.tabRect(1).center(), urls);
    verify(drop(&bar, bar.tabRect(1).center(), urls), "direct Drop accepted");
    verify(dropped.count() == 1, "direct Drop emitted once");
    verify(dropped.first()[0].value<QList<QUrl>>() == urls.urls()
        && dropped.first()[1].toUrl() == QUrl::fromLocalFile("/tmp/tab-test-1"), "direct Drop sources and destination");
    verify(!bar.m_hoverTimer.isActive(), "Drop cancels hover");
    enter(&bar, bar.tabRect(1).center(), urls);
    bar.setDropDirectoryResolver([](int) { return QUrl(); });
    verify(!moveDrag(&bar, bar.tabRect(1).center(), urls), "target invalidated during drag is rejected");
    verify(!drop(&bar, bar.tabRect(1).center(), urls), "Drop revalidates target");
    verify(!bar.m_hoverTimer.isActive(), "invalid target cancels hover");
}

static bool sameTabState(const TabState &actual, const TabState &expected)
{
    return actual.currentUrl == expected.currentUrl
        && actual.history == expected.history
        && actual.historyIndex == expected.historyIndex
        && actual.splitEnabled == expected.splitEnabled
        && actual.splitUrl == expected.splitUrl
        && actual.splitViewMode == expected.splitViewMode
        && actual.splitSortKey == expected.splitSortKey
        && actual.splitSortAscending == expected.splitSortAscending;
}

static void testTabReorderStateIdentity()
{
    QTemporaryDir temp;
    QList<QUrl> urls;
    for (const auto *name : {"Pictures", "Music", "Downloads", "Documents"}) {
        const QUrl url = QUrl::fromLocalFile(temp.path() + QLatin1Char('/') + QString::fromLatin1(name));
        QDir().mkpath(url.toLocalFile());
        urls.push_back(url);
    }
    QList<TabState> originals;
    TabController controller;
    for (int i = 0; i < urls.size(); ++i) {
        controller.create(urls.at(i), i, i % 2 == 0);
        const QUrl split = QUrl::fromLocalFile(temp.path() + QStringLiteral("/split-%1").arg(i));
        TabState state = tabState(urls.at(i), split, i % 2 != 0, i);
        state.splitSortKey = i;
        state.splitSortAscending = i % 2 == 0;
        controller.tabs()[i] = state;
        originals.push_back(state);
    }
    controller.switchTo(0);
    verify(controller.move(3, 0), "controller accepts reorder 4 to 1");
    verify(controller.activeIndex() == 1,
           "moving inactive tab 4 to 1 shifts active tab index with the UI");
    verify(sameTabState(controller.tabs().at(0), originals.at(3))
               && sameTabState(controller.tabs().at(1), originals.at(0))
               && sameTabState(controller.tabs().at(2), originals.at(1))
               && sameTabState(controller.tabs().at(3), originals.at(2)),
           "reorder 4 to 1 preserves every complete TabState");
    verify(controller.move(0, 3), "controller accepts symmetric reorder 1 to 4");
    verify(controller.activeIndex() == 0,
           "symmetric inactive move shifts active index back with the UI");
    verify(sameTabState(controller.tabs().at(0), originals.at(0))
               && sameTabState(controller.tabs().at(1), originals.at(1))
               && sameTabState(controller.tabs().at(2), originals.at(2))
               && sameTabState(controller.tabs().at(3), originals.at(3)),
           "reorder 1 to 4 restores original order without state inheritance");
    verify(controller.move(0, 3) && controller.move(3, 2) && controller.move(2, 0),
           "controller accepts several consecutive reorders");
    verify(controller.activeIndex() == 0
               && sameTabState(controller.tabs().at(0), originals.at(0))
               && sameTabState(controller.tabs().at(1), originals.at(1))
               && sameTabState(controller.tabs().at(2), originals.at(2))
               && sameTabState(controller.tabs().at(3), originals.at(3)),
           "consecutive active-tab reorders neither duplicate nor replace state");

    ThisPcWindow window(urls.at(0));
    for (int i = 1; i < urls.size(); ++i) window.createNewTab(urls.at(i), true);
    window.switchToTab(0);
    QStringList signalOrder;
    QObject::connect(window.m_tabBar, &QTabBar::tabMoved, &window,
                     [&](int, int) { signalOrder << QStringLiteral("tabMoved"); });
    QObject::connect(window.m_tabBar, &QTabBar::currentChanged, &window,
                     [&](int) { signalOrder << QStringLiteral("currentChanged"); });
    window.m_tabBar->moveTab(3, 0);
    verify(signalOrder == QStringList({QStringLiteral("tabMoved"), QStringLiteral("currentChanged")}),
           "Qt deterministically emits tabMoved before currentChanged for reorder");
    verify(window.m_activeTab == 1 && window.m_tabBar->currentIndex() == 1
               && window.m_currentUrl == urls.at(0),
           "window and QTabBar retain the same active tab after inactive reorder");
    verify(window.m_tabs.at(0).currentUrl == urls.at(3)
               && window.m_tabs.at(1).currentUrl == urls.at(0)
               && window.m_tabs.at(2).currentUrl == urls.at(1)
               && window.m_tabs.at(3).currentUrl == urls.at(2),
           "tabMoved integration reorders the model exactly once without copying active state");
}

static void chooseCopy(ThisPcWindow &window)
{
    QTimer::singleShot(0, &window, [] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        verify(menu != nullptr, "existing copy/move menu opened");
        QAction *copy = nullptr;
        for (auto *action : menu->actions()) if (action->text() == "Copy here") copy = action;
        verify(copy != nullptr, "existing Copy here action");
        menu->setActiveAction(copy);
        QTest::keyClick(menu, Qt::Key_Return);
    });
}

static void testWindow(bool split)
{
    QTemporaryDir temp;
    const QUrl a = QUrl::fromLocalFile(temp.path() + "/A");
    const QUrl b = QUrl::fromLocalFile(temp.path() + "/B");
    const QUrl child = childUrlWithName(b, "child");
    const QUrl right = QUrl::fromLocalFile(temp.path() + "/right");
    for (const auto &url : {a, b, child, right}) QDir().mkpath(url.toLocalFile());
    const QUrl source = childUrlWithName(a, "source.txt");
    QFile file(source.toLocalFile()); verify(file.open(QIODevice::WriteOnly), "temporary source created"); file.close();

    verify(dropActionForUrls({source}, b, Qt::NoModifier) == Qt::MoveAction,
           "same local storage defaults to Move");
    verify(dropActionForUrls({source}, b, Qt::ControlModifier) == Qt::CopyAction,
           "Ctrl forces Copy");
    verify(dropActionForUrls({source}, b, Qt::ShiftModifier) == Qt::MoveAction,
           "Shift forces Move");
    if (QDir(QStringLiteral("/dev/shm")).exists()) {
        QTemporaryDir otherStorage(QStringLiteral("/dev/shm/thispc-drop-policy-XXXXXX"));
        if (otherStorage.isValid()
            && !directory_view_detail::localPathsShareStorage(source.toLocalFile(), otherStorage.path())) {
            verify(
                dropActionForUrls(
                    {source},
                    QUrl::fromLocalFile(otherStorage.path()),
                    Qt::NoModifier) == Qt::CopyAction,
                "different local storage defaults to Copy");
        } else {
            qInfo("SKIP: /dev/shm does not expose a distinct writable storage");
        }
    } else {
        qInfo("SKIP: /dev/shm unavailable for distinct-storage test");
    }
    verify(!dropWouldCreateCycle({source}, b),
           "ordinary file Drop is not a directory cycle");
    verify(dropWouldCreateCycle({a}, a),
           "folder cannot be dropped onto itself");
    verify(dropWouldCreateCycle({b}, child),
           "folder cannot be dropped into its direct child");
    const QUrl deepChild = childUrlWithName(child, "deep");
    QDir().mkpath(deepChild.toLocalFile());
    verify(dropWouldCreateCycle({b}, deepChild),
           "folder cannot be dropped into a deep descendant");

    ThisPcWindow window(a);
    window.show(); window.activateWindow();
    if (split) {
        window.setSplitViewEnabled(true);
        window.m_splitPane->setCurrentUrl(right);
    }
    window.createNewTab(b, true);
    if (split) {
        window.setSplitViewEnabled(true);
        window.m_splitPane->setCurrentUrl(right);
        window.m_splitPane->focusView();
    }
    window.switchToTab(0);
    QTest::qWait(200);
    auto *bar = window.m_tabBar;
    QMimeData mime; mime.setUrls({source});
    for (int mode : {0, 1, 2, 3}) {
        window.setDirectoryViewMode(mode);
        const QPoint target = bar->tabRect(1).center();
        verify(enter(bar, target, mime), "A to B dragEnter");
        verify(moveDrag(bar, target, mime), "A to B dragMove");
        QTest::qWait(730);
        verify(window.m_activeTab == 1 && window.m_currentUrl == b, "hover loads B through switchToTab");
        verify(window.m_splitPane->isVisible() == split, "hover preserves split state of B");
        leave(bar);
        QAbstractItemView *view = mode == 2 ? static_cast<QAbstractItemView *>(window.m_directoryDetails) : window.m_directoryList;
        verify(view->viewport()->acceptDrops(), "B viewport still accepts Drop");
        const QPoint background(view->viewport()->width()-8, view->viewport()->height()-8);
        verify(enter(view->viewport(), background, mime), "enter B background");
        verify(moveDrag(view->viewport(), background, mime), "move over B background");
        chooseCopy(window); dispatch = {};
        verify(drop(view->viewport(), background, mime), "Drop on B background accepted");
        verify(dispatch.kind == "copy" && dispatch.sources == QList<QUrl>{source}
            && dispatch.destination == b, "background Drop uses B and existing handler");
        QTest::qWait(150);
        QPoint folderPoint;
        if (mode == 2) {
            verify(window.m_directoryDetails->topLevelItemCount() == 1, "B child loaded in details");
            folderPoint = window.m_directoryDetails->visualItemRect(window.m_directoryDetails->topLevelItem(0)).center();
        } else {
            verify(window.m_directoryList->count() == 1, "B child loaded in list/icons");
            folderPoint = window.m_directoryList->visualItemRect(window.m_directoryList->item(0)).center();
        }
        verify(enter(view->viewport(), folderPoint, mime), "enter folder after tab hover");
        moveDrag(view->viewport(), folderPoint, mime);
        chooseCopy(window); dispatch = {};
        verify(drop(view->viewport(), folderPoint, mime), "folder Drop accepted");
        verify(dispatch.destination == child, "folder Drop destination after hover");
        window.switchToTab(0);
        QApplication::processEvents();
    }
    if (split) window.m_splitPane->focusView();
    const QPoint target = bar->tabRect(1).center();
    enter(bar, target, mime);
    chooseCopy(window); dispatch = {};
    verify(drop(bar, target, mime), "direct Drop through existing handler");
    verify(dispatch.destination == b && dispatch.sources == QList<QUrl>{source}, "direct tab Drop targets tab primary directory regardless of pane focus");

    dispatch = {};
    window.handleDroppedUrls({source}, b, QPoint(1, 1), Qt::ControlModifier);
    verify(dispatch.kind == "copy" && dispatch.destination == b,
           "Ctrl Drop bypasses menu and dispatches Copy");
    dispatch = {};
    window.handleDroppedUrls({source}, b, QPoint(1, 1), Qt::ShiftModifier);
    verify(dispatch.kind == "move" && dispatch.destination == b,
           "Shift Drop bypasses menu and dispatches Move");

    dispatch = {};
    QTimer::singleShot(0, &window, [] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        verify(box != nullptr, "cycle guard shows a warning");
        box->accept();
    });
    window.handleDroppedUrls({b}, child, QPoint(1, 1), Qt::ShiftModifier);
    verify(dispatch.kind.isEmpty(),
           "cycle guard blocks KIO dispatch before Move");

    for (const auto &special : {"thispc:/", "trash:/", "remote:/", "thispcsearch:/", "filenamesearch:/", "https://example.invalid/"}) {
        window.m_tabs[1].currentUrl = QUrl(QString::fromLatin1(special));
        verify(!window.tabDropDirectory(1).isValid(), "special destination validation");
        verify(!enter(bar, target, mime), "special tab does not accept enter");
    }
    window.m_tabs[1].currentUrl = b;
    enter(bar, target, mime);
    window.m_tabs[1].currentUrl = QUrl("trash:/");
    dispatch = {};
    verify(!drop(bar, target, mime) && dispatch.kind.isEmpty(), "special destination rejected before direct Drop dispatch");
    verify(QFile::exists(source.toLocalFile()), "test source was not moved or deleted");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-tab-test");
    QCoreApplication::setApplicationName("tab-test");
    QSettings().clear();
    testTabControllerState();
    testTabEvents();
    testTabReorderStateIdentity();
    testWindow(false);
    testWindow(true);
    qInfo("PASS: %d tab DnD assertions; real Qt events/timers/window, KIO transfer dispatch intercepted", checks);
}
