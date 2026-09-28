#include "selectionmenucontroller.h"
#include "panemenucontroller.h"

#include <QAction>
#include <QFile>
#include <QMenu>
#include <QPointer>
#include <QTemporaryDir>

static int checks = 0;
static void verify(bool condition, const char *message)
{
    ++checks;
    if (!condition) qFatal("selection menu: %s", message);
}

static QMenu *submenu(QMenu &menu, const QString &text)
{
    for (QAction *action : menu.actions())
        if (action->text() == text) return action->menu();
    return nullptr;
}

static QAction *action(QMenu &menu, const QString &text)
{
    for (QAction *candidate : menu.actions()) {
        if (candidate->text() == text) return candidate;
        if (candidate->menu())
            if (QAction *nested = action(*candidate->menu(), text)) return nested;
    }
    return nullptr;
}

static QAction *actionById(QMenu &menu, const QString &id)
{
    for (QAction *candidate : menu.actions()) {
        if (candidate->objectName() == id) return candidate;
        if (candidate->menu())
            if (QAction *nested = actionById(*candidate->menu(), id)) return nested;
    }
    return nullptr;
}

static int actionIdCount(QMenu &menu, const QString &id)
{
    int count = 0;
    for (QAction *candidate : menu.actions()) {
        count += candidate->objectName() == id;
        if (candidate->menu()) count += actionIdCount(*candidate->menu(), id);
    }
    return count;
}

static ActionAvailability availability(const QUrl &directory,
                                       const QList<QUrl> &selection = {},
                                       bool clipboard = false)
{
    ActionStateInput input;
    input.directory = directory;
    input.selection = selection;
    input.isDirectory = true;
    input.isThisPcLocation = directory.scheme() == QStringLiteral("thispc");
    input.isSearchLocation = directory.scheme() == QStringLiteral("thispcsearch");
    input.isTrashLocation = directory.scheme() == QStringLiteral("trash");
    input.isTrashRoot = input.isTrashLocation && directory.path() == QStringLiteral("/");
    input.clipboardHasUrls = clipboard;
    input.recoverySafe = true;
    input.emptyTrashAvailable = true;
    return ActionStateController::compute(input);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QWidget parent;
    SelectionMenuController controller(&parent);
    QTemporaryDir temporary;
    verify(temporary.isValid(), "temporary directory");
    const QString filePath = temporary.filePath(QStringLiteral("note.txt"));
    QFile file(filePath);
    verify(file.open(QIODevice::WriteOnly) && file.write("text") == 4, "text fixture");
    file.close();
    const QString folderPath = temporary.filePath(QStringLiteral("folder"));
    verify(QDir().mkpath(folderPath), "folder fixture");
    const QUrl localFile = QUrl::fromLocalFile(filePath);
    const QUrl localFolder = QUrl::fromLocalFile(folderPath);
    const QUrl remote(QStringLiteral("sftp://example.test/note.txt"));

    verify(controller.allUrlsAreLocalFiles({localFile}, false), "single local file eligibility");
    verify(!controller.allUrlsAreLocalFiles({}, false), "empty selection eligibility");
    verify(!controller.allUrlsAreLocalFiles({localFolder}, false), "folder is not a plain local file");
    verify(controller.allUrlsAreLocalFiles({localFolder}, true), "folder allowed for archives");
    verify(!controller.allUrlsAreLocalFiles({remote}, true), "KIO URL is not local");
    verify(controller.selectionHasCommonParent({localFile, localFolder}), "multi selection common parent");
    verify(!controller.selectionHasCommonParent({localFile, remote}), "mixed local and KIO archive selection rejected");
    verify(controller.canPrintUrl(localFile, false), "local text is printable");
    verify(!controller.canPrintUrl(remote, false), "KIO URL is not printable");
    verify(!controller.canPrintUrl(localFolder, true), "folder is not printable");

    QMenu empty(&parent);
    controller.addOpenWithSubmenu(empty, {});
    controller.addSendToSubmenu(empty, {}, {});
    verify(empty.actions().isEmpty(), "no-selection menu adds no selection submenus");

    QMenu single(&parent);
    controller.addOpenWithSubmenu(single, {localFile});
    int archiveRoute = 0;
    controller.addSendToSubmenu(single, {localFile}, {
        [](const QList<QUrl> &, const QString &, const QString &) {},
        [&](const QList<QUrl> &) { archiveRoute = 1; },
        [&](const QList<QUrl> &) { archiveRoute = 2; },
        [&](const QList<QUrl> &) { archiveRoute = 3; }});
    verify(submenu(single, trLocal("Otwórz za pomocą", "Open with")), "Open With present for file");
    QMenu *send = submenu(single, trLocal("Wyślij do", "Send to"));
    verify(send, "Send To present for file");
    QAction *zip = action(*send, trLocal("Skompresowany plik ZIP…", "Compressed ZIP file…"));
    verify(zip && zip->isEnabled(), "archive creation enabled for local selection");
    zip->trigger();
    verify(archiveRoute == 1, "ZIP routes through supplied backend callback");

    QMenu remoteMenu(&parent);
    controller.addSendToSubmenu(remoteMenu, {remote}, {});
    QMenu *remoteSend = submenu(remoteMenu, trLocal("Wyślij do", "Send to"));
    verify(remoteSend, "Send To remains present for KIO selection");
    verify(!action(*remoteSend, trLocal("Skompresowany plik ZIP…", "Compressed ZIP file…"))->isEnabled(),
           "archive creation disabled for KIO selection");
    verify(!action(*remoteSend, trLocal("E-mail…", "E-mail…"))->isEnabled(),
           "email disabled for KIO selection");
    verify(!action(*remoteSend, trLocal("Bluetooth…", "Bluetooth…"))->isEnabled(),
           "Bluetooth disabled for KIO selection");

    int viewMode = -1, iconSize = -1, sortKey = -1, groupMode = -1;
    bool ascending = false, hidden = false, thumbnails = true;
    QAction preview(QStringLiteral("Preview"), &parent), fullNames(QStringLiteral("Full names"), &parent);
    SelectionMenuController::ViewState state{PaneId::Split, 2, 1, 3, false,
        DirectoryViewSettings::GroupByDate, false, true, &preview, &fullNames};
    SelectionMenuController::ViewCallbacks callbacks{
        [&](int value) { viewMode = value; }, [&](int value) { iconSize = value; },
        [&](int value) { sortKey = value; }, [&](bool value) { ascending = value; },
        [&](int value) { groupMode = value; }, [&](bool value) { hidden = value; },
        [&](bool value) { thumbnails = value; }};
    QMenu background(&parent);
    controller.addViewSubmenu(background, state, callbacks);
    controller.addSortSubmenu(background, state, callbacks);
    verify(submenu(background, trLocal("Widok", "View")), "View submenu present");
    verify(submenu(background, trLocal("Sortuj", "Sort")), "Sort submenu present");
    action(background, trLocal("Ikony", "Icons"))->trigger();
    action(background, trLocal("Małe", "Small"))->trigger();
    QMenu *sortMenu = submenu(background, trLocal("Sortuj", "Sort"));
    action(*sortMenu, trLocal("Nazwa", "Name"))->trigger();
    action(*sortMenu, trLocal("Rosnąco", "Ascending"))->trigger();
    QMenu *groupMenu = submenu(*sortMenu, trLocal("Grupuj według", "Group by"));
    action(*groupMenu, trLocal("Rozmiar", "Size"))->trigger();
    action(background, trLocal("Ukryte elementy", "Hidden items"))->trigger();
    action(background, trLocal("Miniatury obrazów", "Image thumbnails"))->trigger();
    verify(viewMode == 0 && iconSize == 3, "View routes to active-pane callbacks");
    verify(sortKey == 0 && ascending, "Sort routes to active-pane callbacks");
    verify(groupMode == DirectoryViewSettings::GroupBySize, "Grouping routes to active-pane callback");
    verify(hidden && !thumbnails, "Show toggles route through callbacks");

    PaneMenuController paneMenus(&controller);
    PaneMenuController::BackgroundCallbacks backgroundCallbacks;
    backgroundCallbacks.view = callbacks;
    int backgroundRoute = 0;
    backgroundCallbacks.refresh = [&] { backgroundRoute = 1; };
    backgroundCallbacks.openNewTab = [&] { backgroundRoute = 2; };
    backgroundCallbacks.emptyTrash = [&] { backgroundRoute = 3; };
    backgroundCallbacks.openAdmin = [&] { backgroundRoute = 4; };
    bool inheritedRule = false;
    backgroundCallbacks.applyInheritedRule = [&] { inheritedRule = true; };
    backgroundCallbacks.removeInheritedRule = [&] { inheritedRule = false; };

    const QUrl localDirectory = QUrl::fromLocalFile(folderPath);
    PaneMenuController::BackgroundState localBackground{
        localDirectory, availability(localDirectory, {}, true), state,
        true, false, true, true};
    QMenu localBackgroundMenu(&parent);
    paneMenus.buildBackgroundMenu(localBackgroundMenu, localBackground,
                                  backgroundCallbacks);
    verify(actionById(localBackgroundMenu, QStringLiteral("pane.newFolder"))->isEnabled(),
           "local background enables New Folder from ActionAvailability");
    verify(actionById(localBackgroundMenu, QStringLiteral("pane.paste"))->isEnabled(),
           "local background enables Paste from ActionAvailability");
    verify(actionById(localBackgroundMenu, QStringLiteral("pane.quickAccess")),
           "local background exposes Quick Access");
    verify(actionById(localBackgroundMenu, QStringLiteral("pane.openAdmin")),
           "local background exposes administrator action");
    QAction *applyToSubfolders = actionById(
        localBackgroundMenu, QStringLiteral("pane.applyViewToSubfolders"));
    QAction *removeFromSubfolders = actionById(
        localBackgroundMenu, QStringLiteral("pane.removeViewFromSubfolders"));
    verify(applyToSubfolders && !applyToSubfolders->isCheckable()
               && applyToSubfolders->isEnabled(),
           "background View exposes enabled one-shot Apply view to subfolders action");
    verify(removeFromSubfolders && !removeFromSubfolders->isVisible()
               && !removeFromSubfolders->isEnabled(),
           "background View hides Remove view when no rule exists");
    applyToSubfolders->trigger();
    verify(inheritedRule, "Apply view to subfolders routes its one-shot command");
    auto ruledBackground = localBackground;
    ruledBackground.inheritedRuleEnabled = true;
    QMenu ruledBackgroundMenu(&parent);
    paneMenus.buildBackgroundMenu(ruledBackgroundMenu, ruledBackground,
                                  backgroundCallbacks);
    QAction *visibleRemove = actionById(
        ruledBackgroundMenu, QStringLiteral("pane.removeViewFromSubfolders"));
    verify(visibleRemove && visibleRemove->isVisible() && visibleRemove->isEnabled(),
           "background View exposes Remove view only when a rule exists");
    visibleRemove->trigger();
    verify(!inheritedRule, "Remove view routes its one-shot command");
    verify(actionIdCount(localBackgroundMenu, QStringLiteral("pane.openNewTab")) == 1,
           "background navigation action is not duplicated");
    actionById(localBackgroundMenu, QStringLiteral("pane.refresh"))->trigger();
    verify(backgroundRoute == 1, "background action routes through callback");

    const QUrl searchDirectory(QStringLiteral("thispcsearch:/query"));
    QMenu searchBackgroundMenu(&parent);
    auto searchBackground = localBackground;
    searchBackground.directory = searchDirectory;
    searchBackground.availability = availability(searchDirectory, {}, true);
    searchBackground.quickAccessVisible = false;
    searchBackground.terminalEnabled = false;
    searchBackground.adminVisible = false;
    searchBackground.inheritedRuleEnabled = true;
    paneMenus.buildBackgroundMenu(searchBackgroundMenu, searchBackground,
                                  backgroundCallbacks);
    verify(!actionById(searchBackgroundMenu, QStringLiteral("pane.newFolder"))->isEnabled()
               && !actionById(searchBackgroundMenu, QStringLiteral("pane.paste"))->isEnabled(),
           "Search background blocks mutation actions");
    verify(!actionById(searchBackgroundMenu, QStringLiteral("pane.openNewTab"))->isEnabled()
               && !actionById(searchBackgroundMenu, QStringLiteral("pane.openNewWindow"))->isEnabled(),
           "Search background blocks tab and window navigation");
    verify(!actionById(searchBackgroundMenu, QStringLiteral("pane.applyViewToSubfolders"))->isEnabled()
               && !actionById(searchBackgroundMenu, QStringLiteral("pane.removeViewFromSubfolders"))->isVisible(),
           "Search hides Remove and disables Apply view commands");

    const QUrl trashDirectory(QStringLiteral("trash:/"));
    QMenu trashBackgroundMenu(&parent);
    auto trashBackground = localBackground;
    trashBackground.directory = trashDirectory;
    trashBackground.availability = availability(trashDirectory);
    trashBackground.quickAccessVisible = false;
    trashBackground.terminalEnabled = false;
    trashBackground.adminVisible = false;
    paneMenus.buildBackgroundMenu(trashBackgroundMenu, trashBackground,
                                  backgroundCallbacks);
    QAction *emptyTrash = actionById(trashBackgroundMenu, QStringLiteral("pane.emptyTrash"));
    verify(emptyTrash && emptyTrash->isEnabled(), "Trash root exposes enabled Empty Trash");
    emptyTrash->trigger();
    verify(backgroundRoute == 3, "Empty Trash routes through supplied backend callback");

    const QUrl thisPcDirectory(QStringLiteral("thispc:/"));
    QMenu thisPcBackgroundMenu(&parent);
    auto thisPcBackground = localBackground;
    thisPcBackground.directory = thisPcDirectory;
    thisPcBackground.availability = availability(thisPcDirectory);
    thisPcBackground.quickAccessVisible = false;
    thisPcBackground.terminalEnabled = false;
    thisPcBackground.adminVisible = false;
    paneMenus.buildBackgroundMenu(thisPcBackgroundMenu, thisPcBackground,
                                  backgroundCallbacks);
    verify(!actionById(thisPcBackgroundMenu, QStringLiteral("pane.newFolder"))->isEnabled(),
           "thispc background blocks creation");
    verify(actionById(thisPcBackgroundMenu, QStringLiteral("pane.view"))->isEnabled(),
           "thispc directory context preserves view controls");
    verify(!actionById(thisPcBackgroundMenu, QStringLiteral("pane.applyViewToSubfolders"))->isEnabled()
               && !actionById(thisPcBackgroundMenu, QStringLiteral("pane.removeViewFromSubfolders"))->isVisible(),
           "thispc View disables Apply and hides Remove profile commands");

    const QUrl remoteDirectory(QStringLiteral("sftp://example.test/folder"));
    QMenu remoteBackgroundMenu(&parent);
    auto remoteBackground = localBackground;
    remoteBackground.directory = remoteDirectory;
    remoteBackground.availability = availability(remoteDirectory, {}, true);
    remoteBackground.quickAccessVisible = false;
    remoteBackground.terminalEnabled = false;
    remoteBackground.adminVisible = false;
    paneMenus.buildBackgroundMenu(remoteBackgroundMenu, remoteBackground,
                                  backgroundCallbacks);
    verify(actionById(remoteBackgroundMenu, QStringLiteral("pane.newFolder"))->isEnabled()
               && actionById(remoteBackgroundMenu, QStringLiteral("pane.paste"))->isEnabled(),
           "remote KIO background preserves directory mutations");
    verify(!actionById(remoteBackgroundMenu, QStringLiteral("pane.openTerminal"))->isEnabled(),
           "remote KIO background disables terminal");

    PaneMenuController::ItemCallbacks itemCallbacks;
    int itemRoute = 0;
    itemCallbacks.openNewTab = [&] { itemRoute = 1; };
    itemCallbacks.openOtherPane = [&] { itemRoute = 2; };
    itemCallbacks.extractHere = [&] { itemRoute = 3; };
    itemCallbacks.openAdmin = [&] { itemRoute = 4; };
    itemCallbacks.rename = [&] { itemRoute = 5; };
    itemCallbacks.sendTo = {[](const QList<QUrl> &, const QString &, const QString &) {},
                            [](const QList<QUrl> &) {}, [](const QList<QUrl> &) {},
                            [](const QList<QUrl> &) {}};

    QMenu fileMenu(&parent);
    PaneMenuController::ItemState fileState{
        localFile, {localFile}, availability(localDirectory, {localFile}),
        false, false, false, false, false, true, false, false, false, false, true};
    paneMenus.buildItemMenu(fileMenu, fileState, itemCallbacks);
    verify(!actionById(fileMenu, QStringLiteral("item.openNewTab")),
           "single file has no folder navigation actions");
    verify(actionById(fileMenu, QStringLiteral("item.rename"))->isEnabled()
               && actionById(fileMenu, QStringLiteral("item.properties"))->isEnabled(),
           "single file uses single-selection availability");
    verify(actionById(fileMenu, QStringLiteral("item.openAdmin")),
           "single local file exposes administrator action");

    QMenu folderMenu(&parent);
    PaneMenuController::ItemState folderState{
        localFolder, {localFolder}, availability(localDirectory, {localFolder}),
        true, false, true, true, false, false, false, false, true, true, true};
    paneMenus.buildItemMenu(folderMenu, folderState, itemCallbacks);
    verify(actionById(folderMenu, QStringLiteral("item.openNewTab"))
               && actionById(folderMenu, QStringLiteral("item.openNewWindow"))
               && actionById(folderMenu, QStringLiteral("item.openOtherPane")),
           "single folder exposes tab, window and other-pane navigation");
    verify(actionIdCount(folderMenu, QStringLiteral("item.openOtherPane")) == 1,
           "folder navigation action is not duplicated");

    QMenu multiMenu(&parent);
    PaneMenuController::ItemState multiState{
        localFile, {localFile, localFolder},
        availability(localDirectory, {localFile, localFolder}), false};
    paneMenus.buildItemMenu(multiMenu, multiState, itemCallbacks);
    verify(!actionById(multiMenu, QStringLiteral("item.rename"))->isEnabled()
               && actionById(multiMenu, QStringLiteral("item.batchRename"))->isEnabled(),
           "multi-selection swaps Rename for Batch Rename availability");
    verify(!actionById(multiMenu, QStringLiteral("item.openAdmin")),
           "multi-selection hides administrator action");

    QMenu archiveMenu(&parent);
    auto archiveState = fileState;
    archiveState.archiveExtractable = true;
    paneMenus.buildItemMenu(archiveMenu, archiveState, itemCallbacks);
    verify(actionById(archiveMenu, QStringLiteral("item.extractHere"))
               && actionById(archiveMenu, QStringLiteral("item.extractTo")),
           "archive selection exposes both extraction routes");
    actionById(archiveMenu, QStringLiteral("item.extractHere"))->trigger();
    verify(itemRoute == 3, "archive action routes through supplied backend callback");

    QMenu searchItemMenu(&parent);
    auto searchItemState = fileState;
    searchItemState.searchLocation = true;
    paneMenus.buildItemMenu(searchItemMenu, searchItemState, itemCallbacks);
    verify(actionById(searchItemMenu, QStringLiteral("item.openLocation")),
           "Search item exposes Open Location special case");

    int routedPane = -1;
    const int snapshottedPane = int(PaneId::Primary);
    int currentlyActivePane = int(PaneId::Primary);
    auto snapshotCallbacks = itemCallbacks;
    snapshotCallbacks.openOtherPane = [&, snapshottedPane] { routedPane = snapshottedPane; };
    QMenu snapshotMenu(&parent);
    paneMenus.buildItemMenu(snapshotMenu, folderState, snapshotCallbacks);
    currentlyActivePane = int(PaneId::Split);
    actionById(snapshotMenu, QStringLiteral("item.openOtherPane"))->trigger();
    verify(routedPane == snapshottedPane && routedPane != currentlyActivePane,
           "item callback keeps the pane snapshot captured when menu was built");

    int destroyedRoute = 0;
    QPointer<QAction> destroyedAction;
    {
        auto *ephemeral = new QMenu(&parent);
        auto lifecycleCallbacks = itemCallbacks;
        lifecycleCallbacks.rename = [&] { ++destroyedRoute; };
        paneMenus.buildItemMenu(*ephemeral, fileState, lifecycleCallbacks);
        destroyedAction = actionById(*ephemeral, QStringLiteral("item.rename"));
        delete ephemeral;
    }
    verify(destroyedAction.isNull() && destroyedRoute == 0,
           "destroying a built menu destroys actions without firing callbacks");

    const QStringList expectedPrefix{
        QStringLiteral("item.open"), QStringLiteral("item.openNewTab"),
        QStringLiteral("item.openNewWindow"), QStringLiteral("item.openOtherPane")};
    QStringList actualPrefix;
    for (QAction *candidate : folderMenu.actions()) {
        if (!candidate->objectName().isEmpty()) actualPrefix << candidate->objectName();
        if (actualPrefix.size() == expectedPrefix.size()) break;
    }
    verify(actualPrefix == expectedPrefix, "user-visible folder navigation ordering is stable");

    qInfo("PASS: %d selection menu controller assertions", checks);
    return 0;
}
