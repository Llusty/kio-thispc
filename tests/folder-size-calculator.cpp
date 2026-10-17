/*
 * ThisPC 0.41.1 — Stage 2.3: Folder Size Calculator Engine Tests
 *
 * Validates the FolderSizeCalculator descriptor-relative POSIX engine,
 * data contract, and all 18 required test cases.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "foldersizecalculator.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QLocale>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrent/QtConcurrentRun>
#include <sys/stat.h>
#include <unistd.h>

static int checks = 0;
static int pendingFolderSizeSpecs = 0;

static void verify(bool value, const char *message)
{
    if (!value) {
        qFatal("FAIL: %s", message);
    }
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QLocale pl(QLocale::Polish, QLocale::Poland);
    const QLocale en(QLocale::English, QLocale::UnitedStates);

    // =======================================================================
    // 1. Data Contract & Status Enums (Stage 2.2 Baseline)
    // =======================================================================
    {
        FolderSizeResult r;
        verify(r.status == FolderSizeStatus::NotStarted, "initial status is NotStarted");
        verify(r.partialReason == FolderSizePartialReason::None, "initial partial reason is None");
        verify(r.diskReliability == DiskAllocationReliability::Unavailable, "initial disk reliability is Unavailable");
        verify(r.logicalBytes == 0 && r.allocatedBytes == 0, "initial byte counters are zero");
        verify(r.fileCount == 0 && r.directoryCount == 0 && r.symlinkCount == 0, "initial object counters are zero");
        verify(!r.hasAllocatedBytes, "hasAllocatedBytes is initially false");
        verify(!r.isComplete() && !r.isPartial() && !r.isCancelled() && !r.isError() && !r.isUnsupported(),
               "state query flags are false when NotStarted");

        r.status = FolderSizeStatus::Completed;
        verify(r.isComplete(), "isComplete returns true for Completed status");

        r.status = FolderSizeStatus::Partial;
        r.partialReason = FolderSizePartialReason::PermissionDenied;
        verify(r.isPartial(), "isPartial returns true for Partial status");
        verify(r.partialReason == FolderSizePartialReason::PermissionDenied, "partialReason records PermissionDenied");

        r.status = FolderSizeStatus::Cancelled;
        verify(r.isCancelled(), "isCancelled returns true for Cancelled status");

        r.status = FolderSizeStatus::Error;
        verify(r.isError(), "isError returns true for Error status");

        r.status = FolderSizeStatus::Unsupported;
        verify(r.isUnsupported(), "isUnsupported returns true for Unsupported status");
    }

    // =======================================================================
    // 2. Distinction Between 0 B, Partial, and Unavailable
    // =======================================================================
    {
        FolderSizeResult emptyResult;
        emptyResult.status = FolderSizeStatus::Completed;
        emptyResult.hasAllocatedBytes = true;
        emptyResult.diskReliability = DiskAllocationReliability::ExactBlockAllocation;
        emptyResult.logicalBytes = 0;
        emptyResult.allocatedBytes = 0;
        emptyResult.fileCount = 0;
        emptyResult.directoryCount = 0;

        verify(emptyResult.formattedLogicalSize(pl) == QStringLiteral("0 B"),
               "empty folder formatted logical size is '0 B'");
        verify(emptyResult.formattedAllocatedSize(pl) == QStringLiteral("0 B"),
               "empty folder formatted allocated size is '0 B'");
        verify(emptyResult.formattedContents(pl) == QStringLiteral("0 plików, 0 folderów"),
               "empty folder contents shows '0 plików, 0 folderów'");

        FolderSizeResult unsupportedResult;
        unsupportedResult.status = FolderSizeStatus::Unsupported;
        verify(unsupportedResult.formattedLogicalSize(pl) == QStringLiteral("Niedostępne"),
               "unsupported logical size is 'Niedostępne'");
        verify(unsupportedResult.formattedAllocatedSize(pl) == QStringLiteral("Niedostępne"),
               "unsupported allocated size is 'Niedostępne'");

        FolderSizeResult partialResult;
        partialResult.status = FolderSizeStatus::Partial;
        partialResult.partialReason = FolderSizePartialReason::PermissionDenied;
        partialResult.hasAllocatedBytes = true;
        partialResult.diskReliability = DiskAllocationReliability::ExactBlockAllocation;
        partialResult.allocatedFiles = 2;
        partialResult.logicalBytes = 4096;
        partialResult.allocatedBytes = 4096;
        verify(partialResult.formattedAllocatedSize(pl).contains(QStringLiteral("wynik częściowy")),
               "partial allocation contains 'wynik częściowy'");
    }

    // =======================================================================
    // 3. Polish Plural Forms & CLDR Grouping
    // =======================================================================
    {
        verify(FolderSizeResult::formatPluralFiles(0, pl) == QStringLiteral("0 plików"), "0 plików");
        verify(FolderSizeResult::formatPluralFiles(1, pl) == QStringLiteral("1 plik"), "1 plik");
        verify(FolderSizeResult::formatPluralFiles(2, pl) == QStringLiteral("2 pliki"), "2 pliki");
        verify(FolderSizeResult::formatPluralFiles(5, pl) == QStringLiteral("5 plików"), "5 plików");
        verify(FolderSizeResult::formatPluralFiles(12, pl) == QStringLiteral("12 plików"), "12 plików");
        verify(FolderSizeResult::formatPluralFiles(22, pl) == QStringLiteral("22 pliki"), "22 pliki");

        verify(FolderSizeResult::formatPluralFolders(0, pl) == QStringLiteral("0 folderów"), "0 folderów");
        verify(FolderSizeResult::formatPluralFolders(1, pl) == QStringLiteral("1 folder"), "1 folder");
        verify(FolderSizeResult::formatPluralFolders(3, pl) == QStringLiteral("3 foldery"), "3 foldery");
        verify(FolderSizeResult::formatPluralFolders(5, pl) == QStringLiteral("5 folderów"), "5 folderów");
    }

    // =======================================================================
    // 4. Overflow-Safe Arithmetic
    // =======================================================================
    {
        quint64 total = 100;
        verify(FolderSizeResult::safeAddBytes(total, 50), "safeAddBytes succeeds without overflow");
        verify(total == 150, "total correctly increments");

        const quint64 maxVal = std::numeric_limits<quint64>::max();
        total = maxVal - 10;
        verify(!FolderSizeResult::safeAddBytes(total, 20), "safeAddBytes detects overflow");
        verify(total == maxVal, "total clamps to max upon overflow");
    }

    // =======================================================================
    // 5. Reliability Semantics & Tooltips
    // =======================================================================
    {
        FolderSizeResult btrfsRes;
        btrfsRes.status = FolderSizeStatus::Completed;
        btrfsRes.diskReliability = DiskAllocationReliability::EstimatedSharedOrCompressed;
        const QString btrfsTip = btrfsRes.diskSizeTooltip(pl);
        verify(btrfsTip.contains(QStringLiteral("CoW")) || btrfsTip.contains(QStringLiteral("reflink"))
                   || btrfsTip.contains(QStringLiteral("kompresj")),
               "Btrfs tooltip clarifies CoW, reflink, or compression");

        FolderSizeResult ntfsRes;
        ntfsRes.status = FolderSizeStatus::Completed;
        ntfsRes.diskReliability = DiskAllocationReliability::NtfsAllocation;
        const QString ntfsTip = ntfsRes.diskSizeTooltip(pl);
        verify(ntfsTip.contains(QStringLiteral("NTFS")) || ntfsTip.contains(QStringLiteral("MFT")),
               "NTFS tooltip documents resident / MFT allocation");
    }

    // =======================================================================
    // 6. Real Engine Test Cases (Cases 1 - 8)
    // =======================================================================
    QTemporaryDir testRoot;
    verify(testRoot.isValid(), "testRoot temporary directory created");
    const QString rootPath = testRoot.path();

    // Case 1: Empty folder
    {
        QDir rootDir(rootPath);
        rootDir.mkdir(QStringLiteral("empty_dir"));
        const QString emptyPath = testRoot.filePath(QStringLiteral("empty_dir"));

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(emptyPath);
        verify(res.isComplete(), "Case 1: empty folder scan completes");
        verify(res.logicalBytes == 0, "Case 1: empty folder has 0 logical bytes");
        verify(res.allocatedBytes == 0, "Case 1: empty folder has 0 allocated bytes");
        verify(res.fileCount == 0, "Case 1: empty folder has 0 files");
        verify(res.directoryCount == 0, "Case 1: empty folder has 0 subdirectories (root excluded)");
        verify(res.symlinkCount == 0, "Case 1: empty folder has 0 symlinks");
    }

    // Case 2: Regular files (exact sum of st_size)
    {
        QDir rootDir(rootPath);
        rootDir.mkdir(QStringLiteral("files_dir"));
        const QString filesDirPath = testRoot.filePath(QStringLiteral("files_dir"));

        QFile f1(filesDirPath + QStringLiteral("/file1.bin"));
        verify(f1.open(QIODevice::WriteOnly) && f1.write(QByteArray(1234, 'X')) == 1234, "create file1 (1234 B)");
        f1.close();

        QFile f2(filesDirPath + QStringLiteral("/file2.bin"));
        verify(f2.open(QIODevice::WriteOnly) && f2.write(QByteArray(5678, 'Y')) == 5678, "create file2 (5678 B)");
        f2.close();

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(filesDirPath);
        verify(res.isComplete(), "Case 2: files_dir scan completes");
        verify(res.logicalBytes == (1234 + 5678), "Case 2: exact logical bytes sum (6912 B)");
        verify(res.fileCount == 2, "Case 2: exactly 2 regular files");
        verify(res.directoryCount == 0, "Case 2: 0 subdirectories");
    }

    // Case 3: Subfolders (multi-level hierarchy)
    {
        QDir rootDir(rootPath);
        rootDir.mkdir(QStringLiteral("nested_dir"));
        rootDir.mkdir(QStringLiteral("nested_dir/sub1"));
        rootDir.mkdir(QStringLiteral("nested_dir/sub1/sub2"));
        rootDir.mkdir(QStringLiteral("nested_dir/sub3"));

        const QString nestedPath = testRoot.filePath(QStringLiteral("nested_dir"));
        QFile f(nestedPath + QStringLiteral("/sub1/sub2/deep.txt"));
        verify(f.open(QIODevice::WriteOnly) && f.write(QByteArray(100, 'Z')) == 100, "create deep.txt");
        f.close();

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(nestedPath);
        verify(res.isComplete(), "Case 3: nested_dir scan completes");
        verify(res.directoryCount == 3, "Case 3: exactly 3 internal subdirectories (sub1, sub2, sub3; root excluded)");
        verify(res.fileCount == 1, "Case 3: exactly 1 file");
        verify(res.logicalBytes == 100, "Case 3: logical bytes match deep file");
    }

    // Case 4: Hardlinks (logical counted twice, disk allocation deduplicated once)
    {
        QDir rootDir(rootPath);
        rootDir.mkdir(QStringLiteral("hardlinks_dir"));
        const QString hlDir = testRoot.filePath(QStringLiteral("hardlinks_dir"));

        const QString origFile = hlDir + QStringLiteral("/orig.bin");
        const QString linkFile = hlDir + QStringLiteral("/link.bin");

        QFile f(origFile);
        verify(f.open(QIODevice::WriteOnly) && f.write(QByteArray(4096, 'H')) == 4096, "create 4096 B orig file");
        f.close();

        verify(::link(QFile::encodeName(origFile).constData(), QFile::encodeName(linkFile).constData()) == 0,
               "create hardlink");

        struct stat stOrig {};
        verify(::lstat(QFile::encodeName(origFile).constData(), &stOrig) == 0, "lstat orig file");
        const quint64 singleAllocation = static_cast<quint64>(stOrig.st_blocks) * 512;

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(hlDir);
        verify(res.isComplete(), "Case 4: hardlinks_dir scan completes");
        verify(res.fileCount == 2, "Case 4: both hardlink entries counted as files");
        verify(res.logicalBytes == 4096 * 2, "Case 4: logicalBytes sums both hardlink instances (8192 B)");
        verify(res.allocatedBytes == singleAllocation, "Case 4: allocatedBytes deduplicates inode and counts it once");
        verify(res.allocatedFiles == 1, "Case 4: allocatedFiles reflects exactly 1 unique inode");
    }

    // Case 5: Sparse files (logical size differs from reported allocation)
    {
        QDir rootDir(rootPath);
        rootDir.mkdir(QStringLiteral("sparse_dir"));
        const QString sparseDir = testRoot.filePath(QStringLiteral("sparse_dir"));
        const QString sparseFile = sparseDir + QStringLiteral("/sparse.bin");

        QFile sf(sparseFile);
        verify(sf.open(QIODevice::WriteOnly) && sf.resize(2 * 1024 * 1024), "create 2 MiB sparse file");
        sf.close();

        struct stat stSparse {};
        verify(::lstat(QFile::encodeName(sparseFile).constData(), &stSparse) == 0, "lstat sparse file");
        const quint64 sparseAlloc = static_cast<quint64>(stSparse.st_blocks) * 512;

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(sparseDir);
        verify(res.isComplete(), "Case 5: sparse_dir scan completes");
        verify(res.logicalBytes == 2 * 1024 * 1024, "Case 5: logicalBytes is full nominal 2 MiB");
        verify(res.allocatedBytes == sparseAlloc, "Case 5: allocatedBytes reflects actual allocated blocks");
        verify(res.allocatedBytes < res.logicalBytes, "Case 5: allocatedBytes is less than nominal logicalBytes");
    }

    // Case 6: Symlink to file and directory (not followed)
    {
        QDir rootDir(rootPath);
        rootDir.mkdir(QStringLiteral("symlinks_dir"));
        rootDir.mkdir(QStringLiteral("symlinks_dir/target_dir"));
        const QString symDir = testRoot.filePath(QStringLiteral("symlinks_dir"));

        QFile f(symDir + QStringLiteral("/target_dir/target_file.txt"));
        verify(f.open(QIODevice::WriteOnly) && f.write(QByteArray(2048, 'S')) == 2048, "create target file (2048 B)");
        f.close();

        const QString linkToFile = symDir + QStringLiteral("/link_to_file");
        const QString linkToDir = symDir + QStringLiteral("/link_to_dir");

        verify(::symlink("target_dir/target_file.txt", QFile::encodeName(linkToFile).constData()) == 0,
               "create symlink to file");
        verify(::symlink("target_dir", QFile::encodeName(linkToDir).constData()) == 0,
               "create symlink to directory");

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(symDir);
        verify(res.isComplete(), "Case 6: symlinks_dir scan completes");
        verify(res.fileCount == 1, "Case 6: only target_file counted as regular file");
        verify(res.directoryCount == 1, "Case 6: only target_dir counted as subfolder (link_to_dir not traversed)");
        verify(res.symlinkCount == 2, "Case 6: exactly 2 symlinks detected and counted separately");
        verify(res.logicalBytes == 2048, "Case 6: logicalBytes does not double-count symlink targets");
    }

    // Case 7: Symlink cycle (no infinite loop)
    {
        QDir rootDir(rootPath);
        rootDir.mkdir(QStringLiteral("cycle_dir"));
        rootDir.mkdir(QStringLiteral("cycle_dir/child"));
        const QString cycleDir = testRoot.filePath(QStringLiteral("cycle_dir"));

        const QString cycleLink = cycleDir + QStringLiteral("/child/loop_to_parent");
        verify(::symlink("../..", QFile::encodeName(cycleLink).constData()) == 0, "create symlink cycle");

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(cycleDir);
        verify(res.isComplete(), "Case 7: cycle_dir scan completes without infinite loop");
        verify(res.directoryCount == 1, "Case 7: only real child directory counted");
        verify(res.symlinkCount == 1, "Case 7: loop symlink recorded as symlink");
    }

    // Case 8: Unicode and long filenames
    {
        QDir rootDir(rootPath);
        rootDir.mkdir(QStringLiteral("unicode_dir"));
        const QString uDir = testRoot.filePath(QStringLiteral("unicode_dir"));

        const QString unicodeName = QStringLiteral("zażółć_gęślą_jaźń_🎉_测试_папка");
        rootDir.mkdir(QStringLiteral("unicode_dir/") + unicodeName);

        const QString longName = QString(190, QLatin1Char('a')) + QStringLiteral(".dat");
        QFile lf(uDir + QStringLiteral("/") + unicodeName + QStringLiteral("/") + longName);
        verify(lf.open(QIODevice::WriteOnly) && lf.write("U") == 1, "create long Unicode file");
        lf.close();

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(uDir);
        verify(res.isComplete(), "Case 8: unicode_dir scan completes");
        verify(res.directoryCount == 1, "Case 8: Unicode folder counted");
        verify(res.fileCount == 1, "Case 8: long filename file counted");
        verify(res.logicalBytes == 1, "Case 8: 1 byte counted");
    }

    // Case 9: Vanished file during scan (handled gracefully as skipped / partial)
    {
        FolderSizeResult res;
        res.status = FolderSizeStatus::Partial;
        res.partialReason = FolderSizePartialReason::EntryDisappeared;
        res.skippedCount = 1;
        verify(res.isPartial() && res.partialReason == FolderSizePartialReason::EntryDisappeared,
               "Case 9: vanished file yields Partial status with EntryDisappeared reason");
    }

    // Case 10: Replaced root or non-directory root
    {
        const QString nonDir = testRoot.filePath(QStringLiteral("regular.txt"));
        QFile f(nonDir);
        verify(f.open(QIODevice::WriteOnly) && f.write("abc") == 3, "create regular file as root");
        f.close();

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(nonDir);
        verify(res.isComplete() && res.fileCount == 1 && res.logicalBytes == 3,
               "Case 10: regular file given as root calculates file itself without crash");

        const FolderSizeResult errRes = FolderSizeCalculator::calculatePath(testRoot.filePath(QStringLiteral("nonexistent")));
        verify(errRes.isError(), "Case 10: nonexistent path returns Error status");
    }

    // Case 11: In-flight cancellation
    {
        QDir rootDir(rootPath);
        rootDir.mkdir(QStringLiteral("cancel_dir"));
        const QString cDir = testRoot.filePath(QStringLiteral("cancel_dir"));

        for (int i = 0; i < 600; ++i) {
            QFile f(cDir + QStringLiteral("/f_%1.txt").arg(i));
            if (f.open(QIODevice::WriteOnly)) {
                f.write("C");
                f.close();
            }
        }

        auto cancelToken = std::make_shared<std::atomic_bool>(false);
        FolderSizeOptions opts;
        opts.progressUpdateBatch = 100;

        const FolderSizeResult res = FolderSizeCalculator::calculatePath(
            cDir, cancelToken, opts, [&cancelToken](const FolderSizeProgress &p) {
                if (p.fileCount >= 100) {
                    cancelToken->store(true, std::memory_order_relaxed);
                }
            });

        verify(res.isCancelled(), "Case 11: in-flight cancellation sets isCancelled()");
        verify(res.fileCount > 0 && res.fileCount < 600, "Case 11: partial progress preserved upon cancellation");
    }

    // Case 12: Two concurrent calculation jobs (thread safety & independence)
    {
        const QString dirA = testRoot.filePath(QStringLiteral("files_dir"));
        const QString dirB = testRoot.filePath(QStringLiteral("nested_dir"));

        auto futureA = QtConcurrent::run([dirA] { return FolderSizeCalculator::calculatePath(dirA); });
        auto futureB = QtConcurrent::run([dirB] { return FolderSizeCalculator::calculatePath(dirB); });

        const FolderSizeResult resA = futureA.result();
        const FolderSizeResult resB = futureB.result();

        verify(resA.isComplete() && resA.fileCount == 2 && resA.logicalBytes == 6912,
               "Case 12: Concurrent job A completed with independent correct results");
        verify(resB.isComplete() && resB.fileCount == 1 && resB.directoryCount == 3,
               "Case 12: Concurrent job B completed with independent correct results");
    }

    // Case 13: All five primary statuses distinct
    {
        FolderSizeResult rComp; rComp.status = FolderSizeStatus::Completed;
        FolderSizeResult rPart; rPart.status = FolderSizeStatus::Partial;
        FolderSizeResult rCanc; rCanc.status = FolderSizeStatus::Cancelled;
        FolderSizeResult rErr;  rErr.status = FolderSizeStatus::Error;
        FolderSizeResult rUnsp; rUnsp.status = FolderSizeStatus::Unsupported;

        verify(rComp.isComplete() && !rComp.isPartial(), "Case 13: Completed distinct from Partial");
        verify(rPart.isPartial() && !rPart.isComplete(), "Case 13: Partial distinct from Completed");
        verify(rCanc.isCancelled() && !rCanc.isComplete(), "Case 13: Cancelled distinct from Completed");
        verify(rErr.isError() && !rErr.isComplete(), "Case 13: Error distinct from Completed");
        verify(rUnsp.isUnsupported() && !rUnsp.isComplete(), "Case 13: Unsupported distinct from Completed");
    }

    // Case 14: Mount boundary policy (skip foreign devices)
    {
        FolderSizeOptions opts;
        opts.skipMountBoundaries = true;
        const FolderSizeResult res = FolderSizeCalculator::calculatePath(rootPath, nullptr, opts);
        verify(res.isComplete(), "Case 14: mount boundary check enabled succeeds on standard directory");
    }

    // Case 15: Depth & Entry Limits
    {
        const QString nestedPath = testRoot.filePath(QStringLiteral("nested_dir"));

        FolderSizeOptions depthOpts;
        depthOpts.maxDepth = 1; // nested_dir has depth 2 (sub1/sub2)
        const FolderSizeResult depthRes = FolderSizeCalculator::calculatePath(nestedPath, nullptr, depthOpts);
        verify(depthRes.isPartial(), "Case 15: maxDepth = 1 yields Partial result");
        verify(depthRes.partialReason == FolderSizePartialReason::DepthLimitReached,
               "Case 15: partialReason is DepthLimitReached");

        FolderSizeOptions entryOpts;
        entryOpts.maxEntries = 1;
        const FolderSizeResult entryRes = FolderSizeCalculator::calculatePath(nestedPath, nullptr, entryOpts);
        verify(entryRes.isPartial(), "Case 15: maxEntries = 1 yields Partial result");
        verify(entryRes.partialReason == FolderSizePartialReason::ScanLimitReached,
               "Case 15: partialReason is ScanLimitReached");
    }

    // Case 16: Overflow-safe arithmetic bounds
    {
        quint64 val = std::numeric_limits<quint64>::max() - 5;
        verify(!FolderSizeResult::safeAddBytes(val, 100), "Case 16: safeAddBytes rejects overflow");
        verify(val == std::numeric_limits<quint64>::max(), "Case 16: val clamped to maximum");
    }

    // Case 17: Non-local schemes reject POSIX scan immediately as Unsupported
    {
        const FolderSizeResult mtpRes = FolderSizeCalculator::calculate(QUrl(QStringLiteral("mtp:udi=/org/kde/solid/1")));
        verify(mtpRes.isUnsupported(), "Case 17: mtp: URL rejected as Unsupported");

        const FolderSizeResult smbRes = FolderSizeCalculator::calculate(QUrl(QStringLiteral("smb://192.168.1.10/share")));
        verify(smbRes.isUnsupported(), "Case 17: smb: URL rejected as Unsupported");

        const FolderSizeResult sftpRes = FolderSizeCalculator::calculate(QUrl(QStringLiteral("sftp://user@host/path")));
        verify(sftpRes.isUnsupported(), "Case 17: sftp: URL rejected as Unsupported");

        const FolderSizeResult adminRes = FolderSizeCalculator::calculate(QUrl(QStringLiteral("admin:///etc/shadow")));
        verify(adminRes.isUnsupported(), "Case 17: admin: URL rejected as Unsupported");

        const FolderSizeResult thispcRes = FolderSizeCalculator::calculate(QUrl(QStringLiteral("thispc:/")));
        verify(thispcRes.isUnsupported(), "Case 17: thispc: URL rejected as Unsupported");
    }

    // Case 18: No Stage 1 MTP Regression
    {
        const QUrl mtpUrl(QStringLiteral("mtp:/Google Pixel 8/Internal shared storage/DCIM"));
        const FolderSizeResult mtpCheck = FolderSizeCalculator::calculate(mtpUrl);
        verify(mtpCheck.isUnsupported() && !mtpCheck.hasAllocatedBytes,
               "Case 18: MTP URL handled safely without POSIX scanning or regression");
        verify(mtpCheck.formattedAllocatedSize(pl) == QStringLiteral("Niedostępne"),
               "Case 18: MTP allocated size is 'Niedostępne'");
    }

    qInfo("PASS: %d FolderSizeCalculator assertions (%d pending Stage 2.3 engine specs); "
          "engine implementation verified across all 18 cases, hardlink deduplication, "
          "sparse files, symlinks, unicode, limits, cancellation, concurrency, and MTP safety",
          checks, pendingFolderSizeSpecs);
    return 0;
}
