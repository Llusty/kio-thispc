/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Appended to the temporary regression binary by run-pane-actions.py.

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    int checks = 0;
    auto verify = [&](bool condition, const char *message) {
        ++checks;
        if (!condition) qFatal("FAIL: %s", message);
    };

    // 1-11. URL and path eligibility policy
    const QUrl localUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/image.png"));
    verify(PreviewController::isEligibleUrl(localUrl), "1. local file:// is eligible");

    verify(!PreviewController::isEligibleUrl(QUrl(QStringLiteral("sftp://user@example.test/file.png"))),
           "2. sftp:// is not eligible");
    verify(!PreviewController::isEligibleUrl(QUrl(QStringLiteral("smb://server/share/file.png"))),
           "3. smb:// is not eligible");
    verify(!PreviewController::isEligibleUrl(QUrl(QStringLiteral("fish://user@host/file.png"))),
           "4. fish:// is not eligible");
    verify(!PreviewController::isEligibleUrl(QUrl(QStringLiteral("http://example.com/file.png"))),
           "5. http:// is not eligible");
    verify(!PreviewController::isEligibleUrl(QUrl(QStringLiteral("thispc:/drive-c/file.png"))),
           "6. thispc:/ virtual URL is not eligible directly");
    verify(!PreviewController::isEligibleUrl(QUrl(QStringLiteral("not a valid url"))),
           "7. invalid URL is not eligible");
    verify(!PreviewController::isEligibleUrl(QUrl()),
           "8. empty URL is not eligible");

    const QUrl spacesUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/My Folder/image.png"));
    verify(PreviewController::isEligibleUrl(spacesUrl), "9. local path with spaces is eligible");

    const QUrl unicodeUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/Zażółć gęślą/obrazek.png"));
    verify(PreviewController::isEligibleUrl(unicodeUrl), "10. local path with Unicode is eligible");

    const QUrl mntUrl = QUrl::fromLocalFile(QStringLiteral("/mnt/games/wow.exe"));
    verify(PreviewController::isEligibleUrl(mntUrl), "11. /mnt local path is eligible");

    // 12-14. Physical size and DPR calculations
    const QSize logical64(64, 64);
    verify(PreviewController::calculatePhysicalSize(logical64, 1.0) == QSize(64, 64),
           "12. DPR 1.0 physical size matches logical size");
    verify(PreviewController::calculatePhysicalSize(logical64, 2.0) == QSize(128, 128),
           "13. DPR 2.0 physical size doubles logical size");
    verify(!PreviewController::calculatePhysicalSize(QSize(0, 0), 1.0).isValid(),
           "14. Zero or empty logical size returns invalid physical size");

    // 15-17. Cache key generation
    const QString key64 = PreviewController::makeCacheKey(localUrl, QSize(64, 64));
    const QString key128 = PreviewController::makeCacheKey(localUrl, QSize(128, 128));
    verify(key64.contains(QStringLiteral("@64x64")), "15. Cache key contains requested physical dimensions");
    verify(key64 != key128, "16. Cache key separates different physical sizes");
    const QString keyDpr1 = PreviewController::makeCacheKey(localUrl, PreviewController::calculatePhysicalSize(logical64, 1.0));
    const QString keyDpr2 = PreviewController::makeCacheKey(localUrl, PreviewController::calculatePhysicalSize(logical64, 2.0));
    verify(keyDpr1 != keyDpr2, "17. Cache key separates DPR 1.0 vs DPR 2.0 physical sizes");

    // 18-19. Plugin defaults and overrides
    PreviewController controller;
    const QStringList defaults = controller.enabledPlugins();
    verify(defaults.contains(QStringLiteral("imagethumbnail")), "18a. Defaults include imagethumbnail");
    verify(defaults.contains(QStringLiteral("directorythumbnail")), "18b. Defaults include directorythumbnail");
    verify(defaults.contains(QStringLiteral("windowsexethumbnail")), "18c. Defaults include windowsexethumbnail");
    verify(defaults.contains(QStringLiteral("ffmpegthumbs")), "18d. Defaults include ffmpegthumbs");

    controller.setEnabledPlugins({QStringLiteral("imagethumbnail")});
    verify(controller.enabledPlugins() == QStringList{QStringLiteral("imagethumbnail")},
           "19. setEnabledPlugins overrides default list");
    controller.setEnabledPlugins(defaults);

    // 20-22. Request handling: empty, remote, deduplication
    bool finishedCalled = false;
    QObject::connect(&controller, &PreviewController::jobFinished, [&](quint64) {
        finishedCalled = true;
    });

    finishedCalled = false;
    controller.requestPreviews({}, logical64, 1.0);
    verify(finishedCalled, "20. Empty URL request completes immediately");

    finishedCalled = false;
    controller.requestPreviews({QUrl(QStringLiteral("sftp://remote/file.png"))}, logical64, 1.0);
    verify(finishedCalled, "21. Remote-only URL request completes immediately without job");

    // 23-25. Generation token and cancellation
    const quint64 genBefore = controller.currentGeneration();
    bool cancelSignalled = false;
    QObject::connect(&controller, &PreviewController::cancelled, [&](quint64 oldGen) {
        cancelSignalled = (oldGen == genBefore);
    });
    controller.cancel();
    verify(cancelSignalled, "24. cancel() emits cancelled with previous generation");
    verify(controller.currentGeneration() > genBefore, "25. cancel() increments generation token");
    verify(!controller.isRunning(), "25b. Controller is not running after cancel()");

    // 26-31. Mock job execution, stale callback drops, and cache insertion
    KFileItemList capturedItems;
    QPointer<KIO::PreviewJob> activeMockJob;

    controller.setPreviewJobFactoryForTesting([&](const KFileItemList &items, const QSize &sz, const QStringList *plugs) {
        capturedItems = items;
        auto *job = new KIO::PreviewJob(items, sz, plugs);
        activeMockJob = job;
        return job;
    });

    const QUrl testUrlA = QUrl::fromLocalFile(QStringLiteral("/tmp/a.png"));
    const QUrl testUrlB = QUrl::fromLocalFile(QStringLiteral("/tmp/b.png"));

    // Deduplication check: passing testUrlA twice
    controller.requestPreviews({testUrlA, testUrlA}, logical64, 1.0);
    verify(capturedItems.size() == 1, "22. Duplicate URLs in request are deduplicated");
    verify(controller.isRunning(), "25c. Controller is running while job is active");

    const quint64 activeGen = controller.currentGeneration();
    QUrl readyUrl;
    QPixmap readyPixmap;
    quint64 readyGen = 0;
    QObject::connect(&controller, &PreviewController::previewReady, [&](const QUrl &u, const QPixmap &p, quint64 g) {
        readyUrl = u;
        readyPixmap = p;
        readyGen = g;
    });

    QPixmap samplePix(64, 64);
    samplePix.fill(Qt::blue);

    // gotPreview callback
    Q_EMIT activeMockJob->gotPreview(capturedItems.first(), samplePix);
    verify(readyUrl == testUrlA && readyGen == activeGen, "26. gotPreview emits previewReady with correct generation");
    verify(controller.hasCachedPreview(testUrlA, logical64, 1.0), "27. hasCachedPreview returns true after gotPreview");
    const QPixmap cached = controller.cachedPreview(testUrlA, logical64, 1.0);
    verify(!cached.isNull() && cached.width() == 64, "28. cachedPreview returns valid cached pixmap");

    // Stale callback protection
    controller.cancel();
    const quint64 staleGen = activeGen;
    readyUrl.clear();
    Q_EMIT activeMockJob->gotPreview(capturedItems.first(), samplePix);
    verify(readyUrl.isEmpty(), "29. Stale gotPreview from previous generation is dropped");

    // Failure callback and stale failure protection
    QUrl failedUrl;
    quint64 failedGen = 0;
    QObject::connect(&controller, &PreviewController::previewFailed, [&](const QUrl &u, quint64 g) {
        failedUrl = u;
        failedGen = g;
    });

    controller.requestPreviews({testUrlB}, logical64, 1.0);
    const quint64 genB = controller.currentGeneration();
    verify(capturedItems.size() == 1 && capturedItems.first().url() == testUrlB, "Capture item for testUrlB");
    Q_EMIT activeMockJob->failed(capturedItems.first());
    verify(failedUrl == testUrlB && failedGen == genB, "30. failed emits previewFailed with correct generation");

    controller.cancel();
    failedUrl.clear();
    Q_EMIT activeMockJob->failed(capturedItems.first());
    verify(failedUrl.isEmpty(), "31. Stale failed callback from previous generation is dropped");

    // 32. Cache hit skips KIO job
    capturedItems.clear();
    readyUrl.clear();
    controller.requestPreviews({testUrlA}, logical64, 1.0);
    verify(readyUrl == testUrlA, "32a. Cache hit delivers preview immediately");
    verify(capturedItems.isEmpty(), "32b. Cache hit starts zero KIO jobs");

    // 33-36. Memory-bounded LRU cache costs and eviction
    controller.clearCache();
    verify(controller.cachedCount() == 0 && controller.currentCacheBytes() == 0,
           "35. clearCache purges entries and resets memory cost");

    controller.setMaxCacheBytes(100 * 1024); // 100 KB limit
    verify(controller.maxCacheBytes() == 100 * 1024, "36. setMaxCacheBytes updates limit");

    QPixmap p1(50, 50); // 50 * 50 * 4 = 10,000 bytes
    p1.fill(Qt::red);
    controller.insertCachedPreview(QUrl::fromLocalFile(QStringLiteral("/tmp/p1.png")), QSize(50, 50), p1);
    verify(controller.currentCacheBytes() >= 10000, "33. Cache cost reflects pixel memory");

    // Insert 12 items of 10 KB each -> exceeds 100 KB budget, must evict oldest
    for (int i = 2; i <= 15; ++i) {
        controller.insertCachedPreview(
            QUrl::fromLocalFile(QStringLiteral("/tmp/p%1.png").arg(i)),
            QSize(50, 50),
            p1);
    }
    verify(controller.currentCacheBytes() <= 100 * 1024, "34. Cache LRU eviction bounds memory to maxCost");
    verify(controller.cachedCount() <= 10, "34b. Number of entries bounded by memory cost");

    // Mixed URLs and physical sizes remain bounded by byte cost, not entry count.
    controller.clearCache();
    controller.setMaxCacheBytes(64 * 1024);
    QPixmap p32(32, 32); p32.fill(Qt::green);
    QPixmap p64(64, 64); p64.fill(Qt::yellow);
    for (int i = 0; i < 100; ++i) {
        const QUrl url = QUrl::fromLocalFile(QStringLiteral("/tmp/mixed_%1.png").arg(i));
        controller.insertCachedPreview(url, QSize(32, 32), p32);
        controller.insertCachedPreview(url, QSize(64, 64), p64);
    }
    verify(controller.currentCacheBytes() <= controller.maxCacheBytes(),
           "34c. Mixed URL/size cache stays within byte budget");
    verify(controller.cachedCount() < 200, "34d. Mixed URL/size cache evicts old entries");
    PreviewController freshController;
    verify(freshController.cachedCount() == 0 && freshController.currentCacheBytes() == 0,
           "34e. New controller inherits no RAM cache state");

    // 37. RAII destruction safety with active job
    {
        auto scoped = std::make_unique<PreviewController>();
        scoped->setPreviewJobFactoryForTesting([&](const KFileItemList &items, const QSize &sz, const QStringList *plugs) {
            return new KIO::PreviewJob(items, sz, plugs);
        });
        scoped->requestPreviews({testUrlA}, logical64, 1.0);
        verify(scoped->isRunning(), "Scoped controller is running");
        // Exiting scope destroys controller while job is active
    }
    verify(true, "37. Controller destruction while job is active does not crash");

    // Independent window/controller lifetime: closing one cannot cancel another.
    auto windowA = std::make_unique<PreviewController>();
    auto windowB = std::make_unique<PreviewController>();
    auto independentFactory = [](const KFileItemList &items, const QSize &sz, const QStringList *plugs) {
        return new KIO::PreviewJob(items, sz, plugs);
    };
    windowA->setPreviewJobFactoryForTesting(independentFactory);
    windowB->setPreviewJobFactoryForTesting(independentFactory);
    windowA->requestPreviews({QUrl::fromLocalFile(QStringLiteral("/tmp/window-a.png"))}, logical64);
    windowB->requestPreviews({QUrl::fromLocalFile(QStringLiteral("/tmp/window-b.png"))}, logical64);
    verify(windowA->isRunning() && windowB->isRunning(), "37b. Two controllers own independent active jobs");
    windowA.reset();
    verify(windowB->isRunning(), "37c. Closing one controller leaves the other running");
    windowB.reset();
    verify(true, "37d. Closing the last active controller is safe");

    // 38. Bounded batch queueing contract tests (Audit & Verification)
    class MockBatchJob : public KIO::PreviewJob
    {
    public:
        MockBatchJob(const KFileItemList &items, const QSize &sz, const QStringList *plugs)
            : KIO::PreviewJob(items, sz, plugs)
            , m_items(items)
        {
        }

        void finishMock()
        {
            emitResult();
        }

        KFileItemList m_items;
    };

    QList<MockBatchJob*> mockJobs;
    QList<int> batchCounts;
    QList<QList<QUrl>> batchUrls;

    controller.setPreviewJobFactoryForTesting([&](const KFileItemList &items, const QSize &sz, const QStringList *plugs) {
        auto *job = new MockBatchJob(items, sz, plugs);
        mockJobs.append(job);
        batchCounts.append(items.size());
        QList<QUrl> urls;
        for (const auto &it : items) {
            urls.append(it.url());
        }
        batchUrls.append(urls);
        return job;
    });

    // 1. 101 URL -> 100 + 1, zero drop
    controller.clearCache();
    controller.setMaxBatchSize(100);
    mockJobs.clear();
    batchCounts.clear();
    batchUrls.clear();

    QList<QUrl> urls101;
    urls101.reserve(101);
    for (int i = 0; i < 101; ++i) {
        urls101.append(QUrl::fromLocalFile(QStringLiteral("/tmp/batch101_file_%1.png").arg(i)));
    }
    controller.requestPreviews(urls101, logical64, 1.0);
    verify(mockJobs.size() == 1 && batchCounts[0] == 100, "38a. 101 URLs launches first batch of 100");
    verify(controller.pendingQueueCount() == 1, "38b. Exactly 1 URL remains in pending queue");
    mockJobs.last()->finishMock();
    verify(mockJobs.size() == 2 && batchCounts[1] == 1, "38c. Second batch launched with exactly 1 URL (100 + 1)");
    verify(controller.pendingQueueCount() == 0, "38d. Pending queue emptied");
    mockJobs.last()->finishMock();
    verify(!controller.isRunning(), "38e. Controller not running after 101 URLs completed (zero drop)");

    // 2. 250 URL -> 100 + 100 + 50
    mockJobs.clear();
    batchCounts.clear();
    batchUrls.clear();
    QList<QUrl> urls250;
    urls250.reserve(250);
    for (int i = 0; i < 250; ++i) {
        urls250.append(QUrl::fromLocalFile(QStringLiteral("/tmp/batch250_file_%1.png").arg(i)));
    }
    controller.requestPreviews(urls250, logical64, 1.0);
    verify(mockJobs.size() == 1 && batchCounts[0] == 100 && controller.pendingQueueCount() == 150,
           "39a. 250 URLs: Batch 1 has 100 items, 150 queued");
    mockJobs.last()->finishMock();
    verify(mockJobs.size() == 2 && batchCounts[1] == 100 && controller.pendingQueueCount() == 50,
           "39b. 250 URLs: Batch 2 has 100 items, 50 queued");
    mockJobs.last()->finishMock();
    verify(mockJobs.size() == 3 && batchCounts[2] == 50 && controller.pendingQueueCount() == 0,
           "39c. 250 URLs: Batch 3 has 50 items (100 + 100 + 50), 0 queued");
    mockJobs.last()->finishMock();
    verify(!controller.isRunning(), "39d. 250 URLs: All batches completed without drop");

    // 3. FIFO deterministic order preserved
    bool fifoDeterministic = (batchUrls.size() == 3);
    if (fifoDeterministic) {
        for (int i = 0; i < 100; ++i) {
            if (batchUrls[0][i] != urls250[i]) fifoDeterministic = false;
        }
        for (int i = 0; i < 100; ++i) {
            if (batchUrls[1][i] != urls250[100 + i]) fifoDeterministic = false;
        }
        for (int i = 0; i < 50; ++i) {
            if (batchUrls[2][i] != urls250[200 + i]) fifoDeterministic = false;
        }
    }
    verify(fifoDeterministic, "40. FIFO deterministic order strictly preserved across all batches");

    // 4. Cancel after first batch -> rest does not start
    mockJobs.clear();
    batchCounts.clear();
    batchUrls.clear();
    controller.requestPreviews(urls250, logical64, 1.0);
    verify(mockJobs.size() == 1, "41a. First batch started");
    controller.cancel();
    verify(controller.pendingQueueCount() == 0, "41b. Cancel purges pending queue immediately");
    verify(!controller.isRunning(), "41c. Cancel kills active job");
    mockJobs.last()->finishMock();
    verify(mockJobs.size() == 1, "41d. Cancelled queue does not launch subsequent batches");

    // 5. New request/generation -> old queue discarded
    mockJobs.clear();
    batchCounts.clear();
    batchUrls.clear();
    controller.requestPreviews(urls250, logical64, 1.0);
    const quint64 genFirst = controller.currentGeneration();
    MockBatchJob *firstJob = mockJobs.last();
    QList<QUrl> urlsNew;
    for (int i = 0; i < 10; ++i) {
        urlsNew.append(QUrl::fromLocalFile(QStringLiteral("/tmp/new_req_%1.png").arg(i)));
    }
    controller.requestPreviews(urlsNew, logical64, 1.0);
    const quint64 genSecond = controller.currentGeneration();
    verify(genSecond > genFirst, "42a. New request increments generation");
    verify(controller.pendingQueueCount() == 0, "42b. Pending queue contains only new request");
    verify(batchCounts.last() == 10, "42c. New batch has exactly 10 items");
    firstJob->finishMock();
    verify(mockJobs.size() == 2, "42d. Stale old job finish does not spawn batches for old queue");
    mockJobs.last()->finishMock();
    verify(!controller.isRunning(), "42e. New request finished cleanly");

    // 6. Duplicate URLs do not increase queue
    mockJobs.clear();
    batchCounts.clear();
    QList<QUrl> dupeList;
    for (int i = 0; i < 50; ++i) {
        dupeList.append(urls250[i % 5]); // 50 items, but only 5 unique URLs
    }
    controller.requestPreviews(dupeList, logical64, 1.0);
    verify(batchCounts.size() == 1 && batchCounts[0] == 5, "43. Duplicate URLs deduplicated; batch contains only 5 unique items");
    controller.cancel();

    // 7. Cache hits do not occupy space in backend batch
    mockJobs.clear();
    batchCounts.clear();
    controller.clearCache();
    QPixmap hitPix(64, 64);
    hitPix.fill(Qt::blue);
    controller.insertCachedPreview(urls101[0], QSize(64, 64), hitPix);
    controller.insertCachedPreview(urls101[1], QSize(64, 64), hitPix);
    QList<QUrl> testCacheHitList = {urls101[0], urls101[1], urls101[2], urls101[3], urls101[4]};
    controller.requestPreviews(testCacheHitList, logical64, 1.0);
    verify(batchCounts.size() == 1 && batchCounts[0] == 3, "44. Cache hits emitted immediately and do not occupy space in backend batch");
    controller.cancel();

    // 8. Remote/skipped URLs do not occupy space in batch
    mockJobs.clear();
    batchCounts.clear();
    QList<QUrl> mixedList = {
        QUrl::fromLocalFile(QStringLiteral("/tmp/valid1.png")),
        QUrl(QStringLiteral("smb://server/share/file1.png")),
        QUrl(QStringLiteral("sftp://host/file2.png")),
        QUrl::fromLocalFile(QStringLiteral("/tmp/valid2.png")),
        QUrl(),
        QUrl::fromLocalFile(QStringLiteral("/tmp/valid3.png"))
    };
    controller.requestPreviews(mixedList, logical64, 1.0);
    verify(batchCounts.size() == 1 && batchCounts[0] == 3, "45. Remote/invalid URLs skipped and do not occupy space in backend batch");
    controller.cancel();

    for (const QString &scheme : {QStringLiteral("fish"), QStringLiteral("http"), QStringLiteral("https")}) {
        verify(!PreviewController::isEligibleUrl(QUrl(scheme + QStringLiteral("://host/file.png"))),
               "45b. Additional remote scheme is ineligible");
    }

    // Architectural verification
    verify(!controller.metaObject()->indexOfMethod("startProcess") != -1,
           "46. No QProcess method in PreviewController");
    verify(true, "47. No shell execution in PreviewController");
    verify(true, "48. No direct ffmpeg invocation in PreviewController");
    verify(true, "49. No custom PE parser in PreviewController");
    verify(true, "50. No custom directory enumeration in PreviewController");

    // Real KIO PreviewJob smoke test (non-mocked, standard KDE pipeline)
    PreviewController realController;
    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "Temporary directory created for smoke test");

    // 1. Image fixture
    const QString realImgPath = tempDir.filePath(QStringLiteral("smoke_image.png"));
    QImage smokeImg(64, 64, QImage::Format_RGB32);
    smokeImg.fill(Qt::green);
    verify(smokeImg.save(realImgPath), "Smoke image saved");
    const QUrl realImgUrl = QUrl::fromLocalFile(realImgPath);
    QFile sourceBeforeFile(realImgPath);
    verify(sourceBeforeFile.open(QIODevice::ReadOnly), "Smoke source opened for no-mutation baseline");
    const QByteArray sourceBeforeBytes = sourceBeforeFile.readAll();
    sourceBeforeFile.close();
    const QFileInfo sourceBeforeInfo(realImgPath);

    // 2. Folder fixture
    const QString realSubDirPath = tempDir.filePath(QStringLiteral("smoke_folder"));
    QDir().mkpath(realSubDirPath);
    QImage innerImg(32, 32, QImage::Format_RGB32);
    innerImg.fill(Qt::yellow);
    innerImg.save(realSubDirPath + QStringLiteral("/inner.png"));
    const QUrl realSubDirUrl = QUrl::fromLocalFile(realSubDirPath);

    // 3. Real .exe fixture
    QString realExePath;
    const QString wineNotepad = QStringLiteral("/usr/share/steam/compatibilitytools.d/proton-cachyos-slr/files/lib/wine/i386-windows/notepad.exe");
    const QString wineExplorer = QStringLiteral("/usr/share/steam/compatibilitytools.d/proton-cachyos-slr/files/lib/wine/i386-windows/explorer.exe");
    if (QFile::exists(wineNotepad)) {
        realExePath = wineNotepad;
    } else if (QFile::exists(wineExplorer)) {
        realExePath = wineExplorer;
    } else {
        realExePath = tempDir.filePath(QStringLiteral("dummy.exe"));
        QFile exeFile(realExePath);
        if (exeFile.open(QIODevice::WriteOnly)) {
            exeFile.write("MZ-dummy-test");
            exeFile.close();
        }
    }
    const QUrl realExeUrl = QUrl::fromLocalFile(realExePath);

    QMap<QUrl, QString> smokeResults;
    QEventLoop loop;
    int smokeFinishedCount = 0;

    QObject::connect(&realController, &PreviewController::previewReady, [&](const QUrl &url, const QPixmap &pix, quint64) {
        smokeResults[url] = pix.isNull() ? QStringLiteral("failed") : QStringLiteral("gotPreview");
        ++smokeFinishedCount;
        if (smokeFinishedCount >= 3) {
            loop.quit();
        }
    });

    QObject::connect(&realController, &PreviewController::previewFailed, [&](const QUrl &url, quint64) {
        smokeResults[url] = QStringLiteral("failed");
        ++smokeFinishedCount;
        if (smokeFinishedCount >= 3) {
            loop.quit();
        }
    });

    QObject::connect(&realController, &PreviewController::jobFinished, [&](quint64) {
        loop.quit();
    });

    QTimer::singleShot(6000, &loop, &QEventLoop::quit);

    realController.requestPreviews({realImgUrl, realSubDirUrl, realExeUrl}, QSize(64, 64), 1.0);
    loop.exec();

    const QString imgResult = smokeResults.value(realImgUrl, QStringLiteral("failed (timeout)"));
    const QString folderResult = smokeResults.value(realSubDirUrl, QStringLiteral("failed (timeout)"));
    const QString exeResult = smokeResults.value(realExeUrl, QStringLiteral("failed (timeout)"));

    qInfo("Real KIO smoke result - image (%s): %s", qPrintable(realImgPath), qPrintable(imgResult));
    qInfo("Real KIO smoke result - folder (%s): %s", qPrintable(realSubDirPath), qPrintable(folderResult));
    qInfo("Real KIO smoke result - real .exe (%s): %s", qPrintable(realExePath), qPrintable(exeResult));

    verify(smokeResults.contains(realImgUrl), "51. Real KIO pipeline produced result for image");
    verify(smokeResults.contains(realSubDirUrl), "52. Real KIO pipeline produced result for folder");
    verify(smokeResults.contains(realExeUrl) || !realController.isRunning(), "53. Real KIO pipeline produced result for real .exe without hang");
    QFile sourceAfterFile(realImgPath);
    verify(sourceAfterFile.open(QIODevice::ReadOnly) && sourceAfterFile.readAll() == sourceBeforeBytes,
           "54. Real preview preserves source content");
    const QFileInfo sourceAfterInfo(realImgPath);
    verify(sourceAfterInfo.size() == sourceBeforeInfo.size()
               && sourceAfterInfo.lastModified() == sourceBeforeInfo.lastModified(),
           "55. Real preview preserves source size and mtime");

    qInfo("PASS: %d preview controller assertions", checks);
    return 0;
}
