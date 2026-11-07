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

    qInfo("PASS: %d OperationManager assertions", checks);
    return 0;
}
