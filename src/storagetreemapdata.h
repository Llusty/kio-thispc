/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "storagescandata.h"

#include <QList>
#include <QMetaType>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <memory>

enum class StorageTreemapMetric
{
    Allocated, // "Rozmiar na dysku" (default)
    Logical    // "Rozmiar logiczny"
};

enum class StorageTreemapItemType
{
    Directory,
    RegularFile
};

struct StorageTreemapNode
{
    QString path;
    QString name;
    StorageTreemapItemType type = StorageTreemapItemType::RegularFile;

    // Sizing
    quint64 logicalSize = 0;           // Pathname logical extent (st_size)
    quint64 allocatedSize = 0;         // Partition invariant allocated size (canonical for files, aggregated for dirs)
    quint64 rawAllocatedSize = 0;      // Physical allocation from stat (st_blocks * 512) before canonical filter

    // Physical identity & Hardlinks
    quint64 deviceId = 0;
    quint64 inode = 0;
    quint32 linkCount = 1;
    bool isHardlink = false;
    bool isCanonicalHardlink = true;   // True if this is the lexicographically first alias
    QStringList hardlinkAliases;       // All other paths sharing (deviceId, inode)

    // Attributes
    bool isHidden = false;
    bool isMountBoundary = false;

    // Hierarchy pointers
    StorageTreemapNode *parent = nullptr;
    QList<std::shared_ptr<StorageTreemapNode>> children;

    [[nodiscard]] quint64 metricSize(StorageTreemapMetric metric) const
    {
        return (metric == StorageTreemapMetric::Allocated) ? allocatedSize : logicalSize;
    }

    [[nodiscard]] bool isDirectory() const
    {
        return type == StorageTreemapItemType::Directory;
    }
};

struct StorageTreemapLayoutRect
{
    QRectF rect;
    const StorageTreemapNode *node = nullptr;
    quint64 size = 0;
    double percentageOfRoot = 0.0;
    QString label;
    QString formattedSize;
    bool showLabel = true;
};

Q_DECLARE_METATYPE(StorageTreemapMetric)
Q_DECLARE_METATYPE(StorageTreemapItemType)
