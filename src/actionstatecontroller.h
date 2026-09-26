/*
 * Pure file-action availability computation.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QList>
#include <QUrl>

struct ActionStateInput {
    QUrl directory;
    QList<QUrl> selection;
    bool isDirectory = false;
    bool isThisPcLocation = false;
    bool isSearchLocation = false;
    bool isTrashLocation = false;
    bool isTrashRoot = false;
    bool clipboardHasUrls = false;
    bool recoverySafe = false;
    bool emptyTrashAvailable = false;
};

struct ActionAvailability {
    bool canModifyCurrentDirectory = false;
    bool canPasteHere = false;
    bool copyEnabled = false;
    bool cutEnabled = false;
    bool renameEnabled = false;
    bool batchRenameEnabled = false;
    bool propertiesEnabled = false;
    bool trashEnabled = false;
    bool emptyTrashVisible = false;
    bool emptyTrashEnabled = false;
    bool createEnabled = false;
    bool pasteEnabled = false;
    bool viewControlsEnabled = false;
    bool openDolphinEnabled = false;
};

class ActionStateController final
{
public:
    static ActionAvailability compute(const ActionStateInput &input);
};
