#include "panemenucontroller.h"

#include "browsercommon.h"

#include <QAction>
#include <QMenu>

namespace {
QAction *addAction(QMenu &menu, const QString &id, const QIcon &icon,
                   const QString &text, bool enabled,
                   const std::function<void()> &callback)
{
    QAction *action = menu.addAction(icon, text);
    action->setObjectName(id);
    action->setEnabled(enabled);
    if (callback)
        QObject::connect(action, &QAction::triggered, &menu, callback);
    return action;
}

QAction *addAction(QMenu &menu, const QString &id, const QString &text,
                   bool enabled, const std::function<void()> &callback)
{
    QAction *action = menu.addAction(text);
    action->setObjectName(id);
    action->setEnabled(enabled);
    if (callback)
        QObject::connect(action, &QAction::triggered, &menu, callback);
    return action;
}
}

PaneMenuController::PaneMenuController(SelectionMenuController *selectionMenus)
    : m_selectionMenus(selectionMenus)
{
}

void PaneMenuController::buildBackgroundMenu(
    QMenu &menu, const BackgroundState &state,
    const BackgroundCallbacks &callbacks) const
{
    m_selectionMenus->addViewSubmenu(menu, state.view, callbacks.view);
    QAction *viewAction = menu.actions().constLast();
    viewAction->setObjectName(QStringLiteral("pane.view"));
    viewAction->setEnabled(state.availability.viewControlsEnabled);
    addViewProfileActions(*viewAction->menu(), state, callbacks);
    m_selectionMenus->addSortSubmenu(menu, state.view, callbacks.view);
    QAction *sortAction = menu.actions().constLast();
    sortAction->setObjectName(QStringLiteral("pane.sort"));
    sortAction->setEnabled(state.availability.viewControlsEnabled);
    menu.addSeparator();

    addAction(menu, QStringLiteral("pane.refresh"),
              themedIcon(QStringLiteral("view-refresh")),
              trLocal("Odśwież", "Refresh"), true, callbacks.refresh);
    addAction(menu, QStringLiteral("pane.selectAll"),
              themedIcon(QStringLiteral("edit-select-all")),
              trLocal("Zaznacz wszystko", "Select all"), true,
              callbacks.selectAll);
    menu.addSeparator();

    addAction(menu, QStringLiteral("pane.newFolder"),
              themedIcon(QStringLiteral("folder-new")),
              trLocal("Nowy folder", "New folder"),
              state.availability.createEnabled, callbacks.newFolder);
    addAction(menu, QStringLiteral("pane.paste"),
              themedIcon(QStringLiteral("edit-paste")),
              trLocal("Wklej", "Paste"), state.availability.pasteEnabled,
              callbacks.paste);

    if (state.availability.emptyTrashVisible) {
        menu.addSeparator();
        addAction(menu, QStringLiteral("pane.emptyTrash"),
                  themedIcon(QStringLiteral("user-trash")),
                  trLocal("Opróżnij kosz", "Empty Trash"),
                  state.availability.emptyTrashEnabled, callbacks.emptyTrash);
    }

    menu.addSeparator();
    addAction(menu, QStringLiteral("pane.openDolphin"),
              themedIcon(QStringLiteral("system-file-manager")),
              trLocal("Otwórz w Dolphinie", "Open in Dolphin"),
              state.availability.openDolphinEnabled, callbacks.openDolphin);
    addAction(menu, QStringLiteral("pane.openNewTab"),
              themedIcon(QStringLiteral("tab-new")),
              trLocal("Otwórz ten folder w nowej karcie", "Open this folder in new tab"),
              state.availability.openDolphinEnabled, callbacks.openNewTab);
    addAction(menu, QStringLiteral("pane.openNewWindow"),
              themedIcon(QStringLiteral("window-new")),
              trLocal("Otwórz ten folder w nowym oknie", "Open this folder in new window"),
              state.availability.openDolphinEnabled, callbacks.openNewWindow);
    addAction(menu, QStringLiteral("pane.openOtherPane"),
              themedIcon(QStringLiteral("view-split-left-right"), QStringLiteral("view-list-details")),
              trLocal("Otwórz ten folder w drugim panelu", "Open this folder in other pane"),
              state.availability.openDolphinEnabled, callbacks.openOtherPane);

    if (state.quickAccessVisible) {
        addAction(menu, QStringLiteral("pane.quickAccess"),
                  themedIcon(state.quickAccessPinned ? QStringLiteral("list-remove")
                                                     : QStringLiteral("folder-favorites")),
                  state.quickAccessPinned
                      ? trLocal("Odepnij od Szybkiego dostępu", "Unpin from Quick access")
                      : trLocal("Przypnij do Szybkiego dostępu", "Pin to Quick access"),
                  true, callbacks.toggleQuickAccess);
    }

    addAction(menu, QStringLiteral("pane.openTerminal"),
              themedIcon(QStringLiteral("utilities-terminal")),
              trLocal("Otwórz terminal tutaj", "Open terminal here"),
              state.terminalEnabled, callbacks.openTerminal);
    if (state.adminVisible) {
        menu.addSeparator();
        addAction(menu, QStringLiteral("pane.openAdmin"),
                  themedIcon(QStringLiteral("security-high")),
                  trLocal("Otwórz ten folder jako administrator",
                          "Open this folder as administrator"),
                  true, callbacks.openAdmin);
    }
}

void PaneMenuController::addViewProfileActions(
    QMenu &menu, const BackgroundState &state,
    const BackgroundCallbacks &callbacks) const
{
    menu.addSeparator();
    const bool profileLocation = state.directory.isValid()
        && state.directory.scheme() != QStringLiteral("thispcsearch")
        && !sameLocation(state.directory, kThisPcUrl);
    QAction *apply = addAction(
        menu, QStringLiteral("pane.applyViewToSubfolders"),
        trLocal("Zastosuj ten widok do podfolderów", "Apply this view to subfolders"),
        state.availability.viewControlsEnabled && profileLocation,
        callbacks.applyInheritedRule);
    apply->setCheckable(false);
    QAction *remove = addAction(
        menu, QStringLiteral("pane.removeViewFromSubfolders"),
        trLocal("Usuń widok zastosowany do podfolderów",
                "Remove view applied to subfolders"),
        state.availability.viewControlsEnabled && profileLocation
            && state.inheritedRuleEnabled,
        callbacks.removeInheritedRule);
    remove->setVisible(profileLocation && state.inheritedRuleEnabled);
}

void PaneMenuController::buildItemMenu(
    QMenu &menu, const ItemState &state,
    const ItemCallbacks &callbacks) const
{
    const bool single = state.selection.size() == 1;
    addAction(menu, QStringLiteral("item.open"),
              themedIcon(state.isDirectory ? QStringLiteral("folder-open")
                                           : QStringLiteral("document-open")),
              trLocal("Otwórz", "Open"), true, callbacks.open);

    if (single && state.isDirectory) {
        addAction(menu, QStringLiteral("item.openNewTab"),
                  themedIcon(QStringLiteral("tab-new")),
                  trLocal("Otwórz w nowej karcie", "Open in new tab"), true,
                  callbacks.openNewTab);
        addAction(menu, QStringLiteral("item.openNewWindow"),
                  themedIcon(QStringLiteral("window-new")),
                  trLocal("Otwórz w nowym oknie", "Open in new window"), true,
                  callbacks.openNewWindow);
        addAction(menu, QStringLiteral("item.openOtherPane"),
                  themedIcon(QStringLiteral("view-split-left-right"), QStringLiteral("view-list-details")),
                  trLocal("Otwórz w drugim panelu", "Open in other pane"), true,
                  callbacks.openOtherPane);
    }

    if (state.quickAccessVisible) {
        addAction(menu, QStringLiteral("item.quickAccess"),
                  themedIcon(state.quickAccessPinned ? QStringLiteral("list-remove")
                                                     : QStringLiteral("folder-favorites")),
                  state.quickAccessPinned
                      ? trLocal("Odepnij od Szybkiego dostępu", "Unpin from Quick access")
                      : trLocal("Przypnij do Szybkiego dostępu", "Pin to Quick access"),
                  true, callbacks.toggleQuickAccess);
    }

    addAction(menu, QStringLiteral("item.openDolphin"),
              themedIcon(QStringLiteral("system-file-manager")),
              trLocal("Otwórz w Dolphinie", "Open in Dolphin"), true,
              callbacks.openDolphin);
    if (state.searchLocation && single) {
        addAction(menu, QStringLiteral("item.openLocation"),
                  themedIcon(QStringLiteral("folder-open")),
                  state.isDirectory
                      ? trLocal("Otwórz folder nadrzędny", "Open parent folder")
                      : trLocal("Otwórz lokalizację pliku", "Open file location"),
                  true, callbacks.openLocation);
    }

    m_selectionMenus->addOpenWithSubmenu(menu, state.selection);
    if (single && state.archiveExtractable) {
        QMenu *extract = menu.addMenu(themedIcon(QStringLiteral("archive-extract")),
                                      trLocal("Wypakuj", "Extract"));
        extract->setObjectName(QStringLiteral("item.extract"));
        addAction(*extract, QStringLiteral("item.extractHere"),
                  trLocal("Wypakuj tutaj", "Extract Here"), true,
                  callbacks.extractHere);
        addAction(*extract, QStringLiteral("item.extractTo"),
                  trLocal("Wypakuj do…", "Extract To…"), true,
                  callbacks.extractTo);
    }

    addAction(menu, QStringLiteral("item.print"),
              themedIcon(QStringLiteral("document-print")),
              trLocal("Drukuj…", "Print…"), single && state.printable,
              callbacks.print);
    if (single && state.wallpaperVisible) {
        addAction(menu, QStringLiteral("item.wallpaper"),
                  themedIcon(QStringLiteral("preferences-desktop-wallpaper")),
                  trLocal("Ustaw jako tło pulpitu", "Set as desktop wallpaper"),
                  state.wallpaperEnabled, callbacks.wallpaper);
    }

    m_selectionMenus->addSendToSubmenu(menu, state.selection, callbacks.sendTo);
    addAction(menu, QStringLiteral("item.openTerminal"),
              themedIcon(QStringLiteral("utilities-terminal")),
              trLocal("Otwórz terminal tutaj", "Open terminal here"),
              state.terminalEnabled, callbacks.openTerminal);
    if (state.adminVisible) {
        addAction(menu, QStringLiteral("item.openAdmin"),
                  themedIcon(QStringLiteral("security-high")),
                  state.isDirectory
                      ? trLocal("Otwórz jako administrator", "Open as administrator")
                      : trLocal("Otwórz lokalizację jako administrator",
                                "Open location as administrator"),
                  true, callbacks.openAdmin);
    }

    menu.addSeparator();
    addAction(menu, QStringLiteral("item.cut"), themedIcon(QStringLiteral("edit-cut")),
              trLocal("Wytnij", "Cut"), state.availability.cutEnabled,
              callbacks.cut);
    addAction(menu, QStringLiteral("item.copy"), themedIcon(QStringLiteral("edit-copy")),
              trLocal("Kopiuj", "Copy"), state.availability.copyEnabled,
              callbacks.copy);
    addAction(menu, QStringLiteral("item.rename"), themedIcon(QStringLiteral("edit-rename")),
              trLocal("Zmień nazwę", "Rename"), state.availability.renameEnabled,
              callbacks.rename);
    addAction(menu, QStringLiteral("item.batchRename"), themedIcon(QStringLiteral("edit-rename")),
              trLocal("Zmień nazwy zbiorczo…", "Batch Rename…"),
              state.availability.batchRenameEnabled, callbacks.batchRename);
    addAction(menu, QStringLiteral("item.trash"), themedIcon(QStringLiteral("user-trash")),
              trLocal("Do Kosza", "Trash"), state.availability.trashEnabled,
              callbacks.trash);
    addAction(menu, QStringLiteral("item.pasteInto"), themedIcon(QStringLiteral("edit-paste")),
              trLocal("Wklej do tego folderu", "Paste into this folder"),
              state.pasteIntoEnabled, callbacks.pasteInto);

    menu.addSeparator();
    addAction(menu, QStringLiteral("item.copyAddress"), themedIcon(QStringLiteral("edit-copy")),
              trLocal("Kopiuj adres", "Copy address"), true,
              callbacks.copyAddress);
    addAction(menu, QStringLiteral("item.properties"),
              themedIcon(QStringLiteral("document-properties")),
              trLocal("Właściwości", "Properties"),
              state.availability.propertiesEnabled, callbacks.properties);
}
