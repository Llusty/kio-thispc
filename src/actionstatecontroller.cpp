/*
 * Pure file-action availability computation.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "actionstatecontroller.h"

ActionAvailability ActionStateController::compute(const ActionStateInput &input)
{
    ActionAvailability state;

    state.canModifyCurrentDirectory = input.isDirectory
        && input.directory.isValid()
        && !input.isThisPcLocation
        && !input.isTrashLocation
        && !input.isSearchLocation;
    state.canPasteHere = state.canModifyCurrentDirectory
        && input.clipboardHasUrls;

    const bool hasSelection = !input.selection.isEmpty();
    const bool singleSelection = input.selection.size() == 1;
    bool allLocal = hasSelection;
    for (const QUrl &url : input.selection) {
        allLocal = allLocal && url.isLocalFile();
    }

    state.copyEnabled = hasSelection;
    state.cutEnabled = hasSelection;
    state.renameEnabled = input.recoverySafe && singleSelection;
    state.batchRenameEnabled = input.recoverySafe
        && input.selection.size() >= 2
        && input.isDirectory;
    // Opening an inspector is read-only; Apply retains its own recovery fence.
    state.propertiesEnabled = singleSelection && input.selection.first().isValid()
        && !input.selection.first().isEmpty();
    state.trashEnabled = input.recoverySafe && allLocal;
    state.emptyTrashVisible = input.isTrashRoot;
    state.emptyTrashEnabled = input.recoverySafe
        && input.isTrashRoot
        && input.emptyTrashAvailable;
    state.createEnabled = input.recoverySafe
        && state.canModifyCurrentDirectory;
    state.pasteEnabled = input.recoverySafe && state.canPasteHere;
    state.viewControlsEnabled = input.isDirectory;
    state.openDolphinEnabled = !input.isSearchLocation;

    return state;
}
