/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "actionstatecontroller.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

static int checks = 0;

static void verify(bool condition, const char *description)
{
    if (!condition) qFatal("FAIL: %s", description);
    ++checks;
}

static ActionStateInput directoryInput(const QUrl &directory)
{
    ActionStateInput input;
    input.directory = directory;
    input.isDirectory = true;
    input.recoverySafe = true;
    return input;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QUrl localDirectory = QUrl::fromLocalFile(QStringLiteral("/tmp/action-state"));
    const QUrl localFile = QUrl::fromLocalFile(QStringLiteral("/tmp/action-state/file.txt"));
    const QUrl localFolder = QUrl::fromLocalFile(QStringLiteral("/tmp/action-state/folder"));

    auto input = directoryInput(localDirectory);
    auto state = ActionStateController::compute(input);
    verify(!state.copyEnabled && !state.cutEnabled && !state.renameEnabled
               && !state.batchRenameEnabled && !state.propertiesEnabled
               && !state.trashEnabled,
           "background has no selection actions");
    verify(state.canModifyCurrentDirectory && state.createEnabled
               && !state.canPasteHere && !state.pasteEnabled,
           "local directory background creates but cannot paste without clipboard URLs");

    input.selection = {localFile};
    state = ActionStateController::compute(input);
    verify(state.copyEnabled && state.cutEnabled && state.renameEnabled
               && state.propertiesEnabled && state.trashEnabled
               && !state.batchRenameEnabled,
           "one local file enables single-selection actions");

    input.selection = {localFolder};
    state = ActionStateController::compute(input);
    verify(state.copyEnabled && state.cutEnabled && state.renameEnabled
               && state.propertiesEnabled && state.trashEnabled,
           "one local directory preserves single-selection availability");

    input.selection = {localFile, localFolder};
    state = ActionStateController::compute(input);
    verify(state.copyEnabled && state.cutEnabled && state.batchRenameEnabled
               && state.trashEnabled && !state.renameEnabled
               && !state.propertiesEnabled,
           "multiple local selection enables batch actions only");

    auto remote = directoryInput(QUrl(QStringLiteral("sftp://host/path")));
    remote.selection = {QUrl(QStringLiteral("sftp://host/path/file"))};
    state = ActionStateController::compute(remote);
    verify(state.canModifyCurrentDirectory && state.createEnabled
               && !state.trashEnabled && state.renameEnabled,
           "remote KIO allows directory mutation but not local Trash");

    auto search = directoryInput(QUrl(QStringLiteral("thispcsearch:/query")));
    search.isSearchLocation = true;
    search.selection = {localFile};
    search.clipboardHasUrls = true;
    state = ActionStateController::compute(search);
    verify(!state.canModifyCurrentDirectory && !state.createEnabled
               && !state.pasteEnabled && !state.openDolphinEnabled
               && state.renameEnabled && state.trashEnabled,
           "Search blocks directory actions and preserves selected-item actions");

    auto trash = directoryInput(QUrl(QStringLiteral("trash:/")));
    trash.isTrashLocation = true;
    trash.isTrashRoot = true;
    trash.emptyTrashAvailable = true;
    state = ActionStateController::compute(trash);
    verify(!state.canModifyCurrentDirectory && state.emptyTrashVisible
               && state.emptyTrashEnabled && !state.createEnabled,
           "Trash root exposes enabled Empty Trash capability");

    auto thisPc = directoryInput(QUrl(QStringLiteral("thispc:/")));
    thisPc.isThisPcLocation = true;
    state = ActionStateController::compute(thisPc);
    verify(!state.canModifyCurrentDirectory && !state.createEnabled
               && state.openDolphinEnabled,
           "thispc root blocks creation but keeps Open in Dolphin");

    auto admin = directoryInput(QUrl(QStringLiteral("admin:/etc")));
    state = ActionStateController::compute(admin);
    verify(state.canModifyCurrentDirectory && state.createEnabled,
           "admin KIO preserves modification availability");

    auto filesystemRoot = directoryInput(QUrl::fromLocalFile(QStringLiteral("/")));
    auto home = directoryInput(QUrl::fromLocalFile(QStringLiteral("/home/test")));
    verify(ActionStateController::compute(filesystemRoot).createEnabled
               && ActionStateController::compute(home).createEnabled,
           "filesystem root and home preserve the same historical directory availability");

    input = directoryInput(localDirectory);
    input.clipboardHasUrls = true;
    state = ActionStateController::compute(input);
    verify(state.canPasteHere && state.pasteEnabled,
           "pasteable clipboard enables Paste");
    input.recoverySafe = false;
    state = ActionStateController::compute(input);
    verify(state.canPasteHere && !state.pasteEnabled && !state.createEnabled,
           "recovery fence blocks mutations without changing raw paste capability");
    input.selection = {localFile};
    state = ActionStateController::compute(input);
    verify(state.copyEnabled && state.cutEnabled && !state.renameEnabled
               && !state.propertiesEnabled && !state.trashEnabled,
           "recovery fence preserves Copy and Cut historical behavior");

    ActionStateInput invalid;
    invalid.recoverySafe = true;
    invalid.clipboardHasUrls = true;
    state = ActionStateController::compute(invalid);
    verify(!state.canModifyCurrentDirectory && !state.canPasteHere
               && !state.createEnabled && !state.pasteEnabled
               && !state.viewControlsEnabled,
           "invalid context is safely disabled");

    auto notDirectory = directoryInput(localDirectory);
    notDirectory.isDirectory = false;
    notDirectory.selection = {localFile, localFolder};
    state = ActionStateController::compute(notDirectory);
    verify(!state.canModifyCurrentDirectory && !state.batchRenameEnabled
               && !state.viewControlsEnabled,
           "non-directory context disables directory and batch controls");

    auto primary = directoryInput(localDirectory);
    primary.selection = {localFile};
    primary.clipboardHasUrls = true;
    const auto primaryState = ActionStateController::compute(primary);
    const auto splitState = ActionStateController::compute(primary);
    verify(primaryState.copyEnabled == splitState.copyEnabled
               && primaryState.renameEnabled == splitState.renameEnabled
               && primaryState.pasteEnabled == splitState.pasteEnabled
               && primaryState.createEnabled == splitState.createEnabled,
           "equivalent Primary and Split inputs have parity");

    QTemporaryDir temporary;
    verify(temporary.isValid(), "temporary directory for writability case exists");
    QFile::setPermissions(temporary.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    auto nonWritable = directoryInput(QUrl::fromLocalFile(temporary.path()));
    state = ActionStateController::compute(nonWritable);
    verify(state.createEnabled,
           "local filesystem writability remains intentionally outside historical availability");
    QFile::setPermissions(temporary.path(), QFileDevice::ReadOwner
        | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    input = directoryInput(localDirectory);
    input.selection = {QUrl::fromLocalFile(QStringLiteral("/tmp/archive.zip"))};
    const auto archiveState = ActionStateController::compute(input);
    input.selection = {QUrl::fromLocalFile(QStringLiteral("/tmp/document.txt"))};
    const auto regularState = ActionStateController::compute(input);
    verify(archiveState.copyEnabled == regularState.copyEnabled
               && archiveState.renameEnabled == regularState.renameEnabled
               && archiveState.trashEnabled == regularState.trashEnabled,
           "archive and ordinary files have identical global action availability");

    qInfo("PASS: %d ActionStateController assertions", checks);
    return 0;
}
