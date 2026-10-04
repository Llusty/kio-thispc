/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "storagescandata.h"

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

enum class StorageAnalysisRankingMode
{
    LogicalDescending,
    AllocatedDescending
};

struct StorageAnalysisFileEntry
{
    QString path;
    QString relativePath;
    QString name;
    quint64 logicalSize = 0;
    quint64 allocatedSize = 0;
    quint64 deviceId = 0;
    quint64 inode = 0;
    quint32 linkCount = 1;
    bool isHardlink = false;
    bool isHidden = false;
    QStringList aliases;
};

struct StorageAnalysisDirEntry
{
    QString path;
    QString relativePath;
    QString name;
    quint64 logicalSize = 0;
    quint64 allocatedSize = 0;
    quint64 filesCount = 0;
    quint64 dirsCount = 0;
    bool isMountBoundary = false;
    bool isHidden = false;
};

struct StorageAnalysisResult
{
    QString rootPath;
    StorageScanState scanState = StorageScanState::Idle;
    StorageScanStats stats;
    bool isPartial = false;
    QString partialReason;

    QList<StorageAnalysisFileEntry> filesByLogical;
    QList<StorageAnalysisFileEntry> filesByAllocated;
    QList<StorageAnalysisDirEntry> dirsByLogical;
    QList<StorageAnalysisDirEntry> dirsByAllocated;
};

Q_DECLARE_METATYPE(StorageAnalysisRankingMode)
Q_DECLARE_METATYPE(StorageAnalysisFileEntry)
Q_DECLARE_METATYPE(StorageAnalysisDirEntry)
Q_DECLARE_METATYPE(StorageAnalysisResult)
