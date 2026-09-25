// Real filename-search workers only scan this suite's temporary directories.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static QSet<QString> paths(const QList<FileInfo> &files)
{
    QSet<QString> result;
    for (const auto &file : files) result.insert(file.url.toLocalFile());
    return result;
}

static void waitForSearch(SearchController &search)
{
    verify(QTest::qWaitFor([&] { return !search.isRunning(); }, 10000), "KIO search completes");
}

static KIO::UDSEntry entry(const QString &path)
{
    KIO::UDSEntry value;
    value.fastInsert(KIO::UDSEntry::UDS_NAME, QFileInfo(path).fileName());
    value.fastInsert(KIO::UDSEntry::UDS_URL, QUrl::fromLocalFile(path).toString());
    value.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG);
    value.fastInsert(KIO::UDSEntry::UDS_SIZE, 12);
    return value;
}

static void testFilters()
{
    QMimeDatabase db;
    FileInfo file;
    file.name = "sample.txt";
    file.mimeType = "text/plain";
    file.modificationTime = QDateTime::currentSecsSinceEpoch();
    file.size = 50;
    verify(SearchController::matchesFile(file, db, {}), "unfiltered file");
    verify(SearchController::matchesFile(file, db, {3, 1, 1}), "document/date/size conjunction");
    verify(!SearchController::matchesFile(file, db, {2, 0, 0}), "text excluded from images");
    for (const auto &[type, mime] : QList<QPair<int, QString>>{
             {2, "image/png"}, {3, "application/pdf"}, {4, "audio/mpeg"},
             {5, "video/mp4"}, {6, "application/zip"}}) {
        file.mimeType = mime;
        verify(SearchController::matchesFile(file, db, {type, 0, 0}), "MIME category accepted");
    }
    file.isDir = true;
    verify(SearchController::matchesFile(file, db, {1, 0, 0}), "folder filter");
    verify(!SearchController::matchesFile(file, db, {0, 0, 1}), "size filter excludes folders");
    file.isDir = false;
    file.size = 1024 * 1024;
    verify(!SearchController::matchesFile(file, db, {0, 0, 1}), "small size upper boundary excluded");
    verify(SearchController::matchesFile(file, db, {0, 0, 2}), "medium size lower boundary included");
    file.size = 100LL * 1024 * 1024;
    verify(!SearchController::matchesFile(file, db, {0, 0, 2}), "medium upper boundary excluded");
    verify(SearchController::matchesFile(file, db, {0, 0, 3}), "large lower boundary included");
    file.size = 1024LL * 1024 * 1024;
    verify(!SearchController::matchesFile(file, db, {0, 0, 3}), "large upper boundary excluded");
    verify(SearchController::matchesFile(file, db, {0, 0, 4}), "huge lower boundary included");
    file.size = -1;
    verify(!SearchController::matchesFile(file, db, {0, 0, 4}), "unknown size excluded");
    file.modificationTime -= 3 * 24 * 60 * 60;
    verify(!SearchController::matchesFile(file, db, {0, 1, 0}), "old file excluded from last day");
    verify(SearchController::matchesFile(file, db, {0, 2, 0}), "three days fits last week");
    file.modificationTime = 0;
    verify(!SearchController::matchesFile(file, db, {0, 4, 0}), "unknown date excluded");
}

static void testSearchUiContract(const QUrl &rootA, const QUrl &rootB)
{
    QLineEdit edit;
    QAction stopAction(nullptr), scopeAction(nullptr);
    QToolButton filterButton;
    QActionGroup scopeGroup(nullptr), typeGroup(nullptr), dateGroup(nullptr), sizeGroup(nullptr);
    const auto addChoice = [](QActionGroup &group, int value) {
        auto *action = new QAction(&group);
        action->setCheckable(true);
        action->setData(value);
        group.addAction(action);
    };
    for (int value = 0; value <= 2; ++value) addChoice(scopeGroup, value);
    for (int value = 0; value <= 6; ++value) addChoice(typeGroup, value);
    for (int value = 0; value <= 4; ++value) {
        addChoice(dateGroup, value);
        addChoice(sizeGroup, value);
    }
    QFrame progressFrame;
    QProgressBar progressBar;
    QLabel status;
    SearchController backend;
    SearchUiController ui(
        {&edit, &stopAction, &scopeAction, &filterButton, &scopeGroup,
         &typeGroup, &dateGroup, &sizeGroup, &progressFrame, &progressBar, &status},
        {[](const char *, const char *en) { return QString::fromUtf8(en); },
         [](const QUrl &url) { return url.fileName(); }, {}, [] { return false; }});

    PaneSearchState state;
    state.loadLocation(rootA);
    state.text = QStringLiteral(" needle ");
    state.scope = 1;
    state.type = 3;
    state.date = 2;
    state.size = 1;
    ui.updateControls({rootA, &state, &backend});
    verify(edit.text() == QStringLiteral(" needle ")
               && scopeGroup.checkedAction()->data().toInt() == 1,
           "Search UI state maps to edit and scope controls");
    verify(typeGroup.checkedAction()->data().toInt() == 3
               && dateGroup.checkedAction()->data().toInt() == 2
               && sizeGroup.checkedAction()->data().toInt() == 1,
           "Search UI filters map to action groups");
    verify(filterButton.toolTip() == QStringLiteral("Search filters (3 active)"),
           "Search UI active-filter presentation");

    edit.setText(QStringLiteral("  needle  "));
    const auto request = ui.requestFromUi({rootA, &state, &backend},
        {QUrl::fromLocalFile(QStringLiteral("/")), rootA}, rootB);
    verify(request.query == QStringLiteral("needle") && request.scope == 1
               && sameLocation(request.base, rootA),
           "UI controls map to explicit search request");
    const QUrl requestUrl = request.location(state);
    verify(searchQueryFromUrl(requestUrl) == QStringLiteral("needle")
               && searchIntParameter(requestUrl, QStringLiteral("type"), 0) == 3
               && searchIntParameter(requestUrl, QStringLiteral("date"), 0) == 2
               && searchIntParameter(requestUrl, QStringLiteral("size"), 0) == 1,
           "search request preserves URL filter contract");

    const QList<QUrl> roots = SearchUiController::wholeComputerSearchRoots(
        {rootA, normalizedUrl(rootA), rootB});
    verify(roots.size() == 2 && sameLocation(roots.at(0), rootA)
               && sameLocation(roots.at(1), rootB),
           "whole-computer roots are normalized and deduplicated");
    verify(sameLocation(SearchUiController::bestDriveRootForUrl(
                            QUrl::fromLocalFile(rootA.toLocalFile() + QStringLiteral("/nested")),
                            {QUrl::fromLocalFile(QStringLiteral("/")), rootA}), rootA),
           "current-drive scope selects deepest containing root");
    verify(sameLocation(SearchUiController::searchContextUrl(kThisPcUrl, rootB), rootB),
           "This PC search context falls back to home");

    const QUrl activeSearch = makeSearchLocation(QStringLiteral("needle"), 0, rootA, 0, 0, 0);
    state.type = 2;
    state.date = 1;
    state.size = 4;
    const QUrl synced = ui.locationWithSyncedFilters({activeSearch, &state, &backend});
    verify(searchBaseFromUrl(synced) == rootA
               && searchIntParameter(synced, QStringLiteral("type"), 0) == 2
               && searchIntParameter(synced, QStringLiteral("date"), 0) == 1
               && searchIntParameter(synced, QStringLiteral("size"), 0) == 4,
           "filter synchronization preserves search identity and updates filters");
    verify(ui.rootsForLocation(activeSearch, state, {rootA, rootB}, rootB)
               == QList<QUrl>{rootA},
           "folder search URL maps to its encoded root");

    ui.updateProgress(backend);
    ui.updateStatus(backend, 7, 0);
    verify(progressBar.value() == backend.progressPercent(),
           "progress widget mirrors SearchController");
    verify(status.text() == backend.statusText(7, 0),
           "status widget mirrors SearchController text");

    PaneSearchState other;
    other.loadLocation(rootB);
    other.text = QStringLiteral("other pane");
    ui.updateControls({rootB, &other, &backend});
    verify(edit.text() == QStringLiteral("other pane") && other.type == 0,
           "active-pane change restores independent Search UI state");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-search-test");
    QCoreApplication::setApplicationName("search-test");
    QTemporaryDir files;
    verify(files.isValid(), "temporary search directory");
    const QString a = files.filePath("A"), b = files.filePath("B");
    for (const QString &folder : {a + "/nested", a + "/needle-folder", b})
        verify(QDir().mkpath(folder), "temporary search folders");
    for (const QString &name : {a + "/needle.txt", a + "/nested/needle-deep.txt", a + "/other.txt",
                               a + "/.needle-hidden.txt", b + "/needle-second.txt"}) {
        QFile file(name);
        verify(file.open(QIODevice::WriteOnly), "temporary search file");
        file.write("search test\n");
    }
    const QUrl rootA = QUrl::fromLocalFile(a), rootB = QUrl::fromLocalFile(b);
    testSearchUiContract(rootA, rootB);
    const QSet<QString> expectedA{a + "/needle.txt", a + "/nested/needle-deep.txt", a + "/needle-folder"};
    SearchController search;
    QSignalSpy finished(&search, &SearchController::finished);
    QSignalSpy rendered(&search, &SearchController::resultsChanged);
    search.start("needle", {rootA, QUrl::fromLocalFile(a + "/nested")}, false);
    verify(search.isRunning() && search.totalRoots() == 2, "asynchronous multi-root search starts");
    waitForSearch(search);
    verify(search.errorCount() == 0 && search.completedRoots() == 2, "both workers succeed");
    verify(paths(search.files()) == expectedA && search.files().size() == 3, "recursive results deduplicated across overlapping roots");
    verify(search.progressPercent() == 100 && finished.count() == 1 && rendered.count() >= 1, "completion and final render signals");
    search.start("needle", {rootB}, false);
    waitForSearch(search);
    verify(paths(search.files()) == QSet<QString>{b + "/needle-second.txt"}, "new search clears old results");
    search.start("missing-query", {rootA}, false);
    waitForSearch(search);
    verify(search.files().isEmpty() && search.errorCount() == 0, "empty result succeeds");

    // Deterministically deliver late worker signals before deferred deletion.
    search.start("needle", {rootA}, false);
    QPointer<KIO::ListJob> old = search.m_searchJobs.first();
    old->entries(old, {entry(a + "/needle.txt"), entry(a + "/needle.txt"), entry(a + "/.needle-hidden.txt")});
    verify(search.files().size() == 1, "batch deduplicates and hides dot files");
    verify(search.m_searchRenderTimer.isActive(), "partial results schedule a batched render");
    const int finishedBeforeCancel = finished.count();
    verify(search.cancel() && !search.isRunning(), "cancel stops active search");
    verify(paths(search.files()) == QSet<QString>{a + "/needle.txt"}, "cancel preserves partial results");
    verify(!search.m_searchRenderTimer.isActive(), "cancel stops pending render timer");
    verify(old != nullptr, "canceled worker available until deferred deletion");
    old->entries(old, {entry(a + "/stale.txt")});
    verify(search.files().size() == 1, "late entries ignored after cancellation");
    search.start("needle", {rootB}, false);
    old->entries(old, {entry(a + "/stale.txt")});
    verify(search.files().isEmpty(), "late entries cannot contaminate replacement search");
    waitForSearch(search);
    verify(paths(search.files()) == QSet<QString>{b + "/needle-second.txt"}, "replacement search completes cleanly");
    verify(finished.count() == finishedBeforeCancel + 1, "canceled search never reports completion");

    search.start("needle", {rootA}, true);
    auto *hiddenJob = search.m_searchJobs.first().data();
    hiddenJob->entries(hiddenJob, {entry(a + "/.needle-hidden.txt")});
    verify(paths(search.files()).contains(a + "/.needle-hidden.txt"), "show-hidden accepts hidden worker entries");
    search.cancel();
    search.start("needle", {}, false);
    verify(!search.isRunning() && search.files().isEmpty(), "empty root set finishes without hanging");
    search.start("needle", {QUrl("unavailable-thispc-test:/")}, false);
    waitForSearch(search);
    // filenamesearch may swallow a listing error in its base location;
    // completion must still be delivered without hanging the search UI.
    verify(search.completedRoots() == 1 && search.files().isEmpty(), "unavailable root finishes with no results");
    testFilters();

    ThisPcWindow window(rootA, false);
    window.show();
    window.m_refreshTimer.stop();
    if (window.m_driveJob) { window.m_driveJob->kill(); window.m_driveJob = nullptr; }
    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(rootB);
    verify(QTest::qWaitFor([&] { return window.m_splitPane->listView()->count() == 1; }, 5000), "split pane loads test directory");
    const QUrl location = makeSearchLocation("needle", 0, rootA, 0, 0, 0);
    verify(isSearchLocation(location) && searchQueryFromUrl(location) == "needle" && searchBaseFromUrl(location) == rootA,
           "search URL preserves query and base");
    window.navigateTo(location, true);
    waitForSearch(*window.m_searchController);
    verify(window.m_directoryList->count() == 3 && window.m_directoryDetails->topLevelItemCount() == 3, "window displays search results in both views");
    verify(!window.m_searchProgressFrame->isVisible() && !window.m_stopSearchAction->isVisible(), "completion hides search controls");
    verify(window.m_splitPane->currentUrl() == rootB && window.m_splitPane->listView()->count() == 1, "search preserves split pane");
    for (int mode : {0, 1, 2, 3}) {
        window.setDirectoryViewMode(mode);
        window.setSortKey(2);
        window.setSortAscending(false);
        verify(window.m_directoryList->count() == 3, "view and sort changes keep search results");
    }
    window.m_primarySearch.type = 1;
    window.syncCurrentSearchFilters();
    window.renderDirectoryItems();
    verify(window.m_directoryList->count() == 1 && window.m_searchVisibleCount == 1, "changing type filter reuses current results");
    verify(searchIntParameter(window.m_currentUrl, "type", 0) == 1, "filter stored in search history URL");
    window.navigateTo(makeSearchLocation("needle", 0, rootA, 0, 0, 0), true);
    auto *job = window.m_searchController->m_searchJobs.first().data();
    job->entries(job, {entry(a + "/needle.txt")});
    window.m_cancelSearchButton->click();
    verify(!window.m_searchController->isRunning() && window.m_directoryList->count() == 1, "Cancel button displays partial results");
    verify(!window.m_searchProgressFrame->isVisible(), "Cancel hides progress");
    verify(window.statusBar()->currentMessage() == "Search canceled", "Cancel status text");
    window.navigateTo(makeSearchLocation("needle", 0, rootA, 0, 0, 0), true);
    window.navigateTo(rootB, true);
    verify(!window.m_searchController->isRunning(), "navigation cancels current search");
    verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 1; }, 5000), "new directory loads after cancellation");
    verify(window.m_currentUrl == rootB && paths(window.m_primaryPane->files()) == QSet<QString>{b + "/needle-second.txt"}, "late search results do not replace ordinary directory");
    const QString refusedRedoStatus =
        "Files or their inode identities changed; no Undo/Redo was performed. "
        "After safely restoring the expected files, you can try again.";
    window.statusBar()->showMessage(refusedRedoStatus, 12000);
    window.renderDirectoryItems(true);
    verify(window.statusBar()->currentMessage() == refusedRedoStatus,
           "renderDirectoryItems preserves an actionable Redo refusal when requested");
    window.m_undoController->m_refreshViews(true);
    verify(window.m_primaryPane->listingJob()
               && window.m_primaryPane->listingJob()->property("thispcPreserveStatusMessage").toBool(),
           "preserving refresh attaches the marker to the correct listing job");
    verify(QTest::qWaitFor([&] { return window.m_primaryPane->listingJob() == nullptr; }, 5000)
               && window.statusBar()->currentMessage() == refusedRedoStatus,
           "asynchronous directory refresh does not overwrite the Redo refusal");
    // Stage 4: shared Search controls follow the active pane, including drafts,
    // history URLs and workers that continue while the other pane has focus.
    using Pane = ThisPcWindow::PaneId;
    auto *split = window.m_splitPane;
    window.activateWindow();
    window.navigateTo(rootB, true);
    split->setCurrentUrl(rootA);
    verify(QTest::qWaitFor([&] { return split->listView()->count() == 4; }, 5000),
           "right ordinary directory loads before live filtering");
    window.setActivePane(Pane::Split);
    QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
    verify(window.m_searchEdit->hasFocus() && window.m_activePane == Pane::Split,
           "Ctrl+F focuses shared Search while preserving right pane");
    window.m_searchEdit->setText("needle");
    verify(split->listView()->count() == 2 && window.m_directoryList->count() == 1,
           "live filtering affects only the right active directory");
    window.setActivePane(Pane::Primary);
    verify(window.m_searchEdit->text().isEmpty()
               && window.m_searchEdit->placeholderText().contains("B"),
           "left pane restores its own search draft and placeholder");
    window.m_searchEdit->setText("second");
    window.setActivePane(Pane::Split);
    verify(window.m_searchEdit->text() == "needle" && window.m_searchScopeAction->isEnabled(),
           "returning right restores live query and folder scope");
    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
    const QUrl rightSearch = split->currentUrl();
    verify(isSearchLocation(rightSearch) && searchBaseFromUrl(rightSearch) == rootA
               && searchIntParameter(rightSearch, "scope", -1) == 0,
           "Enter routes recursive Search to the right folder");
    verify(window.m_currentUrl == rootB && window.m_primarySearch.text == "second",
           "right Search preserves left location and draft");
    waitForSearch(*split->searchController());
    verify(paths(split->m_pending) == expectedA && split->listView()->count() == 3,
           "right Search receives real recursive KIO results");
    verify(!split->m_details->isColumnHidden(4)
               && split->m_details->topLevelItem(0)->text(4)
                    == parentLocationForDisplay(QUrl(split->m_details->topLevelItem(0)->data(0, Qt::UserRole).toString())),
           "right Search details include each result's parent location");
    verify(split->m_breadcrumbButton->text() == "Results for: needle"
               && split->m_thisPcCrumb->isVisible()
               && window.m_searchEdit->placeholderText() == "New search"
               && !window.m_searchScopeAction->isEnabled(),
           "right Search has friendly breadcrumbs and correct shared controls");
    for (int mode : {0, 1, 2, 3}) {
        for (auto *action : window.m_viewButton->menu()->actions())
            if (action->data().isValid() && action->data().toInt() == mode) action->trigger();
        for (auto *action : window.m_sortButton->menu()->actions())
            if (action->data().isValid() && action->data().toInt() == 2) action->trigger();
        verify(split->viewMode() == mode && split->listView()->count() == 3,
               "shared View and Sort preserve right Search results");
    }
    auto choose = [](QActionGroup *group, int value) {
        for (auto *action : group->actions())
            if (action->data().toInt() == value) action->trigger();
    };
    const int rightHistorySize = split->m_history.size();
    choose(window.m_searchTypeGroup, 1);
    const QUrl filteredSearch = split->currentUrl();
    verify(split->listView()->count() == 1
               && split->m_history.size() == rightHistorySize
               && split->m_history.at(split->m_historyIndex) == filteredSearch
               && searchIntParameter(filteredSearch, "type", 0) == 1,
           "right type filter reuses results and updates existing history entry");
    verify(window.m_tabs.at(window.m_activeTab).splitUrl == filteredSearch,
           "right filtered Search URL is saved in tab state");
    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    verify(split->m_addressEdit->text() == urlForDisplay(filteredSearch),
           "right Ctrl+L exposes current filtered Search URL");
    QTest::keyClick(split->m_addressEdit, Qt::Key_Escape);
    verify(split->m_locationStack->currentWidget() == split->m_breadcrumbFrame,
           "right Escape restores Search breadcrumb");
    window.m_upAction->trigger();
    verify(split->currentUrl() == rootA && window.m_currentUrl == rootB
               && split->m_details->isColumnHidden(4),
           "right Search Up returns to base and hides result-location column");
    window.m_backAction->trigger();
    waitForSearch(*split->searchController());
    verify(split->currentUrl() == filteredSearch && split->listView()->count() == 1
               && window.m_searchTypeGroup->checkedAction()->data().toInt() == 1,
           "Back restores right Search filters and results");
    window.m_forwardAction->trigger();
    verify(split->currentUrl() == rootA && window.m_searchEdit->text().isEmpty(),
           "Forward restores right directory and clears Search draft");
    // Current drive must use the right location, with test-only drive roots.
    DriveInfo driveA; driveA.name = "A"; driveA.targetUrl = rootA;
    DriveInfo driveB; driveB.name = "B"; driveB.targetUrl = rootB;
    window.m_drives = {driveA, driveB};
    split->setCurrentUrl(QUrl::fromLocalFile(a + "/nested"));
    choose(window.m_searchScopeGroup, 1);
    window.m_searchEdit->setText("needle");
    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
    verify(searchBaseFromUrl(split->currentUrl()) == rootA
               && searchIntParameter(split->currentUrl(), "scope", -1) == 1,
           "current-drive Search resolves the active right drive");
    waitForSearch(*split->searchController());
    verify(split->listView()->count() == 3, "right drive Search covers the whole drive fixture");
    window.m_searchEdit->setText("deep");
    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
    verify(searchBaseFromUrl(split->currentUrl()) == rootA
               && searchQueryFromUrl(split->currentUrl()) == "deep",
           "new query within right Search retains its base and scope");
    waitForSearch(*split->searchController());
    verify(split->listView()->count() == 1, "replacement right query displays fresh results");
    window.m_refreshAction->trigger();
    verify(split->searchController()->isRunning() && window.m_currentUrl == rootB,
           "shared Refresh restarts only the active right Search");
    waitForSearch(*split->searchController());

    // Two independent workers: late callbacks may never update the other pane.
    window.navigateTo(location, true);
    split->setCurrentUrl(makeSearchLocation("needle", 0, rootB, 0, 0, 0));
    auto *primaryWorker = window.m_searchController->m_searchJobs.first().data();
    QPointer<KIO::ListJob> rightWorker = split->searchController()->m_searchJobs.first();
    primaryWorker->entries(primaryWorker, {entry(a + "/needle.txt")});
    rightWorker->entries(rightWorker, {entry(b + "/needle-second.txt")});
    window.m_stopSearchAction->trigger();
    verify(!split->searchController()->isRunning() && window.m_searchController->isRunning()
               && split->listView()->count() == 1,
           "shared Stop cancels only right Search and preserves its partial results");
    verify(!window.m_stopSearchAction->isVisible() && !split->m_searchProgressFrame->isVisible(),
           "right cancellation hides only its progress and shared Stop");
    rightWorker->entries(rightWorker, {entry(b + "/stale.txt")});
    verify(split->searchController()->files().size() == 1,
           "late canceled right worker cannot contaminate results");
    window.setActivePane(Pane::Primary);
    verify(window.m_stopSearchAction->isVisible(), "switching left shows its still-running Stop action");
    window.m_stopSearchAction->trigger();
    verify(!window.m_searchController->isRunning() && window.m_directoryList->count() == 1,
           "shared Stop works symmetrically on left Search");
    window.setActivePane(Pane::Split);
    split->setCurrentUrl(rightSearch);
    window.navigateTo(location, true);
    window.m_cancelSearchButton->click();
    verify(!window.m_searchController->isRunning() && split->searchController()->isRunning()
               && window.m_stopSearchAction->isVisible(),
           "left inline Stop keeps right worker and shared Stop intact");
    split->m_cancelSearchButton->click();
    verify(!split->searchController()->isRunning(), "right inline Stop cancels its own worker");
    // Left completion while right runs must not hide the shared Stop action.
    split->setCurrentUrl(rightSearch);
    window.navigateTo(location, true);
    window.m_searchController->start("needle", {}, false);
    verify(split->searchController()->isRunning() && window.m_stopSearchAction->isVisible(),
           "inactive left completion cannot hide active right Stop");
    const QUrl savedRight = split->currentUrl();
    window.setSplitViewEnabled(false);
    verify(!split->searchController()->isRunning() && window.m_activePane == Pane::Primary,
           "closing Split View cancels the hidden right worker");
    window.setSplitViewEnabled(true);
    window.setActivePane(Pane::Split);
    verify(split->currentUrl() == savedRight && split->searchController()->isRunning(),
           "reopening Split View restores the right Search URL instead of its base folder");
    waitForSearch(*split->searchController());
    verify(split->listView()->count() == 3, "restored right Search reloads results");
    choose(window.m_searchTypeGroup, 3);
    choose(window.m_searchDateGroup, 1);
    choose(window.m_searchSizeGroup, 1);
    verify(split->listView()->count() == 2
               && searchIntParameter(split->currentUrl(), "date", 0) == 1
               && searchIntParameter(split->currentUrl(), "size", 0) == 1,
           "right type/date/size filters combine and persist in the address");
    window.m_searchFilterButton->menu()->actions().last()->trigger();
    verify(split->listView()->count() == 3 && split->searchState().type == 0
               && split->searchState().date == 0 && split->searchState().size == 0,
           "Clear filters resets only active right Search filters");
    const QUrl leftBeforeCrumb = window.m_currentUrl;
    QTest::mouseClick(split->m_thisPcCrumb, Qt::LeftButton);
    verify(split->currentUrl() == kThisPcUrl && window.m_currentUrl == leftBeforeCrumb
               && window.m_searchEdit->placeholderText() == "Search this computer",
           "right Search This PC breadcrumb navigates right and updates shared Search state");
    // Restrict whole-computer Search to disposable roots in this test.
    if (window.m_driveJob) { window.m_driveJob->kill(); window.m_driveJob = nullptr; }
    window.m_drives = {driveA, driveB};
    window.m_searchEdit->setText("needle");
    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
    verify(searchIntParameter(split->currentUrl(), "scope", -1) == 2
               && !searchBaseFromUrl(split->currentUrl()).isValid()
               && split->searchController()->totalRoots() == 2,
           "right This PC Search uses whole-computer scope and shared drive inventory");
    waitForSearch(*split->searchController());
    verify(split->listView()->count() == 4, "right whole-computer Search includes both fixture roots");
    window.m_upAction->trigger();
    verify(split->currentUrl() == kThisPcUrl, "whole-computer Search Up returns to right This PC");
    qInfo("PASS: %d search assertions; real KIO workers, filters, cancellation, stale signals, split/view integration", checks);
}
