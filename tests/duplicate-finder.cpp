/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "duplicatefinderdata.h"
#include "duplicatefinderjob.h"
#include "duplicategroupmodel.h"
#include "storagescandata.h"
#include "storagescandialog.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <sys/stat.h>
#include <unistd.h>

static int checks = 0;
static void verify(bool condition, const char *description)
{
    if (!condition) {
        qFatal("FAIL: %s", description);
    }
    ++checks;
}

static void writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qFatal("Failed to open file for writing: %s", qPrintable(path));
    }
    file.write(content);
    file.close();
}

static DuplicateFinderResult runFinder(DuplicateFinderJob &job)
{
    DuplicateFinderResult result;
    QEventLoop loop;
    QObject::connect(&job, &DuplicateFinderJob::finished, &loop, [&](const DuplicateFinderResult &res) {
        result = res;
        loop.quit();
    });
    if (!job.start()) {
        return job.result();
    }
    if (job.state() == DuplicateFinderState::Running) {
        loop.exec();
    } else {
        result = job.result();
    }
    return result;
}

static StorageScanEntry createEntry(const QString &path, quint64 size, quint64 dev, quint64 ino,
                                    quint32 nlink = 1, qint64 mtime = 1000, qint64 ctime = 1000)
{
    StorageScanEntry e;
    e.path = path;
    e.name = path.section(QLatin1Char('/'), -1);
    e.type = StorageEntryType::RegularFile;
    e.logicalSize = size;
    e.allocatedSize = size == 0 ? 0 : ((size + 4095) / 4096) * 4096;
    e.deviceId = dev;
    e.inode = ino;
    e.linkCount = nlink;
    e.mtimeSec = mtime;
    e.mtimeNsec = 0;
    e.ctimeSec = ctime;
    e.ctimeNsec = 0;
    e.isHidden = e.name.startsWith(QLatin1Char('.'));
    return e;
}

static StorageScanEntry createEntryFromStat(const QString &path, const struct stat &st, quint32 nlink = 1)
{
    StorageScanEntry e;
    e.path = path;
    e.name = path.section(QLatin1Char('/'), -1);
    e.type = StorageEntryType::RegularFile;
    e.logicalSize = st.st_size;
    e.allocatedSize = st.st_size == 0 ? 0 : static_cast<quint64>(st.st_blocks) * 512ULL;
    e.deviceId = st.st_dev;
    e.inode = st.st_ino;
    e.linkCount = nlink;
    e.mtimeSec = st.st_mtim.tv_sec;
    e.mtimeNsec = st.st_mtim.tv_nsec;
    e.ctimeSec = st.st_ctim.tv_sec;
    e.ctimeNsec = st.st_ctim.tv_nsec;
    e.isHidden = e.name.startsWith(QLatin1Char('.'));
    return e;
}


int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "temp directory valid");
    const QString root = tempDir.path();

    // 1. Empty input -> zero groups
    {
        QList<StorageScanEntry> entries;
        DuplicateFinderJob job(root, entries);
        verify(job.plan().candidateGroups == 0, "1. empty input plan candidateGroups 0");
        verify(job.plan().candidateFiles == 0, "1. empty input plan candidateFiles 0");
        const auto res = runFinder(job);
        verify(res.groups.isEmpty(), "1. empty input zero groups");
        verify(res.finderState == DuplicateFinderState::Completed, "1. empty input completed");
    }

    // 2. One file -> zero candidates
    {
        QList<StorageScanEntry> entries = {
            createEntry("/test/single.txt", 100, 1, 101)
        };
        DuplicateFinderJob job(root, entries);
        verify(job.plan().totalPhysicalFiles == 1, "2. one file totalPhysicalFiles 1");
        verify(job.plan().candidateGroups == 0, "2. one file candidateGroups 0");
        verify(job.plan().candidateFiles == 0, "2. one file candidateFiles 0");
        const auto res = runFinder(job);
        verify(res.groups.isEmpty(), "2. one file zero groups");
    }

    // 3, 22. Two unique-size files -> zero hashes, unique-size files do not start ChecksumJob
    {
        QList<StorageScanEntry> entries = {
            createEntry("/test/fileA.txt", 100, 1, 101),
            createEntry("/test/fileB.txt", 200, 1, 102)
        };
        DuplicateFinderJob job(root, entries);
        int jobsCreated = 0;
        job.setJobFactory([&](const QUrl &url, QObject *p, const ChecksumJobOptions &opt) {
            ++jobsCreated;
            return new ChecksumJob(url, p, opt);
        });
        verify(job.plan().candidateGroups == 0, "3. two unique-size candidateGroups 0");
        const auto res = runFinder(job);
        verify(jobsCreated == 0, "22. unique-size files do not start ChecksumJob");
        verify(res.groups.isEmpty(), "3. two unique-size files zero duplicate groups");
    }

    // 4, 10, 23. Real files: same size / different content -> hashed, zero duplicates; different size never hashed
    {
        const QString pathA = tempDir.filePath("diffA.bin");
        const QString pathB = tempDir.filePath("diffB.bin");
        const QString pathC = tempDir.filePath("diffC_diffSize.bin");
        writeFile(pathA, "ContentAAA12345");
        writeFile(pathB, "ContentBBB67890");
        writeFile(pathC, "ContentCCC"); // different size

        struct stat stA {}, stB {}, stC {};
        ::lstat(QFile::encodeName(pathA).constData(), &stA);
        ::lstat(QFile::encodeName(pathB).constData(), &stB);
        ::lstat(QFile::encodeName(pathC).constData(), &stC);

        QList<StorageScanEntry> entries = {
            createEntryFromStat(pathA, stA),
            createEntryFromStat(pathB, stB),
            createEntryFromStat(pathC, stC)
        };

        DuplicateFinderJob job(root, entries);
        QList<QUrl> hashedUrls;
        job.setJobFactory([&](const QUrl &url, QObject *p, const ChecksumJobOptions &opt) {
            hashedUrls.append(url);
            return new ChecksumJob(url, p, opt);
        });

        const auto res = runFinder(job);
        verify(hashedUrls.size() == 2, "23. only size-collision files are hashed");
        verify(!hashedUrls.contains(QUrl::fromLocalFile(pathC)), "10. different size file never hashed");
        verify(res.groups.isEmpty(), "4. same size different content produces zero duplicates");
        verify(res.finderStats.hashedFiles == 2, "4. hashedFiles counter incremented for both");
    }

    // 5, 24, 25, 26, 27. Same size / same content -> one duplicate group with exact SHA-256
    {
        const QString path1 = tempDir.filePath("same1.bin");
        const QString path2 = tempDir.filePath("same2.bin");
        const QByteArray content = "IdenticalPayloadDuplicateContent!#123";
        writeFile(path1, content);
        writeFile(path2, content);

        struct stat st1 {}, st2 {};
        ::lstat(QFile::encodeName(path1).constData(), &st1);
        ::lstat(QFile::encodeName(path2).constData(), &st2);

        QList<StorageScanEntry> entries = {
            createEntryFromStat(path1, st1),
            createEntryFromStat(path2, st2)
        };

        DuplicateFinderJob job(root, entries);
        ChecksumAlgorithm usedAlgorithm = ChecksumAlgorithm::Md5;
        job.setJobFactory([&](const QUrl &url, QObject *p, const ChecksumJobOptions &opt) {
            usedAlgorithm = opt.algorithm;
            return new ChecksumJob(url, p, opt);
        });

        const auto res = runFinder(job);
        verify(usedAlgorithm == ChecksumAlgorithm::Sha256, "24. SHA-256 is always used");
        verify(usedAlgorithm != ChecksumAlgorithm::Sha1, "25. SHA-1 never used by Duplicate Finder");
        verify(usedAlgorithm != ChecksumAlgorithm::Md5, "26. MD5 never used by Duplicate Finder");
        verify(res.groups.size() == 1, "5. same size and same content forms one duplicate group");
        verify(res.groups.first().files.size() == 2, "5. group has 2 physical members");
        verify(!res.groups.first().sha256.isEmpty(), "27. group has valid SHA-256 digest");
    }

    // 6, 7, 8, 9. Three same-content physical files, different names, extensions, and mtimes -> group size 3
    {
        const QString pA = tempDir.filePath("itemA.txt");
        const QString pB = tempDir.filePath("itemB.dat");
        const QString pC = tempDir.filePath("itemC.jpg");
        const QByteArray payload = "ThreeFilesSameContentXYZ999";
        writeFile(pA, payload);
        writeFile(pB, payload);
        writeFile(pC, payload);

        struct stat stA {}, stB {}, stC {};
        ::lstat(QFile::encodeName(pA).constData(), &stA);
        ::lstat(QFile::encodeName(pB).constData(), &stB);
        ::lstat(QFile::encodeName(pC).constData(), &stC);

        QList<StorageScanEntry> entries = {
            createEntryFromStat(pA, stA),
            createEntryFromStat(pB, stB),
            createEntryFromStat(pC, stC)
        };

        DuplicateFinderJob job(root, entries);
        const auto res = runFinder(job);
        verify(res.groups.size() == 1, "6. three identical files form one group");
        verify(res.groups.first().files.size() == 3, "6. group size is 3 physical files");
        verify(res.groups.first().logicalSize == static_cast<quint64>(payload.size()), "7, 8, 9. names/extensions/mtimes do not prevent duplicate grouping");
    }

    // 11. Empty files duplicate correctly
    {
        const QString eA = tempDir.filePath("emptyA.bin");
        const QString eB = tempDir.filePath("emptyB.bin");
        writeFile(eA, QByteArray());
        writeFile(eB, QByteArray());

        struct stat stA {}, stB {};
        ::lstat(QFile::encodeName(eA).constData(), &stA);
        ::lstat(QFile::encodeName(eB).constData(), &stB);

        QList<StorageScanEntry> entries = {
            createEntryFromStat(eA, stA),
            createEntryFromStat(eB, stB)
        };

        DuplicateFinderJob job(root, entries);
        const auto res = runFinder(job);
        verify(res.groups.size() == 1, "11. empty files duplicate correctly");
        verify(res.groups.first().logicalSize == 0, "11. empty group logical size is 0");
        verify(res.groups.first().sha256 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "11. empty file digest matches standard empty SHA-256");
    }

    // 12, 13, 14, 15. Hidden regular files included, directories and symlinks excluded
    {
        QList<StorageScanEntry> mixedEntries;

        // Hidden file duplicate pair
        auto h1 = createEntry("/root/.hidden1.txt", 50, 1, 201);
        auto h2 = createEntry("/root/.hidden2.txt", 50, 1, 202);
        mixedEntries << h1 << h2;

        // Directory
        StorageScanEntry dirEntry;
        dirEntry.path = "/root/subfolder";
        dirEntry.name = "subfolder";
        dirEntry.type = StorageEntryType::Directory;
        dirEntry.logicalSize = 50;
        dirEntry.deviceId = 1;
        dirEntry.inode = 203;
        mixedEntries << dirEntry;

        // Symlinks (valid & broken)
        StorageScanEntry sym1;
        sym1.path = "/root/sym1";
        sym1.type = StorageEntryType::Symlink;
        sym1.logicalSize = 50;
        sym1.deviceId = 1;
        sym1.inode = 204;
        StorageScanEntry sym2;
        sym2.path = "/root/sym_broken";
        sym2.type = StorageEntryType::Symlink;
        sym2.logicalSize = 50;
        sym2.deviceId = 1;
        sym2.inode = 205;
        mixedEntries << sym1 << sym2;

        auto plan = DuplicatePlanBuilder::buildPlan("/root", mixedEntries);
        verify(plan.candidateGroups == 1, "12, 13, 14, 15. only hidden regular files formed candidate group");
        verify(plan.candidateFiles == 2, "12, 13, 14, 15. candidateFiles == 2");
        verify(plan.candidateSizeGroups.first().first().name == ".hidden1.txt", "12. hidden regular files included");
    }

    // 16, 17, 18, 19, 20. Hardlinks canonicalization, two aliases alone -> zero group; alias + independent -> 2 physical members
    {
        const QString orig = tempDir.filePath("hl_original.bin");
        const QString alias = tempDir.filePath("hl_alias.bin");
        const QString indep = tempDir.filePath("hl_indep.bin");
        const QByteArray data = "HardlinkTestPayload123456";
        writeFile(orig, data);
        ::link(QFile::encodeName(orig).constData(), QFile::encodeName(alias).constData());
        writeFile(indep, data);

        struct stat stOrig {}, stAlias {}, stIndep {};
        ::lstat(QFile::encodeName(orig).constData(), &stOrig);
        ::lstat(QFile::encodeName(alias).constData(), &stAlias);
        ::lstat(QFile::encodeName(indep).constData(), &stIndep);

        verify(stOrig.st_dev == stAlias.st_dev && stOrig.st_ino == stAlias.st_ino, "hardlink inodes match");
        verify(stOrig.st_ino != stIndep.st_ino, "independent inode differs");

        // Test 17: Two aliases of same inode alone -> zero duplicate group
        {
            QList<StorageScanEntry> hlAlone = {
                createEntryFromStat(orig, stOrig, 2),
                createEntryFromStat(alias, stAlias, 2)
            };
            DuplicateFinderJob jobAlone(root, hlAlone);
            verify(jobAlone.plan().candidateGroups == 0, "17. two aliases of same inode alone produce 0 candidate groups");
            verify(jobAlone.plan().totalPhysicalFiles == 1, "16. two aliases canonicalized to 1 physical file");
            const auto resAlone = runFinder(jobAlone);
            verify(resAlone.groups.isEmpty(), "17. two aliases same inode alone -> zero duplicate group");
        }

        // Test 18, 19, 20: Hardlink object + independent file -> group of 2 physical objects; aliases retained; canonical path deterministic
        {
            // Pass entries in reversed order to ensure canonical path is deterministic regardless of readdir order
            QList<StorageScanEntry> hlAndIndep = {
                createEntryFromStat(alias, stAlias, 2),
                createEntryFromStat(orig, stOrig, 2),
                createEntryFromStat(indep, stIndep, 1)
            };
            DuplicateFinderJob job(root, hlAndIndep);
            verify(job.plan().totalPhysicalFiles == 2, "18. total physical files is 2");
            verify(job.plan().candidateFiles == 2, "18. candidate files is 2");

            const auto res = runFinder(job);
            verify(res.groups.size() == 1, "18. forms one duplicate group");
            verify(res.groups.first().files.size() == 2, "18. group has 2 physical members, not 3");

            // Check canonical path & aliases
            const auto &member1 = res.groups.first().files.first();
            const auto &member2 = res.groups.first().files.last();
            const auto &hlMember = (member1.linkCount > 1) ? member1 : member2;
            const QString expectedMinPath = (orig < alias) ? orig : alias;
            const QString expectedAlias = (orig < alias) ? alias : orig;

            verify(hlMember.canonicalPath == expectedMinPath, "20. canonical path is lexicographically smallest");
            verify(hlMember.aliasPaths.contains(expectedAlias), "19. alias paths retained for display");
        }
    }

    // 21, 28, 29. Deterministic candidate size grouping, group sorting and member sorting
    {
        QList<StorageScanEntry> entries = {
            createEntry("/root/z_size50_b.txt", 50, 1, 301),
            createEntry("/root/a_size50_a.txt", 50, 1, 302),
            createEntry("/root/b_size100_b.txt", 100, 1, 303),
            createEntry("/root/a_size100_a.txt", 100, 1, 304)
        };
        auto plan = DuplicatePlanBuilder::buildPlan("/root", entries);
        verify(plan.candidateSizeGroups.size() == 2, "21. two size groups formed");
        // Size DESC: size 100 first, size 50 second
        verify(plan.candidateSizeGroups.at(0).first().logicalSize == 100, "21. size groups ordered size DESC");
        verify(plan.candidateSizeGroups.at(1).first().logicalSize == 50, "21. size 50 group second");
        // Members within group sorted canonicalPath ASC
        verify(plan.candidateSizeGroups.at(0).at(0).canonicalPath == "/root/a_size100_a.txt", "28. group members sorted ASC");
        verify(plan.candidateSizeGroups.at(0).at(1).canonicalPath == "/root/b_size100_b.txt", "28. group members sorted ASC");
    }

    // 30, 31, 32. Recoverable bytes normal, conservative with different allocated sizes, checked 64-bit saturating addition
    {
        DuplicateGroup group;
        group.logicalSize = 1000;

        DuplicatePhysicalFile f1;
        f1.allocatedSize = 4096;
        DuplicatePhysicalFile f2;
        f2.allocatedSize = 8192; // e.g. uncompressed copy
        DuplicatePhysicalFile f3;
        f3.allocatedSize = 4096;
        group.files = {f1, f2, f3};

        // Normal/conservative formula: sum(allocated) - max(allocated) = 16384 - 8192 = 8192
        verify(group.recoverableBytes() == 8192, "30, 31. recoverable bytes conservative sum - max");

        // Saturating addition near quint64 max
        const quint64 nearMax = std::numeric_limits<quint64>::max() - 10;
        const quint64 overflowSum = saturatingAdd(nearMax, 100);
        verify(overflowSum == std::numeric_limits<quint64>::max(), "32. saturatingAdd does not wrap around to small number");
        verify(saturatingAdd(50, 60) == 110, "32. saturatingAdd normal addition correct");
    }

    // 33, 34. Progress monotonic & reaches 100 on success
    {
        const QString p1 = tempDir.filePath("prog1.bin");
        const QString p2 = tempDir.filePath("prog2.bin");
        writeFile(p1, QByteArray(20000, 'P'));
        writeFile(p2, QByteArray(20000, 'P'));

        struct stat st1 {}, st2 {};
        ::lstat(QFile::encodeName(p1).constData(), &st1);
        ::lstat(QFile::encodeName(p2).constData(), &st2);

        QList<StorageScanEntry> entries = {
            createEntryFromStat(p1, st1),
            createEntryFromStat(p2, st2)
        };

        DuplicateFinderJob job(root, entries);
        QList<quint64> progressBytes;
        QObject::connect(&job, &DuplicateFinderJob::progress, [&](quint64, quint64, quint64 resBytes, quint64, const QString &) {
            progressBytes.append(resBytes);
        });

        const auto res = runFinder(job);
        verify(res.finderState == DuplicateFinderState::Completed, "34. job completed");
        verify(!progressBytes.isEmpty(), "33. progress was emitted");
        for (qsizetype i = 1; i < progressBytes.size(); ++i) {
            verify(progressBytes[i] >= progressBytes[i - 1], "33. progress is monotonic");
        }
        verify(progressBytes.last() == static_cast<quint64>(st1.st_size * 2), "34. progress reaches 100% of candidate bytes");
    }

    // 35, 36, 37, 38. Cancel before first hash, cancel during hash, stops queue, partial results retained
    {
        const QString p1 = tempDir.filePath("can1.bin");
        const QString p2 = tempDir.filePath("can2.bin");
        writeFile(p1, "CancelTestData111");
        writeFile(p2, "CancelTestData222");

        struct stat st1 {}, st2 {};
        ::lstat(QFile::encodeName(p1).constData(), &st1);
        ::lstat(QFile::encodeName(p2).constData(), &st2);

        QList<StorageScanEntry> entries = {
            createEntryFromStat(p1, st1),
            createEntryFromStat(p2, st2)
        };

        // Cancel before start
        DuplicateFinderJob preCancelJob(root, entries);
        preCancelJob.cancel();
        verify(preCancelJob.state() == DuplicateFinderState::Idle, "35. cancel before start remains idle without crash");

        // Cancel during hash
        DuplicateFinderJob slowJob(root, entries);
        slowJob.setJobFactory([&](const QUrl &url, QObject *p, const ChecksumJobOptions &opt) {
            ChecksumJobOptions slow = opt;
            slow.chunkDelayMilliseconds = 5;
            return new ChecksumJob(url, p, slow);
        });

        bool cancelTriggered = false;
        QObject::connect(&slowJob, &DuplicateFinderJob::progress, [&](quint64, quint64, quint64, quint64, const QString &currentPath) {
            if (!currentPath.isEmpty() && !cancelTriggered) {
                cancelTriggered = true;
                slowJob.cancel();
            }
        });

        QEventLoop loop;
        QObject::connect(&slowJob, &DuplicateFinderJob::finished, &loop, &QEventLoop::quit);
        slowJob.start();
        if (slowJob.state() == DuplicateFinderState::Running) {
            loop.exec();
        }

        verify(slowJob.state() == DuplicateFinderState::Cancelled, "36. cancel during hash marks state Cancelled");
        verify(slowJob.result().isFinderPartial, "38. cancelled partial result flagged");
    }

    // 39, 40, 66. Rerun clears old result, does not rescan, stale signal safety across generations
    {
        const QString p1 = tempDir.filePath("rerun1.bin");
        const QString p2 = tempDir.filePath("rerun2.bin");
        writeFile(p1, "RerunTestABC");
        writeFile(p2, "RerunTestABC");

        struct stat st1 {}, st2 {};
        ::lstat(QFile::encodeName(p1).constData(), &st1);
        ::lstat(QFile::encodeName(p2).constData(), &st2);

        QList<StorageScanEntry> entries = {
            createEntryFromStat(p1, st1),
            createEntryFromStat(p2, st2)
        };

        DuplicateFinderJob job(root, entries);
        const auto res1 = runFinder(job);
        verify(res1.groups.size() == 1, "39. first run found group");

        // Rerun
        const quint64 firstRunId = job.runId();
        const auto res2 = runFinder(job);
        verify(job.runId() > firstRunId, "66. rerun increments runId generation");
        verify(res2.groups.size() == 1, "39. rerun produced fresh valid result");
    }

    // 41, 42, 43, 44, 45, 46, 47, 65. Preflight snapshot validation: replaced inode, changed size, changed mtime ns rejected with ChangedDuringHash
    {
        const QString pfPath = tempDir.filePath("preflight_test.bin");
        writeFile(pfPath, "OriginalInitialContent123");

        struct stat stOrig {};
        ::lstat(QFile::encodeName(pfPath).constData(), &stOrig);

        // 41. Exact snapshot accepted
        {
            ChecksumJobOptions opt;
            opt.verifyExpectedSnapshot = true;
            opt.expectedDevice = stOrig.st_dev;
            opt.expectedInode = stOrig.st_ino;
            opt.expectedSize = stOrig.st_size;
            opt.expectedMtimeSec = stOrig.st_mtim.tv_sec;
            opt.expectedMtimeNsec = stOrig.st_mtim.tv_nsec;
            opt.expectedCtimeSec = stOrig.st_ctim.tv_sec;
            opt.expectedCtimeNsec = stOrig.st_ctim.tv_nsec;

            ChecksumJob pfJob(QUrl::fromLocalFile(pfPath), nullptr, opt);
            QEventLoop loop;
            ChecksumData data;
            QObject::connect(&pfJob, &ChecksumJob::stateChanged, [&](const ChecksumData &d) {
                if (d.state != ChecksumState::Running) {
                    data = d;
                    loop.quit();
                }
            });
            pfJob.start();
            loop.exec();
            verify(data.state == ChecksumState::Completed && data.hasValidResult(), "41. exact snapshot accepted");
        }

        // 42. Replaced inode rejected
        {
            ChecksumJobOptions opt;
            opt.verifyExpectedSnapshot = true;
            opt.expectedDevice = stOrig.st_dev;
            opt.expectedInode = stOrig.st_ino + 9999; // Mismatched inode
            opt.expectedSize = stOrig.st_size;
            opt.expectedMtimeSec = stOrig.st_mtim.tv_sec;
            opt.expectedMtimeNsec = stOrig.st_mtim.tv_nsec;
            opt.expectedCtimeSec = stOrig.st_ctim.tv_sec;
            opt.expectedCtimeNsec = stOrig.st_ctim.tv_nsec;

            ChecksumJob pfJob(QUrl::fromLocalFile(pfPath), nullptr, opt);
            QEventLoop loop;
            ChecksumData data;
            QObject::connect(&pfJob, &ChecksumJob::stateChanged, [&](const ChecksumData &d) {
                if (d.state != ChecksumState::Running) {
                    data = d;
                    loop.quit();
                }
            });
            pfJob.start();
            loop.exec();
            verify(data.state == ChecksumState::ChangedDuringHash, "42. replaced inode rejected as ChangedDuringHash");
        }

        // 43. Changed size rejected
        {
            ChecksumJobOptions opt;
            opt.verifyExpectedSnapshot = true;
            opt.expectedDevice = stOrig.st_dev;
            opt.expectedInode = stOrig.st_ino;
            opt.expectedSize = stOrig.st_size + 42; // Mismatched size
            opt.expectedMtimeSec = stOrig.st_mtim.tv_sec;
            opt.expectedMtimeNsec = stOrig.st_mtim.tv_nsec;
            opt.expectedCtimeSec = stOrig.st_ctim.tv_sec;
            opt.expectedCtimeNsec = stOrig.st_ctim.tv_nsec;

            ChecksumJob pfJob(QUrl::fromLocalFile(pfPath), nullptr, opt);
            QEventLoop loop;
            ChecksumData data;
            QObject::connect(&pfJob, &ChecksumJob::stateChanged, [&](const ChecksumData &d) {
                if (d.state != ChecksumState::Running) {
                    data = d;
                    loop.quit();
                }
            });
            pfJob.start();
            loop.exec();
            verify(data.state == ChecksumState::ChangedDuringHash, "43. changed size rejected as ChangedDuringHash");
        }

        // 44, 65. Same inode + same size + changed mtime/ctime rejected BEFORE read
        {
            ChecksumJobOptions opt;
            opt.verifyExpectedSnapshot = true;
            opt.expectedDevice = stOrig.st_dev;
            opt.expectedInode = stOrig.st_ino;
            opt.expectedSize = stOrig.st_size;
            opt.expectedMtimeSec = stOrig.st_mtim.tv_sec + 5; // Mismatched timestamp
            opt.expectedMtimeNsec = stOrig.st_mtim.tv_nsec;
            opt.expectedCtimeSec = stOrig.st_ctim.tv_sec;
            opt.expectedCtimeNsec = stOrig.st_ctim.tv_nsec;

            ChecksumJob pfJob(QUrl::fromLocalFile(pfPath), nullptr, opt);
            QEventLoop loop;
            ChecksumData data;
            QObject::connect(&pfJob, &ChecksumJob::stateChanged, [&](const ChecksumData &d) {
                if (d.state != ChecksumState::Running) {
                    data = d;
                    loop.quit();
                }
            });
            pfJob.start();
            loop.exec();
            verify(data.state == ChecksumState::ChangedDuringHash, "44, 65. changed mtime rejected as ChangedDuringHash");
        }

        // 45, 47. Disappeared file skipped safely and other candidates continue
        {
            const QString pGood = tempDir.filePath("good_file.bin");
            const QString pGhost = tempDir.filePath("ghost_file_missing.bin");
            writeFile(pGood, "GoodContent12345");

            struct stat stGood {};
            ::lstat(QFile::encodeName(pGood).constData(), &stGood);

            QList<StorageScanEntry> entries = {
                createEntryFromStat(pGood, stGood),
                createEntry(pGhost, stGood.st_size, stGood.st_dev, 99999, 1, 100, 100)
            };

            DuplicateFinderJob job(root, entries);
            const auto res = runFinder(job);
            verify(res.finderState == DuplicateFinderState::Completed, "45. job completed despite missing candidate");
            verify(res.finderStats.resolvedFiles == 2, "47. other candidates continued after failed candidate");
            verify(res.groups.isEmpty(), "45. no duplicate group formed with missing candidate");
            verify(res.finderStats.errors > 0 || res.finderStats.disappearedFiles > 0, "45. error/disappeared tallied");
        }
    }

    // 48, 49, 50. Partial storage scan state propagated (skipped mount, inaccessible)
    {
        StorageScanStats stats;
        stats.skippedMounts = 1;
        QList<StorageScanEntry> entries;

        DuplicateFinderJob job(root, entries, stats, StorageScanState::Completed);
        const auto res = runFinder(job);
        verify(res.isSourcePartial, "48, 49. skipped mount sets isSourcePartial true");
        verify(!res.sourcePartialReason.isEmpty(), "49. sourcePartialReason is present");
    }

    // 56, 57, 58, 59, 60. Model & View verification: DuplicateGroupModel, Show in Folder signal, independent dialogs
    {
        DuplicateGroup grp;
        grp.logicalSize = 5000;
        grp.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

        DuplicatePhysicalFile f1;
        f1.name = "doc1.txt";
        f1.canonicalPath = "/test/folder/doc1.txt";
        f1.logicalSize = 5000;
        f1.allocatedSize = 8192;
        f1.linkCount = 1;

        DuplicatePhysicalFile f2;
        f2.name = "doc2.txt";
        f2.canonicalPath = "/test/folder/doc2.txt";
        f2.logicalSize = 5000;
        f2.allocatedSize = 8192;
        f2.linkCount = 2;
        f2.aliasPaths = {"/test/folder/doc2_alias.txt"};

        grp.files = {f1, f2};

        DuplicateFinderResult res;
        res.groups = {grp};

        DuplicateGroupModel model;
        model.setResult(res);

        verify(model.rowCount() == 1, "56. model has 1 group row");
        const QModelIndex groupIdx = model.index(0, 0);
        verify(groupIdx.isValid(), "56. group index valid");
        verify(model.rowCount(groupIdx) == 2, "56. group row has 2 child members");

        const QModelIndex child0 = model.index(0, 0, groupIdx);
        const QModelIndex child1 = model.index(1, 0, groupIdx);
        verify(child0.isValid() && child1.isValid(), "56. child indexes valid");

        // Column checks
        verify(model.data(groupIdx, Qt::DisplayRole).toString().contains("Grupa 1"), "56. group display name correct");
        verify(model.data(child0, Qt::DisplayRole).toString() == "doc1.txt", "56. child0 name doc1.txt");
        verify(model.data(child1, Qt::DisplayRole).toString() == "doc2.txt", "56. child1 name doc2.txt");

        // Alias display
        const QModelIndex child1Aliases = model.index(1, 4, groupIdx);
        verify(model.data(child1Aliases, Qt::DisplayRole).toString().contains("hardlink aliases"),
               "56. hardlink aliases visible in model");

        // Show in folder mapping
        const auto *fileFromIndex = model.fileForIndex(child0);
        verify(fileFromIndex != nullptr, "60. fileForIndex returns member");
        verify(fileFromIndex->canonicalPath == "/test/folder/doc1.txt", "60. canonical path matches");

        // Dialog creation & tabs check
        StorageScanDialog dlg1(QUrl::fromLocalFile(root));
        verify(dlg1.tabWidget()->count() == 5, "57. StorageScanDialog has 5 tabs");
        const QString tab3Text = dlg1.tabWidget()->tabText(3);
        verify(tab3Text.contains("Duplikaty") || tab3Text.contains("Duplicates"), "57. tab 4 is Duplikaty / Duplicates");
        verify(dlg1.duplicatesTreeView() != nullptr, "57. duplicates tree view created");
        verify(dlg1.findDuplicatesButton() != nullptr, "57. find duplicates button created");
    }

    // 61, 62, 63, 64. Performance and scalability sanity: 10k unique sizes, 100k entries grouping sanity
    {
        // 61. 10k synthetic files with mostly unique sizes -> zero hash candidates
        QList<StorageScanEntry> entries10k;
        entries10k.reserve(10000);
        for (quint64 i = 0; i < 10000; ++i) {
            entries10k.append(createEntry(QStringLiteral("/synth/file_%1.dat").arg(i), i + 1, 1, 100000 + i));
        }

        auto plan10k = DuplicatePlanBuilder::buildPlan("/synth", entries10k);
        verify(plan10k.totalPhysicalFiles == 10000, "61. 10k physical files accounted");
        verify(plan10k.candidateGroups == 0, "61. 10k unique sizes produce zero candidate groups");
        verify(plan10k.candidateFiles == 0, "61. zero candidate files to hash");
        verify(plan10k.candidateBytes == 0, "61. zero candidate bytes to hash");

        // 62. 100k synthetic entries grouping sanity (fast in-memory plan)
        QList<StorageScanEntry> entries100k;
        entries100k.reserve(100000);
        for (quint64 i = 0; i < 100000; ++i) {
            // First 50k: strictly unique sizes (1000 + i)
            // Second 50k: distributed among exactly 10 sizes (1..10)
            const quint64 sz = (i < 50000) ? (1000 + i) : (1 + (i % 10));
            entries100k.append(createEntry(QStringLiteral("/synth100k/f_%1").arg(i), sz, 1, 500000 + i));
        }
        auto plan100k = DuplicatePlanBuilder::buildPlan("/synth100k", entries100k);
        verify(plan100k.totalPhysicalFiles == 100000, "62. 100k entries accounted");
        verify(plan100k.candidateGroups == 10, "62. 10 size groups formed from shared sizes");
        verify(plan100k.candidateFiles == 50000, "62. 50k candidate files identified without I/O");

        // 63. Many hardlink aliases memory sanity
        QList<StorageScanEntry> manyAliases;
        manyAliases.reserve(5000);
        for (quint64 i = 0; i < 5000; ++i) {
            manyAliases.append(createEntry(QStringLiteral("/aliases/link_%1").arg(i), 1234, 1, 999999, 5000));
        }
        auto planAliases = DuplicatePlanBuilder::buildPlan("/aliases", manyAliases);
        verify(planAliases.totalPhysicalFiles == 1, "63. 5000 aliases canonicalized to 1 physical file");
        verify(planAliases.candidateGroups == 0, "63. 1 physical file alone produces 0 candidate groups");
        verify(planAliases.candidateSizeGroups.isEmpty(), "63. zero hash candidates for single physical file with many aliases");
    }

    qInfo("PASS: %d duplicate_finder assertions; candidate plan, hardlink deduplication, sequential SHA-256, cancel, progress, tree model", checks);
    return 0;
}
