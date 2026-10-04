/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "storageanalysisdata.h"

#include <QAbstractTableModel>
#include <QList>
#include <QString>

class StorageFilesTableModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        ColName = 0,
        ColLocation,
        ColLogicalSize,
        ColAllocatedSize,
        ColStatus,
        ColumnCount
    };

    explicit StorageFilesTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    void setResult(const StorageAnalysisResult &result)
    {
        beginResetModel();
        m_result = result;
        endResetModel();
    }

    void setLimit(int limit)
    {
        if (m_limit == limit) {
            return;
        }
        beginResetModel();
        m_limit = limit > 0 ? limit : 50;
        endResetModel();
    }

    int limit() const { return m_limit; }

    void setRankingMode(StorageAnalysisRankingMode mode)
    {
        if (m_mode == mode) {
            return;
        }
        beginResetModel();
        m_mode = mode;
        endResetModel();
    }

    StorageAnalysisRankingMode rankingMode() const { return m_mode; }

    const StorageAnalysisFileEntry *entryAt(int row) const
    {
        const auto &items = activeList();
        if (row >= 0 && row < items.size() && row < m_limit) {
            return &items[row];
        }
        return nullptr;
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid()) {
            return 0;
        }
        const auto &items = activeList();
        return std::min(m_limit, static_cast<int>(items.size()));
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid()) {
            return 0;
        }
        return ColumnCount;
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid()) {
            return {};
        }

        const auto *entry = entryAt(index.row());
        if (!entry) {
            return {};
        }

        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case ColName:
                return entry->name;
            case ColLocation:
                return entry->relativePath;
            case ColLogicalSize:
                return StorageScanStats::formatBytes(entry->logicalSize);
            case ColAllocatedSize:
                return StorageScanStats::formatBytes(entry->allocatedSize);
            case ColStatus:
                if (entry->isHardlink) {
                    return trLocal(
                        QStringLiteral("Hardlink (%1 dowiązań)").arg(entry->linkCount).toUtf8().constData(),
                        QStringLiteral("Hardlink (%1 links)").arg(entry->linkCount).toUtf8().constData());
                }
                return trLocal("Plik zwykły", "Regular file");
            default:
                break;
            }
        } else if (role == Qt::ToolTipRole) {
            QString tip = entry->path;
            if (entry->isHardlink && !entry->aliases.isEmpty()) {
                tip += trLocal("\nAliasy hardlinków:\n", "\nHardlink aliases:\n");
                tip += entry->aliases.join(QLatin1Char('\n'));
            }
            return tip;
        } else if (role == Qt::TextAlignmentRole) {
            if (index.column() == ColLogicalSize || index.column() == ColAllocatedSize) {
                return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
            }
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }

        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }

        switch (section) {
        case ColName:
            return trLocal("Nazwa", "Name");
        case ColLocation:
            return trLocal("Lokalizacja", "Location");
        case ColLogicalSize:
            return trLocal("Rozmiar logiczny", "Logical size");
        case ColAllocatedSize:
            return trLocal("Rozmiar na dysku", "Allocated size");
        case ColStatus:
            return trLocal("Status", "Status");
        default:
            break;
        }
        return {};
    }

private:
    const QList<StorageAnalysisFileEntry> &activeList() const
    {
        return m_mode == StorageAnalysisRankingMode::LogicalDescending
            ? m_result.filesByLogical
            : m_result.filesByAllocated;
    }

    StorageAnalysisResult m_result;
    StorageAnalysisRankingMode m_mode = StorageAnalysisRankingMode::LogicalDescending;
    int m_limit = 50;
};

class StorageDirsTableModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        ColName = 0,
        ColLocation,
        ColLogicalSize,
        ColAllocatedSize,
        ColFilesCount,
        ColDirsCount,
        ColStatus,
        ColumnCount
    };

    explicit StorageDirsTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    void setResult(const StorageAnalysisResult &result)
    {
        beginResetModel();
        m_result = result;
        endResetModel();
    }

    void setLimit(int limit)
    {
        if (m_limit == limit) {
            return;
        }
        beginResetModel();
        m_limit = limit > 0 ? limit : 50;
        endResetModel();
    }

    int limit() const { return m_limit; }

    void setRankingMode(StorageAnalysisRankingMode mode)
    {
        if (m_mode == mode) {
            return;
        }
        beginResetModel();
        m_mode = mode;
        endResetModel();
    }

    StorageAnalysisRankingMode rankingMode() const { return m_mode; }

    const StorageAnalysisDirEntry *entryAt(int row) const
    {
        const auto &items = activeList();
        if (row >= 0 && row < items.size() && row < m_limit) {
            return &items[row];
        }
        return nullptr;
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid()) {
            return 0;
        }
        const auto &items = activeList();
        return std::min(m_limit, static_cast<int>(items.size()));
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid()) {
            return 0;
        }
        return ColumnCount;
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid()) {
            return {};
        }

        const auto *entry = entryAt(index.row());
        if (!entry) {
            return {};
        }

        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case ColName:
                return entry->name;
            case ColLocation:
                return entry->relativePath;
            case ColLogicalSize:
                if (entry->isMountBoundary) {
                    return trLocal("— (pominięty)", "— (skipped)");
                }
                return StorageScanStats::formatBytes(entry->logicalSize);
            case ColAllocatedSize:
                if (entry->isMountBoundary) {
                    return trLocal("— (pominięty)", "— (skipped)");
                }
                return StorageScanStats::formatBytes(entry->allocatedSize);
            case ColFilesCount:
                if (entry->isMountBoundary) {
                    return QStringLiteral("—");
                }
                return QString::number(entry->filesCount);
            case ColDirsCount:
                if (entry->isMountBoundary) {
                    return QStringLiteral("—");
                }
                return QString::number(entry->dirsCount);
            case ColStatus:
                if (entry->isMountBoundary) {
                    return trLocal("Inny system plików (pominięty)", "Foreign filesystem (skipped)");
                }
                return trLocal("Katalog", "Directory");
            default:
                break;
            }
        } else if (role == Qt::ToolTipRole) {
            QString tip = entry->path;
            if (entry->isMountBoundary) {
                tip += trLocal("\nGranica punktu montowania — pominięto skanowanie innego systemu plików.",
                               "\nMount boundary — scanning of foreign filesystem was skipped.");
            }
            return tip;
        } else if (role == Qt::TextAlignmentRole) {
            if (index.column() == ColLogicalSize || index.column() == ColAllocatedSize ||
                index.column() == ColFilesCount || index.column() == ColDirsCount) {
                return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
            }
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }

        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }

        switch (section) {
        case ColName:
            return trLocal("Nazwa", "Name");
        case ColLocation:
            return trLocal("Lokalizacja", "Location");
        case ColLogicalSize:
            return trLocal("Rozmiar logiczny", "Logical size");
        case ColAllocatedSize:
            return trLocal("Rozmiar na dysku", "Allocated size");
        case ColFilesCount:
            return trLocal("Pliki", "Files");
        case ColDirsCount:
            return trLocal("Podkatalogi", "Subdirectories");
        case ColStatus:
            return trLocal("Status", "Status");
        default:
            break;
        }
        return {};
    }

private:
    const QList<StorageAnalysisDirEntry> &activeList() const
    {
        return m_mode == StorageAnalysisRankingMode::LogicalDescending
            ? m_result.dirsByLogical
            : m_result.dirsByAllocated;
    }

    StorageAnalysisResult m_result;
    StorageAnalysisRankingMode m_mode = StorageAnalysisRankingMode::LogicalDescending;
    int m_limit = 50;
};
