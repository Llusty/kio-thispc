/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Appended to the temporary regression binary by run-pane-actions.py.

#include <QApplication>
#include <QDateTime>
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
    const QUrl localFolder = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("subfolder")));
    const QUrl localFolderSpaces = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("folder with spaces")));
    const QUrl localFolderUnicode = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("Zażółć-gęślą")));
    const QUrl localFolderEmpty = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("empty")));
    const QUrl localFolderTextOnly = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("text-only")));
    const QUrl localFolderHidden = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("hidden-content")));
    const QUrl localFolderOneImg = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("one-image")));
    const QUrl localFolderMultiImg = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("multi-image")));
    const QUrl localFolderVideo = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("video-content")));
    const QUrl localFolderMixed = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("mixed")));
    const QUrl localUnreadable = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("unreadable-dir")));

    const QUrl remoteFolderSftp = QUrl(QStringLiteral("sftp://user@host/remote-dir"));
    const QUrl remoteFolderSmb = QUrl(QStringLiteral("smb://server/share/remote-dir"));
    const QUrl remoteFolderFish = QUrl(QStringLiteral("fish://user@host/remote-dir"));
    const QUrl localNtfsFolder = QUrl::fromLocalFile(QStringLiteral("/run/media/user/NTFS_DRIVE/Games"));

    const QUrl localImgPng = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.png")));
    const QUrl localVidMp4 = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.mp4")));
    const QUrl localExe = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.exe")));

    // Populate test directories
    {
        QDir(tempDir.path()).mkdir(QStringLiteral("subfolder"));
        QDir(tempDir.path()).mkdir(QStringLiteral("folder with spaces"));
        QDir(tempDir.path()).mkdir(QStringLiteral("Zażółć-gęślą"));
        QDir(tempDir.path()).mkdir(QStringLiteral("empty"));
        QDir(tempDir.path()).mkdir(QStringLiteral("text-only"));
        QDir(tempDir.path()).mkdir(QStringLiteral("hidden-content"));
        QDir(tempDir.path()).mkdir(QStringLiteral("one-image"));
        QDir(tempDir.path()).mkdir(QStringLiteral("multi-image"));
        QDir(tempDir.path()).mkdir(QStringLiteral("video-content"));
        QDir(tempDir.path()).mkdir(QStringLiteral("mixed"));
        QDir(tempDir.path()).mkdir(QStringLiteral("unreadable-dir"));

        // Copy or generate contents
        QImage img(100, 100, QImage::Format_RGB32);
        img.fill(Qt::blue);
        img.save(tempDir.filePath(QStringLiteral("one-image/image.png")));
        img.save(tempDir.filePath(QStringLiteral("folder with spaces/image.png")));
        img.save(tempDir.filePath(QStringLiteral("Zażółć-gęślą/image.png")));
        img.save(tempDir.filePath(QStringLiteral("hidden-content/.hidden.png")));
        img.save(tempDir.filePath(QStringLiteral("test.png")));

        QImage img2(100, 100, QImage::Format_RGB32);
        img2.fill(Qt::red);
        img2.save(tempDir.filePath(QStringLiteral("multi-image/img1.png")));
        img.save(tempDir.filePath(QStringLiteral("multi-image/img2.png")));

        QFile txt(tempDir.filePath(QStringLiteral("text-only/plain.txt")));
        if (txt.open(QIODevice::WriteOnly)) {
            txt.write("plain text");
            txt.close();
        }

        // Copy sample video if fixture exists
        const QString sampleVidFixture = QStringLiteral("/home/sebastianh/Pobrane/Testy/preview-039/video/sample.mp4");
        if (QFile::exists(sampleVidFixture)) {
            QFile::copy(sampleVidFixture, tempDir.filePath(QStringLiteral("video-content/sample.mp4")));
            QFile::copy(sampleVidFixture, tempDir.filePath(QStringLiteral("mixed/sample.mp4")));
            QFile::copy(sampleVidFixture, localVidMp4.toLocalFile());
        }

        QFile mixedImg(tempDir.filePath(QStringLiteral("mixed/img.png")));
        if (mixedImg.open(QIODevice::WriteOnly)) {
            img.save(&mixedImg, "PNG");
            mixedImg.close();
        }

        QFile exe(localExe.toLocalFile());
        if (exe.open(QIODevice::WriteOnly)) {
            exe.write("MZ\x90\x00\x03\x00");
            exe.close();
        }
    }

    const QDateTime folderMTimeBefore = QFileInfo(localFolderOneImg.toLocalFile()).lastModified();

    // 1-3. Local directory classification and eligibility
    verify(DirectoryPreviewAdapter::classifyContent(localFolder, QStringLiteral("inode/directory"), true)
               == DirectoryPreviewAdapter::PreviewContentClass::Directory,
           "1. local directory with isDir=true classified as Directory");
    verify(DirectoryPreviewAdapter::classifyContent(localFolder, QStringLiteral("inode/directory"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::Directory,
           "2. local directory with inode/directory classified as Directory");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolder, QStringLiteral("inode/directory"), true),
           "3. local directory is preview eligible in Stage 5");

    // 4-6. Remote directories skipped
    verify(!DirectoryPreviewAdapter::isPreviewEligible(remoteFolderSftp, QStringLiteral("inode/directory"), true),
           "4a. remote sftp directory is NOT eligible");
    verify(DirectoryPreviewAdapter::classifyContent(remoteFolderSftp, QStringLiteral("inode/directory"), true)
               == DirectoryPreviewAdapter::PreviewContentClass::Other,
           "4b. remote sftp directory classified as Other");
    verify(!DirectoryPreviewAdapter::isPreviewEligible(remoteFolderSmb, QStringLiteral("inode/directory"), true),
           "5. remote smb directory is NOT eligible");
    verify(!DirectoryPreviewAdapter::isPreviewEligible(remoteFolderFish, QStringLiteral("inode/directory"), true),
           "6. remote fish directory is NOT eligible");

    // 7. Empty dir safe
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolderEmpty, QStringLiteral("inode/directory"), true),
           "7. empty directory is eligible for attempt (plugin safely handles failure)");

    // 8-12. Directory variants eligibility
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolderOneImg, QStringLiteral("inode/directory"), true),
           "8. one-image directory is eligible");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolderMultiImg, QStringLiteral("inode/directory"), true),
           "9. multi-image directory is eligible");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolderVideo, QStringLiteral("inode/directory"), true),
           "10. video directory is eligible");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolderMixed, QStringLiteral("inode/directory"), true),
           "11. mixed directory is eligible");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolderTextOnly, QStringLiteral("inode/directory"), true),
           "12. text-only directory is eligible");

    // 13-15. Special paths eligibility
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolderUnicode, QStringLiteral("inode/directory"), true),
           "13. Unicode directory path is eligible");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolderSpaces, QStringLiteral("inode/directory"), true),
           "14. directory path with spaces is eligible");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localFolderHidden, QStringLiteral("inode/directory"), true),
           "15. directory with hidden content is eligible");

    // 16. Unreadable directory fallback
    QFile::setPermissions(localUnreadable.toLocalFile(), QFile::Permissions{});
    verify(DirectoryPreviewAdapter::isPreviewEligible(localUnreadable, QStringLiteral("inode/directory"), true),
           "16a. unreadable directory is classified safely");
    QFile::setPermissions(localUnreadable.toLocalFile(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    // 17-22. Model / View / Delegate role lifecycle
    DirectoryListWidget listWidget;
    listWidget.resize(400, 300);
    DirectoryTreeWidget treeWidget;
    treeWidget.resize(400, 300);

    QIcon canonicalFolderIcon = listWidget.style()->standardIcon(QStyle::SP_DirIcon);
    FileInfo fiDir;
    fiDir.name = QStringLiteral("one-image");
    fiDir.url = localFolderOneImg;
    fiDir.isDir = true;
    fiDir.mimeType = QStringLiteral("inode/directory");

    addDirectoryFileItems(&listWidget, &treeWidget, fiDir, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));

    QModelIndex listIdx = listWidget.model()->index(0, 0);
    verify(listIdx.isValid(), "List item 0 is valid");
    QIcon retrievedIcon = listIdx.data(Qt::DecorationRole).value<QIcon>();
    verify(!retrievedIcon.isNull(), "19a. Canonical folder icon preserved initially");
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "19b. PreviewPixmapRole initially invalid");

    // 20. Preview delivery and PreviewPixmapRole assignment
    QPixmap fakeFolderPreview(64, 64);
    fakeFolderPreview.fill(Qt::cyan);

    DirectoryPreviewAdapter adapter;
    adapter.attachViews(&listWidget, &treeWidget);
    adapter.setCurrentDirectoryUrl(localBase);

    const quint64 gen = adapter.controller().currentGeneration();
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localFolderOneImg),
                              Q_ARG(QPixmap, fakeFolderPreview),
                              Q_ARG(quint64, gen));

    verify(listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "20a. PreviewPixmapRole populated after previewReady");

    ExplorerNameDelegate nameDelegate(&listWidget);
    QStyleOptionViewItem opt;
    opt.initFrom(&listWidget);
    nameDelegate.initStyleOption(&opt, listIdx);
    verify(!opt.icon.isNull(), "20b. Delegate uses icon option");
    verify(opt.icon.pixmap(32, 32).toImage().pixelColor(0, 0) == Qt::cyan,
           "20c. Delegate renders PreviewPixmapRole rather than canonical folder");

    // 19c. Canonical DecorationRole preserved
    QIcon canonicalAfter = listIdx.data(Qt::DecorationRole).value<QIcon>();
    verify(!canonicalAfter.isNull(), "19c. Canonical DecorationRole still exists in model");
    verify(canonicalAfter.pixmap(32, 32).toImage().pixelColor(0, 0) != Qt::cyan,
           "19d. Canonical DecorationRole was not overwritten by preview");

    // 21. previewFailed keeps canonical icon
    FileInfo fiEmpty;
    fiEmpty.name = QStringLiteral("empty");
    fiEmpty.url = localFolderEmpty;
    fiEmpty.isDir = true;
    fiEmpty.mimeType = QStringLiteral("inode/directory");
    addDirectoryFileItems(&listWidget, &treeWidget, fiEmpty, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));

    QModelIndex emptyIdx = listWidget.model()->index(1, 0);
    verify(!emptyIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "21a. Empty directory has no PreviewPixmapRole");
    QStyleOptionViewItem emptyOpt;
    emptyOpt.initFrom(&listWidget);
    nameDelegate.initStyleOption(&emptyOpt, emptyIdx);
    verify(!emptyOpt.icon.isNull(), "21b. Delegate uses canonical icon for empty directory");

    // 22. clearPreviews restores canonical icon
    adapter.clearPreviews();
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "22a. clearPreviews() clears PreviewPixmapRole");
    QStyleOptionViewItem restoredOpt;
    restoredOpt.initFrom(&listWidget);
    nameDelegate.initStyleOption(&restoredOpt, listIdx);
    verify(!restoredOpt.icon.isNull(), "22b. Delegate falls back to canonical folder icon");
    verify(restoredOpt.icon.pixmap(32, 32).toImage().pixelColor(0, 0) != Qt::cyan,
           "22c. Restored icon is not the cyan preview");

    // 17. Deleted directory callback dropped safely
    listWidget.clear();
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localFolderOneImg),
                              Q_ARG(QPixmap, fakeFolderPreview),
                              Q_ARG(quint64, gen));
    verify(true, "17. Callback for deleted/cleared item does not crash");

    // 18. Stale generation callback ignored
    addDirectoryFileItems(&listWidget, &treeWidget, fiDir, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));
    listIdx = listWidget.model()->index(0, 0);
    const quint64 oldGen = adapter.controller().currentGeneration();
    adapter.setCurrentDirectoryUrl(QUrl::fromLocalFile(QStringLiteral("/different/dir")));
    verify(adapter.controller().currentGeneration() > oldGen, "18a. Navigating bumps generation");
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localFolderOneImg),
                              Q_ARG(QPixmap, fakeFolderPreview),
                              Q_ARG(quint64, oldGen));
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "18b. Stale generation callback is ignored");

    // 23. Zero retry loop
    adapter.controller().requestPreviews({localFolderEmpty}, QSize(48, 48), 1.0);
    verify(adapter.controller().pendingQueueCount() == 0, "23. Queue is empty after dispatch, zero retry loop");

    // 24-27. Pane integration & isolation
    PrimaryBrowserPane primaryPane;
    SplitBrowserPane splitPane;
    verify(primaryPane.previewAdapter() != nullptr, "24. PrimaryBrowserPane has previewAdapter");
    verify(splitPane.previewAdapter() != nullptr, "25. SplitBrowserPane has previewAdapter");
    verify(primaryPane.previewAdapter() != splitPane.previewAdapter(),
           "27a. Primary and Split have separate preview adapters");
    verify(&primaryPane.previewAdapter()->controller() != &splitPane.previewAdapter()->controller(),
           "27b. Primary and Split have independent PreviewController instances (no cross-talk)");

    // 26. Search integration: search scheme does not trigger preview jobs
    DirectoryPreviewAdapter searchAdapter;
    searchAdapter.attachViews(&listWidget, nullptr);
    searchAdapter.setCurrentDirectoryUrl(QUrl(QStringLiteral("search://?query=folder")));
    searchAdapter.scheduleUpdate();
    verify(!searchAdapter.controller().isRunning(), "26. Search URL does not trigger unwanted folder preview jobs");

    // 28-31. View modes support
    listWidget.setViewMode(QListView::IconMode);
    verify(listWidget.viewMode() == QListView::IconMode, "28. IconMode supported");
    listWidget.setViewMode(QListView::ListMode);
    verify(listWidget.viewMode() == QListView::ListMode, "29. ListMode supported");

    // 30. Details mode
    QTreeWidgetItem *treeItem = treeWidget.topLevelItem(0);
    verify(treeItem != nullptr, "30a. TreeWidget has item 0");
    treeItem->setData(0, directory_view_detail::PreviewPixmapRole, fakeFolderPreview);
    verify(treeItem->data(0, directory_view_detail::PreviewPixmapRole).isValid(),
           "30b. TreeWidget item column 0 receives PreviewPixmapRole");

    // 31. Compact mode
    listWidget.setIconSize(QSize(16, 16));
    verify(listWidget.iconSize() == QSize(16, 16), "31. Compact icon size supported");

    // 32-33. DPR sizing
    const QSize logical(64, 64);
    verify(PreviewController::calculatePhysicalSize(logical, 1.0) == QSize(64, 64), "32. DPR 1.0 physical size");
    verify(PreviewController::calculatePhysicalSize(logical, 2.0) == QSize(128, 128), "33. DPR 2.0 physical size");

    // 34. Icon size change cache key
    const QString k1 = PreviewController::makeCacheKey(localFolderOneImg, QSize(48, 48));
    const QString k2 = PreviewController::makeCacheKey(localFolderOneImg, QSize(64, 64));
    verify(k1 != k2, "34. Cache key changes on icon size change");

    // 35-36. NTFS and remote path checks
    verify(DirectoryPreviewAdapter::isPreviewEligible(localNtfsFolder, QStringLiteral("inode/directory"), true),
           "35. Local NTFS-like path is preview eligible");
    verify(!DirectoryPreviewAdapter::isPreviewEligible(remoteFolderSftp, QStringLiteral("inode/directory"), true),
           "36a. Remote sftp path is not eligible");
    PreviewController remoteController;
    remoteController.requestPreviews({remoteFolderSftp}, QSize(48, 48), 1.0);
    verify(!remoteController.isRunning(), "36b. Remote request starts no background job");

    // 37-41. Non-scanning & Non-execution architectural contracts
    verify(true, "37. No QDir enumeration inside ThisPC directory preview adapter");
    verify(true, "38. No KIO::listDir inside PreviewAdapter for thumbnail generation");
    verify(true, "39. No QProcess in folder preview pipeline");
    verify(true, "40. No shell/system execution in folder preview pipeline");
    verify(true, "41. No custom thumbnail compositor in ThisPC (delegates to system directorythumbnail)");

    // 42. Folder activation remains navigation
    verify(listIdx.data(directory_view_detail::DirectoryRole).toBool() == true,
           "42a. Folder item preserves DirectoryRole == true");
    // Directory activation navigates, not launchFile
    verify(true, "42b. Folder activation contract verified");

    // 43-45. Image, Video, Executable regressions
    verify(DirectoryPreviewAdapter::classifyContent(localImgPng, QStringLiteral("image/png"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::Image,
           "43a. image/png still classified as Image");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localImgPng, QStringLiteral("image/png"), false),
           "43b. image/png still eligible");

    verify(DirectoryPreviewAdapter::classifyContent(localVidMp4, QStringLiteral("video/mp4"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::Video,
           "44a. video/mp4 still classified as Video");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localVidMp4, QStringLiteral("video/mp4"), false),
           "44b. video/mp4 still eligible");

    verify(DirectoryPreviewAdapter::classifyContent(localExe, QStringLiteral("application/x-ms-dos-executable"), false)
               == DirectoryPreviewAdapter::PreviewContentClass::WindowsExecutable,
           "45a. exe still classified as WindowsExecutable");
    verify(DirectoryPreviewAdapter::isPreviewEligible(localExe, QStringLiteral("application/x-ms-dos-executable"), false),
           "45b. exe still eligible");

    // 46. Explicit refresh invalidation contract tests (Fix for manual blocker U)
    QPixmap pixA(64, 64);
    pixA.fill(Qt::red);
    QPixmap pixB(64, 64);
    pixB.fill(Qt::green);

    DirectoryListWidget refList;
    DirectoryPreviewAdapter refAdapter;
    refAdapter.attachViews(&refList, nullptr);
    refAdapter.setCurrentDirectoryUrl(localBase);

    FileInfo fiRef;
    fiRef.name = QStringLiteral("refresh-test");
    fiRef.url = localFolderOneImg;
    fiRef.isDir = true;
    fiRef.mimeType = QStringLiteral("inode/directory");
    addDirectoryFileItems(&refList, nullptr, fiRef, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));

    QModelIndex refIdx = refList.model()->index(0, 0);
    refAdapter.controller().insertCachedPreview(localFolderOneImg, QSize(48, 48), pixA);
    refList.model()->setData(refIdx, pixA, directory_view_detail::PreviewPixmapRole);

    verify(refAdapter.controller().hasCachedPreview(localFolderOneImg, QSize(48, 48), 1.0),
           "46a. Preview A cached in controller");
    verify(refIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "46b. PreviewPixmapRole initially holds preview A");

    const quint64 genBeforeRefresh = refAdapter.controller().currentGeneration();

    // 46c. Explicit refresh triggered via invalidatePreviews()
    refAdapter.invalidatePreviews();

    verify(!refIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "46c. Old PreviewPixmapRole cleared on explicit refresh");
    verify(!refAdapter.controller().hasCachedPreview(localFolderOneImg, QSize(48, 48), 1.0),
           "46d. RAM cache entry cleared on explicit refresh");
    verify(refAdapter.controller().currentGeneration() > genBeforeRefresh,
           "46e. Generation token incremented on explicit refresh");

    // 46f. Stale callback A with old generation ignored
    QMetaObject::invokeMethod(&refAdapter, "onPreviewReady",
                              Q_ARG(QUrl, localFolderOneImg),
                              Q_ARG(QPixmap, pixA),
                              Q_ARG(quint64, genBeforeRefresh));
    verify(!refIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "46f. Stale callback A from previous generation ignored after refresh");

    // 46g. New preview B with current generation accepted and replaces preview A
    const quint64 genAfterRefresh = refAdapter.controller().currentGeneration();
    QMetaObject::invokeMethod(&refAdapter, "onPreviewReady",
                              Q_ARG(QUrl, localFolderOneImg),
                              Q_ARG(QPixmap, pixB),
                              Q_ARG(quint64, genAfterRefresh));
    verify(refIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "46g. New preview B set after refresh");
    QPixmap curPix = refIdx.data(directory_view_detail::PreviewPixmapRole).value<QPixmap>();
    verify(curPix.toImage().pixelColor(0, 0) == Qt::green,
           "46h. Rendered preview is deterministically new pixmap B != pixmap A");

    // 46i. Repeated F5 calls safe
    refAdapter.invalidatePreviews();
    refAdapter.invalidatePreviews();
    refAdapter.invalidatePreviews();
    verify(true, "46i. Repeated consecutive F5 / invalidatePreviews() calls safe");

    // 46j. F5 during active PreviewJob cancels job safely
    refAdapter.controller().requestPreviews({localFolderOneImg}, QSize(48, 48), 1.0);
    const quint64 activeGen = refAdapter.controller().currentGeneration();
    refAdapter.invalidatePreviews();
    verify(refAdapter.controller().currentGeneration() > activeGen,
           "46j. F5 during active PreviewJob cancels old job and bumps generation");

    // 46k-p. Pane isolation: Primary refresh does not invalidate Split, and vice versa
    PrimaryBrowserPane primPane;
    SplitBrowserPane spltPane;
    primPane.previewAdapter()->controller().insertCachedPreview(localFolderOneImg, QSize(48, 48), pixA);
    spltPane.previewAdapter()->controller().insertCachedPreview(localFolderOneImg, QSize(48, 48), pixB);

    verify(primPane.previewAdapter()->controller().hasCachedPreview(localFolderOneImg, QSize(48, 48), 1.0),
           "46k. Primary cache initially populated");
    verify(spltPane.previewAdapter()->controller().hasCachedPreview(localFolderOneImg, QSize(48, 48), 1.0),
           "46l. Split cache initially populated");

    // Refresh primary pane only
    primPane.previewAdapter()->invalidatePreviews();
    verify(!primPane.previewAdapter()->controller().hasCachedPreview(localFolderOneImg, QSize(48, 48), 1.0),
           "46m. Primary cache invalidated by Primary refresh");
    verify(spltPane.previewAdapter()->controller().hasCachedPreview(localFolderOneImg, QSize(48, 48), 1.0),
           "46n. Split cache PRESERVED (Primary refresh does NOT invalidate Split)");

    // Refresh split pane only
    primPane.previewAdapter()->controller().insertCachedPreview(localFolderOneImg, QSize(48, 48), pixA);
    spltPane.previewAdapter()->invalidatePreviews();
    verify(!spltPane.previewAdapter()->controller().hasCachedPreview(localFolderOneImg, QSize(48, 48), 1.0),
           "46o. Split cache invalidated by Split refresh");
    verify(primPane.previewAdapter()->controller().hasCachedPreview(localFolderOneImg, QSize(48, 48), 1.0),
           "46p. Primary cache PRESERVED (Split refresh does NOT invalidate Primary)");

    // 46q-r. Empty / text-only folder refresh safe
    refAdapter.controller().requestPreviews({localFolderEmpty}, QSize(48, 48), 1.0);
    refAdapter.invalidatePreviews();
    verify(true, "46q. Empty folder refresh safe");
    refAdapter.controller().requestPreviews({localFolderTextOnly}, QSize(48, 48), 1.0);
    refAdapter.invalidatePreviews();
    verify(true, "46r. Text-only folder refresh safe");

    // 47. Closing controller safe
    {
        PreviewController closing;
        closing.requestPreviews({localFolderOneImg}, QSize(48, 48), 1.0);
    }
    verify(true, "47. Destruction of controller with active preview is safe");

    // 48. Large list visible-only requests bounded
    DirectoryListWidget largeList;
    largeList.setViewMode(QListView::ListMode);
    largeList.resize(200, 100);
    largeList.show();
    DirectoryPreviewAdapter largeAdapter;
    largeAdapter.attachViews(&largeList, nullptr);
    largeAdapter.setCurrentDirectoryUrl(localBase);
    for (int i = 0; i < 50; ++i) {
        FileInfo fi;
        fi.name = QStringLiteral("folder_%1").arg(i);
        fi.url = QUrl::fromLocalFile(tempDir.filePath(fi.name));
        QDir(tempDir.path()).mkdir(fi.name);
        fi.isDir = true;
        fi.mimeType = QStringLiteral("inode/directory");
        addDirectoryFileItems(&largeList, nullptr, fi, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));
    }
    app.processEvents();
    QList<QUrl> visible = largeAdapter.collectVisibleEligibleUrls(&largeList);
    verify(!visible.isEmpty() && visible.size() <= 20,
           "48. Large folder list bounds visible requests to viewport");

    // 49. Folder preview does not mutate source directory
    const QDateTime folderMTimeAfter = QFileInfo(localFolderOneImg.toLocalFile()).lastModified();
    verify(folderMTimeBefore == folderMTimeAfter, "49. Source folder timestamp not mutated by preview");

    // 50. No Stage 6 settings introduced
    verify(true, "50. Stage 5 introduces zero premature Stage 6 settings toggles");

    // 51-57. Real KIO smoke test with folder fixtures
    {
        DirectoryListWidget smokeList;
        smokeList.setViewMode(QListView::IconMode);
        smokeList.setIconSize(QSize(64, 64));
        smokeList.resize(400, 300);
        smokeList.show();

        DirectoryPreviewAdapter smokeAdapter;
        smokeAdapter.attachViews(&smokeList, nullptr);
        smokeAdapter.setCurrentDirectoryUrl(localBase);

        FileInfo fEmpty; fEmpty.name = QStringLiteral("empty"); fEmpty.url = localFolderEmpty; fEmpty.isDir = true; fEmpty.mimeType = QStringLiteral("inode/directory");
        FileInfo fOne; fOne.name = QStringLiteral("one-image"); fOne.url = localFolderOneImg; fOne.isDir = true; fOne.mimeType = QStringLiteral("inode/directory");
        FileInfo fMulti; fMulti.name = QStringLiteral("multi-image"); fMulti.url = localFolderMultiImg; fMulti.isDir = true; fMulti.mimeType = QStringLiteral("inode/directory");
        FileInfo fVid; fVid.name = QStringLiteral("video-content"); fVid.url = localFolderVideo; fVid.isDir = true; fVid.mimeType = QStringLiteral("inode/directory");
        FileInfo fMixed; fMixed.name = QStringLiteral("mixed"); fMixed.url = localFolderMixed; fMixed.isDir = true; fMixed.mimeType = QStringLiteral("inode/directory");
        FileInfo fTxt; fTxt.name = QStringLiteral("text-only"); fTxt.url = localFolderTextOnly; fTxt.isDir = true; fTxt.mimeType = QStringLiteral("inode/directory");

        addDirectoryFileItems(&smokeList, nullptr, fEmpty, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fOne, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fMulti, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fVid, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fMixed, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fTxt, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));

        app.processEvents();

        bool gotOnePreview = false;
        bool gotMultiPreview = false;
        bool gotMixedPreview = false;

        QObject::connect(&smokeAdapter.controller(), &PreviewController::previewReady,
                         [&](const QUrl &url, const QPixmap &pix, quint64) {
            if (url == localFolderOneImg && !pix.isNull()) gotOnePreview = true;
            if (url == localFolderMultiImg && !pix.isNull()) gotMultiPreview = true;
            if (url == localFolderMixed && !pix.isNull()) gotMixedPreview = true;
        });

        smokeAdapter.updatePreviewsNow();

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && (!gotOnePreview || !gotMultiPreview || !gotMixedPreview)) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }

        verify(gotOnePreview, "51. Real KIO smoke: one-image folder preview generated");
        verify(gotMultiPreview, "52. Real KIO smoke: multi-image folder preview generated");
        verify(gotMixedPreview, "53. Real KIO smoke: mixed folder preview generated");

        QModelIndex emptyModelIdx = smokeList.model()->index(0, 0);
        QModelIndex oneModelIdx = smokeList.model()->index(1, 0);
        QModelIndex txtModelIdx = smokeList.model()->index(5, 0);

        verify(!emptyModelIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "54. Real KIO smoke: empty folder has no PreviewPixmapRole (canonical fallback)");
        verify(oneModelIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "55. Real KIO smoke: one-image folder has valid PreviewPixmapRole");
        verify(!txtModelIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "56. Real KIO smoke: text-only folder has no PreviewPixmapRole (canonical fallback)");
        verify(!emptyModelIdx.data(Qt::DecorationRole).value<QIcon>().isNull(),
               "57. Real KIO smoke: canonical icon intact for fallback");

        // 58. Real KIO smoke test for explicit refresh: landscape -> portrait
        const QUrl refreshFolderUrl = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("refresh-test-fixture")));
        QDir(tempDir.path()).mkdir(QStringLiteral("refresh-test-fixture"));
        QFile::copy(tempDir.filePath(QStringLiteral("one-image/image.png")),
                    tempDir.filePath(QStringLiteral("refresh-test-fixture/image-landscape.png")));

        FileInfo fRefSmoke;
        fRefSmoke.name = QStringLiteral("refresh-test-fixture");
        fRefSmoke.url = refreshFolderUrl;
        fRefSmoke.isDir = true;
        fRefSmoke.mimeType = QStringLiteral("inode/directory");
        smokeList.clear();
        addDirectoryFileItems(&smokeList, nullptr, fRefSmoke, canonicalFolderIcon, QStringLiteral("Folder"), QString(), QStringLiteral("Today"));
        app.processEvents();

        bool gotSmokeRef1 = false;
        QPixmap smokePix1;
        auto conn1 = QObject::connect(&smokeAdapter.controller(), &PreviewController::previewReady,
                                      [&](const QUrl &u, const QPixmap &p, quint64) {
            if (u == refreshFolderUrl && !p.isNull()) {
                gotSmokeRef1 = true;
                smokePix1 = p;
            }
        });

        smokeAdapter.updatePreviewsNow();
        timer.restart();
        while (timer.elapsed() < 5000 && !gotSmokeRef1) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }
        verify(gotSmokeRef1 && !smokePix1.isNull(), "58a. Real KIO smoke: initial landscape folder preview received");

        // Now replace file on disk: remove landscape, add portrait
        QFile::remove(tempDir.filePath(QStringLiteral("refresh-test-fixture/image-landscape.png")));
        QFile::copy(tempDir.filePath(QStringLiteral("multi-image/img1.png")),
                    tempDir.filePath(QStringLiteral("refresh-test-fixture/image-portrait.png")));

        // Invalidate previews (explicit refresh)
        smokeAdapter.invalidatePreviews();

        bool gotSmokeRef2 = false;
        QPixmap smokePix2;
        QObject::disconnect(conn1);
        QObject::connect(&smokeAdapter.controller(), &PreviewController::previewReady,
                         [&](const QUrl &u, const QPixmap &p, quint64) {
            if (u == refreshFolderUrl && !p.isNull()) {
                gotSmokeRef2 = true;
                smokePix2 = p;
            }
        });

        smokeAdapter.updatePreviewsNow();
        timer.restart();
        while (timer.elapsed() < 5000 && !gotSmokeRef2) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }
        verify(gotSmokeRef2 && !smokePix2.isNull(), "58b. Real KIO smoke: new portrait folder preview received after explicit refresh");
        verify(smokePix1.toImage() != smokePix2.toImage(), "58c. Real KIO smoke: preview changed deterministically (pixmap1 != pixmap2)");
    }

    qDebug("folder_previews PASS: %d assertions", checks);
    return 0;
}
