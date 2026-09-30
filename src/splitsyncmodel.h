/*
 * Safe, read-only Split View pane synchronization plan model.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "splitcomparemodel.h"

#include <QList>
#include <QString>

enum class SyncDirection {
    LeftToRight,
    RightToLeft
};

enum class SyncPlanAction {
    NoAction,
    CopyFile,
    UpdateFile,
    Conflict,
    Unsupported
};

struct SyncPlanEntry {
    QString name;
    SyncDirection direction = SyncDirection::LeftToRight;
    SyncPlanAction action = SyncPlanAction::NoAction;
    QString sourceSide;
    QString destinationSide;
    bool hasSource = false;
    bool hasDestination = false;
    FileInfo sourceInfo;
    FileInfo destinationInfo;
    QString reason;
    QString differenceDetails;
    bool isAutoExecutable = false;
    bool isDirectory = false;
    CompareStatus compareStatus = CompareStatus::Same;
};

struct SyncPlanSummary {
    int total = 0;
    int noAction = 0;
    int copyFile = 0;
    int updateFile = 0;
    int conflict = 0;
    int unsupported = 0;
    int executable = 0;
};

inline QString syncDirectionLabel(SyncDirection direction)
{
    switch (direction) {
    case SyncDirection::LeftToRight:
        return trLocal("Lewy → Prawy", "Left → Right");
    case SyncDirection::RightToLeft:
        return trLocal("Prawy → Lewy", "Right → Left");
    }
    return QString();
}

inline QString syncPlanActionLabel(SyncPlanAction action)
{
    switch (action) {
    case SyncPlanAction::NoAction:
        return trLocal("Bez zmian", "No action");
    case SyncPlanAction::CopyFile:
        return trLocal("Kopiuj", "Copy");
    case SyncPlanAction::UpdateFile:
        return trLocal("Zaktualizuj", "Update");
    case SyncPlanAction::Conflict:
        return trLocal("Konflikt", "Conflict");
    case SyncPlanAction::Unsupported:
        return trLocal("Nieobsługiwane", "Unsupported");
    }
    return QString();
}

inline SyncPlanSummary summarizeSyncPlan(const QList<SyncPlanEntry> &plan)
{
    SyncPlanSummary summary;
    summary.total = static_cast<int>(plan.size());
    for (const SyncPlanEntry &entry : plan) {
        switch (entry.action) {
        case SyncPlanAction::NoAction:
            ++summary.noAction;
            break;
        case SyncPlanAction::CopyFile:
            ++summary.copyFile;
            ++summary.executable;
            break;
        case SyncPlanAction::UpdateFile:
            ++summary.updateFile;
            ++summary.executable;
            break;
        case SyncPlanAction::Conflict:
            ++summary.conflict;
            break;
        case SyncPlanAction::Unsupported:
            ++summary.unsupported;
            break;
        }
    }
    return summary;
}

inline QList<SyncPlanEntry> computeSyncPlan(
    const QList<CompareEntry> &compareEntries,
    SyncDirection direction)
{
    QList<SyncPlanEntry> plan;
    plan.reserve(compareEntries.size());

    const QString leftLabel = trLocal("Lewy panel", "Left pane");
    const QString rightLabel = trLocal("Prawy panel", "Right pane");

    const QString sourceSideName = (direction == SyncDirection::LeftToRight) ? leftLabel : rightLabel;
    const QString destSideName = (direction == SyncDirection::LeftToRight) ? rightLabel : leftLabel;

    for (const CompareEntry &entry : compareEntries) {
        SyncPlanEntry item;
        item.name = entry.name;
        item.direction = direction;
        item.sourceSide = sourceSideName;
        item.destinationSide = destSideName;
        item.compareStatus = entry.status;
        item.isDirectory = entry.isDirectory;
        item.differenceDetails = entry.differenceReason;

        const bool hasLeft = entry.hasLeft;
        const bool hasRight = entry.hasRight;
        const FileInfo &leftInfo = entry.leftInfo;
        const FileInfo &rightInfo = entry.rightInfo;

        if (direction == SyncDirection::LeftToRight) {
            item.hasSource = hasLeft;
            item.hasDestination = hasRight;
            item.sourceInfo = leftInfo;
            item.destinationInfo = rightInfo;
        } else {
            item.hasSource = hasRight;
            item.hasDestination = hasLeft;
            item.sourceInfo = rightInfo;
            item.destinationInfo = leftInfo;
        }

        if (hasLeft && hasRight) {
            if (leftInfo.isDir != rightInfo.isDir) {
                // E. File vs Directory mismatch => Conflict
                item.action = SyncPlanAction::Conflict;
                item.reason = trLocal("Konflikt typu: plik / folder", "Type conflict: file / folder");
                item.isAutoExecutable = false;
                item.isDirectory = false;
            } else if (leftInfo.isDir) {
                // F. Directory on both sides => NoAction
                item.action = SyncPlanAction::NoAction;
                item.reason = trLocal("Folder istnieje po obu stronach; zawartość nie została porównana",
                                      "Folder exists on both sides; contents were not compared");
                item.isAutoExecutable = false;
                item.isDirectory = true;
            } else {
                // Regular files on both sides
                item.isDirectory = false;
                if (entry.status == CompareStatus::Same) {
                    // A. Same => NoAction
                    item.action = SyncPlanAction::NoAction;
                    item.reason = trLocal("Bez zmian", "No action");
                    item.isAutoExecutable = false;
                } else {
                    // D. Changed => UpdateFile
                    item.action = SyncPlanAction::UpdateFile;
                    if (direction == SyncDirection::LeftToRight) {
                        item.reason = trLocal("Zaktualizuj prawy plik wersją z lewego panelu",
                                              "Update right file from left pane");
                    } else {
                        item.reason = trLocal("Zaktualizuj lewy plik wersją z prawego panelu",
                                              "Update left file from right pane");
                    }
                    item.isAutoExecutable = true;
                }
            }
        } else if (item.hasSource) {
            // Exists only on source side
            if (item.sourceInfo.isDir) {
                // G. Directory only on source side => Unsupported
                item.action = SyncPlanAction::Unsupported;
                item.reason = trLocal("Folder tylko po stronie źródłowej — synchronizacja katalogów nie jest częścią Stage 2",
                                      "Directory exists only on source side — directory synchronization is outside Stage 2");
                item.isAutoExecutable = false;
                item.isDirectory = true;
            } else {
                // B. Only on source side — regular file => CopyFile
                item.action = SyncPlanAction::CopyFile;
                if (direction == SyncDirection::LeftToRight) {
                    item.reason = trLocal("Skopiuj do prawego panelu", "Copy to right pane");
                } else {
                    item.reason = trLocal("Skopiuj do lewego panelu", "Copy to left pane");
                }
                item.isAutoExecutable = true;
                item.isDirectory = false;
            }
        } else if (item.hasDestination) {
            // C & H. Exists only on destination side => NoAction
            item.action = SyncPlanAction::NoAction;
            item.reason = trLocal("Pozostaw bez zmian — usuwanie nadmiarowych elementów nie jest częścią Stage 2",
                                  "Keep unchanged — deleting destination-only items is outside Stage 2");
            item.isAutoExecutable = false;
            item.isDirectory = item.destinationInfo.isDir;
        }

        plan.append(std::move(item));
    }

    return plan;
}
