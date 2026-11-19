/*
 * Stage 2A tests for LocalMergePlanJob (FOLDER-MERGE-01).
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localmergeplan.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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

static const LocalMergePlanEntry *findEntry(const LocalMergePlan &plan, const QString &relPath)
{
    for (const auto &e : plan.entries) {
        if (e.relativePath == relPath) return &e;
    }
    return nullptr;
}

static LocalMergePlan runMergePlan(const QList<QUrl> &sources, const QUrl &destDir, LocalMergePlanError *outErr = nullptr)
{
    LocalMergePlanJob job(sources, destDir);
    job.setAutoDelete(false);
    QSignalSpy spy(&job, &KJob::result);
    job.start();
    verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "LocalMergePlanJob finishes");
    if (outErr) *outErr = job.planError();
    return job.plan();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    verify(tmp.isValid(), "temporary root created");

    // 1. Two WTF directories with identical names, partial overlap, no file conflicts + sample output
    {
        const QString srcParent = tmp.filePath(QStringLiteral("s1/src"));
        const QString dstParent = tmp.filePath(QStringLiteral("s1/dst"));
        verify(QDir().mkpath(srcParent + QStringLiteral("/WTF/Account/Acc1"))
            && QDir().mkpath(srcParent + QStringLiteral("/WTF/EmptySub"))
            && QDir().mkpath(dstParent + QStringLiteral("/WTF/Account/Acc2"))
            && writeFile(srcParent + QStringLiteral("/WTF/Account/Acc1/bindings.wtf"), "bind-a")
            && writeFile(dstParent + QStringLiteral("/WTF/Account/Acc2/macros.wtf"), "macro-b")
            && writeFile(dstParent + QStringLiteral("/WTF/existing-only.txt"), "keep"),
            "s1 trees created");

        LocalMergePlanError err = LocalMergePlanError::None;
        const LocalMergePlan plan = runMergePlan(
            {QUrl::fromLocalFile(srcParent + QStringLiteral("/WTF"))},
            QUrl::fromLocalFile(dstParent), &err);
        verify(err == LocalMergePlanError::None && !plan.hasBlockingErrors, "s1 plan succeeds cleanly");
        verify(plan.stats.mergeDirectoryCandidateCount == 2, "s1 has 2 merge dir candidates (WTF, WTF/Account)");
        verify(plan.stats.newDirectoryCount == 2, "s1 has 2 new dirs (WTF/Account/Acc1, WTF/EmptySub)");
        verify(plan.stats.newFileCount == 1, "s1 has 1 new file");
        verify(plan.stats.fileConflictCount == 0, "s1 has 0 file conflicts");
        verify(plan.stats.bytesToCopy == 6, "s1 bytesToCopy == 6");

        const auto *wtfRoot = findEntry(plan, QStringLiteral("WTF"));
        verify(wtfRoot && wtfRoot->classification == LocalMergeEntryClassification::MergeDirectoryCandidate
            && wtfRoot->pendingDecision == LocalMergePendingDecision::AwaitUserFolderDecision
            && wtfRoot->approvedAction == LocalMergeApprovedAction::Unapproved, "WTF root classified as MergeDirectoryCandidate");
        const auto *newFile = findEntry(plan, QStringLiteral("WTF/Account/Acc1/bindings.wtf"));
        verify(newFile && newFile->classification == LocalMergeEntryClassification::CreateNew
            && newFile->pendingDecision == LocalMergePendingDecision::NoneNeeded
            && newFile->approvedAction == LocalMergeApprovedAction::Unapproved, "s1 new file classified");

        qInfo("SAMPLE_WTF_PLAN: entries=%lld mergeDirs=%lld newDirs=%lld newFiles=%lld fileConflicts=%lld bytes=%llu",
              static_cast<long long>(plan.entries.size()),
              static_cast<long long>(plan.stats.mergeDirectoryCandidateCount),
              static_cast<long long>(plan.stats.newDirectoryCount),
              static_cast<long long>(plan.stats.newFileCount),
              static_cast<long long>(plan.stats.fileConflictCount),
              static_cast<unsigned long long>(plan.stats.bytesToCopy));
    }

    // 2 & 3. Single file conflict and multi-level conflicts
    {
        const QString src = tmp.filePath(QStringLiteral("s2/src/WTF"));
        const QString dst = tmp.filePath(QStringLiteral("s2/dst/WTF"));
        verify(QDir().mkpath(src + QStringLiteral("/sub/deep"))
            && QDir().mkpath(dst + QStringLiteral("/sub/deep"))
            && writeFile(src + QStringLiteral("/Config.wtf"), "new-cfg")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "old-cfg-longer")
            && writeFile(src + QStringLiteral("/sub/deep/vars.lua"), "v2")
            && writeFile(dst + QStringLiteral("/sub/deep/vars.lua"), "v1")
            && writeFile(src + QStringLiteral("/sub/fresh.txt"), "fresh"),
            "s2 trees created");

        const LocalMergePlan plan = runMergePlan(
            {QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s2/dst"))));
        verify(plan.stats.mergeDirectoryCandidateCount == 3, "s2 3 merge dirs");
        verify(plan.stats.fileConflictCount == 2, "s2 2 file conflicts");
        verify(plan.stats.newFileCount == 1, "s2 1 new file");
        for (const auto &e : plan.entries) {
            verify(e.approvedAction != LocalMergeApprovedAction::Overwrite, "no entry defaults to Overwrite");
        }
        const auto *cfg = findEntry(plan, QStringLiteral("WTF/Config.wtf"));
        verify(cfg && cfg->classification == LocalMergeEntryClassification::FileConflict
            && cfg->pendingDecision == LocalMergePendingDecision::AwaitUserFileDecision
            && cfg->size == 7 && cfg->destinationSize == 14, "s2 Config.wtf conflict details");
    }

    // 4 & 5. Type conflicts: file vs dir and dir vs file
    {
        const QString src = tmp.filePath(QStringLiteral("s4/src/Pack"));
        const QString dst = tmp.filePath(QStringLiteral("s4/dst/Pack"));
        verify(QDir().mkpath(src + QStringLiteral("/dirOverFile"))
            && QDir().mkpath(dst + QStringLiteral("/fileOverDir"))
            && writeFile(src + QStringLiteral("/fileOverDir"), "file")
            && writeFile(dst + QStringLiteral("/dirOverFile"), "existing-file"),
            "s4 trees created");

        const LocalMergePlan plan = runMergePlan(
            {QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s4/dst"))));
        verify(plan.stats.typeConflictCount == 2, "s4 2 type conflicts");
        const auto *fod = findEntry(plan, QStringLiteral("Pack/fileOverDir"));
        const auto *dof = findEntry(plan, QStringLiteral("Pack/dirOverFile"));
        verify(fod && fod->classification == LocalMergeEntryClassification::TypeConflictFileOverDir
            && fod->pendingDecision == LocalMergePendingDecision::RefusedBlocked, "fileOverDir classified");
        verify(dof && dof->classification == LocalMergeEntryClassification::TypeConflictDirOverFile
            && dof->pendingDecision == LocalMergePendingDecision::RefusedBlocked, "dirOverFile classified");
    }

    // 6, 7, 8. Symlink to file, symlink to directory (not followed!), broken symlink, dest symlink to dir
    {
        const QString src = tmp.filePath(QStringLiteral("s6/src/Links"));
        const QString dst = tmp.filePath(QStringLiteral("s6/dst/Links"));
        const QString outsideDir = tmp.filePath(QStringLiteral("s6/outside"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst) && QDir().mkpath(outsideDir)
            && writeFile(outsideDir + QStringLiteral("/secret.txt"), "secret")
            && QFile::link(outsideDir, src + QStringLiteral("/dir-link"))
            && QFile::link(outsideDir + QStringLiteral("/secret.txt"), src + QStringLiteral("/file-link"))
            && QFile::link(tmp.filePath(QStringLiteral("s6/does-not-exist")), src + QStringLiteral("/broken-link"))
            && QFile::link(outsideDir, dst + QStringLiteral("/target-is-dir-link"))
            && writeFile(src + QStringLiteral("/target-is-dir-link"), "regular"),
            "s6 symlinks created");

        const LocalMergePlan plan = runMergePlan(
            {QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s6/dst"))));
        verify(!findEntry(plan, QStringLiteral("Links/dir-link/secret.txt")), "did NOT recurse into dir symlink");
        const auto *dl = findEntry(plan, QStringLiteral("Links/dir-link"));
        const auto *bl = findEntry(plan, QStringLiteral("Links/broken-link"));
        const auto *tdl = findEntry(plan, QStringLiteral("Links/target-is-dir-link"));
        verify(dl && dl->sourceKind == LocalMergeObjectKind::SymbolicLink && dl->classification == LocalMergeEntryClassification::CreateNew,
               "dir-link stays SymbolicLink");
        verify(bl && bl->classification == LocalMergeEntryClassification::BrokenSymlink && bl->sourceIsBrokenSymlink,
               "broken-link detected");
        verify(tdl && tdl->destinationKind == LocalMergeObjectKind::SymbolicLink
            && tdl->classification == LocalMergeEntryClassification::DestinationSymlinkConflict,
               "destination symlink to dir detected by lstat, NOT misclassified as Directory");
    }

    // 9. Unsupported special object (FIFO, never opened)
    {
        const QString src = tmp.filePath(QStringLiteral("s9/src/Spec"));
        const QString dst = tmp.filePath(QStringLiteral("s9/dst"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst), "s9 dirs");
        verify(::mkfifo(QFile::encodeName(src + QStringLiteral("/pipe.fifo")).constData(), 0600) == 0, "fifo created");

        const LocalMergePlan plan = runMergePlan({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dst));
        const auto *fifoEntry = findEntry(plan, QStringLiteral("Spec/pipe.fifo"));
        verify(fifoEntry && fifoEntry->sourceKind == LocalMergeObjectKind::Special
            && fifoEntry->classification == LocalMergeEntryClassification::UnsupportedSpecial
            && fifoEntry->pendingDecision == LocalMergePendingDecision::RefusedBlocked
            && plan.hasBlockingErrors, "FIFO blocked without opening");
    }

    // 10 & 11. Differing-case collision (Config.wtf / config.wtf) & Unicode / Polish characters
    {
        const QString src = tmp.filePath(QStringLiteral("s10/src/Zażółć"));
        const QString dst = tmp.filePath(QStringLiteral("s10/dst/Zażółć"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "a")
            && writeFile(dst + QStringLiteral("/config.wtf"), "b")
            && writeFile(src + QStringLiteral("/ąęćłńóśźż.txt"), "pl"),
            "s10 unicode & case files");

        const LocalMergePlan plan = runMergePlan(
            {QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s10/dst"))));
        const auto *cfg = findEntry(plan, QStringLiteral("Zażółć/Config.wtf"));
        const auto *pl = findEntry(plan, QStringLiteral("Zażółć/ąęćłńóśźż.txt"));
        verify(cfg && cfg->classification == LocalMergeEntryClassification::CaseInsensitiveCollision
            && (cfg->caseStatus == LocalMergeCaseStatus::SuspectedCaseInsensitiveSibling
                || cfg->caseStatus == LocalMergeCaseStatus::ActualCaseInsensitiveCollision)
            && cfg->caseCollisionSiblingName == QStringLiteral("config.wtf"),
               "Config.wtf vs config.wtf flagged");
        verify(pl && pl->classification == LocalMergeEntryClassification::CreateNew, "Unicode Polish filename CreateNew");
    }

    // 12, 13, 14. Empty subdirs, Self-copy, Copy into descendant (direct & via symlink)
    {
        const QString root = tmp.filePath(QStringLiteral("s13/Folder"));
        const QString child = root + QStringLiteral("/Sub/Empty");
        const QString linkParent = tmp.filePath(QStringLiteral("s13/AliasParent"));
        verify(QDir().mkpath(child) && QDir().mkpath(linkParent)
            && QFile::link(root + QStringLiteral("/Sub"), linkParent + QStringLiteral("/AliasSub")),
            "s13 tree");

        // Self-copy into parent
        const LocalMergePlan selfPlan = runMergePlan(
            {QUrl::fromLocalFile(root)}, QUrl::fromLocalFile(tmp.filePath(QStringLiteral("s13"))));
        const auto *selfEntry = findEntry(selfPlan, QStringLiteral("Folder"));
        verify(selfEntry && selfEntry->classification == LocalMergeEntryClassification::SameObject
            && selfEntry->pendingDecision == LocalMergePendingDecision::RefusedBlocked,
               "self-copy detected as SameObject");

        // Descendant copy (direct)
        const LocalMergePlan descPlan = runMergePlan({QUrl::fromLocalFile(root)}, QUrl::fromLocalFile(child));
        verify(descPlan.hasBlockingErrors && descPlan.stats.sameOrDescendantBlockedCount == 1,
               "direct descendant copy blocked");

        // Descendant copy via symlink
        const LocalMergePlan symDescPlan = runMergePlan(
            {QUrl::fromLocalFile(root)}, QUrl::fromLocalFile(linkParent + QStringLiteral("/AliasSub")));
        verify(symDescPlan.hasBlockingErrors && symDescPlan.stats.sameOrDescendantBlockedCount == 1,
               "symlinked descendant copy blocked");
    }

    // 15. Two sources leading to same destination (exact & differing case)
    {
        const QString sA = tmp.filePath(QStringLiteral("s15/a/Dup"));
        const QString sB = tmp.filePath(QStringLiteral("s15/b/Dup"));
        const QString dst = tmp.filePath(QStringLiteral("s15/dst"));
        verify(QDir().mkpath(sA) && QDir().mkpath(sB) && QDir().mkpath(dst), "s15 dirs");
        const LocalMergePlan dupPlan = runMergePlan(
            {QUrl::fromLocalFile(sA), QUrl::fromLocalFile(sB)}, QUrl::fromLocalFile(dst));
        verify(dupPlan.hasBlockingErrors && dupPlan.stats.ambiguousNameCount >= 1,
               "duplicate destination in batch blocked");
    }

    // 16. Unreadable metadata (chmod 000 on source dir when non-root)
    if (::geteuid() != 0) {
        const QString locked = tmp.filePath(QStringLiteral("s16/Locked"));
        const QString dst = tmp.filePath(QStringLiteral("s16/dst"));
        verify(QDir().mkpath(locked) && QDir().mkpath(dst), "s16 dirs");
        verify(::chmod(QFile::encodeName(locked).constData(), 0000) == 0, "chmod 000");
        const LocalMergePlan plan = runMergePlan({QUrl::fromLocalFile(locked)}, QUrl::fromLocalFile(dst));
        ::chmod(QFile::encodeName(locked).constData(), 0700);
        verify(plan.hasBlockingErrors && plan.stats.unreadableMetadataCount >= 1,
               "unreadable source dir detected fail-closed");
    }

    // 17. Cancellation
    {
        const QString src = tmp.filePath(QStringLiteral("s17/Slow"));
        const QString dst = tmp.filePath(QStringLiteral("s17/dst"));
        verify(QDir().mkpath(src) && QDir().mkpath(dst), "s17 dirs");
        for (int i = 0; i < 10; ++i) {
            verify(writeFile(src + QStringLiteral("/f%1.txt").arg(i), "x"), "s17 file");
        }
        LocalMergePlanJob job({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dst), nullptr, 15);
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&job] { return job.plannedEntryCount() >= 2; }, 3000),
               "s17 begins async planning");
        verify(job.kill(KJob::EmitResult), "s17 kill(EmitResult) succeeds");
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "s17 cancelled finishes");
        verify(job.planError() == LocalMergePlanError::Cancelled, "s17 reports Cancelled");
    }

    qInfo("PASS: %d LocalMergePlan Stage 2A assertions verified", checks);
    return 0;
}
