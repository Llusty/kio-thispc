/*
 * Conservative journaled execution and Undo/Redo for local acyclic Batch Rename.
 * One user-visible action is NOT an atomic filesystem transaction.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "batchrenamejournal.h"

#include <QScopeGuard>
#include <QSet>

// Swaps and cycles require their separate exchange protocol. Do not attempt
// to replay a mixture as a succession of ordinary moves.
inline bool batchRenameLinearHistoryEligible(const BatchRenamePlan &plan)
{
#if defined(Q_OS_LINUX) && defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
    if (!plan.isValid() || plan.executionOrder.size() < 2
        || !plan.atomicSwaps.isEmpty() || !plan.exchangeCycles.isEmpty()
        || plan.executionOrder.size() != plan.activeCount()) return false;
    QSet<int> rows;
    QSet<QString> identities;
    const QString parent = QFileInfo(plan.entries.first().source.toLocalFile()).absolutePath();
    for (int row : plan.executionOrder) {
        if (row < 0 || row >= plan.entries.size() || rows.contains(row)) return false;
        rows.insert(row);
        const auto &entry = plan.entries.at(row);
        if (!entry.source.isLocalFile() || !entry.destination.isLocalFile()
            || QFileInfo(entry.source.toLocalFile()).absolutePath() != parent
            || QFileInfo(entry.destination.toLocalFile()).absolutePath() != parent) return false;
        // Hard-linked entries are not distinguishable by inode; do not expose
        // their ambiguous identities through a composite history command.
        const QString identity = QString::number(entry.sourceDevice)
            + QLatin1Char(':') + QString::number(entry.sourceInode);
        if (identities.contains(identity)) return false;
        identities.insert(identity);
    }
    return true;
#else
    Q_UNUSED(plan)
    return false;
#endif
}

// A full mapping check: verify the identity and metadata at EVERY occupied
// pathname and also verify that every vacant source/destination is still vacant.
// In particular, a late outsider in a hole must not become a rollback target.
inline bool batchRenameLinearMappingMatches(const BatchRenamePlan &plan,
                                            bool undo, int completedSteps)
{
    if (!batchRenameLinearHistoryEligible(plan) || completedSteps < 0
        || completedSteps > plan.executionOrder.size()) return false;
    QSet<QString> occupied;
    QSet<QString> touched;
    for (int position = 0; position < plan.executionOrder.size(); ++position) {
        const auto &entry = plan.entries.at(plan.executionOrder.at(position));
        const QString source = QDir::cleanPath(entry.source.toLocalFile());
        const QString target = QDir::cleanPath(entry.destination.toLocalFile());
        touched.insert(source);
        touched.insert(target);
        const bool alreadyMoved = undo
            ? position >= plan.executionOrder.size() - completedSteps
            : position < completedSteps;
        const QUrl expected = undo
            ? (alreadyMoved ? entry.source : entry.destination)
            : (alreadyMoved ? entry.destination : entry.source);
        if (!batchRenameSnapshotAt(entry, expected)) return false;
        occupied.insert(QDir::cleanPath(expected.toLocalFile()));
    }
#ifdef Q_OS_LINUX
    for (const QString &path : std::as_const(touched)) {
        if (occupied.contains(path)) continue;
        struct stat value {};
        if (::lstat(QFile::encodeName(path).constData(), &value) == 0 || errno != ENOENT)
            return false;
    }
#endif
    return true;
}

// Reverse an individual rename using RENAME_NOREPLACE: even a race after
// preflight must not overwrite an unselected entry.
inline bool batchRenameLinearMove(const BatchRenameEntry &entry, bool towardsOriginal,
                                  QString *error, int directoryFd = AT_FDCWD)
{
    const QUrl from = towardsOriginal ? entry.destination : entry.source;
    const QUrl to = towardsOriginal ? entry.source : entry.destination;
    if (!batchRenameSnapshotAt(entry, from)) {
        if (error) *error = QStringLiteral("Source identity changed before linear history replay.");
        return false;
    }
#ifdef Q_OS_LINUX
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
    struct stat destination {};
    if (::lstat(QFile::encodeName(to.toLocalFile()).constData(), &destination) == 0
        || errno != ENOENT) {
        if (error) *error = QStringLiteral("Destination is no longer vacant. Refusing overwrite: ")
            + to.toLocalFile();
        return false;
    }
    const QByteArray sourceBytes = QFile::encodeName(directoryFd == AT_FDCWD
        ? from.toLocalFile() : QFileInfo(from.toLocalFile()).fileName());
    const QByteArray targetBytes = QFile::encodeName(directoryFd == AT_FDCWD
        ? to.toLocalFile() : QFileInfo(to.toLocalFile()).fileName());
    if (::syscall(SYS_renameat2, directoryFd, sourceBytes.constData(),
                  directoryFd, targetBytes.constData(), RENAME_NOREPLACE) != 0) {
        if (error) *error = QStringLiteral("No-overwrite rename failed: ")
            + QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    return true;
#else
    Q_UNUSED(to)
#endif
#else
    Q_UNUSED(to)
#endif
    if (error) *error = QStringLiteral("Linux renameat2(RENAME_NOREPLACE) is required.");
    return false;
}

// Worker-side replay. Error/cancel returns to the starting mapping when all
// steps remain identifiable. An ambiguous syscall or failed journal checkpoint
// stops immediately and retains the manifest for human inspection.
inline BatchRenameCycleReplayResult batchRenameReplayLinear(
    const BatchRenamePlan &plan, bool undo,
    const QString &root = BatchRenameRecoveryJournal::defaultRoot()
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
    , int failBeforeStep = -1, bool recoverableExecute = false,
    bool recoverableHistory = false
#else
    , bool recoverableExecute = false, bool recoverableHistory = false
#endif
    )
{
    BatchRenameCycleReplayResult result;
    if (!batchRenameLinearMappingMatches(plan, undo, 0)) {
        result.error = trLocal(
            "Pliki lub ich inode zostały zmienione; nie wykonano Undo/Redo. "
            "Po bezpiecznym przywróceniu oczekiwanych plików można ponowić próbę.",
            "Files or their inode identities changed; no Undo/Redo was performed. "
            "After safely restoring the expected files, you can try again.");
        return result;
    }
    BatchRenameRecoveryJournal journal(root);
    const bool recoverableV2 = recoverableExecute || recoverableHistory;
    if (!journal.beginLinear(plan, undo, &result.error, recoverableExecute,
                             recoverableHistory)) return result;
    int dataDirectoryFd = -1;
    if (recoverableV2) {
        dataDirectoryFd = journal.openDataDirectory(&result.error);
        if (dataDirectoryFd < 0) {
            result.uncertain = true;
            result.error = QStringLiteral("Manual inspection required. Recovery journal: ")
                + journal.path() + QStringLiteral("\n") + result.error;
            return result;
        }
    }
    const auto closeDataDirectory = qScopeGuard([&] {
        if (dataDirectoryFd >= 0) ::close(dataDirectoryFd);
    });
    const int count = plan.executionOrder.size();
    int done = 0;
    bool mayRestore = true;
    QString failure;
    while (done < count) {
        if (recoverableV2) batchRenameRecoveryFaultPoint("before-intent");
        if (!batchRenameLinearMappingMatches(plan, undo, done)
            || !journal.checkpointLinear(recoverableV2 ? QStringLiteral("step-intent")
                                               : (undo ? QStringLiteral("undo-intent")
                                                       : QStringLiteral("redo-intent")), done,
                                         &result.error)) {
            if (result.error.isEmpty())
                result.error = QStringLiteral("Linear mapping changed before replay step.");
            mayRestore = false;
            break;
        }
        if (recoverableV2) batchRenameRecoveryFaultPoint("after-intent");
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
        if (done == failBeforeStep) {
            result.error = QStringLiteral("Injected failure before a linear rename.");
            break;
        }
#endif
        const int position = undo ? count - 1 - done : done;
        const auto &entry = plan.entries.at(plan.executionOrder.at(position));
        if (recoverableV2) batchRenameRecoveryFaultPoint("before-syscall");
        if (!batchRenameLinearMove(entry, undo, &result.error,
                                   recoverableV2 ? dataDirectoryFd : AT_FDCWD)) {
            mayRestore = batchRenameLinearMappingMatches(plan, undo, done);
            break;
        }
        ++done;
        if (recoverableV2) {
            batchRenameRecoveryFaultPoint("after-syscall");
            if (done == count) batchRenameRecoveryFaultPoint("after-last-rename-before-goal");
            batchRenameRecoveryFaultPoint("before-data-dir-fsync");
        }
        if (recoverableV2 && !journal.syncDataDirectoryFd(dataDirectoryFd, &result.error)) {
            mayRestore = false;
            break;
        }
        if (recoverableV2) batchRenameRecoveryFaultPoint("after-data-dir-fsync");
        if (!batchRenameLinearMappingMatches(plan, undo, done)
            || !journal.checkpointLinear(recoverableV2 ? QStringLiteral("step-verified")
                                               : (undo ? QStringLiteral("undo-verified")
                                                       : QStringLiteral("redo-verified")), done,
                                         &result.error)) {
            if (result.error.isEmpty())
                result.error = QStringLiteral("Linear mapping changed after rename.");
            mayRestore = false;
            break;
        }
        if (recoverableV2) batchRenameRecoveryFaultPoint("after-verified");
    }
    if (done == count && mayRestore) {
        if (journal.finishLinear(recoverableV2 ? QStringLiteral("verified-complete")
                                     : (undo ? QStringLiteral("verified-undo")
                                             : QStringLiteral("verified-redo")), done,
                                 &result.error)) {
            result.success = true;
            return result;
        }
        mayRestore = false;
    }
    failure = result.error;
    if (recoverableV2 && done > 0) mayRestore = false;
    while (mayRestore && done > 0) {
        if (!batchRenameLinearMappingMatches(plan, undo, done)
            || !journal.checkpointLinear(QStringLiteral("rollback-intent"), done,
                                         &result.error)) {
            mayRestore = false;
            break;
        }
        const int position = undo ? count - done : done - 1;
        const auto &entry = plan.entries.at(plan.executionOrder.at(position));
        if (!batchRenameLinearMove(entry, !undo, &result.error)) {
            mayRestore = false;
            break;
        }
        --done;
        if (!batchRenameLinearMappingMatches(plan, undo, done)
            || !journal.checkpointLinear(QStringLiteral("rollback-verified"), done,
                                         &result.error)) {
            mayRestore = false;
            break;
        }
    }
    if (mayRestore && batchRenameLinearMappingMatches(plan, undo, 0)
        && journal.finishLinear(QStringLiteral("verified-rollback"), 0, &result.error)) {
        result.error = failure + QStringLiteral(" Original mapping was restored.");
        return result;
    }
    result.uncertain = true;
    result.error = QStringLiteral("Manual inspection required. Recovery journal: ")
        + journal.path() + QStringLiteral("\n") + result.error;
    return result;
}

inline BatchRenameCycleReplayResult batchRenameReplayLinearHistoryV2(
    const BatchRenamePlan &plan, bool undo,
    const QString &root = BatchRenameRecoveryJournal::defaultRoot())
{
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
    return batchRenameReplayLinear(plan, undo, root, -1, false, true);
#else
    return batchRenameReplayLinear(plan, undo, root, false, true);
#endif
}

inline BatchRenameCycleReplayResult batchRenameExecuteLinearV2(
    const BatchRenamePlan &plan,
    const QString &root = BatchRenameRecoveryJournal::defaultRoot())
{
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
    return batchRenameReplayLinear(plan, false, root, -1, true);
#else
    return batchRenameReplayLinear(plan, false, root, true);
#endif
}
