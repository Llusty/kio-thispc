/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QList>
#include <QMetaType>
#include <QPair>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <sys/types.h>

enum class StorageScanState
{
    Idle,
    Running,
    Completed,
    Cancelled,
    Failed,
    RemoteUnsupported
};

struct StorageScanOptions
{
    QString rootPath;
    QUrl rootUrl;
    bool stayOnFilesystem = true;
    bool followSymlinks = false;
    bool includeHidden = true;

#ifdef THISPC_TEST_HARNESS
    // Deterministic simulation hook for mount boundary testing in automated test suites
    dev_t simulatedForeignDev = 0;
    QString simulatedForeignDir;
    int progressUpdateBatch = 500;
#endif
};

enum class StorageEntryType
{
    RegularFile,
    Directory,
    Symlink,
    Special
};

struct StorageScanEntry
{
    QString path;
    QString name;
    StorageEntryType type = StorageEntryType::RegularFile;
    quint64 logicalSize = 0;
    quint64 allocatedSize = 0;
    quint64 deviceId = 0;
    quint64 inode = 0;
    quint32 linkCount = 1;
    bool isMountBoundary = false;
    bool isHidden = false;
    qint64 mtimeSec = 0;
    qint64 mtimeNsec = 0;
    qint64 ctimeSec = 0;
    qint64 ctimeNsec = 0;
};

struct StorageScanStats
{
    quint64 scannedEntries = 0;
    quint64 files = 0;
    quint64 directories = 0;
    quint64 symlinks = 0;
    quint64 logicalBytes = 0;
    quint64 allocatedBytes = 0;
    quint64 uniquePhysicalFiles = 0;
    quint64 hardlinkAliases = 0;
    quint64 skippedMounts = 0;
    quint64 inaccessible = 0;
    quint64 disappeared = 0;
    quint64 errors = 0;

    static QString formatBytes(quint64 bytes)
    {
        constexpr quint64 KiB = 1024ULL;
        constexpr quint64 MiB = 1024ULL * KiB;
        constexpr quint64 GiB = 1024ULL * MiB;
        constexpr quint64 TiB = 1024ULL * GiB;

        if (bytes >= TiB) {
            return QString::asprintf("%.2f TiB", static_cast<double>(bytes) / static_cast<double>(TiB));
        }
        if (bytes >= GiB) {
            return QString::asprintf("%.2f GiB", static_cast<double>(bytes) / static_cast<double>(GiB));
        }
        if (bytes >= MiB) {
            return QString::asprintf("%.2f MiB", static_cast<double>(bytes) / static_cast<double>(MiB));
        }
        if (bytes >= KiB) {
            return QString::asprintf("%.2f KiB", static_cast<double>(bytes) / static_cast<double>(KiB));
        }
        return QString::asprintf("%llu B", static_cast<unsigned long long>(bytes));
    }
};

Q_DECLARE_METATYPE(StorageScanState)
Q_DECLARE_METATYPE(StorageScanStats)
Q_DECLARE_METATYPE(StorageScanEntry)
