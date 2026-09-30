/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localtransferplan.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>


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
    return QCryptographicHash::hash(
        file.readAll(), QCryptographicHash::Sha256);
}

static const LocalTransferPlanEntry *findEntry(
    const LocalTransferPlan &plan, const QString &relativePath)
{
    for (const LocalTransferPlanEntry &entry : plan.entries) {
        if (entry.relativePath == relativePath) return &entry;
    }
    return nullptr;
}

static void waitForResult(QSignalSpy &spy, const char *description)
{
    verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), description);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    verify(temporary.isValid(), "temporary planning directory exists");

    const QString sourceRoot = temporary.filePath(QStringLiteral("source-tree"));
    const QString nested = sourceRoot + QStringLiteral("/nested");
    const QString destination = temporary.filePath(QStringLiteral("destination"));
    verify(QDir().mkpath(nested) && QDir().mkpath(destination),
           "source and destination trees are created");
    const QByteArray rootContents("root-data");
    const QByteArray nestedContents("nested-data-123");
    const QByteArray hiddenContents("hidden");
    const QString rootFile = sourceRoot + QStringLiteral("/root.bin");
    const QString nestedFile = nested + QStringLiteral("/child.bin");
    const QString hiddenFile = sourceRoot + QStringLiteral("/.hidden");
    verify(writeFile(rootFile, rootContents)
               && writeFile(nestedFile, nestedContents)
               && writeFile(hiddenFile, hiddenContents),
           "regular and hidden source files are created");
    const QByteArray originalRootHash = fileHash(rootFile);
    const QByteArray originalNestedHash = fileHash(nestedFile);
    const QString linkPath = sourceRoot + QStringLiteral("/child-link");
    verify(QFile::link(nestedFile, linkPath)
               && QFileInfo(linkPath).isSymLink(),
           "source symbolic link is created");

    const QString plannedRoot = destination + QStringLiteral("/source-tree");
    verify(QDir().mkpath(plannedRoot)
               && writeFile(plannedRoot + QStringLiteral("/root.bin"), "old"),
           "two destination conflicts are prepared");

    bool eventLoopResponsive = false;
    QTimer::singleShot(0, [&eventLoopResponsive] {
        eventLoopResponsive = true;
    });
    LocalTransferPlanJob job(
        {QUrl::fromLocalFile(sourceRoot)},
        QUrl::fromLocalFile(destination), nullptr, 2);
    job.setAutoDelete(false);
    QSignalSpy finished(&job, &KJob::result);
    job.start();
    waitForResult(finished, "directory plan emits one result");
    verify(eventLoopResponsive, "planning leaves the event loop responsive");
    verify(job.error() == KJob::NoError
               && job.planError() == LocalTransferPlanError::None,
           "valid local tree is planned successfully");

    const LocalTransferPlan &plan = job.plan();
    verify(plan.entries.size() == 6
               && plan.directoryCount == 2
               && plan.regularFileCount == 3
               && plan.symbolicLinkCount == 1,
           "plan counts directories, regular files and links exactly");
    verify(plan.totalBytes == static_cast<qulonglong>(
               rootContents.size() + nestedContents.size()
               + hiddenContents.size()),
           "plan calculates the byte total for regular files only");
    verify(plan.conflictCount == 2,
           "plan records existing directory and file conflicts");
    verify(job.plannedEntryCount() == plan.entries.size(),
           "asynchronous entry progress reaches the final count");

    const LocalTransferPlanEntry *rootEntry =
        findEntry(plan, QStringLiteral("source-tree"));
    const LocalTransferPlanEntry *fileEntry =
        findEntry(plan, QStringLiteral("source-tree/root.bin"));
    const LocalTransferPlanEntry *hiddenEntry =
        findEntry(plan, QStringLiteral("source-tree/.hidden"));
    const LocalTransferPlanEntry *linkEntry =
        findEntry(plan, QStringLiteral("source-tree/child-link"));
    verify(rootEntry && rootEntry->type == LocalTransferEntryType::Directory
               && rootEntry->destinationExists
               && rootEntry->destinationIsDirectory,
           "root directory conflict is described in the immutable result");
    verify(fileEntry && fileEntry->type == LocalTransferEntryType::RegularFile
               && fileEntry->size == static_cast<qulonglong>(rootContents.size())
               && fileEntry->destinationExists
               && fileEntry->destinationSize == 3
               && fileEntry->destinationPath
                   == plannedRoot + QStringLiteral("/root.bin"),
           "regular-file path, size and conflict snapshot are correct");
    verify(hiddenEntry && hiddenEntry->size == hiddenContents.size(),
           "hidden files are included in the plan");
    verify(linkEntry && linkEntry->type == LocalTransferEntryType::SymbolicLink
               && linkEntry->symbolicLinkTarget == nestedFile,
           "symbolic link target is captured without traversal");
    verify(fileHash(rootFile) == originalRootHash
               && fileHash(nestedFile) == originalNestedHash
               && QFileInfo(linkPath).isSymLink(),
           "planning leaves all source data untouched");

    LocalTransferPlanJob remoteSource(
        {QUrl(QStringLiteral("sftp://example.invalid/file"))},
        QUrl::fromLocalFile(destination));
    remoteSource.setAutoDelete(false);
    QSignalSpy remoteFinished(&remoteSource, &KJob::result);
    remoteSource.start();
    waitForResult(remoteFinished, "non-local source plan finishes");
    verify(remoteSource.planError() == LocalTransferPlanError::NonLocalUrl,
           "non-local source receives an explicit planning error");

    LocalTransferPlanJob missingSource(
        {QUrl::fromLocalFile(temporary.filePath(QStringLiteral("missing")))},
        QUrl::fromLocalFile(destination));
    missingSource.setAutoDelete(false);
    QSignalSpy missingFinished(&missingSource, &KJob::result);
    missingSource.start();
    waitForResult(missingFinished, "missing source plan finishes");
    verify(missingSource.planError() == LocalTransferPlanError::SourceMissing,
           "missing source receives an explicit planning error");

    LocalTransferPlanJob invalidDestination(
        {QUrl::fromLocalFile(sourceRoot)},
        QUrl::fromLocalFile(temporary.filePath(QStringLiteral("absent"))));
    invalidDestination.setAutoDelete(false);
    QSignalSpy invalidFinished(&invalidDestination, &KJob::result);
    invalidDestination.start();
    waitForResult(invalidFinished, "invalid destination plan finishes");
    verify(invalidDestination.planError()
               == LocalTransferPlanError::DestinationInvalid,
           "invalid destination receives an explicit planning error");

    const QString cancellationRoot =
        temporary.filePath(QStringLiteral("cancellation-tree"));
    verify(QDir().mkpath(cancellationRoot), "cancellation tree is created");
    bool cancellationFilesCreated = true;
    for (int index = 0; index < 60; ++index) {
        cancellationFilesCreated = cancellationFilesCreated
            && writeFile(
                cancellationRoot + QStringLiteral("/item-%1").arg(index),
                QByteArray::number(index));
    }
    verify(cancellationFilesCreated, "cancellation test files are created");
    LocalTransferPlanJob cancelled(
        {QUrl::fromLocalFile(cancellationRoot)},
        QUrl::fromLocalFile(destination), nullptr, 2);
    cancelled.setAutoDelete(false);
    QSignalSpy cancelledFinished(&cancelled, &KJob::result);
    cancelled.start();
    verify(waitUntil([&cancelled] {
               return cancelled.plannedEntryCount() >= 3;
           }, 3000),
           "cancellation plan begins asynchronous traversal");
    verify(cancelled.kill(KJob::EmitResult), "active planning can be cancelled");
    waitForResult(cancelledFinished, "cancelled plan emits one result");
    verify(cancelled.error() == KJob::KilledJobError
               && cancelled.planError() == LocalTransferPlanError::Cancelled,
           "cancelled plan reports the cancellation category");
    verify(!QFileInfo::exists(destination + QStringLiteral("/cancellation-tree")),
           "cancelled planning creates no destination data");

    if (::geteuid() != 0) {
        const QString unreadable = temporary.filePath("unreadable");
        verify(QDir().mkdir(unreadable) && writeFile(unreadable + "/data", "keep")
                   && QFile::setPermissions(unreadable, {}), "unreadable directory fixture is created");
        LocalTransferPlanJob denied({QUrl::fromLocalFile(unreadable)}, QUrl::fromLocalFile(destination));
        denied.setAutoDelete(false);
        QSignalSpy deniedResult(&denied, &KJob::result);
        denied.start();
        waitForResult(deniedResult, "unreadable source planning finishes");
        QFile::setPermissions(unreadable, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        verify(denied.planError() == LocalTransferPlanError::SourceRead,
               "unreadable directory fails rather than appearing empty");
    }
    qInfo("PASS: %d LocalTransferPlan assertions", checks);
    return 0;
}
