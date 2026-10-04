/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "duplicateactioncontroller.h"
#include "duplicateactiondata.h"
#include "duplicatefinderdata.h"
#include "duplicatefinderjob.h"
#include "duplicategroupmodel.h"
#include "fileactions.h"
#include "storagescandata.h"
#include "storagescandialog.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QLabel>
#include <QPushButton>

#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

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

static DuplicatePhysicalFile createPhysicalFile(const QString &path, quint64 size, quint64 dev, quint64 ino,
                                                const QStringList &aliases = {}, qint64 mtime = 1000, qint64 mtimeNs = 0,
                                                qint64 ctime = 1000, qint64 ctimeNs = 0)
{
    DuplicatePhysicalFile f;
    f.canonicalPath = path;
    f.name = path.section(QLatin1Char('/'), -1);
    f.logicalSize = size;
    f.allocatedSize = size == 0 ? 0 : ((size + 4095) / 4096) * 4096;
    f.deviceId = dev;
    f.inode = ino;
    f.linkCount = static_cast<quint32>(aliases.size() + 1);
    f.aliasPaths = aliases;
    f.mtimeSec = mtime;
    f.mtimeNsec = mtimeNs;
    f.ctimeSec = ctime;
    f.ctimeNsec = ctimeNs;
    f.relativePath = path.section(QLatin1Char('/'), 0, -2);
    return f;
}

static DuplicatePhysicalFile createPhysicalFromStat(const QString &path, const struct stat &st,
                                                    const QStringList &aliases = {})
{
    DuplicatePhysicalFile f;
    f.canonicalPath = path;
    f.name = path.section(QLatin1Char('/'), -1);
    f.logicalSize = st.st_size;
    f.allocatedSize = st.st_size == 0 ? 0 : static_cast<quint64>(st.st_blocks) * 512ULL;
    f.deviceId = st.st_dev;
    f.inode = st.st_ino;
    f.linkCount = static_cast<quint32>(aliases.size() + 1);
    f.aliasPaths = aliases;
    f.mtimeSec = st.st_mtim.tv_sec;
    f.mtimeNsec = st.st_mtim.tv_nsec;
    f.ctimeSec = st.st_ctim.tv_sec;
    f.ctimeNsec = st.st_ctim.tv_nsec;
    f.relativePath = path.section(QLatin1Char('/'), 0, -2);
    return f;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "temp directory valid");
    const QString root = tempDir.path();

    // =========================================================================
    // 1. Model defaults & empty state
    // =========================================================================
    {
        DuplicateGroupModel model;
        verify(model.rowCount() == 0, "1. empty model rowCount 0");
        verify(model.columnCount() == 6, "1. empty model columnCount 6");
        verify(model.selectedCount() == 0, "1. empty model selectedCount 0");
        verify(model.selectedLogicalBytes() == 0, "1. empty model selectedLogicalBytes 0");
        verify(model.selectedAllocatedBytes() == 0, "1. empty model selectedAllocatedBytes 0");
        verify(model.selectedActionItems().isEmpty(), "1. empty model selectedActionItems empty");

        // Clear selection on empty is a safe no-op
        model.clearSelection();
        verify(model.selectedCount() == 0, "1. clearSelection on empty is safe");
    }

    // =========================================================================
    // 2. Model selection API, tri-state and metrics
    // =========================================================================
    {
        DuplicateGroupModel model;
        DuplicateFinderResult result;
        DuplicateGroup grp1;
        grp1.sha256 = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        grp1.logicalSize = 100;
        grp1.files.append(createPhysicalFile(root + QStringLiteral("/a.txt"), 100, 1, 101));
        grp1.files.append(createPhysicalFile(root + QStringLiteral("/b.txt"), 100, 1, 102));
        grp1.files.append(createPhysicalFile(root + QStringLiteral("/c.txt"), 100, 1, 103));

        DuplicateGroup grp2;
        grp2.sha256 = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        grp2.logicalSize = 500;
        grp2.files.append(createPhysicalFile(root + QStringLiteral("/d.bin"), 500, 1, 201));
        grp2.files.append(createPhysicalFile(root + QStringLiteral("/e.bin"), 500, 1, 202));

        result.groups.append(grp1);
        result.groups.append(grp2);

        QSignalSpy spySelection(&model, &DuplicateGroupModel::selectionChanged);
        model.setResult(result);
        verify(spySelection.count() == 1, "2. setResult emits selectionChanged");
        verify(model.rowCount() == 2, "2. model has 2 groups");
        verify(model.rowCount(model.index(0, 0)) == 3, "2. group 0 has 3 members");
        verify(model.rowCount(model.index(1, 0)) == 2, "2. group 1 has 2 members");

        // Group check state starts Unchecked
        QModelIndex g0 = model.index(0, 0);
        QModelIndex g1 = model.index(1, 0);
        verify(model.data(g0, Qt::CheckStateRole).toInt() == Qt::Unchecked, "2. group 0 unchecked initially");
        verify(model.data(g1, Qt::CheckStateRole).toInt() == Qt::Unchecked, "2. group 1 unchecked initially");

        // Check first member of group 0
        QModelIndex m0_0 = model.index(0, 0, g0);
        QModelIndex m0_1 = model.index(1, 0, g0);
        QModelIndex m0_2 = model.index(2, 0, g0);
        verify(model.data(m0_0, Qt::CheckStateRole).toInt() == Qt::Unchecked, "2. member 0_0 unchecked initially");

        model.setData(m0_0, Qt::Checked, Qt::CheckStateRole);
        verify(model.data(m0_0, Qt::CheckStateRole).toInt() == Qt::Checked, "2. member 0_0 checked");
        verify(model.data(g0, Qt::CheckStateRole).toInt() == Qt::PartiallyChecked, "2. group 0 partially checked");
        verify(model.selectedCount() == 1, "2. selectedCount is 1");
        verify(model.selectedLogicalBytes() == 100, "2. selectedLogicalBytes is 100");

        // Check second member of group 0
        model.setData(m0_1, Qt::Checked, Qt::CheckStateRole);
        verify(model.data(g0, Qt::CheckStateRole).toInt() == Qt::PartiallyChecked, "2. group 0 still partially checked");
        verify(model.selectedCount() == 2, "2. selectedCount is 2");

        // Check third member of group 0 -> group becomes Checked
        model.setData(m0_2, Qt::Checked, Qt::CheckStateRole);
        verify(model.data(g0, Qt::CheckStateRole).toInt() == Qt::Checked, "2. group 0 fully checked");
        verify(model.selectedCount() == 3, "2. selectedCount is 3");
        verify(model.selectedLogicalBytes() == 300, "2. selectedLogicalBytes is 300");

        // Toggle entire group 1 via group row
        model.setData(g1, Qt::Checked, Qt::CheckStateRole);
        verify(model.data(g1, Qt::CheckStateRole).toInt() == Qt::Checked, "2. group 1 checked");
        QModelIndex m1_0 = model.index(0, 0, g1);
        QModelIndex m1_1 = model.index(1, 0, g1);
        verify(model.data(m1_0, Qt::CheckStateRole).toInt() == Qt::Checked, "2. member 1_0 checked");
        verify(model.data(m1_1, Qt::CheckStateRole).toInt() == Qt::Checked, "2. member 1_1 checked");
        verify(model.selectedCount() == 5, "2. selectedCount is 5");
        verify(model.selectedLogicalBytes() == 300 + 1000, "2. selectedLogicalBytes is 1300");

        // Uncheck group 1 via group row
        model.setData(g1, Qt::Unchecked, Qt::CheckStateRole);
        verify(model.data(g1, Qt::CheckStateRole).toInt() == Qt::Unchecked, "2. group 1 unchecked");
        verify(model.data(m1_0, Qt::CheckStateRole).toInt() == Qt::Unchecked, "2. member 1_0 unchecked");
        verify(model.data(m1_1, Qt::CheckStateRole).toInt() == Qt::Unchecked, "2. member 1_1 unchecked");
        verify(model.selectedCount() == 3, "2. selectedCount back to 3");

        // clearSelection()
        model.clearSelection();
        verify(model.selectedCount() == 0, "2. clearSelection zeroes count");
        verify(model.selectedLogicalBytes() == 0, "2. clearSelection zeroes bytes");
        verify(model.data(g0, Qt::CheckStateRole).toInt() == Qt::Unchecked, "2. group 0 unchecked after clearSelection");
        verify(model.data(m0_0, Qt::CheckStateRole).toInt() == Qt::Unchecked, "2. member 0_0 unchecked after clearSelection");
    }

    // =========================================================================
    // 3. Strict Preflight Revalidation - All Valid
    // =========================================================================
    {
        const QString p1 = root + QStringLiteral("/valid1.txt");
        const QString p2 = root + QStringLiteral("/valid2.txt");
        writeFile(p1, "hello duplicates 1");
        writeFile(p2, "hello duplicates 2");

        struct stat st1 {}, st2 {};
        verify(::lstat(QFile::encodeName(p1).constData(), &st1) == 0, "3. lstat valid1");
        verify(::lstat(QFile::encodeName(p2).constData(), &st2) == 0, "3. lstat valid2");

        DuplicatePhysicalFile f1 = createPhysicalFromStat(p1, st1);
        DuplicatePhysicalFile f2 = createPhysicalFromStat(p2, st2);

        DuplicateGroup grp;
        grp.logicalSize = st1.st_size;
        grp.files = {f1, f2};

        QList<DuplicateActionItem> items;
        DuplicateActionItem it1;
        it1.deviceId = f1.deviceId;
        it1.inode = f1.inode;
        it1.targetPath = f1.canonicalPath;
        it1.fileSnapshot = f1;
        it1.groupIndex = 0;
        items.append(it1);

        const auto report = DuplicateReviewValidator::revalidate(items, {grp});
        verify(report.allValid(), "3. report allValid is true");
        verify(report.validItems.size() == 1, "3. 1 valid item");
        verify(report.staleItems.isEmpty(), "3. 0 stale items");
        verify(report.disappearedItems.isEmpty(), "3. 0 disappeared items");
        verify(report.hasAllCopiesWarning == false, "3. only 1 of 2 selected -> no all copies warning");
        verify(report.validLogicalBytes == static_cast<quint64>(st1.st_size), "3. validLogicalBytes matches");
    }

    // =========================================================================
    // 4. Strict Preflight Revalidation - Disappeared file
    // =========================================================================
    {
        const QString p1 = root + QStringLiteral("/missing_file.txt");
        DuplicatePhysicalFile f1 = createPhysicalFile(p1, 50, 1, 999);

        DuplicateGroup grp;
        grp.logicalSize = 50;
        grp.files = {f1, createPhysicalFile(root + QStringLiteral("/other.txt"), 50, 1, 1000)};

        DuplicateActionItem it1;
        it1.deviceId = f1.deviceId;
        it1.inode = f1.inode;
        it1.targetPath = f1.canonicalPath;
        it1.fileSnapshot = f1;
        it1.groupIndex = 0;

        const auto report = DuplicateReviewValidator::revalidate({it1}, {grp});
        verify(!report.allValid(), "4. disappeared file makes allValid false");
        verify(report.validItems.isEmpty(), "4. 0 valid items");
        verify(report.disappearedItems.size() == 1, "4. 1 disappeared item");
        verify(report.disappearedItems.first().status == DuplicateRevalidationStatus::Disappeared, "4. status Disappeared");
    }

    // =========================================================================
    // 5. Strict Preflight Revalidation - Changed File Type (replaced by directory)
    // =========================================================================
    {
        const QString dirPath = root + QStringLiteral("/changed_to_dir");
        QDir().mkdir(dirPath);

        DuplicatePhysicalFile f1 = createPhysicalFile(dirPath, 100, 1, 1234);
        DuplicateGroup grp;
        grp.files = {f1, createPhysicalFile(root + QStringLiteral("/other2.txt"), 100, 1, 1235)};

        DuplicateActionItem it1;
        it1.targetPath = dirPath;
        it1.fileSnapshot = f1;
        it1.groupIndex = 0;

        const auto report = DuplicateReviewValidator::revalidate({it1}, {grp});
        verify(!report.allValid(), "5. type change makes allValid false");
        verify(report.staleItems.size() == 1, "5. 1 stale item");
        verify(report.staleItems.first().status == DuplicateRevalidationStatus::ChangedType, "5. status ChangedType");
    }

    // =========================================================================
    // 6. Strict Preflight Revalidation - Changed Inode / Device
    // =========================================================================
    {
        const QString p1 = root + QStringLiteral("/inode_change.txt");
        writeFile(p1, "original");
        struct stat st {};
        verify(::lstat(QFile::encodeName(p1).constData(), &st) == 0, "6. lstat inode_change");

        // Snapshot recorded with wrong inode
        DuplicatePhysicalFile f1 = createPhysicalFromStat(p1, st);
        f1.inode = st.st_ino + 999; // intentionally mismatched inode

        DuplicateGroup grp;
        grp.files = {f1, createPhysicalFile(root + QStringLiteral("/other3.txt"), st.st_size, st.st_dev, 888)};

        DuplicateActionItem it1;
        it1.targetPath = p1;
        it1.fileSnapshot = f1;
        it1.groupIndex = 0;

        const auto report = DuplicateReviewValidator::revalidate({it1}, {grp});
        verify(!report.allValid(), "6. inode mismatch makes allValid false");
        verify(report.staleItems.size() == 1, "6. 1 stale item");
        verify(report.staleItems.first().status == DuplicateRevalidationStatus::ChangedInode, "6. status ChangedInode");
    }

    // =========================================================================
    // 7. Strict Preflight Revalidation - Changed Size
    // =========================================================================
    {
        const QString p1 = root + QStringLiteral("/size_change.txt");
        writeFile(p1, "short");
        struct stat st {};
        verify(::lstat(QFile::encodeName(p1).constData(), &st) == 0, "7. lstat size_change");

        // Overwrite file with different size
        writeFile(p1, "much longer content now");

        DuplicatePhysicalFile f1 = createPhysicalFromStat(p1, st); // has old size
        DuplicateGroup grp;
        grp.files = {f1, createPhysicalFile(root + QStringLiteral("/other4.txt"), st.st_size, st.st_dev, 777)};

        DuplicateActionItem it1;
        it1.targetPath = p1;
        it1.fileSnapshot = f1;
        it1.groupIndex = 0;

        const auto report = DuplicateReviewValidator::revalidate({it1}, {grp});
        verify(!report.allValid(), "7. size change detected");
        verify(report.staleItems.size() == 1, "7. 1 stale item");
        verify(report.staleItems.first().status == DuplicateRevalidationStatus::ChangedSize, "7. status ChangedSize");
    }

    // =========================================================================
    // 8. Strict Preflight Revalidation - Nanosecond mtime / ctime change
    // =========================================================================
    {
        const QString p1 = root + QStringLiteral("/time_change.txt");
        writeFile(p1, "exact same bytes");
        struct stat st {};
        verify(::lstat(QFile::encodeName(p1).constData(), &st) == 0, "8. lstat time_change");

        // Snapshot with slightly different mtimeNsec
        DuplicatePhysicalFile f1 = createPhysicalFromStat(p1, st);
        f1.mtimeNsec = st.st_mtim.tv_nsec + 500; // mismatch in nanoseconds

        DuplicateGroup grp;
        grp.files = {f1, createPhysicalFile(root + QStringLiteral("/other5.txt"), st.st_size, st.st_dev, 666)};

        DuplicateActionItem it1;
        it1.targetPath = p1;
        it1.fileSnapshot = f1;
        it1.groupIndex = 0;

        const auto report = DuplicateReviewValidator::revalidate({it1}, {grp});
        verify(!report.allValid(), "8. nanosecond mtime change detected");
        verify(report.staleItems.size() == 1, "8. 1 stale item");
        verify(report.staleItems.first().status == DuplicateRevalidationStatus::ChangedMtime, "8. status ChangedMtime");
    }

    // =========================================================================
    // 9. All-copies Warning Detection
    // =========================================================================
    {
        const QString p1 = root + QStringLiteral("/warn1.txt");
        const QString p2 = root + QStringLiteral("/warn2.txt");
        writeFile(p1, "duplicate copy");
        writeFile(p2, "duplicate copy");

        struct stat st1 {}, st2 {};
        verify(::lstat(QFile::encodeName(p1).constData(), &st1) == 0, "9. lstat warn1");
        verify(::lstat(QFile::encodeName(p2).constData(), &st2) == 0, "9. lstat warn2");

        DuplicatePhysicalFile f1 = createPhysicalFromStat(p1, st1);
        DuplicatePhysicalFile f2 = createPhysicalFromStat(p2, st2);

        DuplicateGroup grp;
        grp.files = {f1, f2};

        // Select BOTH physical copies
        DuplicateActionItem it1 {f1.deviceId, f1.inode, p1, f1, 0};
        DuplicateActionItem it2 {f2.deviceId, f2.inode, p2, f2, 0};

        const auto reportAll = DuplicateReviewValidator::revalidate({it1, it2}, {grp});
        verify(reportAll.hasAllCopiesWarning == true, "9. all copies warning triggered when all members selected");
        verify(reportAll.groupsWithAllCopiesSelected.contains(0), "9. group 0 marked in all copies set");

        // Select only ONE physical copy
        const auto reportOne = DuplicateReviewValidator::revalidate({it1}, {grp});
        verify(reportOne.hasAllCopiesWarning == false, "9. all copies warning NOT triggered when 1 copy retained");
    }

    // =========================================================================
    // 10. Confirmation Dialog Widget Verification
    // =========================================================================
    {
        const QString p1 = root + QStringLiteral("/dlg1.txt");
        writeFile(p1, "dialog test");
        struct stat st1 {};
        verify(::lstat(QFile::encodeName(p1).constData(), &st1) == 0, "10. lstat dlg1");
        DuplicatePhysicalFile f1 = createPhysicalFromStat(p1, st1);

        DuplicateGroup grp;
        grp.files = {f1};

        DuplicateActionItem it1 {f1.deviceId, f1.inode, p1, f1, 0};
        const auto report = DuplicateReviewValidator::revalidate({it1}, {grp});

        // Dialog for Trash
        DuplicateConfirmationDialog trashDlg(nullptr, DuplicateConfirmationDialog::ActionType::Trash, report);
        verify(trashDlg.windowTitle().contains(QStringLiteral("Kosz")) || trashDlg.windowTitle().contains(QStringLiteral("Trash")), "10. trash dialog title contains Kosz or Trash");
        verify(trashDlg.allCopiesWarningLabel() != nullptr, "10. all copies warning label present when 1/1 selected");
        verify(trashDlg.itemsListWidget() != nullptr, "10. items list widget present");
        verify(trashDlg.itemsListWidget()->count() == 1, "10. items list has 1 entry");

        // Dialog for Move
        DuplicateConfirmationDialog moveDlg(nullptr, DuplicateConfirmationDialog::ActionType::Move, report, QStringLiteral("/tmp/dest"));
        verify(moveDlg.windowTitle().contains(QStringLiteral("przeniesienia")) || moveDlg.windowTitle().contains(QStringLiteral("Move")), "10. move dialog title matches");
    }

    // =========================================================================
    // 11. Hardlink Canonical Promotion on Trash
    // =========================================================================
    {
        DuplicateGroupModel model;
        DuplicateFinderResult res;
        DuplicateGroup grp;
        grp.sha256 = QStringLiteral("hardlink_hash");
        grp.logicalSize = 256;

        // Physical file 1 has canonical /hl_b.txt and aliases /hl_a.txt, /hl_c.txt
        DuplicatePhysicalFile f1 = createPhysicalFile(root + QStringLiteral("/hl_b.txt"), 256, 1, 555,
            {root + QStringLiteral("/hl_c.txt"), root + QStringLiteral("/hl_a.txt")});

        // Physical file 2 (independent inode)
        DuplicatePhysicalFile f2 = createPhysicalFile(root + QStringLiteral("/hl_other.txt"), 256, 1, 777);

        grp.files = {f1, f2};
        res.groups = {grp};
        model.setResult(res);

        // Select canonical /hl_b.txt
        model.setSelected(f1, true);
        verify(model.selectedCount() == 1, "11. f1 selected");

        // Perform trash on /hl_b.txt
        QSet<QString> trashed;
        trashed.insert(root + QStringLiteral("/hl_b.txt"));
        model.applySuccessfulTrash(trashed);

        // Group must still exist because 2 physical objects remain (f1 promoted alias + f2)
        verify(model.groups().size() == 1, "11. group retained after hardlink promotion");
        const auto &updatedF1 = model.groups().first().files.first();
        // Lowest alphabetical alias was /hl_a.txt -> promoted to canonicalPath!
        verify(updatedF1.canonicalPath == root + QStringLiteral("/hl_a.txt"), "11. /hl_a.txt promoted to canonical");
        verify(updatedF1.aliasPaths == QStringList{root + QStringLiteral("/hl_c.txt")}, "11. remaining alias is /hl_c.txt");
        verify(model.selectedCount() == 0, "11. selection cleared after trash");

        // Now trash /hl_a.txt (the new canonical)
        trashed.clear();
        trashed.insert(root + QStringLiteral("/hl_a.txt"));
        model.applySuccessfulTrash(trashed);

        // /hl_c.txt should now be promoted!
        verify(model.groups().size() == 1, "11. group still retained");
        const auto &updatedF1_c = model.groups().first().files.first();
        verify(updatedF1_c.canonicalPath == root + QStringLiteral("/hl_c.txt"), "11. /hl_c.txt promoted to canonical");
        verify(updatedF1_c.aliasPaths.isEmpty(), "11. no more aliases");

        // Now trash /hl_c.txt -> no aliases left, so f1 is completely gone!
        // That leaves only f2 (< 2 physical files), so the entire group collapses!
        trashed.clear();
        trashed.insert(root + QStringLiteral("/hl_c.txt"));
        model.applySuccessfulTrash(trashed);

        verify(model.groups().isEmpty(), "11. group collapsed after last copy removed");
        verify(model.rowCount() == 0, "11. model rowCount 0 after collapse");
    }

    // =========================================================================
    // 12. Move within Scan Root updates path and relativePath
    // =========================================================================
    {
        DuplicateGroupModel model;
        DuplicateFinderResult res;
        DuplicateGroup grp;
        grp.logicalSize = 300;

        DuplicatePhysicalFile f1 = createPhysicalFile(root + QStringLiteral("/sub1/file1.dat"), 300, 1, 1001);
        DuplicatePhysicalFile f2 = createPhysicalFile(root + QStringLiteral("/sub2/file2.dat"), 300, 1, 1002);
        grp.files = {f1, f2};
        res.groups = {grp};
        model.setResult(res);

        // Move sub1/file1.dat to sub3/file1.dat (both within root)
        QHash<QString, QString> moved;
        moved.insert(root + QStringLiteral("/sub1/file1.dat"), root + QStringLiteral("/sub3/file1.dat"));

        model.setSelected(f1, true);
        model.applySuccessfulMove(moved, root);

        verify(model.groups().size() == 1, "12. group preserved after move within scan root");
        const auto &updated = model.groups().first().files.first();
        verify(updated.canonicalPath == root + QStringLiteral("/sub3/file1.dat"), "12. canonical path updated");
        verify(updated.name == QStringLiteral("file1.dat"), "12. name preserved");
        verify(model.selectedCount() == 0, "12. selection cleared after move");
    }

    // =========================================================================
    // 13. Move outside Scan Root removes item from scope
    // =========================================================================
    {
        DuplicateGroupModel model;
        DuplicateFinderResult res;
        DuplicateGroup grp;
        grp.logicalSize = 300;

        DuplicatePhysicalFile f1 = createPhysicalFile(root + QStringLiteral("/file1.dat"), 300, 1, 2001);
        DuplicatePhysicalFile f2 = createPhysicalFile(root + QStringLiteral("/file2.dat"), 300, 1, 2002);
        grp.files = {f1, f2};
        res.groups = {grp};
        model.setResult(res);

        // Move file1.dat to /opt/archive/file1.dat (outside root)
        QHash<QString, QString> moved;
        moved.insert(root + QStringLiteral("/file1.dat"), QStringLiteral("/opt/archive/file1.dat"));

        model.setSelected(f1, true);
        model.applySuccessfulMove(moved, root);

        // Since f1 was removed, only f2 remains (< 2 members) -> group collapses!
        verify(model.groups().isEmpty(), "13. group collapses when moved outside scan root");
    }

    // =========================================================================
    // 14. Controller Execution & Cancellation Handling
    // =========================================================================
    {
        DuplicateGroupModel model;
        DuplicateFinderResult res;
        DuplicateGroup grp;

        const QString p1 = root + QStringLiteral("/ctrl1.txt");
        const QString p2 = root + QStringLiteral("/ctrl2.txt");
        writeFile(p1, "ctrl content");
        writeFile(p2, "ctrl content");
        struct stat st1 {}, st2 {};
        ::lstat(QFile::encodeName(p1).constData(), &st1);
        ::lstat(QFile::encodeName(p2).constData(), &st2);

        DuplicatePhysicalFile f1 = createPhysicalFromStat(p1, st1);
        DuplicatePhysicalFile f2 = createPhysicalFromStat(p2, st2);
        grp.files = {f1, f2};
        res.groups = {grp};
        model.setResult(res);

        DuplicateActionController controller(nullptr);

        // Empty selection check
        bool called = false;
        controller.executeTrash(&model, root, [&](bool ok, const QString &) {
            called = true;
        });
        verify(!called, "14. executeTrash with empty selection returns without running");

        // Select f1
        model.setSelected(f1, true);

        // Test confirmation reject (Cancel clicked)
        controller.setConfirmationHandler([](DuplicateConfirmationDialog::ActionType,
                                             const DuplicateRevalidationReport &,
                                             const QString &) {
            return false; // simulated User Cancel
        });

        bool finishCalled = false;
        bool finishSuccess = true;
        controller.executeTrash(&model, root, [&](bool ok, const QString &) {
            finishCalled = true;
            finishSuccess = ok;
        });
        verify(finishCalled && !finishSuccess, "14. confirmation rejection cancels trash");
        verify(model.groups().size() == 1, "14. model groups untouched on cancel");
        verify(model.selectedCount() == 1, "14. selection untouched on cancel");

        // Test custom TrashExecutor success
        controller.setConfirmationHandler([](DuplicateConfirmationDialog::ActionType,
                                             const DuplicateRevalidationReport &,
                                             const QString &) {
            return true; // simulated User Accept
        });

        QList<QUrl> executedUrls;
        controller.setTrashExecutor([&](const QList<QUrl> &urls,
                                       std::function<void(const DuplicateActionController::OperationResult &)> cb) {
            executedUrls = urls;
            DuplicateActionController::OperationResult r;
            for (const auto &u : urls) {
                r.successfulPaths.insert(u.toLocalFile());
            }
            cb(r);
        });

        finishCalled = false;
        finishSuccess = false;
        controller.executeTrash(&model, root, [&](bool ok, const QString &) {
            finishCalled = true;
            finishSuccess = ok;
        });
        verify(finishCalled && finishSuccess, "14. mock trash executor succeeded");
        verify(executedUrls.size() == 1, "14. 1 url executed");
        verify(executedUrls.first().toLocalFile() == p1, "14. correct url passed to executor");
        // Group collapses because 1 physical file remaining
        verify(model.groups().isEmpty(), "14. group collapsed after trash");
    }

    // =========================================================================
    // 15. Controller Move Self-Destination Protection
    // =========================================================================
    {
        DuplicateGroupModel model;
        DuplicateFinderResult res;
        DuplicateGroup grp;

        const QString p1 = root + QStringLiteral("/self1.txt");
        const QString p2 = root + QStringLiteral("/self2.txt");
        writeFile(p1, "self test");
        writeFile(p2, "self test");
        struct stat st1 {}, st2 {};
        ::lstat(QFile::encodeName(p1).constData(), &st1);
        ::lstat(QFile::encodeName(p2).constData(), &st2);

        DuplicatePhysicalFile f1 = createPhysicalFromStat(p1, st1);
        DuplicatePhysicalFile f2 = createPhysicalFromStat(p2, st2);
        grp.files = {f1, f2};
        res.groups = {grp};
        model.setResult(res);

        DuplicateActionController controller(nullptr);
        model.setSelected(f1, true);

        // Choose same directory as destination
        controller.setDestinationChooser([&](const QString &) {
            return root; // same directory!
        });

        bool executed = false;
        controller.setMoveExecutor([&](const QList<QUrl> &, const QUrl &,
                                      std::function<void(const DuplicateActionController::OperationResult &)>) {
            executed = true;
        });

        controller.executeMove(&model, root);
        verify(!executed, "15. move to self directory rejected and not dispatched");
    }

    // =========================================================================
    // 16. StorageScanDialog Review Bar & UI Wiring
    // =========================================================================
    {
        StorageScanDialog dlg(QUrl::fromLocalFile(root), nullptr);
        verify(dlg.dupSelectedLabel() != nullptr, "16. dupSelectedLabel exists");
        verify(dlg.dupClearSelButton() != nullptr, "16. dupClearSelButton exists");
        verify(dlg.dupTrashButton() != nullptr, "16. dupTrashButton exists");
        verify(dlg.dupMoveButton() != nullptr, "16. dupMoveButton exists");

        // Action buttons start disabled
        verify(!dlg.dupClearSelButton()->isEnabled(), "16. clear button disabled initially");
        verify(!dlg.dupTrashButton()->isEnabled(), "16. trash button disabled initially");
        verify(!dlg.dupMoveButton()->isEnabled(), "16. move button disabled initially");
        verify(dlg.dupSelectedLabel()->text().contains(QStringLiteral("0")), "16. label shows 0 files");

        // Populate model with a group
        DuplicateFinderResult res;
        DuplicateGroup grp;
        grp.logicalSize = 400;
        DuplicatePhysicalFile f1 = createPhysicalFile(root + QStringLiteral("/ui1.txt"), 400, 1, 301);
        DuplicatePhysicalFile f2 = createPhysicalFile(root + QStringLiteral("/ui2.txt"), 400, 1, 302);
        grp.files = {f1, f2};
        res.groups = {grp};
        dlg.duplicatesModel()->setResult(res);

        // Buttons still disabled because 0 selected
        verify(!dlg.dupTrashButton()->isEnabled(), "16. trash button disabled with 0 selection");

        // Select an item
        dlg.duplicatesModel()->setSelected(f1, true);
        dlg.duplicatesModel()->dataChanged(dlg.duplicatesModel()->index(0, 0), dlg.duplicatesModel()->index(0, 0));
        Q_EMIT dlg.duplicatesModel()->selectionChanged();

        verify(dlg.dupTrashButton()->isEnabled(), "16. trash button enabled when item selected");
        verify(dlg.dupMoveButton()->isEnabled(), "16. move button enabled when item selected");
        verify(dlg.dupClearSelButton()->isEnabled(), "16. clear button enabled when item selected");

        // Click clear button
        dlg.dupClearSelButton()->click();
        verify(dlg.duplicatesModel()->selectedCount() == 0, "16. clear button cleared model selection");
        verify(!dlg.dupTrashButton()->isEnabled(), "16. trash button disabled after clear");
    }

    // =========================================================================
    // 17. Real Live FileActions & UndoController Integration
    // =========================================================================
    {
        QTemporaryDir liveDir;
        verify(liveDir.isValid(), "17. live temp dir valid");
        const QString liveRoot = liveDir.path();

        const QString p1 = liveRoot + QStringLiteral("/live1.txt");
        const QString p2 = liveRoot + QStringLiteral("/live2.txt");
        const QString p3 = liveRoot + QStringLiteral("/live3.txt");
        writeFile(p1, "live duplicate content");
        writeFile(p2, "live duplicate content");
        writeFile(p3, "live duplicate content");

        struct stat st1 {}, st2 {}, st3 {};
        ::lstat(QFile::encodeName(p1).constData(), &st1);
        ::lstat(QFile::encodeName(p2).constData(), &st2);
        ::lstat(QFile::encodeName(p3).constData(), &st3);

        DuplicatePhysicalFile f1 = createPhysicalFromStat(p1, st1);
        DuplicatePhysicalFile f2 = createPhysicalFromStat(p2, st2);
        DuplicatePhysicalFile f3 = createPhysicalFromStat(p3, st3);

        DuplicateGroup grp;
        grp.logicalSize = st1.st_size;
        grp.files = {f1, f2, f3};

        DuplicateFinderResult res;
        res.groups = {grp};

        DuplicateGroupModel model;
        model.setResult(res);

        QWidget parentWidget;
        FileActions fileActions(&parentWidget, nullptr,
            [](KJob *, const QString &, bool, const QString &, const FileActions::RefreshViews &) {},
            {},
            [](const QUrl &, QObject *, FileActions::DirectorySnapshotCallback) -> KJob * { return nullptr; });

        DuplicateActionController controller(nullptr, &fileActions);
        verify(controller.fileActions() == &fileActions, "17. fileActions getter matches");

        // Auto-confirm for test
        controller.setConfirmationHandler([](DuplicateConfirmationDialog::ActionType,
                                             const DuplicateRevalidationReport &,
                                             const QString &) {
            return true;
        });

        // Destination chooser for move
        const QString targetSubdir = liveRoot + QStringLiteral("/moved_target");
        QDir().mkdir(targetSubdir);

        controller.setDestinationChooser([&](const QString &) {
            return targetSubdir;
        });

        // 17A. Live Move of f1
        QList<QUrl> movedUrls;
        QUrl movedDest;
        controller.setMoveExecutor([&](const QList<QUrl> &urls, const QUrl &destUrl,
                                       std::function<void(const DuplicateActionController::OperationResult &)> cb) {
            movedUrls = urls;
            movedDest = destUrl;
            // Execute real filesystem move for the test
            DuplicateActionController::OperationResult r;
            const QString destDir = destUrl.toLocalFile();
            for (const auto &u : urls) {
                const QString src = u.toLocalFile();
                const QString dest = destDir + QLatin1Char('/') + QFileInfo(src).fileName();
                if (QFile::rename(src, dest)) {
                    r.successfulPaths.insert(src);
                    r.movedFinalPaths.insert(src, dest);
                } else {
                    r.failedPaths.append(src);
                }
            }
            cb(r);
        });

        model.setSelected(f1, true);
        bool moveSuccess = false;
        controller.executeMove(&model, liveRoot, [&](bool ok, const QString &) {
            moveSuccess = ok;
        });

        verify(moveSuccess, "17A. move execution succeeded");
        verify(movedUrls.size() == 1, "17A. 1 url moved");
        verify(movedDest.toLocalFile() == targetSubdir, "17A. dest matches targetSubdir");
        verify(QFile::exists(targetSubdir + QStringLiteral("/live1.txt")), "17A. live1.txt moved to target");
        verify(!QFile::exists(p1), "17A. live1.txt removed from original location");

        // 17B. Live Trash of f2
        QList<QUrl> trashedUrls;
        controller.setTrashExecutor([&](const QList<QUrl> &urls,
                                       std::function<void(const DuplicateActionController::OperationResult &)> cb) {
            trashedUrls = urls;
            DuplicateActionController::OperationResult r;
            for (const auto &u : urls) {
                const QString src = u.toLocalFile();
                if (QFile::remove(src)) {
                    r.successfulPaths.insert(src);
                } else {
                    r.failedPaths.append(src);
                }
            }
            cb(r);
        });

        model.setSelected(f2, true);
        bool trashSuccess = false;
        controller.executeTrash(&model, liveRoot, [&](bool ok, const QString &) {
            trashSuccess = ok;
        });

        verify(trashSuccess, "17B. trash execution succeeded");
        verify(trashedUrls.size() == 1, "17B. 1 url trashed");
        verify(!QFile::exists(p2), "17B. live2.txt removed by Trash operation");
        // 2 members remain in group (f1 moved, and f3)
        verify(model.groups().size() == 1, "17B. group retained with 2 members");
        verify(model.groups().first().files.size() == 2, "17B. 2 physical files remaining");

        // 17C. Live Trash of f1 (the moved file) -> leaves only f3 -> group collapses!
        const auto updatedF1 = model.groups().first().files.first();
        model.setSelected(updatedF1, true);
        trashSuccess = false;
        controller.executeTrash(&model, liveRoot, [&](bool ok, const QString &) {
            trashSuccess = ok;
        });
        verify(trashSuccess, "17C. trash of moved file succeeded");
        verify(!QFile::exists(targetSubdir + QStringLiteral("/live1.txt")), "17C. live1.txt removed by Trash");
        verify(model.groups().isEmpty(), "17C. group collapsed after only 1 member remains");
    }

    qDebug() << "duplicate_actions regression suite passed with" << checks << "checks";
    return 0;
}
