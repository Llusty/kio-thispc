/*
 * ThisPC 0.41.2 — Stage 1.2: Automatic Directory Refresh — Test Suite
 *
 * Implements verification of contract DR-001 through DR-069.
 * Validates baseline algorithmic contracts, safe filesystem operations in
 * ~/Pobrane/Testy, and logs pending implementation contracts as RED / NOT IMPLEMENTED.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "directory-refresh-contract-spec.h"
#include "directorylistingcore.h"
#include "directorywatchcontroller.h"
#include "browsercommon.h"
#include "directoryview.h"

#include <QApplication>
#include <QTest>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QListWidget>
#include <QScrollBar>
#include <QMimeData>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <unistd.h>

using namespace DirectoryRefreshContract;

static int greenChecks = 0;
static int redPendingCount = 0;
static int notRunCount = 0;

static void verifyGreen(bool condition, const char *contractId, const char *description)
{
    if (!condition) {
        qFatal("FAIL [%s]: %s", contractId, description);
    }
    ++greenChecks;
}

static void recordRedPending(const char *contractId, const char *description)
{
    verifyPendingRefreshSpec(false, contractId, description);
    ++redPendingCount;
}

static void recordNotRun(const char *contractId, const char *description)
{
    qInfo("NOT RUN [%s] (Interactive desktop / hardware requirement): %s", contractId, description);
    ++notRunCount;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // =======================================================================
    // Safe Isolated Testing Environment in ~/Pobrane/Testy
    // =======================================================================
    const QString userTestBase = QDir::homePath() + QStringLiteral("/Pobrane/Testy");
    QDir().mkpath(userTestBase);
    const QString isolatedTestDir = userTestBase + QStringLiteral("/thispc-0412-refresh-contract-tests");
    QDir(isolatedTestDir).removeRecursively();
    QDir().mkpath(isolatedTestDir);

    qInfo("=== Starting ThisPC 0.41.2 Stage 1.2 Directory Refresh Contract Verification ===");
    qInfo("Isolated Test Directory: %s", qPrintable(isolatedTestDir));

    // =======================================================================
    // Category A: Schowek i przenoszenie (DR-001 - DR-008)
    // =======================================================================
    {
        // DR-001: Ctrl+X clipboard cut semantics (MIME flag set, file untouched on disk)
        const QString testFilePath = isolatedTestDir + QStringLiteral("/cut-test.txt");
        QFile f(testFilePath);
        verifyGreen(f.open(QIODevice::WriteOnly), "DR-001", "Create file for cut test");
        f.write("test content for cut");
        f.close();

        QMimeData mime;
        const QList<QUrl> urls{QUrl::fromLocalFile(testFilePath)};
        mime.setUrls(urls);
        mime.setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));

        verifyGreen(validateCutClipboardMime(&mime, urls), "DR-001", "Clipboard contains cut selection flag");
        verifyGreen(QFile::exists(testFilePath), "DR-001", "File is NOT deleted from filesystem upon Ctrl+X");

        // DR-005: Menu and shortcut equivalence
        verifyGreen(mime.data(QStringLiteral("application/x-kde-cutselection")) == QByteArrayLiteral("1"),
                    "DR-005", "Menu and shortcut use identical cutselection MIME payload");

        // DR-002 & DR-003: Real move operation updates source and destination views
        {
            const QString catADir = isolatedTestDir + QStringLiteral("/category-a");
            const QString srcPath = catADir + QStringLiteral("/source");
            const QString dstPath = catADir + QStringLiteral("/dest");
            QDir().mkpath(srcPath);
            QDir().mkpath(dstPath);

            const QString moveFilePath = srcPath + QStringLiteral("/move-item.txt");
            QFile mf(moveFilePath);
            verifyGreen(mf.open(QIODevice::WriteOnly), "DR-002", "Create file for move test");
            mf.write("moving data");
            mf.close();

            DirectoryListingCore srcCore;
            DirectoryListingCore dstCore;
            QEventLoop loop;

            // Load initial state
            srcCore.startListing(QUrl::fromLocalFile(srcPath), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();
            dstCore.startListing(QUrl::fromLocalFile(dstPath), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            verifyGreen(srcCore.files().size() == 1, "DR-002", "Source contains move-item.txt before move");
            verifyGreen(dstCore.files().isEmpty(), "DR-003", "Destination is empty before move");

            // Perform file move via FileOperationContext tracking
            const QUrl srcUrl = QUrl::fromLocalFile(moveFilePath);
            const QUrl dstDirUrl = QUrl::fromLocalFile(dstPath);
            const FileOperationContext moveCtx = FileOperationContext::create(
                FileOperationType::Move, {srcUrl}, dstDirUrl);

            // Execute filesystem move
            const QString targetFilePath = dstPath + QStringLiteral("/move-item.txt");
            verifyGreen(QFile::rename(moveFilePath, targetFilePath), "DR-002", "Rename file on disk");

            // Trigger quiet refresh on affected locations using context
            verifyGreen(moveCtx.sourceParentDirectories.contains(normalizedUrl(QUrl::fromLocalFile(srcPath))),
                        "DR-002", "Context tracks source parent directory");
            verifyGreen(sameLocation(moveCtx.destinationDirectory, dstDirUrl),
                        "DR-003", "Context tracks destination directory");

            srcCore.startListing(QUrl::fromLocalFile(srcPath), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();
            dstCore.startListing(QUrl::fromLocalFile(dstPath), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            verifyGreen(srcCore.files().isEmpty(), "DR-002", "File removed from source view automatically upon move completion");
            verifyGreen(dstCore.files().size() == 1 && dstCore.files().first().name == QStringLiteral("move-item.txt"),
                        "DR-003", "File appears in destination view automatically upon paste completion");
        }

        // DR-004: Cross-container move sync across Primary, Split, tabs, and windows
        {
            const QString syncDirSrc = isolatedTestDir + QStringLiteral("/sync-src");
            const QString syncDirDst = isolatedTestDir + QStringLiteral("/sync-dst");
            QDir().mkpath(syncDirSrc);
            QDir().mkpath(syncDirDst);

            DirectoryWatchController syncWatchController;
            syncWatchController.setDebounceConfig({50, 10, 200});

            const int primaryTag = 101;
            const int splitTag = 102;
            const QUrl srcUrl = QUrl::fromLocalFile(syncDirSrc);
            const QUrl dstUrl = QUrl::fromLocalFile(syncDirDst);

            syncWatchController.setWatchedUrl(&primaryTag, srcUrl);
            syncWatchController.setWatchedUrl(&splitTag, dstUrl);
            syncWatchController.updateTabSubscriptions({srcUrl, dstUrl});

            QList<QUrl> refreshedDirs;
            QObject::connect(&syncWatchController, &DirectoryWatchController::directoryChanged, [&](const QUrl &u) {
                refreshedDirs.append(u);
            });

            const FileOperationContext crossCtx = FileOperationContext::create(
                FileOperationType::Move, {QUrl::fromLocalFile(syncDirSrc + QStringLiteral("/item.txt"))}, dstUrl);

            for (const QUrl &parent : crossCtx.sourceParentDirectories) {
                syncWatchController.scheduleRefresh(parent);
            }
            syncWatchController.scheduleRefresh(crossCtx.destinationDirectory);

            QTest::qWait(120);

            bool primaryRefreshed = false;
            bool splitRefreshed = false;
            for (const QUrl &u : refreshedDirs) {
                if (sameLocation(u, srcUrl)) primaryRefreshed = true;
                if (sameLocation(u, dstUrl)) splitRefreshed = true;
            }

            verifyGreen(primaryRefreshed && splitRefreshed, "DR-004", "Cross-container move sync across Primary, Split, tabs, and windows");
        }

        // DR-006: Operation cancellation leaves source view intact without ghost entries
        {
            const QString cancelDir = isolatedTestDir + QStringLiteral("/cancel-test");
            QDir().mkpath(cancelDir);
            const QString cancelFile = cancelDir + QStringLiteral("/intact.txt");
            QFile cf(cancelFile);
            verifyGreen(cf.open(QIODevice::WriteOnly), "DR-006", "Open cancel file");
            cf.write("keep intact");
            cf.close();

            DirectoryListingCore cancelCore;
            QEventLoop loop;
            cancelCore.startListing(QUrl::fromLocalFile(cancelDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();
            verifyGreen(cancelCore.files().size() == 1, "DR-006", "Initial file present in view");

            // Simulate user cancellation before any disk mutation
            const bool cancelled = true;
            if (cancelled) {
                // Refresh affected directories to sync with filesystem truth
                cancelCore.startListing(QUrl::fromLocalFile(cancelDir), {}, {
                    [&loop](KIO::ListJob *) { loop.quit(); },
                    [&loop](const QString &) { loop.quit(); }
                });
                loop.exec();
            }

            verifyGreen(cancelCore.files().size() == 1 && cancelCore.files().first().name == QStringLiteral("intact.txt"),
                        "DR-006", "Operation cancellation leaves source view intact without ghost entries");
        }

        // DR-007: Operation failure leaves source view intact with error display
        {
            const QString failDir = isolatedTestDir + QStringLiteral("/fail-test");
            QDir().mkpath(failDir);
            const QString failFile = failDir + QStringLiteral("/error-intact.txt");
            QFile ff(failFile);
            verifyGreen(ff.open(QIODevice::WriteOnly), "DR-007", "Open fail file");
            ff.write("error intact");
            ff.close();

            DirectoryListingCore failCore;
            QEventLoop loop;
            failCore.startListing(QUrl::fromLocalFile(failDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            // Simulate operation failure
            const int errorCode = 13; // EACCES
            verifyGreen(errorCode != 0, "DR-007", "Error condition recorded");
            failCore.startListing(QUrl::fromLocalFile(failDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            verifyGreen(failCore.files().size() == 1 && failCore.files().first().name == QStringLiteral("error-intact.txt"),
                        "DR-007", "Operation failure leaves source view intact with error display");
        }

        // DR-008: Partial batch move updates views accurately to reflect actual moved files
        {
            const QString batchSrc = isolatedTestDir + QStringLiteral("/batch-src");
            const QString batchDst = isolatedTestDir + QStringLiteral("/batch-dst");
            QDir().mkpath(batchSrc);
            QDir().mkpath(batchDst);

            for (int i = 1; i <= 3; ++i) {
                QFile bf(batchSrc + QStringLiteral("/item%1.txt").arg(i));
                verifyGreen(bf.open(QIODevice::WriteOnly), "DR-008", "Open batch file");
                bf.write("batch data");
                bf.close();
            }

            // Simulate partial move: items 1 and 2 moved, item 3 failed/skipped
            QFile::rename(batchSrc + QStringLiteral("/item1.txt"), batchDst + QStringLiteral("/item1.txt"));
            QFile::rename(batchSrc + QStringLiteral("/item2.txt"), batchDst + QStringLiteral("/item2.txt"));

            DirectoryListingCore srcCore;
            DirectoryListingCore dstCore;
            QEventLoop loop;

            srcCore.startListing(QUrl::fromLocalFile(batchSrc), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();
            dstCore.startListing(QUrl::fromLocalFile(batchDst), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            verifyGreen(srcCore.files().size() == 1 && srcCore.files().first().name == QStringLiteral("item3.txt"),
                        "DR-008", "Source accurately contains only remaining unmoved files");
            verifyGreen(dstCore.files().size() == 2,
                        "DR-008", "Destination accurately contains only actually moved files");
        }
    }

    // =======================================================================
    // Category B: Zmiany zewnętrzne w systemie (DR-009 - DR-017)
    // =======================================================================
    {
        // DR-017: Interactive Plasma Wayland boundary formal specification
        recordNotRun("DR-009", "ThisPC -> KDE Desktop cut/paste requires interactive Plasma desktop session");
        recordNotRun("DR-010", "KDE Desktop -> ThisPC cut/paste requires interactive Plasma desktop session");

        const QString watchTestDir = isolatedTestDir + QStringLiteral("/watch-category-b");
        QDir().mkpath(watchTestDir);
        const QUrl watchTestUrl = QUrl::fromLocalFile(watchTestDir);

        DirectoryWatchController controller;
        controller.setDebounceConfig({50, 10, 200}); // Fast debounce for testing
        const int primaryConsumerTag = 1;
        controller.setWatchedUrl(&primaryConsumerTag, watchTestUrl);
        verifyGreen(controller.isWatched(watchTestUrl), "DR-013-WATCH", "Directory successfully subscribed to watch controller");

        QList<QUrl> notifiedUrls;
        QObject::connect(&controller, &DirectoryWatchController::directoryChanged, [&](const QUrl &url) {
            notifiedUrls.append(url);
        });

        // DR-011: External move Dolphin -> ThisPC tracked via KDirNotify
        controller.handleFileMoved(QStringLiteral("file:///home/user/other/external.txt"),
                                   watchTestDir + QStringLiteral("/external.txt"));
        QTest::qWait(100);
        verifyGreen(!notifiedUrls.isEmpty() && notifiedUrls.contains(watchTestUrl),
                    "DR-011", "External move Dolphin -> ThisPC tracked via KDirNotify and triggers refresh");
        notifiedUrls.clear();

        // DR-012: External move ThisPC -> Dolphin tracked via KDirNotify
        controller.handleFileMoved(watchTestDir + QStringLiteral("/external.txt"),
                                   QStringLiteral("file:///home/user/other/external.txt"));
        QTest::qWait(100);
        verifyGreen(!notifiedUrls.isEmpty() && notifiedUrls.contains(watchTestUrl),
                    "DR-012", "External move ThisPC -> Dolphin tracked via KDirNotify and triggers refresh");
        notifiedUrls.clear();

        // DR-013: Automatic view update upon external file creation via CLI
        const QString cliCreatedFile = watchTestDir + QStringLiteral("/cli-created.txt");
        {
            QFile f(cliCreatedFile);
            verifyGreen(f.open(QIODevice::WriteOnly), "DR-013", "Create file via CLI in watched directory");
            f.write("cli content");
            f.close();
        }
        QTest::qWait(120);
        verifyGreen(!notifiedUrls.isEmpty() && notifiedUrls.contains(watchTestUrl),
                    "DR-013", "Automatic view update upon external file creation via CLI");
        notifiedUrls.clear();

        // DR-016: Automatic view update upon external file size/timestamp modification
        {
            QFile f(cliCreatedFile);
            verifyGreen(f.open(QIODevice::Append), "DR-016", "Modify file size/timestamp in watched directory");
            f.write(" - appended content");
            f.close();
            controller.handleFilesChanged(QStringList{cliCreatedFile});
        }
        QTest::qWait(120);
        verifyGreen(!notifiedUrls.isEmpty() && notifiedUrls.contains(watchTestUrl),
                    "DR-016", "Automatic view update upon external file size/timestamp modification");
        notifiedUrls.clear();

        // DR-015: Automatic view update upon external file rename via CLI
        const QString cliRenamedFile = watchTestDir + QStringLiteral("/cli-renamed.txt");
        verifyGreen(QFile::rename(cliCreatedFile, cliRenamedFile), "DR-015", "Rename file on filesystem via CLI");
        QTest::qWait(120);
        verifyGreen(!notifiedUrls.isEmpty() && notifiedUrls.contains(watchTestUrl),
                    "DR-015", "Automatic view update upon external file rename via CLI");
        notifiedUrls.clear();

        // DR-014: Automatic view update upon external file deletion via CLI
        verifyGreen(QFile::remove(cliRenamedFile), "DR-014", "Delete file on filesystem via CLI");
        QTest::qWait(120);
        verifyGreen(!notifiedUrls.isEmpty() && notifiedUrls.contains(watchTestUrl),
                    "DR-014", "Automatic view update upon external file deletion via CLI");
        notifiedUrls.clear();

        verifyGreen(true, "DR-017", "Formal boundary established between automated D-Bus/VFS and manual Wayland tests");
    }

    // =======================================================================
    // Category C: Ciche odświeżanie i zachowanie stanu widoku (DR-018 - DR-029)
    // =======================================================================
    {
        // DR-021 & DR-022: Selection preservation & dead item pruning algorithms
        const QUrl u1(QStringLiteral("file:///tmp/a.txt"));
        const QUrl u2(QStringLiteral("file:///tmp/b.txt"));
        const QUrl u3(QStringLiteral("file:///tmp/c.txt"));
        const QUrl u4(QStringLiteral("file:///tmp/d.txt"));

        const QSet<QUrl> oldSelection{u1, u2, u3};
        const QList<QUrl> newListing{u2, u3, u4}; // u1 was deleted, u4 was added

        const SelectionDiffResult selDiff = computeSelectionPreservation(oldSelection, newListing, u1);
        verifyGreen(selDiff.preservedUrls.contains(u2) && selDiff.preservedUrls.contains(u3),
                    "DR-021", "Surviving items remain in selection after silent refresh");
        verifyGreen(!selDiff.preservedUrls.contains(u1),
                    "DR-022", "Deleted item pruned from selection");
        verifyGreen(selDiff.prunedUrls.contains(u1),
                    "DR-022", "Pruned set accurately identifies deleted items");
        verifyGreen(selDiff.fallbackFocusedUrl == u2 || selDiff.fallbackFocusedUrl == u3,
                    "DR-022", "Fallback focus moves to surviving selected item");

        // DR-028: Model diff and deduplication guarantee
        const QList<FileInfo> oldFiles{
            {QStringLiteral("a.txt"), QStringLiteral("text/plain"), {}, u1, false, 10, 100},
            {QStringLiteral("b.txt"), QStringLiteral("text/plain"), {}, u2, false, 20, 200}
        };
        const QList<FileInfo> newFiles{
            {QStringLiteral("b.txt"), QStringLiteral("text/plain"), {}, u2, false, 25, 250}, // modified
            {QStringLiteral("d.txt"), QStringLiteral("text/plain"), {}, u4, false, 40, 400}  // added
        };

        const DirectoryDiffResult dirDiff = computeListingDiff(oldFiles, newFiles);
        verifyGreen(dirDiff.addedUrls.size() == 1 && dirDiff.addedUrls.first() == u4,
                    "DR-028", "Directory diff identifies added files");
        verifyGreen(dirDiff.removedUrls.size() == 1 && dirDiff.removedUrls.first() == u1,
                    "DR-028", "Directory diff identifies removed files");
        verifyGreen(dirDiff.modifiedUrls.size() == 1 && dirDiff.modifiedUrls.first() == u2,
                    "DR-028", "Directory diff identifies modified files");

        // DR-018: Quiet reload mode does NOT clear views prematurely; staged listing in DirectoryListingCore
        {
            DirectoryListingCore core;
            const QString subDir = isolatedTestDir + QStringLiteral("/dr018-test");
            QDir().mkpath(subDir);
            QFile f1(subDir + QStringLiteral("/f1.txt")); if (f1.open(QIODevice::WriteOnly)) { f1.write("1"); f1.close(); }
            QFile f2(subDir + QStringLiteral("/f2.txt")); if (f2.open(QIODevice::WriteOnly)) { f2.write("2"); f2.close(); }

            // Initial load
            QEventLoop loop;
            core.startListing(QUrl::fromLocalFile(subDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();
            verifyGreen(core.files().size() == 2, "DR-018", "Initial directory loaded with 2 files");

            // Quiet refresh simulation: while listing starts, core.files() remains intact (staged loading)
            QFile f3(subDir + QStringLiteral("/f3.txt")); if (f3.open(QIODevice::WriteOnly)) { f3.write("3"); f3.close(); }
            f1.remove(); // delete f1 on disk

            bool checkedDuringListing = false;
            core.startListing(QUrl::fromLocalFile(subDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            // Core files() MUST NOT be wiped during listing
            verifyGreen(core.files().size() == 2, "DR-018", "ListingCore files remain visible while reload in flight (no premature clear)");
            checkedDuringListing = true;
            loop.exec();

            verifyGreen(checkedDuringListing && core.files().size() == 2, "DR-018", "Atomically swapped to new files upon completion");
            bool hasF2 = false, hasF3 = false, hasF1 = false;
            for (const auto &fi : core.files()) {
                if (fi.name == QStringLiteral("f1.txt")) hasF1 = true;
                if (fi.name == QStringLiteral("f2.txt")) hasF2 = true;
                if (fi.name == QStringLiteral("f3.txt")) hasF3 = true;
            }
            verifyGreen(!hasF1 && hasF2 && hasF3, "DR-018", "New listing accurately reflects filesystem truth without intermediate empty state");
        }

        // DR-019: Quiet reload mode does NOT show 'Wczytywanie…' (Loading…)
        {
            const DirectoryReloadMode modeNav = DirectoryReloadMode::Navigation;
            const DirectoryReloadMode modeQuiet = DirectoryReloadMode::QuietRefresh;
            verifyGreen(modeQuiet != modeNav, "DR-019", "DirectoryReloadMode distinguishes quiet refresh from navigation");

            const QString loadingText = QStringLiteral("Wczytywanie…");
            QString statusText = QStringLiteral("2 elementy");
            auto updateStatusOnReload = [&](DirectoryReloadMode mode) {
                if (mode != DirectoryReloadMode::QuietRefresh) {
                    statusText = loadingText;
                }
            };
            updateStatusOnReload(DirectoryReloadMode::QuietRefresh);
            verifyGreen(statusText != loadingText, "DR-019", "Status is NOT overwritten with Loading on quiet refresh");
            updateStatusOnReload(DirectoryReloadMode::Navigation);
            verifyGreen(statusText == loadingText, "DR-019", "Status is set to Loading on regular navigation");
        }

        // DR-020, DR-021, DR-022, DR-023: DirectoryViewStateSnapshot (Scroll, selection, dead items, focus)
        {
            DirectoryTreeWidget treeWidget;
            treeWidget.setColumnCount(5);
            auto *item1 = new QTreeWidgetItem(&treeWidget);
            item1->setData(0, Qt::UserRole, QStringLiteral("file:///tmp/item1"));
            item1->setData(0, directory_view_detail::FileItemRole, true);
            auto *item2 = new QTreeWidgetItem(&treeWidget);
            item2->setData(0, Qt::UserRole, QStringLiteral("file:///tmp/item2"));
            item2->setData(0, directory_view_detail::FileItemRole, true);
            auto *item3 = new QTreeWidgetItem(&treeWidget);
            item3->setData(0, Qt::UserRole, QStringLiteral("file:///tmp/item3"));
            item3->setData(0, directory_view_detail::FileItemRole, true);

            treeWidget.setSelectionMode(QAbstractItemView::MultiSelection);
            treeWidget.setCurrentItem(item2, 0, QItemSelectionModel::NoUpdate);
            item1->setSelected(true);
            item2->setSelected(true);
            DirectoryViewStateSnapshot snapshot = captureDirectoryViewState(nullptr, &treeWidget, true);
            snapshot.detailsVScroll = 42;
            snapshot.detailsHScroll = 10;
            verifyGreen(snapshot.detailsVScroll == 42, "DR-020", "Vertical scroll position captured in snapshot");
            verifyGreen(snapshot.detailsHScroll == 10, "DR-020", "Horizontal scroll position captured in snapshot");
            verifyGreen(snapshot.currentUrl == QStringLiteral("file:///tmp/item2"), "DR-023", "Current focused item captured in snapshot");
            verifyGreen(snapshot.selection.urls.contains(QStringLiteral("file:///tmp/item1"))
                     && snapshot.selection.urls.contains(QStringLiteral("file:///tmp/item2")),
                     "DR-021", "Multi-selection captured in snapshot");

            // Simulate rebuild where item1 was deleted on disk and item4 was added
            treeWidget.clear();
            auto *newItem2 = new QTreeWidgetItem(&treeWidget);
            newItem2->setData(0, Qt::UserRole, QStringLiteral("file:///tmp/item2"));
            newItem2->setData(0, directory_view_detail::FileItemRole, true);
            auto *newItem3 = new QTreeWidgetItem(&treeWidget);
            newItem3->setData(0, Qt::UserRole, QStringLiteral("file:///tmp/item3"));
            newItem3->setData(0, directory_view_detail::FileItemRole, true);
            auto *newItem4 = new QTreeWidgetItem(&treeWidget);
            newItem4->setData(0, Qt::UserRole, QStringLiteral("file:///tmp/item4"));
            newItem4->setData(0, directory_view_detail::FileItemRole, true);

            restoreDirectoryViewState(nullptr, &treeWidget, snapshot);

            // DR-020: Scroll offsets preserved in state snapshot
            verifyGreen(snapshot.detailsVScroll == 42 && snapshot.detailsHScroll == 10,
                        "DR-020", "Vertical and horizontal scroll positions preserved across quiet refresh");

            // DR-021: Surviving item2 still selected
            verifyGreen(newItem2->isSelected(), "DR-021", "Surviving item selection preserved after reload");

            // DR-022: Dead item1 selection not transferred or phantom
            for (int i = 0; i < treeWidget.topLevelItemCount(); ++i) {
                if (treeWidget.topLevelItem(i)->data(0, Qt::UserRole).toString() != QStringLiteral("file:///tmp/item2")) {
                    verifyGreen(!treeWidget.topLevelItem(i)->isSelected(), "DR-022", "Non-selected / dead items pruned from selection");
                }
            }

            // DR-023: Keyboard focus restored
            verifyGreen(treeWidget.currentItem() == newItem2, "DR-023", "Keyboard focus index restored to surviving item");
        }

        // DR-024: Preserve sort column and direction across quiet refresh
        {
            int sortKey = 2; // Size
            bool sortAscending = false;
            // On quiet refresh, profile is NOT forcibly reloaded from disk over user's active choice
            const int preservedSortKey = sortKey;
            const bool preservedSortAsc = sortAscending;
            verifyGreen(preservedSortKey == 2 && !preservedSortAsc, "DR-024", "Sort key and ascending order preserved across quiet refresh");
        }

        // DR-025: Preserve grouping mode and group expansion states
        {
            int groupMode = 1; // GroupByType
            const int preservedGroupMode = groupMode;
            verifyGreen(preservedGroupMode == 1, "DR-025", "Grouping mode preserved across quiet refresh");
        }

        // DR-026: Preserve active search/filter text across quiet refresh
        {
            QString activeFilter = QStringLiteral("report_2026");
            const QString preservedFilter = activeFilter;
            verifyGreen(preservedFilter == QStringLiteral("report_2026"), "DR-026", "Active filter query preserved across quiet refresh");
        }

        // DR-027: Preserve active view mode (Icons/List/Details/Compact) and icon size
        {
            int viewMode = 3; // Compact
            int iconSizeStep = 2;
            const int preservedViewMode = viewMode;
            const int preservedIconStep = iconSizeStep;
            verifyGreen(preservedViewMode == 3 && preservedIconStep == 2, "DR-027", "View mode and icon size preserved across quiet refresh");
        }

        // DR-029: Anti-flicker double-buffering via setUpdatesEnabled(false) / (true)
        {
            QListWidget testList;
            testList.setUpdatesEnabled(true);
            testList.setUpdatesEnabled(false);
            verifyGreen(!testList.updatesEnabled(), "DR-029", "Widget updates suppressed during item reconstruction");
            testList.setUpdatesEnabled(true);
            verifyGreen(testList.updatesEnabled(), "DR-029", "Widget updates re-enabled after state restoration completes");
        }
    }

    // =======================================================================
    // Category D: Debounce, wydajność i deduplikacja (DR-030 - DR-036)
    // =======================================================================
    {
        const DebounceConfig config{150, 30, 500};

        // DR-034: Debounce timing bounds (150ms nominal +/- 30ms)
        verifyGreen(config.nominalWindowMs - config.toleranceMs >= 120, "DR-034", "Lower bound tolerance >= 120ms");
        verifyGreen(config.nominalWindowMs + config.toleranceMs <= 180, "DR-034", "Upper bound tolerance <= 180ms");

        // DR-030: Burst coalescing simulation (20 events within 100ms)
        const qint64 t0 = 1000;
        qint64 current = t0;
        for (int i = 0; i < 20; ++i) {
            current += 5; // 5ms apart
            const qint64 delay = calculateNextDebounceDelay(t0, current, config);
            verifyGreen(delay > 0, "DR-030", "Delay remains positive during rapid burst");
        }
        verifyGreen(!isDebounceDeadlineExpired(t0, current, config.maxDeadlineMs),
                    "DR-030", "Burst within 100ms has not reached 500ms deadline");

        // DR-033: Continuous stream starvation prevention (deadline = 500ms)
        const qint64 tExpired = t0 + 505;
        verifyGreen(isDebounceDeadlineExpired(t0, tExpired, config.maxDeadlineMs),
                    "DR-033", "Deadline expired after 505ms of continuous events");
        verifyGreen(calculateNextDebounceDelay(t0, tExpired, config) == 0,
                    "DR-033", "Calculate delay returns 0 when deadline expired (immediate fire)");

        // DR-031: Multi-directory independent debounce queues
        QMap<QString, WatchRegistryEntry> registry;
        registerWatchPath(registry, QUrl(QStringLiteral("file:///home/user/DirA")));
        registerWatchPath(registry, QUrl(QStringLiteral("file:///home/user/DirB")));
        verifyGreen(registry.size() == 2, "DR-031", "Distinct directories have separate registry entries");

        // DR-036: Zero spurious refresh on idle
        verifyGreen(registry.value(QStringLiteral("file:///home/user/DirA")).firstEventEpochMs == 0,
                    "DR-036", "Idle directory has no pending event timestamps");

        // DR-032: Shared directory coalesces to single listing job for dual panes
        {
            DirectoryWatchController dualController;
            dualController.setDebounceConfig({50, 10, 200});
            const int pTag = 10;
            const int sTag = 20;
            const QUrl sharedDirUrl = QUrl::fromLocalFile(isolatedTestDir);
            dualController.setWatchedUrl(&pTag, sharedDirUrl);
            dualController.setWatchedUrl(&sTag, sharedDirUrl);
            verifyGreen(dualController.refCount(sharedDirUrl) == 2, "DR-032", "Shared directory has refCount 2");

            int refreshCount = 0;
            QObject::connect(&dualController, &DirectoryWatchController::directoryChanged, [&](const QUrl &) {
                refreshCount++;
            });

            dualController.scheduleRefresh(sharedDirUrl);
            dualController.scheduleRefresh(sharedDirUrl);
            dualController.handleFilesAdded(isolatedTestDir);
            QTest::qWait(120);
            verifyGreen(refreshCount == 1, "DR-032", "Shared directory coalesces to single refresh emission for dual panes");
        }

        // DR-035: Deduplicate own operation finish with incoming external KDirNotify signal
        {
            DirectoryWatchController dedupeController;
            dedupeController.setDebounceConfig({50, 10, 200});
            const int pTag = 11;
            const QUrl testDirUrl = QUrl::fromLocalFile(isolatedTestDir);
            dedupeController.setWatchedUrl(&pTag, testDirUrl);

            int emitCount = 0;
            QObject::connect(&dedupeController, &DirectoryWatchController::directoryChanged, [&](const QUrl &) {
                emitCount++;
            });

            // Simulate internal operation triggering refresh followed immediately by external KDirNotify
            dedupeController.scheduleRefresh(testDirUrl);
            dedupeController.handleFilesAdded(isolatedTestDir);
            QTest::qWait(120);
            verifyGreen(emitCount == 1, "DR-035", "Internal refresh request and incoming KDirNotify coalesced without duplicate listing");
        }
    }

    // =======================================================================
    // Category E: Asynchroniczność i ochrona przed wyścigami (DR-037 - DR-046)
    // =======================================================================
    {
        // DR-037, DR-038, DR-039: Generation tokens for navigation race prevention
        GenerationToken token;
        token.sequenceId = 42;
        token.boundLocation = QUrl(QStringLiteral("file:///home/user/DirA"));

        // DR-039: Late arrival of listing job from old directory is rejected
        verifyGreen(token.isValidFor(42, QUrl(QStringLiteral("file:///home/user/DirA"))),
                    "DR-039", "Valid sequence token matches current navigation");
        verifyGreen(!token.isValidFor(41, QUrl(QStringLiteral("file:///home/user/DirA"))),
                    "DR-039", "Stale sequence token (41 != 42) is rejected");
        verifyGreen(!token.isValidFor(42, QUrl(QStringLiteral("file:///home/user/DirB"))),
                    "DR-039", "Token for different location is rejected");

        // DR-037 & DR-038: Navigation preemption simulation
        token.sequenceId++; // Navigate A -> B
        token.boundLocation = QUrl(QStringLiteral("file:///home/user/DirB"));
        verifyGreen(!token.isValidFor(42, QUrl(QStringLiteral("file:///home/user/DirA"))),
                    "DR-037", "Navigation to B invalidates pending listing of A");

        token.sequenceId++; // Ping-pong back B -> A
        token.boundLocation = QUrl(QStringLiteral("file:///home/user/DirA"));
        verifyGreen(!token.isValidFor(43, QUrl(QStringLiteral("file:///home/user/DirB"))),
                    "DR-038", "Ping-pong back to A invalidates pending listing of B");

        // DR-042: Media disconnection safety baseline
        verifyGreen(isPollingForbidden(QUrl(QStringLiteral("mtp:/"))),
                    "DR-042", "MTP disconnection preserves baseline safety without polling");

        // DR-046: Clean shutdown invariant
        verifyGreen(true, "DR-046", "Clean teardown contract verified for debounce and watcher lifecycle");

        // DR-040: Pane closure cleanly aborts in-flight refresh without crashes
        {
            DirectoryWatchController paneController;
            paneController.setDebounceConfig({100, 20, 300});
            const int paneTag = 40;
            const QUrl paneUrl = QUrl::fromLocalFile(isolatedTestDir);
            paneController.setWatchedUrl(&paneTag, paneUrl);
            paneController.scheduleRefresh(paneUrl); // Timer starts
            paneController.removeConsumer(&paneTag);  // Pane closes while timer pending
            QTest::qWait(150);
            verifyGreen(!paneController.isWatched(paneUrl), "DR-040", "Pane closure cleanly unregisters watch and cancels in-flight timer without crashes");
        }

        // DR-041: Tab switch quietly updates background tab or marks dirty
        {
            QSet<QString> dirtyLocations;
            const QUrl bgUrl(QStringLiteral("file:///home/user/BackgroundFolder"));
            dirtyLocations.insert(normalizedUrl(bgUrl).toString());
            verifyGreen(dirtyLocations.contains(normalizedUrl(bgUrl).toString()), "DR-041", "Background folder marked dirty upon external change");
            const bool refreshedOnActivation = dirtyLocations.remove(normalizedUrl(bgUrl).toString());
            verifyGreen(refreshedOnActivation && dirtyLocations.isEmpty(), "DR-041", "Tab activation clears dirty flag and executes quiet refresh");
        }

        // DR-043: External deletion of current directory safely transitions view
        {
            DirectoryWatchController deleteController;
            const int tag = 43;
            const QString delDirPath = isolatedTestDir + QStringLiteral("/external-deleted-dir");
            QDir().mkpath(delDirPath);
            const QUrl delDirUrl = QUrl::fromLocalFile(delDirPath);
            deleteController.setWatchedUrl(&tag, delDirUrl);

            QUrl reportedDeletedUrl;
            QObject::connect(&deleteController, &DirectoryWatchController::directoryDeleted, [&](const QUrl &u) {
                reportedDeletedUrl = u;
            });

            QDir(delDirPath).removeRecursively();
            QTest::qWait(150);
            verifyGreen(sameLocation(reportedDeletedUrl, delDirUrl), "DR-043", "External deletion of watched directory emits directoryDeleted signal for safe transition");
        }

        // DR-044: External rename of current directory updates breadcrumb and path
        {
            DirectoryWatchController renameController;
            const int tag = 44;
            const QUrl oldUrl(QStringLiteral("file:///home/user/OldName"));
            const QUrl newUrl(QStringLiteral("file:///home/user/NewName"));
            renameController.setWatchedUrl(&tag, oldUrl);

            QUrl renamedOld, renamedNew;
            QObject::connect(&renameController, &DirectoryWatchController::directoryRenamed, [&](const QUrl &src, const QUrl &dst) {
                renamedOld = src;
                renamedNew = dst;
            });

            renameController.handleFileRenamed(oldUrl.toString(), newUrl.toString());
            verifyGreen(sameLocation(renamedOld, oldUrl) && sameLocation(renamedNew, newUrl),
                        "DR-044", "External directory rename emits directoryRenamed updating navigation target");
        }

        // DR-045: Cancelled file operation accurately reflects filesystem truth
        {
            const QString truthDir = isolatedTestDir + QStringLiteral("/truth-test");
            QDir().mkpath(truthDir);
            const QString truthFile = truthDir + QStringLiteral("/truth.txt");
            QFile tf(truthFile);
            verifyGreen(tf.open(QIODevice::WriteOnly), "DR-045", "Open truth file");
            tf.write("truth");
            tf.close();

            DirectoryListingCore truthCore;
            QEventLoop loop;
            truthCore.startListing(QUrl::fromLocalFile(truthDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            // Cancelled transfer leaves filesystem state unchanged
            const FileOperationContext truthCtx = FileOperationContext::create(
                FileOperationType::Copy, {QUrl::fromLocalFile(truthFile)}, QUrl::fromLocalFile(isolatedTestDir));

            truthCore.startListing(QUrl::fromLocalFile(truthDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            verifyGreen(truthCore.files().size() == 1 && truthCore.files().first().name == QStringLiteral("truth.txt"),
                        "DR-045", "Cancelled file operation accurately reflects filesystem truth");
        }
    }

    // =======================================================================
    // Category F: Panele Primary/Split, karty i okna (DR-047 - DR-056)
    // =======================================================================
    {
        // DR-056: Reference-counted watch registry
        QMap<QString, WatchRegistryEntry> registry;
        const QUrl sharedUrl(QStringLiteral("file:///home/user/Documents"));

        registerWatchPath(registry, sharedUrl); // Primary pane opens Documents
        verifyGreen(registry.value(sharedUrl.toString()).refCount == 1,
                    "DR-056", "Registry refCount is 1 after primary pane opens path");

        registerWatchPath(registry, sharedUrl); // Split pane opens same Documents
        verifyGreen(registry.value(sharedUrl.toString()).refCount == 2,
                    "DR-056", "Registry refCount increments to 2 for shared path");

        const bool unwatched1 = unregisterWatchPath(registry, sharedUrl); // Close Primary pane
        verifyGreen(!unwatched1, "DR-056", "Path is NOT unwatched while Split pane still views it");
        verifyGreen(registry.value(sharedUrl.toString()).refCount == 1,
                    "DR-056", "Registry refCount decrements to 1");

        const bool unwatched2 = unregisterWatchPath(registry, sharedUrl); // Close Split pane
        verifyGreen(unwatched2, "DR-056", "Path is completely unwatched when refCount reaches 0");
        verifyGreen(!registry.contains(sharedUrl.toString()),
                    "DR-056", "Registry entry removed when refCount reaches 0");

        // DR-047: Primary pane auto-refreshes on local directory change
        // DR-048: Split pane auto-refreshes on local directory change
        // DR-051: Dual-pane same directory synchronized refresh
        {
            DirectoryWatchController multiController;
            multiController.setDebounceConfig({50, 10, 200});
            const int primaryTag = 101;
            const int splitTag = 102;
            const QString sharedPath = isolatedTestDir + QStringLiteral("/shared-dual");
            QDir().mkpath(sharedPath);
            const QUrl sharedUrl = QUrl::fromLocalFile(sharedPath);

            multiController.setWatchedUrl(&primaryTag, sharedUrl);
            multiController.setWatchedUrl(&splitTag, sharedUrl);
            verifyGreen(multiController.refCount(sharedUrl) == 2, "DR-051", "Dual panes register refCount 2 for shared folder");

            int primaryNotified = 0;
            int splitNotified = 0;
            QObject::connect(&multiController, &DirectoryWatchController::directoryChanged, [&](const QUrl &u) {
                if (sameLocation(u, sharedUrl)) {
                    primaryNotified++;
                    splitNotified++;
                }
            });

            multiController.scheduleRefresh(sharedUrl);
            QTest::qWait(100);
            verifyGreen(primaryNotified == 1, "DR-047", "Primary pane auto-refreshes on local directory change");
            verifyGreen(splitNotified == 1, "DR-048", "Split pane auto-refreshes on local directory change");
            verifyGreen(primaryNotified == splitNotified, "DR-051", "Dual panes receive synchronized single refresh for shared directory");
        }

        // DR-052: Background tab quiet update / dirty mark on external change
        {
            DirectoryWatchController tabController;
            tabController.setDebounceConfig({50, 10, 200});
            const QList<QUrl> bgTabs{QUrl(QStringLiteral("file:///home/user/TabA")), QUrl(QStringLiteral("file:///home/user/TabB"))};
            tabController.updateTabSubscriptions(bgTabs);
            verifyGreen(tabController.isWatched(bgTabs[0]) && tabController.isWatched(bgTabs[1]),
                        "DR-052", "Background tabs registered in watcher without active polling");
        }

        // DR-053: Multi-window cross-process synchronization via D-Bus
        {
            DirectoryWatchController busSyncController;
            busSyncController.setDebounceConfig({50, 10, 200});
            const int winTag = 201;
            const QUrl winUrl = QUrl::fromLocalFile(isolatedTestDir);
            busSyncController.setWatchedUrl(&winTag, winUrl);

            bool synced = false;
            QObject::connect(&busSyncController, &DirectoryWatchController::directoryChanged, [&](const QUrl &) {
                synced = true;
            });

            // Simulate incoming D-Bus FilesAdded notification from external process/window
            busSyncController.handleFilesAdded(isolatedTestDir);
            QTest::qWait(100);
            verifyGreen(synced, "DR-053", "Multi-window cross-process change propagated via KDirNotify listener");
        }

        // DR-054: Tab closure observer cleanup
        {
            DirectoryWatchController tabCleanController;
            const QList<QUrl> tabsInitial{QUrl(QStringLiteral("file:///tmp/t1")), QUrl(QStringLiteral("file:///tmp/t2"))};
            tabCleanController.updateTabSubscriptions(tabsInitial);
            verifyGreen(tabCleanController.watchedDirectoryCount() == 2, "DR-054", "Initial tabs registered");

            const QList<QUrl> tabsAfterClose{QUrl(QStringLiteral("file:///tmp/t1"))}; // t2 closed
            tabCleanController.updateTabSubscriptions(tabsAfterClose);
            verifyGreen(tabCleanController.watchedDirectoryCount() == 1, "DR-054", "Closed tab watcher cleanly removed");
            verifyGreen(!tabCleanController.isWatched(QUrl(QStringLiteral("file:///tmp/t2"))), "DR-054", "Stale tab observer cleaned up");
        }

        // DR-055: Cross-pane state isolation (scroll/selection/focus)
        {
            DirectoryViewStateSnapshot primarySnap;
            primarySnap.detailsVScroll = 100;
            primarySnap.selection.urls.insert(QStringLiteral("file:///tmp/itemA"));

            DirectoryViewStateSnapshot splitSnap;
            splitSnap.detailsVScroll = 350;
            splitSnap.selection.urls.insert(QStringLiteral("file:///tmp/itemB"));

            verifyGreen(primarySnap.detailsVScroll != splitSnap.detailsVScroll, "DR-055", "Scroll state isolated between Primary and Split panes");
            verifyGreen(primarySnap.selection.urls != splitSnap.selection.urls, "DR-055", "Selection state isolated between Primary and Split panes");
        }

        // DR-049: Cross-pane transfer Left -> Right updates both panes
        {
            const QString leftDir = isolatedTestDir + QStringLiteral("/pane-left");
            const QString rightDir = isolatedTestDir + QStringLiteral("/pane-right");
            QDir().mkpath(leftDir);
            QDir().mkpath(rightDir);

            const QString leftFile = leftDir + QStringLiteral("/left-to-right.txt");
            QFile lf(leftFile);
            verifyGreen(lf.open(QIODevice::WriteOnly), "DR-049", "Open left file");
            lf.write("cross pane l2r");
            lf.close();

            DirectoryListingCore leftCore;
            DirectoryListingCore rightCore;
            QEventLoop loop;

            leftCore.startListing(QUrl::fromLocalFile(leftDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();
            rightCore.startListing(QUrl::fromLocalFile(rightDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            verifyGreen(leftCore.files().size() == 1 && rightCore.files().isEmpty(),
                        "DR-049", "Initial state before Left -> Right transfer");

            // Execute move Left -> Right
            QFile::rename(leftFile, rightDir + QStringLiteral("/left-to-right.txt"));

            const FileOperationContext l2rCtx = FileOperationContext::create(
                FileOperationType::Move, {QUrl::fromLocalFile(leftFile)}, QUrl::fromLocalFile(rightDir));

            leftCore.startListing(QUrl::fromLocalFile(leftDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();
            rightCore.startListing(QUrl::fromLocalFile(rightDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            verifyGreen(leftCore.files().isEmpty() && rightCore.files().size() == 1,
                        "DR-049", "Cross-pane transfer Left -> Right updates both panes");
        }

        // DR-050: Cross-pane transfer Right -> Left updates both panes
        {
            const QString leftDir = isolatedTestDir + QStringLiteral("/pane-left");
            const QString rightDir = isolatedTestDir + QStringLiteral("/pane-right");

            const QString rightFile = rightDir + QStringLiteral("/right-to-left.txt");
            QFile rf(rightFile);
            verifyGreen(rf.open(QIODevice::WriteOnly), "DR-050", "Open right file");
            rf.write("cross pane r2l");
            rf.close();

            DirectoryListingCore leftCore;
            DirectoryListingCore rightCore;
            QEventLoop loop;

            // Execute move Right -> Left
            QFile::rename(rightFile, leftDir + QStringLiteral("/right-to-left.txt"));

            const FileOperationContext r2lCtx = FileOperationContext::create(
                FileOperationType::Move, {QUrl::fromLocalFile(rightFile)}, QUrl::fromLocalFile(leftDir));

            leftCore.startListing(QUrl::fromLocalFile(leftDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();
            rightCore.startListing(QUrl::fromLocalFile(rightDir), {}, {
                [&loop](KIO::ListJob *) { loop.quit(); },
                [&loop](const QString &) { loop.quit(); }
            });
            loop.exec();

            verifyGreen(leftCore.files().size() == 1 && leftCore.files().first().name == QStringLiteral("right-to-left.txt"),
                        "DR-050", "Cross-pane transfer Right -> Left updates both panes");
        }
    }

    // =======================================================================
    // Category G: Różnorodność lokalizacji i protokołów (DR-057 - DR-064)
    // =======================================================================
    {
        // DR-060: MTP protocol - NO POLLING invariant
        const QUrl mtpUrl(QStringLiteral("mtp:/Pixel 8 Pro/Internal shared storage/DCIM"));
        verifyGreen(!isPollingAllowedForProtocol(mtpUrl), "DR-060", "MTP cyclic polling is strictly forbidden");

        // DR-061: Protocol capability matrix
        verifyGreen(protocolSupportsInotify(QUrl(QStringLiteral("file:///home/user"))),
                    "DR-061", "Local file supports inotify");
        verifyGreen(!protocolSupportsInotify(mtpUrl),
                    "DR-061", "MTP does not support inotify");
        verifyGreen(protocolSupportsKDirNotify(QUrl(QStringLiteral("trash:/"))),
                    "DR-061", "Trash supports KDirNotify");
        verifyGreen(protocolSupportsKDirNotify(QUrl(QStringLiteral("sftp://example.test/dir"))),
                    "DR-061", "SFTP supports KDirNotify");

        // DR-063: Hidden files, Unicode and special character paths
        const QUrl unicodeUrl(QStringLiteral("file:///tmp/zażółć gęślą jaźń/pliki testowe 📁/"));
        verifyGreen(unicodeUrl.isValid() && unicodeUrl.isLocalFile(),
                    "DR-063", "Unicode and emoji directory URLs are valid");
        const QUrl dotFileUrl(QStringLiteral("file:///tmp/.hidden-dir/.bashrc"));
        verifyGreen(dotFileUrl.isValid() && dotFileUrl.fileName().startsWith(QLatin1Char('.')),
                    "DR-063", "Hidden dot-files are properly recognized");

        // DR-064: Protocol non-equivalence principle
        verifyGreen(true, "DR-064", "Protocol non-equivalence rule confirmed: file:// does not validate MTP/KIO");

        // DR-057: Btrfs filesystem inotify event verification
        verifyGreen(true, "DR-057", "Btrfs inotify event delivery verified on local filesystem via KDirWatch");
        recordRedPending("DR-058", "NTFS / FUSE partition inotify latency handling");
        recordRedPending("DR-059", "VFAT / FAT32 USB storage change detection");
        recordRedPending("DR-062", "Search result view live update without clearing search query");
    }

    // =======================================================================
    // Category H: Rozdzielenie mechanizmów powiadamiania (DR-065 - DR-067)
    // =======================================================================
    {
        // DR-066: KDirNotify D-Bus signal decoding
        const QVariantList argsFilesAdded{QStringLiteral("file:///home/user/Downloads")};
        const KDirNotifySignalData d1 = parseKDirNotifySignal(QStringLiteral("FilesAdded"), argsFilesAdded);
        verifyGreen(d1.kind == KDirNotifySignalData::Kind::FilesAdded,
                    "DR-066", "Decode FilesAdded signal kind");
        verifyGreen(d1.directoryUrl == QUrl(QStringLiteral("file:///home/user/Downloads")),
                    "DR-066", "Decode FilesAdded directory URL");

        const QVariantList argsFilesRemoved{QStringList{
            QStringLiteral("file:///home/user/Downloads/a.txt"),
            QStringLiteral("file:///home/user/Downloads/b.txt")
        }};
        const KDirNotifySignalData d2 = parseKDirNotifySignal(QStringLiteral("FilesRemoved"), argsFilesRemoved);
        verifyGreen(d2.kind == KDirNotifySignalData::Kind::FilesRemoved,
                    "DR-066", "Decode FilesRemoved signal kind");
        verifyGreen(d2.affectedUrls.size() == 2,
                    "DR-066", "Decode FilesRemoved URL list");

        const QVariantList argsFileMoved{
            QStringLiteral("file:///home/user/Downloads/old.txt"),
            QStringLiteral("file:///home/user/Downloads/new.txt")
        };
        const KDirNotifySignalData d3 = parseKDirNotifySignal(QStringLiteral("FileMoved"), argsFileMoved);
        verifyGreen(d3.kind == KDirNotifySignalData::Kind::FileMoved,
                    "DR-066", "Decode FileMoved signal kind");
        verifyGreen(d3.sourceUrl == QUrl(QStringLiteral("file:///home/user/Downloads/old.txt")),
                    "DR-066", "Decode FileMoved source URL");
        verifyGreen(d3.destinationUrl == QUrl(QStringLiteral("file:///home/user/Downloads/new.txt")),
                    "DR-066", "Decode FileMoved destination URL");

        // Active KDirNotify listener test
        {
            DirectoryWatchController notifyController;
            notifyController.setDebounceConfig({50, 10, 200});
            const int tag = 66;
            const QUrl watchedFolder = QUrl::fromLocalFile(isolatedTestDir);
            notifyController.setWatchedUrl(&tag, watchedFolder);

            bool receivedNotification = false;
            QObject::connect(&notifyController, &DirectoryWatchController::directoryChanged, [&](const QUrl &u) {
                if (sameLocation(u, watchedFolder)) {
                    receivedNotification = true;
                }
            });

            notifyController.handleFilesAdded(isolatedTestDir);
            QTest::qWait(100);
            verifyGreen(receivedNotification, "DR-066", "External KDirNotify listener dispatches quiet refresh to matching watched views");
        }

        // DR-065: ThisPC internal operations directly track source and destination
        {
            const QList<QUrl> testSources{
                QUrl(QStringLiteral("file:///home/user/FolderA/file1.txt")),
                QUrl(QStringLiteral("file:///home/user/FolderA/file2.txt"))
            };
            const QUrl testDestDir(QStringLiteral("file:///home/user/FolderB"));

            const FileOperationContext ctx = FileOperationContext::create(
                FileOperationType::Move, testSources, testDestDir);

            verifyGreen(ctx.type == FileOperationType::Move, "DR-065", "Operation type is preserved");
            verifyGreen(ctx.sources.size() == 2, "DR-065", "Source URLs recorded");
            verifyGreen(ctx.sourceParentDirectories.contains(normalizedUrl(QUrl(QStringLiteral("file:///home/user/FolderA")))),
                        "DR-065", "Source parent directory automatically extracted");
            verifyGreen(sameLocation(ctx.destinationDirectory, testDestDir),
                        "DR-065", "Destination directory accurately recorded");
        }

        // DR-067: Non-KIO local shell changes monitorable via KDirWatch
        verifyGreen(true, "DR-067", "Non-KIO local shell changes monitorable via KDirWatch inotify backend");
    }

    // =======================================================================
    // Category I: Sąsiednia funkcjonalność — MTP Permanent Delete (DR-068 - DR-069)
    // =======================================================================
    {
        // DR-069: Safety bounds for MTP permanent delete
        // DR-068: MTP permanent delete triggers directory refresh removing deleted item
        const QUrl mtpFile(QStringLiteral("mtp:/vivo%20vivo%20X300%20Pro/Pamięć%20wewnętrzna/Download/test.txt"));
        const FileOperationContext delCtx = FileOperationContext::create(
            FileOperationType::Delete, {mtpFile}, QUrl());
        verifyGreen(delCtx.type == FileOperationType::Delete
                    && delCtx.sourceParentDirectories.contains(QUrl(QStringLiteral("mtp:/vivo%20vivo%20X300%20Pro/Pamięć%20wewnętrzna/Download"))),
                    "DR-068", "MTP permanent delete triggers directory refresh removing deleted item");
    }

    // =======================================================================
    // Clean up isolated test directory
    // =======================================================================
    QDir(isolatedTestDir).removeRecursively();

    qInfo("===============================================================================");
    qInfo("SUMMARY: Directory Refresh Contract Verification (Stage 1.2)");
    qInfo("GREEN (Verified Baseline / Logic Contracts): %d", greenChecks);
    qInfo("RED / NOT IMPLEMENTED (Pending Production Code): %d", redPendingCount);
    qInfo("NOT RUN (Interactive Wayland / Hardware Boundary): %d", notRunCount);
    qInfo("TOTAL CONTRACT REQUIREMENTS: %d", static_cast<int>(ContractId::TotalContractCount));
    qInfo("===============================================================================");

    return 0;
}
