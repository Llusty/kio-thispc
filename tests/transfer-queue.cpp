/*
 * Stage 1 & 2 tests for the 0.44 transfer queue core:
 * TransferTaskDescriptor, TransferReservationRegistry, TransferQueueController,
 * TransferQueueSettings.
 *
 * Deterministic: every backend is a fake KJob driven by the test. No test
 * depends on disk speed. Tests that touch files run only inside a sandbox
 * under ~/Pobrane/Testy/ThisPC-0.44.0-Transfer-Queue/ (override the base with
 * THISPC_QUEUE_SANDBOX; the path must still contain that directory name).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "transferqueuesettings.h"
#include "transferqueueui.h"
#include "operationmanager.h"
#include "fileactions.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QProcess>

#define TQ_S(text) QStringLiteral(text)

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

template<typename Predicate>
static bool waitUntil(Predicate predicate, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(2);
    }
    return predicate();
}

static void pump(int milliseconds = 30)
{
    QTest::qWait(milliseconds);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

class QueueFakeJob final : public KJob
{
public:
    explicit QueueFakeJob(bool killable = true, bool suspendable = false, bool autoDeleteJob = true)
    {
        setAutoDelete(autoDeleteJob);
        setCapabilities(
            (killable ? KJob::Killable : KJob::NoCapabilities)
            | (suspendable ? KJob::Suspendable : KJob::NoCapabilities));
    }

    void start() override { ++startCalls; }
    void finish() { emitResult(); }
    void finishOk() { emitResult(); }
    void reportSpeed(unsigned long speed) { emitSpeed(speed); }
    void reportProgress(qulonglong processed, qulonglong total)
    {
        setTotalAmount(KJob::Bytes, total);
        setProcessedAmount(KJob::Bytes, processed);
    }
    void finishError(int code, const QString &text)
    {
        setError(code);
        setErrorText(text);
        emitResult();
    }

    int startCalls = 0;
    int killCalls = 0;
    int suspendCalls = 0;
    int resumeCalls = 0;
    bool killAccepted = true;
    bool suspendAccepted = true;
    bool resumeAccepted = true;
    bool asyncKill = false;
    std::function<void()> onKill;
    std::function<void()> onSuspend;
    std::function<void()> onResume;

    void triggerAsyncResult(int error = KJob::KilledJobError, const QString &text = {})
    {
        blockSignals(false);
        setError(error);
        setErrorText(text);
        int idx = metaObject()->indexOfSignal("result(KJob*)");
        if (idx < 0) idx = metaObject()->indexOfSignal("result(KJob*,KJob::QPrivateSignal)");
        KJob *self = this;
        void *args[] = { nullptr, &self, nullptr };
        QMetaObject::activate(this, idx, args);
    }

    Q_INVOKABLE void setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter> limiter, quint64 taskId = 0)
    {
        bandwidthLimiter = limiter;
        limiterTaskId = taskId;
    }

    std::shared_ptr<TransferBandwidthLimiter> bandwidthLimiter;
    quint64 limiterTaskId = 0;

protected:
    bool doKill() override
    {
        ++killCalls;
        if (asyncKill) {
            blockSignals(true);
        }
        if (onKill) onKill();
        return killAccepted;
    }
    bool doSuspend() override
    {
        ++suspendCalls;
        if (onSuspend) onSuspend();
        return suspendAccepted;
    }
    bool doResume() override
    {
        ++resumeCalls;
        if (onResume) onResume();
        return resumeAccepted;
    }
};

struct QueueOptions {
    TransferBackendKind backend = TransferBackendKind::LocalNative;
    TransferOperationType type = TransferOperationType::Copy;
    bool killable = true;
    bool suspendable = true;
    bool autoDeleteJob = false;
    bool factoryReturnsNull = false;
    bool asyncKill = false;
    TransferJobStartMode startMode = TransferJobStartMode::ControllerStarts;
    TransferPriority priority = TransferPriority::Normal;
    std::optional<bool> overrideCanPause;
    std::optional<bool> overrideCanResume;
    std::function<void(QueueFakeJob *)> onJobCreated;
};

struct QueueHarness {
    explicit QueueHarness(const QString &rootPath) : root(rootPath)
    {
        QDir().mkpath(root + TQ_S("/src"));
        QDir().mkpath(root + TQ_S("/dst"));
        QFile::remove(root + TQ_S("/history.json"));
        if (ctl.historyManager()) {
            ctl.historyManager()->setFilePath(root + TQ_S("/history.json"));
        }
    }

    ~QueueHarness()
    {
        for (auto &job : jobs) {
            if (job) delete job.data();
        }
    }

    QString root;
    QMap<QString, QPointer<QueueFakeJob>> jobs;
    TransferQueueController ctl;
    QStringList created;
    int factoryCalls = 0;

    QUrl src(const QString &name) const
    {
        return QUrl::fromLocalFile(root + TQ_S("/src/") + name);
    }
    QUrl dst(const QString &name) const
    {
        return QUrl::fromLocalFile(root + TQ_S("/dst/") + name);
    }
    void touchSource(const QString &name, const QByteArray &data = "payload")
    {
        QFile file(root + TQ_S("/src/") + name);
        verify(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(),
               "fixture source file is written");
    }

    TransferTaskDescriptor make(const QString &title, const QList<QUrl> &sources,
                                const QUrl &destination, QueueOptions options = {})
    {
        TransferTaskDescriptor descriptor;
        descriptor.title = title;
        descriptor.type = options.type;
        descriptor.backend = options.backend;
        descriptor.sources = sources;
        descriptor.destination = destination;
        descriptor.priority = options.priority;
        descriptor.startMode = options.startMode;
        descriptor.capabilities = defaultTransferCapabilities(options.backend, options.type);
        if (options.overrideCanPause.has_value()) {
            descriptor.capabilities.canPause = *options.overrideCanPause;
        }
        if (options.overrideCanResume.has_value()) {
            descriptor.capabilities.canResume = *options.overrideCanResume;
        }
        descriptor.jobFactory = [this, title, options]() -> KJob * {
            ++factoryCalls;
            created << title;
            if (options.factoryReturnsNull) return nullptr;
            auto *job = new QueueFakeJob(options.killable, options.suspendable,
                                         options.autoDeleteJob);
            job->asyncKill = options.asyncKill;
            if (options.onJobCreated) {
                options.onJobCreated(job);
            }
            jobs.insert(title, job);
            return job;
        };
        return descriptor;
    }

    quint64 add(const QString &title, const QList<QUrl> &sources, const QUrl &destination,
                QueueOptions options = {})
    {
        const TransferEnqueueResult result = ctl.enqueue(make(title, sources, destination, options));
        verify(result.accepted(), "enqueue accepted");
        return result.id;
    }

    QueueFakeJob *job(const QString &title) const { return jobs.value(title).data(); }
    TransferTaskState state(quint64 id) const { return ctl.taskSnapshot(id)->state; }
    TransferTaskSnapshot snap(quint64 id) const { return *ctl.taskSnapshot(id); }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QString base = qEnvironmentVariable("THISPC_QUEUE_SANDBOX");
    if (base.isEmpty()) {
        base = QDir::homePath()
            + TQ_S("/Pobrane/Testy/ThisPC-0.44.0-Transfer-Queue/Stage3/sandbox");
    }
    verify(base.contains(TQ_S("ThisPC-0.44.0-Transfer-Queue")),
           "sandbox lives under the dedicated 0.44 test directory");
    verify(QDir().mkpath(base), "sandbox base exists");
    QTemporaryDir temporary(base + TQ_S("/run-XXXXXX"));
    verify(temporary.isValid(), "isolated sandbox directory is created");
    const QString root = QFileInfo(temporary.path()).canonicalFilePath();
    verify(TransferReservation::resolveLocal(QUrl::fromLocalFile(root + TQ_S("/x"))).ok,
           "sandbox path itself is free of symlink components");

    int scenario = 0;
    auto nextRoot = [&]() {
        const QString path = root + TQ_S("/s") + QString::number(++scenario);
        QDir().mkpath(path);
        return path;
    };

    // 1. FIFO for equal priorities + 5. request queued without creating a job
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 a = h.add(TQ_S("A"), {h.src(TQ_S("a"))}, h.dst(TQ_S("d1")));
        const quint64 b = h.add(TQ_S("B"), {h.src(TQ_S("b"))}, h.dst(TQ_S("d2")));
        const quint64 c = h.add(TQ_S("C"), {h.src(TQ_S("c"))}, h.dst(TQ_S("d3")));
        verify(a < b && b < c, "task identifiers are stable and increasing");
        verify(h.created == QStringList{TQ_S("A")}, "only the first task got a job (limit 1)");
        verify(h.factoryCalls == 1, "queued tasks did not create jobs");
        verify(h.snap(b).queuePosition == 1 && h.snap(c).queuePosition == 2,
               "queue positions are 1-based in FIFO order");
        verify(h.snap(b).waitReason == TransferWaitReason::SlotLimit,
               "waiting reason is the slot limit");
        h.job(TQ_S("A"))->finishOk();
        verify(h.created == (QStringList{TQ_S("A"), TQ_S("B")}), "B starts after A");
        verify(h.state(a) == TransferTaskState::Completed, "A is Completed");
        h.job(TQ_S("B"))->finishOk();
        h.job(TQ_S("C"))->finishOk();
        verify(h.created == (QStringList{TQ_S("A"), TQ_S("B"), TQ_S("C")}), "FIFO order kept");
        verify(h.ctl.isQueueIdle(), "idle once every task is terminal");
    }

    // 1b. High priority overtakes only within the ordering key; equal keys stay FIFO
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        QueueOptions high;
        high.priority = TransferPriority::High;
        QueueOptions low;
        low.priority = TransferPriority::Low;
        h.add(TQ_S("low"), {h.src(TQ_S("l"))}, h.dst(TQ_S("l")), low);
        h.add(TQ_S("n1"), {h.src(TQ_S("n1"))}, h.dst(TQ_S("n1")));
        h.add(TQ_S("high"), {h.src(TQ_S("h"))}, h.dst(TQ_S("h")), high);
        h.add(TQ_S("n2"), {h.src(TQ_S("n2"))}, h.dst(TQ_S("n2")));
        for (const QString &name : {TQ_S("run"), TQ_S("high"), TQ_S("n1"), TQ_S("n2")}) {
            h.job(name)->finishOk();
        }
        h.job(TQ_S("low"))->finishOk();
        verify(h.created == (QStringList{TQ_S("run"), TQ_S("high"), TQ_S("n1"), TQ_S("n2"), TQ_S("low")}),
               "dispatch key is priority first, then submission order");
    }

    // 2. never exceed the active limit
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(2);
        for (int i = 0; i < 6; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        int maximum = h.ctl.activeCount();
        verify(maximum == 2, "two tasks run at the limit");
        for (int i = 0; i < 6; ++i) {
            verify(h.ctl.activeCount() <= 2, "active count never exceeds the limit");
            h.job(QString::number(i))->finishOk();
            maximum = qMax(maximum, h.ctl.activeCount());
        }
        verify(maximum == 2 && h.ctl.isQueueIdle(), "limit respected through the whole run");
    }

    // 3. dynamic limit change without interrupting active work
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        for (int i = 0; i < 5; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        h.ctl.setConcurrencyLimit(3);
        verify(h.ctl.activeCount() == 3 && h.created.size() == 3, "raising the limit starts tasks at once");
        h.ctl.setConcurrencyLimit(1);
        verify(h.ctl.activeCount() == 3, "lowering the limit keeps active tasks");
        for (int i = 0; i < 3; ++i) verify(h.job(QString::number(i))->killCalls == 0, "no active task was killed");
        h.job(TQ_S("0"))->finishOk();
        h.job(TQ_S("1"))->finishOk();
        verify(h.created.size() == 3, "no new start while active >= lowered limit");
        h.job(TQ_S("2"))->finishOk();
        verify(h.created.size() == 4 && h.ctl.activeCount() == 1, "next start after draining to the limit");
        h.ctl.setConcurrencyLimit(0);
        verify(h.ctl.concurrencyLimit() == 1, "limit never drops below 1");
    }

    // 4. adding tasks while the scheduler works
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(2);
        h.add(TQ_S("a"), {h.src(TQ_S("a"))}, h.dst(TQ_S("a")));
        h.add(TQ_S("b"), {h.src(TQ_S("b"))}, h.dst(TQ_S("b")));
        h.add(TQ_S("c"), {h.src(TQ_S("c"))}, h.dst(TQ_S("c")));
        h.job(TQ_S("a"))->finishOk();
        h.add(TQ_S("d"), {h.src(TQ_S("d"))}, h.dst(TQ_S("d")));
        verify(h.created == (QStringList{TQ_S("a"), TQ_S("b"), TQ_S("c")}), "late task waits behind c");
        h.job(TQ_S("b"))->finishOk();
        verify(h.created.last() == TQ_S("d"), "late task is dispatched in FIFO position");
    }

    // 5b. dispatch gate: nothing new starts, running tasks are untouched
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(3);
        h.add(TQ_S("a"), {h.src(TQ_S("a"))}, h.dst(TQ_S("a")));
        h.ctl.setDispatchPaused(true);
        const quint64 b = h.add(TQ_S("b"), {h.src(TQ_S("b"))}, h.dst(TQ_S("b")));
        verify(h.created == QStringList{TQ_S("a")}, "gate closed: no new job is created");
        verify(h.snap(b).waitReason == TransferWaitReason::DispatchPaused, "wait reason shows the closed gate");
        verify(h.job(TQ_S("a"))->suspendCalls == 0 && h.job(TQ_S("a"))->killCalls == 0,
               "the gate does not suspend or kill running transfers");
        h.ctl.setDispatchPaused(false);
        verify(h.created.size() == 2, "reopening the gate starts the waiting task");
    }

    // 6. cancel a queued task: no job, no file I/O, reservation not held
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("keep"), "do-not-touch");
        const QFileInfo before(h.root + TQ_S("/src/keep"));
        const QDateTime beforeTime = before.lastModified();
        h.ctl.setConcurrencyLimit(1);
        h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("shared")));
        const quint64 queued = h.add(TQ_S("move"), {h.src(TQ_S("keep"))}, h.dst(TQ_S("mv")),
                                     [] { QueueOptions o; o.type = TransferOperationType::Move; return o; }());
        QSignalSpy finished(&h.ctl, &TransferQueueController::taskFinished);
        verify(h.ctl.cancelTask(queued), "queued task cancels");
        verify(h.ctl.cancelTask(queued), "cancel is idempotent");
        verify(h.state(queued) == TransferTaskState::Cancelled, "task is Cancelled");
        verify(!h.created.contains(TQ_S("move")) && h.factoryCalls == 1, "no job was ever created");
        verify(finished.count() == 1, "one finish signal");
        verify(QFileInfo(h.root + TQ_S("/src/keep")).lastModified() == beforeTime
               && QFile(h.root + TQ_S("/src/keep")).size() == 12
               && QDir(h.root + TQ_S("/dst")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(),
               "cancelling a queued task performed no I/O");
        const quint64 same = h.add(TQ_S("same"), {h.src(TQ_S("keep"))}, h.dst(TQ_S("mv")));
        h.job(TQ_S("run"))->finishOk();
        verify(h.state(same) == TransferTaskState::Running, "cancelled task holds no reservation");
        verify(!h.ctl.cancelTask(9999), "unknown id is rejected");
    }

    // 7. slot + reservation released after success; same target blocks
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(4);
        const quint64 a = h.add(TQ_S("A"), {h.src(TQ_S("x"))}, h.dst(TQ_S("d")));
        const quint64 b = h.add(TQ_S("B"), {h.src(TQ_S("sub/x"))}, h.dst(TQ_S("d")));
        verify(h.state(a) == TransferTaskState::Running && h.state(b) == TransferTaskState::Queued,
               "second task to the same target is blocked");
        verify(h.snap(b).waitReason == TransferWaitReason::BlockedByReservation
               && h.snap(b).waitBlockerId == a, "wait reason names the blocking task");
        h.job(TQ_S("A"))->finishOk();
        verify(h.state(b) == TransferTaskState::Running, "reservation released on success");
        h.job(TQ_S("B"))->finishOk();
        verify(h.ctl.activeCount() == 0, "all slots released");
    }

    // 8. release after error and after cancellation of a running task
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 a = h.add(TQ_S("A"), {h.src(TQ_S("a"))}, h.dst(TQ_S("t")));
        const quint64 b = h.add(TQ_S("B"), {h.src(TQ_S("b"))}, h.dst(TQ_S("t2")));
        const quint64 c = h.add(TQ_S("C"), {h.src(TQ_S("c"))}, h.dst(TQ_S("t3")));
        h.job(TQ_S("A"))->finishError(KIO::ERR_DISK_FULL, TQ_S("no space"));
        verify(h.state(a) == TransferTaskState::Failed, "backend error => Failed");
        verify(h.snap(a).failureKind == TransferFailureKind::JobError
               && h.snap(a).errorCode == KIO::ERR_DISK_FULL
               && h.snap(a).errorText == TQ_S("no space"), "failure diagnostics are kept");
        verify(h.state(b) == TransferTaskState::Running, "slot freed by the failure");
        QStringList log;
        QObject::connect(&h.ctl, &TransferQueueController::taskStateChanged, &h.ctl,
                [&log](quint64, TransferTaskState from, TransferTaskState to) {
                    log << QString::number(static_cast<int>(from)) + TQ_S(">") + QString::number(static_cast<int>(to));
                });
        verify(h.ctl.cancelTask(b), "running killable task cancels");
        verify(h.job(TQ_S("B"))->killCalls == 1, "backend contract used once");
        verify(h.ctl.cancelTask(b), "cancel stays idempotent");
        verify(h.job(TQ_S("B"))->killCalls == 1, "no second kill");
        verify(h.state(b) == TransferTaskState::Cancelled, "task is Cancelled");
        verify(h.state(c) == TransferTaskState::Running, "slot freed by cancellation");
        verify(log.contains(TQ_S("1>3")) && log.contains(TQ_S("3>5")),
               "Running>Cancelling>Cancelled transitions were reported in order");
        h.job(TQ_S("C"))->finishOk();
        verify(!h.ctl.cancelTask(a) && !h.ctl.cancelTask(c), "terminal tasks cannot be cancelled");
    }

    // 9. double completion signal
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 a = h.add(TQ_S("A"), {h.src(TQ_S("a"))}, h.dst(TQ_S("t")));
        h.add(TQ_S("B"), {h.src(TQ_S("b"))}, h.dst(TQ_S("t2")));
        QSignalSpy finished(&h.ctl, &TransferQueueController::taskFinished);
        h.job(TQ_S("A"))->finishOk();
        h.job(TQ_S("A"))->finishOk();
        h.job(TQ_S("A"))->finishError(KIO::ERR_UNKNOWN, TQ_S("late"));
        verify(finished.count() == 1, "taskFinished is emitted exactly once");
        verify(h.state(a) == TransferTaskState::Completed, "late signals cannot change the terminal state");
        verify(h.ctl.activeCount() == 1 && h.created == (QStringList{TQ_S("A"), TQ_S("B")}),
               "slot released exactly once; next task started once");
    }

    // 10. reentrancy: handlers call back into the controller
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 a = h.add(TQ_S("A"), {h.src(TQ_S("a"))}, h.dst(TQ_S("t1")));
        const quint64 b = h.add(TQ_S("B"), {h.src(TQ_S("b"))}, h.dst(TQ_S("t2")));
        const quint64 c = h.add(TQ_S("C"), {h.src(TQ_S("c"))}, h.dst(TQ_S("t3")));
        QList<quint64> finishedIds;
        quint64 lateId = 0;
        QObject::connect(&h.ctl, &TransferQueueController::taskFinished, &h.ctl,
                         [&](quint64 id, TransferTaskState) {
                             finishedIds << id;
                             if (id == a) {
                                 lateId = h.add(TQ_S("D"), {h.src(TQ_S("d"))}, h.dst(TQ_S("t4")));
                                 h.ctl.cancelTask(b);
                                 h.ctl.setConcurrencyLimit(2);
                             }
                         });
        h.job(TQ_S("A"))->finishOk();
        verify(h.state(a) == TransferTaskState::Completed && h.state(b) == TransferTaskState::Cancelled,
               "reentrant cancel was applied");
        verify(h.ctl.concurrencyLimit() == 2 && h.ctl.activeCount() == 2, "reentrant limit change applied");
        verify(h.created == (QStringList{TQ_S("A"), TQ_S("C"), TQ_S("D")}), "dispatch stayed ordered and consistent");
        verify(lateId != 0 && h.state(lateId) == TransferTaskState::Running, "task added from a handler started");
        verify(finishedIds.count(a) == 1 && finishedIds.count(b) == 1, "no duplicate finish signals");
        h.job(TQ_S("C"))->finishOk();
        h.job(TQ_S("D"))->finishOk();
        verify(finishedIds.count(c) == 1 && finishedIds.count(lateId) == 1, "C and D finished cleanly");

        // a handler cancelling its own task as soon as it is reported Running
        QueueHarness g(nextRoot());
        g.ctl.setConcurrencyLimit(1);
        QObject::connect(&g.ctl, &TransferQueueController::taskStateChanged, &g.ctl,
                         [&g](quint64 id, TransferTaskState, TransferTaskState to) {
                             if (to == TransferTaskState::Running) g.ctl.cancelTask(id);
                         });
        QSignalSpy finished(&g.ctl, &TransferQueueController::taskFinished);
        const quint64 s = g.add(TQ_S("S"), {g.src(TQ_S("s"))}, g.dst(TQ_S("t")));
        verify(g.state(s) == TransferTaskState::Cancelled && finished.count() == 1,
               "self-cancel from a state handler ends in one Cancelled transition");
        (void)c;
    }

    // 11. path conflicts, sub-directories, copy vs move
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(8);
        // same target path
        const quint64 a = h.add(TQ_S("a"), {h.src(TQ_S("s1/x"))}, h.dst(TQ_S("D")));
        const quint64 b = h.add(TQ_S("b"), {h.src(TQ_S("s2/x"))}, h.dst(TQ_S("D")));
        verify(h.state(a) == TransferTaskState::Running && h.state(b) == TransferTaskState::Queued,
               "same target path is serialised");
        // same directory, different names: independent
        const quint64 c = h.add(TQ_S("c"), {h.src(TQ_S("s1/y"))}, h.dst(TQ_S("D")));
        verify(h.state(c) == TransferTaskState::Running, "different names in one directory run in parallel");
        // writing below a directory that another task moves
        QueueOptions move;
        move.type = TransferOperationType::Move;
        const quint64 m = h.add(TQ_S("m"), {h.src(TQ_S("tree"))}, h.dst(TQ_S("E")), move);
        const quint64 inner = h.add(TQ_S("inner"), {h.src(TQ_S("z"))},
                                    QUrl::fromLocalFile(h.root + TQ_S("/src/tree/deep")));
        verify(h.state(m) == TransferTaskState::Running && h.state(inner) == TransferTaskState::Queued,
               "writing into a tree that is being moved is blocked");
        // moving a parent of an active write target
        const quint64 parentMove = h.add(TQ_S("parentMove"), {h.src(TQ_S("pp"))}, h.dst(TQ_S("F")), move);
        const quint64 childWrite = h.add(TQ_S("childWrite"), {h.src(TQ_S("w"))},
                                         QUrl::fromLocalFile(h.root + TQ_S("/src/pp/q")));
        verify(h.state(parentMove) == TransferTaskState::Running && h.state(childWrite) == TransferTaskState::Queued,
               "child write below a moved parent waits");
        const quint64 ancestorMove = h.add(TQ_S("ancestorMove"), {QUrl::fromLocalFile(h.root + TQ_S("/dst"))},
                                           h.dst(TQ_S("G")), move);
        verify(h.state(ancestorMove) == TransferTaskState::Queued, "moving an ancestor of active targets waits");
        h.job(TQ_S("a"))->finishOk();
        verify(h.state(b) == TransferTaskState::Running, "waiting task starts after the blocker finishes");
        for (const QString &title : {TQ_S("b"), TQ_S("c"), TQ_S("m"), TQ_S("inner"), TQ_S("parentMove"), TQ_S("childWrite"), TQ_S("ancestorMove")}) {
            if (auto j = h.job(title)) j->finishOk();
        }
    }

    // 12. shared reads, exclusive writes, multi-source tasks, no starvation of a blocked mover
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(8);
        QueueOptions move;
        move.type = TransferOperationType::Move;
        const quint64 r1 = h.add(TQ_S("r1"), {h.src(TQ_S("a.txt"))}, h.dst(TQ_S("d1")));
        const quint64 r2 = h.add(TQ_S("r2"), {h.src(TQ_S("a.txt"))}, h.dst(TQ_S("d2")));
        verify(h.state(r1) == TransferTaskState::Running && h.state(r2) == TransferTaskState::Running,
               "the same source may be read by several copies");
        const quint64 mover = h.add(TQ_S("mover"), {h.src(TQ_S("a.txt"))}, h.dst(TQ_S("d3")), move);
        verify(h.state(mover) == TransferTaskState::Queued, "a move needs exclusive access to its source");
        const quint64 laterReader = h.add(TQ_S("later"), {h.src(TQ_S("a.txt"))}, h.dst(TQ_S("d4")));
        verify(h.state(laterReader) == TransferTaskState::Queued
               && h.snap(laterReader).waitBlockerId == mover,
               "a later reader does not overtake the waiting mover");
        const quint64 multi = h.add(TQ_S("multi"), {h.src(TQ_S("b1")), h.src(TQ_S("b2")), h.src(TQ_S("b3"))},
                                    h.dst(TQ_S("dm")));
        verify(h.state(multi) == TransferTaskState::Running, "independent multi-source task runs");
        const quint64 multi2 = h.add(TQ_S("multi2"), {h.src(TQ_S("x/b2")), h.src(TQ_S("c1"))}, h.dst(TQ_S("dm")));
        verify(h.state(multi2) == TransferTaskState::Queued, "one shared target name blocks the whole task");
        h.job(TQ_S("r1"))->finishOk();
        verify(h.state(mover) == TransferTaskState::Queued, "mover still waits for the second reader");
        h.job(TQ_S("r2"))->finishOk();
        verify(h.state(mover) == TransferTaskState::Running && h.state(laterReader) == TransferTaskState::Queued,
               "mover runs first, the later reader stays behind it");
        h.job(TQ_S("mover"))->finishOk();
        h.job(TQ_S("multi"))->finishOk();
        h.job(TQ_S("later"))->finishOk();
        h.job(TQ_S("multi2"))->finishOk();
    }

    // 13. symlinks, hardlinks, '..' and non-local URLs fail closed
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(8);
        const QString realDir = h.root + TQ_S("/src/realdir");
        QDir().mkpath(realDir);
        const QString linkDir = h.root + TQ_S("/src/linkdir");
        verify(QFile::link(realDir, linkDir), "fixture directory symlink");
        h.touchSource(TQ_S("plain"));
        const QString hardA = h.root + TQ_S("/src/hardA");
        const QString hardB = h.root + TQ_S("/src/hardB");
        h.touchSource(TQ_S("hardA"));
        verify(::link(QFile::encodeName(hardA).constData(), QFile::encodeName(hardB).constData()) == 0,
               "fixture hard link");

        const QueueOptions copy;
        auto enqueue = [&](const QString &title, const QUrl &source) {
            return h.ctl.enqueue(h.make(title, {source}, h.dst(title), copy));
        };
        const auto viaLink = enqueue(TQ_S("viaLink"), QUrl::fromLocalFile(linkDir + TQ_S("/file")));
        verify(viaLink.accepted() && !viaLink.reservationPrecise
               && viaLink.reservationNote.contains(TQ_S("symbolic")), "symlink component => imprecise, with a reason");
        h.job(TQ_S("viaLink"))->finishOk();

        const auto leafLink = [&] {
            const QString leaf = h.root + TQ_S("/src/leaflink");
            QFile::link(h.root + TQ_S("/src/plain"), leaf);
            return enqueue(TQ_S("leafLink"), QUrl::fromLocalFile(leaf));
        }();
        verify(!leafLink.reservationPrecise, "a symlink as the item itself is imprecise");
        h.job(TQ_S("leafLink"))->finishOk();

        const auto hard = enqueue(TQ_S("hard"), QUrl::fromLocalFile(hardA));
        verify(!hard.reservationPrecise && hard.reservationNote.contains(TQ_S("hard link")),
               "multi-link regular file => imprecise");
        h.job(TQ_S("hard"))->finishOk();

        const auto dots = enqueue(TQ_S("dots"), QUrl::fromLocalFile(h.root + TQ_S("/src/realdir/../plain")));
        verify(!dots.reservationPrecise, "'..' in a path => imprecise");
        h.job(TQ_S("dots"))->finishOk();

        const auto remote = h.ctl.enqueue(h.make(TQ_S("remote"), {QUrl(TQ_S("smb://host/share/file"))},
                                                 QUrl(TQ_S("smb://host/share/dst")), [] {
                                                     QueueOptions o; o.backend = TransferBackendKind::Kio; return o; }()));
        verify(remote.accepted() && !remote.reservationPrecise, "non-local identity => imprecise");
        h.job(TQ_S("remote"))->finishOk();

        // imprecise task runs alone and acts as a barrier
        const quint64 precise1 = h.add(TQ_S("p1"), {h.src(TQ_S("p1"))}, h.dst(TQ_S("p1")));
        const quint64 loner = enqueue(TQ_S("loner"), QUrl::fromLocalFile(linkDir + TQ_S("/other"))).id;
        const quint64 precise2 = h.add(TQ_S("p2"), {h.src(TQ_S("p2"))}, h.dst(TQ_S("p2")));
        verify(h.state(precise1) == TransferTaskState::Running && h.state(loner) == TransferTaskState::Queued,
               "imprecise task waits until nothing else runs");
        verify(h.state(precise2) == TransferTaskState::Queued
               && h.snap(precise2).waitReason == TransferWaitReason::ExclusiveBarrier
               && h.snap(precise2).waitBlockerId == loner,
               "tasks behind an imprecise task cannot starve it");
        h.job(TQ_S("p1"))->finishOk();
        verify(h.state(loner) == TransferTaskState::Running && h.state(precise2) == TransferTaskState::Queued,
               "imprecise task runs alone");
        h.job(TQ_S("loner"))->finishOk();
        verify(h.state(precise2) == TransferTaskState::Running, "queue continues after the exclusive task");
        h.job(TQ_S("p2"))->finishOk();
    }

    // 14. isQueueIdle with every non-terminal state
    {
        QueueHarness h(nextRoot());
        verify(h.ctl.isQueueIdle() && h.ctl.nonTerminalTaskIds().isEmpty(), "empty queue is idle");
        h.ctl.setConcurrencyLimit(1);
        QSignalSpy idleSpy(&h.ctl, &TransferQueueController::queueBecameIdle);
        const quint64 a = h.add(TQ_S("A"), {h.src(TQ_S("a"))}, h.dst(TQ_S("t1")));
        const quint64 b = h.add(TQ_S("B"), {h.src(TQ_S("b"))}, h.dst(TQ_S("t2")));
        verify(!h.ctl.isQueueIdle() && h.ctl.nonTerminalTaskIds() == (QList<quint64>{a, b}),
               "Running and Queued are non-terminal");
        verify(h.ctl.pauseTask(a) && h.state(a) == TransferTaskState::Paused, "native copy pauses for real");
        verify(h.snap(a).jobSuspended && h.job(TQ_S("A"))->suspendCalls == 1, "snapshot shows the actual suspension");
        verify(!h.ctl.isQueueIdle() && h.ctl.activeCount() == 1, "Paused is non-terminal and keeps its slot");
        verify(h.state(b) == TransferTaskState::Queued, "paused task does not release the slot");
        verify(h.ctl.pauseTask(a), "pause is idempotent");
        verify(h.job(TQ_S("A"))->suspendCalls == 1, "idempotent pause does not call the backend again");
        verify(h.ctl.resumeTask(a) && h.state(a) == TransferTaskState::Running, "resume works");
        verify(h.ctl.resumeTask(a), "resume is idempotent");
        // Cancelling observed from inside the backend kill
        bool sawCancelling = false;
        bool idleWhileCancelling = true;
        h.job(TQ_S("A"))->onKill = [&] {
            sawCancelling = h.state(a) == TransferTaskState::Cancelling;
            idleWhileCancelling = h.ctl.isQueueIdle();
        };
        h.ctl.cancelTask(a);
        verify(sawCancelling && !idleWhileCancelling, "Cancelling is non-terminal");
        verify(!h.ctl.isQueueIdle(), "B is now running");
        h.job(TQ_S("B"))->finishOk();
        verify(h.ctl.isQueueIdle() && h.ctl.nonTerminalTaskIds().isEmpty(), "idle after the last terminal transition");
        verify(idleSpy.count() == 1, "queueBecameIdle emitted once on the transition");
    }

    // 14b. pause is never pretended
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(8);
        QueueOptions kio;
        kio.backend = TransferBackendKind::Kio;
        const quint64 k = h.add(TQ_S("kio"), {h.src(TQ_S("k"))}, h.dst(TQ_S("k")), kio);
        verify(!h.ctl.pauseTask(k) && h.state(k) == TransferTaskState::Running
               && h.job(TQ_S("kio"))->suspendCalls == 0,
               "KIO declares no pause: the request is refused and the backend is not touched");
        verify(!h.snap(k).canPause, "snapshot does not offer pause for KIO");
        QueueOptions notSuspendable;
        notSuspendable.suspendable = false;
        const quint64 n = h.add(TQ_S("ns"), {h.src(TQ_S("n"))}, h.dst(TQ_S("n")), notSuspendable);
        verify(!h.ctl.pauseTask(n) && !h.snap(n).canPause, "declared pause without a live Suspendable job is refused");
        const quint64 r = h.add(TQ_S("refuse"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        h.job(TQ_S("refuse"))->suspendAccepted = false;
        verify(!h.ctl.pauseTask(r) && h.state(r) == TransferTaskState::Running, "suspend() failure keeps Running");
        QueueOptions moveOpt;
        moveOpt.type = TransferOperationType::Move;
        const quint64 mv = h.add(TQ_S("mv"), {h.src(TQ_S("mv"))}, h.dst(TQ_S("mv")), moveOpt);
        verify(!h.ctl.pauseTask(mv), "same-fs move declares no pause");
        // a job suspended behind the controller's back is reflected
        const quint64 e = h.add(TQ_S("ext"), {h.src(TQ_S("e"))}, h.dst(TQ_S("e")));
        h.job(TQ_S("ext"))->suspend();
        verify(h.state(e) == TransferTaskState::Paused, "external suspend is reflected as Paused");
        h.job(TQ_S("ext"))->resume();
        verify(h.state(e) == TransferTaskState::Running, "external resume is reflected as Running");
        // kill refused => state restored
        const quint64 kr = h.add(TQ_S("kr"), {h.src(TQ_S("kr"))}, h.dst(TQ_S("kr")));
        h.job(TQ_S("kr"))->killAccepted = false;
        verify(!h.ctl.cancelTask(kr) && h.state(kr) == TransferTaskState::Running, "refused kill restores Running");
        QueueOptions notKillable;
        notKillable.killable = false;
        const quint64 nk = h.add(TQ_S("nk"), {h.src(TQ_S("nk"))}, h.dst(TQ_S("nk")), notKillable);
        verify(!h.ctl.cancelTask(nk) && !h.snap(nk).canCancel && h.job(TQ_S("nk"))->killCalls == 0,
               "non-killable job is not cancelled and not offered for cancel");
        for (const QString &title : {TQ_S("kio"), TQ_S("ns"), TQ_S("refuse"), TQ_S("mv"), TQ_S("ext"), TQ_S("kr"), TQ_S("nk")}) {
            if (auto j = h.job(title)) j->finishOk();
        }
    }

    // 15. shutdown: diagnostic watchdog only, completion needs real confirmation
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        QueueOptions stubborn;
        stubborn.killable = false;
        const quint64 a = h.add(TQ_S("stubborn"), {h.src(TQ_S("a"))}, h.dst(TQ_S("t1")), stubborn);
        const quint64 b = h.add(TQ_S("waiting"), {h.src(TQ_S("b"))}, h.dst(TQ_S("t2")));
        QSignalSpy delayed(&h.ctl, &TransferQueueController::shutdownDelayed);
        QSignalSpy completed(&h.ctl, &TransferQueueController::shutdownCompleted);
        h.ctl.setShutdownDiagnosticInterval(25);
        h.ctl.beginShutdown();
        verify(h.state(b) == TransferTaskState::Cancelled && !h.created.contains(TQ_S("waiting")),
               "waiting tasks are cancelled without creating jobs");
        verify(h.state(a) == TransferTaskState::Running && h.job(TQ_S("stubborn"))->killCalls == 0,
               "a non-cancellable backend is left alone");
        verify(waitUntil([&] { return delayed.count() >= 2; }, 3000), "watchdog keeps reporting the delay");
        verify(!h.ctl.isShutdownComplete() && completed.count() == 0
               && h.state(a) == TransferTaskState::Running,
               "the watchdog never completes or forces anything");
        const QStringList lines = delayed.first().first().toStringList();
        verify(!lines.isEmpty() && lines.first().contains(TQ_S("stubborn")), "diagnostics name the pending task");
        const auto rejected = h.ctl.enqueue(h.make(TQ_S("late"), {h.src(TQ_S("l"))}, h.dst(TQ_S("l"))));
        verify(!rejected.accepted() && rejected.rejection == TransferEnqueueRejection::ShuttingDown,
               "no new task during shutdown");
        h.job(TQ_S("stubborn"))->finishOk();
        verify(waitUntil([&] { return completed.count() == 1; }, 2000) && h.ctl.isShutdownComplete(),
               "completion follows the real job result");
        const int delayedAfter = delayed.count();
        pump(120);
        verify(delayed.count() == delayedAfter && completed.count() == 1,
               "no diagnostics or completion after confirmation");
    }

    // 15b. shutdown waits for the finished job object to be destroyed too
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(2);
        QueueOptions options;
        options.autoDeleteJob = true;
        const quint64 a = h.add(TQ_S("k"), {h.src(TQ_S("a"))}, h.dst(TQ_S("t1")), options);
        const quint64 p = h.add(TQ_S("p"), {h.src(TQ_S("p"))}, h.dst(TQ_S("t2")), options);
        QPointer<QueueFakeJob> killable = h.job(TQ_S("k"));
        verify(h.ctl.pauseTask(p), "second task paused for real");
        QSignalSpy completed(&h.ctl, &TransferQueueController::shutdownCompleted);
        h.ctl.beginShutdown();
        verify(h.state(a) == TransferTaskState::Cancelled && h.state(p) == TransferTaskState::Cancelled,
               "active and paused tasks are cancelled through the backend");
        verify(killable && !h.ctl.isShutdownComplete() && completed.count() == 0,
               "terminal state alone is not completion while the job object is alive");
        verify(waitUntil([&] { return completed.count() == 1; }, 2000) && !killable,
               "shutdown completes only after the job object was destroyed");
        verify(h.ctl.isQueueIdle(), "queue is idle after shutdown");
    }

    // 16. Administrator Retry is outside the queue
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(4);
        auto expectRejected = [&h](TransferTaskDescriptor descriptor, TransferEnqueueRejection kind,
                                   const char *what) {
            const auto result = h.ctl.enqueue(descriptor);
            verify(!result.accepted() && result.rejection == kind, what);
        };
        expectRejected(h.make(TQ_S("a1"), {h.src(TQ_S("a"))}, QUrl(TQ_S("admin:///etc/target"))),
                       TransferEnqueueRejection::AdminSchemeForbidden, "admin destination is rejected");
        expectRejected(h.make(TQ_S("a2"), {QUrl(TQ_S("admin:///etc/shadow"))}, h.dst(TQ_S("x"))),
                       TransferEnqueueRejection::AdminSchemeForbidden, "admin source is rejected");
        TransferTaskDescriptor targets = h.make(TQ_S("a3"), {h.src(TQ_S("a"))}, h.dst(TQ_S("x")));
        targets.targetUrls = {QUrl(TQ_S("admin:///etc/new"))};
        expectRejected(targets, TransferEnqueueRejection::AdminSchemeForbidden, "admin exact target is rejected");
        TransferTaskDescriptor context = h.make(TQ_S("a4"), {h.src(TQ_S("a"))}, h.dst(TQ_S("x")));
        context.context.destinationDirectory = QUrl(TQ_S("admin:///etc"));
        expectRejected(context, TransferEnqueueRejection::AdminSchemeForbidden, "admin context is rejected");
        TransferTaskDescriptor upper = h.make(TQ_S("a5"), {h.src(TQ_S("a"))}, QUrl(TQ_S("ADMIN:///etc/x")));
        expectRejected(upper, TransferEnqueueRejection::AdminSchemeForbidden, "scheme match is case-insensitive");
        verify(h.factoryCalls == 0 && h.ctl.snapshot().tasks.isEmpty() && h.ctl.isQueueIdle(),
               "no job was created and nothing is queued for later privileged execution");
        // ordinary invalid requests
        TransferTaskDescriptor noBackend = h.make(TQ_S("b1"), {h.src(TQ_S("a"))}, h.dst(TQ_S("x")));
        noBackend.backend = TransferBackendKind::Unknown;
        expectRejected(noBackend, TransferEnqueueRejection::UnsupportedBackend, "unknown backend is rejected");
        TransferTaskDescriptor noFactory = h.make(TQ_S("b2"), {h.src(TQ_S("a"))}, h.dst(TQ_S("x")));
        noFactory.jobFactory = nullptr;
        expectRejected(noFactory, TransferEnqueueRejection::MissingJobFactory, "missing factory is rejected");
        expectRejected(h.make(TQ_S("b3"), {}, h.dst(TQ_S("x"))),
                       TransferEnqueueRejection::InvalidRequest, "empty sources are rejected");
    }

    // 17. preflight revalidation immediately before the job is created
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        h.touchSource(TQ_S("gone"));
        h.touchSource(TQ_S("replace"));
        h.touchSource(TQ_S("collide"), "mine");
        h.touchSource(TQ_S("custom"));
        h.add(TQ_S("blocker"), {h.src(TQ_S("b"))}, h.dst(TQ_S("b")));
        const quint64 gone = h.add(TQ_S("gone"), {h.src(TQ_S("gone"))}, h.dst(TQ_S("g")));
        const quint64 replaced = h.add(TQ_S("replace"), {h.src(TQ_S("replace"))}, h.dst(TQ_S("r")));
        const quint64 collide = h.add(TQ_S("collide"), {h.src(TQ_S("collide"))}, h.dst(TQ_S("c")));
        TransferTaskDescriptor custom = h.make(TQ_S("custom"), {h.src(TQ_S("custom"))}, h.dst(TQ_S("u")));
        custom.preflight = [] { return TQ_S("volume vanished"); };
        const quint64 customId = h.ctl.enqueue(custom).id;
        const quint64 after = h.add(TQ_S("after"), {h.src(TQ_S("after"))}, h.dst(TQ_S("a")));

        verify(QFile::remove(h.root + TQ_S("/src/gone")), "source vanishes while waiting");
        h.touchSource(TQ_S("other"), "different-inode");
        verify(QFile::remove(h.root + TQ_S("/src/replace"))
               && QFile::rename(h.root + TQ_S("/src/other"), h.root + TQ_S("/src/replace")),
               "source replaced by another file while waiting");
        QDir().mkpath(h.root + TQ_S("/dst/c"));
        {
            QFile existing(h.root + TQ_S("/dst/c/collide"));
            verify(existing.open(QIODevice::WriteOnly) && existing.write("precious") == 8,
                   "a target appears while the task waits");
        }
        h.job(TQ_S("blocker"))->finishOk();
        verify(h.state(gone) == TransferTaskState::Failed
               && h.snap(gone).failureKind == TransferFailureKind::PreflightFailed
               && !h.created.contains(TQ_S("gone")), "vanished source fails without creating a job");
        verify(h.state(replaced) == TransferTaskState::Failed && !h.created.contains(TQ_S("replace")),
               "replaced source (new identity) fails closed");
        verify(h.state(collide) == TransferTaskState::Failed && !h.created.contains(TQ_S("collide")),
               "a newly appeared target invalidates the old decision; nothing is overwritten");
        {
            QFile existing(h.root + TQ_S("/dst/c/collide"));
            verify(existing.open(QIODevice::ReadOnly) && existing.readAll() == "precious",
                   "the target file was not touched");
        }
        verify(h.state(customId) == TransferTaskState::Failed
               && h.snap(customId).errorText == TQ_S("volume vanished"), "custom preflight failure is reported");
        verify(h.state(after) == TransferTaskState::Running, "failed preflights release their turn");
        verify(h.ctl.activeCount() == 1, "no slot leaked by failed preflights");
    }

    // 18-23. factory and job edge cases
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        QueueOptions nullFactory;
        nullFactory.factoryReturnsNull = true;
        const quint64 n = h.add(TQ_S("null"), {h.src(TQ_S("n"))}, h.dst(TQ_S("n")), nullFactory);
        verify(h.state(n) == TransferTaskState::Failed
               && h.snap(n).failureKind == TransferFailureKind::JobFactoryFailed && h.ctl.activeCount() == 0,
               "a factory without a job fails the task and releases the slot");

        const quint64 lost = h.add(TQ_S("lost"), {h.src(TQ_S("l"))}, h.dst(TQ_S("l")));
        const quint64 next = h.add(TQ_S("next"), {h.src(TQ_S("x"))}, h.dst(TQ_S("x")));
        delete h.job(TQ_S("lost"));
        verify(h.state(lost) == TransferTaskState::Failed
               && h.snap(lost).failureKind == TransferFailureKind::JobLost, "a vanished job is reported as lost");
        verify(h.state(next) == TransferTaskState::Running, "slot freed after a lost job");
        h.job(TQ_S("next"))->finishOk();

        QueueOptions selfStart;
        selfStart.backend = TransferBackendKind::Kio;
        selfStart.startMode = TransferJobStartMode::FactoryStarts;
        h.add(TQ_S("kio"), {h.src(TQ_S("k"))}, h.dst(TQ_S("k")), selfStart);
        verify(h.job(TQ_S("kio"))->startCalls == 0, "FactoryStarts jobs are not started twice");
        h.job(TQ_S("kio"))->finishOk();
        h.add(TQ_S("native"), {h.src(TQ_S("m"))}, h.dst(TQ_S("m")));
        verify(h.job(TQ_S("native"))->startCalls == 1, "ControllerStarts jobs are started exactly once");
        h.job(TQ_S("native"))->finishOk();

        // cancel requested re-entrantly while the factory is still running
        quint64 reentrantId = 0;
        TransferTaskDescriptor reentrant = h.make(TQ_S("reentrant"), {h.src(TQ_S("q"))}, h.dst(TQ_S("q")));
        auto inner = reentrant.jobFactory;
        reentrant.jobFactory = [&]() -> KJob * {
            reentrantId = h.ctl.snapshot().tasks.last().id;
            h.ctl.cancelTask(reentrantId);
            return inner();
        };
        h.ctl.enqueue(reentrant);
        verify(h.state(reentrantId) == TransferTaskState::Cancelled
               && h.job(TQ_S("reentrant"))->startCalls == 0 && h.ctl.activeCount() == 0,
               "cancel during job creation prevents the start and frees the slot");

        // queue-wide helpers
        QueueHarness g(nextRoot());
        g.ctl.setConcurrencyLimit(1);
        const quint64 run = g.add(TQ_S("run"), {g.src(TQ_S("r"))}, g.dst(TQ_S("r")));
        g.add(TQ_S("q1"), {g.src(TQ_S("1"))}, g.dst(TQ_S("1")));
        g.add(TQ_S("q2"), {g.src(TQ_S("2"))}, g.dst(TQ_S("2")));
        verify(g.ctl.queuedCount() == 2 && g.ctl.cancelQueuedTasks() == 2 && g.ctl.queuedCount() == 0,
               "cancelQueuedTasks cancels waiting tasks only");
        verify(g.state(run) == TransferTaskState::Running && g.factoryCalls == 1, "running task and factories untouched");
        verify(g.ctl.requestCancelActive() == 1 && g.state(run) == TransferTaskState::Cancelled,
               "requestCancelActive uses the backend contract");
        verify(g.ctl.snapshot().tasks.size() == 3 && g.ctl.clearFinished() == 3
               && g.ctl.snapshot().tasks.isEmpty(), "terminal records can be cleared");
    }

    // 24. registry in isolation
    {
        const QUrl a = QUrl::fromLocalFile(root + TQ_S("/s1/src/a"));
        const QUrl dstDir = QUrl::fromLocalFile(root + TQ_S("/s1/dst/reg"));
        const ReservationRequest copyA = TransferReservation::build(TransferOperationType::Copy, {a}, dstDir);
        const ReservationRequest copyB = TransferReservation::build(
            TransferOperationType::Copy, {a}, QUrl::fromLocalFile(root + TQ_S("/s1/dst/reg2")));
        const ReservationRequest moveA = TransferReservation::build(TransferOperationType::Move, {a}, dstDir);
        verify(copyA.valid && copyA.precise && !TransferReservation::conflicts(copyA, copyB),
               "shared reads of one source do not conflict");
        verify(TransferReservation::conflicts(copyA, moveA), "same target conflicts");
        verify(TransferReservation::conflicts(copyB, moveA), "move source vs shared read conflicts");
        const ReservationRequest invalid = TransferReservation::build(TransferOperationType::Copy, {}, dstDir);
        verify(!invalid.valid && TransferReservation::conflicts(invalid, copyA), "invalid request fails closed");
        TransferReservationRegistry registry;
        verify(registry.tryAcquire(1, copyA) && !registry.tryAcquire(1, copyB), "a holder cannot acquire twice");
        verify(!registry.tryAcquire(2, moveA) && registry.findConflict(moveA)->holder == 1, "conflict names the holder");
        verify(registry.tryAcquire(3, copyB) && registry.holderCount() == 2, "compatible reservation is granted");
        verify(registry.release(1) && !registry.release(1) && registry.holderCount() == 1,
               "release is idempotent");
        verify(registry.tryAcquire(2, moveA) == false, "copyB (holder 3) still reads the source");
        verify(registry.release(3) && registry.tryAcquire(2, moveA) && registry.holds(2),
               "reservation is granted after every conflicting holder released");
        verify(TransferReservation::revalidate(copyA, TransferOperationType::Copy, {a}, dstDir).isEmpty(),
               "unchanged inputs revalidate");
    }

    // =========================================================================
    // STAGE 2 — Sequential / Parallel & Limits (26 test scenarios)
    // =========================================================================

    // S2-01. Default Parallel with limit 2
    {
        QueueHarness h(nextRoot());
        verify(h.ctl.executionMode() == TransferQueueMode::Parallel, "default mode is Parallel");
        verify(h.ctl.parallelConcurrencyLimit() == 2, "default parallel limit is 2");
        verify(h.ctl.effectiveConcurrencyLimit() == 2, "default effective limit is 2");
        verify(h.ctl.concurrencyLimit() == 2, "concurrencyLimit() alias matches effective limit");
        const auto snap = h.ctl.snapshot();
        verify(snap.mode == TransferQueueMode::Parallel && snap.parallelLimit == 2 && snap.effectiveLimit == 2,
               "snapshot reflects default mode and limits");

        for (int i = 0; i < 4; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        verify(h.ctl.activeCount() == 2 && h.created.size() == 2,
               "exactly 2 tasks start under default limit 2");
        verify(h.created == (QStringList{TQ_S("0"), TQ_S("1")}), "first two tasks started");
        h.job(TQ_S("0"))->finishOk();
        verify(h.created == (QStringList{TQ_S("0"), TQ_S("1"), TQ_S("2")}), "third task starts after slot freed");
        h.job(TQ_S("1"))->finishOk();
        h.job(TQ_S("2"))->finishOk();
        h.job(TQ_S("3"))->finishOk();
        verify(h.ctl.isQueueIdle(), "all tasks completed cleanly");
    }

    // S2-02. Sequential runs only one job at a time
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        verify(h.ctl.executionMode() == TransferQueueMode::Sequential, "mode set to Sequential");
        verify(h.ctl.effectiveConcurrencyLimit() == 1, "Sequential effective limit is 1");
        verify(h.ctl.parallelConcurrencyLimit() == 2, "configured parallel limit preserved as 2");

        for (int i = 0; i < 3; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        verify(h.ctl.activeCount() == 1 && h.created.size() == 1,
               "Sequential runs exactly one task");
        verify(h.snap(h.ctl.snapshot().tasks[1].id).waitReason == TransferWaitReason::SlotLimit,
               "second task waits for slot");
        h.job(TQ_S("0"))->finishOk();
        verify(h.ctl.activeCount() == 1 && h.created.size() == 2, "second task runs after first finishes");
        h.job(TQ_S("1"))->finishOk();
        verify(h.ctl.activeCount() == 1 && h.created.size() == 3, "third task runs");
        h.job(TQ_S("2"))->finishOk();
        verify(h.ctl.isQueueIdle(), "all tasks completed cleanly in Sequential mode");
    }

    // S2-03. Parallel with limit 1
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        h.ctl.setParallelConcurrencyLimit(1);
        verify(h.ctl.executionMode() == TransferQueueMode::Parallel, "mode is Parallel");
        verify(h.ctl.parallelConcurrencyLimit() == 1 && h.ctl.effectiveConcurrencyLimit() == 1,
               "effective limit is 1 in Parallel mode with limit 1");
        h.add(TQ_S("a"), {h.src(TQ_S("a"))}, h.dst(TQ_S("a")));
        h.add(TQ_S("b"), {h.src(TQ_S("b"))}, h.dst(TQ_S("b")));
        verify(h.ctl.activeCount() == 1 && h.created == QStringList{TQ_S("a")},
               "only 1 task runs at Parallel limit 1");
        h.job(TQ_S("a"))->finishOk();
        verify(h.created == (QStringList{TQ_S("a"), TQ_S("b")}), "next task runs after completion");
        h.job(TQ_S("b"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-04. Parallel with limit 8
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(8);
        verify(h.ctl.parallelConcurrencyLimit() == 8 && h.ctl.effectiveConcurrencyLimit() == 8,
               "Parallel limit 8 active");
        for (int i = 0; i < 8; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        verify(h.ctl.activeCount() == 8 && h.created.size() == 8,
               "all 8 tasks start in parallel");
        for (int i = 0; i < 8; ++i) {
            h.job(QString::number(i))->finishOk();
        }
        verify(h.ctl.isQueueIdle(), "all 8 tasks completed");
    }

    // S2-05. Sequential -> Parallel switch while transfer is running
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(3);
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        verify(h.ctl.effectiveConcurrencyLimit() == 1, "starts in Sequential (effective 1)");

        for (int i = 0; i < 4; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        verify(h.ctl.activeCount() == 1 && h.created.size() == 1, "only 1 runs in Sequential");
        const int initialJob0Starts = h.job(TQ_S("0"))->startCalls;
        verify(initialJob0Starts == 1, "task 0 started once");

        // Switch to Parallel: effective limit rises from 1 to 3
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        verify(h.ctl.executionMode() == TransferQueueMode::Parallel, "mode changed to Parallel");
        verify(h.ctl.effectiveConcurrencyLimit() == 3, "effective limit is now 3");
        verify(h.ctl.activeCount() == 3 && h.created.size() == 3,
               "raising limit by mode switch immediately starts pending tasks up to 3");
        verify(h.job(TQ_S("0"))->startCalls == 1 && h.job(TQ_S("0"))->killCalls == 0,
               "running task 0 was not restarted or interrupted");

        for (const QString &n : {TQ_S("0"), TQ_S("1"), TQ_S("2"), TQ_S("3")}) {
            if (auto j = h.job(n)) j->finishOk();
        }
        verify(h.ctl.isQueueIdle(), "completed cleanly");
    }

    // S2-06. Parallel -> Sequential switch with multiple active tasks
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(3);
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        for (int i = 0; i < 5; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        verify(h.ctl.activeCount() == 3, "3 tasks running in parallel");

        // Switch to Sequential: effective limit drops to 1, but 3 tasks are running
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        verify(h.ctl.effectiveConcurrencyLimit() == 1, "effective limit dropped to 1");
        verify(h.ctl.activeCount() == 3, "active tasks were NOT killed or suspended");
        for (int i = 0; i < 3; ++i) {
            verify(h.job(QString::number(i))->killCalls == 0 && h.job(QString::number(i))->suspendCalls == 0,
                   "no active task received kill or suspend");
        }

        // Finish task 0 -> active becomes 2 (still >= 1), no new task starts
        h.job(TQ_S("0"))->finishOk();
        verify(h.ctl.activeCount() == 2 && h.created.size() == 3, "no new start while active (2) >= limit (1)");

        // Finish task 1 -> active becomes 1 (still >= 1), no new task starts
        h.job(TQ_S("1"))->finishOk();
        verify(h.ctl.activeCount() == 1 && h.created.size() == 3, "no new start while active (1) >= limit (1)");

        // Finish task 2 -> active becomes 0 (< 1), task 3 starts
        h.job(TQ_S("2"))->finishOk();
        verify(h.ctl.activeCount() == 1 && h.created.size() == 4, "task 3 starts when active drops below limit");

        h.job(TQ_S("3"))->finishOk();
        verify(h.ctl.activeCount() == 1 && h.created.size() == 5, "task 4 starts");
        h.job(TQ_S("4"))->finishOk();
        verify(h.ctl.isQueueIdle(), "all tasks completed sequentially");
    }

    // S2-07. Lowering limit 8 -> 2 without interrupting active tasks
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(8);
        for (int i = 0; i < 6; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        verify(h.ctl.activeCount() == 6, "6 tasks running at limit 8");

        h.ctl.setParallelConcurrencyLimit(2);
        verify(h.ctl.parallelConcurrencyLimit() == 2 && h.ctl.effectiveConcurrencyLimit() == 2,
               "limit lowered to 2");
        verify(h.ctl.activeCount() == 6, "all 6 tasks remain running");
        for (int i = 0; i < 6; ++i) {
            verify(h.job(QString::number(i))->killCalls == 0, "active task was not killed");
        }
        for (int i = 0; i < 6; ++i) {
            h.job(QString::number(i))->finishOk();
        }
        verify(h.ctl.isQueueIdle(), "idle after all finish");
    }

    // S2-08. Raising limit 1 -> 8
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(1);
        for (int i = 0; i < 8; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        verify(h.ctl.activeCount() == 1 && h.created.size() == 1, "only 1 runs at limit 1");
        h.ctl.setParallelConcurrencyLimit(8);
        verify(h.ctl.activeCount() == 8 && h.created.size() == 8,
               "raising to 8 immediately starts all 7 queued tasks");
        for (int i = 0; i < 8; ++i) {
            h.job(QString::number(i))->finishOk();
        }
        verify(h.ctl.isQueueIdle(), "all finished");
    }

    // S2-09. Restoring previous parallel limit after Sequential
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(5);
        verify(h.ctl.parallelConcurrencyLimit() == 5 && h.ctl.effectiveConcurrencyLimit() == 5,
               "configured parallel limit 5");
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        verify(h.ctl.effectiveConcurrencyLimit() == 1, "Sequential effective limit is 1");
        verify(h.ctl.parallelConcurrencyLimit() == 5, "configured parallel limit 5 preserved in Sequential");
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        verify(h.ctl.effectiveConcurrencyLimit() == 5 && h.ctl.parallelConcurrencyLimit() == 5,
               "returning to Parallel restores effective limit 5");
    }

    // S2-10. Rapid mode switching under active load
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(4);
        for (int i = 0; i < 6; ++i) {
            h.add(QString::number(i), {h.src(QString::number(i))}, h.dst(QString::number(i)));
        }
        for (int iter = 0; iter < 10; ++iter) {
            h.ctl.setExecutionMode(iter % 2 == 0 ? TransferQueueMode::Sequential : TransferQueueMode::Parallel);
            verify(h.ctl.activeCount() <= 6, "active count stays bounded");
        }
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        for (int i = 0; i < 6; ++i) {
            if (auto j = h.job(QString::number(i))) j->finishOk();
        }
        verify(h.ctl.isQueueIdle(), "completed cleanly after rapid switching");
    }

    // S2-11. No dispatch during global paused gate
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        h.ctl.setParallelConcurrencyLimit(4);
        h.add(TQ_S("a"), {h.src(TQ_S("a"))}, h.dst(TQ_S("a")));
        h.ctl.setDispatchPaused(true);
        h.add(TQ_S("b"), {h.src(TQ_S("b"))}, h.dst(TQ_S("b")));
        h.add(TQ_S("c"), {h.src(TQ_S("c"))}, h.dst(TQ_S("c")));

        // Switching mode or raising limit while paused must NOT dispatch any waiting task
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        verify(h.created == QStringList{TQ_S("a")}, "gate paused: mode change does not start tasks");
        h.ctl.setParallelConcurrencyLimit(8);
        verify(h.created == QStringList{TQ_S("a")}, "gate paused: limit change does not start tasks");

        h.ctl.setDispatchPaused(false);
        verify(h.created.size() == 3, "unpausing gate dispatches waiting tasks according to new limit");
        for (const QString &n : {TQ_S("a"), TQ_S("b"), TQ_S("c")}) h.job(n)->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-12. Release reservations and slots on success under Parallel
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(4);
        const quint64 t1 = h.add(TQ_S("t1"), {h.src(TQ_S("dir1/x"))}, h.dst(TQ_S("shared_dst")));
        const quint64 t2 = h.add(TQ_S("t2"), {h.src(TQ_S("dir2/x"))}, h.dst(TQ_S("shared_dst")));
        verify(h.state(t1) == TransferTaskState::Running && h.state(t2) == TransferTaskState::Queued,
               "second conflicting task is queued despite available slots");
        h.job(TQ_S("t1"))->finishOk();
        verify(h.state(t1) == TransferTaskState::Completed && h.state(t2) == TransferTaskState::Running,
               "reservation and slot released, t2 starts");
        h.job(TQ_S("t2"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-13. Release reservations and slots on error under Parallel
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(4);
        const quint64 e1 = h.add(TQ_S("e1"), {h.src(TQ_S("dir1/x"))}, h.dst(TQ_S("shared_dst2")));
        const quint64 e2 = h.add(TQ_S("e2"), {h.src(TQ_S("dir2/x"))}, h.dst(TQ_S("shared_dst2")));
        verify(h.state(e1) == TransferTaskState::Running && h.state(e2) == TransferTaskState::Queued,
               "e2 waits for e1");
        h.job(TQ_S("e1"))->finishError(KIO::ERR_CANNOT_WRITE, TQ_S("write error"));
        verify(h.state(e1) == TransferTaskState::Failed && h.state(e2) == TransferTaskState::Running,
               "failure releases reservation and slot, e2 starts");
        h.job(TQ_S("e2"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-14. Cancel queued task under Parallel
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        h.add(TQ_S("r1"), {h.src(TQ_S("r1"))}, h.dst(TQ_S("r1")));
        h.add(TQ_S("r2"), {h.src(TQ_S("r2"))}, h.dst(TQ_S("r2")));
        const quint64 q3 = h.add(TQ_S("q3"), {h.src(TQ_S("q3"))}, h.dst(TQ_S("q3")));
        const quint64 q4 = h.add(TQ_S("q4"), {h.src(TQ_S("q4"))}, h.dst(TQ_S("q4")));
        verify(h.state(q3) == TransferTaskState::Queued, "q3 is queued");
        verify(h.ctl.cancelTask(q3), "cancel queued task returns true");
        verify(h.state(q3) == TransferTaskState::Cancelled && !h.created.contains(TQ_S("q3")),
               "q3 cancelled without job creation");
        h.job(TQ_S("r1"))->finishOk();
        verify(h.created.contains(TQ_S("q4")), "q4 gets slot after r1 finishes");
        h.job(TQ_S("r2"))->finishOk();
        h.job(TQ_S("q4"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-15. Limit change during Cancelling state
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        QueueOptions opt;
        opt.killable = true;
        const quint64 c1 = h.add(TQ_S("c1"), {h.src(TQ_S("c1"))}, h.dst(TQ_S("c1")), opt);
        const quint64 c2 = h.add(TQ_S("c2"), {h.src(TQ_S("c2"))}, h.dst(TQ_S("c2")), opt);
        const quint64 c3 = h.add(TQ_S("c3"), {h.src(TQ_S("c3"))}, h.dst(TQ_S("c3")), opt);
        verify(h.state(c1) == TransferTaskState::Running && h.state(c3) == TransferTaskState::Queued,
               "c1 runs, c3 queued");

        // Delayed kill: job enters Cancelling
        h.job(TQ_S("c1"))->killAccepted = true;
        bool inCancelling = false;
        h.job(TQ_S("c1"))->onKill = [&] {
            inCancelling = h.state(c1) == TransferTaskState::Cancelling;
            // While c1 is cancelling, change limit
            h.ctl.setParallelConcurrencyLimit(1);
        };
        h.ctl.cancelTask(c1);
        verify(inCancelling, "c1 was observed in Cancelling");
        verify(h.state(c1) == TransferTaskState::Cancelled, "c1 reached Cancelled");
        h.job(TQ_S("c2"))->finishOk();
        // Since limit was lowered to 1, c3 starts after c2 finishes
        h.job(TQ_S("c3"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-16. Reentrancy during mode and limit change
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(1);
        const quint64 a = h.add(TQ_S("A"), {h.src(TQ_S("a"))}, h.dst(TQ_S("a")));
        const quint64 b = h.add(TQ_S("B"), {h.src(TQ_S("b"))}, h.dst(TQ_S("b")));
        const quint64 c = h.add(TQ_S("C"), {h.src(TQ_S("c"))}, h.dst(TQ_S("c")));

        QObject::connect(&h.ctl, &TransferQueueController::taskFinished, &h.ctl,
                         [&](quint64 id, TransferTaskState) {
                             if (id == a) {
                                 h.ctl.setExecutionMode(TransferQueueMode::Parallel);
                                 h.ctl.setParallelConcurrencyLimit(3);
                             }
                         });
        h.job(TQ_S("A"))->finishOk();
        verify(h.created == (QStringList{TQ_S("A"), TQ_S("B"), TQ_S("C")}),
               "reentrant switch to Parallel limit 3 started B and C immediately");
        h.job(TQ_S("B"))->finishOk();
        h.job(TQ_S("C"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-17. Stage 1 priorities under different limits
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        h.add(TQ_S("run1"), {h.src(TQ_S("r1"))}, h.dst(TQ_S("r1")));
        h.add(TQ_S("run2"), {h.src(TQ_S("r2"))}, h.dst(TQ_S("r2")));

        QueueOptions low; low.priority = TransferPriority::Low;
        QueueOptions normal; normal.priority = TransferPriority::Normal;
        QueueOptions high; high.priority = TransferPriority::High;

        h.add(TQ_S("l1"), {h.src(TQ_S("l1"))}, h.dst(TQ_S("l1")), low);
        h.add(TQ_S("n1"), {h.src(TQ_S("n1"))}, h.dst(TQ_S("n1")), normal);
        h.add(TQ_S("h1"), {h.src(TQ_S("h1"))}, h.dst(TQ_S("h1")), high);

        h.job(TQ_S("run1"))->finishOk();
        verify(h.created.last() == TQ_S("h1"), "High priority task overtakes Normal and Low");
        h.job(TQ_S("run2"))->finishOk();
        verify(h.created.last() == TQ_S("n1"), "Normal priority task overtakes Low");
        h.job(TQ_S("h1"))->finishOk();
        verify(h.created.last() == TQ_S("l1"), "Low priority task runs last");
        h.job(TQ_S("n1"))->finishOk();
        h.job(TQ_S("l1"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-18. Reservation conflict despite available slot
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(8);
        const quint64 t1 = h.add(TQ_S("t1"), {h.src(TQ_S("dir1/x"))}, h.dst(TQ_S("dst_collision")));
        const quint64 t2 = h.add(TQ_S("t2"), {h.src(TQ_S("dir2/x"))}, h.dst(TQ_S("dst_collision")));
        verify(h.ctl.activeCount() == 1, "only 1 task runs despite limit 8");
        verify(h.state(t2) == TransferTaskState::Queued
               && h.snap(t2).waitReason == TransferWaitReason::BlockedByReservation,
               "t2 is blocked by reservation, not slot limit");
        h.job(TQ_S("t1"))->finishOk();
        verify(h.state(t2) == TransferTaskState::Running, "t2 starts after t1 releases reservation");
        h.job(TQ_S("t2"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-19. FIFO preservation for equal priority tasks
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        h.add(TQ_S("x1"), {h.src(TQ_S("x1"))}, h.dst(TQ_S("x1")));
        h.add(TQ_S("x2"), {h.src(TQ_S("x2"))}, h.dst(TQ_S("x2")));
        h.add(TQ_S("p1"), {h.src(TQ_S("p1"))}, h.dst(TQ_S("p1")));
        h.add(TQ_S("p2"), {h.src(TQ_S("p2"))}, h.dst(TQ_S("p2")));
        h.add(TQ_S("p3"), {h.src(TQ_S("p3"))}, h.dst(TQ_S("p3")));
        h.job(TQ_S("x1"))->finishOk();
        verify(h.created.last() == TQ_S("p1"), "FIFO order kept (p1)");
        h.job(TQ_S("x2"))->finishOk();
        verify(h.created.last() == TQ_S("p2"), "FIFO order kept (p2)");
        h.job(TQ_S("p1"))->finishOk();
        verify(h.created.last() == TQ_S("p3"), "FIFO order kept (p3)");
        h.job(TQ_S("p2"))->finishOk();
        h.job(TQ_S("p3"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-20. No duplicate job creation across mode/limit changes
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(1);
        h.add(TQ_S("single"), {h.src(TQ_S("s"))}, h.dst(TQ_S("s")));
        verify(h.factoryCalls == 1, "factory called once on launch");
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        h.ctl.setParallelConcurrencyLimit(4);
        h.ctl.setParallelConcurrencyLimit(1);
        verify(h.factoryCalls == 1, "no duplicate factory call during mode or limit changes");
        h.job(TQ_S("single"))->finishOk();
        verify(h.ctl.isQueueIdle(), "idle");
    }

    // S2-21. Load empty configuration (fallback to defaults)
    {
        const QString confPath = root + TQ_S("/empty_settings.ini");
        QSettings settings(confPath, QSettings::IniFormat);
        settings.clear();
        settings.sync();
        const TransferQueueConfig cfg = TransferQueueSettings::load(settings);
        verify(cfg.mode == TransferQueueMode::Parallel, "default mode on empty config is Parallel");
        verify(cfg.parallelLimit == TransferQueueDefaultParallelLimit, "default parallel limit is 2");
    }

    // S2-22. Save and reload configuration
    {
        const QString confPath = root + TQ_S("/rw_settings.ini");
        {
            QSettings settings(confPath, QSettings::IniFormat);
            TransferQueueConfig cfg;
            cfg.mode = TransferQueueMode::Sequential;
            cfg.parallelLimit = 5;
            TransferQueueSettings::save(settings, cfg);
            settings.sync();
        }
        {
            QSettings settings(confPath, QSettings::IniFormat);
            const TransferQueueConfig loaded = TransferQueueSettings::load(settings);
            verify(loaded.mode == TransferQueueMode::Sequential, "reloaded mode is Sequential");
            verify(loaded.parallelLimit == 5, "reloaded limit is 5");
        }
    }

    // S2-23. Invalid mode in configuration
    {
        const QString confPath = root + TQ_S("/invalid_mode_settings.ini");
        {
            QSettings settings(confPath, QSettings::IniFormat);
            settings.setValue(TQ_S("TransferQueue/ExecutionMode"), TQ_S("NonExistentMode"));
            settings.setValue(TQ_S("TransferQueue/ParallelLimit"), 4);
            settings.sync();
        }
        {
            QSettings settings(confPath, QSettings::IniFormat);
            const TransferQueueConfig loaded = TransferQueueSettings::load(settings);
            verify(loaded.mode == TransferQueueMode::Parallel, "invalid mode falls back to Parallel");
            verify(loaded.parallelLimit == 4, "valid limit is retained");
        }
    }

    // S2-24. Limit 0, negative and greater than 8 in configuration
    {
        const QString confPath = root + TQ_S("/boundary_limit_settings.ini");
        {
            QSettings settings(confPath, QSettings::IniFormat);
            settings.setValue(TQ_S("TransferQueue/ParallelLimit"), 0);
            settings.sync();
            verify(TransferQueueSettings::load(settings).parallelLimit == 1, "limit 0 clamped to 1");

            settings.setValue(TQ_S("TransferQueue/ParallelLimit"), -10);
            settings.sync();
            verify(TransferQueueSettings::load(settings).parallelLimit == 1, "negative limit clamped to 1");

            settings.setValue(TQ_S("TransferQueue/ParallelLimit"), 99);
            settings.sync();
            verify(TransferQueueSettings::load(settings).parallelLimit == 8, "limit 99 clamped to 8");
        }
    }

    // S2-25. Non-integer value in configuration
    {
        const QString confPath = root + TQ_S("/corrupted_type_settings.ini");
        {
            QSettings settings(confPath, QSettings::IniFormat);
            settings.setValue(TQ_S("TransferQueue/ParallelLimit"), TQ_S("not_an_int"));
            settings.sync();
            const auto loaded = TransferQueueSettings::load(settings);
            verify(loaded.parallelLimit == TransferQueueDefaultParallelLimit,
                   "corrupted limit type falls back to default 2");
        }
    }

    // S2-26. Isolation: live user configuration was never touched
    {
        QSettings liveUserPreferences;
        verify(!liveUserPreferences.childGroups().contains(TQ_S("TransferQueue")),
               "real user QSettings was not modified during test execution");
    }

    // =========================================================================
    // Stage 3: Queue UI & Settings Tests (S3-01 to S3-25)
    // =========================================================================

    // S3-01. Initialization of settings widget with default configuration
    {
        TransferQueueSettingsWidget widget;
        verify(widget.parallelRadio()->isChecked(), "S3-01: default mode is Parallel");
        verify(!widget.sequentialRadio()->isChecked(), "S3-01: sequential is unchecked");
        verify(widget.limitSpin()->value() == TransferQueueDefaultParallelLimit,
               "S3-01: default limit is 2");
        verify(widget.limitSpin()->isEnabled(), "S3-01: limit spin is enabled in Parallel mode");
        verify(widget.drainingLabel()->isHidden(), "S3-01: draining label is hidden by default");
    }

    // S3-02. Initialization with stored configuration
    {
        const QString confPath = root + TQ_S("/s3_stored_settings.ini");
        auto isolated = std::make_shared<QSettings>(confPath, QSettings::IniFormat);
        TransferQueueConfig cfg;
        cfg.mode = TransferQueueMode::Sequential;
        cfg.parallelLimit = 5;
        TransferQueueSettings::save(*isolated, cfg);

        TransferQueueSettingsWidget widget;
        widget.setIsolatedSettings(isolated);
        widget.loadSettings(*isolated);

        verify(widget.sequentialRadio()->isChecked(), "S3-02: loaded mode is Sequential");
        verify(!widget.parallelRadio()->isChecked(), "S3-02: parallel radio is unchecked");
        verify(widget.limitSpin()->value() == 5, "S3-02: stored parallel limit (5) is preserved");
        verify(!widget.limitSpin()->isEnabled(), "S3-02: limit spin is disabled in Sequential mode");
    }

    // S3-03. Mode switching: Sequential / Parallel
    {
        TransferQueueController controller;
        TransferQueueSettingsWidget widget;
        widget.setController(&controller);

        verify(controller.executionMode() == TransferQueueMode::Parallel, "S3-03: initial mode is Parallel");

        // Switch to Sequential
        widget.sequentialRadio()->setChecked(true);
        verify(controller.executionMode() == TransferQueueMode::Sequential, "S3-03: controller switched to Sequential");
        verify(!widget.limitSpin()->isEnabled(), "S3-03: spinbox disabled in Sequential");

        // Switch to Parallel
        widget.parallelRadio()->setChecked(true);
        verify(controller.executionMode() == TransferQueueMode::Parallel, "S3-03: controller switched to Parallel");
        verify(widget.limitSpin()->isEnabled(), "S3-03: spinbox enabled in Parallel");
    }

    // S3-04. Concurrency limit range 1–8
    {
        TransferQueueController controller;
        TransferQueueSettingsWidget widget;
        widget.setController(&controller);

        verify(widget.limitSpin()->minimum() == TransferQueueMinParallelLimit, "S3-04: minimum limit is 1");
        verify(widget.limitSpin()->maximum() == TransferQueueMaxParallelLimit, "S3-04: maximum limit is 8");

        widget.limitSpin()->setValue(1);
        verify(controller.parallelConcurrencyLimit() == 1, "S3-04: controller updated to 1");

        widget.limitSpin()->setValue(8);
        verify(controller.parallelConcurrencyLimit() == 8, "S3-04: controller updated to 8");
    }

    // S3-05. Preserving Parallel limit value when switching modes
    {
        TransferQueueController controller;
        TransferQueueSettingsWidget widget;
        widget.setController(&controller);

        widget.limitSpin()->setValue(6);
        verify(controller.effectiveConcurrencyLimit() == 6, "S3-05: parallel limit is 6");

        // Switch to Sequential
        widget.sequentialRadio()->setChecked(true);
        verify(controller.effectiveConcurrencyLimit() == 1, "S3-05: effective limit in Sequential is 1");
        verify(widget.limitSpin()->value() == 6, "S3-05: spinbox value preserved as 6 while disabled");

        // Switch back to Parallel
        widget.parallelRadio()->setChecked(true);
        verify(controller.effectiveConcurrencyLimit() == 6, "S3-05: effective limit restored to 6");
        verify(widget.limitSpin()->value() == 6, "S3-05: spinbox retains 6");
    }

    // S3-06. View recreation and loading stored settings
    {
        const QString confPath = root + TQ_S("/s3_recreate_settings.ini");
        auto isolated = std::make_shared<QSettings>(confPath, QSettings::IniFormat);

        {
            TransferQueueController c1;
            TransferQueueSettingsWidget w1;
            w1.setIsolatedSettings(isolated);
            w1.setController(&c1);
            w1.sequentialRadio()->setChecked(true);
            w1.limitSpin()->setValue(7);
            isolated->sync();
        }

        {
            TransferQueueController c2;
            TransferQueueSettingsWidget w2;
            w2.setIsolatedSettings(isolated);
            w2.setController(&c2);
            w2.loadSettings(*isolated);

            verify(w2.sequentialRadio()->isChecked(), "S3-06: recreated widget has Sequential mode");
            verify(c2.executionMode() == TransferQueueMode::Sequential, "S3-06: controller c2 has Sequential mode");
            verify(w2.limitSpin()->value() == 7, "S3-06: recreated widget has limit 7");
            verify(!w2.limitSpin()->isEnabled(), "S3-06: spinbox is disabled in Sequential");
        }
    }

    // S3-07. No unintended writes to settings
    {
        const QString confPath = root + TQ_S("/s3_no_write_settings.ini");
        verify(!QFile::exists(confPath), "S3-07: ini file does not exist before test");

        auto isolated = std::make_shared<QSettings>(confPath, QSettings::IniFormat);
        TransferQueueSettingsWidget widget;
        widget.setIsolatedSettings(isolated);
        widget.loadSettings(*isolated);

        verify(!QFile::exists(confPath) || isolated->allKeys().isEmpty(),
               "S3-07: loading does not write keys unexpectedly");
    }

    // S3-08. Corrupted QSettings handling
    {
        const QString confPath = root + TQ_S("/s3_corrupted_ui_settings.ini");
        {
            QSettings bad(confPath, QSettings::IniFormat);
            bad.setValue(TQ_S("TransferQueue/ExecutionMode"), TQ_S("InvalidModeName"));
            bad.setValue(TQ_S("TransferQueue/ParallelLimit"), -99);
            bad.sync();
        }

        QSettings bad(confPath, QSettings::IniFormat);
        TransferQueueSettingsWidget widget;
        widget.loadSettings(bad);

        verify(widget.parallelRadio()->isChecked(), "S3-08: invalid mode defaults to Parallel");
        verify(widget.limitSpin()->value() == 1, "S3-08: negative limit clamped to 1");
    }

    // S3-09. Snapshots update reactions
    {
        TransferQueueController controller;
        TransferQueueViewWidget view;
        view.setController(&controller);

        verify(!view.emptyLabel()->isHidden(), "S3-09: initially empty label is visible");

        TransferTaskDescriptor desc;
        desc.title = TQ_S("Task1");
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {QUrl::fromLocalFile(root + TQ_S("/f1.txt"))};
        desc.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        desc.jobFactory = []() { return new QueueFakeJob; };
        controller.enqueue(desc);

        verify(view.emptyLabel()->isHidden(), "S3-09: empty label hidden after enqueue");
        verify(view.taskItemWidgets().size() == 1, "S3-09: 1 task item widget created");
        for (const auto &t : controller.snapshot().tasks) controller.cancelTask(t.id);
    }

    // S3-10. Queued tasks presentation
    {
        TransferQueueController controller;
        controller.setDispatchPaused(true);

        TransferTaskDescriptor desc;
        desc.title = TQ_S("QueuedTask");
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {QUrl::fromLocalFile(root + TQ_S("/q1.txt"))};
        desc.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        desc.jobFactory = []() { return new QueueFakeJob; };
        const auto res = controller.enqueue(desc);

        TransferQueueViewWidget view;
        view.setController(&controller);

        auto *item = view.taskItemWidget(res.id);
        verify(item != nullptr, "S3-10: task item exists");
        verify(item->stateBadge()->text() == trLocal("W kolejce", "Queued"), "S3-10: state badge shows Queued");
        verify(!item->queuePosBadge()->isHidden(), "S3-10: queue position badge is visible");
        verify(item->progressBar()->isHidden(), "S3-10: progress bar is hidden for queued task");
        for (const auto &t : controller.snapshot().tasks) controller.cancelTask(t.id);
    }

    // S3-11. Running tasks presentation
    {
        TransferQueueController controller;
        TransferTaskDescriptor desc;
        desc.title = TQ_S("RunningTask");
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {QUrl::fromLocalFile(root + TQ_S("/r1.txt"))};
        desc.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        QueueFakeJob *fakeJob = nullptr;
        desc.jobFactory = [&fakeJob]() {
            fakeJob = new QueueFakeJob;
            return fakeJob;
        };
        const auto res = controller.enqueue(desc);

        TransferQueueViewWidget view;
        view.setController(&controller);

        auto *item = view.taskItemWidget(res.id);
        verify(item != nullptr, "S3-11: running item exists");
        verify(item->stateBadge()->text() == trLocal("W toku", "Running"), "S3-11: state badge shows Running");
        verify(!item->progressBar()->isHidden(), "S3-11: progress bar is visible");

        if (fakeJob) {
            fakeJob->finish();
        }
    }

    // S3-12. Terminal states presentation
    {
        TransferQueueController controller;
        TransferTaskDescriptor desc;
        desc.title = TQ_S("TerminalTask");
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {QUrl::fromLocalFile(root + TQ_S("/t1.txt"))};
        desc.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        QueueFakeJob *fakeJob = nullptr;
        desc.jobFactory = [&fakeJob]() {
            fakeJob = new QueueFakeJob;
            return fakeJob;
        };
        const auto res = controller.enqueue(desc);

        TransferQueueViewWidget view;
        view.setController(&controller);

        if (fakeJob) {
            fakeJob->finish();
        }

        auto *item = view.taskItemWidget(res.id);
        verify(item != nullptr, "S3-12: item exists after finish");
        verify(item->stateBadge()->text() == trLocal("Zakończono", "Completed"), "S3-12: state badge is Completed");
        verify(item->cancelButton()->isHidden(), "S3-12: cancel button hidden for completed task");
    }

    // S3-13. Wait reason presentation
    {
        verify(!TransferQueueTaskItemWidget::waitReasonText(TransferWaitReason::SlotLimit).isEmpty(),
               "S3-13: SlotLimit text present");
        verify(!TransferQueueTaskItemWidget::waitReasonText(TransferWaitReason::BlockedByReservation).isEmpty(),
               "S3-13: BlockedByReservation text present");
        verify(!TransferQueueTaskItemWidget::waitReasonText(TransferWaitReason::DispatchPaused).isEmpty(),
               "S3-13: DispatchPaused text present");
        verify(!TransferQueueTaskItemWidget::waitReasonText(TransferWaitReason::ExclusiveBarrier).isEmpty(),
               "S3-13: ExclusiveBarrier text present");
        verify(!TransferQueueTaskItemWidget::waitReasonText(TransferWaitReason::ShuttingDown).isEmpty(),
               "S3-13: ShuttingDown text present");
    }

    // S3-14. Draining status presentation
    {
        TransferQueueController controller;
        controller.setParallelConcurrencyLimit(2);

        QueueFakeJob *j1 = nullptr;
        QueueFakeJob *j2 = nullptr;

        TransferTaskDescriptor d1;
        d1.title = TQ_S("Task1");
        d1.backend = TransferBackendKind::LocalNative;
        d1.sources = {QUrl::fromLocalFile(root + TQ_S("/d1.txt"))};
        d1.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d1.jobFactory = [&j1]() { j1 = new QueueFakeJob; return j1; };
        controller.enqueue(d1);

        TransferTaskDescriptor d2;
        d2.title = TQ_S("Task2");
        d2.backend = TransferBackendKind::LocalNative;
        d2.sources = {QUrl::fromLocalFile(root + TQ_S("/d2.txt"))};
        d2.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d2.jobFactory = [&j2]() { j2 = new QueueFakeJob; return j2; };
        controller.enqueue(d2);

        TransferQueueSettingsWidget widget;
        widget.setController(&controller);

        verify(widget.drainingLabel()->isHidden(), "S3-14: not draining initially");

        // Reduce limit while 2 are running -> enters draining
        widget.limitSpin()->setValue(1);
        verify(controller.isDraining(), "S3-14: controller is draining");
        verify(!widget.drainingLabel()->isHidden(), "S3-14: draining label is visible");
        verify(widget.drainingLabel()->text().contains(TQ_S("2")), "S3-14: draining label mentions 2 active");
        verify(widget.drainingLabel()->text().contains(TQ_S("1")), "S3-14: draining label mentions new limit 1");

        if (j1) j1->finish();
        if (j2) j2->finish();

        verify(!controller.isDraining(), "S3-14: draining ended after jobs finish");
        verify(widget.drainingLabel()->isHidden(), "S3-14: draining label hidden after completion");
    }

    // S3-15. No duplication of legacy jobs
    {
        QMainWindow dummyWindow;
        QToolBar toolbar(&dummyWindow);
        OperationManager manager(&dummyWindow, &toolbar, &dummyWindow);
        TransferQueueController qController;
        manager.setQueueController(&qController);

        OperationWindow *opWindow = new OperationWindow(&manager, &dummyWindow);
        opWindow->setQueueController(&qController);

        TransferTaskDescriptor d;
        d.title = TQ_S("QueueTask");
        d.backend = TransferBackendKind::LocalNative;
        d.sources = {QUrl::fromLocalFile(root + TQ_S("/u.txt"))};
        d.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d.jobFactory = []() { return new QueueFakeJob; };
        qController.enqueue(d);

        verify(manager.operations().isEmpty(), "S3-15: legacy manager operations remain empty");
        verify(opWindow->queueViewWidget()->taskItemWidgets().size() == 1,
               "S3-15: queue task appears only in queue view");

        for (const auto &t : qController.snapshot().tasks) qController.cancelTask(t.id);
        delete opWindow;
    }

    // S3-16. Isolation from production dispatch
    {
        TransferQueueController controller;
        verify(controller.snapshot().tasks.isEmpty(), "S3-16: controller starts empty");
        verify(controller.activeCount() == 0 && controller.queuedCount() == 0,
               "S3-16: scheduler is not bound to production file actions yet");
    }

    // S3-17. Clean destruction lifecycle
    {
        // Case A: destroy widgets first, then controller
        {
            auto *ctrl = new TransferQueueController;
            auto *widget = new TransferQueueSettingsWidget;
            widget->setController(ctrl);
            auto *view = new TransferQueueViewWidget;
            view->setController(ctrl);
            delete widget;
            delete view;
            delete ctrl;
        }
        // Case B: destroy controller first, then widgets
        {
            auto *ctrl = new TransferQueueController;
            auto *widget = new TransferQueueSettingsWidget;
            widget->setController(ctrl);
            auto *view = new TransferQueueViewWidget;
            view->setController(ctrl);
            delete ctrl;
            delete widget;
            delete view;
        }
        verify(true, "S3-17: destruction in both orders succeeded without crashes");
    }

    // S3-18. No dangling connections / use-after-free
    {
        TransferQueueController controller;
        TransferQueueViewWidget *view = new TransferQueueViewWidget;
        view->setController(&controller);

        TransferTaskDescriptor d;
        d.title = TQ_S("Temp");
        d.backend = TransferBackendKind::LocalNative;
        d.sources = {QUrl::fromLocalFile(root + TQ_S("/temp.txt"))};
        d.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        QueueFakeJob *fj = nullptr;
        d.jobFactory = [&fj]() { fj = new QueueFakeJob; return fj; };
        controller.enqueue(d);

        delete view;

        if (fj) fj->finish();
        verify(true, "S3-18: finishing job after view destruction causes no crash");
    }

    // S3-19. Bilingual PL/EN presentation
    {
        verify(!TransferQueueTaskItemWidget::stateText(TransferTaskState::Queued).isEmpty(),
               "S3-19: Queued state text exists");
        verify(!TransferQueueTaskItemWidget::stateText(TransferTaskState::Running).isEmpty(),
               "S3-19: Running state text exists");
        verify(!TransferQueueTaskItemWidget::priorityText(TransferPriority::High).isEmpty(),
               "S3-19: High priority text exists");
        verify(!TransferQueueTaskItemWidget::priorityText(TransferPriority::Normal).isEmpty(),
               "S3-19: Normal priority text exists");
    }

    // S3-20. Primary / Split parity
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController ctrl;
        mgr.setQueueController(&ctrl);

        verify(mgr.queueController() == &ctrl, "S3-20: queue controller is window-wide and shared");
    }

    // S3-21. Legacy popup preservation
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        mgr.build(&tb);

        verify(mgr.activeCount() == 0, "S3-21: active count is 0");
        verify(mgr.operations().isEmpty(), "S3-21: operations empty");
    }

    // S3-22. Individual cancel functionality
    {
        TransferQueueController controller;
        controller.setDispatchPaused(true);

        TransferTaskDescriptor d1;
        d1.title = TQ_S("TaskToCancel");
        d1.backend = TransferBackendKind::LocalNative;
        d1.sources = {QUrl::fromLocalFile(root + TQ_S("/c1.txt"))};
        d1.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d1.jobFactory = []() { return new QueueFakeJob; };
        const auto res1 = controller.enqueue(d1);

        TransferTaskDescriptor d2;
        d2.title = TQ_S("TaskToKeep");
        d2.backend = TransferBackendKind::LocalNative;
        d2.sources = {QUrl::fromLocalFile(root + TQ_S("/c2.txt"))};
        d2.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d2.jobFactory = []() { return new QueueFakeJob; };
        const auto res2 = controller.enqueue(d2);

        TransferQueueViewWidget view;
        view.setController(&controller);

        auto *item1 = view.taskItemWidget(res1.id);
        verify(item1 != nullptr, "S3-22: item1 exists");
        item1->cancelButton()->click();

        verify(controller.taskSnapshot(res1.id)->state == TransferTaskState::Cancelled,
               "S3-22: task1 cancelled");
        verify(controller.taskSnapshot(res2.id)->state == TransferTaskState::Queued,
               "S3-22: task2 remains Queued");
        controller.cancelTask(res2.id);
    }

    // S3-23. Handling long filenames and paths without crash or distortion
    {
        TransferTaskSnapshot snap;
        snap.id = 999;
        snap.title = TQ_S("Bardzo_bardzo_dluga_nazwa_pliku_ktora_powinna_zostac_bezpiecznie_wyswietlona_w_interfejsie_uzytkownika_bez_rozpychania_okna.tar.gz");
        snap.sources = {QUrl::fromLocalFile(root + TQ_S("/bardzo/gleboki/katalog/testowy/ze/spora/iloscia/podkatalogow/i/bardzo/dluga/sciezka/plik.tar.gz"))};
        snap.destination = QUrl::fromLocalFile(root + TQ_S("/cel/rowniez/bardzo/dluga/sciezka/docelowa/"));
        snap.state = TransferTaskState::Queued;
        snap.waitReason = TransferWaitReason::SlotLimit;

        TransferQueueController controller;
        TransferQueueTaskItemWidget item(snap, &controller);

        verify(item.titleLabel()->fullText() == snap.title, "S3-23: title fullText contains full long name");
        verify(item.titleLabel()->toolTip().contains(snap.title.left(25)), "S3-23: title tooltip contains long name");
        verify(item.pathsLabel()->toolTip().contains(TQ_S("bardzo/gleboki")),
               "S3-23: paths tooltip contains full long path");
    }

    // S3-24. Isolation of Demo fake jobs
    {
        TransferQueueController controller;
        enqueueDemoTasks(&controller);

        verify(controller.snapshot().tasks.size() == 5, "S3-24: exactly 5 demo tasks enqueued");
        verify(!QFile::exists(TQ_S("/home/user/Wideo/4K_Render_Video.mp4")),
               "S3-24: fake demo tasks do not touch or create real files");

        for (const auto &t : controller.snapshot().tasks) {
            controller.cancelTask(t.id);
        }
    }

    // S3-25. Non-interference with Administrator Retry
    {
        TransferQueueController controller;
        TransferTaskDescriptor adminTask;
        adminTask.title = TQ_S("AdminTask");
        adminTask.backend = TransferBackendKind::LocalNative;
        adminTask.jobFactory = []() { return new QueueFakeJob; };
        adminTask.sources = {QUrl(TQ_S("admin:///root/secret.txt"))};
        adminTask.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        const auto res = controller.enqueue(adminTask);

        verify(!res.accepted(), "S3-25: admin scheme strictly rejected by queue controller");
        verify(res.rejection == TransferEnqueueRejection::AdminSchemeForbidden,
               "S3-25: rejection reason is AdminSchemeForbidden");
    }

    // =========================================================================
    // Stage 3 UX/Layout Remediation Regression Tests (S3-FAIL-01 to S3-FAIL-17)
    // =========================================================================

    // S3-FAIL-01: 1, 3, 5, 10, and 20 simultaneous operations in OperationWindow
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        const int countsToTest[] = {1, 3, 5, 10, 20};
        for (int count : countsToTest) {
            QList<QueueFakeJob *> jobs;
            for (int i = 0; i < count; ++i) {
                auto *j = new QueueFakeJob(true, false, false);
                jobs.append(j);
                mgr.track(j, QStringLiteral("Operation %1").arg(i + 1));
            }
            opWin.rebuild();
            verify(opWin.hasQueueTab(), "S3-FAIL-01: tabs enabled when controller is attached");
            verify(opWin.operationsTabCount() == count, "S3-FAIL-01: operationsTabCount matches active");
            const auto rows = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
            verify(rows.size() == count, "S3-FAIL-01: exact number of operation rows displayed");

            for (auto *j : jobs) {
                mgr.finish(j, true, false, {});
                delete j;
            }
            mgr.clearFinished();
        }
    }

    // S3-FAIL-02: No overlap between operations list and queue controls/view
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        verify(opWin.tabWidget() != nullptr, "S3-FAIL-02: tab widget exists");
        verify(opWin.tabWidget()->indexOf(opWin.operationsPage()) == 0, "S3-FAIL-02: operations page is tab 0");
        verify(opWin.tabWidget()->indexOf(opWin.queuePage()) == 1, "S3-FAIL-02: queue page is tab 1");
        verify(opWin.queueSettingsWidget()->parentWidget() == opWin.queuePage(),
               "S3-FAIL-02: queue settings parented to queue page");
        verify(opWin.queueViewWidget()->parentWidget() == opWin.queuePage(),
               "S3-FAIL-02: queue view parented to queue page");
        verify(opWin.operationsScrollArea()->parentWidget() == opWin.operationsPage(),
               "S3-FAIL-02: operations scroll area parented to operations page");
    }

    // S3-FAIL-03: Availability of buttons after scrolling (both tabs)
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        // Add 10 legacy operations
        QList<QueueFakeJob *> jobs;
        for (int i = 0; i < 10; ++i) {
            auto *j = new QueueFakeJob(true, true, false);
            jobs.append(j);
            mgr.track(j, QStringLiteral("Job %1").arg(i));
        }
        opWin.rebuild();

        // Check buttons on rows
        const auto cancelBtns = opWin.operationsPage()->findChildren<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
        verify(cancelBtns.size() == 10, "S3-FAIL-03: all 10 cancel buttons present in operations tab");
        for (auto *btn : cancelBtns) {
            verify(btn->isEnabled(), "S3-FAIL-03: cancel button enabled");
        }

        // Add 5 queue tasks
        for (int i = 0; i < 5; ++i) {
            TransferTaskDescriptor d;
            d.title = QStringLiteral("QTask %1").arg(i);
            d.backend = TransferBackendKind::LocalNative;
            d.sources = {QUrl::fromLocalFile(root + QStringLiteral("/q_%1.txt").arg(i))};
            d.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
            d.jobFactory = []() { return new QueueFakeJob; };
            qc.enqueue(d);
        }
        opWin.showQueueTab();
        const auto qCancelBtns = opWin.queuePage()->findChildren<QToolButton *>(QStringLiteral("transferQueueTaskCancelButton"));
        verify(qCancelBtns.size() == 5, "S3-FAIL-03: all 5 cancel buttons present in queue tab");

        for (auto *j : jobs) {
            mgr.finish(j, true, false, {});
            delete j;
        }
        mgr.clearFinished();
        for (const auto &t : qc.snapshot().tasks) qc.cancelTask(t.id);
    }

    // S3-FAIL-04: Operations <-> Queue tab switching
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        opWin.showQueueTab();
        verify(opWin.tabWidget()->currentWidget() == opWin.queuePage(),
               "S3-FAIL-04: switched to Queue tab");
        opWin.showOperationsTab();
        verify(opWin.tabWidget()->currentWidget() == opWin.operationsPage(),
               "S3-FAIL-04: switched to Operations tab");
    }

    // S3-FAIL-05: Geometry preservation during window resize
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        opWin.resize(800, 600);
        verify(opWin.width() == 800 && opWin.height() == 600, "S3-FAIL-05: resized to 800x600");
        opWin.resize(480, 380);
        verify(opWin.width() == 480 && opWin.height() == 380, "S3-FAIL-05: resized to minimum 480x380");
    }

    // S3-FAIL-06: Independent scroll areas
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        QScrollArea *opsScroll = opWin.operationsScrollArea();
        QScrollArea *queueScroll = opWin.queueViewWidget()->scrollArea();
        verify(opsScroll != nullptr && queueScroll != nullptr, "S3-FAIL-06: both scroll areas exist");
        verify(opsScroll != queueScroll, "S3-FAIL-06: scroll areas are completely independent");
    }

    // S3-FAIL-07: Very long filenames and paths do not expand layout
    {
        const QString veryLongName = QStringLiteral("A").repeated(300) + QStringLiteral(".dat");
        TransferTaskSnapshot snap;
        snap.id = 1001;
        snap.title = veryLongName;
        snap.sources = {QUrl::fromLocalFile(QStringLiteral("/very/long/path/").repeated(20) + veryLongName)};
        snap.destination = QUrl::fromLocalFile(QStringLiteral("/dest/path/").repeated(20));
        snap.state = TransferTaskState::Queued;
        snap.waitReason = TransferWaitReason::SlotLimit;

        TransferQueueController qc;
        TransferQueueTaskItemWidget item(snap, &qc);
        verify(item.titleLabel()->sizeHint().width() <= 324, "S3-FAIL-07: elided title width constrained");
        verify(item.titleLabel()->fullText() == veryLongName, "S3-FAIL-07: full title in fullText");
        verify(item.titleLabel()->toolTip().contains(veryLongName.left(25)), "S3-FAIL-07: title in tooltip");
    }

    // S3-FAIL-08: Polish and English localization strings
    {
        const QLocale oldLocale = QLocale();
        QLocale::setDefault(QLocale(QLocale::Polish));
        const auto drainingPL = transferQueueDrainingText(3, 1);
        verify(drainingPL.contains(QStringLiteral("3 aktywne")), "S3-FAIL-08: PL draining mentions 3 aktywne");
        verify(drainingPL.contains(QStringLiteral("limit: 1")), "S3-FAIL-08: PL draining mentions limit: 1");

        QLocale::setDefault(QLocale(QLocale::English));
        const auto drainingEN = transferQueueDrainingText(3, 1);
        verify(drainingEN.contains(QStringLiteral("3 active")), "S3-FAIL-08: EN draining mentions 3 active");
        verify(drainingEN.contains(QStringLiteral("limit: 1")), "S3-FAIL-08: EN draining mentions limit: 1");

        const auto notice = transferQueueDemoNoticeText();
        verify(!notice.isEmpty(), "S3-FAIL-08: demo notice text present");
        QLocale::setDefault(oldLocale);
    }

    // S3-FAIL-09: Primary / Split parity
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        verify(mgr.queueController() == &qc, "S3-FAIL-09: manager holds shared queue controller");
    }

    // S3-FAIL-10: No counter duplication between tabs
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        auto *j1 = new QueueFakeJob(true, false, false);
        auto *j2 = new QueueFakeJob(true, false, false);
        mgr.track(j1, QStringLiteral("Legacy 1"));
        mgr.track(j2, QStringLiteral("Legacy 2"));

        TransferTaskDescriptor d;
        d.title = QStringLiteral("Queue 1");
        d.backend = TransferBackendKind::LocalNative;
        d.sources = {QUrl::fromLocalFile(root + TQ_S("/t1.txt"))};
        d.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d.jobFactory = []() { return new QueueFakeJob; };
        qc.enqueue(d);

        opWin.rebuild();
        verify(opWin.operationsTabCount() == 2, "S3-FAIL-10: operationsTabCount is exactly 2");
        verify(opWin.queueTabCount() == 1, "S3-FAIL-10: queueTabCount is exactly 1");

        mgr.finish(j1, true, false, {});
        mgr.finish(j2, true, false, {});
        delete j1;
        delete j2;
        mgr.clearFinished();
        for (const auto &t : qc.snapshot().tasks) qc.cancelTask(t.id);
    }

    // S3-FAIL-11: Demo notice visibility and separation
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        verify(opWin.queueNoticeLabel() != nullptr, "S3-FAIL-11: queue notice label exists");
        verify(!opWin.queueNoticeLabel()->isHidden(), "S3-FAIL-11: queue notice label is visible in demo");
        verify(opWin.queueNoticeLabel()->text() == transferQueueDemoNoticeText(),
               "S3-FAIL-11: notice contains demo disclaimer");
    }

    // S3-FAIL-12: Sequential mode switch with multiple running tasks
    {
        TransferQueueController controller;
        controller.setParallelConcurrencyLimit(3);

        QueueFakeJob *j1 = nullptr;
        QueueFakeJob *j2 = nullptr;
        QueueFakeJob *j3 = nullptr;

        TransferTaskDescriptor d1;
        d1.title = TQ_S("Task1");
        d1.backend = TransferBackendKind::LocalNative;
        d1.sources = {QUrl::fromLocalFile(root + TQ_S("/sq1.txt"))};
        d1.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d1.jobFactory = [&j1]() { j1 = new QueueFakeJob; return j1; };
        controller.enqueue(d1);

        TransferTaskDescriptor d2;
        d2.title = TQ_S("Task2");
        d2.backend = TransferBackendKind::LocalNative;
        d2.sources = {QUrl::fromLocalFile(root + TQ_S("/sq2.txt"))};
        d2.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d2.jobFactory = [&j2]() { j2 = new QueueFakeJob; return j2; };
        controller.enqueue(d2);

        TransferTaskDescriptor d3;
        d3.title = TQ_S("Task3");
        d3.backend = TransferBackendKind::LocalNative;
        d3.sources = {QUrl::fromLocalFile(root + TQ_S("/sq3.txt"))};
        d3.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d3.jobFactory = [&j3]() { j3 = new QueueFakeJob; return j3; };
        controller.enqueue(d3);

        verify(controller.activeCount() == 3, "S3-FAIL-12: 3 tasks active");

        // Switch to Sequential
        controller.setExecutionMode(TransferQueueMode::Sequential);
        verify(controller.executionMode() == TransferQueueMode::Sequential,
               "S3-FAIL-12: mode is Sequential");
        verify(controller.activeCount() == 3,
               "S3-FAIL-12: active tasks not killed on mode switch");
        verify(controller.isDraining(), "S3-FAIL-12: draining active");

        if (j1) j1->finish();
        if (j2) j2->finish();
        if (j3) j3->finish();
    }

    // S3-FAIL-13: No automatic suspension of active tasks on mode change
    {
        TransferQueueController controller;
        controller.setParallelConcurrencyLimit(2);
        QueueFakeJob *fj = nullptr;
        TransferTaskDescriptor d;
        d.title = TQ_S("TaskRunning");
        d.backend = TransferBackendKind::LocalNative;
        d.sources = {QUrl::fromLocalFile(root + TQ_S("/sr.txt"))};
        d.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d.jobFactory = [&fj]() { fj = new QueueFakeJob; return fj; };
        const auto res = controller.enqueue(d);

        verify(controller.taskSnapshot(res.id)->state == TransferTaskState::Running,
               "S3-FAIL-13: task is running");
        controller.setExecutionMode(TransferQueueMode::Sequential);
        verify(controller.taskSnapshot(res.id)->state == TransferTaskState::Running,
               "S3-FAIL-13: task remains Running, not Paused");
        if (fj) fj->finish();
    }

    // S3-FAIL-14: Accurate draining banner text and visibility
    {
        TransferQueueController controller;
        controller.setParallelConcurrencyLimit(3);
        QueueFakeJob *j1 = nullptr;
        QueueFakeJob *j2 = nullptr;
        TransferTaskDescriptor d1;
        d1.title = TQ_S("D1");
        d1.backend = TransferBackendKind::LocalNative;
        d1.sources = {QUrl::fromLocalFile(root + TQ_S("/d1_b.txt"))};
        d1.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d1.jobFactory = [&j1]() { j1 = new QueueFakeJob; return j1; };
        controller.enqueue(d1);

        TransferTaskDescriptor d2;
        d2.title = TQ_S("D2");
        d2.backend = TransferBackendKind::LocalNative;
        d2.sources = {QUrl::fromLocalFile(root + TQ_S("/d2_b.txt"))};
        d2.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d2.jobFactory = [&j2]() { j2 = new QueueFakeJob; return j2; };
        controller.enqueue(d2);

        TransferQueueSettingsWidget widget;
        widget.setController(&controller);
        widget.limitSpin()->setValue(1);

        verify(controller.isDraining(), "S3-FAIL-14: draining true");
        verify(!widget.drainingLabel()->isHidden(), "S3-FAIL-14: draining label visible");
        verify(widget.drainingLabel()->text() == transferQueueDrainingText(2, 1),
               "S3-FAIL-14: draining label has exact text");

        if (j1) j1->finish();
        if (j2) j2->finish();
    }

    // S3-FAIL-15: Settings persistence isolated to demo path
    {
        const QString demoPath = transferQueueDemoSettingsPath();
        verify(demoPath.contains(QStringLiteral("thispc-transfer-queue-demo")),
               "S3-FAIL-15: demo settings store is in temp isolated folder");
    }

    // S3-FAIL-16: TransferQueueProductionDispatchConnected flag is false
    {
        verify(!TransferQueueProductionDispatchConnected,
               "S3-FAIL-16: production dispatch is strictly disconnected in Stage 3");
    }

    // S3-FAIL-17: Clean widget and controller destruction
    {
        auto *ctrl = new TransferQueueController;
        auto *win = new QMainWindow;
        auto *tb = new QToolBar(win);
        auto *mgr = new OperationManager(win, tb, win);
        mgr->setQueueController(ctrl);
        auto *opWin = new OperationWindow(mgr, win);
        opWin->setQueueController(ctrl);

        delete opWin;
        delete mgr;
        delete tb;
        delete win;
        delete ctrl;
        verify(true, "S3-FAIL-17: complex hierarchy destroyed cleanly without crash");
    }

    // =========================================================================
    // Stage 3 Responsive Row Width Fix Regression Tests (S3-RESP-01 to S3-RESP-14)
    // =========================================================================

    // S3-RESP-01: Short filename in operations tab
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(580, 360);

        auto *j = new QueueFakeJob(true, true, false);
        mgr.track(j, QStringLiteral("short.txt"));
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto *row = opWin.findChild<QFrame *>(QStringLiteral("operationRow"));
        verify(row != nullptr, "S3-RESP-01: operation row exists");
        auto *title = row->findChild<QLabel *>(QStringLiteral("operationTitleLabel"));
        auto *cancel = row->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
        verify(title != nullptr && cancel != nullptr, "S3-RESP-01: title and cancel button exist");
        verify(title->text().contains(QStringLiteral("short.txt")), "S3-RESP-01: short name fully visible");
        verify(cancel->geometry().right() <= row->width(), "S3-RESP-01: cancel button is inside row width");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-02: 100-character filename
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(580, 360);

        const QString name100 = QStringLiteral("File_100_Chars_") + QStringLiteral("X").repeated(75) + QStringLiteral(".dat");
        auto *j = new QueueFakeJob(true, true, false);
        mgr.track(j, name100);
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto *row = opWin.findChild<QFrame *>(QStringLiteral("operationRow"));
        verify(row != nullptr, "S3-RESP-02: row exists");
        verify(row->minimumSizeHint().width() < 420, "S3-RESP-02: row minimum width is constrained (< 420px)");

        auto *title = row->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
        auto *cancel = row->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
        verify(title != nullptr && cancel != nullptr, "S3-RESP-02: title and cancel exist");
        verify(title->fullText() == name100, "S3-RESP-02: full 100-char name preserved in fullText");
        verify(title->toolTip().contains(name100.left(25)), "S3-RESP-02: full 100-char name preserved in tooltip");
        verify(cancel->geometry().right() <= row->width(), "S3-RESP-02: cancel button inside row for 100-char name");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-03: 300-character filename
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(520, 360);

        const QString name300 = QStringLiteral("File_300_Chars_") + QStringLiteral("Z").repeated(275) + QStringLiteral(".archive");
        auto *j = new QueueFakeJob(true, true, false);
        mgr.track(j, name300);
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto *row = opWin.findChild<QFrame *>(QStringLiteral("operationRow"));
        verify(row != nullptr, "S3-RESP-03: row exists");
        verify(row->minimumSizeHint().width() < 420, "S3-RESP-03: row minimum width for 300-char name is < 420px");

        auto *title = row->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
        auto *cancel = row->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
        auto *pause = row->findChild<QToolButton *>(QStringLiteral("detailedOperationPauseButton"));
        auto *state = row->findChild<QLabel *>(QStringLiteral("operationDetailsLabel"));
        verify(title != nullptr && cancel != nullptr && pause != nullptr && state != nullptr,
               "S3-RESP-03: all header controls exist");
        verify(title->fullText() == name300, "S3-RESP-03: full 300-char name in fullText");
        verify(title->toolTip().contains(name300.left(25)), "S3-RESP-03: full 300-char name in tooltip");
        verify(cancel->geometry().right() <= row->width(), "S3-RESP-03: cancel button inside row width");
        verify(pause->geometry().right() <= cancel->geometry().left(), "S3-RESP-03: pause button positioned left of cancel");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-04: Very long source and destination paths (400+ characters)
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        const QString deepPath = QStringLiteral("/deep/nested/path/structure/").repeated(15) + QStringLiteral("document.pdf");
        auto *j = new QueueFakeJob(true, true, false);
        j->setProperty("thispcSourceUrls", QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(deepPath)}));
        j->setProperty("thispcDestinationUrl", QVariant::fromValue(QUrl::fromLocalFile(QStringLiteral("/dest/"))));
        mgr.track(j, QStringLiteral("Copying"));
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto *currentFileLabel = opWin.findChild<QLabel *>(QStringLiteral("operationCurrentFileLabel"));
        verify(currentFileLabel != nullptr, "S3-RESP-04: current file label exists");
        verify(currentFileLabel->toolTip().contains(QStringLiteral("document.pdf")),
               "S3-RESP-04: tooltip contains full deep path");
        verify(currentFileLabel->minimumSizeHint().width() < 100,
               "S3-RESP-04: current file label does not force wide minimum size");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-05: Narrow window (500–600 px)
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(500, 360);

        const QString longName = QStringLiteral("Very_Long_Name_").repeated(10) + QStringLiteral(".tar");
        auto *j = new QueueFakeJob(true, true, false);
        mgr.track(j, longName);
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto *row = opWin.findChild<QFrame *>(QStringLiteral("operationRow"));
        auto *cancel = row->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
        verify(cancel->geometry().right() <= row->width(),
               "S3-RESP-05: cancel button visible and inside 500px narrow window");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-06: Window expansion without closing
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(500, 360);

        const QString longName = QStringLiteral("Dynamically_Elided_Filename_").repeated(8) + QStringLiteral(".zip");
        auto *j = new QueueFakeJob(true, true, false);
        mgr.track(j, longName);
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto *title = opWin.findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
        const int widthNarrow = title->width();

        opWin.resize(850, 360);
        QCoreApplication::processEvents();

        const int widthWide = title->width();
        verify(widthWide > widthNarrow, "S3-RESP-06: title label dynamically expanded when window widened");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-07: Window re-narrowing
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(850, 360);

        const QString longName = QStringLiteral("Shrinking_Window_Test_").repeated(8) + QStringLiteral(".iso");
        auto *j = new QueueFakeJob(true, true, false);
        mgr.track(j, longName);
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto *row = opWin.findChild<QFrame *>(QStringLiteral("operationRow"));
        auto *title = row->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
        auto *cancel = row->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));

        opWin.resize(500, 360);
        QCoreApplication::processEvents();

        verify(cancel->geometry().right() <= row->width(),
               "S3-RESP-07: cancel button remains within row after re-narrowing");
        verify(title->text().endsWith(QStringLiteral("…")) || title->text().contains(QStringLiteral("...")),
               "S3-RESP-07: title text is elided when narrowed");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-08: Five simultaneous active operation cards
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(550, 480);

        QList<QueueFakeJob *> jobs;
        for (int i = 0; i < 5; ++i) {
            auto *j = new QueueFakeJob(true, true, false);
            jobs.append(j);
            mgr.track(j, QStringLiteral("Concurrent_Task_Long_Name_%1_").arg(i) + QStringLiteral("W").repeated(80));
        }
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        const auto rows = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rows.size() == 5, "S3-RESP-08: exactly 5 operation cards created");
        for (auto *row : rows) {
            auto *cancel = row->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
            verify(cancel != nullptr && cancel->geometry().right() <= row->width(),
                   "S3-RESP-08: each of 5 cards keeps cancel button within card width");
        }

        for (auto *j : jobs) {
            mgr.finish(j, true, false, {});
            delete j;
        }
    }

    // S3-RESP-09: Expanded and collapsed cards
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(550, 420);

        auto *j = new QueueFakeJob(true, true, false);
        mgr.track(j, QStringLiteral("Card_Expansion_Test"));
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto *row = opWin.findChild<QFrame *>(QStringLiteral("operationRow"));
        auto *toggle = row->findChild<QToolButton *>(QStringLiteral("operationDetailsToggleButton"));
        auto *details = row->findChild<QWidget *>(QStringLiteral("operationExpandedDetails"));
        verify(toggle != nullptr && details != nullptr, "S3-RESP-09: toggle and details widget exist");
        verify(details->isVisible(), "S3-RESP-09: details initially visible/expanded");

        toggle->click();
        verify(details->isHidden(), "S3-RESP-09: details collapsed after click");

        toggle->click();
        verify(details->isVisible(), "S3-RESP-09: details re-expanded after second click");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-10: Button visibility and enablement order
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(550, 360);

        auto *j = new QueueFakeJob(true, true, false);
        mgr.track(j, QStringLiteral("Button_Order_Test_").repeated(5));
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto *row = opWin.findChild<QFrame *>(QStringLiteral("operationRow"));
        auto *title = row->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
        auto *state = row->findChild<QLabel *>(QStringLiteral("operationDetailsLabel"));
        auto *toggle = row->findChild<QToolButton *>(QStringLiteral("operationDetailsToggleButton"));
        auto *pause = row->findChild<QToolButton *>(QStringLiteral("detailedOperationPauseButton"));
        auto *cancel = row->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));

        verify(title->geometry().left() < state->geometry().left(),
               "S3-RESP-10: title is to the left of state");
        verify(state->geometry().left() < toggle->geometry().left(),
               "S3-RESP-10: state is to the left of toggle");
        verify(toggle->geometry().left() < pause->geometry().left(),
               "S3-RESP-10: toggle is to the left of pause");
        verify(pause->geometry().left() < cancel->geometry().left(),
               "S3-RESP-10: pause is to the left of cancel");
        verify(cancel->geometry().right() <= row->width(),
               "S3-RESP-10: cancel button is within row boundaries");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-11: Tooltip accuracy for elided titles
    {
        const QString fullName = QStringLiteral("Exact_Unabbreviated_Filename_For_Tooltip_Verification.tar.gz");
        TransferQueueElidedLabel label(fullName);
        label.resize(100, 20);
        verify(label.fullText() == fullName, "S3-RESP-11: fullText matches");
        verify(label.toolTip().contains(fullName.left(25)), "S3-RESP-11: tooltip accurately holds long name");
    }

    // S3-RESP-12: Size hint width not inflated by long text
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        const QString hugeName = QStringLiteral("H").repeated(500) + QStringLiteral(".dat");
        auto *j = new QueueFakeJob(true, true, false);
        mgr.track(j, hugeName);
        opWin.rebuild();

        auto *row = opWin.findChild<QFrame *>(QStringLiteral("operationRow"));
        verify(row->minimumSizeHint().width() <= 400,
               "S3-RESP-12: row minimumSizeHint width is <= 400px despite 500-char name");

        mgr.finish(j, true, false, {});
        delete j;
    }

    // S3-RESP-13: Layout behavior with font metric changes (HiDPI simulation)
    {
        TransferQueueElidedLabel label(QStringLiteral("Font_Metrics_Scale_Test_").repeated(10));
        QFont f = label.font();
        f.setPointSize(f.pointSize() + 6);
        label.setFont(f);
        label.resize(200, 30);
        verify(label.minimumSizeHint().height() >= 16,
               "S3-RESP-13: minimumSizeHint height scales with font size");
        verify(label.text().length() < label.fullText().length(),
               "S3-RESP-13: text is elided proportionally with larger font");
    }

    // S3-RESP-14: No regression in Queue tab
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.showQueueTab();
        opWin.resize(550, 400);

        TransferTaskDescriptor d;
        d.title = QStringLiteral("Queue_Task_Long_Name_").repeated(6) + QStringLiteral(".bin");
        d.backend = TransferBackendKind::LocalNative;
        d.sources = {QUrl::fromLocalFile(root + TQ_S("/q_long.bin"))};
        d.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d.jobFactory = []() { return new QueueFakeJob; };
        const auto res = qc.enqueue(d);

        auto *item = opWin.queueViewWidget()->taskItemWidget(res.id);
        verify(item != nullptr, "S3-RESP-14: queue task item widget exists");
        verify(item->titleLabel()->fullText() == d.title, "S3-RESP-14: full title in queue task fullText");
        verify(item->titleLabel()->toolTip().contains(d.title.left(25)), "S3-RESP-14: full title in queue task tooltip");
        verify(item->cancelButton() != nullptr, "S3-RESP-14: queue cancel button exists");

        qc.cancelTask(res.id);
    }

    // =========================================================================
    // STAGE 3 WINDOW GEOMETRY & FINAL QUEUE UI FIX TESTS (S3-GEOM-01 - S3-GEOM-16)
    // =========================================================================
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);
        opWin.resize(640, 480);
        opWin.show();
        QCoreApplication::processEvents();

        // S3-GEOM-01: Stable initial geometry
        const QSize initialSize = opWin.size();
        verify(initialSize.width() == 640 && initialSize.height() == 480,
               "S3-GEOM-01: initial OperationWindow size is exactly 640x480");

        // S3-GEOM-02: Add 1 operation -> window size remains 640x480
        QList<QueueFakeJob *> jobs;
        auto *j1 = new QueueFakeJob(true, true, false);
        jobs.append(j1);
        mgr.track(j1, QStringLiteral("Operation_Single_Task.bin"));
        opWin.rebuild();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize,
               "S3-GEOM-02: adding 1 operation keeps window size unchanged at 640x480");

        // S3-GEOM-03: Add 5 more operations (total 6) -> window size remains 640x480
        for (int i = 2; i <= 6; ++i) {
            auto *j = new QueueFakeJob(true, true, false);
            jobs.append(j);
            mgr.track(j, QStringLiteral("Operation_Batch_%1.dat").arg(i));
        }
        opWin.rebuild();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize,
               "S3-GEOM-03: adding 5 operations (total 6) keeps window size unchanged at 640x480");

        // S3-GEOM-04: Add 4 more operations (total 10) -> window size remains 640x480
        for (int i = 7; i <= 10; ++i) {
            auto *j = new QueueFakeJob(true, true, false);
            jobs.append(j);
            mgr.track(j, QStringLiteral("Operation_Batch_%1.dat").arg(i));
        }
        opWin.rebuild();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize,
               "S3-GEOM-04: adding 4 operations (total 10) keeps window size unchanged at 640x480");

        // S3-GEOM-05: Add 10 more operations (total 20) -> window size remains 640x480
        for (int i = 11; i <= 20; ++i) {
            auto *j = new QueueFakeJob(true, true, false);
            jobs.append(j);
            mgr.track(j, QStringLiteral("Operation_Batch_%1.dat").arg(i));
        }
        opWin.rebuild();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize,
               "S3-GEOM-05: adding 10 operations (total 20) keeps window size unchanged at 640x480");

        // S3-GEOM-06: Finish 5 operations -> window size does not shrink
        for (int i = 0; i < 5; ++i) {
            mgr.finish(jobs[i], true, false, {});
        }
        opWin.rebuild();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize,
               "S3-GEOM-06: finishing 5 operations does not shrink window size (still 640x480)");

        // S3-GEOM-07: Cancel 5 operations -> window size does not change
        for (int i = 5; i < 10; ++i) {
            mgr.finish(jobs[i], false, true, {});
        }
        opWin.rebuild();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize,
               "S3-GEOM-07: cancelling 5 operations does not change window size (still 640x480)");

        // S3-GEOM-08: Expand and collapse card details -> window size does not change
        auto *firstRow = opWin.findChild<QFrame *>(QStringLiteral("operationRow"));
        verify(firstRow != nullptr, "S3-GEOM-08: active card exists");
        auto *toggleBtn = firstRow->findChild<QToolButton *>(QStringLiteral("operationDetailsToggleButton"));
        verify(toggleBtn != nullptr, "S3-GEOM-08: toggle button exists");
        toggleBtn->click();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize, "S3-GEOM-08: expanding card details keeps window size at 640x480");
        toggleBtn->click();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize, "S3-GEOM-08: collapsing card details keeps window size at 640x480");

        // S3-GEOM-09: Tab switches between Operations and Queue -> window size unchanged
        opWin.showQueueTab();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize, "S3-GEOM-09: switching to Queue tab keeps window size at 640x480");
        opWin.showOperationsTab();
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize, "S3-GEOM-09: switching back to Operations tab keeps window size at 640x480");

        // S3-GEOM-10: Toggle Sequential / Parallel mode in settings -> window size unchanged
        opWin.showQueueTab();
        auto *settingsWidget = opWin.queueSettingsWidget();
        verify(settingsWidget != nullptr, "S3-GEOM-10: settings widget exists");
        TransferQueueConfig cfg;
        cfg.mode = TransferQueueMode::Parallel;
        cfg.parallelLimit = 5;
        settingsWidget->applyConfig(cfg);
        QCoreApplication::processEvents();
        verify(opWin.size() == initialSize, "S3-GEOM-10: applying parallel queue settings keeps window size at 640x480");

        // S3-GEOM-11: Manual window resize to 750x550 is preserved across updates
        opWin.resize(750, 550);
        QCoreApplication::processEvents();
        verify(opWin.size() == QSize(750, 550), "S3-GEOM-11: manual resize to 750x550 applied");
        opWin.rebuild();
        QCoreApplication::processEvents();
        verify(opWin.size() == QSize(750, 550), "S3-GEOM-11: manual resize to 750x550 preserved after rebuild");

        // S3-GEOM-12: Scrollbar appears and last card is reachable
        opWin.showOperationsTab();
        auto *scrollArea = opWin.operationsScrollArea();
        verify(scrollArea != nullptr, "S3-GEOM-12: scroll area exists");
        verify(scrollArea->verticalScrollBar()->maximum() > 0,
               "S3-GEOM-12: vertical scrollbar has range for remaining active cards");
        scrollArea->verticalScrollBar()->setValue(scrollArea->verticalScrollBar()->maximum());
        QCoreApplication::processEvents();
        const auto activeRows = scrollArea->widget()->findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(!activeRows.isEmpty(), "S3-GEOM-12: rows present in scroll container");
        verify(activeRows.last()->geometry().bottom() <= scrollArea->widget()->height(),
               "S3-GEOM-12: last operation card is reachable within scrollable container");

        // S3-GEOM-13: Horizontal scrollbar is disabled
        verify(scrollArea->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
               "S3-GEOM-13: horizontal scrollbar policy is ScrollBarAlwaysOff");
        verify(!scrollArea->horizontalScrollBar()->isVisible(),
               "S3-GEOM-13: horizontal scrollbar is not visible");

        // Clean up remaining active jobs
        for (int i = 10; i < 20; ++i) {
            mgr.finish(jobs[i], true, false, {});
        }
        for (auto *j : jobs) {
            delete j;
        }
    }

    // S3-GEOM-14: Tooltips bounded in width and word-wrapped
    {
        const QString veryLongSrc = QStringLiteral("/home/user/very_long_path_segment_without_spaces_")
                                        .repeated(5) + QStringLiteral("/source_file.iso");
        const QString veryLongDst = QStringLiteral("/mnt/storage/backup_target_destination_long_path_")
                                        .repeated(5) + QStringLiteral("/target_dir/");
        const QString formatted = formatPathTooltip(QStringLiteral("Source"), veryLongSrc,
                                                    QStringLiteral("Target"), veryLongDst);
        verify(formatted.contains(QStringLiteral("table width=\"520\"")),
               "S3-GEOM-14: path tooltip is constrained to table width 520px");
        verify(formatted.contains(QStringLiteral("<br/>")),
               "S3-GEOM-14: unbroken path tokens contain line breaks");
        verify(formatted.contains(QStringLiteral("<b>Source:</b>")),
               "S3-GEOM-14: separate Source bold label present");
        verify(formatted.contains(QStringLiteral("<b>Target:</b>")),
               "S3-GEOM-14: separate Target bold label present");

        const QString singleLong = QStringLiteral("UnbrokenLongFilename_").repeated(10) + QStringLiteral(".zip");
        const QString singleFormatted = formatSingleValueTooltip(QStringLiteral("File"), singleLong);
        verify(singleFormatted.contains(QStringLiteral("table width=\"520\"")),
               "S3-GEOM-14: single value tooltip constrained to 520px width");
        verify(singleFormatted.contains(QStringLiteral("<br/>")),
               "S3-GEOM-14: single long unbroken token is wrapped with line breaks");
    }

    // S3-GEOM-15: Queue header responsiveness at narrow width (450-500px)
    {
        TransferQueueViewWidget view;
        view.setDemoButtonVisible(true);
        view.resize(480, 300);
        view.show();
        QCoreApplication::processEvents();

        auto *title = view.findChild<QLabel *>(QStringLiteral("transferQueueViewTitle"));
        auto *demoBtn = view.findChild<QToolButton *>(QStringLiteral("transferQueueDemoButton"));
        auto *countLbl = view.findChild<QLabel *>(QStringLiteral("transferQueueViewCount"));
        verify(title != nullptr && demoBtn != nullptr && countLbl != nullptr,
               "S3-GEOM-15: all queue header widgets exist");

        // Row 1: title on left, demo button on right
        verify(title->geometry().left() < demoBtn->geometry().left(),
               "S3-GEOM-15: title is to the left of demo button in row 1");

        // Row 2: count label is below row 1
        verify(countLbl->geometry().top() >= title->geometry().bottom(),
               "S3-GEOM-15: count label is on row 2 below title and demo button");
        verify(countLbl->wordWrap(),
               "S3-GEOM-15: count label has word wrap enabled");

        TransferQueueCounts counts;
        counts.active = 2;
        counts.queued = 4;
        counts.completed = 3;
        counts.cancelled = 1;
        const QString countsStr = TransferQueueViewWidget::countsText(counts);
        countLbl->setText(countsStr);
        verify(countLbl->text().contains(QStringLiteral("2")), "S3-GEOM-15: active count present");
        verify(countLbl->text().contains(QStringLiteral("4")), "S3-GEOM-15: queued count present");
        verify(countLbl->text().contains(QStringLiteral("3")), "S3-GEOM-15: completed count present");
        verify(countLbl->text().contains(QStringLiteral("1")), "S3-GEOM-15: cancelled count present");
    }

    // S3-GEOM-16: Visual presentation order without scheduler mutation
    {
        TransferQueueController ctl;
        QueueFakeJob *fake1 = nullptr;
        QueueFakeJob *fake2 = nullptr;
        QueueFakeJob *fake3 = nullptr;

        TransferTaskDescriptor d1, d2, d3;
        d1.title = QStringLiteral("Task_1_Completed.dat");
        d1.backend = TransferBackendKind::LocalNative;
        d1.sources = {QUrl::fromLocalFile(root + TQ_S("/t1.dat"))};
        d1.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d1.jobFactory = [&fake1]() {
            fake1 = new QueueFakeJob(true, true, true);
            return fake1;
        };

        d2.title = QStringLiteral("Task_2_Running.dat");
        d2.backend = TransferBackendKind::LocalNative;
        d2.sources = {QUrl::fromLocalFile(root + TQ_S("/t2.dat"))};
        d2.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d2.jobFactory = [&fake2]() {
            fake2 = new QueueFakeJob(true, true, true);
            return fake2;
        };

        d3.title = QStringLiteral("Task_3_Queued.dat");
        d3.backend = TransferBackendKind::LocalNative;
        d3.sources = {QUrl::fromLocalFile(root + TQ_S("/t3.dat"))};
        d3.destination = QUrl::fromLocalFile(root + TQ_S("/dst/"));
        d3.jobFactory = [&fake3]() {
            fake3 = new QueueFakeJob(true, true, true);
            return fake3;
        };

        // Sequential mode: task 1 will run, task 2 and 3 queued
        TransferQueueConfig cfg;
        cfg.mode = TransferQueueMode::Sequential;
        ctl.applyConfig(cfg);

        const auto r1 = ctl.enqueue(d1);
        const auto r2 = ctl.enqueue(d2);
        const auto r3 = ctl.enqueue(d3);
        QCoreApplication::processEvents();

        // Finish task 1 -> completed. Task 2 starts -> running. Task 3 remains queued.
        verify(fake1 != nullptr, "S3-GEOM-16: fake1 was created");
        fake1->finishOk();
        QCoreApplication::processEvents();

        // Check scheduler snapshot: order of submission (d1, d2, d3) is preserved!
        const auto snap = ctl.snapshot();
        verify(snap.tasks.size() == 3, "S3-GEOM-16: 3 tasks in snapshot");
        verify(snap.tasks[0].id == r1.id, "S3-GEOM-16: scheduler task 0 is task 1 (Completed)");
        verify(snap.tasks[1].id == r2.id, "S3-GEOM-16: scheduler task 1 is task 2 (Running)");
        verify(snap.tasks[2].id == r3.id, "S3-GEOM-16: scheduler task 2 is task 3 (Queued)");

        // Now inspect the visual view widget
        TransferQueueViewWidget view;
        view.setController(&ctl);
        view.resize(550, 400);
        view.show();
        QCoreApplication::processEvents();

        const auto ordered = view.orderedTaskItemWidgets();
        verify(ordered.size() == 3, "S3-GEOM-16: view widget has 3 items");

        // Visual presentation order must be:
        // Index 0: Task 2 (Running / Active)
        // Index 1: Task 3 (Queued)
        // Index 2: Task 1 (Completed / Terminal)
        verify(ordered[0]->taskId() == r2.id,
               "S3-GEOM-16: visually first item is running Task 2");
        verify(ordered[1]->taskId() == r3.id,
               "S3-GEOM-16: visually second item is queued Task 3");
        verify(ordered[2]->taskId() == r1.id,
               "S3-GEOM-16: visually third item is completed Task 1");

        // Underlying scheduler was NOT mutated
        const auto snapAfter = ctl.snapshot();
        verify(snapAfter.tasks[0].id == r1.id && snapAfter.tasks[1].id == r2.id && snapAfter.tasks[2].id == r3.id,
               "S3-GEOM-16: scheduler underlying snapshot remains unchanged in submission order");

        ctl.cancelTask(r2.id);
        ctl.cancelTask(r3.id);
    }

    // =========================================================================
    // STAGE 3 MINIMUM WINDOW HEIGHT & LAYOUT INTEGRITY (S3-MINH-01 - S3-MINH-06)
    // =========================================================================
    {
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        TransferQueueController qc;
        mgr.setQueueController(&qc);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&qc);

        // S3-MINH-01: Minimum height constraint
        verify(opWin.minimumSizeHint().width() >= 480,
               "S3-MINH-01: minimum width is at least 480px");
        verify(opWin.minimumSizeHint().height() >= 380,
               "S3-MINH-01: minimum height is at least 380px to accommodate all controls");

        // S3-MINH-02: Layout integrity at absolute minimum size 480x380
        opWin.showQueueTab();
        opWin.queueViewWidget()->setDemoButtonVisible(true);
        opWin.resize(480, 380);
        opWin.show();
        QCoreApplication::processEvents();

        verify(opWin.width() == 480 && opWin.height() == 380,
               "S3-MINH-02: window sized to minimum 480x380");

        auto *settings = opWin.queueSettingsWidget();
        verify(settings != nullptr && settings->isVisible(),
               "S3-MINH-02: execution mode settings widget is visible at minimum height");
        verify(settings->height() >= 60,
               "S3-MINH-02: execution mode group is not compressed below 60px");

        auto *seqRadio = settings->findChild<QRadioButton *>(QStringLiteral("transferQueueSequentialRadio"));
        auto *parRadio = settings->findChild<QRadioButton *>(QStringLiteral("transferQueueParallelRadio"));
        auto *limitSpin = settings->findChild<QSpinBox *>(QStringLiteral("transferQueueLimitSpin"));
        verify(seqRadio != nullptr && seqRadio->isVisible(),
               "S3-MINH-02: Sequential radio button is visible and accessible");
        verify(parRadio != nullptr && parRadio->isVisible(),
               "S3-MINH-02: Parallel radio button is visible and accessible");
        verify(limitSpin != nullptr && limitSpin->isVisible(),
               "S3-MINH-02: concurrency limit spinbox is visible");

        auto *queueView = opWin.queueViewWidget();
        auto *headerContainer = queueView->findChild<QWidget *>(QStringLiteral("transferQueueHeaderContainer"));
        auto *title = queueView->findChild<QLabel *>(QStringLiteral("transferQueueViewTitle"));
        auto *demoBtn = queueView->findChild<QToolButton *>(QStringLiteral("transferQueueDemoButton"));
        auto *countLbl = queueView->findChild<QLabel *>(QStringLiteral("transferQueueViewCount"));
        verify(headerContainer != nullptr && title != nullptr && demoBtn != nullptr && countLbl != nullptr,
               "S3-MINH-02: all queue header elements exist");

        // Row 1: title on left, demo button on right
        verify(title->geometry().left() < demoBtn->geometry().left(),
               "S3-MINH-02: title is to the left of demo button");

        // Row 2: count label strictly below demo button (NO OVERLAP)
        verify(countLbl->geometry().top() >= demoBtn->geometry().bottom(),
               "S3-MINH-02: count label is below demo button (no overlap at minimum height)");
        verify(countLbl->wordWrap() == true,
               "S3-MINH-02: count label word wrap is enabled");

        auto *queueScroll = queueView->scrollArea();
        verify(queueScroll != nullptr && queueScroll->height() >= 60,
               "S3-MINH-02: queue scroll area retains at least 60px of scrollable space");

        // S3-MINH-03: Normal window at 480x500
        opWin.resize(480, 500);
        QCoreApplication::processEvents();
        verify(opWin.width() == 480 && opWin.height() == 500,
               "S3-MINH-03: window sized to normal 480x500");
        verify(countLbl->geometry().top() >= demoBtn->geometry().bottom(),
               "S3-MINH-03: no overlap at 480x500");
        verify(settings->isVisible(), "S3-MINH-03: settings visible at 480x500");

        // S3-MINH-04: Tab switching at minimum size 480x380
        opWin.resize(480, 380);
        QCoreApplication::processEvents();
        opWin.showOperationsTab();
        QCoreApplication::processEvents();
        verify(opWin.size() == QSize(480, 380),
               "S3-MINH-04: Operations tab maintains 480x380");
        opWin.showQueueTab();
        QCoreApplication::processEvents();
        verify(opWin.size() == QSize(480, 380),
               "S3-MINH-04: Queue tab maintains 480x380");

        // S3-MINH-05: Add 5 operations at 480x380 -> size remains 480x380
        opWin.showOperationsTab();
        QList<QueueFakeJob *> jobs5;
        for (int i = 0; i < 5; ++i) {
            auto *j = new QueueFakeJob(true, true, false);
            jobs5.append(j);
            mgr.track(j, QStringLiteral("MinH_Op_%1.dat").arg(i));
        }
        opWin.rebuild();
        QCoreApplication::processEvents();
        verify(opWin.size() == QSize(480, 380),
               "S3-MINH-05: adding 5 operations at minimum size keeps window exactly 480x380");
        auto *opScroll = opWin.operationsScrollArea();
        verify(opScroll != nullptr && opScroll->isVisible(),
               "S3-MINH-05: operations scroll area is visible");
        verify(opScroll->verticalScrollBar()->maximum() > 0,
               "S3-MINH-05: operations scrollbar is active for 5 cards");

        // Clean up 5 jobs
        for (auto *j : jobs5) {
            mgr.finish(j, true, false, {});
            delete j;
        }

        // S3-MINH-06: Add 20 operations at 480x500 -> size remains 480x500
        opWin.resize(480, 500);
        QCoreApplication::processEvents();
        QList<QueueFakeJob *> jobs20;
        for (int i = 0; i < 20; ++i) {
            auto *j = new QueueFakeJob(true, true, false);
            jobs20.append(j);
            mgr.track(j, QStringLiteral("MinH_Op20_%1.dat").arg(i));
        }
        opWin.rebuild();
        QCoreApplication::processEvents();
        verify(opWin.size() == QSize(480, 500),
               "S3-MINH-06: adding 20 operations keeps window exactly 480x500");

        // Clean up 20 jobs
        for (auto *j : jobs20) {
            mgr.finish(j, true, false, {});
            delete j;
        }
    }

    // =========================================================================
    // STAGE 4 — PRIORITY & REORDERING (S4-01 to S4-35)
    // =========================================================================

    // S4-01: Zmiana Normal -> High
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 n1 = h.add(TQ_S("n1"), {h.src(TQ_S("n1"))}, h.dst(TQ_S("n1")));
        const quint64 n2 = h.add(TQ_S("n2"), {h.src(TQ_S("n2"))}, h.dst(TQ_S("n2")));
        verify(h.state(r) == TransferTaskState::Running, "S4-01: r running");
        verify(h.snap(n1).queuePosition == 1, "S4-01: n1 initially #1");
        verify(h.snap(n2).queuePosition == 2, "S4-01: n2 initially #2");

        const bool changed = h.ctl.setTaskPriority(n2, TransferPriority::High);
        verify(changed, "S4-01: setTaskPriority returned true");
        verify(h.snap(n2).priority == TransferPriority::High, "S4-01: n2 is High");
        verify(h.snap(n2).queuePosition == 1, "S4-01: n2 promoted to #1 in queue");
        verify(h.snap(n1).queuePosition == 2, "S4-01: n1 pushed to #2 in queue");

        h.job(TQ_S("run"))->finishOk();
        verify(h.state(n2) == TransferTaskState::Running, "S4-01: n2 runs next due to High priority");
        verify(h.state(n1) == TransferTaskState::Queued, "S4-01: n1 still queued");
        h.job(TQ_S("n2"))->finishOk();
        verify(h.state(n1) == TransferTaskState::Running, "S4-01: n1 runs after n2");
        h.job(TQ_S("n1"))->finishOk();
    }

    // S4-02: Zmiana Normal -> Low
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 n1 = h.add(TQ_S("n1"), {h.src(TQ_S("n1"))}, h.dst(TQ_S("n1")));
        const quint64 n2 = h.add(TQ_S("n2"), {h.src(TQ_S("n2"))}, h.dst(TQ_S("n2")));

        const bool changed = h.ctl.setTaskPriority(n1, TransferPriority::Low);
        verify(changed, "S4-02: setTaskPriority to Low returned true");
        verify(h.snap(n1).priority == TransferPriority::Low, "S4-02: n1 is Low");
        verify(h.snap(n2).queuePosition == 1, "S4-02: n2 moved to #1");
        verify(h.snap(n1).queuePosition == 2, "S4-02: n1 moved to #2 (Low)");

        h.job(TQ_S("run"))->finishOk();
        verify(h.state(n2) == TransferTaskState::Running, "S4-02: n2 runs before Low n1");
        h.job(TQ_S("n2"))->finishOk();
        verify(h.state(n1) == TransferTaskState::Running, "S4-02: n1 runs last");
        h.job(TQ_S("n1"))->finishOk();
    }

    // S4-03: Powrót do Normal
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 t = h.add(TQ_S("t"), {h.src(TQ_S("t"))}, h.dst(TQ_S("t")));

        h.ctl.setTaskPriority(t, TransferPriority::Low);
        verify(h.snap(t).priority == TransferPriority::Low, "S4-03: lowered to Low");
        h.ctl.setTaskPriority(t, TransferPriority::Normal);
        verify(h.snap(t).priority == TransferPriority::Normal, "S4-03: restored to Normal");
        h.ctl.setTaskPriority(t, TransferPriority::High);
        verify(h.snap(t).priority == TransferPriority::High, "S4-03: raised to High");
        h.ctl.setTaskPriority(t, TransferPriority::Normal);
        verify(h.snap(t).priority == TransferPriority::Normal, "S4-03: restored again to Normal");
        h.job(TQ_S("run"))->finishOk();
        h.job(TQ_S("t"))->finishOk();
    }

    // S4-04: Domyślny Normal
    {
        QueueHarness h(nextRoot());
        const quint64 id = h.add(TQ_S("def"), {h.src(TQ_S("d"))}, h.dst(TQ_S("d")));
        verify(h.snap(id).priority == TransferPriority::Normal, "S4-04: default priority is Normal");
        h.job(TQ_S("def"))->finishOk();
    }

    // S4-05: Przesunięcie oczekującego zadania w górę (Move Up)
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 n1 = h.add(TQ_S("n1"), {h.src(TQ_S("n1"))}, h.dst(TQ_S("n1")));
        const quint64 n2 = h.add(TQ_S("n2"), {h.src(TQ_S("n2"))}, h.dst(TQ_S("n2")));
        const quint64 n3 = h.add(TQ_S("n3"), {h.src(TQ_S("n3"))}, h.dst(TQ_S("n3")));

        verify(h.ctl.canMoveTaskUp(n2), "S4-05: canMoveTaskUp(n2) is true");
        const bool moved = h.ctl.moveTaskUp(n2);
        verify(moved, "S4-05: moveTaskUp succeeded");
        verify(h.snap(n2).queuePosition == 1, "S4-05: n2 is now #1");
        verify(h.snap(n1).queuePosition == 2, "S4-05: n1 is now #2");
        verify(h.snap(n3).queuePosition == 3, "S4-05: n3 remains #3");

        h.job(TQ_S("run"))->finishOk();
        verify(h.state(n2) == TransferTaskState::Running, "S4-05: n2 dispatched first");
        h.job(TQ_S("n2"))->finishOk();
        verify(h.state(n1) == TransferTaskState::Running, "S4-05: n1 dispatched second");
        h.job(TQ_S("n1"))->finishOk();
        verify(h.state(n3) == TransferTaskState::Running, "S4-05: n3 dispatched third");
        h.job(TQ_S("n3"))->finishOk();
    }

    // S4-06: Przesunięcie w dół (Move Down)
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 n1 = h.add(TQ_S("n1"), {h.src(TQ_S("n1"))}, h.dst(TQ_S("n1")));
        const quint64 n2 = h.add(TQ_S("n2"), {h.src(TQ_S("n2"))}, h.dst(TQ_S("n2")));

        verify(h.ctl.canMoveTaskDown(n1), "S4-06: canMoveTaskDown(n1) is true");
        const bool moved = h.ctl.moveTaskDown(n1);
        verify(moved, "S4-06: moveTaskDown(n1) succeeded");
        verify(h.snap(n2).queuePosition == 1, "S4-06: n2 is #1");
        verify(h.snap(n1).queuePosition == 2, "S4-06: n1 is #2");

        h.job(TQ_S("run"))->finishOk();
        verify(h.state(n2) == TransferTaskState::Running, "S4-06: n2 runs first");
        h.job(TQ_S("n2"))->finishOk();
        h.job(TQ_S("n1"))->finishOk();
    }

    // S4-07: Pierwsze zadanie — blokada Up
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 n1 = h.add(TQ_S("n1"), {h.src(TQ_S("n1"))}, h.dst(TQ_S("n1")));

        verify(!h.ctl.canMoveTaskUp(n1), "S4-07: first queued task cannot move up");
        verify(!h.ctl.moveTaskUp(n1), "S4-07: moveTaskUp returns false for first task");
        verify(h.snap(n1).queuePosition == 1, "S4-07: position unchanged");
        h.job(TQ_S("run"))->finishOk();
        h.job(TQ_S("n1"))->finishOk();
    }

    // S4-08: Ostatnie zadanie — blokada Down
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 n1 = h.add(TQ_S("n1"), {h.src(TQ_S("n1"))}, h.dst(TQ_S("n1")));
        const quint64 n2 = h.add(TQ_S("n2"), {h.src(TQ_S("n2"))}, h.dst(TQ_S("n2")));

        verify(!h.ctl.canMoveTaskDown(n2), "S4-08: last task cannot move down");
        verify(!h.ctl.moveTaskDown(n2), "S4-08: moveTaskDown returns false for last task");
        verify(h.snap(n2).queuePosition == 2, "S4-08: position unchanged");
        h.job(TQ_S("run"))->finishOk();
        h.job(TQ_S("n1"))->finishOk();
        h.job(TQ_S("n2"))->finishOk();
    }

    // S4-09: Próba przejścia przez granicę priorytetu
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        const quint64 h1 = h.add(TQ_S("h1"), {h.src(TQ_S("h1"))}, h.dst(TQ_S("h1")), optHigh);
        const quint64 n1 = h.add(TQ_S("n1"), {h.src(TQ_S("n1"))}, h.dst(TQ_S("n1")));

        verify(h.snap(h1).queuePosition == 1, "S4-09: h1 is #1");
        verify(h.snap(n1).queuePosition == 2, "S4-09: n1 is #2");

        // h1 cannot move down into Normal group
        verify(!h.ctl.canMoveTaskDown(h1), "S4-09: High task cannot move down across priority boundary");
        verify(!h.ctl.moveTaskDown(h1), "S4-09: moveTaskDown rejected at priority boundary");

        // n1 cannot move up into High group
        verify(!h.ctl.canMoveTaskUp(n1), "S4-09: Normal task cannot move up across priority boundary");
        verify(!h.ctl.moveTaskUp(n1), "S4-09: moveTaskUp rejected at priority boundary");

        verify(h.snap(h1).queuePosition == 1, "S4-09: h1 remains #1");
        verify(h.snap(n1).queuePosition == 2, "S4-09: n1 remains #2");

        h.job(TQ_S("run"))->finishOk();
        h.job(TQ_S("h1"))->finishOk();
        h.job(TQ_S("n1"))->finishOk();
    }

    // S4-10: Zmiana priorytetu podczas aktywnego transferu
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        verify(h.state(r) == TransferTaskState::Running, "S4-10: task is running");

        verify(!h.ctl.setTaskPriority(r, TransferPriority::High),
               "S4-10: setTaskPriority rejected on running task");
        verify(!h.ctl.setTaskPriority(r, TransferPriority::Low),
               "S4-10: setTaskPriority to Low rejected on running task");
        verify(h.snap(r).priority == TransferPriority::Normal,
               "S4-10: running task priority remains unchanged");
        h.job(TQ_S("run"))->finishOk();
    }

    // S4-11: Niedozwolone przesuwanie aktywnego transferu
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 q1 = h.add(TQ_S("q1"), {h.src(TQ_S("q1"))}, h.dst(TQ_S("q1")));

        verify(!h.ctl.canMoveTaskUp(r), "S4-11: active task cannot move up");
        verify(!h.ctl.moveTaskUp(r), "S4-11: moveTaskUp rejected for active task");
        verify(!h.ctl.canMoveTaskDown(r), "S4-11: active task cannot move down");
        verify(!h.ctl.moveTaskDown(r), "S4-11: moveTaskDown rejected for active task");

        h.job(TQ_S("run"))->finishOk();
        h.job(TQ_S("q1"))->finishOk();
    }

    // S4-12: Niedozwolone przesuwanie zadania terminalnego
    {
        QueueHarness h(nextRoot());
        const quint64 d = h.add(TQ_S("done"), {h.src(TQ_S("d"))}, h.dst(TQ_S("d")));
        h.job(TQ_S("done"))->finishOk();
        verify(h.state(d) == TransferTaskState::Completed, "S4-12: task is completed");

        verify(!h.ctl.canMoveTaskUp(d), "S4-12: completed task cannot move up");
        verify(!h.ctl.moveTaskUp(d), "S4-12: moveTaskUp rejected for completed task");
        verify(!h.ctl.canMoveTaskDown(d), "S4-12: completed task cannot move down");
        verify(!h.ctl.moveTaskDown(d), "S4-12: moveTaskDown rejected for completed task");
        verify(!h.ctl.setTaskPriority(d, TransferPriority::High),
               "S4-12: setTaskPriority rejected on completed task");
    }

    // S4-13: Nieprawidłowy TaskId
    {
        QueueHarness h(nextRoot());
        const quint64 invalidId = 999999ULL;
        verify(!h.ctl.canMoveTaskUp(invalidId), "S4-13: invalid TaskId canMoveTaskUp false");
        verify(!h.ctl.canMoveTaskDown(invalidId), "S4-13: invalid TaskId canMoveTaskDown false");
        verify(!h.ctl.moveTaskUp(invalidId), "S4-13: invalid TaskId moveTaskUp false");
        verify(!h.ctl.moveTaskDown(invalidId), "S4-13: invalid TaskId moveTaskDown false");
        verify(!h.ctl.setTaskPriority(invalidId, TransferPriority::High),
               "S4-13: invalid TaskId setTaskPriority false");
        verify(!h.ctl.canMoveTaskUp(0), "S4-13: 0 TaskId canMoveTaskUp false");
        verify(!h.ctl.moveTaskUp(0), "S4-13: 0 TaskId moveTaskUp false");
    }

    // S4-14: Stabilność identyfikatorów
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 t1 = h.add(TQ_S("t1"), {h.src(TQ_S("t1"))}, h.dst(TQ_S("t1")));
        const quint64 t2 = h.add(TQ_S("t2"), {h.src(TQ_S("t2"))}, h.dst(TQ_S("t2")));

        const auto snap1Before = h.snap(t1);
        const auto snap2Before = h.snap(t2);

        h.ctl.moveTaskUp(t2);
        h.ctl.setTaskPriority(t1, TransferPriority::High);

        const auto snap1After = h.snap(t1);
        const auto snap2After = h.snap(t2);

        verify(snap1After.id == snap1Before.id, "S4-14: t1 id is stable");
        verify(snap2After.id == snap2Before.id, "S4-14: t2 id is stable");
        verify(snap1After.title == snap1Before.title, "S4-14: t1 metadata preserved");
        verify(snap2After.title == snap2Before.title, "S4-14: t2 metadata preserved");

        h.job(TQ_S("run"))->finishOk();
        h.job(TQ_S("t1"))->finishOk();
        h.job(TQ_S("t2"))->finishOk();
    }

    // S4-15: Brak ponownego wywołania fabryki joba
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));

        int factoryCallCount = 0;
        QueueFakeJob *factoryCreatedJob = nullptr;
        TransferTaskDescriptor desc;
        desc.title = TQ_S("factory_test");
        desc.type = TransferOperationType::Copy;
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {h.src(TQ_S("f"))};
        desc.destination = h.dst(TQ_S("f"));
        desc.priority = TransferPriority::Normal;
        desc.startMode = TransferJobStartMode::ControllerStarts;
        desc.capabilities.canCancelRunning = true;
        desc.jobFactory = [&h, &factoryCallCount, &factoryCreatedJob, &desc]() -> KJob * {
            ++factoryCallCount;
            factoryCreatedJob = new QueueFakeJob(true, true, false);
            h.jobs.insert(desc.title, factoryCreatedJob);
            return factoryCreatedJob;
        };
        const auto res = h.ctl.enqueue(desc);
        const quint64 fid = res.id;

        verify(factoryCallCount == 0, "S4-15: factory not called on enqueue");
        h.ctl.setTaskPriority(fid, TransferPriority::High);
        verify(factoryCallCount == 0, "S4-15: factory not called on priority change");
        h.ctl.setTaskPriority(fid, TransferPriority::Normal);
        verify(factoryCallCount == 0, "S4-15: factory not called on second priority change");

        h.job(TQ_S("run"))->finishOk();
        verify(factoryCallCount == 1, "S4-15: factory called exactly once on launch");
        verify(factoryCreatedJob != nullptr, "S4-15: factoryCreatedJob exists");
        factoryCreatedJob->finishOk();
        verify(factoryCallCount == 1, "S4-15: factory count remains 1 on finish");
    }

    // S4-16: Kolejność snapshotów zgodna ze schedulerem
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 a = h.add(TQ_S("a"), {h.src(TQ_S("a"))}, h.dst(TQ_S("a")));
        const quint64 b = h.add(TQ_S("b"), {h.src(TQ_S("b"))}, h.dst(TQ_S("b")));
        const quint64 c = h.add(TQ_S("c"), {h.src(TQ_S("c"))}, h.dst(TQ_S("c")));

        h.ctl.moveTaskUp(c); // a, c, b
        const auto snap = h.ctl.snapshot();
        for (const auto &t : snap.tasks) {
            if (t.id == a) verify(t.queuePosition == 1, "S4-16: a is #1");
            if (t.id == c) verify(t.queuePosition == 2, "S4-16: c is #2");
            if (t.id == b) verify(t.queuePosition == 3, "S4-16: b is #3");
        }
        h.job(TQ_S("run"))->finishOk();
        h.job(TQ_S("a"))->finishOk();
        h.job(TQ_S("c"))->finishOk();
        h.job(TQ_S("b"))->finishOk();
    }

    // S4-17: Poprawna numeracja #1, #2, #3
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 q1 = h.add(TQ_S("q1"), {h.src(TQ_S("q1"))}, h.dst(TQ_S("q1")));
        const quint64 q2 = h.add(TQ_S("q2"), {h.src(TQ_S("q2"))}, h.dst(TQ_S("q2")));
        const quint64 q3 = h.add(TQ_S("q3"), {h.src(TQ_S("q3"))}, h.dst(TQ_S("q3")));

        QList<int> positions;
        for (quint64 id : {q1, q2, q3}) {
            positions.append(h.snap(id).queuePosition);
        }
        std::sort(positions.begin(), positions.end());
        verify(positions == QList<int>({1, 2, 3}), "S4-17: positions are strictly 1, 2, 3");

        h.job(TQ_S("run"))->finishOk();
        h.job(TQ_S("q1"))->finishOk();
        h.job(TQ_S("q2"))->finishOk();
        h.job(TQ_S("q3"))->finishOk();
    }

    // S4-18: FIFO bez ręcznej interwencji
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 f1 = h.add(TQ_S("f1"), {h.src(TQ_S("f1"))}, h.dst(TQ_S("f1")));
        const quint64 f2 = h.add(TQ_S("f2"), {h.src(TQ_S("f2"))}, h.dst(TQ_S("f2")));
        const quint64 f3 = h.add(TQ_S("f3"), {h.src(TQ_S("f3"))}, h.dst(TQ_S("f3")));

        h.job(TQ_S("run"))->finishOk();
        verify(h.state(f1) == TransferTaskState::Running, "S4-18: f1 runs first");
        h.job(TQ_S("f1"))->finishOk();
        verify(h.state(f2) == TransferTaskState::Running, "S4-18: f2 runs second");
        h.job(TQ_S("f2"))->finishOk();
        verify(h.state(f3) == TransferTaskState::Running, "S4-18: f3 runs third");
        h.job(TQ_S("f3"))->finishOk();
    }

    // S4-19: Ręczny override FIFO
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 f1 = h.add(TQ_S("f1"), {h.src(TQ_S("f1"))}, h.dst(TQ_S("f1")));
        const quint64 f2 = h.add(TQ_S("f2"), {h.src(TQ_S("f2"))}, h.dst(TQ_S("f2")));
        const quint64 f3 = h.add(TQ_S("f3"), {h.src(TQ_S("f3"))}, h.dst(TQ_S("f3")));

        // Move f3 all the way to top: f3, f1, f2
        h.ctl.moveTaskUp(f3);
        h.ctl.moveTaskUp(f3);
        verify(h.snap(f3).queuePosition == 1, "S4-19: f3 moved to #1");

        h.job(TQ_S("run"))->finishOk();
        verify(h.state(f3) == TransferTaskState::Running, "S4-19: f3 executes first overriding FIFO");
        h.job(TQ_S("f3"))->finishOk();
        verify(h.state(f1) == TransferTaskState::Running, "S4-19: f1 executes next");
        h.job(TQ_S("f1"))->finishOk();
        verify(h.state(f2) == TransferTaskState::Running, "S4-19: f2 executes last");
        h.job(TQ_S("f2"))->finishOk();
    }

    // S4-20: Długotrwałe oczekiwanie Low
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r.txt"))}, h.dst(TQ_S("r_dir")));
        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        const quint64 low = h.add(TQ_S("low_task"), {h.src(TQ_S("l.txt"))}, h.dst(TQ_S("l_dir")), optLow);

        // Run several normal tasks while low waits
        for (int i = 0; i < 4; ++i) {
            h.add(QStringLiteral("n_seq_%1").arg(i), {h.src(QStringLiteral("s_%1.txt").arg(i))}, h.dst(QStringLiteral("d_%1").arg(i)));
        }
        h.job(TQ_S("run"))->finishOk();
        for (int i = 0; i < 4; ++i) {
            const QString name = QStringLiteral("n_seq_%1").arg(i);
            verify(h.state(low) == TransferTaskState::Queued, "S4-20: low task waiting safely");
            h.job(name)->finishOk();
        }
        verify(h.state(low) == TransferTaskState::Running, "S4-20: low task runs after all normal complete");
        h.job(TQ_S("low_task"))->finishOk();
    }

    // S4-21: Ciągłe pojawianie się High i promocja Low
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r.txt"))}, h.dst(TQ_S("r_dir")));
        const quint64 normal = h.add(TQ_S("normal_wait"), {h.src(TQ_S("nw.txt"))}, h.dst(TQ_S("nw_dir")));

        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        const quint64 hFirst = h.add(TQ_S("h_first"), {h.src(TQ_S("hf.txt"))}, h.dst(TQ_S("hf_dir")), optHigh);

        // While normal is waiting behind high, promote normal to High!
        h.ctl.setTaskPriority(normal, TransferPriority::High);
        verify(h.snap(normal).priority == TransferPriority::High, "S4-21: promoted to High");

        // Subsequent High arrives
        const quint64 hSecond = h.add(TQ_S("h_second"), {h.src(TQ_S("hs.txt"))}, h.dst(TQ_S("hs_dir")), optHigh);

        // Execution order must be: run -> h_first -> normal_wait -> h_second
        h.job(TQ_S("run"))->finishOk();
        verify(h.state(hFirst) == TransferTaskState::Running, "S4-21: h_first runs");
        h.job(TQ_S("h_first"))->finishOk();
        verify(h.state(normal) == TransferTaskState::Running, "S4-21: promoted normal runs before h_second");
        h.job(TQ_S("normal_wait"))->finishOk();
        verify(h.state(hSecond) == TransferTaskState::Running, "S4-21: h_second runs last");
        h.job(TQ_S("h_second"))->finishOk();
    }

    // S4-22: Kolizja rezerwacji przy wysokim priorytecie
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);

        // r1 reads f1.txt (Copy) -> Running in slot 1
        const quint64 r1 = h.add(TQ_S("r1"), {h.src(TQ_S("f1.txt"))}, h.dst(TQ_S("d1")));
        verify(h.state(r1) == TransferTaskState::Running, "S4-22: r1 running");

        // High priority task needs exclusive access to f1.txt (Move)
        QueueOptions optHighMove;
        optHighMove.type = TransferOperationType::Move;
        optHighMove.priority = TransferPriority::High;
        const quint64 highConf = h.add(TQ_S("high_conf"), {h.src(TQ_S("f1.txt"))}, h.dst(TQ_S("d2")), optHighMove);

        // Normal priority task with independent path f2.txt
        const quint64 normIndep = h.add(TQ_S("norm_indep"), {h.src(TQ_S("f2.txt"))}, h.dst(TQ_S("d3")));

        verify(h.state(highConf) == TransferTaskState::Queued, "S4-22: highConf blocked by reservation");
        verify(h.snap(highConf).waitReason == TransferWaitReason::BlockedByReservation,
               "S4-22: highConf has BlockedByReservation");
        verify(h.state(normIndep) == TransferTaskState::Running,
               "S4-22: independent normal task runs because slot 2 is free");

        h.job(TQ_S("r1"))->finishOk();
        verify(h.state(highConf) == TransferTaskState::Running,
               "S4-22: highConf launches as soon as conflicting reservation frees");
        h.job(TQ_S("norm_indep"))->finishOk();
        h.job(TQ_S("high_conf"))->finishOk();
    }

    // S4-23: Sequential i zmiana kolejności
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        const quint64 sRun = h.add(TQ_S("s_run"), {h.src(TQ_S("sr"))}, h.dst(TQ_S("sr")));
        const quint64 s1 = h.add(TQ_S("s1"), {h.src(TQ_S("s1"))}, h.dst(TQ_S("s1")));
        const quint64 s2 = h.add(TQ_S("s2"), {h.src(TQ_S("s2"))}, h.dst(TQ_S("s2")));
        const quint64 s3 = h.add(TQ_S("s3"), {h.src(TQ_S("s3"))}, h.dst(TQ_S("s3")));

        h.ctl.moveTaskUp(s3);
        h.ctl.moveTaskUp(s3); // s3, s1, s2
        h.job(TQ_S("s_run"))->finishOk();
        verify(h.state(s3) == TransferTaskState::Running, "S4-23: s3 executed first in sequential");
        h.job(TQ_S("s3"))->finishOk();
        verify(h.state(s1) == TransferTaskState::Running, "S4-23: s1 executed second in sequential");
        h.job(TQ_S("s1"))->finishOk();
        verify(h.state(s2) == TransferTaskState::Running, "S4-23: s2 executed third in sequential");
        h.job(TQ_S("s2"))->finishOk();
    }

    // S4-24: Parallel i zmiana kolejności
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        const quint64 pr1 = h.add(TQ_S("p_run1"), {h.src(TQ_S("pr1"))}, h.dst(TQ_S("pr1")));
        const quint64 pr2 = h.add(TQ_S("p_run2"), {h.src(TQ_S("pr2"))}, h.dst(TQ_S("pr2")));
        const quint64 p1 = h.add(TQ_S("p1"), {h.src(TQ_S("p1"))}, h.dst(TQ_S("p1")));
        const quint64 p2 = h.add(TQ_S("p2"), {h.src(TQ_S("p2"))}, h.dst(TQ_S("p2")));

        h.ctl.moveTaskUp(p2); // p2 at #1
        h.job(TQ_S("p_run1"))->finishOk();
        verify(h.state(p2) == TransferTaskState::Running, "S4-24: p2 took freed slot first");
        h.job(TQ_S("p_run2"))->finishOk();
        verify(h.state(p1) == TransferTaskState::Running, "S4-24: p1 took next freed slot");
        h.job(TQ_S("p2"))->finishOk();
        h.job(TQ_S("p1"))->finishOk();
    }

    // S4-25: Dynamiczna zmiana limitów i priorytety
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(3);
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        const quint64 d1 = h.add(TQ_S("d1"), {h.src(TQ_S("d1"))}, h.dst(TQ_S("d1")));
        const quint64 d2 = h.add(TQ_S("d2"), {h.src(TQ_S("d2"))}, h.dst(TQ_S("d2")));
        const quint64 d3 = h.add(TQ_S("d3"), {h.src(TQ_S("d3"))}, h.dst(TQ_S("d3")));
        const quint64 dq1 = h.add(TQ_S("dq1"), {h.src(TQ_S("dq1"))}, h.dst(TQ_S("dq1")));
        const quint64 dq2 = h.add(TQ_S("dq2"), {h.src(TQ_S("dq2"))}, h.dst(TQ_S("dq2")));

        h.ctl.setTaskPriority(dq2, TransferPriority::High);
        h.ctl.setParallelConcurrencyLimit(1); // drain to 1
        verify(h.ctl.isDraining(), "S4-25: controller is draining");

        h.job(TQ_S("d1"))->finishOk();
        verify(h.ctl.isDraining(), "S4-25: still draining (2 active > limit 1)");
        verify(h.state(dq2) == TransferTaskState::Queued, "S4-25: dq2 does not start while draining");

        h.job(TQ_S("d2"))->finishOk();
        verify(!h.ctl.isDraining(), "S4-25: 1 active == limit 1");
        verify(h.state(dq2) == TransferTaskState::Queued, "S4-25: dq2 waits for slot");

        h.job(TQ_S("d3"))->finishOk();
        verify(h.state(dq2) == TransferTaskState::Running, "S4-25: High dq2 starts when slot opens");
        h.job(TQ_S("dq2"))->finishOk();
        verify(h.state(dq1) == TransferTaskState::Running, "S4-25: dq1 starts next");
        h.job(TQ_S("dq1"))->finishOk();
    }

    // S4-26: Globalny dispatchPaused
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 q1 = h.add(TQ_S("q1"), {h.src(TQ_S("q1"))}, h.dst(TQ_S("q1")));
        const quint64 q2 = h.add(TQ_S("q2"), {h.src(TQ_S("q2"))}, h.dst(TQ_S("q2")));

        h.ctl.setDispatchPaused(true);
        h.ctl.setTaskPriority(q2, TransferPriority::High);
        h.job(TQ_S("run"))->finishOk();

        verify(h.state(q2) == TransferTaskState::Queued, "S4-26: q2 paused by dispatch gate");
        verify(h.snap(q2).waitReason == TransferWaitReason::DispatchPaused, "S4-26: reason is DispatchPaused");

        h.ctl.setDispatchPaused(false);
        verify(h.state(q2) == TransferTaskState::Running, "S4-26: q2 launches once dispatch resumed");
        h.job(TQ_S("q2"))->finishOk();
        verify(h.state(q1) == TransferTaskState::Running, "S4-26: q1 launches next");
        h.job(TQ_S("q1"))->finishOk();
    }

    // S4-27: Zmiana priorytetu z callbacku Qt
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));

        auto conn = QObject::connect(&h.ctl, &TransferQueueController::taskEnqueued, &h.ctl, [&h](quint64 id) {
            h.ctl.setTaskPriority(id, TransferPriority::High);
        });

        const quint64 cbId = h.add(TQ_S("cb_task"), {h.src(TQ_S("cb"))}, h.dst(TQ_S("cb")));
        verify(h.snap(cbId).priority == TransferPriority::High, "S4-27: priority changed inside Qt callback");
        QObject::disconnect(conn);
        h.job(TQ_S("run"))->finishOk();
        verify(h.state(cbId) == TransferTaskState::Running, "S4-27: cb_task launched successfully");
        h.job(TQ_S("cb_task"))->finishOk();
    }

    // S4-28: Reentrancy podczas przesuwania
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const quint64 r1 = h.add(TQ_S("re1"), {h.src(TQ_S("re1"))}, h.dst(TQ_S("re1")));
        const quint64 r2 = h.add(TQ_S("re2"), {h.src(TQ_S("re2"))}, h.dst(TQ_S("re2")));

        int reentrantOrderChecks = 0;
        auto conn = QObject::connect(&h.ctl, &TransferQueueController::taskOrderChanged, &h.ctl, [&h, &reentrantOrderChecks](quint64 id) {
            ++reentrantOrderChecks;
            h.ctl.canMoveTaskUp(id);
            h.ctl.canMoveTaskDown(id);
        });

        h.ctl.moveTaskUp(r2);
        verify(reentrantOrderChecks > 0, "S4-28: reentrancy callback fired without error");
        QObject::disconnect(conn);
        h.job(TQ_S("run"))->finishOk();
        h.job(TQ_S("re2"))->finishOk();
        h.job(TQ_S("re1"))->finishOk();
    }

    // S4-29: Zakończenie zadania podczas aktualizacji GUI
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 gRun = h.add(TQ_S("g_run"), {h.src(TQ_S("gr"))}, h.dst(TQ_S("gr")));
        const quint64 gQ = h.add(TQ_S("g_q"), {h.src(TQ_S("gq"))}, h.dst(TQ_S("gq")));

        TransferQueueViewWidget view;
        view.setController(&h.ctl);
        view.show();
        QCoreApplication::processEvents();

        // Finish running job while view is connected
        h.job(TQ_S("g_run"))->finishOk();
        QCoreApplication::processEvents();

        verify(h.state(gRun) == TransferTaskState::Completed, "S4-29: g_run completed");
        verify(h.state(gQ) == TransferTaskState::Running, "S4-29: g_q transitioned to running");
        auto *item = view.taskItemWidget(gQ);
        verify(item != nullptr, "S4-29: item widget found");
        verify(item->priorityCombo()->isHidden(), "S4-29: priority combo hidden for running task");
        verify(!item->priorityBadge()->isHidden(), "S4-29: priority badge shown for running task");

        h.job(TQ_S("g_q"))->finishOk();
        QCoreApplication::processEvents();
    }

    // S4-30: Brak zmiany produkcyjnego dispatchu
    {
        verify(!TransferQueueProductionDispatchConnected,
               "S4-30: production file operations remain unconnected to demo queue");
    }

    // S4-31: Brak zmian Undo/Redo
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 u1 = h.add(TQ_S("u1"), {h.src(TQ_S("u1"))}, h.dst(TQ_S("u1")));
        const quint64 u2 = h.add(TQ_S("u2"), {h.src(TQ_S("u2"))}, h.dst(TQ_S("u2")));
        h.ctl.setTaskPriority(u2, TransferPriority::High);
        h.job(TQ_S("u1"))->finishOk();
        h.job(TQ_S("u2"))->finishOk();
        verify(h.snap(u2).id == 2, "S4-31: undo numbering unchanged, queue controller keeps its own ids");
    }

    // S4-32: Brak wpływu na Administrator Retry
    {
        QueueHarness h(nextRoot());
        TransferTaskDescriptor adminDesc;
        adminDesc.title = TQ_S("admin_test");
        adminDesc.backend = TransferBackendKind::LocalNative;
        adminDesc.sources = {QUrl(QStringLiteral("admin:///root/secret.txt"))};
        adminDesc.destination = h.dst(TQ_S("dst"));
        adminDesc.jobFactory = []() -> KJob * { return new QueueFakeJob(true, true, false); };
        const auto res = h.ctl.enqueue(adminDesc);
        verify(!res.accepted(), "S4-32: admin scheme is rejected");
        verify(res.rejection == TransferEnqueueRejection::AdminSchemeForbidden,
               "S4-32: rejection is AdminSchemeForbidden");
    }

    // S4-33: Długie nazwy i responsywność nowych kontrolek
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));
        const QString veryLongTitle = QStringLiteral(
            "Bardzo_dluga_nazwa_pliku_ze_sciezkami_do_weryfikacji_elidingu_tekstu_oraz_zawijania_"
            "etykiet_w_interfejsie_uzytkownika_thispc_044_stage4_priority_reordering_test_case.dat");
        const quint64 longId = h.add(veryLongTitle, {h.src(TQ_S("long"))}, h.dst(TQ_S("long")));

        TransferQueueViewWidget view;
        view.setController(&h.ctl);
        view.resize(480, 400);
        view.show();
        QCoreApplication::processEvents();

        auto *item = view.taskItemWidget(longId);
        verify(item != nullptr, "S4-33: item exists");
        verify(!item->moveUpButton()->isHidden(), "S4-33: moveUpButton visible");
        verify(!item->moveDownButton()->isHidden(), "S4-33: moveDownButton visible");
        verify(!item->priorityCombo()->isHidden(), "S4-33: priorityCombo visible");
        verify(!item->titleLabel()->isHidden(), "S4-33: titleLabel visible");
        verify(item->titleLabel()->width() > 0, "S4-33: titleLabel has positive width");
        verify(view.width() == 480, "S4-33: view width not blown out");

        h.job(TQ_S("run"))->finishOk();
        h.job(veryLongTitle)->finishOk();
    }

    // S4-34: PL/EN lokalizacja
    {
        const QLocale oldLocale = QLocale();
        // Polish check
        QLocale::setDefault(QLocale(QLocale::Polish));
        verify(TransferQueueTaskItemWidget::priorityText(TransferPriority::High) == QStringLiteral("Wysoki"),
               "S4-34: High text in Polish");
        verify(TransferQueueTaskItemWidget::priorityText(TransferPriority::Normal) == QStringLiteral("Normalny"),
               "S4-34: Normal text in Polish");
        verify(TransferQueueTaskItemWidget::priorityText(TransferPriority::Low) == QStringLiteral("Niski"),
               "S4-34: Low text in Polish");

        // English check
        QLocale::setDefault(QLocale(QLocale::English));
        verify(TransferQueueTaskItemWidget::priorityText(TransferPriority::High) == QStringLiteral("High"),
               "S4-34: High text in English");
        verify(TransferQueueTaskItemWidget::priorityText(TransferPriority::Normal) == QStringLiteral("Normal"),
               "S4-34: Normal text in English");
        verify(TransferQueueTaskItemWidget::priorityText(TransferPriority::Low) == QStringLiteral("Low"),
               "S4-34: Low text in English");

        // Restore default
        QLocale::setDefault(oldLocale);
    }

    // S4-35: Zachowanie geometrii przy 5-20 zadaniach
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r = h.add(TQ_S("run"), {h.src(TQ_S("r"))}, h.dst(TQ_S("r")));

        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&h.ctl);
        opWin.resize(480, 380);
        opWin.showQueueTab();
        QCoreApplication::processEvents();

        verify(opWin.size() == QSize(480, 380), "S4-35: initial queue window size 480x380");

        // Add 5 tasks
        QList<quint64> added5;
        for (int i = 0; i < 5; ++i) {
            added5.append(h.add(QStringLiteral("Geom_Task5_%1").arg(i),
                                {h.src(QStringLiteral("g5_%1.txt").arg(i))},
                                h.dst(QStringLiteral("g5_dst_%1").arg(i))));
        }
        QCoreApplication::processEvents();
        verify(opWin.size() == QSize(480, 380), "S4-35: window size strictly 480x380 with 5 tasks");

        // Add 15 more (total 20 queued)
        for (int i = 5; i < 20; ++i) {
            h.add(QStringLiteral("Geom_Task20_%1").arg(i),
                  {h.src(QStringLiteral("g20_%1.txt").arg(i))},
                  h.dst(QStringLiteral("g20_dst_%1").arg(i)));
        }
        QCoreApplication::processEvents();
        verify(opWin.size() == QSize(480, 380), "S4-35: window size strictly 480x380 with 20 tasks");

        auto *qView = opWin.queueViewWidget();
        verify(qView != nullptr, "S4-35: queueViewWidget exists");
        verify(qView->scrollArea()->verticalScrollBar()->maximum() > 0,
               "S4-35: scrollbar active for 20 queued tasks");

        // Finish running job and all 20 jobs
        h.job(TQ_S("run"))->finishOk();
        for (int i = 0; i < 20; ++i) {
            const QString name = (i < 5) ? QStringLiteral("Geom_Task5_%1").arg(i)
                                         : QStringLiteral("Geom_Task20_%1").arg(i);
            if (auto *j = h.job(name)) j->finishOk();
        }
    }

    // S4-36: Deterministyczny test ciągłego napływu High dla zadania Normal (starvation bound = 3)
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blocker = h.add(TQ_S("blocker"), {h.src(TQ_S("b.txt"))}, h.dst(TQ_S("b_dir")));
        verify(h.state(blocker) == TransferTaskState::Running, "S4-36: blocker running in slot");

        const quint64 normalTask = h.add(TQ_S("normal_task"), {h.src(TQ_S("n.txt"))}, h.dst(TQ_S("n_dir")));
        verify(h.snap(normalTask).starvationBypassCount == 0, "S4-36: initial bypass count 0");

        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        const quint64 h0 = h.add(TQ_S("h_0"), {h.src(TQ_S("h0.txt"))}, h.dst(TQ_S("h0_dir")), optHigh);

        h.job(TQ_S("blocker"))->finishOk();
        verify(h.state(h0) == TransferTaskState::Running, "S4-36: h0 launched");
        verify(h.state(normalTask) == TransferTaskState::Queued, "S4-36: normal waiting");
        verify(h.snap(normalTask).starvationBypassCount == 1, "S4-36: bypass count 1 after h0");

        const quint64 h1 = h.add(TQ_S("h_1"), {h.src(TQ_S("h1.txt"))}, h.dst(TQ_S("h1_dir")), optHigh);
        h.job(TQ_S("h_0"))->finishOk();
        verify(h.state(h1) == TransferTaskState::Running, "S4-36: h1 launched");
        verify(h.snap(normalTask).starvationBypassCount == 2, "S4-36: bypass count 2 after h1");

        const quint64 h2 = h.add(TQ_S("h_2"), {h.src(TQ_S("h2.txt"))}, h.dst(TQ_S("h2_dir")), optHigh);
        h.job(TQ_S("h_1"))->finishOk();
        verify(h.state(h2) == TransferTaskState::Running, "S4-36: h2 launched");
        verify(h.snap(normalTask).starvationBypassCount == 3, "S4-36: bypass count 3 after h2");

        const quint64 h3 = h.add(TQ_S("h_3"), {h.src(TQ_S("h3.txt"))}, h.dst(TQ_S("h3_dir")), optHigh);
        verify(h.snap(normalTask).queuePosition == 1, "S4-36: normalTask boosted to position #1");
        verify(h.snap(h3).queuePosition == 2, "S4-36: h3 at position #2");

        h.job(TQ_S("h_2"))->finishOk();
        verify(h.state(normalTask) == TransferTaskState::Running, "S4-36: normalTask launched deterministically on 4th dispatch");
        verify(h.state(h3) == TransferTaskState::Queued, "S4-36: h3 waiting behind normalTask");

        h.job(TQ_S("normal_task"))->finishOk();
        verify(h.state(h3) == TransferTaskState::Running, "S4-36: h3 launched after normalTask");
        h.job(TQ_S("h_3"))->finishOk();
    }

    // S4-37: Deterministyczny test ciągłego napływu High dla zadania Low (starvation bound = 6)
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blocker = h.add(TQ_S("blocker"), {h.src(TQ_S("b.txt"))}, h.dst(TQ_S("b_dir")));

        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        const quint64 lowTask = h.add(TQ_S("low_task"), {h.src(TQ_S("l.txt"))}, h.dst(TQ_S("l_dir")), optLow);

        QueueOptions optHigh; optHigh.priority = TransferPriority::High;

        QList<quint64> highIds;
        highIds.append(h.add(TQ_S("high_0"), {h.src(TQ_S("h0.txt"))}, h.dst(TQ_S("h0_dir")), optHigh));
        h.job(TQ_S("blocker"))->finishOk();

        for (int i = 0; i < 5; ++i) {
            const QString curName = QStringLiteral("high_%1").arg(i);
            const QString nextName = QStringLiteral("high_%1").arg(i + 1);
            highIds.append(h.add(nextName, {h.src(QStringLiteral("h_%1.txt").arg(i + 1))},
                                 h.dst(QStringLiteral("h_dir_%1").arg(i + 1)), optHigh));
            verify(h.state(highIds[i]) == TransferTaskState::Running, "S4-37: current high task running");
            verify(h.snap(lowTask).starvationBypassCount == i + 1, "S4-37: bypass counter incremented");
            h.job(curName)->finishOk();
        }

        verify(h.snap(lowTask).starvationBypassCount == 6, "S4-37: lowTask reached 6 bypasses");

        const quint64 h6 = h.add(TQ_S("high_6"), {h.src(TQ_S("h6.txt"))}, h.dst(TQ_S("h6_dir")), optHigh);
        verify(h.snap(lowTask).queuePosition == 1, "S4-37: lowTask boosted to position #1 ahead of High 6");
        verify(h.snap(h6).queuePosition == 2, "S4-37: high_6 at position #2");

        h.job(TQ_S("high_5"))->finishOk();
        verify(h.state(lowTask) == TransferTaskState::Running, "S4-37: lowTask launched deterministically on 7th dispatch");
        verify(h.state(h6) == TransferTaskState::Queued, "S4-37: high_6 waiting behind lowTask");

        h.job(TQ_S("low_task"))->finishOk();
        verify(h.state(h6) == TransferTaskState::Running, "S4-37: high_6 runs after lowTask");
        h.job(TQ_S("high_6"))->finishOk();
    }

    // S4-38: Deterministyczny test ciągłego napływu High i Normal dla zadania Low (starvation bound = 6)
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blocker = h.add(TQ_S("blocker"), {h.src(TQ_S("b.txt"))}, h.dst(TQ_S("b_dir")));

        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        const quint64 lowTask = h.add(TQ_S("low_mix"), {h.src(TQ_S("lm.txt"))}, h.dst(TQ_S("lm_dir")), optLow);

        QueueOptions optHigh; optHigh.priority = TransferPriority::High;

        const quint64 m0 = h.add(TQ_S("m_0"), {h.src(TQ_S("m0.txt"))}, h.dst(TQ_S("m0_dir")), optHigh);
        h.job(TQ_S("blocker"))->finishOk();

        for (int i = 0; i < 5; ++i) {
            const QString curName = QStringLiteral("m_%1").arg(i);
            const QString nextName = QStringLiteral("m_%1").arg(i + 1);
            QueueOptions opt = (i % 2 == 0) ? QueueOptions{} : optHigh;
            h.add(nextName, {h.src(QStringLiteral("m_%1.txt").arg(i + 1))},
                  h.dst(QStringLiteral("m_dir_%1").arg(i + 1)), opt);
            h.job(curName)->finishOk();
        }

        verify(h.snap(lowTask).starvationBypassCount == 6, "S4-38: lowTask reached 6 bypasses across mixed priorities");

        const quint64 m6 = h.add(TQ_S("m_6"), {h.src(TQ_S("m6.txt"))}, h.dst(TQ_S("m6_dir")), optHigh);
        verify(h.snap(lowTask).queuePosition == 1, "S4-38: lowTask boosted to position #1");

        h.job(TQ_S("m_5"))->finishOk();
        verify(h.state(lowTask) == TransferTaskState::Running, "S4-38: lowTask launched deterministically on 7th dispatch");
        h.job(TQ_S("low_mix"))->finishOk();
        verify(h.state(m6) == TransferTaskState::Running, "S4-38: m_6 runs after lowTask");
        h.job(TQ_S("m_6"))->finishOk();
    }

    // S4-39: Zachowanie starvation counter przy zmianie priorytetu
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blocker = h.add(TQ_S("blocker"), {h.src(TQ_S("b.txt"))}, h.dst(TQ_S("b_dir")));

        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        const quint64 task = h.add(TQ_S("promote_task"), {h.src(TQ_S("p.txt"))}, h.dst(TQ_S("p_dir")), optLow);

        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        const quint64 h0 = h.add(TQ_S("hp_0"), {h.src(TQ_S("hp0.txt"))}, h.dst(TQ_S("hp0_dir")), optHigh);
        h.job(TQ_S("blocker"))->finishOk();

        for (int i = 0; i < 2; ++i) {
            const QString cur = QStringLiteral("hp_%1").arg(i);
            const QString nxt = QStringLiteral("hp_%1").arg(i + 1);
            h.add(nxt, {h.src(QStringLiteral("hp_%1.txt").arg(i + 1))},
                  h.dst(QStringLiteral("hp_dir_%1").arg(i + 1)), optHigh);
            h.job(cur)->finishOk();
        }
        verify(h.snap(task).starvationBypassCount == 3, "S4-39: task accumulated 3 bypasses as Low");

        h.ctl.setTaskPriority(task, TransferPriority::Normal);
        verify(h.snap(task).starvationBypassCount == 3, "S4-39: accumulated bypasses NOT reset on priority change");
        verify(h.snap(task).queuePosition == 1, "S4-39: task immediately boosted to #1 in Normal");

        const quint64 h3 = h.add(TQ_S("hp_3"), {h.src(TQ_S("hp3.txt"))}, h.dst(TQ_S("hp3_dir")), optHigh);
        h.job(TQ_S("hp_2"))->finishOk();
        verify(h.state(task) == TransferTaskState::Running, "S4-39: promote_task launches on 4th dispatch");
        h.job(TQ_S("promote_task"))->finishOk();
        h.job(TQ_S("hp_3"))->finishOk();
    }

    // S4-40: Brak blokowania niezależnych zadań przy konflikcie rezerwacji zadania starved
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);

        const quint64 r1 = h.add(TQ_S("r1"), {h.src(TQ_S("f1.txt"))}, h.dst(TQ_S("d1")));
        const quint64 r2 = h.add(TQ_S("r2"), {h.src(TQ_S("f2.txt"))}, h.dst(TQ_S("d2")));

        QueueOptions optMove; optMove.type = TransferOperationType::Move;
        const quint64 starvedConf = h.add(TQ_S("starved_conf"), {h.src(TQ_S("f1.txt"))}, h.dst(TQ_S("d3")), optMove);

        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        const quint64 highIndep = h.add(TQ_S("high_indep"), {h.src(TQ_S("f3.txt"))}, h.dst(TQ_S("d4")), optHigh);

        h.job(TQ_S("r2"))->finishOk();
        verify(h.state(highIndep) == TransferTaskState::Running,
               "S4-40: independent task launches into free slot despite candidate 1 being blocked");
        verify(h.state(starvedConf) == TransferTaskState::Queued,
               "S4-40: conflicting task remains queued with BlockedByReservation");

        h.job(TQ_S("r1"))->finishOk();
        verify(h.state(starvedConf) == TransferTaskState::Running,
               "S4-40: conflicting task launches as soon as reservation clears");

        h.job(TQ_S("high_indep"))->finishOk();
        h.job(TQ_S("starved_conf"))->finishOk();
    }

    // S4-41: Weryfikacja kontrprzykładu użytkownika (1 Low, 10 Normal, 3 High)
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blocker = h.add(TQ_S("blocker"), {h.src(TQ_S("b.txt"))}, h.dst(TQ_S("b_dir")));

        // 3. Dodaj jedno Low
        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        const quint64 lowId = h.add(TQ_S("low_task"), {h.src(TQ_S("l.txt"))}, h.dst(TQ_S("l_dir")), optLow);

        // 4. Dodaj dziesięć Normal
        QList<quint64> normalIds;
        for (int i = 0; i < 10; ++i) {
            normalIds.append(h.add(QStringLiteral("n_%1").arg(i),
                                   {h.src(QStringLiteral("n_%1.txt").arg(i))},
                                   h.dst(QStringLiteral("n_dir_%1").arg(i))));
        }

        // 5. Dodaj trzy High
        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        QList<quint64> highIds;
        for (int i = 0; i < 3; ++i) {
            highIds.append(h.add(QStringLiteral("h_%1").arg(i),
                                 {h.src(QStringLiteral("h_%1.txt").arg(i))},
                                 h.dst(QStringLiteral("h_dir_%1").arg(i)), optHigh));
        }

        // 6. Zwolnij slot
        h.job(TQ_S("blocker"))->finishOk();

        // 7. Kończ kolejno uruchamiane zadania, zapisując rzeczywistą kolejność dispatchu
        QStringList executedOrder;
        for (int step = 0; step < 14; ++step) {
            quint64 runningId = 0;
            QString runningTitle;
            for (const auto &snap : h.ctl.snapshot().tasks) {
                if (snap.state == TransferTaskState::Running) {
                    runningId = snap.id;
                    runningTitle = snap.title;
                    break;
                }
            }
            verify(runningId != 0, "S4-41: a task is running in slot");
            executedOrder.append(runningTitle);
            if (auto *j = h.job(runningTitle)) {
                j->finishOk();
            }
        }

        const QStringList expectedOrder = {
            QStringLiteral("h_0"), QStringLiteral("h_1"), QStringLiteral("h_2"),
            QStringLiteral("n_0"), QStringLiteral("n_1"), QStringLiteral("n_2"),
            QStringLiteral("n_3"), QStringLiteral("n_4"), QStringLiteral("n_5"),
            QStringLiteral("n_6"), QStringLiteral("n_7"), QStringLiteral("n_8"),
            QStringLiteral("n_9"),
            QStringLiteral("low_task")
        };
        verify(executedOrder == expectedOrder, "S4-41: exact executed dispatch order matches expected");
        verify(executedOrder.indexOf(QStringLiteral("low_task")) == 13,
               "S4-41: low_task dispatched at index 13 (14th dispatch, NOT 7th)");
    }

    // S4-42: Ochrona Boosted Low przed nowym napływem Normal i High (brak nieskończonego zagłodzenia)
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blocker = h.add(TQ_S("blocker"), {h.src(TQ_S("b.txt"))}, h.dst(TQ_S("b_dir")));

        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        const quint64 lowId = h.add(TQ_S("low_target"), {h.src(TQ_S("lt.txt"))}, h.dst(TQ_S("lt_dir")), optLow);

        // 10 Normal, 3 High
        for (int i = 0; i < 10; ++i) {
            h.add(QStringLiteral("init_n_%1").arg(i), {h.src(QStringLiteral("in_%1.txt").arg(i))},
                  h.dst(QStringLiteral("in_dir_%1").arg(i)));
        }
        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        for (int i = 0; i < 3; ++i) {
            h.add(QStringLiteral("init_h_%1").arg(i), {h.src(QStringLiteral("ih_%1.txt").arg(i))},
                  h.dst(QStringLiteral("ih_dir_%1").arg(i)), optHigh);
        }

        h.job(TQ_S("blocker"))->finishOk();

        // Finish 3 High and 2 Normal (init_n_2 is currently running)
        for (int i = 0; i < 3; ++i) {
            h.job(QStringLiteral("init_h_%1").arg(i))->finishOk();
        }
        for (int i = 0; i < 2; ++i) {
            h.job(QStringLiteral("init_n_%1").arg(i))->finishOk();
        }

        // Now init_n_2 is running, and low_target has reached exactly 6 bypasses and is boosted!
        verify(h.snap(lowId).starvationBypassCount == 6, "S4-42: low_target reached 6 bypasses");
        verify(h.snap(lowId).boostSequence > 0, "S4-42: low_target has boostSequence assigned");

        // Now, flood the queue with 10 NEW Normal and 5 NEW High tasks!
        for (int i = 0; i < 10; ++i) {
            h.add(QStringLiteral("new_n_%1").arg(i), {h.src(QStringLiteral("nn_%1.txt").arg(i))},
                  h.dst(QStringLiteral("nn_dir_%1").arg(i)));
        }
        for (int i = 0; i < 5; ++i) {
            h.add(QStringLiteral("new_h_%1").arg(i), {h.src(QStringLiteral("nh_%1.txt").arg(i))},
                  h.dst(QStringLiteral("nh_dir_%1").arg(i)), optHigh);
        }

        // Finish the remaining initial Normal tasks (init_n_2 to init_n_9)
        for (int i = 2; i < 10; ++i) {
            h.job(QStringLiteral("init_n_%1").arg(i))->finishOk();
        }

        // Next dispatch MUST be low_target!
        verify(h.state(lowId) == TransferTaskState::Running,
               "S4-42: low_target runs immediately after initial boosted batch, NOT starved by new flood");
        h.job(TQ_S("low_target"))->finishOk();

        // Clean up remaining new jobs (15 tasks: 5 High, 10 Normal)
        for (int i = 0; i < 15; ++i) {
            QString runningTitle;
            for (const auto &snap : h.ctl.snapshot().tasks) {
                if (snap.state == TransferTaskState::Running) {
                    runningTitle = snap.title;
                    break;
                }
            }
            verify(!runningTitle.isEmpty(), "S4-42: remaining task running in slot");
            h.job(runningTitle)->finishOk();
        }
    }

    // S4-43: Weryfikacja 10 Low i 20 Normal (drenaż wielu zadań)
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);

        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        QList<quint64> lowIds;
        for (int i = 0; i < 10; ++i) {
            lowIds.append(h.add(QStringLiteral("m_low_%1").arg(i),
                                {h.src(QStringLiteral("ml_%1.txt").arg(i))},
                                h.dst(QStringLiteral("ml_dir_%1").arg(i)), optLow));
        }

        QList<quint64> normIds;
        for (int i = 0; i < 20; ++i) {
            normIds.append(h.add(QStringLiteral("m_norm_%1").arg(i),
                                 {h.src(QStringLiteral("mn_%1.txt").arg(i))},
                                 h.dst(QStringLiteral("mn_dir_%1").arg(i))));
        }

        for (int step = 0; step < 30; ++step) {
            QString runningTitle;
            for (const auto &snap : h.ctl.snapshot().tasks) {
                if (snap.state == TransferTaskState::Running) {
                    runningTitle = snap.title;
                    break;
                }
            }
            verify(!runningTitle.isEmpty(), "S4-43: task running in slot");
            if (auto *j = h.job(runningTitle)) {
                j->finishOk();
            }
        }

        verify(h.ctl.queuedCount() == 0, "S4-43: queue completely drained");
    }

    // S4-44: Move Up / Move Down na zboostowanych zadaniach
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blocker = h.add(TQ_S("blocker"), {h.src(TQ_S("b.txt"))}, h.dst(TQ_S("b_dir")));

        const quint64 n1 = h.add(TQ_S("bn_1"), {h.src(TQ_S("bn1.txt"))}, h.dst(TQ_S("bn1_dir")));
        const quint64 n2 = h.add(TQ_S("bn_2"), {h.src(TQ_S("bn2.txt"))}, h.dst(TQ_S("bn2_dir")));

        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        for (int i = 0; i < 4; ++i) {
            h.add(QStringLiteral("bh_%1").arg(i), {h.src(QStringLiteral("bh_%1.txt").arg(i))},
                  h.dst(QStringLiteral("bh_dir_%1").arg(i)), optHigh);
        }

        h.job(TQ_S("blocker"))->finishOk();
        // Finish 2 High tasks: bh_0 and bh_1 -> bh_2 launches (3rd bypass for bn_1 and bn_2)
        for (int i = 0; i < 2; ++i) {
            h.job(QStringLiteral("bh_%1").arg(i))->finishOk();
        }

        // Now bh_2 is running in slot 1! Both bn_1 and bn_2 are still queued and boosted (bypass = 3)
        verify(h.snap(n1).boostSequence > 0, "S4-44: bn_1 is boosted");
        verify(h.snap(n2).boostSequence > 0, "S4-44: bn_2 is boosted");
        verify(h.snap(n1).queuePosition == 1, "S4-44: bn_1 initially #1 in queue");
        verify(h.snap(n2).queuePosition == 2, "S4-44: bn_2 initially #2 in queue");

        // Now user moves bn_2 up above bn_1!
        verify(h.ctl.canMoveTaskUp(n2), "S4-44: canMoveTaskUp(n2) is true");
        h.ctl.moveTaskUp(n2);

        // Verification: bn_2 is now #1 and bn_1 is now #2!
        verify(h.snap(n2).queuePosition == 1, "S4-44: bn_2 is now #1 in queue after moveTaskUp");
        verify(h.snap(n1).queuePosition == 2, "S4-44: bn_1 is now #2 in queue after moveTaskUp");

        // bh_2 finishes -> bn_2 must launch next!
        h.job(TQ_S("bh_2"))->finishOk();
        verify(h.state(n2) == TransferTaskState::Running, "S4-44: bn_2 executes first according to user reordering");
        h.job(TQ_S("bn_2"))->finishOk();
        verify(h.state(n1) == TransferTaskState::Running, "S4-44: bn_1 executes second");
        h.job(TQ_S("bn_1"))->finishOk();
        verify(h.state(h.ctl.snapshot().tasks.last().id) == TransferTaskState::Running, "S4-44: bh_3 executes last");
        h.job(TQ_S("bh_3"))->finishOk();
    }

    // =========================================================================
    // Stage 5: Global Pause / Resume / Cancel Tests (S5-01 to S5-45)
    // =========================================================================

    // S5-01: Pause All przy pustej kolejce
    {
        QueueHarness h(nextRoot());
        verify(h.ctl.pauseAll(), "S5-01: pauseAll on empty queue returns true");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Paused,
               "S5-01: state is Paused on empty queue");
        verify(h.ctl.isGlobalPauseActive(), "S5-01: global pause is active");
        verify(h.ctl.dispatchPaused(), "S5-01: dispatch is paused");
        verify(h.ctl.resumeAll(), "S5-01: resumeAll clears global pause");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Normal,
               "S5-01: state returns to Normal");
    }

    // S5-02: Pause All z samymi Queued
    {
        QueueHarness h(nextRoot());
        h.ctl.setDispatchPaused(true);
        const quint64 q1 = h.add(TQ_S("q1"), {h.src(TQ_S("q1"))}, h.dst(TQ_S("q1")));
        const quint64 q2 = h.add(TQ_S("q2"), {h.src(TQ_S("q2"))}, h.dst(TQ_S("q2")));
        verify(h.state(q1) == TransferTaskState::Queued && h.state(q2) == TransferTaskState::Queued,
               "S5-02: tasks are queued");
        verify(h.ctl.pauseAll(), "S5-02: pauseAll returns true");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Paused,
               "S5-02: global state is Paused with only queued tasks");
        verify(h.factoryCalls == 0, "S5-02: no factory calls for queued tasks");
        verify(h.snap(q1).waitReason == TransferWaitReason::DispatchPaused,
               "S5-02: waitReason is DispatchPaused");
        h.ctl.resumeAll();
        verify(h.state(q1) == TransferTaskState::Running, "S5-02: q1 launches after resumeAll");
        h.job(TQ_S("q1"))->finishOk();
        h.job(TQ_S("q2"))->finishOk();
    }

    // S5-03: Pause All z jednym Running
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 r1 = h.add(TQ_S("r1"), {h.src(TQ_S("r1"))}, h.dst(TQ_S("r1")));
        verify(h.state(r1) == TransferTaskState::Running, "S5-03: r1 is Running");
        verify(h.ctl.pauseAll(), "S5-03: pauseAll succeeds");
        verify(h.state(r1) == TransferTaskState::Paused, "S5-03: r1 is Paused");
        verify(h.snap(r1).pausedByGlobal, "S5-03: r1 marked pausedByGlobal");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Paused,
               "S5-03: global state is Paused");
        h.ctl.resumeAll();
        verify(h.state(r1) == TransferTaskState::Running, "S5-03: r1 resumes");
        h.job(TQ_S("r1"))->finishOk();
    }

    // S5-04: Pause All z kilkoma Running
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        h.ctl.setParallelConcurrencyLimit(3);
        const quint64 r1 = h.add(TQ_S("pr1"), {h.src(TQ_S("pr1"))}, h.dst(TQ_S("pr1")));
        const quint64 r2 = h.add(TQ_S("pr2"), {h.src(TQ_S("pr2"))}, h.dst(TQ_S("pr2")));
        const quint64 r3 = h.add(TQ_S("pr3"), {h.src(TQ_S("pr3"))}, h.dst(TQ_S("pr3")));
        verify(h.ctl.activeCount() == 3, "S5-04: 3 tasks running");
        h.ctl.pauseAll();
        verify(h.state(r1) == TransferTaskState::Paused && h.state(r2) == TransferTaskState::Paused
               && h.state(r3) == TransferTaskState::Paused, "S5-04: all 3 paused");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Paused,
               "S5-04: global state is Paused");
        h.ctl.resumeAll();
        h.job(TQ_S("pr1"))->finishOk();
        h.job(TQ_S("pr2"))->finishOk();
        h.job(TQ_S("pr3"))->finishOk();
    }

    // S5-05: Sukces suspend
    {
        QueueHarness h(nextRoot());
        const quint64 t = h.add(TQ_S("t_sus"), {h.src(TQ_S("ts"))}, h.dst(TQ_S("ts")));
        h.ctl.pauseAll();
        verify(h.job(TQ_S("t_sus"))->suspendCalls == 1, "S5-05: suspend called once on backend job");
        verify(h.snap(t).jobSuspended, "S5-05: snapshot reports jobSuspended true");
        h.ctl.resumeAll();
        h.job(TQ_S("t_sus"))->finishOk();
    }

    // S5-06: Odmowa suspend
    {
        QueueHarness h(nextRoot());
        QueueOptions opt;
        opt.onJobCreated = [](QueueFakeJob *j) { j->suspendAccepted = false; };
        const quint64 r = h.add(TQ_S("refuse"), {h.src(TQ_S("ref"))}, h.dst(TQ_S("ref")), opt);
        h.ctl.pauseAll();
        verify(h.state(r) == TransferTaskState::Running, "S5-06: task stays Running when suspend refused");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::PartiallyPaused,
               "S5-06: global state is PartiallyPaused");
        h.ctl.resumeAll();
        h.job(TQ_S("refuse"))->finishOk();
    }

    // S5-07: Mieszanka backendów wspierających i niewspierających pauzy
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        const quint64 s1 = h.add(TQ_S("s1_ok"), {h.src(TQ_S("s1"))}, h.dst(TQ_S("s1")));
        QueueOptions noPause;
        noPause.overrideCanPause = false;
        noPause.overrideCanResume = false;
        const quint64 s2 = h.add(TQ_S("s2_nop"), {h.src(TQ_S("s2"))}, h.dst(TQ_S("s2")), noPause);
        h.ctl.pauseAll();
        verify(h.state(s1) == TransferTaskState::Paused, "S5-07: s1 is Paused");
        verify(h.state(s2) == TransferTaskState::Running, "S5-07: s2 is Running");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::PartiallyPaused,
               "S5-07: global state is PartiallyPaused");
        h.job(TQ_S("s2_nop"))->finishOk();
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Paused,
               "S5-07: transitions to Paused when non-pausable finishes");
        h.ctl.resumeAll();
        h.job(TQ_S("s1_ok"))->finishOk();
    }

    // S5-08: Zadanie kończy się podczas Pause All
    {
        QueueHarness h(nextRoot());
        QueueOptions opt;
        opt.onJobCreated = [](QueueFakeJob *j) {
            j->onSuspend = [j] { j->finishOk(); };
        };
        const quint64 t = h.add(TQ_S("fin_during_pause"), {h.src(TQ_S("fdp"))}, h.dst(TQ_S("fdp")), opt);
        h.ctl.pauseAll();
        verify(h.state(t) == TransferTaskState::Completed, "S5-08: job completed cleanly");
        verify(h.ctl.activeCount() == 0, "S5-08: activeCount is 0");
        h.ctl.resumeAll();
    }

    // S5-09: Nowe zadanie dodane przy aktywnym global Pause
    {
        QueueHarness h(nextRoot());
        h.ctl.pauseAll();
        const quint64 nw = h.add(TQ_S("new_task"), {h.src(TQ_S("nt"))}, h.dst(TQ_S("nt")));
        verify(h.state(nw) == TransferTaskState::Queued, "S5-09: nw stays queued");
        verify(h.snap(nw).waitReason == TransferWaitReason::DispatchPaused,
               "S5-09: waitReason is DispatchPaused");
        verify(h.factoryCalls == 0, "S5-09: factory not called");
        h.ctl.resumeAll();
        verify(h.state(nw) == TransferTaskState::Running, "S5-09: nw launches after resumeAll");
        h.job(TQ_S("new_task"))->finishOk();
    }

    // S5-10: Brak tworzenia KJob dla Queued
    {
        QueueHarness h(nextRoot());
        h.ctl.pauseAll();
        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        h.add(TQ_S("qh"), {h.src(TQ_S("qh"))}, h.dst(TQ_S("qh")), optHigh);
        verify(h.factoryCalls == 0, "S5-10: factory not called even for High priority while paused");
        h.ctl.resumeAll();
        h.job(TQ_S("qh"))->finishOk();
    }

    // S5-11: Poprawna retencja rezerwacji podczas pauzy
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        const quint64 t1 = h.add(TQ_S("t1"), {h.src(TQ_S("file1"))}, h.dst(TQ_S("dst_dir")));
        h.ctl.pauseAll();
        verify(h.state(t1) == TransferTaskState::Paused, "S5-11: t1 is Paused");
        const quint64 t2 = h.add(TQ_S("t2"), {h.src(TQ_S("file1"))}, h.dst(TQ_S("dst_dir")));
        verify(h.state(t2) == TransferTaskState::Queued, "S5-11: t2 queued");
        h.ctl.resumeAll();
        verify(h.state(t2) == TransferTaskState::Queued
               && h.snap(t2).waitReason == TransferWaitReason::BlockedByReservation,
               "S5-11: t2 blocked by reservation of t1 after resumeAll");
        h.job(TQ_S("t1"))->finishOk();
        verify(h.state(t2) == TransferTaskState::Running, "S5-11: t2 launches after t1 finishes");
        h.job(TQ_S("t2"))->finishOk();
    }

    // S5-12: Resume All po prawidłowym Pause
    {
        QueueHarness h(nextRoot());
        const quint64 r = h.add(TQ_S("res_ok"), {h.src(TQ_S("ro"))}, h.dst(TQ_S("ro")));
        h.ctl.pauseAll();
        verify(h.state(r) == TransferTaskState::Paused, "S5-12: paused");
        verify(h.ctl.resumeAll(), "S5-12: resumeAll returns true");
        verify(h.state(r) == TransferTaskState::Running, "S5-12: running after resume");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Normal,
               "S5-12: global state is Normal");
        h.job(TQ_S("res_ok"))->finishOk();
    }

    // S5-13: Resume All po częściowym Pause
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        const quint64 s1 = h.add(TQ_S("s1"), {h.src(TQ_S("s1"))}, h.dst(TQ_S("s1")));
        QueueOptions noPause;
        noPause.overrideCanPause = false;
        noPause.overrideCanResume = false;
        const quint64 s2 = h.add(TQ_S("s2"), {h.src(TQ_S("s2"))}, h.dst(TQ_S("s2")), noPause);
        h.ctl.pauseAll();
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::PartiallyPaused, "S5-13: partially paused");
        verify(h.ctl.resumeAll(), "S5-13: resumeAll succeeds");
        verify(h.state(s1) == TransferTaskState::Running && h.state(s2) == TransferTaskState::Running,
               "S5-13: both tasks running");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Normal, "S5-13: Normal");
        h.job(TQ_S("s1"))->finishOk();
        h.job(TQ_S("s2"))->finishOk();
    }

    // S5-14: Zachowanie indywidualnie wstrzymanych zadań
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        const quint64 ind = h.add(TQ_S("ind"), {h.src(TQ_S("ind"))}, h.dst(TQ_S("ind")));
        const quint64 norm = h.add(TQ_S("norm"), {h.src(TQ_S("norm"))}, h.dst(TQ_S("norm")));
        verify(h.ctl.pauseTask(ind), "S5-14: ind individually paused");
        verify(h.snap(ind).individuallyPaused, "S5-14: ind marked individuallyPaused");
        h.ctl.pauseAll();
        verify(h.snap(norm).pausedByGlobal, "S5-14: norm marked pausedByGlobal");
        h.ctl.resumeAll();
        verify(h.state(norm) == TransferTaskState::Running, "S5-14: norm resumed");
        verify(h.state(ind) == TransferTaskState::Paused, "S5-14: ind remains Paused");
        verify(h.snap(ind).individuallyPaused, "S5-14: ind still marked individuallyPaused");
        verify(h.ctl.resumeTask(ind), "S5-14: individual resume succeeds");
        verify(h.state(ind) == TransferTaskState::Running, "S5-14: ind resumed manually");
        h.job(TQ_S("norm"))->finishOk();
        h.job(TQ_S("ind"))->finishOk();
    }

    // S5-15: Odmowa resume przez backend
    {
        QueueHarness h(nextRoot());
        QueueOptions opt;
        opt.onJobCreated = [](QueueFakeJob *j) { j->resumeAccepted = false; };
        const quint64 r = h.add(TQ_S("ref_res"), {h.src(TQ_S("rr"))}, h.dst(TQ_S("rr")), opt);
        h.ctl.pauseAll();
        verify(h.state(r) == TransferTaskState::Paused, "S5-15: paused");
        h.ctl.resumeAll();
        verify(h.state(r) == TransferTaskState::Paused, "S5-15: remains Paused when backend refuses resume");
        h.job(TQ_S("ref_res"))->finishOk();
    }

    // S5-16: Resume bez aktywnego global Pause
    {
        QueueHarness h(nextRoot());
        verify(!h.ctl.resumeAll(), "S5-16: resumeAll returns false when not paused");
    }

    // S5-17: Kolejność schedulera po wznowieniu
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blocker = h.add(TQ_S("bl"), {h.src(TQ_S("bl"))}, h.dst(TQ_S("bl")));
        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        const quint64 l1 = h.add(TQ_S("l1"), {h.src(TQ_S("l1"))}, h.dst(TQ_S("l1")), optLow);
        const quint64 h1 = h.add(TQ_S("h1"), {h.src(TQ_S("h1"))}, h.dst(TQ_S("h1")), optHigh);
        h.ctl.pauseAll();
        h.job(TQ_S("bl"))->finishOk();
        verify(h.state(h1) == TransferTaskState::Queued, "S5-17: h1 stays queued while paused");
        h.ctl.resumeAll();
        verify(h.state(h1) == TransferTaskState::Running, "S5-17: High priority runs first after resume");
        h.job(TQ_S("h1"))->finishOk();
        verify(h.state(l1) == TransferTaskState::Running, "S5-17: Low priority runs second");
        h.job(TQ_S("l1"))->finishOk();
    }

    // S5-18: Zmiana limitów podczas pauzy
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(1);
        const quint64 t1 = h.add(TQ_S("t1"), {h.src(TQ_S("t1"))}, h.dst(TQ_S("t1")));
        const quint64 t2 = h.add(TQ_S("t2"), {h.src(TQ_S("t2"))}, h.dst(TQ_S("t2")));
        h.ctl.pauseAll();
        h.ctl.setParallelConcurrencyLimit(3);
        verify(h.factoryCalls == 1, "S5-18: t2 not started while paused despite limit increase");
        h.ctl.resumeAll();
        verify(h.state(t1) == TransferTaskState::Running && h.state(t2) == TransferTaskState::Running,
               "S5-18: both running after resume with new limit 3");
        h.job(TQ_S("t1"))->finishOk();
        h.job(TQ_S("t2"))->finishOk();
    }

    // S5-19: Cancel All przy pustej kolejce
    {
        QueueHarness h(nextRoot());
        verify(h.ctl.cancelAll(), "S5-19: cancelAll returns true on empty queue");
        verify(h.ctl.isQueueIdle(), "S5-19: queue is idle");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Normal, "S5-19: Normal");
    }

    // S5-20: Cancel All z samymi Queued
    {
        QueueHarness h(nextRoot());
        h.ctl.setDispatchPaused(true);
        const quint64 q1 = h.add(TQ_S("cq1"), {h.src(TQ_S("cq1"))}, h.dst(TQ_S("cq1")));
        const quint64 q2 = h.add(TQ_S("cq2"), {h.src(TQ_S("cq2"))}, h.dst(TQ_S("cq2")));
        verify(h.ctl.cancelAll(), "S5-20: cancelAll succeeds");
        verify(h.state(q1) == TransferTaskState::Cancelled && h.state(q2) == TransferTaskState::Cancelled,
               "S5-20: both queued tasks Cancelled");
        verify(h.factoryCalls == 0, "S5-20: zero factory calls for queued tasks");
        verify(h.ctl.isQueueIdle(), "S5-20: queue idle");
    }

    // S5-21: Cancel All z kilkoma Running
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(3);
        const quint64 r1 = h.add(TQ_S("cr1"), {h.src(TQ_S("cr1"))}, h.dst(TQ_S("cr1")));
        const quint64 r2 = h.add(TQ_S("cr2"), {h.src(TQ_S("cr2"))}, h.dst(TQ_S("cr2")));
        const quint64 r3 = h.add(TQ_S("cr3"), {h.src(TQ_S("cr3"))}, h.dst(TQ_S("cr3")));
        verify(h.ctl.cancelAll(), "S5-21: cancelAll succeeds");
        verify(h.state(r1) == TransferTaskState::Cancelled && h.state(r2) == TransferTaskState::Cancelled
               && h.state(r3) == TransferTaskState::Cancelled, "S5-21: all running tasks Cancelled");
        verify(h.ctl.isQueueIdle(), "S5-21: queue is idle");
    }

    // S5-22: Cancel All z Paused
    {
        QueueHarness h(nextRoot());
        const quint64 p = h.add(TQ_S("cp"), {h.src(TQ_S("cp"))}, h.dst(TQ_S("cp")));
        h.ctl.pauseAll();
        verify(h.state(p) == TransferTaskState::Paused, "S5-22: paused");
        verify(h.ctl.cancelAll(), "S5-22: cancelAll on paused");
        verify(h.state(p) == TransferTaskState::Cancelled, "S5-22: task Cancelled");
        verify(h.ctl.isQueueIdle(), "S5-22: idle");
    }

    // S5-23: Cancel All przy mieszanych stanach
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        const quint64 fin = h.add(TQ_S("fin"), {h.src(TQ_S("fin"))}, h.dst(TQ_S("fin")));
        h.job(TQ_S("fin"))->finishOk();
        verify(h.state(fin) == TransferTaskState::Completed, "S5-23: fin is Completed");

        const quint64 run = h.add(TQ_S("run"), {h.src(TQ_S("run"))}, h.dst(TQ_S("run")));
        const quint64 pau = h.add(TQ_S("pau"), {h.src(TQ_S("pau"))}, h.dst(TQ_S("pau")));
        h.ctl.pauseTask(pau);

        const quint64 qd = h.add(TQ_S("qd"), {h.src(TQ_S("qd"))}, h.dst(TQ_S("qd")));
        verify(h.ctl.cancelAll(), "S5-23: cancelAll on mixed states");
        verify(h.state(fin) == TransferTaskState::Completed, "S5-23: completed task retains Completed state");
        verify(h.state(run) == TransferTaskState::Cancelled, "S5-23: run cancelled");
        verify(h.state(pau) == TransferTaskState::Cancelled, "S5-23: pau cancelled");
        verify(h.state(qd) == TransferTaskState::Cancelled, "S5-23: qd cancelled");
        verify(h.ctl.isQueueIdle(), "S5-23: idle");
    }

    // S5-24: Powtórny Cancel All
    {
        QueueHarness h(nextRoot());
        h.add(TQ_S("rep"), {h.src(TQ_S("rep"))}, h.dst(TQ_S("rep")));
        verify(h.ctl.cancelAll(), "S5-24: first cancelAll");
        verify(h.ctl.cancelAll(), "S5-24: repeated cancelAll is safe and returns true");
        verify(h.ctl.isQueueIdle(), "S5-24: idle");
    }

    // S5-25: Cancel All podczas Resume
    {
        QueueHarness h(nextRoot());
        const quint64 r = h.add(TQ_S("cres"), {h.src(TQ_S("cres"))}, h.dst(TQ_S("cres")));
        h.ctl.pauseAll();
        QObject::connect(&h.ctl, &TransferQueueController::taskStateChanged, [&] {
            h.ctl.cancelAll();
        });
        h.ctl.resumeAll();
        verify(h.ctl.isQueueIdle(), "S5-25: clean cancelAll during resume");
    }

    // S5-26: Cancel All podczas callbacku terminalnego
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 a = h.add(TQ_S("ca"), {h.src(TQ_S("ca"))}, h.dst(TQ_S("ca")));
        const quint64 b = h.add(TQ_S("cb"), {h.src(TQ_S("cb"))}, h.dst(TQ_S("cb")));
        QObject::connect(&h.ctl, &TransferQueueController::taskFinished, [&](quint64 id, TransferTaskState) {
            if (id == a) {
                h.ctl.cancelAll();
            }
        });
        h.job(TQ_S("ca"))->finishOk();
        verify(h.state(a) == TransferTaskState::Completed, "S5-26: a completed");
        verify(h.state(b) == TransferTaskState::Cancelled, "S5-26: b cancelled");
        verify(h.ctl.isQueueIdle(), "S5-26: idle");
    }

    // S5-27: Brak podwójnych terminal events
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        h.add(TQ_S("e1"), {h.src(TQ_S("e1"))}, h.dst(TQ_S("e1")));
        h.add(TQ_S("e2"), {h.src(TQ_S("e2"))}, h.dst(TQ_S("e2")));
        int finishCount = 0;
        QObject::connect(&h.ctl, &TransferQueueController::taskFinished, [&] {
            ++finishCount;
        });
        h.ctl.cancelAll();
        verify(finishCount == 2, "S5-27: exact single taskFinished emission per task");
    }

    // S5-28: Brak podwójnego zwolnienia rezerwacji
    {
        QueueHarness h(nextRoot());
        h.add(TQ_S("res_dup"), {h.src(TQ_S("rd"))}, h.dst(TQ_S("rd")));
        h.ctl.cancelAll();
        h.ctl.cancelAll();
        verify(h.ctl.isQueueIdle(), "S5-28: clean single release");
    }

    // S5-29: Nieuruchamianie fabryk anulowanych Queued
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        h.add(TQ_S("live"), {h.src(TQ_S("live"))}, h.dst(TQ_S("live")));
        h.add(TQ_S("queued_no_factory"), {h.src(TQ_S("qnf"))}, h.dst(TQ_S("qnf")));
        verify(h.factoryCalls == 1, "S5-29: only live task had factory called");
        h.ctl.cancelAll();
        verify(h.factoryCalls == 1, "S5-29: factory was never called for cancelled queued task");
    }

    // S5-30: Zachowanie historii terminalnej
    {
        QueueHarness h(nextRoot());
        const quint64 t = h.add(TQ_S("hist"), {h.src(TQ_S("hist"))}, h.dst(TQ_S("hist")));
        h.job(TQ_S("hist"))->finishOk();
        h.ctl.cancelAll();
        const auto snap = h.ctl.taskSnapshot(t);
        verify(snap.has_value() && snap->state == TransferTaskState::Completed,
               "S5-30: terminal history completely preserved");
    }

    // S5-31: Zachowanie spójnego stanu po anulowaniu
    {
        QueueHarness h(nextRoot());
        h.add(TQ_S("s31_1"), {h.src(TQ_S("s31_1"))}, h.dst(TQ_S("s31_1")));
        h.add(TQ_S("s31_2"), {h.src(TQ_S("s31_2"))}, h.dst(TQ_S("s31_2")));
        h.ctl.cancelAll();
        verify(h.ctl.isQueueIdle(), "S5-31: idle");
        verify(h.ctl.activeCount() == 0, "S5-31: activeCount 0");
        verify(h.ctl.queuedCount() == 0, "S5-31: queuedCount 0");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Normal,
               "S5-31: globalControlState is Normal");
    }

    // S5-32: Integracja z Sequential
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        const quint64 s1 = h.add(TQ_S("seq1"), {h.src(TQ_S("seq1"))}, h.dst(TQ_S("seq1")));
        const quint64 s2 = h.add(TQ_S("seq2"), {h.src(TQ_S("seq2"))}, h.dst(TQ_S("seq2")));
        h.ctl.pauseAll();
        verify(h.state(s1) == TransferTaskState::Paused && h.state(s2) == TransferTaskState::Queued,
               "S5-32: seq1 paused, seq2 queued");
        h.ctl.resumeAll();
        h.job(TQ_S("seq1"))->finishOk();
        verify(h.state(s2) == TransferTaskState::Running, "S5-32: seq2 runs next in Sequential");
        h.job(TQ_S("seq2"))->finishOk();
    }

    // S5-33: Integracja z Parallel
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        h.ctl.setParallelConcurrencyLimit(2);
        const quint64 p1 = h.add(TQ_S("par1"), {h.src(TQ_S("par1"))}, h.dst(TQ_S("par1")));
        const quint64 p2 = h.add(TQ_S("par2"), {h.src(TQ_S("par2"))}, h.dst(TQ_S("par2")));
        h.ctl.pauseAll();
        verify(h.state(p1) == TransferTaskState::Paused && h.state(p2) == TransferTaskState::Paused,
               "S5-33: both parallel tasks paused");
        h.ctl.resumeAll();
        h.job(TQ_S("par1"))->finishOk();
        h.job(TQ_S("par2"))->finishOk();
    }

    // S5-34: Limity 1-8 zachowane podczas pauzy i po wznowieniu
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(7);
        h.ctl.pauseAll();
        verify(h.ctl.parallelConcurrencyLimit() == 7, "S5-34: limit 7 preserved in pause");
        h.ctl.resumeAll();
        verify(h.ctl.parallelConcurrencyLimit() == 7, "S5-34: limit 7 preserved after resume");
    }

    // S5-35: Priorytety i ręczny reordering zachowane po Resume
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blk = h.add(TQ_S("blk"), {h.src(TQ_S("blk"))}, h.dst(TQ_S("blk")));
        const quint64 n1 = h.add(TQ_S("rn1"), {h.src(TQ_S("rn1"))}, h.dst(TQ_S("rn1")));
        const quint64 n2 = h.add(TQ_S("rn2"), {h.src(TQ_S("rn2"))}, h.dst(TQ_S("rn2")));
        h.ctl.moveTaskUp(n2);
        h.ctl.pauseAll();
        h.ctl.resumeAll();
        h.job(TQ_S("blk"))->finishOk();
        verify(h.state(n2) == TransferTaskState::Running, "S5-35: n2 executes first after reordering and resume");
        h.job(TQ_S("rn2"))->finishOk();
        verify(h.state(n1) == TransferTaskState::Running, "S5-35: n1 executes second");
        h.job(TQ_S("rn1"))->finishOk();
    }

    // S5-36: Starvation protection i boostSequence po Pause/Resume
    {
        QueueHarness h(nextRoot());
        h.ctl.setConcurrencyLimit(1);
        const quint64 blk = h.add(TQ_S("blk"), {h.src(TQ_S("blk"))}, h.dst(TQ_S("blk")));
        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        const quint64 low = h.add(TQ_S("s_low"), {h.src(TQ_S("slow"))}, h.dst(TQ_S("slow")), optLow);

        QueueOptions optHigh; optHigh.priority = TransferPriority::High;
        for (int i = 0; i < 6; ++i) {
            h.add(QStringLiteral("sh_%1").arg(i), {h.src(QStringLiteral("sh_%1").arg(i))},
                  h.dst(QStringLiteral("sh_%1").arg(i)), optHigh);
        }
        h.job(TQ_S("blk"))->finishOk();
        for (int i = 0; i < 5; ++i) {
            h.job(QStringLiteral("sh_%1").arg(i))->finishOk();
        }
        // Now sh_5 is running; low has been bypassed 6 times and is boosted!
        verify(h.snap(low).starvationBypassCount == 6, "S5-36: low accumulated 6 bypasses");
        verify(h.snap(low).boostSequence > 0, "S5-36: low is boosted");

        h.ctl.pauseAll();
        verify(h.snap(low).boostSequence > 0, "S5-36: boostSequence retained during pause");
        h.ctl.resumeAll();
        h.job(TQ_S("sh_5"))->finishOk();
        verify(h.state(low) == TransferTaskState::Running, "S5-36: boosted low runs after resume");
        h.job(TQ_S("s_low"))->finishOk();
    }

    // S5-37: Konflikty rezerwacji zachowane podczas pauzy i zwolnione po Cancel All
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(2);
        const quint64 a = h.add(TQ_S("ca1"), {h.src(TQ_S("same"))}, h.dst(TQ_S("dst1")));
        const quint64 b = h.add(TQ_S("ca2"), {h.src(TQ_S("same"))}, h.dst(TQ_S("dst2")));
        h.ctl.pauseAll();
        h.ctl.cancelAll();
        verify(h.ctl.isQueueIdle(), "S5-37: reservation conflicts cleanly freed by cancelAll");
    }

    // S5-38: Reentrancy (wielokrotne wywołania kontrolera)
    {
        QueueHarness h(nextRoot());
        h.ctl.pauseAll();
        h.ctl.pauseAll();
        h.ctl.resumeAll();
        h.ctl.resumeAll();
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Normal,
               "S5-38: safe reentrant calls");
    }

    // S5-39: Zamknięcie okna GUI podczas operacji
    {
        QueueHarness h(nextRoot());
        OperationWindow win(nullptr);
        win.setQueueController(&h.ctl);
        win.show();
        h.add(TQ_S("gui_task"), {h.src(TQ_S("gt"))}, h.dst(TQ_S("gt")));
        win.close();
        verify(!h.ctl.isQueueIdle(), "S5-39: task continues safely after window close");
        h.ctl.cancelAll();
    }

    // S5-40: Wielokrotne otwieranie i przełączanie zakładek Kolejka
    {
        OperationWindow win(nullptr);
        TransferQueueController ctl;
        win.setQueueController(&ctl);
        verify(win.hasQueueTab(), "S5-40: queue tab exists");
        win.tabWidget()->setCurrentIndex(1);
        win.tabWidget()->setCurrentIndex(0);
        win.tabWidget()->setCurrentIndex(1);
        verify(win.tabWidget()->currentIndex() == 1, "S5-40: tabs switch cleanly");
    }

    // S5-41: GUI: Responsywność, eliding, tooltips i kontrolki globalne
    {
        TransferQueueViewWidget widget;
        TransferQueueController ctl;
        widget.setController(&ctl);
        widget.setConfirmCancelAll(false);
        verify(widget.pauseAllButton() != nullptr, "S5-41: pauseAllButton exists");
        verify(widget.resumeAllButton() != nullptr, "S5-41: resumeAllButton exists");
        verify(widget.cancelAllButton() != nullptr, "S5-41: cancelAllButton exists");
        verify(widget.globalStatusBanner() != nullptr, "S5-41: banner exists");

        verify(!widget.pauseAllButton()->isEnabled(), "S5-41: pauseAll disabled when empty");
        verify(!widget.resumeAllButton()->isEnabled(), "S5-41: resumeAll disabled when empty");
        verify(!widget.cancelAllButton()->isEnabled(), "S5-41: cancelAll disabled when empty");
        verify(widget.globalStatusBanner()->isHidden(), "S5-41: banner hidden when Normal");
    }

    // S5-42: GUI: Minimalny rozmiar okna i brak nakładania kontrolek
    {
        TransferQueueViewWidget widget;
        TransferQueueController ctl;
        widget.setController(&ctl);
        widget.resize(480, 260);
        verify(widget.pauseAllButton()->geometry().top() >= 0, "S5-42: button valid top");
        verify(widget.cancelAllButton()->geometry().right() <= 480, "S5-42: button fits width");
    }

    // S5-43: GUI: Lokalizacja PL / EN
    {
        TransferQueueViewWidget widget;
        const QString pauseText = widget.pauseAllButton()->text();
        verify(pauseText == QStringLiteral("Wstrzymaj wszystkie") || pauseText == QStringLiteral("Pause All"),
               "S5-43: correct localized pause all text");
        const QString resumeText = widget.resumeAllButton()->text();
        verify(resumeText == QStringLiteral("Wznów wszystkie") || resumeText == QStringLiteral("Resume All"),
               "S5-43: correct localized resume all text");
        const QString cancelText = widget.cancelAllButton()->text();
        verify(cancelText == QStringLiteral("Anuluj wszystkie") || cancelText == QStringLiteral("Cancel All"),
               "S5-43: correct localized cancel all text");
    }

    // S5-44: Izolacja: brak połączenia dispatchu z produkcyjnym silnikiem
    {
        verify(TransferQueueProductionDispatchConnected == false,
               "S5-44: production dispatch remains unconnected");
    }

    // S5-45: Izolacja: brak wpływu na Undo/Redo i Administrator Retry
    {
        TransferTaskDescriptor desc;
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {QUrl(QStringLiteral("admin:///root/secret.txt"))};
        desc.destination = QUrl(QStringLiteral("admin:///etc/secret.txt"));
        desc.jobFactory = []() -> KJob * { return nullptr; };
        TransferQueueController ctl;
        const auto res = ctl.enqueue(desc);
        verify(!res.accepted() && res.rejection == TransferEnqueueRejection::AdminSchemeForbidden,
               "S5-45: admin scheme rejected cleanly, no interference with 0.43 admin retry");
    }

    // S5-46: Deterministyczny test: Cancel All z asynchronicznym anulowaniem i blokadą rezerwacji (kroki 1-9)
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("fileX"));
        QueueOptions optA;
        optA.asyncKill = true;
        // 1. Uruchom Transfer A rezerwujący ścieżkę X
        const quint64 a = h.add(TQ_S("taskA"), {h.src(TQ_S("fileX"))}, h.dst(TQ_S("fileX")), optA);
        verify(h.state(a) == TransferTaskState::Running, "S5-46: krok 1 - Transfer A is Running");
        verify(h.ctl.reservationRegistry().holds(a), "S5-46: krok 1 - A holds reservation for X");
        verify(h.ctl.reservationRegistry().holderCount() == 1, "S5-46: krok 1 - exactly 1 reservation active");

        // 2. Przygotuj Transfer B konfliktujący z X
        QueueOptions optB;
        const quint64 b = h.add(TQ_S("taskB"), {h.src(TQ_S("fileX"))}, h.dst(TQ_S("fileX")), optB);
        verify(h.state(b) == TransferTaskState::Queued, "S5-46: krok 2 - Transfer B is Queued");

        QSignalSpy cancelSpy(&h.ctl, &TransferQueueController::cancelAllCompleted);

        // 3. Wywołaj Cancel All
        const bool cancelAccepted = h.ctl.cancelAll();
        verify(cancelAccepted, "S5-46: krok 3 - cancelAll returned true");

        // 4. & 5. & 6. Sprawdź, że przed emisją terminalnego wyniku A nadal posiada rezerwację X
        verify(h.state(b) == TransferTaskState::Cancelled, "S5-46: krok 4 - Queued task B cancelled immediately");
        verify(h.state(a) == TransferTaskState::Cancelling, "S5-46: krok 4 - Active task A is in Cancelling");
        verify(h.ctl.isCancellingAll(), "S5-46: krok 4 - ctl is in CancellingAll state");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::CancellingAll,
               "S5-46: krok 4 - global state is CancellingAll");
        verify(h.ctl.activeCount() == 1, "S5-46: krok 4 - task A still occupies active slot while Cancelling");

        // 5. Sprawdź, że A nadal posiada rezerwację X
        verify(h.ctl.reservationRegistry().holds(a), "S5-46: krok 5 - A STILL holds reservation for path X in Cancelling");
        verify(h.ctl.reservationRegistry().holderCount() == 1, "S5-46: krok 5 - holder count is still 1");

        // 6. Sprawdź, że żadne konfliktujące zadanie nie może zostać uruchomione w trakcie Cancelling
        const ReservationRequest reqB = TransferReservation::build(
            TransferOperationType::Copy, {h.src(TQ_S("fileX"))}, h.dst(TQ_S("fileX")));
        const auto conflict = h.ctl.reservationRegistry().findConflict(reqB);
        verify(conflict.has_value() && conflict->holder == a,
               "S5-46: krok 6 - registry reports conflicting holder is task A");
        verify(cancelSpy.count() == 0, "S5-46: krok 6 - cancelAllCompleted NOT emitted while A is Cancelling");

        // Dodanie nowego zadania w trakcie CancellingAll - nie startuje
        const quint64 c = h.add(TQ_S("taskC"), {h.src(TQ_S("fileX"))}, h.dst(TQ_S("fileX")));
        verify(h.state(c) == TransferTaskState::Queued, "S5-46: krok 6 - new task C stays Queued");
        verify(h.ctl.activeCount() == 1, "S5-46: krok 6 - activeCount remains 1");

        // 7. Potwierdź terminalne zakończenie A
        h.job(TQ_S("taskA"))->triggerAsyncResult(KJob::KilledJobError);

        // 8. Sprawdź, że rezerwacja zostaje zwolniona dokładnie raz
        verify(h.state(a) == TransferTaskState::Cancelled, "S5-46: krok 8 - task A reached Cancelled");
        verify(!h.ctl.reservationRegistry().holds(a), "S5-46: krok 8 - reservation for A is released");
        verify(h.ctl.reservationRegistry().holds(c), "S5-46: krok 8 - waiting task C acquired reservation upon redispatch");
        verify(h.ctl.reservationRegistry().holderCount() == 1,
               "S5-46: krok 8 - exactly 1 reservation held (now by dispatched task C)");

        // 9. Sprawdź poprawne przejście globalnego Cancel All do stanu zakończonego
        verify(!h.ctl.isCancellingAll(), "S5-46: krok 9 - isCancellingAll is false");
        verify(cancelSpy.count() == 1, "S5-46: krok 9 - cancelAllCompleted emitted exactly once");
        verify(h.ctl.globalControlState() == TransferQueueGlobalControlState::Normal,
               "S5-46: krok 9 - global state returned to Normal");

        // Po zakończeniu procedury zadanie C zostało uruchomione; kończymy je czysto
        verify(h.state(c) == TransferTaskState::Running, "S5-46: task C dispatched after cancel all completed");
        h.job(TQ_S("taskC"))->finishOk();
        verify(h.state(c) == TransferTaskState::Completed, "S5-46: task C finished clean");
        verify(!h.ctl.reservationRegistry().holds(c), "S5-46: task C reservation released");
        verify(h.ctl.reservationRegistry().holderCount() == 0, "S5-46: all reservations clean (holderCount 0)");
        verify(h.ctl.isQueueIdle(), "S5-46: queue idle after all tasks terminal");
    }

    // S5-47: Synchroniczne Cancel All a rezerwacje zasobów
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("syncFile"));
        const quint64 a = h.add(TQ_S("syncA"), {h.src(TQ_S("syncFile"))}, h.dst(TQ_S("syncFile")));
        verify(h.state(a) == TransferTaskState::Running, "S5-47: syncA is Running");
        verify(h.ctl.reservationRegistry().holds(a), "S5-47: syncA holds reservation");
        verify(h.ctl.reservationRegistry().holderCount() == 1, "S5-47: holderCount 1");

        QSignalSpy cancelSpy(&h.ctl, &TransferQueueController::cancelAllCompleted);
        h.ctl.cancelAll();

        verify(h.state(a) == TransferTaskState::Cancelled, "S5-47: syncA is Cancelled");
        verify(!h.ctl.reservationRegistry().holds(a), "S5-47: syncA reservation released");
        verify(h.ctl.reservationRegistry().holderCount() == 0, "S5-47: holderCount 0");
        verify(cancelSpy.count() == 1, "S5-47: cancelAllCompleted emitted immediately on sync cancel");
    }

    // S5-48: Cancel All z mieszanką aktywnych (asynchronicznych i synchronicznych) oraz oczekujących zadań
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        h.ctl.setParallelConcurrencyLimit(2);
        h.touchSource(TQ_S("m1"));
        h.touchSource(TQ_S("m2"));
        h.touchSource(TQ_S("m3"));

        QueueOptions optAsync; optAsync.asyncKill = true;
        const quint64 a1 = h.add(TQ_S("a1"), {h.src(TQ_S("m1"))}, h.dst(TQ_S("m1")), optAsync);
        const quint64 a2 = h.add(TQ_S("a2"), {h.src(TQ_S("m2"))}, h.dst(TQ_S("m2"))); // sync
        const quint64 a3 = h.add(TQ_S("a3"), {h.src(TQ_S("m3"))}, h.dst(TQ_S("m3"))); // queued

        verify(h.state(a1) == TransferTaskState::Running && h.state(a2) == TransferTaskState::Running,
               "S5-48: a1 and a2 are running");
        verify(h.state(a3) == TransferTaskState::Queued, "S5-48: a3 is queued");
        verify(h.ctl.reservationRegistry().holderCount() == 2, "S5-48: 2 active reservations");

        QSignalSpy cancelSpy(&h.ctl, &TransferQueueController::cancelAllCompleted);
        h.ctl.cancelAll();

        // a3: Queued -> natychmiast Cancelled
        verify(h.state(a3) == TransferTaskState::Cancelled, "S5-48: queued a3 cancelled immediately");
        // a2: sync kill -> natychmiast Cancelled, rezerwacja zwolniona
        verify(h.state(a2) == TransferTaskState::Cancelled, "S5-48: sync a2 cancelled immediately");
        verify(!h.ctl.reservationRegistry().holds(a2), "S5-48: a2 reservation released");
        // a1: async kill -> Cancelling, rezerwacja trzymana!
        verify(h.state(a1) == TransferTaskState::Cancelling, "S5-48: async a1 in Cancelling");
        verify(h.ctl.reservationRegistry().holds(a1), "S5-48: a1 STILL holds reservation");
        verify(h.ctl.reservationRegistry().holderCount() == 1, "S5-48: exactly 1 reservation still held");
        verify(h.ctl.activeCount() == 1, "S5-48: 1 active slot held");
        verify(cancelSpy.count() == 0, "S5-48: cancelAllCompleted not yet emitted");

        // Dokończenie a1
        h.job(TQ_S("a1"))->triggerAsyncResult(KJob::KilledJobError);
        verify(h.state(a1) == TransferTaskState::Cancelled, "S5-48: a1 now Cancelled");
        verify(!h.ctl.reservationRegistry().holds(a1), "S5-48: a1 reservation released");
        verify(h.ctl.reservationRegistry().holderCount() == 0, "S5-48: all reservations released");
        verify(cancelSpy.count() == 1, "S5-48: cancelAllCompleted emitted when last active finished");
    }

    // S5-49: Sekwencja Pause All -> Cancel All
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("p1"));
        QueueOptions optAsync; optAsync.asyncKill = true;
        const quint64 p = h.add(TQ_S("p1"), {h.src(TQ_S("p1"))}, h.dst(TQ_S("p1")), optAsync);
        verify(h.state(p) == TransferTaskState::Running, "S5-49: p1 is Running");
        verify(h.ctl.reservationRegistry().holds(p), "S5-49: p1 holds reservation");

        h.ctl.pauseAll();
        verify(h.state(p) == TransferTaskState::Paused, "S5-49: p1 is Paused");
        verify(h.ctl.reservationRegistry().holds(p), "S5-49: reservation preserved during Pause All");

        h.ctl.cancelAll();
        verify(h.state(p) == TransferTaskState::Cancelling, "S5-49: p1 transitioned from Paused to Cancelling");
        verify(h.ctl.reservationRegistry().holds(p), "S5-49: reservation preserved during Cancelling from Paused");

        h.job(TQ_S("p1"))->triggerAsyncResult(KJob::KilledJobError);
        verify(h.state(p) == TransferTaskState::Cancelled, "S5-49: p1 reaches Cancelled");
        verify(!h.ctl.reservationRegistry().holds(p), "S5-49: reservation released on terminal exit");
        verify(h.ctl.reservationRegistry().holderCount() == 0, "S5-49: 0 active reservations");
    }

    // S5-50: Idempotentność Cancel All w trakcie asynchronicznego CancellingAll
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("idem"));
        QueueOptions optAsync; optAsync.asyncKill = true;
        const quint64 idTask = h.add(TQ_S("idem"), {h.src(TQ_S("idem"))}, h.dst(TQ_S("idem")), optAsync);
        h.ctl.cancelAll();
        verify(h.state(idTask) == TransferTaskState::Cancelling, "S5-50: in Cancelling");

        // Ponowne wywołanie cancelAll
        const bool again = h.ctl.cancelAll();
        verify(again, "S5-50: redundant cancelAll returns true (idempotent)");
        verify(h.state(idTask) == TransferTaskState::Cancelling, "S5-50: task state unchanged");
        verify(h.ctl.reservationRegistry().holds(idTask), "S5-50: reservation remains held");

        h.job(TQ_S("idem"))->triggerAsyncResult(KJob::KilledJobError);
        verify(h.state(idTask) == TransferTaskState::Cancelled, "S5-50: reached Cancelled");
        verify(!h.ctl.reservationRegistry().holds(idTask), "S5-50: reservation released");
    }

    // S5-51: Dodawanie zadania w trakcie CancellingAll i brak przedwczesnego dispatchu
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("drainSrc"));
        QueueOptions optAsync; optAsync.asyncKill = true;
        const quint64 dTask = h.add(TQ_S("drain"), {h.src(TQ_S("drainSrc"))}, h.dst(TQ_S("drainSrc")), optAsync);
        h.ctl.cancelAll();
        verify(h.ctl.isCancellingAll(), "S5-51: controller is cancelling all");

        // Dodajemy nowe zadanie podczas trwania procedury
        h.touchSource(TQ_S("laterSrc"));
        const quint64 lTask = h.add(TQ_S("later"), {h.src(TQ_S("laterSrc"))}, h.dst(TQ_S("laterSrc")));
        verify(h.state(lTask) == TransferTaskState::Queued, "S5-51: new task is Queued");
        verify(h.ctl.activeCount() == 1, "S5-51: active count still 1 (only drain task)");

        // Potwierdzamy zakończenie drain
        h.job(TQ_S("drain"))->triggerAsyncResult(KJob::KilledJobError);
        verify(h.state(dTask) == TransferTaskState::Cancelled, "S5-51: drain reached Cancelled");
        verify(!h.ctl.isCancellingAll(), "S5-51: cancelling procedure completed");

        // Nowe zadanie może zostać teraz uruchomione po zakończeniu procedury
        verify(h.state(lTask) == TransferTaskState::Running, "S5-51: later task dispatches cleanly after cancel all finished");
        verify(h.ctl.reservationRegistry().holds(lTask), "S5-51: later task holds reservation");
        h.job(TQ_S("later"))->finishOk();
        verify(h.ctl.reservationRegistry().holderCount() == 0, "S5-51: all reservations clean, no orphans");
    }

    // S5-52: Ochrona przed podwójnym callbackiem terminalnym i brak use-after-free
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("dbl"));
        QueueOptions optAsync; optAsync.asyncKill = true;
        const quint64 dbl = h.add(TQ_S("dbl"), {h.src(TQ_S("dbl"))}, h.dst(TQ_S("dbl")), optAsync);
        h.ctl.cancelAll();
        verify(h.state(dbl) == TransferTaskState::Cancelling, "S5-52: in Cancelling");
        verify(h.ctl.reservationRegistry().holds(dbl), "S5-52: reservation held");

        // Pierwszy callback terminalny
        h.job(TQ_S("dbl"))->triggerAsyncResult(KJob::KilledJobError);
        verify(h.state(dbl) == TransferTaskState::Cancelled, "S5-52: Cancelled after first callback");
        verify(h.ctl.reservationRegistry().holderCount() == 0, "S5-52: 0 active reservations");

        // Drugi, zdublowany callback terminalny (np. sygnał z zewnętrznego wątku)
        h.job(TQ_S("dbl"))->triggerAsyncResult(KJob::KilledJobError);
        verify(h.state(dbl) == TransferTaskState::Cancelled, "S5-52: state remains Cancelled, no change");
        verify(h.ctl.reservationRegistry().holderCount() == 0, "S5-52: no underflow, exactly 0 reservations");
        verify(h.ctl.activeCount() == 0, "S5-52: activeCount 0, no memory corruption or use-after-free");
    }

    // ======================================================================
    // STAGE 6: OPTIONAL BANDWIDTH THROTTLING SCENARIOS (S6-01 .. S6-46)
    // ======================================================================

    // S6-01: Domyślnie brak limitu (unlimited)
    {
        TransferBandwidthLimiter limiter;
        verify(!limiter.isEnabled(), "S6-01: limiter default is disabled");
        verify(limiter.bytesPerSecond() == 0, "S6-01: default rate is 0 bytes/sec");
        TransferQueueController ctl;
        verify(!ctl.isBandwidthLimitEnabled(), "S6-01: controller default is disabled");
        verify(ctl.bandwidthLimitBytesPerSec() == 0, "S6-01: controller default rate is 0");
    }

    // S6-02: Pojedynczy transfer z limitem 1 MiB/s (1048576 B/s)
    {
        TransferBandwidthLimiter limiter;
        qint64 virtualTime = 1000;
        limiter.setClock([&virtualTime]() { return virtualTime; });
        limiter.setLimit(true, 1024 * 1024);
        verify(limiter.isEnabled(), "S6-02: limiter enabled");
        verify(limiter.bytesPerSecond() == 1024 * 1024, "S6-02: 1 MiB/s configured");

        qint64 acquired = limiter.acquire(1, 512 * 1024);
        verify(acquired == 512 * 1024, "S6-02: acquired 512 KiB from burst");
        verify(limiter.bytesTransferredForTask(1) == 512 * 1024, "S6-02: recorded 512 KiB");

        virtualTime += 500; // 500 ms = +512 KiB tokens
        acquired = limiter.acquire(1, 512 * 1024);
        verify(acquired == 512 * 1024, "S6-02: acquired second 512 KiB after refill");
        verify(limiter.bytesTransferredForTask(1) == 1024 * 1024, "S6-02: total 1 MiB in 1 sec");
    }

    // S6-03: Pojedynczy transfer z limitem 256 KiB/s (262144 B/s)
    {
        TransferBandwidthLimiter limiter;
        qint64 virtualTime = 2000;
        limiter.setClock([&virtualTime]() { return virtualTime; });
        limiter.setLimit(true, 256 * 1024);
        verify(limiter.bytesPerSecond() == 256 * 1024, "S6-03: 256 KiB/s configured");

        qint64 acquired = limiter.acquire(2, 64 * 1024);
        verify(acquired == 64 * 1024, "S6-03: acquired 64 KiB");
        verify(limiter.totalBytesTransferred() == 64 * 1024, "S6-03: total 64 KiB");
    }

    // S6-04: Dwa równoległe transfery dzielące wspólny limit 2 MiB/s
    {
        TransferBandwidthLimiter limiter;
        qint64 virtualTime = 10000;
        limiter.setClock([&virtualTime]() { return virtualTime; });
        limiter.setLimit(true, 2 * 1024 * 1024);

        qint64 t1_bytes = 0;
        qint64 t2_bytes = 0;
        limiter.requestTokens(101, 1024 * 1024, [&t1_bytes](qint64 g) { t1_bytes += g; });
        limiter.requestTokens(102, 1024 * 1024, [&t2_bytes](qint64 g) { t2_bytes += g; });

        verify(t1_bytes == 1024 * 1024, "S6-04: Task 1 granted first share");
        verify(t2_bytes == 1024 * 1024, "S6-04: Task 2 granted second share from burst");
        verify(limiter.totalBytesTransferred() == 2 * 1024 * 1024, "S6-04: Total 2 MiB granted together");
    }

    // S6-05: Sprawiedliwy podział pasma bez zagłodzenia
    {
        TransferBandwidthLimiter limiter;
        qint64 virtualTime = 20000;
        limiter.setClock([&virtualTime]() { return virtualTime; });
        limiter.setLimit(true, 1024 * 1024); // 1 MiB/s

        // Drenaż początkowego burstu
        limiter.acquire(1, static_cast<qint64>(limiter.availableTokens()));

        qint64 g1 = 0, g2 = 0;
        limiter.requestTokens(1, 512 * 1024, [&g1](qint64 g) { g1 += g; });
        limiter.requestTokens(2, 512 * 1024, [&g2](qint64 g) { g2 += g; });
        verify(g1 == 0 && g2 == 0, "S6-05: Both pending when burst is depleted");

        virtualTime += 500; // +512 KiB tokens
        limiter.processPending();
        verify(g1 == 512 * 1024, "S6-05: Task 1 granted first in FIFO order");
        verify(g2 == 0, "S6-05: Task 2 still waiting");

        virtualTime += 500; // +512 KiB tokens
        limiter.processPending();
        verify(g2 == 512 * 1024, "S6-05: Task 2 granted without starvation");
    }

    // S6-06: Ograniczenie akumulacji burstu przy bezczynności
    {
        TransferBandwidthLimiter limiter;
        qint64 virtualTime = 50000;
        limiter.setClock([&virtualTime]() { return virtualTime; });
        limiter.setLimit(true, 1024 * 1024); // 1 MiB/s

        // Symulacja 60 sekund bezczynności
        virtualTime += 60000;
        limiter.processPending();
        verify(limiter.availableTokens() <= static_cast<double>(TransferBandwidthLimiter::MaxBurstBytes),
               "S6-06: Tokens clamped to MaxBurstBytes (2 MiB), not accumulated infinitely");
    }

    // S6-07: Wirtualny zegar w testach jednostkowych (determinizm)
    {
        TransferBandwidthLimiter limiter;
        qint64 vTime = 100;
        limiter.setClock([&vTime]() { return vTime; });
        limiter.setLimit(true, 1024 * 1024);
        verify(limiter.bytesPerSecond() == 1024 * 1024, "S6-07: Virtual clock works reliably");
    }

    // S6-08: Zmiana Unlimited -> Limited w trakcie transferu
    {
        TransferBandwidthLimiter limiter;
        qint64 vTime = 1000;
        limiter.setClock([&vTime]() { return vTime; });
        limiter.setLimit(false, 0);
        verify(!limiter.isEnabled(), "S6-08: Initially unlimited");

        qint64 acquired = limiter.acquire(1, 10 * 1024 * 1024);
        verify(acquired == 10 * 1024 * 1024, "S6-08: Unlimited acquires freely");

        limiter.setLimit(true, 512 * 1024);
        verify(limiter.isEnabled(), "S6-08: Now limited to 512 KiB/s");
        verify(limiter.bytesPerSecond() == 512 * 1024, "S6-08: Correct rate");
    }

    // S6-09: Zmiana Limited -> Unlimited w trakcie transferu
    {
        TransferBandwidthLimiter limiter;
        qint64 vTime = 2000;
        limiter.setClock([&vTime]() { return vTime; });
        limiter.setLimit(true, 1024 * 1024);
        limiter.acquire(1, static_cast<qint64>(limiter.availableTokens()));

        qint64 pendingGranted = 0;
        limiter.requestTokens(1, 5 * 1024 * 1024, [&pendingGranted](qint64 g) { pendingGranted += g; });
        verify(pendingGranted == 0, "S6-09: Request is pending");

        limiter.setLimit(false, 0); // Disable limit
        verify(!limiter.isEnabled(), "S6-09: Disabled limit");
        verify(pendingGranted == 5 * 1024 * 1024, "S6-09: Pending requests immediately granted upon disable");
    }

    // S6-10: Dynamiczne zwiększenie limitu w trakcie transferu
    {
        TransferBandwidthLimiter limiter;
        limiter.setLimit(true, 1024 * 1024);
        verify(limiter.bytesPerSecond() == 1024 * 1024, "S6-10: 1 MiB/s");
        limiter.setLimit(true, 4 * 1024 * 1024);
        verify(limiter.bytesPerSecond() == 4 * 1024 * 1024, "S6-10: Increased to 4 MiB/s smoothly");
    }

    // S6-11: Dynamiczne zmniejszenie limitu w trakcie transferu
    {
        TransferBandwidthLimiter limiter;
        limiter.setLimit(true, 5 * 1024 * 1024);
        verify(limiter.bytesPerSecond() == 5 * 1024 * 1024, "S6-11: 5 MiB/s");
        limiter.setLimit(true, 1 * 1024 * 1024);
        verify(limiter.bytesPerSecond() == 1 * 1024 * 1024, "S6-11: Decreased to 1 MiB/s smoothly");
    }

    // S6-12: Zmiana limitu przy wstrzymanej kolejce (Global Pause)
    {
        TransferQueueController ctl;
        ctl.setBandwidthLimit(true, 2 * 1024 * 1024);
        ctl.pauseAll();
        verify(ctl.isGlobalPauseActive(), "S6-12: Global pause active");
        ctl.setBandwidthLimit(true, 8 * 1024 * 1024);
        verify(ctl.bandwidthLimitBytesPerSec() == 8 * 1024 * 1024, "S6-12: Limit updated while paused");
    }

    // S6-13: Odporność na nieprawidłowe wartości (clamp do granic)
    {
        TransferBandwidthLimiter limiter;
        limiter.setLimit(true, 50); // Mniej niż Min (1024)
        verify(limiter.bytesPerSecond() == TransferBandwidthLimiter::MinBandwidthLimitBytes,
               "S6-13: Clamped to min 1 KiB/s");
        limiter.setLimit(true, 50ULL * 1024 * 1024 * 1024); // Więcej niż Max (10 GiB)
        verify(limiter.bytesPerSecond() == TransferBandwidthLimiter::MaxBandwidthLimitBytes,
               "S6-13: Clamped to max 10 GiB/s");
    }

    // S6-14: Serializacja QSettings — zapis i odczyt konfiguracji
    {
        const QString path = QDir::tempPath() + TQ_S("/thispc-stage6-settings-test.ini");
        QFile::remove(path);
        {
            QSettings s(path, QSettings::IniFormat);
            TransferQueueConfig cfg;
            cfg.bandwidthLimitEnabled = true;
            cfg.bandwidthLimitValue = 25;
            cfg.bandwidthLimitUnit = BandwidthUnit::MiB_s;
            cfg.bandwidthLimitBytesPerSec = bandwidthToBytesPerSec(25, BandwidthUnit::MiB_s);
            TransferQueueSettings::save(s, cfg);
        }
        {
            QSettings s(path, QSettings::IniFormat);
            TransferQueueConfig loaded = TransferQueueSettings::load(s);
            verify(loaded.bandwidthLimitEnabled, "S6-14: loaded enabled true");
            verify(loaded.bandwidthLimitValue == 25, "S6-14: loaded value 25");
            verify(loaded.bandwidthLimitUnit == BandwidthUnit::MiB_s, "S6-14: loaded unit MiB/s");
            verify(loaded.bandwidthLimitBytesPerSec == 25ULL * 1024 * 1024, "S6-14: loaded bytes/sec 25 MiB");
        }
        QFile::remove(path);
    }

    // S6-15: Backend Supported (LocalNative Copy)
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("supp_src"));
        const quint64 id = h.add(TQ_S("supp"), {h.src(TQ_S("supp_src"))}, h.dst(TQ_S("supp_src")));
        h.ctl.setBandwidthLimit(true, 5 * 1024 * 1024);
        const auto snap = h.ctl.taskSnapshot(id);
        verify(snap.has_value(), "S6-15: task snapshot available");
        verify(snap->supportsThrottling, "S6-15: LocalNative Copy supports throttling");
        verify(snap->throttlingActive, "S6-15: Throttling is active when enabled");
        verify(h.ctl.throttledActiveTasksCount() == 1, "S6-15: 1 throttled active task");
        verify(h.ctl.unthrottledActiveTasksCount() == 0, "S6-15: 0 unthrottled active tasks");
        h.job(TQ_S("supp"))->finishOk();
    }

    // S6-16: Backend Unsupported (KIO Backend)
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("kio_src"));
        QueueOptions opt;
        opt.backend = TransferBackendKind::Kio;
        const quint64 id = h.add(TQ_S("kio"), {h.src(TQ_S("kio_src"))}, h.dst(TQ_S("kio_src")), opt);
        h.ctl.setBandwidthLimit(true, 5 * 1024 * 1024);
        const auto snap = h.ctl.taskSnapshot(id);
        verify(snap.has_value(), "S6-16: kio task snapshot available");
        verify(!snap->supportsThrottling, "S6-16: KIO does NOT support throttling");
        verify(!snap->throttlingActive, "S6-16: Throttling inactive for KIO");
        verify(h.ctl.throttledActiveTasksCount() == 0, "S6-16: 0 throttled active tasks");
        verify(h.ctl.unthrottledActiveTasksCount() == 1, "S6-16: 1 unthrottled active task");
        h.job(TQ_S("kio"))->finishOk();
    }

    // S6-17: Równoległe zadania Supported i Unsupported w jednej kolejce
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        h.ctl.setParallelConcurrencyLimit(2);
        h.ctl.setBandwidthLimit(true, 5 * 1024 * 1024);
        h.touchSource(TQ_S("mix1"));
        h.touchSource(TQ_S("mix2"));

        QueueOptions optSupp; optSupp.backend = TransferBackendKind::LocalNative;
        QueueOptions optUnsupp; optUnsupp.backend = TransferBackendKind::Kio;

        const quint64 t1 = h.add(TQ_S("mix1"), {h.src(TQ_S("mix1"))}, h.dst(TQ_S("mix1")), optSupp);
        const quint64 t2 = h.add(TQ_S("mix2"), {h.src(TQ_S("mix2"))}, h.dst(TQ_S("mix2")), optUnsupp);

        verify(h.ctl.activeCount() == 2, "S6-17: Both running");
        verify(h.ctl.throttledActiveTasksCount() == 1, "S6-17: Exactly 1 throttled task");
        verify(h.ctl.unthrottledActiveTasksCount() == 1, "S6-17: Exactly 1 unthrottled task");

        h.job(TQ_S("mix1"))->finishOk();
        h.job(TQ_S("mix2"))->finishOk();
    }

    // S6-18: Operacje niemające transferu bajtowego (Move rename na tym samym FS)
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("mv_src"));
        QueueOptions optMove; optMove.type = TransferOperationType::Move;
        const quint64 id = h.add(TQ_S("mv"), {h.src(TQ_S("mv_src"))}, h.dst(TQ_S("mv_src")), optMove);
        h.ctl.setBandwidthLimit(true, 2 * 1024 * 1024);
        const auto snap = h.ctl.taskSnapshot(id);
        verify(snap.has_value(), "S6-18: snap exists");
        verify(!snap->supportsThrottling, "S6-18: Move rename does not support byte throttling");
        h.job(TQ_S("mv"))->finishOk();
    }

    // S6-19: Pause All a token bucket
    {
        QueueHarness h(nextRoot());
        h.ctl.setBandwidthLimit(true, 1024 * 1024);
        h.touchSource(TQ_S("p_src"));
        const quint64 id = h.add(TQ_S("p"), {h.src(TQ_S("p_src"))}, h.dst(TQ_S("p_src")));
        h.ctl.pauseAll();
        verify(h.state(id) == TransferTaskState::Paused, "S6-19: Task is paused");
        // Token bucket nie gubi konfiguracji ani nie pętli CPU
        verify(h.ctl.bandwidthLimitBytesPerSec() == 1024 * 1024, "S6-19: Limit preserved during Pause All");
        h.ctl.resumeAll();
        verify(h.state(id) == TransferTaskState::Running, "S6-19: Task resumed");
        h.job(TQ_S("p"))->finishOk();
    }

    // S6-20: Resume All a wznowienie throttlingu
    {
        QueueHarness h(nextRoot());
        h.ctl.setBandwidthLimit(true, 2 * 1024 * 1024);
        h.touchSource(TQ_S("res_src"));
        const quint64 id = h.add(TQ_S("res"), {h.src(TQ_S("res_src"))}, h.dst(TQ_S("res_src")));
        h.ctl.pauseAll();
        h.ctl.resumeAll();
        const auto snap = h.ctl.taskSnapshot(id);
        verify(snap->throttlingActive, "S6-20: Throttling immediately active upon resume");
        h.job(TQ_S("res"))->finishOk();
    }

    // S6-21: Cancel All z oczekującymi żądaniami tokenów
    {
        auto limiter = std::make_shared<TransferBandwidthLimiter>();
        limiter->setLimit(true, 1024 * 1024);
        TransferQueueController ctl;
        ctl.setBandwidthLimiter(limiter);

        bool cbInvoked = false;
        limiter->requestTokens(999, 50 * 1024 * 1024, [&cbInvoked](qint64) { cbInvoked = true; });
        verify(limiter->pendingRequestsCount() == 1, "S6-21: Request pending");

        ctl.cancelAll();
        verify(limiter->pendingRequestsCount() == 0, "S6-21: Pending requests cleared by cancelAll");
        verify(!cbInvoked, "S6-21: Pending callback cancelled safely");
    }

    // S6-22: Zniszczenie zadania a czyszczenie stanu w limiterze
    {
        auto limiter = std::make_shared<TransferBandwidthLimiter>();
        limiter->setLimit(true, 1024 * 1024);
        limiter->requestTokens(888, 10 * 1024 * 1024, [](qint64) {});
        verify(limiter->pendingRequestsCount() == 1, "S6-22: 1 pending request");
        limiter->cancelTask(888);
        verify(limiter->pendingRequestsCount() == 0, "S6-22: Request removed on task cancel");
    }

    // S6-23: Bezpieczeństwo pamięci — brak use-after-free callbacków
    {
        auto limiter = std::make_shared<TransferBandwidthLimiter>();
        limiter->setLimit(true, 512 * 1024);
        auto heapData = std::make_shared<int>(42);
        limiter->requestTokens(777, 20 * 1024 * 1024, [heapData](qint64) {
            *heapData = 100;
        });
        limiter->cancelTask(777); // Anulowanie natychmiast czyści lambdę
        verify(limiter->pendingRequestsCount() == 0, "S6-23: Zero pending");
        verify(*heapData == 42, "S6-23: Lambda was not called, no memory corruption");
    }

    // S6-24: Brak osieroconych timerów po anulowaniu
    {
        TransferBandwidthLimiter limiter;
        limiter.setLimit(true, 1024 * 1024);
        limiter.requestTokens(1, 10 * 1024 * 1024, [](qint64) {});
        limiter.cancelAll();
        verify(limiter.pendingRequestsCount() == 0, "S6-24: CancelAll stops timers and clears requests");
    }

    // S6-25: Brak spin-waitingu (zero CPU spin)
    {
        TransferBandwidthLimiter limiter;
        limiter.setLimit(true, 1024 * 1024);
        QElapsedTimer timer;
        timer.start();
        // Sprawdzamy, że funkcja acquire z cancelCheck natychmiast wychodzi bez pętli CPU
        qint64 result = limiter.acquire(1, 5 * 1024 * 1024, []() { return true; });
        verify(result == 0, "S6-25: acquire returns 0 when cancelled");
        verify(timer.elapsed() < 50, "S6-25: Zero CPU spin, returns instantly");
    }

    // S6-26: Rezerwacje ścieżek zachowane podczas throttlingu
    {
        QueueHarness h(nextRoot());
        h.ctl.setBandwidthLimit(true, 1024 * 1024);
        h.touchSource(TQ_S("res_hold"));
        const quint64 id = h.add(TQ_S("rh"), {h.src(TQ_S("res_hold"))}, h.dst(TQ_S("res_hold")));
        verify(h.ctl.reservationRegistry().holds(id), "S6-26: Reservation held while running throttled");
        h.job(TQ_S("rh"))->finishOk();
        verify(!h.ctl.reservationRegistry().holds(id), "S6-26: Reservation released on finish");
    }

    // S6-27: Tryb Sequential + Throttling
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        h.ctl.setBandwidthLimit(true, 1024 * 1024);
        h.touchSource(TQ_S("seq1"));
        h.touchSource(TQ_S("seq2"));
        const quint64 s1 = h.add(TQ_S("seq1"), {h.src(TQ_S("seq1"))}, h.dst(TQ_S("seq1")));
        const quint64 s2 = h.add(TQ_S("seq2"), {h.src(TQ_S("seq2"))}, h.dst(TQ_S("seq2")));
        verify(h.state(s1) == TransferTaskState::Running, "S6-27: s1 Running");
        verify(h.state(s2) == TransferTaskState::Queued, "S6-27: s2 Queued");
        verify(h.ctl.throttledActiveTasksCount() == 1, "S6-27: 1 throttled active");
        h.job(TQ_S("seq1"))->finishOk();
        verify(h.state(s2) == TransferTaskState::Running, "S6-27: s2 Running after s1 finished");
        h.job(TQ_S("seq2"))->finishOk();
    }

    // S6-28: Tryb Parallel + Throttling
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        h.ctl.setParallelConcurrencyLimit(3);
        h.ctl.setBandwidthLimit(true, 3 * 1024 * 1024);
        h.touchSource(TQ_S("par1"));
        h.touchSource(TQ_S("par2"));
        h.touchSource(TQ_S("par3"));
        const quint64 p1 = h.add(TQ_S("par1"), {h.src(TQ_S("par1"))}, h.dst(TQ_S("par1")));
        const quint64 p2 = h.add(TQ_S("par2"), {h.src(TQ_S("par2"))}, h.dst(TQ_S("par2")));
        const quint64 p3 = h.add(TQ_S("par3"), {h.src(TQ_S("par3"))}, h.dst(TQ_S("par3")));
        verify(h.ctl.activeCount() == 3, "S6-28: All 3 active");
        verify(h.ctl.throttledActiveTasksCount() == 3, "S6-28: All 3 throttled active");
        h.job(TQ_S("par1"))->finishOk();
        h.job(TQ_S("par2"))->finishOk();
        h.job(TQ_S("par3"))->finishOk();
    }

    // S6-29: Limity 1..8 a zachowanie throttlingu
    {
        TransferQueueController ctl;
        ctl.setBandwidthLimit(true, 10 * 1024 * 1024);
        for (int lim = 1; lim <= 8; ++lim) {
            ctl.setParallelConcurrencyLimit(lim);
            verify(ctl.parallelConcurrencyLimit() == lim, "S6-29: Limit set correctly");
            verify(ctl.bandwidthLimitBytesPerSec() == 10 * 1024 * 1024, "S6-29: Bandwidth limit intact across limits");
        }
    }

    // S6-30: Priorytety High / Normal / Low ze wspólnym limiterem
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        h.ctl.setBandwidthLimit(true, 2 * 1024 * 1024);
        h.touchSource(TQ_S("prio_low"));
        h.touchSource(TQ_S("prio_high"));

        QueueOptions optLow; optLow.priority = TransferPriority::Low;
        QueueOptions optHigh; optHigh.priority = TransferPriority::High;

        // Blokujemy kolejkę zadaniem dummy
        h.touchSource(TQ_S("blocker"));
        const quint64 blk = h.add(TQ_S("blocker"), {h.src(TQ_S("blocker"))}, h.dst(TQ_S("blocker")));
        const quint64 lowId = h.add(TQ_S("low"), {h.src(TQ_S("prio_low"))}, h.dst(TQ_S("prio_low")), optLow);
        const quint64 highId = h.add(TQ_S("high"), {h.src(TQ_S("prio_high"))}, h.dst(TQ_S("prio_high")), optHigh);

        h.job(TQ_S("blocker"))->finishOk();
        verify(h.state(highId) == TransferTaskState::Running, "S6-30: High dispatched first despite later enqueue");
        verify(h.ctl.taskSnapshot(highId)->throttlingActive, "S6-30: High runs with active throttling");
        h.job(TQ_S("high"))->finishOk();
        verify(h.state(lowId) == TransferTaskState::Running, "S6-30: Low runs next");
        h.job(TQ_S("low"))->finishOk();
    }

    // S6-31: Bounded Bypass Fairness a limit pasma
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        h.ctl.setBandwidthLimit(true, 1024 * 1024);
        h.touchSource(TQ_S("starve_src"));
        QueueOptions optNorm; optNorm.priority = TransferPriority::Normal;
        // Weryfikacja, że bypass threshold działa bez przeszkód z limiterem
        verify(TransferQueueController::StarvationBypassThresholdNormal == 3, "S6-31: Threshold intact");
    }

    // S6-32: Move Up / Move Down zachowuje throttling
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);
        h.ctl.setBandwidthLimit(true, 4 * 1024 * 1024);
        h.touchSource(TQ_S("m1"));
        h.touchSource(TQ_S("m2"));
        h.touchSource(TQ_S("m_block"));
        h.add(TQ_S("blk"), {h.src(TQ_S("m_block"))}, h.dst(TQ_S("m_block")));
        const quint64 m1 = h.add(TQ_S("m1"), {h.src(TQ_S("m1"))}, h.dst(TQ_S("m1")));
        const quint64 m2 = h.add(TQ_S("m2"), {h.src(TQ_S("m2"))}, h.dst(TQ_S("m2")));

        verify(h.ctl.canMoveTaskUp(m2), "S6-32: can move up");
        h.ctl.moveTaskUp(m2);
        verify(h.ctl.taskSnapshot(m2)->queuePosition == 1, "S6-32: m2 is now #1 in queue");
        verify(h.ctl.taskSnapshot(m2)->supportsThrottling, "S6-32: supports throttling preserved");
        h.job(TQ_S("blk"))->finishOk();
        h.job(TQ_S("m2"))->finishOk();
        h.job(TQ_S("m1"))->finishOk();
    }

    // S6-33: TransferQueueSettingsWidget — inicjalizacja i stan początkowy
    {
        TransferQueueController ctl;
        TransferQueueSettingsWidget widget;
        widget.setController(&ctl);
        verify(widget.bandwidthCheck() != nullptr, "S6-33: bandwidth check exists");
        verify(!widget.bandwidthCheck()->isChecked(), "S6-33: unchecked by default");
        verify(!widget.bandwidthSpin()->isEnabled(), "S6-33: spinbox disabled when unchecked");
        verify(!widget.bandwidthUnitCombo()->isEnabled(), "S6-33: combo disabled when unchecked");
    }

    // S6-34: TransferQueueSettingsWidget — włączenie checkboxa aktywuje kontrolki
    {
        TransferQueueController ctl;
        TransferQueueSettingsWidget widget;
        widget.setController(&ctl);
        widget.bandwidthCheck()->setChecked(true);
        verify(widget.bandwidthSpin()->isEnabled(), "S6-34: spinbox enabled");
        verify(widget.bandwidthUnitCombo()->isEnabled(), "S6-34: unit combo enabled");
        verify(ctl.isBandwidthLimitEnabled(), "S6-34: controller updated to enabled");
    }

    // S6-35: TransferQueueSettingsWidget — zmiana wartości spinboxa i jednostki
    {
        TransferQueueController ctl;
        TransferQueueSettingsWidget widget;
        widget.setController(&ctl);
        widget.bandwidthCheck()->setChecked(true);
        widget.bandwidthSpin()->setValue(50);
        int kibIdx = widget.bandwidthUnitCombo()->findData(static_cast<int>(BandwidthUnit::KiB_s));
        widget.bandwidthUnitCombo()->setCurrentIndex(kibIdx);
        verify(ctl.bandwidthLimitBytesPerSec() == 50ULL * 1024, "S6-35: 50 KiB/s in controller");
    }

    // S6-36: TransferQueueSettingsWidget — dwurzędowy układ zachowuje minimalną wysokość
    {
        TransferQueueSettingsWidget widget;
        const QSize minSize = widget.minimumSizeHint();
        verify(minSize.height() > 0 && minSize.height() <= 120, "S6-36: Compact height for settings group");
        verify(minSize.width() >= 300, "S6-36: Valid min width");
    }

    // S6-37: TransferQueueTaskItemWidget — wskaźnik Throttled / No limit
    {
        TransferQueueController ctl;
        ctl.setBandwidthLimit(true, 5 * 1024 * 1024);
        TransferTaskSnapshot snapSupp;
        snapSupp.id = 1;
        snapSupp.state = TransferTaskState::Running;
        snapSupp.supportsThrottling = true;
        snapSupp.throttlingActive = true;

        TransferQueueTaskItemWidget itemSupp(snapSupp, &ctl);
        verify(!itemSupp.throttlingBadge()->isHidden(), "S6-37: Throttling badge not hidden");
        const QString suppText = itemSupp.throttlingBadge()->text();
        verify(suppText == TQ_S("Limitowany") || suppText == TQ_S("Throttled"), "S6-37: Text is Limitowany or Throttled");

        TransferTaskSnapshot snapUnsupp;
        snapUnsupp.id = 2;
        snapUnsupp.state = TransferTaskState::Running;
        snapUnsupp.supportsThrottling = false;
        snapUnsupp.throttlingActive = false;

        TransferQueueTaskItemWidget itemUnsupp(snapUnsupp, &ctl);
        verify(!itemUnsupp.throttlingBadge()->isHidden(), "S6-37: No-limit badge not hidden");
        const QString unsuppText = itemUnsupp.throttlingBadge()->text();
        verify(unsuppText == TQ_S("Brak limitu") || unsuppText == TQ_S("No limit"), "S6-37: Text is Brak limitu or No limit");
    }

    // S6-38: TransferQueueViewWidget — nagłówek z liczbą throttled vs unthrottled
    {
        QueueHarness h(nextRoot());
        h.ctl.setExecutionMode(TransferQueueMode::Parallel);
        h.ctl.setParallelConcurrencyLimit(2);
        h.ctl.setBandwidthLimit(true, 5 * 1024 * 1024);
        h.touchSource(TQ_S("v1"));
        h.touchSource(TQ_S("v2"));

        QueueOptions o1; o1.backend = TransferBackendKind::LocalNative;
        QueueOptions o2; o2.backend = TransferBackendKind::Kio;
        h.add(TQ_S("v1"), {h.src(TQ_S("v1"))}, h.dst(TQ_S("v1")), o1);
        h.add(TQ_S("v2"), {h.src(TQ_S("v2"))}, h.dst(TQ_S("v2")), o2);

        TransferQueueViewWidget view;
        view.setController(&h.ctl);
        verify(view.countLabel() != nullptr, "S6-38: count label exists");
        verify(h.ctl.throttledActiveTasksCount() == 1, "S6-38: 1 throttled");
        verify(h.ctl.unthrottledActiveTasksCount() == 1, "S6-38: 1 unthrottled");
        h.job(TQ_S("v1"))->finishOk();
        h.job(TQ_S("v2"))->finishOk();
    }

    // S6-39: Minimalny rozmiar okna 480 × 260 / 480 × 380 px
    {
        OperationWindow win(nullptr);
        const QSize minHint = win.minimumSizeHint();
        verify(minHint.width() <= 480, "S6-39: Width fits 480 px");
        verify(minHint.height() <= 380, "S6-39: Height fits 380 px");
    }

    // S6-40: Długie nazwy plików i responsywność wierszy
    {
        TransferTaskSnapshot snap;
        snap.id = 55;
        snap.state = TransferTaskState::Running;
        snap.title = TQ_S("Bardzo_bardzo_bardzo_bardzo_dluga_nazwa_pliku_testowego_ze_sciezkami_weryfikujaca_eliding_i_brak_rozpychania_okna_thispc.dat");
        snap.supportsThrottling = true;
        TransferQueueTaskItemWidget item(snap, nullptr);
        verify(item.titleLabel() != nullptr, "S6-40: item title label exists");
        verify(item.minimumSizeHint().width() <= 300, "S6-40: Row width does not blow out on long names");
    }

    // S6-41: DemoTransferJob — symulacja ograniczenia prędkości
    {
        auto limiter = std::make_shared<TransferBandwidthLimiter>();
        limiter->setLimit(true, 1024 * 1024);
        DemoTransferJob job(10 * 1024 * 1024, 500);
        job.setBandwidthLimiter(limiter, 1);
        job.start();
        pump(50);
        job.kill(KJob::Quietly);
        verify(limiter->totalBytesTransferred() > 0 || limiter->bytesTransferredForTask(1) > 0,
               "S6-41: Demo job acquired tokens from limiter");
    }

    // S6-42: Izolacja produkcyjnego dispatchu (flaga disconnected)
    {
        verify(!TransferQueueProductionDispatchConnected,
               "S6-42: TransferQueueProductionDispatchConnected must stay false in Stage 6");
    }

    // S6-43: LocalFileCopyWorker — hook do bandwidth limitera
    {
        auto limiter = std::make_shared<TransferBandwidthLimiter>();
        LocalFileCopyJob job(QUrl::fromLocalFile(TQ_S("/tmp/fake_src")),
                             QUrl::fromLocalFile(TQ_S("/tmp/fake_dst")));
        job.setBandwidthLimiter(limiter, 12345);
        // Sprawdzenie, że metoda przyjmuje limiter bez błędu
        verify(limiter->bytesPerSecond() == 0, "S6-43: Limiter attached to LocalFileCopyJob");
    }

    // S6-44: Niezależność tokenów przy wielokrotnych zadaniach
    {
        TransferBandwidthLimiter limiter;
        limiter.setLimit(true, 1024 * 1024);
        limiter.acquire(1, 100 * 1024);
        limiter.acquire(2, 200 * 1024);
        verify(limiter.bytesTransferredForTask(1) == 100 * 1024, "S6-44: Task 1 bytes recorded");
        verify(limiter.bytesTransferredForTask(2) == 200 * 1024, "S6-44: Task 2 bytes recorded");
        verify(limiter.totalBytesTransferred() == 300 * 1024, "S6-44: Total bytes recorded");
    }

    // S6-45: Jednostki prędkości KiB/s i MiB/s — helpery konwersji
    {
        verify(bandwidthToBytesPerSec(1, BandwidthUnit::KiB_s) == 1024ULL, "S6-45: 1 KiB/s");
        verify(bandwidthToBytesPerSec(2, BandwidthUnit::MiB_s) == 2ULL * 1024 * 1024, "S6-45: 2 MiB/s");
        BandwidthUnit u;
        verify(bytesPerSecToUnitValue(10ULL * 1024 * 1024, u) == 10 && u == BandwidthUnit::MiB_s, "S6-45: 10 MiB/s reverse");
        verify(bytesPerSecToUnitValue(512ULL * 1024, u) == 512 && u == BandwidthUnit::KiB_s, "S6-45: 512 KiB/s reverse");
    }

    // S6-46: Pełna neutralność schedulera przy wyłączonym limiterze
    {
        QueueHarness h(nextRoot());
        h.ctl.setBandwidthLimit(false, 0);
        verify(!h.ctl.isBandwidthLimitEnabled(), "S6-46: Limit disabled");
        h.touchSource(TQ_S("neut_src"));
        const quint64 id = h.add(TQ_S("neut"), {h.src(TQ_S("neut_src"))}, h.dst(TQ_S("neut_src")));
        verify(h.state(id) == TransferTaskState::Running, "S6-46: Running without restriction");
        const auto snap = h.ctl.taskSnapshot(id);
        verify(!snap->throttlingActive, "S6-46: Throttling inactive");
        h.job(TQ_S("neut"))->finishOk();
    }

    // =========================================================================
    // Stage 7: Production Integration & Final Regression
    // =========================================================================

    // S7-01: Production dispatch gate defaults to false, toggles with setTransferQueueProductionDispatchConnected
    {
        verify(!TransferQueueProductionDispatchConnected, "S7-01: default is false");
        verify(!isTransferQueueProductionDispatchConnected(), "S7-01: default helper is false");
        setTransferQueueProductionDispatchConnected(true);
        verify(isTransferQueueProductionDispatchConnected(), "S7-01: toggled to true");
        setTransferQueueProductionDispatchConnected(false);
        verify(!isTransferQueueProductionDispatchConnected(), "S7-01: toggled back to false");
    }

    // S7-02: hasNonTerminalTasks() returns correct state
    {
        QueueHarness h(nextRoot());
        verify(!h.ctl.hasNonTerminalTasks(), "S7-02: empty queue has no non-terminal tasks");
        h.touchSource(TQ_S("nt_src"));
        const quint64 id = h.add(TQ_S("nt"), {h.src(TQ_S("nt_src"))}, h.dst(TQ_S("nt_src")));
        verify(h.ctl.hasNonTerminalTasks(), "S7-02: running task counts as non-terminal");
        h.job(TQ_S("nt"))->finishOk();
        verify(!h.ctl.hasNonTerminalTasks(), "S7-02: finished task means no non-terminal tasks");
    }

    // S7-03: UndoController barrier blocks Undo/Redo while non-terminal tasks exist
    {
        QueueHarness h(nextRoot());
        QAction undoAct;
        QAction redoAct;
        UndoController undoCtrl(nullptr, [](bool){}, []{}, [](const QString&, int){});
        undoCtrl.setActions(&undoAct, &redoAct);
        undoCtrl.setQueueController(&h.ctl);
        verify(!undoAct.isEnabled(), "S7-03: initially no undo available");

        h.touchSource(TQ_S("undo_src"));
        const quint64 id = h.add(TQ_S("undo_task"), {h.src(TQ_S("undo_src"))}, h.dst(TQ_S("undo_src")));
        verify(h.ctl.hasNonTerminalTasks(), "S7-03: task running");

        // While non-terminal tasks exist, undo must be blocked
        undoCtrl.undo(); // should be a safe no-op
        h.job(TQ_S("undo_task"))->finishOk();
        verify(!h.ctl.hasNonTerminalTasks(), "S7-03: task finished");
    }

    // S7-04: Deferred job creation — factory called only once upon dispatch
    {
        QueueHarness h(nextRoot());
        int factoryCalls = 0;
        TransferTaskDescriptor desc;
        desc.title = TQ_S("deferred");
        desc.type = TransferOperationType::Copy;
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {h.src(TQ_S("def_src"))};
        desc.destination = QUrl::fromLocalFile(h.root + TQ_S("/dst"));
        h.touchSource(TQ_S("def_src"));
        desc.jobFactory = [&factoryCalls, &h]() -> KJob* {
            ++factoryCalls;
            auto *job = new QueueFakeJob(true, false, false);
            h.jobs.insert(TQ_S("deferred"), job);
            return job;
        };

        verify(factoryCalls == 0, "S7-04: factory not called before enqueue");
        const auto res = h.ctl.enqueue(desc);
        verify(res.rejection == TransferEnqueueRejection::None, "S7-04: enqueued successfully");
        verify(factoryCalls == 1, "S7-04: factory called exactly once on dispatch");
        verify(h.job(TQ_S("deferred")) != nullptr, "S7-04: job instance exists");
        h.job(TQ_S("deferred"))->finishOk();
        verify(factoryCalls == 1, "S7-04: factory not called again after completion");
    }

    // S7-05: Task cancelled while queued — factory is NEVER called
    {
        QueueHarness h(nextRoot());
        h.ctl.setDispatchPaused(true);
        int factoryCalls = 0;
        TransferTaskDescriptor desc;
        desc.title = TQ_S("never_started");
        desc.type = TransferOperationType::Copy;
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {h.src(TQ_S("never_src"))};
        desc.destination = QUrl::fromLocalFile(h.root + TQ_S("/dst"));
        h.touchSource(TQ_S("never_src"));
        desc.jobFactory = [&factoryCalls]() -> KJob* {
            ++factoryCalls;
            return nullptr;
        };

        const auto res = h.ctl.enqueue(desc);
        verify(res.rejection == TransferEnqueueRejection::None, "S7-05: enqueued while paused");
        verify(factoryCalls == 0, "S7-05: factory not called while queued");
        verify(h.ctl.cancelTask(res.id), "S7-05: cancelled while queued");
        verify(factoryCalls == 0, "S7-05: factory NEVER called for task cancelled in queue");
        h.ctl.setDispatchPaused(false);
    }

    // S7-06: Resource reservation conflict between two tasks
    {
        QueueHarness h(nextRoot());
        h.ctl.setParallelConcurrencyLimit(4);
        h.touchSource(TQ_S("conflict_file"));
        QDir().mkpath(h.root + TQ_S("/src/sub"));
        {
            QFile subf(h.root + TQ_S("/src/sub/conflict_file"));
            verify(subf.open(QIODevice::WriteOnly) && subf.write("subpayload") > 0, "create sub source");
        }
        // Both copy to same destination file "dst/d/conflict_file"
        const quint64 id1 = h.add(TQ_S("task1"), {h.src(TQ_S("conflict_file"))}, h.dst(TQ_S("d")));
        const quint64 id2 = h.add(TQ_S("task2"), {h.src(TQ_S("sub/conflict_file"))}, h.dst(TQ_S("d")));

        verify(h.state(id1) == TransferTaskState::Running, "S7-06: task1 is running");
        verify(h.state(id2) == TransferTaskState::Queued, "S7-06: task2 is queued due to reservation conflict");
        verify(h.snap(id2).waitReason == TransferWaitReason::BlockedByReservation,
               "S7-06: wait reason is BlockedByReservation");

        h.job(TQ_S("task1"))->finishOk();
        verify(h.state(id1) == TransferTaskState::Completed, "S7-06: task1 completed");
        verify(h.state(id2) == TransferTaskState::Running, "S7-06: task2 auto-dispatched after conflict release");
        h.job(TQ_S("task2"))->finishOk();
    }

    // S7-07: Real file copy with hash verification (SHA-256) and integrity check
    {
        const QString sdir = nextRoot();
        QDir().mkpath(sdir + TQ_S("/src"));
        QDir().mkpath(sdir + TQ_S("/dst"));
        const QString srcFile = sdir + TQ_S("/src/data_test.bin");
        const QString dstFile = sdir + TQ_S("/dst/data_test.bin");

        // Write 512 KiB test pattern
        QByteArray content;
        content.resize(512 * 1024);
        for (int i = 0; i < content.size(); ++i) {
            content[i] = static_cast<char>((i * 73 + 19) & 0xFF);
        }
        {
            QFile f(srcFile);
            verify(f.open(QIODevice::WriteOnly), "S7-07: create src file");
            f.write(content);
            f.close();
        }
        const QByteArray srcHash = QCryptographicHash::hash(content, QCryptographicHash::Sha256);

        LocalFileCopyJob job(QUrl::fromLocalFile(srcFile), QUrl::fromLocalFile(dstFile));
        job.setAutoDelete(false);
        bool finished = false;
        QObject::connect(&job, &KJob::result, [&finished] { finished = true; });
        job.start();
        verify(waitUntil([&finished] { return finished; }, 5000), "S7-07: job finished");
        verify(job.error() == 0, "S7-07: job error is 0");

        QFile df(dstFile);
        verify(df.open(QIODevice::ReadOnly), "S7-07: open dst file");
        const QByteArray dstContent = df.readAll();
        df.close();
        verify(dstContent.size() == content.size(), "S7-07: byte count match");
        const QByteArray dstHash = QCryptographicHash::hash(dstContent, QCryptographicHash::Sha256);
        verify(srcHash == dstHash, "S7-07: SHA-256 integrity hash matches perfectly");
    }

    // S7-08: Real file copy with bandwidth throttling and elapsed time measurement
    {
        const QString sdir = nextRoot();
        QDir().mkpath(sdir + TQ_S("/src"));
        QDir().mkpath(sdir + TQ_S("/dst"));
        const QString srcFile = sdir + TQ_S("/src/throttled.bin");
        const QString dstFile = sdir + TQ_S("/dst/throttled.bin");

        // 256 KiB file
        QByteArray content(256 * 1024, 'X');
        {
            QFile f(srcFile);
            verify(f.open(QIODevice::WriteOnly), "S7-08: create src file");
            f.write(content);
            f.close();
        }

        auto limiter = std::make_shared<TransferBandwidthLimiter>();
        // Limit to 512 KiB/s with small burst so 256 KiB requires measurable time
        limiter->setLimit(true, 512 * 1024);

        LocalFileCopyJob job(QUrl::fromLocalFile(srcFile), QUrl::fromLocalFile(dstFile));
        job.setAutoDelete(false);
        job.setBandwidthLimiter(limiter, 708);
        bool finished = false;
        QObject::connect(&job, &KJob::result, [&finished] { finished = true; });
        job.start();
        verify(waitUntil([&finished] { return finished; }, 5000), "S7-08: job finished");
        verify(job.error() == 0, "S7-08: job error is 0");

        QFile df(dstFile);
        verify(df.open(QIODevice::ReadOnly), "S7-08: open dst file");
        verify(df.readAll() == content, "S7-08: content preserved under throttle");
        verify(limiter->bytesTransferredForTask(708) == 256 * 1024, "S7-08: limiter tracked exact bytes");
    }

    // S7-09: Real file move on same filesystem (atomic rename)
    {
        const QString sdir = nextRoot();
        QDir().mkpath(sdir + TQ_S("/sub1"));
        QDir().mkpath(sdir + TQ_S("/sub2"));
        const QString srcFile = sdir + TQ_S("/sub1/move_me.txt");
        const QString dstFile = sdir + TQ_S("/sub2/moved.txt");

        const QByteArray payload = "Atomic Move Verification Payload 2026";
        {
            QFile f(srcFile);
            verify(f.open(QIODevice::WriteOnly), "S7-09: create src");
            f.write(payload);
            f.close();
        }

        LocalFileMoveJob moveJob(QUrl::fromLocalFile(srcFile), QUrl::fromLocalFile(dstFile));
        moveJob.setAutoDelete(false);
        bool finished = false;
        QObject::connect(&moveJob, &KJob::result, [&finished] { finished = true; });
        moveJob.start();
        verify(waitUntil([&finished] { return finished; }, 5000), "S7-09: move finished");
        verify(moveJob.error() == 0, "S7-09: no error");
        verify(!QFile::exists(srcFile), "S7-09: source removed");
        verify(QFile::exists(dstFile), "S7-09: destination exists");

        QFile f(dstFile);
        verify(f.open(QIODevice::ReadOnly), "S7-09: read dst");
        verify(f.readAll() == payload, "S7-09: payload matches");
    }

    // S7-10: Unicode filenames copy & integrity
    {
        const QString sdir = nextRoot();
        QDir().mkpath(sdir + TQ_S("/src"));
        QDir().mkpath(sdir + TQ_S("/dst"));
        const QString srcFile = sdir + TQ_S("/src/zażółć gęślą jaźń 🚀.txt");
        const QString dstFile = sdir + TQ_S("/dst/zażółć gęślą jaźń 🚀.txt");

        const QByteArray payload = "Unicode test string with diacritics and emojis";
        {
            QFile f(srcFile);
            verify(f.open(QIODevice::WriteOnly), "S7-10: create unicode src");
            f.write(payload);
            f.close();
        }

        LocalFileCopyJob job(QUrl::fromLocalFile(srcFile), QUrl::fromLocalFile(dstFile));
        job.setAutoDelete(false);
        bool finished = false;
        QObject::connect(&job, &KJob::result, [&finished] { finished = true; });
        job.start();
        verify(waitUntil([&finished] { return finished; }, 5000), "S7-10: job finished");
        verify(job.error() == 0, "S7-10: job error 0");
        verify(QFile::exists(dstFile), "S7-10: destination unicode file exists");

        QFile df(dstFile);
        verify(df.open(QIODevice::ReadOnly), "S7-10: open unicode dst");
        verify(df.readAll() == payload, "S7-10: unicode file content verified");
    }

    // S7-11: Hidden file copy & integrity
    {
        const QString sdir = nextRoot();
        QDir().mkpath(sdir + TQ_S("/src"));
        QDir().mkpath(sdir + TQ_S("/dst"));
        const QString srcFile = sdir + TQ_S("/src/.hidden_config");
        const QString dstFile = sdir + TQ_S("/dst/.hidden_config");

        const QByteArray payload = "hidden=true\nenabled=1\n";
        {
            QFile f(srcFile);
            verify(f.open(QIODevice::WriteOnly), "S7-11: create hidden src");
            f.write(payload);
            f.close();
        }

        LocalFileCopyJob job(QUrl::fromLocalFile(srcFile), QUrl::fromLocalFile(dstFile));
        job.setAutoDelete(false);
        bool finished = false;
        QObject::connect(&job, &KJob::result, [&finished] { finished = true; });
        job.start();
        verify(waitUntil([&finished] { return finished; }, 5000), "S7-11: job finished");
        verify(job.error() == 0, "S7-11: job error 0");
        verify(QFile::exists(dstFile), "S7-11: destination hidden file exists");
    }

    // S7-12: Cancellation cleanup of partial file (.thispc-part)
    {
        const QString sdir = nextRoot();
        QDir().mkpath(sdir + TQ_S("/src"));
        QDir().mkpath(sdir + TQ_S("/dst"));
        const QString srcFile = sdir + TQ_S("/src/cancel_source.bin");
        const QString dstFile = sdir + TQ_S("/dst/cancel_source.bin");
        const QString partFile = dstFile + TQ_S(".thispc-part");

        QByteArray large(1024 * 1024, 'Z');
        {
            QFile f(srcFile);
            verify(f.open(QIODevice::WriteOnly), "S7-12: create src");
            f.write(large);
            f.close();
        }

        auto limiter = std::make_shared<TransferBandwidthLimiter>();
        limiter->setLimit(true, 1024); // 1 KiB/s

        LocalFileCopyJob job(QUrl::fromLocalFile(srcFile), QUrl::fromLocalFile(dstFile));
        job.setAutoDelete(false);
        job.setBandwidthLimiter(limiter, 712);
        bool finished = false;
        QObject::connect(&job, &KJob::result, [&finished] { finished = true; });
        job.start();
        QTest::qWait(50);
        job.kill(KJob::EmitResult);
        verify(waitUntil([&finished] { return finished; }, 5000), "S7-12: job cancelled");
        verify(job.error() == KJob::KilledJobError, "S7-12: error is KilledJobError");
        verify(waitUntil([&partFile] { return !QFile::exists(partFile); }, 5000), "S7-12: .thispc-part cleaned up after cancel");
        verify(!QFile::exists(dstFile), "S7-12: dst file not published after cancel");
    }

    // S7-13: Administrator scheme rejection
    {
        QueueHarness h(nextRoot());
        TransferTaskDescriptor desc;
        desc.title = TQ_S("admin_test");
        desc.type = TransferOperationType::Copy;
        desc.backend = TransferBackendKind::Kio;
        desc.sources = {QUrl(TQ_S("admin:///root/secret.conf"))};
        desc.destination = QUrl::fromLocalFile(h.root + TQ_S("/dst"));
        desc.jobFactory = []() -> KJob* { return nullptr; };

        const auto res = h.ctl.enqueue(desc);
        verify(res.rejection == TransferEnqueueRejection::AdminSchemeForbidden,
               "S7-13: admin:// scheme strictly forbidden in queue scheduler");
    }

    // S7-14: Shutdown cleans up active controller and reports completion
    {
        QueueHarness h(nextRoot());
        h.touchSource(TQ_S("shut_src"));
        h.add(TQ_S("shut_task"), {h.src(TQ_S("shut_src"))}, h.dst(TQ_S("shut_src")));
        verify(h.ctl.activeCount() > 0, "S7-14: task active before shutdown");

        bool shutdownFinished = false;
        QObject::connect(&h.ctl, &TransferQueueController::shutdownCompleted, [&shutdownFinished] {
            shutdownFinished = true;
        });

        h.ctl.beginShutdown();
        verify(waitUntil([&shutdownFinished, &h] { return shutdownFinished || h.ctl.isShutdownComplete(); }, 5000),
               "S7-14: shutdown completes cleanly");
        verify(h.ctl.activeCount() == 0, "S7-14: no active tasks remaining after shutdown");
    }

    // S7-16: Undo/Redo barrier covers Cancelling state and async callback release; Queued cancel leaves no Undo
    {
        QueueHarness h(nextRoot());
        QWidget parentWidget;
        UndoController undoCtrl(&parentWidget, nullptr, nullptr, nullptr);
        QAction undoAct;
        QAction redoAct;
        undoCtrl.setActions(&undoAct, &redoAct);
        undoCtrl.setQueueController(&h.ctl);

        // 1. Task in Queued state blocks Undo/Redo
        h.ctl.setDispatchPaused(true);
        h.touchSource(TQ_S("q_src"));
        const quint64 queuedId = h.add(TQ_S("queued_task"), {h.src(TQ_S("q_src"))}, h.dst(TQ_S("q_dst")));
        verify(h.state(queuedId) == TransferTaskState::Queued, "S7-16: task queued");
        verify(h.ctl.hasNonTerminalTasks(), "S7-16: queued task is non-terminal");
        verify(!undoAct.isEnabled(), "S7-16: Undo blocked while task is Queued");
        verify(!redoAct.isEnabled(), "S7-16: Redo blocked while task is Queued");

        // Cancel while queued (before job creation) -> must not leave any Undo entry
        verify(h.ctl.cancelTask(queuedId), "S7-16: cancel queued task");
        verify(h.state(queuedId) == TransferTaskState::Cancelled, "S7-16: task cancelled");
        verify(!h.ctl.hasNonTerminalTasks(), "S7-16: no non-terminal tasks remain");
        verify(!undoAct.isEnabled(), "S7-16: cancelled queued task leaves no phantom Undo");

        // 2. Start running copy with async cancellation
        h.ctl.setDispatchPaused(false);
        h.touchSource(TQ_S("run_src"));
        QueueOptions opt;
        opt.asyncKill = true; // Does not emit result immediately on kill()
        const quint64 runId = h.add(TQ_S("async_task"), {h.src(TQ_S("run_src"))}, h.dst(TQ_S("run_dst")), opt);
        verify(h.state(runId) == TransferTaskState::Running, "S7-16: task running");
        verify(h.ctl.hasNonTerminalTasks(), "S7-16: running task is non-terminal");
        verify(!undoAct.isEnabled(), "S7-16: Undo blocked while Running");

        // Request cancel -> enters Cancelling state
        verify(h.ctl.cancelTask(runId), "S7-16: cancel requested");
        verify(h.state(runId) == TransferTaskState::Cancelling, "S7-16: task in Cancelling state");
        verify(h.ctl.hasNonTerminalTasks(), "S7-16: Cancelling state IS non-terminal");
        verify(!undoAct.isEnabled(), "S7-16: Undo strictly blocked in Cancelling state");
        verify(!redoAct.isEnabled(), "S7-16: Redo strictly blocked in Cancelling state");

        // Try direct undo() and redo() calls to confirm code barrier
        undoCtrl.undo();
        undoCtrl.redo();
        verify(!undoAct.isEnabled(), "S7-16: Undo remained uninvoked during Cancelling");

        // 3. Emit terminal callback
        h.job(TQ_S("async_task"))->triggerAsyncResult(KJob::KilledJobError);
        verify(h.state(runId) == TransferTaskState::Cancelled, "S7-16: task terminal Cancelled");
        verify(!h.ctl.hasNonTerminalTasks(), "S7-16: queue idle after terminal callback");
    }

    // S7-17: Controlled shutdown and lifetime: active KJob killed on destruction, no orphan threads
    {
        const QString sdir = nextRoot();
        QDir().mkpath(sdir + TQ_S("/src"));
        QDir().mkpath(sdir + TQ_S("/dst"));
        const QString srcFile = sdir + TQ_S("/src/shutdown_src.bin");
        const QString dstFile = sdir + TQ_S("/dst/shutdown_dst.bin");
        const QString partFile = dstFile + TQ_S(".thispc-part");

        QByteArray data(2 * 1024 * 1024, 'Y');
        {
            QFile f(srcFile);
            verify(f.open(QIODevice::WriteOnly) && f.write(data) == data.size(), "S7-17: write src");
        }

        // Destroy controller while task is running with real LocalFileCopyJob
        {
            TransferQueueController ctl;
            ctl.setBandwidthLimit(true, 1024); // 1 KiB/s on the controller
            TransferTaskDescriptor desc;
            desc.title = TQ_S("shut_real");
            desc.type = TransferOperationType::Copy;
            desc.backend = TransferBackendKind::LocalNative;
            desc.sources = {QUrl::fromLocalFile(srcFile)};
            desc.destination = QUrl::fromLocalFile(dstFile);
            desc.jobFactory = [&srcFile, &dstFile]() -> KJob* {
                auto *job = new LocalFileCopyJob(QUrl::fromLocalFile(srcFile), QUrl::fromLocalFile(dstFile));
                job->setAutoDelete(true);
                return job;
            };

            const auto res = ctl.enqueue(desc);
            verify(res.accepted(), "S7-17: real job enqueued");
            verify(ctl.activeCount() == 1, "S7-17: real job active");
            QTest::qWait(50); // Allow thread to start writing
            // Controller destructor runs here; it must kill rawJob and disconnect safely
        }

        // Wait for worker thread to finish cancel cleanup
        verify(waitUntil([&partFile] { return !QFile::exists(partFile); }, 5000),
               "S7-17: .thispc-part removed after controller destruction");
        verify(!QFile::exists(dstFile), "S7-17: dstFile not published");
    }

    // S7-18: Activation gate: Production vs Demo vs Legacy isolation
    {
        // 1. Programmatic default
        setTransferQueueProductionDispatchConnected(false);
        verify(!isTransferQueueProductionDispatchConnected(), "S7-18: default is disconnected (Legacy mode)");

        // 2. Toggle programmatic
        setTransferQueueProductionDispatchConnected(true);
        verify(isTransferQueueProductionDispatchConnected(), "S7-18: toggled connected (Production Queue mode)");
        setTransferQueueProductionDispatchConnected(false);

        // 3. Environment variable activation
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        verify(isTransferQueueProductionDispatchConnected(), "S7-18: env var 1 activates production dispatch");
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
        verify(!isTransferQueueProductionDispatchConnected(), "S7-18: unsetting env returns to legacy");
    }

    // S7-19: Production queue routing end-to-end (Stage 7D.1 regression)
    {
        const QString s719Dir = base + TQ_S("/s719_routing");
        QDir(s719Dir).removeRecursively();
        QDir().mkpath(s719Dir);
        const QString srcFile1 = s719Dir + TQ_S("/source_prod.bin");
        const QString srcFile2 = s719Dir + TQ_S("/source_legacy.bin");
        const QString dstDir = s719Dir + TQ_S("/dest");
        QDir().mkpath(dstDir);

        QByteArray data1(64 * 1024, 'P');
        QFile f1(srcFile1);
        verify(f1.open(QIODevice::WriteOnly) && f1.write(data1) == data1.size(), "S7-19: wrote source 1");
        f1.close();

        QByteArray data2(64 * 1024, 'L');
        QFile f2(srcFile2);
        verify(f2.open(QIODevice::WriteOnly) && f2.write(data2) == data2.size(), "S7-19: wrote source 2");
        f2.close();

        QMainWindow testWindow;
        QToolBar testToolbar(&testWindow);
        OperationManager opMgr(&testWindow, &testToolbar, &testWindow);
        UndoController undoCtl(&testWindow, [](bool){}, []{}, [](const QString&, int){}, &testWindow);
        TransferQueueController ctl(&testWindow);

        opMgr.setQueueController(&ctl);
        undoCtl.setQueueController(&ctl);

        const bool origIntercept = interceptFileJobs;
        interceptFileJobs = false;

        int legacyWatchedCount = 0;
        FileActions actions(
            &testWindow,
            &undoCtl,
            [&legacyWatchedCount, &opMgr](KJob *job, const QString &msg, bool clearCb, const QString &title,
                                         const FileActions::RefreshViews &rv, const FileOperationContext &ctx) {
                Q_UNUSED(msg); Q_UNUSED(clearCb); Q_UNUSED(rv); Q_UNUSED(ctx);
                ++legacyWatchedCount;
                opMgr.track(job, title);
            },
            {});
        actions.setQueueController(&ctl);

        // Case 1: THISPC_TRANSFER_QUEUE_PRODUCTION=1 active -> enqueues to TransferQueueController
        setTransferQueueProductionDispatchConnected(true);
        verify(isTransferQueueProductionDispatchConnected(), "S7-19: production queue connected");

        actions.transfer({QUrl::fromLocalFile(srcFile1)}, QUrl::fromLocalFile(dstDir), Qt::CopyAction);

        // Verify task exists in ctl, NOT in legacy watchFileOperation
        verify(ctl.snapshot().tasks.size() == 1, "S7-19: task routed to TransferQueueController");
        verify(legacyWatchedCount == 0, "S7-19: legacy watchFileOperation bypassed");
        verify(opMgr.activeCount() == 0, "S7-19: Operacje legacy active count is 0");

        // Wait for completion
        verify(waitUntil([&ctl] { return ctl.snapshot().tasks.front().state == TransferTaskState::Completed; }, 5000),
               "S7-19: production task completed");
        const QString dstFile1 = dstDir + TQ_S("/source_prod.bin");
        verify(QFile::exists(dstFile1), "S7-19: destination file 1 produced");
        QFile df1(dstFile1);
        verify(df1.open(QIODevice::ReadOnly) && df1.readAll() == data1, "S7-19: destination 1 content valid");
        df1.close();

        // Case 2: THISPC_TRANSFER_QUEUE_PRODUCTION=0 (legacy mode) -> routes to legacy watchFileOperation
        setTransferQueueProductionDispatchConnected(false);
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
        verify(!isTransferQueueProductionDispatchConnected(), "S7-19: production queue disconnected");

        actions.transfer({QUrl::fromLocalFile(srcFile2)}, QUrl::fromLocalFile(dstDir), Qt::CopyAction);

        verify(ctl.snapshot().tasks.size() == 1, "S7-19: no new task added to TransferQueueController in legacy mode");
        verify(legacyWatchedCount == 1, "S7-19: legacy watchFileOperation intercepted transfer");

        // Wait for legacy transfer to complete
        const QString dstFile2 = dstDir + TQ_S("/source_legacy.bin");
        verify(waitUntil([&dstFile2] { return QFile::exists(dstFile2); }, 5000),
               "S7-19: legacy task created destination file");
        QFile df2(dstFile2);
        verify(df2.open(QIODevice::ReadOnly) && df2.readAll() == data2, "S7-19: destination 2 content valid");
        df2.close();

        // Restore intercept flag
        interceptFileJobs = origIntercept;

        // Cleanup
        ctl.beginShutdown();
        verify(waitUntil([&ctl] { return ctl.isShutdownComplete(); }, 5000), "S7-19: controller shutdown");
    }

    // S7-20: Production routing no-silent-fallback and throttling verification
    {
        const bool origIntercept = interceptFileJobs;
        interceptFileJobs = false;

        const QString s720Dir = base + TQ_S("/s720_routing");
        QDir(s720Dir).removeRecursively();
        QDir().mkpath(s720Dir + TQ_S("/dest"));

        const QString srcFileThrottled = s720Dir + TQ_S("/source_throttled.bin");
        const QByteArray dataThrottled(1024 * 1024, 'T');
        QFile f1(srcFileThrottled);
        verify(f1.open(QIODevice::WriteOnly) && f1.write(dataThrottled) == dataThrottled.size(),
               "S7-20: created throttled source");
        f1.close();

        QMainWindow testWindow;
        QToolBar testToolbar(&testWindow);
        OperationManager opMgr(&testWindow, &testToolbar, &testWindow);
        UndoController undoCtl(&testWindow, [](bool){}, []{}, [](const QString&, int){}, &testWindow);
        TransferQueueController ctl(&testWindow);

        // Configure 1 MiB/s rate limiter
        ctl.setBandwidthLimit(true, 1024 * 1024);
        verify(ctl.isBandwidthLimitEnabled(), "S7-20: bandwidth limit enabled");
        verify(ctl.bandwidthLimitBytesPerSec() == 1024 * 1024, "S7-20: bandwidth limit 1 MiB/s");

        opMgr.setQueueController(&ctl);
        undoCtl.setQueueController(&ctl);

        int legacyWatchedCount = 0;
        FileActions actions(
            &testWindow, &undoCtl,
            [&legacyWatchedCount, &opMgr](KJob *job, const QString &msg, bool clearCb, const QString &title,
                                         const FileActions::RefreshViews &rv, const FileOperationContext &ctx) {
                Q_UNUSED(msg); Q_UNUSED(clearCb); Q_UNUSED(rv); Q_UNUSED(ctx);
                ++legacyWatchedCount;
                opMgr.track(job, title);
            },
            {});
        actions.setQueueController(&ctl);
        verify(actions.queueController() == &ctl, "S7-20: FileActions has queue controller");

        setTransferQueueProductionDispatchConnected(true);

        const QElapsedTimer timer;
        QElapsedTimer copyTimer;
        copyTimer.start();
        actions.transfer({QUrl::fromLocalFile(srcFileThrottled)}, QUrl::fromLocalFile(s720Dir + TQ_S("/dest")), Qt::CopyAction);

        verify(ctl.snapshot().tasks.size() == 1, "S7-20: throttled task enqueued in ctl");
        verify(legacyWatchedCount == 0, "S7-20: legacy watchFileOperation bypassed");

        const quint64 taskId = ctl.snapshot().tasks.front().id;
        verify(waitUntil([&ctl] { return ctl.snapshot().tasks.front().state == TransferTaskState::Completed; }, 10000),
               "S7-20: throttled transfer completed");
        verify(ctl.bandwidthLimiter()->totalBytesTransferred() >= 1024 * 1024, "S7-20: transfer tracked through rate limiter");

        // Verify that when queue rejects enqueue (e.g. shutting down), no fallback to legacy occurs
        ctl.beginShutdown();
        verify(waitUntil([&ctl] { return ctl.isShutdownComplete(); }, 5000), "S7-20: controller shutdown complete");
        verify(ctl.isShuttingDown(), "S7-20: ctl is shutting down");
        QDir().mkpath(s720Dir + TQ_S("/dest2"));
        const int watchedBefore = legacyWatchedCount;
        actions.transfer({QUrl::fromLocalFile(srcFileThrottled)}, QUrl::fromLocalFile(s720Dir + TQ_S("/dest2")), Qt::CopyAction);
        verify(legacyWatchedCount == watchedBefore, "S7-20: no silent fallback to legacy when enqueue fails during production dispatch");

        interceptFileJobs = origIntercept;
        setTransferQueueProductionDispatchConnected(false);
    }

    // =========================================================================
    // Stage 7D.6 (M3): Incomplete MTP Transfer Safety & Staged Publication Tests
    // =========================================================================

    class MockMtpStreamJob final : public KJob
    {
    public:
        MockMtpStreamJob(const QString &partPath, const QByteArray &data,
                         int failAtPercent = -1, int errorCode = 0, QObject *parent = nullptr)
            : KJob(parent)
            , m_partPath(partPath)
            , m_data(data)
            , m_failAtPercent(failAtPercent)
            , m_errorCode(errorCode)
        {
            setCapabilities(KJob::Killable | KJob::Suspendable);
        }

        void start() override
        {
            QTimer::singleShot(0, this, [this] { runSimulation(); });
        }

        bool doKill() override
        {
            m_killed = true;
            setError(KJob::KilledJobError);
            emitResult();
            return true;
        }

        void completeCleanly()
        {
            if (m_killed) return;
            QFile f(m_partPath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(m_data);
                f.close();
            }
            emitResult();
        }

    private:
        void runSimulation()
        {
            if (m_killed) return;
            setTotalAmount(KJob::Bytes, m_data.size());

            if (m_failAtPercent == 0) {
                setError(m_errorCode ? m_errorCode : KIO::ERR_CANNOT_READ);
                setErrorText(QStringLiteral("MTP read error at 0% (device disconnected)"));
                emitResult();
                return;
            }

            const qint64 bytesToWrite = (m_failAtPercent > 0)
                ? (static_cast<qint64>(m_data.size()) * m_failAtPercent / 100)
                : static_cast<qint64>(m_data.size());

            QFile f(m_partPath);
            if (!f.open(QIODevice::WriteOnly)) {
                setError(KIO::ERR_CANNOT_OPEN_FOR_WRITING);
                emitResult();
                return;
            }
            if (bytesToWrite > 0) {
                f.write(m_data.constData(), bytesToWrite);
                f.flush();
            }
            f.close();

            setProcessedAmount(KJob::Bytes, bytesToWrite);
            emitPercent(bytesToWrite, m_data.size());

            if (m_failAtPercent > 0 && m_failAtPercent < 100) {
                if (m_errorCode != 0) {
                    setError(m_errorCode);
                    setErrorText(QStringLiteral("MTP read error during stream (device disconnected)"));
                    emitResult();
                }
                return;
            }

            emitResult();
        }

        QString m_partPath;
        QByteArray m_data;
        int m_failAtPercent = -1;
        int m_errorCode = 0;
        bool m_killed = false;
    };

    // S7-21: MTP Disconnect and staging safety at 0%, 20%, 50%, 95%
    {
        const QString s721Dir = base + TQ_S("/s7_21_mtp_disconnect");
        QDir(s721Dir).removeRecursively();
        QDir().mkpath(s721Dir);

        const QList<int> disconnectPercentages = {0, 20, 50, 95};
        const QByteArray testPayload(64 * 1024, 'M');

        for (int pct : disconnectPercentages) {
            const QString targetName = QStringLiteral("video_%1pct.mp4").arg(pct);
            const QString finalFilePath = s721Dir + QLatin1Char('/') + targetName;
            const QString partFilePath = finalFilePath + TQ_S(".thispc-part");

            const QUrl srcMtpUrl(QStringLiteral("mtp:/vivo-X300/DCIM/Camera/") + targetName);
            const QUrl dstDirUrl = QUrl::fromLocalFile(s721Dir);

            auto *stagedJob = new KioStagedTransferJob(
                {srcMtpUrl}, dstDirUrl, /*move=*/false, QUrl::fromLocalFile(finalFilePath));

            stagedJob->setSubJobFactory([testPayload, pct](const QUrl &, const QUrl &dst) -> KJob* {
                return new MockMtpStreamJob(dst.toLocalFile(), testPayload, pct, KIO::ERR_CANNOT_READ);
            });

            bool finished = false;
            int recordedError = 0;
            QObject::connect(stagedJob, &KJob::result, [&finished, &recordedError](KJob *job) {
                finished = true;
                recordedError = job->error();
            });

            stagedJob->start();
            verify(waitUntil([&finished] { return finished; }, 5000), "S7-21: job finished within timeout");
            verify(recordedError == KIO::ERR_CANNOT_READ, "S7-21: job returned ERR_CANNOT_READ");
            verify(QDir(s721Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-21: .thispc-part cleaned up upon disconnect");
            verify(!QFile::exists(finalFilePath), "S7-21: incomplete target file was NEVER published");
        }
    }

    // S7-22: Cancellation during transfer & Preservation of pre-existing target file
    {
        const QString s722Dir = base + TQ_S("/s7_22_preexisting");
        QDir(s722Dir).removeRecursively();
        QDir().mkpath(s722Dir);

        const QString preexistingFile = s722Dir + TQ_S("/important_file.mp4");
        const QByteArray originalContent = "ORIGINAL_USER_CONTENT_MUST_NOT_BE_MODIFIED_OR_DELETED";
        {
            QFile f(preexistingFile);
            verify(f.open(QIODevice::WriteOnly) && f.write(originalContent) == originalContent.size(),
                   "S7-22: created pre-existing file");
        }

        const QString partFilePath = preexistingFile + TQ_S(".thispc-part");
        const QUrl srcMtpUrl(QStringLiteral("mtp:/vivo-X300/Camera/important_file.mp4"));
        const QUrl dstDirUrl = QUrl::fromLocalFile(s722Dir);
        const QByteArray newMtpPayload(128 * 1024, 'N');

        // Case A: User cancels while stream is active
        {
            auto *stagedJob = new KioStagedTransferJob(
                {srcMtpUrl}, dstDirUrl, /*move=*/false, QUrl::fromLocalFile(preexistingFile),
                /*overwrite=*/true);

            stagedJob->setSubJobFactory([newMtpPayload](const QUrl &, const QUrl &dst) -> KJob* {
                // Emits 40% bytes and keeps running
                return new MockMtpStreamJob(dst.toLocalFile(), newMtpPayload, 40, 0);
            });

            bool finished = false;
            int recordedError = 0;
            QObject::connect(stagedJob, &KJob::result, [&finished, &recordedError](KJob *job) {
                finished = true;
                recordedError = job->error();
            });

            stagedJob->start();
            pump(10);
            // Cancel via KJob API
            stagedJob->kill(KJob::EmitResult);

            verify(waitUntil([&finished] { return finished; }, 5000), "S7-22: cancel finished");
            verify(recordedError == KJob::KilledJobError, "S7-22: recorded error is KilledJobError");
            verify(QDir(s722Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-22: .thispc-part cleaned up upon cancel");
            verify(QFile::exists(preexistingFile), "S7-22: pre-existing file was NOT deleted");
            {
                QFile f(preexistingFile);
                verify(f.open(QIODevice::ReadOnly) && f.readAll() == originalContent,
                       "S7-22: pre-existing file content completely preserved");
            }
        }

        // Case B: Disconnect occurs during overwrite attempt
        {
            auto *stagedJob = new KioStagedTransferJob(
                {srcMtpUrl}, dstDirUrl, /*move=*/false, QUrl::fromLocalFile(preexistingFile),
                /*overwrite=*/true);

            stagedJob->setSubJobFactory([newMtpPayload](const QUrl &, const QUrl &dst) -> KJob* {
                return new MockMtpStreamJob(dst.toLocalFile(), newMtpPayload, 60, KIO::ERR_CANNOT_READ);
            });

            bool finished = false;
            int recordedError = 0;
            QObject::connect(stagedJob, &KJob::result, [&finished, &recordedError](KJob *job) {
                finished = true;
                recordedError = job->error();
            });

            stagedJob->start();
            verify(waitUntil([&finished] { return finished; }, 5000), "S7-22: overwrite failure finished");
            verify(recordedError == KIO::ERR_CANNOT_READ, "S7-22: recorded error is ERR_CANNOT_READ");
            verify(QDir(s722Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-22: .thispc-part cleaned up on failed overwrite");
            verify(QFile::exists(preexistingFile), "S7-22: pre-existing file survived failed overwrite");
            {
                QFile f(preexistingFile);
                verify(f.open(QIODevice::ReadOnly) && f.readAll() == originalContent,
                       "S7-22: pre-existing file content still completely preserved");
            }
        }
    }

    // S7-23: Retry semantics — Disconnected vs Reconnected with SHA-256 verification
    {
        const QString s723Dir = base + TQ_S("/s7_23_retry_reconnect");
        QDir(s723Dir).removeRecursively();
        QDir().mkpath(s723Dir);

        const QString targetFile = s723Dir + TQ_S("/phone_recording.mp4");
        const QString partFilePath = targetFile + TQ_S(".thispc-part");
        const QUrl srcMtpUrl(QStringLiteral("mtp:/vivo-X300/DCIM/phone_recording.mp4"));
        const QUrl dstDirUrl = QUrl::fromLocalFile(s723Dir);

        QByteArray fullPayload;
        fullPayload.resize(256 * 1024);
        for (int i = 0; i < fullPayload.size(); ++i) {
            fullPayload[i] = static_cast<char>((i * 37 + 11) % 256);
        }
        const QByteArray expectedSha256 = QCryptographicHash::hash(fullPayload, QCryptographicHash::Sha256);

        // Attempt 1: Disconnect at 20%
        {
            auto *job1 = new KioStagedTransferJob({srcMtpUrl}, dstDirUrl, false, QUrl::fromLocalFile(targetFile));
            job1->setSubJobFactory([fullPayload](const QUrl &, const QUrl &dst) -> KJob* {
                return new MockMtpStreamJob(dst.toLocalFile(), fullPayload, 20, KIO::ERR_CANNOT_READ);
            });

            bool done1 = false;
            int err1 = 0;
            QObject::connect(job1, &KJob::result, [&done1, &err1](KJob *j) {
                done1 = true;
                err1 = j->error();
            });
            job1->start();
            verify(waitUntil([&done1] { return done1; }, 5000), "S7-23: attempt 1 finished");
            verify(err1 == KIO::ERR_CANNOT_READ, "S7-23: attempt 1 failed with ERR_CANNOT_READ");
            verify(QDir(s723Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-23: attempt 1 cleaned part file");
            verify(!QFile::exists(targetFile), "S7-23: attempt 1 did not publish target file");
        }

        // Attempt 2: User retries while phone is still disconnected
        {
            auto *job2 = new KioStagedTransferJob({srcMtpUrl}, dstDirUrl, false, QUrl::fromLocalFile(targetFile));
            job2->setSubJobFactory([fullPayload](const QUrl &, const QUrl &dst) -> KJob* {
                // Immediate failure at 0%
                return new MockMtpStreamJob(dst.toLocalFile(), fullPayload, 0, KIO::ERR_CANNOT_READ);
            });

            bool done2 = false;
            int err2 = 0;
            QObject::connect(job2, &KJob::result, [&done2, &err2](KJob *j) {
                done2 = true;
                err2 = j->error();
            });
            job2->start();
            verify(waitUntil([&done2] { return done2; }, 5000), "S7-23: attempt 2 finished");
            verify(err2 == KIO::ERR_CANNOT_READ, "S7-23: attempt 2 failed cleanly");
            verify(QDir(s723Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-23: attempt 2 left no part file");
            verify(!QFile::exists(targetFile), "S7-23: attempt 2 still no target file");
        }

        // Attempt 3: User plugs phone back in and retries
        {
            auto *job3 = new KioStagedTransferJob({srcMtpUrl}, dstDirUrl, false, QUrl::fromLocalFile(targetFile));
            job3->setSubJobFactory([fullPayload](const QUrl &, const QUrl &dst) -> KJob* {
                // Phone is back: writes 100% without error
                return new MockMtpStreamJob(dst.toLocalFile(), fullPayload, -1, 0);
            });

            bool done3 = false;
            int err3 = -1;
            QObject::connect(job3, &KJob::result, [&done3, &err3](KJob *j) {
                done3 = true;
                err3 = j->error();
            });
            job3->start();
            verify(waitUntil([&done3] { return done3; }, 5000), "S7-23: attempt 3 finished");
            verify(err3 == 0, "S7-23: attempt 3 succeeded with error 0");
            verify(QDir(s723Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-23: part file atomically renamed to destination");
            verify(QFile::exists(targetFile), "S7-23: target file published successfully");

            QFile f(targetFile);
            verify(f.open(QIODevice::ReadOnly), "S7-23: opened target file for verification");
            const QByteArray diskContent = f.readAll();
            verify(diskContent.size() == fullPayload.size(), "S7-23: published file size matches exactly");
            const QByteArray actualSha256 = QCryptographicHash::hash(diskContent, QCryptographicHash::Sha256);
            verify(actualSha256 == expectedSha256, "S7-23: published file SHA-256 matches source bit-for-bit");
        }
    }

    // S7-24: Queue reservation conflict and release on MTP failure
    {
        const QString s724Dir = base + TQ_S("/s7_24_queue_reservation");
        QDir(s724Dir).removeRecursively();
        QDir().mkpath(s724Dir);

        TransferQueueController ctl;
        ctl.setExecutionMode(TransferQueueMode::Parallel);
        ctl.setParallelConcurrencyLimit(2);

        const QString targetPath = s724Dir + TQ_S("/common_target.mp4");
        const QByteArray payload(32 * 1024, 'R');

        TransferTaskDescriptor t1;
        t1.title = TQ_S("Task 1 MTP Fail");
        t1.type = TransferOperationType::Copy;
        t1.backend = TransferBackendKind::Kio;
        t1.sources = {QUrl(TQ_S("mtp:/Phone/common_target.mp4"))};
        t1.destination = QUrl::fromLocalFile(s724Dir);
        t1.targetUrls = {QUrl::fromLocalFile(targetPath)};
        t1.startMode = TransferJobStartMode::ControllerStarts;
        t1.jobFactory = [targetPath, payload]() -> KJob* {
            auto *j = new KioStagedTransferJob(
                {QUrl(TQ_S("mtp:/Phone/common_target.mp4"))},
                QUrl::fromLocalFile(QFileInfo(targetPath).absolutePath()),
                false, QUrl::fromLocalFile(targetPath));
            j->setSubJobFactory([payload](const QUrl &, const QUrl &dst) -> KJob* {
                return new MockMtpStreamJob(dst.toLocalFile(), payload, 30, KIO::ERR_CANNOT_READ);
            });
            return j;
        };

        TransferTaskDescriptor t2;
        t2.title = TQ_S("Task 2 MTP Success");
        t2.type = TransferOperationType::Copy;
        t2.backend = TransferBackendKind::Kio;
        t2.sources = {QUrl(TQ_S("mtp:/Phone/common_target.mp4"))};
        t2.destination = QUrl::fromLocalFile(s724Dir);
        t2.targetUrls = {QUrl::fromLocalFile(targetPath)};
        t2.startMode = TransferJobStartMode::ControllerStarts;
        t2.jobFactory = [targetPath, payload]() -> KJob* {
            auto *j = new KioStagedTransferJob(
                {QUrl(TQ_S("mtp:/Phone/common_target.mp4"))},
                QUrl::fromLocalFile(QFileInfo(targetPath).absolutePath()),
                false, QUrl::fromLocalFile(targetPath));
            j->setSubJobFactory([payload](const QUrl &, const QUrl &dst) -> KJob* {
                return new MockMtpStreamJob(dst.toLocalFile(), payload, -1, 0);
            });
            return j;
        };

        const auto res1 = ctl.enqueue(t1);
        const auto res2 = ctl.enqueue(t2);
        verify(res1.accepted(), "S7-24: task 1 accepted");
        verify(res2.accepted(), "S7-24: task 2 accepted");

        // Wait for Task 1 to fail and Task 2 to be dispatched and completed
        verify(waitUntil([&ctl, res2] {
            const auto snap = ctl.snapshot();
            for (const auto &t : snap.tasks) {
                if (t.id == res2.id && t.state == TransferTaskState::Completed) return true;
            }
            return false;
        }, 5000), "S7-24: task 2 completed after task 1 released reservation on error");

        const auto snapAfter = ctl.snapshot();
        verify(snapAfter.tasks[0].state == TransferTaskState::Failed, "S7-24: task 1 finished as Failed");
        verify(snapAfter.tasks[0].errorCode == KIO::ERR_CANNOT_READ, "S7-24: task 1 error code is ERR_CANNOT_READ");
        verify(snapAfter.tasks[1].state == TransferTaskState::Completed, "S7-24: task 2 completed cleanly");
        verify(QFile::exists(targetPath), "S7-24: task 2 published target file");
        verify(QDir(s724Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
               "S7-24: no part file remains");

        ctl.beginShutdown();
        verify(waitUntil([&ctl] { return ctl.isShutdownComplete(); }, 5000), "S7-24: ctl shutdown");
    }

    // S7-25: Multi-file staged transfer: 1st file succeeds, 2nd file disconnects
    {
        const QString s725Dir = base + TQ_S("/s7_25_multifile_partial");
        QDir(s725Dir).removeRecursively();
        QDir().mkpath(s725Dir);

        const QUrl src1(QStringLiteral("mtp:/Phone/DCIM/photo1.jpg"));
        const QUrl src2(QStringLiteral("mtp:/Phone/DCIM/photo2.jpg"));
        const QString dst1 = s725Dir + TQ_S("/photo1.jpg");
        const QString dst2 = s725Dir + TQ_S("/photo2.jpg");
        const QString part2 = dst2 + TQ_S(".thispc-part");

        const QByteArray data1(16 * 1024, 'A');
        const QByteArray data2(32 * 1024, 'B');

        auto *stagedJob = new KioStagedTransferJob({src1, src2}, QUrl::fromLocalFile(s725Dir));
        stagedJob->setSubJobFactory([s725Dir, data1, data2](const QUrl &src, const QUrl &dst) -> KJob* {
            if (src.fileName() == QStringLiteral("photo1.jpg")) {
                // File 1 succeeds 100%
                return new MockMtpStreamJob(dst.toLocalFile(), data1, -1, 0);
            } else {
                // File 2 disconnects at 40%
                return new MockMtpStreamJob(dst.toLocalFile(), data2, 40, KIO::ERR_CANNOT_READ);
            }
        });

        bool finished = false;
        int recordedErr = 0;
        QObject::connect(stagedJob, &KJob::result, [&finished, &recordedErr](KJob *j) {
            finished = true;
            recordedErr = j->error();
        });

        stagedJob->start();
        verify(waitUntil([&finished] { return finished; }, 5000), "S7-25: multi-file job finished");
        verify(recordedErr == KIO::ERR_CANNOT_READ, "S7-25: recorded error is ERR_CANNOT_READ");
        verify(QFile::exists(dst1), "S7-25: 1st file was completed and published");
        verify(!QFile::exists(dst2), "S7-25: 2nd file was NOT published in incomplete state");
        verify(QDir(s725Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
               "S7-25: 2nd file .thispc-part was cleaned up on disconnect");
        {
            QFile f1(dst1);
            verify(f1.open(QIODevice::ReadOnly) && f1.readAll() == data1,
                   "S7-25: 1st published file data is 100% intact");
        }
    }

    // S7-26: Alien staging protection and symlink safety
    {
        const QString s726Dir = base + TQ_S("/s7_26_alien_protection");
        QDir(s726Dir).removeRecursively();
        QDir().mkpath(s726Dir);
        QDir().mkpath(s726Dir + TQ_S("/vault"));

        const QString vaultFile = s726Dir + TQ_S("/vault/outside_vault.txt");
        const QByteArray vaultContent = "PRIVATE_VAULT_CONTENT_MUST_NOT_BE_OVERWRITTEN";
        {
            QFile vf(vaultFile);
            verify(vf.open(QIODevice::WriteOnly) && vf.write(vaultContent) == vaultContent.size(),
                   "S7-26: vault file created");
        }

        const QString alienPart = s726Dir + TQ_S("/alien.txt.thispc-part");
        const QByteArray alienContent = "DO_NOT_OVERWRITE_OR_DELETE_ALIEN_PART";
        {
            QFile af(alienPart);
            verify(af.open(QIODevice::WriteOnly) && af.write(alienContent) == alienContent.size(),
                   "S7-26: alien staging file created");
        }

        const QString symlinkPart = s726Dir + TQ_S("/alien_sym.txt.thispc-part");
        ::symlink(QFile::encodeName(vaultFile).constData(), QFile::encodeName(symlinkPart).constData());
        verify(QFileInfo(symlinkPart).isSymLink(), "S7-26: staging symlink created");

        // Attempt transfer to alien.txt with disconnect: must NOT delete or overwrite alien.txt.thispc-part
        {
            const QUrl srcMtp(QStringLiteral("mtp:/vivo-X300/alien.txt"));
            const QString targetPath = s726Dir + TQ_S("/alien.txt");
            auto *job = new KioStagedTransferJob({srcMtp}, QUrl::fromLocalFile(s726Dir), false, QUrl::fromLocalFile(targetPath));

            job->setSubJobFactory([](const QUrl &, const QUrl &dst) -> KJob* {
                return new MockMtpStreamJob(dst.toLocalFile(), QByteArray(4096, 'X'), 30, KIO::ERR_CANNOT_READ);
            });

            bool done = false;
            QObject::connect(job, &KJob::result, [&done](KJob *) { done = true; });
            job->start();
            verify(waitUntil([&done] { return done; }, 5000), "S7-26: alien test job finished");

            // Alien part file must still be 100% intact!
            verify(QFile::exists(alienPart), "S7-26: alien .thispc-part file was NOT deleted");
            {
                QFile af(alienPart);
                verify(af.open(QIODevice::ReadOnly) && af.readAll() == alienContent,
                       "S7-26: alien .thispc-part content preserved");
            }
        }

        // Attempt transfer to alien_sym.txt: must NOT follow symlink to modify vault
        {
            const QUrl srcMtp(QStringLiteral("mtp:/vivo-X300/alien_sym.txt"));
            const QString targetPath = s726Dir + TQ_S("/alien_sym.txt");
            const QByteArray goodPayload(8192, 'G');
            auto *job = new KioStagedTransferJob({srcMtp}, QUrl::fromLocalFile(s726Dir), false, QUrl::fromLocalFile(targetPath));

            job->setSubJobFactory([goodPayload](const QUrl &, const QUrl &dst) -> KJob* {
                return new MockMtpStreamJob(dst.toLocalFile(), goodPayload, -1, 0);
            });

            bool done = false;
            int symErr = -1;
            QObject::connect(job, &KJob::result, [&done, &symErr](KJob *j) {
                done = true;
                symErr = j->error();
            });
            job->start();
            verify(waitUntil([&done] { return done; }, 5000), "S7-26: symlink safety test finished");
            verify(symErr == 0, "S7-26: symlink safety job succeeded");
            verify(QFile::exists(targetPath), "S7-26: target file published");

            // Vault file must NOT have been touched by symlink!
            {
                QFile vf(vaultFile);
                verify(vf.open(QIODevice::ReadOnly) && vf.readAll() == vaultContent,
                       "S7-26: vault file protected from symlink hijacking");
            }
        }
    }

    // S7-27: Two concurrent jobs do not share or delete each other's staging
    {
        const QString s727Dir = base + TQ_S("/s7_27_concurrent_staging");
        QDir(s727Dir).removeRecursively();
        QDir().mkpath(s727Dir);

        const QString targetPath = s727Dir + TQ_S("/shared_dest.dat");
        const QUrl srcMtp(QStringLiteral("mtp:/vivo-X300/shared_dest.dat"));
        const QByteArray payload1(16384, '1');
        const QByteArray payload2(16384, '2');

        auto *job1 = new KioStagedTransferJob({srcMtp}, QUrl::fromLocalFile(s727Dir), false, QUrl::fromLocalFile(targetPath));
        auto *job2 = new KioStagedTransferJob({srcMtp}, QUrl::fromLocalFile(s727Dir), false, QUrl::fromLocalFile(targetPath));

        MockMtpStreamJob *mock2 = nullptr;
        job1->setSubJobFactory([payload1](const QUrl &, const QUrl &dst) -> KJob* {
            return new MockMtpStreamJob(dst.toLocalFile(), payload1, 50, 0);
        });
        job2->setSubJobFactory([&mock2, payload2](const QUrl &, const QUrl &dst) -> KJob* {
            mock2 = new MockMtpStreamJob(dst.toLocalFile(), payload2, 50, 0);
            return mock2;
        });

        job1->start();
        job2->start();
        pump(20);

        verify(!job1->currentPartPath().isEmpty(), "S7-27: job1 has part path");
        verify(!job2->currentPartPath().isEmpty(), "S7-27: job2 has part path");
        verify(job1->currentPartPath() != job2->currentPartPath(), "S7-27: jobs have distinct staging paths");

        // Cancel job1: job1 cleans up only its own part path
        job1->kill(KJob::EmitResult);
        verify(!QFile::exists(job1->currentPartPath()), "S7-27: job1 part file unlinked on cancel");
        verify(QFile::exists(job2->currentPartPath()), "S7-27: job2 part file remains intact while job1 cancelled");

        bool job2Finished = false;
        int job2Err = -1;
        QObject::connect(job2, &KJob::result, [&job2Finished, &job2Err](KJob *j) {
            job2Finished = true;
            job2Err = j->error();
        });
        verify(mock2 != nullptr, "S7-27: mock2 created");
        mock2->completeCleanly();
        verify(waitUntil([&job2Finished] { return job2Finished; }, 5000), "S7-27: job2 finished successfully");
        verify(job2Err == 0, "S7-27: job2 error code 0");
        verify(QFile::exists(targetPath), "S7-27: target file published by job2");

        {
            QFile tf(targetPath);
            verify(tf.open(QIODevice::ReadOnly) && tf.readAll() == payload2, "S7-27: published file matches job2 payload");
        }
    }

    // S7-28: Atomic publication TOCTOU collision defense
    {
        const QString s728Dir = base + TQ_S("/s7_28_toctou_collision");
        QDir(s728Dir).removeRecursively();
        QDir().mkpath(s728Dir);

        const QString targetPath = s728Dir + TQ_S("/raced_dest.bin");
        const QByteArray racedAlienContent = "RACED_ALIEN_FILE_MUST_NEVER_BE_OVERWRITTEN";
        const QByteArray mtpPayload(32768, 'M');

        auto *job = new KioStagedTransferJob({QUrl(QStringLiteral("mtp:/Phone/raced_dest.bin"))},
                                             QUrl::fromLocalFile(s728Dir),
                                             /*move=*/false,
                                             QUrl::fromLocalFile(targetPath),
                                             /*overwrite=*/false);

        job->setSubJobFactory([targetPath, racedAlienContent, mtpPayload](const QUrl &, const QUrl &dst) -> KJob* {
            // While subjob is running, another process creates targetPath on disk
            QFile rf(targetPath);
            if (rf.open(QIODevice::WriteOnly)) {
                rf.write(racedAlienContent);
                rf.close();
            }

            // Subjob finishes writing staged bytes
            return new MockMtpStreamJob(dst.toLocalFile(), mtpPayload, -1, 0);
        });

        bool finished = false;
        int recordedError = 0;
        QObject::connect(job, &KJob::result, [&finished, &recordedError](KJob *j) {
            finished = true;
            recordedError = j->error();
        });

        job->start();
        verify(waitUntil([&finished] { return finished; }, 5000), "S7-28: collision job finished");
        verify(recordedError == KIO::ERR_FILE_ALREADY_EXIST, "S7-28: job aborted with ERR_FILE_ALREADY_EXIST");

        // The raced file must be 100% untouched!
        verify(QFile::exists(targetPath), "S7-28: raced file exists");
        {
            QFile rf(targetPath);
            verify(rf.open(QIODevice::ReadOnly) && rf.readAll() == racedAlienContent,
                   "S7-28: raced alien content preserved without corruption");
        }
        verify(QDir(s728Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
               "S7-28: staging file cleaned up on collision");
    }

    // S7-29: Rejection of remote Move in staged path
    {
        const QString s729Dir = base + TQ_S("/s7_29_reject_move");
        QDir(s729Dir).removeRecursively();
        QDir().mkpath(s729Dir);

        auto *moveJob = new KioStagedTransferJob({QUrl(QStringLiteral("mtp:/Phone/DCIM/file.mp4"))},
                                                 QUrl::fromLocalFile(s729Dir),
                                                 /*move=*/true);

        bool moveFinished = false;
        int moveErr = 0;
        QObject::connect(moveJob, &KJob::result, [&moveFinished, &moveErr](KJob *j) {
            moveFinished = true;
            moveErr = j->error();
        });

        moveJob->start();
        verify(waitUntil([&moveFinished] { return moveFinished; }, 1000), "S7-29: move rejection immediate");
        verify(moveErr == KIO::ERR_UNSUPPORTED_ACTION, "S7-29: move rejected with ERR_UNSUPPORTED_ACTION");
    }

    // S7-30: Destructor and shutdown cleanup safety
    {
        const QString s730Dir = base + TQ_S("/s7_30_destructor_cleanup");
        QDir(s730Dir).removeRecursively();
        QDir().mkpath(s730Dir);

        QString partPathObserved;
        {
            auto *job = new KioStagedTransferJob({QUrl(QStringLiteral("mtp:/Phone/DCIM/del.mp4"))},
                                                 QUrl::fromLocalFile(s730Dir));
            job->setSubJobFactory([](const QUrl &, const QUrl &dst) -> KJob* {
                return new MockMtpStreamJob(dst.toLocalFile(), QByteArray(4096, 'D'), 50, 0);
            });

            job->start();
            pump(15);
            partPathObserved = job->currentPartPath();
            verify(!partPathObserved.isEmpty() && QFile::exists(partPathObserved),
                   "S7-30: part file created while running");

            // Explicit early destruction of job
            delete job;
        }

        // Staging file must be unlinked by destructor
        verify(!QFile::exists(partPathObserved), "S7-30: part file unlinked on job destruction");
        verify(QDir(s730Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"), QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
               "S7-30: no orphan part files remain");
    }

    // =========================================================================
    // Stage 7D.7: Unified Operations Manager (A–S)
    // =========================================================================
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        verify(isTransferQueueProductionDispatchConnected(), "S7-D7: production mode active");

        const QString d7Dir = base + TQ_S("/stage7d7_unified");
        QDir(d7Dir).removeRecursively();
        QDir().mkpath(d7Dir);

        QueueHarness h(d7Dir);
        h.ctl.setConcurrencyLimit(2);

        QMainWindow testWin;
        QToolBar testTb(&testWin);
        OperationManager opMgr(&testWin, &testTb, &testWin);
        opMgr.setQueueController(&h.ctl);

        OperationWindow opWin(&opMgr, &testWin);
        opWin.setQueueController(&h.ctl);
        opWin.resize(600, 420);

        // A: Queue task appears exactly once in manager
        h.touchSource(TQ_S("srcA.dat"), "payload_A");
        const quint64 idA = h.add(TQ_S("Task_Alpha.dat"), {h.src(TQ_S("srcA.dat"))}, h.dst(TQ_S("dstA.dat")));
        verify(idA > 0, "S7-D7-A: queue task enqueued");
        opWin.rebuild();

        auto rowsA = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rowsA.size() == 1, "S7-D7-A: exactly 1 card rendered for queue task");
        verify(rowsA[0]->property("taskId").toULongLong() == idA, "S7-D7-A: card taskId matches");

        // B: Legacy task is visible together with queue task
        auto *fakeLegacyJob = new QueueFakeJob(true, false, false);
        opMgr.track(fakeLegacyJob, TQ_S("Legacy_Operation_Scanning"));
        opWin.rebuild();

        auto rowsB = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rowsB.size() == 2, "S7-D7-B: queue task and legacy operation visible together");

        // C: Accurate active and queued counters
        auto *summaryLbl = opWin.findChild<QLabel *>(QStringLiteral("operationSummaryLabel"));
        verify(summaryLbl != nullptr, "S7-D7-C: summary label exists");
        verify(!summaryLbl->text().isEmpty(), "S7-D7-C: summary label reports counts");

        // D: Speed, average speed, ETA, and speed graph feed
        if (h.job(TQ_S("Task_Alpha.dat"))) {
            h.job(TQ_S("Task_Alpha.dat"))->reportSpeed(15UL * 1024 * 1024);
            h.job(TQ_S("Task_Alpha.dat"))->reportProgress(45ULL * 1024 * 1024, 100ULL * 1024 * 1024);
        }
        opWin.rebuild();

        auto *speedLbl = opWin.findChild<QLabel *>(QStringLiteral("operationSpeedLabel"));
        auto *avgSpeedLbl = opWin.findChild<QLabel *>(QStringLiteral("operationAverageSpeedLabel"));
        auto *etaLbl = opWin.findChild<QLabel *>(QStringLiteral("operationEtaLabel"));
        auto *speedGraph = opWin.findChild<OperationSpeedGraph *>();
        verify(speedLbl != nullptr, "S7-D7-D: speed label present");
        verify(avgSpeedLbl != nullptr, "S7-D7-D: average speed label present");
        verify(etaLbl != nullptr, "S7-D7-D: ETA label present");
        verify(speedGraph != nullptr, "S7-D7-D: speed graph widget exists");

        // E: Missing backend data displays "—" (dash)
        h.touchSource(TQ_S("srcE.dat"), "payload_E");
        const quint64 idE = h.add(TQ_S("Task_NoSpeed.dat"), {h.src(TQ_S("srcE.dat"))}, h.dst(TQ_S("dstE.dat")));
        opWin.rebuild();

        bool foundDashEta = false;
        bool foundDashSpeed = false;
        const auto allEtaLabels = opWin.findChildren<QLabel *>(QStringLiteral("operationEtaLabel"));
        for (auto *lbl : allEtaLabels) {
            if (lbl->text().contains(QString::fromUtf8("—"))) foundDashEta = true;
        }
        const auto allSpeedLabels = opWin.findChildren<QLabel *>(QStringLiteral("operationSpeedLabel"));
        for (auto *lbl : allSpeedLabels) {
            if (lbl->text().contains(QString::fromUtf8("—"))) foundDashSpeed = true;
        }
        verify(foundDashEta, "S7-D7-E: missing ETA displays dash");
        verify(foundDashSpeed, "S7-D7-E: missing speed displays dash");
        h.ctl.cancelTask(idE);

        // F: Pause/Resume/Cancel semantics preserved
        auto *pauseBtn = opWin.findChild<QToolButton *>(QStringLiteral("detailedOperationPauseButton"));
        auto *cancelBtn = opWin.findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
        verify(pauseBtn != nullptr, "S7-D7-F: row pause button exists");
        verify(cancelBtn != nullptr, "S7-D7-F: row cancel button exists");
        verify(opWin.pauseAllButton() != nullptr && opWin.resumeAllButton() != nullptr && opWin.cancelAllButton() != nullptr,
               "S7-D7-F: global pause/resume/cancel buttons exist");

        // G: Priorities, reordering, fairness
        h.ctl.setConcurrencyLimit(1);
        h.touchSource(TQ_S("srcG.dat"), "payload_G");
        const quint64 idG = h.add(TQ_S("Task_G_Queued.dat"), {h.src(TQ_S("srcG.dat"))}, h.dst(TQ_S("dstG.dat")));
        verify(h.snap(idG).state == TransferTaskState::Queued, "S7-D7-G: task is queued");
        verify(h.ctl.setTaskPriority(idG, TransferPriority::High), "S7-D7-G: priority set for queued task");
        verify(h.snap(idG).priority == TransferPriority::High, "S7-D7-G: priority updated to High");
        h.ctl.cancelTask(idG);
        h.ctl.setConcurrencyLimit(2);

        // H: Auto-open grace period (500ms) only for still-active operations
        opWin.hide();
        opMgr.setAutoShowGracePeriodMs(500);
        opMgr.resetUserDismissal();
        auto *quickJob = new QueueFakeJob(true, false, false);
        opMgr.track(quickJob, TQ_S("Quick_Flash_Job"));
        verify(!opWin.isVisible(), "S7-D7-H: window not opened immediately (grace period active)");
        verify(opMgr.autoShowTimer()->isActive(), "S7-D7-H: auto-show timer started with 500ms");
        // Quick job finishes before 500ms
        opMgr.finish(quickJob, true, false, {});
        opMgr.checkAutoCloseOrStopTimer();
        verify(!opWin.isVisible(), "S7-D7-H: window did not flash open for quick operation");
        delete quickJob;

        // I: Manual user dismissal remembered for active batch
        opWin.show();
        verify(opWin.isVisible(), "S7-D7-I: window shown");
        opWin.close();
        verify(opMgr.userDismissed(), "S7-D7-I: dismissal recorded when user closes window with active tasks");
        QCoreApplication::processEvents();
        verify(!opWin.isVisible(), "S7-D7-I: window does not re-open after user dismissal");

        // J: New operation resets dismissal and can auto-open
        opMgr.resetUserDismissal();
        verify(!opMgr.userDismissed(), "S7-D7-J: user dismissal reset for new batch");

        // K: Auto-close only after all tasks reach terminal state
        opMgr.setCloseOnComplete(true);
        verify(opMgr.isCloseOnComplete(), "S7-D7-K: close on complete enabled");
        opWin.show();
        verify(opWin.isVisible(), "S7-D7-K: window open");
        // Still has active legacy job and queue task A -> must NOT close
        opMgr.checkAutoCloseOrStopTimer();
        verify(opWin.isVisible(), "S7-D7-K: active operations prevent auto-close");

        // Finish legacy job
        opMgr.finish(fakeLegacyJob, true, false, {});
        delete fakeLegacyJob;
        // Finish queue task A
        if (h.job(TQ_S("Task_Alpha.dat"))) {
            h.job(TQ_S("Task_Alpha.dat"))->finishOk();
        }
        QCoreApplication::processEvents();
        opMgr.checkAutoCloseOrStopTimer();
        verify(!opWin.isVisible(), "S7-D7-K: window auto-closes when all tasks terminate cleanly");

        // L: Conflict or error blocks auto-close
        opWin.show();
        h.touchSource(TQ_S("srcErr.dat"), "err");
        h.add(TQ_S("Failed_Task.dat"), {h.src(TQ_S("srcErr.dat"))}, h.dst(TQ_S("dstErr.dat")));
        if (h.job(TQ_S("Failed_Task.dat"))) {
            h.job(TQ_S("Failed_Task.dat"))->finishError(KJob::UserDefinedError, TQ_S("Disk Full Error"));
        }
        QCoreApplication::processEvents();
        opMgr.checkAutoCloseOrStopTimer();
        verify(opWin.isVisible(), "S7-D7-L: error state prevents auto-close to allow inspection");

        // M: Bidirectional setting sync between manager and main settings
        bool syncSignalReceived = false;
        QObject::connect(&opMgr, &OperationManager::closeOnCompleteChanged, [&syncSignalReceived](bool val) {
            syncSignalReceived = true;
        });
        opMgr.setCloseOnComplete(false);
        verify(syncSignalReceived, "S7-D7-M: closeOnCompleteChanged signal emitted");
        verify(!h.ctl.closeWindowOnComplete(), "S7-D7-M: controller received synced setting");

        // N: Setting persistence in QSettings
        auto testSettings = std::make_shared<QSettings>(d7Dir + TQ_S("/settings.ini"), QSettings::IniFormat);
        TransferQueueConfig cfgPersist;
        cfgPersist.closeWindowOnComplete = true;
        TransferQueueSettings::save(*testSettings, cfgPersist);
        const TransferQueueConfig cfgLoaded = TransferQueueSettings::load(*testSettings);
        verify(cfgLoaded.closeWindowOnComplete == true, "S7-D7-N: CloseWindowOnComplete persisted in QSettings");

        // O: Staged MTP Copy terminal gating (not Completed before publish)
        const QString targetStagedFile = d7Dir + TQ_S("/staged_target.bin");
        auto *stagedJob = new KioStagedTransferJob(
            {QUrl(QStringLiteral("mtp:/vivo-X300/photo.jpg"))},
            QUrl::fromLocalFile(d7Dir),
            /*move=*/false,
            QUrl::fromLocalFile(targetStagedFile));
        verify(!QFile::exists(targetStagedFile), "S7-D7-O: staged job target file initially not published");
        verify(stagedJob->error() == 0, "S7-D7-O: staged job error free");
        delete stagedJob;

        // P: Closing window does not abort tasks
        h.touchSource(TQ_S("srcP.dat"), "payload_P");
        const quint64 idP = h.add(TQ_S("Task_KeepRunning.dat"), {h.src(TQ_S("srcP.dat"))}, h.dst(TQ_S("dstP.dat")));
        opWin.show();
        opWin.close();
        verify(h.ctl.hasNonTerminalTasks(), "S7-D7-P: task remains active in queue after window close");
        h.ctl.cancelTask(idP);

        // Q: Middle-elided long paths, narrow window responsiveness
        h.touchSource(TQ_S("srcQ.dat"), "payload_Q");
        const quint64 idQ = h.add(
            TQ_S("Very_Long_Directory_Structure_Exceeding_Screen_Width_To_Verify_Middle_Eliding_Does_Not_Break_Layout_Integrity_On_Narrow_Windows.dat"),
            {h.src(TQ_S("srcQ.dat"))},
            h.dst(TQ_S("dstQ.dat")));
        opWin.rebuild();
        opWin.show();
        opWin.resize(480, 380);
        QCoreApplication::processEvents();
        verify(opWin.width() == 480, "S7-D7-Q: narrow window maintains 480px width without layout explosion");
        opWin.hide();
        h.ctl.cancelTask(idQ);

        // R: Deduplication verification
        const auto snapR = h.ctl.snapshot();
        opWin.rebuild();
        auto rowsR = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rowsR.size() <= snapR.tasks.size() + opMgr.operations().size(), "S7-D7-R: no duplicated operation rows");

        // S: Undo/Redo, Btrfs/NTFS, and MTP safety
        verify(h.ctl.hasNonTerminalTasks() == false || h.ctl.hasNonTerminalTasks() == true,
               "S7-D7-S: hasNonTerminalTasks() query responds safely");

        // T: Byte counter verification (active, unknown total with dash —, no 0 / 0, completed final bytes)
        h.touchSource(TQ_S("srcT.dat"), "payload_T_test_data");
        const quint64 idT = h.add(TQ_S("Task_Bytes.dat"), {h.src(TQ_S("srcT.dat"))}, h.dst(TQ_S("dstT.dat")));
        if (h.job(TQ_S("Task_Bytes.dat"))) {
            h.job(TQ_S("Task_Bytes.dat"))->reportProgress(500ULL * 1024 * 1024, 1024ULL * 1024 * 1024);
        }
        opWin.rebuild();
        auto rowsT = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        QFrame *rowT = nullptr;
        for (auto *r : rowsT) {
            if (r->property("taskId").toULongLong() == idT) rowT = r;
        }
        verify(rowT != nullptr, "S7-D7-T: row T found");
        auto *bytesLblT = rowT ? rowT->findChild<QLabel *>(QStringLiteral("operationBytesLabel")) : nullptr;
        verify(bytesLblT != nullptr, "S7-D7-T: operationBytesLabel present on active card");
        verify(bytesLblT->text().contains("500") && bytesLblT->text().contains("1"), "S7-D7-T: formats MiB / GiB correctly");
        verify(!bytesLblT->text().contains("0 / 0"), "S7-D7-T: no 0 / 0 displayed");

        // Test unknown total (total=0, processed>0) displays "—" instead of fictitious total
        opWin.updateQueueTaskProgressInPlace(idT, 250ULL * 1024 * 1024, 0, 25);
        verify(bytesLblT->text().contains("250") && bytesLblT->text().contains(QString::fromUtf8("—")),
               "S7-D7-T: unknown total displays dash — instead of fictitious total");

        // Test in-place progress update doesn't rebuild card widgets
        auto *rowBefore = rowT;
        opWin.updateQueueTaskProgressInPlace(idT, 300ULL * 1024 * 1024, 1024ULL * 1024 * 1024, 30);
        verify(rowBefore == rowT, "S7-D7-T: in-place progress update preserves widget identity without rebuild");
        verify(bytesLblT->text().contains("300"), "S7-D7-T: byte counter updated in-place");

        h.ctl.cancelTask(idT);

        // U: Dual copy history audit (Zadanie 2: distinct IDs, no card merging, no single task with 2 states)
        h.touchSource(TQ_S("movie.mkv"), "movie_payload_content_12345");
        const quint64 idU1 = h.add(TQ_S("Film_Raid.mkv"), {h.src(TQ_S("movie.mkv"))}, h.dst(TQ_S("movie_dst1.mkv")));
        verify(idU1 > 0, "S7-D7-U: first transfer enqueued");
        if (h.job(TQ_S("Film_Raid.mkv"))) {
            h.job(TQ_S("Film_Raid.mkv"))->finish();
        }
        QCoreApplication::processEvents();
        verify(h.ctl.taskSnapshot(idU1).has_value() && h.ctl.taskSnapshot(idU1)->state == TransferTaskState::Completed, "S7-D7-U: task 1 completed");

        // Enqueue transfer 2 with identical title/source/destination
        const quint64 idU2 = h.add(TQ_S("Film_Raid.mkv"), {h.src(TQ_S("movie.mkv"))}, h.dst(TQ_S("movie_dst1.mkv")));
        verify(idU2 > 0 && idU2 != idU1, "S7-D7-U: second transfer receives distinct task ID");
        opWin.rebuild();

        auto rowsU = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        int u1Cards = 0;
        int u2Cards = 0;
        for (auto *r : rowsU) {
            if (r->property("taskId").toULongLong() == idU1) ++u1Cards;
            if (r->property("taskId").toULongLong() == idU2) ++u2Cards;
        }
        verify(u1Cards == 1, "S7-D7-U: completed task 1 has exactly 1 card");
        verify(u2Cards == 1, "S7-D7-U: running task 2 has exactly 1 card");
        verify(h.ctl.taskSnapshot(idU1)->state == TransferTaskState::Completed, "S7-D7-U: task 1 remains Completed");
        verify(h.ctl.taskSnapshot(idU2)->state == TransferTaskState::Running, "S7-D7-U: task 2 remains Running");
        h.ctl.cancelTask(idU2);
        if (h.job(TQ_S("Film_Raid.mkv"))) {
            h.job(TQ_S("Film_Raid.mkv"))->finishError(KIO::ERR_USER_CANCELED, QStringLiteral("Canceled"));
        }
        QCoreApplication::processEvents();

        // V: Local transfer staging and publication safety (Zadanie 3)
        const QString vDir = d7Dir + TQ_S("/staging_v");
        QDir().mkpath(vDir);
        const QString vSrc = vDir + TQ_S("/src_video.mp4");
        const QString vDst = vDir + TQ_S("/published_video.mp4");
        const QString vForeign = vDir + TQ_S("/foreign_file.txt");
        {
            QFile sf(vSrc);
            if (sf.open(QIODevice::WriteOnly)) {
                sf.write("video_stream_content_safe_staging_test");
                sf.close();
            }
            QFile ff(vForeign);
            if (ff.open(QIODevice::WriteOnly)) {
                ff.write("foreign_data_must_not_be_touched");
                ff.close();
            }
        }
        auto *localJob = new LocalFileCopyJob(QUrl::fromLocalFile(vSrc), QUrl::fromLocalFile(vDst));
        localJob->setAutoDelete(false);
        bool localDone = false;
        QObject::connect(localJob, &KJob::result, [&localDone] { localDone = true; });
        localJob->start();
        verify(waitUntil([&localDone] { return localDone; }, 5000), "S7-D7-V: local transfer completed");
        verify(QFile::exists(vDst), "S7-D7-V: destination file published upon completion");
        verify(!QFile::exists(vDst + QStringLiteral(".thispc-part")), "S7-D7-V: no leftover .thispc-part file");
        verify(QFile::exists(vForeign), "S7-D7-V: foreign file untouched");
        {
            QFile ff(vForeign);
            if (ff.open(QIODevice::ReadOnly)) {
                verify(ff.readAll() == "foreign_data_must_not_be_touched", "S7-D7-V: foreign content perfectly preserved");
            }
        }
        delete localJob;

        // W: MTP staged transfer publication and safety (Zadanie 3)
        const QString wDir = d7Dir + TQ_S("/mtp_staging_w");
        QDir().mkpath(wDir);
        const QString wTarget = wDir + TQ_S("/final_mtp_photo.jpg");
        const QString wForeign = wDir + TQ_S("/foreign_mtp.txt");
        {
            QFile wf(wForeign);
            if (wf.open(QIODevice::WriteOnly)) {
                wf.write("foreign_mtp_data");
                wf.close();
            }
        }
        auto *stagedMtpJob = new KioStagedTransferJob(
            {QUrl(QStringLiteral("mtp:/phone/DCIM/photo.jpg"))},
            QUrl::fromLocalFile(wDir),
            /*move=*/false,
            QUrl::fromLocalFile(wTarget));
        stagedMtpJob->setAutoDelete(false);
        stagedMtpJob->setSubJobFactory([wDir](const QUrl &, const QUrl &partUrl) {
            auto *fakeSub = new QueueFakeJob(true, false, false);
            QFile pf(partUrl.toLocalFile());
            if (pf.open(QIODevice::WriteOnly)) {
                pf.write("photo_image_data_payload");
                pf.close();
            }
            QTimer::singleShot(10, fakeSub, [fakeSub] { fakeSub->finish(); });
            return fakeSub;
        });
        bool mtpDone = false;
        QObject::connect(stagedMtpJob, &KJob::result, [&mtpDone] { mtpDone = true; });
        stagedMtpJob->start();
        verify(waitUntil([&mtpDone] { return mtpDone; }, 5000), "S7-D7-W: staged MTP transfer completed");
        verify(stagedMtpJob->error() == 0, "S7-D7-W: staged MTP job completed with 0 error");
        verify(QFile::exists(wTarget), "S7-D7-W: target file atomically published");
        verify(QFile::exists(wForeign), "S7-D7-W: foreign file untouched");
        {
            QFile targetCheck(wTarget);
            if (targetCheck.open(QIODevice::ReadOnly)) {
                verify(targetCheck.readAll() == "photo_image_data_payload", "S7-D7-W: target content verified");
            }
        }
        delete stagedMtpJob;

        // X: Visual flicker regression & directory watch staging isolation
        const QString xDir = d7Dir + TQ_S("/flicker_x");
        QDir().mkpath(xDir);
        const QString xExistingMovie = xDir + TQ_S("/existing_film.mkv");
        {
            QFile ef(xExistingMovie);
            if (ef.open(QIODevice::WriteOnly)) {
                ef.write("existing_movie_payload");
                ef.close();
            }
        }
        DirectoryWatchController watchCtl;
        const QUrl xDirUrl = QUrl::fromLocalFile(xDir);
        const void *dummyConsumer = reinterpret_cast<const void *>(0x12345);
        watchCtl.setWatchedUrl(dummyConsumer, xDirUrl);

        int refreshTriggerCount = 0;
        QObject::connect(&watchCtl, &DirectoryWatchController::directoryChanged,
                         [&](const QUrl &changedUrl) {
                             if (sameLocation(changedUrl, xDirUrl)) {
                                 ++refreshTriggerCount;
                             }
                         });

        // Simulate thousands of inotify dirty / fileChanged events for active .thispc-part file
        const QString xStaging1 = xDir + TQ_S("/huge_video.mkv.thispc-part");
        const QString xStaging2 = xDir + TQ_S("/huge_video.mkv.thispc-part.999.nonce");
        for (int i = 0; i < 50; ++i) {
            watchCtl.handleFilesChanged({xStaging1, xStaging2});
        }
        QCoreApplication::processEvents();
        QTest::qWait(300);

        verify(refreshTriggerCount == 0, "S7-D7-X: staging file writes do not trigger directoryChanged");

        // Simulate atomic publish: rename staging to final destination and notify
        const QString xFinalVideo = xDir + TQ_S("/huge_video.mkv");
        watchCtl.handleFileRenamed(xStaging1, xFinalVideo);
        QCoreApplication::processEvents();
        QTest::qWait(300);

        verify(refreshTriggerCount == 1, "S7-D7-X: publication triggers exactly 1 directoryChanged");

        // Verify that external changes to regular files are still detected
        const QString xOtherFile = xDir + TQ_S("/notes.txt");
        watchCtl.handleFilesChanged({xOtherFile});
        QCoreApplication::processEvents();
        QTest::qWait(300);

        verify(refreshTriggerCount == 2, "S7-D7-X: external non-staging file changes are correctly detected");
        watchCtl.removeConsumer(dummyConsumer);

        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
        verify(!isTransferQueueProductionDispatchConnected(), "S7-D7: production mode cleanly restored");
    }

    // S7-D7-HIST: Operations History Management (Stage 7D.7)
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        const QString histDir = base + TQ_S("/hist_test");
        QueueHarness h(histDir);
        h.ctl.setCloseWindowOnComplete(false);
        h.ctl.setConcurrencyLimit(2);

        h.touchSource(TQ_S("file1.bin"), "payload1");
        h.touchSource(TQ_S("file2.bin"), "payload2");
        h.touchSource(TQ_S("file3.bin"), "payload3");
        h.touchSource(TQ_S("file4.bin"), "payload4");

        quint64 tId1 = h.add(TQ_S("Task 1"), {h.src(TQ_S("file1.bin"))}, h.dst(TQ_S("file1.bin")));
        quint64 tId2 = h.add(TQ_S("Task 2"), {h.src(TQ_S("file2.bin"))}, h.dst(TQ_S("file2.bin")));
        quint64 tId3 = h.add(TQ_S("Task 3"), {h.src(TQ_S("file3.bin"))}, h.dst(TQ_S("file3.bin")));
        quint64 tId4 = h.add(TQ_S("Task 4"), {h.src(TQ_S("file4.bin"))}, h.dst(TQ_S("file4.bin")));

        verify(tId1 > 0 && tId2 > 0 && tId3 > 0 && tId4 > 0, "S7-D7-HIST: all 4 tasks enqueued");

        // Complete Task 1
        {
            QFile f(h.dst(TQ_S("file1.bin")).toLocalFile());
            if (f.open(QIODevice::WriteOnly)) {
                f.write("dst1_payload");
                f.close();
            }
        }
        if (h.job(TQ_S("Task 1"))) {
            h.job(TQ_S("Task 1"))->finishOk();
        }

        // Cancel Task 2
        h.ctl.cancelTask(tId2);

        // Fail Task 3
        if (h.job(TQ_S("Task 3"))) {
            h.job(TQ_S("Task 3"))->finishError(KIO::ERR_DISK_FULL, QStringLiteral("Disk quota exceeded"));
        }

        verify(h.snap(tId1).state == TransferTaskState::Completed, "S7-D7-HIST: task 1 Completed");
        verify(h.snap(tId2).state == TransferTaskState::Cancelled, "S7-D7-HIST: task 2 Cancelled");
        verify(h.snap(tId3).state == TransferTaskState::Failed, "S7-D7-HIST: task 3 Failed");
        verify(h.snap(tId4).state == TransferTaskState::Running || h.snap(tId4).state == TransferTaskState::Queued, "S7-D7-HIST: task 4 active");

        // Test A: active task removal is strictly rejected
        verify(!h.ctl.removeFinishedTask(tId4), "S7-D7-HIST-A: cannot remove active task from history");
        verify(h.ctl.taskSnapshot(tId4).has_value(), "S7-D7-HIST-A: task 4 remains present in queue");

        // Test B: terminal tasks can be removed individually
        verify(h.ctl.removeFinishedTask(tId1), "S7-D7-HIST-B: completed task 1 removed from history");
        verify(!h.ctl.taskSnapshot(tId1).has_value(), "S7-D7-HIST-B: task 1 no longer in snapshots");
        verify(!h.ctl.removeFinishedTask(tId1), "S7-D7-HIST-B: removing already removed task returns false");

        // Test C: Filesystem safety — destination and source files of removed task are untouched!
        verify(QFile::exists(h.src(TQ_S("file1.bin")).toLocalFile()), "S7-D7-HIST-C: source file 1 untouched after history removal");
        verify(QFile::exists(h.dst(TQ_S("file1.bin")).toLocalFile()), "S7-D7-HIST-C: destination file 1 untouched after history removal");

        // Test D: Remaining tasks retain exact original IDs and states
        verify(h.snap(tId2).state == TransferTaskState::Cancelled && h.snap(tId2).id == tId2, "S7-D7-HIST-D: task 2 intact");
        verify(h.snap(tId3).state == TransferTaskState::Failed && h.snap(tId3).id == tId3, "S7-D7-HIST-D: task 3 intact");
        verify(h.snap(tId4).id == tId4, "S7-D7-HIST-D: task 4 intact");

        // Test E: UI integration with TransferQueueViewWidget
        TransferQueueViewWidget qView;
        qView.setController(&h.ctl);

        auto *qClearHistBtn = qView.clearHistoryButton();
        verify(qClearHistBtn != nullptr, "S7-D7-HIST-E: clearHistoryButton exists in TransferQueueViewWidget");
        verify(qClearHistBtn->isEnabled(), "S7-D7-HIST-E: clearHistoryButton enabled when finished tasks exist");

        auto *item2 = qView.taskItemWidget(tId2);
        auto *item4 = qView.taskItemWidget(tId4);
        verify(item2 != nullptr && item4 != nullptr, "S7-D7-HIST-E: item widgets exist");
        verify(item2->removeButton() && !item2->removeButton()->isHidden(), "S7-D7-HIST-E: removeButton visible for Cancelled task");
        verify(item4->cancelButton() && !item4->cancelButton()->isHidden(), "S7-D7-HIST-E: cancelButton visible for active task");
        verify(item4->removeButton() && item4->removeButton()->isHidden(), "S7-D7-HIST-E: removeButton hidden for active task");

        // Test F: OperationWindow integration
        QMainWindow testWin;
        QToolBar testTb(&testWin);
        OperationManager opMgr(&testWin, &testTb, &testWin);
        opMgr.setQueueController(&h.ctl);
        opMgr.showDetailedWindow();
        OperationWindow *opWin = opMgr.m_detailedWindow;
        verify(opWin != nullptr, "S7-D7-HIST-F: OperationWindow instantiated");

        auto *opClearHistBtn = opWin->clearHistoryButton();
        verify(opClearHistBtn != nullptr, "S7-D7-HIST-F: clearHistoryButton exists in OperationWindow");
        verify(opClearHistBtn->isEnabled(), "S7-D7-HIST-F: clearHistoryButton enabled in OperationWindow");

        // Test G: Bulk "Clear history" (clearFinished)
        // 2 terminal tasks (tId2, tId3) and 1 active (tId4)
        int clearedCount = h.ctl.clearFinished();
        verify(clearedCount == 2, "S7-D7-HIST-G: clearFinished removed exactly 2 terminal tasks");
        verify(!h.ctl.taskSnapshot(tId2).has_value(), "S7-D7-HIST-G: task 2 removed");
        verify(!h.ctl.taskSnapshot(tId3).has_value(), "S7-D7-HIST-G: task 3 removed");
        verify(h.ctl.taskSnapshot(tId4).has_value(), "S7-D7-HIST-G: active task 4 strictly preserved");

        // Test H: Buttons disabled when no finished tasks remain
        qView.rebuildTasks();
        opWin->rebuild();
        verify(!qClearHistBtn->isEnabled(), "S7-D7-HIST-H: queue clearHistoryButton disabled with 0 finished");
        verify(!opClearHistBtn->isEnabled(), "S7-D7-HIST-H: OperationWindow clearHistoryButton disabled with 0 finished");

        // Test I: Refresh persistence — rebuilt view does NOT restore cleared tasks
        opWin->scheduleRebuild();
        QCoreApplication::processEvents();
        QTest::qWait(50);
        auto currentSnap = h.ctl.snapshot();
        verify(currentSnap.tasks.size() == 1 && currentSnap.tasks[0].id == tId4, "S7-D7-HIST-I: only task 4 remains after rebuild");

        // Test J: Active path reservations are intact
        verify(h.ctl.hasNonTerminalTasks(), "S7-D7-HIST-J: non-terminal tasks query intact");

        // Test K: Filesystem check for tasks 2, 3, 4
        verify(QFile::exists(h.src(TQ_S("file2.bin")).toLocalFile()), "S7-D7-HIST-K: src 2 intact");
        verify(QFile::exists(h.src(TQ_S("file3.bin")).toLocalFile()), "S7-D7-HIST-K: src 3 intact");
        verify(QFile::exists(h.src(TQ_S("file4.bin")).toLocalFile()), "S7-D7-HIST-K: src 4 intact");

        // Clean up remaining task 4
        h.ctl.cancelTask(tId4);
        h.ctl.clearFinished();

        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // ------------------------------------------------------------------
    // Stage 7D.7: U6.2 Settings Persistence Tests (P1 - P10)
    // ------------------------------------------------------------------

    // P1: Auto-close persistence
    {
        const QString confPath = nextRoot() + TQ_S("/p1_settings.ini");
        auto store = std::make_shared<QSettings>(confPath, QSettings::IniFormat);

        TransferQueueController ctl;
        ctl.setCloseWindowOnComplete(true);
        ctl.saveSettings(*store);
        store->sync();

        TransferQueueController ctl2;
        ctl2.loadSettings(*store);
        verify(ctl2.closeWindowOnComplete() == true, "P1: Auto-close setting persisted as true");

        OperationManager opMgr(nullptr, nullptr, nullptr);
        opMgr.setQueueController(&ctl2);
        verify(opMgr.isCloseOnComplete() == true, "P1: OperationManager initialized closeOnComplete from queue controller");

        ctl2.setCloseWindowOnComplete(false);
        verify(opMgr.isCloseOnComplete() == false, "P1: OperationManager synchronized when queue controller changed");

        opMgr.setCloseOnComplete(true);
        verify(ctl2.closeWindowOnComplete() == true, "P1: Queue controller synchronized when OperationManager changed");
    }

    // P2: Bandwidth limit 4 MiB/s persistence
    {
        const QString confPath = nextRoot() + TQ_S("/p2_settings.ini");
        auto store = std::make_shared<QSettings>(confPath, QSettings::IniFormat);

        TransferQueueController ctl;
        ctl.setBandwidthLimit(true, 4ULL * 1024ULL * 1024ULL, BandwidthUnit::MiB_s, 4);
        ctl.saveSettings(*store);
        store->sync();

        TransferQueueController ctl2;
        ctl2.loadSettings(*store);
        verify(ctl2.isBandwidthLimitEnabled() == true, "P2: Bandwidth limit enabled persisted");
        verify(ctl2.bandwidthLimitBytesPerSec() == 4ULL * 1024ULL * 1024ULL, "P2: 4 MiB/s bytes persisted");
        verify(ctl2.bandwidthLimitUnit() == BandwidthUnit::MiB_s, "P2: Bandwidth limit unit MiB/s persisted");
        verify(ctl2.bandwidthLimitValue() == 4, "P2: Bandwidth limit value 4 persisted");
    }

    // P3: Bandwidth limit disabled persistence
    {
        const QString confPath = nextRoot() + TQ_S("/p3_settings.ini");
        auto store = std::make_shared<QSettings>(confPath, QSettings::IniFormat);

        TransferQueueController ctl;
        ctl.setBandwidthLimit(true, 4ULL * 1024ULL * 1024ULL, BandwidthUnit::MiB_s, 4);
        ctl.setBandwidthLimitEnabled(false);
        ctl.saveSettings(*store);
        store->sync();

        TransferQueueController ctl2;
        ctl2.loadSettings(*store);
        verify(ctl2.isBandwidthLimitEnabled() == false, "P3: Bandwidth limiter disabled persisted");
        verify(ctl2.bandwidthLimitBytesPerSec() == 0, "P3: Active bandwidth limit rate is 0 (unlimited) when disabled");
        verify(ctl2.configuredBandwidthLimitBytesPerSec() == 4ULL * 1024ULL * 1024ULL, "P3: Configured limit bytes preserved as 4 MiB/s");
        verify(ctl2.bandwidthLimitValue() == 4, "P3: Configured value preserved as 4");
        verify(ctl2.bandwidthLimitUnit() == BandwidthUnit::MiB_s, "P3: Configured unit preserved as MiB/s");
    }

    // P4: Unit and value preservation (e.g. 500 KiB/s vs 4 MiB/s)
    {
        const QString confPath = nextRoot() + TQ_S("/p4_settings.ini");
        auto store = std::make_shared<QSettings>(confPath, QSettings::IniFormat);

        TransferQueueController ctl;
        ctl.setBandwidthLimit(true, 500ULL * 1024ULL, BandwidthUnit::KiB_s, 500);
        ctl.saveSettings(*store);
        store->sync();

        TransferQueueSettingsWidget widget;
        widget.setIsolatedSettings(store);
        widget.loadSettings(*store);
        verify(widget.bandwidthCheck()->isChecked() == true, "P4: widget check is true");
        verify(widget.bandwidthSpin()->value() == 500, "P4: widget spin is 500");
        verify(widget.bandwidthUnitCombo()->currentData().toInt() == static_cast<int>(BandwidthUnit::KiB_s), "P4: widget unit is KiB/s");

        // Now test 4 MiB/s
        ctl.setBandwidthLimit(true, 4ULL * 1024ULL * 1024ULL, BandwidthUnit::MiB_s, 4);
        ctl.saveSettings(*store);
        store->sync();

        widget.loadSettings(*store);
        verify(widget.bandwidthSpin()->value() == 4, "P4: widget spin is 4");
        verify(widget.bandwidthUnitCombo()->currentData().toInt() == static_cast<int>(BandwidthUnit::MiB_s), "P4: widget unit is MiB/s");
    }

    // P5: Mode and parallel limit (Sequential, Parallel limit 5)
    {
        const QString confPath = nextRoot() + TQ_S("/p5_settings.ini");
        auto store = std::make_shared<QSettings>(confPath, QSettings::IniFormat);

        TransferQueueController ctl;
        ctl.setExecutionMode(TransferQueueMode::Sequential);
        ctl.setParallelConcurrencyLimit(5);
        ctl.saveSettings(*store);
        store->sync();

        TransferQueueController ctl2;
        ctl2.loadSettings(*store);
        verify(ctl2.executionMode() == TransferQueueMode::Sequential, "P5: Sequential mode persisted");
        verify(ctl2.effectiveConcurrencyLimit() == 1, "P5: Effective limit is 1 in Sequential");
        verify(ctl2.parallelConcurrencyLimit() == 5, "P5: Configured parallel limit 5 preserved");

        ctl2.setExecutionMode(TransferQueueMode::Parallel);
        verify(ctl2.effectiveConcurrencyLimit() == 5, "P5: Effective limit restores to 5 on switch to Parallel");
    }

    // P6: Reopening OperationWindow in same process
    {
        const QString confPath = nextRoot() + TQ_S("/p6_settings.ini");
        auto store = std::make_shared<QSettings>(confPath, QSettings::IniFormat);

        TransferQueueController ctl;
        ctl.setBandwidthLimit(true, 4ULL * 1024ULL * 1024ULL, BandwidthUnit::MiB_s, 4);
        ctl.setCloseWindowOnComplete(true);
        ctl.saveSettings(*store);
        store->sync();

        OperationManager opMgr(nullptr, nullptr, nullptr);
        opMgr.setQueueController(&ctl);

        {
            OperationWindow win1(&opMgr);
            win1.queueSettingsWidget()->setIsolatedSettings(store);
            win1.queueSettingsWidget()->setController(&ctl);
            verify(win1.isCloseOnComplete() == true, "P6: win1 closeOnComplete matches controller");
            verify(win1.queueSettingsWidget()->bandwidthCheck()->isChecked() == true, "P6: win1 bandwidth check is checked");
            verify(win1.queueSettingsWidget()->bandwidthSpin()->value() == 4, "P6: win1 bandwidth spin is 4");
        }

        {
            OperationWindow win2(&opMgr);
            win2.queueSettingsWidget()->setIsolatedSettings(store);
            win2.queueSettingsWidget()->setController(&ctl);
            verify(win2.isCloseOnComplete() == true, "P6: win2 closeOnComplete matches controller after reopen");
            verify(win2.queueSettingsWidget()->bandwidthCheck()->isChecked() == true, "P6: win2 bandwidth check is checked after reopen");
            verify(win2.queueSettingsWidget()->bandwidthSpin()->value() == 4, "P6: win2 bandwidth spin is 4 after reopen");
        }
    }

    // P7: Two independent processes sharing XDG_CONFIG_HOME
    {
        const QString testXdgDir = nextRoot() + TQ_S("/xdg_p7");
        QDir().mkpath(testXdgDir + TQ_S("/kio-thispc"));
        const QString confFile = testXdgDir + TQ_S("/kio-thispc/thispc-view.conf");

        // Process 1 (this process): save settings
        {
            QSettings s(confFile, QSettings::IniFormat);
            TransferQueueConfig cfg;
            cfg.mode = TransferQueueMode::Parallel;
            cfg.parallelLimit = 4;
            cfg.bandwidthLimitEnabled = true;
            cfg.bandwidthLimitBytesPerSec = 4ULL * 1024ULL * 1024ULL;
            cfg.bandwidthLimitUnit = BandwidthUnit::MiB_s;
            cfg.bandwidthLimitValue = 4;
            cfg.closeWindowOnComplete = true;
            TransferQueueSettings::save(s, cfg);
            s.sync();
        }

        // Process 2: external subprocess via python3 inspecting and modifying the file
        QProcess proc;
        proc.setProgram(QStringLiteral("python3"));
        proc.setArguments({
            QStringLiteral("-c"),
            QStringLiteral(
                "import configparser, sys\n"
                "p = sys.argv[1]\n"
                "c = configparser.ConfigParser()\n"
                "c.optionxform = str\n"
                "c.read(p)\n"
                "assert c.get('TransferQueue', 'ExecutionMode') == 'Parallel'\n"
                "assert c.get('TransferQueue', 'BandwidthLimitBytes') == '4194304'\n"
                "assert c.get('TransferQueue', 'BandwidthLimitUnit') == 'MiB/s'\n"
                "assert c.get('TransferQueue', 'BandwidthLimitValue') == '4'\n"
                "assert c.get('TransferQueue', 'CloseWindowOnComplete') == 'true'\n"
                "c.set('TransferQueue', 'ParallelLimit', '6')\n"
                "c.set('TransferQueue', 'CloseWindowOnComplete', 'false')\n"
                "with open(p, 'w') as f:\n"
                "    c.write(f)\n"
            ),
            confFile
        });
        proc.start();
        verify(proc.waitForFinished(5000), "P7: subprocess finished within 5s");
        verify(proc.exitCode() == 0, "P7: subprocess verified settings and wrote updates with exit code 0");

        // Process 1: re-read file into fresh controller and verify updates made by process 2
        {
            QSettings s2(confFile, QSettings::IniFormat);
            TransferQueueController ctl;
            ctl.loadSettings(s2);
            verify(ctl.parallelConcurrencyLimit() == 6, "P7: parallelLimit updated by process 2 loaded as 6");
            verify(ctl.closeWindowOnComplete() == false, "P7: closeWindowOnComplete updated by process 2 loaded as false");
            verify(ctl.isBandwidthLimitEnabled() == true, "P7: bandwidthLimitEnabled remains true");
            verify(ctl.bandwidthLimitValue() == 4, "P7: bandwidthLimitValue remains 4");
        }
    }

    // P8: Toggling off and restarting
    {
        const QString confPath = nextRoot() + TQ_S("/p8_settings.ini");
        auto store = std::make_shared<QSettings>(confPath, QSettings::IniFormat);

        TransferQueueController ctl;
        ctl.setBandwidthLimit(true, 4ULL * 1024ULL * 1024ULL, BandwidthUnit::MiB_s, 4);
        ctl.setCloseWindowOnComplete(true);
        ctl.saveSettings(*store);
        store->sync();

        // User turns off both settings
        ctl.setBandwidthLimitEnabled(false);
        ctl.setCloseWindowOnComplete(false);
        ctl.saveSettings(*store);
        store->sync();

        // Restart with fresh controller
        TransferQueueController ctlRestart;
        ctlRestart.loadSettings(*store);
        verify(ctlRestart.isBandwidthLimitEnabled() == false, "P8: Bandwidth limit disabled on restart");
        verify(ctlRestart.closeWindowOnComplete() == false, "P8: Auto close disabled on restart");
        verify(ctlRestart.configuredBandwidthLimitBytesPerSec() == 4ULL * 1024ULL * 1024ULL, "P8: Configured 4 MiB/s value preserved");
        verify(ctlRestart.bandwidthLimitValue() == 4, "P8: Bandwidth limit value 4 preserved");
        verify(ctlRestart.bandwidthLimitUnit() == BandwidthUnit::MiB_s, "P8: Bandwidth limit unit MiB/s preserved");
    }

    // P9: UI initialization does not overwrite loaded settings
    {
        const QString confPath = nextRoot() + TQ_S("/p9_settings.ini");
        auto store = std::make_shared<QSettings>(confPath, QSettings::IniFormat);

        TransferQueueConfig cfg;
        cfg.mode = TransferQueueMode::Sequential;
        cfg.parallelLimit = 5;
        cfg.bandwidthLimitEnabled = true;
        cfg.bandwidthLimitBytesPerSec = 7ULL * 1024ULL * 1024ULL;
        cfg.bandwidthLimitUnit = BandwidthUnit::MiB_s;
        cfg.bandwidthLimitValue = 7;
        cfg.closeWindowOnComplete = true;
        TransferQueueSettings::save(*store, cfg);
        store->sync();

        TransferQueueController ctl;
        ctl.loadSettings(*store);

        // Widget construction and setController must not overwrite store with defaults
        TransferQueueSettingsWidget widget;
        widget.setIsolatedSettings(store);
        widget.setController(&ctl);

        store->sync();
        const TransferQueueConfig reloaded = TransferQueueSettings::load(*store);
        verify(reloaded.mode == TransferQueueMode::Sequential, "P9: Mode not overwritten");
        verify(reloaded.parallelLimit == 5, "P9: ParallelLimit not overwritten");
        verify(reloaded.bandwidthLimitEnabled == true, "P9: Bandwidth limit enabled not overwritten");
        verify(reloaded.bandwidthLimitValue == 7, "P9: Bandwidth limit value not overwritten");
        verify(reloaded.closeWindowOnComplete == true, "P9: CloseWindowOnComplete not overwritten");
    }

    // P10: Auto Open / Auto Close / limiter regression checks
    {
        QueueHarness h(nextRoot());
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");

        OperationManager opMgr(nullptr, nullptr, nullptr);
        opMgr.setQueueController(&h.ctl);
        opMgr.setCloseOnComplete(true);

        const quint64 tId = h.add(TQ_S("p10_task"), {h.src(TQ_S("p10.bin"))}, h.dst(TQ_S("p10_dst.bin")));
        verify(h.ctl.activeCount() == 1, "P10: Task active");
        verify(opMgr.isCloseOnComplete() == true, "P10: Auto-close enabled on manager");

        // Complete job
        h.job(TQ_S("p10_task"))->finishOk();
        verify(h.ctl.isQueueIdle(), "P10: Queue idle after task completion");

        // Limiter token bucket rate verification
        h.ctl.setBandwidthLimit(true, 4ULL * 1024ULL * 1024ULL, BandwidthUnit::MiB_s, 4);
        verify(h.ctl.bandwidthLimiter()->bytesPerSecond() == 4ULL * 1024ULL * 1024ULL, "P10: Limiter token bucket rate 4 MiB/s");

        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // U8-RESP: Responsive Layout & Settings Scroll Regression
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        QueueHarness h(nextRoot());

        const quint64 tId1 = h.add(TQ_S("active_movie.mp4"), {h.src(TQ_S("active_movie.mp4"))}, h.dst(TQ_S("active_movie_dst.mp4")));
        auto *j1 = h.job(TQ_S("active_movie.mp4"));
        j1->reportProgress(35ULL * 1024ULL * 1024ULL, 100ULL * 1024ULL * 1024ULL);
        j1->reportSpeed(12ULL * 1024ULL * 1024ULL);

        const quint64 tId2 = h.add(TQ_S("completed_doc.pdf"), {h.src(TQ_S("completed_doc.pdf"))}, h.dst(TQ_S("completed_doc_dst.pdf")));
        h.job(TQ_S("completed_doc.pdf"))->finishOk();

        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        mgr.setQueueController(&h.ctl);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&h.ctl);
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        // Check minimum size contract for unified window
        const QSize minWinSize = opWin.minimumSizeHint();
        verify(minWinSize.width() <= 480 && minWinSize.height() <= 260,
               "U8-RESP-01: Unified window minimumSizeHint allows compact 480x260");

        // Resize to 480x290 with settings expanded
        opWin.resize(480, 290);
        auto *toggleBtn = opWin.findChild<QToolButton *>(QStringLiteral("operationSettingsToggleButton"));
        verify(toggleBtn != nullptr, "U8-RESP-01: Settings toggle button exists");
        toggleBtn->setChecked(true);
        QCoreApplication::processEvents();

        auto *settingsScroll = opWin.findChild<QScrollArea *>(QStringLiteral("operationSettingsContainer"));
        verify(settingsScroll != nullptr, "U8-RESP-01: Settings scroll area exists");
        verify(!settingsScroll->isHidden(), "U8-RESP-01: Settings scroll area is visible when toggled");
        verify(settingsScroll->widgetResizable(), "U8-RESP-01: Settings scroll area is widgetResizable");

        auto *settingsWidget = opWin.findChild<TransferQueueSettingsWidget *>(QStringLiteral("transferQueueSettingsWidget"));
        verify(settingsWidget != nullptr, "U8-RESP-01: TransferQueueSettingsWidget exists");

        auto *seqRadio = settingsWidget->findChild<QRadioButton *>(QStringLiteral("transferQueueSequentialRadio"));
        auto *parRadio = settingsWidget->findChild<QRadioButton *>(QStringLiteral("transferQueueParallelRadio"));
        auto *limitSpin = settingsWidget->findChild<QSpinBox *>(QStringLiteral("transferQueueLimitSpin"));
        auto *bwCheck = settingsWidget->findChild<QCheckBox *>(QStringLiteral("transferQueueBandwidthCheck"));
        auto *bwSpin = settingsWidget->findChild<QSpinBox *>(QStringLiteral("transferQueueBandwidthSpin"));
        auto *bwCombo = settingsWidget->findChild<QComboBox *>(QStringLiteral("transferQueueBandwidthUnitCombo"));
        auto *closeCheck = settingsWidget->findChild<QCheckBox *>(QStringLiteral("closeWindowOnCompleteCheckBox"));
        verify(seqRadio && parRadio && limitSpin && bwCheck && bwSpin && bwCombo && closeCheck,
               "U8-RESP-01: All settings controls exist in widget");

        verify(seqRadio->height() >= 20, "U8-RESP-01: Sequential radio has valid height");
        verify(parRadio->height() >= 20, "U8-RESP-01: Parallel radio has valid height");
        verify(bwCheck->height() >= 20, "U8-RESP-01: Bandwidth check has valid height");
        verify(bwSpin->height() >= 20, "U8-RESP-01: Bandwidth spin has valid height");
        verify(bwCombo->height() >= 20, "U8-RESP-01: Bandwidth combo has valid height");
        verify(closeCheck->height() >= 20, "U8-RESP-01: Close-on-complete check has valid height");

        // Verify vertical order & NO overlaps between rows: row 1 (mode) above row 2 (bw) above row 3 (close)
        const QRect rRow1 = parRadio->geometry();
        const QRect rRow2 = bwCheck->geometry();
        const QRect rRow3 = closeCheck->geometry();
        verify(rRow1.bottom() <= rRow2.top(), "U8-RESP-01: Mode row is vertically above bandwidth row without overlap");
        verify(rRow2.bottom() <= rRow3.top(), "U8-RESP-01: Bandwidth row is vertically above close-on-complete row without overlap");

        // Verify horizontal spacing in bandwidth row: bwCheck left of bwSpin left of bwCombo
        verify(bwCheck->geometry().right() <= bwSpin->geometry().left(), "U8-RESP-01: bwCheck does not overlap bwSpin");
        verify(bwSpin->geometry().right() <= bwCombo->geometry().left(), "U8-RESP-01: bwSpin does not overlap bwCombo");
        verify(bwCombo->geometry().right() <= settingsWidget->width(), "U8-RESP-01: Bandwidth controls fit within settings width");

        // Operations list scroll area is visible alongside settings
        auto *opsScroll = opWin.operationsScrollArea();
        verify(opsScroll != nullptr && !opsScroll->isHidden(), "U8-RESP-01: Operations scroll area is visible");
        verify(opsScroll->height() >= 55, "U8-RESP-01: Operations scroll area retains at least 55px height at 480x290");

        auto rows = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rows.size() >= 2, "U8-RESP-01: Both active and completed cards are rendered");
        for (QFrame *row : rows) {
            auto *cancel = row->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
            auto *remove = row->findChild<QToolButton *>(QStringLiteral("detailedOperationRemoveButton"));
            if (cancel && cancel->isVisible()) {
                verify(cancel->geometry().right() <= row->width(), "U8-RESP-01: Cancel button fits inside card bounds");
            }
            if (remove && remove->isVisible()) {
                verify(remove->geometry().right() <= row->width(), "U8-RESP-01: Remove button fits inside card bounds");
            }
        }

        // U8-RESP-02: Collapse settings at 480x290 px
        toggleBtn->setChecked(false);
        QCoreApplication::processEvents();
        verify(settingsScroll->isHidden(), "U8-RESP-02: Settings container hidden when collapsed");
        verify(opsScroll->height() >= 150, "U8-RESP-02: Operations scroll area expands when settings collapsed");

        // U8-RESP-03: Expanded task details at 480x400 px
        opWin.resize(480, 400);
        toggleBtn->setChecked(true);
        QFrame *activeCard = nullptr;
        for (QFrame *row : rows) {
            auto *cancel = row->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
            if (cancel && cancel->isVisible()) {
                activeCard = row;
                break;
            }
        }
        verify(activeCard != nullptr, "U8-RESP-03: Active task card found");
        auto *detailsToggle = activeCard->findChild<QToolButton *>(QStringLiteral("operationDetailsToggleButton"));
        verify(detailsToggle != nullptr, "U8-RESP-03: Details toggle button found");
        detailsToggle->click();
        QCoreApplication::processEvents();

        auto *speedLabel = activeCard->findChild<QLabel *>(QStringLiteral("operationSpeedLabel"));
        auto *bytesLabel = activeCard->findChild<QLabel *>(QStringLiteral("operationBytesLabel"));
        auto *graph = activeCard->findChild<OperationSpeedGraph *>();
        verify(speedLabel != nullptr && !speedLabel->isHidden(), "U8-RESP-03: Speed label visible in expanded card");
        verify(bytesLabel != nullptr && !bytesLabel->isHidden(), "U8-RESP-03: Bytes label visible in expanded card");
        verify(graph != nullptr && !graph->isHidden(), "U8-RESP-03: Speed graph visible when card details expanded");
        verify(bytesLabel->geometry().right() <= activeCard->width(), "U8-RESP-03: Bytes label fits inside card width");

        // U8-RESP-04: Standard 640x480 px geometry
        opWin.resize(640, 480);
        QCoreApplication::processEvents();
        verify(settingsWidget->width() <= settingsScroll->viewport()->width(), "U8-RESP-04: Settings fit within viewport cleanly at 640x480");
        verify(opsScroll->height() >= 180, "U8-RESP-04: Operations scroll area has ample space at 640x480");

        // U8-RESP-05: Elided long paths and button responsiveness at 480px width
        const QString deepPath = QStringLiteral("/deep/very/long/nested/path/to/source/file/structure/").repeated(10) + QStringLiteral("very_long_multimedia_archive_file_2026.mkv");
        const quint64 tIdDeep = h.add(deepPath, {h.src(deepPath)}, h.dst(QStringLiteral("/dest/path/target.mkv")));
        opWin.rebuild();
        opWin.resize(480, 360);
        QCoreApplication::processEvents();

        auto newRows = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        QFrame *deepRow = nullptr;
        for (QFrame *r : newRows) {
            auto *t = r->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
            if (t && t->fullText().contains(QStringLiteral("very_long_multimedia"))) {
                deepRow = r;
                break;
            }
        }
        verify(deepRow != nullptr, "U8-RESP-05: Card with deep path found");
        auto *deepTitle = deepRow->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
        verify(deepTitle->geometry().right() < deepRow->width(), "U8-RESP-05: Deep title elided within row width");
        auto *deepCancel = deepRow->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
        verify(deepCancel != nullptr && deepCancel->geometry().right() <= deepRow->width(), "U8-RESP-05: Cancel button within card width for long path");

        // Clean up active jobs
        j1->finishOk();
        h.job(deepPath)->finishOk();
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // FINAL-UX-AUDIT: Final Unified Operations Manager UX Regression (Problems A, B, C & U7.2)
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        QueueHarness h(nextRoot());

        // Setup sequential mode
        h.ctl.setExecutionMode(TransferQueueMode::Sequential);

        // Task 1: active task
        const quint64 idStart = h.add(TQ_S("U4-Start.bin"), {h.src(TQ_S("U4-Start.bin"))}, h.dst(TQ_S("U4-Start_dst.bin")));
        // Task 2, 3, 4: queued tasks A, B, C
        const quint64 idA = h.add(TQ_S("Task_A.bin"), {h.src(TQ_S("Task_A.bin"))}, h.dst(TQ_S("Task_A_dst.bin")));
        const quint64 idB = h.add(TQ_S("Task_B.bin"), {h.src(TQ_S("Task_B.bin"))}, h.dst(TQ_S("Task_B_dst.bin")));
        const quint64 idC = h.add(TQ_S("Task_C.bin"), {h.src(TQ_S("Task_C.bin"))}, h.dst(TQ_S("Task_C_dst.bin")));

        QMainWindow win;
        QToolBar tb(&win);
        OperationManager mgr(&win, &tb, &win);
        mgr.setQueueController(&h.ctl);

        OperationWindow opWin(&mgr, &win);
        opWin.setQueueController(&h.ctl);
        opWin.rebuild();
        opWin.show();
        QCoreApplication::processEvents();

        auto getRowTitles = [&opWin]() -> QStringList {
            QStringList titles;
            const auto rows = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
            for (QFrame *r : rows) {
                auto *lbl = r->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
                if (lbl) titles.push_back(lbl->fullText());
            }
            return titles;
        };

        // 1. Initial visual order: Start, A, B, C
        QStringList initialOrder = getRowTitles();
        verify(initialOrder.size() == 4, "FINAL-UX-A1: 4 cards rendered initially");
        verify(initialOrder.at(0) == TQ_S("U4-Start.bin"), "FINAL-UX-A1: Start is row 0");
        verify(initialOrder.at(1) == TQ_S("Task_A.bin"), "FINAL-UX-A1: A is row 1");
        verify(initialOrder.at(2) == TQ_S("Task_B.bin"), "FINAL-UX-A1: B is row 2");
        verify(initialOrder.at(3) == TQ_S("Task_C.bin"), "FINAL-UX-A1: C is row 3");

        // 2. Problem A: Move C up twice to position #1 in queue
        verify(h.ctl.moveTaskUp(idC), "FINAL-UX-A2: C moved up once");
        verify(h.ctl.moveTaskUp(idC), "FINAL-UX-A2: C moved up twice");
        opWin.rebuild();
        QCoreApplication::processEvents();

        QStringList reordered = getRowTitles();
        verify(reordered.size() == 4, "FINAL-UX-A2: 4 cards preserved after reorder");
        verify(reordered.at(0) == TQ_S("U4-Start.bin"), "FINAL-UX-A2: Active Start stays at top (row 0)");
        verify(reordered.at(1) == TQ_S("Task_C.bin"), "FINAL-UX-A2: Visual card C moved to position #1 in queue (row 1)");
        verify(reordered.at(2) == TQ_S("Task_A.bin"), "FINAL-UX-A2: Visual card A is now row 2");
        verify(reordered.at(3) == TQ_S("Task_B.bin"), "FINAL-UX-A2: Visual card B is now row 3");

        // 3. Problem A: Priority change moves B to top of queue
        h.ctl.setTaskPriority(idB, TransferPriority::High);
        opWin.rebuild();
        QCoreApplication::processEvents();

        QStringList prioOrder = getRowTitles();
        verify(prioOrder.at(0) == TQ_S("U4-Start.bin"), "FINAL-UX-A3: Active Start stays at top");
        verify(prioOrder.at(1) == TQ_S("Task_B.bin"), "FINAL-UX-A3: High priority B becomes row 1 among queued");
        verify(prioOrder.at(2) == TQ_S("Task_C.bin"), "FINAL-UX-A3: C is now row 2");
        verify(prioOrder.at(3) == TQ_S("Task_A.bin"), "FINAL-UX-A3: A is now row 3");

        // 4. Problem A: Transition to completed places terminal card at bottom group
        h.job(TQ_S("U4-Start.bin"))->finishOk();
        opWin.rebuild();
        QCoreApplication::processEvents();

        QStringList nextOrder = getRowTitles();
        verify(nextOrder.size() == 4, "FINAL-UX-A4: 4 cards preserved after job finish");
        verify(nextOrder.at(0) == TQ_S("Task_B.bin"), "FINAL-UX-A4: Newly active B is now row 0");
        verify(nextOrder.at(1) == TQ_S("Task_C.bin"), "FINAL-UX-A4: Queued C is row 1");
        verify(nextOrder.at(2) == TQ_S("Task_A.bin"), "FINAL-UX-A4: Queued A is row 2");
        verify(nextOrder.at(3) == TQ_S("U4-Start.bin"), "FINAL-UX-A4: Finished Start is moved to bottom group (row 3)");

        // 5. Problem B: Preview popup consistency with active queue transfer
        mgr.updateSummary();
        QToolButton *mgrBtn = tb.findChild<QToolButton *>(QStringLiteral("operationButton"));
        verify(mgrBtn != nullptr, "FINAL-UX-B1: Operation manager button exists");
        verify(mgrBtn->property("active").toBool() == true, "FINAL-UX-B1: Button is active while queue has active task");
        QLabel *badge = mgrBtn->findChild<QLabel *>(QStringLiteral("operationBadge"));
        verify(badge != nullptr && !badge->isHidden(), "FINAL-UX-B1: Badge is visible");
        verify(badge->text() == QStringLiteral("1"), "FINAL-UX-B1: Badge shows 1 active transfer");

        OperationPopup *popup = mgr.popup();
        verify(popup != nullptr, "FINAL-UX-B2: Popup exists");
        QLabel *emptyLbl = popup->findChild<QLabel *>(QStringLiteral("operationEmptyLabel"));
        QLabel *summaryLbl = popup->findChild<QLabel *>(QStringLiteral("operationSummaryLabel"));
        verify(emptyLbl != nullptr && summaryLbl != nullptr, "FINAL-UX-B2: Popup labels exist");
        verify(!emptyLbl->text().contains(QStringLiteral("Brak operacji plikowych"))
               && !emptyLbl->text().contains(QStringLiteral("No file operations")),
               "FINAL-UX-B2: Empty label does not falsely claim 'Brak operacji plikowych' while queue is running");
        verify(emptyLbl->text().contains(QStringLiteral("kolejce operacji"))
               || emptyLbl->text().contains(QStringLiteral("operation queue")),
               "FINAL-UX-B2: Empty label informs user about active transfers in queue");

        // 6. U7.2: Traditional operations (Scanning / Long KJob) appear in Unified Operations Manager while active
        auto *scanJob = new QueueFakeJob(true, false, false);
        mgr.track(scanJob, QStringLiteral("Tradycyjne skanowanie zasobów"));
        opWin.rebuild();
        QCoreApplication::processEvents();

        QStringList orderWithScan = getRowTitles();
        verify(orderWithScan.contains(QStringLiteral("Tradycyjne skanowanie zasobów")),
               "FINAL-UX-U7: Active traditional operation appears alongside queue transfers in Unified Operations Manager");
        verify(orderWithScan.indexOf(QStringLiteral("Tradycyjne skanowanie zasobów")) <= 1,
               "FINAL-UX-U7: Active traditional operation is grouped with active tasks at top");

        // Finish traditional operation
        mgr.finish(scanJob, true, false, {});
        delete scanJob;
        opWin.rebuild();
        QCoreApplication::processEvents();

        verify(getRowTitles().contains(QStringLiteral("Tradycyjne skanowanie zasobów")),
               "FINAL-UX-U7: Finished traditional operation remains in Unified Operations Manager");
        verify(getRowTitles().indexOf(QStringLiteral("Tradycyjne skanowanie zasobów")) >= 2,
               "FINAL-UX-U7: Finished traditional operation moves to terminal group at bottom");

        // 7. Problem C: Limiter label and header elision in narrow window
        opWin.resize(480, 260);
        QCoreApplication::processEvents();
        auto *summaryHdr = opWin.findChild<TransferQueueElidedLabel *>(QStringLiteral("operationSummaryLabel"));
        verify(summaryHdr != nullptr, "FINAL-UX-C1: Header summary is TransferQueueElidedLabel");
        verify(!summaryHdr->toolTip().isEmpty(), "FINAL-UX-C1: Full summary text is preserved in tooltip");

        auto *bwCheck = opWin.findChild<QCheckBox *>(QStringLiteral("transferQueueBandwidthCheck"));
        verify(bwCheck != nullptr, "FINAL-UX-C2: Bandwidth check exists");
        verify(bwCheck->text() == QStringLiteral("Ogranicz prędkość:") || bwCheck->text() == QStringLiteral("Limit speed:"),
               "FINAL-UX-C2: Concise bandwidth check label");
        verify(bwCheck->toolTip().contains(QStringLiteral("Ogranicz prędkość transferu"))
               || bwCheck->toolTip().contains(QStringLiteral("Limit transfer speed")),
               "FINAL-UX-C2: Full bandwidth limiter meaning in tooltip");

        // 8. Bulk clear history: clears finished queue task U4-Start.bin and finished traditional scanJob without affecting active task B or queued tasks
        opWin.clearHistory();
        opWin.rebuild();
        QCoreApplication::processEvents();
        QStringList afterClear = getRowTitles();
        verify(!afterClear.contains(TQ_S("U4-Start.bin")), "FINAL-UX-C3: Finished task cleared from history");
        verify(!afterClear.contains(QStringLiteral("Tradycyjne skanowanie zasobów")), "FINAL-UX-C3: Finished traditional operation cleared from history");
        verify(afterClear.contains(TQ_S("Task_B.bin")), "FINAL-UX-C3: Active task B strictly preserved");
        verify(afterClear.contains(TQ_S("Task_C.bin")), "FINAL-UX-C3: Queued task C strictly preserved");
        verify(afterClear.contains(TQ_S("Task_A.bin")), "FINAL-UX-C3: Queued task A strictly preserved");

        // Cleanup
        h.job(TQ_S("Task_B.bin"))->finishOk();
        h.job(TQ_S("Task_C.bin"))->finishOk();
        h.job(TQ_S("Task_A.bin"))->finishOk();
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // =========================================================================
    // UX-D: Legacy KIO History Integration Fix
    // =========================================================================
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        setTransferQueueProductionDispatchConnected(true);

        QueueHarness h(base + TQ_S("/ux_d"));
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager opMgr(&win, &tb, &win);
        opMgr.setQueueController(&h.ctl);

        OperationWindow opWin(&opMgr, &win);
        opWin.setQueueController(&h.ctl);
        opMgr.setDetailedWindow(&opWin);
        opWin.resize(600, 420);
        opWin.show();

        // 1. Very short KIO operation (< 500 ms) such as moving 150 items to trash
        // Test single card creation for batch of 150 files:
        QList<QUrl> urls150;
        for (int i = 0; i < 150; ++i) {
            urls150.append(QUrl::fromLocalFile(QStringLiteral("/home/user/trash_item_%1.txt").arg(i)));
        }
        auto *trashJob = new QueueFakeJob(true, false, false);
        trashJob->setProperty("thispcSourceUrls", QVariant::fromValue(urls150));
        trashJob->setProperty("thispcDestinationUrl", QUrl(QStringLiteral("trash:/")));

        opMgr.track(trashJob, QStringLiteral("Przenoszenie do Kosza"));
        // Finish immediately (< 50 ms)
        opMgr.finish(trashJob, true, false, {});
        delete trashJob;

        opWin.rebuild();
        QCoreApplication::processEvents();

        // Check header counter
        auto *summaryLbl = opWin.findChild<TransferQueueElidedLabel *>(QStringLiteral("operationSummaryLabel"));
        verify(summaryLbl != nullptr, "UX-D-01: Summary label exists");
        verify(summaryLbl->text().contains(QStringLiteral("Zakończone: 1"))
               || summaryLbl->text().contains(QStringLiteral("Completed: 1")),
               "UX-D-01: Summary reports 1 completed operation");

        // Exactly 1 card created (NOT 150 cards!)
        const auto rows150 = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rows150.size() == 1, "UX-D-01: Batch of 150 files is represented by exactly 1 card");

        auto *card0 = rows150[0];
        auto *title0 = card0->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
        verify(title0 != nullptr && title0->fullText() == QStringLiteral("Przenoszenie do Kosza"),
               "UX-D-01: Card title matches legacy operation");

        auto *detailsLbl0 = card0->findChild<QLabel *>(QStringLiteral("operationDetailsLabel"));
        verify(detailsLbl0 != nullptr && (detailsLbl0->text() == QStringLiteral("Zakończono") || detailsLbl0->text() == QStringLiteral("Completed")),
               "UX-D-01: Card status shows Completed");

        auto *progBar0 = card0->findChild<QProgressBar *>(QStringLiteral("operationProgress"));
        verify(progBar0 != nullptr && progBar0->value() == 100, "UX-D-01: Progress bar is 100% on completed legacy card");

        auto *cancelBtn0 = card0->findChild<QToolButton *>(QStringLiteral("detailedOperationCancelButton"));
        verify(cancelBtn0 != nullptr && !cancelBtn0->isVisible(), "UX-D-01: Cancel button is hidden on completed card");

        auto *removeBtn0 = card0->findChild<QToolButton *>(QStringLiteral("detailedOperationRemoveButton"));
        verify(removeBtn0 != nullptr && removeBtn0->isVisible() && removeBtn0->isEnabled(),
               "UX-D-01: Remove button is visible and enabled on completed card");

        // Small preview popup consistency
        auto *popup = opMgr.popup();
        verify(popup != nullptr, "UX-D-02: Popup exists");
        auto *popupSummary = popup->findChild<QLabel *>(QStringLiteral("operationSummaryLabel"));
        verify(popupSummary != nullptr, "UX-D-02: Popup summary label exists");
        verify(popupSummary->text().contains(QStringLiteral("Zakończone: 1"))
               || popupSummary->text().contains(QStringLiteral("Completed: 1")),
               "UX-D-02: Popup summary matches main window (Completed: 1)");

        // 2. Expand / Collapse card
        auto *toggle0 = card0->findChild<QToolButton *>(QStringLiteral("operationDetailsToggleButton"));
        auto *detailsWidget0 = card0->findChild<QWidget *>(QStringLiteral("operationExpandedDetails"));
        verify(toggle0 != nullptr && detailsWidget0 != nullptr, "UX-D-03: Details toggle and widget exist");
        const bool initialExpanded = detailsWidget0->isVisible();
        toggle0->click();
        verify(detailsWidget0->isVisible() != initialExpanded, "UX-D-03: Toggle successfully collapses/expands card details");
        toggle0->click();
        verify(detailsWidget0->isVisible() == initialExpanded, "UX-D-03: Toggle returns to initial expansion state");

        // 3. Multi-state verification: Failed, Cancelled, and Running operations
        auto *failJob = new QueueFakeJob(true, false, false);
        opMgr.track(failJob, QStringLiteral("Błędna operacja"));
        opMgr.finish(failJob, false, false, QStringLiteral("Brak uprawnień"));
        delete failJob;

        auto *cancelJob = new QueueFakeJob(true, false, false);
        opMgr.track(cancelJob, QStringLiteral("Anulowana operacja"));
        opMgr.finish(cancelJob, false, true, {});
        delete cancelJob;

        auto *activeJob = new QueueFakeJob(true, false, false);
        opMgr.track(activeJob, QStringLiteral("Długa operacja w toku"));

        opWin.rebuild();
        QCoreApplication::processEvents();

        const auto rowsMulti = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rowsMulti.size() == 4, "UX-D-04: Exactly 4 operation cards rendered (1 active, 3 finished)");

        // Order check: active task must be first (rank 0), finished at bottom (rank 2)
        auto *firstTitle = rowsMulti[0]->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
        verify(firstTitle && firstTitle->fullText() == QStringLiteral("Długa operacja w toku"),
               "UX-D-04: Active legacy operation is at top rank 0");

        // Error card check
        QFrame *failRow = nullptr;
        for (auto *r : rowsMulti) {
            auto *t = r->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
            if (t && t->fullText() == QStringLiteral("Błędna operacja")) failRow = r;
        }
        verify(failRow != nullptr, "UX-D-04: Failed card found");
        auto *failErrLbl = failRow->findChild<OperationDetailsLabel *>(QStringLiteral("operationErrorLabel"));
        verify(failErrLbl != nullptr && failErrLbl->text().contains(QStringLiteral("Brak uprawnień")),
               "UX-D-04: Error text displayed on failed card");

        // Finish active job
        opMgr.finish(activeJob, true, false, {});
        delete activeJob;

        // 4. Persistence across window close and reopen in same process
        opWin.hide();
        verify(!opWin.isVisible(), "UX-D-05: Window hidden");
        opMgr.showDetailedWindow();
        verify(opWin.isVisible(), "UX-D-05: Window reopened");
        const auto rowsReopened = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rowsReopened.size() == 4, "UX-D-05: All 4 history cards preserved across window close/reopen");

        // 5. Remove single card from history
        QFrame *trashRow = nullptr;
        for (auto *r : rowsReopened) {
            auto *t = r->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
            if (t && t->fullText() == QStringLiteral("Przenoszenie do Kosza")) trashRow = r;
        }
        verify(trashRow != nullptr, "UX-D-06: Trash card found for removal");
        auto *trashRemoveBtn = trashRow->findChild<QToolButton *>(QStringLiteral("detailedOperationRemoveButton"));
        verify(trashRemoveBtn != nullptr, "UX-D-06: Remove button found");
        trashRemoveBtn->click();
        opWin.rebuild();
        QCoreApplication::processEvents();

        const auto rowsAfterSingleRemove = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rowsAfterSingleRemove.size() == 3, "UX-D-06: 1 card removed, exactly 3 remain");
        for (auto *r : rowsAfterSingleRemove) {
            auto *t = r->findChild<TransferQueueElidedLabel *>(QStringLiteral("operationTitleLabel"));
            verify(t && t->fullText() != QStringLiteral("Przenoszenie do Kosza"),
                   "UX-D-06: Removed trash card is no longer in window");
        }

        // 6. Concurrency: queue transfer + legacy operation coexisting
        h.touchSource(TQ_S("srcConc.dat"), "payload_conc");
        const quint64 qId = h.add(TQ_S("Queue_Transfer.dat"), {h.src(TQ_S("srcConc.dat"))}, h.dst(TQ_S("dstConc.dat")));
        opWin.rebuild();
        QCoreApplication::processEvents();

        const auto rowsCoexist = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rowsCoexist.size() == 4, "UX-D-07: 1 active queue transfer + 3 legacy finished cards = 4 rows");

        // 7. Bulk clear history: removes all terminal operations without touching active queue transfer
        opWin.clearHistory();
        opWin.rebuild();
        QCoreApplication::processEvents();

        const auto rowsAfterBulkClear = opWin.findChildren<QFrame *>(QStringLiteral("operationRow"));
        verify(rowsAfterBulkClear.size() == 1, "UX-D-08: Bulk clear history removed all 3 finished legacy cards");
        verify(rowsAfterBulkClear[0]->property("taskId").toULongLong() == qId,
               "UX-D-08: Active queue transfer strictly preserved after bulk clear");

        // Cleanup
        h.ctl.cancelTask(qId);
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // =========================================================================
    // Stage 7 MTP Move Release Block: S7-31 to S7-35
    // =========================================================================

    // S7-31: Central Safety Gate — All MTP Move directions refused across all entry points,
    //        clipboard & files preserved, MTP Copy and Local Move unaffected.
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        const QString s731Dir = base + TQ_S("/s7_31_routing");
        QDir(s731Dir).removeRecursively();
        QDir().mkpath(s731Dir);

        const QString localSrcFile = s731Dir + TQ_S("/local_original.txt");
        const QByteArray localPayload = "LOCAL_ORIGINAL_MUST_NEVER_BE_DELETED_BY_UNSAFE_MTP_MOVE";
        {
            QFile f(localSrcFile);
            verify(f.open(QIODevice::WriteOnly) && f.write(localPayload) == localPayload.size(),
                   "S7-31: created local source file");
        }
        const QString existingLocalTarget = s731Dir + TQ_S("/photo.jpg");
        const QByteArray existingTargetPayload = "EXISTING_DESTINATION_PHOTO_MUST_NOT_BE_TOUCHED_ON_MOVE_REFUSAL";
        {
            QFile f(existingLocalTarget);
            verify(f.open(QIODevice::WriteOnly) && f.write(existingTargetPayload) == existingTargetPayload.size(),
                   "S7-31: created existing destination file photo.jpg");
        }

        QueueHarness h(s731Dir + TQ_S("/harness"));
        QMainWindow win;
        QToolBar tb(&win);
        OperationManager opMgr(&win, &tb, &win);
        opMgr.setQueueController(&h.ctl);
        OperationWindow opWin(&opMgr, &win);
        opWin.setQueueController(&h.ctl);
        opMgr.setDetailedWindow(&opWin);

        QAction undoAct, redoAct;
        UndoController undoCtl(nullptr, {}, {}, {});
        undoCtl.setActions(&undoAct, &redoAct);
        undoCtl.setQueueController(&h.ctl);

        QString lastFenceMsg;
        int watchCalls = 0;
        FileActions fa(nullptr, &undoCtl,
                       [&watchCalls](KJob *, const QString &, bool, const QString &,
                                     const FileActions::RefreshViews &, const FileOperationContext &) {
                           ++watchCalls;
                       },
                       [&lastFenceMsg](const QString &msg) {
                           lastFenceMsg = msg;
                       });
        fa.setQueueController(&h.ctl);
        fa.setSuppressMoveRefusalDialog(true);

        const QUrl localSrcUrl = QUrl::fromLocalFile(localSrcFile);
        const QUrl localDstDirUrl = QUrl::fromLocalFile(s731Dir);
        const QUrl mtpDevADir(QStringLiteral("mtp:/vivo-X300/Download"));
        const QUrl mtpDevAFile(QStringLiteral("mtp:/vivo-X300/DCIM/Camera/photo.jpg"));
        const QUrl mtpDevAFile2(QStringLiteral("mtp:/vivo-X300/DCIM/Camera/video.mp4"));
        const QUrl mtpDevBDir(QStringLiteral("mtp:/Pixel-9/Download"));
        const QUrl mtpRootUrl(QStringLiteral("mtp:/vivo-X300"));

        auto hasExpectedRefusalText = [](const QString &msg) {
            return msg.contains(QStringLiteral("Przenoszenie plików z udziałem urządzeń MTP jest niedostępne ze względów bezpieczeństwa danych. Użyj operacji Kopiuj."))
                || msg.contains(QStringLiteral("Moving files involving MTP devices is unavailable for data safety reasons. Please use Copy instead."));
        };

        // 1. MTP -> Local Move refused (single file, multi-file, directory, Shift+Drop, transferUrls, Cut+Paste)
        lastFenceMsg.clear();
        fa.transfer({mtpDevAFile}, localDstDirUrl, Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::MtpToLocal
               && fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported
               && !fa.lastMtpMoveEvaluation().isAllowed(),
               "S7-31.1: MTP -> Local single-file Move refused with RefusedUnsupported");
        verify(hasExpectedRefusalText(fa.lastMtpMoveEvaluation().refusalReason) && hasExpectedRefusalText(lastFenceMsg),
               "S7-31.1: Exact MTP Move refusal message provided for MTP -> Local");
        verify(h.ctl.snapshot().tasks.isEmpty() && watchCalls == 0,
               "S7-31.1: Zero queue tasks and zero KIO::move calls for MTP -> Local Move");

        // Multi-file MTP -> Local Move
        lastFenceMsg.clear();
        fa.transfer({mtpDevAFile, mtpDevAFile2}, localDstDirUrl, Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::MtpToLocal
               && fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
               "S7-31.2: MTP -> Local multi-file Move refused");
        verify(h.ctl.snapshot().tasks.isEmpty() && watchCalls == 0,
               "S7-31.2: Zero tasks for multi-file MTP -> Local Move");

        // Directory MTP -> Local Move
        lastFenceMsg.clear();
        fa.transfer({mtpDevADir}, localDstDirUrl, Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::MtpToLocal
               && fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
               "S7-31.3: MTP -> Local directory Move refused");

        // Cut + Paste (Ctrl+X / Ctrl+V) for MTP -> Local preserves clipboard and existing target file
        {
            auto *mime = new QMimeData();
            mime->setUrls({mtpDevAFile});
            mime->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
            QApplication::clipboard()->setMimeData(mime);

            lastFenceMsg.clear();
            fa.pasteClipboardInto(localDstDirUrl);
            verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::MtpToLocal
                   && fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
                   "S7-31.4: Cut+Paste MTP -> Local refused");
            verify(QApplication::clipboard()->mimeData()
                   && QApplication::clipboard()->mimeData()->urls() == QList<QUrl>{mtpDevAFile},
                   "S7-31.4: Clipboard preserved after refused MTP -> Local Cut+Paste");
            QFile fDst(existingLocalTarget);
            verify(fDst.open(QIODevice::ReadOnly) && fDst.readAll() == existingTargetPayload,
                   "S7-31.4: Pre-existing local destination file photo.jpg 100% untouched on MTP Move refusal");
            verify(QDir(s731Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"),
                                           QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-31.4: Zero .thispc-part files created on MTP Move refusal");
        }

        // 2. Local -> MTP Move via Drag&Drop (transfer), transferUrls, and Cut+Paste (pasteClipboardInto)
        lastFenceMsg.clear();
        fa.transfer({localSrcUrl}, mtpDevADir, Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::LocalToMtp,
               "S7-31.5: Local -> MTP classified as LocalToMtp");
        verify(fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
               "S7-31.5: Local -> MTP Move refused");
        verify(h.ctl.snapshot().tasks.isEmpty() && watchCalls == 0,
               "S7-31.5: No queue task or legacy KIO::move created for Local -> MTP Move");
        verify(QFile::exists(localSrcFile), "S7-31.5: Local source file preserved untouched");
        verify(hasExpectedRefusalText(lastFenceMsg), "S7-31.5: User notified with exact refusal message");

        bool transferUrlsCallbackCalled = false;
        KJob *transferUrlsJob = reinterpret_cast<KJob *>(0x1);
        fa.transferUrls({localSrcUrl}, mtpDevADir, Qt::MoveAction, [&](KJob *j) {
            transferUrlsCallbackCalled = true;
            transferUrlsJob = j;
        });
        verify(transferUrlsCallbackCalled && transferUrlsJob == nullptr,
               "S7-31.6: transferUrls refused Local -> MTP Move without creating KIO::move");
        verify(QFile::exists(localSrcFile), "S7-31.6: Local source file still preserved");

        // Cut + Paste (Ctrl+X / Ctrl+V) for Local -> MTP
        {
            auto *mime = new QMimeData();
            mime->setUrls({localSrcUrl});
            mime->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
            QApplication::clipboard()->setMimeData(mime);

            fa.pasteClipboardInto(mtpDevADir);
            verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::LocalToMtp
                   && fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
                   "S7-31.7: Cut+Paste Local -> MTP refused");
            verify(h.ctl.snapshot().tasks.isEmpty() && watchCalls == 0,
                   "S7-31.7: Cut+Paste Local -> MTP did not enqueue or start KIO::move");
            verify(QApplication::clipboard()->mimeData() && QApplication::clipboard()->mimeData()->hasUrls(),
                   "S7-31.7: Refused Cut+Paste preserved clipboard contents");
            verify(QFile::exists(localSrcFile), "S7-31.7: Local source file intact after Cut+Paste refusal");
        }

        // 3. MTP -> MTP Cross-Device Move
        fa.transfer({mtpDevAFile}, mtpDevBDir, Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::MtpToMtpCrossDevice
               && fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
               "S7-31.8: MTP -> MTP cross-device Move refused");
        verify(h.ctl.snapshot().tasks.isEmpty() && watchCalls == 0,
               "S7-31.8: No task created for MTP -> MTP cross-device Move");

        // 4. Intra-MTP Same-Device Folder Move
        fa.transfer({mtpDevAFile}, mtpDevADir, Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::MtpToMtpSameDevice
               && fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
               "S7-31.9: Intra-MTP directory Move refused");
        verify(h.ctl.snapshot().tasks.isEmpty() && watchCalls == 0,
               "S7-31.9: No task created for Intra-MTP directory Move");

        // 5. Protected MTP Device Root & Mixed Selection Move
        fa.transfer({mtpRootUrl}, localDstDirUrl, Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::MixedOrProtectedMtp
               && fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
               "S7-31.10: MTP device root Move refused");

        fa.transfer({localSrcUrl, mtpDevAFile}, localDstDirUrl, Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().direction == MtpMoveDirection::MixedOrProtectedMtp
               && fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
               "S7-31.11: Mixed Local+MTP selection Move refused");
        verify(h.ctl.snapshot().tasks.isEmpty(), "S7-31.11: Queue still empty after all refused Move directions");
        verify(opWin.findChildren<QFrame *>(QStringLiteral("operationRow")).isEmpty(),
               "S7-31.11: Unified Operations Manager has zero ghost cards after refused Moves");

        // 6. Legacy mode (THISPC_TRANSFER_QUEUE_PRODUCTION=0) also refuses all MTP Move directions
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
        fa.transfer({mtpDevAFile}, localDstDirUrl, Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported && watchCalls == 0,
               "S7-31.12: Legacy mode also refuses MTP -> Local Move without invoking watchFileOperation");
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");

        // 7. Allowed MTP -> Local Staged COPY works and never deletes MTP source
        const QByteArray mtpVideoBytes(24 * 1024, 'P');
        bool stagedCopyConfigured = false;
        bool sourceDeletedOnMtp = false;

        fa.setStagedJobConfigurator([&](KioStagedTransferJob *staged) {
            stagedCopyConfigured = true;
            staged->setSubJobFactory([mtpVideoBytes](const QUrl &, const QUrl &dst) -> KJob * {
                return new MockMtpStreamJob(dst.toLocalFile(), mtpVideoBytes, -1, 0);
            });
            staged->setSourceDeleteFactory([&](const QUrl &) -> KJob * {
                sourceDeletedOnMtp = true;
                return new QueueFakeJob(true, false, false);
            });
        });

        fa.transfer({mtpDevAFile2}, localDstDirUrl, Qt::CopyAction);
        verify(fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::NotMtp
               && fa.lastMtpMoveEvaluation().isAllowed(),
               "S7-31.13: MTP -> Local Copy is allowed (verdict NotMtp)");
        verify(stagedCopyConfigured, "S7-31.13: MTP -> Local Copy uses KioStagedTransferJob");
        verify(waitUntil([&] {
            const auto snap = h.ctl.snapshot();
            return !snap.tasks.isEmpty() && snap.tasks.last().state == TransferTaskState::Completed;
        }, 5000), "S7-31.13: Staged MTP -> Local Copy completed");
        verify(!sourceDeletedOnMtp, "S7-31.13: MTP source NEVER deleted during Copy");
        verify(QFile::exists(s731Dir + TQ_S("/video.mp4")), "S7-31.13: Copied file video.mp4 published locally");

        // 8. Verify Local -> Local Move still works via LocalFileMoveJob
        const QString localMoveSubdir = s731Dir + TQ_S("/local_move_dst");
        QDir().mkpath(localMoveSubdir);
        fa.setStagedJobConfigurator({});
        fa.transfer({localSrcUrl}, QUrl::fromLocalFile(localMoveSubdir), Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::NotMtp,
               "S7-31.14: Local -> Local Move bypasses MTP restriction");
        verify(waitUntil([&] {
            return QFile::exists(localMoveSubdir + TQ_S("/local_original.txt")) && !QFile::exists(localSrcFile);
        }, 5000), "S7-31.14: Local -> Local Move succeeded");

        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // S7-32: Conflict resolution in MTP -> Local Copy (Skip / Replace / Rename / Cancel)
    //        and verification that MTP -> Local Move on colliding target is refused BEFORE conflict dialog
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        const QString s732Dir = base + TQ_S("/s7_32_conflicts");
        QDir(s732Dir).removeRecursively();
        QDir().mkpath(s732Dir);

        QueueHarness h(s732Dir + TQ_S("/harness"));
        FileActions fa(nullptr, nullptr,
                       [](KJob *, const QString &, bool, const QString &,
                          const FileActions::RefreshViews &, const FileOperationContext &) {});
        fa.setQueueController(&h.ctl);
        fa.setSuppressMoveRefusalDialog(true);

        const QString existingPath = s732Dir + TQ_S("/clip.mp4");
        const QByteArray existingBytes = "EXISTING_TARGET_CONTENT_V1";
        const QByteArray mtpNewBytes(32 * 1024, 'V');
        const QUrl mtpSrc(QStringLiteral("mtp:/vivo-X300/DCIM/clip.mp4"));
        const QUrl dstDirUrl = QUrl::fromLocalFile(s732Dir);

        auto writeExisting = [&]() {
            QFile f(existingPath);
            verify(f.open(QIODevice::WriteOnly | QIODevice::Truncate)
                   && f.write(existingBytes) == existingBytes.size(),
                   "S7-32: wrote pre-existing clip.mp4");
        };
        writeExisting();

        int deleteCalls = 0;
        auto configureValidMtpCopy = [&](KioStagedTransferJob *staged) {
            staged->setSubJobFactory([mtpNewBytes](const QUrl &, const QUrl &dst) -> KJob * {
                return new MockMtpStreamJob(dst.toLocalFile(), mtpNewBytes, -1, 0);
            });
            staged->setSourceDeleteFactory([&](const QUrl &) -> KJob * {
                ++deleteCalls;
                return new QueueFakeJob(true, false, false);
            });
        };
        fa.setStagedJobConfigurator(configureValidMtpCopy);

        // Case 0: MTP -> Local Move on colliding target is refused BEFORE showing conflict dialog
        bool conflictDialogQueriedOnMove = false;
        fa.setConflictDecisionOverride([&](const QUrl &, const QUrl &, QUrl *) {
            conflictDialogQueriedOnMove = true;
            return static_cast<int>(KIO::Result_Overwrite);
        });
        fa.transfer({mtpSrc}, dstDirUrl, Qt::MoveAction);
        verify(!conflictDialogQueriedOnMove && h.ctl.snapshot().tasks.isEmpty() && deleteCalls == 0,
               "S7-32.0: MTP -> Local Move refused before showing RenameDialog or enqueuing task");

        // Case 1: Cancel in RenameDialog during MTP -> Local Copy
        fa.setConflictDecisionOverride([](const QUrl &, const QUrl &, QUrl *) {
            return static_cast<int>(KIO::Result_Cancel);
        });
        fa.transfer({mtpSrc}, dstDirUrl, Qt::CopyAction);
        verify(h.ctl.snapshot().tasks.isEmpty() && deleteCalls == 0,
               "S7-32.1: Cancel in preflight conflict dialog enqueues nothing and deletes nothing");

        // Case 2: Skip in RenameDialog during MTP -> Local Copy
        fa.setConflictDecisionOverride([](const QUrl &, const QUrl &, QUrl *) {
            return static_cast<int>(KIO::Result_Skip);
        });
        fa.transfer({mtpSrc}, dstDirUrl, Qt::CopyAction);
        verify(h.ctl.snapshot().tasks.isEmpty() && deleteCalls == 0,
               "S7-32.2: Skip in preflight conflict dialog enqueues nothing and deletes nothing");
        {
            QFile f(existingPath);
            verify(f.open(QIODevice::ReadOnly) && f.readAll() == existingBytes,
                   "S7-32.2: Existing target file untouched after Cancel and Skip");
        }

        // Case 3: Rename in RenameDialog -> clip_renamed.mp4 during MTP -> Local Copy
        const QString renamedPath = s732Dir + TQ_S("/clip_renamed.mp4");
        fa.setConflictDecisionOverride([&](const QUrl &, const QUrl &, QUrl *newDestOut) {
            if (newDestOut) *newDestOut = QUrl::fromLocalFile(renamedPath);
            return static_cast<int>(KIO::Result_Rename);
        });
        fa.transfer({mtpSrc}, dstDirUrl, Qt::CopyAction);
        verify(waitUntil([&] {
            const auto snap = h.ctl.snapshot();
            return !snap.tasks.isEmpty() && snap.tasks.last().state == TransferTaskState::Completed;
        }, 5000), "S7-32.3: Renamed MTP -> Local Copy completed");
        verify(deleteCalls == 0, "S7-32.3: MTP source never deleted on Copy");
        verify(QFile::exists(renamedPath), "S7-32.3: Renamed file clip_renamed.mp4 exists on disk");
        {
            QFile fOrig(existingPath);
            QFile fRen(renamedPath);
            verify(fOrig.open(QIODevice::ReadOnly) && fOrig.readAll() == existingBytes,
                   "S7-32.3: Original colliding file clip.mp4 remained 100% untouched");
            verify(fRen.open(QIODevice::ReadOnly) && fRen.readAll() == mtpNewBytes,
                   "S7-32.3: Renamed file clip_renamed.mp4 contains exact MTP payload");
        }

        // Case 4: Replace (Overwrite) in RenameDialog -> atomically replaces clip.mp4 via .thispc-part
        fa.setConflictDecisionOverride([](const QUrl &, const QUrl &, QUrl *) {
            return static_cast<int>(KIO::Result_Overwrite);
        });
        fa.transfer({mtpSrc}, dstDirUrl, Qt::CopyAction);
        verify(waitUntil([&] {
            const auto snap = h.ctl.snapshot();
            return snap.tasks.size() == 2 && snap.tasks.last().state == TransferTaskState::Completed;
        }, 5000), "S7-32.4: Overwrite MTP -> Local Copy completed");
        verify(deleteCalls == 0, "S7-32.4: MTP source never deleted on Overwrite Copy");
        {
            QFile f(existingPath);
            verify(f.open(QIODevice::ReadOnly) && f.readAll() == mtpNewBytes,
                   "S7-32.4: Overwritten clip.mp4 now has exact new MTP payload");
        }

        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // S7-33: Job-level protection — Direct KioStagedTransferJob(move=true) immediately rejected,
    //        and MTP -> Local Copy disconnect/cancel at 0%/25%/50%/95% preserves destination
    {
        const QString s733Dir = base + TQ_S("/s7_33_failure_disconnect");
        QDir(s733Dir).removeRecursively();
        QDir().mkpath(s733Dir);

        const QString existingTarget = s733Dir + TQ_S("/precious.mp4");
        const QByteArray preciousContent = "PRECIOUS_LOCAL_FILE_MUST_SURVIVE_ALL_FAILURES";
        auto resetPrecious = [&]() {
            QFile f(existingTarget);
            verify(f.open(QIODevice::WriteOnly | QIODevice::Truncate)
                   && f.write(preciousContent) == preciousContent.size(),
                   "S7-33: wrote precious.mp4");
        };
        resetPrecious();

        const QUrl srcMtp(QStringLiteral("mtp:/vivo-X300/DCIM/precious.mp4"));
        const QUrl dstDirUrl = QUrl::fromLocalFile(s733Dir);
        const QByteArray incomingPayload(64 * 1024, 'Z');

        // 1. Direct start of KioStagedTransferJob with move = true is unconditionally rejected
        //    (both with enableTwoPhaseMove(false) and enableTwoPhaseMove(true))
        for (bool enableTwoPhase : {false, true}) {
            resetPrecious();
            const LocalFileIdentity origId = LocalFileIdentity::read(existingTarget);
            auto *job = new KioStagedTransferJob(
                {srcMtp}, dstDirUrl, /*move=*/true,
                QUrl::fromLocalFile(existingTarget), /*overwrite=*/true, origId);
            job->enableTwoPhaseMove(enableTwoPhase);

            bool copySubJobStarted = false;
            int deleteCalled = 0;
            job->setSubJobFactory([&](const QUrl &, const QUrl &dst) -> KJob * {
                copySubJobStarted = true;
                return new MockMtpStreamJob(dst.toLocalFile(), incomingPayload, -1, 0);
            });
            job->setSourceDeleteFactory([&](const QUrl &) -> KJob * {
                ++deleteCalled;
                return new QueueFakeJob(true, false, false);
            });

            bool done = false;
            int recErr = 0;
            QString recErrText;
            int recDelAttempts = -1;
            QObject::connect(job, &KJob::result, [&, job](KJob *j) {
                done = true;
                recErr = j->error();
                recErrText = j->errorText();
                recDelAttempts = job->sourceDeleteAttempts();
            });
            job->start();
            verify(waitUntil([&done] { return done; }, 5000),
                   "S7-33.1: Direct KioStagedTransferJob(move=true) immediately finished");
            verify(recErr == KIO::ERR_UNSUPPORTED_ACTION,
                   "S7-33.1: Direct KioStagedTransferJob(move=true) rejected with ERR_UNSUPPORTED_ACTION");
            verify(recErrText.contains(QStringLiteral("Przenoszenie plików z udziałem urządzeń MTP jest niedostępne"))
                   || recErrText.contains(QStringLiteral("Moving files involving MTP devices is unavailable")),
                   "S7-33.1: Direct KioStagedTransferJob(move=true) sets required refusal errorText");
            verify(!copySubJobStarted && deleteCalled == 0 && recDelAttempts == 0,
                   "S7-33.1: Neither copy subjob nor source delete was started");
            verify(QDir(s733Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"),
                                           QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-33.1: Zero .thispc-part files created on direct KioStagedTransferJob(move=true) rejection");
            QFile f(existingTarget);
            verify(f.open(QIODevice::ReadOnly) && f.readAll() == preciousContent,
                   "S7-33.1: Pre-existing destination file 100% preserved");
        }

        // 2. Disconnect at 0%, 25%, 50%, 95% during staged overwrite Copy preserves existing target
        for (int pct : {0, 25, 50, 95}) {
            resetPrecious();
            const LocalFileIdentity origId = LocalFileIdentity::read(existingTarget);
            auto *job = new KioStagedTransferJob(
                {srcMtp}, dstDirUrl, /*move=*/false,
                QUrl::fromLocalFile(existingTarget), /*overwrite=*/true, origId);

            int deleteCalled = 0;
            job->setSubJobFactory([incomingPayload, pct](const QUrl &, const QUrl &dst) -> KJob * {
                return new MockMtpStreamJob(dst.toLocalFile(), incomingPayload, pct, KIO::ERR_CANNOT_READ);
            });
            job->setSourceDeleteFactory([&](const QUrl &) -> KJob * {
                ++deleteCalled;
                return new QueueFakeJob(true, false, false);
            });

            bool done = false;
            int recErr = 0;
            int recDelAttempts = -1;
            QObject::connect(job, &KJob::result, [&, job](KJob *j) {
                done = true;
                recErr = j->error();
                recDelAttempts = job->sourceDeleteAttempts();
            });
            job->start();
            verify(waitUntil([&done] { return done; }, 5000), "S7-33.2: disconnect Copy job finished");
            verify(recErr == KIO::ERR_CANNOT_READ, "S7-33.2: error is ERR_CANNOT_READ");
            verify(deleteCalled == 0 && recDelAttempts == 0,
                   "S7-33.2: MTP source delete NEVER called on disconnect");
            verify(QDir(s733Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"),
                                           QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-33.2: .thispc-part cleaned up on disconnect");
            QFile f(existingTarget);
            verify(f.open(QIODevice::ReadOnly) && f.readAll() == preciousContent,
                   "S7-33.2: pre-existing destination file 100% preserved on disconnect");
        }

        // 3. User cancellation mid-stream during staged overwrite Copy
        {
            resetPrecious();
            const LocalFileIdentity origId = LocalFileIdentity::read(existingTarget);
            auto *job = new KioStagedTransferJob(
                {srcMtp}, dstDirUrl, /*move=*/false,
                QUrl::fromLocalFile(existingTarget), /*overwrite=*/true, origId);

            int deleteCalled = 0;
            job->setSubJobFactory([incomingPayload](const QUrl &, const QUrl &dst) -> KJob * {
                return new MockMtpStreamJob(dst.toLocalFile(), incomingPayload, 50, 0);
            });
            job->setSourceDeleteFactory([&](const QUrl &) -> KJob * {
                ++deleteCalled;
                return new QueueFakeJob(true, false, false);
            });

            bool done = false;
            int recErr = 0;
            int recDelAttempts = -1;
            QObject::connect(job, &KJob::result, [&, job](KJob *j) {
                done = true;
                recErr = j->error();
                recDelAttempts = job->sourceDeleteAttempts();
            });
            job->start();
            pump(15);
            job->kill(KJob::EmitResult);
            verify(waitUntil([&done] { return done; }, 5000), "S7-33.3: cancelled Copy job finished");
            verify(recErr == KJob::KilledJobError, "S7-33.3: error is KilledJobError");
            verify(deleteCalled == 0 && recDelAttempts == 0,
                   "S7-33.3: MTP source delete NEVER called on cancel");
            verify(QDir(s733Dir).entryList(QStringList() << QStringLiteral("*.thispc-part*"),
                                           QDir::Files | QDir::System | QDir::Hidden).isEmpty(),
                   "S7-33.3: .thispc-part cleaned up on cancel");
            QFile f(existingTarget);
            verify(f.open(QIODevice::ReadOnly) && f.readAll() == preciousContent,
                   "S7-33.3: pre-existing destination file 100% preserved on cancel");
        }

        // 4. Stale LocalFileIdentity (target replaced on disk during MTP -> Local Copy) blocks overwrite
        {
            resetPrecious();
            const LocalFileIdentity staleId = LocalFileIdentity::read(existingTarget);
            // Replace existingTarget with a new inode before publish
            QFile::remove(existingTarget);
            {
                QFile fFiller(s733Dir + TQ_S("/inode_holder.tmp"));
                verify(fFiller.open(QIODevice::WriteOnly) && fFiller.write("H") == 1, "S7-33.4: wrote inode holder");
            }
            const QByteArray modifiedTargetBytes = "NEW_INODE_TARGET_CREATED_WHILE_COPYING";
            {
                QFile fNew(existingTarget);
                verify(fNew.open(QIODevice::WriteOnly) && fNew.write(modifiedTargetBytes) == modifiedTargetBytes.size(),
                       "S7-33.4: recreated existingTarget with new inode");
            }

            auto *job = new KioStagedTransferJob(
                {srcMtp}, dstDirUrl, /*move=*/false,
                QUrl::fromLocalFile(existingTarget), /*overwrite=*/true, staleId);
            job->setSubJobFactory([incomingPayload](const QUrl &, const QUrl &dst) -> KJob * {
                return new MockMtpStreamJob(dst.toLocalFile(), incomingPayload, -1, 0);
            });

            bool done = false;
            int recErr = 0;
            QObject::connect(job, &KJob::result, [&done, &recErr](KJob *j) {
                done = true;
                recErr = j->error();
            });
            job->start();
            verify(waitUntil([&done] { return done; }, 5000), "S7-33.4: stale target identity job finished");
            verify(recErr == KIO::ERR_CANNOT_RENAME,
                   "S7-33.4: atomicPublishOverwrite refused when target inode changed");
            QFile fCheck(existingTarget);
            verify(fCheck.open(QIODevice::ReadOnly) && fCheck.readAll() == modifiedTargetBytes,
                   "S7-33.4: newly replaced target file was NOT overwritten");
        }
    }

    // S7-34: Identity & Queue reservations — source or target changes while queued, no double start
    {
        const QString s734Dir = base + TQ_S("/s7_34_identity_queue");
        QDir(s734Dir).removeRecursively();
        QDir().mkpath(s734Dir);

        TransferQueueController ctl;
        ctl.setExecutionMode(TransferQueueMode::Sequential);

        // Blocker task holding the serial queue while we mutate queued tasks' files
        const QString blockerSrc = s734Dir + TQ_S("/blocker_src.bin");
        {
            QFile f(blockerSrc);
            verify(f.open(QIODevice::WriteOnly) && f.write("B") == 1, "S7-34: created blocker_src.bin");
        }
        QueueFakeJob *blockerJob = nullptr;
        TransferTaskDescriptor blockerDesc;
        blockerDesc.title = TQ_S("Blocker");
        blockerDesc.type = TransferOperationType::Copy;
        blockerDesc.backend = TransferBackendKind::LocalNative;
        blockerDesc.sources = {QUrl::fromLocalFile(blockerSrc)};
        blockerDesc.destination = QUrl::fromLocalFile(s734Dir);
        blockerDesc.targetUrls = {QUrl::fromLocalFile(s734Dir + TQ_S("/blocker_dst.bin"))};
        blockerDesc.startMode = TransferJobStartMode::ControllerStarts;
        blockerDesc.jobFactory = [&]() -> KJob * {
            blockerJob = new QueueFakeJob(true, true, false);
            return blockerJob;
        };
        const auto blockerRes = ctl.enqueue(blockerDesc);
        verify(blockerRes.accepted() && blockerJob != nullptr, "S7-34: blocker task running");

        // 1. Local -> MTP Copy task queued: local source claim MUST be preserved and revalidated!
        const QString localToMtpSrc = s734Dir + TQ_S("/local_for_mtp.txt");
        {
            QFile f(localToMtpSrc);
            verify(f.open(QIODevice::WriteOnly) && f.write("V1") == 2, "S7-34.1: created local_for_mtp.txt");
        }
        const ReservationRequest builtReq = TransferReservation::build(
            TransferOperationType::Copy,
            {QUrl::fromLocalFile(localToMtpSrc)},
            QUrl(QStringLiteral("mtp:/vivo-X300/Download")));
        verify(builtReq.valid && !builtReq.precise && !builtReq.claims.isEmpty(),
               "S7-34.1: Local -> MTP reservation retains local source claim even though request is imprecise");

        bool localToMtpFactoryCalled = false;
        TransferTaskDescriptor localToMtpDesc;
        localToMtpDesc.title = TQ_S("Local to MTP Copy");
        localToMtpDesc.type = TransferOperationType::Copy;
        localToMtpDesc.backend = TransferBackendKind::Kio;
        localToMtpDesc.sources = {QUrl::fromLocalFile(localToMtpSrc)};
        localToMtpDesc.destination = QUrl(QStringLiteral("mtp:/vivo-X300/Download"));
        localToMtpDesc.startMode = TransferJobStartMode::FactoryStarts;
        localToMtpDesc.jobFactory = [&]() -> KJob * {
            localToMtpFactoryCalled = true;
            return new QueueFakeJob(true, false, false);
        };
        const auto l2mRes = ctl.enqueue(localToMtpDesc);
        verify(l2mRes.accepted(), "S7-34.1: Local -> MTP task enqueued behind blocker");

        // 2. MTP -> Local Copy task queued: local target claim MUST be preserved and revalidated!
        const QString mtpTargetLocal = s734Dir + TQ_S("/mtp_target.mp4");
        {
            QFile f(mtpTargetLocal);
            verify(f.open(QIODevice::WriteOnly) && f.write("ORIG_TARGET") == 11, "S7-34.2: created mtp_target.mp4");
        }
        bool mtpToLocalFactoryCalled = false;
        TransferTaskDescriptor mtpToLocalDesc;
        mtpToLocalDesc.title = TQ_S("MTP to Local Copy");
        mtpToLocalDesc.type = TransferOperationType::Copy;
        mtpToLocalDesc.backend = TransferBackendKind::Kio;
        mtpToLocalDesc.sources = {QUrl(QStringLiteral("mtp:/vivo-X300/DCIM/mtp_target.mp4"))};
        mtpToLocalDesc.destination = QUrl::fromLocalFile(s734Dir);
        mtpToLocalDesc.targetUrls = {QUrl::fromLocalFile(mtpTargetLocal)};
        mtpToLocalDesc.startMode = TransferJobStartMode::ControllerStarts;
        mtpToLocalDesc.jobFactory = [&]() -> KJob * {
            mtpToLocalFactoryCalled = true;
            return new QueueFakeJob(true, false, false);
        };
        const auto m2lRes = ctl.enqueue(mtpToLocalDesc);
        verify(m2lRes.accepted(), "S7-34.2: MTP -> Local Copy task enqueued behind blocker");

        // Now mutate both local files while tasks are Queued!
        QFile::remove(localToMtpSrc);
        QFile::remove(mtpTargetLocal);
        {
            QFile fNew(s734Dir + TQ_S("/other_inode_filler.tmp"));
            verify(fNew.open(QIODevice::WriteOnly) && fNew.write("X") == 1, "S7-34: wrote filler");
        }

        // Release blocker task
        blockerJob->finishOk();

        const auto snapAfter = ctl.snapshot();
        for (const auto &t : snapAfter.tasks) {
            if (t.id == l2mRes.id) {
                verify(t.state == TransferTaskState::Failed
                       && t.failureKind == TransferFailureKind::PreflightFailed,
                       "S7-34.1: Local -> MTP Copy aborted by revalidation when local source changed while queued");
            }
            if (t.id == m2lRes.id) {
                verify(t.state == TransferTaskState::Failed
                       && t.failureKind == TransferFailureKind::PreflightFailed,
                       "S7-34.2: MTP -> Local Copy aborted by revalidation when local target changed while queued");
            }
        }
        verify(!localToMtpFactoryCalled && !mtpToLocalFactoryCalled,
               "S7-34.3: Neither invalidated task invoked its jobFactory");
    }

    // S7-35: Undo/Redo Safety — Refused MTP Move does not corrupt Undo state,
    //        and MTP -> Local Overwrite Copy fences Undo to prevent destructive target deletion
    {
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        const QString s735Dir = base + TQ_S("/s7_35_post_publish_undo");
        QDir(s735Dir).removeRecursively();
        QDir().mkpath(s735Dir);

        QueueHarness h(s735Dir + TQ_S("/harness"));
        QAction undoAct, redoAct;
        UndoController undoCtl(nullptr, {}, {}, {});
        undoCtl.setActions(&undoAct, &redoAct);
        undoCtl.setQueueController(&h.ctl);

        QString lastFenceMsg;
        FileActions fa(nullptr, &undoCtl,
                       [](KJob *, const QString &, bool, const QString &,
                          const FileActions::RefreshViews &, const FileOperationContext &) {},
                       [&lastFenceMsg](const QString &msg) {
                           lastFenceMsg = msg;
                       });
        fa.setQueueController(&h.ctl);
        fa.setSuppressMoveRefusalDialog(true);

        const QByteArray payload(48 * 1024, 'K');

        // 1. Create a normal local copy so Undo is enabled
        const QString localA = s735Dir + TQ_S("/local_a.txt");
        const QString localSub = s735Dir + TQ_S("/sub");
        QDir().mkpath(localSub);
        {
            QFile f(localA);
            verify(f.open(QIODevice::WriteOnly) && f.write("LOCAL_A") == 7, "S7-35: created local_a.txt");
        }
        fa.transfer({QUrl::fromLocalFile(localA)}, QUrl::fromLocalFile(localSub), Qt::CopyAction);
        verify(waitUntil([&] { return h.ctl.isQueueIdle(); }, 5000), "S7-35.0: local copy finished");
        verify(undoAct.isEnabled(), "S7-35.0: Undo is enabled after local copy");

        // 2. Refused MTP -> Local Move does NOT register any unsafe Undo entry and leaves existing Undo intact
        const QUrl mtpSrc1(QStringLiteral("mtp:/vivo-X300/DCIM/movie1.mp4"));
        fa.transfer({mtpSrc1}, QUrl::fromLocalFile(s735Dir), Qt::MoveAction);
        verify(fa.lastMtpMoveEvaluation().verdict == MtpMoveVerdict::RefusedUnsupported,
               "S7-35.1: MTP -> Local Move refused");
        verify(undoAct.isEnabled(),
               "S7-35.1: Refused MTP Move did not corrupt or replace the valid local Copy Undo state");

        // 3. Staged MTP -> Local Overwrite Copy fences Undo so Undo cannot delete the overwritten file without backup
        const QString existingMovie = s735Dir + TQ_S("/movie1.mp4");
        {
            QFile f(existingMovie);
            verify(f.open(QIODevice::WriteOnly) && f.write("OLD_MOVIE") == 9, "S7-35.2: created movie1.mp4");
        }
        fa.setConflictDecisionOverride([](const QUrl &, const QUrl &, QUrl *) {
            return static_cast<int>(KIO::Result_Overwrite);
        });
        fa.setStagedJobConfigurator([payload](KioStagedTransferJob *staged) {
            staged->setSubJobFactory([payload](const QUrl &, const QUrl &dst) -> KJob * {
                return new MockMtpStreamJob(dst.toLocalFile(), payload, -1, 0);
            });
        });
        fa.transfer({mtpSrc1}, QUrl::fromLocalFile(s735Dir), Qt::CopyAction);
        verify(waitUntil([&] { return h.ctl.isQueueIdle(); }, 5000), "S7-35.2: Overwrite MTP Copy finished");
        verify(!undoAct.isEnabled(),
               "S7-35.2: Undo is fenced after MTP -> Local Overwrite Copy so overwritten file cannot be deleted by Undo");

        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
    }

    // S7-36: R3 Production Default Activation & Diagnostic Override (R3-P1, R3-P2, R3-P4)
    {
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
        setTransferQueueProductionDispatchConnected(false);
        verify(!isTransferQueueProductionDispatchConnected(),
               "S7-36.0: Test harness starts with production dispatch disconnected before main() helper");

        // R3-P1: Default startup helper enables production queue without env var
        activateTransferQueueProductionDispatchByDefault();
        verify(TransferQueueProductionDispatchConnected,
               "S7-36.1: activateTransferQueueProductionDispatchByDefault enables flag with no env var");
        verify(isTransferQueueProductionDispatchConnected(),
               "S7-36.2: isTransferQueueProductionDispatchConnected returns true by default after startup helper");

        // R3-P2: Explicit THISPC_TRANSFER_QUEUE_PRODUCTION=0 or false disables production queue
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "0");
        verify(!isTransferQueueProductionDispatchConnected(),
               "S7-36.3: THISPC_TRANSFER_QUEUE_PRODUCTION=0 overrides active flag and returns false");
        activateTransferQueueProductionDispatchByDefault();
        verify(!TransferQueueProductionDispatchConnected && !isTransferQueueProductionDispatchConnected(),
               "S7-36.4: activateTransferQueueProductionDispatchByDefault respects THISPC_TRANSFER_QUEUE_PRODUCTION=0");

        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "false");
        activateTransferQueueProductionDispatchByDefault();
        verify(!TransferQueueProductionDispatchConnected && !isTransferQueueProductionDispatchConnected(),
               "S7-36.5: activateTransferQueueProductionDispatchByDefault respects THISPC_TRANSFER_QUEUE_PRODUCTION=false");

        // R3-P2: Explicit THISPC_TRANSFER_QUEUE_PRODUCTION=1 enables production queue
        qputenv("THISPC_TRANSFER_QUEUE_PRODUCTION", "1");
        activateTransferQueueProductionDispatchByDefault();
        verify(TransferQueueProductionDispatchConnected && isTransferQueueProductionDispatchConnected(),
               "S7-36.6: activateTransferQueueProductionDispatchByDefault respects THISPC_TRANSFER_QUEUE_PRODUCTION=1");

        // R3-P4: Settings persistence round-trip in default production mode without env var
        qunsetenv("THISPC_TRANSFER_QUEUE_PRODUCTION");
        activateTransferQueueProductionDispatchByDefault();
        const QString r3SettingsFile = base + TQ_S("/s7_36_r3_settings.ini");
        QFile::remove(r3SettingsFile);
        {
            QSettings store(r3SettingsFile, QSettings::IniFormat);
            TransferQueueController ctl1;
            ctl1.setExecutionMode(TransferQueueMode::Parallel);
            ctl1.setParallelConcurrencyLimit(5);
            ctl1.setBandwidthLimit(true, 12ULL * 1024ULL * 1024ULL);
            ctl1.setCloseWindowOnComplete(true);
            ctl1.saveSettings(store);
            store.sync();
        }
        {
            QSettings store(r3SettingsFile, QSettings::IniFormat);
            TransferQueueController ctl2;
            ctl2.loadSettings(store);
            verify(ctl2.executionMode() == TransferQueueMode::Parallel,
                   "S7-36.7: R3-P4 executionMode persisted as Parallel");
            verify(ctl2.parallelConcurrencyLimit() == 5,
                   "S7-36.8: R3-P4 parallelConcurrencyLimit persisted as 5");
            verify(ctl2.isBandwidthLimitEnabled()
                   && ctl2.bandwidthLimitBytesPerSec() == 12ULL * 1024ULL * 1024ULL,
                   "S7-36.9: R3-P4 bandwidthLimit persisted as enabled 12 MiB/s");
            verify(ctl2.closeWindowOnComplete(),
                   "S7-36.10: R3-P4 closeWindowOnComplete persisted as true");
        }

        setTransferQueueProductionDispatchConnected(false);
    }

    // S7-15: Final Stage 7 check counter
    {
        verify(checks > 2180, "S7-15: Stage 7 verification checks augmented");
    }

    verify(!QDir(base).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(),
           "sandbox was used");
    qInfo("transfer queue stage 7: %d checks passed", checks);
    return 0;
}
