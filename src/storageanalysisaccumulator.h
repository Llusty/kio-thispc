/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "storageanalysisdata.h"
#include "storagescandata.h"

#include <QDir>
#include <QHash>
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>

class StorageAnalysisAccumulator
{
public:
    static StorageAnalysisResult analyze(const QString &rootPath,
                                         const QList<StorageScanEntry> &entries,
                                         const StorageScanStats &stats,
                                         StorageScanState scanState)
    {
        StorageAnalysisResult result;
        result.rootPath = rootPath;
        result.scanState = scanState;
        result.stats = stats;

        if (scanState == StorageScanState::Cancelled) {
            result.isPartial = true;
            result.partialReason = QStringLiteral("Wyniki częściowe — skanowanie anulowane");
        } else if (stats.inaccessible > 0 || stats.disappeared > 0 || stats.errors > 0 || stats.skippedMounts > 0) {
            result.isPartial = true;
            result.partialReason = QStringLiteral("Wyniki mogą być niepełne");
        }

        const QString cleanRoot = QDir::cleanPath(rootPath);

        // 1. Map hardlink aliases for regular files
        QHash<QPair<quint64, quint64>, QStringList> hardlinkPaths;
        for (const auto &entry : entries) {
            if (entry.type == StorageEntryType::RegularFile && entry.linkCount > 1) {
                hardlinkPaths[QPair<quint64, quint64>(entry.deviceId, entry.inode)].append(entry.path);
            }
        }

        // 2. Prepare Largest Files (Logical & Allocated)
        QSet<QPair<quint64, quint64>> seenInodesForAllocated;

        for (const auto &entry : entries) {
            if (entry.type != StorageEntryType::RegularFile) {
                continue;
            }

            StorageAnalysisFileEntry fileEntry;
            fileEntry.path = entry.path;
            fileEntry.name = entry.name;
            fileEntry.logicalSize = entry.logicalSize;
            fileEntry.allocatedSize = entry.allocatedSize;
            fileEntry.deviceId = entry.deviceId;
            fileEntry.inode = entry.inode;
            fileEntry.linkCount = entry.linkCount;
            fileEntry.isHardlink = (entry.linkCount > 1);
            fileEntry.isHidden = entry.isHidden;

            QString parentDir = entry.path.section(QLatin1Char('/'), 0, -2);
            if (parentDir.startsWith(cleanRoot)) {
                QString rel = parentDir.mid(cleanRoot.length());
                if (rel.startsWith(QLatin1Char('/'))) {
                    rel = rel.mid(1);
                }
                fileEntry.relativePath = rel.isEmpty() ? QStringLiteral(".") : rel;
            } else {
                fileEntry.relativePath = parentDir.isEmpty() ? QStringLiteral(".") : parentDir;
            }

            if (fileEntry.isHardlink) {
                const auto &all = hardlinkPaths.value(QPair<quint64, quint64>(entry.deviceId, entry.inode));
                for (const auto &p : all) {
                    if (p != entry.path) {
                        fileEntry.aliases.append(p);
                    }
                }
            }

            // Every pathname is included in logical ranking
            result.filesByLogical.append(fileEntry);

            // In allocated ranking, deduplicate by (deviceId, inode) to represent canonical physical object
            if (fileEntry.isHardlink) {
                const QPair<quint64, quint64> identity(entry.deviceId, entry.inode);
                if (!seenInodesForAllocated.contains(identity)) {
                    seenInodesForAllocated.insert(identity);
                    result.filesByAllocated.append(fileEntry);
                }
            } else {
                result.filesByAllocated.append(fileEntry);
            }
        }

        // Deterministic sorting for files
        auto fileLogicalCompare = [](const StorageAnalysisFileEntry &a, const StorageAnalysisFileEntry &b) {
            if (a.logicalSize != b.logicalSize) {
                return a.logicalSize > b.logicalSize; // descending
            }
            return a.path < b.path; // ascending tie-break
        };
        auto fileAllocatedCompare = [](const StorageAnalysisFileEntry &a, const StorageAnalysisFileEntry &b) {
            if (a.allocatedSize != b.allocatedSize) {
                return a.allocatedSize > b.allocatedSize; // descending
            }
            return a.path < b.path; // ascending tie-break
        };

        std::sort(result.filesByLogical.begin(), result.filesByLogical.end(), fileLogicalCompare);
        std::sort(result.filesByAllocated.begin(), result.filesByAllocated.end(), fileAllocatedCompare);

        // 3. Aggregate Directories
        struct DirNode {
            int id = -1;
            int parentId = -1;
            QString path;
            QString name;
            quint64 logicalSize = 0;
            quint64 singleLinkAllocatedSize = 0;
            quint64 allocatedSize = 0;
            quint64 filesCount = 0;
            quint64 dirsCount = 0;
            bool isMountBoundary = false;
            bool isHidden = false;
        };

        QVector<DirNode> nodes;
        QHash<QString, int> dirPathToId;

        // Ensure root is in dirPathToId
        if (!cleanRoot.isEmpty()) {
            DirNode rootNode;
            rootNode.id = 0;
            rootNode.path = cleanRoot;
            rootNode.name = cleanRoot.section(QLatin1Char('/'), -1);
            nodes.append(rootNode);
            dirPathToId.insert(cleanRoot, 0);
        }

        for (const auto &entry : entries) {
            if (entry.type == StorageEntryType::Directory) {
                const QString p = QDir::cleanPath(entry.path);
                auto it = dirPathToId.find(p);
                if (it == dirPathToId.end()) {
                    DirNode node;
                    node.id = nodes.size();
                    node.path = p;
                    node.name = entry.name;
                    node.isMountBoundary = entry.isMountBoundary;
                    node.isHidden = entry.isHidden;
                    nodes.append(node);
                    dirPathToId.insert(p, node.id);
                } else {
                    nodes[*it].isMountBoundary = entry.isMountBoundary;
                    nodes[*it].isHidden = entry.isHidden;
                }
            }
        }

        // Establish parent relationships
        for (int i = 0; i < nodes.size(); ++i) {
            if (nodes[i].path == cleanRoot) {
                nodes[i].parentId = -1;
                continue;
            }
            QString parentPath = nodes[i].path.section(QLatin1Char('/'), 0, -2);
            if (parentPath.isEmpty()) {
                parentPath = QStringLiteral("/");
            }
            auto it = dirPathToId.find(parentPath);
            if (it != dirPathToId.end()) {
                nodes[i].parentId = *it;
            } else {
                int rootId = dirPathToId.value(cleanRoot, -1);
                nodes[i].parentId = rootId;
            }
        }

        // Distribute files and symlinks
        struct MultiLinkFile {
            QPair<quint64, quint64> identity;
            quint64 allocatedSize = 0;
            QList<int> dirIds;
        };
        QHash<QPair<quint64, quint64>, int> multiLinkMap;
        QVector<MultiLinkFile> multiLinkList;

        for (const auto &entry : entries) {
            if (entry.type == StorageEntryType::Directory) {
                continue;
            }
            QString parentPath = entry.path.section(QLatin1Char('/'), 0, -2);
            if (parentPath.isEmpty()) {
                parentPath = QStringLiteral("/");
            }
            int dirId = dirPathToId.value(parentPath, -1);
            if (dirId < 0) {
                dirId = dirPathToId.value(cleanRoot, -1);
            }
            if (dirId >= 0 && dirId < nodes.size()) {
                nodes[dirId].logicalSize += entry.logicalSize;
                if (entry.type == StorageEntryType::RegularFile) {
                    nodes[dirId].filesCount += 1;
                }
                if (entry.linkCount <= 1) {
                    nodes[dirId].singleLinkAllocatedSize += entry.allocatedSize;
                } else {
                    const QPair<quint64, quint64> identity(entry.deviceId, entry.inode);
                    auto it = multiLinkMap.find(identity);
                    if (it == multiLinkMap.end()) {
                        MultiLinkFile ml;
                        ml.identity = identity;
                        ml.allocatedSize = entry.allocatedSize;
                        ml.dirIds.append(dirId);
                        const int idx = multiLinkList.size();
                        multiLinkList.append(ml);
                        multiLinkMap.insert(identity, idx);
                    } else {
                        multiLinkList[*it].dirIds.append(dirId);
                    }
                }
            }
        }

        // Post-order subtree accumulation for additive properties
        QVector<int> sortedDirIds(nodes.size());
        for (int i = 0; i < nodes.size(); ++i) {
            sortedDirIds[i] = i;
        }
        std::sort(sortedDirIds.begin(), sortedDirIds.end(), [&](int a, int b) {
            return nodes[a].path.count(QLatin1Char('/')) > nodes[b].path.count(QLatin1Char('/'));
        });

        for (int dirId : sortedDirIds) {
            int pId = nodes[dirId].parentId;
            if (pId >= 0 && pId < nodes.size() && pId != dirId) {
                nodes[pId].logicalSize += nodes[dirId].logicalSize;
                nodes[pId].singleLinkAllocatedSize += nodes[dirId].singleLinkAllocatedSize;
                nodes[pId].filesCount += nodes[dirId].filesCount;
                nodes[pId].dirsCount += 1 + nodes[dirId].dirsCount;
            }
        }

        // Deduplicate multi-link inodes per directory subtree
        QVector<int> visitedNodes;
        visitedNodes.reserve(32);
        QVector<bool> isVisited(nodes.size(), false);

        for (const auto &ml : multiLinkList) {
            visitedNodes.clear();
            for (int dId : ml.dirIds) {
                int cur = dId;
                while (cur >= 0 && cur < nodes.size()) {
                    if (isVisited[cur]) {
                        break;
                    }
                    isVisited[cur] = true;
                    visitedNodes.append(cur);
                    cur = nodes[cur].parentId;
                }
            }
            for (int vId : visitedNodes) {
                nodes[vId].allocatedSize += ml.allocatedSize;
                isVisited[vId] = false;
            }
        }

        for (int i = 0; i < nodes.size(); ++i) {
            nodes[i].allocatedSize += nodes[i].singleLinkAllocatedSize;
        }

        // Populate directory result entries, excluding scan root
        for (const auto &node : nodes) {
            if (node.path == cleanRoot) {
                continue; // Scan root is excluded from largest directories ranking
            }

            StorageAnalysisDirEntry dirEntry;
            dirEntry.path = node.path;
            dirEntry.name = node.name;
            if (node.path.startsWith(cleanRoot)) {
                QString rel = node.path.mid(cleanRoot.length());
                if (rel.startsWith(QLatin1Char('/'))) {
                    rel = rel.mid(1);
                }
                dirEntry.relativePath = rel.isEmpty() ? node.name : rel;
            } else {
                dirEntry.relativePath = node.path;
            }
            dirEntry.logicalSize = node.logicalSize;
            dirEntry.allocatedSize = node.allocatedSize;
            dirEntry.filesCount = node.filesCount;
            dirEntry.dirsCount = node.dirsCount;
            dirEntry.isMountBoundary = node.isMountBoundary;
            dirEntry.isHidden = node.isHidden;

            result.dirsByLogical.append(dirEntry);
            result.dirsByAllocated.append(dirEntry);
        }

        auto dirLogicalCompare = [](const StorageAnalysisDirEntry &a, const StorageAnalysisDirEntry &b) {
            if (a.logicalSize != b.logicalSize) {
                return a.logicalSize > b.logicalSize; // descending
            }
            return a.path < b.path; // ascending tie-break
        };
        auto dirAllocatedCompare = [](const StorageAnalysisDirEntry &a, const StorageAnalysisDirEntry &b) {
            if (a.allocatedSize != b.allocatedSize) {
                return a.allocatedSize > b.allocatedSize; // descending
            }
            return a.path < b.path; // ascending tie-break
        };

        std::sort(result.dirsByLogical.begin(), result.dirsByLogical.end(), dirLogicalCompare);
        std::sort(result.dirsByAllocated.begin(), result.dirsByAllocated.end(), dirAllocatedCompare);

        return result;
    }
};
