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

    qInfo("PASS: %d LocalFileCopyJob assertions", checks);
    return 0;
}
