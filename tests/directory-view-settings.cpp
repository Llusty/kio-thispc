#include <QSignalSpy>

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

    for (int mode = 0; mode < 4; ++mode) {
        const int extent = directory_view_detail::iconExtentForMode(mode);
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
        verify(window.m_directoryIconSizeMode == 0
                   && window.m_directoryList->iconSize() == QSize(24, 24),
               "window startup restores Very large preference without changing List geometry");
        verify(window.m_groupMode == DirectoryViewSettings::GroupByType,
               "window startup restores type grouping for the initial folder");
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
        verify(window.m_directoryIconSizeMode == 3,
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
        verify(window.m_splitPane->iconSizeMode() == 3,
               "split pane applies Small preference before session save");
        verify(window.m_splitPane->groupMode() == DirectoryViewSettings::NoGrouping,
               "split pane applies its independent grouping preference");
        window.m_splitPane->setFiles({datedUnknown, datedToday});
        window.m_splitPane->setGroupMode(DirectoryViewSettings::GroupByDate);
        verify(window.m_splitPane->groupMode() == DirectoryViewSettings::GroupByDate
                   && window.m_splitPane->listView()->isCategorized()
                   && window.m_splitPane->m_dateGroupingTimer.isActive(),
               "split Date action routes independently and schedules its own boundary refresh");
        window.m_splitPane->setGroupMode(DirectoryViewSettings::NoGrouping);
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
        verify(restored.m_directoryIconSizeMode == 0,
               "session restore reapplies the active folder icon size");
        verify(restored.m_groupMode == DirectoryViewSettings::GroupByType,
               "session restore reapplies the active folder grouping");
        verify(!restored.m_splitPane->isHidden(),
               "session restore reopens Split View");
        verify(restored.m_splitPane->viewMode() == 3
                   && restored.m_splitPane->listView()->compactMode(),
               "session restore reapplies the split Compact folder preference");
        verify(restored.m_splitPane->iconSizeMode() == 3,
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

    qInfo("PASS: %d per-folder view settings assertions", checks);
    return 0;
}
