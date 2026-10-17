/*
 * ThisPC 0.41.1 — Folder Size Calculator Engine.
 *
 * Implements a descriptor-relative, non-blocking, safe POSIX directory
 * traversal engine for single-folder size calculation, adhering to the
 * FolderSizeContract specification.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "foldersizecontract.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStorageInfo>
#include <QString>
#include <QUrl>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#ifndef STATX_MNT_ID
#define STATX_MNT_ID 0x00001000U
#endif
#ifndef STATX_BASIC_STATS
#define STATX_BASIC_STATS 0x000007ffU
#endif
#endif

struct FolderSizeOptions {
    quint64 maxEntries = 1000000;
    unsigned maxDepth = 128;
    int progressUpdateBatch = 256;
    bool skipMountBoundaries = true;
};

class FolderSizeCalculator {
public:
    using CancelToken = std::shared_ptr<std::atomic_bool>;
    using ProgressCallback = std::function<void(const FolderSizeProgress &)>;

    static FolderSizeResult calculate(
        const QUrl &url,
        const CancelToken &cancel = nullptr,
        const FolderSizeOptions &options = {},
        ProgressCallback progress = nullptr)
    {
        if (!url.isValid() || !url.isLocalFile()) {
            FolderSizeResult res;
            res.status = FolderSizeStatus::Unsupported;
            res.diskReliability = DiskAllocationReliability::Unavailable;
            res.hasAllocatedBytes = false;
            res.errorMessage = QStringLiteral("Zdalne lub wirtualne lokalizacje nie są obsługiwane.");
            return res;
        }

        return calculatePath(url.toLocalFile(), cancel, options, std::move(progress));
    }

    static FolderSizeResult calculatePath(
        const QString &localPath,
        const CancelToken &cancel = nullptr,
        const FolderSizeOptions &options = {},
        ProgressCallback progress = nullptr)
    {
        FolderSizeResult result;
        result.status = FolderSizeStatus::Calculating;

        if (localPath.isEmpty() || !localPath.startsWith(QLatin1Char('/'))) {
            result.status = FolderSizeStatus::Error;
            result.errorMessage = QStringLiteral("Nieprawidłowa ścieżka lokalna.");
            return result;
        }

        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.status = FolderSizeStatus::Cancelled;
            result.partialReason = FolderSizePartialReason::Cancelled;
            return result;
        }

        // Determine filesystem type and allocation reliability
        QStorageInfo storage(localPath);
        if (storage.isValid()) {
            const QString fsType = QString::fromLatin1(storage.fileSystemType()).toLower();
            if (fsType == QStringLiteral("btrfs") || fsType == QStringLiteral("zfs")) {
                result.diskReliability = DiskAllocationReliability::EstimatedSharedOrCompressed;
            } else if (fsType == QStringLiteral("ntfs") || fsType == QStringLiteral("ntfs3")
                       || fsType == QStringLiteral("fuseblk")) {
                result.diskReliability = DiskAllocationReliability::NtfsAllocation;
            } else {
                result.diskReliability = DiskAllocationReliability::ExactBlockAllocation;
            }
        } else {
            result.diskReliability = DiskAllocationReliability::ExactBlockAllocation;
        }
        result.hasAllocatedBytes = true;

        const QByteArray rootEncoded = QFile::encodeName(localPath);
        struct stat rootStat {};
        if (::lstat(rootEncoded.constData(), &rootStat) != 0) {
            const int err = errno;
            result.status = FolderSizeStatus::Error;
            result.partialReason = (err == EACCES || err == EPERM)
                ? FolderSizePartialReason::PermissionDenied
                : FolderSizePartialReason::EntryDisappeared;
            result.errorMessage = QString::fromUtf8(std::strerror(err));
            return result;
        }

        // Symlink root itself: do not follow
        if (S_ISLNK(rootStat.st_mode)) {
            result.symlinkCount = 1;
            result.status = FolderSizeStatus::Completed;
            return result;
        }

        // Regular file root: calculate and return
        if (S_ISREG(rootStat.st_mode)) {
            result.fileCount = 1;
            result.logicalBytes = static_cast<quint64>(rootStat.st_size);
            if (rootStat.st_blocks >= 0) {
                result.allocatedBytes = static_cast<quint64>(rootStat.st_blocks) * 512;
                result.allocatedFiles = 1;
            } else {
                result.allocationUnavailableCount = 1;
            }
            result.status = FolderSizeStatus::Completed;
            return result;
        }

        if (!S_ISDIR(rootStat.st_mode)) {
            result.status = FolderSizeStatus::Error;
            result.errorMessage = QStringLiteral("Podany obiekt nie jest katalogiem.");
            return result;
        }

        // Directory root: open descriptor and verify identity
        const int rootFd = ::open(rootEncoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (rootFd < 0) {
            const int err = errno;
            result.status = FolderSizeStatus::Error;
            result.partialReason = (err == EACCES || err == EPERM)
                ? FolderSizePartialReason::PermissionDenied
                : FolderSizePartialReason::EntryDisappeared;
            result.errorMessage = QString::fromUtf8(std::strerror(err));
            return result;
        }

        struct stat openedStat {};
        if (::fstat(rootFd, &openedStat) != 0 || openedStat.st_dev != rootStat.st_dev
            || openedStat.st_ino != rootStat.st_ino) {
            ::close(rootFd);
            result.status = FolderSizeStatus::Error;
            result.partialReason = FolderSizePartialReason::EntryModified;
            result.errorMessage = QStringLiteral("Węzeł katalogu został zmodyfikowany podczas otwierania.");
            return result;
        }

        uint64_t rootMntId = 0;
        bool hasMntId = false;
#ifdef __linux__
        struct statx stx {};
        if (statx(rootFd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &stx) == 0) {
            if (stx.stx_mask & STATX_MNT_ID) {
                rootMntId = stx.stx_mnt_id;
                hasMntId = true;
            }
        }
#endif

        Engine engine(cancel, options, std::move(progress), rootStat.st_dev, rootMntId, hasMntId);
        engine.run(rootFd, result);

        return result;
    }

private:
    class Engine {
    public:
        Engine(CancelToken cancel,
               FolderSizeOptions options,
               ProgressCallback progress,
               dev_t rootDev,
               uint64_t rootMntId,
               bool hasMntId)
            : m_cancel(std::move(cancel))
            , m_options(options)
            , m_progress(std::move(progress))
            , m_rootDev(rootDev)
            , m_rootMntId(rootMntId)
            , m_hasMntId(hasMntId)
        {
        }

        void run(int rootFd, FolderSizeResult &res)
        {
            traverse(rootFd, 0, res);
            if (m_cancel && m_cancel->load(std::memory_order_relaxed)) {
                res.status = FolderSizeStatus::Cancelled;
                res.partialReason = FolderSizePartialReason::Cancelled;
            } else if (res.status == FolderSizeStatus::Calculating) {
                if (m_hadPartialReason) {
                    res.status = FolderSizeStatus::Partial;
                } else {
                    res.status = FolderSizeStatus::Completed;
                }
            }
        }

    private:
        bool isCancelled() const
        {
            return m_cancel && m_cancel->load(std::memory_order_relaxed);
        }

        void notifyProgress(const FolderSizeResult &res)
        {
            if (m_progress && m_processedEntries % m_options.progressUpdateBatch == 0) {
                FolderSizeProgress p;
                p.logicalBytes = res.logicalBytes;
                p.allocatedBytes = res.allocatedBytes;
                p.fileCount = res.fileCount;
                p.directoryCount = res.directoryCount;
                p.symlinkCount = res.symlinkCount;
                p.skippedCount = res.skippedCount;
                m_progress(p);
            }
        }

        void traverse(int dirFd, unsigned depth, FolderSizeResult &res)
        {
            DIR *dir = ::fdopendir(dirFd);
            if (!dir) {
                ::close(dirFd);
                res.skippedCount++;
                m_hadPartialReason = true;
                if (res.partialReason == FolderSizePartialReason::None) {
                    res.partialReason = FolderSizePartialReason::PermissionDenied;
                }
                return;
            }

            while (!isCancelled()) {
                if (m_processedEntries >= m_options.maxEntries) {
                    res.status = FolderSizeStatus::Partial;
                    res.partialReason = FolderSizePartialReason::ScanLimitReached;
                    m_hadPartialReason = true;
                    break;
                }

                errno = 0;
                struct dirent *entry = ::readdir(dir);
                if (!entry) {
                    if (errno != 0) {
                        res.skippedCount++;
                        m_hadPartialReason = true;
                        if (res.partialReason == FolderSizePartialReason::None) {
                            res.partialReason = FolderSizePartialReason::PermissionDenied;
                        }
                    }
                    break;
                }

                if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
                    continue;
                }

                ++m_processedEntries;
                notifyProgress(res);

                struct stat st {};
                if (::fstatat(dirFd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                    const int err = errno;
                    res.skippedCount++;
                    m_hadPartialReason = true;
                    if (res.partialReason == FolderSizePartialReason::None) {
                        res.partialReason = (err == ENOENT)
                            ? FolderSizePartialReason::EntryDisappeared
                            : FolderSizePartialReason::PermissionDenied;
                    }
                    continue;
                }

                // Mount boundary verification: st_dev check
                if (m_options.skipMountBoundaries && st.st_dev != m_rootDev) {
                    res.skippedCount++;
                    m_hadPartialReason = true;
                    if (res.partialReason == FolderSizePartialReason::None) {
                        res.partialReason = FolderSizePartialReason::MountBoundaryEncountered;
                    }
                    continue;
                }

                // Mount boundary verification: statx stx_mnt_id check (detects bind mounts on same st_dev)
                if (m_options.skipMountBoundaries && m_hasMntId && S_ISDIR(st.st_mode)) {
#ifdef __linux__
                    struct statx stx {};
                    if (statx(dirFd, entry->d_name, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &stx) == 0) {
                        if ((stx.stx_mask & STATX_MNT_ID) && stx.stx_mnt_id != m_rootMntId) {
                            res.skippedCount++;
                            m_hadPartialReason = true;
                            if (res.partialReason == FolderSizePartialReason::None) {
                                res.partialReason = FolderSizePartialReason::MountBoundaryEncountered;
                            }
                            continue;
                        }
                    }
#endif
                }

                if (S_ISLNK(st.st_mode)) {
                    // Symlink: count without following
                    res.symlinkCount++;
                } else if (S_ISREG(st.st_mode)) {
                    // Regular file: add logical size and deduplicate inode for allocation
                    res.fileCount++;
                    FolderSizeResult::safeAddBytes(res.logicalBytes, static_cast<quint64>(st.st_size));

                    const auto key = qMakePair(static_cast<quint64>(st.st_dev), static_cast<quint64>(st.st_ino));
                    if (!m_seenInodes.contains(key)) {
                        m_seenInodes.insert(key);
                        if (st.st_blocks >= 0) {
                            const quint64 alloc = static_cast<quint64>(st.st_blocks) * 512;
                            FolderSizeResult::safeAddBytes(res.allocatedBytes, alloc);
                            res.allocatedFiles++;
                        } else {
                            res.allocationUnavailableCount++;
                        }
                    }
                } else if (S_ISDIR(st.st_mode)) {
                    // Directory: increment count, guard depth and recursion cycles
                    res.directoryCount++;

                    const auto dirKey = qMakePair(static_cast<quint64>(st.st_dev), static_cast<quint64>(st.st_ino));
                    if (m_seenDirs.contains(dirKey)) {
                        continue;
                    }

                    if (depth >= m_options.maxDepth) {
                        res.skippedCount++;
                        res.status = FolderSizeStatus::Partial;
                        res.partialReason = FolderSizePartialReason::DepthLimitReached;
                        m_hadPartialReason = true;
                        continue;
                    }

                    m_seenDirs.insert(dirKey);

                    const int subFd = ::openat(dirFd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    if (subFd < 0) {
                        const int err = errno;
                        res.skippedCount++;
                        m_hadPartialReason = true;
                        if (res.partialReason == FolderSizePartialReason::None) {
                            res.partialReason = (err == ENOENT)
                                ? FolderSizePartialReason::EntryDisappeared
                                : FolderSizePartialReason::PermissionDenied;
                        }
                        continue;
                    }

                    struct stat subOpened {};
                    if (::fstat(subFd, &subOpened) != 0 || subOpened.st_dev != st.st_dev
                        || subOpened.st_ino != st.st_ino) {
                        ::close(subFd);
                        res.skippedCount++;
                        m_hadPartialReason = true;
                        if (res.partialReason == FolderSizePartialReason::None) {
                            res.partialReason = FolderSizePartialReason::EntryModified;
                        }
                        continue;
                    }

                    traverse(subFd, depth + 1, res);
                } else {
                    // Special file (fifo, socket, device node)
                    res.skippedCount++;
                }
            }

            ::closedir(dir);
        }

        CancelToken m_cancel;
        FolderSizeOptions m_options;
        ProgressCallback m_progress;
        dev_t m_rootDev = 0;
        uint64_t m_rootMntId = 0;
        bool m_hasMntId = false;

        quint64 m_processedEntries = 0;
        bool m_hadPartialReason = false;
        QSet<QPair<quint64, quint64>> m_seenInodes;
        QSet<QPair<quint64, quint64>> m_seenDirs;
    };
};
