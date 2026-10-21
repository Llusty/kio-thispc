/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include <QSignalSpy>
#include <QClipboard>

// Appended to a temporary, instrumented copy by run-pane-actions.py.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-view-settings-test");
    QCoreApplication::setApplicationName("view-settings-test");
    QSettings().clear();

    QTemporaryDir files;
    verify(files.isValid(), "temporary directory");
    const QUrl localA = QUrl::fromLocalFile(files.filePath("a"));
    const QUrl localB = QUrl::fromLocalFile(files.filePath("b"));
    const QUrl remoteA(QStringLiteral("sftp://example.test/home/user/Documents"));
    const QUrl remoteB(QStringLiteral("sftp://example.test/home/user/Pictures"));

    const QUrl profileParent = QUrl::fromLocalFile(files.filePath("profile-parent"));
    const QUrl profileChild = QUrl::fromLocalFile(files.filePath("profile-parent/child"));
    const QUrl profileDeep = QUrl::fromLocalFile(files.filePath("profile-parent/child/deeper"));
    const DirectoryViewProfile defaults{0, DirectoryViewSettings::DefaultIconSizeStep, 0, true,
                                        DirectoryViewSettings::NoGrouping};
    verify(DirectoryViewSettings::resolveProfile(profileChild) == defaults,
           "missing profile resolves to stable global default");
    const DirectoryViewProfile parentSnapshot{0, 2, 3, false, DirectoryViewSettings::GroupByType};
    DirectoryViewSettings::saveExplicitProfile(profileParent, parentSnapshot);
    DirectoryViewSettings::setInheritedRule(profileParent, parentSnapshot, true);
    verify(DirectoryViewSettings::hasExplicitProfile(profileParent)
               && DirectoryViewSettings::hasInheritedRule(profileParent),
           "parent stores separate explicit profile and inherited rule");
    verify(DirectoryViewSettings::resolveProfile(profileChild) == parentSnapshot
               && DirectoryViewSettings::resolveProfile(profileDeep) == parentSnapshot,
           "children lazily inherit the nearest ancestor snapshot");
    const DirectoryViewProfile childExplicit{2, 3, 1, true, DirectoryViewSettings::GroupBySize};
    DirectoryViewSettings::saveExplicitProfile(profileChild, childExplicit);
    verify(DirectoryViewSettings::resolveProfile(profileChild) == childExplicit,
           "exact child profile wins over ancestor rule");
    const DirectoryViewProfile changedParent{1, 0, 2, true, DirectoryViewSettings::GroupByDate};
    DirectoryViewSettings::saveExplicitProfile(profileParent, changedParent);
    verify(DirectoryViewSettings::resolveProfile(profileDeep) == parentSnapshot,
           "changing parent explicit profile does not mutate snapshot rule");
    DirectoryViewSettings::setInheritedRule(profileChild, childExplicit, true);
    verify(DirectoryViewSettings::resolveProfile(profileDeep) == childExplicit,
           "nearest ancestor rule wins over farther ancestor");
    DirectoryViewSettings::setInheritedRule(profileChild, childExplicit, false);
    verify(DirectoryViewSettings::resolveProfile(profileDeep) == parentSnapshot,
           "disabling nearest rule falls back to the next ancestor");
    DirectoryViewSettings::setInheritedRule(profileParent, parentSnapshot, false);
    const QUrl sibling = QUrl::fromLocalFile(files.filePath("profile-parent/sibling"));
    verify(DirectoryViewSettings::resolveProfile(sibling) == defaults,
           "disabling the last rule returns an unprofiled child to default");

    const QUrl remoteParent(QStringLiteral("sftp://example.test/home/user"));
    const QUrl remoteChild(QStringLiteral("sftp://example.test/home/user/docs/report"));
    DirectoryViewSettings::setInheritedRule(remoteParent, parentSnapshot, true);
    verify(DirectoryViewSettings::resolveProfile(remoteChild) == parentSnapshot,
           "remote KIO URL inheritance uses URL parents without listing");
    const QUrl adminParent(QStringLiteral("admin:///etc"));
    DirectoryViewSettings::setInheritedRule(adminParent, childExplicit, true);
    verify(DirectoryViewSettings::resolveProfile(QUrl(QStringLiteral("admin:///etc/systemd")))
               == childExplicit,
           "admin URL inheritance follows the same profile resolver");
    QUrl remoteTrailing(QStringLiteral("sftp://example.test/home/user/"));
    verify(DirectoryViewSettings::hasInheritedRule(remoteTrailing),
           "trailing slash normalization preserves the same stored rule");
    const QUrl queriedChild(QStringLiteral("sftp://example.test/home/user/docs?revision=1"));
    DirectoryViewSettings::saveExplicitProfile(queriedChild, childExplicit);
    verify(DirectoryViewSettings::resolveProfile(queriedChild) == childExplicit
               && DirectoryViewSettings::resolveProfile(
                      QUrl(QStringLiteral("sftp://example.test/home/user/docs?revision=2")))
                      == parentSnapshot,
           "query-bearing exact profiles stay distinct while ancestor lookup follows URL parents");
    QSettings().sync();
    verify(DirectoryViewSettings::resolveProfile(remoteChild) == parentSnapshot,
           "profile and rule survive a settings sync/reload boundary");

    verify(DirectoryViewSettings::viewMode(localA, 2) == 2,
           "missing local preference uses fallback");
    DirectoryViewSettings::setViewMode(localA, 1);
    DirectoryViewSettings::setViewMode(localB, 3);
    DirectoryViewSettings::setViewMode(remoteA, 0);
    DirectoryViewSettings::setViewMode(remoteB, 2);
    DirectoryViewSettings::setViewMode(QUrl(QStringLiteral("trash:/")), 3);

    QSettings().sync();
    verify(DirectoryViewSettings::viewMode(localA, 0) == 1
               && DirectoryViewSettings::viewMode(localB, 0) == 3,
           "local folders keep independent modes across settings reloads");
    verify(DirectoryViewSettings::viewMode(remoteA, 1) == 0
               && DirectoryViewSettings::viewMode(remoteB, 1) == 2,
           "remote KIO folders keep independent modes");
    verify(DirectoryViewSettings::viewMode(QUrl(QStringLiteral("trash:/")), 0) == 3,
           "Compact persists for KIO locations");

    verify(DirectoryViewSettings::iconSizeMode(localA, 1) == 1,
           "missing icon size uses the stable Large fallback");
    DirectoryViewSettings::setIconSizeMode(localA, 0);
    DirectoryViewSettings::setIconSizeMode(localB, 3);
    DirectoryViewSettings::setIconSizeMode(remoteA, 2);
    verify(DirectoryViewSettings::iconSizeMode(localA, 1) == 0
               && DirectoryViewSettings::iconSizeMode(localB, 1) == 3
               && DirectoryViewSettings::iconSizeMode(remoteA, 1) == 2,
           "local and KIO folders keep independent icon sizes");

    verify(DirectoryViewSettings::groupMode(localA, DirectoryViewSettings::NoGrouping)
               == DirectoryViewSettings::NoGrouping,
           "missing grouping preference uses None fallback");
    DirectoryViewSettings::setGroupMode(localA, DirectoryViewSettings::GroupByType);
    DirectoryViewSettings::setGroupMode(remoteA, DirectoryViewSettings::GroupByType);
    verify(DirectoryViewSettings::groupMode(localA, DirectoryViewSettings::NoGrouping)
               == DirectoryViewSettings::GroupByType
               && DirectoryViewSettings::groupMode(remoteA, DirectoryViewSettings::NoGrouping)
               == DirectoryViewSettings::GroupByType,
           "type grouping persists independently for local and KIO locations");
    DirectoryViewSettings::setGroupMode(remoteA, DirectoryViewSettings::GroupByDate);
    verify(DirectoryViewSettings::groupMode(remoteA, DirectoryViewSettings::NoGrouping)
               == DirectoryViewSettings::GroupByDate,
           "date grouping persists for KIO locations");
    DirectoryViewSettings::setGroupMode(remoteA, DirectoryViewSettings::GroupBySize);
    verify(DirectoryViewSettings::groupMode(remoteA, DirectoryViewSettings::NoGrouping)
               == DirectoryViewSettings::GroupBySize,
           "size grouping persists for KIO locations");

    QUrl equivalentRemote(QStringLiteral("sftp://example.test/home/user/../user/Documents"));
    verify(DirectoryViewSettings::viewMode(equivalentRemote, 2) == 0,
           "normalized equivalent URL shares a preference");

    QSettings settings;
    settings.beginGroup(QStringLiteral("directory/perLocationViewMode"));
    settings.setValue(DirectoryViewSettings::keyForUrl(QUrl(QStringLiteral("recentlyused:/"))), 99);
    settings.endGroup();
    settings.sync();
    verify(DirectoryViewSettings::viewMode(QUrl(QStringLiteral("recentlyused:/")), 1) == 1,
           "invalid persisted mode fails safely to the current default");
    settings.beginGroup(QStringLiteral("directory/perLocationIconSize"));
    settings.setValue(DirectoryViewSettings::keyForUrl(QUrl(QStringLiteral("trash:/"))), 99);
    settings.endGroup();
    verify(DirectoryViewSettings::iconSizeMode(QUrl(QStringLiteral("trash:/")), 1) == 1,
           "invalid persisted icon size fails safely");
    settings.beginGroup(QStringLiteral("directory/perLocationGrouping"));
    settings.setValue(DirectoryViewSettings::keyForUrl(remoteB), 99);
    settings.endGroup();
    verify(DirectoryViewSettings::groupMode(remoteB, DirectoryViewSettings::NoGrouping)
               == DirectoryViewSettings::NoGrouping,
           "invalid persisted grouping fails safely to None");

    const QUrl legacyChild(QStringLiteral("sftp://legacy.test/root/child"));
    const QUrl legacyGrandchild(QStringLiteral("sftp://legacy.test/root/child/deeper"));
    settings.beginGroup(QStringLiteral("directory/perLocationViewMode"));
    settings.setValue(DirectoryViewSettings::keyForUrl(legacyChild), 2);
    settings.endGroup();
    settings.beginGroup(QStringLiteral("directory/perLocationIconSize"));
    settings.setValue(DirectoryViewSettings::keyForUrl(legacyChild), 3);
    settings.endGroup();
    settings.beginGroup(QStringLiteral("directory/perLocationGrouping"));
    settings.setValue(DirectoryViewSettings::keyForUrl(legacyChild), DirectoryViewSettings::GroupByDate);
    settings.endGroup();
    const DirectoryViewProfile legacy = DirectoryViewSettings::resolveProfile(legacyChild);
    verify(legacy.viewMode == 2 && legacy.iconSizeStep == 1
               && legacy.groupMode == DirectoryViewSettings::GroupByDate
               && legacy.sortKey == 0 && legacy.sortAscending,
           "legacy exact fields migrate lazily while sort uses stable default");
    verify(DirectoryViewSettings::resolveProfile(legacyGrandchild) == defaults,
           "legacy exact entries never become ancestor rules");

    const QUrl versionOneUrl(QStringLiteral("sftp://legacy-profile.test/folder"));
    settings.beginGroup(QStringLiteral("directory/profiles/%1")
                            .arg(DirectoryViewSettings::keyForUrl(versionOneUrl)));
    settings.setValue(QStringLiteral("url"), versionOneUrl.toString(QUrl::FullyEncoded));
    settings.setValue(QStringLiteral("version"), 1);
    settings.setValue(QStringLiteral("explicit/viewMode"), 0);
    settings.setValue(QStringLiteral("explicit/iconSizeMode"), 0);
    settings.setValue(QStringLiteral("explicit/sortKey"), 0);
    settings.setValue(QStringLiteral("explicit/sortAscending"), true);
    settings.setValue(QStringLiteral("explicit/groupMode"), DirectoryViewSettings::NoGrouping);
    settings.endGroup();
    verify(DirectoryViewSettings::resolveProfile(versionOneUrl).iconSizeStep == 7,
           "version-one profile migrates 96 px to the matching new step");

    const QUrl invalidStepUrl(QStringLiteral("sftp://invalid-profile.test/folder"));
    settings.beginGroup(QStringLiteral("directory/profiles/%1")
                            .arg(DirectoryViewSettings::keyForUrl(invalidStepUrl)));
    settings.setValue(QStringLiteral("url"), invalidStepUrl.toString(QUrl::FullyEncoded));
    settings.setValue(QStringLiteral("version"), DirectoryViewSettings::ProfileVersion);
    settings.setValue(QStringLiteral("explicit/viewMode"), 0);
    settings.setValue(QStringLiteral("explicit/iconSizeStep"), 999);
    settings.endGroup();
    verify(DirectoryViewSettings::resolveProfile(invalidStepUrl).iconSizeStep == 8,
           "out-of-range version-two step clamps safely to the maximum");

    DirectoryListWidget categorized;
    DirectoryTreeWidget categorizedDetails;
    categorizedDetails.setColumnCount(4);
    const QList<FileInfo> categoryFiles = {
        {QStringLiteral("folder"), QStringLiteral("inode/directory"), QString(),
         childUrlWithName(localA, QStringLiteral("folder")), true, -1, 0},
        {QStringLiteral("b.txt"), QStringLiteral("text/plain"), QString(),
         childUrlWithName(localA, QStringLiteral("b.txt")), false, 2, 0},
        {QStringLiteral("a.txt"), QStringLiteral("text/plain"), QString(),
         childUrlWithName(localA, QStringLiteral("a.txt")), false, 1, 0}};
    addDirectoryFileItems(&categorized, &categorizedDetails, categoryFiles.at(0), QIcon(),
                          QStringLiteral("Folder"), QStringLiteral("—"), QStringLiteral("—"), {},
                          QStringLiteral("Folders"), QString());
    addDirectoryFileItems(&categorized, &categorizedDetails, categoryFiles.at(1), QIcon(),
                          QStringLiteral("Text file"), QStringLiteral("2 B"), QStringLiteral("—"), {},
                          QStringLiteral("Text file"), QStringLiteral("text file"));
    addDirectoryFileItems(&categorized, &categorizedDetails, categoryFiles.at(2), QIcon(),
                          QStringLiteral("Text file"), QStringLiteral("1 B"), QStringLiteral("—"), {},
                          QStringLiteral("Text file"), QStringLiteral("text file"));
    categorized.setCategorized(true);
    verify(categorized.count() == 3
               && categorized.item(0).data(KCategorizedSortFilterProxyModel::CategoryDisplayRole).toString()
                      == QStringLiteral("Folders")
               && categorized.item(1).data(Qt::DisplayRole).toString() == QStringLiteral("b.txt")
               && categorized.item(2).data(Qt::DisplayRole).toString() == QStringLiteral("a.txt"),
           "category proxy keeps headers outside file rows and preserves stable order inside a group");
    addDirectoryGroupHeader(&categorizedDetails, QStringLiteral("Text file"));
    verify(!categorizedDetails.topLevelItem(3)->data(
                   0, directory_view_detail::FileItemRole).toBool()
               && !(categorizedDetails.topLevelItem(3)->flags() & Qt::ItemIsSelectable),
           "Details group header is explicitly non-file and non-selectable");

    // Regression: refresh captures selection before model reset and restores
    // the surviving URLs, including a selected current item, in both views.
    categorized.setSelectionMode(QAbstractItemView::ExtendedSelection);
    categorizedDetails.setSelectionMode(QAbstractItemView::ExtendedSelection);
    categorized.selectionModel()->setCurrentIndex(
        categorized.item(1),
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
    categorized.selectionModel()->select(
        categorized.item(2), QItemSelectionModel::Select);
    const DirectorySelectionSnapshot refreshSelection =
        captureDirectorySelection(&categorized, &categorizedDetails, false);
    verify(refreshSelection.urls.size() == 2
               && refreshSelection.currentUrl == categoryFiles.at(1).url.toString(),
           "refresh snapshot captures multi-selection and its selected current URL before reset");
    categorized.clear();
    categorizedDetails.clear();
    addDirectoryFileItems(&categorized, &categorizedDetails, categoryFiles.at(1), QIcon(),
                          QStringLiteral("Text file"), QStringLiteral("2 B"), QStringLiteral("—"));
    restoreDirectorySelection(&categorized, &categorizedDetails, refreshSelection);
    verify(categorized.selectedItems().size() == 1
               && categorizedDetails.selectedItems().size() == 1
               && categorized.selectedItems().first().data(
                      directory_view_detail::UrlRole).toString()
                      == categoryFiles.at(1).url.toString(),
           "refresh restore preserves surviving URLs in list and Details and ignores removed items");
    verify(categorized.currentIndex().isValid()
               && categorized.selectionModel()->isSelected(categorized.currentIndex())
               && categorizedDetails.currentItem()
               && categorizedDetails.currentItem()->isSelected(),
           "refresh restore keeps currentIndex aligned with restored selection in both views");

    // Restore must replace any selection already present in the target view.
    // This models a queued/current-view selection update arriving before the
    // List -> Details transfer and makes the formerly intermittent failure
    // deterministic.
    DirectoryListWidget replacementList;
    DirectoryTreeWidget replacementDetails;
    replacementList.setSelectionMode(QAbstractItemView::ExtendedSelection);
    replacementDetails.setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const FileInfo &file : categoryFiles) {
        addDirectoryFileItems(&replacementList, &replacementDetails, file, QIcon(),
                              QStringLiteral("File"), QStringLiteral("1 B"),
                              QStringLiteral("Today"));
    }
    replacementList.selectionModel()->setCurrentIndex(
        replacementList.item(1),
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
    const DirectorySelectionSnapshot replacementSnapshot =
        captureDirectorySelection(&replacementList, &replacementDetails, false);
    replacementDetails.selectAll();
    QSignalSpy replacementChanges(
        replacementDetails.selectionModel(), &QItemSelectionModel::selectionChanged);
    restoreDirectorySelection(
        &replacementList, &replacementDetails, replacementSnapshot);
    const int changesAfterRestore = replacementChanges.count();
    app.processEvents();
    verify(selectedDirectoryListUrls(&replacementList) == replacementSnapshot.urls
               && selectedDirectoryDetailsUrls(&replacementDetails) == replacementSnapshot.urls,
           "restore replaces pre-existing target selection with the exact snapshot set");
    verify(replacementDetails.currentItem()
               && replacementDetails.currentItem()->isSelected()
               && replacementDetails.currentItem()->data(0, Qt::UserRole).toString()
                   == replacementSnapshot.currentUrl,
           "replacement restore keeps current inside the exact selected set");
    verify(replacementChanges.count() == changesAfterRestore,
           "queued events do not emit a later selection expansion after restore");
    QStyle::State restoredPaintState =
        QStyle::State_Selected | QStyle::State_HasFocus | QStyle::State_MouseOver;
    directory_view_detail::synchronizeIconItemState(
        restoredPaintState, true, false);
    verify(!(restoredPaintState & QStyle::State_MouseOver)
               && (restoredPaintState & QStyle::State_HasFocus),
           "selection restore does not revive stale hover state");

    categorized.clearSelection();
    categorizedDetails.clearSelection();
    categorizedDetails.setCurrentItem(
        categorizedDetails.topLevelItem(0), 0,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
    const DirectorySelectionSnapshot viewSwitchSelection =
        captureDirectorySelection(&categorized, &categorizedDetails, true);
    restoreDirectorySelection(&categorized, &categorizedDetails, viewSwitchSelection);
    verify(categorized.selectedItems().size() == 1
               && categorizedDetails.selectedItems().size() == 1
               && categorized.selectionModel()->isSelected(categorized.currentIndex())
               && categorizedDetails.currentItem()->isSelected(),
           "view-mode transfer mirrors selected/current URL between list and Details adapters");

    // Regression: a normal click must not retain stale selections in a
    // categorized QListView. Ctrl-click still supports intentional multi-
    // selection. All three grouping criteria use this same view/proxy path.
    for (const QString &grouping : {QStringLiteral("type"),
                                    QStringLiteral("date"),
                                    QStringLiteral("size")}) {
        DirectoryListWidget groupedSelection;
        groupedSelection.resize(900, 450);
        groupedSelection.setViewMode(QListView::IconMode);
        groupedSelection.setIconSize(QSize(64, 64));
        groupedSelection.updateGridGeometry();
        groupedSelection.setSelectionMode(QAbstractItemView::ExtendedSelection);
        for (int i = 0; i < categoryFiles.size(); ++i) {
            groupedSelection.addFileItem(
                categoryFiles.at(i), QIcon(), QStringLiteral("File"),
                QStringLiteral("1 B"), QStringLiteral("Today"), QString(),
                (i == 0 ? QStringLiteral("First group %1")
                        : QStringLiteral("Second group %1")).arg(grouping),
                i == 0 ? QStringLiteral("00") : QStringLiteral("10"));
        }
        groupedSelection.setCategorized(true);
        groupedSelection.show();
        groupedSelection.setFocus();
        app.processEvents();
        const auto click = [&](int row, Qt::KeyboardModifiers modifiers) {
            const QModelIndex index = groupedSelection.item(row);
            const QRect rect = groupedSelection.visualItemRect(index);
            verify(index.isValid() && rect.isValid()
                       && groupedSelection.viewport()->rect().contains(rect.center())
                       && groupedSelection.itemAt(rect.center()) == index,
                   "grouped click test targets an actual visible file, not a category header");
            QTest::mouseClick(groupedSelection.viewport(), Qt::LeftButton,
                              modifiers, rect.center());
            app.processEvents();
        };
        click(0, Qt::NoModifier);
        click(1, Qt::NoModifier);
        verify(groupedSelection.selectedItems().size() == 1
                   && groupedSelection.selectedItems().first() == groupedSelection.item(1),
               "unmodified click across categories replaces the previous selection");
        click(2, Qt::ControlModifier);
        verify(groupedSelection.selectedItems().size() == 2,
               "Ctrl-click still creates an intentional multi-selection in grouped mode");
        click(0, Qt::NoModifier);
        verify(groupedSelection.selectedItems().size() == 1
                   && groupedSelection.selectedItems().first() == groupedSelection.item(0),
               "unmodified click on an unselected file collapses grouped multi-selection");
        click(1, Qt::ControlModifier);
        verify(groupedSelection.selectedItems().size() == 2,
               "Ctrl-click still works after a plain-click selection reset");
        click(1, Qt::NoModifier);
        verify(groupedSelection.selectedItems().size() == 1
                   && groupedSelection.selectedItems().first() == groupedSelection.item(1),
               "unmodified click on an already-selected file collapses grouped multi-selection");
        verify(groupedSelection.currentItem() == groupedSelection.item(1),
               "plain click keeps the current index aligned with the actual selection");
    }

    const QTimeZone warsaw("Europe/Warsaw");
    const QDateTime reference(QDate(2026, 3, 25), QTime(12, 0), warsaw);
    const auto atNoon = [&warsaw](const QDate &date) {
        return QDateTime(date, QTime(12, 0), warsaw).toSecsSinceEpoch();
    };
    const QList<std::pair<qint64, QString>> dateCases = {
        {atNoon(QDate(2026, 3, 26)), QStringLiteral("00")},
        {atNoon(QDate(2026, 3, 25)), QStringLiteral("10")},
        {atNoon(QDate(2026, 3, 24)), QStringLiteral("20")},
        {atNoon(QDate(2026, 3, 23)), QStringLiteral("30")},
        {atNoon(QDate(2026, 3, 22)), QStringLiteral("40")},
        {atNoon(QDate(2026, 3, 8)), QStringLiteral("50")},
        {atNoon(QDate(2026, 2, 28)), QStringLiteral("60")},
        {atNoon(QDate(2025, 12, 31)), QStringLiteral("70")},
        {0, QStringLiteral("90")},
        {-1, QStringLiteral("90")},
    };
    for (const auto &[seconds, expected] : dateCases) {
        verify(directory_view_detail::dateCategoryForModification(seconds, reference).sortKey
                   == expected,
               "date grouping assigns every boundary to one deterministic bucket");
    }

    FileInfo sizedFile;
    const QList<std::pair<qint64, QString>> sizeCases = {
        {-1, QStringLiteral("90")},
        {-2, QStringLiteral("90")},
        {0, QStringLiteral("10")},
        {1, QStringLiteral("20")},
        {1023, QStringLiteral("20")},
        {1024, QStringLiteral("30")},
        {1025, QStringLiteral("30")},
        {1024LL * 1024 - 1, QStringLiteral("30")},
        {1024LL * 1024, QStringLiteral("40")},
        {1024LL * 1024 + 1, QStringLiteral("40")},
        {1024LL * 1024 * 1024 - 1, QStringLiteral("40")},
        {1024LL * 1024 * 1024, QStringLiteral("50")},
        {1024LL * 1024 * 1024 + 1, QStringLiteral("50")},
        {1024LL * 1024 * 1024 * 1024 - 1, QStringLiteral("50")},
        {1024LL * 1024 * 1024 * 1024, QStringLiteral("60")},
        {1024LL * 1024 * 1024 * 1024 + 1, QStringLiteral("60")},
        {std::numeric_limits<qint64>::max(), QStringLiteral("60")},
    };
    for (const auto &[bytes, expected] : sizeCases) {
        sizedFile.size = bytes;
        verify(directory_view_detail::sizeCategoryForFile(sizedFile).sortKey == expected,
               "size boundaries assign exactly one ordered bucket");
    }
    sizedFile.isDir = true;
    for (qint64 bytes : {-1LL, 0LL, 1024LL, std::numeric_limits<qint64>::max()}) {
        sizedFile.size = bytes;
        verify(directory_view_detail::sizeCategoryForFile(sizedFile).sortKey
                   == QStringLiteral("00"),
               "directory grouping ignores the worker's directory byte count");
    }
    sizedFile.isDir = false;
    sizedFile.size = 1023;
    verify(directory_view_detail::sizeCategoryForFile(sizedFile).sortKey
               == QStringLiteral("20"), "initial size metadata category");
    sizedFile.size = 1024;
    verify(directory_view_detail::sizeCategoryForFile(sizedFile).sortKey
               == QStringLiteral("30"), "changed size metadata moves across the boundary");

    const qint64 boundaryEpoch = QDateTime(
        QDate(2026, 3, 24), QTime(22, 30), QTimeZone::UTC).toSecsSinceEpoch();
    const QDateTime warsawAfterMidnight(
        QDate(2026, 3, 25), QTime(0, 30), warsaw);
    const QDateTime utcBeforeMidnight(
        QDate(2026, 3, 24), QTime(23, 30), QTimeZone::UTC);
    verify(directory_view_detail::dateCategoryForModification(
               boundaryEpoch, warsawAfterMidnight).sortKey == QStringLiteral("20")
               && directory_view_detail::dateCategoryForModification(
                   boundaryEpoch, utcBeforeMidnight).sortKey == QStringLiteral("10"),
           "the same epoch follows the explicitly supplied local timezone boundary");

    const QDateTime springDst(QDate(2026, 3, 29), QTime(0, 0), warsaw);
    const QDateTime autumnDst(QDate(2026, 10, 25), QTime(0, 0), warsaw);
    const int springDelay = directory_view_detail::millisecondsUntilNextLocalDay(springDst);
    const int autumnDelay = directory_view_detail::millisecondsUntilNextLocalDay(autumnDst);
    verify(springDelay >= 23 * 60 * 60 * 1000
               && springDelay < 24 * 60 * 60 * 1000
               && autumnDelay >= 25 * 60 * 60 * 1000
               && autumnDelay < 26 * 60 * 60 * 1000,
           "midnight refresh follows 23-hour and 25-hour DST calendar days");

    verify(DirectoryViewSettings::iconSizeStepCount() == 9,
           "icon sizing exposes nine stable steps");
    int previousExtent = 0;
    for (int step = 0; step < DirectoryViewSettings::iconSizeStepCount(); ++step) {
        const int extent = directory_view_detail::iconExtentForStep(step);
        verify(extent > previousExtent, "icon extents increase monotonically");
        previousExtent = extent;
        const QSize grid = directory_view_detail::iconGridSize(extent, false);
        const QRect itemRect(120, 40, grid.width(), grid.height());
        const QRect viewportRect(0, 0, 800, 600);
        const QRect callout = directory_view_detail::selectedNameCalloutRect(
            itemRect, extent, 140, viewportRect);
        verify(callout.left() == itemRect.left() + 1
                   && callout.width() == itemRect.width() - 2
                   && callout.left() >= viewportRect.left()
                   && callout.right() <= viewportRect.right(),
               "selected full-name overlay follows its cell width at every icon size");
        const QFont font;
        const int innerWidth = callout.width() - 12;
        verify(!directory_view_detail::selectedNameNeedsCallout(
                   QStringLiteral("one"), font, innerWidth, 2),
               "one-line selection keeps the ordinary single outline");
        verify(directory_view_detail::selectedNameNeedsCallout(
                   QStringLiteral("two lines because this selected filename is wider"),
                   font, innerWidth, 2),
               "two-line selected name uses the unified expanded outline");
        verify(directory_view_detail::selectedNameNeedsCallout(
                   QStringLiteral("three or more lines because this deliberately very long selected filename must wrap repeatedly at every supported icon size"),
                   font, innerWidth, 2),
               "three-line selected name uses the unified expanded outline");
        const QPainterPath outline = directory_view_detail::selectedNameOutlinePath(
            itemRect, callout);
        verify(outline.contains(QPointF(itemRect.center().x(), itemRect.bottom() - 1))
                   && outline.contains(QPointF(callout.center().x(), callout.bottom() - 1)),
               "expanded selection is one connected shape without an internal bottom edge");
    }
    verify(DirectoryViewSettings::iconExtentForStep(-10) == 24
               && DirectoryViewSettings::iconExtentForStep(99) == 128,
           "icon extent lookup clamps to minimum and maximum steps");
    verify(DirectoryViewSettings::legacyModeToStep(0) == 7
               && DirectoryViewSettings::legacyModeToStep(1) == 5
               && DirectoryViewSettings::legacyModeToStep(2) == 3
               && DirectoryViewSettings::legacyModeToStep(3) == 1
               && DirectoryViewSettings::iconExtentForStep(7) == 96
               && DirectoryViewSettings::iconExtentForStep(5) == 64
               && DirectoryViewSettings::iconExtentForStep(3) == 48
               && DirectoryViewSettings::iconExtentForStep(1) == 32,
           "legacy modes map deterministically without visual size changes");

    const QList<QRect> edgeCells = {
        QRect(-2, 10, 104, 84),
        QRect(0, 20, 136, 116),
        QRect(664, 30, 136, 150),
        QRect(798, 40, 176, 182),
    };
    const QList<int> textHeights = {18, 36, 71, 143};
    const QList<qreal> devicePixelRatios = {1.0, 1.25, 1.5, 2.0};
    const QRect viewportRect(0, 0, 800, 600);
    for (const QRect &cell : edgeCells) {
        for (int textHeight : textHeights) {
            const QRect callout = directory_view_detail::selectedNameCalloutRect(
                cell, 64, textHeight, viewportRect);
            verify(callout.left() == cell.left() + 1
                       && callout.right() == cell.right() - 1,
                   "selected label keeps both horizontal edges aligned at viewport boundaries");
            const QPainterPath outline = directory_view_detail::selectedNameOutlinePath(
                cell, callout);
            for (qreal dpr : devicePixelRatios) {
                const QPainterPath deviceOutline = QTransform::fromScale(dpr, dpr).map(outline);
                const qreal expectedRight = (cell.right() - 0.5) * dpr;
                verify(qAbs(deviceOutline.boundingRect().right() - expectedRight) < 0.001,
                       "unified right outline remains exact across fractional DPR values");
            }
        }
    }

    QPalette lightPalette;
    lightPalette.setColor(QPalette::Active, QPalette::Base, QColor(250, 250, 250));
    lightPalette.setColor(QPalette::Active, QPalette::Highlight, QColor(0, 100, 220));
    const QColor lightSelection = directory_view_detail::blendedSelectionColor(
        lightPalette, QPalette::Active, 16);
    verify(lightSelection == QColor(210, 226, 245) && lightSelection.alpha() == 255,
           "selection is a subtle opaque palette blend rather than solid highlight");

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Active, QPalette::Base, QColor(30, 30, 30));
    darkPalette.setColor(QPalette::Active, QPalette::Highlight, QColor(60, 130, 220));
    const QColor darkSelection = directory_view_detail::blendedSelectionColor(
        darkPalette, QPalette::Active, 16);
    verify(darkSelection == QColor(34, 46, 60) && darkSelection.alpha() == 255,
           "selection blend remains restrained and readable in dark themes");

    QStyle::State staleFocus = QStyle::State_Enabled
        | QStyle::State_HasFocus | QStyle::State_Selected | QStyle::State_MouseOver;
    directory_view_detail::synchronizeIconItemState(staleFocus, false, false);
    verify(!(staleFocus & QStyle::State_Selected)
               && !(staleFocus & QStyle::State_MouseOver)
               && !(staleFocus & QStyle::State_HasFocus),
           "unselected current icon drops stale selection, hover, and label focus outline");
    QStyle::State selectedFocus = QStyle::State_Enabled | QStyle::State_HasFocus;
    directory_view_detail::synchronizeIconItemState(selectedFocus, true, true);
    verify((selectedFocus & QStyle::State_HasFocus)
               && !(selectedFocus & QStyle::State_MouseOver)
               && !(selectedFocus & QStyle::State_Selected),
           "selected icon retains keyboard focus while custom painting owns selection and hover");

    DirectoryListWidget hiddenFocusLifecycle;
    hiddenFocusLifecycle.resize(400, 240);
    hiddenFocusLifecycle.setSelectionMode(QAbstractItemView::ExtendedSelection);
    FileInfo hiddenLifecycleFile;
    hiddenLifecycleFile.name = QStringLiteral(".hidden");
    hiddenLifecycleFile.url = QUrl::fromLocalFile(QStringLiteral("/.hidden"));
    hiddenLifecycleFile.isHidden = true;
    hiddenFocusLifecycle.addFileItem(
        hiddenLifecycleFile, QIcon(), QString(), QString(), QString(), QString());
    hiddenFocusLifecycle.show();
    hiddenFocusLifecycle.setFocus();
    app.processEvents();
    const QModelIndex hiddenLifecycleIndex = hiddenFocusLifecycle.model()->index(0, 0);
    QTest::mouseClick(hiddenFocusLifecycle.viewport(), Qt::LeftButton,
                      Qt::NoModifier,
                      hiddenFocusLifecycle.visualRect(hiddenLifecycleIndex).center());
    verify(hiddenFocusLifecycle.selectionModel()->isSelected(hiddenLifecycleIndex)
               && hiddenFocusLifecycle.currentIndex() == hiddenLifecycleIndex
               && directory_view_detail::itemOpacity(true, true, false, true) == 1.0,
           "keyboard-focused selected hidden item remains fully readable");
    const QPoint hiddenBackgroundPoint =
        hiddenFocusLifecycle.viewport()->rect().bottomRight() - QPoint(2, 2);
    verify(!hiddenFocusLifecycle.indexAt(hiddenBackgroundPoint).isValid(),
           "hidden lifecycle test uses actual empty viewport background");
    QTest::mouseClick(hiddenFocusLifecycle.viewport(), Qt::LeftButton,
                      Qt::NoModifier, hiddenBackgroundPoint);
    app.processEvents();
    const bool selectedAfterBackground =
        hiddenFocusLifecycle.selectionModel()->isSelected(hiddenLifecycleIndex);
    const bool currentAfterBackground =
        hiddenFocusLifecycle.currentIndex() == hiddenLifecycleIndex;
    const bool visibleKeyboardFocus = selectedAfterBackground
        && hiddenFocusLifecycle.hasFocus() && currentAfterBackground;
    verify(!selectedAfterBackground,
           "background click clears the hidden item selection");
    verify(currentAfterBackground,
           "background click preserves the hidden item current index");
    verify(directory_view_detail::itemOpacity(
               true, selectedAfterBackground, false, visibleKeyboardFocus)
               == directory_view_detail::HiddenItemOpacity,
           "current-but-unselected hidden item immediately returns to dim opacity");
    app.processEvents();
    verify(directory_view_detail::itemOpacity(
               true,
               hiddenFocusLifecycle.selectionModel()->isSelected(hiddenLifecycleIndex),
               false,
               hiddenFocusLifecycle.selectionModel()->isSelected(hiddenLifecycleIndex)
                   && hiddenFocusLifecycle.hasFocus()
                   && hiddenFocusLifecycle.currentIndex() == hiddenLifecycleIndex)
               == directory_view_detail::HiddenItemOpacity,
           "event drain does not revive stale current or hover brightness");

    DirectoryListWidget hoverLifecycle;
    hoverLifecycle.resize(400, 240);
    hoverLifecycle.addItem(QStringLiteral("hovered before clear"));
    hoverLifecycle.show();
    app.processEvents();
    const QPoint hoverPoint =
        hoverLifecycle.visualItemRect(hoverLifecycle.item(0)).center();
    QMouseEvent hoverMove(
        QEvent::MouseMove, hoverPoint, hoverPoint,
        hoverLifecycle.viewport()->mapToGlobal(hoverPoint),
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(hoverLifecycle.viewport(), &hoverMove);
    hoverLifecycle.clear();
    QEvent leaveAfterClear(QEvent::Leave);
    QApplication::sendEvent(&hoverLifecycle, &leaveAfterClear);
    app.processEvents();
    verify(hoverLifecycle.count() == 0,
           "clearing a hovered categorized view makes a later Leave event safe");

    DirectoryViewSettings::setViewMode(localA, -4);
    verify(DirectoryViewSettings::viewMode(localA, 2) == 0,
           "writes clamp modes to the supported range");

    QStackedWidget compactStack;
    DirectoryListWidget compact;
    DirectoryTreeWidget compactDetails;
    compact.resize(520, 120);
    for (int i = 0; i < 20; ++i) {
        compact.addItem(QStringLiteral("compact item %1 with a long display name").arg(i));
    }
    compactStack.addWidget(&compact);
    compactStack.addWidget(&compactDetails);
    applyDirectoryViewLayout(&compact, &compactDetails, &compactStack, nullptr, 3);
    compact.show();
    app.processEvents();
    verify(compact.compactMode()
               && compact.viewMode() == QListView::ListMode
               && compact.flow() == QListView::TopToBottom
               && compact.isWrapping()
               && compact.iconSize() == QSize(20, 20),
           "Compact uses a dense top-to-bottom column layout rather than Icons or List geometry");
    const QRect firstCompactRect = compact.visualItemRect(compact.item(0));
    const QRect laterCompactRect = compact.visualItemRect(compact.item(10));
    verify(firstCompactRect.height() < 32
               && laterCompactRect.left() > firstCompactRect.left(),
           "Compact has dense rows and creates additional columns within the viewport");
    compact.setCurrentRow(0);
    compact.setFocus();
    QTest::keyClick(&compact, Qt::Key_Down);
    verify(compact.currentRow() == 1,
           "Compact keyboard Down follows the visual row order");
    QTest::keyClick(&compact, Qt::Key_Right);
    verify(compact.currentRow() > 1,
           "Compact keyboard Right moves into the next visual column");
    applyDirectoryViewLayout(&compact, &compactDetails, &compactStack, nullptr, 0, 1);
    verify(!compact.compactMode() && compact.viewMode() == QListView::IconMode,
           "leaving Compact restores the ordinary icon layout and full-name delegate contract");

    verify(QDir().mkpath(localA.toLocalFile())
               && QDir().mkpath(localB.toLocalFile()),
           "create integration folders");
    DirectoryViewSettings::setViewMode(localA, 1);
    DirectoryViewSettings::setViewMode(localB, 3);
    DirectoryViewSettings::setIconSizeMode(localA, 0);
    DirectoryViewSettings::setIconSizeMode(localB, 3);
    DirectoryViewSettings::setGroupMode(localA, DirectoryViewSettings::GroupByType);
    DirectoryViewSettings::setGroupMode(localB, DirectoryViewSettings::NoGrouping);
    {
        ThisPcWindow window(localA, false);
        window.show();
        app.processEvents();
        auto *recoveryStatus = window.findChild<QLabel *>(QStringLiteral("recoveryFenceStatus"));
        verify(recoveryStatus && !recoveryStatus->isVisible()
                   && !window.statusBar()->currentMessage().isEmpty(),
               "completed recovery is transient while blocked recovery keeps its fence widget");
        window.statusBar()->showMessage(QStringLiteral("newer operation status"), 4000);
        window.updateRecoveryFencePresentation();
        verify(window.statusBar()->currentMessage() == QStringLiteral("newer operation status"),
               "late clean recovery completion does not overwrite a newer operation status");
        QTest::qWait(6500);
        window.updateRecoveryFencePresentation();
        QTest::qWait(1100);
        verify(!window.statusBar()->currentMessage().contains(
                   QStringLiteral("recovery check completed"), Qt::CaseInsensitive)
                   && !window.statusBar()->currentMessage().contains(
                       QStringLiteral("odzyskiwania po awarii zakończone"), Qt::CaseInsensitive),
               "clean recovery success stays gone after timeout and timer reentry");
        window.statusBar()->showMessage(QStringLiteral("newest operation status"), 2500);
        window.updateRecoveryFencePresentation();
        verify(window.statusBar()->currentMessage() == QStringLiteral("newest operation status"),
               "consumed recovery success never clobbers a newer message");
        verify(window.m_directoryViewMode == 1,
               "window startup applies the initial folder preference");
        verify(window.m_directoryIconSizeStep == 7
                   && window.m_directoryList->iconSize() == QSize(24, 24),
               "window startup migrates legacy 96 px preference without changing List geometry");
        verify(window.m_groupMode == DirectoryViewSettings::GroupByType,
               "window startup restores type grouping for the initial folder");
        const DirectoryViewProfile globalBeforeChange = DirectoryViewSettings::globalDefault();
        window.setDirectoryViewMode(2);
        window.setDirectoryIconSizeStep(2);
        window.setSortKey(3);
        window.setSortAscending(false);
        verify(DirectoryViewSettings::globalDefault() == globalBeforeChange,
               "ordinary folder view changes do not mutate stable global default");
        const DirectoryViewProfile savedLocalA = DirectoryViewSettings::resolveProfile(localA);
        verify(savedLocalA.viewMode == 2 && savedLocalA.iconSizeStep == 2
                   && savedLocalA.sortKey == 3 && !savedLocalA.sortAscending,
               "view icon size and sort order persist together in the current explicit profile");
        window.setDirectoryViewMode(1);
        window.setDirectoryIconSizeStep(0);
        window.setSortKey(0);
        window.setSortAscending(true);
        FileInfo datedToday{QStringLiteral("today.txt"), QStringLiteral("text/plain"), QString(),
                            childUrlWithName(localA, QStringLiteral("today.txt")), false, 1,
                            QDateTime::currentSecsSinceEpoch()};
        FileInfo datedUnknown{QStringLiteral("unknown.txt"), QStringLiteral("text/plain"), QString(),
                              childUrlWithName(localA, QStringLiteral("unknown.txt")), false, 1, 0};
        window.m_primaryPane->setFiles({datedUnknown, datedToday});
        window.setGroupMode(DirectoryViewSettings::GroupByDate);
        verify(window.m_groupMode == DirectoryViewSettings::GroupByDate
                   && window.m_directoryList->isCategorized()
                   && window.m_dateGroupingTimer.isActive(),
               "primary Date action routes to real categorized mode and schedules midnight refresh");
        verify(window.m_directoryList->item(0).data(
                   KCategorizedSortFilterProxyModel::CategorySortRole).toString()
                       == QStringLiteral("10")
                   && window.m_directoryList->item(1).data(
                       KCategorizedSortFilterProxyModel::CategorySortRole).toString()
                       == QStringLiteral("90"),
               "primary Date routing orders Today before missing metadata without changing file rows");
        window.setGroupMode(DirectoryViewSettings::GroupByType);
        FileInfo sizedSmall = datedToday;
        sizedSmall.name = QStringLiteral("small.txt");
        sizedSmall.url = childUrlWithName(localA, sizedSmall.name);
        sizedSmall.size = 1023;
        FileInfo sizedLarge = datedToday;
        sizedLarge.name = QStringLiteral("large.txt");
        sizedLarge.url = childUrlWithName(localA, sizedLarge.name);
        sizedLarge.size = 1024;
        window.m_primaryPane->setFiles({sizedLarge, sizedSmall});
        window.setGroupMode(DirectoryViewSettings::GroupBySize);
        verify(window.m_directoryList->isCategorized()
                   && window.m_directoryList->item(0).data(
                       KCategorizedSortFilterProxyModel::CategorySortRole).toString()
                       == QStringLiteral("20")
                   && window.m_directoryList->item(1).data(
                       KCategorizedSortFilterProxyModel::CategorySortRole).toString()
                       == QStringLiteral("30")
                   && !window.m_dateGroupingTimer.isActive(),
               "primary Size action groups listed metadata without a date timer");
        verify(window.m_directoryList->item(0).data(directory_view_detail::UrlRole).toUrl()
                   == sizedSmall.url
                   && window.m_directoryDetails->topLevelItem(0)->data(
                       0, directory_view_detail::FileItemRole).toBool() == false,
               "Size categorization retains file source URL and non-file Details header");
        window.m_directoryList->setRowSelected(0, true);
        window.m_directoryDetails->topLevelItem(1)->setSelected(true);
        window.m_primaryPane->mutableFiles()[0].size = 512;
        window.renderDirectoryItems();
        verify(window.m_directoryList->item(0).data(
                   KCategorizedSortFilterProxyModel::CategorySortRole).toString()
                   == QStringLiteral("20")
                   && window.m_directoryList->item(1).data(
                       KCategorizedSortFilterProxyModel::CategorySortRole).toString()
                       == QStringLiteral("20"),
               "updated listing metadata reclassifies the file on rerender");
        verify(window.m_directoryList->selectedItems().size() == 1
                   && window.m_directoryList->selectedItems().first().data(
                       directory_view_detail::UrlRole).toString() == sizedSmall.url.toString()
                   && window.m_directoryDetails->selectedItems().size() == 1
                   && window.m_directoryDetails->selectedItems().first()->data(
                       0, Qt::UserRole).toString() == sizedSmall.url.toString(),
               "size regrouping preserves file selection in list and Details");
        window.setGroupMode(DirectoryViewSettings::GroupByType);
        window.createNewTab(localB, true);
        verify(window.m_directoryViewMode == 3
                   && window.m_directoryList->compactMode(),
               "new tab applies its Compact folder preference");
        verify(window.m_directoryIconSizeStep == 1,
               "new tab applies its folder icon size");
        verify(window.m_groupMode == DirectoryViewSettings::NoGrouping,
               "new tab applies its independent grouping preference");
        window.switchToTab(0);
        verify(window.m_directoryViewMode == 1,
               "tab switch restores the destination folder preference");
        verify(window.m_groupMode == DirectoryViewSettings::GroupByType,
               "tab switch restores the destination grouping preference");
        window.setSplitViewEnabled(true);
        window.m_splitPane->setCurrentUrl(localB, false);
        verify(window.m_splitPane->viewMode() == 3
                   && window.m_splitPane->listView()->compactMode(),
               "split pane applies its Compact folder preference before session save");
        verify(window.m_splitPane->iconSizeStep() == 1,
               "split pane applies migrated 32 px preference before session save");
        verify(window.m_splitPane->groupMode() == DirectoryViewSettings::NoGrouping,
               "split pane applies its independent grouping preference");
        window.m_splitPane->setFiles({datedUnknown, datedToday});
        window.m_splitPane->setGroupMode(DirectoryViewSettings::GroupByDate);
        verify(window.m_splitPane->groupMode() == DirectoryViewSettings::GroupByDate
                   && window.m_splitPane->listView()->isCategorized()
                   && window.m_splitPane->m_dateGroupingTimer.isActive(),
               "split Date action routes independently and schedules its own boundary refresh");
        window.m_splitPane->setGroupMode(DirectoryViewSettings::NoGrouping);
        window.m_splitPane->setViewMode(2);
        window.m_splitPane->setIconSizeStep(2);
        window.m_splitPane->setSortState(3, false);
        const QUrl splitInheritedChild = QUrl::fromLocalFile(
            localB.toLocalFile() + QStringLiteral("/inherited"));
        window.applyViewToSubfolders(PaneId::Split, localB);
        const DirectoryViewProfile splitRule = DirectoryViewSettings::resolveProfile(splitInheritedChild);
        verify(splitRule.viewMode == 2 && splitRule.iconSizeStep == 2
                   && splitRule.sortKey == 3 && !splitRule.sortAscending,
               "Apply-to-subfolders captures the initiating Split pane profile");
        window.m_splitPane->setViewMode(1);
        window.applyViewToSubfolders(PaneId::Split, localB);
        verify(DirectoryViewSettings::resolveProfile(splitInheritedChild).viewMode == 1,
               "reapplying overwrites the Split ancestor rule with a fresh snapshot");
        window.removeViewFromSubfolders(localB);
        verify(!DirectoryViewSettings::hasInheritedRule(localB),
               "Remove-view command disables the Split ancestor rule");
        verify(DirectoryViewSettings::hasExplicitProfile(localB),
               "Remove-view command preserves the parent exact profile");
        window.m_splitPane->setViewMode(3);
        window.m_splitPane->setIconSizeStep(3);
        window.m_splitPane->setSortState(0, true);
        window.m_splitPane->setFiles({sizedLarge, sizedSmall});
        window.m_splitPane->setGroupMode(DirectoryViewSettings::GroupBySize);
        verify(window.m_splitPane->listView()->isCategorized()
                   && window.m_splitPane->listView()->item(0).data(
                       KCategorizedSortFilterProxyModel::CategorySortRole).toString()
                       == QStringLiteral("20")
                   && !window.m_splitPane->m_dateGroupingTimer.isActive(),
               "split Size action groups independently without a date timer");
        window.m_splitPane->setGroupMode(DirectoryViewSettings::NoGrouping);
        window.saveSessionState();
    }
    {
        ThisPcWindow restored(kThisPcUrl, true);
        verify(restored.m_directoryViewMode == 1,
               "session restore reapplies the active folder preference");
        verify(restored.m_directoryIconSizeStep == 0,
               "session restore reapplies the active folder icon size");
        verify(restored.m_groupMode == DirectoryViewSettings::GroupByType,
               "session restore reapplies the active folder grouping");
        verify(!restored.m_splitPane->isHidden(),
               "session restore reopens Split View");
        verify(restored.m_splitPane->viewMode() == 3
                   && restored.m_splitPane->listView()->compactMode(),
               "session restore reapplies the split Compact folder preference");
        verify(restored.m_splitPane->iconSizeStep() == 3,
                   "session restore reapplies the split folder icon size");
    }

    // Regression: QListView's default DoubleClicked edit trigger used to open
    // an inline editor at the same time as file activation.  A directory hid
    // the race by replacing the model during navigation; a file left the
    // editor visible while focus moved to the external application.
    for (const QString &paneName : {QStringLiteral("Primary"), QStringLiteral("Split")}) {
        QStackedWidget activationStack;
        DirectoryListWidget activationView;
        DirectoryTreeWidget activationDetails;
        activationStack.addWidget(&activationView);
        activationStack.addWidget(&activationDetails);
        activationView.resize(800, 420);
        activationView.setSelectionMode(QAbstractItemView::ExtendedSelection);
        const FileInfo activationFile{
            QStringLiteral("Roadmap.md"), QStringLiteral("text/markdown"), QString(),
            childUrlWithName(localA, QStringLiteral("Roadmap.md")), false, 8, 0};
        const FileInfo activationFolder{
            QStringLiteral("Folder"), QStringLiteral("inode/directory"), QString(),
            childUrlWithName(localA, QStringLiteral("Folder")), true, -1, 0};
        addDirectoryFileItems(&activationView, &activationDetails, activationFile,
                              QIcon(), QStringLiteral("Markdown"), QStringLiteral("8 B"),
                              QStringLiteral("Today"));
        addDirectoryFileItems(&activationView, &activationDetails, activationFolder,
                              QIcon(), QStringLiteral("Folder"), QStringLiteral("—"),
                              QStringLiteral("Today"));
        QSignalSpy activations(&activationView, &DirectoryListWidget::itemDoubleClicked);
        QModelIndex fileIndex;
        QModelIndex folderIndex;
        for (int row = 0; row < activationView.count(); ++row) {
            const QModelIndex index = activationView.item(row);
            if (index.data(directory_view_detail::DirectoryRole).toBool())
                folderIndex = index;
            else
                fileIndex = index;
        }
        verify(fileIndex.isValid() && folderIndex.isValid(),
               "activation fixture contains one file and one folder");

        for (int mode : {0, 1, 3}) {
            applyDirectoryViewLayout(&activationView, &activationDetails,
                                     &activationStack, nullptr, mode);
            activationStack.show();
            activationView.show();
            activationView.setFocus();
            app.processEvents();
            verify(!(activationView.editTriggers() & QAbstractItemView::DoubleClicked)
                       && (activationView.editTriggers() & QAbstractItemView::SelectedClicked)
                       && (activationView.editTriggers() & QAbstractItemView::EditKeyPressed),
                   "double-click editing is disabled while slow-click and keyboard editing remain available");

            const QPoint filePoint = activationView.visualItemRect(fileIndex).center();
            QTest::mouseClick(activationView.viewport(), Qt::LeftButton, {}, filePoint);
            app.processEvents();
            verify(!activationView.isEditingName()
                       && activationView.selectionModel()->isSelected(fileIndex),
                   "single click selects a file without opening an editor");
            QTest::mouseDClick(activationView.viewport(), Qt::LeftButton, {}, filePoint);
            app.processEvents();
            verify(activations.count() >= 1
                       && !activationView.isEditingName(),
                   "double-click file activates without an inline editor");

            QTest::qWait(QApplication::doubleClickInterval() + 50);
            verify(!activationView.isEditingName(),
                   "fast double-click cancels the delayed SelectedClicked edit");

            QTest::mouseClick(activationView.viewport(), Qt::LeftButton, {}, filePoint);
            verify(QTest::qWaitFor(
                       [&] { return activationView.isEditingName(); },
                       QApplication::doubleClickInterval() + 250),
                   "slow second click on a selected file opens the inline editor");
            activationView.cancelEditingForActivation();
            app.processEvents();
            verify(!activationView.isEditingName(),
                   "activation cancellation closes a slow-click editor");

            activationView.edit(fileIndex);
            app.processEvents();
            verify(activationView.isEditingName(),
                   "explicit keyboard/action rename still opens the inline editor");
            activationView.cancelEditingForActivation();
            app.processEvents();

            const int beforeFolderActivation = activations.count();
            QMetaObject::invokeMethod(
                &activationView, "doubleClicked", Qt::DirectConnection,
                Q_ARG(QModelIndex, folderIndex));
            app.processEvents();
            verify(activations.count() == beforeFolderActivation + 1
                       && !activationView.isEditingName(),
                   "double-click folder activates without an inline editor");

            for (int repeat = 0; repeat < 3; ++repeat) {
                QTest::mouseDClick(activationView.viewport(), Qt::LeftButton, {}, filePoint);
                app.processEvents();
            }
            QTest::qWait(QApplication::doubleClickInterval() + 50);
            verify(!activationView.isEditingName()
                       && activationView.currentIndex().isValid()
                       && activationView.selectionModel()->isSelected(
                           activationView.currentIndex()),
                   "repeated double-clicks leave no editor or stale current selection");

            QTest::mouseClick(activationView.viewport(), Qt::LeftButton, {}, filePoint);
            verify(QTest::qWaitFor(
                       [&] { return activationView.isEditingName(); },
                       QApplication::doubleClickInterval() + 250),
                   "slow second click still renames after repeated fast double-clicks");
            activationView.cancelEditingForActivation();
            app.processEvents();
        }

        applyDirectoryViewLayout(&activationView, &activationDetails,
                                 &activationStack, nullptr, 2);
        activationDetails.resize(800, 420);
        activationDetails.setSelectionMode(QAbstractItemView::ExtendedSelection);
        activationDetails.show();
        activationDetails.setFocus();
        app.processEvents();
        auto *detailsFileItem = activationDetails.topLevelItem(0);
        auto *detailsFolderItem = activationDetails.topLevelItem(1);
        const QModelIndex detailsFileName = activationDetails.indexFromItem(detailsFileItem, 0);
        const QPoint detailsFile = activationDetails.visualItemRect(detailsFileItem).center();
        QSignalSpy detailsActivations(&activationDetails, &QTreeWidget::itemDoubleClicked);
        QObject::connect(&activationDetails, &QTreeWidget::itemDoubleClicked,
                         &activationDetails,
                         [&activationDetails](QTreeWidgetItem *, int) {
                             activationDetails.cancelEditingForActivation();
                         });
        verify((detailsFileItem->flags() & Qt::ItemIsEditable)
                   && !(activationDetails.editTriggers() & QAbstractItemView::DoubleClicked)
                   && (activationDetails.editTriggers() & QAbstractItemView::SelectedClicked),
               "Details file and folder rows support slow-click editing without double-click editing");

        QTest::mouseClick(activationDetails.viewport(), Qt::LeftButton, {}, detailsFile);
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        verify(detailsFileItem->isSelected() && !activationDetails.isEditingName(),
               "single click selects a Details file without opening an editor");

        QTest::mouseDClick(activationDetails.viewport(), Qt::LeftButton, {}, detailsFile);
        app.processEvents();
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        verify(detailsActivations.count() >= 1 && !activationDetails.isEditingName(),
               "fast double-click activates a Details file without an inline editor");

        QTest::mouseClick(activationDetails.viewport(), Qt::LeftButton, {}, detailsFile);
        verify(QTest::qWaitFor(
                   [&] { return activationDetails.isEditingName(); },
                   QApplication::doubleClickInterval() + 250),
               "slow second click opens the Details Name editor");
        verify(activationDetails.currentIndex().column() == 0,
               "Details inline rename is confined to the Name column");
        if (auto *detailsEditor = activationDetails.findChild<QLineEdit *>()) {
            const QRect nameCell = activationDetails.visualRect(detailsFileName);
            const QRect typeCell = activationDetails.visualRect(detailsFileName.siblingAtColumn(1));
            verify(nameCell.adjusted(-2, -1, 2, 1).contains(detailsEditor->geometry())
                       && !typeCell.intersects(detailsEditor->geometry()),
                   "Details editor geometry is confined to the Name column and excludes Type");
        } else {
            verify(false, "Details editor widget is available for geometry verification");
        }
        activationDetails.cancelEditingForActivation();
        app.processEvents();

        activationDetails.editItem(detailsFileItem, 1);
        app.processEvents();
        verify(!activationDetails.isEditingName(),
               "Details Type/Size/Date columns cannot open an inline editor");
        activationDetails.editItem(detailsFileItem, 0);
        app.processEvents();
        verify(activationDetails.isEditingName()
                   && activationDetails.currentIndex() == detailsFileName,
               "explicit Details name editing remains available");
        activationDetails.cancelEditingForActivation();

        const int beforeFolderDetails = detailsActivations.count();
        QMetaObject::invokeMethod(
            &activationDetails, "itemDoubleClicked", Qt::DirectConnection,
            Q_ARG(QTreeWidgetItem *, detailsFolderItem), Q_ARG(int, 0));
        app.processEvents();
        verify(detailsActivations.count() == beforeFolderDetails + 1,
               "Details folder double-click emits exactly one activation");
        verify(!activationDetails.isEditingName(),
               "Details folder activation leaves no stale editor");

        for (int repeat = 0; repeat < 3; ++repeat) {
            QTest::mouseDClick(activationDetails.viewport(), Qt::LeftButton, {}, detailsFile);
            app.processEvents();
        }
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        verify(!activationDetails.isEditingName(),
               "repeated fast Details double-clicks leave no editor");
        QTest::mouseClick(activationDetails.viewport(), Qt::LeftButton, {}, detailsFile);
        verify(QTest::qWaitFor(
                   [&] { return activationDetails.isEditingName(); },
                   QApplication::doubleClickInterval() + 250),
               "Details slow-click rename still works after repeated activation");
        activationDetails.cancelEditingForActivation();

        QStackedWidget geometryStack;
        DirectoryListWidget geometryView;
        DirectoryTreeWidget geometryDetails;
        geometryStack.addWidget(&geometryView);
        geometryStack.addWidget(&geometryDetails);
        geometryView.resize(520, 360);
        FileInfo geometryFile{QStringLiteral("short.txt"), QStringLiteral("text/plain"), {},
            QUrl(QStringLiteral("sftp://example.test/path/short.txt")), false, 1, 0};
        QPixmap geometryIconPixmap(16, 16);
        geometryIconPixmap.fill(Qt::blue);
        geometryView.addFileItem(
            geometryFile, QIcon(geometryIconPixmap), {}, {}, {}, {});
        geometryStack.show();
        geometryView.setCurrentRow(0);
        geometryView.setFocus();
        app.processEvents();
        auto *nameDelegate = dynamic_cast<ExplorerNameDelegate *>(geometryView.itemDelegate());
        verify(nameDelegate != nullptr, "directory list uses Explorer name delegate");
        for (int mode : {0, 1, 3}) {
            applyDirectoryViewLayout(&geometryView, &geometryDetails,
                                     &geometryStack, nullptr, mode);
            geometryStack.show(); geometryView.show(); app.processEvents();
            const QModelIndex index = geometryView.currentIndex();
            QStyleOptionViewItem option;
            option.rect = geometryView.visualRect(index);
            option.widget = &geometryView;
            const QRect editorRect = nameDelegate->nameEditorRect(option, index);
            const QRect labelRect = nameDelegate->styledNameRect(option, index);
            const QRect iconRect = nameDelegate->decorationRect(option, index);
            verify((mode == 0
                        ? editorRect == directory_view_detail::singleLineNameRect(
                              labelRect, geometryView.font())
                        : editorRect == labelRect),
                   mode == 0 ? "Icons editor equals the painted first-line rect"
                             : mode == 1 ? "List editor equals the initialized style text rect"
                                         : "Compact editor equals the initialized style text rect");
            verify(mode != 0 || (editorRect.top() >= iconRect.bottom()
                       && !editorRect.intersects(iconRect)),
                   "Icons short-name editor is below the icon and does not cover it");

            QLineEdit positionedEditor(geometryView.viewport());
            nameDelegate->updateEditorGeometry(&positionedEditor, option, index);
            verify(positionedEditor.geometry() == editorRect,
                   mode == 0 ? "Icons updateEditorGeometry uses the name geometry contract"
                             : mode == 1 ? "List updateEditorGeometry uses the name geometry contract"
                                         : "Compact updateEditorGeometry uses the name geometry contract");

            QWidget *created = nameDelegate->createEditor(
                geometryView.viewport(), option, index);
            auto *lineEditor = qobject_cast<QLineEdit *>(created);
            auto *iconEditor = dynamic_cast<directory_view_detail::IconNameEditor *>(created);
            verify(mode == 0 ? iconEditor != nullptr
                             : lineEditor && !lineEditor->hasFrame()
                                 && lineEditor->textMargins() == QMargins(),
                   mode == 0 ? "Icons uses the wrapped filename editor"
                             : "list editor removes frame and content margins that shift the text");
            verify(mode != 0 || (iconEditor
                       && iconEditor->lineWrapMode() == QPlainTextEdit::WidgetWidth
                       && iconEditor->wordWrapMode()
                           == QTextOption::WrapAtWordBoundaryOrAnywhere),
                   "Icons editor wraps at the bounded widget width");
            verify(mode == 0
                       ? nameDelegate->suppressesOriginalIconText(index)
                       : !nameDelegate->suppressesOriginalIconText(index),
                   mode == 0
                       ? "active short-name Icons editor suppresses the original label"
                       : "List and Compact editors do not enter Icons text suppression");
            if (lineEditor) {
                nameDelegate->updateEditorGeometry(lineEditor, option, index);
                verify(qAbs(lineEditor->geometry().center().y()
                                - editorRect.center().y()) <= 1,
                       mode == 0 ? "Icons editor baseline anchor stays on the painted first line"
                                 : mode == 1 ? "List editor vertical anchor matches the text rect"
                                             : "Compact editor vertical anchor matches the text rect");
            }
            delete created;
            app.processEvents();
            verify(!nameDelegate->suppressesOriginalIconText(index),
                   "destroying an editor restores normal label painting");
        }

        applyDirectoryViewLayout(&geometryView, &geometryDetails,
                                 &geometryStack, nullptr, 0);
        QModelIndex geometryIndex = geometryView.currentIndex();
        const QSize gridBefore = geometryView.gridSize();
        FileInfo neighborFile{QStringLiteral("neighbor.txt"), QStringLiteral("text/plain"), {},
            QUrl(QStringLiteral("sftp://example.test/path/neighbor.txt")), false, 1, 0};
        geometryView.addFileItem(
            neighborFile, QIcon(geometryIconPixmap), {}, {}, {}, {});
        geometryView.model()->setData(geometryIndex,
            QStringLiteral("a very long wrapped filename that needs an expanded selected callout.txt"));
        app.processEvents();
        geometryIndex = geometryView.currentIndex();
        QStyleOptionViewItem longOption;
        longOption.rect = geometryView.visualRect(geometryIndex);
        longOption.widget = &geometryView;
        const QRect longEditorRect = nameDelegate->nameEditorRect(longOption, geometryIndex);
        const QRect longIconRect = nameDelegate->decorationRect(
            longOption, geometryIndex);
        const QRect longCellRect = geometryView.visualRect(geometryIndex);
        const QRect neighborCellRect = geometryView.visualRect(
            geometryView.model()->index(1, 0));
        verify(longEditorRect.isValid()
                   && longEditorRect.top() >= longIconRect.bottom()
                   && !longEditorRect.intersects(longIconRect)
                   && longEditorRect.height() >= 2 * QFontMetrics(geometryView.font()).lineSpacing()
                   && geometryView.gridSize() == gridBefore,
               "wrapped selected-name editor has multiple visual lines below the icon without changing grid rows");
        verify(longEditorRect.left() >= longCellRect.left()
                   && longEditorRect.right() <= longCellRect.right(),
               "wrapped Icons editor width stays inside its bounded grid-label cell");
        verify(!longEditorRect.intersects(neighborCellRect),
               "wrapped Icons editor does not intersect the neighboring item cell");

        QStackedWidget splitGeometryStack;
        DirectoryListWidget splitGeometryView;
        DirectoryTreeWidget splitGeometryDetails;
        splitGeometryStack.addWidget(&splitGeometryView);
        splitGeometryStack.addWidget(&splitGeometryDetails);
        splitGeometryView.resize(520, 360);
        splitGeometryView.addFileItem(
            geometryFile, QIcon(geometryIconPixmap), {}, {}, {}, {});
        splitGeometryStack.show();
        splitGeometryView.setCurrentRow(0);
        applyDirectoryViewLayout(&splitGeometryView, &splitGeometryDetails,
                                 &splitGeometryStack, nullptr, 0);
        QModelIndex splitGeometryIndex = splitGeometryView.currentIndex();
        splitGeometryView.model()->setData(splitGeometryIndex,
            geometryIndex.data(Qt::DisplayRole));
        splitGeometryView.selectionModel()->setCurrentIndex(splitGeometryIndex,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
        app.processEvents();
        QStyleOptionViewItem splitLongOption;
        splitLongOption.rect = splitGeometryView.visualRect(splitGeometryIndex);
        splitLongOption.widget = &splitGeometryView;
        auto *splitNameDelegate = dynamic_cast<ExplorerNameDelegate *>(
            splitGeometryView.itemDelegate());
        verify(splitNameDelegate
                   && splitNameDelegate->nameEditorRect(
                          splitLongOption, splitGeometryIndex).size()
                       == longEditorRect.size(),
               "Primary and Split Icons use the same bounded long-name editor geometry");
        QWidget *splitEditor = splitNameDelegate->createEditor(
            splitGeometryView.viewport(), splitLongOption, splitGeometryIndex);
        verify(splitNameDelegate->suppressesOriginalIconText(splitGeometryIndex),
               "Split active Icons editor suppresses its original label like Primary");
        delete splitEditor;
        app.processEvents();
        verify(!splitNameDelegate->suppressesOriginalIconText(splitGeometryIndex),
               "Split label painting returns when its editor is destroyed");
        splitGeometryStack.hide();

        int renameRequests = 0;
        QUrl requestedSource;
        QString requestedName;
        geometryView.setRenameRequestHandler([&](const QUrl &source, const QString &name) {
            ++renameRequests; requestedSource = source; requestedName = name;
        });
        geometryView.selectionModel()->setCurrentIndex(geometryIndex,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
        geometryStack.show();
        geometryView.show();
        geometryView.setFocus();
        app.processEvents();
        QStyleOptionViewItem liveLongOption;
        liveLongOption.rect = geometryView.visualRect(geometryIndex);
        liveLongOption.widget = &geometryView;
        const QRect liveLongEditorRect = nameDelegate->nameEditorRect(
            liveLongOption, geometryIndex);
        geometryView.edit(geometryIndex);
        app.processEvents();
        directory_view_detail::IconNameEditor *cancelEditor = nullptr;
        for (QPlainTextEdit *candidate : geometryView.findChildren<QPlainTextEdit *>()) {
            if (auto *typed = dynamic_cast<directory_view_detail::IconNameEditor *>(candidate)) {
                cancelEditor = typed;
                break;
            }
        }
        verify(cancelEditor && cancelEditor->isVisible(), "explicit Icons rename creates a wrapped editor");
        verify(nameDelegate->suppressesOriginalIconText(geometryIndex)
                   && !nameDelegate->suppressesOriginalIconText(
                       geometryView.model()->index(1, 0)),
               "long-name suppression applies only to the actively edited index");
        const auto activeLayers = nameDelegate->iconPaintLayers(geometryIndex);
        verify(activeLayers.backgroundAndSelection && activeLayers.decoration
                   && !activeLayers.text,
               "active Icons editing suppresses only text, preserving selection and icon layers");
        verify(cancelEditor
                   && cancelEditor->geometry().width() <= liveLongEditorRect.width()
                   && cancelEditor->geometry().height() <= liveLongEditorRect.height()
                   && cancelEditor->geometry().right()
                       <= geometryView.visualRect(geometryIndex).right()
                   && !cancelEditor->geometry().intersects(geometryView.visualRect(
                       geometryView.model()->index(1, 0))),
               "live long-name editor remains bounded after Qt installs its text");

        QTest::keyClick(cancelEditor, Qt::Key_Escape);
        app.processEvents();
        verify(renameRequests == 0
                   && geometryIndex.data(Qt::DisplayRole).toString().startsWith(QStringLiteral("a very long")),
               "Escape cancels without backend request or local model mutation");
        verify(!nameDelegate->suppressesOriginalIconText(geometryIndex),
               "Escape restores original long-name label painting");

        QWidget *commitLifecycleEditor = nameDelegate->createEditor(
            geometryView.viewport(), liveLongOption, geometryIndex);
        verify(nameDelegate->suppressesOriginalIconText(geometryIndex),
               "commit lifecycle re-enters original-label suppression");
        nameDelegate->destroyEditor(commitLifecycleEditor, geometryIndex);
        verify(!nameDelegate->suppressesOriginalIconText(geometryIndex),
               "commit/close editor destruction restores original label painting");
        renameRequests = 0;
        directory_view_detail::IconNameEditor commitEditor;
        commitEditor.setFileName(QStringLiteral("remote-renamed.txt"));
        nameDelegate->setModelData(&commitEditor, geometryView.model(), geometryIndex);
        verify(renameRequests == 1 && requestedSource == geometryFile.url
                   && requestedName == QStringLiteral("remote-renamed.txt")
                   && geometryIndex.data(Qt::DisplayRole).toString().startsWith(QStringLiteral("a very long")),
               "Enter requests backend rename for the source URL without optimistic model edit");

        renameRequests = 0;
        directory_view_detail::IconNameEditor pasteEditor;
        pasteEditor.setFileName(QStringLiteral("archive.tar.gz"));
        pasteEditor.selectFileNameStem();
        verify(pasteEditor.textCursor().selectedText() == QStringLiteral("archive.tar"),
               "Icons editor preserves stem-without-extension selection semantics");
        pasteEditor.setFocus();
        QApplication::clipboard()->setText(QStringLiteral("safe\r\nname.txt"));
        QTest::keyClick(&pasteEditor, Qt::Key_V, Qt::ControlModifier);
        verify(!pasteEditor.toPlainText().contains(QLatin1Char('\r'))
                   && !pasteEditor.toPlainText().contains(QLatin1Char('\n')),
               "pasted CR/LF cannot enter the filename value");
        nameDelegate->setModelData(&pasteEditor, geometryView.model(), geometryIndex);
        verify(renameRequests == 1 && !requestedName.contains(QLatin1Char('\n'))
                   && !requestedName.contains(QLatin1Char('\r')),
               "sanitized multiline paste reaches the existing rename backend as one filename");

        applyDirectoryViewLayout(&activationView, &activationDetails,
                                 &activationStack, nullptr, 0);
        app.processEvents();
        auto *staleDetailsEditor = activationDetails.findChild<QLineEdit *>();
        verify(!activationDetails.isEditingName()
                   && (!staleDetailsEditor || !staleDetailsEditor->isVisible())
                   && !qobject_cast<QLineEdit *>(QApplication::focusWidget()),
               "switching away from Details leaves no stale editor or focus");
        Q_UNUSED(paneName)
    }

    {
        QStackedWidget primaryStack;
        QStackedWidget splitStack;
        DirectoryListWidget primaryList;
        DirectoryListWidget splitList;
        DirectoryTreeWidget primaryDetails;
        DirectoryTreeWidget splitDetails;
        primaryStack.addWidget(&primaryList);
        primaryStack.addWidget(&primaryDetails);
        splitStack.addWidget(&splitList);
        splitStack.addWidget(&splitDetails);
        configureDirectoryViewWidgets(&primaryList, &primaryDetails);
        configureDirectoryViewWidgets(&splitList, &splitDetails);
        primaryList.resize(420, 300);
        splitList.resize(420, 300);
        primaryDetails.resize(420, 300);
        splitDetails.resize(420, 300);

        const FileInfo shortFile{QStringLiteral("short.txt"), QStringLiteral("text/plain"), {},
            QUrl(QStringLiteral("sftp://example.test/share/short.txt")), false, 10, 0};
        const FileInfo longFile{QStringLiteral("a very long file name used for primary split parity.txt"),
            QStringLiteral("text/plain"), {},
            QUrl(QStringLiteral("sftp://example.test/share/long.txt")), false, 20, 0};
        QPixmap parityPixmap(16, 16);
        parityPixmap.fill(Qt::green);
        const QIcon parityIcon(parityPixmap);
        for (DirectoryListWidget *view : {&primaryList, &splitList}) {
            view->addFileItem(shortFile, parityIcon, {}, {}, {}, {});
            view->addFileItem(longFile, parityIcon, {}, {}, {}, {});
        }
        for (DirectoryTreeWidget *view : {&primaryDetails, &splitDetails}) {
            addDirectoryGroupHeader(view, QStringLiteral("Text"));
            auto *item = new QTreeWidgetItem(
                view, QStringList{shortFile.name, QStringLiteral("Text"),
                                  QStringLiteral("10 B"), QStringLiteral("Now")});
            item->setIcon(0, parityIcon);
            item->setData(0, directory_view_detail::FileItemRole, true);
        }

        verify(primaryList.iconSize() == splitList.iconSize()
                   && primaryList.spacing() == splitList.spacing()
                   && primaryDetails.iconSize() == splitDetails.iconSize(),
               "Primary and Split start with shared list and Details metrics");
        // All five columns: col0=Stretch, col1=Interactive 220, col2=Interactive 90,
        // col3=Interactive 150, col4=Interactive 320 (hidden). ResizeToContents is
        // explicitly absent for cols 2/3 to guarantee parity across directories.

        verify(primaryDetails.header()->height() == splitDetails.header()->height()
                   && primaryDetails.columnWidth(1) == splitDetails.columnWidth(1)
                   && primaryDetails.columnWidth(1) == 220
                   && primaryDetails.columnWidth(2) == splitDetails.columnWidth(2)
                   && primaryDetails.columnWidth(2) == 90
                   && primaryDetails.columnWidth(3) == splitDetails.columnWidth(3)
                   && primaryDetails.columnWidth(3) == 150
                   && primaryDetails.isColumnHidden(4) && splitDetails.isColumnHidden(4),
               "Details all five columns share identical deterministic defaults");
        primaryDetails.setColumnHidden(4, false);
        splitDetails.setColumnHidden(4, false);
        verify(primaryDetails.columnWidth(4) == splitDetails.columnWidth(4)
                   && primaryDetails.columnWidth(4) == 320,
               "Details column 4 has width 320 when unhidden");
        primaryDetails.setColumnHidden(4, true);
        splitDetails.setColumnHidden(4, true);
        verify(primaryDetails.header()->sectionResizeMode(0) == QHeaderView::Stretch
                   && primaryDetails.header()->sectionResizeMode(1) == QHeaderView::Interactive
                   && primaryDetails.header()->sectionResizeMode(2) == QHeaderView::Interactive
                   && primaryDetails.header()->sectionResizeMode(3) == QHeaderView::Interactive
                   && splitDetails.header()->sectionResizeMode(2) == QHeaderView::Interactive
                   && splitDetails.header()->sectionResizeMode(3) == QHeaderView::Interactive,
               "Size and Date columns are Interactive not ResizeToContents for layout determinism");

        for (int step : {0, 4, 8}) {
            applyDirectoryViewLayout(&primaryList, &primaryDetails, &primaryStack,
                                     nullptr, 0, step);
            applyDirectoryViewLayout(&splitList, &splitDetails, &splitStack,
                                     nullptr, 0, step);
            primaryStack.show();
            splitStack.show();
            app.processEvents();
            verify(primaryList.iconSize() == splitList.iconSize()
                       && primaryList.gridSize() == splitList.gridSize()
                       && primaryList.spacing() == splitList.spacing(),
                   "Icons 24/56/128 use identical extent, grid, and spacing");
            for (int row : {0, 1}) {
                const QModelIndex primaryIndex = primaryList.item(row);
                const QModelIndex splitIndex = splitList.item(row);
                verify(primaryList.visualRect(primaryIndex).size()
                           == splitList.visualRect(splitIndex).size(),
                       "short and long Icons labels keep equal Primary/Split cells");
            }
            auto *primaryDelegate = dynamic_cast<ExplorerNameDelegate *>(primaryList.itemDelegate());
            auto *splitDelegate = dynamic_cast<ExplorerNameDelegate *>(splitList.itemDelegate());
            QStyleOptionViewItem primaryOption;
            QStyleOptionViewItem splitOption;
            primaryOption.rect = primaryList.visualRect(primaryList.item(1));
            splitOption.rect = splitList.visualRect(splitList.item(1));
            primaryOption.widget = &primaryList;
            splitOption.widget = &splitList;
            verify(primaryDelegate && splitDelegate
                       && primaryDelegate->nameEditorRect(primaryOption, primaryList.item(1)).size()
                           == splitDelegate->nameEditorRect(splitOption, splitList.item(1)).size()
                       && primaryDelegate->decorationRect(primaryOption, primaryList.item(1)).size()
                           == splitDelegate->decorationRect(splitOption, splitList.item(1)).size(),
                   "Icons label, icon, and inline rename rectangles are shared");
        }

        for (int mode : {1, 3}) {
            applyDirectoryViewLayout(&primaryList, &primaryDetails, &primaryStack,
                                     nullptr, mode);
            applyDirectoryViewLayout(&splitList, &splitDetails, &splitStack,
                                     nullptr, mode);
            app.processEvents();
            verify(primaryList.iconSize() == splitList.iconSize()
                       && primaryList.spacing() == splitList.spacing()
                       && primaryList.visualRect(primaryList.item(0)).size()
                           == splitList.visualRect(splitList.item(0)).size(),
                   mode == 1
                       ? "List row icon text and padding metrics are shared"
                       : "Compact row icon text and flow metrics are shared");
        }

        applyDirectoryViewLayout(&primaryList, &primaryDetails, &primaryStack, nullptr, 2);
        applyDirectoryViewLayout(&splitList, &splitDetails, &splitStack, nullptr, 2);
        app.processEvents();
        verify(primaryDetails.sizeHintForRow(1) == splitDetails.sizeHintForRow(1)
                   && primaryDetails.visualItemRect(primaryDetails.topLevelItem(1)).size()
                       == splitDetails.visualItemRect(splitDetails.topLevelItem(1)).size(),
               "Details row icon text selection and focus geometry are shared");
        verify(primaryDetails.visualItemRect(primaryDetails.topLevelItem(0)).size()
                   == splitDetails.visualItemRect(splitDetails.topLevelItem(0)).size(),
               "grouping header metrics are shared");

        primaryList.setProperty("active", true);
        splitList.setProperty("active", false);
        applyDirectoryViewLayout(&primaryList, &primaryDetails, &primaryStack, nullptr, 0, 4);
        applyDirectoryViewLayout(&splitList, &splitDetails, &splitStack, nullptr, 0, 4);
        app.processEvents();
        verify(primaryList.visualRect(primaryList.item(0)).size()
                   == splitList.visualRect(splitList.item(0)).size(),
               "active and inactive pane cues do not alter content geometry");
        splitList.resize(150, 220);
        app.processEvents();
        verify(splitList.gridSize().isValid()
                   && splitList.visualRect(splitList.item(0)).isValid(),
               "narrow Split keeps valid non-overlapping Icons geometry");
    }

    // =========================================================================
    // Stage 3.2.2: Regression Safety Foundation & Model Behavior Contract
    // =========================================================================
    {
        // 1. Empty catalog handling across all 4 view modes
        QStackedWidget emptyStack;
        DirectoryListWidget emptyList;
        DirectoryTreeWidget emptyDetails;
        emptyStack.addWidget(&emptyList);
        emptyStack.addWidget(&emptyDetails);
        configureDirectoryViewWidgets(&emptyList, &emptyDetails);

        for (int mode : {0, 1, 2, 3}) {
            applyDirectoryViewLayout(&emptyList, &emptyDetails, &emptyStack, nullptr, mode);
            app.processEvents();
            if (mode == 2) {
                verify(emptyDetails.topLevelItemCount() == 0,
                       "empty catalog in Details mode has 0 top-level items");
                verify(emptyDetails.selectedItems().isEmpty(),
                       "empty catalog in Details mode has empty selection");
            } else {
                verify(emptyList.count() == 0,
                       "empty catalog in List/Icons/Compact mode has 0 items");
                verify(emptyList.selectedItems().isEmpty(),
                       "empty catalog in List/Icons/Compact mode has empty selection");
            }
        }

        // 2. Realistic multi-item catalog (folders + files)
        const QUrl fixtureUrl(QStringLiteral("file:///tmp/stage32_safety_fixture"));
        QList<FileInfo> testFiles = {
            {QStringLiteral("Folder_Z"), QStringLiteral("inode/directory"), {}, childUrlWithName(fixtureUrl, QStringLiteral("Folder_Z")), true, -1, 1000},
            {QStringLiteral("Folder_A"), QStringLiteral("inode/directory"), {}, childUrlWithName(fixtureUrl, QStringLiteral("Folder_A")), true, -1, 2000},
            {QStringLiteral("doc1.pdf"), QStringLiteral("application/pdf"), {}, childUrlWithName(fixtureUrl, QStringLiteral("doc1.pdf")), false, 1024, 3000},
            {QStringLiteral("image1.png"), QStringLiteral("image/png"), {}, childUrlWithName(fixtureUrl, QStringLiteral("image1.png")), false, 2048, 4000},
            {QStringLiteral("text1.txt"), QStringLiteral("text/plain"), {}, childUrlWithName(fixtureUrl, QStringLiteral("text1.txt")), false, 512, 5000},
            {QStringLiteral("video1.mp4"), QStringLiteral("video/mp4"), {}, childUrlWithName(fixtureUrl, QStringLiteral("video1.mp4")), false, 8192, 6000},
        };

        QStackedWidget multiStack;
        DirectoryListWidget multiList;
        DirectoryTreeWidget multiDetails;
        multiStack.addWidget(&multiList);
        multiStack.addWidget(&multiDetails);
        configureDirectoryViewWidgets(&multiList, &multiDetails);

        for (const auto &f : testFiles) {
            addDirectoryFileItems(&multiList, &multiDetails, f, QIcon(),
                                  f.isDir ? QStringLiteral("Folder") : QStringLiteral("File"),
                                  f.isDir ? QString() : QString::number(f.size) + QStringLiteral(" B"),
                                  QStringLiteral("2026-10-20"),
                                  {parentLocationForDisplay(f.url)});
        }

        verify(multiList.count() == testFiles.size(),
               "multi-item list view populated with exact file count");
        verify(multiDetails.topLevelItemCount() == testFiles.size(),
               "multi-item details view populated with exact file count");

        // 3. View mode transition cycles (0 -> 2 -> 0 -> 1 -> 2 -> 3 -> 0)
        for (int mode : {0, 2, 0, 1, 2, 3, 0}) {
            applyDirectoryViewLayout(&multiList, &multiDetails, &multiStack, nullptr, mode);
            app.processEvents();
            if (mode == 2) {
                verify(multiStack.currentWidget() == &multiDetails,
                       "Details mode activates DirectoryTreeWidget in stack");
            } else {
                verify(multiStack.currentWidget() == &multiList,
                       "Icons/List/Compact modes activate DirectoryListWidget in stack");
            }
        }

        // 4. Selection preservation contract across view mode transitions
        // 4a. Single selection
        const QString selectedUrl1 = testFiles.at(2).url.toString();
        multiList.clearSelection();
        multiDetails.clearSelection();
        for (int r = 0; r < multiList.count(); ++r) {
            if (multiList.item(r).data(directory_view_detail::UrlRole).toString() == selectedUrl1) {
                multiList.setRowSelected(r, true);
                break;
            }
        }
        DirectorySelectionSnapshot snapSingle = captureDirectorySelection(&multiList, &multiDetails, false);
        verify(snapSingle.urls.size() == 1 && snapSingle.urls.contains(selectedUrl1),
               "single selection captured correctly from list view");

        // Transition Icons (0) -> Details (2)
        applyDirectoryViewLayout(&multiList, &multiDetails, &multiStack, nullptr, 2);
        restoreDirectorySelection(&multiList, &multiDetails, snapSingle);
        app.processEvents();
        QSet<QString> detailsSelected = selectedDirectoryDetailsUrls(&multiDetails);
        verify(detailsSelected.size() == 1 && detailsSelected.contains(selectedUrl1),
               "single selection preserved exactly in Details view");

        // Transition Details (2) -> Compact (3)
        DirectorySelectionSnapshot snapFromDetails = captureDirectorySelection(&multiList, &multiDetails, true);
        applyDirectoryViewLayout(&multiList, &multiDetails, &multiStack, nullptr, 3);
        restoreDirectorySelection(&multiList, &multiDetails, snapFromDetails);
        app.processEvents();
        QSet<QString> compactSelected = selectedDirectoryListUrls(&multiList);
        verify(compactSelected.size() == 1 && compactSelected.contains(selectedUrl1),
               "single selection preserved exactly in Compact view");

        // 4b. Multi-selection (disjoint items)
        const QString selectedUrl2 = testFiles.at(4).url.toString();
        const QSet<QString> multiExpected{selectedUrl1, selectedUrl2};
        multiList.clearSelection();
        for (int r = 0; r < multiList.count(); ++r) {
            const QString u = multiList.item(r).data(directory_view_detail::UrlRole).toString();
            if (multiExpected.contains(u)) multiList.setRowSelected(r, true);
        }
        DirectorySelectionSnapshot snapMulti = captureDirectorySelection(&multiList, &multiDetails, false);
        verify(snapMulti.urls == multiExpected,
               "disjoint multi-selection captured correctly");

        // Transition Compact (3) -> Details (2) -> List (1) -> Icons (0)
        applyDirectoryViewLayout(&multiList, &multiDetails, &multiStack, nullptr, 2);
        restoreDirectorySelection(&multiList, &multiDetails, snapMulti);
        app.processEvents();
        verify(selectedDirectoryDetailsUrls(&multiDetails) == multiExpected,
               "disjoint multi-selection preserved in Details view");

        applyDirectoryViewLayout(&multiList, &multiDetails, &multiStack, nullptr, 1);
        restoreDirectorySelection(&multiList, &multiDetails, snapMulti);
        app.processEvents();
        verify(selectedDirectoryListUrls(&multiList) == multiExpected,
               "disjoint multi-selection preserved in List view");

        // 4c. Select All (Ctrl+A equivalent)
        QSet<QString> allUrls;
        for (const auto &f : testFiles) allUrls.insert(f.url.toString());
        for (int r = 0; r < multiList.count(); ++r) multiList.setRowSelected(r, true);
        DirectorySelectionSnapshot snapAll = captureDirectorySelection(&multiList, &multiDetails, false);
        verify(snapAll.urls == allUrls, "select-all captured all URLs");

        applyDirectoryViewLayout(&multiList, &multiDetails, &multiStack, nullptr, 2);
        restoreDirectorySelection(&multiList, &multiDetails, snapAll);
        app.processEvents();
        verify(selectedDirectoryDetailsUrls(&multiDetails) == allUrls,
               "select-all preserved completely in Details view");

        // 5. Split View Independence Contract
        QStackedWidget primStack, splStack;
        DirectoryListWidget primList, splList;
        DirectoryTreeWidget primDetails, splDetails;
        primStack.addWidget(&primList); primStack.addWidget(&primDetails);
        splStack.addWidget(&splList); splStack.addWidget(&splDetails);
        configureDirectoryViewWidgets(&primList, &primDetails);
        configureDirectoryViewWidgets(&splList, &splDetails);

        for (const auto &f : testFiles) {
            addDirectoryFileItems(&primList, &primDetails, f, QIcon(), QStringLiteral("Type"), QStringLiteral("1 KB"), QStringLiteral("Date"));
            addDirectoryFileItems(&splList, &splDetails, f, QIcon(), QStringLiteral("Type"), QStringLiteral("1 KB"), QStringLiteral("Date"));
        }

        // Primary in Icons (0), Split in Details (2)
        applyDirectoryViewLayout(&primList, &primDetails, &primStack, nullptr, 0);
        applyDirectoryViewLayout(&splList, &splDetails, &splStack, nullptr, 2);
        app.processEvents();
        verify(primStack.currentWidget() == &primList, "Primary pane displays DirectoryListWidget");
        verify(splStack.currentWidget() == &splDetails, "Split pane displays DirectoryTreeWidget");

        // Independent selections: select file 2 in Primary, file 3 in Split
        primList.setRowSelected(2, true);
        splDetails.topLevelItem(3)->setSelected(true);
        verify(selectedDirectoryListUrls(&primList).size() == 1, "Primary has independent single selection");
        verify(selectedDirectoryDetailsUrls(&splDetails).size() == 1, "Split has independent single selection");
        verify(selectedDirectoryListUrls(&primList) != selectedDirectoryDetailsUrls(&splDetails),
               "Primary and Split maintain disjoint independent selections");

        // Changing Primary view mode to List (1) does not mutate Split view mode
        applyDirectoryViewLayout(&primList, &primDetails, &primStack, nullptr, 1);
        app.processEvents();
        verify(primStack.currentWidget() == &primList, "Primary switched to List");
        verify(splStack.currentWidget() == &splDetails, "Split remains in Details");
        verify(selectedDirectoryDetailsUrls(&splDetails).size() == 1,
               "Split selection intact after Primary view layout switch");

        // 6. Search location presentation: Location column 4 hidden in normal, visible in search
        primDetails.setColumnHidden(4, true);
        verify(primDetails.isColumnHidden(4),
               "Location column 4 is hidden during ordinary folder browsing");
        primDetails.setColumnHidden(4, false);
        verify(!primDetails.isColumnHidden(4),
               "Location column 4 is visible during search results display");
        primDetails.setColumnHidden(4, true);

        // 7. Grouping header contract
        DirectoryTreeWidget groupTree;
        addDirectoryGroupHeader(&groupTree, QStringLiteral("Images (5)"));
        addDirectoryFileItems(&multiList, &groupTree, testFiles.at(3), QIcon(), QStringLiteral("PNG"), QStringLiteral("2 KB"), QStringLiteral("Today"));
        verify(groupTree.topLevelItemCount() == 2,
               "grouping tree holds category header and item");
        verify(!groupTree.topLevelItem(0)->data(0, directory_view_detail::FileItemRole).toBool(),
               "group header has FileItemRole == false");
        verify(groupTree.topLevelItem(1)->data(0, directory_view_detail::FileItemRole).toBool(),
               "group item has FileItemRole == true");
        groupTree.topLevelItem(0)->setSelected(true);
        groupTree.topLevelItem(1)->setSelected(true);
        QSet<QString> groupSelected = selectedDirectoryDetailsUrls(&groupTree);
        verify(groupSelected.size() == 1 && groupSelected.contains(testFiles.at(3).url.toString()),
               "selectedDirectoryDetailsUrls filters out non-file group headers");
    }

    // =========================================================================
    // Stage 3.2.3: View Profile Fallback & Lazy Model Contracts (Scenarios A–G)
    // =========================================================================
    {
        QTemporaryDir stage32Dir;
        verify(stage32Dir.isValid(), "stage32 temporary directory");

        auto createTestFolderWithFile = [&](const QString &folderName, const QString &fileName) -> QUrl {
            const QString folderPath = stage32Dir.filePath(folderName);
            verify(QDir().mkpath(folderPath), "create stage32 test folder");
            QFile f(folderPath + QLatin1Char('/') + fileName);
            verify(f.open(QIODevice::WriteOnly) && f.write("stage32_content") == 15, "create stage32 test file");
            f.close();
            return QUrl::fromLocalFile(folderPath);
        };

        QSettings appSettings;

        // --- Scenario A ---
        // Global default Icons (0), Folder A has Details (2), navigate A -> B (unprofiled)
        // => B opens in Icons (0), List populated (1), Details empty (0).
        {
            appSettings.setValue(QStringLiteral("directory/viewMode"), 0);
            verify(DirectoryViewSettings::globalDefault().viewMode == 0, "Scenario A: global default is Icons (0)");

            const QUrl urlA = createTestFolderWithFile(QStringLiteral("scenA_dirA"), QStringLiteral("fileA.txt"));
            const QUrl urlB = createTestFolderWithFile(QStringLiteral("scenA_dirB"), QStringLiteral("fileB.txt"));

            DirectoryViewSettings::saveExplicitProfile(urlA, {2, 0, 0, true, 0, false});
            verify(DirectoryViewSettings::hasExplicitProfile(urlA), "urlA has explicit Details profile");
            verify(!DirectoryViewSettings::hasExplicitProfile(urlB) && !DirectoryViewSettings::hasInheritedRule(urlB),
                   "urlB is unprofiled");

            ThisPcWindow window(urlA, false);
            window.show();
            verify(QTest::qWaitFor([&] { return window.m_directoryDetails->topLevelItemCount() == 1; }, 5000),
                   "Scenario A: urlA loaded in Details");
            verify(window.m_directoryViewMode == 2, "Scenario A: urlA viewMode == 2");
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryDetails,
                   "Scenario A: urlA stack shows Details widget");
            verify(window.m_directoryList->count() == 0,
                   "Scenario A: urlA inactive List model is lazy / empty (0)");

            window.navigateTo(urlB, true);
            verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 1; }, 5000),
                   "Scenario A: urlB loaded in List");
            verify(window.m_directoryViewMode == 0,
                   "Scenario A: urlB falls back to global default Icons (0) without inheriting urlA Details");
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryList,
                   "Scenario A: urlB stack shows List widget");
            verify(window.m_directoryDetails->topLevelItemCount() == 0,
                   "Scenario A: urlB inactive Details model is lazy / empty (0)");
        }

        // --- Scenario B ---
        // Global default Details (2), Folder A has Icons (0), navigate A -> B (unprofiled)
        // => B opens in Details (2), Details populated (1), List empty (0).
        {
            appSettings.setValue(QStringLiteral("directory/viewMode"), 2);
            verify(DirectoryViewSettings::globalDefault().viewMode == 2, "Scenario B: global default is Details (2)");

            const QUrl urlB_A = createTestFolderWithFile(QStringLiteral("scenB_dirA"), QStringLiteral("fileBA.txt"));
            const QUrl urlB_B = createTestFolderWithFile(QStringLiteral("scenB_dirB"), QStringLiteral("fileBB.txt"));

            DirectoryViewSettings::saveExplicitProfile(urlB_A, {0, 0, 0, true, 0, false});
            verify(DirectoryViewSettings::hasExplicitProfile(urlB_A), "urlB_A has explicit Icons profile");
            verify(!DirectoryViewSettings::hasExplicitProfile(urlB_B) && !DirectoryViewSettings::hasInheritedRule(urlB_B),
                   "urlB_B is unprofiled");

            ThisPcWindow window(urlB_A, false);
            window.show();
            verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 1; }, 5000),
                   "Scenario B: urlB_A loaded in Icons");
            verify(window.m_directoryViewMode == 0, "Scenario B: urlB_A viewMode == 0");
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryList,
                   "Scenario B: urlB_A stack shows List widget");
            verify(window.m_directoryDetails->topLevelItemCount() == 0,
                   "Scenario B: urlB_A inactive Details model is lazy / empty (0)");

            window.navigateTo(urlB_B, true);
            verify(QTest::qWaitFor([&] { return window.m_directoryDetails->topLevelItemCount() == 1; }, 5000),
                   "Scenario B: urlB_B loaded in Details");
            verify(window.m_directoryViewMode == 2,
                   "Scenario B: urlB_B falls back to global default Details (2) without inheriting urlB_A Icons");
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryDetails,
                   "Scenario B: urlB_B stack shows Details widget");
            verify(window.m_directoryList->count() == 0,
                   "Scenario B: urlB_B inactive List model is lazy / empty (0)");
        }

        // --- Scenario C ---
        // Global default Details (2), explicit profile Compact (3) on Folder C
        // => C opens in Compact (3) [wins over global default], List populated (1), Details empty (0).
        {
            appSettings.setValue(QStringLiteral("directory/viewMode"), 2);
            const QUrl urlC = createTestFolderWithFile(QStringLiteral("scenC_dir"), QStringLiteral("fileC.txt"));
            DirectoryViewSettings::saveExplicitProfile(urlC, {3, 0, 0, true, 0, false});

            ThisPcWindow window(urlC, false);
            window.show();
            verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 1; }, 5000),
                   "Scenario C: urlC loaded in Compact");
            verify(window.m_directoryViewMode == 3,
                   "Scenario C: explicit Compact profile (3) overrides global default Details (2)");
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryList,
                   "Scenario C: urlC stack shows List widget");
            verify(window.m_directoryDetails->topLevelItemCount() == 0,
                   "Scenario C: urlC inactive Details model is lazy / empty (0)");
        }

        // --- Scenario D ---
        // Global default Icons (0), inherited rule on Parent Details (2), navigate to Child
        // => Child opens in Details (2) [inherited rule wins over global default], Details populated (1), List empty (0).
        {
            appSettings.setValue(QStringLiteral("directory/viewMode"), 0);
            const QUrl urlD_parent = createTestFolderWithFile(QStringLiteral("scenD_parent"), QStringLiteral("parent.txt"));
            const QUrl urlD_child = createTestFolderWithFile(QStringLiteral("scenD_parent/child"), QStringLiteral("child.txt"));

            DirectoryViewSettings::setInheritedRule(urlD_parent, {2, 0, 0, true, 0, false}, true);
            verify(!DirectoryViewSettings::hasExplicitProfile(urlD_child), "urlD_child has no explicit profile");
            verify(DirectoryViewSettings::hasInheritedRule(urlD_parent), "urlD_parent has inherited rule configured");
            verify(DirectoryViewSettings::resolveProfile(urlD_child).viewMode == 2, "urlD_child resolves rule from parent");

            ThisPcWindow window(urlD_child, false);
            window.show();
            verify(QTest::qWaitFor([&] { return window.m_directoryDetails->topLevelItemCount() == 1; }, 5000),
                   "Scenario D: urlD_child loaded in Details");
            verify(window.m_directoryViewMode == 2,
                   "Scenario D: inherited rule Details (2) overrides global default Icons (0)");
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryDetails,
                   "Scenario D: urlD_child stack shows Details widget");
            verify(window.m_directoryList->count() == 0,
                   "Scenario D: urlD_child inactive List model is lazy / empty (0)");
        }

        // --- Scenario E ---
        // Tab switching between different profiles:
        // Tab 0 has Icons (0), Tab 1 has Details (2).
        // => Switching tabs switches active view widget and model correctly.
        {
            const QUrl urlE_1 = createTestFolderWithFile(QStringLiteral("scenE_tab1"), QStringLiteral("e1.txt"));
            const QUrl urlE_2 = createTestFolderWithFile(QStringLiteral("scenE_tab2"), QStringLiteral("e2.txt"));
            DirectoryViewSettings::saveExplicitProfile(urlE_1, {0, 0, 0, true, 0, false});
            DirectoryViewSettings::saveExplicitProfile(urlE_2, {2, 0, 0, true, 0, false});

            ThisPcWindow window(urlE_1, false);
            window.show();
            verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 1; }, 5000),
                   "Scenario E: Tab 0 initial load");
            verify(window.m_activeTab == 0 && window.m_directoryViewMode == 0, "Scenario E: Tab 0 is Icons");

            window.createNewTab(urlE_2, true);
            verify(QTest::qWaitFor([&] { return window.m_directoryDetails->topLevelItemCount() == 1; }, 5000),
                   "Scenario E: Tab 1 loaded in Details");
            verify(window.m_activeTab == 1 && window.m_directoryViewMode == 2,
                   "Scenario E: Tab 1 switched and viewMode == 2");
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryDetails,
                   "Scenario E: Tab 1 stack shows Details widget");
            verify(window.m_directoryList->count() == 0,
                   "Scenario E: Tab 1 inactive List model is lazy / empty (0)");

            window.switchToTab(0);
            verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 1; }, 5000),
                   "Scenario E: Tab 0 switched back to Icons");
            verify(window.m_activeTab == 0 && window.m_directoryViewMode == 0,
                   "Scenario E: Tab 0 active and viewMode == 0");
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryList,
                   "Scenario E: Tab 0 stack shows List widget");
            verify(window.m_directoryDetails->topLevelItemCount() == 0,
                   "Scenario E: Tab 0 inactive Details model is lazy / empty (0)");
        }

        // --- Scenario F ---
        // QuietRefresh of the same directory preserves view mode and lazy model state.
        {
            const QUrl urlF = createTestFolderWithFile(QStringLiteral("scenF_dir"), QStringLiteral("f1.txt"));
            DirectoryViewSettings::saveExplicitProfile(urlF, {2, 0, 0, true, 0, false});

            ThisPcWindow window(urlF, false);
            window.show();
            verify(QTest::qWaitFor([&] { return window.m_directoryDetails->topLevelItemCount() == 1; }, 5000),
                   "Scenario F: urlF loaded in Details");
            verify(window.m_directoryViewMode == 2, "Scenario F: urlF viewMode == 2");
            verify(window.m_directoryDetails->topLevelItemCount() == 1, "Scenario F: details has 1 item");
            verify(window.m_directoryList->count() == 0, "Scenario F: list is lazy / empty (0)");

            // Create second file on disk
            QFile f2(stage32Dir.filePath(QStringLiteral("scenF_dir/f2.txt")));
            verify(f2.open(QIODevice::WriteOnly) && f2.write("f2_content") == 10, "Scenario F: create f2");
            f2.close();

            window.loadDirectory(window.m_navigation.currentUrl(), false, DirectoryReloadMode::QuietRefresh);
            verify(QTest::qWaitFor([&] { return window.m_directoryDetails->topLevelItemCount() == 2; }, 5000),
                   "Scenario F: QuietRefresh updated Details model to 2 items");
            verify(window.m_directoryViewMode == 2, "Scenario F: viewMode remains Details (2)");
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryDetails,
                   "Scenario F: stack remains Details widget");
            verify(window.m_directoryList->count() == 0,
                   "Scenario F: inactive List model remains lazy / empty (0)");
        }

        // --- Scenario G ---
        // Search results vs regular folder mode semantics.
        {
            const QUrl urlG_reg = createTestFolderWithFile(QStringLiteral("scenG_dir"), QStringLiteral("g1.txt"));
            DirectoryViewSettings::saveExplicitProfile(urlG_reg, {2, 0, 0, true, 0, false});

            ThisPcWindow window(urlG_reg, false);
            window.show();
            verify(QTest::qWaitFor([&] { return window.m_directoryDetails->topLevelItemCount() == 1; }, 5000),
                   "Scenario G: regular folder loaded in Details");
            verify(window.m_directoryDetails->isColumnHidden(4),
                   "Scenario G: Location column hidden in regular directory Details");
            verify(window.m_directoryDetails->topLevelItemCount() == 1, "Scenario G: details has 1 item");
            verify(window.m_directoryList->count() == 0, "Scenario G: list is lazy / empty (0)");

            // Switch to search
            const QUrl searchLoc = makeSearchLocation(QStringLiteral("g1"), 0, urlG_reg, 0, 0, 0);
            window.navigateTo(searchLoc, true);
            verify(QTest::qWaitFor([&] { return !window.m_searchController->isRunning(); }, 5000),
                   "Scenario G: search completed");
            verify(!window.m_directoryDetails->isColumnHidden(4),
                   "Scenario G: Location column visible in search Details");

            // Navigate back to regular folder
            window.navigateTo(urlG_reg, true);
            verify(QTest::qWaitFor([&] { return window.m_directoryDetails->topLevelItemCount() == 1; }, 5000),
                   "Scenario G: navigated back to regular folder");
            verify(window.m_directoryDetails->isColumnHidden(4),
                   "Scenario G: Location column hidden again in regular directory Details");
            verify(window.m_directoryViewMode == 2, "Scenario G: viewMode is Details (2)");
            verify(window.m_directoryDetails->topLevelItemCount() == 1, "Scenario G: details has 1 item");
            verify(window.m_directoryList->count() == 0, "Scenario G: list is lazy / empty (0)");
        }

        // Reset global default to 0 (clean state)
        appSettings.setValue(QStringLiteral("directory/viewMode"), 0);
    }

    qInfo("PASS: %d per-folder view settings assertions", checks);
    return 0;
}
