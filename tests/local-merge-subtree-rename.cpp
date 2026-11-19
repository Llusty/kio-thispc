/*
 * Safe Subtree Rename Execution, Revalidation & Subtree Skip tests
 * for FOLDER-MERGE-01 Stage 3B.2 (scenarios A..Z).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localmergeexecutionjob.h"
#include "localmergeplan.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

static int checks = 0;

static void verify(bool cond, const char *msg)
{
    ++checks;
    if (!cond) {
        qFatal("FAIL: %s", msg);
    }
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

static QByteArray sha256File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}

static bool waitUntil(const std::function<bool()> &pred, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return pred();
}

static LocalMergePlan makePlan(const QString &srcSubdir, const QString &dstParent)
{
    LocalMergePlanJob job({QUrl::fromLocalFile(srcSubdir)}, QUrl::fromLocalFile(dstParent));
    job.setAutoDelete(false);
    QSignalSpy spy(&job, &KJob::result);
    job.start();
    verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "plan finishes");
    verify(job.error() == KJob::NoError, "plan succeeds");
    return job.plan();
}

static bool hasLeftoverStaging(const QString &dirPath)
{
    const QStringList entries = QDir(dirPath).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &e : entries) {
        if (e.startsWith(QStringLiteral(".thispc-part")) || e.startsWith(QStringLiteral(".thispc-tx"))) {
            return true;
        }
    }
    return false;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    verify(tmp.isValid(), "temp dir valid");

    // A: Rename root directory (DemoRoot -> DemoRoot (kopia)) + W, X, Y
    {
        const QString base = tmp.filePath(QStringLiteral("sc_a"));
        const QString src = base + QStringLiteral("/src/DemoRoot");
        const QString dst = base + QStringLiteral("/dst/DemoRoot");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src + QStringLiteral("/Sub")) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Sub/A.txt"), "AAA")
            && writeFile(dst + QStringLiteral("/Existing.txt"), "OLD-EXISTING"), "A trees");

        LocalMergePlan plan = makePlan(src, dstRoot);
        approveAdditiveMergePlan(plan);
        verify(approveDirectoryRenameAndRevalidate(plan, QStringLiteral("DemoRoot"), QStringLiteral("DemoRoot (kopia)")), "A bind rename");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "A finishes");
        verify(job.error() == KJob::NoError
            && job.report().outcome == LocalMergeExecutionOutcome::Succeeded
            && sha256File(dstRoot + QStringLiteral("/DemoRoot (kopia)/Sub/A.txt")) == QCryptographicHash::hash("AAA", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Existing.txt")) == QCryptographicHash::hash("OLD-EXISTING", QCryptographicHash::Sha256)
            && sha256File(src + QStringLiteral("/Sub/A.txt")) == QCryptographicHash::hash("AAA", QCryptographicHash::Sha256)
            && !hasLeftoverStaging(dstRoot),
               "A: Root directory rename copies subtree under new name & leaves original dst/src untouched");
    }

    // B, C, D, J, W, X, Y: Level 2 & Level 3 subfolder rename + many files + Polish Unicode
    {
        const QString base = tmp.filePath(QStringLiteral("sc_bcdj"));
        const QString src = base + QStringLiteral("/src/Demo");
        const QString dst = base + QStringLiteral("/dst/Demo");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src + QStringLiteral("/Account/SavedVariables/Nested"))
            && QDir().mkpath(dst + QStringLiteral("/Account/SavedVariables"))
            && writeFile(src + QStringLiteral("/Account/Config.ini"), "CFG")
            && writeFile(src + QStringLiteral("/Account/SavedVariables/Addon.lua"), "ADDON")
            && writeFile(src + QStringLiteral("/Account/SavedVariables/Nested/Deep.txt"), "DEEP")
            && writeFile(dst + QStringLiteral("/Account/Existing.lua"), "KEEP-OLD")
            && writeFile(dst + QStringLiteral("/Account/SavedVariables/OldVar.lua"), "KEEP-OLD-VAR"), "B/C/D trees");

        // B + J: Rename level-2 Account -> Konto_Żółć
        LocalMergePlan planB = makePlan(src, dstRoot);
        approveAdditiveMergePlan(planB);
        verify(approveDirectoryRenameAndRevalidate(planB, QStringLiteral("Demo/Account"), QStringLiteral("Konto_Żółć")), "B+J bind");
        LocalMergeExecutionJob jobB(planB);
        jobB.setAutoDelete(false);
        QSignalSpy spyB(&jobB, &KJob::result);
        jobB.start();
        verify(waitUntil([&spyB] { return spyB.count() == 1; }, 5000), "B finishes");
        verify(jobB.error() == KJob::NoError
            && sha256File(dst + QStringLiteral("/Konto_Żółć/Config.ini")) == QCryptographicHash::hash("CFG", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Konto_Żółć/SavedVariables/Addon.lua")) == QCryptographicHash::hash("ADDON", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Konto_Żółć/SavedVariables/Nested/Deep.txt")) == QCryptographicHash::hash("DEEP", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Account/Existing.lua")) == QCryptographicHash::hash("KEEP-OLD", QCryptographicHash::Sha256)
            && !hasLeftoverStaging(dst + QStringLiteral("/Konto_Żółć")),
               "B+D+J: Level-2 Unicode rename copies all nested files & leaves Account untouched");

        // C: Rename level-3 SavedVariables -> SavedVariables (kopia)
        LocalMergePlan planC = makePlan(src, dstRoot);
        approveAdditiveMergePlan(planC);
        verify(approveDirectoryRenameAndRevalidate(planC, QStringLiteral("Demo/Account/SavedVariables"), QStringLiteral("SavedVariables (kopia)")), "C bind");
        LocalMergeExecutionJob jobC(planC);
        jobC.setAutoDelete(false);
        QSignalSpy spyC(&jobC, &KJob::result);
        jobC.start();
        verify(waitUntil([&spyC] { return spyC.count() == 1; }, 5000), "C finishes");
        verify(jobC.error() == KJob::NoError
            && sha256File(dst + QStringLiteral("/Account/SavedVariables (kopia)/Addon.lua")) == QCryptographicHash::hash("ADDON", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Account/SavedVariables (kopia)/Nested/Deep.txt")) == QCryptographicHash::hash("DEEP", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Account/SavedVariables/OldVar.lua")) == QCryptographicHash::hash("KEEP-OLD-VAR", QCryptographicHash::Sha256),
               "C: Level-3 rename copies descendants under new path & keeps original SavedVariables untouched");
    }

    // E: Rename empty folder
    {
        const QString base = tmp.filePath(QStringLiteral("sc_e"));
        const QString src = base + QStringLiteral("/src/EmptyDir");
        const QString dst = base + QStringLiteral("/dst/EmptyDir");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src) && QDir().mkpath(dst), "E trees");
        LocalMergePlan plan = makePlan(src, dstRoot);
        approveAdditiveMergePlan(plan);
        verify(approveDirectoryRenameAndRevalidate(plan, QStringLiteral("EmptyDir"), QStringLiteral("EmptyDir (kopia)")), "E bind");
        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "E finishes");
        verify(job.error() == KJob::NoError && QFileInfo(dstRoot + QStringLiteral("/EmptyDir (kopia)")).isDir(),
               "E: Empty folder renamed and created safely");
    }

    // F, G, H, I, K, L, M: Revalidation rejections (existing, symlink, broken symlink, case collision, >255 bytes, src/dst inode change)
    {
        const QString base = tmp.filePath(QStringLiteral("sc_reval"));
        const QString src = base + QStringLiteral("/src/Demo");
        const QString dst = base + QStringLiteral("/dst/Demo");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src + QStringLiteral("/Sub"))
            && QDir().mkpath(dst + QStringLiteral("/Sub"))
            && QDir().mkpath(dst + QStringLiteral("/ExistingTarget"))
            && QFile::link(dst + QStringLiteral("/Sub"), dst + QStringLiteral("/SymTarget"))
            && QFile::link(QStringLiteral("/nonexistent/broken"), dst + QStringLiteral("/BrokenSym"))
            && QDir().mkpath(dst + QStringLiteral("/CaseTarget")), "reval trees");

        LocalMergePlan plan = makePlan(src, dstRoot);
        approveAdditiveMergePlan(plan);
        bool needsReplan = false;
        QString reason;

        // F: existing dir
        verify(!approveDirectoryRenameAndRevalidate(plan, QStringLiteral("Demo/Sub"), QStringLiteral("ExistingTarget"), &reason, &needsReplan) && needsReplan,
               "F: Rename onto existing dir refused with needsReplan");
        // G: symlink
        verify(!approveDirectoryRenameAndRevalidate(plan, QStringLiteral("Demo/Sub"), QStringLiteral("SymTarget"), &reason, &needsReplan) && needsReplan,
               "G: Rename onto symlink refused with needsReplan");
        // H: broken symlink
        verify(!approveDirectoryRenameAndRevalidate(plan, QStringLiteral("Demo/Sub"), QStringLiteral("BrokenSym"), &reason, &needsReplan) && needsReplan,
               "H: Rename onto broken symlink refused with needsReplan");
        // I: differing case
        verify(!approveDirectoryRenameAndRevalidate(plan, QStringLiteral("Demo/Sub"), QStringLiteral("casetarget"), &reason, &needsReplan) && needsReplan,
               "I: Rename onto differing case refused with needsReplan");
        // K: > 255 bytes
        verify(!approveDirectoryRenameAndRevalidate(plan, QStringLiteral("Demo/Sub"), QString(260, QLatin1Char('a')), &reason, &needsReplan),
               "K: Rename > 255 bytes refused");

        // L: source inode changed
        QDir(src + QStringLiteral("/Sub")).removeRecursively();
        QDir().mkpath(src + QStringLiteral("/Sub"));
        verify(!approveDirectoryRenameAndRevalidate(plan, QStringLiteral("Demo/Sub"), QStringLiteral("FreshName")),
               "L: Rename after source inode change refused");

        // M: destination inode changed
        LocalMergePlan planM = makePlan(src, dstRoot);
        approveAdditiveMergePlan(planM);
        QDir(dst + QStringLiteral("/Sub")).removeRecursively();
        QDir().mkpath(dst + QStringLiteral("/Sub"));
        verify(!approveDirectoryRenameAndRevalidate(planM, QStringLiteral("Demo/Sub"), QStringLiteral("FreshNameM"), &reason, &needsReplan) && needsReplan,
               "M: Rename after destination inode change refused");
    }

    // N, O, P, Q: Parent replaced with symlink, parent moved outside tree, collision created AFTER approval -> NeedsReplan
    {
        const QString base = tmp.filePath(QStringLiteral("sc_nopq"));
        const QString src = base + QStringLiteral("/src/Demo");
        const QString dst = base + QStringLiteral("/dst/Demo");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src + QStringLiteral("/Sub"))
            && QDir().mkpath(dst + QStringLiteral("/Sub"))
            && writeFile(src + QStringLiteral("/Sub/F.txt"), "FFF"), "NOPQ trees");

        // P & Q: Collision created AFTER approval -> NeedsReplan
        LocalMergePlan planPQ = makePlan(src, dstRoot);
        approveAdditiveMergePlan(planPQ);
        verify(approveDirectoryRenameAndRevalidate(planPQ, QStringLiteral("Demo/Sub"), QStringLiteral("Sub (kopia)")), "PQ bind");
        verify(QDir().mkpath(dst + QStringLiteral("/Sub (kopia)"))
            && writeFile(dst + QStringLiteral("/Sub (kopia)/Keep.txt"), "KEEP-RACER"), "create post-approval collision");
        LocalMergeExecutionJob jobPQ(planPQ);
        jobPQ.setAutoDelete(false);
        QSignalSpy spyPQ(&jobPQ, &KJob::result);
        jobPQ.start();
        verify(waitUntil([&spyPQ] { return spyPQ.count() == 1; }, 5000), "PQ finishes");
        verify(jobPQ.error() != KJob::NoError
            && jobPQ.report().outcome == LocalMergeExecutionOutcome::NeedsReplan
            && jobPQ.report().entriesNeedingReplan == QStringList({QStringLiteral("Demo/Sub (kopia)")})
            && sha256File(dst + QStringLiteral("/Sub (kopia)/Keep.txt")) == QCryptographicHash::hash("KEEP-RACER", QCryptographicHash::Sha256),
               "P+Q: Post-approval collision returns NeedsReplan without merging or touching racer dir");

        // N: Parent directory replaced with symlink during execution
        QDir(dst + QStringLiteral("/Sub (kopia)")).removeRecursively();
        LocalMergePlan planN = makePlan(src, dstRoot);
        approveAdditiveMergePlan(planN);
        verify(approveDirectoryRenameAndRevalidate(planN, QStringLiteral("Demo/Sub"), QStringLiteral("Sub (kopia)")), "N bind");
        LocalMergeExecutionJob jobN(planN);
        jobN.setAutoDelete(false);
        jobN.setPrePublishHook([dst, base](const QString &, int) {
            QDir(dst).removeRecursively();
            QFile::link(base + QStringLiteral("/dst"), dst);
        });
        QSignalSpy spyN(&jobN, &KJob::result);
        jobN.start();
        verify(waitUntil([&spyN] { return spyN.count() == 1; }, 5000), "N finishes");
        verify(jobN.error() != KJob::NoError, "N: Parent replaced with symlink during execution fails closed");

        // O: Parent directory moved outside approved tree during execution
        QFile::remove(dst);
        verify(QDir().mkpath(dst + QStringLiteral("/Sub")), "restore dst for O");
        LocalMergePlan planO = makePlan(src, dstRoot);
        approveAdditiveMergePlan(planO);
        verify(approveDirectoryRenameAndRevalidate(planO, QStringLiteral("Demo/Sub"), QStringLiteral("Sub (kopia)")), "O bind");
        LocalMergeExecutionJob jobO(planO);
        jobO.setAutoDelete(false);
        jobO.setPrePublishHook([dst, base](const QString &, int) {
            QDir().rename(dst, base + QStringLiteral("/moved_away"));
        });
        QSignalSpy spyO(&jobO, &KJob::result);
        jobO.start();
        verify(waitUntil([&spyO] { return spyO.count() == 1; }, 5000), "O finishes");
        verify(jobO.error() != KJob::NoError, "O: Parent moved outside tree during execution fails closed");
    }

    // R & S: Skip entire subtree + SkipAllFolders independent of file conflicts
    {
        const QString base = tmp.filePath(QStringLiteral("sc_rs"));
        const QString src = base + QStringLiteral("/src/Demo");
        const QString dst = base + QStringLiteral("/dst/Demo");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src + QStringLiteral("/SkipMe/Inner"))
            && QDir().mkpath(src + QStringLiteral("/KeepMe"))
            && QDir().mkpath(dst + QStringLiteral("/SkipMe"))
            && writeFile(src + QStringLiteral("/SkipMe/A.txt"), "SKIP-A")
            && writeFile(src + QStringLiteral("/SkipMe/Inner/B.txt"), "SKIP-B")
            && writeFile(src + QStringLiteral("/KeepMe/Ok.txt"), "KEEP-OK")
            && writeFile(src + QStringLiteral("/TopConf.txt"), "SRC-CONF")
            && writeFile(dst + QStringLiteral("/TopConf.txt"), "DST-CONF"), "RS trees");

        LocalMergePlan plan = makePlan(src, dstRoot);
        for (auto &e : plan.entries) {
            if (e.relativePath == QLatin1String("Demo")) {
                e.approvedAction = LocalMergeApprovedAction::MergeDirectory;
            } else if (e.classification == LocalMergeEntryClassification::CreateNew
                       && !e.relativePath.startsWith(QLatin1String("Demo/SkipMe/"))) {
                e.approvedAction = LocalMergeApprovedAction::Create;
            }
        }
        verify(applySkipAllFoldersPolicy(plan) == 1, "S: SkipAllFolders scopes strictly to Demo/SkipMe");
        verify(approveFileSkip(plan, QStringLiteral("Demo/TopConf.txt")), "bind TopConf skip");

        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "RS finishes");
        verify(job.error() == KJob::NoError
            && job.report().outcome == LocalMergeExecutionOutcome::SucceededWithSkippedConflicts
            && job.report().skippedSubtreeCount >= 1
            && !QFileInfo::exists(dst + QStringLiteral("/SkipMe/A.txt"))
            && !QFileInfo::exists(dst + QStringLiteral("/SkipMe/Inner"))
            && sha256File(dst + QStringLiteral("/KeepMe/Ok.txt")) == QCryptographicHash::hash("KEEP-OK", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/TopConf.txt")) == QCryptographicHash::hash("DST-CONF", QCryptographicHash::Sha256),
               "R+S: Skipped subtree creates zero objects while independent KeepMe succeeds");
    }

    // T, U, V: Cancel before write, Cancel after partial success, Disk full / I/O error
    {
        const QString base = tmp.filePath(QStringLiteral("sc_tuv"));
        const QString src = base + QStringLiteral("/src/Demo");
        const QString dst = base + QStringLiteral("/dst/Demo");
        const QString dstRoot = base + QStringLiteral("/dst");
        QByteArray bigData(256 * 1024, 'Z');
        verify(QDir().mkpath(src + QStringLiteral("/Sub")) && QDir().mkpath(dst + QStringLiteral("/Sub"))
            && writeFile(src + QStringLiteral("/Sub/1.bin"), bigData)
            && writeFile(src + QStringLiteral("/Sub/2.bin"), bigData), "TUV trees");

        // T: Cancel before first write
        LocalMergePlan planT = makePlan(src, dstRoot);
        approveAdditiveMergePlan(planT);
        verify(approveDirectoryRenameAndRevalidate(planT, QStringLiteral("Demo/Sub"), QStringLiteral("Sub (T)")), "T bind");
        LocalMergeExecutionJob jobT(planT, nullptr, 64 * 1024, 25);
        jobT.setAutoDelete(false);
        QSignalSpy spyT(&jobT, &KJob::result);
        jobT.start();
        jobT.kill(KJob::Quietly);
        verify(waitUntil([&spyT] { return spyT.count() == 1; }, 5000), "T finishes");
        verify(jobT.report().outcome == LocalMergeExecutionOutcome::CancelledNoChanges
            && jobT.report().retryAllowed, "T: Cancel before first write reports CancelledNoChanges + retryAllowed");

        // U: Cancel after Sub (U) created & 1.bin published (during 2.bin)
        LocalMergePlan planU = makePlan(src, dstRoot);
        approveAdditiveMergePlan(planU);
        verify(approveDirectoryRenameAndRevalidate(planU, QStringLiteral("Demo/Sub"), QStringLiteral("Sub (U)")), "U bind");
        LocalMergeExecutionJob jobU(planU, nullptr, 64 * 1024, 15);
        jobU.setAutoDelete(false);
        jobU.setPrePublishHook([&jobU](const QString &rel, int) {
            if (rel.endsWith(QStringLiteral("2.bin"))) {
                QMetaObject::invokeMethod(&jobU, [&jobU] { jobU.kill(KJob::Quietly); }, Qt::BlockingQueuedConnection);
            }
        });
        QSignalSpy spyU(&jobU, &KJob::result);
        jobU.start();
        verify(waitUntil([&spyU] { return spyU.count() == 1; }, 5000), "U finishes");
        verify(jobU.report().outcome == LocalMergeExecutionOutcome::CancelledPartialSuccess
            && !jobU.report().retryAllowed
            && sha256File(dst + QStringLiteral("/Sub (U)/1.bin")) == QCryptographicHash::hash(bigData, QCryptographicHash::Sha256),
               "U: Cancel after partial success reports CancelledPartialSuccess and disables retry");

        // V: Simulated ENOSPC during descendant copy
        LocalMergePlan planV = makePlan(src, dstRoot);
        approveAdditiveMergePlan(planV);
        verify(approveDirectoryRenameAndRevalidate(planV, QStringLiteral("Demo/Sub"), QStringLiteral("Sub (V)")), "V bind");
        LocalMergeExecutionJob jobV(planV);
        jobV.setAutoDelete(false);
        jobV.setCopyJobFactory([](const QUrl &, const QUrl &, QObject *parent) -> KJob * {
            class FailDiskFull final : public KJob {
            public:
                explicit FailDiskFull(QObject *p) : KJob(p) {}
                void start() override {
                    QTimer::singleShot(0, this, [this] {
                        setError(KIO::ERR_DISK_FULL);
                        setErrorText(QStringLiteral("Simulated ENOSPC"));
                        emitResult();
                    });
                }
            };
            return new FailDiskFull(parent);
        });
        QSignalSpy spyV(&jobV, &KJob::result);
        jobV.start();
        verify(waitUntil([&spyV] { return spyV.count() == 1; }, 5000), "V finishes");
        verify(jobV.error() == KIO::ERR_DISK_FULL
            && jobV.report().outcome == LocalMergeExecutionOutcome::FailedPartialSuccess
            && !jobV.report().retryAllowed,
               "V: ENOSPC during descendant copy reports FailedPartialSuccess and refuses retry");
    }

    qInfo("PASS: %d Stage 3B.2 LocalMergeSubtreeRename & Revalidation assertions verified", checks);
    return 0;
}
