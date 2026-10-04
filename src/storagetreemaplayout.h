/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "storagescandata.h"
#include "storagetreemapdata.h"

#include <QDir>
#include <QHash>
#include <QList>
#include <QPair>
#include <QRectF>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <memory>

class StorageTreemapBuilder
{
public:
    // Pure in-memory construction from snapshot. ZERO filesystem access.
    static std::shared_ptr<StorageTreemapNode> buildTree(const QString &rootPath,
                                                         const QList<StorageScanEntry> &entries,
                                                         const StorageScanStats &stats,
                                                         StorageScanState scanState)
    {
        Q_UNUSED(stats);
        Q_UNUSED(scanState);

        const QString cleanRoot = QDir::cleanPath(rootPath.isEmpty() ? QStringLiteral("/") : rootPath);

        auto rootNode = std::make_shared<StorageTreemapNode>();
        rootNode->path = cleanRoot;
        rootNode->name = cleanRoot.section(QLatin1Char('/'), -1);
        if (rootNode->name.isEmpty()) {
            rootNode->name = cleanRoot;
        }
        rootNode->type = StorageTreemapItemType::Directory;

        QHash<QString, std::shared_ptr<StorageTreemapNode>> dirMap;
        dirMap.insert(cleanRoot, rootNode);

        // 1. Collect and register all directories
        for (const auto &entry : entries) {
            if (entry.type == StorageEntryType::Directory) {
                const QString p = QDir::cleanPath(entry.path);
                if (dirMap.contains(p)) {
                    auto existing = dirMap.value(p);
                    existing->isMountBoundary = entry.isMountBoundary;
                    existing->isHidden = entry.isHidden;
                    continue;
                }
                auto dirNode = std::make_shared<StorageTreemapNode>();
                dirNode->path = p;
                dirNode->name = entry.name.isEmpty() ? p.section(QLatin1Char('/'), -1) : entry.name;
                dirNode->type = StorageTreemapItemType::Directory;
                dirNode->deviceId = entry.deviceId;
                dirNode->inode = entry.inode;
                dirNode->isMountBoundary = entry.isMountBoundary;
                dirNode->isHidden = entry.isHidden;
                dirMap.insert(p, dirNode);
            }
        }

        // 2. Link directory parent-child hierarchy
        for (auto it = dirMap.begin(); it != dirMap.end(); ++it) {
            const QString &p = it.key();
            if (p == cleanRoot) {
                continue;
            }
            QString parentPath = p.section(QLatin1Char('/'), 0, -2);
            if (parentPath.isEmpty()) {
                parentPath = QStringLiteral("/");
            }
            auto parentNode = dirMap.value(parentPath, rootNode);
            it.value()->parent = parentNode.get();
            parentNode->children.append(it.value());
        }

        // 3. Map hardlinks to identify deterministic canonical ownership
        // Inode key -> sorted list of paths
        QHash<QPair<quint64, quint64>, QStringList> hardlinks;
        for (const auto &entry : entries) {
            if (entry.type == StorageEntryType::RegularFile && entry.linkCount > 1) {
                hardlinks[QPair<quint64, quint64>(entry.deviceId, entry.inode)].append(entry.path);
            }
        }
        for (auto it = hardlinks.begin(); it != hardlinks.end(); ++it) {
            std::sort(it.value().begin(), it.value().end());
        }

        // 4. Create file nodes and attach to parent directories
        for (const auto &entry : entries) {
            if (entry.type != StorageEntryType::RegularFile) {
                continue; // Skip directories, symlinks, and special files from storage rectangles
            }

            auto fileNode = std::make_shared<StorageTreemapNode>();
            fileNode->path = entry.path;
            fileNode->name = entry.name.isEmpty() ? entry.path.section(QLatin1Char('/'), -1) : entry.name;
            fileNode->type = StorageEntryType::RegularFile == entry.type
                ? StorageTreemapItemType::RegularFile
                : StorageTreemapItemType::Directory;
            fileNode->logicalSize = entry.logicalSize;
            fileNode->rawAllocatedSize = entry.allocatedSize;
            fileNode->deviceId = entry.deviceId;
            fileNode->inode = entry.inode;
            fileNode->linkCount = entry.linkCount;
            fileNode->isHidden = entry.isHidden;
            fileNode->isHardlink = (entry.linkCount > 1);

            if (fileNode->isHardlink) {
                const auto &all = hardlinks.value(QPair<quint64, quint64>(entry.deviceId, entry.inode));
                const QString canonicalPath = all.isEmpty() ? entry.path : all.first();
                fileNode->isCanonicalHardlink = (entry.path == canonicalPath);
                // Partition invariant: Only canonical alias contributes allocated size to treemap geometry
                fileNode->allocatedSize = fileNode->isCanonicalHardlink ? entry.allocatedSize : 0;

                for (const auto &alias : all) {
                    if (alias != entry.path) {
                        fileNode->hardlinkAliases.append(alias);
                    }
                }
            } else {
                fileNode->isCanonicalHardlink = true;
                fileNode->allocatedSize = entry.allocatedSize;
            }

            QString parentPath = entry.path.section(QLatin1Char('/'), 0, -2);
            if (parentPath.isEmpty()) {
                parentPath = QStringLiteral("/");
            }
            auto parentDir = dirMap.value(parentPath, rootNode);
            fileNode->parent = parentDir.get();
            parentDir->children.append(fileNode);
        }

        // 5. Iterative bottom-up post-order aggregation by slash count (no recursion stack overflow)
        QVector<std::shared_ptr<StorageTreemapNode>> allDirs;
        allDirs.reserve(dirMap.size());
        for (auto it = dirMap.begin(); it != dirMap.end(); ++it) {
            allDirs.append(it.value());
        }

        std::sort(allDirs.begin(), allDirs.end(), [](const std::shared_ptr<StorageTreemapNode> &a,
                                                    const std::shared_ptr<StorageTreemapNode> &b) {
            return a->path.count(QLatin1Char('/')) > b->path.count(QLatin1Char('/'));
        });

        for (const auto &dir : allDirs) {
            quint64 sumLogical = 0;
            quint64 sumAllocated = 0;
            for (const auto &child : dir->children) {
                sumLogical += child->logicalSize;
                sumAllocated += child->allocatedSize;
            }
            dir->logicalSize = sumLogical;
            dir->allocatedSize = sumAllocated;
        }

        return rootNode;
    }
};

class StorageTreemapLayout
{
public:
    // Pure in-memory Squarified Treemap layout algorithm (Bruls, Huizing, van Wijk).
    static QList<StorageTreemapLayoutRect> layoutSquarified(const StorageTreemapNode *visualRoot,
                                                            const QRectF &bounds,
                                                            StorageTreemapMetric metric)
    {
        QList<StorageTreemapLayoutRect> result;
        if (!visualRoot || bounds.width() <= 0.0 || bounds.height() <= 0.0) {
            return result;
        }

        // Collect candidate children with non-zero metric size
        struct ItemCandidate {
            const StorageTreemapNode *node = nullptr;
            quint64 size = 0;
        };

        QList<ItemCandidate> candidates;
        quint64 totalSize = 0;

        for (const auto &child : visualRoot->children) {
            const quint64 sz = child->metricSize(metric);
            if (sz > 0) {
                candidates.append({child.get(), sz});
                totalSize += sz;
            }
        }

        if (candidates.isEmpty() || totalSize == 0) {
            return result;
        }

        // Deterministic sort: size descending, path ascending (case-sensitive) tie-breaker
        std::sort(candidates.begin(), candidates.end(), [](const ItemCandidate &a, const ItemCandidate &b) {
            if (a.size != b.size) {
                return a.size > b.size;
            }
            return a.node->path < b.node->path;
        });

        const qreal totalArea = bounds.width() * bounds.height();
        if (totalArea <= 0.0) {
            return result;
        }

        // Normalized area for each item: area_i = (size_i / totalSize) * totalArea
        QVector<qreal> areas(candidates.size());
        for (int i = 0; i < candidates.size(); ++i) {
            areas[i] = (static_cast<qreal>(candidates[i].size) / static_cast<qreal>(totalSize)) * totalArea;
        }

        QRectF currentBounds = bounds;
        QVector<int> currentRow;
        qreal currentRowArea = 0.0;

        auto worstAspectRatio = [](const QVector<int> &row, const QVector<qreal> &allAreas,
                                   qreal rowArea, qreal sideLength) -> qreal {
            if (row.isEmpty() || rowArea <= 0.0 || sideLength <= 0.0) {
                return 1e9;
            }
            const qreal s2 = rowArea * rowArea;
            const qreal side2 = sideLength * sideLength;
            qreal maxRatio = 0.0;
            for (int idx : row) {
                const qreal a = allAreas[idx];
                if (a <= 0.0) continue;
                const qreal r1 = (side2 * a) / s2;
                const qreal r2 = s2 / (side2 * a);
                const qreal r = (r1 > r2) ? r1 : r2;
                if (r > maxRatio) {
                    maxRatio = r;
                }
            }
            return maxRatio;
        };

        auto layoutRow = [&](const QVector<int> &row, qreal rowArea, QRectF &remaining) {
            if (row.isEmpty() || rowArea <= 0.0) {
                return;
            }
            const bool horizontal = (remaining.width() >= remaining.height());
            const qreal primarySide = horizontal ? remaining.height() : remaining.width();
            if (primarySide <= 0.0) {
                return;
            }

            const qreal thickness = rowArea / primarySide;

            qreal offset = 0.0;
            for (int idx : row) {
                const qreal itemArea = areas[idx];
                const qreal itemLength = (thickness > 0.0) ? (itemArea / thickness) : 0.0;

                QRectF itemRect;
                if (horizontal) {
                    itemRect = QRectF(remaining.x(), remaining.y() + offset, thickness, itemLength);
                } else {
                    itemRect = QRectF(remaining.x() + offset, remaining.y(), itemLength, thickness);
                }
                offset += itemLength;

                // Clamp to boundary to guarantee child contained in bounds and non-negative
                qreal rx = std::max(remaining.x(), itemRect.x());
                qreal ry = std::max(remaining.y(), itemRect.y());
                qreal rw = std::max<qreal>(0.0, std::min(bounds.right() - rx, itemRect.width()));
                qreal rh = std::max<qreal>(0.0, std::min(bounds.bottom() - ry, itemRect.height()));
                itemRect = QRectF(rx, ry, rw, rh);

                const auto &cand = candidates[idx];
                StorageTreemapLayoutRect r;
                r.rect = itemRect;
                r.node = cand.node;
                r.size = cand.size;
                r.percentageOfRoot = (totalSize > 0)
                    ? ((static_cast<double>(cand.size) * 100.0) / static_cast<double>(totalSize))
                    : 0.0;
                r.formattedSize = StorageScanStats::formatBytes(cand.size);
                r.label = cand.node->name;
                r.showLabel = (itemRect.width() >= 40.0 && itemRect.height() >= 18.0);

                result.append(r);
            }

            if (horizontal) {
                remaining.setLeft(remaining.left() + thickness);
            } else {
                remaining.setTop(remaining.top() + thickness);
            }
        };

        for (int i = 0; i < candidates.size(); ++i) {
            const qreal side = std::min(currentBounds.width(), currentBounds.height());
            if (side <= 0.0) {
                break;
            }

            QVector<int> testRow = currentRow;
            testRow.append(i);
            const qreal testArea = currentRowArea + areas[i];

            if (currentRow.isEmpty()) {
                currentRow = testRow;
                currentRowArea = testArea;
            } else {
                const qreal currentWorst = worstAspectRatio(currentRow, areas, currentRowArea, side);
                const qreal newWorst = worstAspectRatio(testRow, areas, testArea, side);

                if (newWorst <= currentWorst) {
                    currentRow = testRow;
                    currentRowArea = testArea;
                } else {
                    layoutRow(currentRow, currentRowArea, currentBounds);
                    currentRow.clear();
                    currentRow.append(i);
                    currentRowArea = areas[i];
                }
            }
        }

        if (!currentRow.isEmpty()) {
            layoutRow(currentRow, currentRowArea, currentBounds);
        }

        return result;
    }
};
