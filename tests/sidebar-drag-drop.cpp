// Appended to the instrumented application by run-pane-actions.py --sidebar-dnd.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static bool enter(QWidget *widget, const QMimeData &mime,
                  Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QDragEnterEvent event(widget->rect().center(),
                          Qt::CopyAction | Qt::MoveAction, &mime,
                          Qt::LeftButton, modifiers);
    QApplication::sendEvent(widget, &event);
    return event.isAccepted();
}

static bool moveDrag(QWidget *widget, const QMimeData &mime,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QDragMoveEvent event(widget->rect().center(),
                         Qt::CopyAction | Qt::MoveAction, &mime,
                         Qt::LeftButton, modifiers);
    QApplication::sendEvent(widget, &event);
    return event.isAccepted();
}

static bool drop(QWidget *widget, const QMimeData &mime,
                 Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QDropEvent event(QPointF(widget->rect().center().x(), widget->height() - 1),
                     Qt::CopyAction | Qt::MoveAction, &mime,
                     Qt::LeftButton, modifiers);
    QApplication::sendEvent(widget, &event);
    return event.isAccepted();
}

static void testTargetPolicyAndFeedback()
{
    QTemporaryDir temp;
    const QUrl destination = QUrl::fromLocalFile(temp.path() + "/target");
    const QUrl source = QUrl::fromLocalFile(temp.path() + "/source.txt");
    verify(QDir().mkpath(destination.toLocalFile()), "target directory created");
    QFile file(source.toLocalFile());
    verify(file.open(QIODevice::WriteOnly), "source file created");
    file.close();

    SidebarPanel panel;
    SidebarButton target("Target", "folder", destination, &panel);
    target.resize(190, 31);
    panel.registerTransferDropTarget(&target, destination);
    int transferCount = 0;
    QList<QUrl> droppedUrls;
    QUrl droppedDestination;
    Qt::KeyboardModifiers droppedModifiers;
    QObject::connect(&panel, &SidebarPanel::urlsDropped, &panel,
                     [&](const QList<QUrl> &items, const QUrl &where,
                         const QPoint &, Qt::KeyboardModifiers modifiers) {
        ++transferCount;
        droppedUrls = items;
        droppedDestination = where;
        droppedModifiers = modifiers;
    });

    QMimeData urls;
    urls.setUrls({source});
    const QSize size = target.size();
    verify(enter(&target, urls), "real directory accepts URL drag");
    verify(target.property("dropActive").toBool(), "valid target gets hover feedback");
    verify(target.size() == size, "hover feedback does not change geometry");
    verify(moveDrag(&target, urls, Qt::ControlModifier), "drag move remains accepted");
    verify(drop(&target, urls, Qt::ControlModifier), "valid Drop accepted");
    verify(!target.property("dropActive").toBool(), "Drop clears hover feedback");
    verify(transferCount == 1, "Drop forwarded exactly once");
    verify(droppedUrls == urls.urls()
               && droppedDestination == destination
               && droppedModifiers.testFlag(Qt::ControlModifier),
           "Drop forwards source destination and modifiers");

    enter(&target, urls);
    QDragLeaveEvent leave;
    QApplication::sendEvent(&target, &leave);
    verify(!target.property("dropActive").toBool(), "drag leave clears hover feedback");

    QMimeData text;
    text.setText("not URLs");
    verify(!enter(&target, text), "non-URL MIME is prohibited");
    verify(!target.property("dropActive").toBool(), "invalid drag has no feedback");

    SidebarButton missing("Missing", "folder",
                          QUrl::fromLocalFile(temp.path() + "/missing"), &panel);
    panel.registerTransferDropTarget(&missing, missing.url());
    verify(!enter(&missing, urls), "missing local directory is prohibited");
    for (const char *special : {"thispc:/", "trash:/", "remote:/",
                                "thispcsearch:/", "filenamesearch:/"}) {
        SidebarButton virtualTarget("Virtual", "folder", QUrl(special), &panel);
        panel.registerTransferDropTarget(&virtualTarget, virtualTarget.url());
        verify(!enter(&virtualTarget, urls), "virtual destination is prohibited");
    }

    CollapsibleSection section("Places", "dnd-test", &panel);
    verify(!section.m_header->acceptDrops() && !enter(section.m_header, urls),
           "section header is not a Drop target");
}

static void testConcreteSidebarTargetsAndQuickAccessPriority()
{
    QTemporaryDir temp;
    const QUrl first = QUrl::fromLocalFile(temp.path() + "/first");
    const QUrl second = QUrl::fromLocalFile(temp.path() + "/second");
    const QUrl source = QUrl::fromLocalFile(temp.path() + "/source.txt");
    QDir().mkpath(first.toLocalFile());
    QDir().mkpath(second.toLocalFile());
    QFile file(source.toLocalFile());
    verify(file.open(QIODevice::WriteOnly), "Quick Access source created");
    file.close();

    SidebarPanel panel;
    panel.m_quickAccessUrls = {first, second};
    panel.rebuildQuickAccess();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    verify(panel.m_quickAccessButtons.size() == 2, "Quick Access entries rebuilt");
    auto *target = panel.m_quickAccessButtons.at(1);
    int transferCount = 0;
    QObject::connect(&panel, &SidebarPanel::urlsDropped, &panel,
                     [&](const QList<QUrl> &, const QUrl &, const QPoint &,
                         Qt::KeyboardModifiers) { ++transferCount; });

    QMimeData both;
    both.setUrls({source});
    both.setData("application/x-thispc-quick-access",
                 first.toString(QUrl::FullyEncoded).toUtf8());
    verify(enter(target, both), "Quick Access reorder MIME accepts drag");
    verify(drop(target, both), "Quick Access reorder Drop accepted");
    verify(transferCount == 0, "reorder MIME never becomes a transfer");
    verify(panel.m_quickAccessUrls == QList<QUrl>({second, first}),
           "existing Quick Access reorder handles mixed MIME");

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    target = panel.m_quickAccessButtons.first();
    QMimeData urls; urls.setUrls({source});
    verify(enter(target, urls) && drop(target, urls, Qt::ShiftModifier),
           "Quick Access folder accepts file transfer");
    verify(transferCount == 1, "Quick Access transfer forwarded once");

    panel.m_recentLocationUrls = {first};
    panel.rebuildRecentLocations();
    auto recentButtons = panel.m_recentLocationsLayout->parentWidget()
                             ->findChildren<SidebarButton *>(QString(), Qt::FindDirectChildrenOnly);
    verify(!recentButtons.isEmpty() && !recentButtons.first()->acceptDrops(),
           "Recent entry is not a Drop target");

    DriveInfo drive;
    drive.name = "Test drive";
    drive.mountPoint = first.toLocalFile();
    drive.targetUrl = first;
    panel.setDrives({drive});
    verify(panel.m_driveSidebarButtons.size() == 1
               && panel.m_driveSidebarButtons.first()->acceptDrops(),
           "device directory is a Drop target");
    verify(enter(panel.m_driveSidebarButtons.first(), urls)
               && drop(panel.m_driveSidebarButtons.first(), urls, Qt::ControlModifier),
           "device Drop accepted");
    verify(transferCount == 2, "device Drop forwarded once");

    bool homeRegistered = false;
    for (SidebarButton *button : std::as_const(panel.m_staticSidebarButtons)) {
        if (sameLocation(button->url(), QUrl::fromLocalFile(QDir::homePath()))) {
            homeRegistered = panel.m_transferDropTargets.contains(button);
        }
        if (sameLocation(button->url(), kThisPcUrl)
            || button->url().scheme() == QStringLiteral("trash")
            || button->url().scheme() == QStringLiteral("remote")) {
            verify(!panel.m_transferDropTargets.contains(button),
                   "virtual static place is not registered");
        }
    }
    verify(homeRegistered, "standard Home place is registered");
}

static void testWindowRoutingPreservesActivePane()
{
    QTemporaryDir temp;
    const QUrl left = QUrl::fromLocalFile(temp.path() + "/left");
    const QUrl right = QUrl::fromLocalFile(temp.path() + "/right");
    const QUrl destination = QUrl::fromLocalFile(temp.path() + "/destination");
    const QUrl source = QUrl::fromLocalFile(temp.path() + "/source.txt");
    for (const QUrl &url : {left, right, destination}) QDir().mkpath(url.toLocalFile());
    QFile file(source.toLocalFile());
    verify(file.open(QIODevice::WriteOnly), "window routing source created");
    file.close();

    ThisPcWindow window;
    window.navigatePane(ThisPcWindow::PaneId::Primary, left);
    window.setSplitViewEnabled(true);
    window.navigatePane(ThisPcWindow::PaneId::Split, right);
    window.show();
    QApplication::processEvents();
    window.setActivePane(ThisPcWindow::PaneId::Split);
    SidebarButton target("Target", "folder", destination, window.m_sidebar);
    target.resize(190, 31);
    window.m_sidebar->registerTransferDropTarget(&target, destination);
    QMimeData urls; urls.setUrls({source});

    dispatch = {};
    verify(enter(&target, urls, Qt::ControlModifier)
               && drop(&target, urls, Qt::ControlModifier),
           "sidebar Drop reaches window");
    verify(dispatch.kind == "copy" && dispatch.sources == urls.urls()
               && dispatch.destination == destination,
           "sidebar uses existing FileActions transfer routing");
    verify(window.m_activePane == ThisPcWindow::PaneId::Split,
           "drag and Drop preserve right active pane");

    window.setActivePane(ThisPcWindow::PaneId::Primary);
    dispatch = {};
    verify(enter(&target, urls, Qt::ShiftModifier)
               && drop(&target, urls, Qt::ShiftModifier),
           "second sidebar Drop reaches window");
    verify(dispatch.kind == "move", "Shift uses existing Move routing");
    verify(window.m_activePane == ThisPcWindow::PaneId::Primary,
           "drag and Drop preserve left active pane");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-sidebar-dnd-test");
    QCoreApplication::setApplicationName("sidebar-dnd-test");
    testTargetPolicyAndFeedback();
    testConcreteSidebarTargetsAndQuickAccessPriority();
    testWindowRoutingPreservesActivePane();
    qInfo("PASS: %d sidebar DnD assertions; real Qt events, routing intercepted", checks);
}
