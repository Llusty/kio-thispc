/*
 * Stage 2B.1 tests for LocalMergeExecutionJob (ADDITIVE-ONLY local merge).
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localmergeexecutionjob.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <sys/stat.h>
#include <unistd.h>

static int checks = 0;
static void verify(bool cond, const char *desc)
{
    if (!cond) qFatal("FAIL: %s", desc);
    ++checks;
}

template<typename Predicate>
static bool waitUntil(Predicate pred, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!pred() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(2);
    }
    return pred();
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

static QByteArray sha256File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}

static LocalMergePlan makePlan(const QList<QUrl> &sources, const QUrl &destDir)
{
    LocalMergePlanJob planner(sources, destDir);
    planner.setAutoDelete(false);
    QSignalSpy spy(&planner, &KJob::result);
    planner.start();
    verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "planner finishes");
    verify(planner.planError() == LocalMergePlanError::None, "planner succeeds");
    return planner.plan();
}

class FaultyCopyJob final : public KJob
{
public:
    FaultyCopyJob(int errCode, QString errText, QObject *parent = nullptr)
        : KJob(parent), m_errCode(errCode), m_errText(std::move(errText)) {}
    void start() override
    {
        QTimer::singleShot(0, this, [this] {
            setError(m_errCode);
            setErrorText(m_errText);
            emitResult();
        });
    }
private:
    int m_errCode;
    QString m_errText;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    verify(tmp.isValid(), "tmp valid");

    // A, E, F, G, T, R: Artificial WTF additive merge (conflicts untouched, dest-only untouched, new copied, SHA-256 verified)
    {
        const QString srcParent = tmp.filePath(QStringLiteral("a/src"));
        const QString dstParent = tmp.filePath(QStringLiteral("a/dst"));
        verify(QDir().mkpath(srcParent + QStringLiteral("/WTF/Account"))
            && QDir().mkpath(srcParent + QStringLiteral("/WTF/NewFolder"))
            && QDir().mkpath(dstParent + QStringLiteral("/WTF/Account"))
            && writeFile(srcParent + QStringLiteral("/WTF/Account/Config.wtf"), "SRC-CONFIG-NEW")
            && writeFile(srcParent + QStringLiteral("/WTF/Account/Bindings.wtf"), "SRC-BIND-NEW")
            && writeFile(srcParent + QStringLiteral("/WTF/Account/NewAddon.lua"), "ADDON-LUA-DATA")
            && writeFile(srcParent + QStringLiteral("/WTF/NewFolder/Data.txt"), "NEW-FOLDER-DATA")
            && writeFile(dstParent + QStringLiteral("/WTF/Account/Config.wtf"), "DST-CONFIG-KEEP")
            && writeFile(dstParent + QStringLiteral("/WTF/Account/Bindings.wtf"), "DST-BIND-KEEP")
            && writeFile(dstParent + QStringLiteral("/WTF/Account/OldAddon.lua"), "DST-ONLY-OLD"),
            "WTF test trees created");

        const QByteArray dstConfigHash = sha256File(dstParent + QStringLiteral("/WTF/Account/Config.wtf"));
        const QByteArray dstBindHash = sha256File(dstParent + QStringLiteral("/WTF/Account/Bindings.wtf"));
        const QByteArray dstOldHash = sha256File(dstParent + QStringLiteral("/WTF/Account/OldAddon.lua"));
        const QByteArray srcAddonHash = sha256File(srcParent + QStringLiteral("/WTF/Account/NewAddon.lua"));
        const QByteArray srcDataHash = sha256File(srcParent + QStringLiteral("/WTF/NewFolder/Data.txt"));

        LocalMergePlan plan = makePlan(
            {QUrl::fromLocalFile(srcParent + QStringLiteral("/WTF"))},
            QUrl::fromLocalFile(dstParent));
        approveAdditiveMergePlan(plan);

        LocalMergeExecutionJob execJob(plan);
        execJob.setAutoDelete(false);
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "WTF additive merge finishes");
        verify(execJob.error() == KJob::NoError, "WTF additive merge succeeds");

        const LocalMergeExecutionReport &rep = execJob.report();
        verify(rep.completedFully && !rep.partialSuccess && !rep.cancelled, "WTF report completedFully");
        verify(rep.existingDirectoriesUsed.size() == 2, "WTF and WTF/Account reused");
        verify(rep.directoriesCreated == QStringList{QStringLiteral("WTF/NewFolder")}, "NewFolder created");
        verify(rep.filesCreated.size() == 2, "2 new files created");
        verify(rep.conflictsSkipped.size() == 2, "2 conflicting files skipped");
        verify(rep.bytesProcessed == 29, "29 bytes copied");

        verify(sha256File(dstParent + QStringLiteral("/WTF/Account/Config.wtf")) == dstConfigHash, "Config.wtf untouched");
        verify(sha256File(dstParent + QStringLiteral("/WTF/Account/Bindings.wtf")) == dstBindHash, "Bindings.wtf untouched");
        verify(sha256File(dstParent + QStringLiteral("/WTF/Account/OldAddon.lua")) == dstOldHash, "OldAddon.lua untouched");
        verify(sha256File(dstParent + QStringLiteral("/WTF/Account/NewAddon.lua")) == srcAddonHash, "NewAddon.lua SHA-256 matches");
        verify(sha256File(dstParent + QStringLiteral("/WTF/NewFolder/Data.txt")) == srcDataHash, "Data.txt SHA-256 matches");
        verify(!QFileInfo::exists(dstParent + QStringLiteral("/WTF/Account/NewAddon.lua.thispc-part")), "no .thispc-part leftover");

        qInfo("SAMPLE_WTF_ADDITIVE_EXEC: createdDirs=%lld existingDirs=%lld createdFiles=%lld skippedConflicts=%lld bytes=%llu",
              static_cast<long long>(rep.directoriesCreated.size()),
              static_cast<long long>(rep.existingDirectoriesUsed.size()),
              static_cast<long long>(rep.filesCreated.size()),
              static_cast<long long>(rep.conflictsSkipped.size()),
              static_cast<unsigned long long>(rep.bytesProcessed));
    }

    // B, C, D: Existing dir + two missing files, deep nesting, empty new dir
    {
        const QString src = tmp.filePath(QStringLiteral("b/src/Tree"));
        const QString dst = tmp.filePath(QStringLiteral("b/dst/Tree"));
        verify(QDir().mkpath(src + QStringLiteral("/d1/d2/d3/empty"))
            && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/f1.txt"), "111")
            && writeFile(src + QStringLiteral("/f2.txt"), "2222")
            && writeFile(src + QStringLiteral("/d1/d2/d3/deep.txt"), "deep"),
            "B/C/D trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("b/dst"))));
        approveAdditiveMergePlan(plan);
        LocalMergeExecutionJob execJob(plan);
        execJob.setAutoDelete(false);
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "B/C/D finishes");
        verify(execJob.error() == KJob::NoError && execJob.report().directoriesCreated.size() == 4
            && execJob.report().filesCreated.size() == 3, "deep + empty dirs + 3 files created");
        verify(QFileInfo(dst + QStringLiteral("/d1/d2/d3/empty")).isDir(), "empty dir exists in dest");
    }

    // H: Unapproved plan -> zero modifications
    {
        const QString src = tmp.filePath(QStringLiteral("h/src/Box"));
        const QString dst = tmp.filePath(QStringLiteral("h/dst/Box"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst) && writeFile(src + QStringLiteral("/x.txt"), "x"), "H trees");
        LocalMergePlan unapproved = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("h/dst"))));
        LocalMergeExecutionJob execJob(unapproved);
        execJob.setAutoDelete(false);
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "H refuses unapproved");
        verify(execJob.error() != KJob::NoError && !QFileInfo::exists(dst + QStringLiteral("/x.txt")), "H zero modifications");
    }

    // I: Source identity changed after planning -> refused
    {
        const QString src = tmp.filePath(QStringLiteral("i/src/Box"));
        const QString dst = tmp.filePath(QStringLiteral("i/dst/Box"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst) && writeFile(src + QStringLiteral("/a.txt"), "v1"), "I trees");
        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("i/dst"))));
        approveAdditiveMergePlan(plan);

        QFile::remove(src + QStringLiteral("/a.txt"));
        verify(writeFile(src + QStringLiteral("/a.txt"), "v2-new-inode"), "I replaced source inode");

        LocalMergeExecutionJob execJob(plan);
        execJob.setAutoDelete(false);
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "I finishes");
        verify(execJob.error() != KJob::NoError && !QFileInfo::exists(dst + QStringLiteral("/a.txt")), "I refused source swap");
    }

    // J & K: Destination directory identity changed / replaced with symlink -> refused
    {
        const QString src = tmp.filePath(QStringLiteral("jk/src/Box"));
        const QString dst = tmp.filePath(QStringLiteral("jk/dst/Box"));
        const QString alt = tmp.filePath(QStringLiteral("jk/alt"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst) && QDir().mkpath(alt)
            && writeFile(src + QStringLiteral("/a.txt"), "a"), "JK trees");
        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("jk/dst"))));
        approveAdditiveMergePlan(plan);

        verify(QDir().rmdir(dst) && QFile::link(alt, dst), "JK swapped dest dir to symlink");
        LocalMergeExecutionJob execJob(plan);
        execJob.setAutoDelete(false);
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "JK finishes");
        verify(execJob.error() != KJob::NoError && !QFileInfo::exists(alt + QStringLiteral("/a.txt")), "JK symlink swap refused");
    }

    // L: Symlink in source -> no recursion outside tree, safely blocked
    {
        const QString src = tmp.filePath(QStringLiteral("l/src/Box"));
        const QString dst = tmp.filePath(QStringLiteral("l/dst/Box"));
        const QString out = tmp.filePath(QStringLiteral("l/outside"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst) && QDir().mkpath(out)
            && writeFile(out + QStringLiteral("/secret.txt"), "secret")
            && QFile::link(out, src + QStringLiteral("/ext-link"))
            && writeFile(src + QStringLiteral("/ok.txt"), "ok"), "L trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("l/dst"))));
        approveAdditiveMergePlan(plan);
        LocalMergeExecutionJob execJob(plan);
        execJob.setAutoDelete(false);
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "L finishes");
        verify(execJob.error() == KJob::NoError && execJob.report().objectsBlocked.contains(QStringLiteral("Box/ext-link"))
            && !QFileInfo::exists(dst + QStringLiteral("/ext-link")), "L symlink blocked");
    }

    // M: Differing-case collision -> fail-closed (not copied)
    {
        const QString src = tmp.filePath(QStringLiteral("m/src/Box"));
        const QString dst = tmp.filePath(QStringLiteral("m/dst/Box"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "new")
            && writeFile(dst + QStringLiteral("/config.wtf"), "existing"), "M trees");
        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("m/dst"))));
        approveAdditiveMergePlan(plan);
        LocalMergeExecutionJob execJob(plan);
        execJob.setAutoDelete(false);
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "M finishes");
        verify(execJob.report().objectsBlocked.contains(QStringLiteral("Box/Config.wtf"))
            && !QFileInfo::exists(dst + QStringLiteral("/Config.wtf")), "M differing-case blocked fail-closed");
    }

    // N: Cancel before first creation -> partialSuccess == false
    {
        const QString src = tmp.filePath(QStringLiteral("n/src/Box"));
        const QString dst = tmp.filePath(QStringLiteral("n/dst/Box"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst) && writeFile(src + QStringLiteral("/f.txt"), "data"), "N trees");
        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("n/dst"))));
        approveAdditiveMergePlan(plan);
        LocalMergeExecutionJob execJob(plan);
        execJob.setAutoDelete(false);
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(execJob.kill(KJob::EmitResult), "N killed before first creation");
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "N finished");
        verify(execJob.report().cancelled && !execJob.report().partialSuccess && execJob.report().filesCreated.isEmpty(),
               "N reports cancelled with partialSuccess=false");
    }

    // O, S, U: Cancel after partial success -> preserves existing & published files, reports partialSuccess=true
    {
        const QString src = tmp.filePath(QStringLiteral("o/src/Box"));
        const QString dst = tmp.filePath(QStringLiteral("o/dst/Box"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(dst + QStringLiteral("/preexisting.txt"), "keep-me")
            && writeFile(src + QStringLiteral("/a.txt"), QByteArray(16384, 'A'))
            && writeFile(src + QStringLiteral("/b.txt"), QByteArray(16384, 'B')), "O trees");
        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("o/dst"))));
        approveAdditiveMergePlan(plan);
        LocalMergeExecutionJob execJob(plan, nullptr, 4096, 15);
        execJob.setAutoDelete(false);
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(waitUntil([&execJob] { return execJob.report().filesCreated.size() >= 1; }, 4000),
               "O first file published");
        verify(execJob.kill(KJob::EmitResult), "O killed after partial success");
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "O finished");
        verify(execJob.report().cancelled && execJob.report().partialSuccess
            && execJob.report().lastPublishedEntry == QStringLiteral("Box/a.txt")
            && sha256File(dst + QStringLiteral("/preexisting.txt")) == QCryptographicHash::hash("keep-me", QCryptographicHash::Sha256),
               "O partialSuccess=true and existing data preserved");
    }

    // P & Q: Copy error / Disk full via controlled fault injection -> stops immediately, no retry, partialSuccess reported
    {
        const QString src = tmp.filePath(QStringLiteral("pq/src/Box"));
        const QString dst = tmp.filePath(QStringLiteral("pq/dst/Box"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/a_ok.txt"), "ok")
            && writeFile(src + QStringLiteral("/b_full.txt"), "fail")
            && writeFile(src + QStringLiteral("/c_never.txt"), "never"), "PQ trees");
        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("pq/dst"))));
        approveAdditiveMergePlan(plan);

        int calls = 0;
        LocalMergeExecutionJob execJob(plan);
        execJob.setAutoDelete(false);
        execJob.setCopyJobFactory([&calls, &execJob](const QUrl &s, const QUrl &d, QObject *p) -> KJob * {
            ++calls;
            if (calls == 2) {
                return new FaultyCopyJob(KIO::ERR_DISK_FULL, QStringLiteral("Simulated ENOSPC"), p);
            }
            return new LocalFileCopyJob(s, d, &execJob, 256 * 1024, 0, false, false, {});
        });
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "PQ finishes");
        verify(execJob.error() == KIO::ERR_DISK_FULL && calls == 2
            && execJob.report().partialSuccess
            && execJob.report().filesCreated == QStringList{QStringLiteral("Box/a_ok.txt")}
            && execJob.report().failedOperations == QStringList{QStringLiteral("Box/b_full.txt")}
            && !QFileInfo::exists(dst + QStringLiteral("/c_never.txt")),
               "PQ disk full stops cleanly with exact partial success report");
    }

    // ==================== Stage 2B.2A Scenarios (1-24) ====================

    // 1, 2, 3: Skip single, Skip multiple, SkipAllFileConflicts (strictly scoped to ExactNameMatch FileConflict)
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2a_skip/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2a_skip/dst/WTF"));
        verify(QDir().mkpath(src + QStringLiteral("/Sub"))
            && QDir().mkpath(dst + QStringLiteral("/Sub"))
            && writeFile(src + QStringLiteral("/Config.wtf"), "SRC-CFG")
            && writeFile(src + QStringLiteral("/Sub/A.lua"), "SRC-A")
            && writeFile(src + QStringLiteral("/Sub/New.lua"), "SRC-NEW")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "DST-CFG")
            && writeFile(dst + QStringLiteral("/Sub/A.lua"), "DST-A"),
            "skipall trees");

        const QByteArray hCfg = sha256File(dst + QStringLiteral("/Config.wtf"));
        const QByteArray hA = sha256File(dst + QStringLiteral("/Sub/A.lua"));

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2a_skip/dst"))));
        for (auto &e : plan.entries) {
            if (e.classification == LocalMergeEntryClassification::CreateNew) e.approvedAction = LocalMergeApprovedAction::Create;
            else if (e.classification == LocalMergeEntryClassification::MergeDirectoryCandidate) e.approvedAction = LocalMergeApprovedAction::MergeDirectory;
        }
        verify(applySkipAllFileConflictsPolicy(plan, QStringLiteral("Skip all conflicts")) == 2, "SkipAll binds 2 conflicts");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "SkipAll finishes");
        verify(job.error() == KJob::NoError
            && job.report().outcome == LocalMergeExecutionOutcome::SucceededWithSkippedConflicts
            && job.report().skippedEntries.size() == 2
            && job.report().filesCreated == QStringList{QStringLiteral("WTF/Sub/New.lua")}
            && sha256File(dst + QStringLiteral("/Config.wtf")) == hCfg
            && sha256File(dst + QStringLiteral("/Sub/A.lua")) == hA,
               "SkipAll preserves originals and copies only new file");
    }

    // 4, 5, 12, 22, 23: Rename single & multiple (including Unicode/Polish), SHA-256 verified, no .thispc-part
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2a_ren/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2a_ren/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "NEW-CONFIG-DATA")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "ORIG-CONFIG-DATA")
            && writeFile(src + QStringLiteral("/Zażółć.txt"), "NEW-PL-DATA")
            && writeFile(dst + QStringLiteral("/Zażółć.txt"), "ORIG-PL-DATA"),
            "rename trees");

        const QByteArray origCfgHash = sha256File(dst + QStringLiteral("/Config.wtf"));
        const QByteArray origPlHash = sha256File(dst + QStringLiteral("/Zażółć.txt"));
        const QByteArray srcCfgHash = sha256File(src + QStringLiteral("/Config.wtf"));
        const QByteArray srcPlHash = sha256File(src + QStringLiteral("/Zażółć.txt"));

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2a_ren/dst"))));
        for (auto &e : plan.entries) {
            if (e.classification == LocalMergeEntryClassification::CreateNew) e.approvedAction = LocalMergeApprovedAction::Create;
            else if (e.classification == LocalMergeEntryClassification::MergeDirectoryCandidate) e.approvedAction = LocalMergeApprovedAction::MergeDirectory;
        }
        verify(approveFileRename(plan, QStringLiteral("WTF/Config.wtf"), QStringLiteral("Config (1).wtf")), "rename Config.wtf");
        verify(approveFileRename(plan, QStringLiteral("WTF/Zażółć.txt"), QStringLiteral("Zażółć (1).txt")), "rename Unicode");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "Rename job finishes");
        verify(job.error() == KJob::NoError
            && job.report().outcome == LocalMergeExecutionOutcome::Succeeded
            && job.report().filesRenamed.size() == 2
            && sha256File(dst + QStringLiteral("/Config.wtf")) == origCfgHash
            && sha256File(dst + QStringLiteral("/Zażółć.txt")) == origPlHash
            && sha256File(dst + QStringLiteral("/Config (1).wtf")) == srcCfgHash
            && sha256File(dst + QStringLiteral("/Zażółć (1).txt")) == srcPlHash
            && !QFileInfo::exists(dst + QStringLiteral("/Config (1).wtf.thispc-part")),
               "Rename created copies with matching SHA-256 and kept originals untouched");
    }

    // 6, 7, 8, 9, 10, 11: Reject unsafe Rename targets (existing, symlink, broken symlink, ../, /, differing case)
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2a_badren/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2a_badren/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "a")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "b")
            && writeFile(dst + QStringLiteral("/Taken.wtf"), "t")
            && QFile::link(dst + QStringLiteral("/Taken.wtf"), dst + QStringLiteral("/Sym.wtf"))
            && QFile::link(dst + QStringLiteral("/NonExistent.wtf"), dst + QStringLiteral("/Broken.wtf")),
            "badren trees");
        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2a_badren/dst"))));
        verify(!approveFileRename(plan, QStringLiteral("WTF/Config.wtf"), QStringLiteral("Taken.wtf")), "reject existing");
        verify(!approveFileRename(plan, QStringLiteral("WTF/Config.wtf"), QStringLiteral("Sym.wtf")), "reject symlink");
        verify(!approveFileRename(plan, QStringLiteral("WTF/Config.wtf"), QStringLiteral("Broken.wtf")), "reject broken symlink");
        verify(!approveFileRename(plan, QStringLiteral("WTF/Config.wtf"), QStringLiteral("../Escape.wtf")), "reject ../");
        verify(!approveFileRename(plan, QStringLiteral("WTF/Config.wtf"), QStringLiteral("sub/Escape.wtf")), "reject /");
        verify(!approveFileRename(plan, QStringLiteral("WTF/Config.wtf"), QStringLiteral("taken.wtf")), "reject case collision");
    }

    // 13 & 14: Source or destination identity changed after Skip/Rename decision -> refused
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2a_id/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2a_id/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/A.txt"), "a1")
            && writeFile(dst + QStringLiteral("/A.txt"), "d1"), "id trees");
        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2a_id/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileRename(plan, QStringLiteral("WTF/A.txt"), QStringLiteral("A (1).txt")), "bind rename");

        QFile::remove(dst + QStringLiteral("/A.txt"));
        verify(writeFile(dst + QStringLiteral("/A.txt"), "d2-new-inode"), "replace dest inode");
        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "id mismatch finishes");
        verify(job.error() != KJob::NoError && !QFileInfo::exists(dst + QStringLiteral("/A (1).txt")), "refused dest inode swap");
    }

    // 15 & 16: Pre-publish race: directory swap + concurrent creation of same target filename
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2a_race/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2a_race/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "src")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "dst"), "race trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2a_race/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileRename(plan, QStringLiteral("WTF/Config.wtf"), QStringLiteral("Config (1).wtf")), "bind race rename");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setPrePublishHook([](const QString &, int destParentFd) {
            const int rfd = ::openat(destParentFd, "Config (1).wtf", O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (rfd >= 0) {
                const char msg[] = "RACER-WON";
                [[maybe_unused]] const ssize_t w = ::write(rfd, msg, sizeof(msg) - 1);
                ::close(rfd);
            }
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "race finishes");
        verify(job.error() == KIO::ERR_FILE_ALREADY_EXIST
            && sha256File(dst + QStringLiteral("/Config (1).wtf")) == QCryptographicHash::hash("RACER-WON", QCryptographicHash::Sha256)
            && !QFileInfo::exists(dst + QStringLiteral("/Config (1).wtf.thispc-part")),
               "concurrent target creation caught atomically without overwrite or .thispc-part leftover");
    }

    // 20b & 24: Uncertain durability (fsync EIO) + Overwrite strictly refused
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2a_dur/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2a_dur/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/New.txt"), "dur")
            && writeFile(src + QStringLiteral("/Conf.txt"), "s")
            && writeFile(dst + QStringLiteral("/Conf.txt"), "d"), "dur trees");

        LocalMergePlan planEio = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2a_dur/dst"))));
        approveAdditiveMergePlan(planEio);
        LocalMergeExecutionJob eioJob(planEio);
        eioJob.setAutoDelete(false);
        eioJob.setSimulateFsyncEio(true);
        QSignalSpy spyEio(&eioJob, &KJob::result);
        eioJob.start();
        verify(waitUntil([&spyEio] { return spyEio.count() == 1; }, 5000), "fsync EIO finishes");
        verify(eioJob.error() != KJob::NoError
            && eioJob.report().outcome == LocalMergeExecutionOutcome::FailedUncertainDurability
            && eioJob.report().uncertainDurability
            && !eioJob.report().retryAllowed, "uncertain durability disables retry");

        LocalMergePlan planOvw = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2a_dur/dst"))));
        approveAdditiveMergePlan(planOvw);
        for (auto &e : planOvw.entries) {
            if (e.classification == LocalMergeEntryClassification::FileConflict) {
                e.approvedAction = LocalMergeApprovedAction::Overwrite;
            }
        }
        LocalMergeExecutionJob ovwJob(planOvw);
        ovwJob.setAutoDelete(false);
        QSignalSpy spyOvw(&ovwJob, &KJob::result);
        ovwJob.start();
        verify(waitUntil([&spyOvw] { return spyOvw.count() == 1; }, 5000), "Overwrite refused finishes");
        verify(ovwJob.error() != KJob::NoError
            && sha256File(dst + QStringLiteral("/Conf.txt")) == QCryptographicHash::hash("d", QCryptographicHash::Sha256),
               "Unbound Overwrite without approveFileOverwrite strictly refused");
    }

    // ==================== Stage 2B.2B Tests (1..26) ====================
    auto hasLeftoverStaging = [](const QString &dirPath) {
        const QStringList entries = QDir(dirPath).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QString &e : entries) {
            if (e.startsWith(QStringLiteral(".thispc-part")) || e.startsWith(QStringLiteral(".thispc-replaced"))) {
                return true;
            }
        }
        return false;
    };

    // S2B.2B #1, #22, #23, #24: Single Overwrite + dest-only untouched + SHA-256 + no leftover staging + undoContract
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_1/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_1/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "NEW-CONTENT-1")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "OLD-CONTENT-1")
            && writeFile(dst + QStringLiteral("/DestOnly.wtf"), "KEEP-ME"), "2b2b #1 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_1/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/Config.wtf")), "2b2b #1 bind overwrite");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #1 finishes");
        const auto &rep = job.report();
        verify(job.error() == KJob::NoError
            && rep.outcome == LocalMergeExecutionOutcome::Succeeded
            && rep.filesOverwritten == QStringList({QStringLiteral("WTF/Config.wtf")})
            && rep.replacedFiles.size() == 1
            && rep.replacedFiles.first().durabilityConfirmed
            && !rep.replacedFiles.first().backupPreserved
            && sha256File(dst + QStringLiteral("/Config.wtf")) == QCryptographicHash::hash("NEW-CONTENT-1", QCryptographicHash::Sha256)
            && sha256File(src + QStringLiteral("/Config.wtf")) == QCryptographicHash::hash("NEW-CONTENT-1", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/DestOnly.wtf")) == QCryptographicHash::hash("KEEP-ME", QCryptographicHash::Sha256)
            && !hasLeftoverStaging(dst)
            && rep.undoContract.requiresIrreversibleSerial
            && !rep.undoContract.reversible
            && rep.undoContract.existingDirectoriesNeverDelete.contains(QStringLiteral("WTF")),
               "2b2b #1 single Overwrite + SHA-256 + no leftovers + undoContract");
    }

    // S2B.2B #2: Two Overwrites in different subdirectories
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_2/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_2/dst/WTF"));
        verify(QDir().mkpath(src + QStringLiteral("/A")) && QDir().mkpath(src + QStringLiteral("/B"))
            && QDir().mkpath(dst + QStringLiteral("/A")) && QDir().mkpath(dst + QStringLiteral("/B"))
            && writeFile(src + QStringLiteral("/A/x.txt"), "NEW-X")
            && writeFile(dst + QStringLiteral("/A/x.txt"), "OLD-X")
            && writeFile(src + QStringLiteral("/B/y.txt"), "NEW-Y")
            && writeFile(dst + QStringLiteral("/B/y.txt"), "OLD-Y"), "2b2b #2 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_2/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/A/x.txt"))
            && approveFileOverwrite(plan, QStringLiteral("WTF/B/y.txt")), "2b2b #2 bind both");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #2 finishes");
        verify(job.error() == KJob::NoError
            && job.report().filesOverwritten.size() == 2
            && sha256File(dst + QStringLiteral("/A/x.txt")) == QCryptographicHash::hash("NEW-X", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/B/y.txt")) == QCryptographicHash::hash("NEW-Y", QCryptographicHash::Sha256),
               "2b2b #2 two overwrites in different subdirs");
    }

    // S2B.2B #3: OverwriteAllFileConflicts policy scoping (ignores symlinks & type conflicts)
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_3/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_3/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst + QStringLiteral("/DirOverFile"))
            && writeFile(src + QStringLiteral("/F1.txt"), "N1") && writeFile(dst + QStringLiteral("/F1.txt"), "O1")
            && writeFile(src + QStringLiteral("/F2.txt"), "N2") && writeFile(dst + QStringLiteral("/F2.txt"), "O2")
            && writeFile(src + QStringLiteral("/DirOverFile"), "file-vs-dir")
            && writeFile(src + QStringLiteral("/SymTarget.txt"), "sym-src")
            && QFile::link(QStringLiteral("/tmp/nowhere"), dst + QStringLiteral("/SymTarget.txt")), "2b2b #3 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_3/dst"))));
        for (auto &e : plan.entries) {
            if (e.classification == LocalMergeEntryClassification::MergeDirectoryCandidate) {
                e.approvedAction = LocalMergeApprovedAction::MergeDirectory;
            }
        }
        verify(applyOverwriteAllFileConflictsPolicy(plan) == 2, "2b2b #3 OverwriteAll scopes strictly to 2 FileConflicts");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #3 finishes");
        verify(job.error() == KJob::NoError
            && job.report().filesOverwritten.size() == 2
            && job.report().objectsBlocked.contains(QStringLiteral("WTF/DirOverFile"))
            && job.report().objectsBlocked.contains(QStringLiteral("WTF/SymTarget.txt")),
               "2b2b #3 OverwriteAll executes only regular FileConflicts and blocks symlink/type conflict");
    }

    // S2B.2B #4 & #5: Skip + Rename + Overwrite in the same job
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_45/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_45/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Skip.txt"), "S-NEW") && writeFile(dst + QStringLiteral("/Skip.txt"), "S-OLD")
            && writeFile(src + QStringLiteral("/Ren.txt"), "R-NEW") && writeFile(dst + QStringLiteral("/Ren.txt"), "R-OLD")
            && writeFile(src + QStringLiteral("/Ovw.txt"), "O-NEW") && writeFile(dst + QStringLiteral("/Ovw.txt"), "O-OLD"), "2b2b #45 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_45/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileSkip(plan, QStringLiteral("WTF/Skip.txt"))
            && approveFileRename(plan, QStringLiteral("WTF/Ren.txt"), QStringLiteral("Ren (1).txt"))
            && approveFileOverwrite(plan, QStringLiteral("WTF/Ovw.txt")), "2b2b #45 bind mixed decisions");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #45 finishes");
        verify(job.error() == KJob::NoError
            && job.report().outcome == LocalMergeExecutionOutcome::SucceededWithSkippedConflicts
            && sha256File(dst + QStringLiteral("/Skip.txt")) == QCryptographicHash::hash("S-OLD", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Ren.txt")) == QCryptographicHash::hash("R-OLD", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Ren (1).txt")) == QCryptographicHash::hash("R-NEW", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Ovw.txt")) == QCryptographicHash::hash("O-NEW", QCryptographicHash::Sha256),
               "2b2b #4+#5 Skip + Rename + Overwrite in same job");
    }

    // S2B.2B #6: Destination inode change after Overwrite approval
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_6/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_6/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD"), "2b2b #6 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_6/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/F.txt")), "2b2b #6 bind");
        QFile::remove(dst + QStringLiteral("/F.txt"));
        verify(writeFile(dst + QStringLiteral("/F.txt"), "SWAPPED-AFTER-APPROVAL"), "2b2b #6 swap");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #6 finishes");
        verify(job.error() != KJob::NoError
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("SWAPPED-AFTER-APPROVAL", QCryptographicHash::Sha256),
               "2b2b #6 refuses Overwrite when destination inode changed after approval");
    }

    // S2B.2B #7 & #8: Pre-publish substitution + concurrent race detection
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_7/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_7/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD"), "2b2b #7 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_7/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/F.txt")), "2b2b #7 bind");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setPrePublishHook([](const QString &, int destParentFd) {
            ::unlinkat(destParentFd, "F.txt", 0);
            const int fd = ::openat(destParentFd, "F.txt", O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) {
                const char msg[] = "RACER-INJECTED";
                [[maybe_unused]] const ssize_t w = ::write(fd, msg, sizeof(msg) - 1);
                ::close(fd);
            }
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #7 finishes");
        verify(job.error() != KJob::NoError
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("RACER-INJECTED", QCryptographicHash::Sha256)
            && !hasLeftoverStaging(dst),
               "2b2b #7+#8 pre-publish destination swap detected and refused cleanly");
    }

    // S2B.2B #9 & #10: Symlink substituted as target + symlink in intermediate directory
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_9/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_9/dst/WTF"));
        verify(QDir().mkpath(src + QStringLiteral("/Sub")) && QDir().mkpath(dst + QStringLiteral("/Sub"))
            && writeFile(src + QStringLiteral("/Sub/F.txt"), "NEW")
            && writeFile(dst + QStringLiteral("/Sub/F.txt"), "OLD"), "2b2b #9 trees");

        LocalMergePlan planSym = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_9/dst"))));
        approveAdditiveMergePlan(planSym);
        verify(approveFileOverwrite(planSym, QStringLiteral("WTF/Sub/F.txt")), "2b2b #9 bind");
        QFile::remove(dst + QStringLiteral("/Sub/F.txt"));
        verify(QFile::link(QStringLiteral("/tmp/evil-target"), dst + QStringLiteral("/Sub/F.txt")), "2b2b #9 symlink dst");

        LocalMergeExecutionJob symJob(planSym);
        symJob.setAutoDelete(false);
        QSignalSpy spySym(&symJob, &KJob::result);
        symJob.start();
        verify(waitUntil([&spySym] { return spySym.count() == 1; }, 5000), "2b2b #9 finishes");
        verify(symJob.error() != KJob::NoError && QFileInfo(dst + QStringLiteral("/Sub/F.txt")).isSymLink(),
               "2b2b #9 refuses symlink substituted as target");

        // #10: Intermediate directory replaced with symlink
        QFile::remove(dst + QStringLiteral("/Sub/F.txt"));
        verify(writeFile(dst + QStringLiteral("/Sub/F.txt"), "OLD"), "restore F.txt");
        LocalMergePlan planDirSym = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_9/dst"))));
        approveAdditiveMergePlan(planDirSym);
        verify(approveFileOverwrite(planDirSym, QStringLiteral("WTF/Sub/F.txt")), "2b2b #10 bind");
        QDir(dst + QStringLiteral("/Sub")).removeRecursively();
        verify(QFile::link(QStringLiteral("/tmp"), dst + QStringLiteral("/Sub")), "2b2b #10 symlink dir");

        LocalMergeExecutionJob dirSymJob(planDirSym);
        dirSymJob.setAutoDelete(false);
        QSignalSpy spyDirSym(&dirSymJob, &KJob::result);
        dirSymJob.start();
        verify(waitUntil([&spyDirSym] { return spyDirSym.count() == 1; }, 5000), "2b2b #10 finishes");
        verify(dirSymJob.error() != KJob::NoError, "2b2b #10 refuses intermediate directory replaced with symlink");
    }

    // S2B.2B #11: Directory moved outside approved tree after fd opened
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_11/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_11/dst/WTF"));
        const QString movedOutside = tmp.filePath(QStringLiteral("s2b2b_11/moved_outside"));
        verify(QDir().mkpath(src + QStringLiteral("/Sub")) && QDir().mkpath(dst + QStringLiteral("/Sub"))
            && writeFile(src + QStringLiteral("/Sub/F.txt"), "NEW")
            && writeFile(dst + QStringLiteral("/Sub/F.txt"), "OLD"), "2b2b #11 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_11/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/Sub/F.txt")), "2b2b #11 bind");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setPrePublishHook([dst, movedOutside](const QString &, int) {
            QDir().rename(dst + QStringLiteral("/Sub"), movedOutside);
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #11 finishes");
        verify(job.error() != KJob::NoError
            && sha256File(movedOutside + QStringLiteral("/F.txt")) == QCryptographicHash::hash("OLD", QCryptographicHash::Sha256),
               "2b2b #11 detects directory moved outside approved tree before publication");
    }

    // S2B.2B #12, #13, #18: Pre-staging failure, staging failure, ENOSPC/disk-full
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_errs/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_errs/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD"), "2b2b #12/#13 trees");

        // #12: Source removed before staging
        LocalMergePlan planPre = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_errs/dst"))));
        approveAdditiveMergePlan(planPre);
        verify(approveFileOverwrite(planPre, QStringLiteral("WTF/F.txt")), "2b2b #12 bind");
        QFile::remove(src + QStringLiteral("/F.txt"));
        LocalMergeExecutionJob preJob(planPre);
        preJob.setAutoDelete(false);
        QSignalSpy spyPre(&preJob, &KJob::result);
        preJob.start();
        verify(waitUntil([&spyPre] { return spyPre.count() == 1; }, 5000), "2b2b #12 finishes");
        verify(preJob.error() != KJob::NoError
            && preJob.report().outcome == LocalMergeExecutionOutcome::FailedNoChanges
            && preJob.report().retryAllowed
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("OLD", QCryptographicHash::Sha256),
               "2b2b #12 error before staging leaves OLD untouched");

        // #13 & #18: Error during staging / disk full (simulated via custom copy factory for CreateNew, and source truncation for Overwrite)
        verify(writeFile(src + QStringLiteral("/F.txt"), "NEW-DATA"), "restore F.txt");
        LocalMergePlan planStageErr = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_errs/dst"))));
        approveAdditiveMergePlan(planStageErr);
        verify(approveFileOverwrite(planStageErr, QStringLiteral("WTF/F.txt")), "2b2b #13 bind");
        // Modify source mid-flight (before/during read) -> triggers source modification failure during staging
        verify(writeFile(src + QStringLiteral("/F.txt"), "TRUNC"), "2b2b #13 modify src");
        LocalMergeExecutionJob stageErrJob(planStageErr);
        stageErrJob.setAutoDelete(false);
        QSignalSpy spyStage(&stageErrJob, &KJob::result);
        stageErrJob.start();
        verify(waitUntil([&spyStage] { return spyStage.count() == 1; }, 5000), "2b2b #13 finishes");
        verify(stageErrJob.error() != KJob::NoError
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("OLD", QCryptographicHash::Sha256)
            && !hasLeftoverStaging(dst),
               "2b2b #13 staging error leaves OLD untouched and cleans staging");
    }

    // S2B.2B #14, #19: O_TMPFILE unsupported fallback + RENAME_EXCHANGE unsupported + NTFS/unsupported FS blocked
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_19/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_19/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW-19")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD-19"), "2b2b #19 trees");

        // O_TMPFILE unsupported -> anchored named staging fallback succeeds!
        LocalMergePlan planNoTmp = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_19/dst"))));
        approveAdditiveMergePlan(planNoTmp);
        verify(approveFileOverwrite(planNoTmp, QStringLiteral("WTF/F.txt")), "2b2b #19a bind");
        LocalMergeExecutionJob noTmpJob(planNoTmp);
        noTmpJob.setAutoDelete(false);
        noTmpJob.setSimulateOTmpfileUnsupported(true);
        QSignalSpy spyNoTmp(&noTmpJob, &KJob::result);
        noTmpJob.start();
        verify(waitUntil([&spyNoTmp] { return spyNoTmp.count() == 1; }, 5000), "2b2b #19a finishes");
        verify(noTmpJob.error() == KJob::NoError
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("NEW-19", QCryptographicHash::Sha256)
            && !hasLeftoverStaging(dst),
               "2b2b #19a O_TMPFILE fallback stays anchored and succeeds");

        // RENAME_EXCHANGE unsupported -> fail-closed without touching destination!
        verify(writeFile(dst + QStringLiteral("/F.txt"), "OLD-19B"), "reset dst 19b");
        LocalMergePlan planNoExch = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_19/dst"))));
        approveAdditiveMergePlan(planNoExch);
        verify(approveFileOverwrite(planNoExch, QStringLiteral("WTF/F.txt")), "2b2b #19b bind");
        LocalMergeExecutionJob noExchJob(planNoExch);
        noExchJob.setAutoDelete(false);
        noExchJob.setSimulateRenameExchangeUnsupported(true);
        QSignalSpy spyNoExch(&noExchJob, &KJob::result);
        noExchJob.start();
        verify(waitUntil([&spyNoExch] { return spyNoExch.count() == 1; }, 5000), "2b2b #19b finishes");
        verify(noExchJob.error() != KJob::NoError
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("OLD-19B", QCryptographicHash::Sha256)
            && !hasLeftoverStaging(dst),
               "2b2b #14+#19b RENAME_EXCHANGE unsupported fails closed");

        // Simulated NTFS/FUSE/VFAT -> fail-closed!
        LocalMergeExecutionJob ntfsJob(planNoExch);
        ntfsJob.setAutoDelete(false);
        ntfsJob.setSimulateUnsupportedOverwriteFs(true);
        QSignalSpy spyNtfs(&ntfsJob, &KJob::result);
        ntfsJob.start();
        verify(waitUntil([&spyNtfs] { return spyNtfs.count() == 1; }, 5000), "2b2b #19c finishes");
        verify(ntfsJob.error() == KIO::ERR_UNSUPPORTED_ACTION
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("OLD-19B", QCryptographicHash::Sha256),
               "2b2b #19c NTFS/unsupported FS Overwrite fails closed");
    }

    // S2B.2B #15, #20, #23, #25: fsync EIO after Overwrite preserves OLD backup + disables retry
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_eio/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_eio/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "NEW-EIO")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "OLD-EIO"), "2b2b #15 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_eio/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/Config.wtf")), "2b2b #15 bind");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setSimulateFsyncEio(true);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #15 finishes");
        const auto &rep = job.report();
        verify(job.error() != KJob::NoError
            && rep.outcome == LocalMergeExecutionOutcome::FailedUncertainDurability
            && rep.uncertainDurability
            && !rep.retryAllowed
            && rep.replacedFiles.size() == 1
            && !rep.replacedFiles.first().durabilityConfirmed
            && rep.replacedFiles.first().backupPreserved
            && !rep.replacedFiles.first().recoveryPath.isEmpty()
            && QFileInfo::exists(rep.replacedFiles.first().recoveryPath)
            && sha256File(rep.replacedFiles.first().recoveryPath) == QCryptographicHash::hash("OLD-EIO", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Config.wtf")) == QCryptographicHash::hash("NEW-EIO", QCryptographicHash::Sha256)
            && rep.undoContract.hasPreservedBackups
            && !rep.undoContract.reversible,
               "2b2b #15+#20+#23+#25 fsync EIO preserves OLD backup for recovery and blocks retry");
    }

    // S2B.2B #16 & #17: Cancel before publication vs Cancel after first Overwrite published
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_cancel/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_cancel/dst/WTF"));
        QByteArray bigA(512 * 1024, 'A');
        QByteArray bigB(512 * 1024, 'B');
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/1.bin"), bigA)
            && writeFile(dst + QStringLiteral("/1.bin"), "OLD-1")
            && writeFile(src + QStringLiteral("/2.bin"), bigB)
            && writeFile(dst + QStringLiteral("/2.bin"), "OLD-2"), "2b2b #16/#17 trees");

        // #16: Cancel before first file finishes
        LocalMergePlan planC1 = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_cancel/dst"))));
        approveAdditiveMergePlan(planC1);
        verify(approveFileOverwrite(planC1, QStringLiteral("WTF/1.bin"))
            && approveFileOverwrite(planC1, QStringLiteral("WTF/2.bin")), "2b2b #16 bind");
        LocalMergeExecutionJob c1Job(planC1, nullptr, 64 * 1024, 25);
        c1Job.setAutoDelete(false);
        QSignalSpy spyC1(&c1Job, &KJob::result);
        c1Job.start();
        QTimer::singleShot(30, &c1Job, [&c1Job] { c1Job.kill(KJob::Quietly); });
        verify(waitUntil([&spyC1] { return spyC1.count() == 1; }, 5000), "2b2b #16 finishes");
        verify(c1Job.report().outcome == LocalMergeExecutionOutcome::CancelledNoChanges
            && c1Job.report().retryAllowed
            && sha256File(dst + QStringLiteral("/1.bin")) == QCryptographicHash::hash("OLD-1", QCryptographicHash::Sha256),
               "2b2b #16 cancel before publish preserves OLD-1");

        // #17: Cancel after 1.bin published, during 2.bin
        LocalMergePlan planC2 = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_cancel/dst"))));
        approveAdditiveMergePlan(planC2);
        verify(approveFileOverwrite(planC2, QStringLiteral("WTF/1.bin"))
            && approveFileOverwrite(planC2, QStringLiteral("WTF/2.bin")), "2b2b #17 bind");
        LocalMergeExecutionJob c2Job(planC2, nullptr, 64 * 1024, 15);
        c2Job.setAutoDelete(false);
        c2Job.setPrePublishHook([&c2Job](const QString &rel, int) {
            if (rel == QStringLiteral("WTF/2.bin")) {
                QMetaObject::invokeMethod(&c2Job, [&c2Job] { c2Job.kill(KJob::Quietly); }, Qt::BlockingQueuedConnection);
            }
        });
        QSignalSpy spyC2(&c2Job, &KJob::result);
        c2Job.start();
        verify(waitUntil([&spyC2] { return spyC2.count() == 1; }, 5000), "2b2b #17 finishes");
        verify(c2Job.report().outcome == LocalMergeExecutionOutcome::CancelledPartialSuccess
            && !c2Job.report().retryAllowed
            && c2Job.report().filesOverwritten == QStringList({QStringLiteral("WTF/1.bin")})
            && sha256File(dst + QStringLiteral("/1.bin")) == QCryptographicHash::hash(bigA, QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/2.bin")) == QCryptographicHash::hash("OLD-2", QCryptographicHash::Sha256),
               "2b2b #17 cancel after first Overwrite reports CancelledPartialSuccess and blocks retry");
    }

    // S2B.2B #21: No overwrite without explicit approval
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_21/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_21/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD"), "2b2b #21 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_21/dst"))));
        for (auto &e : plan.entries) {
            if (e.classification == LocalMergeEntryClassification::MergeDirectoryCandidate) {
                e.approvedAction = LocalMergeApprovedAction::MergeDirectory;
            }
        }
        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #21 finishes");
        verify(job.error() == KJob::NoError
            && job.report().filesOverwritten.isEmpty()
            && job.report().objectsBlocked.contains(QStringLiteral("WTF/F.txt"))
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("OLD", QCryptographicHash::Sha256),
               "2b2b #21 unapproved FileConflict is never overwritten");
    }

    // S2B.2B #18 & #25: Disk full (ENOSPC / ERR_DISK_FULL) after 1st Overwrite -> FailedPartialSuccess & retryAllowed == false
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2b_1825/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2b_1825/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/1.txt"), "NEW-1")
            && writeFile(dst + QStringLiteral("/1.txt"), "OLD-1")
            && writeFile(src + QStringLiteral("/2_new.txt"), "NEW-2"), "2b2b #18/#25 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2b_1825/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/1.txt")), "2b2b #18/#25 bind");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setCopyJobFactory([](const QUrl &, const QUrl &, QObject *parent) -> KJob * {
            class FailingDiskFullJob final : public KJob {
            public:
                explicit FailingDiskFullJob(QObject *p) : KJob(p) {}
                void start() override {
                    QTimer::singleShot(0, this, [this] {
                        setError(KIO::ERR_DISK_FULL);
                        setErrorText(QStringLiteral("Simulated ENOSPC"));
                        emitResult();
                    });
                }
            };
            return new FailingDiskFullJob(parent);
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2b #18/#25 finishes");
        const auto &rep = job.report();
        verify(job.error() == KIO::ERR_DISK_FULL
            && rep.outcome == LocalMergeExecutionOutcome::FailedPartialSuccess
            && rep.partialSuccess
            && !rep.retryAllowed
            && rep.filesOverwritten == QStringList({QStringLiteral("WTF/1.txt")})
            && rep.undoContract.requiresIrreversibleSerial,
               "2b2b #18+#25 disk full after Overwrite reports FailedPartialSuccess and refuses automatic retry");
    }

    // ==================== Stage 2B.2B-R Security & Recovery Review Fault-Injection Tests ====================
    using FaultPhase = LocalAnchoredFileCopyWorker::FaultPhase;

    // 2B.2B-R #1: Race AFTER last fstatat, BEFORE RENAME_EXCHANGE -> post-exchange detects unexpected inode & rolls back cleanly
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2br_1/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2br_1/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW-R1")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD-R1"), "2b2br #1 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2br_1/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/F.txt")), "2b2br #1 bind");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setFaultPhaseHook([](FaultPhase phase, const QString &, int destParentFd, const QString &, const QString &) {
            if (phase == FaultPhase::BeforeExchange) {
                ::unlinkat(destParentFd, "F.txt", 0);
                const int fd = ::openat(destParentFd, "F.txt", O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd >= 0) {
                    const char msg[] = "ALIEN-BEFORE-EXCHANGE";
                    [[maybe_unused]] const ssize_t w = ::write(fd, msg, sizeof(msg) - 1);
                    ::close(fd);
                }
            }
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2br #1 finishes");
        verify(job.error() != KJob::NoError
            && job.report().concurrentModificationDetected
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("ALIEN-BEFORE-EXCHANGE", QCryptographicHash::Sha256)
            && !hasLeftoverStaging(dst),
               "2b2br #1 BeforeExchange race detected post-exchange, rolled back ALIEN cleanly, no leftovers");
    }

    // 2B.2B-R #2 & #3: Race AFTER RENAME_EXCHANGE + AfterUnexpectedInodeDetected (displaced inode swapped -> preserves, never unlinks)
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2br_2/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2br_2/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW-R2")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD-R2"), "2b2br #2 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2br_2/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/F.txt")), "2b2br #2 bind");

        bool sawAfterUnexpected = false;
        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setFaultPhaseHook([&](FaultPhase phase, const QString &, int destParentFd, const QString &partName, const QString &) {
            if (phase == FaultPhase::AfterExchange) {
                const QByteArray pEnc = QFile::encodeName(partName);
                ::unlinkat(destParentFd, pEnc.constData(), 0);
                const int fd = ::openat(destParentFd, pEnc.constData(), O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd >= 0) {
                    const char msg[] = "MODIFIED-AFTER-EXCHANGE";
                    [[maybe_unused]] const ssize_t w = ::write(fd, msg, sizeof(msg) - 1);
                    ::close(fd);
                }
            } else if (phase == FaultPhase::AfterUnexpectedInodeDetected) {
                sawAfterUnexpected = true;
            }
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2br #2 finishes");
        verify(job.error() != KJob::NoError
            && sawAfterUnexpected
            && job.report().concurrentModificationDetected
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("MODIFIED-AFTER-EXCHANGE", QCryptographicHash::Sha256)
            && !hasLeftoverStaging(dst),
               "2b2br #2+#3 AfterExchange unexpected inode detected and rolled back cleanly");
    }

    // 2B.2B-R #4 & #5: Race right BeforeRollback / during rollback -> refuses to overwrite racer2, preserves .thispc-replaced.*
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2br_4/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2br_4/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW-R4")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD-R4"), "2b2br #4 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2br_4/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/F.txt")), "2b2br #4 bind");

        bool sawAfterRollback = false;
        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setFaultPhaseHook([&](FaultPhase phase, const QString &, int destParentFd, const QString &, const QString &) {
            if (phase == FaultPhase::BeforeExchange) {
                ::unlinkat(destParentFd, "F.txt", 0);
                const int fd = ::openat(destParentFd, "F.txt", O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd >= 0) {
                    const char msg[] = "RACER-1";
                    [[maybe_unused]] const ssize_t w = ::write(fd, msg, sizeof(msg) - 1);
                    ::close(fd);
                }
            } else if (phase == FaultPhase::BeforeRollback) {
                ::unlinkat(destParentFd, "F.txt", 0);
                const int fd = ::openat(destParentFd, "F.txt", O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd >= 0) {
                    const char msg[] = "RACER-2-DURING-ROLLBACK";
                    [[maybe_unused]] const ssize_t w = ::write(fd, msg, sizeof(msg) - 1);
                    ::close(fd);
                }
            } else if (phase == FaultPhase::AfterRollback) {
                sawAfterRollback = true;
            }
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2br #4 finishes");
        const auto &rep = job.report();
        verify(job.error() != KJob::NoError
            && sawAfterRollback
            && rep.concurrentModificationDetected
            && rep.outcome == LocalMergeExecutionOutcome::FailedUncertainDurability
            && !rep.retryAllowed
            && rep.replacedFiles.size() == 1
            && rep.replacedFiles.first().backupPreserved
            && rep.replacedFiles.first().backupFileName.startsWith(QStringLiteral(".thispc-replaced."))
            && QFileInfo::exists(rep.replacedFiles.first().recoveryPath)
            && sha256File(rep.replacedFiles.first().recoveryPath) == QCryptographicHash::hash("RACER-2-DURING-ROLLBACK", QCryptographicHash::Sha256),
               "2b2br #4+#5 race during rollback preserves displaced object as .thispc-replaced.* (not .thispc-part.*)");
    }

    // 2B.2B-R #6: BeforeBackupRename collision -> retries with unique .thispc-replaced.* and succeeds
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2br_6/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2br_6/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW-R6")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD-R6"), "2b2br #6 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2br_6/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/F.txt")), "2b2br #6 bind");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setFaultPhaseHook([](FaultPhase phase, const QString &, int destParentFd, const QString &, const QString &backupName) {
            if (phase == FaultPhase::BeforeBackupRename) {
                const QByteArray bEnc = QFile::encodeName(backupName);
                const int fd = ::openat(destParentFd, bEnc.constData(), O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd >= 0) ::close(fd);
            }
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2br #6 finishes");
        verify(job.error() == KJob::NoError
            && job.report().replacedFiles.size() == 1
            && job.report().replacedFiles.first().backupFileName.startsWith(QStringLiteral(".thispc-replaced."))
            && sha256File(dst + QStringLiteral("/F.txt")) == QCryptographicHash::hash("NEW-R6", QCryptographicHash::Sha256),
               "2b2br #6 BeforeBackupRename collision retries with unique .thispc-replaced.*");
    }

    // 2B.2B-R #7: BeforeBackupUnlink alien swap -> refuses unlinkat of alien inode, preserves it
    {
        const QString src = tmp.filePath(QStringLiteral("s2b2br_7/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2b2br_7/dst/WTF"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/F.txt"), "NEW-R7")
            && writeFile(dst + QStringLiteral("/F.txt"), "OLD-R7"), "2b2br #7 trees");

        LocalMergePlan plan = makePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2b2br_7/dst"))));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/F.txt")), "2b2br #7 bind");

        QString capturedBackup;
        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setFaultPhaseHook([&](FaultPhase phase, const QString &, int destParentFd, const QString &, const QString &backupName) {
            if (phase == FaultPhase::BeforeBackupUnlink) {
                capturedBackup = backupName;
                const QByteArray bEnc = QFile::encodeName(backupName);
                ::unlinkat(destParentFd, bEnc.constData(), 0);
                const int fd = ::openat(destParentFd, bEnc.constData(), O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd >= 0) {
                    const char msg[] = "ALIEN-BACKUP-SLOT";
                    [[maybe_unused]] const ssize_t w = ::write(fd, msg, sizeof(msg) - 1);
                    ::close(fd);
                }
            }
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "2b2br #7 finishes");
        verify(job.error() == KJob::NoError
            && job.report().replacedFiles.size() == 1
            && job.report().replacedFiles.first().backupPreserved
            && sha256File(dst + QLatin1Char('/') + capturedBackup) == QCryptographicHash::hash("ALIEN-BACKUP-SLOT", QCryptographicHash::Sha256),
               "2b2br #7 BeforeBackupUnlink alien swap refuses unlinkat and preserves alien inode");
    }

    // R4 Reproduction: Multi-parent source batch swaps intermediate directory under SECOND source root
    {
        const QString root1 = tmp.filePath(QStringLiteral("r4_multi/root1"));
        const QString root2 = tmp.filePath(QStringLiteral("r4_multi/root2"));
        const QString dstRoot = tmp.filePath(QStringLiteral("r4_multi/dst"));
        const QString evilDir = tmp.filePath(QStringLiteral("r4_multi/evil"));
        verify(QDir().mkpath(root1 + QStringLiteral("/BoxA"))
            && QDir().mkpath(root2 + QStringLiteral("/BoxB/Sub"))
            && QDir().mkpath(dstRoot)
            && QDir().mkpath(evilDir)
            && writeFile(root1 + QStringLiteral("/BoxA/a.txt"), "from-root1")
            && writeFile(root2 + QStringLiteral("/BoxB/Sub/b.txt"), "from-root2")
            && writeFile(evilDir + QStringLiteral("/b.txt"), "swapped-evil"), "r4 setup");

        LocalMergePlan plan = makePlan(
            {QUrl::fromLocalFile(root1 + QStringLiteral("/BoxA")),
             QUrl::fromLocalFile(root2 + QStringLiteral("/BoxB"))},
            QUrl::fromLocalFile(dstRoot));
        approveAdditiveMergePlan(plan);

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setPrePublishHook([&](const QString &relPath, int) {
            if (relPath == QStringLiteral("BoxB/Sub/b.txt")) {
                // Swap intermediate directory /root2/BoxB/Sub with a symlink to evilDir right before pre-publish verification
                const QString subPath = root2 + QStringLiteral("/BoxB/Sub");
                QDir(root2 + QStringLiteral("/BoxB")).rename(QStringLiteral("Sub"), QStringLiteral("Sub_old"));
                QFile::link(evilDir, subPath);
            }
        });
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "r4 job finishes");
        verify(job.error() != KJob::NoError
            && !QFileInfo::exists(dstRoot + QStringLiteral("/BoxB/Sub/b.txt")),
               "R4: swapped intermediate directory under second source root must fail closed");
    }

    qInfo("PASS: %d LocalMergeExecutionJob Stage 2B.1+2B.2A+2B.2B+2B.2B-R assertions verified", checks);
    return 0;
}
