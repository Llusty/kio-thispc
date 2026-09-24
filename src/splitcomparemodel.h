/*
 * Safe, read-only Split View pane comparison model.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

enum class CompareStatus {
    Same,
    OnlyLeft,
    OnlyRight,
    Changed
};

struct CompareEntry {
    QString name;
    CompareStatus status = CompareStatus::Same;
    bool isDirectory = false;
    bool hasLeft = false;
    bool hasRight = false;
    FileInfo leftInfo;
    FileInfo rightInfo;
    QString differenceReason;
};

inline QString compareStatusLabel(CompareStatus status)
{
    switch (status) {
    case CompareStatus::Same:
        return trLocal("Takie same", "Same");
    case CompareStatus::OnlyLeft:
        return trLocal("Tylko po lewej", "Only left");
    case CompareStatus::OnlyRight:
        return trLocal("Tylko po prawej", "Only right");
    case CompareStatus::Changed:
        return trLocal("Zmienione", "Changed");
    }
    return QString();
}

inline QList<CompareEntry> computePaneComparison(
    const QList<FileInfo> &leftFiles,
    const QList<FileInfo> &rightFiles)
{
    // Collect all unique names in deterministic order
    std::map<QString, std::vector<FileInfo>> leftMap;
    for (const FileInfo &file : leftFiles) {
        if (!file.name.isEmpty()) {
            leftMap[file.name].push_back(file);
        }
    }

    std::map<QString, std::vector<FileInfo>> rightMap;
    for (const FileInfo &file : rightFiles) {
        if (!file.name.isEmpty()) {
            rightMap[file.name].push_back(file);
        }
    }

    // Set of all names
    std::set<QString> allNames;
    for (const auto &[name, _] : leftMap) allNames.insert(name);
    for (const auto &[name, _] : rightMap) allNames.insert(name);

    QList<CompareEntry> result;

    for (const QString &name : allNames) {
        auto itL = leftMap.find(name);
        auto itR = rightMap.find(name);

        const size_t countL = (itL != leftMap.end()) ? itL->second.size() : 0;
        const size_t countR = (itR != rightMap.end()) ? itR->second.size() : 0;
        const size_t maxCount = std::max(countL, countR);

        for (size_t i = 0; i < maxCount; ++i) {
            CompareEntry entry;
            entry.name = name;

            if (i < countL && i < countR) {
                const FileInfo &left = itL->second[i];
                const FileInfo &right = itR->second[i];
                entry.hasLeft = true;
                entry.hasRight = true;
                entry.leftInfo = left;
                entry.rightInfo = right;

                if (left.isDir != right.isDir) {
                    entry.status = CompareStatus::Changed;
                    entry.isDirectory = false;
                    entry.differenceReason = trLocal("Różny typ (plik / folder)", "Different type (file / folder)");
                } else if (left.isDir) {
                    entry.status = CompareStatus::Same;
                    entry.isDirectory = true;
                    entry.differenceReason = trLocal("Folder po obu stronach", "Folder on both sides");
                } else {
                    entry.isDirectory = false;
                    QStringList reasons;
                    bool metadataIncomplete = false;

                    // Size comparison
                    if (left.size >= 0 && right.size >= 0) {
                        if (left.size != right.size) {
                            reasons.append(trLocal("Różny rozmiar", "Different size"));
                        }
                    } else if (left.size >= 0 || right.size >= 0) {
                        // Known on one side, unknown on the other => Changed
                        reasons.append(trLocal("Brak metadanych rozmiaru po jednej stronie",
                                               "Size metadata missing on one side"));
                    } else {
                        // Unknown on both sides
                        metadataIncomplete = true;
                    }

                    // Modification time comparison
                    if (left.modificationTime > 0 && right.modificationTime > 0) {
                        if (left.modificationTime != right.modificationTime) {
                            reasons.append(trLocal("Różny czas modyfikacji", "Different modification time"));
                        }
                    } else if (left.modificationTime > 0 || right.modificationTime > 0) {
                        // Known on one side, unknown on the other => Changed
                        reasons.append(trLocal("Brak metadanych czasu po jednej stronie",
                                               "Time metadata missing on one side"));
                    } else {
                        // Unknown on both sides
                        metadataIncomplete = true;
                    }

                    if (!reasons.isEmpty()) {
                        entry.status = CompareStatus::Changed;
                        entry.differenceReason = reasons.join(QStringLiteral(", "));
                    } else if (metadataIncomplete) {
                        entry.status = CompareStatus::Same;
                        entry.differenceReason = trLocal(
                            "Brak różnic w dostępnych metadanych; część metadanych niedostępna",
                            "No differences in available metadata; some metadata unavailable");
                    } else {
                        entry.status = CompareStatus::Same;
                        entry.differenceReason = trLocal("Metadane zgodne", "Metadata matches");
                    }
                }
            } else if (i < countL) {
                const FileInfo &left = itL->second[i];
                entry.hasLeft = true;
                entry.leftInfo = left;
                entry.status = CompareStatus::OnlyLeft;
                entry.isDirectory = left.isDir;
                entry.differenceReason = trLocal("Brak w prawym panelu", "Missing in right pane");
            } else {
                const FileInfo &right = itR->second[i];
                entry.hasRight = true;
                entry.rightInfo = right;
                entry.status = CompareStatus::OnlyRight;
                entry.isDirectory = right.isDir;
                entry.differenceReason = trLocal("Brak w lewym panelu", "Missing in left pane");
            }

            result.append(std::move(entry));
        }
    }

    std::stable_sort(result.begin(), result.end(), [](const CompareEntry &a, const CompareEntry &b) {
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory;
        }
        return a.name.localeAwareCompare(b.name) < 0;
    });

    return result;
}

inline QString formatFileItemMetadata(const FileInfo &info)
{
    if (info.isDir) {
        return trLocal("Folder", "Folder");
    }

    const QString sizeStr = (info.size >= 0)
        ? formatFileSize(info.size, false)
        : QStringLiteral("—");

    const QString timeStr = (info.modificationTime > 0)
        ? formatModificationTime(info.modificationTime)
        : QStringLiteral("—");

    return QStringLiteral("%1  (%2)").arg(sizeStr, timeStr);
}
