class TestOperationJob final : public KJob
{
public:
    explicit TestOperationJob(bool killable)
    {
        setAutoDelete(false);
        setCapabilities(killable ? KJob::Killable : KJob::NoCapabilities);
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

public:
    bool killed = false;
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

    qInfo("PASS: %d OperationManager assertions", checks);
    return 0;
}
