/*
 * Isolated GUI tests for FOLDER-MERGE-01 Stage 3A dialogs.
 * Tests LocalFolderMergeConflictDialog and LocalFileConflictResolutionDialog
 * on synthetic LocalMergePlanEntry data with zero filesystem I/O.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localmergedialogs.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QPalette>
#include <QTemporaryDir>
#include <QTest>

#include <cstdlib>

static int checks = 0;

static void verify(bool cond, const char *msg)
{
    ++checks;
    if (!cond) {
        qFatal("FAIL: %s", msg);
    }
}

static LocalMergePlanEntry makeFolderConflictEntry(const QString &rel, const QString &src, const QString &dst, bool selfCopy = false)
{
    LocalMergePlanEntry e;
    e.relativePath = rel;
    e.sourcePath = src;
    e.destinationPath = dst;
    e.sourceKind = LocalMergeObjectKind::Directory;
    e.destinationKind = LocalMergeObjectKind::Directory;
    e.classification = selfCopy ? LocalMergeEntryClassification::SameObject
                                : LocalMergeEntryClassification::MergeDirectoryCandidate;
    e.caseStatus = LocalMergeCaseStatus::ExactNameMatch;
    e.pendingDecision = LocalMergePendingDecision::AwaitUserFolderDecision;
    return e;
}

static LocalMergePlanEntry makeFileEntry(
    const QString &rel,
    const QString &src,
    const QString &dst,
    LocalMergeEntryClassification cls,
    LocalMergeObjectKind srcKind = LocalMergeObjectKind::RegularFile,
    LocalMergeObjectKind dstKind = LocalMergeObjectKind::RegularFile,
    LocalMergeCaseStatus caseSt = LocalMergeCaseStatus::ExactNameMatch)
{
    LocalMergePlanEntry e;
    e.relativePath = rel;
    e.sourcePath = src;
    e.destinationPath = dst;
    e.sourceKind = srcKind;
    e.destinationKind = dstKind;
    e.classification = cls;
    e.caseStatus = caseSt;
    e.size = 4096;
    e.destinationSize = 2048;
    e.sourceStatValid = true;
    e.destinationStatValid = true;
    e.sourceStat.st_mtim.tv_sec = 1760000000;
    e.destinationStat.st_mtim.tv_sec = 1759000000;
    return e;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 20: Guard temp dir to prove dialogs perform ZERO filesystem operations
    QTemporaryDir guardTmp;
    verify(guardTmp.isValid(), "guard temp dir valid");
    const QString guardFile = guardTmp.filePath(QStringLiteral("untouched.txt"));
    {
        QFile f(guardFile);
        verify(f.open(QIODevice::WriteOnly) && f.write("DEMO") == 4, "write guard file");
    }

    const QLocale plLocale(QLocale::Polish, QLocale::Poland);
    const QLocale enLocale(QLocale::English, QLocale::UnitedStates);

    // 1, 2, 8: Folder conflict dialog - Merge (Enter default), Rename, Skip, Cancel
    {
        const auto entry = makeFolderConflictEntry(
            QStringLiteral("DemoFolder"),
            QStringLiteral("/demo/src/DemoFolder"),
            QStringLiteral("/demo/dst/DemoFolder"));

        // #2 & #8: Enter triggers safe default MergeDirectory
        LocalFolderMergeConflictDialog dlgMerge(entry, {}, false, plLocale);
        verify(dlgMerge.mergeButton() != nullptr && dlgMerge.mergeButton()->isDefault(), "Merge is default for folder conflict");
        dlgMerge.show();
        QTest::keyClick(&dlgMerge, Qt::Key_Return);
        verify(dlgMerge.result() == QDialog::Accepted
            && dlgMerge.decision().action == LocalMergeApprovedAction::MergeDirectory,
               "Enter confirms MergeDirectory");

        // #3: Rename valid + unsafe name blocked
        LocalFolderMergeConflictDialog dlgRen(entry, {QStringLiteral("DemoFolder (kopia)")}, false, plLocale);
        verify(dlgRen.renameLineEdit()->text() == QStringLiteral("DemoFolder (kopia 2)"), "Rename avoids existing sibling");
        dlgRen.renameLineEdit()->setText(QStringLiteral("../evil"));
        verify(!dlgRen.renameButton()->isEnabled() && !dlgRen.validationLabel()->isHidden(), "Unsafe .. blocked");
        dlgRen.renameLineEdit()->setText(QStringLiteral("a/b"));
        verify(!dlgRen.renameButton()->isEnabled(), "Slash blocked");
        dlgRen.renameLineEdit()->setText(QStringLiteral("DemoFolder_Nowy"));
        verify(dlgRen.renameButton()->isEnabled(), "Safe folder name enabled");
        dlgRen.renameButton()->click();
        verify(dlgRen.result() == QDialog::Accepted
            && dlgRen.decision().action == LocalMergeApprovedAction::Rename
            && dlgRen.decision().targetFolderName == QStringLiteral("DemoFolder_Nowy"),
               "Rename folder returns Rename + targetFolderName");

        // #4: Skip
        LocalFolderMergeConflictDialog dlgSkip(entry, {}, false, enLocale);
        dlgSkip.skipButton()->click();
        verify(dlgSkip.result() == QDialog::Accepted
            && dlgSkip.decision().action == LocalMergeApprovedAction::Skip,
               "Skip folder returns Skip");

        // #5, #6, #7: Cancel button, X close, Escape
        LocalFolderMergeConflictDialog dlgCancel(entry, {}, false, enLocale);
        dlgCancel.cancelButton()->click();
        verify(dlgCancel.result() == QDialog::Rejected && dlgCancel.decision().action == LocalMergeApprovedAction::Abort,
               "Cancel button returns Abort");

        LocalFolderMergeConflictDialog dlgX(entry, {}, false, enLocale);
        dlgX.show();
        QCloseEvent ce;
        QApplication::sendEvent(&dlgX, &ce);
        verify(dlgX.result() == QDialog::Rejected && dlgX.decision().action == LocalMergeApprovedAction::Abort,
               "Window close X returns Abort");

        LocalFolderMergeConflictDialog dlgEsc(entry, {}, false, enLocale);
        dlgEsc.show();
        QTest::keyClick(&dlgEsc, Qt::Key_Escape);
        verify(dlgEsc.result() == QDialog::Rejected && dlgEsc.decision().action == LocalMergeApprovedAction::Abort,
               "Escape returns Abort");
    }

    // Self-copy folder conflict NEVER offers Merge
    {
        const auto selfEntry = makeFolderConflictEntry(
            QStringLiteral("Katalog_Żółć"),
            QStringLiteral("/demo/same/Katalog_Żółć"),
            QStringLiteral("/demo/same/Katalog_Żółć"),
            true);
        LocalFolderMergeConflictDialog dlgSelf(selfEntry, {}, true, plLocale);
        verify(dlgSelf.mergeButton() != nullptr
            && !dlgSelf.mergeButton()->isHidden()
            && !dlgSelf.mergeButton()->isEnabled(),
               "Self-copy shows Merge button as disabled with explanation");
        verify(dlgSelf.renameButton()->isDefault(), "Self-copy defaults to Rename");
        verify(dlgSelf.titleLabel()->text().contains(QStringLiteral("Katalog_Żółć")), "Unicode folder name preserved");
    }

    // 9 & 10: Long paths & Unicode Polish filenames
    {
        const QString longSrc = QStringLiteral("/mnt/source/") + QStringLiteral("bardzo_długi_katalog_").repeated(8) + QStringLiteral("Zażółć gęślą jaźń.txt");
        const QString longDst = QStringLiteral("/mnt/dest/") + QStringLiteral("docelowy_katalog_").repeated(8) + QStringLiteral("Zażółć gęślą jaźń.txt");
        const auto e = makeFileEntry(QStringLiteral("Zażółć gęślą jaźń.txt"), longSrc, longDst, LocalMergeEntryClassification::FileConflict);
        LocalMergeBackendCapabilities caps;
        caps.canSafeOverwrite = false;
        caps.filesystemLabel = QStringLiteral("NTFS-3G (FUSE)");
        LocalFileConflictResolutionDialog dlg(e, caps, 0, {}, plLocale);
        verify(dlg.sourcePathLabel()->fullPath() == longSrc
            && dlg.destinationPathLabel()->fullPath() == longDst
            && dlg.sourcePathLabel()->toolTip() == longSrc
            && dlg.destinationPathLabel()->toolTip() == longDst
            && dlg.sourcePathLabel()->text().size() < longSrc.size(),
               "Long paths elide cleanly without overflow and expose full path in tooltip");
        verify(dlg.detailsFrame()->isHidden(), "Technical details collapsed by default");
        verify(!dlg.renameLineEdit()->hasFocus(), "Rename field is not auto-focused on open");
        verify(dlg.sizeHint().width() <= 760 && dlg.sizeHint().height() <= 520,
               "Dialog remains compact for smaller resolutions");
    }

    // 11, 12, 15, 16, 17, 18: File conflict capabilities, NTFS blocked Overwrite, verified Overwrite, ApplyToAll, Enter safety, PL/EN
    {
        const auto fileEntry = makeFileEntry(
            QStringLiteral("Sub/Config.demo"),
            QStringLiteral("/demo/src/Sub/Config.demo"),
            QStringLiteral("/demo/dst/Sub/Config.demo"),
            LocalMergeEntryClassification::FileConflict);

        // #15: NTFS-3G / unverified ntfs3 blocks Overwrite via capabilities model (not path text!)
        LocalMergeBackendCapabilities ntfsCaps;
        ntfsCaps.canMergeDirectories = true;
        ntfsCaps.canCreateNewFiles = true;
        ntfsCaps.canRenameConflictingCopies = true;
        ntfsCaps.canSafeOverwrite = false;
        ntfsCaps.filesystemLabel = QStringLiteral("NTFS-3G (FUSE)");

        LocalFileConflictResolutionDialog dlgNtfs(fileEntry, ntfsCaps, 3, {}, plLocale);
        verify(dlgNtfs.overwriteButton() != nullptr
            && !dlgNtfs.overwriteButton()->isHidden()
            && !dlgNtfs.overwriteButton()->isEnabled(),
               "Overwrite visible but disabled when canSafeOverwrite is false");
        verify(!dlgNtfs.safetyLabel()->isHidden() && dlgNtfs.safetyLabel()->text().contains(QStringLiteral("niedostępne")),
               "NTFS safety notice shown in Polish");
        verify(dlgNtfs.capabilitiesLabel()->text().contains(QStringLiteral("scalanie folderów: tak"))
            && dlgNtfs.capabilitiesLabel()->text().contains(QStringLiteral("bezpieczne zastępowanie: nie")),
               "Capabilities distinguish folder merge/new/rename from overwrite");

        // #12: ApplyToAll Skip
        verify(!dlgNtfs.applyToAllCheckBox()->isHidden() && dlgNtfs.applyToAllCheckBox()->isEnabled(), "ApplyToAll visible for remaining file conflicts");
        dlgNtfs.applyToAllCheckBox()->setChecked(true);
        dlgNtfs.skipButton()->click();
        verify(dlgNtfs.decision().action == LocalMergeApprovedAction::Skip
            && dlgNtfs.decision().applyPolicy == LocalMergeApplyPolicy::SkipAllFileConflicts,
               "ApplyToAll + Skip returns SkipAllFileConflicts");

        // #16 & #17: Verified backend enables Overwrite, but Enter NEVER triggers Overwrite
        LocalMergeBackendCapabilities btrfsCaps;
        btrfsCaps.canSafeOverwrite = true;
        btrfsCaps.filesystemLabel = QStringLiteral("Btrfs");

        LocalFileConflictResolutionDialog dlgEnter(fileEntry, btrfsCaps, 2, {}, enLocale);
        verify(dlgEnter.overwriteButton() != nullptr
            && !dlgEnter.overwriteButton()->isDefault()
            && !dlgEnter.overwriteButton()->autoDefault()
            && dlgEnter.skipButton()->isDefault(),
               "Overwrite is never default or autoDefault; Skip is default");
        dlgEnter.show();
        QTest::keyClick(&dlgEnter, Qt::Key_Return);
        verify(dlgEnter.result() == QDialog::Accepted && dlgEnter.decision().action == LocalMergeApprovedAction::Skip,
               "Enter triggers Skip, never accidental Overwrite");

        LocalFileConflictResolutionDialog dlgOvw(fileEntry, btrfsCaps, 2, {}, enLocale);
        dlgOvw.applyToAllCheckBox()->setChecked(true);
        dlgOvw.overwriteButton()->click();
        verify(dlgOvw.result() == QDialog::Accepted
            && dlgOvw.decision().action == LocalMergeApprovedAction::Overwrite
            && dlgOvw.decision().applyPolicy == LocalMergeApplyPolicy::OverwriteAllFileConflicts,
               "Explicit Overwrite click + ApplyToAll returns OverwriteAllFileConflicts");

        // Cancel / Escape / X on file conflict dialog
        LocalFileConflictResolutionDialog dlgEsc(fileEntry, btrfsCaps, 0, {}, enLocale);
        verify(dlgEsc.applyToAllCheckBox()->isHidden(), "ApplyToAll hidden when remainingExactFileConflicts == 0");
        dlgEsc.show();
        QTest::keyClick(&dlgEsc, Qt::Key_Escape);
        verify(dlgEsc.result() == QDialog::Rejected && dlgEsc.decision().action == LocalMergeApprovedAction::Abort,
               "Escape on file dialog returns Abort");
    }

    // 13 & 14: Symlink conflict, Type conflict, Case-insensitive collision block Overwrite & ApplyToAll
    {
        LocalMergeBackendCapabilities btrfsCaps;
        btrfsCaps.canSafeOverwrite = true;
        btrfsCaps.filesystemLabel = QStringLiteral("Btrfs");

        const auto symEntry = makeFileEntry(
            QStringLiteral("Link.txt"), QStringLiteral("/s/Link.txt"), QStringLiteral("/d/Link.txt"),
            LocalMergeEntryClassification::DestinationSymlinkConflict,
            LocalMergeObjectKind::RegularFile, LocalMergeObjectKind::SymbolicLink);
        LocalFileConflictResolutionDialog dlgSym(symEntry, btrfsCaps, 5, {}, plLocale);
        verify(dlgSym.overwriteButton() != nullptr
            && !dlgSym.overwriteButton()->isEnabled()
            && dlgSym.applyToAllCheckBox()->isHidden(),
               "Symlink conflict disables Overwrite and hides ApplyToAll");

        const auto typeEntry = makeFileEntry(
            QStringLiteral("DirVsFile"), QStringLiteral("/s/DirVsFile"), QStringLiteral("/d/DirVsFile"),
            LocalMergeEntryClassification::TypeConflictFileOverDir,
            LocalMergeObjectKind::RegularFile, LocalMergeObjectKind::Directory);
        LocalFileConflictResolutionDialog dlgType(typeEntry, btrfsCaps, 5, {}, plLocale);
        verify(dlgType.overwriteButton() != nullptr
            && !dlgType.overwriteButton()->isEnabled()
            && !dlgType.renameButton()->isEnabled()
            && dlgType.applyToAllCheckBox()->isHidden(),
               "Type conflict disables Overwrite & Rename and hides ApplyToAll");

        const auto caseEntry = makeFileEntry(
            QStringLiteral("config.wtf"), QStringLiteral("/s/config.wtf"), QStringLiteral("/d/Config.wtf"),
            LocalMergeEntryClassification::CaseInsensitiveCollision,
            LocalMergeObjectKind::RegularFile, LocalMergeObjectKind::RegularFile,
            LocalMergeCaseStatus::ActualCaseInsensitiveCollision);
        LocalFileConflictResolutionDialog dlgCase(caseEntry, btrfsCaps, 5, {}, plLocale);
        verify(dlgCase.overwriteButton() != nullptr
            && !dlgCase.overwriteButton()->isEnabled()
            && dlgCase.applyToAllCheckBox()->isHidden(),
               "Case-insensitive collision disables Overwrite and hides ApplyToAll");
    }

    // 19: Dark & Light palette compatibility (no hardcoded unreadable foreground/background)
    {
        QPalette darkPal;
        darkPal.setColor(QPalette::Window, QColor(30, 30, 30));
        darkPal.setColor(QPalette::WindowText, QColor(235, 235, 235));
        darkPal.setColor(QPalette::Base, QColor(24, 24, 24));
        darkPal.setColor(QPalette::Text, QColor(235, 235, 235));

        const auto entry = makeFileEntry(
            QStringLiteral("Palette.txt"), QStringLiteral("/s/Palette.txt"), QStringLiteral("/d/Palette.txt"),
            LocalMergeEntryClassification::FileConflict);
        LocalMergeBackendCapabilities caps;
        caps.canSafeOverwrite = false;
        LocalFileConflictResolutionDialog dlgDark(entry, caps, 0, {}, enLocale);
        dlgDark.setPalette(darkPal);
        verify(dlgDark.palette().color(QPalette::WindowText) == QColor(235, 235, 235)
            && dlgDark.safetyLabel()->palette().color(QPalette::WindowText) == QColor(235, 235, 235),
               "Dark palette propagates cleanly without hardcoded black text");

        QPalette lightPal;
        lightPal.setColor(QPalette::Window, QColor(245, 245, 245));
        lightPal.setColor(QPalette::WindowText, QColor(20, 20, 20));
        dlgDark.setPalette(lightPal);
        verify(dlgDark.safetyLabel()->palette().color(QPalette::WindowText) == QColor(20, 20, 20),
               "Light palette propagates cleanly without hardcoded white text");
    }

    // ==================== Stage 3A Addendum: Recursive Folder & File Conflict UX ====================
    auto makeDeepPlan = [&]() {
        LocalMergePlan plan;
        plan.entries.append(makeFolderConflictEntry(
            QStringLiteral("DemoRoot"), QStringLiteral("/demo/src/DemoRoot"), QStringLiteral("/demo/dst/DemoRoot")));
        LocalMergePlanEntry cleanFile;
        cleanFile.relativePath = QStringLiteral("DemoRoot/CleanNew.txt");
        cleanFile.sourcePath = QStringLiteral("/demo/src/DemoRoot/CleanNew.txt");
        cleanFile.destinationPath = QStringLiteral("/demo/dst/DemoRoot/CleanNew.txt");
        cleanFile.sourceKind = LocalMergeObjectKind::RegularFile;
        cleanFile.destinationKind = LocalMergeObjectKind::Missing;
        cleanFile.classification = LocalMergeEntryClassification::CreateNew;
        plan.entries.append(cleanFile);

        plan.entries.append(makeFolderConflictEntry(
            QStringLiteral("DemoRoot/Account"), QStringLiteral("/demo/src/DemoRoot/Account"), QStringLiteral("/demo/dst/DemoRoot/Account")));
        plan.entries.append(makeFolderConflictEntry(
            QStringLiteral("DemoRoot/Account/SavedVariables"), QStringLiteral("/demo/src/DemoRoot/Account/SavedVariables"), QStringLiteral("/demo/dst/DemoRoot/Account/SavedVariables")));
        plan.entries.append(makeFileEntry(
            QStringLiteral("DemoRoot/Account/SavedVariables/Addon.lua"),
            QStringLiteral("/demo/src/DemoRoot/Account/SavedVariables/Addon.lua"),
            QStringLiteral("/demo/dst/DemoRoot/Account/SavedVariables/Addon.lua"),
            LocalMergeEntryClassification::FileConflict));
        plan.entries.append(makeFileEntry(
            QStringLiteral("DemoRoot/Account/SavedVariables/Extra.lua"),
            QStringLiteral("/demo/src/DemoRoot/Account/SavedVariables/Extra.lua"),
            QStringLiteral("/demo/dst/DemoRoot/Account/SavedVariables/Extra.lua"),
            LocalMergeEntryClassification::FileConflict));
        return plan;
    };

    // Addendum #1, #2, #3, #4: Parent->Child recursive prompting, non-conflicting auto-approved, merging root does NOT auto-merge child
    {
        LocalMergePlan plan = makeDeepPlan();
        LocalMergeBackendCapabilities caps;
        caps.canSafeOverwrite = true;
        caps.filesystemLabel = QStringLiteral("Btrfs");

        QStringList promptedOrder;
        QList<LocalMergeConflictProgress> seenProgress;
        const LocalMergeResolutionResult res = LocalMergeConflictResolver::resolve(
            plan, caps,
            [&](const LocalMergePlanEntry &entry, qsizetype remFolders, LocalMergeConflictProgress prog) {
                promptedOrder.append(entry.relativePath);
                seenProgress.append(prog);
                // Verify real widget displays relative path & progress counter
                LocalFolderMergeConflictDialog dlg(entry, {}, false, plLocale, nullptr, remFolders, prog);
                verify(dlg.progressLabel()->text().contains(entry.relativePath)
                    && dlg.progressLabel()->text().contains(QString::number(prog.currentIndex))
                    && dlg.progressLabel()->text().contains(QString::number(prog.totalConflicts)),
                       "Folder dialog shows relative path and progress counter");
                return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
            },
            [&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &c, qsizetype remFiles, LocalMergeConflictProgress prog) {
                promptedOrder.append(entry.relativePath);
                seenProgress.append(prog);
                LocalFileConflictResolutionDialog dlg(entry, c, remFiles, {}, plLocale, nullptr, prog);
                verify(dlg.progressLabel()->text().contains(entry.relativePath), "File dialog shows deep relative path");
                return LocalFileConflictDecision{
                    entry.relativePath.endsWith(QStringLiteral("Addon.lua"))
                        ? LocalMergeApprovedAction::Overwrite
                        : LocalMergeApprovedAction::Skip,
                    LocalMergeApplyPolicy::None, {}, true};
            });

        verify(res.completed && !res.cancelled
            && res.folderPromptsShown == 3
            && res.filePromptsShown == 2
            && res.nonConflictingAutoApproved == 1
            && promptedOrder == QStringList({
                QStringLiteral("DemoRoot"),
                QStringLiteral("DemoRoot/Account"),
                QStringLiteral("DemoRoot/Account/SavedVariables"),
                QStringLiteral("DemoRoot/Account/SavedVariables/Addon.lua"),
                QStringLiteral("DemoRoot/Account/SavedVariables/Extra.lua")}),
               "Recursive resolver prompts parent->child on all 3 folder levels + 2 files, skipping CleanNew.txt");
    }

    // Addendum #5: Independent ApplyToAll for folders (MergeAllFolders) does NOT auto-enable file Overwrite
    {
        LocalMergePlan plan = makeDeepPlan();
        LocalMergeBackendCapabilities caps;
        caps.canSafeOverwrite = false;
        caps.filesystemLabel = QStringLiteral("NTFS-3G (FUSE)");

        const LocalMergeResolutionResult res = LocalMergeConflictResolver::resolve(
            plan, caps,
            [&](const LocalMergePlanEntry &entry, qsizetype remFolders, LocalMergeConflictProgress prog) {
                LocalFolderMergeConflictDialog dlg(entry, {}, false, plLocale, nullptr, remFolders, prog);
                verify(!dlg.applyToAllCheckBox()->isHidden(), "Folder ApplyToAll visible when remaining folder conflicts > 0");
                return LocalFolderConflictDecision{
                    LocalMergeApprovedAction::MergeDirectory,
                    LocalMergeApplyPolicy::MergeAllFolders, {}, true};
            },
            [&](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
                return LocalFileConflictDecision{
                    LocalMergeApprovedAction::Skip,
                    LocalMergeApplyPolicy::SkipAllFileConflicts, {}, true};
            });

        verify(res.completed
            && res.folderPromptsShown == 1
            && res.filePromptsShown == 1
            && plan.entries[2].approvedAction == LocalMergeApprovedAction::MergeDirectory
            && plan.entries[3].approvedAction == LocalMergeApprovedAction::MergeDirectory
            && plan.entries[4].approvedAction == LocalMergeApprovedAction::Skip
            && plan.entries[5].approvedAction == LocalMergeApprovedAction::Skip,
               "MergeAllFolders merges all subfolders without auto-overwriting files; SkipAllFileConflicts skips remaining files");
    }

    // Addendum #6: Skipping subfolder skips entire subtree without prompting descendants
    {
        LocalMergePlan plan = makeDeepPlan();
        LocalMergeBackendCapabilities caps;
        QStringList prompted;
        const LocalMergeResolutionResult res = LocalMergeConflictResolver::resolve(
            plan, caps,
            [&](const LocalMergePlanEntry &entry, qsizetype, LocalMergeConflictProgress) {
                prompted.append(entry.relativePath);
                if (entry.relativePath == QStringLiteral("DemoRoot/Account")) {
                    return LocalFolderConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
                }
                return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
            },
            [&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
                prompted.append(entry.relativePath);
                return LocalFileConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
            });

        verify(res.completed
            && res.subtreesSkipped == 1
            && prompted == QStringList({QStringLiteral("DemoRoot"), QStringLiteral("DemoRoot/Account")})
            && plan.entries[2].approvedAction == LocalMergeApprovedAction::Skip
            && plan.entries[3].approvedAction == LocalMergeApprovedAction::Skip
            && plan.entries[4].approvedAction == LocalMergeApprovedAction::Skip
            && plan.entries[5].approvedAction == LocalMergeApprovedAction::Skip,
               "Skipping Account skips SavedVariables and all files inside without prompting");
    }

    // Addendum #7: Renaming subfolder remaps all descendants and flags Stage 3B revalidation
    {
        LocalMergePlan plan = makeDeepPlan();
        LocalMergeBackendCapabilities caps;
        const LocalMergeResolutionResult res = LocalMergeConflictResolver::resolve(
            plan, caps,
            [&](const LocalMergePlanEntry &entry, qsizetype, LocalMergeConflictProgress) {
                if (entry.relativePath == QStringLiteral("DemoRoot/Account")) {
                    return LocalFolderConflictDecision{
                        LocalMergeApprovedAction::Rename, LocalMergeApplyPolicy::None,
                        QStringLiteral("Account (kopia)"), true};
                }
                return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
            },
            [&](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
                return LocalFileConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
            });

        verify(res.completed
            && res.renamedSubtreeEntriesRemapped == 3
            && plan.entries[2].relativePath == QStringLiteral("DemoRoot/Account (kopia)")
            && plan.entries[3].relativePath == QStringLiteral("DemoRoot/Account (kopia)/SavedVariables")
            && plan.entries[4].relativePath == QStringLiteral("DemoRoot/Account (kopia)/SavedVariables/Addon.lua")
            && plan.entries[4].destinationPath == QStringLiteral("/demo/dst/DemoRoot/Account (kopia)/SavedVariables/Addon.lua")
            && res.entriesRequiringStage3BRevalidation.size() == 4,
               "Renaming Account remaps all descendants and records Stage 3B revalidation requirement");
    }

    // Addendum: Cancelling at level 3 stops further prompting immediately
    {
        LocalMergePlan plan = makeDeepPlan();
        LocalMergeBackendCapabilities caps;
        QStringList prompted;
        const LocalMergeResolutionResult res = LocalMergeConflictResolver::resolve(
            plan, caps,
            [&](const LocalMergePlanEntry &entry, qsizetype, LocalMergeConflictProgress) {
                prompted.append(entry.relativePath);
                if (entry.relativePath == QStringLiteral("DemoRoot/Account/SavedVariables")) {
                    return LocalFolderConflictDecision{LocalMergeApprovedAction::Abort, LocalMergeApplyPolicy::None, {}, false};
                }
                return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
            },
            [&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
                prompted.append(entry.relativePath);
                return LocalFileConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
            });

        verify(!res.completed && res.cancelled
            && prompted.size() == 3
            && res.filePromptsShown == 0,
               "Abort at level 3 stops resolver before prompting files");
    }

    // ==================== Stage 3A UX Polish: Scenario G Rename Audit (NTFS + Btrfs) & Suggest Button ====================
    {
        // NTFS file rename
        LocalMergePlan ntfsPlan;
        ntfsPlan.entries = {
            makeFolderConflictEntry(QStringLiteral("Archive"), QStringLiteral("/demo/src/Archive"), QStringLiteral("/mnt/demo_ntfs/Archive")),
            makeFileEntry(QStringLiteral("Archive/Config.ini"), QStringLiteral("/demo/src/Archive/Config.ini"), QStringLiteral("/mnt/demo_ntfs/Archive/Config.ini"), LocalMergeEntryClassification::FileConflict),
        };
        LocalMergeBackendCapabilities ntfsCaps;
        ntfsCaps.canSafeOverwrite = false;
        ntfsCaps.filesystemLabel = QStringLiteral("NTFS-3G (FUSE)");

        const auto resNtfs = LocalMergeConflictResolver::resolve(
            ntfsPlan, ntfsCaps,
            [&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
                return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
            },
            [&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &c, qsizetype rem, LocalMergeConflictProgress prog) {
                LocalFileConflictResolutionDialog dlg(entry, c, rem, {}, plLocale, nullptr, prog);
                dlg.renameLineEdit()->setText(QStringLiteral("custom.ini"));
                dlg.suggestButton()->click();
                verify(dlg.renameLineEdit()->text() == QStringLiteral("Config (kopia).ini"), "Suggest button restores safe copy name");
                dlg.renameButton()->click();
                return dlg.decision();
            });
        verify(resNtfs.completed, "NTFS rename resolve completes");
        const auto ntfsView = resolveEntryDestinationView(ntfsPlan.entries[1]);
        verify(ntfsView.originalConflictDestination == QStringLiteral("/mnt/demo_ntfs/Archive/Config.ini")
            && ntfsView.approvedNewName == QStringLiteral("Config (kopia).ini")
            && ntfsView.effectiveDestination == QStringLiteral("/mnt/demo_ntfs/Archive/Config (kopia).ini"),
               "Scenario G Rename audit: originalConflictDestination preserved, approvedNewName stored, effectiveDestination computed accurately on NTFS");

        // Btrfs file rename
        LocalMergePlan btrfsPlan;
        btrfsPlan.entries = {
            makeFileEntry(QStringLiteral("Report.txt"), QStringLiteral("/demo/src/Report.txt"), QStringLiteral("/demo/dst/Report.txt"), LocalMergeEntryClassification::FileConflict),
        };
        LocalMergeBackendCapabilities btrfsCaps;
        btrfsCaps.canSafeOverwrite = true;
        btrfsCaps.filesystemLabel = QStringLiteral("Btrfs");
        const auto resBtrfs = LocalMergeConflictResolver::resolve(
            btrfsPlan, btrfsCaps,
            [](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) { return LocalFolderConflictDecision{}; },
            [&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &c, qsizetype rem, LocalMergeConflictProgress prog) {
                LocalFileConflictResolutionDialog dlg(entry, c, rem, {}, enLocale, nullptr, prog);
                verify(dlg.conflictTypeLabel()->text().contains(QStringLiteral("different sizes")),
                       "Comparison state reports different sizes honestly");
                dlg.renameButton()->click();
                return dlg.decision();
            });
        verify(resBtrfs.completed, "Btrfs rename resolve completes");
        const auto btrfsView = resolveEntryDestinationView(btrfsPlan.entries[0]);
        verify(btrfsView.originalConflictDestination == QStringLiteral("/demo/dst/Report.txt")
            && btrfsView.approvedNewName == QStringLiteral("Report (copy).txt")
            && btrfsView.effectiveDestination == QStringLiteral("/demo/dst/Report (copy).txt"),
               "Btrfs file rename computes effectiveDestination accurately");
    }

    // ==================== Stage 3A Final UX Fixes: H-DETAILS-01, UX-ICONS, UX-METADATA, UX-NTFS ====================
    {
        // H-DETAILS-01: collapsed -> expanded -> collapsed (1, 10, 20 cycles) on BOTH dialogs with long unbroken paths
        const QString longSeg = QStringLiteral("bardzo_długi_katalog_roboczy_").repeated(8);
        const auto longFile = makeFileEntry(
            QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_1.txt"),
            QStringLiteral("/demo/źródło/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_1.txt"),
            QStringLiteral("/demo/cel/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_1.txt"),
            LocalMergeEntryClassification::FileConflict);
        LocalMergeBackendCapabilities ntfsCaps;
        ntfsCaps.canSafeOverwrite = false;
        ntfsCaps.filesystemLabel = QStringLiteral("NTFS-3G (FUSE)");

        LocalFileConflictResolutionDialog fileDlg(longFile, ntfsCaps, 4, {}, plLocale);
        fileDlg.show();
        QApplication::processEvents();
        const QSize initFileSize = fileDlg.size();
        verify(initFileSize.width() <= 760, "Initial file dialog width stays compact");

        for (int cycle = 1; cycle <= 20; ++cycle) {
            fileDlg.detailsToggleButton()->setChecked(true);
            QApplication::processEvents();
            verify(fileDlg.width() == initFileSize.width(), "Expanded Details does not bloat file dialog width");
            verify(!fileDlg.detailsFrame()->isHidden(), "Details visible when expanded");

            fileDlg.detailsToggleButton()->setChecked(false);
            QApplication::processEvents();
            if (cycle == 1 || cycle == 10 || cycle == 20) {
                verify(fileDlg.size() == initFileSize && fileDlg.detailsFrame()->isHidden(),
                       "File dialog restores exact compact size after 1/10/20 collapse cycles");
            }
        }

        const auto longFolder = makeFolderConflictEntry(
            QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń"),
            QStringLiteral("/demo/źródło/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń"),
            QStringLiteral("/demo/cel/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń"));
        LocalFolderMergeConflictDialog folderDlg(longFolder, {}, false, plLocale);
        folderDlg.show();
        QApplication::processEvents();
        const QSize initFolderSize = folderDlg.size();
        for (int cycle = 1; cycle <= 20; ++cycle) {
            folderDlg.detailsToggleButton()->setChecked(true);
            QApplication::processEvents();
            verify(folderDlg.width() == initFolderSize.width(), "Expanded Details does not bloat folder dialog width");
            folderDlg.detailsToggleButton()->setChecked(false);
            QApplication::processEvents();
            if (cycle == 1 || cycle == 10 || cycle == 20) {
                verify(folderDlg.size() == initFolderSize,
                       "Folder dialog restores exact compact size after 1/10/20 collapse cycles");
            }
        }

        // UX-ICONS: .ini icon resolves cleanly without null fallback
        const QIcon iniIcon = LocalMergeDialogDetail::iconForMergeEntry(
            LocalMergeObjectKind::RegularFile, QStringLiteral("Config.ini"), app.style());
        verify(!iniIcon.isNull() && !iniIcon.pixmap(36, 36).isNull(), "Config.ini resolves non-null KDE document icon");

        // UX-METADATA: folder with no size and no date hides "— • —"; folder with only date omits separator
        const auto noMetaFolder = makeFolderConflictEntry(
            QStringLiteral("EmptyMeta"), QStringLiteral("/s/EmptyMeta"), QStringLiteral("/d/EmptyMeta"));
        LocalFolderMergeConflictDialog noMetaDlg(noMetaFolder, {}, false, plLocale);
        verify(noMetaDlg.findChildren<QLabel *>(QStringLiteral("comparisonMetaLabel")).isEmpty(),
               "Folder with neither size nor date hides metadata row (no '— • —')");

        auto dateOnlyFolder = noMetaFolder;
        dateOnlyFolder.sourceStatValid = true;
        dateOnlyFolder.sourceStat.st_mtim.tv_sec = 1760000000;
        LocalFolderMergeConflictDialog dateOnlyDlg(dateOnlyFolder, {}, false, plLocale);
        const auto metaLabels = dateOnlyDlg.findChildren<QLabel *>(QStringLiteral("comparisonMetaLabel"));
        verify(metaLabels.size() == 1 && !metaLabels.first()->text().contains(QStringLiteral("•")),
               "Folder with only date shows date without separator");

        // UX-NTFS: concise main notice + full reason in tooltip
        verify(fileDlg.safetyLabel()->text().contains(QStringLiteral("Zastępowanie niedostępne na tym systemie plików"))
            && fileDlg.overwriteButton()->toolTip().contains(QStringLiteral("atomowej wymiany")),
               "UX-NTFS shows concise main notice and full reason in tooltip");
    }

    // 20: Confirm guard directory is completely untouched
    verify(QDir(guardTmp.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot) == QStringList({QStringLiteral("untouched.txt")}),
           "Dialogs performed zero filesystem operations");

    qInfo("PASS: %d Stage 3A+FinalUXFixes LocalMergeDialogs GUI assertions verified", checks);
    return 0;
}
