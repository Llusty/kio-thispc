/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "duplicatefinderdata.h"

#include <QList>
#include <QPair>
#include <QSet>
#include <QString>
#include <QUrl>

#include <sys/stat.h>
#include <unistd.h>

struct DuplicateActionItem
{
    quint64 deviceId = 0;
    quint64 inode = 0;
    QString targetPath; // The specific chosen pathname (e.g. canonicalPath)
    DuplicatePhysicalFile fileSnapshot;
    int groupIndex = -1;
};

enum class DuplicateRevalidationStatus
{
    Valid,
    Disappeared,
    ChangedType,
    ChangedDevice,
    ChangedInode,
    ChangedSize,
    ChangedMtime,
    ChangedCtime
};

struct DuplicateRevalidatedItem
{
    DuplicateActionItem item;
    DuplicateRevalidationStatus status = DuplicateRevalidationStatus::Valid;
    QString reason;
};

struct DuplicateRevalidationReport
{
    QList<DuplicateRevalidatedItem> validItems;
    QList<DuplicateRevalidatedItem> staleItems;
    QList<DuplicateRevalidatedItem> disappearedItems;

    quint64 validLogicalBytes = 0;
    quint64 validAllocatedBytes = 0;

    int affectedGroupsCount = 0;
    bool hasAllCopiesWarning = false;
    QSet<int> groupsWithAllCopiesSelected;

    bool allValid() const
    {
        return staleItems.isEmpty() && disappearedItems.isEmpty();
    }

    int totalRequested() const
    {
        return validItems.size() + staleItems.size() + disappearedItems.size();
    }
};

class DuplicateReviewValidator
{
public:
    static DuplicateRevalidationReport revalidate(
        const QList<DuplicateActionItem> &selectedItems,
        const QList<DuplicateGroup> &currentGroups)
    {
        DuplicateRevalidationReport report;
        QHash<int, int> selectedPhysicalPerGroup;
        QSet<int> affectedGroups;

        for (const auto &item : selectedItems) {
            affectedGroups.insert(item.groupIndex);
            selectedPhysicalPerGroup[item.groupIndex]++;

            struct stat st {};
            const QByteArray encoded = QFile::encodeName(item.targetPath);
            if (::lstat(encoded.constData(), &st) != 0) {
                DuplicateRevalidatedItem rev;
                rev.item = item;
                rev.status = DuplicateRevalidationStatus::Disappeared;
                rev.reason = QStringLiteral("Plik nie istnieje (zniknął przed wykonaniem operacji)");
                report.disappearedItems.append(rev);
                continue;
            }

            if (!S_ISREG(st.st_mode)) {
                DuplicateRevalidatedItem rev;
                rev.item = item;
                rev.status = DuplicateRevalidationStatus::ChangedType;
                rev.reason = QStringLiteral("Element zmienił typ (nie jest zwykłym plikiem)");
                report.staleItems.append(rev);
                continue;
            }

            if (static_cast<quint64>(st.st_dev) != item.fileSnapshot.deviceId) {
                DuplicateRevalidatedItem rev;
                rev.item = item;
                rev.status = DuplicateRevalidationStatus::ChangedDevice;
                rev.reason = QStringLiteral("Zmieniło się urządzenie/system plików");
                report.staleItems.append(rev);
                continue;
            }

            if (static_cast<quint64>(st.st_ino) != item.fileSnapshot.inode) {
                DuplicateRevalidatedItem rev;
                rev.item = item;
                rev.status = DuplicateRevalidationStatus::ChangedInode;
                rev.reason = QStringLiteral("Plik został zastąpiony innym obiektem (inny numer inode)");
                report.staleItems.append(rev);
                continue;
            }

            if (static_cast<quint64>(st.st_size) != item.fileSnapshot.logicalSize) {
                DuplicateRevalidatedItem rev;
                rev.item = item;
                rev.status = DuplicateRevalidationStatus::ChangedSize;
                rev.reason = QStringLiteral("Rozmiar pliku uległ zmianie");
                report.staleItems.append(rev);
                continue;
            }

            if (static_cast<qint64>(st.st_mtim.tv_sec) != item.fileSnapshot.mtimeSec ||
                static_cast<qint64>(st.st_mtim.tv_nsec) != item.fileSnapshot.mtimeNsec) {
                DuplicateRevalidatedItem rev;
                rev.item = item;
                rev.status = DuplicateRevalidationStatus::ChangedMtime;
                rev.reason = QStringLiteral("Czas modyfikacji uległ zmianie");
                report.staleItems.append(rev);
                continue;
            }

            if (static_cast<qint64>(st.st_ctim.tv_sec) != item.fileSnapshot.ctimeSec ||
                static_cast<qint64>(st.st_ctim.tv_nsec) != item.fileSnapshot.ctimeNsec) {
                DuplicateRevalidatedItem rev;
                rev.item = item;
                rev.status = DuplicateRevalidationStatus::ChangedCtime;
                rev.reason = QStringLiteral("Czas zmiany metadanych uległ zmianie");
                report.staleItems.append(rev);
                continue;
            }

            // Valid snapshot
            DuplicateRevalidatedItem rev;
            rev.item = item;
            rev.status = DuplicateRevalidationStatus::Valid;
            report.validItems.append(rev);
            report.validLogicalBytes = saturatingAdd(report.validLogicalBytes, item.fileSnapshot.logicalSize);
            report.validAllocatedBytes = saturatingAdd(report.validAllocatedBytes, item.fileSnapshot.allocatedSize);
        }

        report.affectedGroupsCount = affectedGroups.size();

        // Check if all physical copies in any group were selected
        for (auto it = selectedPhysicalPerGroup.begin(); it != selectedPhysicalPerGroup.end(); ++it) {
            const int gIdx = it.key();
            const int count = it.value();
            if (gIdx >= 0 && gIdx < currentGroups.size()) {
                const int totalPhysicalInGroup = currentGroups.at(gIdx).files.size();
                if (count >= totalPhysicalInGroup && totalPhysicalInGroup > 0) {
                    report.hasAllCopiesWarning = true;
                    report.groupsWithAllCopiesSelected.insert(gIdx);
                }
            }
        }

        return report;
    }
};
