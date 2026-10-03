/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Exercises Split View pane comparison logic, metadata rules, search handling,
// dialog population, cancellation, and safety.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-compare-test");
    QCoreApplication::setApplicationName("compare-test");

    QTemporaryDir files;
    verify(files.isValid(), "disposable directory created");

    const QUrl leftDir = QUrl::fromLocalFile(files.filePath("left"));
    const QUrl rightDir = QUrl::fromLocalFile(files.filePath("right"));
    QDir().mkpath(leftDir.toLocalFile());
    QDir().mkpath(rightDir.toLocalFile());

    // 1. Identical files
    FileInfo f1_left;
    f1_left.name = QStringLiteral("file1.txt");
    f1_left.url = childUrlWithName(leftDir, f1_left.name);
    f1_left.isDir = false;
    f1_left.size = 100;
    f1_left.modificationTime = 1700000000;

    FileInfo f1_right = f1_left;
    f1_right.url = childUrlWithName(rightDir, f1_right.name);

    auto result1 = computePaneComparison({f1_left}, {f1_right});
    verify(result1.size() == 1, "comparison contains 1 entry");
    verify(result1.at(0).status == CompareStatus::Same, "identical files yield Same");
    verify(!result1.at(0).isDirectory, "entry is recognized as regular file");
    verify(result1.at(0).hasLeft && result1.at(0).hasRight, "entry exists on both sides");

    // 2. Different file sizes
    FileInfo f2_left = f1_left;
    f2_left.name = QStringLiteral("size_diff.bin");
    f2_left.size = 200;

    FileInfo f2_right = f2_left;
    f2_right.size = 500;

    auto result2 = computePaneComparison({f2_left}, {f2_right});
    verify(result2.size() == 1 && result2.at(0).status == CompareStatus::Changed,
           "differing size yields Changed");
    verify(result2.at(0).differenceReason.contains(trLocal("rozmiar", "size"), Qt::CaseInsensitive),
           "difference reason notes different size");

    // 3. Different modification time
    FileInfo f3_left = f1_left;
    f3_left.name = QStringLiteral("time_diff.txt");
    f3_left.modificationTime = 1700000100;

    FileInfo f3_right = f3_left;
    f3_right.modificationTime = 1700000200;

    auto result3 = computePaneComparison({f3_left}, {f3_right});
    verify(result3.size() == 1 && result3.at(0).status == CompareStatus::Changed,
           "differing modification time yields Changed");
    verify(result3.at(0).differenceReason.contains(trLocal("czas", "time"), Qt::CaseInsensitive),
           "difference reason notes different modification time");

    // 4. File vs Directory mismatch
    FileInfo f4_left = f1_left;
    f4_left.name = QStringLiteral("mismatch");
    f4_left.isDir = false;

    FileInfo f4_right = f4_left;
    f4_right.isDir = true;
    f4_right.size = -1;

    auto result4 = computePaneComparison({f4_left}, {f4_right});
    verify(result4.size() == 1 && result4.at(0).status == CompareStatus::Changed,
           "file vs directory yields Changed");
    verify(result4.at(0).differenceReason.contains(trLocal("typ", "type"), Qt::CaseInsensitive),
           "difference reason notes different type");

    // 5. Only on left
    FileInfo f5_left = f1_left;
    f5_left.name = QStringLiteral("left_only.dat");

    auto result5 = computePaneComparison({f5_left}, {});
    verify(result5.size() == 1 && result5.at(0).status == CompareStatus::OnlyLeft,
           "left-only file yields OnlyLeft");
    verify(result5.at(0).hasLeft && !result5.at(0).hasRight, "OnlyLeft flags");

    // 6. Only on right
    FileInfo f6_right = f1_right;
    f6_right.name = QStringLiteral("right_only.dat");

    auto result6 = computePaneComparison({}, {f6_right});
    verify(result6.size() == 1 && result6.at(0).status == CompareStatus::OnlyRight,
           "right-only file yields OnlyRight");
    verify(!result6.at(0).hasLeft && result6.at(0).hasRight, "OnlyRight flags");

    // 7. Directory on both sides
    FileInfo f7_left;
    f7_left.name = QStringLiteral("SubFolder");
    f7_left.isDir = true;
    f7_left.size = -1;
    f7_left.modificationTime = 0;

    FileInfo f7_right = f7_left;

    auto result7 = computePaneComparison({f7_left}, {f7_right});
    verify(result7.size() == 1 && result7.at(0).status == CompareStatus::Same,
           "directory on both sides yields Same");
    verify(result7.at(0).isDirectory, "entry is marked as directory");

    // 8. Unicode and diacritics
    FileInfo f8_left;
    f8_left.name = QStringLiteral("Zażółć gęślą jaźń — 𝄞.txt");
    f8_left.isDir = false;
    f8_left.size = 1234;
    f8_left.modificationTime = 1700000000;

    FileInfo f8_right = f8_left;

    auto result8 = computePaneComparison({f8_left}, {f8_right});
    verify(result8.size() == 1 && result8.at(0).status == CompareStatus::Same
               && result8.at(0).name == f8_left.name,
           "Unicode filenames compare accurately");

    // 9. Case sensitivity (exact matching)
    FileInfo f9_left = f1_left;
    f9_left.name = QStringLiteral("README.TXT");

    FileInfo f9_right = f1_right;
    f9_right.name = QStringLiteral("readme.txt");

    auto result9 = computePaneComparison({f9_left}, {f9_right});
    verify(result9.size() == 2, "different case produces separate entries");
    verify(result9.at(0).status == CompareStatus::OnlyLeft || result9.at(0).status == CompareStatus::OnlyRight,
           "case difference is not merged blindly");

    // 10. Missing / unknown metadata handling and formatting
    FileInfo f10_known = f1_left;
    f10_known.name = QStringLiteral("meta_test.bin");
    f10_known.size = 100;
    f10_known.modificationTime = 1700000000;

    // 10a. Both size unknown (-1), mtime known and equal
    FileInfo f10a_left = f10_known;
    f10a_left.size = -1;
    FileInfo f10a_right = f10_known;
    f10a_right.size = -1;
    auto res10a = computePaneComparison({f10a_left}, {f10a_right});
    verify(res10a.size() == 1, "10a count");
    verify(res10a.at(0).status == CompareStatus::Same, "both size unknown yields Same");
    verify(res10a.at(0).differenceReason == trLocal(
               "Brak różnic w dostępnych metadanych; część metadanych niedostępna",
               "No differences in available metadata; some metadata unavailable"),
           "10a differenceReason notes incomplete metadata");

    // 10b. Size known and equal, both mtime unknown (0)
    FileInfo f10b_left = f10_known;
    f10b_left.modificationTime = 0;
    FileInfo f10b_right = f10_known;
    f10b_right.modificationTime = 0;
    auto res10b = computePaneComparison({f10b_left}, {f10b_right});
    verify(res10b.size() == 1, "10b count");
    verify(res10b.at(0).status == CompareStatus::Same, "both mtime unknown yields Same");
    verify(res10b.at(0).differenceReason == trLocal(
               "Brak różnic w dostępnych metadanych; część metadanych niedostępna",
               "No differences in available metadata; some metadata unavailable"),
           "10b differenceReason notes incomplete metadata");

    // 10c. Both size (-1) and mtime (0) unknown on both sides
    FileInfo f10c_left = f10_known;
    f10c_left.size = -1;
    f10c_left.modificationTime = 0;
    FileInfo f10c_right = f10c_left;
    auto res10c = computePaneComparison({f10c_left}, {f10c_right});
    verify(res10c.size() == 1, "10c count");
    verify(res10c.at(0).status == CompareStatus::Same, "both size & mtime unknown yields Same");
    verify(res10c.at(0).differenceReason == trLocal(
               "Brak różnic w dostępnych metadanych; część metadanych niedostępna",
               "No differences in available metadata; some metadata unavailable"),
           "10c differenceReason notes incomplete metadata");

    // 10d. Metadata unknown on one side only (size)
    FileInfo f10d_left = f10_known;
    f10d_left.size = -1;
    FileInfo f10d_right = f10_known;
    f10d_right.size = 100;
    auto res10d = computePaneComparison({f10d_left}, {f10d_right});
    verify(res10d.size() == 1, "10d count");
    verify(res10d.at(0).status == CompareStatus::Changed, "size known on one side only yields Changed");
    verify(res10d.at(0).differenceReason.contains(trLocal("rozmiaru", "size"), Qt::CaseInsensitive),
           "10d differenceReason notes missing size metadata on one side");

    // 10e. Metadata unknown on one side only (mtime)
    FileInfo f10e_left = f10_known;
    f10e_left.modificationTime = 1700000000;
    FileInfo f10e_right = f10_known;
    f10e_right.modificationTime = 0;
    auto res10e = computePaneComparison({f10e_left}, {f10e_right});
    verify(res10e.size() == 1, "10e count");
    verify(res10e.at(0).status == CompareStatus::Changed, "mtime known on one side only yields Changed");
    verify(res10e.at(0).differenceReason.contains(trLocal("czas", "time"), Qt::CaseInsensitive),
           "10e differenceReason notes missing time metadata on one side");

    // 10f. Formatting helper formatFileItemMetadata
    verify(formatFileItemMetadata(f7_left) == trLocal("Folder", "Folder"), "folder format");
    verify(formatFileItemMetadata(f10c_left) == QStringLiteral("—  (—)"), "both unknown format");
    verify(formatFileItemMetadata(f10a_left).startsWith(QStringLiteral("—  (")), "size unknown format");
    verify(formatFileItemMetadata(f10b_left).endsWith(QStringLiteral("(—)")), "mtime unknown format");

    // 11. Duplicate names / Multiplicity handling (no entries silently lost)
    FileInfo dupA1 = f1_left;
    dupA1.name = QStringLiteral("duplicate.txt");
    dupA1.size = 100;
    FileInfo dupA2 = dupA1;
    dupA2.size = 200;

    FileInfo dupA_right = dupA1;
    dupA_right.size = 100;

    // 2x duplicate.txt on left, 1x on right => 2 entries: 1x Same, 1x OnlyLeft
    auto resDup1 = computePaneComparison({dupA1, dupA2}, {dupA_right});
    verify(resDup1.size() == 2, "2 left vs 1 right yields 2 entries (none lost)");
    verify(resDup1.at(0).status == CompareStatus::Same, "first duplicate entry paired and Same");
    verify(resDup1.at(1).status == CompareStatus::OnlyLeft, "second duplicate entry OnlyLeft");

    // 2x duplicate.txt on left, 2x on right => 2 entries
    auto resDup2 = computePaneComparison({dupA1, dupA2}, {dupA_right, dupA2});
    verify(resDup2.size() == 2, "2 left vs 2 right yields 2 entries");
    verify(resDup2.at(0).status == CompareStatus::Same && resDup2.at(1).status == CompareStatus::Same,
           "both duplicate entries paired and Same");

    // 1x duplicate.txt on left, 2x on right => 2 entries: 1x Same, 1x OnlyRight
    auto resDup3 = computePaneComparison({dupA1}, {dupA_right, dupA2});
    verify(resDup3.size() == 2, "1 left vs 2 right yields 2 entries");
    verify(resDup3.at(0).status == CompareStatus::Same, "first entry paired and Same");
    verify(resDup3.at(1).status == CompareStatus::OnlyRight, "second entry OnlyRight");

    // 12. Determinism independent of input order
    QList<FileInfo> leftBatch = {f1_left, f2_left, f3_left, f5_left, f7_left, f8_left};
    QList<FileInfo> rightBatch = {f8_right, f7_right, f6_right, f3_right, f2_right, f1_right};

    auto orderA = computePaneComparison(leftBatch, rightBatch);

    std::reverse(leftBatch.begin(), leftBatch.end());
    std::reverse(rightBatch.begin(), rightBatch.end());
    auto orderB = computePaneComparison(leftBatch, rightBatch);

    verify(orderA.size() == orderB.size(), "determinstic size");
    for (qsizetype i = 0; i < orderA.size(); ++i) {
        verify(orderA.at(i).name == orderB.at(i).name
                   && orderA.at(i).status == orderB.at(i).status
                   && orderA.at(i).isDirectory == orderB.at(i).isDirectory,
               "results are completely deterministic and order-independent");
    }

    // 13. UI and Window integration offscreen
    ThisPcWindow window(leftDir);
    window.show();
    window.activateWindow();
    verify(window.m_comparePanesAction != nullptr, "m_comparePanesAction exists");
    verify(!window.m_comparePanesAction->isVisible(), "m_comparePanesAction hidden when Split View disabled");

    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(rightDir);
    verify(window.m_comparePanesAction->isVisible() && window.m_comparePanesAction->isEnabled(),
           "m_comparePanesAction visible and enabled when Split View active");

    // Create real files in left and right directories for live listing test
    QFile fileL(leftDir.toLocalFile() + "/test1.txt");
    verify(fileL.open(QIODevice::WriteOnly) && fileL.write("hello left") == 10, "create left file");
    fileL.close();

    QFile fileR(rightDir.toLocalFile() + "/test1.txt");
    verify(fileR.open(QIODevice::WriteOnly) && fileR.write("hello right") == 11, "create right file");
    fileR.close();

    // Exercise SplitCompareDialog
    SplitCompareDialog dialog(leftDir, rightDir, false, &window);
    verify(dialog.windowTitle() == trLocal("Porównanie paneli", "Compare Panels"),
           "dialog title matches");
    const quint64 genInitial = dialog.generation();
    verify(genInitial > 0, "dialog initial generation > 0");

    // Wait for async listing jobs to complete
    verify(QTest::qWaitFor([&] {
        return !dialog.m_leftJob && !dialog.m_rightJob && dialog.m_leftFinished && dialog.m_rightFinished;
    }, 5000), "dialog async listing finishes");

    auto dialogEntries = dialog.comparisonResults();
    verify(dialogEntries.size() == 1, "dialog populated 1 entry");
    verify(dialogEntries.at(0).name == QStringLiteral("test1.txt"), "dialog entry name matches");
    verify(dialogEntries.at(0).status == CompareStatus::Changed, "differing size recognized in live test");

    // Test Refresh and generation increment
    dialog.startListing();
    verify(dialog.generation() == genInitial + 1, "generation increments on refresh");
    verify(QTest::qWaitFor([&] {
        return !dialog.m_leftJob && !dialog.m_rightJob && dialog.m_leftFinished && dialog.m_rightFinished;
    }, 5000), "refresh listing finishes");

    // Test safe destruction during active job (no crash / use-after-free)
    {
        auto *transientDialog = new SplitCompareDialog(leftDir, rightDir, false, &window);
        verify(transientDialog->generation() > 0, "transient dialog started");
        // Destroy while jobs might be active or pending
        delete transientDialog;
    }
    verify(true, "dialog destroyed during active jobs safely");

    // Test Search location blocking in compareSplitPanes
    window.m_navigation.updateCurrent(makeSearchLocation(QStringLiteral("query"), 0, leftDir, 0, 0, 0));
    window.m_primaryPane->setCurrentUrl(window.m_navigation.currentUrl());
    // In search mode, compareSplitPanes must show information dialog and not crash
    QTimer::singleShot(50, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            box->accept();
        }
    });
    window.compareSplitPanes();
    verify(true, "compareSplitPanes safely blocks search locations");

    // Test thispc:/ location blocking in compareSplitPanes
    window.m_navigation.updateCurrent(QUrl(QStringLiteral("thispc:/")));
    window.m_primaryPane->setCurrentUrl(window.m_navigation.currentUrl());
    QTimer::singleShot(50, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            box->accept();
        }
    });
    window.compareSplitPanes();
    verify(true, "compareSplitPanes safely blocks thispc:/ root location");

    // 14. Stage 2: Pure Sync Plan computation (Left -> Right)
    auto compEntries = computePaneComparison({f1_left, f2_left, f4_left, f5_left, f7_left},
                                             {f1_right, f2_right, f4_right, f6_right, f7_right});
    // compEntries has:
    // SubFolder (dir on both sides) -> Same
    // file1.txt (identical files) -> Same
    // left_only.dat (file only on left) -> OnlyLeft
    // mismatch (left file, right dir) -> Changed
    // right_only.dat (file only on right) -> OnlyRight
    // size_diff.bin (differing size files) -> Changed

    auto planL2R = computeSyncPlan(compEntries, SyncDirection::LeftToRight);
    verify(planL2R.size() == compEntries.size(), "Stage 2: plan size matches compare entries");

    // Find entries by name
    auto findPlanItem = [](const QList<SyncPlanEntry> &plan, const QString &name) -> SyncPlanEntry {
        for (const auto &item : plan) {
            if (item.name == name) return item;
        }
        return SyncPlanEntry();
    };

    // A. Same regular file => NoAction
    SyncPlanEntry p1 = findPlanItem(planL2R, QStringLiteral("file1.txt"));
    verify(p1.action == SyncPlanAction::NoAction, "L2R: Same file yields NoAction");
    verify(p1.reason == trLocal("Bez zmian", "No action"), "L2R: Same file reason");
    verify(!p1.isAutoExecutable, "L2R: NoAction is not executable");
    verify(p1.direction == SyncDirection::LeftToRight, "L2R: direction is LeftToRight");

    // B. OnlyLeft regular file => CopyFile
    SyncPlanEntry p5 = findPlanItem(planL2R, QStringLiteral("left_only.dat"));
    verify(p5.action == SyncPlanAction::CopyFile, "L2R: OnlyLeft file yields CopyFile");
    verify(p5.reason == trLocal("Skopiuj do prawego panelu", "Copy to right pane"), "L2R: CopyFile reason");
    verify(p5.isAutoExecutable, "L2R: CopyFile is auto executable");
    verify(p5.hasSource && !p5.hasDestination, "L2R: CopyFile has source and no destination");

    // C. OnlyRight regular file => NoAction
    SyncPlanEntry p6 = findPlanItem(planL2R, QStringLiteral("right_only.dat"));
    verify(p6.action == SyncPlanAction::NoAction, "L2R: OnlyRight file yields NoAction");
    verify(p6.reason == trLocal("Pozostaw bez zmian — usuwanie nadmiarowych elementów nie jest częścią Stage 2",
                                "Keep unchanged — deleting destination-only items is outside Stage 2"),
           "L2R: OnlyRight reason");
    verify(!p6.isAutoExecutable, "L2R: OnlyRight is not executable");
    verify(!p6.hasSource && p6.hasDestination, "L2R: OnlyRight has destination and no source");

    // D. Changed regular file => UpdateFile
    SyncPlanEntry p2 = findPlanItem(planL2R, QStringLiteral("size_diff.bin"));
    verify(p2.action == SyncPlanAction::UpdateFile, "L2R: Changed regular file yields UpdateFile");
    verify(p2.reason == trLocal("Zaktualizuj prawy plik wersją z lewego panelu",
                                "Update right file from left pane"),
           "L2R: UpdateFile reason");
    verify(p2.isAutoExecutable, "L2R: UpdateFile is auto executable");
    verify(p2.hasSource && p2.hasDestination, "L2R: UpdateFile has both source and destination");

    // E. File vs Directory mismatch => Conflict
    SyncPlanEntry p4 = findPlanItem(planL2R, QStringLiteral("mismatch"));
    verify(p4.action == SyncPlanAction::Conflict, "L2R: file vs dir mismatch yields Conflict");
    verify(p4.reason == trLocal("Konflikt typu: plik / folder", "Type conflict: file / folder"),
           "L2R: Conflict reason");
    verify(!p4.isAutoExecutable, "L2R: Conflict is not executable");

    // F. Directory on both sides => NoAction
    SyncPlanEntry p7 = findPlanItem(planL2R, QStringLiteral("SubFolder"));
    verify(p7.action == SyncPlanAction::NoAction, "L2R: Directory on both sides yields NoAction");
    verify(p7.reason == trLocal("Folder istnieje po obu stronach; zawartość nie została porównana",
                                "Folder exists on both sides; contents were not compared"),
           "L2R: Dir both reason");
    verify(!p7.isAutoExecutable, "L2R: Dir both is not executable");
    verify(p7.isDirectory, "L2R: marked as isDirectory");

    // G. Directory only on source side => Unsupported
    FileInfo dirOnlyL;
    dirOnlyL.name = QStringLiteral("LeftOnlyDir");
    dirOnlyL.isDir = true;
    auto compDirOnlyL = computePaneComparison({dirOnlyL}, {});
    auto planDirOnlyL = computeSyncPlan(compDirOnlyL, SyncDirection::LeftToRight);
    verify(planDirOnlyL.size() == 1, "dirOnlyL count");
    verify(planDirOnlyL.at(0).action == SyncPlanAction::Unsupported, "L2R: dir on source only yields Unsupported");
    verify(planDirOnlyL.at(0).reason == trLocal(
               "Folder tylko po stronie źródłowej — synchronizacja katalogów nie jest częścią Stage 2",
               "Directory exists only on source side — directory synchronization is outside Stage 2"),
           "L2R: Unsupported dir reason");
    verify(!planDirOnlyL.at(0).isAutoExecutable, "L2R: Unsupported is not executable");

    // H. Directory only on destination side => NoAction
    FileInfo dirOnlyR;
    dirOnlyR.name = QStringLiteral("RightOnlyDir");
    dirOnlyR.isDir = true;
    auto compDirOnlyR = computePaneComparison({}, {dirOnlyR});
    auto planDirOnlyR = computeSyncPlan(compDirOnlyR, SyncDirection::LeftToRight);
    verify(planDirOnlyR.size() == 1, "dirOnlyR count");
    verify(planDirOnlyR.at(0).action == SyncPlanAction::NoAction, "L2R: dir on dest only yields NoAction");
    verify(planDirOnlyR.at(0).reason == trLocal(
               "Pozostaw bez zmian — usuwanie nadmiarowych elementów nie jest częścią Stage 2",
               "Keep unchanged — deleting destination-only items is outside Stage 2"),
           "L2R: dest-only dir reason");

    // 15. Stage 2: Pure Sync Plan computation (Right -> Left)
    auto planR2L = computeSyncPlan(compEntries, SyncDirection::RightToLeft);
    verify(planR2L.size() == compEntries.size(), "Stage 2: R2L plan size matches");

    // A. Same regular file => NoAction
    SyncPlanEntry r1 = findPlanItem(planR2L, QStringLiteral("file1.txt"));
    verify(r1.action == SyncPlanAction::NoAction, "R2L: Same file yields NoAction");
    verify(r1.reason == trLocal("Bez zmian", "No action"), "R2L: Same file reason");
    verify(!r1.isAutoExecutable, "R2L: Same is not executable");
    verify(r1.direction == SyncDirection::RightToLeft, "R2L: direction is RightToLeft");

    // B. OnlyRight regular file => CopyFile (in R2L Right is source!)
    SyncPlanEntry r6 = findPlanItem(planR2L, QStringLiteral("right_only.dat"));
    verify(r6.action == SyncPlanAction::CopyFile, "R2L: OnlyRight file yields CopyFile");
    verify(r6.reason == trLocal("Skopiuj do lewego panelu", "Copy to left pane"), "R2L: CopyFile reason");
    verify(r6.isAutoExecutable, "R2L: CopyFile is auto executable");
    verify(r6.hasSource && !r6.hasDestination, "R2L: CopyFile has source (right) and no dest (left)");

    // C. OnlyLeft regular file => NoAction (in R2L Left is destination!)
    SyncPlanEntry r5 = findPlanItem(planR2L, QStringLiteral("left_only.dat"));
    verify(r5.action == SyncPlanAction::NoAction, "R2L: OnlyLeft file yields NoAction");
    verify(r5.reason == trLocal("Pozostaw bez zmian — usuwanie nadmiarowych elementów nie jest częścią Stage 2",
                                "Keep unchanged — deleting destination-only items is outside Stage 2"),
           "R2L: OnlyLeft reason");
    verify(!r5.isAutoExecutable, "R2L: OnlyLeft is not executable");

    // D. Changed regular file => UpdateFile (in R2L Left is updated from Right)
    SyncPlanEntry r2 = findPlanItem(planR2L, QStringLiteral("size_diff.bin"));
    verify(r2.action == SyncPlanAction::UpdateFile, "R2L: Changed file yields UpdateFile");
    verify(r2.reason == trLocal("Zaktualizuj lewy plik wersją z prawego panelu",
                                "Update left file from right pane"),
           "R2L: UpdateFile reason");
    verify(r2.isAutoExecutable, "R2L: UpdateFile is auto executable");

    // E. File vs Directory mismatch => Conflict
    SyncPlanEntry r4 = findPlanItem(planR2L, QStringLiteral("mismatch"));
    verify(r4.action == SyncPlanAction::Conflict, "R2L: mismatch yields Conflict");
    verify(r4.reason == trLocal("Konflikt typu: plik / folder", "Type conflict: file / folder"),
           "R2L: Conflict reason");

    // G. Directory on Right only (source in R2L) => Unsupported
    auto planDirOnlyR_R2L = computeSyncPlan(compDirOnlyR, SyncDirection::RightToLeft);
    verify(planDirOnlyR_R2L.at(0).action == SyncPlanAction::Unsupported, "R2L: dir on right (source) yields Unsupported");
    verify(planDirOnlyR_R2L.at(0).reason == trLocal(
               "Folder tylko po stronie źródłowej — synchronizacja katalogów nie jest częścią Stage 2",
               "Directory exists only on source side — directory synchronization is outside Stage 2"),
           "R2L: Unsupported reason");

    // H. Directory on Left only (dest in R2L) => NoAction
    auto planDirOnlyL_R2L = computeSyncPlan(compDirOnlyL, SyncDirection::RightToLeft);
    verify(planDirOnlyL_R2L.at(0).action == SyncPlanAction::NoAction, "R2L: dir on left (dest) yields NoAction");

    // 16. Incomplete metadata preservation in plan
    auto compMetaAsym = computePaneComparison({f10d_left}, {f10d_right});
    auto planMetaAsym = computeSyncPlan(compMetaAsym, SyncDirection::LeftToRight);
    verify(planMetaAsym.size() == 1, "planMetaAsym count");
    verify(planMetaAsym.at(0).action == SyncPlanAction::UpdateFile, "asymmetric metadata yields UpdateFile");
    verify(planMetaAsym.at(0).differenceDetails.contains(trLocal("rozmiaru", "size"), Qt::CaseInsensitive),
           "differenceDetails preserves asymmetric metadata reason");

    // 17. Summary counters helper
    SyncPlanSummary sumL2R = summarizeSyncPlan(planL2R);
    verify(sumL2R.total == 6, "summarize total count");
    verify(sumL2R.noAction == 3, "summarize noAction count (file1, right_only, SubFolder)");
    verify(sumL2R.copyFile == 1, "summarize copyFile count (left_only)");
    verify(sumL2R.updateFile == 1, "summarize updateFile count (size_diff)");
    verify(sumL2R.conflict == 1, "summarize conflict count (mismatch)");
    verify(sumL2R.unsupported == 0, "summarize unsupported count");
    verify(sumL2R.executable == 2, "summarize executable count (copy + update)");

    // 18. Multiplicity / Duplicate names preserved in plan
    auto compDups = computePaneComparison({dupA1, dupA2}, {dupA_right});
    auto planDupsL2R = computeSyncPlan(compDups, SyncDirection::LeftToRight);
    verify(planDupsL2R.size() == 2, "duplicate names preserved with multiplicity in plan");
    verify(planDupsL2R.at(0).action == SyncPlanAction::NoAction, "first duplicate is Same -> NoAction");
    verify(planDupsL2R.at(1).action == SyncPlanAction::CopyFile, "second duplicate is OnlyLeft -> CopyFile");

    // 19. Action and Direction Labels
    verify(syncDirectionLabel(SyncDirection::LeftToRight) == trLocal("Lewy → Prawy", "Left → Right"), "direction label L2R");
    verify(syncDirectionLabel(SyncDirection::RightToLeft) == trLocal("Prawy → Lewy", "Right → Left"), "direction label R2L");
    verify(syncPlanActionLabel(SyncPlanAction::NoAction) == trLocal("Bez zmian", "No action"), "action label NoAction");
    verify(syncPlanActionLabel(SyncPlanAction::CopyFile) == trLocal("Kopiuj", "Copy"), "action label CopyFile");
    verify(syncPlanActionLabel(SyncPlanAction::UpdateFile) == trLocal("Zaktualizuj", "Update"), "action label UpdateFile");
    verify(syncPlanActionLabel(SyncPlanAction::Conflict) == trLocal("Konflikt", "Conflict"), "action label Conflict");
    verify(syncPlanActionLabel(SyncPlanAction::Unsupported) == trLocal("Nieobsługiwane", "Unsupported"), "action label Unsupported");

    // 21. Stage 3: Path Safety
    verify(isSafeSyncEntryName(QStringLiteral("valid_file.txt")), "safe filename simple");
    verify(isSafeSyncEntryName(QStringLiteral("Zażółć_gęślą_123.bin")), "safe filename unicode");
    verify(!isSafeSyncEntryName(QStringLiteral("")), "empty filename is unsafe");
    verify(!isSafeSyncEntryName(QStringLiteral(".")), "dot filename is unsafe");
    verify(!isSafeSyncEntryName(QStringLiteral("..")), "dotdot filename is unsafe");
    verify(!isSafeSyncEntryName(QStringLiteral("../escaped.txt")), "path traversal is unsafe");
    verify(!isSafeSyncEntryName(QStringLiteral("sub/file.txt")), "slash in filename is unsafe");
    verify(!isSafeSyncEntryName(QStringLiteral("sub\\file.txt")), "backslash in filename is unsafe");

    // 22. Stage 3: Preflight Validation (revalidateSyncPlan)
    QTemporaryDir stage3Dir;
    verify(stage3Dir.isValid(), "stage3 temp dir created");
    const QUrl s3LeftDir = QUrl::fromLocalFile(stage3Dir.filePath("left"));
    const QUrl s3RightDir = QUrl::fromLocalFile(stage3Dir.filePath("right"));
    QDir().mkpath(s3LeftDir.toLocalFile());
    QDir().mkpath(s3RightDir.toLocalFile());

    // Create initial disk files for preflight test
    const QString pfSrcPath = s3LeftDir.toLocalFile() + "/source.txt";
    QFile pfSrc(pfSrcPath);
    verify(pfSrc.open(QIODevice::WriteOnly) && pfSrc.write("source initial data") == 19, "create pfSrc");
    pfSrc.close();

    const QString pfDstPath = s3RightDir.toLocalFile() + "/update_me.txt";
    QFile pfDst(pfDstPath);
    verify(pfDst.open(QIODevice::WriteOnly) && pfDst.write("destination old data") == 20, "create pfDst");
    pfDst.close();

    const QString pfUpdSrcPath = s3LeftDir.toLocalFile() + "/update_me.txt";
    QFile pfUpdSrc(pfUpdSrcPath);
    verify(pfUpdSrc.open(QIODevice::WriteOnly) && pfUpdSrc.write("update new data from left") == 25, "create pfUpdSrc");
    pfUpdSrc.close();

    struct stat pfSrcStat {};
    ::lstat(QFile::encodeName(pfSrcPath).constData(), &pfSrcStat);
    struct stat pfDstStat {};
    ::lstat(QFile::encodeName(pfDstPath).constData(), &pfDstStat);
    struct stat pfUpdSrcStat {};
    ::lstat(QFile::encodeName(pfUpdSrcPath).constData(), &pfUpdSrcStat);

    SyncPlanEntry pfCopyEntry;
    pfCopyEntry.name = QStringLiteral("source.txt");
    pfCopyEntry.action = SyncPlanAction::CopyFile;
    pfCopyEntry.isAutoExecutable = true;
    pfCopyEntry.sourceInfo.name = pfCopyEntry.name;
    pfCopyEntry.sourceInfo.size = pfSrcStat.st_size;
    pfCopyEntry.sourceInfo.modificationTime = pfSrcStat.st_mtim.tv_sec;

    SyncPlanEntry pfUpdateEntry;
    pfUpdateEntry.name = QStringLiteral("update_me.txt");
    pfUpdateEntry.action = SyncPlanAction::UpdateFile;
    pfUpdateEntry.isAutoExecutable = true;
    pfUpdateEntry.sourceInfo.name = pfUpdateEntry.name;
    pfUpdateEntry.sourceInfo.size = pfUpdSrcStat.st_size;
    pfUpdateEntry.sourceInfo.modificationTime = pfUpdSrcStat.st_mtim.tv_sec;
    pfUpdateEntry.destinationInfo.name = pfUpdateEntry.name;
    pfUpdateEntry.destinationInfo.size = pfDstStat.st_size;
    pfUpdateEntry.destinationInfo.modificationTime = pfDstStat.st_mtim.tv_sec;

    // 22a. Valid preflight check
    auto pfValid = revalidateSyncPlan(s3LeftDir, s3RightDir, {pfCopyEntry, pfUpdateEntry});
    verify(pfValid.isValid, "preflight passes when filesystem exactly matches snapshot");
    verify(pfValid.issues.isEmpty(), "no preflight issues for matching filesystem");

    // 22b. Source missing
    SyncPlanEntry pfMissingSrc = pfCopyEntry;
    pfMissingSrc.name = QStringLiteral("nonexistent.txt");
    auto pfResMiss = revalidateSyncPlan(s3LeftDir, s3RightDir, {pfMissingSrc});
    verify(!pfResMiss.isValid, "preflight fails when source is missing");
    verify(!pfResMiss.issues.isEmpty() && pfResMiss.issues.at(0).name == QStringLiteral("nonexistent.txt"),
           "issue identifies missing source file");

    // 22c. Source changed size
    SyncPlanEntry pfSizeDiffSrc = pfCopyEntry;
    pfSizeDiffSrc.sourceInfo.size = 99999;
    auto pfResSize = revalidateSyncPlan(s3LeftDir, s3RightDir, {pfSizeDiffSrc});
    verify(!pfResSize.isValid, "preflight fails when source size changed");

    // 22d. Source changed modification time
    SyncPlanEntry pfMtimeDiffSrc = pfCopyEntry;
    pfMtimeDiffSrc.sourceInfo.modificationTime = 100;
    auto pfResMtime = revalidateSyncPlan(s3LeftDir, s3RightDir, {pfMtimeDiffSrc});
    verify(!pfResMtime.isValid, "preflight fails when source mtime changed");

    // 22e. CopyFile destination appeared after compare
    const QString pfAppearedDstPath = s3RightDir.toLocalFile() + "/source.txt";
    QFile pfAppearedDst(pfAppearedDstPath);
    verify(pfAppearedDst.open(QIODevice::WriteOnly) && pfAppearedDst.write("intruder") == 8, "create appeared dst");
    pfAppearedDst.close();
    auto pfResDstAppeared = revalidateSyncPlan(s3LeftDir, s3RightDir, {pfCopyEntry});
    verify(!pfResDstAppeared.isValid, "preflight fails when destination appeared for CopyFile");
    QFile::remove(pfAppearedDstPath);

    // 22f. UpdateFile destination missing after compare
    QFile::remove(pfDstPath);
    auto pfResDstMissing = revalidateSyncPlan(s3LeftDir, s3RightDir, {pfUpdateEntry});
    verify(!pfResDstMissing.isValid, "preflight fails when destination disappeared for UpdateFile");
    // Restore destination for subsequent tests
    verify(pfDst.open(QIODevice::WriteOnly) && pfDst.write("destination old data") == 20, "restore pfDst");
    pfDst.close();

    // 22g. Type changed: source replaced with directory
    const QString pfDirSrcPath = s3LeftDir.toLocalFile() + "/source_dir";
    QDir().mkdir(pfDirSrcPath);
    SyncPlanEntry pfDirSrcEntry;
    pfDirSrcEntry.name = QStringLiteral("source_dir");
    pfDirSrcEntry.action = SyncPlanAction::CopyFile;
    pfDirSrcEntry.isAutoExecutable = true;
    auto pfResDirSrc = revalidateSyncPlan(s3LeftDir, s3RightDir, {pfDirSrcEntry});
    verify(!pfResDirSrc.isValid, "preflight fails when source changed to directory for CopyFile");

    // 23. Stage 3: SplitSyncExecutor Live Execution (Left -> Right) & Mutation Protection
    QTemporaryDir execDir;
    verify(execDir.isValid(), "exec temp dir created");
    const QUrl execLeftDir = QUrl::fromLocalFile(execDir.filePath("left"));
    const QUrl execRightDir = QUrl::fromLocalFile(execDir.filePath("right"));
    QDir().mkpath(execLeftDir.toLocalFile());
    QDir().mkpath(execRightDir.toLocalFile());

    // Create Left files
    const QString leftOnlyPath = execLeftDir.toLocalFile() + "/only-left.txt";
    QFile fLeftOnly(leftOnlyPath);
    verify(fLeftOnly.open(QIODevice::WriteOnly) && fLeftOnly.write("content of only-left") == 20, "create left only");
    fLeftOnly.close();

    const QString leftUpdatePath = execLeftDir.toLocalFile() + "/changed-size.txt";
    QFile fLeftUpdate(leftUpdatePath);
    verify(fLeftUpdate.open(QIODevice::WriteOnly) && fLeftUpdate.write("new updated content from left") == 29, "create left update");
    fLeftUpdate.close();

    const QString leftMismatchPath = execLeftDir.toLocalFile() + "/type-mismatch";
    QFile fLeftMismatch(leftMismatchPath);
    verify(fLeftMismatch.open(QIODevice::WriteOnly) && fLeftMismatch.write("left file mismatch") == 18, "create left mismatch");
    fLeftMismatch.close();

    const QString leftOnlyDirPath = execLeftDir.toLocalFile() + "/only-left-dir";
    QDir().mkdir(leftOnlyDirPath);

    const QString leftCommonDirPath = execLeftDir.toLocalFile() + "/common-dir";
    QDir().mkdir(leftCommonDirPath);

    // Create Right files
    const QString rightOnlyPath = execRightDir.toLocalFile() + "/only-right.txt";
    QFile fRightOnly(rightOnlyPath);
    verify(fRightOnly.open(QIODevice::WriteOnly) && fRightOnly.write("content of only-right") == 21, "create right only");
    fRightOnly.close();

    const QString rightUpdatePath = execRightDir.toLocalFile() + "/changed-size.txt";
    QFile fRightUpdate(rightUpdatePath);
    verify(fRightUpdate.open(QIODevice::WriteOnly) && fRightUpdate.write("old right") == 9, "create right update");
    fRightUpdate.close();

    const QString rightMismatchPath = execRightDir.toLocalFile() + "/type-mismatch";
    QDir().mkdir(rightMismatchPath); // directory on right vs file on left

    const QString rightCommonDirPath = execRightDir.toLocalFile() + "/common-dir";
    QDir().mkdir(rightCommonDirPath);

    // Compute live compare and plan for Left -> Right
    QList<FileInfo> lFiles;
    for (const auto &entry : QDir(execLeftDir.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = entry.isDir();
        fi.size = entry.isDir() ? -1 : entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        lFiles.append(fi);
    }
    QList<FileInfo> rFiles;
    for (const auto &entry : QDir(execRightDir.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = entry.isDir();
        fi.size = entry.isDir() ? -1 : entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        rFiles.append(fi);
    }

    auto liveCompL2R = computePaneComparison(lFiles, rFiles);
    auto livePlanL2R = computeSyncPlan(liveCompL2R, SyncDirection::LeftToRight);
    verify(livePlanL2R.size() == 6, "livePlanL2R contains 6 items");

    // Pre-execution mutation audit: verify right/only-left.txt does NOT exist yet
    const QString rightDestOnlyLeft = execRightDir.toLocalFile() + "/only-left.txt";
    verify(!QFile::exists(rightDestOnlyLeft), "pre-exec: destination file does not exist yet");

    // Execute plan with SplitSyncExecutor
    SplitSyncExecutor executorL2R(execLeftDir, execRightDir, livePlanL2R, SyncDirection::LeftToRight, &window);
    bool executorFinished = false;
    SyncExecutionReport finalReportL2R;
    QObject::connect(&executorL2R, &SplitSyncExecutor::finished, [&](const SyncExecutionReport &rep) {
        executorFinished = true;
        finalReportL2R = rep;
    });

    executorL2R.start();
    verify(QTest::qWaitFor([&] { return executorFinished; }, 5000), "executor completes asynchronous run");

    // Verify report
    verify(!finalReportL2R.preflightFailed, "preflight passed during execution");
    verify(!finalReportL2R.wasCancelled, "execution was not cancelled");
    verify(finalReportL2R.copiedCount == 1, "exactly 1 file copied (only-left.txt)");
    verify(finalReportL2R.updatedCount == 1, "exactly 1 file updated (changed-size.txt)");
    verify(finalReportL2R.conflictCount == 1, "1 conflict reported untouched (type-mismatch)");
    verify(finalReportL2R.unsupportedCount == 1, "1 unsupported reported untouched (only-left-dir)");
    verify(finalReportL2R.noActionCount == 2, "2 no-action reported untouched (common-dir, only-right.txt)");
    verify(finalReportL2R.errorCount == 0, "0 execution errors");

    // Post-execution mutation audit:
    // 1. CopyFile result: right/only-left.txt created and content matches left
    verify(QFile::exists(rightDestOnlyLeft), "post-exec: only-left.txt exists on right");
    QFile fCopied(rightDestOnlyLeft);
    verify(fCopied.open(QIODevice::ReadOnly) && fCopied.readAll() == "content of only-left", "copied content matches");
    fCopied.close();
    // Source file left/only-left.txt remains intact
    QFile fSrcCheck(leftOnlyPath);
    verify(fSrcCheck.open(QIODevice::ReadOnly) && fSrcCheck.readAll() == "content of only-left", "source file remains intact");
    fSrcCheck.close();

    // 2. UpdateFile result: right/changed-size.txt updated with left content
    QFile fUpdated(rightUpdatePath);
    verify(fUpdated.open(QIODevice::ReadOnly) && fUpdated.readAll() == "new updated content from left", "updated content matches");
    fUpdated.close();

    // 3. Destination-only items: right/only-right.txt was NOT deleted
    verify(QFile::exists(rightOnlyPath), "destination-only file was NOT deleted");
    QFile fRightCheck(rightOnlyPath);
    verify(fRightCheck.open(QIODevice::ReadOnly) && fRightCheck.readAll() == "content of only-right", "destination-only content intact");
    fRightCheck.close();

    // 4. Conflict: right/type-mismatch remains directory, left remains file
    verify(QFileInfo(rightMismatchPath).isDir(), "conflict on right remains a directory");
    verify(QFileInfo(leftMismatchPath).isFile(), "conflict on left remains a file");

    // 5. Unsupported: only-left-dir was NOT copied to right
    verify(!QFile::exists(execRightDir.toLocalFile() + "/only-left-dir"), "unsupported source directory was NOT copied");

    // 6. Common-dir: directories on both sides remain untouched
    verify(QFileInfo(leftCommonDirPath).isDir() && QFileInfo(rightCommonDirPath).isDir(), "common directories remain untouched");

    // 24. Stage 3: Live Symmetry (Right -> Left Execution)
    QTemporaryDir symmDir;
    verify(symmDir.isValid(), "symm temp dir created");
    const QUrl symmLeftDir = QUrl::fromLocalFile(symmDir.filePath("left"));
    const QUrl symmRightDir = QUrl::fromLocalFile(symmDir.filePath("right"));
    QDir().mkpath(symmLeftDir.toLocalFile());
    QDir().mkpath(symmRightDir.toLocalFile());

    // Create files for R2L test
    const QString symmRightOnlyPath = symmRightDir.toLocalFile() + "/right_source.txt";
    QFile sRightOnly(symmRightOnlyPath);
    verify(sRightOnly.open(QIODevice::WriteOnly) && sRightOnly.write("right source content") == 20, "create symm right only");
    sRightOnly.close();

    const QString symmRightUpdPath = symmRightDir.toLocalFile() + "/update_target.txt";
    QFile sRightUpd(symmRightUpdPath);
    verify(sRightUpd.open(QIODevice::WriteOnly) && sRightUpd.write("newer version on right") == 22, "create symm right upd");
    sRightUpd.close();

    const QString symmLeftUpdPath = symmLeftDir.toLocalFile() + "/update_target.txt";
    QFile sLeftUpd(symmLeftUpdPath);
    verify(sLeftUpd.open(QIODevice::WriteOnly) && sLeftUpd.write("old version on left") == 19, "create symm left upd");
    sLeftUpd.close();

    QList<FileInfo> symmLFiles = {fileInfoForEntry(symmLeftDir, {})};
    symmLFiles.clear();
    for (const auto &entry : QDir(symmLeftDir.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = entry.isDir();
        fi.size = entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        symmLFiles.append(fi);
    }
    QList<FileInfo> symmRFiles;
    for (const auto &entry : QDir(symmRightDir.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = entry.isDir();
        fi.size = entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        symmRFiles.append(fi);
    }

    auto symmComp = computePaneComparison(symmLFiles, symmRFiles);
    auto symmPlanR2L = computeSyncPlan(symmComp, SyncDirection::RightToLeft);
    verify(symmPlanR2L.size() == 2, "symmPlanR2L size");

    SplitSyncExecutor symmExecutor(symmRightDir, symmLeftDir, symmPlanR2L, SyncDirection::RightToLeft, &window);
    bool symmFinished = false;
    SyncExecutionReport symmReport;
    QObject::connect(&symmExecutor, &SplitSyncExecutor::finished, [&](const SyncExecutionReport &rep) {
        symmFinished = true;
        symmReport = rep;
    });

    symmExecutor.start();
    verify(QTest::qWaitFor([&] { return symmFinished; }, 5000), "R2L executor finishes");
    verify(symmReport.copiedCount == 1, "R2L copied exactly 1 file");
    verify(symmReport.updatedCount == 1, "R2L updated exactly 1 file");

    const QString symmLeftCopied = symmLeftDir.toLocalFile() + "/right_source.txt";
    verify(QFile::exists(symmLeftCopied), "R2L: right_source.txt copied to left");
    QFile sCopiedCheck(symmLeftCopied);
    verify(sCopiedCheck.open(QIODevice::ReadOnly) && sCopiedCheck.readAll() == "right source content", "R2L: content matches");
    sCopiedCheck.close();

    QFile sUpdatedCheck(symmLeftUpdPath);
    verify(sUpdatedCheck.open(QIODevice::ReadOnly) && sUpdatedCheck.readAll() == "newer version on right", "R2L: update matches");
    sUpdatedCheck.close();

    // 25. Stage 3: Cancellation handling
    QTemporaryDir cancelDir;
    verify(cancelDir.isValid(), "cancel temp dir created");
    const QUrl cLeft = QUrl::fromLocalFile(cancelDir.filePath("left"));
    const QUrl cRight = QUrl::fromLocalFile(cancelDir.filePath("right"));
    QDir().mkpath(cLeft.toLocalFile());
    QDir().mkpath(cRight.toLocalFile());

    for (int i = 0; i < 5; ++i) {
        QFile cf(cLeft.toLocalFile() + QStringLiteral("/file_%1.txt").arg(i));
        verify(cf.open(QIODevice::WriteOnly) && cf.write(QByteArray(1024 * 64, 'A')) == 1024 * 64, "create cancel test file");
        cf.close();
    }

    QList<FileInfo> cFiles;
    for (const auto &entry : QDir(cLeft.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = false;
        fi.size = entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        cFiles.append(fi);
    }
    auto cancelComp = computePaneComparison(cFiles, {});
    auto cancelPlan = computeSyncPlan(cancelComp, SyncDirection::LeftToRight);
    verify(cancelPlan.size() == 5, "cancel plan has 5 entries");

    SplitSyncExecutor cancelExecutor(cLeft, cRight, cancelPlan, SyncDirection::LeftToRight, &window);
    bool cancelFinished = false;
    SyncExecutionReport cancelReport;
    QObject::connect(&cancelExecutor, &SplitSyncExecutor::finished, [&](const SyncExecutionReport &rep) {
        cancelFinished = true;
        cancelReport = rep;
    });

    cancelExecutor.start();
    // Cancel immediately after starting
    cancelExecutor.cancel();
    verify(QTest::qWaitFor([&] { return cancelFinished; }, 5000), "cancelled executor completes");
    verify(cancelReport.wasCancelled, "report marks cancelled as true");
    verify(cancelExecutor.wasCancelled(), "executor marks cancelled as true");

    // 26. Stage 3: Re-Compare after Execution
    // Re-list the execDir from test 23 where only-left.txt and changed-size.txt were synced
    QList<FileInfo> postLFiles;
    for (const auto &entry : QDir(execLeftDir.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = entry.isDir();
        fi.size = entry.isDir() ? -1 : entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        postLFiles.append(fi);
    }
    QList<FileInfo> postRFiles;
    for (const auto &entry : QDir(execRightDir.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = entry.isDir();
        fi.size = entry.isDir() ? -1 : entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        postRFiles.append(fi);
    }
    auto postComp = computePaneComparison(postLFiles, postRFiles);
    auto postPlanL2R = computeSyncPlan(postComp, SyncDirection::LeftToRight);

    auto postOnlyLeft = findPlanItem(postPlanL2R, QStringLiteral("only-left.txt"));
    verify(postOnlyLeft.action == SyncPlanAction::NoAction, "re-compare: previously copied only-left.txt is now NoAction");
    verify(postOnlyLeft.compareStatus == CompareStatus::Same, "re-compare: previously copied only-left.txt is now Same");

    auto postChangedSize = findPlanItem(postPlanL2R, QStringLiteral("changed-size.txt"));
    verify(postChangedSize.action == SyncPlanAction::NoAction, "re-compare: previously updated changed-size.txt is now NoAction");
    verify(postChangedSize.compareStatus == CompareStatus::Same, "re-compare: previously updated changed-size.txt is now Same");

    // 27. Stage 3: Dialogs & UI Integration
    SyncConfirmationDialog confirmDlg(execLeftDir, execRightDir, SyncDirection::LeftToRight, summarizeSyncPlan(livePlanL2R), &window);
    verify(confirmDlg.windowTitle() == trLocal("Potwierdzenie synchronizacji", "Confirm Synchronization"),
           "confirmation dialog title");

    SyncPlanPreviewDialog previewDlg(execLeftDir, execRightDir, livePlanL2R, SyncDirection::LeftToRight, 1, &window);
    verify(previewDlg.windowTitle() == trLocal("Podgląd planu synchronizacji", "Sync Plan Preview"),
           "preview dialog title");
    verify(!previewDlg.wasExecuted(), "preview dialog initial wasExecuted is false");
    verify(previewDlg.syncButton() != nullptr, "syncButton initialized in preview dialog");
    verify(previewDlg.syncButton()->isEnabled(), "syncButton enabled when executable items exist");

    // 28. Stage 3: SyncExecutionDialog Live Lifecycle & Timeout Deadlock Detection
    QTemporaryDir uiExecDir;
    verify(uiExecDir.isValid(), "uiExecDir temp dir created");
    const QUrl uiLDir = QUrl::fromLocalFile(uiExecDir.filePath("left"));
    const QUrl uiRDir = QUrl::fromLocalFile(uiExecDir.filePath("right"));
    QDir().mkpath(uiLDir.toLocalFile());
    QDir().mkpath(uiRDir.toLocalFile());

    QFile uiF1(uiLDir.toLocalFile() + "/ui_copy.txt");
    verify(uiF1.open(QIODevice::WriteOnly) && uiF1.write("ui copy content") == 15, "create ui_copy");
    uiF1.close();

    QFile uiF2L(uiLDir.toLocalFile() + "/ui_update.txt");
    verify(uiF2L.open(QIODevice::WriteOnly) && uiF2L.write("new content") == 11, "create ui_update left");
    uiF2L.close();

    QFile uiF2R(uiRDir.toLocalFile() + "/ui_update.txt");
    verify(uiF2R.open(QIODevice::WriteOnly) && uiF2R.write("old") == 3, "create ui_update right");
    uiF2R.close();

    QList<FileInfo> uiLFiles;
    for (const auto &entry : QDir(uiLDir.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = false;
        fi.size = entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        uiLFiles.append(fi);
    }
    QList<FileInfo> uiRFiles;
    for (const auto &entry : QDir(uiRDir.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = false;
        fi.size = entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        uiRFiles.append(fi);
    }
    auto uiComp = computePaneComparison(uiLFiles, uiRFiles);
    auto uiPlan = computeSyncPlan(uiComp, SyncDirection::LeftToRight);
    verify(uiPlan.size() == 2, "uiPlan has 2 entries");

    SyncExecutionDialog uiExecDlg(uiLDir, uiRDir, uiPlan, SyncDirection::LeftToRight, &window);
    uiExecDlg.show();
    // Verify that the dialog transitions from Preparing to Finished via event loop within timeout
    verify(QTest::qWaitFor([&] {
        return uiExecDlg.m_closeButton && uiExecDlg.m_closeButton->isVisible();
    }, 5000), "SyncExecutionDialog finishes asynchronously via event loop without hanging at Preparing");

    verify(uiExecDlg.report().copiedCount == 1, "SyncExecutionDialog copied 1 file");
    verify(uiExecDlg.report().updatedCount == 1, "SyncExecutionDialog updated 1 file");
    verify(uiExecDlg.report().errorCount == 0, "SyncExecutionDialog 0 errors");
    verify(!uiExecDlg.report().wasCancelled, "SyncExecutionDialog was not cancelled");

    // Verify disk content
    QFile uiCopiedCheck(uiRDir.toLocalFile() + "/ui_copy.txt");
    verify(uiCopiedCheck.open(QIODevice::ReadOnly) && uiCopiedCheck.readAll() == "ui copy content", "ui_copy content on right matches");
    uiCopiedCheck.close();

    QFile uiUpdatedCheck(uiRDir.toLocalFile() + "/ui_update.txt");
    verify(uiUpdatedCheck.open(QIODevice::ReadOnly) && uiUpdatedCheck.readAll() == "new content", "ui_update content on right updated");
    uiUpdatedCheck.close();

    // Verify no .thispc-part files left
    QDirIterator itUiPart(uiExecDir.path(), QStringList() << QStringLiteral("*thispc-part*"), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    verify(!itUiPart.hasNext(), "no .thispc-part files remain after UI execution");
    uiExecDlg.close();

    // 29. Stage 3: SyncExecutionDialog Live Cancel test
    QTemporaryDir uiCancelDir;
    verify(uiCancelDir.isValid(), "uiCancelDir temp dir created");
    const QUrl uicLDir = QUrl::fromLocalFile(uiCancelDir.filePath("left"));
    const QUrl uicRDir = QUrl::fromLocalFile(uiCancelDir.filePath("right"));
    QDir().mkpath(uicLDir.toLocalFile());
    QDir().mkpath(uicRDir.toLocalFile());

    for (int i = 0; i < 4; ++i) {
        QFile cf(uicLDir.toLocalFile() + QStringLiteral("/cancel_%1.txt").arg(i));
        verify(cf.open(QIODevice::WriteOnly) && cf.write(QByteArray(1024 * 128, 'Z')) == 1024 * 128, "create uic file");
        cf.close();
    }
    QList<FileInfo> uicLFiles;
    for (const auto &entry : QDir(uicLDir.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = false;
        fi.size = entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        uicLFiles.append(fi);
    }
    auto uicComp = computePaneComparison(uicLFiles, {});
    auto uicPlan = computeSyncPlan(uicComp, SyncDirection::LeftToRight);

    SyncExecutionDialog uiCancelDlg(uicLDir, uicRDir, uicPlan, SyncDirection::LeftToRight, &window);
    uiCancelDlg.show();
    // Trigger cancel immediately
    uiCancelDlg.m_cancelButton->click();

    verify(QTest::qWaitFor([&] {
        return uiCancelDlg.m_closeButton && uiCancelDlg.m_closeButton->isVisible();
    }, 5000), "SyncExecutionDialog finishes after cancel");
    verify(uiCancelDlg.report().wasCancelled, "SyncExecutionDialog report marks wasCancelled == true");

    QDirIterator itCancelPart(uiCancelDir.path(), QStringList() << QStringLiteral("*thispc-part*"), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    verify(!itCancelPart.hasNext(), "no .thispc-part files remain after cancellation");
    uiCancelDlg.close();

    // 30. Stage 3: Exact Acceptance Fixture End-to-End Test
    QTemporaryDir fixtureDir;
    verify(fixtureDir.isValid(), "fixtureDir created");
    const QUrl fixL = QUrl::fromLocalFile(fixtureDir.filePath("sync_test_left"));
    const QUrl fixR = QUrl::fromLocalFile(fixtureDir.filePath("sync_test_right"));
    QDir().mkpath(fixL.toLocalFile());
    QDir().mkpath(fixR.toLocalFile());

    // Left fixture setup
    QFile fixOnlyL(fixL.toLocalFile() + "/only-left.txt");
    verify(fixOnlyL.open(QIODevice::WriteOnly) && fixOnlyL.write("dane lewy 1\n") == 12, "fixOnlyL");
    fixOnlyL.close();

    QFile fixChgL(fixL.toLocalFile() + "/changed-size.txt");
    verify(fixChgL.open(QIODevice::WriteOnly) && fixChgL.write("nowa wersja z lewego, wyraznie dluzsza\n") == 39, "fixChgL");
    fixChgL.close();

    QDir().mkdir(fixL.toLocalFile() + "/only-left-dir");
    QDir().mkdir(fixL.toLocalFile() + "/common-dir");

    QFile fixMisL(fixL.toLocalFile() + "/type-mismatch");
    verify(fixMisL.open(QIODevice::WriteOnly) && fixMisL.write("plik mismatch\n") == 14, "fixMisL");
    fixMisL.close();

    // Right fixture setup
    QFile fixOnlyR(fixR.toLocalFile() + "/only-right.txt");
    verify(fixOnlyR.open(QIODevice::WriteOnly) && fixOnlyR.write("dane prawy tylko\n") == 17, "fixOnlyR");
    fixOnlyR.close();

    QFile fixChgR(fixR.toLocalFile() + "/changed-size.txt");
    verify(fixChgR.open(QIODevice::WriteOnly) && fixChgR.write("stara\n") == 6, "fixChgR");
    fixChgR.close();

    QDir().mkdir(fixR.toLocalFile() + "/common-dir");
    QDir().mkdir(fixR.toLocalFile() + "/type-mismatch");

    // Live listing & plan
    QList<FileInfo> fLFiles;
    for (const auto &entry : QDir(fixL.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = entry.isDir();
        fi.size = entry.isDir() ? -1 : entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        fLFiles.append(fi);
    }
    QList<FileInfo> fRFiles;
    for (const auto &entry : QDir(fixR.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = entry.isDir();
        fi.size = entry.isDir() ? -1 : entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        fRFiles.append(fi);
    }
    auto fComp = computePaneComparison(fLFiles, fRFiles);
    auto fPlan = computeSyncPlan(fComp, SyncDirection::LeftToRight);
    verify(fPlan.size() == 6, "fixture plan size is 6");

    SyncExecutionDialog fixExecDlg(fixL, fixR, fPlan, SyncDirection::LeftToRight, &window);
    fixExecDlg.show();
    verify(QTest::qWaitFor([&] {
        return fixExecDlg.m_closeButton && fixExecDlg.m_closeButton->isVisible();
    }, 5000), "fixture execution finishes via event loop");

    const auto fReport = fixExecDlg.report();
    verify(fReport.copiedCount == 1, "fixture: copiedCount == 1");
    verify(fReport.updatedCount == 1, "fixture: updatedCount == 1");
    verify(fReport.conflictCount == 1, "fixture: conflictCount == 1");
    verify(fReport.unsupportedCount == 1, "fixture: unsupportedCount == 1");
    verify(fReport.noActionCount == 2, "fixture: noActionCount == 2");
    verify(fReport.errorCount == 0, "fixture: errorCount == 0");
    verify(!fReport.wasCancelled, "fixture: not cancelled");
    verify(!fReport.preflightFailed, "fixture: preflight succeeded");

    // Verify files on disk for fixture
    QFile fCopiedL(fixR.toLocalFile() + "/only-left.txt");
    verify(fCopiedL.open(QIODevice::ReadOnly) && fCopiedL.readAll() == "dane lewy 1\n", "fixture: only-left copied correctly");
    fCopiedL.close();

    QFile fUpdL(fixR.toLocalFile() + "/changed-size.txt");
    verify(fUpdL.open(QIODevice::ReadOnly) && fUpdL.readAll() == "nowa wersja z lewego, wyraznie dluzsza\n", "fixture: changed-size updated correctly");
    fUpdL.close();

    verify(QFileInfo(fixR.toLocalFile() + "/only-right.txt").isFile(), "fixture: only-right.txt intact");
    verify(QFileInfo(fixR.toLocalFile() + "/type-mismatch").isDir(), "fixture: right type-mismatch intact as dir");
    verify(!QFile::exists(fixR.toLocalFile() + "/only-left-dir"), "fixture: only-left-dir not copied");

    QDirIterator itFixPart(fixtureDir.path(), QStringList() << QStringLiteral("*thispc-part*"), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    verify(!itFixPart.hasNext(), "fixture: no .thispc-part files exist anywhere");

    fixExecDlg.close();

    // 31. Stage 3: Deterministic Cancel Race Test (Scenario A: Active job completes successfully, next items cancelled)
    QTemporaryDir raceADir;
    verify(raceADir.isValid(), "raceADir created");
    const QUrl rAL = QUrl::fromLocalFile(raceADir.filePath("left"));
    const QUrl rAR = QUrl::fromLocalFile(raceADir.filePath("right"));
    QDir().mkpath(rAL.toLocalFile());
    QDir().mkpath(rAR.toLocalFile());

    for (int i = 0; i < 5; ++i) {
        QFile f(rAL.toLocalFile() + QStringLiteral("/raceA_%1.txt").arg(i));
        verify(f.open(QIODevice::WriteOnly) && f.write("small file") == 10, "create raceA file");
        f.close();
    }
    QList<FileInfo> rALFiles;
    for (const auto &entry : QDir(rAL.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = false;
        fi.size = entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        rALFiles.append(fi);
    }
    auto rAComp = computePaneComparison(rALFiles, {});
    auto rAPlan = computeSyncPlan(rAComp, SyncDirection::LeftToRight);
    verify(rAPlan.size() == 5, "rAPlan has 5 entries");

    SplitSyncExecutor raceAExecutor(rAL, rAR, rAPlan, SyncDirection::LeftToRight, &window);
    bool raceAFinished = false;
    SyncExecutionReport raceAReport;

    // Connect progress: cancel exactly when item 1 reports progress, but let item 1 finish
    QObject::connect(&raceAExecutor, &SplitSyncExecutor::progress, [&](int current, int, const SyncPlanEntry &, const QString &) {
        if (current == 1) {
            raceAExecutor.cancel();
        }
    });
    QObject::connect(&raceAExecutor, &SplitSyncExecutor::finished, [&](const SyncExecutionReport &rep) {
        raceAFinished = true;
        raceAReport = rep;
    });

    raceAExecutor.start();
    verify(QTest::qWaitFor([&] { return raceAFinished; }, 5000), "raceAExecutor finishes");
    verify(raceAReport.wasCancelled, "raceA: wasCancelled is true");

    // Invariant 1: Total executable accounting equals sum of components
    const int totalExecA = summarizeSyncPlan(rAPlan).executable;
    verify(raceAReport.copiedCount + raceAReport.updatedCount + raceAReport.errorCount + raceAReport.cancelledCount == totalExecA,
           "raceA: accounting invariant (copied + updated + error + cancelled == totalExecutable)");

    // Invariant 2: Number of destination files created on disk MUST EXACTLY match copiedCount
    int diskFilesCountA = 0;
    for (const auto &entry : QDir(rAR.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::Files)) {
        if (!entry.fileName().contains(QStringLiteral("thispc-part"))) {
            ++diskFilesCountA;
        }
    }
    verify(diskFilesCountA == raceAReport.copiedCount, "raceA: disk files count strictly equals reported copiedCount (no off-by-one)");

    // 32. Stage 3: Deterministic Cancel Race Test (Scenario B: Cancel before start or with unexecuted jobs)
    QTemporaryDir raceBDir;
    verify(raceBDir.isValid(), "raceBDir created");
    const QUrl rBL = QUrl::fromLocalFile(raceBDir.filePath("left"));
    const QUrl rBR = QUrl::fromLocalFile(raceBDir.filePath("right"));
    QDir().mkpath(rBL.toLocalFile());
    QDir().mkpath(rBR.toLocalFile());

    for (int i = 0; i < 4; ++i) {
        QFile f(rBL.toLocalFile() + QStringLiteral("/raceB_%1.txt").arg(i));
        verify(f.open(QIODevice::WriteOnly) && f.write("race b data") == 11, "create raceB file");
        f.close();
    }
    QList<FileInfo> rBLFiles;
    for (const auto &entry : QDir(rBL.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        FileInfo fi;
        fi.name = entry.fileName();
        fi.isDir = false;
        fi.size = entry.size();
        fi.modificationTime = entry.lastModified().toSecsSinceEpoch();
        rBLFiles.append(fi);
    }
    auto rBComp = computePaneComparison(rBLFiles, {});
    auto rBPlan = computeSyncPlan(rBComp, SyncDirection::LeftToRight);
    verify(rBPlan.size() == 4, "rBPlan size 4");

    SplitSyncExecutor raceBExecutor(rBL, rBR, rBPlan, SyncDirection::LeftToRight, &window);
    bool raceBFinished = false;
    SyncExecutionReport raceBReport;
    QObject::connect(&raceBExecutor, &SplitSyncExecutor::finished, [&](const SyncExecutionReport &rep) {
        raceBFinished = true;
        raceBReport = rep;
    });

    // Cancel before starting
    raceBExecutor.cancel();
    raceBExecutor.start(); // Should immediately emit finished without running
    verify(QTest::qWaitFor([&] { return raceBFinished; }, 5000), "raceBExecutor finished immediately");
    verify(raceBReport.wasCancelled, "raceB: wasCancelled");
    verify(raceBReport.copiedCount == 0, "raceB: copiedCount == 0");
    verify(raceBReport.cancelledCount == 4, "raceB: cancelledCount == 4");

    int diskFilesCountB = 0;
    for (const auto &entry : QDir(rBR.toLocalFile()).entryInfoList(QDir::NoDotAndDotDot | QDir::Files)) {
        ++diskFilesCountB;
    }
    verify(diskFilesCountB == 0, "raceB: 0 files created on disk");

    // 33. Stage 3: Version consistency check
    verify(window.m_versionLabel != nullptr, "window versionLabel exists");
    verify(window.m_versionLabel->text() == QStringLiteral("v0.37.0"), "window versionLabel shows v0.37.0");
    verify(window.m_versionLabel->toolTip().contains(QStringLiteral("0.37.0")), "version tooltip contains 0.37.0");

    qInfo("PASS: %d Split View pane comparison, sync plan, and executor assertions", checks);
    return 0;
}
