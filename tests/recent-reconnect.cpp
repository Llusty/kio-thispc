/* SPDX-License-Identifier: MIT */
static int checks = 0;
static void verify(bool condition, const char *message)
{ if (!condition) qFatal("FAIL: %s", message); ++checks; }

static DirectoryListingCore &core(ThisPcWindow &w, bool split)
{ return split ? w.m_splitPane->m_listingCore : w.m_primaryPane->listingCoreForTesting(); }
static QUrl current(ThisPcWindow &w, bool split)
{ return split ? w.m_splitPane->currentUrl() : w.m_navigation.currentUrl(); }
static NavigationHistory::Snapshot history(ThisPcWindow &w, bool split)
{ return split ? w.m_splitPane->navigationSnapshot() : w.m_navigation.snapshot(); }
static RemoteReconnectBar *notice(ThisPcWindow &w, bool split)
{ return split ? w.m_splitPane->m_reconnect : w.m_primaryReconnect; }
static void navigate(ThisPcWindow &w, bool split, const QUrl &url)
{ if (split) w.m_splitPane->setCurrentUrl(url, true); else w.navigateTo(url, true); }
static void cancel(ThisPcWindow &w, bool split)
{ if (split) w.m_splitPane->cancelListing(); else w.m_primaryPane->cancelListing(); }
static void complete(ThisPcWindow &w, bool split, bool success)
{
    auto &listing = core(w, split);
    QPointer<KIO::ListJob> job = listing.listingJob();
    verify(job != nullptr, "production navigation creates a KIO ListJob");
    if (success) {
        KIO::UDSEntry entry;
        entry.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("confirmed.txt"));
        entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG);
        DirectoryListingCore::appendEntries(const_cast<QList<FileInfo>&>(listing.stagedFiles()),
            job->url(), {entry}, {});
        // Deliver the real result callback without relying on a live server.
        listing.slotJobFinished(job);
        if (job) job->kill(KJob::Quietly);
    } else {
        job->kill(KJob::EmitResult);
    }
}
static void waitForListing(ThisPcWindow &w, bool split)
{
    QElapsedTimer timer;
    timer.start();
    while (core(w, split).isLoading() && timer.elapsed() < 5000) QTest::qWait(10);
    verify(!core(w, split).isLoading(), "real local listing reaches terminal state");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-stage4-test");
    QCoreApplication::setApplicationName("recent-reconnect");
    QSettings().clear();
    QTemporaryDir files;
    verify(files.isValid(), "disposable local fixture");
    QFile payload(files.filePath("retained.txt"));
    verify(payload.open(QIODevice::WriteOnly) && payload.write("retained") == 8,
           "retained listing contains an actual disposable file");
    payload.close();
    const QUrl local = QUrl::fromLocalFile(files.path());
    const QUrl remote("sftp://alice@example.test:2222/docs");
    const QUrl unavailable("sftp://alice@example.test:2222/unavailable");

    // Existing storage, not a replacement MRU implementation.
    {
        SidebarPanel sidebar;
        for (int i = 0; i < 13; ++i)
            sidebar.recordRecentLocation(QUrl::fromLocalFile(files.filePath(QString::number(i))));
        verify(sidebar.m_recentLocationUrls.size() == 10, "MRU limit is ten");
        verify(sidebar.m_recentLocationUrls.first().path().endsWith("/12"), "newest first");
        verify(sidebar.m_recentLocationUrls.last().path().endsWith("/3"), "oldest entries evicted");
        const QUrl existing = sidebar.m_recentLocationUrls.at(5);
        sidebar.recordRecentLocation(existing);
        verify(sidebar.m_recentLocationUrls.size() == 10, "promotion adds no duplicate");
        verify(sidebar.m_recentLocationUrls.first() == existing, "duplicate promoted to front");
        const auto before = sidebar.m_recentLocationUrls;
        sidebar.recordRecentLocation(kThisPcUrl);
        sidebar.recordRecentLocation(makeSearchLocation("x", 0, local, 0, 0, 0));
        verify(sidebar.m_recentLocationUrls == before, "virtual Home and Search excluded");
        SidebarPanel reload;
        verify(reload.m_recentLocationUrls == before, "MRU persists in existing settings");
        sidebar.recordRecentLocation(QUrl("sftp://alice:SECRET@example.test:2222/docs/"));
        verify(sidebar.m_recentLocationUrls.first() == remote, "remote normalization removes password and slash");
    }

    for (bool split : {false, true}) {
        QSettings().remove("quickAccess/recentLocations");
        ThisPcWindow w(kThisPcUrl, false);
        w.show();
        w.setSplitViewEnabled(true);
        w.m_primaryPane->cancelListing();
        w.m_splitPane->cancelListing();
        verify(w.m_sidebar->m_recentLocationUrls.isEmpty(), "no recent on Home");
        navigate(w, split, local);
        verify(w.m_sidebar->m_recentLocationUrls.isEmpty(), "requested local URL is not recent yet");
        waitForListing(w, split);
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>{local}, "successful real local listing enters Recent");

        const QUrl missing = QUrl::fromLocalFile(files.filePath("missing"));
        navigate(w, split, missing);
        waitForListing(w, split);
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>{local}, "failed real local listing leaves MRU untouched");
        navigate(w, split, local);
        cancel(w, split);
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>{local}, "canceled local listing leaves MRU untouched");
        navigate(w, split, local);
        waitForListing(w, split);

        const auto before = history(w, split);
        const auto previousFiles = core(w, split).files();
        const auto savedCount = SavedRemoteLocationsStore::instance().count();
        SavedRemoteLocationsStore::instance().addLocation("Saved", unavailable);
        const auto savedRemote = SavedRemoteLocationsStore::instance().locationForUrl(unavailable);
        verify(savedRemote.isValid(), "saved connection fixture exists");
        verify(!previousFiles.isEmpty() || QDir(files.path()).isEmpty(), "retained listing is real local state");
        navigate(w, split, unavailable);
        verify(current(w, split) == local, "pending remote retains committed URL");
        verify(history(w, split).history == before.history, "pending remote retains history");
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>{local}, "pending remote does not enter Recent");
        complete(w, split, false);
        verify(current(w, split) == local, "failed remote rolls back URL");
        verify(history(w, split).history == before.history, "failed remote preserves history");
        verify(core(w, split).files().size() == previousFiles.size(), "failed remote preserves listing");
        verify(core(w, split).files().first().name == previousFiles.first().name
            && core(w, split).files().first().url == previousFiles.first().url,
            "failed remote preserves actual previous listing entry");
        verify(SavedRemoteLocationsStore::instance().locationForUrl(unavailable).id == savedRemote.id,
               "failed navigation leaves Saved Remote unchanged");
        verify(!notice(w, split)->isHidden(), "failed remote exposes reconnect even on retained view");
        verify(notice(w, split)->targetUrl() == unavailable, "reconnect retains failed sanitized target");
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>{local}, "failed remote leaves MRU unchanged");
        notice(w, split)->retryButton()->click();
        verify(core(w, split).listingJob() != nullptr, "error retry dispatches ordinary KIO navigation");
        complete(w, split, false);
        verify(current(w, split) == local, "failed retry preserves committed URL");
        verify(history(w, split).history == before.history, "failed retry does not add history");
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>{local}, "failed retry does not promote Recent");
        notice(w, split)->retryButton()->click();
        cancel(w, split);
        verify(current(w, split) == local, "canceled retry retains URL");
        verify(!core(w, split).isLoading(), "canceled retry reaches terminal state");
        verify(!notice(w, split)->isHidden(), "canceled retry remains reconnectable");
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>{local}, "canceled retry leaves MRU unchanged");

        navigate(w, split, remote);
        complete(w, split, true);
        verify(current(w, split) == remote, "successful remote commits URL");
        verify(core(w, split).files().first().name == "confirmed.txt", "real success callback publishes staged listing");
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>({remote, local}), "successful remote enters MRU first");
        verify(notice(w, split)->isHidden(), "successful navigation hides error notice");
        const auto confirmed = history(w, split);
        navigate(w, split, local);
        cancel(w, split);
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>({remote, local}),
               "canceled existing local listing does not promote its Recent entry");
        navigate(w, split, local);
        waitForListing(w, split);
        const auto localFirst = w.m_sidebar->m_recentLocationUrls;
        navigate(w, split, remote);
        complete(w, split, false);
        verify(w.m_sidebar->m_recentLocationUrls == localFirst, "failed reconnect does not promote existing entry");
        notice(w, split)->retryButton()->click();
        complete(w, split, true);
        verify(w.m_sidebar->m_recentLocationUrls == QList<QUrl>({remote, local}), "successful retry promotes without duplicates");
        verify(history(w, split).history.last() == remote, "successful retry uses ordinary history semantics");
        const auto lastHistory = history(w, split);
        navigate(w, split, remote);
        complete(w, split, true);
        verify(history(w, split).history == lastHistory.history, "same-location reconnect does not duplicate history");
        verify(w.m_sidebar->m_recentLocationUrls.size() == 2, "same-location reconnect does not duplicate Recent");

        // Context action routes via SidebarPanel to the active pane.
        w.setActivePane(split ? PaneId::Split : PaneId::Primary);
        navigate(w, split, local);
        waitForListing(w, split);
        SidebarButton *button = nullptr;
        for (int i = 0; i < w.m_sidebar->m_recentLocationsLayout->count(); ++i) {
            auto *candidate = qobject_cast<SidebarButton *>(w.m_sidebar->m_recentLocationsLayout->itemAt(i)->widget());
            if (candidate && sameLocation(candidate->url(), remote)) button = candidate;
        }
        verify(button != nullptr, "remote Recent UI entry exists");
        bool foundReconnect = false;
        QTimer::singleShot(0, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (auto *action : menu->actions()) {
                if (action->text() == trLocal("Połącz ponownie", "Reconnect")) {
                    foundReconnect = true;
                    menu->setActiveAction(action);
                    QTest::keyClick(menu, Qt::Key_Return);
                    return;
                }
            }
            menu->close();
        });
        QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, QPoint(2, 2), button->mapToGlobal(QPoint(2, 2)));
        QApplication::sendEvent(button, &menuEvent);
        verify(foundReconnect, "remote Recent context menu contains Reconnect");
        verify(core(w, split).isLoading(), "Recent Reconnect routes to active pane ordinary navigation");
        complete(w, split, true);
        verify(current(w, split) == remote, "Recent Reconnect commits on success");
        verify(w.m_sidebar->m_recentLocationUrls.size() == 2, "Recent menu retry adds no duplicate");
        SavedRemoteLocationsStore::instance().removeLocation(savedRemote.id);
        verify(SavedRemoteLocationsStore::instance().count() == savedCount,
               "isolated saved fixture cleaned without affecting other entries");
    }
    qInfo("PASS: %d Recent/Reconnect assertions", checks);
    return 0;
}
