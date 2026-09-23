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
    testTabEvents();
    testWindow(false);
    testWindow(true);
    qInfo("PASS: %d tab DnD assertions; real Qt events/timers/window, KIO transfer dispatch intercepted", checks);
}
