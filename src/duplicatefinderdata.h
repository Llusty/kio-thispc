/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "storagescandata.h"

#include <QDir>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QPair>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <limits>

inline quint64 saturatingAdd(quint64 a, quint64 b)
{
    if (std::numeric_limits<quint64>::max() - a < b) {
        return std::numeric_limits<quint64>::max();
    }
    return a + b;
}

enum class DuplicateFinderState
{
    Idle,
    Running,
    Completed,
    Cancelled,
    Failed
};

struct DuplicatePhysicalFile
{
    quint64 deviceId = 0;
    quint64 inode = 0;
    quint64 logicalSize = 0;
    quint64 allocatedSize = 0;
    quint32 linkCount = 1;
    qint64 mtimeSec = 0;
    qint64 mtimeNsec = 0;
    qint64 ctimeSec = 0;
    qint64 ctimeNsec = 0;
    QString canonicalPath;
    QString name;
    QString relativePath;
    QStringList aliasPaths;
};

struct DuplicateCandidatePlan
{
    quint64 totalPhysicalFiles = 0;
    quint64 candidateGroups = 0;
    quint64 candidateFiles = 0;
    quint64 candidateBytes = 0;
    QList<QList<DuplicatePhysicalFile>> candidateSizeGroups;
};

struct DuplicateGroup
{
    quint64 logicalSize = 0;
    QString sha256;
    QList<DuplicatePhysicalFile> files;

    quint64 recoverableBytes() const
    {
        quint64 totalAllocated = 0;
        quint64 maxAllocated = 0;
        for (const auto &f : files) {
            totalAllocated = saturatingAdd(totalAllocated, f.allocatedSize);
            if (f.allocatedSize > maxAllocated) {
                maxAllocated = f.allocatedSize;
            }
        }
        return (totalAllocated >= maxAllocated) ? (totalAllocated - maxAllocated) : 0;
    }
};

struct DuplicateFinderStats
{
    quint64 totalCandidateFiles = 0;
    quint64 totalCandidateBytes = 0;
    quint64 hashedFiles = 0;
    quint64 hashedBytes = 0;
    quint64 resolvedFiles = 0;
    quint64 resolvedBytes = 0;
    quint64 staleFiles = 0;
    quint64 disappearedFiles = 0;
    quint64 unreadableFiles = 0;
    quint64 changedDuringHash = 0;
    quint64 errors = 0;
};

struct DuplicateFinderResult
{
    QString rootPath;
    StorageScanState sourceScanState = StorageScanState::Idle;
    StorageScanStats sourceScanStats;
    bool isSourcePartial = false;
    QString sourcePartialReason;

    DuplicateFinderState finderState = DuplicateFinderState::Idle;
    DuplicateFinderStats finderStats;
    bool isFinderPartial = false;
    QString finderPartialReason;

    QList<DuplicateGroup> groups;

    quint64 totalRecoverableBytes() const
    {
        quint64 total = 0;
        for (const auto &g : groups) {
            total = saturatingAdd(total, g.recoverableBytes());
        }
        return total;
    }
};

class DuplicatePlanBuilder
{
public:
    static DuplicateCandidatePlan buildPlan(const QString &rootPath,
                                            const QList<StorageScanEntry> &entries)
    {
        DuplicateCandidatePlan plan;
        const QString cleanRoot = QDir::cleanPath(rootPath);

        // 1. Group regular files by physical identity: (deviceId, inode)
        struct PhysicalCollector {
            StorageScanEntry primaryEntry;
            QStringList paths;
        };
        QHash<QPair<quint64, quint64>, PhysicalCollector> physicalMap;

        for (const auto &entry : entries) {
            if (entry.type != StorageEntryType::RegularFile) {
                continue;
            }
            const QPair<quint64, quint64> id(entry.deviceId, entry.inode);
            auto it = physicalMap.find(id);
            if (it == physicalMap.end()) {
                PhysicalCollector col;
                col.primaryEntry = entry;
                col.paths.append(entry.path);
                physicalMap.insert(id, col);
            } else {
                it->paths.append(entry.path);
            }
        }

        plan.totalPhysicalFiles = static_cast<quint64>(physicalMap.size());

        // 2. Build DuplicatePhysicalFile list with deterministic canonical path
        QList<DuplicatePhysicalFile> physicalFiles;
        physicalFiles.reserve(physicalMap.size());

        for (auto it = physicalMap.begin(); it != physicalMap.end(); ++it) {
            auto &paths = it.value().paths;
            // Case-sensitive deterministic lexicographical sort
            std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) {
                return a < b;
            });

            DuplicatePhysicalFile phys;
            phys.deviceId = it.key().first;
            phys.inode = it.key().second;
            phys.canonicalPath = paths.first();
            for (qsizetype i = 1; i < paths.size(); ++i) {
                phys.aliasPaths.append(paths.at(i));
            }

            const auto &prim = it.value().primaryEntry;
            phys.logicalSize = prim.logicalSize;
            phys.allocatedSize = prim.allocatedSize;
            phys.linkCount = prim.linkCount;
            phys.mtimeSec = prim.mtimeSec;
            phys.mtimeNsec = prim.mtimeNsec;
            phys.ctimeSec = prim.ctimeSec;
            phys.ctimeNsec = prim.ctimeNsec;
            phys.name = phys.canonicalPath.section(QLatin1Char('/'), -1);

            QString parentDir = phys.canonicalPath.section(QLatin1Char('/'), 0, -2);
            if (parentDir.startsWith(cleanRoot)) {
                QString rel = parentDir.mid(cleanRoot.length());
                if (rel.startsWith(QLatin1Char('/'))) {
                    rel = rel.mid(1);
                }
                phys.relativePath = rel.isEmpty() ? QStringLiteral(".") : rel;
            } else {
                phys.relativePath = parentDir.isEmpty() ? QStringLiteral(".") : parentDir;
            }

            physicalFiles.append(std::move(phys));
        }

        // 3. Group physical files by exact logicalSize
        QHash<quint64, QList<DuplicatePhysicalFile>> sizeBuckets;
        for (const auto &file : physicalFiles) {
            sizeBuckets[file.logicalSize].append(file);
        }

        // 4. Filter size groups with >= 2 physical files
        for (auto it = sizeBuckets.begin(); it != sizeBuckets.end(); ++it) {
            auto &group = it.value();
            if (group.size() >= 2) {
                // Sort members within size group deterministically by canonicalPath ASC
                std::sort(group.begin(), group.end(), [](const DuplicatePhysicalFile &a, const DuplicatePhysicalFile &b) {
                    return a.canonicalPath < b.canonicalPath;
                });

                plan.candidateGroups++;
                plan.candidateFiles = saturatingAdd(plan.candidateFiles, static_cast<quint64>(group.size()));
                for (const auto &f : group) {
                    plan.candidateBytes = saturatingAdd(plan.candidateBytes, f.logicalSize);
                }
                plan.candidateSizeGroups.append(std::move(group));
            }
        }

        // 5. Deterministic order of candidate size groups:
        // Size DESC, tie: smallest canonicalPath ASC
        std::sort(plan.candidateSizeGroups.begin(), plan.candidateSizeGroups.end(),
                  [](const QList<DuplicatePhysicalFile> &a, const QList<DuplicatePhysicalFile> &b) {
                      if (a.first().logicalSize != b.first().logicalSize) {
                          return a.first().logicalSize > b.first().logicalSize; // Size DESC
                      }
                      return a.first().canonicalPath < b.first().canonicalPath; // Path ASC
                  });

        return plan;
    }
};

Q_DECLARE_METATYPE(DuplicateFinderState)
Q_DECLARE_METATYPE(DuplicatePhysicalFile)
Q_DECLARE_METATYPE(DuplicateGroup)
Q_DECLARE_METATYPE(DuplicateCandidatePlan)
Q_DECLARE_METATYPE(DuplicateFinderStats)
Q_DECLARE_METATYPE(DuplicateFinderResult)
