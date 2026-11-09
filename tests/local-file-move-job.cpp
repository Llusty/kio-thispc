/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localfilemovejob.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>


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
        QTest::qWait(3);
    }
    return predicate();
}

static bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

static QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

static QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static void waitForResult(QSignalSpy &spy, const char *description)
{
    verify(waitUntil([&spy] { return spy.count() == 1; }, 10000), description);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    verify(temporary.isValid(), "temporary move directory exists");

    QByteArray payload(8 * 1024 * 1024, Qt::Uninitialized);
    for (qsizetype index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>((index * 19 + 7) & 0xff);
    }
    const QByteArray expectedHash =
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    const QString source = temporary.filePath(QStringLiteral("source.bin"));
    const QString destination = temporary.filePath(QStringLiteral("destination.bin"));
    verify(writeFile(source, payload), "cross-device-style source is created");

    LocalFileMoveJob move(
        QUrl::fromLocalFile(source), QUrl::fromLocalFile(destination), nullptr,
        LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
    move.setAutoDelete(false);
    QSignalSpy moved(&move, &KJob::result);
    move.start();
    verify(waitUntil([&move] {
               return move.processedAmount(KJob::Bytes) >= 256 * 1024;
           }, 3000),
           "copy-before-remove move reports progress");
    verify(move.suspend(), "copy-before-remove move can be paused exactly");
    const qulonglong pausedBytes = move.processedAmount(KJob::Bytes);
    const QString partial = destination + QStringLiteral(".thispc-part");
    const qint64 pausedSize = QFileInfo(partial).size();
    QTest::qWait(120);
    verify(move.processedAmount(KJob::Bytes) == pausedBytes
               && QFileInfo(partial).size() == pausedSize
               && QFileInfo::exists(source),
           "pause freezes the partial and retains the source");
    verify(move.resume(), "paused move resumes");
    waitForResult(moved, "copy-before-remove move emits one result");
    verify(move.error() == KJob::NoError
               && move.moveError() == LocalFileMoveError::None
               && !move.usedAtomicMove(),
           "forced copy-before-remove move succeeds through native copy");
    verify(!QFileInfo::exists(source)
               && QFileInfo::exists(destination)
               && fileHash(destination) == expectedHash
               && !QFileInfo::exists(partial),
           "source is removed only after byte-exact destination publication");

    const QString cancelSource = temporary.filePath(QStringLiteral("cancel-source.bin"));
    const QString cancelDestination = temporary.filePath(QStringLiteral("cancel-destination.bin"));
    verify(writeFile(cancelSource, payload), "cancel source is created");
    LocalFileMoveJob cancelled(
        QUrl::fromLocalFile(cancelSource), QUrl::fromLocalFile(cancelDestination), nullptr,
        LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
    cancelled.setAutoDelete(false);
    QSignalSpy cancelledResult(&cancelled, &KJob::result);
    cancelled.start();
    verify(waitUntil([&cancelled] {
               return cancelled.processedAmount(KJob::Bytes) >= 128 * 1024;
           }, 3000),
           "cancelled move reaches active copy state");
    verify(cancelled.kill(KJob::EmitResult), "active move can be cancelled");
    waitForResult(cancelledResult, "cancelled move emits one result");
    verify(waitUntil([&] { return !QFileInfo::exists(cancelDestination + QStringLiteral(".thispc-part")); }, 3000)
               && QFileInfo::exists(cancelSource)
               && !QFileInfo::exists(cancelDestination),
           "cancelled move retains source and removes partial destination");

    const QString removeSource = temporary.filePath(QStringLiteral("remove-failure-source"));
    const QString removeDestination = temporary.filePath(QStringLiteral("remove-failure-destination"));
    verify(writeFile(removeSource, "retain-me"), "source-removal failure fixture is created");
    LocalFileMoveJob removeFailure(
        QUrl::fromLocalFile(removeSource), QUrl::fromLocalFile(removeDestination), nullptr,
        LocalFileMoveStrategy::ForceCopyAndRemove, 4096, 0,
        [](const QString &) { return false; });
    removeFailure.setAutoDelete(false);
    QSignalSpy removeFailureResult(&removeFailure, &KJob::result);
    removeFailure.start();
    waitForResult(removeFailureResult, "source-removal failure emits one result");
    verify(removeFailure.moveError() == LocalFileMoveError::SourceRemove
               && QFileInfo::exists(removeSource)
               && QFileInfo::exists(removeDestination)
               && readFile(removeSource) == "retain-me"
               && readFile(removeDestination) == "retain-me",
           "failed source removal preserves both source and destination intact");

    LocalMoveHistory safetyHistory;
    verify(!safetyHistory.recordCompletedMove(
               QUrl::fromLocalFile(removeSource),
               QUrl::fromLocalFile(removeDestination)),
           "history rejects recording move when source still exists");
    verify(safetyHistory.count() == 0,
           "history remains empty after failed source removal");

    const QString atomicSource = temporary.filePath(QStringLiteral("atomic-source"));
    const QString atomicDestination = temporary.filePath(QStringLiteral("atomic-destination"));
    verify(writeFile(atomicSource, "atomic"), "same-filesystem source is created");
    LocalFileMoveJob atomic(
        QUrl::fromLocalFile(atomicSource), QUrl::fromLocalFile(atomicDestination));
    atomic.setAutoDelete(false);
    QSignalSpy atomicResult(&atomic, &KJob::result);
    atomic.start();
    waitForResult(atomicResult, "same-filesystem move emits one result");
    verify(atomic.error() == KJob::NoError && atomic.usedAtomicMove()
               && !QFileInfo::exists(atomicSource)
               && QFileInfo::exists(atomicDestination),
           "same-filesystem move uses atomic rename");

    LocalMoveHistory history;
    verify(!history.recordCompletedMove(
               QUrl::fromLocalFile(atomicSource),
               QUrl::fromLocalFile(temporary.filePath(QStringLiteral("missing")))),
           "history rejects an operation that did not complete");

    const QString historySource = temporary.filePath(QStringLiteral("history-source"));
    const QString historyDestination = temporary.filePath(QStringLiteral("history-destination"));
    verify(writeFile(historySource, "history"), "history source is created");
    LocalFileMoveJob recordedMove(
        QUrl::fromLocalFile(historySource), QUrl::fromLocalFile(historyDestination));
    recordedMove.setAutoDelete(false);
    verify(history.recordWhenCompleted(&recordedMove),
           "successful move is registered for completion tracking");
    QSignalSpy recordedResult(&recordedMove, &KJob::result);
    recordedMove.start();
    waitForResult(recordedResult, "tracked move completes");
    verify(history.count() == 1 && history.position() == 1
               && history.canUndo() && !history.canRedo(),
           "only the completed move enters native history");

    KJob *undo = history.undo();
    verify(undo != nullptr, "native Undo starts");
    QSignalSpy undoResult(undo, &KJob::result);
    waitForResult(undoResult, "native Undo completes");
    verify(QFileInfo::exists(historySource)
               && !QFileInfo::exists(historyDestination)
               && !history.canUndo() && history.canRedo(),
           "Undo reverses the move and advances history only after success");

    KJob *redo = history.redo();
    verify(redo != nullptr, "native Redo starts");
    QSignalSpy redoResult(redo, &KJob::result);
    waitForResult(redoResult, "native Redo completes");
    verify(!QFileInfo::exists(historySource)
               && QFileInfo::exists(historyDestination)
               && history.canUndo() && !history.canRedo(),
           "Redo reapplies the completed move");

    const QString failedSource = temporary.filePath(QStringLiteral("failed-source"));
    verify(writeFile(failedSource, "failed")
               && writeFile(temporary.filePath(QStringLiteral("occupied")), "occupied"),
           "failed tracked move fixture is created");
    LocalFileMoveJob failed(
        QUrl::fromLocalFile(failedSource),
        QUrl::fromLocalFile(temporary.filePath(QStringLiteral("occupied"))));
    failed.setAutoDelete(false);
    history.recordWhenCompleted(&failed);
    QSignalSpy failedResult(&failed, &KJob::result);
    failed.start();
    waitForResult(failedResult, "failed tracked move finishes");
    verify(history.count() == 1 && history.position() == 1,
           "failed move does not create an Undo record");

    const QString overwriteSource = temporary.filePath("overwrite-source");
    const QString overwriteDestination = temporary.filePath("overwrite-destination");
    verify(writeFile(overwriteSource, payload) && writeFile(overwriteDestination, "old destination"),
           "overwrite move fixtures are created");
    for (LocalFileMoveStrategy strategy : {LocalFileMoveStrategy::ForceCopyAndRemove, LocalFileMoveStrategy::Automatic}) {
        LocalFileMoveJob overwrite(
            QUrl::fromLocalFile(overwriteSource), QUrl::fromLocalFile(overwriteDestination), nullptr,
            strategy, 16 * 1024, 1,
            {}, true);
        overwrite.setAutoDelete(false);
        history.recordWhenCompleted(&overwrite);
        QSignalSpy result(&overwrite, &KJob::result);
        overwrite.start();
        waitForResult(result, "overwrite move emits result immediately");

        verify(overwrite.error() != 0 && overwrite.moveError() == LocalFileMoveError::InvalidInput,
               "overwrite move reports InvalidInput error fail-closed");
        verify(overwrite.errorText().contains(QStringLiteral("nie jest obecnie obsługiwane"))
                   || overwrite.errorText().contains(QStringLiteral("not currently supported")),
               "overwrite move reports clear unsupported error message");
        verify(readFile(overwriteDestination) == "old destination"
                   && fileHash(overwriteSource) == expectedHash,
               "overwrite move leaves both source and destination completely untouched");
        verify(!QFileInfo::exists(overwriteDestination + QStringLiteral(".thispc-part")),
               "overwrite move creates no partial files or backup files");
        verify(history.count() == 1 && history.position() == 1,
               "fail-closed overwrite move is never recorded in history as Completed");
    }

    // SEC-045-02: Atomic no-overwrite publication in LocalFileMoveJob (ForceCopyAndRemove)
    // preserves both source and concurrently created destination when overwrite = false.
    {
        const QString raceMoveSrc = temporary.filePath(QStringLiteral("sec04502-move-src.bin"));
        const QString raceMoveDst = temporary.filePath(QStringLiteral("sec04502-move-dst.bin"));
        verify(writeFile(raceMoveSrc, payload), "SEC-045-02: move source created");
        const QByteArray srcHashBefore = fileHash(raceMoveSrc);

        LocalFileMoveJob raceMove(
            QUrl::fromLocalFile(raceMoveSrc),
            QUrl::fromLocalFile(raceMoveDst),
            nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove,
            16 * 1024,
            1,
            {},
            false);
        raceMove.setAutoDelete(false);
        QSignalSpy raceMoveResult(&raceMove, &KJob::result);
        raceMove.start();
        verify(waitUntil([&] { return raceMove.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && raceMove.suspend(),
               "SEC-045-02: non-overwrite move pauses in flight");

        const QByteArray concurrentTargetBytes("concurrent-move-destination-keep-safe-04502");
        writeFile(raceMoveDst, concurrentTargetBytes);
        const QByteArray dstHashBefore = fileHash(raceMoveDst);

        verify(raceMove.resume(), "SEC-045-02: non-overwrite move resumes after destination creation");
        waitForResult(raceMoveResult, "SEC-045-02: non-overwrite move finishes after collision");
        verify(raceMove.moveError() == LocalFileMoveError::DestinationExists,
               "SEC-045-02: non-overwrite move reports DestinationExists on publication collision");
        verify(QFileInfo::exists(raceMoveSrc) && fileHash(raceMoveSrc) == srcHashBefore,
               "SEC-045-02: move source file is preserved with identical SHA-256 on collision");
        verify(QFileInfo::exists(raceMoveDst) && fileHash(raceMoveDst) == dstHashBefore
                   && readFile(raceMoveDst) == concurrentTargetBytes,
               "SEC-045-02: concurrent destination file is preserved with identical SHA-256");
        verify(!QFileInfo::exists(raceMoveDst + QStringLiteral(".thispc-part")),
               "SEC-045-02: move partial .thispc-part is cleaned up after collision");
    }

    // SEC-045-02.1: Atomic move (Automatic strategy) destination collision protects foreign file.
    {
        const QString secAtomicMoveSrc = temporary.filePath(QStringLiteral("sec045021-atomic-move-src.bin"));
        const QString secAtomicMoveDst = temporary.filePath(QStringLiteral("sec045021-atomic-move-dst.bin"));
        verify(writeFile(secAtomicMoveSrc, payload), "SEC-045-02.1: atomic move source created");
        const QByteArray srcHashBefore = fileHash(secAtomicMoveSrc);

        const QByteArray foreignData("FOREIGN-PRE-EXISTING-MOVE-DEST-045021");
        verify(writeFile(secAtomicMoveDst, foreignData), "SEC-045-02.1: foreign destination created");
        const QByteArray foreignHashBefore = fileHash(secAtomicMoveDst);

        LocalFileMoveJob atomicMove(
            QUrl::fromLocalFile(secAtomicMoveSrc),
            QUrl::fromLocalFile(secAtomicMoveDst),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            1,
            {},
            false);
        atomicMove.setAutoDelete(false);
        QSignalSpy atomicMoveResult(&atomicMove, &KJob::result);
        atomicMove.start();
        waitForResult(atomicMoveResult, "SEC-045-02.1: atomic move with pre-existing destination finishes");
        verify(atomicMove.moveError() == LocalFileMoveError::DestinationExists,
               "SEC-045-02.1: atomic move detects existing destination and reports DestinationExists");
        verify(fileHash(secAtomicMoveDst) == foreignHashBefore
                   && readFile(secAtomicMoveDst) == foreignData,
               "SEC-045-02.1: pre-existing foreign file is 100% untouched and never unlinked");
        verify(fileHash(secAtomicMoveSrc) == srcHashBefore,
               "SEC-045-02.1: move source file is 100% preserved with intact SHA-256");
    }

    // SEC-045-02.1: Fail-closed atomic move when RENAME_NOREPLACE is unsupported.
    {
        const QString secNoRenameSrc = temporary.filePath(QStringLiteral("sec045021-norename-src.bin"));
        const QString secNoRenameDst = temporary.filePath(QStringLiteral("sec045021-norename-dst.bin"));
        verify(writeFile(secNoRenameSrc, payload), "SEC-045-02.1: unsupported move source created");
        const QByteArray srcHashBefore = fileHash(secNoRenameSrc);

        simulateRenameNoReplaceUnsupported() = true;

        LocalFileMoveJob noRenameMove(
            QUrl::fromLocalFile(secNoRenameSrc),
            QUrl::fromLocalFile(secNoRenameDst),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            1,
            {},
            false);
        noRenameMove.setAutoDelete(false);
        QSignalSpy noRenameResult(&noRenameMove, &KJob::result);
        noRenameMove.start();
        waitForResult(noRenameResult, "SEC-045-02.1: unsupported move finishes");
        verify(noRenameMove.moveError() == LocalFileMoveError::AtomicMove
                   && noRenameMove.systemError() == EOPNOTSUPP,
               "SEC-045-02.1: unsupported RENAME_NOREPLACE reports AtomicMove error with EOPNOTSUPP");
        verify(noRenameMove.errorText().contains(QStringLiteral("nie obsługuje"))
                   || noRenameMove.errorText().contains(QStringLiteral("does not support")),
               "SEC-045-02.1: unsupported RENAME_NOREPLACE returns clear descriptive error message");
        verify(!QFileInfo::exists(secNoRenameDst),
               "SEC-045-02.1: move destination was never created on unsupported filesystem");
        verify(fileHash(secNoRenameSrc) == srcHashBefore,
               "SEC-045-02.1: move source is 100% preserved with intact SHA-256");

        simulateRenameNoReplaceUnsupported() = false;
    }

    // SEC-045-G2A: Scenario B - Source inode change during transfer triggers Verification error and preserves both files.
    {
        const QString srcB = temporary.filePath(QStringLiteral("sec045-inode-src.bin"));
        const QString dstB = temporary.filePath(QStringLiteral("sec045-inode-dst.bin"));
        verify(writeFile(srcB, payload), "SEC-045-G2A: Scenario B source created");
        LocalFileMoveJob moveB(
            QUrl::fromLocalFile(srcB), QUrl::fromLocalFile(dstB), nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
        moveB.setAutoDelete(false);
        QSignalSpy spyB(&moveB, &KJob::result);
        moveB.start();
        verify(waitUntil([&moveB] { return moveB.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && moveB.suspend(),
               "SEC-045-G2A: Scenario B move pauses in flight");
        QFile::remove(srcB);
        verify(writeFile(srcB, payload), "SEC-045-G2A: Scenario B source recreated with new inode");
        verify(moveB.resume(), "SEC-045-G2A: Scenario B move resumes after inode change");
        waitForResult(spyB, "SEC-045-G2A: Scenario B move finishes after inode change");
        verify(moveB.moveError() == LocalFileMoveError::Verification,
               "SEC-045-G2A: Scenario B reports Verification error on inode change");
        verify(QFileInfo::exists(srcB) && QFileInfo::exists(dstB),
               "SEC-045-G2A: Scenario B preserves both source and destination intact");
        verify(fileHash(srcB) == expectedHash && fileHash(dstB) == expectedHash,
               "SEC-045-G2A: Scenario B files have intact matching hash");
    }

    // SEC-045-G2A: Scenario C - Content modification without size change triggers Verification error and preserves both files.
    {
        const QString srcC = temporary.filePath(QStringLiteral("sec045-content-src.bin"));
        const QString dstC = temporary.filePath(QStringLiteral("sec045-content-dst.bin"));
        verify(writeFile(srcC, payload), "SEC-045-G2A: Scenario C source created");
        LocalFileMoveJob moveC(
            QUrl::fromLocalFile(srcC), QUrl::fromLocalFile(dstC), nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
        moveC.setAutoDelete(false);
        QSignalSpy spyC(&moveC, &KJob::result);
        moveC.start();
        verify(waitUntil([&moveC] { return moveC.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && moveC.suspend(),
               "SEC-045-G2A: Scenario C move pauses in flight");
        {
            QFile file(srcC);
            verify(file.open(QIODevice::ReadWrite), "SEC-045-G2A: Scenario C source opened for in-place edit");
            file.seek(0);
            char c = 0x5a;
            file.write(&c, 1);
            file.flush();
            file.close();
        }
        verify(moveC.resume(), "SEC-045-G2A: Scenario C move resumes after content modification");
        waitForResult(spyC, "SEC-045-G2A: Scenario C move finishes after content modification");
        verify(moveC.moveError() == LocalFileMoveError::Verification,
               "SEC-045-G2A: Scenario C reports Verification error on modified content/mtime");
        verify(QFileInfo::exists(srcC) && QFileInfo::exists(dstC),
               "SEC-045-G2A: Scenario C preserves both source and destination intact");
        verify(fileHash(dstC) == expectedHash,
               "SEC-045-G2A: Scenario C destination retained verified original payload");
    }

    // SEC-045-G2A: Scenario D - Symlink substitution before removal triggers Verification error and protects symlink.
    {
        const QString srcD = temporary.filePath(QStringLiteral("sec045-symlink-src.bin"));
        const QString dstD = temporary.filePath(QStringLiteral("sec045-symlink-dst.bin"));
        const QString targetD = temporary.filePath(QStringLiteral("sec045-symlink-target.bin"));
        verify(writeFile(srcD, payload) && writeFile(targetD, "symlink-target-payload"),
               "SEC-045-G2A: Scenario D fixtures created");
        LocalFileMoveJob moveD(
            QUrl::fromLocalFile(srcD), QUrl::fromLocalFile(dstD), nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
        moveD.setAutoDelete(false);
        QSignalSpy spyD(&moveD, &KJob::result);
        moveD.start();
        verify(waitUntil([&moveD] { return moveD.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && moveD.suspend(),
               "SEC-045-G2A: Scenario D move pauses in flight");
        QFile::remove(srcD);
        verify(QFile::link(targetD, srcD), "SEC-045-G2A: Scenario D source replaced with symlink");
        verify(moveD.resume(), "SEC-045-G2A: Scenario D move resumes after symlink substitution");
        waitForResult(spyD, "SEC-045-G2A: Scenario D move finishes after symlink substitution");
        verify(moveD.moveError() == LocalFileMoveError::Verification,
               "SEC-045-G2A: Scenario D reports Verification error on symlink substitution");
        verify(QFileInfo(srcD).isSymLink() && QFileInfo::exists(dstD),
               "SEC-045-G2A: Scenario D preserves symlink and destination intact");
        verify(fileHash(dstD) == expectedHash,
               "SEC-045-G2A: Scenario D destination retained original payload");
        verify(readFile(targetD) == "symlink-target-payload",
               "SEC-045-G2A: Scenario D symlink target file is completely untouched");
    }

    // SEC-045-G2A: Scenario E - Source truncation during transfer triggers Verification error and preserves both files.
    {
        const QString srcE = temporary.filePath(QStringLiteral("sec045-truncate-src.bin"));
        const QString dstE = temporary.filePath(QStringLiteral("sec045-truncate-dst.bin"));
        verify(writeFile(srcE, payload), "SEC-045-G2A: Scenario E source created");
        LocalFileMoveJob moveE(
            QUrl::fromLocalFile(srcE), QUrl::fromLocalFile(dstE), nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
        moveE.setAutoDelete(false);
        QSignalSpy spyE(&moveE, &KJob::result);
        moveE.start();
        verify(waitUntil([&moveE] { return moveE.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && moveE.suspend(),
               "SEC-045-G2A: Scenario E move pauses in flight");
        {
            QFile file(srcE);
            verify(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "SEC-045-G2A: Scenario E source truncated");
            file.write("truncated-data");
            file.close();
        }
        verify(moveE.resume(), "SEC-045-G2A: Scenario E move resumes after source truncation");
        waitForResult(spyE, "SEC-045-G2A: Scenario E move finishes after source truncation");
        verify(moveE.moveError() == LocalFileMoveError::Verification,
               "SEC-045-G2A: Scenario E reports Verification error on truncated source");
        verify(QFileInfo::exists(srcE) && QFileInfo::exists(dstE),
               "SEC-045-G2A: Scenario E preserves both source and destination intact");
    }

    // SEC-045-G2A: Scenario F - Source directory permission failure on removal preserves both source and destination.
    {
        const QString subDir = temporary.filePath(QStringLiteral("readonly-dir"));
        verify(QDir().mkdir(subDir), "SEC-045-G2A: Scenario F subfolder created");
        const QString srcF = subDir + QStringLiteral("/src.bin");
        const QString dstF = temporary.filePath(QStringLiteral("readonly-dst.bin"));
        verify(writeFile(srcF, payload), "SEC-045-G2A: Scenario F source created");
        LocalFileMoveJob moveF(
            QUrl::fromLocalFile(srcF), QUrl::fromLocalFile(dstF), nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
        moveF.setAutoDelete(false);
        QSignalSpy spyF(&moveF, &KJob::result);
        moveF.start();
        verify(waitUntil([&moveF] { return moveF.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && moveF.suspend(),
               "SEC-045-G2A: Scenario F move pauses in flight");
        verify(::chmod(QFile::encodeName(subDir).constData(), 0555) == 0,
               "SEC-045-G2A: Scenario F parent dir set to 0555");
        verify(moveF.resume(), "SEC-045-G2A: Scenario F move resumes with read-only parent dir");
        waitForResult(spyF, "SEC-045-G2A: Scenario F move finishes with read-only parent dir");
        ::chmod(QFile::encodeName(subDir).constData(), 0755);
        verify(moveF.moveError() == LocalFileMoveError::SourceRemove,
               "SEC-045-G2A: Scenario F reports SourceRemove error on permission denied");
        verify(moveF.systemError() == EACCES,
               "SEC-045-G2A: Scenario F systemError is EACCES");
        verify(QFileInfo::exists(srcF) && QFileInfo::exists(dstF),
               "SEC-045-G2A: Scenario F preserves both source and destination intact on permission failure");
        verify(fileHash(srcF) == expectedHash && fileHash(dstF) == expectedHash,
               "SEC-045-G2A: Scenario F both files have identical intact SHA-256");
    }

    // SEC-045-G2A: Scenario C2 - Byte modification with forged/restored mtime fails cryptographic SHA-256 integrity verification.
    {
        const QString srcC2 = temporary.filePath(QStringLiteral("sec045-mtime-forge-src.bin"));
        const QString dstC2 = temporary.filePath(QStringLiteral("sec045-mtime-forge-dst.bin"));
        verify(writeFile(srcC2, payload), "SEC-045-G2A: Scenario C2 source created");
        struct stat origStat {};
        verify(::lstat(QFile::encodeName(srcC2).constData(), &origStat) == 0,
               "SEC-045-G2A: Scenario C2 captured original stat");

        LocalFileMoveJob moveC2(
            QUrl::fromLocalFile(srcC2), QUrl::fromLocalFile(dstC2), nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
        moveC2.setAutoDelete(false);
        QSignalSpy spyC2(&moveC2, &KJob::result);
        moveC2.start();
        verify(waitUntil([&moveC2] { return moveC2.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && moveC2.suspend(),
               "SEC-045-G2A: Scenario C2 move pauses in flight");

        // Modify byte in source
        {
            QFile file(srcC2);
            verify(file.open(QIODevice::ReadWrite), "SEC-045-G2A: Scenario C2 source opened for edit");
            file.seek(42);
            char c = 0x77;
            file.write(&c, 1);
            file.flush();
            file.close();
        }
        // Restore original mtime via utimensat to simulate forged mtime
        struct timespec times[2] = {
            origStat.st_atim,
            origStat.st_mtim
        };
        verify(::utimensat(AT_FDCWD, QFile::encodeName(srcC2).constData(), times, 0) == 0,
               "SEC-045-G2A: Scenario C2 mtime forged/restored via utimensat");

        // Verify that lstat alone would have seen identical mtime and size
        struct stat forgedStat {};
        verify(::lstat(QFile::encodeName(srcC2).constData(), &forgedStat) == 0
                   && forgedStat.st_size == origStat.st_size
                   && forgedStat.st_mtim.tv_sec == origStat.st_mtim.tv_sec
                   && forgedStat.st_mtim.tv_nsec == origStat.st_mtim.tv_nsec,
               "SEC-045-G2A: Scenario C2 confirms mtime and size match baseline exactly");

        verify(moveC2.resume(), "SEC-045-G2A: Scenario C2 move resumes");
        waitForResult(spyC2, "SEC-045-G2A: Scenario C2 move finishes");
        verify(moveC2.moveError() == LocalFileMoveError::Verification,
               "SEC-045-G2A: Scenario C2 cryptographic SHA-256 integrity check detected forged modification");
        verify(QFileInfo::exists(srcC2) && QFileInfo::exists(dstC2),
               "SEC-045-G2A: Scenario C2 both source and destination preserved intact");
        verify(fileHash(dstC2) == expectedHash,
               "SEC-045-G2A: Scenario C2 destination has verified original payload");
    }

    // SEC-045-G2A: Scenario M - Documenting TOCTOU window between fstatat() and unlinkat().
    {
        const QString srcM = temporary.filePath(QStringLiteral("sec045-race-src.bin"));
        const QString dstM = temporary.filePath(QStringLiteral("sec045-race-dst.bin"));
        verify(writeFile(srcM, payload), "SEC-045-G2A: Scenario M source created");

        bool hookTriggered = false;
        simulateRaceBeforeSourceUnlink() = [&]() {
            hookTriggered = true;
            // Simulate another process substituting the entry between fstatat and unlinkat
            QFile::remove(srcM);
            writeFile(srcM, "alien-replacement-in-race-window");
        };

        LocalFileMoveJob moveM(
            QUrl::fromLocalFile(srcM), QUrl::fromLocalFile(dstM), nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 0);
        moveM.setAutoDelete(false);
        QSignalSpy spyM(&moveM, &KJob::result);
        moveM.start();
        waitForResult(spyM, "SEC-045-G2A: Scenario M move finishes");

        simulateRaceBeforeSourceUnlink() = nullptr;

        verify(hookTriggered, "SEC-045-G2A: Scenario M race hook executed between fstatat and unlinkat");
        verify(QFileInfo::exists(dstM) && fileHash(dstM) == expectedHash,
               "SEC-045-G2A: Scenario M destination has complete verified payload");
    }

    // =========================================================================
    // Stage 5.3.2G.2B: Safe Same-Volume NTFS-3G Move (Scenarios A - I)
    // =========================================================================

    // Test A, C, H, I: Safe NTFS-3G Move in Automatic strategy moves file between directories,
    // verifies SHA-256 integrity, cleans staging, and records in history.
    {
        simulateRenameNoReplaceUnsupported() = true; // Native RENAME_NOREPLACE returns EINVAL on NTFS-3G
        simulateNtfs3gFilesystem() = true;           // Identified as verified same-volume NTFS-3G mount

        const QString subDirA1 = temporary.filePath(QStringLiteral("ntfs3g-dir-a1"));
        const QString subDirA2 = temporary.filePath(QStringLiteral("ntfs3g-dir-a2"));
        verify(QDir().mkdir(subDirA1) && QDir().mkdir(subDirA2), "G2B Test A: subdirectories created");

        const QString srcA = subDirA1 + QStringLiteral("/file-a.bin");
        const QString dstA = subDirA2 + QStringLiteral("/file-a-moved.bin");
        verify(writeFile(srcA, payload), "G2B Test A: source file created");

        LocalFileMoveJob moveA(
            QUrl::fromLocalFile(srcA),
            QUrl::fromLocalFile(dstA),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            0);
        moveA.setAutoDelete(false);
        history.recordWhenCompleted(&moveA);
        QSignalSpy spyA(&moveA, &KJob::result);
        moveA.start();
        waitForResult(spyA, "G2B Test A: NTFS-3G move finishes");

        verify(moveA.error() == KJob::NoError && moveA.moveError() == LocalFileMoveError::None,
               "G2B Test A: NTFS-3G move reports NoError");
        verify(!moveA.usedAtomicMove(),
               "G2B Test A: confirmed fallback to safe Copy+Verify+Publish+Remove engine (not broken atomic rename)");
        verify(!QFileInfo::exists(srcA),
               "G2B Test A: source file safely unlinked after verified publication");
        verify(QFileInfo::exists(dstA),
               "G2B Test A: destination file published");
        verify(fileHash(dstA) == expectedHash,
               "G2B Test A/C: destination file SHA-256 matches payload 100%");
        verify(!QFileInfo::exists(dstA + QStringLiteral(".thispc-part")),
               "G2B Test H: no orphaned .thispc-part staging file remains");
        verify(history.count() == 2 && history.position() == 2,
               "G2B Test I: successfully completed NTFS-3G move recorded in history");

        simulateRenameNoReplaceUnsupported() = false;
        simulateNtfs3gFilesystem() = false;
    }

    // Test B: Collision EEXIST on NTFS-3G Move preserves both existing destination and source
    {
        simulateRenameNoReplaceUnsupported() = true;
        simulateNtfs3gFilesystem() = true;

        const QString srcB = temporary.filePath(QStringLiteral("ntfs3g-col-src.bin"));
        const QString dstB = temporary.filePath(QStringLiteral("ntfs3g-col-dst.bin"));
        const QByteArray foreignBytes("FOREIGN_DESTINATION_DO_NOT_OVERWRITE_G2B");
        verify(writeFile(srcB, payload) && writeFile(dstB, foreignBytes),
               "G2B Test B: fixtures created");

        LocalFileMoveJob moveB(
            QUrl::fromLocalFile(srcB),
            QUrl::fromLocalFile(dstB),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            0);
        moveB.setAutoDelete(false);
        history.recordWhenCompleted(&moveB);
        QSignalSpy spyB(&moveB, &KJob::result);
        moveB.start();
        waitForResult(spyB, "G2B Test B: collision move finishes");

        verify(moveB.moveError() == LocalFileMoveError::DestinationExists,
               "G2B Test B: collision reports DestinationExists");
        verify(QFileInfo::exists(srcB) && fileHash(srcB) == expectedHash,
               "G2B Test B: source file is 100% preserved with intact SHA-256");
        verify(QFileInfo::exists(dstB) && readFile(dstB) == foreignBytes,
               "G2B Test B: pre-existing destination is 100% untouched and preserved");
        verify(!QFileInfo::exists(dstB + QStringLiteral(".thispc-part")),
               "G2B Test B/H: no orphaned .thispc-part staging file remains");
        verify(history.count() == 2,
               "G2B Test B/I: collided move is never recorded in history as Completed");

        simulateRenameNoReplaceUnsupported() = false;
        simulateNtfs3gFilesystem() = false;
    }

    // Test D: Source removal failure on NTFS-3G preserves published destination and source
    {
        simulateRenameNoReplaceUnsupported() = true;
        simulateNtfs3gFilesystem() = true;

        const QString srcD = temporary.filePath(QStringLiteral("ntfs3g-rmfail-src.bin"));
        const QString dstD = temporary.filePath(QStringLiteral("ntfs3g-rmfail-dst.bin"));
        verify(writeFile(srcD, payload), "G2B Test D: source fixture created");

        LocalFileMoveJob moveD(
            QUrl::fromLocalFile(srcD),
            QUrl::fromLocalFile(dstD),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            0,
            [](const QString &) { return false; }); // Custom remover simulating unlink failure
        moveD.setAutoDelete(false);
        QSignalSpy spyD(&moveD, &KJob::result);
        moveD.start();
        waitForResult(spyD, "G2B Test D: move finishes with removal failure");

        verify(moveD.moveError() == LocalFileMoveError::SourceRemove,
               "G2B Test D: reports SourceRemove error");
        verify(QFileInfo::exists(srcD) && fileHash(srcD) == expectedHash,
               "G2B Test D: source file remains intact");
        verify(QFileInfo::exists(dstD) && fileHash(dstD) == expectedHash,
               "G2B Test D: published destination is NEVER deleted or rolled back on removal error");
        verify(!QFileInfo::exists(dstD + QStringLiteral(".thispc-part")),
               "G2B Test D/H: staging file cleanly unlinked");

        simulateRenameNoReplaceUnsupported() = false;
        simulateNtfs3gFilesystem() = false;
    }

    // Test E: Source tampering during transfer on NTFS-3G triggers Verification error
    {
        simulateRenameNoReplaceUnsupported() = true;
        simulateNtfs3gFilesystem() = true;

        const QString srcE = temporary.filePath(QStringLiteral("ntfs3g-tamper-src.bin"));
        const QString dstE = temporary.filePath(QStringLiteral("ntfs3g-tamper-dst.bin"));
        verify(writeFile(srcE, payload), "G2B Test E: source fixture created");

        LocalFileMoveJob moveE(
            QUrl::fromLocalFile(srcE),
            QUrl::fromLocalFile(dstE),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            1);
        moveE.setAutoDelete(false);
        QSignalSpy spyE(&moveE, &KJob::result);
        moveE.start();
        verify(waitUntil([&moveE] { return moveE.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && moveE.suspend(),
               "G2B Test E: move pauses in flight");

        // Modify byte in source while paused
        {
            QFile file(srcE);
            verify(file.open(QIODevice::ReadWrite), "G2B Test E: source opened for in-place edit");
            file.seek(10);
            char c = 0x33;
            file.write(&c, 1);
            file.close();
        }

        verify(moveE.resume(), "G2B Test E: move resumes");
        waitForResult(spyE, "G2B Test E: move finishes after tamper");

        verify(moveE.moveError() == LocalFileMoveError::Verification,
               "G2B Test E: reports Verification error on tampered source");
        verify(QFileInfo::exists(srcE) && QFileInfo::exists(dstE),
               "G2B Test E: both source and destination preserved intact");

        simulateRenameNoReplaceUnsupported() = false;
        simulateNtfs3gFilesystem() = false;
    }

    // Test F & H: Cancellation before publication cleans up staging and preserves files
    {
        simulateRenameNoReplaceUnsupported() = true;
        simulateNtfs3gFilesystem() = true;

        const QString srcF = temporary.filePath(QStringLiteral("ntfs3g-cancel-src.bin"));
        const QString dstF = temporary.filePath(QStringLiteral("ntfs3g-cancel-dst.bin"));
        verify(writeFile(srcF, payload), "G2B Test F: source fixture created");

        LocalFileMoveJob moveF(
            QUrl::fromLocalFile(srcF),
            QUrl::fromLocalFile(dstF),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            1);
        moveF.setAutoDelete(false);
        QSignalSpy spyF(&moveF, &KJob::result);
        moveF.start();
        verify(waitUntil([&moveF] { return moveF.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && moveF.kill(KJob::EmitResult),
               "G2B Test F: move reaches progress and cancels");
        waitForResult(spyF, "G2B Test F: cancel emits result");

        verify(moveF.error() == KJob::KilledJobError
                   && moveF.moveError() == LocalFileMoveError::Cancelled,
               "G2B Test F: reports Cancelled / KilledJobError");
        verify(QFileInfo::exists(srcF) && fileHash(srcF) == expectedHash,
               "G2B Test F: source is untouched");
        verify(!QFileInfo::exists(dstF),
               "G2B Test F: destination was never published");
        verify(waitUntil([&dstF] {
                   return !QFileInfo::exists(dstF + QStringLiteral(".thispc-part"));
               }, 3000),
               "G2B Test F/H: staging .thispc-part is cleanly removed on cancel");

        simulateRenameNoReplaceUnsupported() = false;
        simulateNtfs3gFilesystem() = false;
    }

    // Test G: Destination on-disk data integrity verification (Step 1b)
    {
        simulateRenameNoReplaceUnsupported() = true;
        simulateNtfs3gFilesystem() = true;

        const QString srcG = temporary.filePath(QStringLiteral("ntfs3g-destverify-src.bin"));
        const QString dstG = temporary.filePath(QStringLiteral("ntfs3g-destverify-dst.bin"));
        verify(writeFile(srcG, payload), "G2B Test G: source fixture created");

        LocalFileMoveJob moveG(
            QUrl::fromLocalFile(srcG),
            QUrl::fromLocalFile(dstG),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            0);
        moveG.setAutoDelete(false);
        QSignalSpy spyG(&moveG, &KJob::result);
        moveG.start();
        waitForResult(spyG, "G2B Test G: move finishes with verified destination integrity");

        verify(moveG.error() == KJob::NoError && moveG.moveError() == LocalFileMoveError::None,
               "G2B Test G: reports NoError when destination on-disk hash matches");
        verify(fileHash(dstG) == expectedHash,
               "G2B Test G: destination SHA-256 verified on disk");

        simulateRenameNoReplaceUnsupported() = false;
        simulateNtfs3gFilesystem() = false;
    }

    // =========================================================================
    // Stage 5.3.2G.3A.1: Durability Gate Final Fix (Scenarios A - L)
    // =========================================================================

    // Test A: fsync of destination directory returns 0 — standard Move succeeds
    {
        const QString srcA = temporary.filePath(QStringLiteral("g3a1-testa-src.bin"));
        const QString dstA = temporary.filePath(QStringLiteral("g3a1-testa-dst.bin"));
        verify(writeFile(srcA, payload), "G3A.1 Test A: src created");

        LocalFileMoveJob moveA(
            QUrl::fromLocalFile(srcA),
            QUrl::fromLocalFile(dstA),
            nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove,
            16 * 1024,
            0);
        moveA.setAutoDelete(false);
        LocalMoveHistory histA;
        histA.recordWhenCompleted(&moveA);
        QSignalSpy spyA(&moveA, &KJob::result);
        moveA.start();
        waitForResult(spyA, "G3A.1 Test A: fsync 0 move finishes");

        verify(moveA.error() == KJob::NoError && moveA.moveError() == LocalFileMoveError::None,
               "G3A.1 Test A: move reports NoError");
        verify(moveA.destinationPublished(), "G3A.1 Test A: destination published");
        verify(moveA.directoryDurabilityConfirmed(), "G3A.1 Test A: directory durability confirmed");
        verify(!QFileInfo::exists(srcA) && QFileInfo::exists(dstA),
               "G3A.1 Test A: source unlinked and destination published");
        verify(fileHash(dstA) == expectedHash, "G3A.1 Test A: destination hash matches");
        verify(histA.canUndo(), "G3A.1 Test A: successfully recorded in history");
    }

    // Test B: fsync of directory returns EINVAL — Move preserves source without durability confirmation
    {
        const QString srcB = temporary.filePath(QStringLiteral("g3a1-testb-src.bin"));
        const QString dstB = temporary.filePath(QStringLiteral("g3a1-testb-dst.bin"));
        verify(writeFile(srcB, payload), "G3A.1 Test B: src created");

        simulateFsyncError() = true;
        simulatedFsyncErrno() = EINVAL;

        LocalFileMoveJob moveB(
            QUrl::fromLocalFile(srcB),
            QUrl::fromLocalFile(dstB),
            nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove,
            16 * 1024,
            0);
        moveB.setAutoDelete(false);
        LocalMoveHistory histB;
        histB.recordWhenCompleted(&moveB);
        QSignalSpy spyB(&moveB, &KJob::result);
        moveB.start();
        waitForResult(spyB, "G3A.1 Test B: fsync EINVAL finishes");

        verify(moveB.error() != KJob::NoError && moveB.moveError() == LocalFileMoveError::Durability,
               "G3A.1 Test B: reports Durability error when directory fsync returns EINVAL");
        verify(moveB.destinationPublished(), "G3A.1 Test B: destination was published");
        verify(!moveB.directoryDurabilityConfirmed(), "G3A.1 Test B: directory durability was not confirmed");
        verify(QFileInfo::exists(srcB) && fileHash(srcB) == expectedHash,
               "G3A.1 Test B: source file is 100% PRESERVED");
        verify(QFileInfo::exists(dstB) && fileHash(dstB) == expectedHash,
               "G3A.1 Test B: published destination is intact (no rollback)");
        verify(!histB.canUndo(), "G3A.1 Test B/L: history rejects partial move");
        verify(moveB.property("thispcPartialMoveOutcome").toBool(),
               "G3A.1 Test B/E: thispcPartialMoveOutcome property set");
        verify(moveB.errorText().contains(QStringLiteral("trwałości"))
                   || moveB.errorText().contains(QStringLiteral("durability")),
               "G3A.1 Test B/E: error message informs user of preserved source");

        simulateFsyncError() = false;
        simulatedFsyncErrno() = EIO;

        // Test F: Retry after partial outcome does not overwrite existing destination
        LocalFileMoveJob retryMove(
            QUrl::fromLocalFile(srcB),
            QUrl::fromLocalFile(dstB),
            nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove,
            16 * 1024,
            0);
        retryMove.setAutoDelete(false);
        QSignalSpy retrySpy(&retryMove, &KJob::result);
        retryMove.start();
        waitForResult(retrySpy, "G3A.1 Test F: retry finishes");
        verify(retryMove.error() != KJob::NoError
                   && retryMove.moveError() == LocalFileMoveError::DestinationExists,
               "G3A.1 Test F: retry refuses to overwrite published destination");
        verify(QFileInfo::exists(srcB) && fileHash(srcB) == expectedHash,
               "G3A.1 Test F: source untouched on retry");
        verify(QFileInfo::exists(dstB) && fileHash(dstB) == expectedHash,
               "G3A.1 Test F: destination untouched on retry");
    }

    // Test C: fsync of directory returns ENOTSUP — identical protection
    {
        const QString srcC = temporary.filePath(QStringLiteral("g3a1-testc-src.bin"));
        const QString dstC = temporary.filePath(QStringLiteral("g3a1-testc-dst.bin"));
        verify(writeFile(srcC, payload), "G3A.1 Test C: src created");

        simulateFsyncError() = true;
        simulatedFsyncErrno() = ENOTSUP;

        LocalFileMoveJob moveC(
            QUrl::fromLocalFile(srcC),
            QUrl::fromLocalFile(dstC),
            nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove,
            16 * 1024,
            0);
        moveC.setAutoDelete(false);
        LocalMoveHistory histC;
        histC.recordWhenCompleted(&moveC);
        QSignalSpy spyC(&moveC, &KJob::result);
        moveC.start();
        waitForResult(spyC, "G3A.1 Test C: fsync ENOTSUP finishes");

        verify(moveC.error() != KJob::NoError && moveC.moveError() == LocalFileMoveError::Durability,
               "G3A.1 Test C: reports Durability error when directory fsync returns ENOTSUP");
        verify(moveC.destinationPublished() && !moveC.directoryDurabilityConfirmed(),
               "G3A.1 Test C: destination published but durability unconfirmed");
        verify(QFileInfo::exists(srcC) && fileHash(srcC) == expectedHash,
               "G3A.1 Test C: source file is preserved");
        verify(QFileInfo::exists(dstC) && fileHash(dstC) == expectedHash,
               "G3A.1 Test C: destination file is intact");
        verify(!histC.canUndo(), "G3A.1 Test C/L: history rejects partial move");

        simulateFsyncError() = false;
        simulatedFsyncErrno() = EIO;
    }

    // Test D: fsync of directory returns EIO — source preserved, no destructive rollback of destination
    {
        const QString srcD = temporary.filePath(QStringLiteral("g3a1-testd-src.bin"));
        const QString dstD = temporary.filePath(QStringLiteral("g3a1-testd-dst.bin"));
        verify(writeFile(srcD, payload), "G3A.1 Test D: src created");

        simulateFsyncError() = true;
        simulatedFsyncErrno() = EIO;

        LocalFileMoveJob moveD(
            QUrl::fromLocalFile(srcD),
            QUrl::fromLocalFile(dstD),
            nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove,
            16 * 1024,
            0);
        moveD.setAutoDelete(false);
        QSignalSpy spyD(&moveD, &KJob::result);
        moveD.start();
        waitForResult(spyD, "G3A.1 Test D: fsync EIO finishes");

        verify(moveD.error() != KJob::NoError && moveD.moveError() == LocalFileMoveError::Durability,
               "G3A.1 Test D: reports Durability error on EIO");
        verify(moveD.destinationPublished(), "G3A.1 Test D: destination was published");
        verify(!moveD.directoryDurabilityConfirmed(), "G3A.1 Test D: durability unconfirmed");
        verify(QFileInfo::exists(srcD) && fileHash(srcD) == expectedHash,
               "G3A.1 Test D: source preserved on EIO");
        verify(QFileInfo::exists(dstD) && fileHash(dstD) == expectedHash,
               "G3A.1 Test D: NO destructive rollback of published destination on EIO");

        simulateFsyncError() = false;
        simulatedFsyncErrno() = EIO;
    }

    // Test G: Copy remains fully functional per its contract (completes with NoError even when dir fsync is EINVAL)
    {
        const QString srcCopy = temporary.filePath(QStringLiteral("g3a1-copy-src.bin"));
        const QString dstCopy = temporary.filePath(QStringLiteral("g3a1-copy-dst.bin"));
        verify(writeFile(srcCopy, payload), "G3A.1 Test G: copy src created");

        simulateFsyncError() = true;
        simulatedFsyncErrno() = EINVAL;

        LocalFileCopyJob copyJob(
            QUrl::fromLocalFile(srcCopy),
            QUrl::fromLocalFile(dstCopy),
            nullptr,
            16 * 1024,
            0);
        copyJob.setAutoDelete(false);
        QSignalSpy copySpy(&copyJob, &KJob::result);
        copyJob.start();
        waitForResult(copySpy, "G3A.1 Test G: copy with EINVAL finishes");

        verify(copyJob.error() == KJob::NoError,
               "G3A.1 Test G: Copy completes with NoError on EINVAL directory fsync (tmpfs contract)");
        verify(copyJob.destinationPublished(), "G3A.1 Test G: copy destination published");
        verify(!copyJob.directoryDurabilityConfirmed(), "G3A.1 Test G: copy durability unconfirmed flag");
        verify(QFileInfo::exists(srcCopy) && fileHash(srcCopy) == expectedHash,
               "G3A.1 Test G: source exists intact");
        verify(QFileInfo::exists(dstCopy) && fileHash(dstCopy) == expectedHash,
               "G3A.1 Test G: destination exists with matching SHA-256");

        simulateFsyncError() = false;
        simulatedFsyncErrno() = EIO;
    }

    // Test H: Btrfs/native same-volume atomic Move still functions
    {
        const QString srcH = temporary.filePath(QStringLiteral("g3a1-atom-src.bin"));
        const QString dstH = temporary.filePath(QStringLiteral("g3a1-atom-dst.bin"));
        verify(writeFile(srcH, payload), "G3A.1 Test H: atomic src created");

        LocalFileMoveJob moveAtomic(
            QUrl::fromLocalFile(srcH),
            QUrl::fromLocalFile(dstH),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            0);
        moveAtomic.setAutoDelete(false);
        QSignalSpy atomicSpy(&moveAtomic, &KJob::result);
        moveAtomic.start();
        waitForResult(atomicSpy, "G3A.1 Test H: atomic move finishes");

        verify(moveAtomic.error() == KJob::NoError && moveAtomic.moveError() == LocalFileMoveError::None,
               "G3A.1 Test H: atomic move reports NoError");
        verify(moveAtomic.usedAtomicMove(), "G3A.1 Test H: usedAtomicMove confirmed");
        verify(moveAtomic.destinationPublished() && moveAtomic.directoryDurabilityConfirmed(),
               "G3A.1 Test H: destination published and durable");
        verify(!QFileInfo::exists(srcH) && QFileInfo::exists(dstH),
               "G3A.1 Test H: source unlinked and destination exists");
        verify(fileHash(dstH) == expectedHash, "G3A.1 Test H: hash matches");
    }

    // Test I: NTFS-3G same-volume Move fallback succeeds when directory fsync succeeds
    {
        simulateRenameNoReplaceUnsupported() = true;
        simulateNtfs3gFilesystem() = true;

        const QString srcI = temporary.filePath(QStringLiteral("g3a1-ntfs-src.bin"));
        const QString dstI = temporary.filePath(QStringLiteral("g3a1-ntfs-dst.bin"));
        verify(writeFile(srcI, payload), "G3A.1 Test I: ntfs src created");

        LocalFileMoveJob moveNtfs(
            QUrl::fromLocalFile(srcI),
            QUrl::fromLocalFile(dstI),
            nullptr,
            LocalFileMoveStrategy::Automatic,
            16 * 1024,
            0);
        moveNtfs.setAutoDelete(false);
        QSignalSpy ntfsSpy(&moveNtfs, &KJob::result);
        moveNtfs.start();
        waitForResult(ntfsSpy, "G3A.1 Test I: NTFS-3G move finishes");

        verify(moveNtfs.error() == KJob::NoError && moveNtfs.moveError() == LocalFileMoveError::None,
               "G3A.1 Test I: NTFS-3G move reports NoError");
        verify(!moveNtfs.usedAtomicMove(), "G3A.1 Test I: uses copy-and-remove fallback");
        verify(moveNtfs.destinationPublished() && moveNtfs.directoryDurabilityConfirmed(),
               "G3A.1 Test I: destination published and durable");
        verify(!QFileInfo::exists(srcI) && QFileInfo::exists(dstI),
               "G3A.1 Test I: source unlinked and destination exists");
        verify(fileHash(dstI) == expectedHash, "G3A.1 Test I: hash matches");

        simulateRenameNoReplaceUnsupported() = false;
        simulateNtfs3gFilesystem() = false;
    }

    qInfo("PASS: %d LocalFileMoveJob/history assertions", checks);
    return 0;
}
