/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

class TestOperationJob final : public KJob
{
public:
    explicit TestOperationJob(bool killable, bool suspendable = false)
    {
        setAutoDelete(false);
        setCapabilities(
            (killable ? KJob::Killable : KJob::NoCapabilities)
            | (suspendable ? KJob::Suspendable : KJob::NoCapabilities));
    }

    void start() override {}

    void report(
        qulonglong processed,
        qulonglong total,
        unsigned long speed)
    {
        setTotalAmount(KJob::Bytes, total);
        setProcessedAmount(KJob::Bytes, processed);
        emitSpeed(speed);
        description(
            this,
            QStringLiteral("Copying"),
            {QStringLiteral("Source"), QStringLiteral("source.txt")},
            {QStringLiteral("Destination"), QStringLiteral("target")});
    }

    void reportSpeed(unsigned long speed)
    {
        emitSpeed(speed);
    }

protected:
    bool doKill() override
    {
        killed = true;
        return true;
    }

    bool doSuspend() override
    {
        paused = true;
        return true;
    }

    bool doResume() override
    {
        paused = false;
        return true;
    }

public:
    void finishError(int code, const QString &text)
    {
        setError(code);
        setErrorText(text);
        emitResult();
    }
    bool killed = false;
    bool paused = false;
};

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow window;
    QToolBar toolbar(&window);
    OperationManager manager(&window, &toolbar);
    QSignalSpy changed(&manager, &OperationManager::operationsChanged);

    TestOperationJob first(true);
    TestOperationJob second(false);
    manager.track(&first, QStringLiteral("Copying"));
    const int oneOperationHeight = manager.m_detailedWindow->height();
    manager.track(&second, QStringLiteral("Scanning"));

    verify(manager.m_detailedWindow != nullptr
               && manager.m_detailedWindow->isVisible(),
           "detailed window opens automatically for a new operation");
    verify(manager.m_detailedWindow
                   ->findChildren<QFrame *>(QStringLiteral("operationRow"))
                   .size() == 2,
           "detailed window combines simultaneous active operations");
    verify(manager.m_detailedWindow->height() > oneOperationHeight,
           "detailed window grows for simultaneous operations");

    auto operations = manager.operations();
    verify(operations.size() == 2 && manager.activeCount() == 2,
           "multiple active operations are exposed");
    verify(operations[0].id != 0 && operations[0].id != operations[1].id,
           "stable operation identifiers are unique");
    verify(operations[0].title == QStringLiteral("Copying")
               && operations[0].state == OperationManager::State::Running
               && operations[0].canCancel,
           "running killable operation state");
    verify(!operations[1].canCancel, "non-killable operation state");

    first.report(256, 1024, 128);
    operations = manager.operations();
    verify(operations[0].processedBytes == 256
               && operations[0].totalBytes == 1024
               && operations[0].percent == 25
               && operations[0].speedBytesPerSecond == 128,
           "progress values are exposed from one shared state");
    verify(operations[0].contextText.contains(QStringLiteral("source.txt"))
               && operations[0].contextText.contains(QStringLiteral("target")),
           "KJob context is exposed");
    const QUrl nativeCurrent = QUrl::fromLocalFile(QStringLiteral("/tmp/tree/nested/current.bin"));
    first.setProperty("thispcCurrentSourceUrl", nativeCurrent);
    first.setProperty("thispcCurrentDestinationUrl", QUrl::fromLocalFile(QStringLiteral("/tmp/output/current.bin")));
    first.report(256, 1024, 128);
    verify(manager.operations().first().currentSourceUrl == nativeCurrent,
           "native tree descriptions update the current-file display");

    const quint64 firstId = operations[0].id;
    verify(manager.cancelOperation(firstId) && first.killed,
           "single operation cancellation uses manager path");
    operations = manager.operations();
    verify(operations[0].state == OperationManager::State::Cancelling
               && !operations[0].canCancel
               && manager.cancelRequested(&first),
           "cancellation state is exposed");
    verify(!manager.cancelOperation(operations[1].id),
           "non-killable operation is not cancelled");

    manager.finish(&first, false, true, {});
    manager.finish(&second, true, false, {});
    QTest::qWait(160);
    operations = manager.operations();
    verify(manager.activeCount() == 0
               && operations[0].state == OperationManager::State::Cancelled
               && operations[1].state == OperationManager::State::Completed
               && operations[1].percent == 100,
           "terminal operation states are exposed");
    const int changesAfterFinish = changed.count();
    first.report(900, 1024, 999);
    verify(changed.count() == changesAfterFinish
               && manager.operations()[0].state
                   == OperationManager::State::Cancelled,
           "late job progress is disconnected after completion");
    verify(changed.count() >= 8, "state changes notify observers");

    verify(manager.m_detailedWindow
                   ->findChildren<QFrame *>(QStringLiteral("operationRow"))
                   .isEmpty()
               && !manager.m_detailedWindow->isVisible()
               && manager.operations().size() == 2,
           "detailed window closes while completed history remains available");
    verify(manager.m_detailedWindow->windowFlags().testFlag(Qt::Window)
               && !manager.m_detailedWindow->isModal(),
           "detailed window is a normal non-modal top-level window");
    OperationWindow *sameWindow = manager.m_detailedWindow;
    sameWindow->close();
    verify(!sameWindow->isVisible() && manager.operations().size() == 2,
           "closing detailed window preserves operation history");
    manager.m_popup->m_showDetailsButton->click();
    verify(manager.m_detailedWindow == sameWindow
               && !manager.m_detailedWindow->isVisible(),
           "completed history does not reopen in the active window");

    manager.clearFinished();
    verify(manager.operations().isEmpty()
               && manager.m_detailedWindow
                      ->findChildren<QFrame *>(QStringLiteral("operationRow"))
                      .isEmpty(),
           "completed history clears both shared state and detailed list");

    TestOperationJob third(true);
    manager.track(&third, QStringLiteral("Moving"));
    verify(manager.m_detailedWindow == sameWindow
               && manager.m_detailedWindow->isVisible(),
           "new work reuses and reopens the detailed window");
    auto *windowCancel = manager.m_detailedWindow->findChild<QToolButton *>(
        QStringLiteral("detailedOperationCancelButton"));
    verify(windowCancel && windowCancel->isEnabled(),
           "detailed window exposes cancellation for active operation");
    windowCancel->click();
    verify(third.killed
               && manager.operations().first().state
                   == OperationManager::State::Cancelling,
           "detailed window cancellation uses shared manager path");
    manager.finish(&third, false, true, {});
    manager.clearFinished();

    TestOperationJob pauseable(true, true);
    manager.track(&pauseable, QStringLiteral("Native copy"));
    verify(manager.operations().constLast().canPause
               && !manager.operations().constLast().paused,
           "suspendable operation exposes pause capability");
    auto *pauseButton = manager.m_detailedWindow->findChild<QToolButton *>(
        QStringLiteral("detailedOperationPauseButton"));
    verify(pauseButton && !pauseButton->isHidden() && pauseButton->isEnabled(),
           "detailed window shows an enabled pause button");
    pauseButton->click();
    verify(pauseable.paused
               && manager.operations().constLast().state
                   == OperationManager::State::Paused,
           "pause control suspends the job and updates shared state");
    QCoreApplication::processEvents();
    QTest::qWait(160);
    verify(manager.m_detailedWindow->isVisible()
               && manager.m_detailedWindow
                      ->findChildren<QFrame *>(QStringLiteral("operationRow"))
                      .size() == 1,
           "paused operation remains visible in the detailed window");
    pauseButton = manager.m_detailedWindow->findChild<QToolButton *>(
        QStringLiteral("detailedOperationPauseButton"));
    verify(pauseButton && pauseButton->isEnabled(),
           "paused operation exposes an enabled resume button");
    pauseButton->click();
    verify(!pauseable.paused
               && manager.operations().constLast().state
                   == OperationManager::State::Running,
           "same control resumes the paused operation");
    manager.finish(&pauseable, true, false, {});
    manager.clearFinished();

    TestOperationJob timed(false);
    manager.track(&timed, QStringLiteral("Timed transfer"));
    operations = manager.operations();
    verify(operations.first().averageSpeedBytesPerSecond == 0
               && operations.first().etaSeconds == -1,
           "average speed and ETA stay unknown before progress");
    QTest::qWait(25);
    timed.report(1024, 4096, 512);
    timed.reportSpeed(1024);
    QTest::qWait(160);
    operations = manager.operations();
    verify(operations.first().averageSpeedBytesPerSecond > 0
               && operations.first().etaSeconds > 0,
           "average speed and ETA are calculated from elapsed progress");
    auto *speedLabel = manager.m_detailedWindow->findChild<QLabel *>(
        QStringLiteral("operationSpeedLabel"));
    auto *averageLabel = manager.m_detailedWindow->findChild<QLabel *>(
        QStringLiteral("operationAverageSpeedLabel"));
    auto *etaLabel = manager.m_detailedWindow->findChild<QLabel *>(
        QStringLiteral("operationEtaLabel"));
    verify(speedLabel && !speedLabel->text().contains(QStringLiteral("—"))
               && averageLabel && !averageLabel->text().contains(QStringLiteral("—"))
               && etaLabel && !etaLabel->text().contains(QStringLiteral("…")),
           "detailed window shows current speed, average speed and ETA");
    verify(operations.first().speedSamples == QList<qulonglong>{512, 1024}
               && manager.m_detailedWindow->findChild<OperationSpeedGraph *>(
                   QStringLiteral("operationSpeedGraph")),
           "speed samples feed a graph in detailed view");

    auto *detailsToggle = manager.m_detailedWindow->findChild<QToolButton *>(
        QStringLiteral("operationDetailsToggleButton"));
    auto *topLineCancel = manager.m_detailedWindow->findChild<QToolButton *>(
        QStringLiteral("detailedOperationCancelButton"));
    auto *expandedDetails = manager.m_detailedWindow->findChild<QWidget *>(
        QStringLiteral("operationExpandedDetails"));
    verify(detailsToggle && expandedDetails && !expandedDetails->isHidden(),
           "new operation details start expanded");
    verify(topLineCancel->parentWidget() == detailsToggle->parentWidget(),
           "operation controls share the header so content uses full width");
    detailsToggle->click();
    verify(expandedDetails->isHidden(), "operation details collapse");

    manager.m_detailedWindow->hide();
    for (unsigned long speed = 1; speed <= 130; ++speed) {
        timed.reportSpeed(speed);
    }
    operations = manager.operations();
    verify(operations.first().speedSamples.size() == 120
               && operations.first().speedSamples.constLast() == 130,
           "speed history remains bounded to newest 120 samples");
    manager.m_popup->m_showDetailsButton->click();
    expandedDetails = manager.m_detailedWindow->findChild<QWidget *>(
        QStringLiteral("operationExpandedDetails"));
    detailsToggle = manager.m_detailedWindow->findChild<QToolButton *>(
        QStringLiteral("operationDetailsToggleButton"));
    verify(expandedDetails && expandedDetails->isHidden(),
           "collapsed state survives progress-driven rebuild");
    detailsToggle->click();
    verify(!expandedDetails->isHidden()
               && expandedDetails->findChild<OperationSpeedGraph *>(
                   QStringLiteral("operationSpeedGraph")),
           "expanded details restore the bounded speed graph");
    manager.finish(&timed, true, false, {});
    verify(manager.operations().first().etaSeconds == 0
               && manager.operations().first().averageSpeedBytesPerSecond > 0,
           "successful completion keeps average speed and sets zero ETA");
    manager.clearFinished();

    const QUrl sourceOne = QUrl::fromLocalFile(
        QStringLiteral("/tmp/source/one.txt"));
    const QUrl sourceTwo = QUrl::fromLocalFile(
        QStringLiteral("/tmp/source/two.txt"));
    const QUrl destination = QUrl::fromLocalFile(
        QStringLiteral("/tmp/destination"));
    KIO::CopyJob *copyJob = KIO::copy(
        {sourceOne, sourceTwo},
        destination,
        KIO::HideProgressInfo);
    copyJob->setAutoDelete(false);
    manager.track(copyJob, QStringLiteral("Copying"));
    operations = manager.operations();
    verify(operations.first().sourceUrls == QList<QUrl>{sourceOne, sourceTwo}
               && operations.first().destinationUrl == destination,
           "CopyJob sources and destination are captured structurally");

    const QUrl currentDestination = QUrl::fromLocalFile(
        QStringLiteral("/tmp/destination/two.txt"));
    copyJob->copying(copyJob, sourceTwo, currentDestination);
    QTest::qWait(160);
    operations = manager.operations();
    verify(operations.first().currentSourceUrl == sourceTwo
               && operations.first().currentDestinationUrl == currentDestination,
           "current CopyJob file is updated from transfer signal");
    auto *currentLabel = manager.m_detailedWindow->findChild<QLabel *>(
        QStringLiteral("operationCurrentFileLabel"));
    verify(currentLabel && currentLabel->text().contains(QStringLiteral("two.txt"))
               && !manager.m_detailedWindow->findChild<QLabel *>(
                   QStringLiteral("operationSourceLabel"))
               && !manager.m_detailedWindow->findChild<QLabel *>(
                   QStringLiteral("operationDestinationLabel")),
           "detailed window keeps current file without duplicate path rows");
    auto *percentLabel = manager.m_detailedWindow->findChild<QLabel *>(
        QStringLiteral("operationPercentLabel"));
    verify(percentLabel && percentLabel->font().bold()
               && percentLabel->font().pointSize()
                   > QApplication::font().pointSize(),
           "progress percentage uses a separate prominent label");
    manager.finish(copyJob, false, true, {});
    copyJob->kill(KJob::Quietly);
    manager.clearFinished();

    // History management in OperationManager and OperationWindow
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        TestOperationJob job1(true);
        TestOperationJob job2(true);
        TestOperationJob job3(true);

        manager.track(&job1, QStringLiteral("Job 1 Running"));
        manager.track(&job2, QStringLiteral("Job 2 Completed"));
        manager.track(&job3, QStringLiteral("Job 3 Failed"));

        operations = manager.operations();
        quint64 id1 = operations[0].id;
        quint64 id2 = operations[1].id;
        quint64 id3 = operations[2].id;

        manager.finish(&job2, true, false, {});
        manager.finish(&job3, false, true, QStringLiteral("I/O error"));

        manager.showDetailedWindow();
        auto *clearHistBtn = manager.m_detailedWindow->findChild<QToolButton *>(
            QStringLiteral("operationClearHistoryButton"));
        verify(clearHistBtn != nullptr, "operationClearHistoryButton exists in OperationWindow");
        verify(clearHistBtn->isEnabled(), "clearHistoryButton enabled when finished operations exist");

        // Active job cannot be removed from history
        verify(!manager.removeFinishedOperation(id1), "cannot remove active job from history");

        // Finished queue task has remove button
        TransferQueueController qc;
        manager.setQueueController(&qc);
        quint64 qTaskId = qc.enqueue(TransferTaskDescriptor{}).id;
        qc.cancelTask(qTaskId);
        manager.m_detailedWindow->rebuild();

        auto removeBtns = manager.m_detailedWindow->findChildren<QToolButton *>(
            QStringLiteral("detailedOperationRemoveButton"));
        verify(!removeBtns.isEmpty(), "detailedOperationRemoveButton present on finished cards");
        verify(removeBtns.first()->toolTip().contains(QStringLiteral("histor")),
               "remove button tooltip references history");
        qc.clearFinished();

        // Finished legacy job can be removed
        verify(manager.removeFinishedOperation(id2), "completed job removed from history");
        verify(!manager.removeFinishedOperation(id2), "removing already removed job returns false");

        operations = manager.operations();
        verify(operations.size() == 2, "exactly 2 operations remain");
        verify(operations[0].id == id1 && operations[1].id == id3, "remaining operations keep their IDs");

        // Clear all finished
        int cleared = manager.clearFinished();
        verify(cleared == 1, "clearFinished removed remaining finished job 3");
        operations = manager.operations();
        verify(operations.size() == 1 && operations[0].id == id1, "only active job 1 remains");

        manager.m_detailedWindow->rebuild();
        verify(!clearHistBtn->isEnabled(), "clearHistoryButton disabled when no finished operations exist");

        // Clean up job1
        manager.finish(&job1, false, false, {});
        manager.clearFinished();
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // ==================================================================
    // Stage 3 Safe Retry UX Follow-up: Live update in open OperationWindow
    // ==================================================================
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        QTemporaryDir tempDir;
        verify(tempDir.isValid(), "UX Follow-up: QTemporaryDir is valid");

        const QString srcDir = tempDir.path() + QStringLiteral("/src");
        const QString dstDir = tempDir.path() + QStringLiteral("/dst");
        QDir().mkpath(srcDir);
        QDir().mkpath(dstDir);

        const QString srcFilePath = srcDir + QStringLiteral("/source.txt");
        const QString dstFilePath = dstDir + QStringLiteral("/source.txt");
        {
            QFile f(srcFilePath);
            verify(f.open(QIODevice::WriteOnly), "UX Follow-up: Created test source file");
            f.write("safe-retry-ux-data");
            f.close();
        }

        TransferQueueController qc;
        manager.setQueueController(&qc);
        manager.showDetailedWindow();
        verify(manager.m_detailedWindow != nullptr, "UX Follow-up: detailed window exists");
        verify(manager.m_detailedWindow->isVisible(), "UX Follow-up: detailed window is visible");

        QPointer<TestOperationJob> fakeJob;
        TransferTaskDescriptor desc;
        desc.title = QStringLiteral("UX Test Copy");
        desc.type = TransferOperationType::Copy;
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {QUrl::fromLocalFile(srcFilePath)};
        desc.destination = QUrl::fromLocalFile(dstDir);
        desc.capabilities = defaultTransferCapabilities(desc.backend, desc.type);
        desc.jobFactory = [&fakeJob]() -> KJob* {
            fakeJob = new TestOperationJob(true, true);
            return fakeJob.data();
        };

        const auto enqRes = qc.enqueue(desc);
        verify(enqRes.accepted(), "UX Follow-up: Task enqueued");
        QCoreApplication::processEvents();

        // Simulate initial failure due to read permission error (chmod 000)
        QFile::setPermissions(srcFilePath, QFileDevice::Permissions{});
        if (fakeJob) {
            fakeJob->finishError(KIO::ERR_CANNOT_READ, QStringLiteral("Permission denied"));
        }
        QCoreApplication::processEvents();
        manager.m_detailedWindow->rebuild();

        auto snap = qc.taskSnapshot(enqRes.id);
        verify(snap.has_value() && snap->state == TransferTaskState::Failed,
               "UX Follow-up: Task entered Failed state");
        verify(!manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.1: Initial chmod 000 -> Retry button disabled in open window");
        {
            const QString tt = manager.m_detailedWindow->retryButtonToolTipForTask(enqRes.id);
            verify(tt.contains(QStringLiteral("uprawnie")) || tt.contains(QStringLiteral("permission")),
                   "UX.1: Tooltip references read permissions");
        }

        // UX.1: chmod 000 -> 644 enables button without closing window
        QFile::setPermissions(srcFilePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.1: chmod 644 -> Retry button enabled in open window without reopening");
        {
            const QString tt = manager.m_detailedWindow->retryButtonToolTipForTask(enqRes.id);
            verify(tt.contains(QStringLiteral("Ponów")) || tt.contains(QStringLiteral("Retry")),
                   "UX.1: Tooltip updated to ready state");
        }

        // UX.2: chmod 644 -> 000 disables button
        QFile::setPermissions(srcFilePath, QFileDevice::Permissions{});
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(!manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.2: chmod 000 -> Retry button disabled again");

        // Restore permissions for subsequent checks
        QFile::setPermissions(srcFilePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.2: Restored permissions re-enables button");

        // UX.3: source removal disables retry, restoring re-enables
        const QString srcBackupPath = srcFilePath + QStringLiteral(".bak");
        verify(QFile::rename(srcFilePath, srcBackupPath), "UX.3: Renamed source file");
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(!manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.3: Missing source file disables retry");
        {
            const QString tt = manager.m_detailedWindow->retryButtonToolTipForTask(enqRes.id);
            verify(tt.contains(QStringLiteral("nie istnieje")) || tt.contains(QStringLiteral("exist")),
                   "UX.3: Tooltip indicates source file missing");
        }

        verify(QFile::rename(srcBackupPath, srcFilePath), "UX.3: Restored source file");
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.3: Restoring source file re-enables retry button");

        // UX.5: target conflict creation blocks retry, removal restores availability
        {
            QFile conflictFile(dstFilePath);
            verify(conflictFile.open(QIODevice::WriteOnly), "UX.5: Created conflict file");
            conflictFile.write("target-conflict");
            conflictFile.close();
        }
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(!manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.5: Target conflict blocks retry in open window");
        {
            const QString tt = manager.m_detailedWindow->retryButtonToolTipForTask(enqRes.id);
            verify(tt.contains(QStringLiteral("konflikt")) || tt.contains(QStringLiteral("conflict")),
                   "UX.5: Tooltip indicates conflict");
        }

        verify(QFile::remove(dstFilePath), "UX.5: Removed target conflict file");
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.5: Removing target conflict restores retry availability");

        // UX.5b: .thispc-part temporary file blocks retry, removal restores availability
        const QString partFilePath = dstFilePath + QStringLiteral(".thispc-part");
        {
            QFile partFile(partFilePath);
            verify(partFile.open(QIODevice::WriteOnly), "UX.5b: Created .thispc-part file");
            partFile.write("staged-data");
            partFile.close();
        }
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(!manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.5b: .thispc-part blocks retry");
        {
            const QString tt = manager.m_detailedWindow->retryButtonToolTipForTask(enqRes.id);
            verify(tt.contains(QStringLiteral(".thispc-part")),
                   "UX.5b: Tooltip indicates .thispc-part staging file");
        }

        verify(QFile::remove(partFilePath), "UX.5b: Removed .thispc-part file");
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.5b: Removing .thispc-part restores retry availability");

        // UX.4: inode swap leaves retry disabled
        verify(QFile::remove(srcFilePath), "UX.4: Removed original source file");
        {
            QFile newFile(srcFilePath);
            verify(newFile.open(QIODevice::WriteOnly), "UX.4: Created replacement file with new inode");
            newFile.write("new-inode-data");
            newFile.close();
        }
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(!manager.m_detailedWindow->isRetryButtonEnabledForTask(enqRes.id),
               "UX.4: Inode swap permanently blocks retry for original task");
        {
            const QString tt = manager.m_detailedWindow->retryButtonToolTipForTask(enqRes.id);
            verify(tt.contains(QStringLiteral("podmieniony")) || tt.contains(QStringLiteral("replaced")),
                   "UX.4: Tooltip indicates source replacement");
        }

        // UX.6: Archival card after restart remains strictly disabled
        TransferHistoryRecord archRec = TransferHistoryRecord::createNew(
            TransferOperationType::Copy,
            TransferBackendKind::LocalNative,
            TransferTaskState::Failed,
            {QUrl::fromLocalFile(srcFilePath)},
            QUrl::fromLocalFile(dstDir),
            QDateTime::currentDateTimeUtc().addSecs(-10),
            QDateTime::currentDateTimeUtc(),
            10000,
            1024,
            0,
            std::nullopt,
            100,
            QStringLiteral("Old disk error"),
            QStringLiteral("Archival failed task"));
        archRec.uuid = QStringLiteral("archival-test-uuid-ux6");
        if (qc.historyManager()) {
            qc.historyManager()->addRecord(archRec, false);
            manager.m_detailedWindow->rebuild();

            auto *archRetryBtn = manager.m_detailedWindow->findChild<QToolButton *>(
                QStringLiteral("detailedOperationHistoryRetryButton"));
            verify(archRetryBtn != nullptr, "UX.6: Archival retry button exists");
            verify(!archRetryBtn->isEnabled(), "UX.6: Archival retry button strictly disabled");
            verify(archRetryBtn->toolTip().contains(QStringLiteral("restar"), Qt::CaseInsensitive),
                   "UX.6: Archival retry button explains restart context limitation");
        }

        qc.clearFinished();
        manager.m_detailedWindow->close();
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // ==================================================================
    // Stage 4: Show in Folder ("Pokaż w folderze") — Scenarios A through L
    // ==================================================================
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        QTemporaryDir stage4Dir;
        verify(stage4Dir.isValid(), "Stage 4: QTemporaryDir is valid");

        const QString srcDir = stage4Dir.path() + QStringLiteral("/src");
        const QString dstDir = stage4Dir.path() + QStringLiteral("/dst_copy");
        const QString dstMoveDir = stage4Dir.path() + QStringLiteral("/dst_move");
        const QString dstArchDir = stage4Dir.path() + QStringLiteral("/dst_arch");
        const QString dstMultiDir = stage4Dir.path() + QStringLiteral("/dst_multi");
        const QString dstSpecialDir = stage4Dir.path() + QStringLiteral("/dst_special");
        const QString paneLeftDir = stage4Dir.path() + QStringLiteral("/pane_left");
        const QString paneRightDir = stage4Dir.path() + QStringLiteral("/pane_right");
        const QString dstSplitTargetDir = stage4Dir.path() + QStringLiteral("/dst_split_target");

        for (const QString &d : {srcDir, dstDir, dstMoveDir, dstArchDir, dstMultiDir, dstSpecialDir, paneLeftDir, paneRightDir, dstSplitTargetDir}) {
            verify(QDir().mkpath(d), "Stage 4: Created test directory");
        }

        auto writeTestFile = [](const QString &path, const QByteArray &data = QByteArrayLiteral("stage4-payload")) -> bool {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) {
                return false;
            }
            f.write(data);
            f.close();
            return true;
        };

        // ------------------------------------------------------------------
        // Scenario A: Completed Copy, single file (+ conflict-renamed targetUrls)
        // ------------------------------------------------------------------
        const QString copySrcFile = srcDir + QStringLiteral("/alpha.txt");
        const QString copyDstFile = dstDir + QStringLiteral("/alpha.txt");
        const QString copyOtherFile = dstDir + QStringLiteral("/zzz_unrelated.txt");
        verify(writeTestFile(copySrcFile, "alpha-content"), "Scenario A: Created source file");
        verify(writeTestFile(copyDstFile, "alpha-content"), "Scenario A: Created copied target file");
        verify(writeTestFile(copyOtherFile, "other-content"), "Scenario A: Created unrelated file in dstDir");

        TransferQueueController qc4;
        qc4.clearHistory();
        manager.setQueueController(&qc4);
        manager.showDetailedWindow();
        verify(manager.m_detailedWindow != nullptr && manager.m_detailedWindow->isVisible(),
               "Scenario A: OperationWindow is open");

        QPointer<TestOperationJob> jobA;
        TransferTaskDescriptor descA;
        descA.title = QStringLiteral("Copy alpha.txt");
        descA.type = TransferOperationType::Copy;
        descA.backend = TransferBackendKind::LocalNative;
        descA.sources = {QUrl::fromLocalFile(copySrcFile)};
        descA.destination = QUrl::fromLocalFile(dstDir);
        descA.capabilities = defaultTransferCapabilities(descA.backend, descA.type);
        descA.jobFactory = [&jobA]() -> KJob* {
            jobA = new TestOperationJob(true, true);
            return jobA.data();
        };

        const auto enqA = qc4.enqueue(descA);
        verify(enqA.accepted(), "Scenario A: Task enqueued");
        QCoreApplication::processEvents();
        if (jobA) {
            jobA->report(13, 13, 1024);
            jobA->finishError(0, QString());
        }
        QCoreApplication::processEvents();
        manager.m_detailedWindow->rebuild();

        const auto snapA = qc4.taskSnapshot(enqA.id);
        verify(snapA.has_value() && snapA->state == TransferTaskState::Completed,
               "Scenario A: Task is Completed");
        verify(manager.m_detailedWindow->hasShowInFolderButtonForTask(enqA.id),
               "Scenario A: Completed task has Show in Folder button");
        verify(manager.m_detailedWindow->isShowInFolderButtonEnabledForTask(enqA.id),
               "Scenario A: Show in Folder button is enabled");

        const ShowInFolderResolution resA = qc4.checkShowInFolderEligibility(enqA.id);
        verify(resA.eligible && resA.reason == ShowInFolderIneligibilityReason::None,
               "Scenario A: checkShowInFolderEligibility returns eligible");
        verify(sameLocation(resA.folderUrl, QUrl::fromLocalFile(dstDir)),
               "Scenario A: folderUrl matches destination directory");
        verify(resA.selectUrls.size() == 1
               && sameLocation(resA.selectUrls.first(), QUrl::fromLocalFile(copyDstFile)),
               "Scenario A: selectUrls contains exact copied file URL");

        QSignalSpy spyShowReq(&manager, &OperationManager::showInFolderRequested);
        QSignalSpy spyShowFail(&manager, &OperationManager::showInFolderFailed);
        verify(manager.m_detailedWindow->triggerShowInFolderForTask(enqA.id),
               "Scenario A: triggerShowInFolderForTask succeeds");
        verify(spyShowReq.count() == 1 && spyShowFail.count() == 0,
               "Scenario A: showInFolderRequested emitted once without error");

        // Conflict-renamed single file via targetUrls
        const QString renamedDstFile = dstDir + QStringLiteral("/alpha (1).txt");
        verify(writeTestFile(renamedDstFile, "alpha-renamed"), "Scenario A2: Created conflict-renamed target file");
        QPointer<TestOperationJob> jobA2;
        TransferTaskDescriptor descA2 = descA;
        descA2.targetUrls = {QUrl::fromLocalFile(renamedDstFile)};
        descA2.jobFactory = [&jobA2]() -> KJob* {
            jobA2 = new TestOperationJob(true, true);
            return jobA2.data();
        };
        const auto enqA2 = qc4.enqueue(descA2);
        verify(enqA2.accepted(), "Scenario A2: Conflict-renamed task enqueued");
        QCoreApplication::processEvents();
        if (jobA2) {
            jobA2->finishError(0, QString());
        }
        QCoreApplication::processEvents();
        const ShowInFolderResolution resA2 = qc4.checkShowInFolderEligibility(enqA2.id);
        verify(resA2.eligible && resA2.selectUrls.size() == 1
               && sameLocation(resA2.selectUrls.first(), QUrl::fromLocalFile(renamedDstFile)),
               "Scenario A2: targetUrls override selects conflict-renamed file");

        // ------------------------------------------------------------------
        // Scenario B: Completed Move, destination folder (source removed)
        // ------------------------------------------------------------------
        const QString moveSrcFile = srcDir + QStringLiteral("/moved_report.txt");
        const QString moveDstFile = dstMoveDir + QStringLiteral("/moved_report.txt");
        // Note: moveSrcFile does NOT exist on disk anymore because Move removed it
        verify(!QFileInfo::exists(moveSrcFile), "Scenario B: Source file is absent after Move");
        verify(writeTestFile(moveDstFile, "moved-data"), "Scenario B: Moved file exists at destination");

        QPointer<TestOperationJob> jobB;
        TransferTaskDescriptor descB;
        descB.title = QStringLiteral("Move moved_report.txt");
        descB.type = TransferOperationType::Move;
        descB.backend = TransferBackendKind::LocalNative;
        descB.sources = {QUrl::fromLocalFile(moveSrcFile)};
        descB.destination = QUrl::fromLocalFile(dstMoveDir);
        descB.capabilities = defaultTransferCapabilities(descB.backend, descB.type);
        descB.jobFactory = [&jobB]() -> KJob* {
            jobB = new TestOperationJob(true, true);
            return jobB.data();
        };
        const auto enqB = qc4.enqueue(descB);
        verify(enqB.accepted(), "Scenario B: Move task enqueued");
        QCoreApplication::processEvents();
        if (jobB) {
            jobB->finishError(0, QString());
        }
        QCoreApplication::processEvents();
        manager.m_detailedWindow->rebuild();

        const ShowInFolderResolution resB = qc4.checkShowInFolderEligibility(enqB.id);
        verify(resB.eligible, "Scenario B: Completed Move is eligible even though source was removed");
        verify(sameLocation(resB.folderUrl, QUrl::fromLocalFile(dstMoveDir)),
               "Scenario B: Move points to destination folder, never removed source folder");
        verify(resB.selectUrls.size() == 1
               && sameLocation(resB.selectUrls.first(), QUrl::fromLocalFile(moveDstFile)),
               "Scenario B: Move selects moved file in destination folder");

        // ------------------------------------------------------------------
        // Scenario C: Completed transfer from archival history after restart
        // ------------------------------------------------------------------
        const QString archDstFile = dstArchDir + QStringLiteral("/archival_invoice.pdf");
        verify(writeTestFile(archDstFile, "archival-pdf"), "Scenario C: Created archival target file");

        TransferHistoryRecord archCompleted = TransferHistoryRecord::createNew(
            TransferOperationType::Copy,
            TransferBackendKind::LocalNative,
            TransferTaskState::Completed,
            {QUrl::fromLocalFile(srcDir + QStringLiteral("/archival_invoice.pdf"))},
            QUrl::fromLocalFile(dstArchDir),
            QDateTime::currentDateTimeUtc().addSecs(-120),
            QDateTime::currentDateTimeUtc().addSecs(-115),
            5000,
            4096,
            4096,
            std::nullopt,
            0,
            QString(),
            QStringLiteral("Archival Completed Copy"));
        archCompleted.uuid = QStringLiteral("stage4-archival-completed-uuid");
        archCompleted.targetUrls = {QUrl::fromLocalFile(archDstFile)};

        // Simulate restart: new controller with empty active queue but persisted history record
        TransferQueueController qcRestarted;
        qcRestarted.clearHistory();
        if (qcRestarted.historyManager()) {
            qcRestarted.historyManager()->addRecord(archCompleted, true);
        }
        manager.setQueueController(&qcRestarted);
        manager.m_detailedWindow->rebuild();

        verify(manager.m_detailedWindow->hasShowInFolderButtonForHistory(archCompleted.uuid),
               "Scenario C: Archival completed card after restart has Show in Folder button");
        verify(manager.m_detailedWindow->isShowInFolderButtonEnabledForHistory(archCompleted.uuid),
               "Scenario C: Archival completed card Show in Folder button is enabled");

        spyShowReq.clear();
        verify(manager.m_detailedWindow->triggerShowInFolderForHistory(archCompleted.uuid),
               "Scenario C: Triggering Show in Folder on archival card succeeds");
        verify(spyShowReq.count() == 1,
               "Scenario C: Archival card emitted showInFolderRequested");
        {
            const QList<QVariant> args = spyShowReq.takeFirst();
            const QUrl emittedFolder = args.at(0).toUrl();
            const QList<QUrl> emittedSelect = args.at(1).value<QList<QUrl>>();
            verify(sameLocation(emittedFolder, QUrl::fromLocalFile(dstArchDir)),
                   "Scenario C: Archival card emitted destination folder URL");
            verify(emittedSelect.size() == 1
                   && sameLocation(emittedSelect.first(), QUrl::fromLocalFile(archDstFile)),
                   "Scenario C: Archival card emitted exact target file URL for selection");
        }

        // Restore qc4 on manager
        manager.setQueueController(&qc4);
        manager.m_detailedWindow->rebuild();

        // ------------------------------------------------------------------
        // Scenario D: Multiple files in one task (opens folder, no random single selection)
        // ------------------------------------------------------------------
        const QString multi1 = dstMultiDir + QStringLiteral("/m1.txt");
        const QString multi2 = dstMultiDir + QStringLiteral("/m2.txt");
        const QString multi3 = dstMultiDir + QStringLiteral("/m3.txt");
        verify(writeTestFile(multi1, "m1") && writeTestFile(multi2, "m2") && writeTestFile(multi3, "m3"),
               "Scenario D: Created multi-item files in destination");

        QPointer<TestOperationJob> jobD;
        TransferTaskDescriptor descD;
        descD.title = QStringLiteral("Copy 3 files");
        descD.type = TransferOperationType::Copy;
        descD.backend = TransferBackendKind::LocalNative;
        descD.sources = {
            QUrl::fromLocalFile(srcDir + QStringLiteral("/m1.txt")),
            QUrl::fromLocalFile(srcDir + QStringLiteral("/m2.txt")),
            QUrl::fromLocalFile(srcDir + QStringLiteral("/m3.txt"))
        };
        descD.destination = QUrl::fromLocalFile(dstMultiDir);
        descD.capabilities = defaultTransferCapabilities(descD.backend, descD.type);
        descD.jobFactory = [&jobD]() -> KJob* {
            jobD = new TestOperationJob(true, true);
            return jobD.data();
        };
        const auto enqD = qc4.enqueue(descD);
        verify(enqD.accepted(), "Scenario D: Multi-item task enqueued");
        QCoreApplication::processEvents();
        if (jobD) {
            jobD->finishError(0, QString());
        }
        QCoreApplication::processEvents();
        manager.m_detailedWindow->rebuild();

        const ShowInFolderResolution resD = qc4.checkShowInFolderEligibility(enqD.id);
        verify(resD.eligible, "Scenario D: Multi-item completed task is eligible");
        verify(sameLocation(resD.folderUrl, QUrl::fromLocalFile(dstMultiDir)),
               "Scenario D: Multi-item task resolves destination directory");
        verify(resD.selectUrls.isEmpty(),
               "Scenario D: Multi-item task does not select a random single file (selectUrls is empty)");

        // ------------------------------------------------------------------
        // Scenario E: Changed or missing target file / destination directory + live watcher
        // ------------------------------------------------------------------
        const QString copyDstBackup = copyDstFile + QStringLiteral(".moved_away");
        verify(QFile::rename(copyDstFile, copyDstBackup),
               "Scenario E: Renamed/removed target file outside ThisPC");
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();

        verify(!manager.m_detailedWindow->isShowInFolderButtonEnabledForTask(enqA.id),
               "Scenario E: Live watcher disables Show in Folder button when target file disappears");
        {
            const QString tt = manager.m_detailedWindow->showInFolderButtonToolTipForTask(enqA.id);
            verify(tt.contains(QStringLiteral("nie istnieje")) || tt.contains(QStringLiteral("no longer exists")),
                   "Scenario E: Tooltip explains target file no longer exists");
        }
        spyShowReq.clear();
        spyShowFail.clear();
        verify(!manager.m_detailedWindow->triggerShowInFolderForTask(enqA.id),
               "Scenario E: Triggering Show in Folder when target file is missing fails closed");
        verify(spyShowReq.count() == 0 && spyShowFail.count() == 1,
               "Scenario E: Emits showInFolderFailed and does not emit showInFolderRequested");

        // Restore target file -> live watcher re-enables button without reopening window
        verify(QFile::rename(copyDstBackup, copyDstFile),
               "Scenario E: Restored target file");
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        verify(manager.m_detailedWindow->isShowInFolderButtonEnabledForTask(enqA.id),
               "Scenario E: Restoring target file re-enables Show in Folder button live");

        // Missing destination directory for multi-item task when all items removed
        verify(QFile::remove(multi1) && QFile::remove(multi2) && QFile::remove(multi3),
               "Scenario E2: Removed all transferred items from multi-item destination");
        const ShowInFolderResolution resE2 = qc4.checkShowInFolderEligibility(enqD.id);
        verify(!resE2.eligible && resE2.reason == ShowInFolderIneligibilityReason::TargetItemMissing,
               "Scenario E2: Multi-item transfer fails closed when none of its transferred items exist anymore");
        verify(writeTestFile(multi1, "m1") && writeTestFile(multi2, "m2") && writeTestFile(multi3, "m3"),
               "Scenario E2: Restored multi-item files");

        // ------------------------------------------------------------------
        // Scenario F: Disconnected media (unmounted/removed mount dir & MTP)
        // ------------------------------------------------------------------
        const QString usbMountDir = stage4Dir.path() + QStringLiteral("/usb_removable_media/photos");
        verify(QDir().mkpath(usbMountDir), "Scenario F: Created simulated USB mount directory");
        const QString usbFile = usbMountDir + QStringLiteral("/dsc001.jpg");
        verify(writeTestFile(usbFile, "jpeg-bytes"), "Scenario F: Created file on simulated USB media");

        QPointer<TestOperationJob> jobF;
        TransferTaskDescriptor descF;
        descF.title = QStringLiteral("Copy to USB");
        descF.type = TransferOperationType::Copy;
        descF.backend = TransferBackendKind::LocalNative;
        descF.sources = {QUrl::fromLocalFile(copySrcFile)};
        descF.destination = QUrl::fromLocalFile(usbMountDir);
        descF.targetUrls = {QUrl::fromLocalFile(usbFile)};
        descF.capabilities = defaultTransferCapabilities(descF.backend, descF.type);
        descF.jobFactory = [&jobF]() -> KJob* {
            jobF = new TestOperationJob(true, true);
            return jobF.data();
        };
        const auto enqF = qc4.enqueue(descF);
        verify(enqF.accepted(), "Scenario F: USB task enqueued");
        QCoreApplication::processEvents();
        if (jobF) {
            jobF->finishError(0, QString());
        }
        QCoreApplication::processEvents();
        verify(qc4.checkShowInFolderEligibility(enqF.id).eligible,
               "Scenario F: Eligible while USB media is mounted");

        // Simulate USB disconnect
        verify(QDir(stage4Dir.path() + QStringLiteral("/usb_removable_media")).removeRecursively(),
               "Scenario F: Simulated USB media disconnection");
        manager.m_detailedWindow->triggerRetryWatcherCheckNow();
        const ShowInFolderResolution resF = qc4.checkShowInFolderEligibility(enqF.id);
        verify(!resF.eligible && resF.reason == ShowInFolderIneligibilityReason::DestinationDirectoryMissing,
               "Scenario F: Disconnected USB mount fails closed with DestinationDirectoryMissing");
        verify(!manager.m_detailedWindow->isShowInFolderButtonEnabledForTask(enqF.id),
               "Scenario F: Button disabled after USB disconnect");

        // MTP destination URL
        const ShowInFolderResolution resMtp = resolveShowInFolder(
            TransferTaskState::Completed,
            {QUrl::fromLocalFile(copySrcFile)},
            QUrl(QStringLiteral("mtp:/Phone/Internal shared storage/Download")),
            {},
            1);
        verify(!resMtp.eligible && resMtp.reason == ShowInFolderIneligibilityReason::DisconnectedOrMtpMedia,
               "Scenario F: MTP destination fails closed with DisconnectedOrMtpMedia");

        // ------------------------------------------------------------------
        // Scenario G: Remote or unsupported KIO locations
        // ------------------------------------------------------------------
        for (const QString &remoteUrlStr : {
                 QStringLiteral("smb://fileserver/share/docs"),
                 QStringLiteral("sftp://192.168.1.10/home/user/docs"),
                 QStringLiteral("fish://192.168.1.10/var/tmp")}) {
            const ShowInFolderResolution resRem = resolveShowInFolder(
                TransferTaskState::Completed,
                {QUrl::fromLocalFile(copySrcFile)},
                QUrl(remoteUrlStr),
                {},
                1);
            verify(!resRem.eligible && resRem.reason == ShowInFolderIneligibilityReason::RemoteLocationUnsupported,
                   "Scenario G: Remote KIO destination fails closed with RemoteLocationUnsupported");
        }

        for (const QString &unsupportedUrlStr : {
                 QStringLiteral("trash:/"),
                 QStringLiteral("admin:///etc"),
                 QStringLiteral("http://example.com/files"),
                 QStringLiteral("thispc:/"),
                 QStringLiteral("thispcsearch:/?q=test")}) {
            const ShowInFolderResolution resUnsup = resolveShowInFolder(
                TransferTaskState::Completed,
                {QUrl::fromLocalFile(copySrcFile)},
                QUrl(unsupportedUrlStr),
                {},
                1);
            verify(!resUnsup.eligible && resUnsup.reason == ShowInFolderIneligibilityReason::UnsupportedScheme,
                   "Scenario G: Unsupported scheme fails closed with UnsupportedScheme");
        }

        // ------------------------------------------------------------------
        // Scenario H: Directory, symlink (including dangling symlink), and special filenames
        // ------------------------------------------------------------------
        // H1: Directory transfer
        const QString copiedSubdir = dstSpecialDir + QStringLiteral("/CopiedProjectFolder");
        verify(QDir().mkpath(copiedSubdir), "Scenario H1: Created copied directory in destination");
        const ShowInFolderResolution resH1 = resolveShowInFolder(
            TransferTaskState::Completed,
            {QUrl::fromLocalFile(srcDir + QStringLiteral("/CopiedProjectFolder"))},
            QUrl::fromLocalFile(dstSpecialDir),
            {QUrl::fromLocalFile(copiedSubdir)},
            1);
        verify(resH1.eligible
               && sameLocation(resH1.folderUrl, QUrl::fromLocalFile(dstSpecialDir))
               && resH1.selectUrls.size() == 1
               && sameLocation(resH1.selectUrls.first(), QUrl::fromLocalFile(copiedSubdir)),
               "Scenario H1: Completed directory transfer opens parent destination and selects copied directory");

        // H2: Symlink transfer (valid symlink and dangling symlink via ::lstat)
        const QString danglingLinkPath = dstSpecialDir + QStringLiteral("/dangling_symlink.lnk");
        verify(::symlink("/nonexistent/target/for/stage4/dangling", QFile::encodeName(danglingLinkPath).constData()) == 0,
               "Scenario H2: Created dangling symlink in destination");
        const ShowInFolderResolution resH2 = resolveShowInFolder(
            TransferTaskState::Completed,
            {QUrl::fromLocalFile(srcDir + QStringLiteral("/dangling_symlink.lnk"))},
            QUrl::fromLocalFile(dstSpecialDir),
            {QUrl::fromLocalFile(danglingLinkPath)},
            1);
        verify(resH2.eligible
               && resH2.selectUrls.size() == 1
               && sameLocation(resH2.selectUrls.first(), QUrl::fromLocalFile(danglingLinkPath)),
               "Scenario H2: Dangling symlink at destination is detected via lstat and selected without following target");

        // H3: Unusual filenames (spaces, Polish diacritics, #, %, brackets)
        const QString specialName = QStringLiteral("plik ze spacją ąęćłńóśźż #1 %20 [test].txt");
        const QString specialDstFile = dstSpecialDir + QLatin1Char('/') + specialName;
        verify(writeTestFile(specialDstFile, "special-chars"), "Scenario H3: Created file with special characters");
        const ShowInFolderResolution resH3 = resolveShowInFolder(
            TransferTaskState::Completed,
            {QUrl::fromLocalFile(srcDir + QLatin1Char('/') + specialName)},
            QUrl::fromLocalFile(dstSpecialDir),
            {QUrl::fromLocalFile(specialDstFile)},
            1);
        verify(resH3.eligible
               && resH3.selectUrls.size() == 1
               && resH3.selectUrls.first().toLocalFile() == specialDstFile,
               "Scenario H3: File with spaces, diacritics, #, %, and brackets resolves accurately");

        // ------------------------------------------------------------------
        // Scenario J: Failed, Cancelled, Queued, Running, Paused operations
        // ------------------------------------------------------------------
        for (TransferTaskState nonCompleted : {
                 TransferTaskState::Queued,
                 TransferTaskState::Running,
                 TransferTaskState::Paused,
                 TransferTaskState::Cancelling,
                 TransferTaskState::Cancelled,
                 TransferTaskState::Failed}) {
            const ShowInFolderResolution resNonComp = resolveShowInFolder(
                nonCompleted,
                {QUrl::fromLocalFile(copySrcFile)},
                QUrl::fromLocalFile(dstDir),
                {QUrl::fromLocalFile(copyDstFile)},
                1);
            verify(!resNonComp.eligible && resNonComp.reason == ShowInFolderIneligibilityReason::NotCompleted,
                   "Scenario J: Non-completed state is strictly ineligible for Show in Folder");
        }

        // Verify Failed and Cancelled queue tasks in OperationWindow do NOT render Show in Folder button
        QPointer<TestOperationJob> jobFailed;
        TransferTaskDescriptor descFail = descA;
        descFail.title = QStringLiteral("Failed task");
        descFail.jobFactory = [&jobFailed]() -> KJob* {
            jobFailed = new TestOperationJob(true, true);
            return jobFailed.data();
        };
        const auto enqFail = qc4.enqueue(descFail);
        QCoreApplication::processEvents();
        if (jobFailed) {
            jobFailed->finishError(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        }
        QCoreApplication::processEvents();

        QPointer<TestOperationJob> jobCancelled;
        TransferTaskDescriptor descCancel = descA;
        descCancel.title = QStringLiteral("Cancelled task");
        descCancel.jobFactory = [&jobCancelled]() -> KJob* {
            jobCancelled = new TestOperationJob(true, true);
            return jobCancelled.data();
        };
        const auto enqCancel = qc4.enqueue(descCancel);
        QCoreApplication::processEvents();
        qc4.cancelTask(enqCancel.id);
        QCoreApplication::processEvents();
        manager.m_detailedWindow->rebuild();

        verify(!manager.m_detailedWindow->hasShowInFolderButtonForTask(enqFail.id),
               "Scenario J: Failed task card does not expose Show in Folder button");
        verify(!manager.m_detailedWindow->hasShowInFolderButtonForTask(enqCancel.id),
               "Scenario J: Cancelled task card does not expose Show in Folder button");

        // ------------------------------------------------------------------
        // Scenario K: Corrupted or incomplete history records & unsafe paths
        // ------------------------------------------------------------------
        // K1: Empty destination
        {
            TransferHistoryRecord r = archCompleted;
            r.destinationUrl = QUrl();
            const ShowInFolderResolution res = resolveShowInFolder(r);
            verify(!res.eligible && res.reason == ShowInFolderIneligibilityReason::InvalidDestination,
                   "Scenario K1: Empty destinationUrl rejected");
        }
        // K2: Empty sourceUrls or mismatched itemCount
        {
            TransferHistoryRecord r = archCompleted;
            r.sourceUrls.clear();
            const ShowInFolderResolution res1 = resolveShowInFolder(r);
            verify(!res1.eligible && res1.reason == ShowInFolderIneligibilityReason::CorruptedRecord,
                   "Scenario K2a: Empty sourceUrls rejected as CorruptedRecord");

            TransferHistoryRecord r2 = archCompleted;
            r2.itemCount = 5;
            const ShowInFolderResolution res2 = resolveShowInFolder(r2);
            verify(!res2.eligible && res2.reason == ShowInFolderIneligibilityReason::CorruptedRecord,
                   "Scenario K2b: Mismatched itemCount rejected as CorruptedRecord");
        }
        // K3: Path traversal '..' in destination, source, or target
        {
            const ShowInFolderResolution resTravDst = resolveShowInFolder(
                TransferTaskState::Completed,
                {QUrl::fromLocalFile(copySrcFile)},
                QUrl::fromLocalFile(dstDir + QStringLiteral("/../dst_copy")),
                {},
                1);
            verify(!resTravDst.eligible && resTravDst.reason == ShowInFolderIneligibilityReason::UnsafePath,
                   "Scenario K3a: '..' in destination path rejected as UnsafePath");

            const ShowInFolderResolution resTravTgt = resolveShowInFolder(
                TransferTaskState::Completed,
                {QUrl::fromLocalFile(copySrcFile)},
                QUrl::fromLocalFile(dstDir),
                {QUrl::fromLocalFile(dstDir + QStringLiteral("/../src/alpha.txt"))},
                1);
            verify(!resTravTgt.eligible && resTravTgt.reason == ShowInFolderIneligibilityReason::UnsafePath,
                   "Scenario K3b: '..' in targetUrls rejected as UnsafePath");
        }
        // K4: Destination directory is a symlink -> refused via lstat
        {
            const QString symlinkDir = stage4Dir.path() + QStringLiteral("/symlink_to_dst");
            verify(::symlink(QFile::encodeName(dstDir).constData(), QFile::encodeName(symlinkDir).constData()) == 0,
                   "Scenario K4: Created symlink to destination directory");
            const ShowInFolderResolution resSymDir = resolveShowInFolder(
                TransferTaskState::Completed,
                {QUrl::fromLocalFile(copySrcFile)},
                QUrl::fromLocalFile(symlinkDir),
                {},
                1);
            verify(!resSymDir.eligible && resSymDir.reason == ShowInFolderIneligibilityReason::UnsafePath,
                   "Scenario K4: Symlinked destination directory rejected as UnsafePath");
        }
        // K5: Target is a .thispc-part temporary file
        {
            const QString partTarget = dstDir + QStringLiteral("/alpha.txt.thispc-part");
            verify(writeTestFile(partTarget, "part"), "Scenario K5: Created .thispc-part file");
            const ShowInFolderResolution resPart = resolveShowInFolder(
                TransferTaskState::Completed,
                {QUrl::fromLocalFile(copySrcFile)},
                QUrl::fromLocalFile(dstDir),
                {QUrl::fromLocalFile(partTarget)},
                1);
            verify(!resPart.eligible && resPart.reason == ShowInFolderIneligibilityReason::TargetItemMissing,
                   "Scenario K5: .thispc-part file rejected for Show in Folder");
            QFile::remove(partTarget);
        }

        // ------------------------------------------------------------------
        // Scenario I & End-to-End GUI Navigation in ThisPcWindow
        // (Primary / Split View routing, strict pane independence, selection)
        // ------------------------------------------------------------------
        {
            const QString leftStayFile = paneLeftDir + QStringLiteral("/left_stay.txt");
            const QString rightStayFile = paneRightDir + QStringLiteral("/right_stay.txt");
            const QString splitTargetFile = dstSplitTargetDir + QStringLiteral("/split_revealed.txt");
            const QString splitUnrelatedFile = dstSplitTargetDir + QStringLiteral("/split_other.txt");
            verify(writeTestFile(leftStayFile, "left")
                   && writeTestFile(rightStayFile, "right")
                   && writeTestFile(splitTargetFile, "split-target")
                   && writeTestFile(splitUnrelatedFile, "split-other"),
                   "Scenario I: Created fixture files for Primary/Split View test");

            ThisPcWindow fullWin(QUrl::fromLocalFile(paneLeftDir));
            fullWin.show();
            fullWin.activateWindow();
            verify(QTest::qWaitFor([&] {
                return !fullWin.m_primaryPane->isLoading()
                    && fullWin.m_directoryList->count() >= 1;
            }, 5000), "Scenario I: Primary pane loaded paneLeftDir");

            // Select left_stay.txt in Primary
            DirectorySelectionSnapshot leftSel;
            leftSel.urls.insert(normalizedUrl(QUrl::fromLocalFile(leftStayFile)).toString());
            leftSel.currentUrl = normalizedUrl(QUrl::fromLocalFile(leftStayFile)).toString();
            restoreDirectorySelection(fullWin.m_directoryList, fullWin.m_directoryDetails, leftSel);
            verify(selectedDirectoryListUrls(fullWin.m_directoryList).contains(leftSel.currentUrl),
                   "Scenario I: left_stay.txt is selected in Primary pane");

            // Enable Split View and load paneRightDir
            fullWin.setSplitViewEnabled(true);
            fullWin.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(paneRightDir), true);
            verify(QTest::qWaitFor([&] {
                return !fullWin.m_splitPane->listingJob()
                    && fullWin.m_splitPane->listView()->count() >= 1;
            }, 5000), "Scenario I: Split pane loaded paneRightDir");

            // Select right_stay.txt in Split
            DirectorySelectionSnapshot rightSel;
            rightSel.urls.insert(normalizedUrl(QUrl::fromLocalFile(rightStayFile)).toString());
            rightSel.currentUrl = normalizedUrl(QUrl::fromLocalFile(rightStayFile)).toString();
            restoreDirectorySelection(fullWin.m_splitPane->listView(), fullWin.m_splitPane->detailsView(), rightSel);
            verify(selectedDirectoryListUrls(fullWin.m_splitPane->listView()).contains(rightSel.currentUrl),
                   "Scenario I: right_stay.txt is selected in Split pane");

            // Case I.1: Primary is active -> Show in Folder opens dstDir and selects alpha.txt in Primary,
            // while Split remains at paneRightDir with right_stay.txt selected!
            fullWin.setActivePane(PaneId::Primary);
            Q_EMIT fullWin.m_operationManager->showInFolderRequested(
                QUrl::fromLocalFile(dstDir),
                {QUrl::fromLocalFile(copyDstFile)});

            verify(QTest::qWaitFor([&] {
                return sameLocation(fullWin.m_navigation.currentUrl(), QUrl::fromLocalFile(dstDir))
                    && !fullWin.m_primaryPane->isLoading()
                    && fullWin.m_directoryList->count() >= 2;
            }, 5000), "Scenario I.1: Primary pane navigated to dstDir");

            const QString expectedAlphaUrl = normalizedUrl(QUrl::fromLocalFile(copyDstFile)).toString();
            const QSet<QString> primarySelectedAfterI1 = selectedDirectoryListUrls(fullWin.m_directoryList);
            verify(primarySelectedAfterI1.size() == 1 && primarySelectedAfterI1.contains(expectedAlphaUrl),
                   "Scenario I.1: Primary pane selected ONLY alpha.txt in dstDir");
            verify(sameLocation(fullWin.m_splitPane->currentUrl(), QUrl::fromLocalFile(paneRightDir)),
                   "Scenario I.1: Split pane location remained untouched at paneRightDir");
            verify(selectedDirectoryListUrls(fullWin.m_splitPane->listView()) == rightSel.urls,
                   "Scenario I.1: Split pane selection remained untouched (right_stay.txt)");

            // Case I.2: Split is active -> Show in Folder opens dstSplitTargetDir and selects split_revealed.txt in Split,
            // while Primary remains at dstDir with alpha.txt selected!
            fullWin.setActivePane(PaneId::Split);
            Q_EMIT fullWin.m_operationManager->showInFolderRequested(
                QUrl::fromLocalFile(dstSplitTargetDir),
                {QUrl::fromLocalFile(splitTargetFile)});

            verify(QTest::qWaitFor([&] {
                return sameLocation(fullWin.m_splitPane->currentUrl(), QUrl::fromLocalFile(dstSplitTargetDir))
                    && !fullWin.m_splitPane->listingJob()
                    && fullWin.m_splitPane->listView()->count() >= 2;
            }, 5000), "Scenario I.2: Split pane navigated to dstSplitTargetDir");

            const QString expectedSplitUrl = normalizedUrl(QUrl::fromLocalFile(splitTargetFile)).toString();
            const QSet<QString> splitSelectedAfterI2 = selectedDirectoryListUrls(fullWin.m_splitPane->listView());
            verify(splitSelectedAfterI2.size() == 1 && splitSelectedAfterI2.contains(expectedSplitUrl),
                   "Scenario I.2: Split pane selected ONLY split_revealed.txt");
            verify(sameLocation(fullWin.m_navigation.currentUrl(), QUrl::fromLocalFile(dstDir)),
                   "Scenario I.2: Primary pane location remained untouched at dstDir");
            verify(selectedDirectoryListUrls(fullWin.m_directoryList) == primarySelectedAfterI1,
                   "Scenario I.2: Primary pane selection remained untouched (alpha.txt)");

            // Case I.3: Multi-item Show in Folder in Primary clears selection so no random file is selected
            fullWin.setActivePane(PaneId::Primary);
            Q_EMIT fullWin.m_operationManager->showInFolderRequested(
                QUrl::fromLocalFile(dstMultiDir),
                {});
            verify(QTest::qWaitFor([&] {
                return sameLocation(fullWin.m_navigation.currentUrl(), QUrl::fromLocalFile(dstMultiDir))
                    && !fullWin.m_primaryPane->isLoading()
                    && fullWin.m_directoryList->count() == 3;
            }, 5000), "Scenario I.3: Primary pane navigated to dstMultiDir");
            verify(selectedDirectoryListUrls(fullWin.m_directoryList).isEmpty(),
                   "Scenario I.3: Multi-item Show in Folder leaves selection empty in Primary pane");
            verify(sameLocation(fullWin.m_splitPane->currentUrl(), QUrl::fromLocalFile(dstSplitTargetDir))
                   && selectedDirectoryListUrls(fullWin.m_splitPane->listView()) == splitSelectedAfterI2,
                   "Scenario I.3: Split pane remained completely untouched");

            fullWin.m_refreshTimer.stop();
            fullWin.m_primaryPane->cancelListing();
            fullWin.m_splitPane->cancelListing();
            fullWin.close();
        }

        // ------------------------------------------------------------------
        // Scenario L: GUI stability & coexistence of Safe Retry + Show in Folder
        // ------------------------------------------------------------------
        verify(manager.m_detailedWindow->hasShowInFolderButtonForTask(enqA.id),
               "Scenario L: Completed card still has Show in Folder button");
        verify(!manager.m_detailedWindow->hasShowInFolderButtonForTask(enqFail.id),
               "Scenario L: Failed card still does not have Show in Folder button");
        // Verify Failed card has Retry button while Completed card has Show in Folder button
        {
            const auto retryBtns = manager.m_detailedWindow->findChildren<QToolButton *>(
                QStringLiteral("detailedOperationRetryButton"));
            const auto showBtns = manager.m_detailedWindow->findChildren<QToolButton *>(
                QStringLiteral("detailedOperationShowInFolderButton"));
            verify(!retryBtns.isEmpty() && !showBtns.isEmpty(),
                   "Scenario L: Retry buttons on Failed cards and Show in Folder buttons on Completed cards coexist cleanly");
        }

        qc4.clearFinished();
        qc4.clearHistory();
        manager.m_detailedWindow->close();
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    qInfo("PASS: %d OperationManager assertions", checks);
    return 0;
}
