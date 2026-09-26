// Appended to the instrumented application by run-pane-actions.py --sidebar-layout.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static void settle()
{
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QApplication::processEvents();
}

static QImage rendered(QWidget *widget)
{
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget->render(&image);
    return image;
}

static void verifyButtonElision(SidebarButton *button, bool expectElision)
{
    QStyleOptionButton option;
    option.initFrom(button);
    option.features = QStyleOptionButton::Flat;
    const int available = button->style()->subElementRect(
        QStyle::SE_PushButtonContents, &option, button).width()
        - (button->icon().isNull() ? 0 : button->iconSize().width() + 4);
    const QString expected = button->fontMetrics().elidedText(
        button->text(), Qt::ElideRight, available, Qt::TextShowMnemonic);
    verify(expectElision ? expected.endsWith(QChar(0x2026)) && expected != button->text()
                         : expected == button->text(),
           "label elides only when it exceeds the available text area");

    // Compare actual pixels to a native button with the expected visible text.
    // Checking text() or font metrics alone would also pass for clipped text.
    QPushButton reference(button->icon(), expected, button->parentWidget());
    reference.setObjectName(button->objectName());
    reference.setFlat(button->isFlat());
    reference.setFont(button->font());
    reference.setPalette(button->palette());
    reference.setIconSize(button->iconSize());
    reference.setLayoutDirection(button->layoutDirection());
    reference.setProperty("current", button->property("current"));
    reference.setProperty("dropActive", button->property("dropActive"));
    reference.setDown(button->isDown());
    reference.resize(button->size());
    reference.ensurePolished();
    verify(rendered(button) == rendered(&reference),
           "painted sidebar button contains the visible ellipsis and native styling");
    verify(button->toolTip() == button->text(),
           "tooltip retains the complete sidebar label");
}

static void testScrollableSidebar()
{
    ThisPcWindow window(kThisPcUrl, false);
    window.resize(850, 430);
    window.show();
    settle();

    verify(window.m_sidebarScrollArea != nullptr,
           "sidebar has its own scroll area");
    verify(window.m_sidebarScrollArea->horizontalScrollBarPolicy()
               == Qt::ScrollBarAlwaysOff,
           "horizontal scrollbar is always disabled");
    verify(window.m_sidebarScrollArea->verticalScrollBarPolicy()
               == Qt::ScrollBarAsNeeded,
           "vertical scrollbar appears as needed");
    verify(window.m_sidebarScrollArea->minimumWidth() == 205
               && window.m_sidebarScrollArea->maximumWidth() == 480,
           "sidebar enforces approved width limits");

    const QSize originalWindowSize = window.size();
    const auto sections = window.m_sidebar->findChildren<CollapsibleSection *>();
    for (CollapsibleSection *section : sections) {
        section->m_header->setChecked(true);
    }
    settle();
    verify(window.size() == originalWindowSize,
           "expanding sections does not resize the main window");
    verify(window.m_sidebarScrollArea->verticalScrollBar()->maximum() > 0,
           "expanded sidebar gets a vertical scroll range");
}

static void testLongLabels()
{
    QSettings settings;
    settings.clear();
    settings.setValue("sidebar/width", 205);
    QTemporaryDir files;
    verify(files.isValid(), "temporary sidebar destinations");
    const QString fullName = QStringLiteral("Zażółć — ") + QString(90, QLatin1Char('W'));
    const QUrl url = QUrl::fromLocalFile(files.filePath(fullName));
    verify(QDir().mkpath(url.toLocalFile()), "long Quick Access destination exists");

    ThisPcWindow window(kThisPcUrl, false);
    window.resize(1100, 430);
    window.show();
    settle();
    const int selectedWidth = window.m_sidebarScrollArea->width();
    const QSize windowSize = window.size();
    window.m_sidebar->pinQuickAccessLocation(url);
    window.m_sidebar->recordRecentLocation(url);
    DriveInfo drive;
    drive.name = fullName;
    drive.targetUrl = url;
    drive.mountPoint = url.toLocalFile();
    window.m_sidebar->setDrives({drive});
    settle();

    auto *quick = window.m_sidebar->m_quickAccessButtons.first();
    auto *recent = qobject_cast<SidebarButton *>(
        window.m_sidebar->m_recentLocationsLayout->itemAt(0)->widget());
    verify(quick->text() == fullName && recent && recent->text() == fullName,
           "rebuilt Quick Access and Recent entries retain the complete label");
    verifyButtonElision(quick, true);
    verifyButtonElision(recent, true);
    verify(window.size() == windowSize
               && window.m_sidebarScrollArea->width() == selectedWidth
               && window.m_sidebar->width() == window.m_sidebarScrollArea->viewport()->width()
               && quick->width() <= window.m_sidebar->width(),
           "long labels fit the viewport without changing the selected sidebar or window width");
    verify(!window.m_sidebarScrollArea->horizontalScrollBar()->isVisible()
               && window.m_sidebarScrollArea->horizontalScrollBar()->maximum() == 0,
           "long labels produce neither a horizontal scrollbar nor hidden horizontal overflow");
    verify(settings.value("sidebar/width").toInt() == 205,
           "rebuilding long labels leaves persisted sidebar width unchanged");

    auto *driveButton = window.m_sidebar->m_driveSidebarButtons.first();
    auto *name = driveButton->findChild<QLabel *>("sidebarDriveName");
    verify(name && name->text() == fullName && driveButton->toolTip().contains(fullName)
               && driveButton->toolTip().contains(drive.mountPoint),
           "drive label and existing detailed tooltip retain the full name and mount point");
    const QString visibleDriveName = name->fontMetrics().elidedText(
        fullName, Qt::ElideRight, name->contentsRect().width());
    verify(visibleDriveName.endsWith(QChar(0x2026)), "long drive name needs an ellipsis");
    QLabel referenceName(visibleDriveName);
    referenceName.setTextFormat(Qt::PlainText);
    referenceName.setFont(name->font());
    referenceName.setPalette(name->palette());
    referenceName.resize(name->size());
    verify(rendered(name) == rendered(&referenceName),
           "drive name paints an ellipsis inside its available width");

    quick->setCurrent(true);
    verifyButtonElision(quick, true);
    quick->setProperty("dropActive", true);
    quick->setDown(true);
    verifyButtonElision(quick, true);
    quick->setDown(false);

    const QUrl mediumUrl = QUrl::fromLocalFile(
        files.filePath(QStringLiteral("Folder with a moderately long name for resize checks")));
    window.m_sidebar->pinQuickAccessLocation(mediumUrl);
    settle();
    auto mediumButton = [&] {
        for (QuickAccessSidebarButton *button :
             std::as_const(window.m_sidebar->m_quickAccessButtons)) {
            if (sameLocation(button->url(), mediumUrl)) return button;
        }
        return static_cast<QuickAccessSidebarButton *>(nullptr);
    };
    verify(mediumButton() != nullptr, "medium Quick Access entry exists");
    verifyButtonElision(mediumButton(), true);
    window.m_sidebarSplitter->setSizes({480, 615});
    window.m_sidebarSplitter->splitterMoved(480, 1);
    settle();
    verifyButtonElision(mediumButton(), false);
    verify(settings.value("sidebar/width").toInt() == 480,
           "divider remains resizable and persists its new width with long labels present");
    window.resize(500, 430);
    settle();
    verifyButtonElision(mediumButton(), true);
    verify(settings.value("sidebar/width").toInt() == 480
               && window.m_sidebarScrollArea->horizontalScrollBar()->maximum() == 0,
           "window compression re-elides labels without overwriting the width preference");

    QSignalSpy activated(window.m_sidebar, &SidebarPanel::activated);
    window.m_sidebar->m_quickAccessButtons.first()->click();
    verify(activated.count() == 1 && activated.first().first().toUrl() == url,
           "elided Quick Access button still activates the complete destination URL");
}

static void testResizePersistenceAndRebuildScroll()
{
    QSettings settings;
    settings.clear();
    settings.setValue("sidebar/width", 360);

    ThisPcWindow window(kThisPcUrl, false);
    window.resize(1000, 430);
    window.show();
    settle();
    verify(qAbs(window.m_sidebarScrollArea->width() - 360) <= 2,
           "preferred sidebar width is restored");

    window.m_sidebarSplitter->setSizes({420, 575});
    window.m_sidebarSplitter->splitterMoved(420, 1);
    settle();
    verify(settings.value("sidebar/width").toInt() == 420,
           "user divider movement saves preferred width");

    window.resize(500, 430);
    settle();
    verify(settings.value("sidebar/width").toInt() == 420,
           "window compression does not overwrite preferred width");

    window.resize(1000, 430);
    settle();
    QScrollBar *bar = window.m_sidebarScrollArea->verticalScrollBar();
    bar->setValue(bar->maximum());
    const int previous = bar->value();
    QList<DriveInfo> drives;
    for (int i = 0; i < 8; ++i) {
        DriveInfo drive;
        drive.name = QStringLiteral("Drive %1").arg(i);
        drive.targetUrl = QUrl::fromLocalFile(QStringLiteral("/tmp"));
        drives.push_back(drive);
    }
    window.m_sidebar->setDrives(drives);
    settle();
    verify(bar->value() == std::clamp(previous, bar->minimum(), bar->maximum()),
           "sidebar rebuild preserves a still-valid scroll position");
}

static DriveInfo menuTestDrive(const QString &name, const QString &fileSystem,
                               const QString &mountPoint)
{
    DriveInfo drive;
    drive.name = name;
    drive.fileSystem = fileSystem;
    drive.mountPoint = mountPoint;
    drive.targetUrl = QUrl::fromLocalFile(mountPoint);
    return drive;
}

static void testDriveMenuSurvivesSnapshotReplacement(const QString &fileSystem)
{
    SidebarPanel panel;
    const DriveInfo original = menuTestDrive(
        QStringLiteral("Original %1 drive").arg(fileSystem), fileSystem,
        QStringLiteral("/tmp/original-%1").arg(fileSystem.toLower()));
    const DriveInfo replacement = menuTestDrive(
        QStringLiteral("Replacement drive"), QStringLiteral("ext4"),
        QStringLiteral("/tmp/replacement"));
    panel.setDrives({original});
    auto *button = panel.m_driveSidebarButtons.first();
    QPointer<SidebarDriveButton> originalButton(button);
    QSignalSpy activated(&panel, &SidebarPanel::activated);

    QTimer::singleShot(0, &panel, [&] {
        verify(panel.m_activeDriveContextMenus == 1,
               "drive menu reports an active lifecycle");
        panel.setDrives({replacement});
        verify(panel.m_driveSidebarButtons.first() == button
                   && panel.m_driveRebuildPending,
               "drive snapshot replacement is deferred while menu is open");
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        verify(menu && !menu->actions().isEmpty(),
               "real drive context menu is open");
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                          menu->actionGeometry(menu->actions().first()).center());
    });

    QContextMenuEvent event(QContextMenuEvent::Mouse, button->rect().center(),
                            button->mapToGlobal(button->rect().center()));
    QApplication::sendEvent(button, &event);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();

    verify(event.isAccepted(), "drive context menu event is accepted");
    verify(activated.count() == 1
               && activated.first().first().toUrl() == original.targetUrl,
           "menu action uses the original DriveInfo value snapshot");
    verify(originalButton.isNull(),
           "old drive button is destroyed only after the menu closes");
    verify(panel.m_activeDriveContextMenus == 0 && !panel.m_driveRebuildPending,
           "menu close drains the deferred rebuild without stale callbacks");
    verify(panel.m_driveSidebarButtons.size() == 1
               && panel.m_driveSidebarButtons.first()->url() == replacement.targetUrl,
           "replacement drive snapshot is published after menu lifetime");
}

static void testRepeatedDriveMenuOpenClose()
{
    SidebarPanel panel;
    panel.setDrives({menuTestDrive(QStringLiteral("Local drive"),
                                   QStringLiteral("ext4"),
                                   QStringLiteral("/tmp/local-drive"))});
    for (int iteration = 0; iteration < 4; ++iteration) {
        auto *button = panel.m_driveSidebarButtons.first();
        QTimer::singleShot(0, &panel, [] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                menu->close();
            }
        });
        QContextMenuEvent event(QContextMenuEvent::Mouse, button->rect().center(),
                                button->mapToGlobal(button->rect().center()));
        QApplication::sendEvent(button, &event);
        verify(panel.m_activeDriveContextMenus == 0,
               "repeated menu close releases lifecycle guard");
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-sidebar-layout-test");
    QCoreApplication::setApplicationName("sidebar-layout-test");
    app.setStyleSheet(applicationStyleSheet());
    if (qEnvironmentVariableIsSet("THISPC_SIDEBAR_MENU_ONLY")) {
        testDriveMenuSurvivesSnapshotReplacement(QStringLiteral("ntfs"));
        testDriveMenuSurvivesSnapshotReplacement(QStringLiteral("ext4"));
        testRepeatedDriveMenuOpenClose();
        qInfo("PASS: %d sidebar drive menu assertions", checks);
        return 0;
    }
    testScrollableSidebar();
    testLongLabels();
    testResizePersistenceAndRebuildScroll();
    testDriveMenuSurvivesSnapshotReplacement(QStringLiteral("ntfs"));
    testDriveMenuSurvivesSnapshotReplacement(QStringLiteral("ext4"));
    testRepeatedDriveMenuOpenClose();
    qInfo("PASS: %d sidebar layout assertions", checks);
}
