/*
 * ThisPC 0.41.1 — Folder Size Calculator Data Contract.
 *
 * Defines explicit data types, progress models, partial outcome semantics,
 * and presentation contracts for single-folder recursive size calculation.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "aggregatepropertiesformat.h"
#include "browsercommon.h"

#include <QLocale>
#include <QString>
#include <QUrl>
#include <cstdint>
#include <limits>

enum class FolderSizeStatus {
    NotStarted,
    Calculating,
    Completed,
    Partial,
    Cancelled,
    Error,
    Unsupported
};

enum class FolderSizePartialReason {
    None,
    PermissionDenied,
    EntryDisappeared,
    EntryModified,
    ScanLimitReached,
    DepthLimitReached,
    MountBoundaryEncountered,
    DeviceDisconnected,
    Cancelled
};

enum class DiskAllocationReliability {
    ExactBlockAllocation,
    StandardBlockAllocation = ExactBlockAllocation,
    EstimatedSharedOrCompressed,
    NtfsAllocation,
    Unavailable
};

struct FolderSizeProgress {
    quint64 logicalBytes = 0;
    quint64 allocatedBytes = 0;
    quint64 fileCount = 0;
    quint64 directoryCount = 0;
    quint64 symlinkCount = 0;
    quint64 skippedCount = 0;
    QString currentPath;
};

struct FolderSizeResult {
    FolderSizeStatus status = FolderSizeStatus::NotStarted;
    FolderSizePartialReason partialReason = FolderSizePartialReason::None;
    DiskAllocationReliability diskReliability = DiskAllocationReliability::Unavailable;

    quint64 logicalBytes = 0;
    quint64 allocatedBytes = 0;
    quint64 fileCount = 0;
    quint64 directoryCount = 0;
    quint64 symlinkCount = 0;
    quint64 skippedCount = 0;
    quint64 allocatedFiles = 0;
    quint64 allocationUnavailableCount = 0;

    bool hasAllocatedBytes = false;
    QString errorMessage;

    [[nodiscard]] bool isComplete() const { return status == FolderSizeStatus::Completed; }
    [[nodiscard]] bool isPartial() const { return status == FolderSizeStatus::Partial; }
    [[nodiscard]] bool isCancelled() const { return status == FolderSizeStatus::Cancelled; }
    [[nodiscard]] bool isError() const { return status == FolderSizeStatus::Error; }
    [[nodiscard]] bool isUnsupported() const { return status == FolderSizeStatus::Unsupported; }

    static bool safeAddBytes(quint64 &total, quint64 add)
    {
        const quint64 maxVal = std::numeric_limits<quint64>::max();
        if (add > maxVal - total) {
            total = maxVal;
            return false;
        }
        total += add;
        return true;
    }

    [[nodiscard]] QString formattedLogicalSize(const QLocale &locale = QLocale()) const
    {
        if (status == FolderSizeStatus::Unsupported) {
            return locale.language() == QLocale::Polish ? QStringLiteral("Niedostępne") : QStringLiteral("Unavailable");
        }
        if (status == FolderSizeStatus::Error && logicalBytes == 0) {
            return locale.language() == QLocale::Polish ? QStringLiteral("Błąd obliczania") : QStringLiteral("Calculation error");
        }
        return aggregateSize(logicalBytes, locale);
    }

    [[nodiscard]] QString formattedAllocatedSize(const QLocale &locale = QLocale()) const
    {
        if (status == FolderSizeStatus::Unsupported || !hasAllocatedBytes || diskReliability == DiskAllocationReliability::Unavailable) {
            return locale.language() == QLocale::Polish ? QStringLiteral("Niedostępne") : QStringLiteral("Unavailable");
        }
        if (!allocatedFiles && (isPartial() || allocationUnavailableCount > 0)) {
            return locale.language() == QLocale::Polish ? QStringLiteral("Niedostępne") : QStringLiteral("Unavailable");
        }
        QString result = aggregateSize(allocatedBytes, locale);
        if (isPartial() || allocationUnavailableCount > 0) {
            result += locale.language() == QLocale::Polish
                ? QStringLiteral(" — wynik częściowy")
                : QStringLiteral(" — partial results");
        }
        return result;
    }

    [[nodiscard]] static QString formatBriefSize(quint64 bytes, const QLocale &locale = QLocale())
    {
        if (bytes < 1024) return aggregateCount(bytes, locale) + QStringLiteral(" B");
        static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
        double value = double(bytes);
        int unit = 0;
        while (value >= 1024 && unit < 6) { value /= 1024; ++unit; }
        return QStringLiteral("%1 %2").arg(locale.toString(value, 'f', 1), QString::fromLatin1(units[unit]));
    }

    [[nodiscard]] static QString formatPluralFiles(quint64 count, const QLocale &locale = QLocale())
    {
        const QString numStr = aggregateCount(count, locale);
        if (locale.language() == QLocale::Polish) {
            if (count == 1) return QStringLiteral("1 plik");
            const quint64 mod10 = count % 10;
            const quint64 mod100 = count % 100;
            if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) {
                return QStringLiteral("%1 pliki").arg(numStr);
            }
            return QStringLiteral("%1 plików").arg(numStr);
        }
        return count == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(numStr);
    }

    [[nodiscard]] static QString formatPluralFolders(quint64 count, const QLocale &locale = QLocale())
    {
        const QString numStr = aggregateCount(count, locale);
        if (locale.language() == QLocale::Polish) {
            if (count == 1) return QStringLiteral("1 folder");
            const quint64 mod10 = count % 10;
            const quint64 mod100 = count % 100;
            if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) {
                return QStringLiteral("%1 foldery").arg(numStr);
            }
            return QStringLiteral("%1 folderów").arg(numStr);
        }
        return count == 1 ? QStringLiteral("1 folder") : QStringLiteral("%1 folders").arg(numStr);
    }

    [[nodiscard]] QString formattedContents(const QLocale &locale = QLocale()) const
    {
        if (status == FolderSizeStatus::Unsupported) {
            return locale.language() == QLocale::Polish ? QStringLiteral("Niedostępne") : QStringLiteral("Unavailable");
        }
        const QString filesStr = formatPluralFiles(fileCount, locale);
        const QString foldersStr = formatPluralFolders(directoryCount, locale);
        return QStringLiteral("%1, %2").arg(filesStr, foldersStr);
    }

    [[nodiscard]] QString diskSizeTooltip(const QLocale &locale = QLocale()) const
    {
        const bool pl = locale.language() == QLocale::Polish;
        QString text;
        switch (diskReliability) {
        case DiskAllocationReliability::ExactBlockAllocation:
            text = pl
                ? QStringLiteral("Alokacja zwykłych plików raportowana przez system plików (st_blocks × 512).\nHardlinki liczone raz. Bez metadanych katalogów i symlinków.")
                : QStringLiteral("Regular-file allocation reported by the filesystem (st_blocks × 512).\nHardlinks counted once. Excludes directory metadata and symlinks.");
            break;
        case DiskAllocationReliability::EstimatedSharedOrCompressed:
            text = pl
                ? QStringLiteral("Alokacja raportowana przez system plików (st_blocks × 512).\nMoże zawierać współdzielone ekstenty CoW (reflink), snapshoty lub kompresję (np. Btrfs/ZFS) i stanowić wartość szacunkową.")
                : QStringLiteral("Allocation reported by the filesystem (st_blocks × 512).\nMay reflect shared CoW extents (reflink), snapshots, or transparent compression (e.g. Btrfs/ZFS) and represents an estimated allocation.");
            break;
        case DiskAllocationReliability::NtfsAllocation:
            text = pl
                ? QStringLiteral("Alokacja klastrów NTFS raportowana przez sterownik.\nMałe pliki mogą być rezydentne w rekordach MFT bez oddzielnych bloków danych.")
                : QStringLiteral("NTFS cluster allocation reported by the driver.\nSmall resident files may be stored directly in MFT records without separate data blocks.");
            break;
        case DiskAllocationReliability::Unavailable:
        default:
            return pl ? QStringLiteral("Rozmiar na dysku niedostępny dla tego typu lokalizacji.")
                      : QStringLiteral("Size on disk unavailable for this location type.");
        }

        if (allocationUnavailableCount > 0) {
            text += QStringLiteral("\n") + (pl
                ? QStringLiteral("Brak danych alokacji dla %1 plików.").arg(aggregateCount(allocationUnavailableCount, locale))
                : QStringLiteral("Allocation data unavailable for %1 files.").arg(aggregateCount(allocationUnavailableCount, locale)));
        }
        if (isPartial()) {
            text += QStringLiteral("\n") + (pl
                ? QStringLiteral("Uwaga: wynik częściowy z powodu przerwania lub błędu odczytu.")
                : QStringLiteral("Note: partial result due to cancellation or access error."));
        }
        return text;
    }
};
