/*
 * DirectoryWatchController & ThisPcWindow Reentrancy Regression Suite
 * Stage 5.3.2G.3C.1
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "directorywatchcontroller.h"

#include <QApplication>
#include <QDir>
#include <QMetaObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static void settleWindow(ThisPcWindow &window)
{
    verify(QTest::qWaitFor([&] {
        const bool primaryDone = !window.m_primaryPane->listingJob();
        const bool splitDone = !window.m_splitPane || !window.m_splitPane->listingJob();
        return primaryDone && splitDone;
    }, 5000), "directory navigation settles");
    QCoreApplication::processEvents();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("thispc-dir-watch-reentrancy-test"));
    QCoreApplication::setApplicationName(QStringLiteral("directory-watch-reentrancy-test"));
    QSettings settings;
    settings.clear();

    QTemporaryDir rootTmp;
    verify(rootTmp.isValid(), "disposable root directory created");

    // =========================================================================
    // Part 1: Deterministic Unit Tests on DirectoryWatchController (Scenarios E, F, G, H, I, J)
    // =========================================================================

    // Scenario E & F: Callback removes current entry, other entries, or all entries during iteration
    {
        DirectoryWatchController ctl;
        const QString dir1 = rootTmp.filePath(QStringLiteral("unit-e-dir1"));
        const QString dir2 = rootTmp.filePath(QStringLiteral("unit-e-dir2"));
        const QString dir3 = rootTmp.filePath(QStringLiteral("unit-e-dir3"));
        verify(QDir().mkpath(dir1) && QDir().mkpath(dir2) && QDir().mkpath(dir3), "Unit E/F: dirs created");

        const int c1 = 1, c2 = 2, c3 = 3;
        ctl.setWatchedUrl(&c1, QUrl::fromLocalFile(dir1));
        ctl.setWatchedUrl(&c2, QUrl::fromLocalFile(dir2));
        ctl.setWatchedUrl(&c3, QUrl::fromLocalFile(dir3));
        verify(ctl.watchedDirectoryCount() == 3, "Unit E/F: 3 entries watched");

        int deletedSignals = 0;
        QObject::connect(&ctl, &DirectoryWatchController::directoryDeleted, &ctl, [&](const QUrl &) {
            ++deletedSignals;
            // Remove current entry AND another entry synchronously inside the callback
            ctl.removeConsumer(&c1);
            ctl.removeConsumer(&c3);
        });

        verify(QMetaObject::invokeMethod(&ctl, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, dir1)),
               "Unit E/F: invoke onKDirWatchDeleted(dir1)");
        verify(deletedSignals == 1, "Unit E/F: directoryDeleted emitted once");
        verify(!ctl.isWatched(QUrl::fromLocalFile(dir1)), "Unit E/F: dir1 unwatched");
        verify(ctl.isWatched(QUrl::fromLocalFile(dir2)), "Unit E/F: dir2 still watched");
        verify(!ctl.isWatched(QUrl::fromLocalFile(dir3)), "Unit E/F: dir3 removed by callback");

        // Also test removing ALL entries (clear) inside directoryDeleted
        QObject::disconnect(&ctl, &DirectoryWatchController::directoryDeleted, nullptr, nullptr);
        QObject::connect(&ctl, &DirectoryWatchController::directoryDeleted, &ctl, [&](const QUrl &) {
            ctl.clear();
        });
        verify(QMetaObject::invokeMethod(&ctl, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, dir2)),
               "Unit F-clear: invoke onKDirWatchDeleted(dir2)");
        verify(ctl.watchedDirectoryCount() == 0, "Unit F-clear: clear() inside signal leaves 0 entries safely");
    }

    // Scenario F2: Parent + Child both match onKDirWatchDeleted (parent dirChanged hits deadline & removes child)
    {
        DirectoryWatchController ctl;
        ctl.setDebounceConfig({10, 5, 0}); // Force immediate synchronous directoryChanged on deadline
        const QString parentDir = rootTmp.filePath(QStringLiteral("unit-f2-parent"));
        const QString childDir = parentDir + QStringLiteral("/child");
        verify(QDir().mkpath(childDir), "Unit F2: parent/child created");

        const int cParent = 10, cChild = 11;
        ctl.setWatchedUrl(&cParent, QUrl::fromLocalFile(parentDir));
        ctl.setWatchedUrl(&cChild, QUrl::fromLocalFile(childDir));

        int childDeletedCount = 0;
        QObject::connect(&ctl, &DirectoryWatchController::directoryChanged, &ctl, [&](const QUrl &u) {
            if (sameLocation(u, QUrl::fromLocalFile(parentDir))) {
                // Synchronously unsubscribe child during parent's dirty/changed emission!
                ctl.removeConsumer(&cChild);
            }
        });
        QObject::connect(&ctl, &DirectoryWatchController::directoryDeleted, &ctl, [&](const QUrl &u) {
            if (sameLocation(u, QUrl::fromLocalFile(childDir))) {
                ++childDeletedCount;
            }
        });

        verify(QMetaObject::invokeMethod(&ctl, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, childDir)),
               "Unit F2: invoke onKDirWatchDeleted(childDir)");
        verify(childDeletedCount == 0, "Unit F2: unsubscribed child does not emit stale directoryDeleted");
    }

    // Scenario G: Callback switches path and registers a new entry (RB-tree insert + erase during iteration)
    {
        DirectoryWatchController ctl;
        const QString dA = rootTmp.filePath(QStringLiteral("unit-g-aaa"));
        const QString dB = rootTmp.filePath(QStringLiteral("unit-g-bbb"));
        const QString dNew = rootTmp.filePath(QStringLiteral("unit-g-ccc-new"));
        verify(QDir().mkpath(dA) && QDir().mkpath(dB) && QDir().mkpath(dNew), "Unit G: dirs created");

        const int cA = 20, cB = 21;
        ctl.setWatchedUrl(&cA, QUrl::fromLocalFile(dA));
        ctl.setWatchedUrl(&cB, QUrl::fromLocalFile(dB));

        QObject::connect(&ctl, &DirectoryWatchController::directoryDeleted, &ctl, [&](const QUrl &) {
            ctl.setWatchedUrl(&cB, QUrl::fromLocalFile(dNew));
            ctl.setWatchedUrl(&cA, QUrl::fromLocalFile(dNew));
        });

        verify(QMetaObject::invokeMethod(&ctl, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, dB)),
               "Unit G: invoke onKDirWatchDeleted(dB)");
        verify(!ctl.isWatched(QUrl::fromLocalFile(dB)), "Unit G: dB removed");
        verify(ctl.isWatched(QUrl::fromLocalFile(dNew)) && ctl.refCount(QUrl::fromLocalFile(dNew)) == 2,
               "Unit G: new watch registered with refCount 2");
    }

    // Scenario I: Synchronous directoryChanged when maxDeadlineMs expires during dirty/created/checkAndSchedule
    {
        DirectoryWatchController ctl;
        ctl.setDebounceConfig({50, 10, 0}); // immediate deadline expiration
        const QString dir1 = rootTmp.filePath(QStringLiteral("unit-i-dir1"));
        const QString dir2 = rootTmp.filePath(QStringLiteral("unit-i-dir2"));
        verify(QDir().mkpath(dir1) && QDir().mkpath(dir2), "Unit I: dirs created");

        const int c1 = 30, c2 = 31;
        ctl.setWatchedUrl(&c1, QUrl::fromLocalFile(dir1));
        ctl.setWatchedUrl(&c2, QUrl::fromLocalFile(dir2));

        int changedCount = 0;
        QObject::connect(&ctl, &DirectoryWatchController::directoryChanged, &ctl, [&](const QUrl &u) {
            ++changedCount;
            if (sameLocation(u, QUrl::fromLocalFile(dir1))) {
                ctl.removeConsumer(&c1);
                ctl.setWatchedUrl(&c2, QUrl::fromLocalFile(rootTmp.path()));
            }
        });

        verify(QMetaObject::invokeMethod(&ctl, "onKDirWatchDirty", Qt::DirectConnection,
                                         Q_ARG(QString, dir1 + QStringLiteral("/file.txt"))),
               "Unit I: onKDirWatchDirty with expired deadline");
        verify(QMetaObject::invokeMethod(&ctl, "onKDirWatchCreated", Qt::DirectConnection,
                                         Q_ARG(QString, rootTmp.path())),
               "Unit I: onKDirWatchCreated with expired deadline");
        ctl.scheduleRefresh(QUrl::fromLocalFile(rootTmp.path()));
        verify(changedCount >= 2, "Unit I: synchronous directoryChanged handled cleanly without UAF");
    }

    // Scenario J: Delayed and repeated events / timer timeout for removed or re-added watcher
    {
        DirectoryWatchController ctl;
        ctl.setDebounceConfig({30, 10, 500});
        const QString dirJ = rootTmp.filePath(QStringLiteral("unit-j-dir"));
        verify(QDir().mkpath(dirJ), "Unit J: dir created");

        const int cJ = 40;
        ctl.setWatchedUrl(&cJ, QUrl::fromLocalFile(dirJ));
        ctl.scheduleRefresh(QUrl::fromLocalFile(dirJ)); // arms timer for generation 1

        // Remove and immediately re-add same directory before timer fires (generation 2)
        ctl.removeConsumer(&cJ);
        ctl.setWatchedUrl(&cJ, QUrl::fromLocalFile(dirJ));

        int changedCount = 0;
        QObject::connect(&ctl, &DirectoryWatchController::directoryChanged, &ctl, [&](const QUrl &) {
            ++changedCount;
            // Remove consumer synchronously inside timer timeout signal!
            ctl.removeConsumer(&cJ);
        });

        QTest::qWait(80);
        verify(changedCount == 0, "Unit J: stale timer from removed generation did not fire");

        // Now schedule on generation 2 and let timeout fire & synchronously remove consumer inside slot
        ctl.scheduleRefresh(QUrl::fromLocalFile(dirJ));
        verify(QTest::qWaitFor([&] { return changedCount == 1; }, 1000),
               "Unit J: active generation timer fired cleanly and survived synchronous removeConsumer");
        verify(!ctl.isWatched(QUrl::fromLocalFile(dirJ)), "Unit J: entry cleanly removed inside timer callback");
    }

    // =========================================================================
    // Part 2: Full ThisPcWindow Split View Integration Tests (Scenarios A, B, C, D, H, K, L)
    // =========================================================================
    {
        const QString base = rootTmp.filePath(QStringLiteral("gui-split"));
        const QString dirA = base + QStringLiteral("/alpha-primary");
        const QString dirB = base + QStringLiteral("/beta-split");
        const QString dirShared = base + QStringLiteral("/shared-both");
        const QString dirParent = base + QStringLiteral("/nested-parent");
        const QString dirChild = dirParent + QStringLiteral("/nested-child");
        const QString dirRenameOld = base + QStringLiteral("/rename-old");
        const QString dirRenameNew = base + QStringLiteral("/rename-new");
        for (const QString &d : {dirA, dirB, dirShared, dirChild, dirRenameOld}) {
            verify(QDir().mkpath(d), "GUI fixtures created");
        }

        ThisPcWindow window(QUrl::fromLocalFile(dirA), false);
        window.resize(1280, 720);
        window.show();
        window.setSplitViewEnabled(true);
        window.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(dirB), true);
        settleWindow(window);

        DirectoryWatchController *wc = window.watchControllerForTesting();
        verify(wc != nullptr, "GUI: DirectoryWatchController exists");
        verify(wc->isWatched(QUrl::fromLocalFile(dirA)) && wc->isWatched(QUrl::fromLocalFile(dirB)),
               "GUI: both Primary (dirA) and Split (dirB) are watched");

        // Scenario B: Delete Split View directory while Primary stays active (exact crash scenario!)
        verify(QDir().rmdir(dirB), "Scenario B: rmdir dirB");
        QMetaObject::invokeMethod(wc, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, dirB));
        settleWindow(window);
        verify(sameLocation(window.m_splitPane->currentUrl(), kThisPcUrl),
               "Scenario B: Split pane navigated to thispc:/ cleanly after deletion");
        verify(sameLocation(window.m_navigation.currentUrl(), QUrl::fromLocalFile(dirA)),
               "Scenario B: Primary pane stayed intact on dirA");
        verify(!wc->isWatched(QUrl::fromLocalFile(dirB)), "Scenario B: dirB unwatched");

        // Scenario A: Delete Primary directory -> falls back to existing parent (base)
        verify(QDir().rmdir(dirA), "Scenario A: rmdir dirA");
        QMetaObject::invokeMethod(wc, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, dirA));
        settleWindow(window);
        verify(sameLocation(window.m_navigation.currentUrl(), QUrl::fromLocalFile(base)),
               "Scenario A: Primary pane safely fell back to parent directory");
        verify(!wc->isWatched(QUrl::fromLocalFile(dirA)), "Scenario A: dirA unwatched");

        // Scenario C: Rapid sequential deletion of both Primary and Split directories
        const QString dirC1 = base + QStringLiteral("/c-primary");
        const QString dirC2 = base + QStringLiteral("/c-split");
        verify(QDir().mkpath(dirC1) && QDir().mkpath(dirC2), "Scenario C: dirs created");
        window.navigateTo(QUrl::fromLocalFile(dirC1), true);
        window.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(dirC2), true);
        settleWindow(window);

        verify(QDir().rmdir(dirC2) && QDir().rmdir(dirC1), "Scenario C: rmdir both");
        QMetaObject::invokeMethod(wc, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, dirC2));
        QMetaObject::invokeMethod(wc, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, dirC1));
        settleWindow(window);
        verify(sameLocation(window.m_navigation.currentUrl(), QUrl::fromLocalFile(base)),
               "Scenario C: Primary fell back cleanly to base");
        verify(sameLocation(window.m_splitPane->currentUrl(), kThisPcUrl),
               "Scenario C: Split fell back cleanly to thispc:/");

        // Scenario D: Primary and Split watch the SAME directory, then deleted
        window.navigateTo(QUrl::fromLocalFile(dirShared), true);
        window.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(dirShared), true);
        settleWindow(window);
        verify(wc->refCount(QUrl::fromLocalFile(dirShared)) == 2, "Scenario D: refCount == 2 for shared watched dir");

        verify(QDir().rmdir(dirShared), "Scenario D: rmdir dirShared");
        QMetaObject::invokeMethod(wc, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, dirShared));
        settleWindow(window);
        verify(!wc->isWatched(QUrl::fromLocalFile(dirShared)), "Scenario D: dirShared cleanly unwatched");
        verify(sameLocation(window.m_navigation.currentUrl(), QUrl::fromLocalFile(base)),
               "Scenario D: Primary fell back to parent");
        verify(sameLocation(window.m_splitPane->currentUrl(), kThisPcUrl),
               "Scenario D: Split fell back to thispc:/");

        // Scenario H: External rename of watched directory opened in both Primary and Split
        window.navigateTo(QUrl::fromLocalFile(dirRenameOld), true);
        window.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(dirRenameOld), true);
        settleWindow(window);
        verify(QDir().rename(dirRenameOld, dirRenameNew), "Scenario H: rename dir on disk");
        wc->handleFileRenamed(QUrl::fromLocalFile(dirRenameOld).toString(),
                              QUrl::fromLocalFile(dirRenameNew).toString());
        settleWindow(window);
        verify(sameLocation(window.m_navigation.currentUrl(), QUrl::fromLocalFile(dirRenameNew)),
               "Scenario H: Primary followed renamed directory");
        verify(sameLocation(window.m_splitPane->currentUrl(), QUrl::fromLocalFile(dirRenameNew)),
               "Scenario H: Split followed renamed directory");
        verify(!wc->isWatched(QUrl::fromLocalFile(dirRenameOld)), "Scenario H: old dir unwatched");
        verify(wc->isWatched(QUrl::fromLocalFile(dirRenameNew)), "Scenario H: new dir watched");

        // Scenario K: Deletion of nested child AND its parent directory -> safe fallback all the way up
        window.navigateTo(QUrl::fromLocalFile(dirChild), true);
        window.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(base), true);
        settleWindow(window);
        verify(QDir().rmdir(dirChild) && QDir().rmdir(dirParent), "Scenario K: removed both child and parent");
        QMetaObject::invokeMethod(wc, "onKDirWatchDeleted", Qt::DirectConnection, Q_ARG(QString, dirChild));
        settleWindow(window);
        // Because dirParent no longer exists on disk, Primary skips missing parent and falls back safely to kThisPcUrl
        verify(sameLocation(window.m_navigation.currentUrl(), kThisPcUrl),
               "Scenario K: missing parent fallback safely transitions Primary to thispc:/");

        // Unit Scenario H: handleFileRenamed/handleFileMoved synchronous reentrancy on directoryRenamed
        {
            DirectoryWatchController rctl;
            const QString rOld = rootTmp.filePath(QStringLiteral("unit-h-old"));
            const QString rNew = rootTmp.filePath(QStringLiteral("unit-h-new"));
            const QString rOther = rootTmp.filePath(QStringLiteral("unit-h-other"));
            verify(QDir().mkpath(rOld) && QDir().mkpath(rNew) && QDir().mkpath(rOther), "Unit H: dirs created");
            const int rc1 = 50, rc2 = 51;
            rctl.setWatchedUrl(&rc1, QUrl::fromLocalFile(rOld));
            rctl.setWatchedUrl(&rc2, QUrl::fromLocalFile(rOther));
            int renamedSignals = 0;
            QObject::connect(&rctl, &DirectoryWatchController::directoryRenamed, &rctl,
                             [&](const QUrl &, const QUrl &dst) {
                ++renamedSignals;
                rctl.setWatchedUrl(&rc1, dst);
                rctl.removeConsumer(&rc2);
            });
            rctl.handleFileRenamed(QUrl::fromLocalFile(rOld).toString(), QUrl::fromLocalFile(rNew).toString());
            verify(renamedSignals == 1, "Unit H: directoryRenamed fired once");
            verify(!rctl.isWatched(QUrl::fromLocalFile(rOld)), "Unit H: old removed");
            verify(rctl.isWatched(QUrl::fromLocalFile(rNew)), "Unit H: new watched");
            verify(!rctl.isWatched(QUrl::fromLocalFile(rOther)), "Unit H: other safely removed in callback");
        }

        // Scenario L: Continued automatic refresh after location fallback/transition
        window.navigateTo(QUrl::fromLocalFile(base), true);
        settleWindow(window);
        verify(wc->isWatched(QUrl::fromLocalFile(base)), "Scenario L: fallback directory base is actively watched");
        QSignalSpy spyRefresh(wc, &DirectoryWatchController::directoryChanged);
        QFile refreshProbe(base + QStringLiteral("/after-recovery-probe.txt"));
        verify(refreshProbe.open(QIODevice::WriteOnly), "Scenario L: create probe file in recovered directory");
        refreshProbe.write("refresh");
        refreshProbe.close();
        verify(QTest::qWaitFor([&] { return spyRefresh.count() >= 1; }, 2000),
               "Scenario L: automatic directory refresh still works cleanly after deletion recovery");

        // Real inotify rmdir test (without manual onKDirWatchDeleted invocation)
        {
            const QString inotifyDir = base + QStringLiteral("/real-inotify-split");
            verify(QDir().mkpath(inotifyDir), "Real inotify: dir created");
            window.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(inotifyDir), true);
            settleWindow(window);
            verify(wc->isWatched(QUrl::fromLocalFile(inotifyDir)), "Real inotify: split dir watched");

            verify(QDir().rmdir(inotifyDir), "Real inotify: rmdir triggers real KDirWatch::deleted");
            verify(QTest::qWaitFor([&] {
                return sameLocation(window.m_splitPane->currentUrl(), kThisPcUrl)
                    && !wc->isWatched(QUrl::fromLocalFile(inotifyDir));
            }, 3000), "Real inotify: Split pane transitioned cleanly on real KDirWatch::deleted without crash");
        }
    }

    qInfo("PASS: %d DirectoryWatchController/Split reentrancy assertions", checks);
    return 0;
}
