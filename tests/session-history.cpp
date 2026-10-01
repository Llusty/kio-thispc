/* SPDX-License-Identifier: MIT */
static int checks = 0;
static void verify(bool condition, const char *message)
{ if (!condition) qFatal("FAIL: %s", message); ++checks; }

static void complete(DirectoryListingCore &core, bool success)
{
    QPointer<KIO::ListJob> job = core.listingJob();
    verify(job != nullptr, "restore dispatches ordinary KIO ListJob");
    if (success) {
        KIO::UDSEntry entry;
        entry.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("restored.txt"));
        entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG);
        DirectoryListingCore::appendEntries(const_cast<QList<FileInfo>&>(core.stagedFiles()),
                                            job->url(), {entry}, {});
        core.slotJobFinished(job);
        if (job) job->kill(KJob::Quietly);
    } else job->kill(KJob::EmitResult);
}
static QByteArray configBytes()
{
    QSettings settings;
    settings.sync();
    QFile file(settings.fileName());
    verify(file.open(QIODevice::ReadOnly), "isolated configuration can be inspected");
    return file.readAll();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-stage4-test");
    QCoreApplication::setApplicationName("session-history");
    QSettings().clear();
    const QUrl legacy("sftp://alice:SECRET@example.test:2222/docs?q=1#section");
    const QUrl remote("sftp://alice@example.test:2222/docs?q=1#section");
    const QUrl next("sftp://alice@example.test:2222/next");
    QTemporaryDir files;
    verify(files.isValid(), "isolated local session fallback");
    const QUrl local = QUrl::fromLocalFile(files.path());
    const QUrl local2 = QUrl::fromLocalFile(files.filePath("child"));
    verify(QDir().mkpath(local2.toLocalFile()), "second local session folder exists");

    // Seed legacy bytes directly. Never use a real credential.
    {
        QSettings settings;
        settings.setValue("quickAccess/recentLocations", QStringList{legacy.toString(), remote.toString()});
        settings.setValue("quickAccess/favorites", QStringList{legacy.toString()});
        settings.setValue("unrelated/keep", 47);
        settings.beginWriteArray("session/tabs", 1);
        settings.setArrayIndex(0);
        settings.setValue("currentUrl", legacy.toString());
        settings.setValue("history", QStringList{local.toString(), legacy.toString(), next.toString()});
        settings.setValue("historyIndex", 1);
        settings.setValue("splitEnabled", true);
        settings.setValue("splitUrl", legacy.toString());
        settings.setValue("splitHistory", QStringList{local.toString(), legacy.toString(), next.toString()});
        settings.setValue("splitHistoryIndex", 1);
        settings.endArray();
    }
    {
        SidebarPanel sidebar;
        verify(sidebar.m_recentLocationUrls == QList<QUrl>{remote}, "legacy Recent sanitized and deduplicated");
        verify(sidebar.m_quickAccessUrls == QList<QUrl>{remote}, "legacy favorites cannot leak through clipboard");
        auto loaded = SessionManager::load(0, true);
        verify(loaded.tabs.size() == 1, "legacy session loads");
        const auto &tab = loaded.tabs.first();
        verify(tab.currentUrl == remote, "current remote URL sanitized");
        verify(tab.history == QList<QUrl>({local, remote, next}), "Primary history sanitized preserving order");
        verify(tab.historyIndex == 1, "Primary index preserved");
        verify(tab.splitUrl == remote, "Split current URL sanitized");
        verify(tab.splitHistory == QList<QUrl>({local, remote, next}), "Split history sanitized preserving order");
        verify(tab.splitHistoryIndex == 1, "Split index preserved");
        verify(tab.currentUrl.userName() == "alice" && tab.currentUrl.port() == 2222,
               "username and port preserved");
        verify(tab.currentUrl.query() == "q=1" && tab.currentUrl.fragment() == "section",
               "existing query and fragment semantics preserved");
        verify(!configBytes().contains("SECRET"), "SECRET absent from all migrated settings bytes");
        verify(QSettings().value("unrelated/keep").toInt() == 47, "migration retains unrelated settings");
        const auto bytes = configBytes();
        SessionManager::load(0, true);
        SidebarPanel again;
        verify(configBytes() == bytes, "sanitized read is idempotent");
    }

    NavigationHistory nav;
    nav.navigate(legacy);
    verify(nav.currentUrl() == remote && nav.history().first() == remote, "history sanitizes direct navigation input");
    nav.updateCurrent(legacy, true);
    verify(nav.currentUrl().password().isEmpty() && nav.history().first().password().isEmpty(),
           "history sanitizes direct current-entry update");
    nav.restore({legacy, {local, legacy, next}, 1});
    verify(nav.currentUrl() == remote && nav.history().at(1) == remote, "history sanitizes snapshots");
    verify(!urlForDisplay(legacy).contains("SECRET"), "display path cannot expose legacy password");
    TabController controller;
    const int tabIndex = controller.create(legacy, 0, true);
    verify(controller.tabs().at(tabIndex).currentUrl == remote, "new tab sanitizes current URL");
    verify(controller.tabs().at(tabIndex).splitHistory == QList<QUrl>{remote}, "new tab initializes sanitized Split history");

    SessionSnapshot saved;
    TabState state;
    state.currentUrl = local;
    state.history = {kThisPcUrl, local, local2};
    state.historyIndex = 1;
    state.splitEnabled = true;
    state.splitUrl = local;
    state.splitHistory = {kThisPcUrl, local, local2};
    state.splitHistoryIndex = 1;
    saved.tabs = {state};
    saved.splitPaneActive = true;
    SessionManager::save(saved);
    {
        const auto loaded = SessionManager::load(0, true);
        verify(loaded.tabs.first().history == state.history, "Primary back/forward stack persists unchanged");
        verify(loaded.tabs.first().historyIndex == 1, "Primary index persists unchanged");
        verify(loaded.tabs.first().splitHistory == state.splitHistory, "Split back/forward stack persists");
        verify(loaded.tabs.first().splitHistoryIndex == 1, "Split index persists");
        ThisPcWindow w(kThisPcUrl, true);
        w.show();
        QApplication::processEvents();
        verify(w.m_activePane == PaneId::Split, "restored active Split resolves after window display");
        verify(w.m_navigation.currentUrl() == local, "local Primary session fallback remains usable");
        verify(w.m_splitPane->navigationSnapshot().history == state.splitHistory, "Split restored stack reaches pane");
        verify(w.m_splitPane->canGoBack() && w.m_splitPane->canGoForward(), "Split restored Back and Forward enabled");
        w.m_splitPane->navigateForward();
        verify(w.m_splitPane->currentUrl() == local2, "restored Split Forward reaches exact target");
        w.m_splitPane->navigateBack();
        verify(w.m_splitPane->currentUrl() == local, "restored Split Back reaches exact target");
        w.saveSessionState();
        verify(SessionManager::load(0, true).tabs.first().splitHistory == state.splitHistory,
               "saving restored pane preserves forward stack");
        w.setSplitViewEnabled(false);
        w.setSplitViewEnabled(true);
        verify(w.m_splitPane->navigationSnapshot().history == state.splitHistory,
               "Split toggle preserves restored history");
        w.createNewTab(local2, true);
        w.setSplitViewEnabled(true);
        const auto other = w.m_splitPane->navigationSnapshot();
        w.switchToTab(0);
        verify(w.m_splitPane->navigationSnapshot().history == state.splitHistory,
               "switching tabs restores first Split history independently");
        w.setSplitViewEnabled(false);
        w.switchToTab(1);
        w.switchToTab(0);
        w.setSplitViewEnabled(true);
        verify(w.m_splitPane->navigationSnapshot().history == state.splitHistory,
               "hidden Split history survives a visit to another tab");
        w.switchToTab(1);
        verify(w.m_splitPane->navigationSnapshot().history == other.history,
               "other tab retains its own Split history");
    }

    for (bool split : {false, true}) {
        for (int outcome : {0, 1, 2}) { // success, result failure, explicit cancel
            QSettings().remove("quickAccess/recentLocations");
            state.currentUrl = split ? local : remote;
            state.history = split ? QList<QUrl>{local} : QList<QUrl>{local, remote, next};
            state.historyIndex = split ? 0 : 1;
            state.splitEnabled = split;
            state.splitUrl = split ? remote : local;
            state.splitHistory = split ? QList<QUrl>{local, remote, next} : QList<QUrl>{local};
            state.splitHistoryIndex = split ? 1 : 0;
            saved.tabs = {state};
            SessionManager::save(saved);
            ThisPcWindow w(kThisPcUrl, true);
            w.show();
            auto &listing = split ? w.m_splitPane->m_listingCore : w.m_primaryPane->listingCoreForTesting();
            auto snapshot = [&]() { return split ? w.m_splitPane->navigationSnapshot() : w.m_navigation.snapshot(); };
            auto *retry = split ? w.m_splitPane->m_reconnect : w.m_primaryReconnect;
            verify(snapshot().currentUrl == local, "pending restored remote retains safe local fallback");
            verify(!snapshot().history.contains(remote), "pending restored remote absent from committed history");
            verify(!w.m_sidebar->m_recentLocationUrls.contains(remote), "pending restored remote absent from Recent");
            verify(listing.isLoading(), "remote session is pending through normal listing path");
            if (outcome == 2) {
                if (split) w.m_splitPane->cancelListing(); else w.m_primaryPane->cancelListing();
            } else complete(listing, outcome == 0);
            if (outcome == 0) {
                verify(snapshot().currentUrl == remote, "successful remote restore commits target");
                verify(snapshot().history == QList<QUrl>({local, remote, next}), "successful restore preserves back/forward order");
                verify(snapshot().historyIndex == 1, "successful restore commits original history index");
                verify(w.m_sidebar->m_recentLocationUrls.contains(remote), "successful restored listing enters Recent");
                verify(retry->isHidden(), "successful restore has no stale error notice");
            } else {
                verify(snapshot().currentUrl == local, "failed/canceled restore retains safe fallback");
                verify(!snapshot().history.contains(remote), "failed/canceled restore adds no remote history entry");
                verify(!w.m_sidebar->m_recentLocationUrls.contains(remote), "failed/canceled restore adds no remote Recent");
                verify(!retry->isHidden(), "failed/canceled restore exposes persistent Reconnect");
                const QString status = split ? w.m_splitPane->m_status->text() : w.m_directoryStatus->text();
                verify(status != trLocal("Wczytywanie…", "Loading…"), "failed/canceled restore does not stay Loading");
                retry->retryButton()->click();
                complete(listing, true);
                verify(snapshot().currentUrl == remote, "retry of restored target commits successfully");
                verify(snapshot().history == QList<QUrl>({local, remote, next}), "retry restores desired original history");
                verify(snapshot().historyIndex == 1, "retry restores desired original index");
            }
            // Forward into a remote is also transactional.
            if (split) w.m_splitPane->navigateForward(); else QMetaObject::invokeMethod(&w, "goForward", Qt::DirectConnection);
            verify(snapshot().currentUrl == remote && snapshot().historyIndex == 1,
                   "remote Forward leaves committed state pending until success");
            complete(listing, false);
            verify(snapshot().currentUrl == remote && snapshot().historyIndex == 1,
                   "failed remote Forward preserves index and URL");
            retry->retryButton()->click();
            complete(listing, true);
            verify(snapshot().currentUrl == next && snapshot().historyIndex == 2,
                   "Forward retry commits exact original index");
            if (split) w.m_splitPane->navigateBack(); else QMetaObject::invokeMethod(&w, "goBack", Qt::DirectConnection);
            verify(snapshot().currentUrl == next && snapshot().historyIndex == 2,
                   "remote Back remains pending until success");
            complete(listing, true);
            verify(snapshot().currentUrl == remote && snapshot().historyIndex == 1,
                   "successful remote Back commits exact original index");
        }
    }

    // Defensive save path also strips secrets from caller-provided snapshots.
    state.currentUrl = legacy;
    state.history = {legacy};
    state.historyIndex = 0;
    state.splitUrl = legacy;
    state.splitHistory = {legacy};
    state.splitHistoryIndex = 0;
    saved.tabs = {state};
    SessionManager::save(saved);
    verify(!configBytes().contains("SECRET"), "session write sanitizes every URL field defensively");
    qInfo("PASS: %d Session/History assertions", checks);
    return 0;
}
