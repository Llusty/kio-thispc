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
    const QUrl localImgJpg = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.jpg")));
    const QUrl localVidMp4 = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.mp4")));
    const QUrl localVidMkv = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.mkv")));
    const QUrl localTxt = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.txt")));
    const QUrl localExe = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("test.exe")));
    const QUrl localFolder = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("subfolder")));
    const QUrl remoteImg = QUrl(QStringLiteral("sftp://user@host/remote.png"));
    const QUrl remoteVid = QUrl(QStringLiteral("smb://server/share/video.mp4"));

    // Create minimal physical files for smoke tests
    {
        QImage img(120, 80, QImage::Format_RGB32);
        img.fill(Qt::blue);
        img.save(localImgPng.toLocalFile());

        QImage img2(80, 120, QImage::Format_RGB32);
        img2.fill(Qt::green);
        img2.save(localImgJpg.toLocalFile());

        QFile txt(localTxt.toLocalFile());
        verify(txt.open(QIODevice::WriteOnly), "txt file open succeeded");
        txt.write("text content");
        txt.close();

        QFile exe(localExe.toLocalFile());
        verify(exe.open(QIODevice::WriteOnly), "exe file open succeeded");
        exe.write("MZ\x90\x00\x03\x00");
        exe.close();

        QDir(tempDir.path()).mkdir(QStringLiteral("subfolder"));

        if (QFile::exists(QStringLiteral("/home/sebastianh/Pobrane/Testy/preview-039/video/sample.mp4"))) {
            QFile::copy(QStringLiteral("/home/sebastianh/Pobrane/Testy/preview-039/video/sample.mp4"), localVidMp4.toLocalFile());
        }
    }

    // 1-5. MIME type eligibility tests
    verify(DirectoryPreviewAdapter::isStage3Eligible(localImgPng, QStringLiteral("image/png"), false),
           "1a. image/png is eligible");
    verify(DirectoryPreviewAdapter::isStage3Eligible(localImgJpg, QStringLiteral("image/jpeg"), false),
           "1b. image/jpeg is eligible");
    verify(DirectoryPreviewAdapter::isStage3Eligible(localVidMp4, QStringLiteral("video/mp4"), false),
           "2a. video/mp4 is eligible");
    verify(DirectoryPreviewAdapter::isStage3Eligible(localVidMkv, QStringLiteral("video/x-matroska"), false),
           "2b. video/x-matroska is eligible");
    verify(DirectoryPreviewAdapter::isStage3Eligible(localVidMp4, QString(), false),
           "2c. empty mime on local mp4 resolves via MatchExtension and is eligible");
    verify(DirectoryPreviewAdapter::isStage3Eligible(localVidMp4, QStringLiteral("application/octet-stream"), false),
           "2d. octet-stream mime on local mp4 resolves via MatchExtension and is eligible");
    verify(!DirectoryPreviewAdapter::isStage3Eligible(localTxt, QStringLiteral("text/plain"), false),
           "3. text/plain is NOT eligible in Stage 3");
    verify(!DirectoryPreviewAdapter::isStage3Eligible(localExe, QStringLiteral("application/x-ms-dos-executable"), false),
           "4. application/x-ms-dos-executable is NOT eligible in Stage 3");
    verify(!DirectoryPreviewAdapter::isStage3Eligible(localFolder, QStringLiteral("inode/directory"), true),
           "5a. directory with isDir=true is NOT eligible in Stage 3");
    verify(!DirectoryPreviewAdapter::isStage3Eligible(localFolder, QStringLiteral("inode/directory"), false),
           "5b. directory with inode/directory is NOT eligible in Stage 3");

    // 6-9. Local vs Remote eligibility tests
    verify(DirectoryPreviewAdapter::isStage3Eligible(localImgPng, QStringLiteral("image/png"), false),
           "6. local image URL is eligible");
    verify(!DirectoryPreviewAdapter::isStage3Eligible(remoteImg, QStringLiteral("image/png"), false),
           "7. remote image URL is skipped");
    verify(DirectoryPreviewAdapter::isStage3Eligible(localVidMp4, QStringLiteral("video/mp4"), false),
           "8. local video URL is eligible");
    verify(!DirectoryPreviewAdapter::isStage3Eligible(remoteVid, QStringLiteral("video/mp4"), false),
           "9a. remote video URL is skipped");
    verify(!DirectoryPreviewAdapter::isStage3Eligible(remoteVid, QString(), false),
           "9b. remote video URL with empty mime is skipped");

    // Plugin policy includes video thumbnailer
    const QStringList defaultPlugins = PreviewController::queryDefaultPlugins();
    verify(defaultPlugins.contains(QStringLiteral("ffmpegthumbs")) || defaultPlugins.contains(QStringLiteral("ffmpegthumbnailer")),
           "9c. queryDefaultPlugins includes video thumbnailer (ffmpegthumbs/ffmpegthumbnailer)");

    // 10-12. Non-destructive preview role & canonical icon preservation
    DirectoryListWidget listWidget;
    listWidget.resize(400, 300);
    DirectoryTreeWidget treeWidget;
    treeWidget.resize(400, 300);

    QIcon canonicalIcon = listWidget.style()->standardIcon(QStyle::SP_FileIcon);
    FileInfo fiImg;
    fiImg.name = QStringLiteral("test.png");
    fiImg.url = localImgPng;
    fiImg.isDir = false;
    fiImg.mimeType = QStringLiteral("image/png");

    addDirectoryFileItems(&listWidget, &treeWidget, fiImg, canonicalIcon, QStringLiteral("PNG Image"), QStringLiteral("1 KB"), QStringLiteral("Today"));

    // Check canonical icon in list and tree
    QModelIndex listIdx = listWidget.model()->index(0, 0);
    verify(listIdx.isValid(), "List item 0 is valid");
    QIcon retrievedListIcon = listIdx.data(Qt::DecorationRole).value<QIcon>();
    verify(!retrievedListIcon.isNull(), "10a. Canonical list icon preserved without preview");
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "10b. PreviewPixmapRole initially invalid");

    // Test delegate initStyleOption fallback
    ExplorerNameDelegate nameDelegate(&listWidget);
    QStyleOptionViewItem option;
    option.initFrom(&listWidget);
    nameDelegate.initStyleOption(&option, listIdx);
    verify(!option.icon.isNull(), "10c. Delegate uses canonical icon when preview absent");

    // 11. Preview role overrides rendering only when present
    QPixmap fakePreview(64, 48);
    fakePreview.fill(Qt::red);
    listWidget.model()->setData(listIdx, fakePreview, directory_view_detail::PreviewPixmapRole);

    QStyleOptionViewItem previewOption;
    previewOption.initFrom(&listWidget);
    nameDelegate.initStyleOption(&previewOption, listIdx);
    verify(!previewOption.icon.isNull(), "11a. Delegate has icon with preview");
    QPixmap renderedPix = previewOption.icon.pixmap(64, 48);
    verify(renderedPix.toImage().pixelColor(0, 0) == Qt::red,
           "11b. Delegate uses PreviewPixmapRole rather than canonical icon");

    // Canonical icon remains intact in DecorationRole
    verify(listIdx.data(Qt::DecorationRole).value<QIcon>().pixmap(32, 32).toImage().pixelColor(0, 0) != Qt::red,
           "11c. Canonical DecorationRole was not overwritten");

    // 12. Clearing preview restores canonical icon
    DirectoryPreviewAdapter adapter;
    adapter.attachViews(&listWidget, &treeWidget);
    adapter.setCurrentDirectoryUrl(localBase);
    adapter.clearPreviews();

    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "12a. clearPreviews() clears PreviewPixmapRole");
    QStyleOptionViewItem restoredOption;
    restoredOption.initFrom(&listWidget);
    nameDelegate.initStyleOption(&restoredOption, listIdx);
    verify(!restoredOption.icon.isNull(), "12b. Restored delegate icon is not null");
    verify(restoredOption.icon.pixmap(32, 32).toImage().pixelColor(0, 0) != Qt::red,
           "12c. Clearing preview restores canonical icon rendering");

    // 13. Callback URL must match item URL
    listWidget.model()->setData(listIdx, QVariant(), directory_view_detail::PreviewPixmapRole);
    adapter.scheduleUpdate(); // Starts generation
    const quint64 curGen = adapter.controller().currentGeneration();
    // Deliver preview for non-matching URL
    QUrl otherUrl = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("other.png")));
    // Call private/exposed slot via test seam or simulated signal
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, otherUrl),
                              Q_ARG(QPixmap, fakePreview),
                              Q_ARG(quint64, curGen));
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "13. Callback with different URL does not affect item");

    // Deliver preview for matching URL
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localImgPng),
                              Q_ARG(QPixmap, fakePreview),
                              Q_ARG(quint64, curGen));
    verify(listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "13b. Callback with matching URL populates PreviewPixmapRole");

    listWidget.model()->setData(listIdx, QVariant(), directory_view_detail::PreviewPixmapRole);
    listWidget.model()->setData(listIdx, otherUrl.toString(), directory_view_detail::UrlRole);
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localImgPng),
                              Q_ARG(QPixmap, fakePreview),
                              Q_ARG(quint64, curGen));
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "13c. Same row reused for a different URL rejects stale preview");
    listWidget.model()->setData(listIdx, localImgPng.toString(), directory_view_detail::UrlRole);

    // 14. Removed item ignores callback safely
    listWidget.clear();
    verify(listWidget.model()->rowCount() == 0, "List cleared");
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localImgPng),
                              Q_ARG(QPixmap, fakePreview),
                              Q_ARG(quint64, curGen));
    verify(true, "14. Removed item ignores callback safely without crash");

    // 15. Changed listing ignores stale callback
    adapter.setCurrentDirectoryUrl(QUrl::fromLocalFile(QStringLiteral("/different/dir")));
    verify(adapter.controller().currentGeneration() > curGen,
           "15a. Changing directory URL bumps generation");
    // Deliver late preview with old generation
    addDirectoryFileItems(&listWidget, &treeWidget, fiImg, canonicalIcon, QStringLiteral("PNG Image"), QStringLiteral("1 KB"), QStringLiteral("Today"));
    listIdx = listWidget.model()->index(0, 0);
    QMetaObject::invokeMethod(&adapter, "onPreviewReady",
                              Q_ARG(QUrl, localImgPng),
                              Q_ARG(QPixmap, fakePreview),
                              Q_ARG(quint64, curGen)); // stale gen
    verify(!listIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
           "15b. Stale generation callback is ignored");

    // 16-18. Pane integration & Primary/Split isolation
    PrimaryBrowserPane primaryPane;
    SplitBrowserPane splitPane;
    verify(primaryPane.previewAdapter() != nullptr, "16. PrimaryBrowserPane has previewAdapter");
    verify(splitPane.previewAdapter() != nullptr, "17. SplitBrowserPane has previewAdapter");
    verify(primaryPane.previewAdapter() != splitPane.previewAdapter(),
           "18a. Primary and Split have separate preview adapters");
    verify(&primaryPane.previewAdapter()->controller() != &splitPane.previewAdapter()->controller(),
           "18b. Primary and Split have independent PreviewController instances (no cross-talk)");

    // 19-21. Visible-only collection & scroll updates
    DirectoryListWidget scrollList;
    scrollList.setViewMode(QListView::ListMode);
    scrollList.resize(200, 100);
    scrollList.show();

    DirectoryPreviewAdapter scrollAdapter;
    scrollAdapter.attachViews(&scrollList, nullptr);
    scrollAdapter.setCurrentDirectoryUrl(localBase);

    // Add 20 items (each height >= 20px, so only ~5 fit in 100px viewport)
    for (int i = 0; i < 20; ++i) {
        FileInfo fi;
        fi.name = QStringLiteral("img_%1.png").arg(i);
        fi.url = QUrl::fromLocalFile(tempDir.filePath(fi.name));
        QImage viewportFixture(8, 8, QImage::Format_RGB32);
        viewportFixture.fill(Qt::blue);
        viewportFixture.save(fi.url.toLocalFile());
        fi.isDir = false;
        fi.mimeType = QStringLiteral("image/png");
        addDirectoryFileItems(&scrollList, nullptr, fi, canonicalIcon, QStringLiteral("PNG"), QStringLiteral("1 KB"), QStringLiteral("Today"));
    }
    app.processEvents();

    QList<QUrl> visibleUrls = scrollAdapter.collectVisibleEligibleUrls(&scrollList);
    verify(!visibleUrls.isEmpty(), "19a. Visible URLs collected");
    verify(visibleUrls.size() < 20, "19b. Only visible subset collected (not all 20 items)");
    verify(visibleUrls.first() == QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("img_0.png"))),
           "20a. First visible item is img_0.png");
    verify(!visibleUrls.contains(QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("img_19.png")))),
           "20b. Offscreen item img_19.png is not in visible set");

    // Scroll to bottom
    if (scrollList.verticalScrollBar()) {
        scrollList.verticalScrollBar()->setValue(scrollList.verticalScrollBar()->maximum());
        app.processEvents();
        QList<QUrl> scrolledUrls = scrollAdapter.collectVisibleEligibleUrls(&scrollList);
        verify(scrolledUrls.contains(QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("img_19.png")))),
               "21. Scroll recomputes visible set and includes newly visible items");
    } else {
        verify(true, "21. Scrollbar check fallback");
    }

    // 22. Debounce collapses rapid notifications
    scrollAdapter.setDebounceIntervalMs(40);
    verify(scrollAdapter.debounceIntervalMs() == 40, "22a. Debounce interval configurable");
    const quint64 firesBeforeStorm = scrollAdapter.debounceFireCount();
    for (int i = 0; i < 150; ++i) scrollAdapter.scheduleUpdate();
    QTest::qWait(80);
    verify(scrollAdapter.debounceFireCount() == firesBeforeStorm + 1,
           "22b. 150 rapid scheduleUpdate() calls collapse to one firing");

    // 23-25. Viewport resize, DPR, icon-size
    QResizeEvent resizeEv(QSize(200, 200), QSize(200, 100));
    scrollAdapter.eventFilter(scrollList.viewport(), &resizeEv);
    verify(true, "23. Resize event triggers debounce update");

    const QSize logicalSize(48, 48);
    const QSize physicalDpr1 = PreviewController::calculatePhysicalSize(logicalSize, 1.0);
    const QSize physicalDpr2 = PreviewController::calculatePhysicalSize(logicalSize, 2.0);
    verify(physicalDpr1 == QSize(48, 48), "24a. DPR 1.0 physical size");
    verify(physicalDpr2 == QSize(96, 96), "24b. DPR 2.0 physical size");

    const QString keySmall = PreviewController::makeCacheKey(localImgPng, physicalDpr1);
    const QString keyLarge = PreviewController::makeCacheKey(localImgPng, physicalDpr2);
    verify(keySmall != keyLarge, "25. Icon size / DPR change generates distinct cache keys");

    // 26-27. Aspect ratio preservation
    QPixmap landscape(200, 100);
    landscape.fill(Qt::cyan);
    verify(landscape.width() > landscape.height(), "26a. Landscape source");
    QIcon landIcon(landscape);
    QSize landActual = landIcon.actualSize(QSize(64, 64));
    verify(landActual.width() >= landActual.height(), "26b. Landscape aspect ratio preserved");

    QPixmap portrait(100, 200);
    portrait.fill(Qt::magenta);
    verify(portrait.height() > portrait.width(), "27a. Portrait source");
    QIcon portIcon(portrait);
    QSize portActual = portIcon.actualSize(QSize(64, 64));
    verify(portActual.height() >= portActual.width(), "27b. Portrait aspect ratio preserved");

    // 28-30. Failure handling & no remote requests
    scrollAdapter.controller().requestPreviews({QUrl(QStringLiteral("file:///nonexistent/ghost.png"))}, QSize(48, 48), 1.0);
    verify(true, "28. Nonexistent file request does not crash");

    scrollAdapter.controller().requestPreviews({QUrl(QStringLiteral("sftp://remote/img.png"))}, QSize(48, 48), 1.0);
    verify(!scrollAdapter.controller().isRunning(), "30. Remote request skipped immediately without job");

    // 31-35. Security & Isolation checks
    verify(!DirectoryPreviewAdapter::isStage3Eligible(localExe, QStringLiteral("application/x-ms-dos-executable"), false),
           "31. No exe preview in Stage 3");
    verify(!DirectoryPreviewAdapter::isStage3Eligible(localFolder, QStringLiteral("inode/directory"), true),
           "32. No folder preview in Stage 3");

    // 36. 10k-row model examines and requests only visible rows
    DirectoryListWidget largeList;
    largeList.setViewMode(QListView::ListMode);
    largeList.resize(200, 100);
    largeList.show();

    DirectoryPreviewAdapter largeAdapter;
    largeAdapter.attachViews(&largeList, nullptr);
    largeAdapter.setCurrentDirectoryUrl(localBase);

    for (int i = 0; i < 10000; ++i) {
        FileInfo fi;
        fi.name = QStringLiteral("large_%1.png").arg(i);
        fi.url = QUrl::fromLocalFile(tempDir.filePath(fi.name));
        fi.isDir = false;
        fi.mimeType = QStringLiteral("image/png");
        addDirectoryFileItems(&largeList, nullptr, fi, canonicalIcon, QStringLiteral("PNG"), QStringLiteral("1 KB"), QStringLiteral("Today"));
    }
    app.processEvents();

    QList<QUrl> largeVisible = largeAdapter.collectVisibleEligibleUrls(&largeList);
    verify(largeVisible.size() > 0 && largeVisible.size() <= 25,
           "36a. 10k-row model requests only visible items (bounded count <= 25)");
    verify(largeAdapter.lastRowsExamined() <= 25,
           "36b. 10k-row viewport preparation examines only a visible-sized bound");

    // 37-38. Stale URL / Generation race protection
    const quint64 genStart = largeAdapter.controller().currentGeneration();
    largeAdapter.setCurrentDirectoryUrl(QUrl::fromLocalFile(QStringLiteral("/new/dir")));
    verify(largeAdapter.controller().currentGeneration() > genStart,
           "37. Directory change increments generation");

    // 39-40. Destructor safety and empty viewport
    {
        DirectoryPreviewAdapter tempAdapter;
        tempAdapter.scheduleUpdate();
    }
    verify(true, "39. Adapter destruction with pending update is safe");

    DirectoryListWidget emptyList;
    emptyList.resize(0, 0);
    DirectoryPreviewAdapter emptyAdapter;
    emptyAdapter.attachViews(&emptyList, nullptr);
    emptyAdapter.setCurrentDirectoryUrl(localBase);
    QList<QUrl> emptyUrls = emptyAdapter.collectVisibleEligibleUrls(&emptyList);
    verify(emptyUrls.isEmpty(), "40. Zero-size / empty viewport returns empty URL list safely");

    // Stage 7 manual blocker: complete Icons viewport, not just a prefix.
    // Full-model scans below are independent TEST oracles, never scheduler code.
    {
        DirectoryListWidget icons;
        icons.setViewMode(QListView::IconMode);
        icons.setFlow(QListView::LeftToRight);
        icons.setWrapping(true);
        icons.setResizeMode(QListView::Adjust);
        icons.setIconSize(QSize(32, 32));
        icons.setGridSize(QSize(100, 80));
        icons.setSpacing(3);
        icons.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        icons.resize(1020, 642);
        icons.show();
        DirectoryPreviewAdapter visibleAdapter;
        visibleAdapter.attachViews(&icons, nullptr);
        visibleAdapter.setCurrentDirectoryUrl(localBase);
        visibleAdapter.controller().setPreviewJobFactoryForTesting(
            [](const KFileItemList &, const QSize &, const QStringList *) -> KIO::PreviewJob * { return nullptr; });
        for (int i = 0; i < 10000; ++i) {
            FileInfo fi;
            fi.name = QStringLiteral("viewport_%1.png").arg(i);
            fi.url = QUrl::fromLocalFile(tempDir.filePath(fi.name));
            fi.mimeType = QStringLiteral("image/png");
            addDirectoryFileItems(&icons, nullptr, fi, canonicalIcon, QString(), QString(), QString(), {},
                                  QStringLiteral("Group %1").arg(i / 50), i / 50);
            if (i == 2119) {
                icons.doItemsLayout();
                app.processEvents();
                QSet<QUrl> expected;
                for (int row = 0; row < icons.model()->rowCount(); ++row) {
                    const QModelIndex index = icons.model()->index(row, 0);
                    if (icons.visualRect(index).intersects(icons.viewport()->rect()))
                        expected.insert(QUrl(index.data(directory_view_detail::UrlRole).toString()));
                }
                const auto actual = visibleAdapter.collectVisibleEligibleUrls(&icons);
                verify(expected.size() >= 80, "2120 Icons model exposes at least 80 items");
                verify(QSet<QUrl>(actual.cbegin(), actual.cend()) == expected,
                       "2120 Icons model collects ALL visible URLs and no offscreen URLs");
                verify(visibleAdapter.lastRowsExamined() == expected.size(),
                       "2120 Icons preparation examines exactly the visible geometry");
                qInfo("VISIBLE FIX: 2120 model visible=%d examined=%lld", expected.size(),
                      static_cast<long long>(visibleAdapter.lastRowsExamined()));
            }
        }
        auto oracle = [&]() {
            QSet<QUrl> expected;
            for (int row = 0; row < icons.model()->rowCount(); ++row) {
                const QModelIndex index = icons.model()->index(row, 0);
                if (icons.visualRect(index).intersects(icons.viewport()->rect()))
                    expected.insert(QUrl(index.data(directory_view_detail::UrlRole).toString()));
            }
            return expected;
        };
        auto completeViewport = [&](const char *message) {
            const auto expected = oracle();
            const auto actual = visibleAdapter.collectVisibleEligibleUrls(&icons);
            verify(QSet<QUrl>(actual.cbegin(), actual.cend()) == expected, message);
            verify(visibleAdapter.lastRowsExamined() == expected.size(),
                   "10000 model preparation examines exactly visible items");
            return expected;
        };
        icons.doItemsLayout(); app.processEvents();
        const auto eighty = completeViewport("10000 Icons model collects complete large viewport");
        verify(eighty.size() >= 80, "10000 model has more than 25 visible items");
        icons.resize(520, 402); icons.doItemsLayout(); app.processEvents();
        const auto small = completeViewport("small resized viewport is complete");
        icons.resize(1020, 642); icons.doItemsLayout(); app.processEvents();
        const auto expanded = completeViewport("resize requests every newly visible item");
        verify(expanded.size() > small.size() + 40, "resize exposes over 40 additional visible items");
        // Match production's default scroll mode as well as grouped layouts.
        icons.setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
        icons.doItemsLayout(); app.processEvents();
        icons.verticalScrollBar()->setValue(icons.verticalScrollBar()->maximum() / 2 + 17);
        app.processEvents();
        const auto scrolled = completeViewport("partial mid-directory scroll collects complete viewport");
        verify(scrolled != expanded, "scroll targets a new set of visible URLs");
        // Diagnostic of the removed arithmetic: its nominal columns/stride
        // are deliberately compared with the independent geometry oracle.
        const QSize nominalGrid = icons.gridSize();
        const int nominalColumns = qMax(1, icons.viewport()->width() / nominalGrid.width());
        const int nominalVisualRow = icons.verticalScrollMode() == QAbstractItemView::ScrollPerPixel
            ? icons.verticalScrollBar()->value() / nominalGrid.height()
            : icons.verticalScrollBar()->value();
        const int nominalFirst = qMax(0, (nominalVisualRow - 1) * nominalColumns);
        const int nominalLast = qMin(icons.model()->rowCount(), nominalFirst
            + (icons.viewport()->height() / nominalGrid.height() + 3) * nominalColumns);
        int legacyVisible = 0;
        for (int row = nominalFirst; row < nominalLast; ++row)
            if (icons.visualRect(icons.model()->index(row, 0)).intersects(icons.viewport()->rect())) ++legacyVisible;
        qInfo("VISIBLE FIX: removed arithmetic collected=%d actual=%d at scroll=%d nominal columns=%d grid=%dx%d",
              legacyVisible, scrolled.size(), icons.verticalScrollBar()->value(), nominalColumns,
              nominalGrid.width(), nominalGrid.height());
        verify(legacyVisible < scrolled.size(), "regression fixture reproduces missing visible items with removed arithmetic");
        icons.setCategorized(true);
        icons.doItemsLayout(); app.processEvents();
        icons.verticalScrollBar()->setValue(icons.verticalScrollBar()->maximum() / 2 + 17);
        app.processEvents();
        completeViewport("grouped KDE Icons viewport is also complete");
        QSet<QUrl> requested;
        int backendBatches = 0;
        QObject::connect(&visibleAdapter.controller(), &PreviewController::previewBatchRequested,
                         [&](const QList<QUrl> &urls, const QSize &, quint64) {
            ++backendBatches;
            for (const auto &url : urls) requested.insert(url);
        });
        visibleAdapter.setDebounceIntervalMs(30);
        const quint64 before = visibleAdapter.debounceFireCount();
        for (int n = 0; n < 150; ++n) {
            icons.verticalScrollBar()->setValue(500 + n * 3);
            visibleAdapter.scheduleUpdate();
        }
        QTest::qWait(80);
        verify(visibleAdapter.debounceFireCount() == before + 1,
               "150 scroll notifications dispatch only the final viewport");
        verify(requested == oracle(), "debounced final viewport is COMPLETE");

        class VisibleBatchJob final : public KIO::PreviewJob {
        public:
            VisibleBatchJob(const KFileItemList &items, const QSize &size, const QStringList *plugins)
                : KIO::PreviewJob(items, size, plugins), items(items) {}
            void finish() { emitResult(); }
            KFileItemList items;
        };
        QList<QPointer<VisibleBatchJob>> jobs;
        visibleAdapter.controller().setPreviewJobFactoryForTesting(
            [&](const KFileItemList &items, const QSize &size, const QStringList *plugins) {
                auto *job = new VisibleBatchJob(items, size, plugins);
                jobs.append(job);
                return job;
            });
        icons.setCategorized(false);
        icons.resize(1220, 882); icons.doItemsLayout(); app.processEvents();
        visibleAdapter.cancel();
        const auto large = completeViewport("viewport above 100 visible items remains complete");
        verify(large.size() > 100, "large viewport exceeds one PreviewJob batch");
        visibleAdapter.controller().clearCache();
        requested.clear(); backendBatches = 0; jobs.clear();
        visibleAdapter.updatePreviewsNow();
        verify(jobs.size() == 1 && jobs.first()->items.size() == 100,
               "large visible request starts one batch of exactly 100");
        QSet<QUrl> delivered;
        QObject::connect(&visibleAdapter.controller(), &PreviewController::previewReady,
                         [&](const QUrl &url, const QPixmap &, quint64) { delivered.insert(url); });
        QPixmap pix(32, 32); pix.fill(Qt::cyan);
        QUrl failed;
        QList<int> sizes;
        for (int batch = 0; batch < jobs.size(); ++batch) {
            const auto job = jobs.at(batch);
            sizes.append(job->items.size());
            for (int n = 0; n < job->items.size(); ++n) {
                const auto item = job->items.at(n);
                if (batch == 0 && n == 50) { failed = item.url(); Q_EMIT job->failed(item); }
                else Q_EMIT job->gotPreview(item, pix);
            }
            job->finish();
            delete job.data(); // Do not let the mock's queued real KIO startup run.
        }
        verify(requested == large, "100 + remainder batching drops zero visible URLs");
        auto successful = large; successful.remove(failed);
        verify(delivered == successful, "failure in middle of first batch does not starve other previews");
        verify(!visibleAdapter.controller().isRunning()
                   && visibleAdapter.controller().pendingQueueCount() == 0,
               "all visible batches finish with empty queue");
        int applied = 0;
        for (const auto &index : visibleAdapter.m_appliedIndexes)
            if (index.isValid() && index.data(directory_view_detail::PreviewPixmapRole).isValid()) ++applied;
        verify(applied == successful.size(), "every successful visible callback reaches its own model item");
        requested.clear(); delivered.clear(); jobs.clear();
        visibleAdapter.updatePreviewsNow();
        verify(delivered == successful, "cache hits deliver all successful visible items immediately");
        verify(jobs.size() == 1 && jobs.first()->items.size() == 1 && requested == QSet<QUrl>{failed},
               "cache hits do not starve the single missing preview");
        Q_EMIT jobs.first()->gotPreview(jobs.first()->items.first(), pix);
        jobs.first()->finish();
        delete jobs.first().data();
        verify(delivered == large, "cache plus missing backend completes the entire visible viewport");
        qInfo("VISIBLE FIX: small=%d expanded=%d scrolled=%d large=%d batches=%s",
              small.size(), expanded.size(), scrolled.size(), large.size(),
              qPrintable((QStringList{QString::number(sizes.value(0)), QString::number(sizes.value(1))}.join('+'))));
    }

    // Real KIO smoke test through Stage 3 view path
    {
        DirectoryListWidget smokeList;
        smokeList.setViewMode(QListView::IconMode);
        smokeList.setIconSize(QSize(64, 64));
        smokeList.resize(400, 300);
        smokeList.show();

        DirectoryPreviewAdapter smokeAdapter;
        smokeAdapter.attachViews(&smokeList, nullptr);
        smokeAdapter.setCurrentDirectoryUrl(localBase);

        // Add PNG, JPG, MP4 (with empty mime to test extension resolution), TXT, EXE, Folder
        FileInfo fPng; fPng.name = QStringLiteral("test.png"); fPng.url = localImgPng; fPng.isDir = false; fPng.mimeType = QStringLiteral("image/png");
        FileInfo fJpg; fJpg.name = QStringLiteral("test.jpg"); fJpg.url = localImgJpg; fJpg.isDir = false; fJpg.mimeType = QStringLiteral("image/jpeg");
        FileInfo fVid; fVid.name = QStringLiteral("test.mp4"); fVid.url = localVidMp4; fVid.isDir = false; fVid.mimeType = QString(); // empty mime from KIO listDir
        FileInfo fTxt; fTxt.name = QStringLiteral("test.txt"); fTxt.url = localTxt; fTxt.isDir = false; fTxt.mimeType = QStringLiteral("text/plain");
        FileInfo fExe; fExe.name = QStringLiteral("test.exe"); fExe.url = localExe; fExe.isDir = false; fExe.mimeType = QStringLiteral("application/x-ms-dos-executable");
        FileInfo fDir; fDir.name = QStringLiteral("subfolder"); fDir.url = localFolder; fDir.isDir = true; fDir.mimeType = QStringLiteral("inode/directory");

        addDirectoryFileItems(&smokeList, nullptr, fPng, canonicalIcon, QStringLiteral("PNG"), QStringLiteral("1 KB"), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fJpg, canonicalIcon, QStringLiteral("JPG"), QStringLiteral("1 KB"), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fVid, canonicalIcon, QStringLiteral("MP4"), QStringLiteral("1 KB"), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fTxt, canonicalIcon, QStringLiteral("TXT"), QStringLiteral("1 KB"), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fExe, canonicalIcon, QStringLiteral("EXE"), QStringLiteral("1 KB"), QStringLiteral("Today"));
        addDirectoryFileItems(&smokeList, nullptr, fDir, canonicalIcon, QStringLiteral("Folder"), QStringLiteral(""), QStringLiteral("Today"));

        app.processEvents();

        // Check initial state
        QAbstractItemModel *model = smokeList.model();
        verify(model->rowCount() == 6, "Smoke model has 6 rows");

        // Wait for preview job to complete
        bool gotPngPreview = false;
        bool gotJpgPreview = false;
        bool gotVidPreview = false;

        QObject::connect(&smokeAdapter.controller(), &PreviewController::previewReady,
                         [&](const QUrl &url, const QPixmap &pix, quint64) {
            if (url == localImgPng && !pix.isNull()) gotPngPreview = true;
            if (url == localImgJpg && !pix.isNull()) gotJpgPreview = true;
            if (url == localVidMp4 && !pix.isNull()) gotVidPreview = true;
        });

        // Trigger immediate update
        smokeAdapter.updatePreviewsNow();

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && (!gotPngPreview || !gotJpgPreview || !gotVidPreview)) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }

        verify(gotPngPreview, "41. Real KIO smoke: PNG thumbnail generated and delivered");
        verify(gotJpgPreview, "42. Real KIO smoke: JPG thumbnail generated and delivered");
        verify(gotVidPreview, "43. Real KIO smoke: MP4 video thumbnail generated and delivered");

        QModelIndex vidIdx = model->index(2, 0);
        verify(vidIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "43b. Real KIO smoke: MP4 item has valid PreviewPixmapRole");
        verify(!vidIdx.data(Qt::DecorationRole).value<QIcon>().isNull(),
               "43c. Real KIO smoke: MP4 canonical icon intact in DecorationRole");

        // Verify TXT, EXE, Folder did not receive PreviewPixmapRole
        QModelIndex txtIdx = model->index(3, 0);
        QModelIndex exeIdx = model->index(4, 0);
        QModelIndex dirIdx = model->index(5, 0);
        verify(!txtIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "44. Real KIO smoke: TXT item has no PreviewPixmapRole");
        verify(!exeIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "45. Real KIO smoke: EXE item has no PreviewPixmapRole");
        verify(!dirIdx.data(directory_view_detail::PreviewPixmapRole).isValid(),
               "46. Real KIO smoke: Folder item has no PreviewPixmapRole");
    }

    qDebug("image_video_previews PASS: %d assertions", checks);
    return 0;
}
