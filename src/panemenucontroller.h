/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "actionstatecontroller.h"
#include "selectionmenucontroller.h"

#include <QList>
#include <QUrl>

#include <functional>

class QMenu;

class PaneMenuController final
{
public:
    struct BackgroundState {
        QUrl directory;
        ActionAvailability availability;
        SelectionMenuController::ViewState view;
        bool quickAccessVisible = false;
        bool quickAccessPinned = false;
        bool terminalEnabled = false;
        bool adminVisible = false;
        bool inheritedRuleEnabled = false;
        bool saveRemoteVisible = false;
        bool isMountRoot = false;
    };

    struct BackgroundCallbacks {
        SelectionMenuController::ViewCallbacks view;
        std::function<void()> refresh;
        std::function<void()> selectAll;
        std::function<void()> newFolder;
        std::function<void()> paste;
        std::function<void()> emptyTrash;
        std::function<void()> openDolphin;
        std::function<void()> openNewTab;
        std::function<void()> openNewWindow;
        std::function<void()> openOtherPane;
        std::function<void()> toggleQuickAccess;
        std::function<void()> openTerminal;
        std::function<void()> openAdmin;
        std::function<void()> applyInheritedRule;
        std::function<void()> removeInheritedRule;
        std::function<void()> saveRemote;
        std::function<void()> analyzeStorage;
        std::function<void()> properties;
    };

    struct ItemState {
        QUrl url;
        QList<QUrl> selection;
        ActionAvailability availability;
        bool isDirectory = false;
        bool searchLocation = false;
        bool quickAccessVisible = false;
        bool quickAccessPinned = false;
        bool archiveExtractable = false;
        bool printable = false;
        bool wallpaperVisible = false;
        bool wallpaperEnabled = false;
        bool pasteIntoEnabled = false;
        bool terminalEnabled = false;
        bool adminVisible = false;
        bool saveRemoteVisible = false;
        bool checksumVisible = false;
    };

    struct ItemCallbacks {
        std::function<void()> open;
        std::function<void()> openNewTab;
        std::function<void()> openNewWindow;
        std::function<void()> openOtherPane;
        std::function<void()> toggleQuickAccess;
        std::function<void()> openDolphin;
        std::function<void()> openLocation;
        std::function<void()> extractHere;
        std::function<void()> extractTo;
        std::function<void()> print;
        std::function<void()> wallpaper;
        SelectionMenuController::SendToCallbacks sendTo;
        std::function<void()> openTerminal;
        std::function<void()> openAdmin;
        std::function<void()> cut;
        std::function<void()> copy;
        std::function<void()> rename;
        std::function<void()> batchRename;
        std::function<void()> trash;
        std::function<void()> pasteInto;
        std::function<void()> copyAddress;
        std::function<void()> analyzeStorage;
        std::function<void()> calculateChecksum;
        std::function<void()> properties;
        std::function<void()> saveRemote;
    };

    explicit PaneMenuController(SelectionMenuController *selectionMenus);

    void buildBackgroundMenu(QMenu &menu, const BackgroundState &state,
                             const BackgroundCallbacks &callbacks) const;
    void addViewProfileActions(QMenu &menu, const BackgroundState &state,
                               const BackgroundCallbacks &callbacks) const;
    void buildItemMenu(QMenu &menu, const ItemState &state,
                       const ItemCallbacks &callbacks) const;

private:
    SelectionMenuController *m_selectionMenus = nullptr;
};
