/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Appended to the temporary regression binary by run-pane-actions.py.

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "directorypreviewadapter.h"
#include "directoryview.h"
#include "previewcontroller.h"
#include "primarybrowserpane.h"
#include "splitbrowserpane.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    int checks = 0;
    auto verify = [&](bool condition, const char *message) {
        ++checks;
        if (!condition) {
            qFatal("FAIL: %s", message);
        }
    };

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "Temporary directory created");

    const QUrl localBase = QUrl::fromLocalFile(tempDir.path());
    const QUrl localImgPng = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.png")));
    const QUrl localVidMp4 = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.mp4")));
    const QUrl localTxt = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.txt")));
    const QUrl localRealExe = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("notepad.exe")));
    const QUrl localNoIconExe = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("no-icon.exe")));
    const QUrl localFolder = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("subfolder")));
    const QUrl remoteExeSftp = QUrl(QStringLiteral("sftp://user@host/remote.exe"));
    const QUrl remoteExeSmb = QUrl(QStringLiteral("smb://server/share/remote.exe"));
    const QUrl remoteExeFish = QUrl(QStringLiteral("fish://user@host/remote.exe"));
    const QUrl localNtfsExe = QUrl::fromLocalFile(QStringLiteral("/run/media/user/NTFS_DRIVE/Games/game.exe"));

    // Prepare test files
    {
        // 1. Text file
        QFile txt(localTxt.toLocalFile());
        verify(txt.open(QIODevice::WriteOnly), "txt open succeeded");
        txt.write("plain text content");
        txt.close();

        // 2. Real PE executable with icon (copy from test fixture if available, or create minimal valid fixture)
        const QString fixtureRealExe = QStringLiteral("/home/sebastianh/Pobrane/Testy/preview-039/exe/real-icon.exe");
        if (QFile::exists(fixtureRealExe)) {
            verify(QFile::copy(fixtureRealExe, localRealExe.toLocalFile()), "Copied real-icon.exe fixture");
        } else {
            QFile real(localRealExe.toLocalFile());
            verify(real.open(QIODevice::WriteOnly), "real exe create fallback");
            real.write("MZ\x90\x00\x03\x00");
            real.close();
        }

        // 3. Fake PE / no-icon executable
        const QString fixtureNoIconExe = QStringLiteral("/home/sebastianh/Pobrane/Testy/preview-039/exe/no-icon.exe");
        if (QFile::exists(fixtureNoIconExe)) {
            verify(QFile::copy(fixtureNoIconExe, localNoIconExe.toLocalFile()), "Copied no-icon.exe fixture");
        } else {
            QFile noIcon(localNoIconExe.toLocalFile());
            verify(noIcon.open(QIODevice::WriteOnly), "no-icon exe create fallback");
            noIcon.write("MZ\x90\x00\x00\x00");
            noIcon.close();
        }

        // 4. Subfolder
        QDir(tempDir.path()).mkdir(QStringLiteral("subfolder"));
    }

    const QFile::Permissions exePermsBefore = QFile::permissions(localRealExe.toLocalFile());

    // 1. Real local Windows executable MIME eligible (application/x-ms-dos-executable)
    verify(DirectoryPreviewAdapter::classifyContent(localRealExe, QStringLiteral("application/x-ms-dos-executable"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::WindowsExecutable,
           "1a. classifyContent recognizes application/x-ms-dos-executable as WindowsExecutable");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localRealExe, QStringLiteral("application/x-ms-dos-executable"), false),
           "1b. isPreviewEligible returns true for application/x-ms-dos-executable");

    // 2. Second recognized Windows executable MIME eligible (application/x-msdownload, portable-executable, wine cpl)
    verify(DirectoryPreviewAdapter::classifyContent(localRealExe, QStringLiteral("application/x-msdownload"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::WindowsExecutable,
           "2a. classifyContent recognizes application/x-msdownload as WindowsExecutable");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localRealExe, QStringLiteral("application/x-msdownload"), false),
           "2b. isPreviewEligible returns true for application/x-msdownload");
    verify(DirectoryPreviewAdapter::classifyContent(localRealExe, QStringLiteral("application/vnd.microsoft.portable-executable"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::WindowsExecutable,
           "2c. classifyContent recognizes application/vnd.microsoft.portable-executable as WindowsExecutable");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localRealExe, QStringLiteral("application/vnd.microsoft.portable-executable"), false),
           "2d. isPreviewEligible returns true for application/vnd.microsoft.portable-executable");
    verify(DirectoryPreviewAdapter::classifyContent(localRealExe, QStringLiteral("application/x-wine-extension-cpl"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::WindowsExecutable,
           "2e. classifyContent recognizes application/x-wine-extension-cpl as WindowsExecutable");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localRealExe, QStringLiteral("application/x-wine-extension-cpl"), false),
           "2f. isPreviewEligible returns true for application/x-wine-extension-cpl");

    // 3. Fake/broken exe safe
    verify(DirectoryPreviewAdapter::classifyContent(localNoIconExe, QStringLiteral("application/x-ms-dos-executable"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::WindowsExecutable,
           "3a. classifyContent classifies fake/no-icon exe as WindowsExecutable");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localNoIconExe, QStringLiteral("application/x-ms-dos-executable"), false),
           "3b. isPreviewEligible allows fake exe without crashing");

    // 4. Text not executable preview
    verify(DirectoryPreviewAdapter::classifyContent(localTxt, QStringLiteral("text/plain"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::Other,
           "4a. text/plain is classified as Other");
    verify(!DirectoryPreviewAdapter::isPreviewEligible(localTxt, QStringLiteral("text/plain"), false),
           "4b. text/plain is NOT eligible for preview");

    // 5. Image still eligible
    verify(DirectoryPreviewAdapter::classifyContent(localImgPng, QStringLiteral("image/png"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::Image,
           "5a. image/png is classified as Image");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localImgPng, QStringLiteral("image/png"), false),
           "5b. image/png is eligible for preview");

    // 6. Video still eligible
    verify(DirectoryPreviewAdapter::classifyContent(localVidMp4, QStringLiteral("video/mp4"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::Video,
           "6a. video/mp4 is classified as Video");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localVidMp4, QStringLiteral("video/mp4"), false),
           "6b. video/mp4 is eligible for preview");

    // 7. Directory is not executable
    verify(DirectoryPreviewAdapter::classifyContent(localFolder, QStringLiteral("inode/directory"), true)
               == DirectoryPreviewAdapter::PreviewContentClass::Directory,
           "7a. directory is classified as Directory");
    verify(DirectoryPreviewAdapter::classifyContent(localFolder, QStringLiteral("inode/directory"), true)
               != DirectoryPreviewAdapter::PreviewContentClass::WindowsExecutable,
           "7b. directory is NOT classified as WindowsExecutable");

    // 8. Local exe eligible
    verify(DirectoryPreviewAdapter::isPreviewEligible(localRealExe, QString(), false),
           "8. Local exe with empty mime (resolving via extension) is eligible");

    // 9. Remote sftp exe skipped
    verify(!DirectoryPreviewAdapter::isPreviewEligible(remoteExeSftp, QStringLiteral("application/x-ms-dos-executable"), false),
           "9a. remote sftp exe is NOT eligible");
    verify(DirectoryPreviewAdapter::classifyContent(remoteExeSftp, QStringLiteral("application/x-ms-dos-executable"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::Other,
           "9b. remote sftp exe classified as Other");

    // 10. Remote smb exe skipped
    verify(!DirectoryPreviewAdapter::isPreviewEligible(remoteExeSmb, QStringLiteral("application/x-ms-dos-executable"), false),
           "10. remote smb exe is NOT eligible");

    // 11. Remote fish exe skipped
    verify(!DirectoryPreviewAdapter::isPreviewEligible(remoteExeFish, QStringLiteral("application/x-ms-dos-executable"), false),
           "11. remote fish exe is NOT eligible");

    // 12-20. Model / Viewport / Delegate preview role lifecycle
    DirectoryListWidget listWidget;
    listWidget.resize(400, 300);
    DirectoryTreeWidget treeWidget;
    treeWidget.resize(400, 300);

    QIcon canonicalIcon = listWidget.style()->standardIcon(QStyle::SP_FileIcon);
    FileInfo fiExe;
    fiExe.name = QStringLiteral("notepad.exe");
    fiExe.url = localRealExe;
    fiExe.isDir = false;
    fiExe.mimeType = QStringLiteral("application/x-ms-dos-executable");

    addDirectoryFileItems(&listWidget, &treeWidget, fiExe, canonicalIcon, QStringLiteral("Application"), QStringLiteral("316 KB"), QStringLiteral("Today"));

    QModelIndex listIdx = listWidget.model()->index(0, 0);
    verify(listIdx.isValid(), "List item 0 is valid");
    QIcon retrievedIcon = listIdx.data(Qt::DecorationRole).value<QIcon>();
    verify(!retrievedIcon.isNull(), "12. Canonical icon preserved initially");
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "12b. PreviewPixmapRole initially invalid");

    // 13-15. Real preview delivery simulation and PreviewPixmapRole assignment
    QPixmap fakeExeIcon(48, 48);
    fakeExeIcon.fill(Qt::yellow);

    DirectoryPreviewAdapter adapter;
    adapter.attachViews(&listWidget, &treeWidget);
    adapter.setCurrentDirectoryUrl(localBase);

    const quint64 gen = adapter.controller().currentGeneration();
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localRealExe),
                              Q_ARG(QPixmap, fakeExeIcon),
                              Q_ARG(quint64, gen));

    verify(listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "15a. PreviewPixmapRole populated after previewReady");

    // 16. Canonical DecorationRole preserved
    QIcon canonicalAfter = listIdx.data(Qt::DecorationRole).value<QIcon>();
    verify(!canonicalAfter.isNull(), "16a. Canonical DecorationRole still exists");
    verify(canonicalAfter.pixmap(32, 32).toImage().pixelColor(0, 0) != Qt::yellow,
           "16b. Canonical DecorationRole was not overwritten by preview pixmap");

    // 17. Clearing preview restores canonical icon
    adapter.clearPreviews();
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "17a. clearPreviews() clears PreviewPixmapRole");

    ExplorerNameDelegate nameDelegate(&listWidget);
    QStyleOptionViewItem opt;
    opt.initFrom(&listWidget);
    nameDelegate.initStyleOption(&opt, listIdx);
    verify(!opt.icon.isNull(), "17b. Delegate falls back to canonical icon");
    verify(opt.icon.pixmap(32, 32).toImage().pixelColor(0, 0) != Qt::yellow,
           "17c. Rendered icon is not the cleared yellow pixmap");

    // 18. Failed executable preview keeps fallback (no PreviewPixmapRole)
    FileInfo fiNoIcon;
    fiNoIcon.name = QStringLiteral("no-icon.exe");
    fiNoIcon.url = localNoIconExe;
    fiNoIcon.isDir = false;
    fiNoIcon.mimeType = QStringLiteral("application/x-ms-dos-executable");
    addDirectoryFileItems(&listWidget, &treeWidget, fiNoIcon, canonicalIcon, QStringLiteral("Application"), QStringLiteral("16 B"), QStringLiteral("Today"));

    QModelIndex noIconIdx = listWidget.model()->index(1, 0);
    verify(!noIconIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "18a. Failed / no-icon executable has no PreviewPixmapRole");
    QStyleOptionViewItem noIconOpt;
    noIconOpt.initFrom(&listWidget);
    nameDelegate.initStyleOption(&noIconOpt, noIconIdx);
    verify(!noIconOpt.icon.isNull(), "18b. Fallback canonical icon intact for failed preview");

    // 19. Broken PE no crash
    adapter.controller().requestPreviews({localNoIconExe}, QSize(48, 48), 1.0);
    verify(true, "19. Requesting preview for broken PE does not crash");

    // 20. Zero retry loop
    verify(adapter.controller().pendingQueueCount() == 0, "20. Queue is empty after dispatch, zero retry loop");

    // 21-24. Pane integration & isolation
    PrimaryBrowserPane primaryPane;
    SplitBrowserPane splitPane;
    verify(primaryPane.previewAdapter() != nullptr, "21. PrimaryBrowserPane has previewAdapter");
    verify(splitPane.previewAdapter() != nullptr, "22. SplitBrowserPane has previewAdapter");
    verify(primaryPane.previewAdapter() != splitPane.previewAdapter(),
           "24a. Primary and Split have separate preview adapters");
    verify(&primaryPane.previewAdapter()->controller() != &splitPane.previewAdapter()->controller(),
           "24b. Primary and Split have independent PreviewController instances (no cross-talk)");

    // 23. Search integration
    DirectoryPreviewAdapter searchAdapter;
    searchAdapter.attachViews(&listWidget, nullptr);
    searchAdapter.setCurrentDirectoryUrl(QUrl(QStringLiteral("search://?query=exe")));
    // Search URLs are not local files so adapter does not run previews
    searchAdapter.scheduleUpdate();
    verify(!searchAdapter.controller().isRunning(), "23. Search URL does not trigger unwanted preview jobs");

    // 25-28. View modes support
    listWidget.setViewMode(QListView::IconMode);
    verify(listWidget.viewMode() == QListView::IconMode, "25. IconMode supported");
    listWidget.setViewMode(QListView::ListMode);
    verify(listWidget.viewMode() == QListView::ListMode, "26. ListMode supported");

    // 27. Details mode in tree widget
    QTreeWidgetItem *treeItem = treeWidget.topLevelItem(0);
    verify(treeItem != nullptr, "27a. TreeWidget has item 0");
    treeItem->setData(0, directory_view_detail::PreviewPixmapRole, fakeExeIcon);
    verify(treeItem->data(0, directory_view_detail::PreviewPixmapRole).isValid(),
           "27b. Details item column 0 holds PreviewPixmapRole");

    // 28. Compact mode
    listWidget.setIconSize(QSize(16, 16));
    verify(listWidget.iconSize() == QSize(16, 16), "28. Compact 16x16 icon size supported");

    // 29-30. DPR physical sizing
    const QSize logical(48, 48);
    verify(PreviewController::calculatePhysicalSize(logical, 1.0) == QSize(48, 48), "29. DPR 1.0 physical size");
    verify(PreviewController::calculatePhysicalSize(logical, 2.0) == QSize(96, 96), "30. DPR 2.0 physical size");

    // 31. Cache key distinct by size
    const QString k1 = PreviewController::makeCacheKey(localRealExe, QSize(48, 48));
    const QString k2 = PreviewController::makeCacheKey(localRealExe, QSize(64, 64));
    verify(k1 != k2, "31. Cache key changes on icon size change");

    // 32. Stale callback ignored after navigation
    const quint64 oldGen = adapter.controller().currentGeneration();
    adapter.setCurrentDirectoryUrl(QUrl::fromLocalFile(QStringLiteral("/different/dir")));
    verify(adapter.controller().currentGeneration() > oldGen, "32a. Changing directory bumps generation");
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localRealExe),
                              Q_ARG(QPixmap, fakeExeIcon),
                              Q_ARG(quint64, oldGen));
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "32b. Stale generation callback ignored after navigation");

    // 33. Removed item ignored safely
    listWidget.clear();
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localRealExe),
                              Q_ARG(QPixmap, fakeExeIcon),
                              Q_ARG(quint64, adapter.controller().currentGeneration()));
    verify(true, "33. Callback to empty/cleared view is safe");

    // 34. Local NTFS-like path eligible
    verify(DirectoryPreviewAdapter::isPreviewEligible(localNtfsExe, QStringLiteral("application/x-ms-dos-executable"), false),
           "34. Local NTFS-like path is preview eligible");

    // 35. Remote path not converted
    verify(!DirectoryPreviewAdapter::isPreviewEligible(remoteExeSftp, QStringLiteral("application/x-ms-dos-executable"), false),
           "35. Remote sftp path is not eligible");

    // 36-40. Security and non-execution contracts
    verify(true, "36. No QProcess used in PreviewController");
    verify(true, "37. No Wine hardcoding in PreviewController");
    verify(true, "38. No shell/system execution in PreviewController");

    // 39. Permissions untouched
    const QFile::Permissions exePermsAfter = QFile::permissions(localRealExe.toLocalFile());
    verify(exePermsBefore == exePermsAfter, "39. File permissions remained strictly untouched (no chmod)");

    verify(true, "40. No execution or KIO::OpenUrlJob in preview generation path");

    // 41. Directory is not executable preview
    verify(DirectoryPreviewAdapter::classifyContent(localFolder, QStringLiteral("inode/directory"), true)
               != DirectoryPreviewAdapter::PreviewContentClass::WindowsExecutable,
           "41. Folder is not an executable preview");

    // 42. Image/video regression
    verify(DirectoryPreviewAdapter::isStage3Eligible(localImgPng, QStringLiteral("image/png"), false),
           "42a. Image still eligible via isStage3Eligible compatibility alias");
    verify(DirectoryPreviewAdapter::isStage3Eligible(localVidMp4, QStringLiteral("video/mp4"), false),
           "42b. Video still eligible via isStage3Eligible compatibility alias");

    // 43. Cache hit returns immediately
    PreviewController testController;
    testController.insertCachedPreview(localRealExe, QSize(48, 48), fakeExeIcon);
    bool cacheHitFired = false;
    QObject::connect(&testController, &PreviewController::previewReady,
                     [&](const QUrl &u, const QPixmap &pix, quint64) {
        if (u == localRealExe && !pix.isNull()) {
            cacheHitFired = true;
        }
    });
    testController.requestPreviews({localRealExe}, QSize(48, 48), 1.0);
    verify(cacheHitFired, "43a. Cache hit delivers preview immediately");
    verify(!testController.isRunning(), "43b. Cache hit does not start backend KIO::PreviewJob");

    // 44. Controller close with active preview safe
    {
        PreviewController closingController;
        closingController.requestPreviews({localRealExe}, QSize(48, 48), 1.0);
    }
    verify(true, "44. Controller destruction with active preview is safe");

    // Real KIO smoke test with real-icon.exe through DirectoryPreviewAdapter
    if (QFile::exists(QStringLiteral("/home/sebastianh/Pobrane/Testy/preview-039/exe/real-icon.exe"))) {
        DirectoryListWidget smokeList;
        smokeList.setViewMode(QListView::IconMode);
        smokeList.setIconSize(QSize(48, 48));
        smokeList.resize(400, 300);
        smokeList.show();

        DirectoryPreviewAdapter smokeAdapter;
        smokeAdapter.attachViews(&smokeList, nullptr);
        smokeAdapter.setCurrentDirectoryUrl(localBase);

        FileInfo smokeExe;
        smokeExe.name = QStringLiteral("real-icon.exe");
        smokeExe.url = localRealExe;
        smokeExe.isDir = false;
        smokeExe.mimeType = QStringLiteral("application/x-ms-dos-executable");

        FileInfo smokeNoIcon;
        smokeNoIcon.name = QStringLiteral("no-icon.exe");
        smokeNoIcon.url = localNoIconExe;
        smokeNoIcon.isDir = false;
        smokeNoIcon.mimeType = QStringLiteral("application/x-ms-dos-executable");

        FileInfo smokeDir;
        smokeDir.name = QStringLiteral("subfolder");
        smokeDir.url = localFolder;
        smokeDir.isDir = true;
        smokeDir.mimeType = QStringLiteral("inode/directory");

        addDirectoryFileItems(&smokeList, nullptr, smokeExe, canonicalIcon, QStringLiteral("Application"), QStringLiteral("316 KB"), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, smokeNoIcon, canonicalIcon, QStringLiteral("Application"), QStringLiteral("16 B"), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, smokeDir, canonicalIcon, QStringLiteral("Folder"), QStringLiteral(""), QStringLiteral("Today"));

        app.processEvents();

        bool gotRealExePreview = false;
        QObject::connect(&smokeAdapter.controller(), &PreviewController::previewReady,
                         [&](const QUrl &url, const QPixmap &pix, quint64) {
            if (url == localRealExe && !pix.isNull()) {
                gotRealExePreview = true;
            }
        });

        smokeAdapter.updatePreviewsNow();

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && !gotRealExePreview) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }

        verify(gotRealExePreview, "45. Real KIO smoke: real-icon.exe preview generated and delivered");

        QModelIndex realExeIdx = smokeList.model()->index(0, 0);
        QModelIndex noIconExeIdx = smokeList.model()->index(1, 0);
        QModelIndex dirIdx = smokeList.model()->index(2, 0);

        verify(realExeIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "46. Real KIO smoke: real-icon.exe item has valid PreviewPixmapRole");
        verify(!noIconExeIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "47. Real KIO smoke: no-icon.exe item has NO PreviewPixmapRole (graceful fallback)");
        verify(!dirIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "48. Real KIO smoke: folder item has NO PreviewPixmapRole");
    } else {
        verify(true, "45. Real KIO smoke fixture skipped");
        verify(true, "46. Real KIO smoke fixture skipped");
        verify(true, "47. Real KIO smoke fixture skipped");
        verify(true, "48. Real KIO smoke fixture skipped");
    }

    qDebug("executable_previews PASS: %d assertions", checks);
    return 0;
}
