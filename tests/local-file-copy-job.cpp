/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localfilecopyjob.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>


static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result();
}

static bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

template<typename Predicate>
static bool waitUntil(Predicate predicate, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(5);
    }
    return predicate();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    verify(directory.isValid(), "temporary transfer directory exists");

    const QString sourcePath = directory.filePath(QStringLiteral("source.bin"));
    const QString destinationPath =
        directory.filePath(QStringLiteral("destination.bin"));
    QFile source(sourcePath);
    verify(source.open(QIODevice::WriteOnly), "source opens for creation");
    QByteArray block(64 * 1024, Qt::Uninitialized);
    for (qsizetype index = 0; index < block.size(); ++index) {
        block[index] = static_cast<char>((index * 31 + 17) & 0xff);
    }
    bool sourceWritten = true;
    for (int index = 0; index < 128; ++index) {
        sourceWritten = sourceWritten
            && source.write(block) == block.size();
    }
    verify(sourceWritten, "all source blocks are written");
    const QFileDevice::Permissions expectedPermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    const QDateTime expectedModified =
        QDateTime::currentDateTimeUtc().addDays(-2);
    const QDateTime expectedAccessed = expectedModified.addSecs(-3600);
    verify(source.flush()
               && source.setPermissions(expectedPermissions)
               && source.setFileTime(
                   expectedAccessed,
                   QFileDevice::FileAccessTime)
               && source.setFileTime(
                   expectedModified,
                   QFileDevice::FileModificationTime),
           "source metadata is prepared");
    source.close();

    LocalFileCopyJob copy(
        QUrl::fromLocalFile(sourcePath),
        QUrl::fromLocalFile(destinationPath),
        nullptr,
        16 * 1024,
        1);
    copy.setAutoDelete(false);
    QSignalSpy copyFinished(&copy, &KJob::result);
    copy.start();
    verify(waitUntil(
               [&copy] {
                   return copy.processedAmount(KJob::Bytes) >= 256 * 1024;
               },
               3000),
           "copy reports initial chunk progress");
    verify(copy.suspend() && copy.isSuspended(),
           "copy acknowledges a real pause");
    const qulonglong pausedBytes = copy.processedAmount(KJob::Bytes);
    const qint64 pausedFileSize = QFileInfo(copy.partialPath()).size();
    QTest::qWait(150);
    verify(copy.processedAmount(KJob::Bytes) == pausedBytes
               && QFileInfo(copy.partialPath()).size() == pausedFileSize,
           "processed bytes and partial file stay fixed while paused");
    verify(copy.resume() && !copy.isSuspended(), "copy resumes after pause");
    verify(waitUntil([&copyFinished] { return copyFinished.count() == 1; }, 10000),
           "resumed copy emits one completion result");
    const QFileInfo copiedInfo(destinationPath);
    const QFileDevice::Permissions publicPermissions =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther
        | QFileDevice::WriteOther | QFileDevice::ExeOther;
    verify((copiedInfo.permissions() & expectedPermissions)
                   == expectedPermissions
               && !(copiedInfo.permissions() & publicPermissions)
               && qAbs(copiedInfo.lastModified().toUTC().secsTo(expectedModified))
                   <= 1
               && qAbs(copiedInfo.lastRead().toUTC().secsTo(expectedAccessed))
                   <= 1,
           "copy preserves permissions and access/modification times");
    verify(copy.error() == KJob::NoError
               && QFileInfo(destinationPath).size() == QFileInfo(sourcePath).size()
               && fileHash(destinationPath) == fileHash(sourcePath),
           "resumed copy completes with byte-exact content");
    verify(!QFileInfo::exists(copy.partialPath()),
           "successful copy publishes without a partial file");

    const QString cancelledPath =
        directory.filePath(QStringLiteral("cancelled.bin"));
    LocalFileCopyJob cancelled(
        QUrl::fromLocalFile(sourcePath),
        QUrl::fromLocalFile(cancelledPath),
        nullptr,
        16 * 1024,
        1);
    cancelled.setAutoDelete(false);
    QSignalSpy cancelledFinished(&cancelled, &KJob::result);
    cancelled.start();
    verify(waitUntil(
               [&cancelled] {
                   return cancelled.processedAmount(KJob::Bytes) >= 128 * 1024;
               },
               3000),
           "cancel test reaches active copy state");
    verify(cancelled.kill(KJob::EmitResult), "active local copy can be cancelled");
    verify(waitUntil(
               [&cancelledFinished] { return cancelledFinished.count() == 1; },
               3000),
           "cancelled copy emits one result");
    verify(waitUntil(
               [&cancelled] {
                   return !QFileInfo::exists(cancelled.partialPath());
               },
               3000),
           "cancelled copy removes its partial file");
    verify(!QFileInfo::exists(cancelledPath),
           "cancelled copy leaves no destination file");

    const QString existingPath =
        directory.filePath(QStringLiteral("existing.bin"));
    QFile existing(existingPath);
    verify(existing.open(QIODevice::WriteOnly)
               && existing.write("keep") == 4,
           "existing destination sentinel is created");
    existing.close();
    LocalFileCopyJob existingJob(
        QUrl::fromLocalFile(sourcePath),
        QUrl::fromLocalFile(existingPath));
    existingJob.setAutoDelete(false);
    QSignalSpy existingFinished(&existingJob, &KJob::result);
    existingJob.start();
    verify(waitUntil(
               [&existingFinished] { return existingFinished.count() == 1; },
               3000)
               && existingJob.transferError()
                   == LocalTransferError::DestinationExists,
           "existing destination has an explicit error category");
    verify(existing.open(QIODevice::ReadOnly) && existing.readAll() == "keep",
           "existing destination remains untouched");
    existing.close();

    const QString partialDestination =
        directory.filePath(QStringLiteral("partial-target.bin"));
    const QString partialPath =
        partialDestination + QStringLiteral(".thispc-part");
    QFile partial(partialPath);
    verify(partial.open(QIODevice::WriteOnly)
               && partial.write("recover-me") == 10,
           "pre-existing partial sentinel is created");
    partial.close();
    LocalFileCopyJob partialJob(
        QUrl::fromLocalFile(sourcePath),
        QUrl::fromLocalFile(partialDestination));
    partialJob.setAutoDelete(false);
    QSignalSpy partialFinished(&partialJob, &KJob::result);
    partialJob.start();
    verify(waitUntil(
               [&partialFinished] { return partialFinished.count() == 1; },
               3000)
               && partialJob.transferError() == LocalTransferError::PartialExists,
           "pre-existing partial file is rejected explicitly");
    verify(partial.open(QIODevice::ReadOnly)
               && partial.readAll() == "recover-me"
               && !QFileInfo::exists(partialDestination),
           "pre-existing partial file remains untouched");
    partial.close();

    LocalFileCopyJob missingSource(
        QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.bin"))),
        QUrl::fromLocalFile(directory.filePath(QStringLiteral("unused.bin"))));
    missingSource.setAutoDelete(false);
    QSignalSpy missingFinished(&missingSource, &KJob::result);
    missingSource.start();
    verify(waitUntil(
               [&missingFinished] { return missingFinished.count() == 1; },
               3000)
               && missingSource.transferError() == LocalTransferError::SourceOpen,
           "missing source has an explicit error category");

    LocalFileCopyJob invalidDestination(
        QUrl::fromLocalFile(sourcePath),
        QUrl::fromLocalFile(
            directory.filePath(QStringLiteral("missing-dir/target.bin"))));
    invalidDestination.setAutoDelete(false);
    QSignalSpy invalidFinished(&invalidDestination, &KJob::result);
    invalidDestination.start();
    verify(waitUntil(
               [&invalidFinished] { return invalidFinished.count() == 1; },
               3000)
               && invalidDestination.transferError()
                   == LocalTransferError::DestinationCreate,
           "destination creation failure has an explicit category");

    verify(LocalFileCopyWorker::classifyWriteError(ENOSPC)
                   == LocalTransferError::DiskFull
               && LocalFileCopyWorker::classifyWriteError(EDQUOT)
                   == LocalTransferError::DiskFull
               && LocalFileCopyWorker::classifyWriteError(EIO)
                   == LocalTransferError::DestinationWrite,
           "disk-full and generic write errors are classified separately");

    const auto readFile = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    for (bool cancelOverwrite : {true, false}) {
        LocalFileCopyJob overwrite(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(existingPath),
            nullptr, 16 * 1024, 1, true);
        overwrite.setAutoDelete(false);
        QSignalSpy result(&overwrite, &KJob::result);
        overwrite.start();
        verify(waitUntil([&] { return overwrite.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000),
               "overwrite reaches native chunk progress");
        verify(overwrite.suspend(), "overwrite acknowledges pause");
        const auto stopped = overwrite.processedAmount(KJob::Bytes);
        const auto stoppedSize = QFileInfo(overwrite.partialPath()).size();
        QTest::qWait(120);
        verify(overwrite.processedAmount(KJob::Bytes) == stopped
                   && QFileInfo(overwrite.partialPath()).size() == stoppedSize
                   && readFile(existingPath) == "keep",
               "paused overwrite freezes its partial and retains the old destination");
        verify(cancelOverwrite ? overwrite.kill(KJob::EmitResult) : overwrite.resume(),
               "paused overwrite accepts cancel or resume");
        verify(waitUntil([&] { return result.count() == 1; }, 10000)
                   && waitUntil([&] { return !QFileInfo::exists(overwrite.partialPath()); }, 3000),
               "overwrite emits one result and removes its partial");
        verify(cancelOverwrite
                   ? overwrite.error() == KJob::KilledJobError && readFile(existingPath) == "keep"
                   : overwrite.error() == 0 && fileHash(existingPath) == fileHash(sourcePath),
               "overwrite keeps old bytes on cancel and publishes exact bytes on success");
    }
    LocalFileCopyJob sameFile(QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(sourcePath),
                              nullptr, 4096, 0, true);
    sameFile.setAutoDelete(false);
    QSignalSpy sameResult(&sameFile, &KJob::result);
    sameFile.start();
    verify(waitUntil([&] { return sameResult.count() == 1; }, 3000)
               && sameFile.transferError() == LocalTransferError::DestinationChanged,
           "overwrite refuses to replace the source with itself");

    LocalFileCopyJob changedTarget(QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(existingPath),
                                  nullptr, 4096, 0, true);
    changedTarget.setAutoDelete(false);
    QFile changed(existingPath);
    verify(changed.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && changed.write("new external content") == 20, "destination changes before overwrite starts");
    changed.close();
    QSignalSpy changedResult(&changedTarget, &KJob::result);
    changedTarget.start();
    verify(waitUntil([&] { return changedResult.count() == 1; }, 3000)
               && changedTarget.transferError() == LocalTransferError::DestinationChanged
               && readFile(existingPath) == "new external content",
           "overwrite does not discard a changed destination");

    LocalFileCopyJob changedWhilePaused(
        QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(existingPath),
        nullptr, 16 * 1024, 1, true);
    changedWhilePaused.setAutoDelete(false);
    QSignalSpy lateResult(&changedWhilePaused, &KJob::result);
    changedWhilePaused.start();
    verify(waitUntil([&] { return changedWhilePaused.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
               && changedWhilePaused.suspend(), "destination-change test pauses active overwrite");
    verify(changed.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && changed.write("changed while paused") == 20,
           "external writer changes the destination while overwrite is paused");
    changed.close();
    verify(changedWhilePaused.resume()
               && waitUntil([&] { return lateResult.count() == 1; }, 10000)
               && changedWhilePaused.transferError() == LocalTransferError::Publish
               && readFile(existingPath) == "changed while paused"
               && !QFileInfo::exists(changedWhilePaused.partialPath()),
           "publication refuses a destination changed during transfer and cleans the partial");

    // SEC-045-02: Atomic no-overwrite publication prevents TOCTOU overwrite when destination
    // appears while a non-overwrite copy is in flight.
    {
        const QByteArray expectedSourceHash = fileHash(sourcePath);
        const QString raceDstPath = directory.filePath(QStringLiteral("sec04502-race-dst.bin"));
        LocalFileCopyJob raceCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(raceDstPath),
            nullptr, 16 * 1024, 1, false);
        raceCopy.setAutoDelete(false);
        QSignalSpy raceResult(&raceCopy, &KJob::result);
        raceCopy.start();
        verify(waitUntil([&] { return raceCopy.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000)
                   && raceCopy.suspend(),
               "SEC-045-02: non-overwrite copy pauses in flight before publication");

        const QByteArray concurrentBytes("concurrent-target-must-never-be-overwritten-04502");
        QFile raceTarget(raceDstPath);
        verify(raceTarget.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                   && raceTarget.write(concurrentBytes) == concurrentBytes.size(),
               "SEC-045-02: concurrent destination created while copy is in flight");
        raceTarget.close();
        const QByteArray expectedRaceTargetHash = fileHash(raceDstPath);

        verify(raceCopy.resume()
                   && waitUntil([&] { return raceResult.count() == 1; }, 10000),
               "SEC-045-02: resumed non-overwrite copy finishes with error");
        verify(raceCopy.transferError() == LocalTransferError::DestinationExists
                   && raceCopy.systemError() == EEXIST,
               "SEC-045-02: atomic RENAME_NOREPLACE reports DestinationExists (EEXIST)");
        verify(fileHash(raceDstPath) == expectedRaceTargetHash
                   && readFile(raceDstPath) == concurrentBytes,
               "SEC-045-02: existing destination file SHA-256 and contents are 100% intact");
        verify(!QFileInfo::exists(raceCopy.partialPath()),
               "SEC-045-02: partial .thispc-part file is removed after collision");
        verify(fileHash(sourcePath) == expectedSourceHash,
               "SEC-045-02: source file SHA-256 remains intact");
    }

    // SEC-045-02.1 / SEC-045-02.2: Safe publication when RENAME_NOREPLACE is unsupported (NTFS-3G).
    // When RENAME_NOREPLACE is unsupported, LocalFileCopyJob successfully falls back to descriptor-based
    // linkat (AT_EMPTY_PATH / /proc/self/fd) without failing closed if the filesystem supports hard links.
    {
        const QByteArray expectedSourceHash = fileHash(sourcePath);
        const QString fallbackDstPath = directory.filePath(QStringLiteral("sec045022-fallback-dst.bin"));
        simulateRenameNoReplaceUnsupported() = true;

        LocalFileCopyJob fallbackCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(fallbackDstPath),
            nullptr, 16 * 1024, 1, false);
        fallbackCopy.setAutoDelete(false);
        QSignalSpy fallbackResult(&fallbackCopy, &KJob::result);
        fallbackCopy.start();
        verify(waitUntil([&] { return fallbackResult.count() == 1; }, 10000),
               "SEC-045-02.2: copy finishes when RENAME_NOREPLACE is unsupported and linkat succeeds");
        verify(fallbackCopy.error() == KJob::NoError
                   && fallbackCopy.transferError() == LocalTransferError::None,
               "SEC-045-02.2: descriptor linkat fallback succeeds seamlessly");
        verify(QFileInfo::exists(fallbackDstPath),
               "SEC-045-02.2: destination file was created via descriptor linkat");
        verify(fileHash(fallbackDstPath) == expectedSourceHash,
               "SEC-045-02.2: destination file SHA-256 matches source 100%");
        verify(!QFileInfo::exists(fallbackCopy.partialPath()),
               "SEC-045-02.2: staging .thispc-part file is cleanly unlinked after publication");

        // Now test collision with fallback active:
        LocalFileCopyJob collisionCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(fallbackDstPath),
            nullptr, 16 * 1024, 1, false);
        collisionCopy.setAutoDelete(false);
        QSignalSpy collisionResult(&collisionCopy, &KJob::result);
        collisionCopy.start();
        verify(waitUntil([&] { return collisionResult.count() == 1; }, 10000),
               "SEC-045-02.2: collision test finishes with descriptor fallback");
        verify(collisionCopy.transferError() == LocalTransferError::DestinationExists
                   && collisionCopy.systemError() == EEXIST,
               "SEC-045-02.2: descriptor linkat rejects collision with EEXIST");
        verify(fileHash(fallbackDstPath) == expectedSourceHash,
               "SEC-045-02.2: existing destination file remains 100% untouched after collision");

        // Now test fail-closed when BOTH RENAME_NOREPLACE and descriptor linkat are unsupported (e.g. FAT32).
        simulateDescriptorLinkatUnsupported() = true;
        const QString allUnsupportedDst = directory.filePath(QStringLiteral("sec045022-all-unsupported-dst.bin"));
        LocalFileCopyJob allUnsupportedCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(allUnsupportedDst),
            nullptr, 16 * 1024, 1, false);
        allUnsupportedCopy.setAutoDelete(false);
        QSignalSpy allUnsupportedResult(&allUnsupportedCopy, &KJob::result);
        allUnsupportedCopy.start();
        verify(waitUntil([&] { return allUnsupportedResult.count() == 1; }, 10000),
               "SEC-045-02.2: all unsupported finishes");
        verify(allUnsupportedCopy.transferError() == LocalTransferError::Publish
                   && allUnsupportedCopy.systemError() == EOPNOTSUPP,
               "SEC-045-02.2: reports Publish error with EOPNOTSUPP when both methods unsupported");
        verify(allUnsupportedCopy.errorText().contains(QStringLiteral("nie obsługuje"))
                   || allUnsupportedCopy.errorText().contains(QStringLiteral("does not support")),
               "SEC-045-02.2: provides clear descriptive error message");
        verify(!QFileInfo::exists(allUnsupportedDst),
               "SEC-045-02.2: destination file was never created");
        verify(!QFileInfo::exists(allUnsupportedCopy.partialPath()),
               "SEC-045-02.2: staging file is removed");

        simulateDescriptorLinkatUnsupported() = false;
        simulateRenameNoReplaceUnsupported() = false;
    }

    // SEC-045-02.1: Concurrent destination substitution and foreign file integrity verification.
    {
        const QByteArray expectedSourceHash = fileHash(sourcePath);
        const QString foreignDstPath = directory.filePath(QStringLiteral("sec045021-foreign-dst.bin"));
        const QByteArray sensitiveBytes("FOREIGN-SENSITIVE-COPY-DEST-045021-DO-NOT-DELETE");
        verify(writeFile(foreignDstPath, sensitiveBytes), "SEC-045-02.1: sensitive foreign destination created");
        const QByteArray foreignHashBefore = fileHash(foreignDstPath);

        LocalFileCopyJob foreignCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(foreignDstPath),
            nullptr, 16 * 1024, 1, false);
        foreignCopy.setAutoDelete(false);
        QSignalSpy foreignResult(&foreignCopy, &KJob::result);
        foreignCopy.start();
        verify(waitUntil([&] { return foreignResult.count() == 1; }, 10000),
               "SEC-045-02.1: copy to existing foreign destination finishes");
        verify(foreignCopy.transferError() == LocalTransferError::DestinationExists
                   && foreignCopy.systemError() == EEXIST,
               "SEC-045-02.1: copy reports DestinationExists (EEXIST) without touching foreign file");
        verify(fileHash(foreignDstPath) == foreignHashBefore
                   && readFile(foreignDstPath) == sensitiveBytes,
               "SEC-045-02.1: foreign destination file content and SHA-256 are 100% untouched");
        verify(!QFileInfo::exists(foreignCopy.partialPath()),
               "SEC-045-02.1: staging .thispc-part file cleaned up after collision");
        verify(fileHash(sourcePath) == expectedSourceHash,
               "SEC-045-02.1: source file SHA-256 remains intact");
    }

    // SEC-045-E1: Injected staging substitution test before publication and during cleanup
    {
        const QString raceDstPath = directory.filePath(QStringLiteral("sec045e1-staging-subst.bin"));
        const QString racePartPath = raceDstPath + QStringLiteral(".thispc-part");

        // Test direct publishOwnedStagingNoOverwrite with an alien file injected under staging path
        // while holding a real stagingFd
        const QString realStagingPath = directory.filePath(QStringLiteral("sec045e1-real.tmp"));
        const QByteArray realData("REAL_STAGING_DATA_VERIFIED_IDENTITY");
        verify(writeFile(realStagingPath, realData), "SEC-045-E1: real staging written");
        int realFd = ::open(QFile::encodeName(realStagingPath).constData(), O_RDWR | O_CLOEXEC);
        verify(realFd >= 0, "SEC-045-E1: realFd opened");
        LocalFileIdentity realIdentity = LocalFileIdentity::read(realStagingPath);
        verify(realIdentity.valid, "SEC-045-E1: realIdentity is valid");

        // Alien file at racePartPath
        const QByteArray alienData("ALIEN_ATTACKER_DATA_DO_NOT_PUBLISH_OR_UNLINK");
        verify(writeFile(racePartPath, alienData), "SEC-045-E1: alien file written under staging path");
        const QByteArray alienHashBefore = fileHash(racePartPath);

        // Attempting to publish with realFd but passing racePartPath where alien file resides
        // publishOwnedStagingNoOverwrite compares realFd inode against expectedStagingIdentity
        bool collision = false;
        int pubErr = 0;
        bool unlinkFailed = false;
        // If someone substituted racePartPath before renameat2, renameat2 would attempt to rename alien file.
        // But our post-publish check or linkat fallback verifies inode integrity!
        simulateRenameNoReplaceUnsupported() = true; // force linkat from realFd
        bool pubOk = publishOwnedStagingNoOverwrite(
            realFd, racePartPath, raceDstPath, realIdentity, &collision, &pubErr, &unlinkFailed);
        verify(pubOk, "SEC-045-E1: descriptor linkat published realFd directly to raceDstPath");
        verify(fileHash(raceDstPath) == fileHash(realStagingPath),
               "SEC-045-E1: published destination matches real staging data, NOT alien data");
        // Verify alien file under racePartPath was REFUSED deletion because its inode did not match realFd!
        verify(unlinkFailed, "SEC-045-E1: unlinking alien file was refused and flagged");
        verify(QFileInfo::exists(racePartPath) && fileHash(racePartPath) == alienHashBefore,
               "SEC-045-E1: alien file at staging path was 100% PRESERVED, never unlinked!");

        ::close(realFd);
        QFile::remove(realStagingPath);
        QFile::remove(racePartPath);
        QFile::remove(raceDstPath);
        simulateRenameNoReplaceUnsupported() = false;
    }

    // SEC-045-E2: Deterministic test: unsupported descriptor linkat fails closed safely
    {
        const QString raceDstPath = directory.filePath(QStringLiteral("sec045e2-native-subst.bin"));
        const QString racePartPath = raceDstPath + QStringLiteral(".thispc-part");

        const QString realStagingPath = directory.filePath(QStringLiteral("sec045e2-real.tmp"));
        const QByteArray realData("REAL_STAGING_DATA_DESCRIPTOR_TEST");
        verify(writeFile(realStagingPath, realData), "SEC-045-E2: real staging written");
        int realFd = ::open(QFile::encodeName(realStagingPath).constData(), O_RDWR | O_CLOEXEC);
        verify(realFd >= 0, "SEC-045-E2: realFd opened");
        LocalFileIdentity realIdentity = LocalFileIdentity::read(realStagingPath);
        verify(realIdentity.valid, "SEC-045-E2: realIdentity valid");

        // Alien file injected at racePartPath
        const QByteArray alienData("ALIEN_INJECTED_UNDER_STAGING_NAME");
        verify(writeFile(racePartPath, alienData), "SEC-045-E2: alien file written");

        // Force descriptor linkat to be skipped
        simulateDescriptorLinkatUnsupported() = true;

        bool collision = false;
        int pubErr = 0;
        bool unlinkFailed = false;
        // publishOwnedStagingNoOverwrite must fail closed with EOPNOTSUPP, never attempting path-based rename
        bool pubOk = publishOwnedStagingNoOverwrite(
            realFd, racePartPath, raceDstPath, realIdentity, &collision, &pubErr, &unlinkFailed);
        verify(!pubOk && pubErr == EOPNOTSUPP, "SEC-045-E2: publication FAILS CLOSED when descriptor linkat unsupported");
        verify(!QFileInfo::exists(raceDstPath), "SEC-045-E2: destination was NEVER created");
        verify(QFileInfo::exists(racePartPath), "SEC-045-E2: alien file was NEVER touched");

        simulateDescriptorLinkatUnsupported() = false;
        ::close(realFd);
        QFile::remove(realStagingPath);
        QFile::remove(racePartPath);
    }

    // SEC-045-F: Comprehensive VFAT Atomic Publication & Security Verification
    {
        // F.A & F.J: Successful publication on simulated VFAT using RENAME_NOREPLACE with SHA-256 integrity
        simulateDescriptorLinkatUnsupported() = true;
        simulateVfatFilesystem() = true;

        const QString vfatDstPath = directory.filePath(QStringLiteral("sec045f-vfat-ok.bin"));
        LocalFileCopyJob vfatCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(vfatDstPath),
            nullptr, 16 * 1024, 1, false);
        vfatCopy.setAutoDelete(false);
        QSignalSpy vfatResult(&vfatCopy, &KJob::result);
        vfatCopy.start();
        verify(waitUntil([&] { return vfatResult.count() == 1; }, 10000),
               "SEC-045-F.A: VFAT copy finishes");
        verify(vfatCopy.error() == 0,
               "SEC-045-F.A: VFAT copy succeeds without error");
        verify(QFileInfo::exists(vfatDstPath),
               "SEC-045-F.A: VFAT destination file exists");
        verify(fileHash(vfatDstPath) == fileHash(sourcePath),
               "SEC-045-F.J: VFAT published destination SHA-256 exactly matches source");
        verify(!QFileInfo::exists(vfatCopy.partialPath()),
               "SEC-045-F.A: VFAT staging file is gone after atomic rename");

        // F.B: EEXIST Collision protection on VFAT
        LocalFileCopyJob vfatCollisionCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(vfatDstPath),
            nullptr, 16 * 1024, 1, false);
        vfatCollisionCopy.setAutoDelete(false);
        QSignalSpy vfatCollisionResult(&vfatCollisionCopy, &KJob::result);
        const QByteArray dstHashBefore = fileHash(vfatDstPath);
        vfatCollisionCopy.start();
        verify(waitUntil([&] { return vfatCollisionResult.count() == 1; }, 10000),
               "SEC-045-F.B: VFAT collision copy finishes");
        verify(vfatCollisionCopy.transferError() == LocalTransferError::DestinationExists
                   && vfatCollisionCopy.systemError() == EEXIST,
               "SEC-045-F.B: VFAT collision reports DestinationExists (EEXIST)");
        verify(fileHash(vfatDstPath) == dstHashBefore,
               "SEC-045-F.B: existing VFAT destination file is 100% UNTOUCHED");
        verify(!QFileInfo::exists(vfatCollisionCopy.partialPath()),
               "SEC-045-F.B: VFAT staging file is cleaned up after collision");

        // F.D: Non-VFAT filesystem without hardlinks fails closed with EOPNOTSUPP
        simulateVfatFilesystem() = false;
        const QString nonVfatDst = directory.filePath(QStringLiteral("sec045f-nonvfat.bin"));
        LocalFileCopyJob nonVfatCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(nonVfatDst),
            nullptr, 16 * 1024, 1, false);
        nonVfatCopy.setAutoDelete(false);
        QSignalSpy nonVfatResult(&nonVfatCopy, &KJob::result);
        nonVfatCopy.start();
        verify(waitUntil([&] { return nonVfatResult.count() == 1; }, 10000),
               "SEC-045-F.D: non-VFAT copy finishes");
        verify(nonVfatCopy.transferError() == LocalTransferError::Publish
                   && nonVfatCopy.systemError() == EOPNOTSUPP,
               "SEC-045-F.D: non-VFAT without linkat FAILS CLOSED with EOPNOTSUPP");
        verify(!QFileInfo::exists(nonVfatDst),
               "SEC-045-F.D: destination file never created on unsupported filesystem");

        // F.E: VFAT without RENAME_NOREPLACE fails closed with EOPNOTSUPP
        simulateVfatFilesystem() = true;
        simulateRenameNoReplaceUnsupported() = true;
        const QString noRenameDst = directory.filePath(QStringLiteral("sec045f-norename.bin"));
        LocalFileCopyJob noRenameCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(noRenameDst),
            nullptr, 16 * 1024, 1, false);
        noRenameCopy.setAutoDelete(false);
        QSignalSpy noRenameResult(&noRenameCopy, &KJob::result);
        noRenameCopy.start();
        verify(waitUntil([&] { return noRenameResult.count() == 1; }, 10000),
               "SEC-045-F.E: no RENAME_NOREPLACE finishes");
        verify(noRenameCopy.transferError() == LocalTransferError::Publish
                   && noRenameCopy.systemError() == EOPNOTSUPP,
               "SEC-045-F.E: VFAT without RENAME_NOREPLACE FAILS CLOSED with EOPNOTSUPP");
        verify(!QFileInfo::exists(noRenameDst),
               "SEC-045-F.E: destination file never created");
        simulateRenameNoReplaceUnsupported() = false;

        // F.F: Injected staging substitution test before VFAT publication
        const QString vfatSubstDst = directory.filePath(QStringLiteral("sec045f-subst.bin"));
        const QString vfatSubstPart = vfatSubstDst + QStringLiteral(".thispc-part");
        const QString vfatRealTmp = directory.filePath(QStringLiteral("sec045f-real.tmp"));
        verify(writeFile(vfatRealTmp, QByteArray("REAL_VFAT_PAYLOAD")), "SEC-045-F.F: real tmp written");
        int realVfatFd = ::open(QFile::encodeName(vfatRealTmp).constData(), O_RDWR | O_CLOEXEC);
        verify(realVfatFd >= 0, "SEC-045-F.F: real fd opened");
        LocalFileIdentity realVfatId = LocalFileIdentity::read(vfatRealTmp);

        // Alien file at staging path
        const QByteArray alienVfatData("ALIEN_VFAT_TAMPERED_STAGING");
        verify(writeFile(vfatSubstPart, alienVfatData), "SEC-045-F.F: alien file written under staging");

        bool vfatCollision = false;
        int vfatPubErr = 0;
        bool vfatUnlinkFailed = false;
        bool vfatPubOk = publishOwnedStagingNoOverwrite(
            realVfatFd, vfatSubstPart, vfatSubstDst, realVfatId,
            &vfatCollision, &vfatPubErr, &vfatUnlinkFailed);
        verify(!vfatPubOk && vfatPubErr == EBADF,
               "SEC-045-F.F: publication REFUSED when staging inode/size was substituted");
        verify(!QFileInfo::exists(vfatSubstDst),
               "SEC-045-F.F: destination was NEVER created from substituted staging");
        verify(readFile(vfatSubstPart) == alienVfatData,
               "SEC-045-F.F: alien file at staging path was NEVER unlinked or touched");
        ::close(realVfatFd);
        QFile::remove(vfatRealTmp);
        QFile::remove(vfatSubstPart);

        // F.H: Cancel before publication on VFAT
        const QString vfatCancelDst = directory.filePath(QStringLiteral("sec045f-cancel.bin"));
        LocalFileCopyJob vfatCancelCopy(
            QUrl::fromLocalFile(sourcePath), QUrl::fromLocalFile(vfatCancelDst),
            nullptr, 16 * 1024, 1, false);
        vfatCancelCopy.setAutoDelete(false);
        QSignalSpy vfatCancelSpy(&vfatCancelCopy, &KJob::result);
        vfatCancelCopy.start();
        verify(vfatCancelCopy.kill(KJob::EmitResult), "SEC-045-F.H: active VFAT copy can be cancelled");
        verify(waitUntil([&] { return vfatCancelSpy.count() == 1; }, 10000),
               "SEC-045-F.H: cancelled VFAT copy finishes");
        verify(!QFileInfo::exists(vfatCancelDst),
               "SEC-045-F.H: cancelled destination file was never created");
        verify(!QFileInfo::exists(vfatCancelCopy.partialPath()),
               "SEC-045-F.H: cancelled staging file was safely cleaned up");

        // F.I: Fsync directory error handling on VFAT
        const QString vfatFsyncDst = directory.filePath(QStringLiteral("sec045f-fsync.bin"));
        const QString vfatFsyncTmp = directory.filePath(QStringLiteral("sec045f-fsync.tmp"));
        verify(writeFile(vfatFsyncTmp, QByteArray("FSYNC_TEST_PAYLOAD")), "SEC-045-F.I: tmp file written");
        int fsyncTestFd = ::open(QFile::encodeName(vfatFsyncTmp).constData(), O_RDWR | O_CLOEXEC);
        verify(fsyncTestFd >= 0, "SEC-045-F.I: fd opened");
        LocalFileIdentity fsyncId = LocalFileIdentity::read(vfatFsyncTmp);

        simulateFsyncError() = true;
        bool fsyncCollision = false;
        int fsyncPubErr = 0;
        bool fsyncUnlinkFailed = false;
        bool fsyncPubOk = publishOwnedStagingNoOverwrite(
            fsyncTestFd, vfatFsyncTmp, vfatFsyncDst, fsyncId,
            &fsyncCollision, &fsyncPubErr, &fsyncUnlinkFailed);
        verify(!fsyncPubOk && fsyncPubErr == EIO,
               "SEC-045-F.I: publish reports EIO when fsync fails");
        simulateFsyncError() = false;
        ::close(fsyncTestFd);
        QFile::remove(vfatFsyncTmp);
        QFile::remove(vfatFsyncDst);

        // Reset simulation flags
        simulateVfatFilesystem() = false;
        simulateDescriptorLinkatUnsupported() = false;
    }

    qInfo("PASS: %d LocalFileCopyJob assertions", checks);
    return 0;
}
