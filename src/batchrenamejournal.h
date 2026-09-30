/*
 * Conservative crash-inspection journal for a local Batch Rename exchange cycle.
 * This is NOT automatic recovery, a filesystem transaction, or an Undo command.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "batchrename.h"
#include "batchrenamerecovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <memory>
#include <utility>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

inline void batchRenameRecoveryFaultPoint(const char *name)
{
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
    if (qgetenv("THISPC_RECOVERY_KILL_AT") == QByteArray(name)) ::raise(SIGKILL);
#else
    Q_UNUSED(name)
#endif
}

class BatchRenameRecoveryJournal final
{
public:
    explicit BatchRenameRecoveryJournal(QString root = defaultRoot())
        : m_root(std::move(root))
    {
    }

    static QString defaultRoot()
    {
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + QStringLiteral("/thispc-view/batch-rename-recovery");
    }

    static QString pendingJournal(const QString &root = defaultRoot())
    {
        const QFileInfoList files = QDir(root).entryInfoList(
            {QStringLiteral("cycle-*.json"), QStringLiteral("linear-*.json"),
             QStringLiteral("swap-*.json")},
            QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name);
        return files.isEmpty() ? QString() : files.first().absoluteFilePath();
    }

    bool begin(const BatchRenamePlan &plan, const QList<int> &cycle, QString *error = nullptr)
    {
        return beginFromState(plan, cycle, batchRenameCycleInitialState(cycle), error);
    }

    // Undo starts from the *completed* inode permutation. Never require the
    // original source ordering before consulting an unresolved journal.
    bool beginFromState(const BatchRenamePlan &plan, const QList<int> &cycle,
                        const BatchRenameCycleState &initialState, QString *error = nullptr)
    {
        if (!m_path.isEmpty())
            return fail(error, QStringLiteral("Invalid recovery-journal state or stale cycle."));
        if (!acquireProtocolGate(error)) return false;
        const QFileInfo recoveryDirectory(m_root);
        if (recoveryDirectory.isSymLink() || !recoveryDirectory.isDir()
            || !QFile::setPermissions(m_root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                | QFileDevice::ExeOwner))
            return fail(error, QStringLiteral("Cannot secure recovery directory: ") + m_root);
        m_lock = std::make_unique<QLockFile>(m_root + QStringLiteral("/cycle.lock"));
        if (!m_lock->tryLock(0)) {
            m_lock.reset();
            return fail(error, QStringLiteral("Another recovery journal is locked. No files were changed."));
        }
        const QString pending = pendingJournal(m_root);
        if (!pending.isEmpty()) {
            m_lock.reset();
            return fail(error, QStringLiteral("Unresolved cycle journal: ") + pending);
        }
        if (!batchRenameCycleSnapshotsMatch(plan, cycle, initialState)) {
            m_lock.reset();
            return fail(error, QStringLiteral("Invalid recovery-journal state or stale cycle."));
        }
        m_path = m_root + QStringLiteral("/cycle-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json");
        m_record.insert(QStringLiteral("schema"), 1);
        m_record.insert(QStringLiteral("scope"), QStringLiteral("manual-inspection-only"));
        m_record.insert(QStringLiteral("directory"),
                        QFileInfo(plan.entries.at(cycle.first()).source.toLocalFile()).absolutePath());
        QJsonArray items;
        for (int row : cycle) {
            const BatchRenameEntry &entry = plan.entries.at(row);
            QJsonObject item;
            item.insert(QStringLiteral("row"), row);
            item.insert(QStringLiteral("source"), entry.source.toLocalFile());
            item.insert(QStringLiteral("destination"), entry.destination.toLocalFile());
#ifndef Q_OS_WIN
            // JSON numbers cannot exactly represent arbitrary 64-bit device/inode values.
            item.insert(QStringLiteral("device"), QString::number(entry.sourceDevice));
            item.insert(QStringLiteral("inode"), QString::number(entry.sourceInode));
#endif
            items.append(item);
        }
        m_record.insert(QStringLiteral("originals"), items);
        QJsonArray cycleRows;
        for (int row : cycle) cycleRows.append(row);
        m_record.insert(QStringLiteral("cycleRows"), cycleRows);
        return checkpoint(QStringLiteral("prepared"), initialState, error);
    }

    // A separate manifest format for native no-overwrite history replays.
    // Uses the SAME lock and pending-journal fence as exchange cycles.
    bool beginLinear(const BatchRenamePlan &plan, bool undo, QString *error = nullptr,
                     bool recoverableExecute = false, bool recoverableHistory = false)
    {
        if (!m_path.isEmpty() || !plan.isValid() || plan.executionOrder.size() < 2
            || !plan.atomicSwaps.isEmpty() || !plan.exchangeCycles.isEmpty()
            || plan.executionOrder.size() != plan.activeCount())
            return fail(error, QStringLiteral("Invalid linear journal state."));
        if (!acquireProtocolGate(error)) return false;
        const QFileInfo recoveryDirectory(m_root);
        if (recoveryDirectory.isSymLink() || !recoveryDirectory.isDir()
            || !QFile::setPermissions(m_root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                | QFileDevice::ExeOwner))
            return fail(error, QStringLiteral("Cannot secure recovery directory: ") + m_root);
        m_lock = std::make_unique<QLockFile>(m_root + QStringLiteral("/cycle.lock"));
        if (!m_lock->tryLock(0)) {
            m_lock.reset();
            return fail(error, QStringLiteral("Another recovery journal is locked. No files changed."));
        }
        const QString pending = pendingJournal(m_root);
        if (!pending.isEmpty()) {
            m_lock.reset();
            return fail(error, QStringLiteral("Unresolved batch journal: ") + pending);
        }
        m_path = m_root + QStringLiteral("/linear-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json");
        if (recoverableExecute && (undo || recoverableHistory))
            return fail(error, QStringLiteral("Invalid v2 linear direction."));
        m_v2 = recoverableExecute || recoverableHistory;
        m_record.insert(QStringLiteral("schema"), m_v2 ? 2 : 1);
        m_record.insert(QStringLiteral("scope"), m_v2 ? QStringLiteral("recovery-audit-v2")
            : QStringLiteral("linear-history-manual-inspection"));
        m_record.insert(QStringLiteral("direction"), recoverableExecute ? QStringLiteral("forward")
            : (undo ? QStringLiteral("undo") : QStringLiteral("redo")));
        if (m_v2) {
            const QString directory = QFileInfo(plan.entries.at(plan.executionOrder.first())
                                                    .source.toLocalFile()).absolutePath();
            struct stat directoryStat {};
            if (::lstat(QFile::encodeName(directory).constData(), &directoryStat) != 0
                || !S_ISDIR(directoryStat.st_mode))
                return fail(error, QStringLiteral("Cannot snapshot the data directory."));
            m_record.insert(QStringLiteral("kind"), QStringLiteral("linear"));
            m_record.insert(QStringLiteral("directory"), directory);
            m_record.insert(QStringLiteral("directoryDevice"),
                            QString::number(quint64(directoryStat.st_dev)));
            m_record.insert(QStringLiteral("directoryInode"),
                            QString::number(quint64(directoryStat.st_ino)));
        }
        QJsonArray items;
        for (int row : plan.executionOrder) {
            const auto &entry = plan.entries.at(row);
            QJsonObject item;
            item.insert(QStringLiteral("row"), row);
            item.insert(QStringLiteral("source"), entry.source.toLocalFile());
            item.insert(QStringLiteral("destination"), entry.destination.toLocalFile());
#ifndef Q_OS_WIN
            item.insert(QStringLiteral("device"), QString::number(entry.sourceDevice));
            item.insert(QStringLiteral("inode"), QString::number(entry.sourceInode));
            if (m_v2) {
                struct stat value {};
                const QString occupiedPath = undo ? entry.destination.toLocalFile()
                                                  : entry.source.toLocalFile();
                if (::lstat(QFile::encodeName(occupiedPath).constData(), &value) != 0
                    || quint64(value.st_dev) != entry.sourceDevice
                    || quint64(value.st_ino) != entry.sourceInode)
                    return fail(error, QStringLiteral("Source changed while preparing v2 journal."));
                item.insert(QStringLiteral("mode"), QString::number(quint64(value.st_mode)));
                item.insert(QStringLiteral("size"), QString::number(qint64(value.st_size)));
                item.insert(QStringLiteral("mtimeNs"), QString::number(
                    qint64(value.st_mtim.tv_sec) * 1000000000LL + value.st_mtim.tv_nsec));
                item.insert(QStringLiteral("type"), S_ISLNK(value.st_mode) ? QStringLiteral("symlink")
                    : (S_ISDIR(value.st_mode) ? QStringLiteral("directory") : QStringLiteral("file")));
            }
#endif
            items.append(item);
        }
        m_record.insert(m_v2 ? QStringLiteral("items") : QStringLiteral("executionOrder"), items);
        batchRenameRecoveryFaultPoint("before-prepared");
        const bool prepared = checkpointLinear(QStringLiteral("prepared"), 0, error);
        if (prepared) batchRenameRecoveryFaultPoint("after-prepared");
        return prepared;
    }

    bool checkpointLinear(const QString &phase, int completedSteps, QString *error = nullptr)
    {
        BatchRenameCycleState state;
        state.stepsDone = completedSteps;
        return checkpoint(phase, state, error);
    }

    bool finishLinear(const QString &phase, int completedSteps, QString *error = nullptr)
    {
        BatchRenameCycleState state;
        state.stepsDone = completedSteps;
        return finish(phase, state, error);
    }

    bool checkpoint(const QString &phase, const BatchRenameCycleState &state,
                    QString *error = nullptr)
    {
        if (m_path.isEmpty())
            return fail(error, QStringLiteral("Recovery journal has not been initialized."));
        m_record.insert(QStringLiteral("phase"), phase);
        m_record.insert(m_v2 ? QStringLiteral("completedSteps")
                            : QStringLiteral("verifiedSteps"), state.stepsDone);
        QJsonArray occupants;
        for (int row : state.occupants) occupants.append(row);
        if (!m_v2) m_record.insert(QStringLiteral("occupants"), occupants);
        if (m_v2) m_record.insert(QStringLiteral("digest"),
                                  BatchRenameRecoveryDetail::digest(m_record));
        QSaveFile file(m_path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            return fail(error, QStringLiteral("Cannot write cycle journal: ") + m_path);
        const QByteArray data = QJsonDocument(m_record).toJson(QJsonDocument::Indented);
        if (file.write(data) != data.size() || !file.commit())
            return fail(error, QStringLiteral("Cannot commit cycle journal: ") + m_path);
        if (!syncJournal())
            return fail(error, QStringLiteral("Cannot synchronize cycle journal: ") + m_path);
        return true;
    }

    // Only call after verifying EVERY inode in the complete final/restored mapping.
    bool finish(const QString &finalPhase, const BatchRenameCycleState &state,
                QString *error = nullptr)
    {
        if (!checkpoint(finalPhase, state, error)) return false;
        if (m_v2) batchRenameRecoveryFaultPoint("after-goal-before-unlink");
        if (!QFile::remove(m_path))
            return fail(error, QStringLiteral("Cycle journal remains; inspect it: ") + m_path);
        if (m_v2) batchRenameRecoveryFaultPoint("after-unlink-before-journal-dir-fsync");
        if (!syncDirectory())
            return fail(error, QStringLiteral("Cycle journal cleanup is not durable: ") + m_path);
        m_path.clear();
        m_lock.reset();
        return true;
    }

    QString path() const { return m_path; }

    int openDataDirectory(QString *error = nullptr) const
    {
#ifdef Q_OS_LINUX
        const QByteArray path = QFile::encodeName(m_record.value(QStringLiteral("directory")).toString());
        const int fd = ::open(path.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            fail(error, QStringLiteral("Cannot open the recorded data directory."));
            return -1;
        }
        struct stat value {};
        if (::fstat(fd, &value) != 0
            || quint64(value.st_dev) != m_record.value(QStringLiteral("directoryDevice")).toString().toULongLong()
            || quint64(value.st_ino) != m_record.value(QStringLiteral("directoryInode")).toString().toULongLong()) {
            ::close(fd);
            fail(error, QStringLiteral("Data directory identity changed."));
            return -1;
        }
        return fd;
#else
        fail(error, QStringLiteral("Crash recovery requires Linux directory fsync."));
        return -1;
#endif
    }

    bool syncDataDirectoryFd(int fd, QString *error = nullptr) const
    {
#ifdef Q_OS_LINUX
        return (fd >= 0 && ::fsync(fd) == 0)
            || fail(error, QStringLiteral("Cannot durably synchronize the data directory."));
#else
        Q_UNUSED(fd)
        return fail(error, QStringLiteral("Crash recovery requires Linux directory fsync."));
#endif
    }

    bool syncDataDirectory(QString *error = nullptr) const
    {
#ifdef Q_OS_LINUX
        const int fd = openDataDirectory(error);
        if (fd < 0) return false;
        const bool ok = syncDataDirectoryFd(fd, error);
        ::close(fd);
        return ok;
#else
        return fail(error, QStringLiteral("Crash recovery requires Linux directory fsync."));
#endif
    }

private:
    bool acquireProtocolGate(QString *error)
    {
        if (BatchRenameRecoveryGate::processGateOwns(m_root)) return true;
        m_protocolGate = std::make_unique<BatchRenameRecoveryGate>(m_root);
        if (!m_protocolGate->ownsProtocolLock() || m_protocolGate->mutationsBlocked()) {
            m_protocolGate.reset();
            return fail(error, QStringLiteral("Recovery audit lock is unavailable. No files were changed."));
        }
        return true;
    }

    static bool fail(QString *error, const QString &message)
    {
        if (error) *error = message;
        return false;
    }

    bool syncDirectory() const
    {
#ifdef Q_OS_LINUX
        const QByteArray path = QFile::encodeName(m_root);
        const int fd = ::open(path.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return false;
        const bool ok = ::fsync(fd) == 0;
        ::close(fd);
        return ok;
#else
        return true; // No crash-durability promise on non-Linux; cycles are Linux-only.
#endif
    }

    bool syncJournal() const
    {
#ifdef Q_OS_LINUX
        const QByteArray path = QFile::encodeName(m_path);
        const int fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return false;
        const bool ok = ::fsync(fd) == 0;
        ::close(fd);
        return ok && syncDirectory();
#else
        return true;
#endif
    }

    QString m_root;
    QString m_path;
    QJsonObject m_record;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<BatchRenameRecoveryGate> m_protocolGate;
    bool m_v2 = false;
};

// Result distinguishes an ordinary failed syscall (starting permutation
// restored) from a genuinely unresolved state that requires manual inspection.
struct BatchRenameCycleReplayResult
{
    bool success = false;
    bool uncertain = false;
    QString error;
};

// Called from a worker thread by UndoController; the GUI never waits for
// journal fsyncs. Only the EXACT original snapshot and one isolated cycle
// are accepted. A failure during exchange replays the steps back towards
// the starting permutation, with durable checkpoints for every syscall.
inline BatchRenameCycleReplayResult batchRenameReplaySingleCycle(
    const BatchRenamePlan &plan, bool undo,
    const QString &root = BatchRenameRecoveryJournal::defaultRoot()
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
    , int failBeforeStep = -1 // Fault injection exists ONLY in isolated test builds.
#endif
    )
{
    BatchRenameCycleReplayResult result;
    if (!batchRenameSingleCycleHistoryEligible(plan)) {
        result.error = QStringLiteral("Only a single isolated cycle is undoable.");
        return result;
    }
    const auto cycle = plan.exchangeCycles.first();
    auto state = undo ? batchRenameCycleFinalState(cycle)
                      : batchRenameCycleInitialState(cycle);
    const auto startingState = state;
    const int targetSteps = undo ? 0 : cycle.size() - 1;
    if (!batchRenameCycleSnapshotsMatch(plan, cycle, state)) {
        result.error = QStringLiteral("Cycle entries changed; no Undo/Redo was performed.");
        return result;
    }
    BatchRenameRecoveryJournal journal(root);
    if (!journal.beginFromState(plan, cycle, state, &result.error)) return result;

    // A missing durable intent, failed verified checkpoint, or changed inode
    // must NOT trigger speculative rollback: retain the journal instead.
    bool mayRestore = true;
    while (state.stepsDone != targetSteps) {
        if (!batchRenameCycleSnapshotsMatch(plan, cycle, state)
            || !journal.checkpoint(undo ? QStringLiteral("undo-intent")
                                        : QStringLiteral("redo-intent"), state,
                                   &result.error)) {
            mayRestore = false;
            if (result.error.isEmpty())
                result.error = QStringLiteral("Cycle contents changed during replay.");
            break;
        }
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
        if (state.stepsDone == failBeforeStep) {
            result.error = QStringLiteral("Injected exchange failure before syscall.");
            break;
        }
#endif
        const int step = undo ? state.stepsDone - 1 : state.stepsDone;
        if (!batchRenameCycleExchange(plan, cycle, state, step, &result.error)) {
            mayRestore = !state.uncertain;
            break;
        }
        state.stepsDone += undo ? -1 : 1;
        if (!journal.checkpoint(undo ? QStringLiteral("undo-verified")
                                     : QStringLiteral("redo-verified"), state,
                                &result.error)) {
            mayRestore = false;
            break;
        }
    }
    if (state.stepsDone == targetSteps && mayRestore) {
        if (!batchRenameCycleSnapshotsMatch(plan, cycle, state)) {
            mayRestore = false;
            result.error = QStringLiteral("Cycle metadata changed after replay.");
        } else if (journal.finish(undo ? QStringLiteral("verified-undo")
                                      : QStringLiteral("verified-redo"), state,
                                  &result.error)) {
            result.success = true;
            return result;
        } else {
            mayRestore = false;
        }
    }
    const QString failure = result.error;
    if (mayRestore) {
        // A failed syscall has a known pre-syscall state; reverse only the
        // verified exchanges. A normal error leaves the original history
        // action available for a later retry once the cause is removed.
        while (state.stepsDone != startingState.stepsDone) {
            if (!batchRenameCycleSnapshotsMatch(plan, cycle, state)
                || !journal.checkpoint(QStringLiteral("history-rollback-intent"),
                                       state, &result.error)) {
                mayRestore = false;
                break;
            }
            const bool forwards = state.stepsDone < startingState.stepsDone;
            const int step = forwards ? state.stepsDone : state.stepsDone - 1;
            if (!batchRenameCycleExchange(plan, cycle, state, step, &result.error)) {
                mayRestore = false;
                break;
            }
            state.stepsDone += forwards ? 1 : -1;
            if (!journal.checkpoint(QStringLiteral("history-rollback-verified"),
                                    state, &result.error)) {
                mayRestore = false;
                break;
            }
        }
        if (mayRestore && batchRenameCycleSnapshotsMatch(plan, cycle, state)
            && journal.finish(QStringLiteral("history-rollback-complete"),
                              state, &result.error)) {
            result.error = failure + QStringLiteral(" Original names were restored.");
            return result;
        }
    }
    result.uncertain = true;
    result.error = QStringLiteral("Manual inspection required. Recovery journal: ")
        + journal.path() + QStringLiteral("\n") + result.error;
    return result;
}
