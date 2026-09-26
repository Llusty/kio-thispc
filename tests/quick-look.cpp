// Appended to a temporary, instrumented copy by run-pane-actions.py.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static void addFixtureItem(DirectoryListWidget *list, const QUrl &url, const QString &name)
{
    FileInfo file{name, QString(), QString(), url, false, 0, 0};
    list->addFileItem(file, QIcon(), QStringLiteral("File"),
                      QStringLiteral("0"), QStringLiteral("Today"), QString());
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("thispc-quick-look-test"));
    QCoreApplication::setApplicationName(QStringLiteral("quick-look-test"));
    QTemporaryDir files;
    verify(files.isValid(), "temporary Quick Look directory");
    const QString leftPath = files.filePath(QStringLiteral("left"));
    const QString rightPath = files.filePath(QStringLiteral("right"));
    verify(QDir().mkpath(leftPath) && QDir().mkpath(rightPath), "pane fixture folders");
    auto write = [](const QString &path, const QByteArray &contents) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    };
    verify(write(leftPath + QStringLiteral("/left.txt"), "left preview"), "left fixture");
    verify(write(leftPath + QStringLiteral("/new.txt"), "new preview"), "replacement fixture");
    verify(write(rightPath + QStringLiteral("/right.txt"), "right preview"), "right fixture");
    verify(write(rightPath + QStringLiteral("/second.txt"), "second preview"), "second split fixture");

    ThisPcWindow window(QUrl::fromLocalFile(leftPath));
    window.resize(1000, 700);
    window.show();
    window.activateWindow();
    QTest::qWait(150);
    window.m_refreshTimer.stop();
    window.m_primaryPane->cancelListing();

    window.m_directoryList->clear();
    addFixtureItem(window.m_directoryList, QUrl::fromLocalFile(leftPath + "/left.txt"), "left.txt");
    addFixtureItem(window.m_directoryList, QUrl::fromLocalFile(leftPath + "/new.txt"), "new.txt");
    window.m_directoryList->clearSelection();
    window.m_directoryList->setFocus();
    QTest::keyClick(window.m_directoryList, Qt::Key_Space);
    verify(!window.m_quickLook->isVisible(), "zero primary selections cannot open Quick Look");
    window.m_directoryList->selectionModel()->select(window.m_directoryList->item(0), QItemSelectionModel::Select);
    window.m_directoryList->selectionModel()->select(window.m_directoryList->item(1), QItemSelectionModel::Select);
    QTest::keyClick(window.m_directoryList, Qt::Key_Space);
    verify(!window.m_quickLook->isVisible(), "multiple primary selections cannot open Quick Look");
    window.m_directoryList->setCurrentRow(0);
    window.m_directoryList->setFocus();
    app.processEvents();
    QTest::keyClick(window.m_directoryList, Qt::Key_Space);
    app.processEvents();
    verify(window.m_quickLook->isVisible(), "Space opens Quick Look from the primary pane");
    verify(window.m_quickLook->width() > 520, "Quick Look is larger than Preview Pane");
    verify(window.m_directoryList->hasFocus(), "opening preserves directory focus");
    verify(QTest::qWaitFor([&] {
        return window.m_quickLook->previewPane()->m_text->toPlainText() == QStringLiteral("left preview");
    }, 5000), "primary selection is previewed asynchronously");
    const QRect initialGeometry = window.m_quickLook->geometry();
    window.resize(1200, 800);
    app.processEvents();
    const QRect area = window.centralWidget()->rect().adjusted(36, 36, -36, -36);
    const int expectedWidth = qMin(1100, qMax(420, area.width() * 4 / 5));
    const int expectedHeight = qMin(760, qMax(300, area.height() * 4 / 5));
    const QRect expectedGeometry(area.center().x() - expectedWidth / 2,
                                 area.center().y() - expectedHeight / 2,
                                 expectedWidth, expectedHeight);
    verify(window.m_quickLook->geometry() != initialGeometry
               && window.m_quickLook->geometry() == expectedGeometry,
           "window resize repositions and resizes Quick Look around the host center");

    QTest::keyClick(window.m_directoryList, Qt::Key_Right);
    verify(window.m_directoryList->currentRow() == 1, "arrow navigation remains active behind Quick Look");
    verify(QTest::qWaitFor([&] {
        return window.m_quickLook->previewPane()->m_text->toPlainText() == QStringLiteral("new preview");
    }, 5000), "arrow navigation refreshes an open Quick Look");

    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(rightPath), false);
    QTest::qWait(100);
    if (window.m_splitPane->m_job) { window.m_splitPane->m_job->kill(); window.m_splitPane->m_job = nullptr; }
    window.m_splitPane->listView()->clear();
    addFixtureItem(window.m_splitPane->listView(),
                   QUrl::fromLocalFile(rightPath + "/right.txt"), "right.txt");
    addFixtureItem(window.m_splitPane->listView(),
                   QUrl::fromLocalFile(rightPath + "/second.txt"), "second.txt");
    window.m_splitPane->listView()->setCurrentRow(0);
    window.setActivePane(ThisPcWindow::PaneId::Split);
    window.m_splitPane->listView()->setFocus();
    app.processEvents();
    verify(QTest::qWaitFor([&] {
        return window.m_quickLook->previewPane()->m_text->toPlainText() == QStringLiteral("right preview");
    }, 5000), "active split pane reroutes an open Quick Look");
    window.m_splitPane->listView()->clearSelection();
    app.processEvents();
    verify(window.m_quickLook->isVisible()
               && window.m_quickLook->previewPane()->m_title->text().isEmpty(),
           "invalid split selection clears but preserves the open Quick Look overlay");
    window.m_splitPane->listView()->setCurrentRow(0);
    app.processEvents();
    verify(QTest::qWaitFor([&] {
        return window.m_quickLook->previewPane()->m_text->toPlainText() == QStringLiteral("right preview");
    }, 5000), "restoring one split selection refreshes the open Quick Look");
    window.m_splitPane->listView()->selectionModel()->select(
        window.m_splitPane->listView()->item(1), QItemSelectionModel::Select);
    app.processEvents();
    verify(window.m_quickLook->isVisible()
               && window.m_quickLook->previewPane()->m_title->text().isEmpty(),
           "multiple split selections clear but preserve the open Quick Look overlay");
    window.m_splitPane->listView()->setCurrentRow(0);
    window.setSplitViewEnabled(false);
    window.m_directoryList->setFocus();
    app.processEvents();
    verify(window.m_quickLook->isVisible() && QTest::qWaitFor([&] {
        return window.m_quickLook->previewPane()->m_text->toPlainText() == QStringLiteral("new preview");
    }, 5000), "disabling Split View reroutes open Quick Look to the primary selection");

    QTest::keyClick(window.m_directoryList, Qt::Key_Escape);
    verify(!window.m_quickLook->isVisible(), "Esc closes Quick Look");
    verify(window.m_directoryList->hasFocus(), "closing preserves primary-pane focus after Split View closes");

    window.m_searchEdit->setFocus();
    QTest::keyClick(window.m_searchEdit, Qt::Key_Space);
    verify(!window.m_quickLook->isVisible() && window.m_searchEdit->text() == QStringLiteral(" "),
           "Space remains text input in Search");
    window.m_splitPane->m_addressEdit->setFocus();
    window.m_splitPane->m_addressEdit->clear();
    QTest::keyClick(window.m_splitPane->m_addressEdit, Qt::Key_Space);
    verify(!window.m_quickLook->isVisible()
               && window.m_splitPane->m_addressEdit->text() == QStringLiteral(" "),
           "Space remains text input in address editing");

    window.m_previewAction->setChecked(true);
    window.m_directoryList->setCurrentRow(0);
    window.m_directoryList->setFocus();
    app.processEvents();
    window.setQuickLookVisible(true);
    verify(window.m_quickLook->isVisible() && !window.m_previewPane->isHidden(),
           "Quick Look coexists with Alt+P Preview Pane");
    window.m_previewAction->setChecked(false);
    verify(window.m_quickLook->isVisible(), "Alt+P does not close Quick Look");
    QTest::keyClick(window.m_directoryList, Qt::Key_Space);
    verify(!window.m_quickLook->isVisible(), "Space toggles Quick Look closed");

    qInfo("PASS: %d Quick Look assertions", checks);
}
