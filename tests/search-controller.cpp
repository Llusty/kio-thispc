/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "searchpopover.h"
#include "searchprogressbar.h"
#include "advancedsearchfiltersdialog.h"
#include <QToolBar>

// Real filename-search workers only scan this suite's temporary directories.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static QSet<QString> paths(const QList<FileInfo> &files)
{
    QSet<QString> result;
    for (const auto &file : files) result.insert(file.url.toLocalFile());
    return result;
}

static void waitForSearch(SearchController &search)
{
    verify(QTest::qWaitFor([&] { return !search.isRunning(); }, 10000), "KIO search completes");
}

static KIO::UDSEntry entry(const QString &path)
{
    KIO::UDSEntry value;
    value.fastInsert(KIO::UDSEntry::UDS_NAME, QFileInfo(path).fileName());
    value.fastInsert(KIO::UDSEntry::UDS_URL, QUrl::fromLocalFile(path).toString());
    value.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG);
    value.fastInsert(KIO::UDSEntry::UDS_SIZE, 12);
    return value;
}

static void testFilters()
{
    QMimeDatabase db;
    FileInfo file;
    file.name = "sample.txt";
    file.mimeType = "text/plain";
    file.modificationTime = QDateTime::currentSecsSinceEpoch();
    file.size = 50;
    verify(SearchController::matchesFile(file, db, {}), "unfiltered file");
    verify(SearchController::matchesFile(file, db, {3, 1, 1}), "document/date/size conjunction");
    verify(!SearchController::matchesFile(file, db, {2, 0, 0}), "text excluded from images");
    for (const auto &[type, mime] : QList<QPair<int, QString>>{
             {2, "image/png"}, {3, "application/pdf"}, {4, "audio/mpeg"},
             {5, "video/mp4"}, {6, "application/zip"}}) {
        file.mimeType = mime;
        verify(SearchController::matchesFile(file, db, {type, 0, 0}), "MIME category accepted");
    }
    file.isDir = true;
    verify(SearchController::matchesFile(file, db, {1, 0, 0}), "folder filter");
    verify(!SearchController::matchesFile(file, db, {0, 0, 1}), "size filter excludes folders");
    file.isDir = false;
    file.size = 1024 * 1024;
    verify(!SearchController::matchesFile(file, db, {0, 0, 1}), "small size upper boundary excluded");
    verify(SearchController::matchesFile(file, db, {0, 0, 2}), "medium size lower boundary included");
    file.size = 100LL * 1024 * 1024;
    verify(!SearchController::matchesFile(file, db, {0, 0, 2}), "medium upper boundary excluded");
    verify(SearchController::matchesFile(file, db, {0, 0, 3}), "large lower boundary included");
    file.size = 1024LL * 1024 * 1024;
    verify(!SearchController::matchesFile(file, db, {0, 0, 3}), "large upper boundary excluded");
    verify(SearchController::matchesFile(file, db, {0, 0, 4}), "huge lower boundary included");
    file.size = -1;
    verify(!SearchController::matchesFile(file, db, {0, 0, 4}), "unknown size excluded");
    file.modificationTime -= 3 * 24 * 60 * 60;
    verify(!SearchController::matchesFile(file, db, {0, 1, 0}), "old file excluded from last day");
    verify(SearchController::matchesFile(file, db, {0, 2, 0}), "three days fits last week");
    file.modificationTime = 0;
    verify(!SearchController::matchesFile(file, db, {0, 4, 0}), "unknown date excluded");
}

static void testSearchUiContract(const QUrl &rootA, const QUrl &rootB)
{
    QLineEdit edit;
    QAction stopAction(nullptr), scopeAction(nullptr);
    QToolButton filterButton;
    QActionGroup scopeGroup(nullptr), typeGroup(nullptr), dateGroup(nullptr), sizeGroup(nullptr);
    const auto addChoice = [](QActionGroup &group, int value) {
        auto *action = new QAction(&group);
        action->setCheckable(true);
        action->setData(value);
        group.addAction(action);
    };
    for (int value = 0; value <= 2; ++value) addChoice(scopeGroup, value);
    for (int value = 0; value <= 6; ++value) addChoice(typeGroup, value);
    for (int value = 0; value <= 4; ++value) {
        addChoice(dateGroup, value);
        addChoice(sizeGroup, value);
    }
    QFrame progressFrame;
    SearchProgressBar progressBar;
    QLabel status;
    QToolButton scopeButton;
    QAction filterIndicatorAction(nullptr);
    QAction typeInfoAction(nullptr);
    QAction dateInfoAction(nullptr);
    QAction sizeInfoAction(nullptr);
    SearchController backend;
    SearchUiController ui(
        {&edit, &stopAction, &scopeAction, &scopeButton, &filterButton, &filterIndicatorAction,
         &scopeGroup, &typeGroup, &typeInfoAction, &dateGroup, &dateInfoAction, &sizeGroup, &sizeInfoAction,
         &progressFrame, &progressBar, &status},
        {[](const char *, const char *en) { return QString::fromUtf8(en); },
         [](const QUrl &url) { return url.fileName(); }, {}, [] { return false; }});

    PaneSearchState state;
    state.loadLocation(rootA);
    state.text = QStringLiteral(" needle ");
    state.scope = 1;
    state.type = 3;
    state.date = 2;
    state.size = 1;
    ui.updateControls({rootA, &state, &backend});
    verify(edit.text() == QStringLiteral(" needle ")
               && scopeGroup.checkedAction()->data().toInt() == 1,
           "Search UI state maps to edit and scope controls");
    verify(scopeButton.text() == QStringLiteral("Current drive") && scopeButton.isEnabled(),
           "Search UI scope button reflects active scope label and state");
    verify(typeGroup.checkedAction()->data().toInt() == 3
               && dateGroup.checkedAction()->data().toInt() == 2
               && sizeGroup.checkedAction()->data().toInt() == 1,
           "Search UI filters map to action groups");
    verify(!typeInfoAction.isVisible(), "typeInfoAction is hidden when single category / type preset is active");
    verify(filterButton.toolTip() == QStringLiteral("Search filters (3 active)")
               && filterButton.text() == QStringLiteral("Filters (3)")
               && filterButton.property("activeFilters").toBool(),
           "Search UI active-filter presentation and badge count");
    verify(filterIndicatorAction.isVisible()
               && filterIndicatorAction.toolTip() == QStringLiteral("Search filters (3 active)\nClick to open panel."),
           "Search UI filter indicator action is visible and shows count tooltip");

    // Multiple categories test in SearchUiController
    state.type = 0;
    state.categories = {SearchQuery::Category::Images, SearchQuery::Category::Archives};
    ui.updateControls({rootA, &state, &backend});
    verify(typeGroup.checkedAction() == nullptr, "No type action checked when multiple categories active (not 'All')");
    verify(typeInfoAction.isVisible() && typeInfoAction.text() == QStringLiteral("Multiple categories (2)"),
           "typeInfoAction shows 'Multiple categories (2)' and is visible");

    // Single category in state.categories: maps to preset type
    state.type = 2;
    state.categories = {SearchQuery::Category::Images};
    ui.updateControls({rootA, &state, &backend});
    verify(typeGroup.checkedAction() && typeGroup.checkedAction()->data().toInt() == 2,
           "Single category in state maps to type preset");
    verify(!typeInfoAction.isVisible(), "typeInfoAction is hidden for single category");

    // No categories, only MIME: typeGroup has 'All' checked (type 0)
    state.type = 0;
    state.categories.clear();
    state.mimeTypes = {QStringLiteral("image/png")};
    ui.updateControls({rootA, &state, &backend});
    verify(typeGroup.checkedAction() && typeGroup.checkedAction()->data().toInt() == 0,
           "MIME only with no categories checks 'All' in typeGroup");
    verify(!typeInfoAction.isVisible(), "typeInfoAction is hidden when categories is empty");

    // Restore state filters for requestFromUi test
    state.type = 3;
    state.categories.clear();
    state.mimeTypes.clear();

    edit.setText(QStringLiteral("  needle  "));
    const auto request = ui.requestFromUi({rootA, &state, &backend},
        {QUrl::fromLocalFile(QStringLiteral("/")), rootA}, rootB);
    verify(request.query == QStringLiteral("needle") && request.scope == 1
               && sameLocation(request.base, rootA),
           "UI controls map to explicit search request");
    const QUrl requestUrl = request.location(state);
    verify(searchQueryFromUrl(requestUrl) == QStringLiteral("needle")
               && searchIntParameter(requestUrl, QStringLiteral("type"), 0) == 3
               && searchIntParameter(requestUrl, QStringLiteral("date"), 0) == 2
               && searchIntParameter(requestUrl, QStringLiteral("size"), 0) == 1,
           "search request preserves URL filter contract");

    const QList<QUrl> roots = SearchUiController::wholeComputerSearchRoots(
        {rootA, normalizedUrl(rootA), rootB});
    verify(roots.size() == 2 && sameLocation(roots.at(0), rootA)
               && sameLocation(roots.at(1), rootB),
           "whole-computer roots are normalized and deduplicated");
    verify(sameLocation(SearchUiController::bestDriveRootForUrl(
                            QUrl::fromLocalFile(rootA.toLocalFile() + QStringLiteral("/nested")),
                            {QUrl::fromLocalFile(QStringLiteral("/")), rootA}), rootA),
           "current-drive scope selects deepest containing root");
    verify(sameLocation(SearchUiController::searchContextUrl(kThisPcUrl, rootB), rootB),
           "This PC search context falls back to home");

    const QUrl activeSearch = makeSearchLocation(QStringLiteral("needle"), 0, rootA, 0, 0, 0);
    state.type = 2;
    state.date = 1;
    state.size = 4;
    const QUrl synced = ui.locationWithSyncedFilters({activeSearch, &state, &backend});
    verify(searchBaseFromUrl(synced) == rootA
               && searchIntParameter(synced, QStringLiteral("type"), 0) == 2
               && searchIntParameter(synced, QStringLiteral("date"), 0) == 1
               && searchIntParameter(synced, QStringLiteral("size"), 0) == 4,
           "filter synchronization preserves search identity and updates filters");
    verify(ui.rootsForLocation(activeSearch, state, {rootA, rootB}, rootB)
               == QList<QUrl>{rootA},
           "folder search URL maps to its encoded root");

    ui.updateProgress(backend);
    ui.updateStatus(backend, 7, 0);
    verify(progressBar.value() == backend.progressPercent(),
           "progress widget mirrors SearchController");
    verify(status.text() == backend.statusText(7, 0),
           "status widget mirrors SearchController text");

    PaneSearchState other;
    other.loadLocation(rootB);
    other.text = QStringLiteral("other pane");
    ui.updateControls({rootB, &other, &backend});
    verify(edit.text() == QStringLiteral("other pane") && other.type == 0,
           "active-pane change restores independent Search UI state");
}

static void testSearchPopover()
{
    QWidget parent;
    parent.show();
    QLineEdit edit(&parent);
    SearchPopover popover(&parent);
    verify(popover.scopeButton() != nullptr && popover.filterButton() != nullptr,
           "Search popover contains scope and filter buttons");

    popover.attachAnchor(&edit);
    popover.updateStableWidths();

    const int scopeWidth = popover.scopeButton()->width();
    verify(scopeWidth > 0, "Search popover calculates positive stable scope width");
    verify(popover.stableWidth() > scopeWidth, "Search popover stable width encompasses buttons and margins");
    verify(edit.width() >= 300 && edit.width() <= 340 && popover.stableWidth() > edit.width(),
           "Variant A: Search edit has compact width (300-340 px) while popover stable width is wider");

    // Changing scope label does not change button width (stable width property)
    popover.scopeButton()->setText(QStringLiteral("Current drive"));
    verify(popover.scopeButton()->width() == scopeWidth, "Scope button width remains constant after label change");
    popover.scopeButton()->setText(QStringLiteral("This PC"));
    verify(popover.scopeButton()->width() == scopeWidth, "Scope button width remains constant for short label");

    // Height parity and geometry
    verify(popover.scopeButton()->height() == popover.filterButton()->height(),
           "Both popover buttons have identical height");
    verify(popover.scopeButton()->height() >= 28,
           "Popover buttons have comfortable height");
    verify(popover.height() > popover.scopeButton()->height(),
           "Popover height accommodates button height plus padding");

    // Visibility
    verify(!popover.isVisible(), "Search popover is initially hidden");
    popover.showUnder(&edit);
    verify(popover.isVisible(), "Search popover opens under search edit");
    popover.hidePopover();
    verify(!popover.isVisible(), "Search popover hides on hidePopover");

    // Toolbar height invariant
    QToolBar toolbar(&parent);
    auto *toolbarEdit = new QLineEdit(&toolbar);
    toolbar.addWidget(toolbarEdit);
    const int heightBefore = toolbar.sizeHint().height();
    popover.showUnder(toolbarEdit);
    const int heightAfter = toolbar.sizeHint().height();
    verify(heightBefore == heightAfter, "Search popover overlay does not change toolbar height");
    popover.hidePopover();
}

static void testAdvancedSearchFiltersDialog()
{
    // 1. Initial state from SearchQuery
    SearchQuery q;
    q.setText(QStringLiteral("report"));
    q.setNameMode(SearchQuery::NameMode::Glob);
    q.setMatchCase(true);
    q.setExtensions({QStringLiteral("tar.gz"), QStringLiteral("pdf")});
    q.setTargetType(SearchQuery::TargetType::FilesOnly);
    q.setCategories({SearchQuery::Category::Images, SearchQuery::Category::Documents});
    q.setMimeTypes({QStringLiteral("image/png"), QStringLiteral("application/pdf")});

    AdvancedSearchFiltersDialog dlg(q);
    verify(dlg.nameMode() == SearchQuery::NameMode::Glob, "Dialog preserves glob name mode");
    verify(dlg.matchCase() == true, "Dialog preserves matchCase true");
    verify(dlg.extensions() == QStringList({QStringLiteral("tar.gz"), QStringLiteral("pdf")}),
           "Dialog preserves extension list");
    verify(dlg.targetType() == SearchQuery::TargetType::FilesOnly, "Dialog preserves FilesOnly target");
    verify(dlg.categories() == QList<SearchQuery::Category>({SearchQuery::Category::Images, SearchQuery::Category::Documents}),
           "Dialog preserves selected categories");
    verify(dlg.mimeTypes() == QStringList({QStringLiteral("image/png"), QStringLiteral("application/pdf")}),
           "Dialog preserves MIME types");

    SearchQuery configured = dlg.configuredQuery();
    verify(configured.text() == QStringLiteral("report"), "Configured query retains query text");
    verify(configured.nameMode() == SearchQuery::NameMode::Glob, "Configured query retains glob mode");
    verify(configured.matchCase() == true, "Configured query retains matchCase");
    verify(configured.extensions() == QStringList({QStringLiteral("tar.gz"), QStringLiteral("pdf")}),
           "Configured query retains extensions");
    verify(configured.targetType() == SearchQuery::TargetType::FilesOnly, "Configured query retains targetType");
    verify(configured.categories() == QList<SearchQuery::Category>({SearchQuery::Category::Images, SearchQuery::Category::Documents}),
           "Configured query retains categories");
    verify(configured.mimeTypes() == QStringList({QStringLiteral("image/png"), QStringLiteral("application/pdf")}),
           "Configured query retains mimeTypes");

    // 2. Reset / Clear
    QMetaObject::invokeMethod(&dlg, "resetToDefaults");
    verify(dlg.nameMode() == SearchQuery::NameMode::Auto, "Reset restores auto name mode");
    verify(dlg.matchCase() == false, "Reset restores matchCase false");
    verify(dlg.extensions().isEmpty(), "Reset clears extensions");
    verify(dlg.targetType() == SearchQuery::TargetType::All, "Reset restores targetType All");
    verify(dlg.categories().isEmpty(), "Reset clears categories");
    verify(dlg.mimeTypes().isEmpty(), "Reset clears MIME types");

    SearchQuery resetQuery = dlg.configuredQuery();
    verify(resetQuery.nameMode() == SearchQuery::NameMode::Auto, "Reset query is auto");
    verify(resetQuery.matchCase() == false, "Reset query is case-insensitive");
    verify(resetQuery.extensions().isEmpty(), "Reset query has no extensions");
    verify(resetQuery.targetType() == SearchQuery::TargetType::All, "Reset query target is All");
    verify(resetQuery.categories().isEmpty(), "Reset query has no categories");
    verify(resetQuery.mimeTypes().isEmpty(), "Reset query has no mimeTypes");

    // 3. PaneSearchState integration and activeFilterCount
    PaneSearchState state;
    state.text = QStringLiteral("docs");
    state.scope = 0;
    state.nameMode = SearchQuery::NameMode::Glob;
    state.matchCase = true;
    state.extensions = {QStringLiteral("txt"), QStringLiteral("md")};
    state.targetType = SearchQuery::TargetType::FilesOnly;
    state.categories = {SearchQuery::Category::Documents};
    state.mimeTypes = {QStringLiteral("text/plain")};

    SearchQuery fromState = state.toQuery(QUrl::fromLocalFile(QStringLiteral("/home/user")));
    // activeFilterCount: targetType (1) + ext (1) + category (1) + mime (1) = 4
    verify(fromState.activeFilterCount() == 4, "State with targetType + ext + cat + mime produces 4 active filter criteria");
    verify(fromState.matchCase(), "State preserves matchCase attribute");
    verify(fromState.nameMode() == SearchQuery::NameMode::Glob, "State preserves glob nameMode attribute");
    verify(fromState.usesAdvancedFeatures(), "State uses advanced features");

    // Serialization roundtrip
    QUrl url = fromState.toUrl();
    PaneSearchState restored;
    restored.loadLocation(url);
    verify(restored.text == QStringLiteral("docs"), "Restored text matches");
    verify(restored.nameMode == SearchQuery::NameMode::Glob, "Restored nameMode matches");
    verify(restored.matchCase == true, "Restored matchCase matches");
    verify(restored.extensions == QStringList({QStringLiteral("txt"), QStringLiteral("md")}), "Restored extensions match");
    verify(restored.targetType == SearchQuery::TargetType::FilesOnly, "Restored targetType matches");
    verify(restored.categories == QList<SearchQuery::Category>({SearchQuery::Category::Documents}), "Restored categories match");
    verify(restored.mimeTypes == QStringList({QStringLiteral("text/plain")}), "Restored mimeTypes match");

    // 4. Non-modal dialog properties
    QWidget parentWidget;
    parentWidget.show();
    AdvancedSearchFiltersDialog nonModalDlg(q, &parentWidget);
    verify(!nonModalDlg.isModal(), "AdvancedSearchFiltersDialog is not modal");
    verify(nonModalDlg.windowModality() == Qt::NonModal, "AdvancedSearchFiltersDialog has Qt::NonModal windowModality");
    verify((nonModalDlg.windowFlags() & Qt::WindowStaysOnTopHint) == 0,
           "AdvancedSearchFiltersDialog does not set WindowStaysOnTopHint");

    nonModalDlg.show();
    verify(nonModalDlg.isVisible(), "Dialog is visible when shown");
    verify(parentWidget.isEnabled(), "Parent window remains enabled while dialog is open");

    // 5. Cancel without changes (signal not emitted on reject)
    QSignalSpy appliedSpy(&nonModalDlg, &AdvancedSearchFiltersDialog::filtersApplied);
    nonModalDlg.reject();
    verify(!nonModalDlg.isVisible(), "Dialog closes on reject");
    verify(appliedSpy.isEmpty(), "No filtersApplied signal emitted on reject / cancel");

    // 6. Apply emits filtersApplied with configured query
    nonModalDlg.show();
    QMetaObject::invokeMethod(&nonModalDlg, "onApply");
    verify(appliedSpy.count() == 1, "filtersApplied emitted exactly once on onApply");
    const SearchQuery appliedQuery = appliedSpy.takeFirst().at(0).value<SearchQuery>();
    verify(appliedQuery.nameMode() == SearchQuery::NameMode::Glob, "Applied query preserves glob mode");
    verify(appliedQuery.matchCase() == true, "Applied query preserves matchCase");
    verify(appliedQuery.targetType() == SearchQuery::TargetType::FilesOnly, "Applied query preserves targetType");
    verify(appliedQuery.categories() == QList<SearchQuery::Category>({SearchQuery::Category::Images, SearchQuery::Category::Documents}),
           "Applied query preserves categories");
    verify(appliedQuery.mimeTypes() == QStringList({QStringLiteral("image/png"), QStringLiteral("application/pdf")}),
           "Applied query preserves mimeTypes");

    // 7. Reopening / setQuery updates controls safely without recreating or leaking
    SearchQuery updatedQ;
    updatedQ.setNameMode(SearchQuery::NameMode::Substring);
    updatedQ.setMatchCase(false);
    updatedQ.setExtensions({QStringLiteral("zip")});
    updatedQ.setTargetType(SearchQuery::TargetType::FoldersOnly);
    updatedQ.setCategories({SearchQuery::Category::Archives});
    updatedQ.setMimeTypes({QStringLiteral("application/zip")});

    nonModalDlg.setQuery(updatedQ);
    verify(nonModalDlg.nameMode() == SearchQuery::NameMode::Substring, "setQuery updates nameMode");
    verify(nonModalDlg.matchCase() == false, "setQuery updates matchCase");
    verify(nonModalDlg.extensions() == QStringList({QStringLiteral("zip")}), "setQuery updates extensions");
    verify(nonModalDlg.targetType() == SearchQuery::TargetType::FoldersOnly, "setQuery updates targetType");
    verify(nonModalDlg.categories() == QList<SearchQuery::Category>({SearchQuery::Category::Archives}),
           "setQuery updates categories");
    verify(nonModalDlg.mimeTypes() == QStringList({QStringLiteral("application/zip")}),
           "setQuery updates mimeTypes");

    // 8. Pane routing isolation: modifying one pane's state does not alter another
    PaneSearchState primaryState;
    primaryState.loadLocation(QUrl::fromLocalFile(QStringLiteral("/home/user")));
    PaneSearchState splitState;
    splitState.loadLocation(QUrl::fromLocalFile(QStringLiteral("/home/user/Downloads")));

    // Apply filters to primaryState only
    primaryState.nameMode = appliedQuery.nameMode();
    primaryState.matchCase = appliedQuery.matchCase();
    primaryState.extensions = appliedQuery.extensions();
    primaryState.targetType = appliedQuery.targetType();
    primaryState.categories = appliedQuery.categories();
    primaryState.mimeTypes = appliedQuery.mimeTypes();

    verify(splitState.nameMode == SearchQuery::NameMode::Auto, "Split state nameMode untouched");
    verify(splitState.matchCase == false, "Split state matchCase untouched");
    verify(splitState.extensions.isEmpty(), "Split state extensions untouched");
    verify(splitState.targetType == SearchQuery::TargetType::All, "Split state targetType untouched");
    verify(splitState.categories.isEmpty(), "Split state categories untouched");
    verify(splitState.mimeTypes.isEmpty(), "Split state mimeTypes untouched");

    // 9. Quick menu replacement: choosing a single category or "All" replaces categories
    // while strictly preserving mimeTypes, extensions, targetType, date, size
    primaryState.type = 2; // Images
    primaryState.categories = {SearchQuery::Category::Images}; // Replaced by quick menu selection
    verify(primaryState.type == 2, "Quick menu sets type preset");
    verify(primaryState.categories == QList<SearchQuery::Category>({SearchQuery::Category::Images}),
           "Quick menu sets matching single category");
    verify(primaryState.mimeTypes == appliedQuery.mimeTypes(), "MIME types preserved on quick menu change");
    verify(primaryState.extensions == appliedQuery.extensions(), "Extensions preserved on quick menu change");
    verify(primaryState.targetType == appliedQuery.targetType(), "Target type preserved on quick menu change");

    // 10. Quick menu preset -> Advanced dialog synchronization (Test 3B.2.9)
    // When user picked Documents (3) in quick menu, opening dialog reflects Documents (3) checked
    primaryState.type = 3; // Documents
    primaryState.categories = {SearchQuery::Category::Documents};
    SearchQuery docQuery = primaryState.toQuery();
    verify(docQuery.categories() == QList<SearchQuery::Category>({SearchQuery::Category::Documents}),
           "PaneSearchState::toQuery maps Documents preset to categories");
    verify(docQuery.activeFilterCount() == 4, // targetType (files), ext, mimes, category
           "activeFilterCount counts single category exactly once without duplicate legacyType");
    nonModalDlg.setQuery(docQuery);
    verify(nonModalDlg.categories() == QList<SearchQuery::Category>({SearchQuery::Category::Documents}),
           "Advanced dialog reflects Documents checked from quick menu selection");

    // Clicking "Zastosuj" without changing anything preserves Documents and all other filters
    SearchQuery queryFromDlg = nonModalDlg.configuredQuery();
    verify(queryFromDlg.categories() == QList<SearchQuery::Category>({SearchQuery::Category::Documents}),
           "Dialog configuredQuery keeps Documents intact");
    verify(queryFromDlg.mimeTypes() == appliedQuery.mimeTypes(), "Dialog configuredQuery keeps MIME types intact");
    verify(queryFromDlg.extensions() == appliedQuery.extensions(), "Dialog configuredQuery keeps extensions intact");
    verify(queryFromDlg.targetType() == appliedQuery.targetType(), "Dialog configuredQuery keeps targetType intact");
    verify(queryFromDlg.activeFilterCount() == 4, "Dialog configuredQuery keeps activeFilterCount at 4");

    // 11. Choosing "All" (type 0)
    primaryState.type = 0;
    primaryState.categories.clear();
    verify(primaryState.type == 0, "Quick menu All sets type 0");
    verify(primaryState.categories.isEmpty(), "Quick menu All has empty categories");
    verify(!primaryState.mimeTypes.isEmpty(), "MIME types still preserved when quick menu selects All");
    SearchQuery allQuery = primaryState.toQuery();
    nonModalDlg.setQuery(allQuery);
    verify(nonModalDlg.categories().isEmpty(), "Advanced dialog has no categories checked when All is chosen");
}

int main(int argc, char **argv)
{
    if (qgetenv("THISPC_SEARCH_SCALE_CHECK") == "1") {
        QApplication app(argc, argv);
        QWidget host;
        host.resize(1040, 680);
        host.show();
        QLineEdit edit(&host);
        edit.move(680, 12);
        SearchPopover pop(&host);
        pop.scopeButton()->setText(QStringLiteral("Bieżący folder i podfoldery"));
        pop.nameModeButton()->setText(QStringLiteral("Tryb: Maska glob"));
        pop.filterButton()->setText(QStringLiteral("Filtry (2)"));
        pop.attachAnchor(&edit);
        pop.showUnder(&edit);
        QApplication::processEvents();
        const bool ok = pop.stableWidth() > 0
            && pop.buttonHeight() >= 32
            && pop.scopeButton()->height() == pop.nameModeButton()->height()
            && pop.nameModeButton()->height() == pop.filterButton()->height()
            && pop.scopeButton()->y() == pop.nameModeButton()->y()
            && pop.nameModeButton()->y() == pop.filterButton()->y()
            && pop.scopeButton()->geometry().right() < pop.nameModeButton()->geometry().left()
            && pop.nameModeButton()->geometry().right() < pop.filterButton()->geometry().left()
            && pop.x() >= 8
            && pop.x() + pop.width() <= host.width() - 8
            && edit.width() == SearchPopover::kDefaultAnchorWidth;
        return ok ? 0 : 1;
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-search-test");
    QCoreApplication::setApplicationName("search-test");
    QTemporaryDir files;
    verify(files.isValid(), "temporary search directory");
    const QString a = files.filePath("A"), b = files.filePath("B");
    for (const QString &folder : {a + "/nested", a + "/needle-folder", b})
        verify(QDir().mkpath(folder), "temporary search folders");
    for (const QString &name : {a + "/needle.txt", a + "/nested/needle-deep.txt", a + "/other.txt",
                                a + "/.needle-hidden.txt", b + "/needle-second.txt"}) {
        QFile file(name);
        verify(file.open(QIODevice::WriteOnly), "temporary search file");
        file.write("search test\n");
    }
    const QUrl rootA = QUrl::fromLocalFile(a), rootB = QUrl::fromLocalFile(b);
    testSearchUiContract(rootA, rootB);
    testSearchPopover();
    testAdvancedSearchFiltersDialog();
    const QSet<QString> expectedA{a + "/needle.txt", a + "/nested/needle-deep.txt", a + "/needle-folder"};
    SearchController search;
    QSignalSpy finished(&search, &SearchController::finished);
    QSignalSpy rendered(&search, &SearchController::resultsChanged);
    search.start("needle", {rootA, QUrl::fromLocalFile(a + "/nested")}, false);
    verify(search.isRunning() && search.totalRoots() == 2, "asynchronous multi-root search starts");
    waitForSearch(search);
    verify(search.errorCount() == 0 && search.completedRoots() == 2, "both workers succeed");
    verify(paths(search.files()) == expectedA && search.files().size() == 3, "recursive results deduplicated across overlapping roots");
    verify(search.progressPercent() == 100 && finished.count() == 1 && rendered.count() >= 1, "completion and final render signals");
    search.start("needle", {rootB}, false);
    waitForSearch(search);
    verify(paths(search.files()) == QSet<QString>{b + "/needle-second.txt"}, "new search clears old results");
    search.start("missing-query", {rootA}, false);
    waitForSearch(search);
    verify(search.files().isEmpty() && search.errorCount() == 0, "empty result succeeds");

    // Deterministically deliver late worker signals before deferred deletion.
    search.start("needle", {rootA}, false);
    QPointer<KIO::ListJob> old = search.m_searchJobs.first();
    old->entries(old, {entry(a + "/needle.txt"), entry(a + "/needle.txt"), entry(a + "/.needle-hidden.txt")});
    verify(search.files().size() == 1, "batch deduplicates and hides dot files");
    verify(search.m_searchRenderTimer.isActive(), "partial results schedule a batched render");
    const int finishedBeforeCancel = finished.count();
    verify(search.cancel() && !search.isRunning(), "cancel stops active search");
    verify(paths(search.files()) == QSet<QString>{a + "/needle.txt"}, "cancel preserves partial results");
    verify(!search.m_searchRenderTimer.isActive(), "cancel stops pending render timer");
    verify(old != nullptr, "canceled worker available until deferred deletion");
    old->entries(old, {entry(a + "/stale.txt")});
    verify(search.files().size() == 1, "late entries ignored after cancellation");
    search.start("needle", {rootB}, false);
    old->entries(old, {entry(a + "/stale.txt")});
    verify(search.files().isEmpty(), "late entries cannot contaminate replacement search");
    waitForSearch(search);
    verify(paths(search.files()) == QSet<QString>{b + "/needle-second.txt"}, "replacement search completes cleanly");
    verify(finished.count() == finishedBeforeCancel + 1, "canceled search never reports completion");

    search.start("needle", {rootA}, true);
    auto *hiddenJob = search.m_searchJobs.first().data();
    hiddenJob->entries(hiddenJob, {entry(a + "/.needle-hidden.txt")});
    verify(paths(search.files()).contains(a + "/.needle-hidden.txt"), "show-hidden accepts hidden worker entries");
    search.cancel();
    search.start("needle", {}, false);
    verify(!search.isRunning() && search.files().isEmpty(), "empty root set finishes without hanging");
    search.start("needle", {QUrl("unavailable-thispc-test:/")}, false);
    waitForSearch(search);
    // filenamesearch may swallow a listing error in its base location;
    // completion must still be delivered without hanging the search UI.
    verify(search.completedRoots() == 1 && search.files().isEmpty(), "unavailable root finishes with no results");

    // Stage 2 real KIO execution: empty search phrase with advanced filters
    // Setup dedicated fixture with archive.tar.gz, sub/nested.tar.gz, folder.tar.gz, doc.pdf, photo.jpg
    const QString stage2Dir = files.filePath("Stage2");
    verify(QDir().mkpath(stage2Dir + "/sub"), "stage2 sub directory");
    verify(QDir().mkpath(stage2Dir + "/folder.tar.gz"), "stage2 folder.tar.gz directory");
    for (const QString &fn : {stage2Dir + "/archive.tar.gz", stage2Dir + "/sub/nested.tar.gz",
                             stage2Dir + "/doc.pdf", stage2Dir + "/photo.jpg"}) {
        QFile f(fn);
        verify(f.open(QIODevice::WriteOnly), "stage2 test file");
        f.write("content 1234567890\n"); // 19 bytes
    }
    const QUrl stage2Root = QUrl::fromLocalFile(stage2Dir);

    // 1. search=&ext=tar.gz&target=files (recursive): exactly 2 files, no folder
    const QUrl qTarFiles = QUrl(QStringLiteral("thispcsearch:/?search=&ext=tar.gz&target=files&scope=0&base=")
                               + stage2Root.toString());
    verify(search.startFromLocation(qTarFiles, {stage2Root}, false), "start qTarFiles search");
    waitForSearch(search);
    const QSet<QString> tarPaths = paths(search.files());
    verify(tarPaths.size() == 2
               && tarPaths.contains(stage2Dir + "/archive.tar.gz")
               && tarPaths.contains(stage2Dir + "/sub/nested.tar.gz"),
           "qTarFiles returns exactly archive.tar.gz and nested.tar.gz, excluding folder.tar.gz");

    // 2. Empty phrase + non-recursive (recursive=0): only archive.tar.gz direct child
    const QUrl qNonRec = QUrl(QStringLiteral("thispcsearch:/?search=&ext=tar.gz&target=files&recursive=0&scope=0&base=")
                              + stage2Root.toString());
    verify(search.startFromLocation(qNonRec, {stage2Root}, false), "start qNonRec search");
    waitForSearch(search);
    verify(search.errorCount() == 0, "qNonRec search succeeds");
    verify(paths(search.files()) == QSet<QString>{stage2Dir + "/archive.tar.gz"},
           "non-recursive search excludes sub/nested.tar.gz");

    // 3. Empty phrase + MIME filter: doc.pdf
    const QUrl qMime = QUrl(QStringLiteral("thispcsearch:/?search=&mimes=application/pdf&scope=0&base=")
                            + stage2Root.toString());
    verify(search.startFromLocation(qMime, {stage2Root}, false), "start qMime search");
    waitForSearch(search);
    verify(search.errorCount() == 0, "qMime search succeeds");
    verify(paths(search.files()) == QSet<QString>{stage2Dir + "/doc.pdf"},
           "empty search phrase with mimes filter matches doc.pdf");

    // 4. Empty phrase + Size filter (sizemin=10B, sizemax=50B): files have 19 bytes, folders 0/excluded
    const QUrl qSize = QUrl(QStringLiteral("thispcsearch:/?search=&sizemin=10B&sizemax=50B&scope=0&base=")
                            + stage2Root.toString());
    verify(search.startFromLocation(qSize, {stage2Root}, false), "start qSize search");
    waitForSearch(search);
    verify(search.errorCount() == 0, "qSize search succeeds");
    verify(search.files().size() == 4, "all 4 files matched by size range, folders excluded");

    // 5. Empty phrase + Date filter (datemin from 1 hour ago)
    const qint64 anHourAgo = QDateTime::currentSecsSinceEpoch() - 3600;
    const QUrl qDate = QUrl(QStringLiteral("thispcsearch:/?search=&datemin=%1&scope=0&base=").arg(anHourAgo)
                            + stage2Root.toString());
    verify(search.startFromLocation(qDate, {stage2Root}, false), "start qDate search");
    waitForSearch(search);
    verify(search.errorCount() == 0, "qDate search succeeds");
    verify(!search.files().isEmpty(), "recent files matched by datemin filter");

    // 6. Invalid query rejection (e.g. invalid sizemin): rejected immediately with error
    const QUrl qBad = QUrl(QStringLiteral("thispcsearch:/?search=&sizemin=999invalid&scope=0&base=")
                           + stage2Root.toString());
    verify(!search.startFromLocation(qBad, {stage2Root}, false), "invalid query rejected");
    verify(search.errorCount() == 1 && !search.isRunning(), "rejected query reports errorCount 1 and not running");

    // 7. Glob matching with target=files (Test 2.9 regression): *.tar.gz returns archive.tar.gz and nested.tar.gz
    const QUrl qGlobTar = QUrl(QStringLiteral("thispcsearch:/?search=*.tar.gz&match=glob&base=")
                              + stage2Root.toString() + QStringLiteral("&scope=0&target=files"));
    verify(search.startFromLocation(qGlobTar, {stage2Root}, false), "start qGlobTar search");
    waitForSearch(search);
    verify(search.errorCount() == 0, "qGlobTar search reports no errors");
    const QSet<QString> globTarPaths = paths(search.files());
    verify(globTarPaths.size() == 2
               && globTarPaths.contains(stage2Dir + "/archive.tar.gz")
               && globTarPaths.contains(stage2Dir + "/sub/nested.tar.gz"),
           "qGlobTar returns exactly 2 files (archive.tar.gz, nested.tar.gz) and no directory");

    // 8. Glob with '?' single character matching: archive-?.txt
    for (const QString &fn : {stage2Dir + "/archive-1.txt", stage2Dir + "/archive-2.txt", stage2Dir + "/archive-12.txt"}) {
        QFile f(fn);
        verify(f.open(QIODevice::WriteOnly), "create archive-?.txt test file");
        f.write("test\n");
    }
    const QUrl qGlobQuest = QUrl(QStringLiteral("thispcsearch:/?search=archive-?.txt&match=glob&base=")
                                + stage2Root.toString() + QStringLiteral("&scope=0&target=files"));
    verify(search.startFromLocation(qGlobQuest, {stage2Root}, false), "start qGlobQuest search");
    waitForSearch(search);
    verify(search.errorCount() == 0, "qGlobQuest search succeeds without errors");
    const QSet<QString> globQuestPaths = paths(search.files());
    verify(globQuestPaths.size() == 2
               && globQuestPaths.contains(stage2Dir + "/archive-1.txt")
               && globQuestPaths.contains(stage2Dir + "/archive-2.txt"),
           "qGlobQuest matches archive-1.txt and archive-2.txt, excludes archive-12.txt");

    // 8b. URL-encoded '%3F' (?): exact equivalence with raw '?'
    const QUrl qGlobQuestEncoded = QUrl(QStringLiteral("thispcsearch:/?search=archive-%3F.txt&match=glob&base=")
                                       + stage2Root.toString() + QStringLiteral("&scope=0&target=files"));
    verify(searchQueryFromUrl(qGlobQuestEncoded) == QStringLiteral("archive-?.txt"),
           "searchQueryFromUrl decodes %3F to '?'");
    verify(search.startFromLocation(qGlobQuestEncoded, {stage2Root}, false), "start qGlobQuestEncoded search");
    waitForSearch(search);
    verify(search.errorCount() == 0, "qGlobQuestEncoded search succeeds without errors");
    verify(paths(search.files()) == globQuestPaths,
           "encoded %3F returns identical results to raw ? (archive-1.txt and archive-2.txt)");

    // 9. Glob case sensitivity: case=1 vs case=0
    const QString upperFn = stage2Dir + "/UPPERCASE.txt";
    QFile fUpper(upperFn);
    verify(fUpper.open(QIODevice::WriteOnly), "create UPPERCASE.txt");
    fUpper.write("upper\n");
    const QUrl qGlobCase0 = QUrl(QStringLiteral("thispcsearch:/?search=uppercase*.txt&match=glob&case=0&base=")
                                 + stage2Root.toString() + QStringLiteral("&scope=0"));
    verify(search.startFromLocation(qGlobCase0, {stage2Root}, false), "start qGlobCase0 search");
    waitForSearch(search);
    verify(paths(search.files()).contains(upperFn), "qGlobCase0 matches case-insensitively");
    const QUrl qGlobCase1 = QUrl(QStringLiteral("thispcsearch:/?search=uppercase*.txt&match=glob&case=1&base=")
                                 + stage2Root.toString() + QStringLiteral("&scope=0"));
    verify(search.startFromLocation(qGlobCase1, {stage2Root}, false), "start qGlobCase1 search");
    waitForSearch(search);
    verify(!paths(search.files()).contains(upperFn), "qGlobCase1 case-sensitive excludes UPPERCASE.txt");

    // 10. Substring query with regex special characters (e.g. 'report (draft)+v1.pdf')
    const QString specialFn = stage2Dir + "/report (draft)+v1.pdf";
    QFile fSpecial(specialFn);
    verify(fSpecial.open(QIODevice::WriteOnly), "create special chars file");
    fSpecial.write("special\n");
    fSpecial.close();
    const QUrl qSpecial = QUrl(QStringLiteral("thispcsearch:/?search=(draft)+v1&base=")
                               + stage2Root.toString() + QStringLiteral("&scope=0"));
    verify(search.startFromLocation(qSpecial, {stage2Root}, false), "start qSpecial search");
    waitForSearch(search);
    verify(search.errorCount() == 0, "qSpecial search has no PCRE errors");
    verify(paths(search.files()).contains(specialFn), "qSpecial matches literal special characters in filename");

    testFilters();

    ThisPcWindow window(rootA, false);
    window.show();
    window.m_refreshTimer.stop();
    window.m_driveHomeCoordinator.cancel();
    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(rootB);
    verify(QTest::qWaitFor([&] { return window.m_splitPane->listView()->count() == 1; }, 5000), "split pane loads test directory");
    const QUrl location = makeSearchLocation("needle", 0, rootA, 0, 0, 0);
    verify(isSearchLocation(location) && searchQueryFromUrl(location) == "needle" && searchBaseFromUrl(location) == rootA,
           "search URL preserves query and base");
    window.navigateTo(location, true);
    waitForSearch(*window.m_searchController);
    verify(window.m_directoryList->count() == 3 && window.m_directoryDetails->topLevelItemCount() == 3, "window displays search results in both views");
    verify(!window.m_searchProgressFrame->isVisible() && !window.m_stopSearchAction->isVisible(), "completion hides search controls");
    verify(window.m_splitPane->currentUrl() == rootB && window.m_splitPane->listView()->count() == 1, "search preserves split pane");
    for (int mode : {0, 1, 2, 3}) {
        window.setDirectoryViewMode(mode);
        window.setSortKey(2);
        window.setSortAscending(false);
        verify(window.m_directoryList->count() == 3, "view and sort changes keep search results");
    }
    window.m_primarySearch.type = 1;
    window.syncCurrentSearchFilters();
    window.renderDirectoryItems();
    verify(window.m_directoryList->count() == 1 && window.m_searchVisibleCount == 1, "changing type filter reuses current results");
    verify(searchIntParameter(window.m_navigation.currentUrl(), "type", 0) == 1, "filter stored in search history URL");
    window.navigateTo(makeSearchLocation("needle", 0, rootA, 0, 0, 0), true);
    auto *job = window.m_searchController->m_searchJobs.first().data();
    job->entries(job, {entry(a + "/needle.txt")});
    window.m_cancelSearchButton->click();
    verify(!window.m_searchController->isRunning() && window.m_directoryList->count() == 1, "Cancel button displays partial results");
    verify(!window.m_searchProgressFrame->isVisible(), "Cancel hides progress");
    verify(window.statusBar()->currentMessage() == "Search canceled", "Cancel status text");
    window.navigateTo(makeSearchLocation("needle", 0, rootA, 0, 0, 0), true);
    window.navigateTo(rootB, true);
    verify(!window.m_searchController->isRunning(), "navigation cancels current search");
    verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 1; }, 5000), "new directory loads after cancellation");
    verify(window.m_navigation.currentUrl() == rootB && paths(window.m_primaryPane->files()) == QSet<QString>{b + "/needle-second.txt"}, "late search results do not replace ordinary directory");
    const QString refusedRedoStatus =
        "Files or their inode identities changed; no Undo/Redo was performed. "
        "After safely restoring the expected files, you can try again.";
    window.statusBar()->showMessage(refusedRedoStatus, 12000);
    window.renderDirectoryItems(true);
    verify(window.statusBar()->currentMessage() == refusedRedoStatus,
           "renderDirectoryItems preserves an actionable Redo refusal when requested");
    window.m_undoController->m_refreshViews(true);
    verify(window.m_primaryPane->listingJob()
               && window.m_primaryPane->listingJob()->property("thispcPreserveStatusMessage").toBool(),
           "preserving refresh attaches the marker to the correct listing job");
    verify(QTest::qWaitFor([&] { return window.m_primaryPane->listingJob() == nullptr; }, 5000)
               && window.statusBar()->currentMessage() == refusedRedoStatus,
           "asynchronous directory refresh does not overwrite the Redo refusal");
    // Stage 4: shared Search controls follow the active pane, including drafts,
    // history URLs and workers that continue while the other pane has focus.
    using Pane = ThisPcWindow::PaneId;
    auto *split = window.m_splitPane;
    window.activateWindow();
    window.navigateTo(rootB, true);
    split->setCurrentUrl(rootA);
    verify(QTest::qWaitFor([&] { return split->listView()->count() == 4; }, 5000),
           "right ordinary directory loads before live filtering");
    window.setActivePane(Pane::Split);
    QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
    verify(window.m_searchEdit->hasFocus() && window.m_activePane == Pane::Split,
           "Ctrl+F focuses shared Search while preserving right pane");
    window.m_searchEdit->setText("needle");
    verify(split->listView()->count() == 2 && window.m_directoryList->count() == 1,
           "live filtering affects only the right active directory");
    window.setActivePane(Pane::Primary);
    verify(window.m_searchEdit->text().isEmpty()
               && window.m_searchEdit->placeholderText().contains("B"),
           "left pane restores its own search draft and placeholder");
    window.m_searchEdit->setText("second");
    window.setActivePane(Pane::Split);
    verify(window.m_searchEdit->text() == "needle" && window.m_searchScopeAction->isEnabled(),
           "returning right restores live query and folder scope");
    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
    const QUrl rightSearch = split->currentUrl();
    verify(isSearchLocation(rightSearch) && searchBaseFromUrl(rightSearch) == rootA
               && searchIntParameter(rightSearch, "scope", -1) == 0,
           "Enter routes recursive Search to the right folder");
    verify(window.m_navigation.currentUrl() == rootB && window.m_primarySearch.text == "second",
           "right Search preserves left location and draft");
    waitForSearch(*split->searchController());
    verify(paths(split->files()) == expectedA && split->listView()->count() == 3,
           "right Search receives real recursive KIO results");
    verify(!split->m_details->isColumnHidden(4)
               && split->m_details->topLevelItem(0)->text(4)
                    == parentLocationForDisplay(QUrl(split->m_details->topLevelItem(0)->data(0, Qt::UserRole).toString())),
           "right Search details include each result's parent location");
    verify(split->m_breadcrumbButton->text() == "Results for: needle"
               && split->m_thisPcCrumb->isVisible()
               && window.m_searchEdit->placeholderText() == "New search"
               && !window.m_searchScopeAction->isEnabled(),
           "right Search has friendly breadcrumbs and correct shared controls");
    for (int mode : {0, 1, 2, 3}) {
        for (auto *action : window.m_viewButton->menu()->actions())
            if (action->data().isValid() && action->data().toInt() == mode) action->trigger();
        for (auto *action : window.m_sortButton->menu()->actions())
            if (action->data().isValid() && action->data().toInt() == 2) action->trigger();
        verify(split->viewMode() == mode && split->listView()->count() == 3,
               "shared View and Sort preserve right Search results");
    }
    auto choose = [](QActionGroup *group, int value) {
        for (auto *action : group->actions())
            if (action->data().toInt() == value) action->trigger();
    };
    const int rightHistorySize = split->m_history.size();
    choose(window.m_searchTypeGroup, 1);
    const QUrl filteredSearch = split->currentUrl();
    verify(split->listView()->count() == 1
               && split->m_history.size() == rightHistorySize
               && split->m_history.at(split->m_historyIndex) == filteredSearch
               && searchIntParameter(filteredSearch, "type", 0) == 1,
           "right type filter reuses results and updates existing history entry");
    verify(window.m_tabs.at(window.m_activeTab).splitUrl == filteredSearch,
           "right filtered Search URL is saved in tab state");
    QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier);
    verify(split->m_addressEdit->text() == urlForDisplay(filteredSearch),
           "right Ctrl+L exposes current filtered Search URL");
    QTest::keyClick(split->m_addressEdit, Qt::Key_Escape);
    verify(split->m_locationStack->currentWidget() == split->m_breadcrumbFrame,
           "right Escape restores Search breadcrumb");
    window.m_upAction->trigger();
    verify(split->currentUrl() == rootA && window.m_navigation.currentUrl() == rootB
               && split->m_details->isColumnHidden(4),
           "right Search Up returns to base and hides result-location column");
    window.m_backAction->trigger();
    waitForSearch(*split->searchController());
    verify(split->currentUrl() == filteredSearch && split->listView()->count() == 1
               && window.m_searchTypeGroup->checkedAction()->data().toInt() == 1,
           "Back restores right Search filters and results");
    window.m_forwardAction->trigger();
    verify(split->currentUrl() == rootA && window.m_searchEdit->text().isEmpty(),
           "Forward restores right directory and clears Search draft");
    // Current drive must use the right location, with test-only drive roots.
    DriveInfo driveA; driveA.name = "A"; driveA.targetUrl = rootA;
    DriveInfo driveB; driveB.name = "B"; driveB.targetUrl = rootB;
    window.m_driveHomeCoordinator.setSnapshotForTesting({driveA, driveB});
    split->setCurrentUrl(QUrl::fromLocalFile(a + "/nested"));
    choose(window.m_searchScopeGroup, 1);
    window.m_searchEdit->setText("needle");
    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
    verify(searchBaseFromUrl(split->currentUrl()) == rootA
               && searchIntParameter(split->currentUrl(), "scope", -1) == 1,
           "current-drive Search resolves the active right drive");
    waitForSearch(*split->searchController());
    verify(split->listView()->count() == 3, "right drive Search covers the whole drive fixture");
    window.m_searchEdit->setText("deep");
    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
    verify(searchBaseFromUrl(split->currentUrl()) == rootA
               && searchQueryFromUrl(split->currentUrl()) == "deep",
           "new query within right Search retains its base and scope");
    waitForSearch(*split->searchController());
    verify(split->listView()->count() == 1, "replacement right query displays fresh results");
    window.m_refreshAction->trigger();
    verify(split->searchController()->isRunning() && window.m_navigation.currentUrl() == rootB,
           "shared Refresh restarts only the active right Search");
    waitForSearch(*split->searchController());

    // Two independent workers: late callbacks may never update the other pane.
    window.navigateTo(location, true);
    split->setCurrentUrl(makeSearchLocation("needle", 0, rootB, 0, 0, 0));
    auto *primaryWorker = window.m_searchController->m_searchJobs.first().data();
    QPointer<KIO::ListJob> rightWorker = split->searchController()->m_searchJobs.first();
    primaryWorker->entries(primaryWorker, {entry(a + "/needle.txt")});
    rightWorker->entries(rightWorker, {entry(b + "/needle-second.txt")});
    window.m_stopSearchAction->trigger();
    verify(!split->searchController()->isRunning() && window.m_searchController->isRunning()
               && split->listView()->count() == 1,
           "shared Stop cancels only right Search and preserves its partial results");
    verify(!window.m_stopSearchAction->isVisible() && !split->m_searchProgressFrame->isVisible(),
           "right cancellation hides only its progress and shared Stop");
    rightWorker->entries(rightWorker, {entry(b + "/stale.txt")});
    verify(split->searchController()->files().size() == 1,
           "late canceled right worker cannot contaminate results");
    window.setActivePane(Pane::Primary);
    verify(window.m_stopSearchAction->isVisible(), "switching left shows its still-running Stop action");
    window.m_stopSearchAction->trigger();
    verify(!window.m_searchController->isRunning() && window.m_directoryList->count() == 1,
           "shared Stop works symmetrically on left Search");
    window.setActivePane(Pane::Split);
    split->setCurrentUrl(rightSearch);
    window.navigateTo(location, true);
    window.m_cancelSearchButton->click();
    verify(!window.m_searchController->isRunning() && split->searchController()->isRunning()
               && window.m_stopSearchAction->isVisible(),
           "left inline Stop keeps right worker and shared Stop intact");
    split->m_cancelSearchButton->click();
    verify(!split->searchController()->isRunning(), "right inline Stop cancels its own worker");
    // Left completion while right runs must not hide the shared Stop action.
    split->setCurrentUrl(rightSearch);
    window.navigateTo(location, true);
    window.m_searchController->start("needle", {}, false);
    verify(split->searchController()->isRunning() && window.m_stopSearchAction->isVisible(),
           "inactive left completion cannot hide active right Stop");
    const QUrl savedRight = split->currentUrl();
    window.setSplitViewEnabled(false);
    verify(!split->searchController()->isRunning() && window.m_activePane == Pane::Primary,
           "closing Split View cancels the hidden right worker");
    window.setSplitViewEnabled(true);
    window.setActivePane(Pane::Split);
    verify(split->currentUrl() == savedRight && split->searchController()->isRunning(),
           "reopening Split View restores the right Search URL instead of its base folder");
    waitForSearch(*split->searchController());
    verify(split->listView()->count() == 3, "restored right Search reloads results");
    choose(window.m_searchTypeGroup, 3);
    choose(window.m_searchDateGroup, 1);
    choose(window.m_searchSizeGroup, 1);
    verify(split->listView()->count() == 2
               && searchIntParameter(split->currentUrl(), "date", 0) == 1
               && searchIntParameter(split->currentUrl(), "size", 0) == 1,
           "right type/date/size filters combine and persist in the address");
    window.m_searchFilterButton->menu()->actions().last()->trigger();
    verify(split->listView()->count() == 3 && split->searchState().type == 0
               && split->searchState().date == 0 && split->searchState().size == 0,
           "Clear filters resets only active right Search filters");
    const QUrl leftBeforeCrumb = window.m_navigation.currentUrl();
    QTest::mouseClick(split->m_thisPcCrumb, Qt::LeftButton);
    verify(split->currentUrl() == kThisPcUrl && window.m_navigation.currentUrl() == leftBeforeCrumb
               && window.m_searchEdit->placeholderText() == "Search this computer",
           "right Search This PC breadcrumb navigates right and updates shared Search state");
    // Restrict whole-computer Search to disposable roots in this test.
    window.m_driveHomeCoordinator.setSnapshotForTesting({driveA, driveB});
    window.m_searchEdit->setText("needle");
    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
    verify(searchIntParameter(split->currentUrl(), "scope", -1) == 2
               && !searchBaseFromUrl(split->currentUrl()).isValid()
               && split->searchController()->totalRoots() == 2,
           "right This PC Search uses whole-computer scope and shared drive inventory");
    waitForSearch(*split->searchController());
    verify(split->listView()->count() == 4, "right whole-computer Search includes both fixture roots");
    window.m_upAction->trigger();
    verify(split->currentUrl() == kThisPcUrl, "whole-computer Search Up returns to right This PC");

    // =========================================================================
    // Stage 3B.2 Advanced Filters Dialog Primary/Split Routing & Lifecycle Test
    // =========================================================================
    // Setup: Left pane has search for txt files in rootA, Right pane has search in rootB
    window.setActivePane(Pane::Primary);
    const QUrl leftSearchLoc = makeSearchLocation("needle", 0, rootA, 0, 0, 0);
    window.navigateTo(leftSearchLoc, true);
    waitForSearch(*window.m_searchController);

    window.setActivePane(Pane::Split);
    const QUrl rightSearchLoc = makeSearchLocation("needle", 0, rootB, 0, 0, 0);
    split->setCurrentUrl(rightSearchLoc);
    waitForSearch(*split->searchController());

    // 1. Case A: Primary active -> open dialog -> applies strictly to Primary
    window.setActivePane(Pane::Primary);
    window.showAdvancedFiltersDialog();
    verify(window.m_advancedFiltersDialog != nullptr, "Advanced filters dialog created");
    verify(window.m_advancedFiltersPane == PaneId::Primary, "Dialog opened with Primary target");
    SearchQuery leftFilterChange;
    leftFilterChange.setNameMode(SearchQuery::NameMode::Glob);
    leftFilterChange.setTargetType(SearchQuery::TargetType::FilesOnly);
    leftFilterChange.setExtensions({QStringLiteral("txt")});
    // Apply filters
    Q_EMIT window.m_advancedFiltersDialog->filtersApplied(leftFilterChange);
    verify(window.m_primarySearch.extensions == QStringList({QStringLiteral("txt")}),
           "Primary pane state received txt extension filter");
    verify(split->searchState().extensions.isEmpty(),
           "Case A: Split pane state strictly untouched when Primary applied");

    // 2. Case B: Split active -> reopen dialog -> applies strictly to Split (Bug 3B.2.11 repro)
    window.setActivePane(Pane::Split);
    window.showAdvancedFiltersDialog();
    verify(window.m_advancedFiltersPane == PaneId::Split,
           "Case B: Dialog correctly targets Split when opened while Split is active");
    SearchQuery rightFilterChange;
    rightFilterChange.setNameMode(SearchQuery::NameMode::Glob);
    rightFilterChange.setTargetType(SearchQuery::TargetType::FilesOnly);
    rightFilterChange.setCategories({SearchQuery::Category::Archives});
    rightFilterChange.setMimeTypes({QStringLiteral("application/zip")});
    // Apply filters to Split
    Q_EMIT window.m_advancedFiltersDialog->filtersApplied(rightFilterChange);
    verify(split->searchState().categories == QList<SearchQuery::Category>({SearchQuery::Category::Archives}),
           "Case B: Split pane received Archives category filter");
    verify(split->searchState().mimeTypes == QStringList({QStringLiteral("application/zip")}),
           "Case B: Split pane received application/zip MIME filter");
    verify(window.m_primarySearch.extensions == QStringList({QStringLiteral("txt")}),
           "Case B: Primary pane preserved its own extensions and did not get overwritten");
    verify(window.m_primarySearch.categories.isEmpty(),
           "Case B: Primary pane did not receive Split Archives category");

    // 3. Case C: Open for Split -> switch active pane to Primary -> apply -> goes to Split
    window.setActivePane(Pane::Split);
    window.showAdvancedFiltersDialog();
    verify(window.m_advancedFiltersPane == PaneId::Split, "Dialog opened for Split");
    // User clicks left pane (switches active pane to Primary while dialog is open)
    window.setActivePane(Pane::Primary);
    verify(window.m_activePane == PaneId::Primary, "Active pane switched to Primary");
    SearchQuery splitNewFilter;
    splitNewFilter.setNameMode(SearchQuery::NameMode::Glob);
    splitNewFilter.setExtensions({QStringLiteral("zip"), QStringLiteral("tar.gz")});
    Q_EMIT window.m_advancedFiltersDialog->filtersApplied(splitNewFilter);
    verify(split->searchState().extensions == QStringList({QStringLiteral("zip"), QStringLiteral("tar.gz")}),
           "Case C: Split received extensions even though user clicked Primary before Apply");
    verify(window.m_primarySearch.extensions == QStringList({QStringLiteral("txt")}),
           "Case C: Primary was NOT overwritten by late apply targeting Split");

    // 4. Case D (Reverse of C): Open for Primary -> switch to Split -> apply -> goes to Primary
    window.setActivePane(Pane::Primary);
    window.showAdvancedFiltersDialog();
    verify(window.m_advancedFiltersPane == PaneId::Primary, "Dialog opened for Primary");
    window.setActivePane(Pane::Split);
    verify(window.m_activePane == PaneId::Split, "Active pane switched to Split");
    SearchQuery primaryNewFilter;
    primaryNewFilter.setNameMode(SearchQuery::NameMode::Glob);
    primaryNewFilter.setExtensions({QStringLiteral("pdf")});
    Q_EMIT window.m_advancedFiltersDialog->filtersApplied(primaryNewFilter);
    verify(window.m_primarySearch.extensions == QStringList({QStringLiteral("pdf")}),
           "Case D: Primary received pdf filter even though user clicked Split before Apply");
    verify(split->searchState().extensions == QStringList({QStringLiteral("zip"), QStringLiteral("tar.gz")}),
           "Case D: Split was NOT overwritten by late apply targeting Primary");

    // 5. Safety: Closing Split View while dialog was targeting Split does not crash
    window.showAdvancedFiltersDialog(); // target is Split (since active was Split)
    verify(window.m_advancedFiltersPane == PaneId::Split, "Dialog targeting Split");
    window.setSplitViewEnabled(false);
    verify(!window.m_splitPane || !window.m_splitPane->isVisible(), "Split view disabled");
    // Apply when Split pane is invisible/closed is safely ignored
    Q_EMIT window.m_advancedFiltersDialog->filtersApplied(splitNewFilter);
    verify(window.m_primarySearch.extensions == QStringList({QStringLiteral("pdf")}),
           "Primary pane safe when Split is closed");

    // Restore Split view for cleanup
    window.setSplitViewEnabled(true);

    // =========================================================================
    // Stage 3B.3 Advanced Filters Dialog: Date & Size Controls, UI Validation,
    // Sync with Quick Menus
    // =========================================================================
    window.setActivePane(Pane::Primary);
    window.showAdvancedFiltersDialog();
    AdvancedSearchFiltersDialog *dialog = window.m_advancedFiltersDialog.data();
    verify(dialog != nullptr, "Advanced filters dialog instance exists");

    // Reset to defaults
    dialog->resetToDefaults();
    verify(!dialog->dateMin().has_value() && !dialog->dateMax().has_value(), "resetToDefaults clears date filters");
    verify(!dialog->sizeMin().has_value() && !dialog->sizeMax().has_value(), "resetToDefaults clears size filters");
    verify(dialog->configuredQuery().activeFilterCount() == 0, "resetToDefaults yields 0 active filters");

    // 1. Date Range validation: Start > End disables Apply button
    dialog->m_dateMinCheck->setChecked(true);
    dialog->m_dateMaxCheck->setChecked(true);
    dialog->m_dateMinEdit->setDate(QDate(2026, 10, 15));
    dialog->m_dateMaxEdit->setDate(QDate(2026, 10, 10)); // Start > End
    dialog->validateInputs();
    verify(!dialog->m_applyButton->isEnabled(), "Apply button disabled when dateMin > dateMax");
    verify(dialog->m_validationErrorLabel->isVisible(), "Validation error label visible for date error");

    // Fix date range: Start <= End enables Apply button
    dialog->m_dateMaxEdit->setDate(QDate(2026, 10, 20));
    dialog->validateInputs();
    verify(dialog->m_applyButton->isEnabled(), "Apply button enabled when dateMin <= dateMax");
    verify(!dialog->m_validationErrorLabel->isVisible(), "Validation error label hidden for valid dates");

    // 2. Size Range validation: Start > End disables Apply button
    dialog->m_sizeMinCheck->setChecked(true);
    dialog->m_sizeMaxCheck->setChecked(true);
    dialog->m_sizeMinSpin->setValue(10.0);
    dialog->m_sizeMinUnitCombo->setCurrentIndex(2); // 10 MiB
    dialog->m_sizeMaxSpin->setValue(5.0);
    dialog->m_sizeMaxUnitCombo->setCurrentIndex(2); // 5 MiB
    dialog->validateInputs();
    verify(!dialog->m_applyButton->isEnabled(), "Apply button disabled when sizeMin > sizeMax");
    verify(dialog->m_validationErrorLabel->isVisible(), "Validation error label visible for size error");

    // Unit awareness in validation: 1 GiB > 500 MiB even if number 1 < 500
    dialog->m_sizeMinSpin->setValue(1.0);
    dialog->m_sizeMinUnitCombo->setCurrentIndex(3); // 1 GiB = 1024 MiB
    dialog->m_sizeMaxSpin->setValue(500.0);
    dialog->m_sizeMaxUnitCombo->setCurrentIndex(2); // 500 MiB
    dialog->validateInputs();
    verify(!dialog->m_applyButton->isEnabled(), "Apply button disabled when 1 GiB > 500 MiB");

    // Fix size range: 100 KiB to 500 MiB
    dialog->m_sizeMinSpin->setValue(100.0);
    dialog->m_sizeMinUnitCombo->setCurrentIndex(1); // 100 KiB
    dialog->validateInputs();
    verify(dialog->m_applyButton->isEnabled(), "Apply button enabled when 100 KiB <= 500 MiB");

    // Verify getters
    verify(dialog->sizeMin().has_value() && *dialog->sizeMin() == 100LL * 1024LL, "sizeMin getter returns 102400 bytes");
    verify(dialog->sizeMax().has_value() && *dialog->sizeMax() == 500LL * 1024LL * 1024LL, "sizeMax getter returns 524288000 bytes");

    // 3. Applying custom date and size ranges updates PaneSearchState and syncs quick menus
    const SearchQuery appliedQuery = dialog->configuredQuery();
    Q_EMIT dialog->filtersApplied(appliedQuery);
    verify(window.m_primarySearch.dateMin.has_value() && window.m_primarySearch.dateMax.has_value(),
           "Primary search state received dateMin and dateMax");
    verify(window.m_primarySearch.sizeMin.has_value() && window.m_primarySearch.sizeMax.has_value(),
           "Primary search state received sizeMin and sizeMax");
    verify(window.m_primarySearch.date == 0, "Legacy date preset cleared when custom date is set");
    verify(window.m_primarySearch.size == 0, "Legacy size preset cleared when custom size is set");

    // Check quick menu informational action visibility
    window.updateSearchControls();
    verify(window.m_searchDateInfoAction != nullptr && window.m_searchDateInfoAction->isVisible(),
           "Date quick menu displays 'Niestandardowy zakres' informational action");
    verify(window.m_searchSizeInfoAction != nullptr && window.m_searchSizeInfoAction->isVisible(),
           "Size quick menu displays 'Niestandardowy zakres' informational action");

    // 4. Selecting a quick date preset resets custom date range but preserves custom size
    choose(window.m_searchDateGroup, 1); // Dzisiaj
    verify(window.m_primarySearch.date == 1, "Quick date preset set to 1");
    verify(!window.m_primarySearch.dateMin.has_value() && !window.m_primarySearch.dateMax.has_value(),
           "Custom date range reset when quick date preset chosen");
    verify(window.m_primarySearch.sizeMin.has_value() && window.m_primarySearch.sizeMax.has_value(),
           "Custom size range preserved when quick date preset chosen");

    // 5. Selecting a quick size preset resets custom size range
    choose(window.m_searchSizeGroup, 2); // Średni
    verify(window.m_primarySearch.size == 2, "Quick size preset set to 2");
    verify(!window.m_primarySearch.sizeMin.has_value() && !window.m_primarySearch.sizeMax.has_value(),
           "Custom size range reset when quick size preset chosen");

    // 7. Stage 3B.3.10: Quick Size Preset <-> Advanced Filters synchronization
    // Set quick size preset 1 ("Mniejszy niż 1 MiB")
    choose(window.m_searchSizeGroup, 1);
    verify(window.m_primarySearch.size == 1, "Quick size preset 1 set");
    verify(!window.m_primarySearch.sizeMin.has_value() && !window.m_primarySearch.sizeMax.has_value(),
           "No custom size bounds initially");

    // Reopen Advanced Filters dialog and verify preset 1 is reflected in preset combo
    window.showAdvancedFiltersDialog();
    verify(dialog->legacySize() == 1, "Dialog legacySize getter reflects preset 1");
    verify(dialog->m_sizePresetCombo != nullptr && dialog->m_sizePresetCombo->currentIndex() == 1,
           "Dialog size preset combo selected index 1 ('Mniejszy niż 1 MiB')");
    verify(!dialog->m_sizeMinCheck->isChecked() && !dialog->m_sizeMaxCheck->isChecked(),
           "Custom size checkboxes are unchecked when preset is active");

    // Applying dialog without changes MUST preserve legacySize = 1 and NOT set custom bounds
    SearchQuery preservedQuery = dialog->configuredQuery();
    verify(preservedQuery.legacySize() == 1, "Configured query retains legacySize = 1");
    verify(!preservedQuery.sizeMin().has_value() && !preservedQuery.sizeMax().has_value(),
           "Configured query has no custom bounds");
    Q_EMIT dialog->filtersApplied(preservedQuery);
    verify(window.m_primarySearch.size == 1, "Primary search retains size preset 1 after Apply without changes");
    verify(!window.m_primarySearch.sizeMin.has_value() && !window.m_primarySearch.sizeMax.has_value(),
           "Primary search keeps nullopt custom bounds after Apply without changes");

    // Verify exact SearchMatcher semantics: size < 1 MiB (strict inequality, not <= 1 MiB)
    QMimeDatabase db;
    const qint64 testModTime = QDateTime::currentSecsSinceEpoch();
    FileInfo fZero; fZero.name = "needle_zero.bin"; fZero.size = 0; fZero.modificationTime = testModTime;
    FileInfo f999; f999.name = "needle_start.bin"; f999.size = 1000; f999.modificationTime = testModTime;
    FileInfo fJustBelow; fJustBelow.name = "needle_end.bin"; fJustBelow.size = 1024LL * 1024LL - 1; fJustBelow.modificationTime = testModTime;
    FileInfo fExact1MiB; fExact1MiB.name = "needle_large.bin"; fExact1MiB.size = 1024LL * 1024LL; fExact1MiB.modificationTime = testModTime;
    FileInfo fAbove1MiB; fAbove1MiB.name = "needle_huge.bin"; fAbove1MiB.size = 1024LL * 1024LL + 1; fAbove1MiB.modificationTime = testModTime;
    SearchMatcher::Prepared matcher(preservedQuery);
    verify(matcher.matches(fZero, db), "0 B matches preset 1 (< 1 MiB)");
    verify(matcher.matches(f999, db), "1000 B matches preset 1 (< 1 MiB)");
    verify(matcher.matches(fJustBelow, db), "1048575 B matches preset 1 (< 1 MiB)");
    verify(!matcher.matches(fExact1MiB, db), "Exactly 1048576 B is EXCLUDED by preset 1 (< 1 MiB strict)");
    verify(!matcher.matches(fAbove1MiB, db), "1048577 B is EXCLUDED by preset 1 (< 1 MiB)");

    // Editing custom size in dialog overrides the preset
    dialog->m_sizeMinCheck->setChecked(true);
    dialog->m_sizeMinSpin->setValue(2.0);
    dialog->m_sizeMinUnitCombo->setCurrentIndex(2); // 2 MiB
    verify(dialog->m_sizePresetCombo->currentIndex() == 0,
           "Preset combo automatically reverted to 0 ('Dowolny / niestandardowy') when custom size was set");
    verify(dialog->legacySize() == 0, "legacySize() returns 0 when custom bounds are active");

    SearchQuery customOverrideQuery = dialog->configuredQuery();
    verify(customOverrideQuery.legacySize() == 0, "legacySize is 0 in customOverrideQuery");
    verify(customOverrideQuery.sizeMin().has_value() && *customOverrideQuery.sizeMin() == 2LL * 1024LL * 1024LL,
           "sizeMin is 2 MiB in customOverrideQuery");
    Q_EMIT dialog->filtersApplied(customOverrideQuery);
    verify(window.m_primarySearch.size == 0, "Primary search legacy size preset cleared on custom override");
    verify(window.m_primarySearch.sizeMin.has_value() && *window.m_primarySearch.sizeMin == 2LL * 1024LL * 1024LL,
           "Primary search received custom sizeMin 2 MiB");

    // Switching back to preset in dialog clears custom bounds
    dialog->m_sizePresetCombo->setCurrentIndex(3); // 100 MiB–1 GiB
    verify(!dialog->m_sizeMinCheck->isChecked() && !dialog->m_sizeMaxCheck->isChecked(),
           "Custom size checkboxes unchecked when preset 3 chosen in combo");
    verify(dialog->legacySize() == 3, "legacySize() returns 3");
    SearchQuery preset3Query = dialog->configuredQuery();
    verify(preset3Query.legacySize() == 3 && !preset3Query.sizeMin().has_value(),
           "preset3Query has legacySize 3 and nullopt sizeMin");
    Q_EMIT dialog->filtersApplied(preset3Query);
    verify(window.m_primarySearch.size == 3 && !window.m_primarySearch.sizeMin.has_value(),
           "Primary search received size preset 3 and cleared custom bounds");

    // Clean up
    auto triggerClearFilters = [&window](Pane pane) {
        window.setActivePane(pane);
        for (QAction *action : window.m_searchFilterButton->menu()->actions()) {
            if (action->text().contains(QStringLiteral("Wyczyść")) || action->text().contains(QStringLiteral("Clear"))) {
                action->trigger();
                break;
            }
        }
    };
    triggerClearFilters(Pane::Primary);
    triggerClearFilters(Pane::Split);
    window.setActivePane(Pane::Primary);
    verify(window.m_primarySearch.toQuery().activeFilterCount() == 0, "Primary search state reset to 0 filters");
    verify(split->searchState().toQuery().activeFilterCount() == 0, "Split search state reset to 0 filters");

    // =========================================================================
    // Stage 3B.4 Final Integration & Primary/Split Full Lifecycle
    // =========================================================================
    // 1. Full Independence: Primary has Filters(3), Split has Filters(1)
    // Configure Primary with 3 filters: target=files, match=glob, ext=pdf,png, size preset 1 (< 1 MiB)
    window.setActivePane(Pane::Primary);
    choose(window.m_searchSizeGroup, 1);
    window.showAdvancedFiltersDialog();
    dialog->m_targetTypeCombo->setCurrentIndex(dialog->m_targetTypeCombo->findData(static_cast<int>(SearchQuery::TargetType::FilesOnly)));
    dialog->m_extensionsEdit->setText(QStringLiteral("pdf, png"));
    Q_EMIT dialog->filtersApplied(dialog->configuredQuery());

    // Primary active filter count: target (1) + ext (1) + size preset 1 (1) = 3
    verify(window.m_primarySearch.toQuery().activeFilterCount() == 3, "Primary activeFilterCount is 3");
    window.updateSearchControls();
    verify(window.m_searchFilterButton->text().contains(QStringLiteral("(3)")), "Primary filter button text shows (3)");

    // Configure Split with 1 filter: date preset 1 (Today)
    window.setActivePane(Pane::Split);
    choose(window.m_searchDateGroup, 1);
    verify(split->searchState().toQuery().activeFilterCount() == 1, "Split activeFilterCount is 1");
    window.updateSearchControls();
    verify(window.m_searchFilterButton->text().contains(QStringLiteral("(1)")), "Split filter button text shows (1)");

    // Switching active pane back to Primary updates filter button text to (3) without state leak
    window.setActivePane(Pane::Primary);
    window.updateSearchControls();
    verify(window.m_searchFilterButton->text().contains(QStringLiteral("(3)")), "Primary filter button text correctly restored to (3)");
    verify(window.m_primarySearch.size == 1 && window.m_primarySearch.targetType == SearchQuery::TargetType::FilesOnly,
           "Primary state completely intact");
    verify(split->searchState().date == 1 && split->searchState().size == 0,
           "Split state completely intact without leaks");

    // 2. Cancel after unapplied edits in dialog leaves state completely unchanged
    window.showAdvancedFiltersDialog();
    verify(window.m_advancedFiltersPane == PaneId::Primary, "Dialog targeting Primary");
    // Make unapplied changes in dialog widgets
    dialog->m_extensionsEdit->setText(QStringLiteral("xyz, abc"));
    dialog->m_sizePresetCombo->setCurrentIndex(4); // > 1 GiB
    // User clicks Cancel (or closes dialog without Apply)
    dialog->reject();
    verify(window.m_primarySearch.extensions == QStringList({QStringLiteral("pdf"), QStringLiteral("png")}),
           "Primary extensions unchanged after Cancel");
    verify(window.m_primarySearch.size == 1, "Primary size preset unchanged after Cancel");

    // 3. Reopening dialog repopulates existing applied state exactly
    window.showAdvancedFiltersDialog();
    verify(dialog->legacySize() == 1, "Reopened dialog reflects applied legacySize 1");
    verify(dialog->extensions() == QStringList({QStringLiteral("pdf"), QStringLiteral("png")}),
           "Reopened dialog reflects applied extensions");
    verify(dialog->targetType() == SearchQuery::TargetType::FilesOnly,
           "Reopened dialog reflects applied targetType FilesOnly");

    // 4. "Wyczyść filtry" (Clear Filters) resets all criteria cleanly to 0
    window.setActivePane(Pane::Primary);
    // Trigger clearFilters action
    for (QAction *action : window.m_searchFilterButton->menu()->actions()) {
        if (action->text().contains(QStringLiteral("Wyczyść")) || action->text().contains(QStringLiteral("Clear"))) {
            action->trigger();
            break;
        }
    }
    verify(window.m_primarySearch.toQuery().activeFilterCount() == 0, "Clear filters resets Primary filter count to 0");
    verify(window.m_primarySearch.type == 0 && window.m_primarySearch.date == 0 && window.m_primarySearch.size == 0,
           "Clear filters cleared presets");
    verify(window.m_primarySearch.extensions.isEmpty() && window.m_primarySearch.categories.isEmpty(),
           "Clear filters cleared extensions and categories");
    verify(!window.m_primarySearch.sizeMin.has_value() && !window.m_primarySearch.dateMin.has_value(),
           "Clear filters cleared custom bounds");
    // Verify Split still has its 1 active filter intact
    verify(split->searchState().toQuery().activeFilterCount() == 1, "Split still has its 1 active filter intact after Primary clear");

    // Also clear Split for clean test teardown
    window.setActivePane(Pane::Split);
    for (QAction *action : window.m_searchFilterButton->menu()->actions()) {
        if (action->text().contains(QStringLiteral("Wyczyść")) || action->text().contains(QStringLiteral("Clear"))) {
            action->trigger();
            break;
        }
    }
    verify(split->searchState().toQuery().activeFilterCount() == 0, "Clear filters resets Split filter count to 0");

    // =========================================================================
    // Stage 3C.1 Search Results Sorting & View Parity
    // =========================================================================
    window.setActivePane(Pane::Primary);
    const QUrl searchLoc = makeSearchLocation(QStringLiteral("needle"), 0, rootA, 0, 0, 0);
    window.navigateTo(searchLoc, true);
    waitForSearch(*window.m_searchController);
    verify(window.m_directoryList->count() == 3 && window.m_directoryDetails->topLevelItemCount() == 3,
           "Search results loaded into both view widgets (3 results: 1 folder, 2 files)");

    // 1. View Parity across all 4 modes (0: Icons, 1: List, 2: Details, 3: Compact)
    for (int mode : {0, 1, 2, 3}) {
        window.setDirectoryViewMode(mode);
        verify(window.m_directoryViewMode == mode, "View mode applied");
        if (mode == 2) {
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryDetails,
                   "Details mode activates m_directoryDetails in stack");
            verify(window.m_directoryDetails->topLevelItemCount() == 3,
                   "Details mode has 3 items");
            verify(!window.m_directoryDetails->isColumnHidden(4),
                   "Search in Details mode displays column 4 (Location)");
        } else {
            verify(window.m_directoryViewStack->currentWidget() == window.m_directoryList,
                   "Icons/List/Compact activates m_directoryList in stack");
            verify(window.m_directoryList->count() == 3,
                   "Icons/List/Compact has 3 items");
        }
        // Parity: item count and folder-first ordering identical across views
        verify(window.m_directoryDetails->topLevelItem(0)->data(0, Qt::UserRole + 1).toBool(),
               "Details item 0 is folder");
        verify(window.m_directoryList->item(0).data(Qt::UserRole + 1).toBool(),
               "List item 0 is folder");
    }

    // 2. In-Memory Sorting without restarting search job
    // Sort by Name (0) Ascending
    window.setSortKey(0);
    window.setSortAscending(true);
    verify(!window.m_searchController->isRunning(), "Changing sort does NOT re-run search job");
    verify(window.m_directoryDetails->topLevelItemCount() == 3, "Sorting maintains 3 items");
    const QString name0 = window.m_directoryDetails->topLevelItem(0)->text(0);
    const QString name1 = window.m_directoryDetails->topLevelItem(1)->text(0);
    const QString name2 = window.m_directoryDetails->topLevelItem(2)->text(0);
    verify(name0 == QStringLiteral("needle-folder"), "Folder is first in Name ASC");
    verify(name1 == QStringLiteral("needle-deep.txt"), "needle-deep.txt is second in Name ASC");
    verify(name2 == QStringLiteral("needle.txt"), "needle.txt is third in Name ASC");

    // Sort by Name (0) Descending
    window.setSortAscending(false);
    verify(!window.m_searchController->isRunning(), "Changing sort direction does NOT re-run search");
    verify(window.m_directoryDetails->topLevelItem(0)->text(0) == QStringLiteral("needle-folder"),
           "Folder remains first in Name DESC");
    verify(window.m_directoryDetails->topLevelItem(1)->text(0) == QStringLiteral("needle.txt"),
           "needle.txt is second in Name DESC");
    verify(window.m_directoryDetails->topLevelItem(2)->text(0) == QStringLiteral("needle-deep.txt"),
           "needle-deep.txt is third in Name DESC");

    // Sort by Size (2) and Date (3)
    window.setSortKey(2);
    window.setSortAscending(true);
    verify(window.m_directoryDetails->topLevelItemCount() == 3 && window.m_directoryList->count() == 3,
           "Sort by Size preserves count in both views");
    window.setSortKey(3);
    window.setSortAscending(true);
    verify(window.m_directoryDetails->topLevelItemCount() == 3 && window.m_directoryList->count() == 3,
           "Sort by Date preserves count in both views");

    // 3. Selection preservation across View Modes and Sorting
    window.setDirectoryViewMode(2); // Details
    window.m_directoryDetails->clearSelection();
    window.m_directoryDetails->topLevelItem(1)->setSelected(true);
    const QString selectedUrl = window.m_directoryDetails->topLevelItem(1)->data(0, Qt::UserRole).toString();
    verify(!selectedUrl.isEmpty(), "Selected item has valid URL");

    // Switch to Compact (mode 3)
    window.setDirectoryViewMode(3);
    verify(selectedDirectoryListUrls(window.m_directoryList).contains(selectedUrl),
           "Selection preserved when switching from Details to Compact in search");

    // Switch sort order to Descending while in Compact mode
    window.setSortAscending(false);
    verify(selectedDirectoryListUrls(window.m_directoryList).contains(selectedUrl),
           "Selection preserved when changing sort direction in search");

    // 4. Primary and Split View Isolation
    window.setDirectoryViewMode(0); // Primary in Icons
    window.setSortKey(0);           // Primary sort Name
    split->setViewMode(2);          // Split in Details
    split->setSortState(2, false);  // Split sort Size DESC
    verify(window.m_directoryViewMode == 0 && window.m_sortKey == 0,
           "Primary view mode and sort state preserved");
    verify(split->viewMode() == 2 && split->sortKey() == 2 && !split->sortAscending(),
           "Split view mode and sort state preserved independently");

    // 5. Navigation from Search to Regular Folder and Back
    window.setDirectoryViewMode(2); // Details
    verify(!window.m_directoryDetails->isColumnHidden(4), "Location column visible in search Details");
    window.navigateTo(rootB, true);
    verify(QTest::qWaitFor([&] { return window.m_directoryDetails->topLevelItemCount() == 1; }, 5000),
           "Regular folder loaded");
    verify(window.m_directoryDetails->isColumnHidden(4), "Location column hidden in regular directory Details");
    window.m_backAction->trigger();
    verify(isSearchLocation(window.m_navigation.currentUrl()), "Navigated back to search URL");
    waitForSearch(*window.m_searchController);
    verify(!window.m_directoryDetails->isColumnHidden(4), "Location column restored on return to search");
    // =========================================================================
    // Stage 3C.2: Search Progress & Loading States + Animation Fix
    // =========================================================================
    // 1. Polish & English Result Count Pluralization
    {
        // Polish numeral inflections: 0, 1, 2, 5, 12, 21, 22, 25, 101
        verify(SearchController::formatResultCount(0, true) == QStringLiteral("0 wyników"), "Polish: 0 wyników");
        verify(SearchController::formatResultCount(1, true) == QStringLiteral("1 wynik"), "Polish: 1 wynik");
        verify(SearchController::formatResultCount(2, true) == QStringLiteral("2 wyniki"), "Polish: 2 wyniki");
        verify(SearchController::formatResultCount(3, true) == QStringLiteral("3 wyniki"), "Polish: 3 wyniki");
        verify(SearchController::formatResultCount(4, true) == QStringLiteral("4 wyniki"), "Polish: 4 wyniki");
        verify(SearchController::formatResultCount(5, true) == QStringLiteral("5 wyników"), "Polish: 5 wyników");
        verify(SearchController::formatResultCount(12, true) == QStringLiteral("12 wyników"), "Polish: 12 wyników (nastki)");
        verify(SearchController::formatResultCount(21, true) == QStringLiteral("21 wyników"), "Polish: 21 wyników");
        verify(SearchController::formatResultCount(22, true) == QStringLiteral("22 wyniki"), "Polish: 22 wyniki");
        verify(SearchController::formatResultCount(25, true) == QStringLiteral("25 wyników"), "Polish: 25 wyników");
        verify(SearchController::formatResultCount(101, true) == QStringLiteral("101 wyników"), "Polish: 101 wyników");
        verify(SearchController::formatResultCount(102, true) == QStringLiteral("102 wyniki"), "Polish: 102 wyniki");
        verify(SearchController::formatResultCount(112, true) == QStringLiteral("112 wyników"), "Polish: 112 wyników");
        verify(SearchController::formatResultCount(122, true) == QStringLiteral("122 wyniki"), "Polish: 122 wyniki");

        // English inflections: 0, 1, 2, 5, 101
        verify(SearchController::formatResultCount(0, false) == QStringLiteral("0 results"), "English: 0 results");
        verify(SearchController::formatResultCount(1, false) == QStringLiteral("1 result"), "English: 1 result");
        verify(SearchController::formatResultCount(2, false) == QStringLiteral("2 results"), "English: 2 results");
        verify(SearchController::formatResultCount(5, false) == QStringLiteral("5 results"), "English: 5 results");
        verify(SearchController::formatResultCount(101, false) == QStringLiteral("101 results"), "English: 101 results");
    }

    // 2. SearchProgressBar: Indeterminate Animation, Phase Advance, Determinate Mode & Timer Lifecycle
    {
        SearchProgressBar bar;
        bar.resize(300, 7);
        verify(!bar.isAnimationRunning(), "SearchProgressBar initially not animating");
        verify(!bar.isIndeterminate(), "SearchProgressBar initially not indeterminate");

        // Enter indeterminate mode while visible
        bar.setRange(0, 0);
        bar.show();
        verify(bar.isIndeterminate(), "SearchProgressBar recognized as indeterminate when range is (0, 0)");
        verify(bar.isAnimationRunning(), "SearchProgressBar animation starts when indeterminate and visible");

        // Real visual animation phase advance over time
        const qreal phase1 = bar.animationPhase();
        QTest::qWait(60);
        const qreal phase2 = bar.animationPhase();
        verify(phase1 != phase2, "SearchProgressBar animation phase actively changes over time (fluid motion)");

        // Switch to determinate mode
        bar.setRange(0, 100);
        bar.setValue(50);
        verify(!bar.isIndeterminate(), "SearchProgressBar recognized as determinate when range is (0, 100)");
        verify(!bar.isAnimationRunning(), "SearchProgressBar animation timer stops in determinate mode");

        // Return to indeterminate mode, then hide widget
        bar.setRange(0, 0);
        verify(bar.isAnimationRunning(), "SearchProgressBar animation resumes on indeterminate mode");
        bar.hide();
        verify(!bar.isAnimationRunning(), "SearchProgressBar animation timer stops immediately when widget is hidden");

        // Primary and Split independent instances
        SearchProgressBar splitBar;
        splitBar.resize(300, 7);
        bar.show(); // bar is indeterminate range(0, 0)
        splitBar.show();
        splitBar.setRange(0, 100); // splitBar is determinate
        verify(bar.isAnimationRunning(), "Primary bar animates in indeterminate mode");
        verify(!splitBar.isAnimationRunning(), "Split bar does not animate in determinate mode");
        bar.hide();
        splitBar.hide();
        verify(!bar.isAnimationRunning() && !splitBar.isAnimationRunning(), "No orphaned timers after hiding bars");
    }

    // 3. Determinate vs Indeterminate Progress Bar States & SearchController status text
    {
        SearchController sc;
        QFrame progressFrame;
        SearchProgressBar progressBar(&progressFrame);
        QLabel status;
        QLineEdit edit;
        QAction stopAction(nullptr), scopeAction(nullptr), filterIndicatorAction(nullptr);
        QToolButton scopeButton, filterButton;
        QActionGroup scopeGroup(nullptr), typeGroup(nullptr), dateGroup(nullptr), sizeGroup(nullptr);
        QAction typeInfo(nullptr), dateInfo(nullptr), sizeInfo(nullptr);

        SearchUiController ui(
            {&edit, &stopAction, &scopeAction, &scopeButton, &filterButton, &filterIndicatorAction,
             &scopeGroup, &typeGroup, &typeInfo, &dateGroup, &dateInfo, &sizeGroup, &sizeInfo,
             &progressFrame, &progressBar, &status},
            {[](const char *, const char *en) { return QString::fromUtf8(en); },
             [](const QUrl &url) { return url.fileName(); }, {}, [] { return false; }});

        // A. Idle state: not running
        verify(!sc.isRunning() && !sc.isDeterminateProgress(), "SearchController initially idle");
        ui.updateProgress(sc);
        ui.updateStatus(sc, 0, 0);
        verify(!progressFrame.isVisible(), "Progress frame is hidden when search is idle");
        verify(progressBar.minimum() == 0 && progressBar.maximum() == 100, "Progress bar range is (0, 100) when idle");
        verify(!progressBar.isAnimationRunning(), "Animation is stopped when search is idle");
        verify(status.text() == QStringLiteral("No results • scope: Current folder and subfolders"),
               "Idle with 0 visible count shows clean Empty State (not '0 results')");

        // B. Running state, 0 visible count, indeterminate (progressPercent == 0)
        sc.setRunningForTest(true, 4, 1, 0);
        sc.setProgressPercentForTest(0);
        verify(sc.isRunning() && !sc.isDeterminateProgress(), "Search is running with no percentage reported (indeterminate)");
        ui.updateProgress(sc);
        ui.updateStatus(sc, 0, 0);
        verify(progressFrame.isVisible(), "Progress frame is shown while search is running");
        verify(progressBar.minimum() == 0 && progressBar.maximum() == 0,
               "Progress bar is in indeterminate mode range (0, 0) when no percent available");
        verify(progressBar.isAnimationRunning(), "Progress bar animation actively runs while search is running indeterminate");
        verify(status.text() == QStringLiteral("Searching… • 1/4 locations • Current folder and subfolders"),
               "Status text while running with 0 items indicates Searching state, not 'No results'");
        verify(!status.text().contains(QStringLiteral("No results")) && !status.text().contains(QStringLiteral("0 results")),
               "Running state NEVER displays 'No results' or '0 results'");

        // C. Running state with partial results found (1 item vs >1 items)
        ui.updateStatus(sc, 1, 0);
        verify(status.text() == QStringLiteral("1 result • 1/4 locations • Current folder and subfolders"),
               "Running state with 1 result uses singular '1 result' (not '1 results')");
        ui.updateStatus(sc, 5, 0);
        verify(status.text() == QStringLiteral("5 results • 1/4 locations • Current folder and subfolders"),
               "Running state with partial results shows item count and completed/total locations");

        // D. Running state with determinate progress reported by KIO
        sc.setProgressPercentForTest(75);
        verify(sc.isRunning() && sc.isDeterminateProgress() && sc.progressPercent() == 75,
               "Search has determinate progress when percent > 0");
        ui.updateProgress(sc);
        verify(progressFrame.isVisible(), "Progress frame remains visible with determinate progress");
        verify(progressBar.minimum() == 0 && progressBar.maximum() == 100 && progressBar.value() == 75,
               "Progress bar switches to range (0, 100) with value 75 when determinate progress is available");
        verify(!progressBar.isAnimationRunning(), "Animation timer stops when determinate progress is active");

        // E. Completed state with results
        sc.setRunningForTest(false, 4, 4, 0);
        sc.setProgressPercentForTest(100);
        verify(!sc.isRunning() && !sc.isDeterminateProgress(), "Search is completed");
        ui.updateProgress(sc);
        ui.updateStatus(sc, 1, 0);
        verify(!progressFrame.isVisible(), "Progress frame is hidden on completion");
        verify(progressBar.minimum() == 0 && progressBar.maximum() == 100 && progressBar.value() == 100,
               "Progress bar is (0, 100) with value 100 on completion");
        verify(!progressBar.isAnimationRunning(), "Animation timer stops on completion");
        verify(status.text() == QStringLiteral("1 result • scope: Current folder and subfolders"),
               "Completed state with 1 result displays singular '1 result'");
        ui.updateStatus(sc, 12, 0);
        verify(status.text() == QStringLiteral("12 results • scope: Current folder and subfolders"),
               "Completed state with results displays count and scope");

        // F. Completed state with 0 results (Empty State)
        ui.updateStatus(sc, 0, 0);
        verify(status.text() == QStringLiteral("No results • scope: Current folder and subfolders"),
               "Completed state with 0 results displays clean Empty State");

        // G. Completed state with errors
        sc.setRunningForTest(false, 4, 4, 2);
        ui.updateStatus(sc, 0, 0);
        verify(status.text() == QStringLiteral("No results • scope: Current folder and subfolders • 2 locations with errors"),
               "Empty state with errors reports error locations summary");
        ui.updateStatus(sc, 8, 1);
        verify(status.text() == QStringLiteral("8 results • scope: Current drive • 2 locations with errors"),
               "Completed state with results and errors reports count and error locations");

        // H. Scope label translation
        sc.setRunningForTest(true, 2, 0, 0);
        verify(sc.statusText(0, 2).contains(QStringLiteral("This PC")), "English scope 2 label");
        verify(sc.statusText(0, 1).contains(QStringLiteral("Current drive")), "English scope 1 label");
        verify(sc.statusText(0, 0).contains(QStringLiteral("Current folder and subfolders")), "English scope 0 label with subfolders");
        verify(sc.statusText(0, 0, false).contains(QStringLiteral("Current folder only")), "English scope 0 label without subfolders");
    }

    // 4. Batching / UI Thread Protection (Stress ingestion)
    {
        SearchController batchSearch;
        QSignalSpy renderSpy(&batchSearch, &SearchController::resultsChanged);
        for (int i = 0; i < 20000; ++i) {
            FileInfo info;
            info.name = QStringLiteral("item_%1.txt").arg(i);
            info.url = QUrl::fromLocalFile(QStringLiteral("/dummy/item_%1.txt").arg(i));
            info.size = 42;
            batchSearch.addPendingFileForTest(info);
        }
        verify(batchSearch.files().size() == 20000, "SearchController ingests 20,000 files");
        verify(batchSearch.isRenderTimerActiveForTest(), "Batch render timer is active during ingestion");
        verify(QTest::qWaitFor([&] { return renderSpy.count() >= 1; }, 1000),
               "Batch render timer coalesces 20,000 items and emits resultsChanged");
    }

    // 5. Live Window & Split View Progress Animation & Isolation
    {
        // Primary pane empty search: verify indeterminate mode and animation state
        const QUrl searchNone = makeSearchLocation(QStringLiteral("nonexistent_pattern_xyz"), 0, rootA, 0, 0, 0);
        window.loadSearchLocation(searchNone);
        if (window.m_searchController->isRunning()) {
            verify(window.m_searchProgressFrame->isVisible(), "Primary progress frame visible while searching");
            verify(window.m_searchProgressBar->minimum() == 0 && window.m_searchProgressBar->maximum() == 0,
                   "Primary progress bar is in indeterminate mode range (0, 0)");
            verify(window.m_searchProgressBar->isAnimationRunning(),
                   "Primary progress bar has active running animation during search");
            verify(!window.m_directoryStatus->text().contains(QStringLiteral("Brak wyników"))
                   && !window.m_directoryStatus->text().contains(QStringLiteral("No results")),
                   "Primary status label does not display 'Brak wyników' while still running");
        }
        waitForSearch(*window.m_searchController);
        verify(!window.m_searchProgressFrame->isVisible(), "Primary progress frame hidden on completion");
        verify(!window.m_searchProgressBar->isAnimationRunning(), "Primary animation stopped on completion");
        verify(window.m_directoryStatus->text().contains(QStringLiteral("Brak wyników"))
               || window.m_directoryStatus->text().contains(QStringLiteral("No results")),
               "Primary status label displays Empty State after search finishes with 0 items");

        // Split pane search: verify independence from Primary
        split->navigateTo(searchNone, true);
        if (split->searchController()->isRunning()) {
            verify(split->searchProgressFrame()->isVisible(), "Split progress frame visible while split searching");
            verify(split->searchProgressBar()->minimum() == 0 && split->searchProgressBar()->maximum() == 0,
                   "Split progress bar is in indeterminate mode range (0, 0)");
            verify(split->searchProgressBar()->isAnimationRunning(),
                   "Split progress bar has active running animation during split search");
            verify(!window.m_searchProgressFrame->isVisible(),
                   "Primary progress frame is not affected by split search");
            verify(!window.m_searchProgressBar->isAnimationRunning(),
                   "Primary progress bar animation does not run when only Split is searching");
        }
        waitForSearch(*split->searchController());
        verify(!split->searchProgressFrame()->isVisible(), "Split progress frame hidden on completion");
        verify(!split->searchProgressBar()->isAnimationRunning(), "Split animation stopped on completion");
        verify(split->statusLabel()->text().contains(QStringLiteral("Brak wyników"))
               || split->statusLabel()->text().contains(QStringLiteral("No results")),
               "Split status label displays Empty State after search finishes with 0 items");

        // Split cancellation
        const QUrl searchNeedle = makeSearchLocation(QStringLiteral("needle"), 0, rootA, 0, 0, 0);
        split->navigateTo(searchNeedle, true);
        split->cancelSearch(true);
        verify(!split->searchProgressFrame()->isVisible(), "Split progress frame hidden immediately on cancel");
        verify(!split->searchProgressBar()->isAnimationRunning(), "Split animation stopped on cancel");
        verify(!split->searchController()->isRunning(), "Split search controller canceled");
    }

    // =========================================================================
    // Stage 3C.3: Search Errors, Cancellation & Retry
    // =========================================================================
    // 1. Cancellation State, Partial Results Retention, Status Messaging & Re-entrancy
    {
        SearchController sc;
        verify(!sc.wasCanceled(), "Initially not canceled");
        verify(!sc.cancel(), "cancel() returns false when no search is active");
        verify(!sc.wasCanceled(), "wasCanceled remains false when cancel() returns false");

        // Start search and cancel as user
        sc.start(QStringLiteral("needle"), {rootA}, false);
        verify(sc.isRunning(), "Search started and running");
        verify(!sc.wasCanceled(), "wasCanceled is false while running");

        // Multiple rapid cancel calls: first succeeds and sets wasCanceled, subsequent return false and preserve state
        verify(sc.cancel(true), "First cancel(true) returns true");
        verify(!sc.isRunning(), "Search is no longer running after cancel");
        verify(sc.wasCanceled(), "wasCanceled is true after cancel(true)");
        verify(!sc.cancel(true), "Second immediate cancel(true) returns false (idempotent)");
        verify(sc.wasCanceled(), "wasCanceled remains true on repeated cancel");

        // Internal non-user cancel (e.g. from starting a new search or navigation)
        sc.start(QStringLiteral("needle"), {rootA}, false);
        verify(sc.isRunning(), "Replacement search is running");
        verify(!sc.wasCanceled(), "New start() resets wasCanceled to false");
        verify(sc.cancel(false), "Internal cancel(false) returns true");
        verify(!sc.wasCanceled(), "cancel(false) does not set wasCanceled");
    }

    // 2. StatusText Formatting: Canceled vs Empty vs Errors vs Results
    {
        SearchController sc;
        // Test cancellation statusText in current test environment (English by default under LANG=C.UTF-8)
        sc.setRunningForTest(false, 1, 1, 0);
        sc.setCanceledForTest(true);
        verify(sc.statusText(0, 0) == QStringLiteral("Search canceled • no results • scope: Current folder and subfolders")
               || sc.statusText(0, 0) == QStringLiteral("Wyszukiwanie anulowane • brak wyników • zakres: Bieżący folder i podfoldery"),
               "Canceled with 0 results indicates cancellation and empty status");
        verify(sc.statusText(1, 0) == QStringLiteral("Search canceled • 1 result • scope: Current folder and subfolders")
               || sc.statusText(1, 0) == QStringLiteral("Wyszukiwanie anulowane • 1 wynik • zakres: Bieżący folder i podfoldery"),
               "Canceled with 1 result indicates cancellation and preserves 1 result");
        verify(sc.statusText(3, 1) == QStringLiteral("Search canceled • 3 results • scope: Current drive")
               || sc.statusText(3, 1) == QStringLiteral("Wyszukiwanie anulowane • 3 wyniki • zakres: Bieżący dysk"),
               "Canceled with 3 results indicates cancellation and preserves count");
        verify(sc.statusText(15, 2) == QStringLiteral("Search canceled • 15 results • scope: This PC")
               || sc.statusText(15, 2) == QStringLiteral("Wyszukiwanie anulowane • 15 wyników • zakres: Ten komputer"),
               "Canceled with 15 results indicates cancellation and preserves count and scope");

        // Normal non-canceled
        sc.setCanceledForTest(false);
        verify(sc.statusText(0, 0) == QStringLiteral("No results • scope: Current folder and subfolders")
               || sc.statusText(0, 0) == QStringLiteral("Brak wyników • zakres: Bieżący folder i podfoldery"),
               "Normal finished with 0 results");
        verify(sc.statusText(2, 0) == QStringLiteral("2 results • scope: Current folder and subfolders")
               || sc.statusText(2, 0) == QStringLiteral("2 wyniki • zakres: Bieżący folder i podfoldery"),
               "Normal finished with 2 results");

        // With partial root errors
        sc.setRunningForTest(false, 3, 3, 1);
        verify(sc.statusText(4, 0).contains(QStringLiteral("4 results • scope: Current folder and subfolders • 1 locations with errors"))
               || sc.statusText(4, 0).contains(QStringLiteral("4 wyniki • zakres: Bieżący folder i podfoldery • 1 lokalizacji z błędem")),
               "Completed with results and 1 error location");
        sc.setCanceledForTest(true);
        verify(sc.statusText(4, 0).contains(QStringLiteral("Search canceled • 4 results • scope: Current folder and subfolders • 1 locations with errors"))
               || sc.statusText(4, 0).contains(QStringLiteral("Wyszukiwanie anulowane • 4 wyniki • zakres: Bieżący folder i podfoldery • 1 lokalizacji z błędem")),
               "Canceled with results and 1 error location");

        // Also test Polish pluralization helper explicitly
        verify(SearchController::formatResultCount(0, true) == QStringLiteral("0 wyników"), "Polish 0 wyników");
        verify(SearchController::formatResultCount(1, true) == QStringLiteral("1 wynik"), "Polish 1 wynik");
        verify(SearchController::formatResultCount(2, true) == QStringLiteral("2 wyniki"), "Polish 2 wyniki");
        verify(SearchController::formatResultCount(4, true) == QStringLiteral("4 wyniki"), "Polish 4 wyniki");
        verify(SearchController::formatResultCount(5, true) == QStringLiteral("5 wyników"), "Polish 5 wyników");
        verify(SearchController::formatResultCount(12, true) == QStringLiteral("12 wyników"), "Polish 12 wyników");
        verify(SearchController::formatResultCount(22, true) == QStringLiteral("22 wyniki"), "Polish 22 wyniki");
        verify(SearchController::formatResultCount(25, true) == QStringLiteral("25 wyników"), "Polish 25 wyników");
    }

    // 3. Generation Isolation & Stale Worker Rejection on Retry (Start A -> Cancel -> Start B)
    {
        SearchController sc;
        sc.start(QStringLiteral("needle"), {rootA}, false);
        verify(sc.m_searchJobs.size() == 1, "One search job launched for rootA");
        QPointer<KIO::ListJob> jobA = sc.m_searchJobs.first();
        verify(jobA != nullptr, "jobA is valid pointer");

        // Cancel A
        sc.cancel(true);
        verify(sc.wasCanceled(), "Search A canceled");

        // Retry with Search B immediately
        sc.start(QStringLiteral("needle"), {rootB}, false);
        verify(!sc.wasCanceled(), "Search B start resets wasCanceled");
        verify(sc.isRunning(), "Search B is actively running");

        // Attempt to deliver stale entries from dead job A
        jobA->entries(jobA, {entry(a + "/stale_stage3c3.txt")});
        verify(sc.files().isEmpty(), "Late worker entries from job A discarded by Search B generation check");

        waitForSearch(sc);
        verify(!sc.isRunning(), "Search B completed");
        verify(paths(sc.files()) == QSet<QString>{b + "/needle-second.txt"}, "Search B contains only its own results");
    }

    // 4. Primary and Split Pane Live Cancellation & Isolation
    {
        // Cancel primary via UI action while running
        const QUrl searchAllA = makeSearchLocation(QStringLiteral("*"), 0, rootA, 0, 0, 0);
        window.loadSearchLocation(searchAllA);
        verify(window.m_searchController->isRunning(), "Primary search running");
        verify(window.m_searchProgressFrame->isVisible(), "Primary progress frame visible");

        window.cancelSearch(PaneId::Primary, true);
        verify(!window.m_searchController->isRunning(), "Primary search stopped after cancelSearch");
        verify(!window.m_searchProgressFrame->isVisible(), "Primary progress frame hidden immediately on cancel");
        verify(window.m_searchController->wasCanceled(), "Primary searchController wasCanceled is true");
        verify(window.m_directoryStatus->text().contains(QStringLiteral("Wyszukiwanie anulowane"))
               || window.m_directoryStatus->text().contains(QStringLiteral("Search canceled")),
               "Primary status label displays cancellation message");

        // Primary retry after cancellation
        window.loadSearchLocation(searchAllA);
        verify(window.m_searchController->isRunning(), "Primary search re-run starts cleanly");
        verify(!window.m_searchController->wasCanceled(), "Primary retry resets wasCanceled");
        waitForSearch(*window.m_searchController);
        verify(!window.m_searchController->isRunning(), "Primary retry completed");

        // Split pane cancel does not affect Primary
        split->navigateTo(searchAllA, true);
        verify(split->searchController()->isRunning(), "Split search running");
        verify(!window.m_searchController->isRunning(), "Primary remains untouched");

        split->cancelSearch(true);
        verify(!split->searchController()->isRunning(), "Split search stopped");
        verify(!split->searchProgressFrame()->isVisible(), "Split progress frame hidden on cancel");
        verify(split->searchController()->wasCanceled(), "Split search wasCanceled is true");
        verify(split->statusLabel()->text().contains(QStringLiteral("Wyszukiwanie anulowane"))
               || split->statusLabel()->text().contains(QStringLiteral("Search canceled")),
               "Split status label displays cancellation message");
    }

    // 5. Comprehensive KIO Error Handling, Partial Success, Retry & Pane Isolation
    {
        // A. Non-recursive search with invalid root triggers real KIO listing error
        const QUrl nonexistentRoot = QUrl::fromLocalFile(files.filePath("nonexistent_directory_error_audit"));
        SearchPlan plan;
        plan.roots = {rootB, nonexistentRoot};
        plan.recursive = false;
        plan.workerPatterns = {QString()};

        SearchController errorSc;
        errorSc.start(plan, false);
        waitForSearch(errorSc);
        verify(!errorSc.isRunning(), "Search with partial error completed");
        verify(errorSc.totalRoots() == 2, "2 total roots scheduled");
        verify(errorSc.completedRoots() == 2, "2 roots completed");
        verify(errorSc.errorCount() == 1, "Exactly 1 root error reported for nonexistent root");
        verify(!errorSc.lastError().isEmpty(), "errorSc recorded KIO error string");
        verify(paths(errorSc.files()) == QSet<QString>{b + "/needle-second.txt"},
               "Partial success: valid root results preserved despite sibling root error");
        verify(!errorSc.wasCanceled(), "Error search is not marked as canceled");
        const QString partialErrorStatus = errorSc.statusText(errorSc.files().size(), 0);
        verify(partialErrorStatus.contains(QStringLiteral("1 result")) || partialErrorStatus.contains(QStringLiteral("1 wynik")),
               "Status preserves result count for partial success");
        verify(partialErrorStatus.contains(QStringLiteral("error")) || partialErrorStatus.contains(QStringLiteral("błęd")),
               "Status includes error indicator for partial success");

        // B. Rejection of invalid search location (e.g. malformed criteria)
        SearchController rejectSc;
        const QUrl invalidSearchUrl(QStringLiteral("thispcsearch:/?search=&sizemin=invalid-size&base=") + rootA.toString());
        verify(!rejectSc.startFromLocation(invalidSearchUrl, {rootA}, false), "Invalid location rejected by startFromLocation");
        verify(!rejectSc.isRunning(), "Rejected search is not running");
        verify(rejectSc.errorCount() == 1, "Rejected search records 1 error");
        verify(!rejectSc.lastError().isEmpty(), "Rejected search stores error description");
        verify(!rejectSc.wasCanceled(), "Rejected search is not marked as canceled");

        // C. Clean retry after error clears error count and errors in status
        errorSc.start(QStringLiteral("needle"), {rootB}, false);
        verify(errorSc.isRunning(), "Retry search running");
        verify(errorSc.errorCount() == 0, "Retry resets error count to 0");
        verify(errorSc.lastError().isEmpty(), "Retry clears lastError");
        waitForSearch(errorSc);
        verify(!errorSc.isRunning(), "Retry completed successfully");
        verify(errorSc.errorCount() == 0, "Retry finished with 0 errors");
        verify(!errorSc.statusText(errorSc.files().size(), 0).contains(QStringLiteral("error"))
               && !errorSc.statusText(errorSc.files().size(), 0).contains(QStringLiteral("błęd")),
               "Retry status contains no error notices");

        // D. Split and Primary Error Isolation in live window
        window.loadSearchLocation(makeSearchLocation(QStringLiteral("needle"), 0, rootA, 0, 0, 0));
        waitForSearch(*window.m_searchController);
        verify(!window.m_searchProgressFrame->isVisible(), "Primary progress frame hidden after search completes");
        verify(window.m_searchController->errorCount() == 0, "Primary search succeeded on rootA");

        split->navigateTo(makeSearchLocation(QStringLiteral("needle"), 0, rootB, 0, 0, 0), true);
        waitForSearch(*split->searchController());
        verify(!split->searchProgressFrame()->isVisible(), "Split progress frame hidden after successful search");
        verify(split->searchController()->errorCount() == 0, "Split search succeeded with 0 errors");
        verify(paths(split->searchController()->files()) == QSet<QString>{b + "/needle-second.txt"},
               "Split results unaffected by prior Primary operations");
    }

    // =========================================================================
    // Stage 3C.4: Final Search Integration & Regression
    // =========================================================================
    {
        // 1. Comprehensive Criteria Conjunction & URL Restoration
        // Query with text pattern, category (Images), dateMin, sizeMax
        const qint64 anHourAgo = QDateTime::currentSecsSinceEpoch() - 3600;
        SearchQuery fullQuery;
        fullQuery.setText(QStringLiteral("photo"));
        fullQuery.setCategories({SearchQuery::Category::Images});
        fullQuery.setDateRange(anHourAgo, std::nullopt);
        fullQuery.setSizeRange(std::nullopt, 1024 * 1024); // 1 MiB
        fullQuery.setScope(SearchQuery::Scope::CurrentFolder);
        fullQuery.setBase(stage2Root);

        const QUrl fullQueryUrl = fullQuery.toUrl();
        verify(fullQueryUrl.isValid(), "Stage 3C.4 full query URL is valid");
        SearchRequest req = SearchRequest::fromLocation(fullQueryUrl);
        verify(req.isRunnable(), "Stage 3C.4 request is runnable");
        verify(req.query().categories() == QList<SearchQuery::Category>{SearchQuery::Category::Images},
               "Categories restored accurately from URL");
        verify(req.query().sizeMax() == 1024 * 1024, "SizeMax restored accurately from URL");

        // Execute against stage2 fixture (has photo.jpg and doc.pdf)
        SearchController fullSc;
        verify(fullSc.startFromLocation(fullQueryUrl, {stage2Root}, false), "Start fullSc query");
        waitForSearch(fullSc);
        verify(!fullSc.isRunning(), "fullSc completed");
        verify(fullSc.errorCount() == 0, "fullSc completed without errors");
        verify(paths(fullSc.files()) == QSet<QString>{stage2Dir + "/photo.jpg"},
               "photo.jpg matched by AND conjunction (name, image category, date, size), doc.pdf excluded");

        // 2. Rapid Stress Cycles: Multiple rapid Start/Stop/Retry sequences
        for (int cycle = 0; cycle < 5; ++cycle) {
            fullSc.start(QStringLiteral("needle"), {rootA}, false);
            verify(fullSc.isRunning(), "Cycle start is running");
            verify(fullSc.cancel(true), "Cycle cancel succeeds");
            verify(!fullSc.isRunning(), "Cycle stopped");
            verify(fullSc.wasCanceled(), "Cycle wasCanceled is true");
        }
        // Immediate clean start after rapid cancel stress
        fullSc.start(QStringLiteral("needle"), {rootB}, false);
        verify(fullSc.isRunning(), "Clean start running after rapid cancel stress");
        verify(!fullSc.wasCanceled(), "wasCanceled reset to false");
        waitForSearch(fullSc);
        verify(!fullSc.isRunning() && paths(fullSc.files()) == QSet<QString>{b + "/needle-second.txt"},
               "Clean search completes deterministically after rapid stress cycles");

        // 3. Repeatable View Modes Cycling across Search Results in Live Window
        window.setActivePane(Pane::Primary);
        window.navigateTo(makeSearchLocation(QStringLiteral("needle"), 0, rootA, 0, 0, 0), true);
        waitForSearch(*window.m_searchController);
        verify(window.m_directoryDetails->topLevelItemCount() == 3, "3 results in Details view");

        // Details -> Icons -> List -> Compact -> Details
        const int viewModes[] = {0, 1, 3, 2};
        for (int vm : viewModes) {
            window.setDirectoryViewMode(vm);
            verify(window.m_directoryViewMode == vm, "View mode switched cleanly");
            verify(!window.m_searchProgressFrame->isVisible(), "Progress frame stays hidden across view mode switches");
        }
        verify(!window.m_directoryDetails->isColumnHidden(4), "Location column visible in search Details after cycling modes");

        // 4. Primary and Split Full Isolation: Concurrent distinct searches
        const QUrl searchA = makeSearchLocation(QStringLiteral("needle"), 0, rootA, 0, 0, 0);
        const QUrl searchB = makeSearchLocation(QStringLiteral("needle"), 0, rootB, 0, 0, 0);

        window.loadSearchLocation(searchA);
        split->navigateTo(searchB, true);
        waitForSearch(*window.m_searchController);
        waitForSearch(*split->searchController());

        verify(paths(window.m_searchController->files()) == expectedA,
               "Primary contains exclusively rootA results");
        verify(paths(split->searchController()->files()) == QSet<QString>{b + "/needle-second.txt"},
               "Split contains exclusively rootB results");

        // Verify independent sorting & view mode
        window.setDirectoryViewMode(0); // Icons
        window.setSortKey(0);          // Name
        split->setViewMode(2);         // Details
        split->setSortState(2, false); // Size DESC

        verify(window.m_directoryViewMode == 0 && window.m_sortKey == 0,
               "Primary preserves Icons mode and Name sorting");
        verify(split->viewMode() == 2 && split->sortKey() == 2 && !split->sortAscending(),
               "Split preserves Details mode and Size DESC sorting");
    }

    // =========================================================================
    // Stage 3D.1: Search Scope & Glob UX Polish — Scope descriptions & recursive
    // =========================================================================
    {
        SearchController sc;

        // 1. Scope 0 with recursive=true (default) vs recursive=false across all states
        // A. Running state
        sc.setRunningForTest(true, 1, 0, 0);
        verify(sc.statusText(0, 0, true) == QStringLiteral("Searching… • 0/1 locations • Current folder and subfolders")
               || sc.statusText(0, 0, true) == QStringLiteral("Wyszukiwanie… • 0/1 lokalizacji • Bieżący folder i podfoldery"),
               "Stage 3D.1: Scope 0 recursive=true running state");
        verify(sc.statusText(0, 0, false) == QStringLiteral("Searching… • 0/1 locations • Current folder only")
               || sc.statusText(0, 0, false) == QStringLiteral("Wyszukiwanie… • 0/1 lokalizacji • Tylko bieżący folder"),
               "Stage 3D.1: Scope 0 recursive=false running state");
        verify(sc.statusText(5, 0, true) == QStringLiteral("5 results • 0/1 locations • Current folder and subfolders")
               || sc.statusText(5, 0, true) == QStringLiteral("5 wyników • 0/1 lokalizacji • Bieżący folder i podfoldery"),
               "Stage 3D.1: Scope 0 recursive=true running with results");
        verify(sc.statusText(5, 0, false) == QStringLiteral("5 results • 0/1 locations • Current folder only")
               || sc.statusText(5, 0, false) == QStringLiteral("5 wyników • 0/1 lokalizacji • Tylko bieżący folder"),
               "Stage 3D.1: Scope 0 recursive=false running with results");

        // B. Empty state (0 results, not running, not canceled)
        sc.setRunningForTest(false, 1, 1, 0);
        sc.setCanceledForTest(false);
        verify(sc.statusText(0, 0, true) == QStringLiteral("No results • scope: Current folder and subfolders")
               || sc.statusText(0, 0, true) == QStringLiteral("Brak wyników • zakres: Bieżący folder i podfoldery"),
               "Stage 3D.1: Scope 0 recursive=true empty state");
        verify(sc.statusText(0, 0, false) == QStringLiteral("No results • scope: Current folder only")
               || sc.statusText(0, 0, false) == QStringLiteral("Brak wyników • zakres: Tylko bieżący folder"),
               "Stage 3D.1: Scope 0 recursive=false empty state");

        // C. Results state
        verify(sc.statusText(1, 0, true) == QStringLiteral("1 result • scope: Current folder and subfolders")
               || sc.statusText(1, 0, true) == QStringLiteral("1 wynik • zakres: Bieżący folder i podfoldery"),
               "Stage 3D.1: Scope 0 recursive=true 1 result");
        verify(sc.statusText(1, 0, false) == QStringLiteral("1 result • scope: Current folder only")
               || sc.statusText(1, 0, false) == QStringLiteral("1 wynik • zakres: Tylko bieżący folder"),
               "Stage 3D.1: Scope 0 recursive=false 1 result");
        verify(sc.statusText(3, 0, true) == QStringLiteral("3 results • scope: Current folder and subfolders")
               || sc.statusText(3, 0, true) == QStringLiteral("3 wyniki • zakres: Bieżący folder i podfoldery"),
               "Stage 3D.1: Scope 0 recursive=true multiple results");
        verify(sc.statusText(3, 0, false) == QStringLiteral("3 results • scope: Current folder only")
               || sc.statusText(3, 0, false) == QStringLiteral("3 wyniki • zakres: Tylko bieżący folder"),
               "Stage 3D.1: Scope 0 recursive=false multiple results");

        // D. Canceled state
        sc.setCanceledForTest(true);
        verify(sc.statusText(0, 0, true) == QStringLiteral("Search canceled • no results • scope: Current folder and subfolders")
               || sc.statusText(0, 0, true) == QStringLiteral("Wyszukiwanie anulowane • brak wyników • zakres: Bieżący folder i podfoldery"),
               "Stage 3D.1: Scope 0 recursive=true canceled empty");
        verify(sc.statusText(0, 0, false) == QStringLiteral("Search canceled • no results • scope: Current folder only")
               || sc.statusText(0, 0, false) == QStringLiteral("Wyszukiwanie anulowane • brak wyników • zakres: Tylko bieżący folder"),
               "Stage 3D.1: Scope 0 recursive=false canceled empty");
        verify(sc.statusText(4, 0, true) == QStringLiteral("Search canceled • 4 results • scope: Current folder and subfolders")
               || sc.statusText(4, 0, true) == QStringLiteral("Wyszukiwanie anulowane • 4 wyniki • zakres: Bieżący folder i podfoldery"),
               "Stage 3D.1: Scope 0 recursive=true canceled with results");
        verify(sc.statusText(4, 0, false) == QStringLiteral("Search canceled • 4 results • scope: Current folder only")
               || sc.statusText(4, 0, false) == QStringLiteral("Wyszukiwanie anulowane • 4 wyniki • zakres: Tylko bieżący folder"),
               "Stage 3D.1: Scope 0 recursive=false canceled with results");

        // E. Errors state
        sc.setCanceledForTest(false);
        sc.setRunningForTest(false, 2, 2, 1);
        verify(sc.statusText(2, 0, true).contains(QStringLiteral("scope: Current folder and subfolders • 1 locations with errors"))
               || sc.statusText(2, 0, true).contains(QStringLiteral("zakres: Bieżący folder i podfoldery • 1 lokalizacji z błędem")),
               "Stage 3D.1: Scope 0 recursive=true with errors");
        verify(sc.statusText(2, 0, false).contains(QStringLiteral("scope: Current folder only • 1 locations with errors"))
               || sc.statusText(2, 0, false).contains(QStringLiteral("zakres: Tylko bieżący folder • 1 lokalizacji z błędem")),
               "Stage 3D.1: Scope 0 recursive=false with errors");

        // 2. Scope 1 and Scope 2 consistency (unaffected by recursive flag)
        verify(sc.statusText(0, 1, true).contains(QStringLiteral("Current drive"))
               || sc.statusText(0, 1, true).contains(QStringLiteral("Bieżący dysk")),
               "Stage 3D.1: Scope 1 with recursive=true");
        verify(sc.statusText(0, 1, false).contains(QStringLiteral("Current drive"))
               || sc.statusText(0, 1, false).contains(QStringLiteral("Bieżący dysk")),
               "Stage 3D.1: Scope 1 with recursive=false");
        verify(sc.statusText(0, 2, true).contains(QStringLiteral("This PC"))
               || sc.statusText(0, 2, true).contains(QStringLiteral("Ten komputer")),
               "Stage 3D.1: Scope 2 with recursive=true");
        verify(sc.statusText(0, 2, false).contains(QStringLiteral("This PC"))
               || sc.statusText(0, 2, false).contains(QStringLiteral("Ten komputer")),
               "Stage 3D.1: Scope 2 with recursive=false");

        // 3. PaneSearchState recursive lifecycle and synchronization
        PaneSearchState pss;
        verify(pss.recursive == true, "Stage 3D.1: PaneSearchState default recursive is true");
        const SearchQuery defaultSq = pss.toQuery();
        verify(defaultSq.recursive() == true, "Stage 3D.1: toQuery propagates default recursive=true");

        // Non-recursive URL loading
        QUrl nonRecursiveUrl(QStringLiteral("thispcsearch:/?search=test&scope=0&recursive=0"));
        pss.loadLocation(nonRecursiveUrl);
        verify(pss.recursive == false, "Stage 3D.1: loadLocation sets recursive=false from recursive=0 URL");
        const SearchQuery nonRecSq = pss.toQuery();
        verify(nonRecSq.recursive() == false, "Stage 3D.1: toQuery propagates recursive=false");

        // Location change resets recursive to true
        pss.loadLocation(QUrl::fromLocalFile(QStringLiteral("/home/user")));
        verify(pss.recursive == true, "Stage 3D.1: Navigating to standard directory resets recursive to true");

        // Recursive URL loading
        QUrl recursiveUrl(QStringLiteral("thispcsearch:/?search=test&scope=0&recursive=1"));
        pss.loadLocation(recursiveUrl);
        verify(pss.recursive == true, "Stage 3D.1: loadLocation sets recursive=true from recursive=1 URL");

        // Legacy search URL loading (implicit recursive=true)
        QUrl legacySearchUrl(QStringLiteral("thispcsearch:/?search=test&scope=0"));
        pss.loadLocation(legacySearchUrl);
        verify(pss.recursive == true, "Stage 3D.1: loadLocation with legacy URL defaults recursive to true");

        // 4. SearchUiController::updateStatus forwarding recursive
        QLineEdit edit;
        QLabel status;
        SearchUiController::Widgets widgets;
        widgets.edit = &edit;
        widgets.statusLabel = &status;
        SearchUiController::Presentation pres;
        pres.polish = [] { return false; };
        SearchUiController ui(widgets, pres);

        ui.updateStatus(sc, 0, 0, true);
        verify(status.text().contains(QStringLiteral("Current folder and subfolders"))
               || status.text().contains(QStringLiteral("Bieżący folder i podfoldery")),
               "Stage 3D.1: SearchUiController forwards recursive=true to status text");
        ui.updateStatus(sc, 0, 0, false);
        verify(status.text().contains(QStringLiteral("Current folder only"))
               || status.text().contains(QStringLiteral("Tylko bieżący folder")),
               "Stage 3D.1: SearchUiController forwards recursive=false to status text");

        // 5. Stage 3D.1 Auto / Text / Glob Search Mode
        // 10. Przełączenie trybu w popoverze: Auto -> Tekst -> Glob -> powrót do Auto
        SearchPopover popover;
        QToolButton *modeBtn = popover.nameModeButton();
        verify(modeBtn != nullptr, "Stage 3D.1: SearchPopover contains nameModeButton");

        QMenu modeMenu(modeBtn);
        QActionGroup modeGroup(&modeMenu);
        modeGroup.setExclusive(true);
        QAction *actAuto = modeGroup.addAction(QStringLiteral("Auto"));
        actAuto->setCheckable(true);
        actAuto->setData(static_cast<int>(SearchQuery::NameMode::Auto));
        QAction *actSub = modeGroup.addAction(QStringLiteral("Text"));
        actSub->setCheckable(true);
        actSub->setData(static_cast<int>(SearchQuery::NameMode::Substring));
        QAction *actGlob = modeGroup.addAction(QStringLiteral("Glob"));
        actGlob->setCheckable(true);
        actGlob->setData(static_cast<int>(SearchQuery::NameMode::Glob));

        PaneSearchState primaryState;
        primaryState.nameMode = SearchQuery::NameMode::Auto;

        SearchUiController::Widgets modeWidgets;
        modeWidgets.nameModeButton = modeBtn;
        modeWidgets.nameModeGroup = &modeGroup;
        SearchUiController::Presentation modePres;
        modePres.polish = [] { return true; };
        modePres.translate = [](const char *pl, const char *) { return QString::fromUtf8(pl); };
        SearchUiController modeUi(modeWidgets, modePres);

        SearchUiController::Context modeCtx;
        modeCtx.location = QUrl::fromLocalFile(QStringLiteral("/home/test"));
        modeCtx.state = &primaryState;

        // Verify initial Auto state
        modeUi.updateControls(modeCtx);
        verify(actAuto->isChecked(), "Stage 3D.1: Auto action is initially checked");
        verify(modeBtn->text().contains(QStringLiteral("Auto")), "Stage 3D.1: Button text displays Auto");

        // Switch to Text
        primaryState.nameMode = SearchQuery::NameMode::Substring;
        modeUi.updateControls(modeCtx);
        verify(actSub->isChecked(), "Stage 3D.1: Text action checked after switching to Substring");
        verify(modeBtn->text().contains(QStringLiteral("Tekst")), "Stage 3D.1: Button text displays Tekst");

        // Switch to Glob
        primaryState.nameMode = SearchQuery::NameMode::Glob;
        modeUi.updateControls(modeCtx);
        verify(actGlob->isChecked(), "Stage 3D.1: Glob action checked after switching to Glob");
        verify(modeBtn->text().contains(QStringLiteral("Maska glob")), "Stage 3D.1: Button text displays Maska glob");

        // Return to Auto
        primaryState.nameMode = SearchQuery::NameMode::Auto;
        modeUi.updateControls(modeCtx);
        verify(actAuto->isChecked(), "Stage 3D.1: Auto action checked after returning to Auto");

        // 11. Aktualizacja etykiety i tooltipa w zależności od obecności wildcarda
        primaryState.text = QStringLiteral("plain");
        modeUi.updateControls(modeCtx);
        verify(modeBtn->toolTip().contains(QStringLiteral("tekstowe")),
               "Stage 3D.1: Auto tooltip indicates text matching for plain query");

        primaryState.text = QStringLiteral("*.png");
        modeUi.updateControls(modeCtx);
        verify(modeBtn->toolTip().contains(QStringLiteral("maski glob")),
               "Stage 3D.1: Auto tooltip indicates glob pattern matching for query with wildcard");

        // 12 & 13. Stery w oknie zaawansowanym: odczyt i zapis Auto / Tekst / Glob oraz zastosowanie w SearchQuery
        SearchQuery testQuery;
        testQuery.setNameMode(SearchQuery::NameMode::Auto);
        AdvancedSearchFiltersDialog advDlg(testQuery);
        verify(advDlg.nameMode() == SearchQuery::NameMode::Auto, "Stage 3D.1: Dialog initializes with Auto mode");

        // Switch to Substring in dialog
        SearchQuery qSetSub;
        qSetSub.setNameMode(SearchQuery::NameMode::Substring);
        advDlg.setQuery(qSetSub);
        verify(advDlg.nameMode() == SearchQuery::NameMode::Substring, "Stage 3D.1: Dialog switches to Substring mode");
        SearchQuery appliedSub = advDlg.configuredQuery();
        verify(appliedSub.nameMode() == SearchQuery::NameMode::Substring, "Stage 3D.1: configuredQuery reflects Substring");

        // Switch to Glob in dialog
        SearchQuery qSetGlob;
        qSetGlob.setNameMode(SearchQuery::NameMode::Glob);
        advDlg.setQuery(qSetGlob);
        verify(advDlg.nameMode() == SearchQuery::NameMode::Glob, "Stage 3D.1: Dialog switches to Glob mode");
        SearchQuery appliedGlob = advDlg.configuredQuery();
        verify(appliedGlob.nameMode() == SearchQuery::NameMode::Glob, "Stage 3D.1: configuredQuery reflects Glob");

        // 14. Wyczyść w oknie zaawansowanym -> powrót do Auto
        QMetaObject::invokeMethod(&advDlg, "resetToDefaults");
        verify(advDlg.nameMode() == SearchQuery::NameMode::Auto, "Stage 3D.1: resetToDefaults restores Auto mode");
        verify(advDlg.configuredQuery().nameMode() == SearchQuery::NameMode::Auto, "Stage 3D.1: configuredQuery after reset is Auto");

        // 15. Wyczyść filtry w menu filtrów -> powrót do Auto
        PaneSearchState filterMenuState;
        filterMenuState.nameMode = SearchQuery::NameMode::Glob;
        filterMenuState.type = 2;
        // Simulate clearFilters action lambda
        filterMenuState.type = 0;
        filterMenuState.nameMode = SearchQuery::NameMode::Auto;
        verify(filterMenuState.nameMode == SearchQuery::NameMode::Auto, "Stage 3D.1: Clear filters action restores Auto mode");

        // 16. Niezależność wyboru trybu między Primary i Split pane
        PaneSearchState splitState;
        splitState.nameMode = SearchQuery::NameMode::Glob;
        primaryState.nameMode = SearchQuery::NameMode::Substring;
        verify(splitState.nameMode == SearchQuery::NameMode::Glob, "Stage 3D.1: Split state retains Glob");
        verify(primaryState.nameMode == SearchQuery::NameMode::Substring, "Stage 3D.1: Primary state retains Substring independently");
    }

    // =========================================================================
    // Stage 3D.2: Compact Search Bar & Responsive Popover (Variant A)
    // =========================================================================
    {
        window.setActivePane(Pane::Primary);
        window.navigateTo(rootA, true);
        verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 4; }, 5000),
               "Stage 3D.2: Primary directory loaded for Variant A tests");

        // Ensure standard window geometry (>= 900 px wide)
        window.resize(1040, 680);
        QApplication::processEvents();
        window.m_searchPopover->hidePopover();

        // 1. Constant search field width during focus, click, typing, clearing, and popover toggle
        const int initialEditWidth = window.m_searchEdit->width();
        verify(initialEditWidth >= 300 && initialEditWidth <= 340,
               "Stage 3D.2 (1): Search edit has compact width (300-340 px) in standard window");
        verify(window.m_searchPopover->stableWidth() > initialEditWidth,
               "Stage 3D.2 (1): Popover stable width is wider than compact search edit");

        auto *navToolbar = window.findChild<QToolBar *>(QStringLiteral("navigationToolbar"));
        verify(navToolbar != nullptr, "Stage 3D.2 (1): Navigation toolbar exists");
        QWidget *refreshBtnWidget = navToolbar->widgetForAction(window.m_refreshAction);
        const int refreshXBefore = refreshBtnWidget ? refreshBtnWidget->x() : 0;

        // Click search edit -> opens popover without changing edit width or shifting toolbar buttons
        QTest::mouseClick(window.m_searchEdit, Qt::LeftButton);
        verify(window.m_searchPopover->isVisible(),
               "Stage 3D.2 (2): Clicking search edit opens popover");
        verify(window.m_searchEdit->width() == initialEditWidth,
               "Stage 3D.2 (1): Search edit width remains unchanged on click/focus and popover open");
        if (refreshBtnWidget) {
            verify(refreshBtnWidget->x() == refreshXBefore,
                   "Stage 3D.2 (1): Adjacent toolbar controls do not shift when popover opens");
        }

        // Typing short, wildcard, and very long queries does not change search edit width or popover width
        const int popWidthOpen = window.m_searchPopover->width();
        window.m_searchEdit->setText(QStringLiteral("short"));
        verify(window.m_searchEdit->width() == initialEditWidth && window.m_searchPopover->width() == popWidthOpen,
               "Stage 3D.2 (1): Typing short text preserves edit and popover width");
        window.m_searchEdit->setText(QStringLiteral("*.tar.gz"));
        verify(window.m_searchEdit->width() == initialEditWidth && window.m_searchPopover->width() == popWidthOpen,
               "Stage 3D.2 (1): Typing wildcard pattern preserves edit and popover width");
        window.m_searchEdit->setText(QStringLiteral("very_long_search_query_that_exceeds_the_compact_box_width_1234567890"));
        verify(window.m_searchEdit->width() == initialEditWidth && window.m_searchPopover->width() == popWidthOpen,
               "Stage 3D.2 (1): Typing long text preserves edit and popover width");
        window.m_searchEdit->clear();
        verify(window.m_searchEdit->width() == initialEditWidth,
               "Stage 3D.2 (1): Clearing text preserves edit width");

        // 2 & 5. Escape, outside click, Alt+Down, leading icon, and Enter behavior
        window.m_searchEdit->setText(QStringLiteral("needle"));
        verify(window.m_searchPopover->isVisible(), "Stage 3D.2 (5): Popover visible before Escape");
        QTest::keyClick(window.m_searchEdit, Qt::Key_Escape);
        verify(!window.m_searchPopover->isVisible(),
               "Stage 3D.2 (5): Escape closes popover");
        verify(window.m_searchEdit->text() == QStringLiteral("needle"),
               "Stage 3D.2 (5): Escape preserves current search text");

        // Reopen via Alt+Down keyboard shortcut
        QTest::keyClick(window.m_searchEdit, Qt::Key_Down, Qt::AltModifier);
        verify(window.m_searchPopover->isVisible(),
               "Stage 3D.2 (2): Alt+Down reopens popover from search edit");

        // Close by clicking outside (e.g. on primary directory list viewport)
        QTest::mouseClick(window.m_directoryList->viewport(), Qt::LeftButton);
        verify(!window.m_searchPopover->isVisible(),
               "Stage 3D.2 (2): Clicking outside closes popover predictably");

        // Reopen via leading search icon action
        window.m_searchScopeAction->trigger();
        verify(window.m_searchPopover->isVisible(),
               "Stage 3D.2 (2): Triggering leading search icon opens popover");

        // 4. Cursor movement between search edit and popover + opening submenus does not close popover
        QEvent leaveEdit(QEvent::Leave);
        QApplication::sendEvent(window.m_searchEdit, &leaveEdit);
        QEvent enterPopover(QEvent::Enter);
        QApplication::sendEvent(window.m_searchPopover, &enterPopover);
        verify(window.m_searchPopover->isVisible(),
               "Stage 3D.2 (4): Cursor transition from search edit to popover does not close popover");

        // Submenus exist in Filter menu (Type, Date, Size) and clicks inside QMenu do not close popover
        QMenu *fMenu = window.m_searchFilterButton->menu();
        verify(fMenu != nullptr && fMenu->actions().size() >= 5,
               "Stage 3D.2 (4): Filter menu retains quick submenus, Advanced Filters, and Clear Filters");
        QMenu *subMenuType = fMenu->actions().at(0)->menu();
        QMenu *subMenuDate = fMenu->actions().at(1)->menu();
        QMenu *subMenuSize = fMenu->actions().at(2)->menu();
        verify(subMenuType != nullptr && subMenuDate != nullptr && subMenuSize != nullptr,
               "Stage 3D.2 (4): Quick filter submenus (Type, Date, Size) are intact");

        // Simulate MouseButtonPress on a submenu widget while popover is open
        QMouseEvent menuPress(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(subMenuType, &menuPress);
        verify(window.m_searchPopover->isVisible(),
               "Stage 3D.2 (4): Interacting with Type submenu does not close SearchPopover");
        QApplication::sendEvent(subMenuDate, &menuPress);
        verify(window.m_searchPopover->isVisible(),
               "Stage 3D.2 (4): Interacting with Date submenu does not close SearchPopover");
        QApplication::sendEvent(subMenuSize, &menuPress);
        verify(window.m_searchPopover->isVisible(),
               "Stage 3D.2 (4): Interacting with Size submenu does not close SearchPopover");

        // 8. Switching Auto / Tekst / Maska glob in popover does not change popover or button width
        const int modeBtnW = window.m_searchNameModeButton->width();
        const int scopeBtnW = window.m_searchScopeButton->width();
        const int filterBtnW = window.m_searchFilterButton->width();
        const int totalPopW = window.m_searchPopover->width();
        verify(window.m_searchScopeButton->height() == window.m_searchNameModeButton->height()
               && window.m_searchNameModeButton->height() == window.m_searchFilterButton->height(),
               "Stage 3D.2 (8): All three popover buttons share identical height in one horizontal row");
        verify(window.m_searchScopeButton->y() == window.m_searchNameModeButton->y()
               && window.m_searchNameModeButton->y() == window.m_searchFilterButton->y(),
               "Stage 3D.2 (8): Scope, Mode, and Filter buttons are aligned in a single horizontal row");

        for (int modeVal : {static_cast<int>(SearchQuery::NameMode::Substring),
                            static_cast<int>(SearchQuery::NameMode::Glob),
                            static_cast<int>(SearchQuery::NameMode::Auto)}) {
            choose(window.m_searchNameModeGroup, modeVal);
            verify(window.m_searchNameModeButton->width() == modeBtnW
                   && window.m_searchScopeButton->width() == scopeBtnW
                   && window.m_searchFilterButton->width() == filterBtnW
                   && window.m_searchPopover->width() == totalPopW,
                   "Stage 3D.2 (8): Switching Auto/Text/Glob never changes popover or button widths");
        }

        // 6 & 7. Filter counter badge + Quick Filters <-> Advanced Filters sync
        choose(window.m_searchTypeGroup, 2); // Images
        choose(window.m_searchDateGroup, 2); // Last 7 days
        verify(window.m_searchFilterButton->text().contains(QStringLiteral("(2)")),
               "Stage 3D.2 (6): Filter button displays active filter count (2)");
        verify(window.m_searchFilterIndicatorAction->isVisible(),
               "Stage 3D.2 (6): Trailing filter indicator badge in search edit is visible");
        verify(window.m_searchPopover->width() == totalPopW,
               "Stage 3D.2 (6): Activating filter badge count does not change popover width");

        // Opening Advanced Filters hides popover and preserves active criteria
        window.showAdvancedFiltersDialog();
        verify(!window.m_searchPopover->isVisible(),
               "Stage 3D.2 (7): Opening Advanced Filters cleanly hides floating popover");
        verify(window.m_advancedFiltersDialog != nullptr
               && window.m_advancedFiltersDialog->categories() == QList<SearchQuery::Category>{SearchQuery::Category::Images},
               "Stage 3D.2 (7): Advanced Filters dialog preserves criteria selected in quick menu");
        window.m_advancedFiltersDialog->reject();

        // Clear filters for next checks
        triggerClearFilters(Pane::Primary);
        verify(!window.m_searchFilterIndicatorAction->isVisible(),
               "Stage 3D.2 (6): Clearing filters hides trailing filter badge");

        // 3. Geometry, anchoring & responsiveness across wide, standard, narrow windows, and Split View
        window.m_searchPopover->showUnder(window.m_searchEdit);
        verify(!window.m_searchPopover->isCompactMode(),
               "Stage 3D.2 (3): Standard window uses full stable popover width (not compact mode)");
        verify(window.m_searchPopover->x() >= 8
               && window.m_searchPopover->x() + window.m_searchPopover->width() <= window.width() - 8,
               "Stage 3D.2 (3): Popover stays strictly within window horizontal margins in standard window");

        // Equal & unequal Split View pane widths: popover never overflows window and never alters splitter sizes
        window.setSplitViewEnabled(true);
        window.m_contentSplitter->setSizes({500, 500});
        QApplication::processEvents();
        const QList<int> equalSizesBefore = window.m_contentSplitter->sizes();
        window.m_searchPopover->showUnder(window.m_searchEdit);
        verify(window.m_contentSplitter->sizes() == equalSizesBefore,
               "Stage 3D.2 (3): Showing popover in equal Split View does not alter splitter pane widths");
        verify(window.m_searchPopover->x() >= 8
               && window.m_searchPopover->x() + window.m_searchPopover->width() <= window.width() - 8,
               "Stage 3D.2 (3): Popover stays within bounds in equal Split View");

        window.m_contentSplitter->setSizes({350, 690});
        QApplication::processEvents();
        const QList<int> unequalSizesBefore = window.m_contentSplitter->sizes();
        window.m_searchPopover->reposition();
        verify(window.m_contentSplitter->sizes() == unequalSizesBefore,
               "Stage 3D.2 (3): Repositioning popover in unequal Split View does not alter splitter pane widths");

        // Narrow container responsiveness (compact fallback test on standalone resizable parent)
        {
            QWidget responsiveHost;
            responsiveHost.resize(900, 400);
            responsiveHost.show();
            QLineEdit hostEdit(&responsiveHost);
            hostEdit.move(560, 12);
            SearchPopover hostPopover(&responsiveHost);
            hostPopover.scopeButton()->setText(QStringLiteral("Bieżący folder i podfoldery"));
            hostPopover.scopeButton()->setToolTip(QStringLiteral("Zakres wyszukiwania: Bieżący folder i podfoldery"));
            hostPopover.nameModeButton()->setText(QStringLiteral("Tryb: Maska glob"));
            hostPopover.nameModeButton()->setToolTip(QStringLiteral("Tryb dopasowania: Maska glob (*, ?)"));
            hostPopover.filterButton()->setText(QStringLiteral("Filtry (3)"));
            hostPopover.filterButton()->setToolTip(QStringLiteral("Filtry wyszukiwania (3 aktywne)"));
            hostPopover.attachAnchor(&hostEdit);
            hostPopover.showUnder(&hostEdit);

            // At 900 px wide: full stable width, no compact mode
            verify(!hostPopover.isCompactMode(), "Stage 3D.2 (3): Host at 900 px is in full width mode");
            verify(hostEdit.width() == SearchPopover::kDefaultAnchorWidth,
                   "Stage 3D.2 (3): Host edit has target compact width in wide host");
            verify(hostPopover.x() >= 8 && hostPopover.x() + hostPopover.width() <= responsiveHost.width() - 8,
                   "Stage 3D.2 (3): Popover within bounds at 900 px");
            verify(hostPopover.visibleButtonText(hostPopover.scopeButton()) == hostPopover.scopeButton()->text(),
                   "Stage 3D.2 (3): Full scope button text visible when not in compact mode");

            // Shrink host narrower than popover stableWidth (e.g. 380 px)
            const int narrowWidth = std::min(380, hostPopover.stableWidth() - 80);
            responsiveHost.resize(narrowWidth, 400);
            hostEdit.move(std::max(8, narrowWidth - hostEdit.width() - 8), 12);
            hostPopover.reposition();

            verify(hostPopover.isCompactMode(),
                   "Stage 3D.2 (3): Narrow host activates compact fallback mode");
            verify(hostPopover.width() <= responsiveHost.width() - 16,
                   "Stage 3D.2 (3): Compact popover width fits strictly within narrow host margins");
            verify(hostPopover.x() >= 8 && hostPopover.x() + hostPopover.width() <= responsiveHost.width() - 8,
                   "Stage 3D.2 (3): Compact popover never overflows narrow host left or right edges");

            // Full logical text and tooltips are preserved even when visible label is elided
            verify(hostPopover.scopeButton()->text() == QStringLiteral("Bieżący folder i podfoldery"),
                   "Stage 3D.2 (3): Full logical scope text preserved in compact mode");
            verify(hostPopover.scopeButton()->toolTip().contains(QStringLiteral("Bieżący folder i podfoldery")),
                   "Stage 3D.2 (3): Full scope tooltip preserved in compact mode");
            verify(hostPopover.nameModeButton()->toolTip().contains(QStringLiteral("Maska glob")),
                   "Stage 3D.2 (3): Full mode tooltip preserved in compact mode");
            verify(hostPopover.filterButton()->toolTip().contains(QStringLiteral("3 aktywne")),
                   "Stage 3D.2 (3): Full filter tooltip preserved in compact mode");

            // Switching modes in compact mode keeps compact popover width invariant
            const int compactPopW = hostPopover.width();
            const int compactModeBtnW = hostPopover.nameModeButton()->width();
            hostPopover.nameModeButton()->setText(QStringLiteral("Tryb: Auto"));
            hostPopover.reposition();
            verify(hostPopover.width() == compactPopW && hostPopover.nameModeButton()->width() == compactModeBtnW,
                   "Stage 3D.2 (3): Switching mode in compact mode preserves button and popover width");
        }

        // 5, 9 & 10. Enter closes popover and runs search; Primary/Split isolation and history preserved
        window.setActivePane(Pane::Primary);
        window.m_primarySearch.nameMode = SearchQuery::NameMode::Auto;
        window.m_splitPane->setCurrentUrl(rootB);
        window.m_splitPane->searchState().nameMode = SearchQuery::NameMode::Glob;
        verify(QTest::qWaitFor([&] { return window.m_splitPane->listView()->count() == 1; }, 5000),
               "Stage 3D.2 (9): Split pane loaded rootB");

        window.m_searchPopover->showUnder(window.m_searchEdit);
        window.m_searchEdit->setText(QStringLiteral("*.txt"));
        QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
        verify(!window.m_searchPopover->isVisible(),
               "Stage 3D.2 (5): Pressing Enter closes popover");
        verify(window.m_searchEdit->text() == QStringLiteral("*.txt"),
               "Stage 3D.2 (5): Pressing Enter leaves query text intact");
        verify(window.m_searchEdit->width() == initialEditWidth,
               "Stage 3D.2 (1): Pressing Enter and starting search keeps search edit width constant");
        waitForSearch(*window.m_searchController);
        verify(window.m_directoryList->count() == 3,
               "Stage 3D.2 (10): Auto wildcard search '*.txt' returns 3 txt files in rootA");
        verify(window.m_splitPane->currentUrl() == rootB
               && window.m_splitPane->searchState().nameMode == SearchQuery::NameMode::Glob,
               "Stage 3D.2 (9): Primary search execution does not alter Split pane URL or mode");

        // =====================================================================
        // Stage 3D.3: Responsive Layout & HiDPI Scaling (12 priority areas)
        // =====================================================================
        {
            // 1. HiDPI scaling verification across 1.0, 1.25, 1.5, 2.0:
            //    (a) Real subprocess verification with QT_SCALE_FACTOR=1.0, 1.25, 1.5, 2.0
            for (const char *scaleStr : {"1.0", "1.25", "1.5", "2.0"}) {
                QProcess proc;
                QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
                env.insert(QStringLiteral("QT_SCALE_FACTOR"), QString::fromLatin1(scaleStr));
                env.insert(QStringLiteral("THISPC_SEARCH_SCALE_CHECK"), QStringLiteral("1"));
                proc.setProcessEnvironment(env);
                proc.start(QCoreApplication::applicationFilePath(), {QStringLiteral("search")});
                verify(proc.waitForFinished(15000) && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
                       "Stage 3D.3 (1a): SearchPopover and anchor geometry valid under real QT_SCALE_FACTOR subprocess");
            }

            //    (b) Dynamic font / DPI scaling (1.0x, 1.25x, 1.5x, 2.0x) via changeEvent / FontChange / DevicePixelRatioChange
            QWidget dpiHost;
            dpiHost.resize(1280, 720);
            dpiHost.show();
            QLineEdit dpiEdit(&dpiHost);
            dpiEdit.move(800, 12);
            SearchPopover dpiPopover(&dpiHost);
            dpiPopover.scopeButton()->setText(QStringLiteral("Bieżący folder i podfoldery"));
            dpiPopover.nameModeButton()->setText(QStringLiteral("Tryb: Maska glob"));
            dpiPopover.filterButton()->setText(QStringLiteral("Filtry (2)"));
            dpiPopover.attachAnchor(&dpiEdit);
            dpiPopover.showUnder(&dpiEdit);

            const QFont baseFont = dpiPopover.font();
            const int basePixelSize = baseFont.pixelSize() > 0
                ? baseFont.pixelSize()
                : std::max(10, QFontMetrics(baseFont).height() - 4);

            int prevStableW = 0;
            int prevBtnH = 0;
            for (double factor : {1.0, 1.25, 1.5, 2.0}) {
                QFont scaledFont = baseFont;
                scaledFont.setPixelSize(std::max(10, static_cast<int>(std::lround(basePixelSize * factor))));
                dpiPopover.setFont(scaledFont);
                QEvent dprEvent(QEvent::DevicePixelRatioChange);
                QApplication::sendEvent(&dpiPopover, &dprEvent);
                QApplication::processEvents();

                verify(dpiPopover.stableWidth() >= prevStableW,
                       "Stage 3D.3 (1b): Popover stableWidth scales monotonically with HiDPI font factor");
                verify(dpiPopover.buttonHeight() >= prevBtnH && dpiPopover.buttonHeight() >= 32,
                       "Stage 3D.3 (1b): Popover buttonHeight scales monotonically and stays >= 32 px");
                verify(dpiPopover.scopeButton()->height() == dpiPopover.nameModeButton()->height()
                       && dpiPopover.nameModeButton()->height() == dpiPopover.filterButton()->height(),
                       "Stage 3D.3 (1b): All 3 buttons keep identical height at scale factor");
                verify(dpiPopover.scopeButton()->geometry().right() < dpiPopover.nameModeButton()->geometry().left()
                       && dpiPopover.nameModeButton()->geometry().right() < dpiPopover.filterButton()->geometry().left(),
                       "Stage 3D.3 (1b): Popover buttons never overlap at scale factor");
                verify(dpiPopover.visibleButtonText(dpiPopover.scopeButton()) == dpiPopover.scopeButton()->text(),
                       "Stage 3D.3 (1b): Full scope text remains un-truncated in wide host at scale factor");
                prevStableW = dpiPopover.stableWidth();
                prevBtnH = dpiPopover.buttonHeight();
            }

            // 2. Search edit width stability across window resizes
            for (int w : {900, 1040, 1280, 1600, 1040}) {
                window.resize(w, 680);
                QApplication::processEvents();
                verify(window.m_searchEdit->width() == SearchPopover::kDefaultAnchorWidth,
                       "Stage 3D.3 (2): Search edit keeps constant compact width (320 px) across standard/wide window sizes");
            }

            // 3, 4, 7 & 8. Left/right boundary clamping, very narrow windows, elision + tooltip preservation, and full restoration
            QWidget clampHost;
            clampHost.resize(1000, 480);
            clampHost.show();
            QLineEdit clampEdit(&clampHost);
            SearchPopover clampPopover(&clampHost);
            clampPopover.scopeButton()->setText(QStringLiteral("Bieżący folder i podfoldery"));
            clampPopover.scopeButton()->setToolTip(QStringLiteral("Zakres wyszukiwania: Bieżący folder i podfoldery"));
            clampPopover.nameModeButton()->setText(QStringLiteral("Tryb: Maska glob"));
            clampPopover.nameModeButton()->setToolTip(QStringLiteral("Tryb dopasowania: Maska glob (*, ?)"));
            clampPopover.filterButton()->setText(QStringLiteral("Filtry (4)"));
            clampPopover.filterButton()->setToolTip(QStringLiteral("Filtry wyszukiwania (4 aktywne)"));
            clampPopover.attachAnchor(&clampEdit);

            // (3a) Anchor near left edge of window
            clampEdit.move(8, 12);
            clampPopover.showUnder(&clampEdit);
            verify(clampPopover.x() == 8
                   && clampPopover.x() + clampPopover.width() <= clampHost.width() - 8,
                   "Stage 3D.3 (3a): Popover aligns cleanly near left window edge without negative overflow");

            // (3b) Anchor near extreme right edge of window
            clampEdit.move(clampHost.width() - clampEdit.width() - 4, 12);
            clampPopover.reposition();
            verify(clampPopover.x() >= 8
                   && clampPopover.x() + clampPopover.width() <= clampHost.width() - 8,
                   "Stage 3D.3 (3b): Popover clamps within 8 px of right window edge when anchor is at right boundary");

            // (4 & 8) Very narrow window (e.g. 320 px and ultra-narrow 190 px)
            for (int narrowW : {340, 260, 190}) {
                clampHost.resize(narrowW, 480);
                clampEdit.move(8, 12);
                QApplication::processEvents();
                clampPopover.reposition();
                verify(clampPopover.isCompactMode(),
                       "Stage 3D.3 (4): Popover enters compact mode in very narrow window");
                verify(clampPopover.width() <= narrowW - 16,
                       "Stage 3D.3 (4): Popover width never exceeds narrow parent available width");
                verify(clampPopover.x() >= 8 && clampPopover.x() + clampPopover.width() <= narrowW - 8,
                       "Stage 3D.3 (4): Popover stays within [8, narrowW - 8] in very narrow window");
                verify(clampPopover.scopeButton()->y() == clampPopover.nameModeButton()->y()
                       && clampPopover.nameModeButton()->y() == clampPopover.filterButton()->y(),
                       "Stage 3D.3 (4): All three buttons remain in a single horizontal row in very narrow window");
                verify(clampPopover.scopeButton()->geometry().right() < clampPopover.nameModeButton()->geometry().left()
                       && clampPopover.nameModeButton()->geometry().right() < clampPopover.filterButton()->geometry().left(),
                       "Stage 3D.3 (4): Buttons do not overlap in very narrow window");
                // (8) Elision + tooltip preservation
                const QString elidedScope = clampPopover.visibleButtonText(clampPopover.scopeButton());
                verify(elidedScope != clampPopover.scopeButton()->text() && elidedScope.contains(QChar(0x2026)),
                       "Stage 3D.3 (8): Visible scope label is cleanly elided with ellipsis in very narrow window");
                verify(clampPopover.scopeButton()->text() == QStringLiteral("Bieżący folder i podfoldery")
                       && clampPopover.scopeButton()->toolTip().contains(QStringLiteral("Bieżący folder i podfoldery"))
                       && clampPopover.nameModeButton()->toolTip().contains(QStringLiteral("Maska glob"))
                       && clampPopover.filterButton()->toolTip().contains(QStringLiteral("4 aktywne")),
                       "Stage 3D.3 (8): Full logical texts and tooltips are preserved during compact elision");
            }

            // (9) Opening submenus in constrained/narrow space does not close SearchPopover
            clampHost.resize(320, 480);
            clampPopover.reposition();
            verify(clampPopover.isVisible() && clampPopover.isCompactMode(),
                   "Stage 3D.3 (9): Popover is visible in constrained 320 px space before submenu interaction");
            QMouseEvent constrainedMenuPress(QEvent::MouseButtonPress, QPointF(4, 4), QPointF(4, 4),
                                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(subMenuType, &constrainedMenuPress);
            QApplication::sendEvent(subMenuDate, &constrainedMenuPress);
            QApplication::sendEvent(subMenuSize, &constrainedMenuPress);
            verify(clampPopover.isVisible(),
                   "Stage 3D.3 (9): Interacting with quick filter submenus in narrow window keeps SearchPopover open");

            // (7) Full label restoration after widening window back to standard width
            clampHost.resize(1000, 480);
            clampEdit.move(660, 12);
            QApplication::processEvents();
            clampPopover.reposition();
            verify(!clampPopover.isCompactMode() && clampPopover.width() == clampPopover.stableWidth(),
                   "Stage 3D.3 (7): Widening window exits compact mode and restores full stable popover width");
            verify(clampEdit.width() == SearchPopover::kDefaultAnchorWidth,
                   "Stage 3D.3 (7): Widening window restores search edit width to kDefaultAnchorWidth (320 px)");
            verify(clampPopover.visibleButtonText(clampPopover.scopeButton()) == clampPopover.scopeButton()->text()
                   && clampPopover.visibleButtonText(clampPopover.nameModeButton()) == clampPopover.nameModeButton()->text()
                   && clampPopover.visibleButtonText(clampPopover.filterButton()) == clampPopover.filterButton()->text(),
                   "Stage 3D.3 (7): Widening window restores full un-elided button labels");

            // (11) Rapid multi-step resize stress cycles without geometry corruption
            for (int rapidW : {1280, 360, 920, 280, 1440, 410, 220, 1040}) {
                clampHost.resize(rapidW, 480);
                clampEdit.move(std::max(8, rapidW - clampEdit.width() - 12), 12);
                QApplication::processEvents();
                clampPopover.reposition();
                verify(clampPopover.x() >= 8 && clampPopover.x() + clampPopover.width() <= rapidW - 8,
                       "Stage 3D.3 (11): Rapid resize cycle preserves horizontal clamping bounds");
                verify(clampPopover.height() == clampPopover.popoverHeight(),
                       "Stage 3D.3 (11): Rapid resize cycle preserves popover height");
            }
            verify(!clampPopover.isCompactMode() && clampPopover.width() == clampPopover.stableWidth(),
                   "Stage 3D.3 (11): Rapid resize cycle ends at 1040 px with exact full stable geometry");

            // (5, 6 & 10) Split View resize, toggle ON/OFF, active pane switching while SearchPopover is open,
            //             swapSplitPanes(), and preservation of search state/sorting/view/history
            window.setSplitViewEnabled(true);
            window.navigateTo(rootA, true);
            verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 4; }, 5000),
                   "Stage 3D.3 (6): Primary loaded rootA before pane-switch popover sync test");
            window.m_splitPane->setCurrentUrl(rootB, true);
            verify(QTest::qWaitFor([&] { return window.m_splitPane->listView()->count() == 1; }, 5000),
                   "Stage 3D.3 (6): Split loaded rootB before pane-switch popover sync test");

            window.setActivePane(Pane::Primary);
            window.m_primarySearch.location = window.m_navigation.currentUrl();
            window.m_primarySearch.scope = 0; // Current folder and subfolders
            window.m_primarySearch.recursive = true;
            window.m_primarySearch.nameMode = SearchQuery::NameMode::Auto;
            window.m_primarySearch.type = 2; // Images
            window.m_primarySearch.categories = {SearchQuery::Category::Images};
            window.m_primarySearch.text = QStringLiteral("*.txt");

            window.m_splitPane->searchState().location = window.m_splitPane->currentUrl();
            window.m_splitPane->searchState().scope = 2; // This PC
            window.m_splitPane->searchState().nameMode = SearchQuery::NameMode::Substring;
            window.m_splitPane->searchState().type = 0;
            window.m_splitPane->searchState().categories.clear();
            window.m_splitPane->searchState().text = QStringLiteral("needle-second");

            window.updateSearchControls();
            window.m_searchPopover->showUnder(window.m_searchEdit);
            const int openPopoverStableW = window.m_searchPopover->width();
            verify(window.m_searchPopover->isVisible(),
                   "Stage 3D.3 (6): SearchPopover is open on Primary pane");
            verify((window.m_searchScopeButton->text().contains(QStringLiteral("podfoldery"))
                    || window.m_searchScopeButton->text().contains(QStringLiteral("subfolders")))
                   && window.m_searchNameModeButton->text().contains(QStringLiteral("Auto"))
                   && window.m_searchFilterButton->text().contains(QStringLiteral("(1)")),
                   "Stage 3D.3 (6): Open SearchPopover reflects Primary pane Scope, Mode, and Filter badge (1)");

            // Switch active pane to Split while SearchPopover remains open
            window.setActivePane(Pane::Split);
            verify(window.m_searchPopover->isVisible(),
                   "Stage 3D.3 (6): Switching active pane to Split keeps SearchPopover open");
            verify(window.m_searchPopover->width() == openPopoverStableW,
                   "Stage 3D.3 (6): Switching active pane to Split does not change SearchPopover width");
            verify((window.m_searchScopeButton->text().contains(QStringLiteral("Ten komputer"))
                    || window.m_searchScopeButton->text().contains(QStringLiteral("This PC")))
                   && (window.m_searchNameModeButton->text().contains(QStringLiteral("Tekst"))
                       || window.m_searchNameModeButton->text().contains(QStringLiteral("Text")))
                   && !window.m_searchFilterButton->text().contains(QStringLiteral("("))
                   && window.m_searchEdit->text() == QStringLiteral("needle-second"),
                   "Stage 3D.3 (6): Open SearchPopover immediately updates Scope, Mode, Filter badge, and query text for Split pane");

            // Switch back to Primary while SearchPopover remains open
            window.setActivePane(Pane::Primary);
            verify((window.m_searchScopeButton->text().contains(QStringLiteral("podfoldery"))
                    || window.m_searchScopeButton->text().contains(QStringLiteral("subfolders")))
                   && window.m_searchNameModeButton->text().contains(QStringLiteral("Auto"))
                   && window.m_searchFilterButton->text().contains(QStringLiteral("(1)"))
                   && window.m_searchEdit->text() == QStringLiteral("*.txt"),
                   "Stage 3D.3 (6): Switching back to Primary restores Primary controls in open SearchPopover");

            // Swap Split panes while Split is active and verify shared search controls sync
            window.setActivePane(Pane::Split);
            window.swapSplitPanes();
            verify(window.m_navigation.currentUrl() == rootB && window.m_splitPane->currentUrl() == rootA,
                   "Stage 3D.3 (5): swapSplitPanes swaps Primary and Split URLs");
            verify(window.m_searchPopover->width() == openPopoverStableW,
                   "Stage 3D.3 (5): swapSplitPanes preserves SearchPopover stable width");

            // Toggle Split View OFF and ON without losing Primary search state or corrupting popover geometry
            window.setSplitViewEnabled(false);
            verify(window.m_activePane == Pane::Primary,
                   "Stage 3D.3 (5): Disabling Split View returns active pane to Primary");
            window.m_searchPopover->reposition();
            verify(window.m_searchPopover->x() >= 8
                   && window.m_searchPopover->x() + window.m_searchPopover->width() <= window.width() - 8,
                   "Stage 3D.3 (5): Popover stays within bounds after disabling Split View");
            window.setSplitViewEnabled(true);
            window.m_searchPopover->hidePopover();

            // (12) Lifecycle & eventFilter safety: destroying transient SearchPopover and anchor leaves no dangling filter
            {
                auto *transientEdit = new QLineEdit(&clampHost);
                auto *transientPopover = new SearchPopover(&clampHost);
                transientPopover->attachAnchor(transientEdit);
                transientPopover->showUnder(transientEdit);
                verify(transientPopover->isVisible(), "Stage 3D.3 (12): Transient popover shown");
                delete transientPopover;
                delete transientEdit;
            }
            clampHost.resize(820, 480);
            QApplication::processEvents();
            verify(true, "Stage 3D.3 (12): No dangling pointers, timers, or event filter crashes after transient popover teardown");

            // =================================================================
            // Stage 3D.3.2: Split View Swap Search State Fix
            // =================================================================
            {
                const QString pobraneDir = files.filePath(QStringLiteral("Pobrane"));
                const QString dokumentyDir = files.filePath(QStringLiteral("Dokumenty"));
                verify(QDir().mkpath(pobraneDir) && QDir().mkpath(dokumentyDir),
                       "Stage 3D.3.2: Created Pobrane and Dokumenty test folders");
                for (const QString &fn : {pobraneDir + QStringLiteral("/icon.png"),
                                          pobraneDir + QStringLiteral("/literal-*.png.txt"),
                                          dokumentyDir + QStringLiteral("/diagram.png"),
                                          dokumentyDir + QStringLiteral("/notes.txt")}) {
                    QFile f(fn);
                    verify(f.open(QIODevice::WriteOnly), "Stage 3D.3.2: Created test file in Pobrane/Dokumenty");
                    f.write("swap test\n");
                }
                const QUrl pobraneUrl = QUrl::fromLocalFile(pobraneDir);
                const QUrl dokumentyUrl = QUrl::fromLocalFile(dokumentyDir);

                window.setSplitViewEnabled(true);
                window.navigateTo(pobraneUrl, true);
                verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 2; }, 5000),
                       "Stage 3D.3.2: Left pane loaded Pobrane");
                window.m_splitPane->setCurrentUrl(dokumentyUrl, true);
                verify(QTest::qWaitFor([&] { return window.m_splitPane->listView()->count() == 2; }, 5000),
                       "Stage 3D.3.2: Right pane loaded Dokumenty");

                // Initial setup:
                // - Left (Primary): Pobrane, NameMode = Tekst (Substring), Scope = 0 (Current folder only, recursive=false), Filter = Images (2)
                window.setActivePane(Pane::Primary);
                choose(window.m_searchNameModeGroup, static_cast<int>(SearchQuery::NameMode::Substring));
                window.m_primarySearch.scope = 0;
                window.m_primarySearch.recursive = false;
                choose(window.m_searchTypeGroup, 2); // Images
                window.m_searchEdit->setText(QStringLiteral("draft-left"));
                window.updateSearchControls();

                verify(window.m_primarySearch.nameMode == SearchQuery::NameMode::Substring,
                       "Stage 3D.3.2: Initial Left pane (Pobrane) has NameMode::Substring (Tekst)");
                verify(window.m_searchNameModeButton->text().contains(QStringLiteral("Tekst"))
                       || window.m_searchNameModeButton->text().contains(QStringLiteral("Text")),
                       "Stage 3D.3.2: Initial Left pane UI button shows Tekst/Text");

                // - Right (Split): Dokumenty, NameMode = Auto, Scope = 2 (This PC), no filters
                window.setActivePane(Pane::Split);
                choose(window.m_searchNameModeGroup, static_cast<int>(SearchQuery::NameMode::Auto));
                choose(window.m_searchScopeGroup, 2); // This PC
                choose(window.m_searchTypeGroup, 0); // All
                window.m_searchEdit->setText(QStringLiteral("draft-right"));
                window.updateSearchControls();

                verify(window.m_splitPane->searchState().nameMode == SearchQuery::NameMode::Auto,
                       "Stage 3D.3.2: Initial Right pane (Dokumenty) has NameMode::Auto");
                verify(window.m_searchNameModeButton->text().contains(QStringLiteral("Auto")),
                       "Stage 3D.3.2: Initial Right pane UI button shows Auto");

                // Perform 1st Swap ("Zamień panele") while Right (Split) is active
                window.swapSplitPanes();
                verify(!window.m_searchController->isRunning()
                       && !window.m_splitPane->searchController()->isRunning(),
                       "Stage 3D.3.2: swapSplitPanes does not start unnecessary KIO searches");

                // Verify URLs and search states swapped:
                // - Left (Primary) now has Dokumenty + Auto + Scope 2 + no filters + "draft-right"
                // - Right (Split) now has Pobrane + Tekst (Substring) + Scope 0 (recursive=false) + Images (2) + "draft-left"
                verify(sameLocation(window.m_navigation.currentUrl(), dokumentyUrl),
                       "Stage 3D.3.2: After 1st swap, Left pane URL is Dokumenty");
                verify(sameLocation(window.m_splitPane->currentUrl(), pobraneUrl),
                       "Stage 3D.3.2: After 1st swap, Right pane URL is Pobrane");
                verify(window.m_primarySearch.nameMode == SearchQuery::NameMode::Auto
                       && window.m_primarySearch.scope == 2
                       && window.m_primarySearch.type == 0
                       && window.m_primarySearch.text == QStringLiteral("draft-right"),
                       "Stage 3D.3.2: After 1st swap, Left pane state holds Dokumenty's Auto mode, Scope 2, and draft query");
                verify(window.m_splitPane->searchState().nameMode == SearchQuery::NameMode::Substring
                       && window.m_splitPane->searchState().scope == 0
                       && !window.m_splitPane->searchState().recursive
                       && window.m_splitPane->searchState().type == 2
                       && window.m_splitPane->searchState().categories == QList<SearchQuery::Category>{SearchQuery::Category::Images}
                       && window.m_splitPane->searchState().text == QStringLiteral("draft-left"),
                       "Stage 3D.3.2: After 1st swap, Right pane state holds Pobrane's Tekst mode, Scope 0 (non-recursive), Images filter, and draft query");

                // Right (Split) is currently active — verify UI controls immediately show Pobrane's Tekst mode & filters!
                verify((window.m_searchNameModeButton->text().contains(QStringLiteral("Tekst"))
                        || window.m_searchNameModeButton->text().contains(QStringLiteral("Text")))
                       && window.m_searchNameModeGroup->checkedAction() != nullptr
                       && window.m_searchNameModeGroup->checkedAction()->data().toInt()
                           == static_cast<int>(SearchQuery::NameMode::Substring),
                       "Stage 3D.3.2: Right pane UI button and checked action show Tekst immediately after 1st swap");
                verify((window.m_searchScopeButton->text().contains(QStringLiteral("Tylko bieżący folder"))
                        || window.m_searchScopeButton->text().contains(QStringLiteral("Current folder only")))
                       && window.m_searchFilterButton->text().contains(QStringLiteral("(1)"))
                       && window.m_searchEdit->text() == QStringLiteral("draft-left"),
                       "Stage 3D.3.2: Right pane UI reflects Pobrane's non-recursive scope, (1) filter badge, and draft text");

                // Activate Left (Primary) and verify UI controls show Dokumenty's Auto mode
                window.setActivePane(Pane::Primary);
                verify(window.m_searchNameModeButton->text().contains(QStringLiteral("Auto"))
                       && window.m_searchNameModeGroup->checkedAction() != nullptr
                       && window.m_searchNameModeGroup->checkedAction()->data().toInt()
                           == static_cast<int>(SearchQuery::NameMode::Auto)
                       && (window.m_searchScopeButton->text().contains(QStringLiteral("Ten komputer"))
                           || window.m_searchScopeButton->text().contains(QStringLiteral("This PC")))
                       && !window.m_searchFilterButton->text().contains(QStringLiteral("("))
                       && window.m_searchEdit->text() == QStringLiteral("draft-right"),
                       "Stage 3D.3.2: Activating Left pane after 1st swap shows Dokumenty's Auto mode, This PC scope, and no filters");

                // Activate Right (Split) again and verify Tekst was not overwritten by Auto
                window.setActivePane(Pane::Split);
                verify((window.m_searchNameModeButton->text().contains(QStringLiteral("Tekst"))
                        || window.m_searchNameModeButton->text().contains(QStringLiteral("Text")))
                       && window.m_splitPane->searchState().nameMode == SearchQuery::NameMode::Substring,
                       "Stage 3D.3.2: Re-activating Right pane preserves Tekst mode without overwriting by Auto");

                // Perform 2nd Swap (double swap -> return to original sides)
                window.swapSplitPanes();
                verify(sameLocation(window.m_navigation.currentUrl(), pobraneUrl)
                       && sameLocation(window.m_splitPane->currentUrl(), dokumentyUrl),
                       "Stage 3D.3.2: After 2nd swap, Left is back at Pobrane and Right is back at Dokumenty");
                verify(window.m_primarySearch.nameMode == SearchQuery::NameMode::Substring
                       && window.m_primarySearch.scope == 0
                       && !window.m_primarySearch.recursive
                       && window.m_primarySearch.type == 2
                       && window.m_primarySearch.text == QStringLiteral("draft-left"),
                       "Stage 3D.3.2: After 2nd swap, Left pane (Pobrane) restored Tekst, Scope 0, Images filter, and draft-left");
                verify(window.m_splitPane->searchState().nameMode == SearchQuery::NameMode::Auto
                       && window.m_splitPane->searchState().scope == 2
                       && window.m_splitPane->searchState().type == 0
                       && window.m_splitPane->searchState().text == QStringLiteral("draft-right"),
                       "Stage 3D.3.2: After 2nd swap, Right pane (Dokumenty) restored Auto, Scope 2, and draft-right");

                // Verify '*.png' execution after swapping panes (no regression in Auto/Tekst/Glob):
                // Reset filters to All and Scope to 0 (recursive) on both panes, keep Left=Tekst (Pobrane), Right=Auto (Dokumenty), then swap!
                window.setActivePane(Pane::Primary);
                triggerClearFilters(Pane::Primary);
                window.m_searchEdit->clear();
                choose(window.m_searchScopeGroup, 0);
                window.m_primarySearch.recursive = true;
                choose(window.m_searchNameModeGroup, static_cast<int>(SearchQuery::NameMode::Substring)); // Tekst

                window.setActivePane(Pane::Split);
                triggerClearFilters(Pane::Split);
                window.m_searchEdit->clear();
                choose(window.m_searchScopeGroup, 0);
                window.m_splitPane->searchState().recursive = true;
                choose(window.m_searchNameModeGroup, static_cast<int>(SearchQuery::NameMode::Auto)); // Auto

                // Swap panes so Left = Dokumenty (Auto) and Right = Pobrane (Tekst)
                window.swapSplitPanes();
                verify(QTest::qWaitFor([&] {
                    return window.m_directoryList->count() == 2 && window.m_splitPane->listView()->count() == 2;
                }, 5000), "Stage 3D.3.2: Both swapped folders listed before '*.png' search check");

                // 1) In Right pane (Pobrane, Tekst mode), searching '*.png' matches literal substring '*.png' (literal-*.png.txt), NOT icon.png!
                window.setActivePane(Pane::Split);
                window.m_searchEdit->setText(QStringLiteral("*.png"));
                window.startSearchFromUi();
                waitForSearch(*window.m_splitPane->searchController());
                verify(window.m_splitPane->listView()->count() == 1
                       && window.m_splitPane->listView()->item(0).data().toString() == QStringLiteral("literal-*.png.txt"),
                       "Stage 3D.3.2: After swap, '*.png' in Right pane (Pobrane, Tekst mode) matches only literal-*.png.txt");

                // 2) In Left pane (Dokumenty, Auto mode), searching '*.png' auto-detects wildcard glob and matches diagram.png!
                window.setActivePane(Pane::Primary);
                window.m_searchEdit->setText(QStringLiteral("*.png"));
                window.startSearchFromUi();
                waitForSearch(*window.m_searchController);
                verify(window.m_directoryList->count() == 1
                       && window.m_directoryList->item(0).data().toString() == QStringLiteral("diagram.png"),
                       "Stage 3D.3.2: After swap, '*.png' in Left pane (Dokumenty, Auto mode) matches diagram.png via glob");

                // =============================================================
                // Stage 3D.4: Keyboard, Accessibility & Final Search UI Acceptance
                // =============================================================
                {
                    // Return both panes to ordinary folders for clean keyboard/focus testing
                    window.setActivePane(Pane::Primary);
                    window.navigateTo(pobraneUrl, true);
                    verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 2; }, 5000),
                           "Stage 3D.4: Left pane loaded Pobrane for keyboard audit");
                    window.m_splitPane->setCurrentUrl(dokumentyUrl, true);
                    verify(QTest::qWaitFor([&] { return window.m_splitPane->listView()->count() == 2; }, 5000),
                           "Stage 3D.4: Right pane loaded Dokumenty for keyboard audit");

                    window.resize(1040, 680);
                    window.activateWindow();
                    QApplication::processEvents();

                    // 1. Ctrl+F focus, selectAll, typing, repeated Ctrl+F, and constant 320 px width
                    window.setActivePane(Pane::Primary);
                    window.m_searchPopover->hidePopover();
                    window.m_searchEdit->setText(QStringLiteral("stara_fraza"));
                    window.m_directoryList->setFocus();
                    QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
                    verify(window.m_searchEdit->hasFocus(),
                           "Stage 3D.4 (1): Ctrl+F focuses search edit");
                    verify(window.m_searchEdit->selectedText() == QStringLiteral("stara_fraza"),
                           "Stage 3D.4 (1): Ctrl+F selects entire existing query text");
                    verify(window.m_searchPopover->isVisible(),
                           "Stage 3D.4 (1): Ctrl+F opens SearchPopover");
                    verify(window.m_searchEdit->width() == SearchPopover::kDefaultAnchorWidth,
                           "Stage 3D.4 (1): Search edit keeps constant 320 px width on Ctrl+F");

                    // Typing replaces selected text
                    QTest::keyClicks(window.m_searchEdit, QStringLiteral("icon"));
                    verify(window.m_searchEdit->text() == QStringLiteral("icon"),
                           "Stage 3D.4 (1): Typing after Ctrl+F replaces selected text with new query");

                    // Repeated Ctrl+F while already focused re-selects text and keeps popover open
                    QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
                    verify(window.m_searchEdit->hasFocus()
                           && window.m_searchEdit->selectedText() == QStringLiteral("icon")
                           && window.m_searchPopover->isVisible(),
                           "Stage 3D.4 (1): Repeated Ctrl+F preserves focus, selects all text, and keeps popover open");

                    // 2. Tab / Shift+Tab / Arrow keys navigation & opening/closing menus purely by keyboard
                    QTest::keyClick(window.m_searchEdit, Qt::Key_Tab);
                    verify(window.m_searchScopeButton->hasFocus(),
                           "Stage 3D.4 (2): Tab from search edit moves focus to Scope button");
                    QTest::keyClick(window.m_searchScopeButton, Qt::Key_Tab);
                    verify(window.m_searchNameModeButton->hasFocus(),
                           "Stage 3D.4 (2): Tab from Scope button moves focus to NameMode button");
                    QTest::keyClick(window.m_searchNameModeButton, Qt::Key_Tab);
                    verify(window.m_searchFilterButton->hasFocus(),
                           "Stage 3D.4 (2): Tab from NameMode button moves focus to Filter button");

                    // Shift+Tab backwards traversal
                    QTest::keyClick(window.m_searchFilterButton, Qt::Key_Tab, Qt::ShiftModifier);
                    verify(window.m_searchNameModeButton->hasFocus(),
                           "Stage 3D.4 (2): Shift+Tab from Filter button moves focus back to NameMode button");
                    QTest::keyClick(window.m_searchNameModeButton, Qt::Key_Tab, Qt::ShiftModifier);
                    verify(window.m_searchScopeButton->hasFocus(),
                           "Stage 3D.4 (2): Shift+Tab from NameMode button moves focus back to Scope button");
                    QTest::keyClick(window.m_searchScopeButton, Qt::Key_Tab, Qt::ShiftModifier);
                    verify(window.m_searchEdit->hasFocus(),
                           "Stage 3D.4 (2): Shift+Tab from Scope button returns focus to search edit");

                    // Down / Right / Left / Up arrow navigation between search edit and popover buttons
                    QTest::keyClick(window.m_searchEdit, Qt::Key_Down);
                    verify(window.m_searchScopeButton->hasFocus(),
                           "Stage 3D.4 (2): Down arrow in search edit focuses first enabled popover button (Scope)");
                    QTest::keyClick(window.m_searchScopeButton, Qt::Key_Right);
                    verify(window.m_searchNameModeButton->hasFocus(),
                           "Stage 3D.4 (2): Right arrow moves focus from Scope to NameMode button");
                    QTest::keyClick(window.m_searchNameModeButton, Qt::Key_Right);
                    verify(window.m_searchFilterButton->hasFocus(),
                           "Stage 3D.4 (2): Right arrow moves focus from NameMode to Filter button");
                    QTest::keyClick(window.m_searchFilterButton, Qt::Key_Left);
                    verify(window.m_searchNameModeButton->hasFocus(),
                           "Stage 3D.4 (2): Left arrow moves focus from Filter to NameMode button");
                    QTest::keyClick(window.m_searchNameModeButton, Qt::Key_Up);
                    verify(window.m_searchEdit->hasFocus(),
                           "Stage 3D.4 (2): Up arrow from popover button returns focus to search edit");

                    // Open NameMode menu via keyboard (Space) and select Tekst via Space, then reopen and select Auto via Enter
                    window.m_searchNameModeButton->setFocus();
                    QTest::keyClick(window.m_searchNameModeButton, Qt::Key_Space);
                    verify(window.m_searchNameModeMenu->isVisible(),
                           "Stage 3D.4 (2): Pressing Space on focused NameMode button opens NameMode QMenu");
                    // Navigate down to Tekst (Substring) and activate with Space
                    for (QAction *act : window.m_searchNameModeMenu->actions()) {
                        if (act->isCheckable() && act->data().toInt() == static_cast<int>(SearchQuery::NameMode::Substring)) {
                            window.m_searchNameModeMenu->setActiveAction(act);
                            break;
                        }
                    }
                    QTest::keyClick(window.m_searchNameModeMenu, Qt::Key_Space);
                    verify(!window.m_searchNameModeMenu->isVisible(),
                           "Stage 3D.4 (2): Pressing Space on active QMenu item closes the menu");
                    verify(window.m_primarySearch.nameMode == SearchQuery::NameMode::Substring
                           && (window.m_searchNameModeButton->text().contains(QStringLiteral("Tekst"))
                               || window.m_searchNameModeButton->text().contains(QStringLiteral("Text"))),
                           "Stage 3D.4 (2): Space in NameMode QMenu activates Tekst mode");
                    verify(window.m_searchEdit->hasFocus(),
                           "Stage 3D.4 (2): Selecting an option in NameMode menu returns focus to search edit");

                    // Reopen NameMode menu via Enter and select Auto via Enter
                    window.m_searchNameModeButton->setFocus();
                    QTest::keyClick(window.m_searchNameModeButton, Qt::Key_Return);
                    verify(window.m_searchNameModeMenu->isVisible(),
                           "Stage 3D.4 (2): Pressing Enter on focused NameMode button opens NameMode QMenu");
                    for (QAction *act : window.m_searchNameModeMenu->actions()) {
                        if (act->isCheckable() && act->data().toInt() == static_cast<int>(SearchQuery::NameMode::Auto)) {
                            window.m_searchNameModeMenu->setActiveAction(act);
                            break;
                        }
                    }
                    QTest::keyClick(window.m_searchNameModeMenu, Qt::Key_Return);
                    verify(!window.m_searchNameModeMenu->isVisible()
                           && window.m_primarySearch.nameMode == SearchQuery::NameMode::Auto,
                           "Stage 3D.4 (2): Pressing Enter on active QMenu item selects Auto mode and closes menu");

                    // 3. Escape across all 5 distinct hierarchy levels:
                    //    Level 1: Open submenu (typeMenu) -> closes only submenu, parent filterMenu & SearchPopover stay open
                    //    Level 2: Open parent menu (filterMenu) -> closes only parent menu, SearchPopover stays open & focus returns to Filter button
                    //    Level 3: Open SearchPopover -> closes SearchPopover, focus returns to search edit, query & filters preserved
                    //    Level 4: Search edit (popover closed) -> preserves query text & filters
                    //    Level 5: Running search -> Escape closes popover without cancelling running search
                    choose(window.m_searchTypeGroup, 2); // Set 1 active filter (Images) to verify Escape never clears filters
                    window.m_searchEdit->setText(QStringLiteral("*.png"));
                    window.m_searchPopover->showUnder(window.m_searchEdit);
                    window.m_searchFilterButton->setFocus();
                    QTest::keyClick(window.m_searchFilterButton, Qt::Key_Space);
                    QMenu *filterMenu = window.m_searchFilterButton->menu();
                    verify(filterMenu != nullptr && filterMenu->isVisible(),
                           "Stage 3D.4 (3): Filter menu opened via keyboard before multi-level Escape test");
                    QAction *typeSubMenuAction = filterMenu->actions().at(0);
                    QMenu *typeSubMenu = typeSubMenuAction ? typeSubMenuAction->menu() : nullptr;
                    verify(typeSubMenu != nullptr, "Stage 3D.4 (3): Type submenu exists");
                    filterMenu->setActiveAction(typeSubMenuAction);
                    typeSubMenu->popup(filterMenu->mapToGlobal(QPoint(20, 10)));
                    verify(typeSubMenu->isVisible() && filterMenu->isVisible() && window.m_searchPopover->isVisible(),
                           "Stage 3D.4 (3): Submenu, parent menu, and SearchPopover are all visible");

                    // Level 1 Escape: close submenu only
                    QTest::keyClick(typeSubMenu, Qt::Key_Escape);
                    verify(!typeSubMenu->isVisible() && filterMenu->isVisible() && window.m_searchPopover->isVisible(),
                           "Stage 3D.4 (3 - Level 1): Escape on open submenu closes only submenu, keeping parent menu and SearchPopover open");

                    // Level 2 Escape: close parent menu only
                    QTest::keyClick(filterMenu, Qt::Key_Escape);
                    verify(!filterMenu->isVisible() && window.m_searchPopover->isVisible(),
                           "Stage 3D.4 (3 - Level 2): Escape on parent QMenu closes only parent menu, keeping SearchPopover open");
                    verify(window.m_searchFilterButton->hasFocus(),
                           "Stage 3D.4 (3 - Level 2): Closing parent QMenu with Escape restores focus to Filter button");

                    // Level 3 Escape: close SearchPopover from focused popover button
                    QTest::keyClick(window.m_searchFilterButton, Qt::Key_Escape);
                    verify(!window.m_searchPopover->isVisible(),
                           "Stage 3D.4 (3 - Level 3): Escape on popover button closes SearchPopover");
                    verify(window.m_searchEdit->hasFocus(),
                           "Stage 3D.4 (3 - Level 3): Closing SearchPopover with Escape returns focus to search edit");
                    verify(window.m_searchEdit->text() == QStringLiteral("*.png")
                           && window.m_primarySearch.type == 2,
                           "Stage 3D.4 (3 - Level 3): Escape on SearchPopover preserves query text and active filters");

                    // Level 4 Escape: Escape in search edit when popover is already closed
                    QTest::keyClick(window.m_searchEdit, Qt::Key_Escape);
                    verify(!window.m_searchPopover->isVisible()
                           && window.m_searchEdit->text() == QStringLiteral("*.png")
                           && window.m_primarySearch.type == 2,
                           "Stage 3D.4 (3 - Level 4): Escape in search edit when popover is closed preserves query and filters");

                    // Reopen SearchPopover with Alt+Down
                    QTest::keyClick(window.m_searchEdit, Qt::Key_Down, Qt::AltModifier);
                    verify(window.m_searchPopover->isVisible(),
                           "Stage 3D.4 (3): Alt+Down reopens SearchPopover after Escape");
                    triggerClearFilters(Pane::Primary);

                    // 4. Enter: single KIO search execution, closes popover, preserves text, never activates underlying view item
                    window.m_directoryList->setCurrentRow(0);
                    window.m_searchEdit->setFocus();
                    window.m_searchEdit->setText(QStringLiteral("*.png"));
                    window.m_searchPopover->showUnder(window.m_searchEdit);
                    const int historySizeBeforeEnter = window.m_navigation.snapshot().history.size();
                    int finishedCount = 0;
                    QMetaObject::Connection finishConn = QObject::connect(
                        window.m_searchController, &SearchController::finished,
                        &window, [&finishedCount] { ++finishedCount; });

                    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
                    verify(!window.m_searchPopover->isVisible(),
                           "Stage 3D.4 (4): Enter closes SearchPopover");
                    verify(window.m_searchEdit->text() == QStringLiteral("*.png"),
                           "Stage 3D.4 (4): Enter preserves query text in search edit");

                    // Level 5 Escape check if search is still running, or verify Escape during search location
                    if (window.m_searchController->isRunning()) {
                        window.m_searchPopover->showUnder(window.m_searchEdit);
                        QTest::keyClick(window.m_searchEdit, Qt::Key_Escape);
                        verify(!window.m_searchPopover->isVisible() && window.m_searchController->isRunning(),
                               "Stage 3D.4 (3 - Level 5): Escape closes popover without cancelling running search");
                    }
                    waitForSearch(*window.m_searchController);
                    QObject::disconnect(finishConn);
                    verify(finishedCount == 1,
                           "Stage 3D.4 (4): Enter triggers KIO search exactly once (no duplicate search execution)");
                    verify(window.m_navigation.snapshot().history.size() == historySizeBeforeEnter + 1
                           && isSearchLocation(window.m_navigation.currentUrl()),
                           "Stage 3D.4 (4): Enter pushes exactly one search location without activating underlying directory item");

                    // Also verify Level 5 Escape deterministically on window.m_searchController in-progress state
                    {
                        window.m_searchController->m_searchInProgress = true;
                        window.m_searchPopover->showUnder(window.m_searchEdit);
                        QTest::keyClick(window.m_searchEdit, Qt::Key_Escape);
                        verify(!window.m_searchPopover->isVisible() && window.m_searchController->isRunning(),
                               "Stage 3D.4 (3 - Level 5): Escape on search edit closes popover without cancelling active search");
                        window.m_searchController->m_searchInProgress = false;
                    }

                    // 5. Advanced Filters Dialog — keyboard Apply (Enter) / Cancel (Escape) & focus restoration
                    window.showAdvancedFiltersDialog();
                    verify(window.m_advancedFiltersDialog != nullptr && window.m_advancedFiltersDialog->isVisible(),
                           "Stage 3D.4 (5): AdvancedSearchFiltersDialog opened");
                    // Modify fields in dialog, then Cancel via Escape (reject)
                    window.m_advancedFiltersDialog->m_caseSensitiveCheck->setChecked(true);
                    window.m_advancedFiltersDialog->m_extensionsEdit->setText(QStringLiteral("pdf, epub"));
                    QTest::keyClick(window.m_advancedFiltersDialog, Qt::Key_Escape);
                    QApplication::processEvents();
                    verify(!window.m_advancedFiltersDialog->isVisible(),
                           "Stage 3D.4 (5): Escape closes AdvancedSearchFiltersDialog (Cancel)");
                    verify(!window.m_primarySearch.matchCase && window.m_primarySearch.extensions.isEmpty(),
                           "Stage 3D.4 (5): Cancelling AdvancedSearchFiltersDialog via Escape leaves applied criteria untouched");
                    verify(window.m_searchEdit->hasFocus()
                           || (window.activateWindow(), QApplication::processEvents(), window.m_searchEdit->hasFocus()),
                           "Stage 3D.4 (5): Closing AdvancedSearchFiltersDialog via Escape restores focus to search edit");

                    // Reopen AdvancedSearchFiltersDialog, configure Tekst + Images, and Apply via Enter
                    window.showAdvancedFiltersDialog();
                    window.m_advancedFiltersDialog->resetToDefaults();
                    const int subIdx = window.m_advancedFiltersDialog->m_nameModeCombo->findData(
                        static_cast<int>(SearchQuery::NameMode::Substring));
                    window.m_advancedFiltersDialog->m_nameModeCombo->setCurrentIndex(subIdx);
                    window.m_advancedFiltersDialog->m_catImagesCheck->setChecked(true);
                    QTest::keyClick(window.m_advancedFiltersDialog->m_applyButton, Qt::Key_Return);
                    QApplication::processEvents();
                    verify(!window.m_advancedFiltersDialog->isVisible(),
                           "Stage 3D.4 (5): Pressing Enter on Apply button accepts and closes AdvancedSearchFiltersDialog");
                    verify(window.m_primarySearch.nameMode == SearchQuery::NameMode::Substring
                           && window.m_primarySearch.categories == QList<SearchQuery::Category>{SearchQuery::Category::Images}
                           && window.m_searchFilterButton->text().contains(QStringLiteral("(1)")),
                           "Stage 3D.4 (5): Applying AdvancedSearchFiltersDialog updates active pane state and Filtry (1) badge");
                    verify(window.m_searchEdit->hasFocus()
                           || (window.activateWindow(), QApplication::processEvents(), window.m_searchEdit->hasFocus()),
                           "Stage 3D.4 (5): Accepting AdvancedSearchFiltersDialog restores focus to search edit");
                    triggerClearFilters(Pane::Primary);

                    // 6 & 7. Stage 3D.4.3 Primary/Split keyboard & navigation scenario:
                    //    - Left (Pobrane): '*.png', Tekst mode, filter Images (2), Details view (2), sort by Size (2) descending
                    //    - Right (Dokumenty): '*.png', Auto mode, no filter (0), Icons view (0), sort by Name (0) ascending
                    window.setActivePane(Pane::Primary);
                    window.navigateTo(pobraneUrl, true);
                    verify(QTest::qWaitFor([&] { return window.m_directoryList->count() == 2; }, 5000),
                           "Stage 3D.4 (6): Left pane back at Pobrane");
                    window.setDirectoryViewMode(2); // Details
                    window.setSortKey(2); // Size
                    window.setSortAscending(false);
                    choose(window.m_searchNameModeGroup, static_cast<int>(SearchQuery::NameMode::Substring)); // Tekst
                    choose(window.m_searchTypeGroup, 0); // All types so literal-*.png.txt matches
                    window.m_searchEdit->setText(QStringLiteral("*.png"));
                    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
                    waitForSearch(*window.m_searchController);
                    verify(window.m_directoryDetails->topLevelItemCount() == 1
                           && window.m_directoryDetails->topLevelItem(0)->text(0) == QStringLiteral("literal-*.png.txt"),
                           "Stage 3D.4 (6): Left pane Enter executes '*.png' in Tekst mode (matches literal-*.png.txt)");

                    // Switch to Right pane via F6 keyboard shortcut
                    QTest::keyClick(&window, Qt::Key_F6);
                    verify(window.m_activePane == Pane::Split,
                           "Stage 3D.4 (6): F6 shortcut switches active pane from Primary to Split");
                    window.m_splitPane->setViewMode(0); // Icons
                    window.m_splitPane->setSortState(0, true); // Name ascending
                    choose(window.m_searchNameModeGroup, static_cast<int>(SearchQuery::NameMode::Auto)); // Auto
                    choose(window.m_searchTypeGroup, 2); // Images
                    QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
                    verify(window.m_searchEdit->hasFocus() && window.m_activePane == Pane::Split,
                           "Stage 3D.4 (6): Ctrl+F focuses search edit for active Split pane");
                    window.m_searchEdit->setText(QStringLiteral("*.png"));
                    QTest::keyClick(window.m_searchEdit, Qt::Key_Return);
                    waitForSearch(*window.m_splitPane->searchController());
                    verify(window.m_splitPane->listView()->count() == 1
                           && window.m_splitPane->listView()->item(0).data().toString() == QStringLiteral("diagram.png"),
                           "Stage 3D.4 (6): Right pane Enter executes '*.png' in Auto mode with Images filter (matches diagram.png)");

                    // Verify switching panes updates SearchPopover without starting extra KIO jobs
                    window.m_searchPopover->showUnder(window.m_searchEdit);
                    QTest::keyClick(&window, Qt::Key_F6);
                    verify(window.m_activePane == Pane::Primary
                           && !window.m_searchController->isRunning()
                           && !window.m_splitPane->searchController()->isRunning(),
                           "Stage 3D.4 (6): F6 back to Primary does not trigger redundant KIO jobs");
                    verify((window.m_searchNameModeButton->text().contains(QStringLiteral("Tekst"))
                            || window.m_searchNameModeButton->text().contains(QStringLiteral("Text")))
                           && !window.m_searchFilterButton->text().contains(QStringLiteral("(")),
                           "Stage 3D.4 (6): SearchPopover reflects Primary Tekst mode and 0 filters");

                    QTest::keyClick(&window, Qt::Key_F6);
                    verify(window.m_activePane == Pane::Split
                           && window.m_searchNameModeButton->text().contains(QStringLiteral("Auto"))
                           && window.m_searchFilterButton->text().contains(QStringLiteral("(1)")),
                           "Stage 3D.4 (6): SearchPopover reflects Split Auto mode and (1) filter");
                    window.m_searchPopover->hidePopover();

                    // Back / Forward in Split pane restores folder and then search results + filters
                    window.setActivePane(Pane::Split);
                    window.m_backAction->trigger();
                    verify(QTest::qWaitFor([&] {
                        return sameLocation(window.m_splitPane->currentUrl(), dokumentyUrl)
                            && window.m_splitPane->listView()->count() == 2;
                    }, 5000), "Stage 3D.4 (6): Back in Split pane returns from search results to Dokumenty folder");
                    window.m_forwardAction->trigger();
                    waitForSearch(*window.m_splitPane->searchController());
                    verify(isSearchLocation(window.m_splitPane->currentUrl())
                           && window.m_splitPane->listView()->count() == 1
                           && window.m_splitPane->searchState().type == 2,
                           "Stage 3D.4 (6): Forward in Split pane restores search results and Images filter");

                    // Swap panes while both hold search results, then verify Swap back
                    window.swapSplitPanes();
                    waitForSearch(*window.m_searchController);
                    waitForSearch(*window.m_splitPane->searchController());
                    verify(window.m_primarySearch.nameMode == SearchQuery::NameMode::Auto
                           && window.m_splitPane->searchState().nameMode == SearchQuery::NameMode::Substring,
                           "Stage 3D.4 (7): Swapping search result panes swaps Auto and Tekst modes cleanly");
                    window.swapSplitPanes();
                    waitForSearch(*window.m_searchController);
                    waitForSearch(*window.m_splitPane->searchController());
                    verify(window.m_primarySearch.nameMode == SearchQuery::NameMode::Substring
                           && window.m_splitPane->searchState().nameMode == SearchQuery::NameMode::Auto,
                           "Stage 3D.4 (7): Swapping back restores original search modes to each pane");

                    // 8 & 9. Accessibility audit: focusPolicy, accessibleName, accessibleDescription, and :focus QSS
                    verify(window.m_searchEdit->focusPolicy() == Qt::StrongFocus
                           && window.m_searchScopeButton->focusPolicy() == Qt::StrongFocus
                           && window.m_searchNameModeButton->focusPolicy() == Qt::StrongFocus
                           && window.m_searchFilterButton->focusPolicy() == Qt::StrongFocus
                           && window.m_cancelSearchButton->focusPolicy() == Qt::StrongFocus,
                           "Stage 3D.4 (9): All interactive search controls use Qt::StrongFocus");
                    verify(!window.m_searchEdit->accessibleName().isEmpty()
                           && !window.m_searchEdit->accessibleDescription().isEmpty()
                           && !window.m_searchPopover->accessibleName().isEmpty()
                           && !window.m_searchScopeButton->accessibleName().isEmpty()
                           && !window.m_searchNameModeButton->accessibleName().isEmpty()
                           && !window.m_searchFilterButton->accessibleName().isEmpty()
                           && !window.m_searchProgressBar->accessibleName().isEmpty()
                           && !window.m_cancelSearchButton->accessibleName().isEmpty()
                           && !window.m_splitPane->m_searchProgressBar->accessibleName().isEmpty()
                           && !window.m_splitPane->m_cancelSearchButton->accessibleName().isEmpty(),
                           "Stage 3D.4 (9): Search edit, popover, buttons, progress bars, and stop buttons have non-empty accessibleName");
                    verify(!window.m_advancedFiltersDialog->accessibleName().isEmpty()
                           && !window.m_advancedFiltersDialog->m_nameModeCombo->accessibleName().isEmpty()
                           && !window.m_advancedFiltersDialog->m_extensionsEdit->accessibleName().isEmpty()
                           && !window.m_advancedFiltersDialog->m_mimesEdit->accessibleName().isEmpty()
                           && !window.m_advancedFiltersDialog->m_applyButton->accessibleName().isEmpty(),
                           "Stage 3D.4 (9): AdvancedSearchFiltersDialog and its controls expose non-empty accessibleName");
                    const QString appQss = applicationStyleSheet();
                    verify(appQss.contains(QStringLiteral("QToolButton#searchScopeButton:focus"))
                           && appQss.contains(QStringLiteral("QToolButton#searchNameModeButton:focus"))
                           && appQss.contains(QStringLiteral("QToolButton#searchFilterButton:focus"))
                           && appQss.contains(QStringLiteral("QToolButton#searchFilterButton[activeFilters=\"true\"]:focus")),
                           "Stage 3D.4 (9): Application stylesheet defines explicit :focus indicator rules for all SearchPopover buttons");

                    // 10. Stage 3D.4 Final Polish: Active Filter Indicator
                    // Vector icon rendering, multi-resolution pixmaps, HiDPI scaling, clear button non-collision, and pane independence
                    verify(appQss.contains(QStringLiteral("QToolButton#searchFilterIndicatorButton")),
                           "Stage 3D.4 Polish: Application stylesheet defines rules for searchFilterIndicatorButton");

                    // Multi-DPR / multi-size pixmap generation from FilterBadgeIconEngine
                    const QIcon badgeIcon1 = window.searchFilterBadgeIcon(1);
                    const QIcon badgeIcon10 = window.searchFilterBadgeIcon(10);
                    verify(!badgeIcon1.isNull() && !badgeIcon10.isNull(),
                           "Stage 3D.4 Polish: searchFilterBadgeIcon returns non-null QIcon for single and multi-digit counts");
                    const QPixmap badgePm16 = badgeIcon1.pixmap(QSize(16, 16));
                    const QPixmap badgePm20 = badgeIcon1.pixmap(QSize(20, 20));
                    const QPixmap badgePm32 = badgeIcon1.pixmap(QSize(32, 32));
                    verify(badgePm16.width() == 16 && badgePm16.height() == 16
                           && badgePm20.width() == 20 && badgePm20.height() == 20
                           && badgePm32.width() >= 32 && badgePm32.height() >= 32,
                           "Stage 3D.4 Polish: Badge icon engine generates sharp pixmaps for 16px, 20px and 32px (HiDPI)");

                    auto findClearBtn = [&window]() -> QToolButton* {
                        for (QToolButton *btn : window.m_searchEdit->findChildren<QToolButton*>()) {
                            if (btn->defaultAction() == window.m_clearSearchAction) return btn;
                        }
                        return nullptr;
                    };
                    auto findFilterBtn = [&window]() -> QToolButton* {
                        for (QToolButton *btn : window.m_searchEdit->findChildren<QToolButton*>()) {
                            if (btn->defaultAction() == window.m_searchFilterIndicatorAction) return btn;
                        }
                        return nullptr;
                    };

                    // 1. Non-empty query without filters: Clear button ('X') visible at far right, filter indicator hidden
                    window.setActivePane(Pane::Primary);
                    triggerClearFilters(Pane::Primary);
                    window.m_searchEdit->setText(QStringLiteral("*.png"));
                    QApplication::processEvents();

                    QToolButton *clearBtn = findClearBtn();
                    QToolButton *filterIndicatorBtn = findFilterBtn();
                    verify(clearBtn != nullptr && clearBtn->isVisible(),
                           "Stage 3D.4 Polish: Clear button ('X') is visible when query text is non-empty");
                    verify(filterIndicatorBtn != nullptr && !filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Filter indicator is hidden when no filters are active");
                    verify(clearBtn->geometry().x() >= 280,
                           "Stage 3D.4 Polish: Clear button is positioned at the rightmost edge of search edit");
                    verify(clearBtn->cursor().shape() == Qt::PointingHandCursor,
                           "Stage 3D.4 Polish: Clear button uses PointingHandCursor");

                    // 2. Non-empty query with 1 filter: [ query ........ ] [ filter 1 ] [ X ]
                    choose(window.m_searchTypeGroup, 2); // Images filter
                    QApplication::processEvents();

                    verify(clearBtn->isVisible(),
                           "Stage 3D.4 Polish: Clear button remains visible with active filter and non-empty query");
                    verify(filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Filter indicator is visible when filter is active");
                    verify(window.m_searchFilterIndicatorAction->text().contains(QStringLiteral("1")),
                           "Stage 3D.4 Polish: Filter indicator text exposes active count 1");
                    verify(filterIndicatorBtn->geometry().right() < clearBtn->geometry().left(),
                           "Stage 3D.4 Polish: Expected layout order: filter indicator is positioned to the left of clear button [ filter 1 ] [ X ]");
                    verify(clearBtn->geometry().left() - filterIndicatorBtn->geometry().right() >= 2,
                           "Stage 3D.4 Polish: Positive non-zero gap between filter indicator and clear button");
                    verify(!filterIndicatorBtn->geometry().intersects(clearBtn->geometry()),
                           "Stage 3D.4 Polish: Filter indicator button and clear button do not intersect or collide");
                    verify(filterIndicatorBtn->cursor().shape() == Qt::PointingHandCursor,
                           "Stage 3D.4 Polish: Filter indicator button uses PointingHandCursor");

                    // 3. Non-empty query with multiple filters (2-digit badge): both visible and non-colliding
                    SearchQuery multiQuery = window.m_primarySearch.toQuery();
                    multiQuery.setExtensions({QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg")});
                    window.m_primarySearch.extensions = multiQuery.extensions();
                    window.updateSearchControls();
                    QApplication::processEvents();

                    verify(clearBtn->isVisible() && filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Both clear button and filter indicator visible with multiple filters");
                    verify(filterIndicatorBtn->geometry().right() < clearBtn->geometry().left(),
                           "Stage 3D.4 Polish: Multiple-filter badge stays to the left of clear button without collision");
                    verify(!filterIndicatorBtn->geometry().intersects(clearBtn->geometry()),
                           "Stage 3D.4 Polish: Multiple-filter badge and clear button do not intersect");

                    // 4. Empty query with active filter: Clear button hidden, filter indicator visible
                    window.m_searchEdit->clear();
                    QApplication::processEvents();

                    verify(!clearBtn->isVisible(),
                           "Stage 3D.4 Polish: Clear button is hidden when query text is empty");
                    verify(filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Filter indicator remains visible when query is empty");
                    verify(filterIndicatorBtn->geometry().x() >= 280,
                           "Stage 3D.4 Polish: Filter indicator shifts to the rightmost edge when clear button is hidden");

                    // 5. Clicking clear button: clears query text, keeps active filters intact, hides clear button, keeps filter indicator
                    window.m_searchEdit->setText(QStringLiteral("query_to_clear"));
                    QApplication::processEvents();
                    verify(clearBtn->isVisible() && filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Both buttons visible before clicking clear");

                    // Trigger clear action (simulating user clicking X)
                    window.m_clearSearchAction->trigger();
                    QApplication::processEvents();

                    verify(window.m_searchEdit->text().isEmpty(),
                           "Stage 3D.4 Polish: Clicking clear button empties search query text");
                    verify(!clearBtn->isVisible(),
                           "Stage 3D.4 Polish: Clear button is hidden after clearing text");
                    verify(window.m_primarySearch.type == 2 && !window.m_primarySearch.extensions.isEmpty(),
                           "Stage 3D.4 Polish: Active search filters are preserved after clicking clear button");
                    verify(filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Filter indicator remains visible after clicking clear button");

                    // 6. Clearing filters while keeping query: filter indicator hides, clear button remains visible
                    window.m_searchEdit->setText(QStringLiteral("keep_query"));
                    triggerClearFilters(Pane::Primary);
                    window.m_primarySearch.extensions.clear();
                    window.updateSearchControls();
                    QApplication::processEvents();

                    verify(!filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Filter indicator is hidden after clearing filters");
                    verify(clearBtn->isVisible(),
                           "Stage 3D.4 Polish: Clear button remains visible when query is non-empty after clearing filters");
                    verify(window.m_searchEdit->text() == QStringLiteral("keep_query"),
                           "Stage 3D.4 Polish: Search query is intact after clearing filters");

                    // 7. Primary vs Split pane independence:
                    // Primary has text and 1 filter
                    choose(window.m_searchTypeGroup, 2);
                    window.updateSearchControls();
                    verify(clearBtn->isVisible() && filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Primary pane shows both buttons");

                    // Switch to Split pane (empty query, 0 filters)
                    window.setActivePane(Pane::Split);
                    choose(window.m_searchTypeGroup, 0);
                    window.m_splitPane->searchState().text.clear();
                    window.updateSearchControls();
                    QApplication::processEvents();

                    verify(!clearBtn->isVisible() && !filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Split pane with empty query and 0 filters has neither button visible");

                    // Switch back to Primary -> both buttons restored
                    window.setActivePane(Pane::Primary);
                    window.updateSearchControls();
                    QApplication::processEvents();

                    verify(clearBtn->isVisible() && filterIndicatorBtn->isVisible(),
                           "Stage 3D.4 Polish: Switching back to Primary restores both buttons");
                    verify(filterIndicatorBtn->geometry().right() < clearBtn->geometry().left(),
                           "Stage 3D.4 Polish: Layout order [ filter 1 ] [ X ] preserved on pane switch");

                    // 8. Window resizing: no button overlap at 900 px, 1280 px, 1600 px
                    for (int w : {900, 1280, 1600, 1040}) {
                        window.resize(w, 680);
                        QApplication::processEvents();
                        verify(filterIndicatorBtn->geometry().right() < clearBtn->geometry().left(),
                               "Stage 3D.4 Polish: [ filter 1 ] [ X ] order and non-overlap preserved across window resizes");
                    }

                    // Reset Primary pane state cleanly
                    triggerClearFilters(Pane::Primary);
                    window.m_searchEdit->clear();
                    window.updateSearchControls();

                    // 11. Clean lifecycle at test end: no running KIO jobs, no open popups, popover hidden
                    window.m_searchPopover->hidePopover();
                    verify(!window.m_searchController->isRunning()
                           && !window.m_splitPane->searchController()->isRunning()
                           && QApplication::activePopupWidget() == nullptr
                           && !window.m_searchPopover->isVisible(),
                           "Stage 3D.4 (10): No lingering KIO search jobs or open popup widgets after Stage 3D.4 tests");
                }
            }
        }
    }

    qInfo("PASS: %d search assertions; real KIO workers, filters, cancellation, stale signals, split/view integration, Stage 3B.3 date & size GUI, 3B.3.10 size preset sync, Stage 3B.4 final integration, Stage 3C.1 sorting & view parity, Stage 3C.2 progress & loading states, Stage 3C.3 cancellation & retry, Stage 3C.4 final search integration & regression, Stage 3D.1 search scope & glob UX polish, Stage 3D.2 compact search bar & responsive popover, Stage 3D.3 responsive layout & HiDPI scaling, Stage 3D.3.2 split swap search state, Stage 3D.4 keyboard & accessibility", checks);
}
