/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "duplicateactiondata.h"
#include "duplicatefinderdata.h"
#include "storagescandata.h"

#include <QAbstractItemModel>
#include <QSet>
#include <QUrl>

class DuplicateGroupModel final : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Roles
    {
        IsGroupRole = Qt::UserRole + 1,
        CanonicalPathRole,
        FileUrlRole,
        Sha256Role,
        AliasPathsRole,
        LogicalSizeRole,
        AllocatedSizeRole,
        GroupIndexRole,
        MemberIndexRole
    };

    explicit DuplicateGroupModel(QObject *parent = nullptr)
        : QAbstractItemModel(parent)
    {
    }

    void setResult(const DuplicateFinderResult &result)
    {
        beginResetModel();
        m_result = result;
        m_selectedKeys.clear();
        endResetModel();
        Q_EMIT selectionChanged();
    }

    void clear()
    {
        beginResetModel();
        m_result = DuplicateFinderResult();
        m_selectedKeys.clear();
        endResetModel();
        Q_EMIT selectionChanged();
    }

    const DuplicateFinderResult &result() const { return m_result; }
    const QList<DuplicateGroup> &groups() const { return m_result.groups; }

    const DuplicatePhysicalFile *fileForIndex(const QModelIndex &index) const
    {
        if (!index.isValid() || index.internalId() == 0) {
            return nullptr;
        }
        const int gIdx = static_cast<int>(index.internalId() - 1);
        const int mIdx = index.row();
        if (gIdx >= 0 && gIdx < m_result.groups.size()) {
            const auto &grp = m_result.groups.at(gIdx);
            if (mIdx >= 0 && mIdx < grp.files.size()) {
                return &grp.files.at(mIdx);
            }
        }
        return nullptr;
    }

    // --- Selection API ---

    void clearSelection()
    {
        if (m_selectedKeys.isEmpty()) {
            return;
        }
        m_selectedKeys.clear();
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::CheckStateRole});
        Q_EMIT selectionChanged();
    }

    bool isSelected(const DuplicatePhysicalFile &file) const
    {
        const QPair<quint64, quint64> key(file.deviceId, file.inode);
        return m_selectedKeys.contains(key);
    }

    void setSelected(const DuplicatePhysicalFile &file, bool selected)
    {
        const QPair<quint64, quint64> key(file.deviceId, file.inode);
        if (selected) {
            m_selectedKeys.insert(key);
        } else {
            m_selectedKeys.remove(key);
        }
    }

    int selectedCount() const
    {
        return m_selectedKeys.size();
    }

    quint64 selectedLogicalBytes() const
    {
        quint64 total = 0;
        for (const auto &grp : m_result.groups) {
            for (const auto &file : grp.files) {
                if (isSelected(file)) {
                    total = saturatingAdd(total, file.logicalSize);
                }
            }
        }
        return total;
    }

    quint64 selectedAllocatedBytes() const
    {
        quint64 total = 0;
        for (const auto &grp : m_result.groups) {
            for (const auto &file : grp.files) {
                if (isSelected(file)) {
                    total = saturatingAdd(total, file.allocatedSize);
                }
            }
        }
        return total;
    }

    QList<DuplicateActionItem> selectedActionItems() const
    {
        QList<DuplicateActionItem> items;
        for (int gIdx = 0; gIdx < m_result.groups.size(); ++gIdx) {
            const auto &grp = m_result.groups.at(gIdx);
            for (const auto &file : grp.files) {
                if (isSelected(file)) {
                    DuplicateActionItem item;
                    item.deviceId = file.deviceId;
                    item.inode = file.inode;
                    item.targetPath = file.canonicalPath; // The specific chosen action target path
                    item.fileSnapshot = file;
                    item.groupIndex = gIdx;
                    items.append(item);
                }
            }
        }
        return items;
    }

    // --- Hardlink-aware model update after action ---

    void applySuccessfulTrash(const QSet<QString> &trashedPaths)
    {
        if (trashedPaths.isEmpty()) return;

        beginResetModel();
        QList<DuplicateGroup> newGroups;

        for (const auto &grp : m_result.groups) {
            QList<DuplicatePhysicalFile> remainingFiles;

            for (const auto &file : grp.files) {
                if (trashedPaths.contains(file.canonicalPath)) {
                    // Item was acted upon: clear selection
                    const QPair<quint64, quint64> key(file.deviceId, file.inode);
                    m_selectedKeys.remove(key);

                    // Canonical path was removed.
                    // Check if any hardlink alias paths remain!
                    if (!file.aliasPaths.isEmpty()) {
                        // Alias paths remain -> physical object still exists!
                        // Promote smallest alias to canonicalPath deterministically.
                        QStringList newAliases = file.aliasPaths;
                        std::sort(newAliases.begin(), newAliases.end());
                        DuplicatePhysicalFile updatedFile = file;
                        updatedFile.canonicalPath = newAliases.takeFirst();
                        updatedFile.aliasPaths = newAliases;
                        updatedFile.name = updatedFile.canonicalPath.section(QLatin1Char('/'), -1);
                        remainingFiles.append(updatedFile);
                    }
                } else {
                    // Canonical path not trashed. But what if one of aliasPaths was trashed?
                    QStringList remainingAliases;
                    for (const auto &alias : file.aliasPaths) {
                        if (!trashedPaths.contains(alias)) {
                            remainingAliases.append(alias);
                        }
                    }
                    DuplicatePhysicalFile updatedFile = file;
                    updatedFile.aliasPaths = remainingAliases;
                    remainingFiles.append(updatedFile);
                }
            }

            // Only retain groups with at least 2 distinct physical objects
            if (remainingFiles.size() >= 2) {
                DuplicateGroup updatedGrp = grp;
                updatedGrp.files = remainingFiles;
                newGroups.append(updatedGrp);
            } else {
                // Group collapsed: remove selection for remaining file
                for (const auto &f : remainingFiles) {
                    const QPair<quint64, quint64> key(f.deviceId, f.inode);
                    m_selectedKeys.remove(key);
                }
            }
        }

        m_result.groups = newGroups;
        endResetModel();
        Q_EMIT selectionChanged();
    }

    void applySuccessfulMove(const QHash<QString, QString> &movedPaths, const QString &scanRoot)
    {
        if (movedPaths.isEmpty()) return;

        beginResetModel();
        const QString cleanRoot = QDir::cleanPath(scanRoot);
        const QString cleanRootWithSlash = cleanRoot.endsWith(QLatin1Char('/')) ? cleanRoot : cleanRoot + QLatin1Char('/');
        QList<DuplicateGroup> newGroups;

        for (const auto &grp : m_result.groups) {
            QList<DuplicatePhysicalFile> remainingFiles;

            for (const auto &file : grp.files) {
                if (movedPaths.contains(file.canonicalPath)) {
                    const QPair<quint64, quint64> key(file.deviceId, file.inode);
                    m_selectedKeys.remove(key);

                    const QString destPath = movedPaths.value(file.canonicalPath);
                    const bool movedInsideRoot = !cleanRoot.isEmpty() &&
                        (destPath == cleanRoot || destPath.startsWith(cleanRootWithSlash));

                    if (movedInsideRoot) {
                        // File moved within scan root: update canonicalPath and retain in group
                        DuplicatePhysicalFile updatedFile = file;
                        updatedFile.canonicalPath = destPath;
                        updatedFile.name = destPath.section(QLatin1Char('/'), -1);
                        struct stat st {};
                        if (::lstat(QFile::encodeName(destPath).constData(), &st) == 0) {
                            updatedFile.mtimeSec = st.st_mtim.tv_sec;
                            updatedFile.mtimeNsec = st.st_mtim.tv_nsec;
                            updatedFile.ctimeSec = st.st_ctim.tv_sec;
                            updatedFile.ctimeNsec = st.st_ctim.tv_nsec;
                            updatedFile.deviceId = st.st_dev;
                            updatedFile.inode = st.st_ino;
                        }
                        remainingFiles.append(updatedFile);
                    } else {
                        // File moved outside scan root:
                        // If alias paths remain, promote alias
                        if (!file.aliasPaths.isEmpty()) {
                            QStringList newAliases = file.aliasPaths;
                            std::sort(newAliases.begin(), newAliases.end());
                            DuplicatePhysicalFile updatedFile = file;
                            updatedFile.canonicalPath = newAliases.takeFirst();
                            updatedFile.aliasPaths = newAliases;
                            updatedFile.name = updatedFile.canonicalPath.section(QLatin1Char('/'), -1);
                            remainingFiles.append(updatedFile);
                        }
                    }
                } else {
                    remainingFiles.append(file);
                }
            }

            if (remainingFiles.size() >= 2) {
                DuplicateGroup updatedGrp = grp;
                updatedGrp.files = remainingFiles;
                newGroups.append(updatedGrp);
            } else {
                for (const auto &f : remainingFiles) {
                    const QPair<quint64, quint64> key(f.deviceId, f.inode);
                    m_selectedKeys.remove(key);
                }
            }
        }

        m_result.groups = newGroups;
        endResetModel();
        Q_EMIT selectionChanged();
    }

    // --- QAbstractItemModel overrides ---

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid()) {
            return Qt::NoItemFlags;
        }

        Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (index.column() == 0) {
            f |= Qt::ItemIsUserCheckable;
            if (index.internalId() == 0) {
                f |= Qt::ItemIsAutoTristate;
            }
        }
        return f;
    }

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override
    {
        if (column < 0 || column >= 6 || row < 0) {
            return QModelIndex();
        }

        if (!parent.isValid()) {
            // Group item
            if (row < m_result.groups.size()) {
                return createIndex(row, column, quintptr(0));
            }
            return QModelIndex();
        }

        // Child member item
        if (parent.internalId() == 0) {
            const int gIdx = parent.row();
            if (gIdx >= 0 && gIdx < m_result.groups.size()) {
                if (row < m_result.groups.at(gIdx).files.size()) {
                    return createIndex(row, column, quintptr(gIdx + 1));
                }
            }
        }

        return QModelIndex();
    }

    QModelIndex parent(const QModelIndex &index) const override
    {
        if (!index.isValid()) {
            return QModelIndex();
        }

        const quintptr id = index.internalId();
        if (id == 0) {
            // Group item has no parent
            return QModelIndex();
        }

        const int gIdx = static_cast<int>(id - 1);
        if (gIdx >= 0 && gIdx < m_result.groups.size()) {
            return createIndex(gIdx, 0, quintptr(0));
        }

        return QModelIndex();
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (!parent.isValid()) {
            return static_cast<int>(m_result.groups.size());
        }

        if (parent.internalId() == 0) {
            const int gIdx = parent.row();
            if (gIdx >= 0 && gIdx < m_result.groups.size()) {
                return static_cast<int>(m_result.groups.at(gIdx).files.size());
            }
        }

        return 0;
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        Q_UNUSED(parent);
        return 6;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QVariant();
        }

        switch (section) {
        case 0: return tr("Plik / Grupa");
        case 1: return tr("Ścieżka");
        case 2: return tr("Rozmiar");
        case 3: return tr("Rozmiar na dysku");
        case 4: return tr("Odzyskiwalne miejsce");
        case 5: return tr("SHA-256");
        default: return QVariant();
        }
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid()) {
            return QVariant();
        }

        const quintptr id = index.internalId();

        if (id == 0) {
            // Group Row
            const int gIdx = index.row();
            if (gIdx < 0 || gIdx >= m_result.groups.size()) {
                return QVariant();
            }
            const auto &grp = m_result.groups.at(gIdx);

            if (role == IsGroupRole) return true;
            if (role == GroupIndexRole) return gIdx;
            if (role == Sha256Role) return grp.sha256;
            if (role == LogicalSizeRole) return grp.logicalSize;

            if (role == Qt::CheckStateRole && index.column() == 0) {
                int checkedCount = 0;
                for (const auto &f : grp.files) {
                    if (isSelected(f)) {
                        checkedCount++;
                    }
                }
                if (checkedCount == 0) {
                    return Qt::Unchecked;
                } else if (checkedCount == grp.files.size()) {
                    return Qt::Checked;
                } else {
                    return Qt::PartiallyChecked;
                }
            }

            if (role == Qt::DisplayRole) {
                switch (index.column()) {
                case 0:
                    return tr("Grupa %1 (%2 kopie fizyczne)")
                        .arg(gIdx + 1)
                        .arg(grp.files.size());
                case 1:
                    return QString();
                case 2:
                    return StorageScanStats::formatBytes(grp.logicalSize);
                case 3:
                    return QString();
                case 4:
                    return StorageScanStats::formatBytes(grp.recoverableBytes());
                case 5:
                    return QString(grp.sha256.left(12) + QStringLiteral("…"));
                }
            } else if (role == Qt::ToolTipRole) {
                return tr("Grupa duplikatów %1\nKopie fizyczne: %2\nRozmiar logiczny: %3\nPotencjalnie odzyskiwalne: %4\nSHA-256: %5")
                    .arg(gIdx + 1)
                    .arg(grp.files.size())
                    .arg(StorageScanStats::formatBytes(grp.logicalSize))
                    .arg(StorageScanStats::formatBytes(grp.recoverableBytes()))
                    .arg(grp.sha256);
            }

            return QVariant();
        }

        // Child Member Row
        const int gIdx = static_cast<int>(id - 1);
        const int mIdx = index.row();
        if (gIdx < 0 || gIdx >= m_result.groups.size()) {
            return QVariant();
        }
        const auto &grp = m_result.groups.at(gIdx);
        if (mIdx < 0 || mIdx >= grp.files.size()) {
            return QVariant();
        }
        const auto &file = grp.files.at(mIdx);

        if (role == IsGroupRole) return false;
        if (role == GroupIndexRole) return gIdx;
        if (role == MemberIndexRole) return mIdx;
        if (role == CanonicalPathRole) return file.canonicalPath;
        if (role == FileUrlRole) return QUrl::fromLocalFile(file.canonicalPath);
        if (role == Sha256Role) return grp.sha256;
        if (role == AliasPathsRole) return file.aliasPaths;
        if (role == LogicalSizeRole) return file.logicalSize;
        if (role == AllocatedSizeRole) return file.allocatedSize;

        if (role == Qt::CheckStateRole && index.column() == 0) {
            return isSelected(file) ? Qt::Checked : Qt::Unchecked;
        }

        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case 0:
                return file.name;
            case 1:
                return file.canonicalPath;
            case 2:
                return StorageScanStats::formatBytes(file.logicalSize);
            case 3:
                return StorageScanStats::formatBytes(file.allocatedSize);
            case 4:
                if (!file.aliasPaths.isEmpty()) {
                    return tr("%1 ścieżki (hardlink aliases)").arg(file.aliasPaths.size() + 1);
                }
                return QString();
            case 5:
                return QString();
            }
        } else if (role == Qt::ToolTipRole) {
            QString tip = tr("Ścieżka: %1\nRozmiar logiczny: %2 (%3 B)\nRozmiar na dysku: %4 (%5 B)\nInode: %6 (urządzenie %7)")
                .arg(file.canonicalPath)
                .arg(StorageScanStats::formatBytes(file.logicalSize))
                .arg(file.logicalSize)
                .arg(StorageScanStats::formatBytes(file.allocatedSize))
                .arg(file.allocatedSize)
                .arg(file.inode)
                .arg(file.deviceId);
            if (!file.aliasPaths.isEmpty()) {
                tip += tr("\nAliasy hardlinków (%1):\n").arg(file.aliasPaths.size());
                for (const auto &alias : file.aliasPaths) {
                    tip += QStringLiteral("  • ") + alias + QLatin1Char('\n');
                }
            }
            return tip.trimmed();
        }

        return QVariant();
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override
    {
        if (!index.isValid() || role != Qt::CheckStateRole || index.column() != 0) {
            return false;
        }

        const quintptr id = index.internalId();
        const auto checkState = static_cast<Qt::CheckState>(value.toInt());
        const bool select = (checkState == Qt::Checked);

        if (id == 0) {
            // Group Row: toggle all physical members in group
            const int gIdx = index.row();
            if (gIdx >= 0 && gIdx < m_result.groups.size()) {
                const auto &grp = m_result.groups.at(gIdx);
                for (const auto &f : grp.files) {
                    setSelected(f, select);
                }
                Q_EMIT dataChanged(index, index, {Qt::CheckStateRole});
                if (!grp.files.isEmpty()) {
                    const QModelIndex childFirst = this->index(0, 0, index);
                    const QModelIndex childLast = this->index(grp.files.size() - 1, 0, index);
                    Q_EMIT dataChanged(childFirst, childLast, {Qt::CheckStateRole});
                }
                Q_EMIT selectionChanged();
                return true;
            }
        } else {
            // Child Member Row
            const int gIdx = static_cast<int>(id - 1);
            const int mIdx = index.row();
            if (gIdx >= 0 && gIdx < m_result.groups.size()) {
                const auto &grp = m_result.groups.at(gIdx);
                if (mIdx >= 0 && mIdx < grp.files.size()) {
                    setSelected(grp.files.at(mIdx), select);
                    Q_EMIT dataChanged(index, index, {Qt::CheckStateRole});
                    const QModelIndex parentGroupIndex = this->index(gIdx, 0);
                    Q_EMIT dataChanged(parentGroupIndex, parentGroupIndex, {Qt::CheckStateRole});
                    Q_EMIT selectionChanged();
                    return true;
                }
            }
        }

        return false;
    }

Q_SIGNALS:
    void selectionChanged();

private:
    DuplicateFinderResult m_result;
    QSet<QPair<quint64, quint64>> m_selectedKeys;
};
