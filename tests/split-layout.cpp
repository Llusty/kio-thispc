// Exercises the real window/layouts, including deferred Qt layout requests.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static void settle(ThisPcWindow &window)
{
    verify(QTest::qWaitFor([&] {
        return !window.m_primaryPane->listingJob() && !window.m_splitPane->listingJob();
    }, 5000), "directory navigation completes");
    for (int i = 0; i < 5; ++i) {
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
    }
}

static void verifySizes(ThisPcWindow &window, const QList<int> &expected, const char *context)
{
    settle(window);
    const auto actual = window.m_contentSplitter->sizes();
    if (actual != expected)
        qWarning() << context << "expected" << expected << "actual" << actual
                   << "minimum hints" << window.m_primaryPane->minimumSizeHint()
                   << window.m_splitPane->minimumSizeHint();
    verify(actual == expected, context);
}

static QImage rendered(QWidget *widget)
{
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget->render(&image);
    return image;
}

static void verifyDriveCardGeometry()
{
    DriveInfo drive;
    drive.name = QStringLiteral("System");
    drive.freeText = QStringLiteral("40 GiB");
    drive.capacityText = QStringLiteral("100 GiB");
    drive.fileSystem = QStringLiteral("ext4");
    drive.mountPoint = QStringLiteral("/");
    drive.targetUrl = QUrl::fromLocalFile(QStringLiteral("/"));
    drive.iconName = QStringLiteral("drive-harddisk");
    drive.usedPercent = 60;
    QWidget host;
    auto *layout = new QVBoxLayout(&host);
    auto *primary = makeDriveCard(drive, &host);
    auto *split = makeDriveCard(drive, &host);
    layout->addWidget(primary);
    layout->addWidget(split);
    host.show();

    const auto verifyAtWidth = [&](int width) {
        host.resize(width, 240);
        for (int i = 0; i < 3; ++i) {
            QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QApplication::processEvents();
        }
        const auto verifyCard = [&](DriveFrame *card) {
            auto *icon = card->findChild<QLabel *>(QStringLiteral("driveCardIcon"));
            auto *content = card->findChild<QWidget *>(QStringLiteral("driveCardContent"));
            verify(icon && content, "drive card exposes its shared bounded content geometry");
            verify(content->width() <= 335, "drive card content width remains bounded");
            const int gap = content->geometry().left() - icon->geometry().right() - 1;
            verify(gap >= 0 && gap <= 12, "drive icon-to-content distance remains bounded");
            return card->contentsRect().right() - content->geometry().right();
        };
        const int primaryTrailing = verifyCard(primary);
        const int splitTrailing = verifyCard(split);
        verify(primary->findChild<QWidget *>(QStringLiteral("driveCardContent"))->geometry()
                   == split->findChild<QWidget *>(QStringLiteral("driveCardContent"))->geometry(),
               "Primary and Split use identical drive-card geometry");
        verify(primaryTrailing == splitTrailing,
               "Primary and Split leave identical trailing drive-card space");
        return primaryTrailing;
    };

    const int compactTrailing = verifyAtWidth(420);
    const int mediumTrailing = verifyAtWidth(640);
    const int wideTrailing = verifyAtWidth(1000);
    verify(mediumTrailing > compactTrailing && wideTrailing > mediumTrailing,
           "trailing free space grows instead of stretching drive content");
}

static void verifyOverflow(ThisPcWindow &window, const QUrl &left, const QUrl &right)
{
    auto *frame = window.m_breadcrumbFrame;
    auto *scroll = frame->findChild<QScrollArea *>();
    verify(scroll && scroll->horizontalScrollBar()->maximum() > 0,
           "deep left path scrolls within the address row");
    auto *bar = scroll->horizontalScrollBar();
    verify(bar->value() == bar->maximum(), "current left folder is revealed after navigation");
    for (int i = 0; i < window.m_breadcrumbLayout->count(); ++i) {
        auto *button = qobject_cast<QToolButton *>(window.m_breadcrumbLayout->itemAt(i)->widget());
        if (button) verify(button->width() >= button->sizeHint().width(),
                           "scrolling retains readable full breadcrumb segments");
    }
    const auto arrows = frame->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
    verify(arrows.size() == 2, "overflow has two scroll controls");
    QTest::mouseClick(arrows[0], Qt::LeftButton);
    verify(bar->value() < bar->maximum(), "left scroll control exposes ancestors");
    bar->setValue(0);
    verify(!arrows[0]->isEnabled() && arrows[1]->isEnabled(), "scroll controls reflect start of path");
    QTest::mouseClick(arrows[1], Qt::LeftButton);
    verify(bar->value() > 0, "right scroll control exposes later path segments");
    verify(frame->toolTip() == urlForDisplay(left), "left tooltip retains the complete address");

    auto *button = window.m_splitPane->m_breadcrumbButton;
    verify(button->toolTip() == urlForDisplay(right), "right tooltip retains the complete address");
    verify(button->segmentCount() >= 3,
           "long right path exposes individual folder segments");

    const QRect first = button->segmentRect(0);
    const QRect last = button->segmentRect(button->segmentCount() - 1);

    auto *pathScroll = window.m_splitPane->m_breadcrumbScroll;

    verify(pathScroll && pathScroll->horizontalScrollBar()->maximum() > 0,
           "long right path scrolls without shortening folder names");

    verify(!first.isEmpty(),
           "long right path retains complete ancestor segments");

    auto *rightBar = pathScroll->horizontalScrollBar();
    verify(QTest::qWaitFor([rightBar] {
        return rightBar->value() == rightBar->maximum();
    }), "right path scroll settles at the destination");

    const QRect visibleLast(button->mapTo(pathScroll->viewport(), last.topLeft()),
                            button->mapTo(pathScroll->viewport(), last.bottomRight()));
    verify(visibleLast.intersects(pathScroll->viewport()->rect())
           && visibleLast.right() <= pathScroll->viewport()->rect().right(),
           "right path reveals the destination end inside its scroll viewport");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-split-layout-test");
    QCoreApplication::setApplicationName("split-layout-test");
    app.setStyleSheet(applicationStyleSheet());
    QSettings settings;
    settings.clear();
    verifyDriveCardGeometry();
    QTemporaryDir files;
    verify(files.isValid(), "disposable paths");
    const QUrl shortPath = QUrl::fromLocalFile(files.filePath("short"));
    QString deep = files.path();
    for (int i = 0; i < 18; ++i) deep += QStringLiteral("/segment_%1_long_folder_name").arg(i);
    const QUrl deepPath = QUrl::fromLocalFile(deep);
    const QUrl longName = QUrl::fromLocalFile(files.filePath(QString(180, QLatin1Char('W'))));
    for (const auto &url : {shortPath, deepPath, longName})
        verify(QDir().mkpath(url.toLocalFile()), "create navigation fixture");

    ThisPcWindow window(shortPath, false);
    window.resize(1400, 720);
    window.show();
    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(shortPath, true);
    settle(window);

    auto *splitFrame = window.m_splitPane->m_breadcrumbFrame;
    auto *splitViewport = window.m_splitPane->m_breadcrumbScroll->viewport();
    verify(splitFrame->cursor().shape() == window.m_breadcrumbFrame->cursor().shape()
               && splitFrame->cursor().shape() == Qt::IBeamCursor,
           "Split neutral address-bar cursor matches Primary text cursor");
    verify(splitViewport->cursor().shape() == Qt::IBeamCursor,
           "Split blank viewport inherits the neutral text cursor");
    QTest::mouseClick(splitViewport, Qt::LeftButton, Qt::NoModifier,
                      QPoint(splitViewport->width() - 2, splitViewport->height() / 2));
    verify(window.m_splitPane->m_locationStack->currentWidget()
               == window.m_splitPane->m_addressEdit,
           "clicking blank Split address-bar viewport begins address editing");
    QTest::keyClick(window.m_splitPane->m_addressEdit, Qt::Key_Escape);
    verify(window.m_splitPane->m_locationStack->currentWidget() == splitFrame,
           "Escape restores the Split breadcrumb presentation");

    QTest::mouseClick(splitFrame, Qt::LeftButton, Qt::NoModifier,
                      QPoint(2, splitFrame->height() / 2));
    verify(window.m_splitPane->m_locationStack->currentWidget()
               == window.m_splitPane->m_addressEdit,
           "clicking the Split address-bar margin begins address editing");
    QTest::keyClick(window.m_splitPane->m_addressEdit, Qt::Key_Escape);

    const QRect splitSegment = window.m_splitPane->m_breadcrumbButton->segmentRect(0);
    QTest::mouseMove(window.m_splitPane->m_breadcrumbButton, splitSegment.center());
    verify(window.m_splitPane->m_breadcrumbButton->cursor().shape()
               == Qt::PointingHandCursor,
           "Split breadcrumb segment retains its interactive cursor");
    QTest::mouseClick(window.m_splitPane->m_breadcrumbButton, Qt::LeftButton,
                      Qt::NoModifier, splitSegment.center());
    verify(window.m_splitPane->m_locationStack->currentWidget() == splitFrame,
           "clicking a Split breadcrumb segment keeps navigation behavior");

    window.m_splitPane->setCurrentUrl(deepPath, true);
    settle(window);
    auto *splitPrevious = window.m_splitPane->m_breadcrumbPrevious;
    const int beforeScroll = window.m_splitPane->m_breadcrumbScroll
                                 ->horizontalScrollBar()->value();
    QTest::mouseClick(splitPrevious, Qt::LeftButton);
    verify(window.m_splitPane->m_locationStack->currentWidget() == splitFrame,
           "Split breadcrumb scroll control is not captured as blank edit");
    verify(window.m_splitPane->m_breadcrumbScroll->horizontalScrollBar()->value()
               < beforeScroll,
           "Split breadcrumb scroll control retains scrolling behavior");

    window.m_splitPane->setCurrentUrl(shortPath, true);
    settle(window);
    window.m_splitPane->beginAddressEdit();
    verify(window.m_splitPane->m_addressEdit->text() == urlForDisplay(shortPath),
           "Split editing starts with the complete raw address");
    QTest::mouseClick(window.m_splitPane->m_list->viewport(), Qt::LeftButton,
                      Qt::NoModifier, QPoint(2, 2));
    QApplication::processEvents();
    verify(QTest::qWaitFor([&] {
        return window.m_splitPane->m_locationStack->currentWidget() == splitFrame;
    }), "Split focus-out hides the editor and restores breadcrumbs");

    window.m_splitPane->beginAddressEdit();
    window.m_splitPane->m_addressEdit->setText(urlForDisplay(longName));
    QTest::keyClick(window.m_splitPane->m_addressEdit, Qt::Key_Return);
    settle(window);
    verify(sameLocation(window.m_splitPane->currentUrl(), longName),
           "Split Enter navigates to a valid address");
    verify(window.m_splitPane->m_locationStack->currentWidget() == splitFrame,
           "Split Enter returns to breadcrumb presentation");

    window.navigateTo(shortPath, true);
    settle(window);
    QTest::mouseClick(window.m_breadcrumbFrame, Qt::LeftButton, Qt::NoModifier,
                      QPoint(2, window.m_breadcrumbFrame->height() / 2));
    verify(window.m_addressStack->currentWidget() == window.m_addressEdit,
           "Primary blank address-bar click behavior is unchanged");
    QTest::mouseClick(window.m_primaryPane->listView()->viewport(), Qt::LeftButton,
                      Qt::NoModifier, QPoint(2, 2));
    QApplication::processEvents();
    verify(QTest::qWaitFor([&] {
        return window.m_addressStack->currentWidget() == window.m_breadcrumbFrame;
    }), "Primary focus-out behavior remains unchanged");

    window.m_splitPane->setCurrentUrl(shortPath, true);
    settle(window);
    window.m_contentSplitter->setSizes({620, 500});
    settle(window);
    const QSize originalWindow = window.size();
    auto expected = window.m_contentSplitter->sizes();
    const auto checkNavigation = [&](const QUrl &left, const QUrl &right, const char *context) {
        window.navigateTo(left, true);
        window.m_splitPane->setCurrentUrl(right, true);
        verifySizes(window, expected, context);
        verify(window.size() == originalWindow, "paths never enlarge the window");
    };
    checkNavigation(deepPath, shortPath, "deep left breadcrumbs preserve divider");
    checkNavigation(shortPath, deepPath, "deep right breadcrumbs preserve divider");
    checkNavigation(longName, shortPath, "long left component/title preserves divider");
    checkNavigation(shortPath, longName, "long right component/title preserves divider");
    checkNavigation(deepPath, longName, "both long paths preserve divider");
    verifyOverflow(window, deepPath, longName);
    if (!qEnvironmentVariableIsEmpty("THISPC_LAYOUT_SNAPSHOT"))
        window.grab().save(qEnvironmentVariable("THISPC_LAYOUT_SNAPSHOT"));

    for (auto pane : {ThisPcWindow::PaneId::Primary, ThisPcWindow::PaneId::Split}) {
        window.setActivePane(pane);
        window.beginAddressEdit(pane);
        verifySizes(window, expected, "address editing and focus preserve divider");
        auto *edit = pane == ThisPcWindow::PaneId::Primary
            ? static_cast<QLineEdit *>(window.m_addressEdit) : window.m_splitPane->m_addressEdit;
        verify(edit->text() == urlForDisplay(pane == ThisPcWindow::PaneId::Primary ? deepPath : longName),
               "address editing exposes the complete path");
        QTest::keyClick(edit, Qt::Key_Escape);
        verifySizes(window, expected, "Escape to breadcrumbs preserves divider");
    }

    // Home hides the directory page but QStackedWidget still considers its hints.
    checkNavigation(kThisPcUrl, kThisPcUrl, "hidden long pages do not push home panels");
    checkNavigation(deepPath, longName, "returning from home preserves divider");
    window.swapSplitPanes();
    verifySizes(window, expected, "swapping long locations preserves divider");
    window.setSplitViewEnabled(false);
    settle(window);
    window.setSplitViewEnabled(true);
    verifySizes(window, expected, "closing and reopening Split preserves divider");

    // Exercise real mouse events rather than setSizes()/emitting splitterMoved.
    auto *handle = window.m_contentSplitter->handle(1);
    QSignalSpy moved(window.m_contentSplitter, &QSplitter::splitterMoved);
    const QPoint press = handle->rect().center();
    QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, press);
    QTest::mouseMove(handle, press + QPoint(85, 0));
    QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, handle->rect().center());
    settle(window);
    verify(moved.count() > 0 && window.m_contentSplitter->sizes() != expected,
           "user can drag divider with long paths in both panes");
    expected = window.m_contentSplitter->sizes();
    const QByteArray saved = settings.value("split/state").toByteArray();
    verify(saved == window.m_contentSplitter->saveState(), "manual divider position is persisted");
    checkNavigation(shortPath, deepPath, "navigation preserves manually dragged divider");

    const double ratio = double(expected[0]) / (expected[0] + expected[1]);
    window.resize(1700, 800);
    settle(window);
    const auto larger = window.m_contentSplitter->sizes();
    verify(qAbs(double(larger[0]) / (larger[0] + larger[1]) - ratio) < 0.01,
           "growing window preserves chosen pane proportions");
    window.navigateTo(deepPath, true);
    window.m_splitPane->setCurrentUrl(shortPath, true);
    verifySizes(window, larger, "navigation after resize preserves divider");
    window.resize(originalWindow);
    settle(window);
    const auto restored = window.m_contentSplitter->sizes();
    verify(qAbs(restored[0] - expected[0]) <= 1 && qAbs(restored[1] - expected[1]) <= 1,
           "resize round trip restores chosen widths");
    verify(settings.value("split/state").toByteArray() == saved,
           "navigation and window resize do not overwrite saved manual split");

    // Minimum widths may constrain a very narrow window, but must not replace
    // the ratio selected by the user once there is room for it again.
    window.resize(720, 600);
    settle(window);
    const auto narrow = window.m_contentSplitter->sizes();
    window.navigateTo(longName, true);
    window.m_splitPane->setCurrentUrl(deepPath, true);
    verifySizes(window, narrow, "long paths preserve divider in a narrow window");
    window.resize(originalWindow);
    settle(window);
    verify(qAbs(window.m_contentSplitter->sizes()[0] - expected[0]) <= 1,
           "expansion after minimum width constraint restores user allocation");
    window.setSplitViewEnabled(false);
    settle(window);
    window.close();
    const auto persisted = settings.value("split/state").toByteArray();
    verify(!persisted.isEmpty(), "closing with Split hidden retains the two-pane state");
    ThisPcWindow reopened(shortPath, false);
    reopened.resize(originalWindow);
    reopened.show();
    reopened.setSplitViewEnabled(true);
    settle(reopened);
    verify(qAbs(reopened.m_contentSplitter->sizes()[0] - expected[0]) <= 1,
           "a new window restores the user's split after closing with Split hidden");
    qInfo("PASS: %d split layout assertions", checks);
}
