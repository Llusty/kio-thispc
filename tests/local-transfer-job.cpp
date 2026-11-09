/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localtransferjob.h"
#include <QCryptographicHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

static QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static void waitResult(QSignalSpy &spy)
{
    verify(QTest::qWaitFor([&] { return spy.count() == 1; }, 15000), "tree transfer emits one result");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    verify(root.isValid(), "disposable tree directory exists");
    const QString source = root.filePath("source");
    const QString destination = root.filePath("destination");
    verify(QDir().mkpath(source + "/nested/empty") && QDir().mkpath(destination),
           "source and destination directories are created");
    const QByteArray payload(2 * 1024 * 1024, 'x');
    verify(writeFile(source + "/first.bin", payload)
               && writeFile(source + "/nested/.hidden", "hidden")
               && writeFile(root.filePath("second.txt"), "second"), "multiple-source fixtures are created");
    const QList<QUrl> sources{QUrl::fromLocalFile(source), QUrl::fromLocalFile(root.filePath("second.txt"))};
    LocalTransferJob copy(sources, QUrl::fromLocalFile(destination), false, nullptr, 4096, 1);
    copy.setAutoDelete(false);
    QSignalSpy result(&copy, &KJob::result);
    QSignalSpy current(&copy, &LocalTransferJob::currentFileChanged);
    copy.start();
    verify(QTest::qWaitFor([&] { return copy.processedAmount(KJob::Bytes) >= 64 * 1024; }, 5000)
               && copy.suspend(), "tree copy pauses during file data transfer");
    const auto stopped = copy.processedAmount(KJob::Bytes);
    const auto partial = destination + "/source/first.bin.thispc-part";
    const auto stoppedSize = QFileInfo(partial).size();
    const QString parallelDestination = root.filePath("parallel");
    verify(QDir().mkpath(parallelDestination), "parallel destination is created");
    LocalTransferJob parallel(sources, QUrl::fromLocalFile(parallelDestination), false);
    parallel.setAutoDelete(false);
    QSignalSpy parallelResult(&parallel, &KJob::result);
    parallel.start();
    waitResult(parallelResult);
    verify(parallel.error() == 0 && copy.isSuspended(),
           "a second tree transfer completes independently while the first is paused");
    QTest::qWait(120);
    verify(copy.processedAmount(KJob::Bytes) == stopped && QFileInfo(partial).size() == stoppedSize,
           "tree pause freezes aggregate bytes and the active partial");
    verify(copy.resume(), "tree copy resumes");
    waitResult(result);
    verify(copy.error() == 0 && copy.totalAmount(KJob::Bytes) == static_cast<qulonglong>(payload.size() + 12)
               && copy.processedAmount(KJob::Bytes) == copy.totalAmount(KJob::Bytes),
           "tree copy reports aggregate progress for all sources");
    verify(readFile(destination + "/source/first.bin") == payload
               && readFile(destination + "/source/nested/.hidden") == "hidden"
               && readFile(destination + "/second.txt") == "second"
               && QDir(destination + "/source/nested/empty").exists()
               && readFile(source + "/first.bin") == payload && current.count() == 3,
           "tree copy preserves hidden files, empty directories, source data and current-file updates");

    const QString cancelDestination = root.filePath("cancel");
    verify(QDir().mkpath(cancelDestination), "cancellation destination exists");
    LocalTransferJob cancelled(sources, QUrl::fromLocalFile(cancelDestination), false, nullptr, 4096, 1);
    cancelled.setAutoDelete(false);
    QSignalSpy cancelledResult(&cancelled, &KJob::result);
    cancelled.start();
    verify(QTest::qWaitFor([&] { return cancelled.processedAmount(KJob::Bytes) >= 64 * 1024; }, 5000)
               && cancelled.suspend() && cancelled.kill(KJob::EmitResult),
           "active tree transfer can be cancelled while paused");
    waitResult(cancelledResult);
    verify(QTest::qWaitFor([&] {
               return !QFileInfo::exists(cancelDestination + "/source/first.bin.thispc-part");
           }, 5000) && !QFileInfo::exists(cancelDestination + "/source/first.bin")
               && readFile(source + "/first.bin") == payload,
           "tree cancellation removes the active partial and preserves untransferred sources");

    const QString movedDestination = root.filePath("moved");
    verify(QDir().mkpath(movedDestination), "move destination exists");
    LocalTransferJob move(sources, QUrl::fromLocalFile(movedDestination), true);
    move.setAutoDelete(false);
    QSignalSpy moved(&move, &KJob::result);
    move.start();
    waitResult(moved);
    verify(move.error() != 0 && QFileInfo::exists(source) && QFileInfo::exists(root.filePath("second.txt"))
               && !QFileInfo::exists(movedDestination + "/source")
               && move.errorText() == moveContainmentErrorMessage(),
           "tree move fails closed under containment and preserves all sources without touching destination");

    LocalTransferJob conflict({QUrl::fromLocalFile(destination + "/source")},
                              QUrl::fromLocalFile(destination), false);
    conflict.setAutoDelete(false);
    QSignalSpy conflictResult(&conflict, &KJob::result);
    conflict.start();
    waitResult(conflictResult);
    verify(conflict.error() != 0 && conflict.completedEntries().isEmpty()
               && readFile(destination + "/source/first.bin") == payload,
           "unresolved plan conflicts fail before execution touches files");

    LocalTransferJob cycle({QUrl::fromLocalFile(destination)},
                           QUrl::fromLocalFile(destination + "/source"), false);
    cycle.setAutoDelete(false);
    QSignalSpy cycleResult(&cycle, &KJob::result);
    cycle.start();
    waitResult(cycleResult);
    verify(cycle.error() != 0 && cycle.completedEntries().isEmpty(),
           "directory self-copy is rejected before planning or execution");
    LocalTransferJob duplicate(
        {QUrl::fromLocalFile(destination + "/source"),
         QUrl::fromLocalFile(destination + "/source/first.bin")},
        QUrl::fromLocalFile(cancelDestination), false);
    duplicate.setAutoDelete(false);
    QSignalSpy duplicateResult(&duplicate, &KJob::result);
    duplicate.start();
    waitResult(duplicateResult);
    verify(duplicate.error() != 0 && duplicate.completedEntries().isEmpty(),
           "overlapping source selections are rejected before any move or copy");
    const QString linkRoot = root.filePath("links");
    const QString linkDestination = root.filePath("link-destination");
    verify(QDir().mkpath(linkRoot + "/nested") && QDir().mkpath(linkDestination)
               && writeFile(linkRoot + "/nested/data", "link payload")
               && writeFile(root.filePath("external"), "external sentinel"),
           "symbolic-link and metadata fixtures are created");
    verify(::symlink("nested/data", QFile::encodeName(linkRoot + "/relative").constData()) == 0
               && ::symlink("absent", QFile::encodeName(linkRoot + "/broken").constData()) == 0
               && ::symlink("nested", QFile::encodeName(linkRoot + "/directory-link").constData()) == 0
               && ::symlink(QFile::encodeName(root.filePath("external")).constData(),
                            QFile::encodeName(linkRoot + "/outside").constData()) == 0,
           "relative, broken, directory and external links are created");
    const timespec expectedTimes[]{{1600000000, 123456789}, {1600003600, 987654321}};
    verify(::chmod(QFile::encodeName(linkRoot + "/nested").constData(), 0550) == 0
               && ::utimensat(AT_FDCWD, QFile::encodeName(linkRoot + "/nested").constData(), expectedTimes, 0) == 0
               && ::utimensat(AT_FDCWD, QFile::encodeName(linkRoot).constData(), expectedTimes, 0) == 0,
           "directory permissions and nanosecond timestamps are prepared");
    LocalTransferJob links({QUrl::fromLocalFile(linkRoot)}, QUrl::fromLocalFile(linkDestination), false);
    links.setAutoDelete(false);
    QSignalSpy linkResult(&links, &KJob::result);
    links.start();
    waitResult(linkResult);
    const QString copiedLinks = linkDestination + "/links";
    struct stat nestedStat {}, rootStat {};
    verify(links.error() == 0 && ::lstat(QFile::encodeName(copiedLinks + "/nested").constData(), &nestedStat) == 0
               && ::lstat(QFile::encodeName(copiedLinks).constData(), &rootStat) == 0
               && (nestedStat.st_mode & 0777) == 0550
               && nestedStat.st_atim.tv_sec == expectedTimes[0].tv_sec
               && nestedStat.st_atim.tv_nsec == expectedTimes[0].tv_nsec
               && nestedStat.st_mtim.tv_sec == expectedTimes[1].tv_sec
               && nestedStat.st_mtim.tv_nsec == expectedTimes[1].tv_nsec
               && rootStat.st_mtim.tv_nsec == expectedTimes[1].tv_nsec,
           "new directory metadata is restored after child creation with nanosecond precision");
    verify(readLocalLink(copiedLinks + "/relative") == "nested/data"
               && readLocalLink(copiedLinks + "/broken") == "absent"
               && QFileInfo(copiedLinks + "/broken").isSymLink()
               && readLocalLink(copiedLinks + "/directory-link") == "nested"
               && readLocalLink(copiedLinks + "/outside") == QFile::encodeName(root.filePath("external"))
               && readFile(root.filePath("external")) == "external sentinel"
               && links.transferPlan().symbolicLinkCount == 4
               && links.totalAmount(KJob::Bytes) == 12,
           "symbolic links retain their literal targets without traversing or copying target data");
    ::chmod(QFile::encodeName(linkRoot + "/nested").constData(), 0750);
    ::chmod(QFile::encodeName(copiedLinks + "/nested").constData(), 0750);

    const QString movedLinks = root.filePath("moved-links");
    verify(QDir().mkdir(movedLinks), "symbolic-link move destination is created");
    LocalTransferJob linkMove({QUrl::fromLocalFile(linkRoot)}, QUrl::fromLocalFile(movedLinks), true);
    linkMove.setAutoDelete(false);
    QSignalSpy linkMoved(&linkMove, &KJob::result);
    linkMove.start();
    waitResult(linkMoved);
    verify(linkMove.error() != 0 && QFileInfo::exists(linkRoot)
               && !QFileInfo::exists(movedLinks + "/links")
               && linkMove.errorText() == moveContainmentErrorMessage(),
           "symbolic-link move fails closed under containment without modifying source or destination");

    // =========================================================================
    // Stage 5.3.2G.3A.1: Local Move Safety Containment Scenarios (A - L)
    // =========================================================================

    // Scenario A: Single regular file move without overwrite still succeeds
    {
        const QString srcA = root.filePath("single-regular-src.txt");
        const QString dstA = root.filePath("single-regular-dst.txt");
        verify(writeFile(srcA, "scenario-a-payload"), "Scenario A: regular file fixture created");
        LocalFileMoveJob moveA(QUrl::fromLocalFile(srcA), QUrl::fromLocalFile(dstA));
        moveA.setAutoDelete(false);
        QSignalSpy spyA(&moveA, &KJob::result);
        moveA.start();
        waitResult(spyA);
        verify(moveA.error() == 0 && !QFileInfo::exists(srcA) && readFile(dstA) == "scenario-a-payload",
               "Scenario A: local move of single regular file without overwrite still succeeds");
    }

    // Scenario B: Single symlink move is fail-closed before modification
    {
        const QString linkTarget = root.filePath("symlink-target.txt");
        const QString linkSrc = root.filePath("symlink-src.lnk");
        const QString linkDst = root.filePath("symlink-dst.lnk");
        verify(writeFile(linkTarget, "target-payload")
                   && ::symlink(QFile::encodeName(linkTarget).constData(), QFile::encodeName(linkSrc).constData()) == 0,
               "Scenario B: symlink fixture created");
        LocalTransferJob moveB({QUrl::fromLocalFile(linkSrc)}, QUrl::fromLocalFile(root.filePath("dir-b")), true);
        moveB.setAutoDelete(false);
        QSignalSpy spyB(&moveB, &KJob::result);
        moveB.start();
        waitResult(spyB);
        verify(moveB.error() != 0 && QFileInfo(linkSrc).isSymLink() && !QFileInfo::exists(linkDst)
                   && moveB.errorText() == moveContainmentErrorMessage(),
               "Scenario B: symlink move is fail-closed before modification");
    }

    // Scenario C: Directory move is fail-closed before modification
    {
        const QString dirSrc = root.filePath("dir-c-src");
        const QString dirDst = root.filePath("dir-c-dst");
        verify(QDir().mkpath(dirSrc + "/nested") && writeFile(dirSrc + "/data.bin", "dir-c-data"),
               "Scenario C: directory fixture created");
        LocalTransferJob moveC({QUrl::fromLocalFile(dirSrc)}, QUrl::fromLocalFile(dirDst), true);
        moveC.setAutoDelete(false);
        QSignalSpy spyC(&moveC, &KJob::result);
        moveC.start();
        waitResult(spyC);
        verify(moveC.error() != 0 && QFileInfo::exists(dirSrc) && QFileInfo::exists(dirSrc + "/data.bin")
                   && !QFileInfo::exists(dirDst + "/dir-c-src")
                   && moveC.errorText() == moveContainmentErrorMessage(),
               "Scenario C: directory move is fail-closed before modification");
    }

    // Scenario D: Multiple files move is fail-closed before modification
    {
        const QString fileD1 = root.filePath("multi-d1.txt");
        const QString fileD2 = root.filePath("multi-d2.txt");
        const QString dirDstD = root.filePath("dir-d-dst");
        verify(writeFile(fileD1, "d1") && writeFile(fileD2, "d2") && QDir().mkpath(dirDstD),
               "Scenario D: multi-file fixtures created");
        LocalTransferJob moveD({QUrl::fromLocalFile(fileD1), QUrl::fromLocalFile(fileD2)},
                               QUrl::fromLocalFile(dirDstD), true);
        moveD.setAutoDelete(false);
        QSignalSpy spyD(&moveD, &KJob::result);
        moveD.start();
        waitResult(spyD);
        verify(moveD.error() != 0 && QFileInfo::exists(fileD1) && QFileInfo::exists(fileD2)
                   && !QFileInfo::exists(dirDstD + "/multi-d1.txt")
                   && moveD.errorText() == moveContainmentErrorMessage(),
               "Scenario D: multi-file move is fail-closed before modification");
    }

    // Scenario E: Plan conflict does not invoke KIO fallback when move is true
    {
        bool fallbackInvoked = false;
        LocalTransferJob moveE(sources, QUrl::fromLocalFile(destination), true);
        moveE.setAutoDelete(false);
        moveE.setFallbackFactory([&]() -> KIO::CopyJob * {
            fallbackInvoked = true;
            return nullptr;
        });
        QSignalSpy spyE(&moveE, &KJob::result);
        moveE.start();
        waitResult(spyE);
        verify(moveE.error() != 0 && !fallbackInvoked && !moveE.usedKio(),
               "Scenario E: move containment prevents KIO fallback invocation on conflict");
    }

    // Scenario F: UnsupportedEntry does not invoke KIO fallback for blocked local move
    {
        bool fallbackInvokedF = false;
        LocalTransferJob moveF(sources, QUrl::fromLocalFile(root.filePath("non-existent-parent/dir")), true);
        moveF.setAutoDelete(false);
        moveF.setFallbackFactory([&]() -> KIO::CopyJob * {
            fallbackInvokedF = true;
            return nullptr;
        });
        QSignalSpy spyF(&moveF, &KJob::result);
        moveF.start();
        waitResult(spyF);
        verify(moveF.error() != 0 && !fallbackInvokedF && !moveF.usedKio(),
               "Scenario F: move containment prevents KIO fallback on unsupported/invalid entry");
    }

    // Scenario G: Error message conforms strictly to UX requirements
    {
        const QString expectedMsg = moveContainmentErrorMessage();
        verify(!expectedMsg.isEmpty() && (expectedMsg.contains("bezpieczeństwa") || expectedMsg.contains("safety")),
               "Scenario G: containment error message is clear, localized, and actionable");
    }

    // Scenario H: Direct invocation of LocalTransferJob cannot bypass the containment lock
    {
        LocalTransferJob moveH(sources, QUrl::fromLocalFile(root.filePath("direct-invoc")), true);
        moveH.setAutoDelete(false);
        QSignalSpy spyH(&moveH, &KJob::result);
        moveH.start();
        waitResult(spyH);
        verify(moveH.error() != 0 && moveH.errorText() == moveContainmentErrorMessage(),
               "Scenario H: direct invocation of LocalTransferJob fails closed");
    }

    // Scenario I: Ordinary copy of files and directories does not regress
    {
        const QString copySrcDir = root.filePath("copy-reg-src");
        const QString copyDstDir = root.filePath("copy-reg-dst");
        verify(QDir().mkpath(copySrcDir) && QDir().mkpath(copyDstDir) && writeFile(copySrcDir + "/test.txt", "copy-data"),
               "Scenario I: copy fixture created");
        LocalTransferJob copyI({QUrl::fromLocalFile(copySrcDir)}, QUrl::fromLocalFile(copyDstDir), false);
        copyI.setAutoDelete(false);
        QSignalSpy spyI(&copyI, &KJob::result);
        copyI.start();
        waitResult(spyI);
        verify(copyI.error() == 0 && QFileInfo::exists(copySrcDir + "/test.txt")
                   && readFile(copyDstDir + "/copy-reg-src/test.txt") == "copy-data",
               "Scenario I: regular copy continues to work without regression");
    }

    // Scenario K: Failed move is rejected from history and Undo
    {
        LocalTreeHistory history;
        LocalTransferJob moveK(sources, QUrl::fromLocalFile(root.filePath("history-fail")), true);
        moveK.setAutoDelete(false);
        QSignalSpy spyK(&moveK, &KJob::result);
        moveK.start();
        waitResult(spyK);
        history.recordCompleted(&moveK, 99);
        verify(moveK.error() != 0 && !history.canUndo() && !history.canRedo(),
               "Scenario K: failed contained move is rejected from history and Undo");
    }

    // Scenario L: Blocked operations leave no staging (.thispc-part) files or modified data
    {
        const QString stagingDst = root.filePath("check-staging-dir");
        verify(QDir().mkpath(stagingDst), "Scenario L: staging dir created");
        LocalTransferJob moveL(sources, QUrl::fromLocalFile(stagingDst), true);
        moveL.setAutoDelete(false);
        QSignalSpy spyL(&moveL, &KJob::result);
        moveL.start();
        waitResult(spyL);
        const QStringList entries = QDir(stagingDst).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        verify(entries.isEmpty(), "Scenario L: zero staging files or leftover entries in destination");
    }

    qInfo("PASS: %d LocalTransferJob assertions", checks);
    return 0;
}
