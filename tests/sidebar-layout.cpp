/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

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

static int expectedButtonWidthInParent(const QWidget *button)
{
    if (!button || !button->parentWidget() || !button->parentWidget()->layout()) {
        return -1;
    }
    const QMargins margins = button->parentWidget()->layout()->contentsMargins();
    return button->parentWidget()->contentsRect().width() - margins.left() - margins.right();
}

template <typename Lookup, typename WidthPred>
static SidebarButton *waitForButtonLaidOut(Lookup lookup, WidthPred widthPred, const char *description)
{
    verify(QTest::qWaitFor([&] {
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        SidebarButton *btn = lookup();
        if (!btn || !btn->isVisible()) {
            return false;
        }
        const int targetWidth = expectedButtonWidthInParent(btn);
        return targetWidth > 0 && btn->width() == targetWidth && widthPred(btn->width());
    }, 2000), description);
    SidebarButton *btn = lookup();
    verify(btn != nullptr && btn->isVisible(), description);
    return btn;
}

static void verifyButtonElision(SidebarButton *button, bool expectElision)
{
    verify(button != nullptr && button->isVisible(),
           "sidebar button is visible before elision and pixel verification");
    QStyleOptionButton option;
    option.initFrom(button);
    option.features = QStyleOptionButton::Flat;
    option.icon = button->icon();
    option.iconSize = button->iconSize();
    const int available = button->style()->subElementRect(
        QStyle::SE_PushButtonContents, &option, button).width()
        - (option.icon.isNull() ? 0 : option.icon.actualSize(option.iconSize).width() + 4);
    const QString expected = option.fontMetrics.elidedText(
        button->text(), Qt::ElideRight, std::max(0, available), Qt::TextShowMnemonic);
    verify(expectElision ? expected.endsWith(QChar(0x2026)) && expected != button->text()
                         : expected == button->text(),
           "label elides only when it exceeds the available text area");

    // Compare actual pixels to a native button with the expected visible text.
    // Checking text() or font metrics alone would also pass for clipped text.
    QPushButton reference(button->icon(), expected, button->parentWidget());
    reference.setObjectName(button->objectName());
    reference.setFlat(button->isFlat());
    reference.setAutoDefault(button->autoDefault());
    reference.setDefault(button->isDefault());
    reference.setFont(button->font());
    reference.setPalette(button->palette());
    reference.setStyleSheet(button->styleSheet());
    reference.setIconSize(button->iconSize());
    reference.setLayoutDirection(button->layoutDirection());
    reference.setFocusPolicy(button->focusPolicy());
    reference.setProperty("current", button->property("current"));
    reference.setProperty("dropActive", button->property("dropActive"));
    reference.setDown(button->isDown());
    reference.setGeometry(button->geometry());
    reference.setAttribute(Qt::WA_DontShowOnScreen, true);
    reference.setVisible(button->isVisible());
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

    auto quickButton = [&]() -> QuickAccessSidebarButton * {
        for (QuickAccessSidebarButton *button :
             std::as_const(window.m_sidebar->m_quickAccessButtons)) {
            if (sameLocation(button->url(), url)) return button;
        }
        return nullptr;
    };
    auto recentButton = [&]() -> SidebarButton * {
        if (!window.m_sidebar->m_recentLocationsLayout
            || window.m_sidebar->m_recentLocationsLayout->count() == 0) {
            return nullptr;
        }
        return qobject_cast<SidebarButton *>(
            window.m_sidebar->m_recentLocationsLayout->itemAt(0)->widget());
    };

    auto *quick = static_cast<QuickAccessSidebarButton *>(
        waitForButtonLaidOut(quickButton, [&](int w) { return w <= selectedWidth; },
                             "long Quick Access button is visible and laid out"));
    auto *recent = waitForButtonLaidOut(recentButton, [&](int w) { return w <= selectedWidth; },
                                        "long Recent button is visible and laid out");
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

    quick = static_cast<QuickAccessSidebarButton *>(
        waitForButtonLaidOut(quickButton, [&](int w) { return w <= selectedWidth; },
                             "Quick Access button remains laid out before state checks"));
    quick->setCurrent(true);
    verifyButtonElision(quick, true);
    quick->setProperty("dropActive", true);
    quick->setDown(true);
    verifyButtonElision(quick, true);
    quick->setDown(false);

    const int expectedBtnWidth = quick->width();
    const QUrl mediumUrl = QUrl::fromLocalFile(
        files.filePath(QStringLiteral("Folder with a moderately long name for resize checks")));
    window.m_sidebar->pinQuickAccessLocation(mediumUrl);
    settle();
    auto mediumButton = [&]() -> QuickAccessSidebarButton * {
        for (QuickAccessSidebarButton *button :
             std::as_const(window.m_sidebar->m_quickAccessButtons)) {
            if (sameLocation(button->url(), mediumUrl)) return button;
        }
        return nullptr;
    };
    verifyButtonElision(
        waitForButtonLaidOut(mediumButton, [&](int w) { return w == expectedBtnWidth; },
                             "medium Quick Access entry exists and is laid out"),
        true);
    window.m_sidebarSplitter->setSizes({480, 615});
    window.m_sidebarSplitter->splitterMoved(480, 1);
    settle();
    verifyButtonElision(
        waitForButtonLaidOut(mediumButton, [](int w) { return w >= 400; },
                             "medium Quick Access entry expands with widened sidebar"),
        false);
    verify(settings.value("sidebar/width").toInt() == 480,
           "divider remains resizable and persists its new width with long labels present");
    window.resize(500, 430);
    settle();
    verifyButtonElision(
        waitForButtonLaidOut(mediumButton, [](int w) { return w < 300; },
                             "medium Quick Access entry compresses with narrowed window"),
        true);
    verify(settings.value("sidebar/width").toInt() == 480
               && window.m_sidebarScrollArea->horizontalScrollBar()->maximum() == 0,
           "window compression re-elides labels without overwriting the width preference");

    QSignalSpy activated(window.m_sidebar, &SidebarPanel::activated);
    auto *activeQuick = waitForButtonLaidOut(quickButton, [](int w) { return w > 0; },
                                             "Quick Access button ready for click activation");
    activeQuick->click();
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

static void testToolbarOverflowPaletteIntegrity()
{
    ThisPcWindow window(kThisPcUrl, false);
    window.resize(350, 400);
    window.show();
    settle();

    auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("fileCommandToolbar"));
    verify(toolbar != nullptr, "fileCommandToolbar found");

    QToolButton *ext = nullptr;
    for (auto *child : toolbar->children()) {
        if (child->inherits("QToolBarExtension")) {
            ext = qobject_cast<QToolButton *>(child);
            break;
        }
    }
    verify(ext != nullptr, "QToolBarExtension found");
    verify(ext->isVisible(), "QToolBarExtension is visible when window is narrow");

    const QColor expectedWindowColor = window.palette().color(QPalette::Window);
    verify(toolbar->palette().color(QPalette::Window) == expectedWindowColor,
           "toolbar palette window color matches main window background before click");

    QTimer::singleShot(50, [ext]() {
        ext->showMenu();
    });
    QTimer::singleShot(150, [&]() {
        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            menu->close();
        }
    });
    QTest::qWait(250);
    settle();

    verify(toolbar->palette().color(QPalette::Window) == expectedWindowColor,
           "toolbar palette window color matches main window background after overflow interaction");
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
    testToolbarOverflowPaletteIntegrity();
    testScrollableSidebar();
    testLongLabels();
    testResizePersistenceAndRebuildScroll();
    testDriveMenuSurvivesSnapshotReplacement(QStringLiteral("ntfs"));
    testDriveMenuSurvivesSnapshotReplacement(QStringLiteral("ext4"));
    testRepeatedDriveMenuOpenClose();
    qInfo("PASS: %d sidebar layout assertions", checks);
}
