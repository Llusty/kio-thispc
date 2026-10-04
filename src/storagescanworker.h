/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "storagescandata.h"

#include <QAtomicInteger>
#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

class StorageScanWorker final : public QObject
{
    Q_OBJECT

public:
    explicit StorageScanWorker(StorageScanOptions options)
        : m_options(std::move(options))
    {
    }

    void requestCancel()
    {
        m_cancelled.store(true, std::memory_order_release);
    }

    bool isCancelled() const
    {
        return m_cancelled.load(std::memory_order_acquire);
    }

public Q_SLOTS:
    void run()
    {
        StorageScanStats stats;
        QList<StorageScanEntry> entries;

        if (m_cancelled.load(std::memory_order_acquire)) {
            Q_EMIT finished(StorageScanState::Cancelled, stats, entries);
            return;
        }

        // Local-only verification
        QString localPath = m_options.rootPath;
        if (localPath.isEmpty() && m_options.rootUrl.isValid()) {
            if (!m_options.rootUrl.isLocalFile()) {
                Q_EMIT finished(StorageScanState::RemoteUnsupported, stats, entries);
                return;
            }
            localPath = m_options.rootUrl.toLocalFile();
        } else if (!localPath.isEmpty() && !localPath.startsWith(QLatin1Char('/'))) {
            QUrl candidate(localPath);
            if (candidate.isValid() && !candidate.scheme().isEmpty() && candidate.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) != 0) {
                Q_EMIT finished(StorageScanState::RemoteUnsupported, stats, entries);
                return;
            }
            if (candidate.isLocalFile()) {
                localPath = candidate.toLocalFile();
            }
        }

        if (localPath.isEmpty()) {
            Q_EMIT finished(StorageScanState::Failed, stats, entries);
            return;
        }

        const QByteArray rootEncoded = QFile::encodeName(localPath);
        struct stat rootStat {};
        if (::lstat(rootEncoded.constData(), &rootStat) != 0) {
            const int err = errno;
            if (err == ENOENT) {
                stats.disappeared++;
            } else if (err == EACCES || err == EPERM) {
                stats.inaccessible++;
            } else {
                stats.errors++;
            }
            Q_EMIT finished(StorageScanState::Failed, stats, entries);
            return;
        }

        const dev_t rootDev = rootStat.st_dev;

        // If root itself is a symlink, account it and stop (no symlink follow)
        if (S_ISLNK(rootStat.st_mode)) {
            StorageScanEntry rec;
            rec.path = localPath;
            rec.name = localPath.section(QLatin1Char('/'), -1);
            rec.type = StorageEntryType::Symlink;
            rec.logicalSize = static_cast<quint64>(rootStat.st_size);
            rec.allocatedSize = static_cast<quint64>(rootStat.st_blocks) * 512;
            rec.deviceId = static_cast<quint64>(rootStat.st_dev);
            rec.inode = static_cast<quint64>(rootStat.st_ino);
            rec.linkCount = static_cast<quint32>(rootStat.st_nlink);
            rec.isHidden = rec.name.startsWith(QLatin1Char('.'));
            rec.mtimeSec = static_cast<qint64>(rootStat.st_mtim.tv_sec);
            rec.mtimeNsec = static_cast<qint64>(rootStat.st_mtim.tv_nsec);
            rec.ctimeSec = static_cast<qint64>(rootStat.st_ctim.tv_sec);
            rec.ctimeNsec = static_cast<qint64>(rootStat.st_ctim.tv_nsec);

            stats.symlinks++;
            stats.scannedEntries++;
            entries.append(rec);

            Q_EMIT progress(stats, localPath);
            Q_EMIT finished(StorageScanState::Completed, stats, entries);
            return;
        }

        // If root is a regular file, account it and stop
        if (S_ISREG(rootStat.st_mode)) {
            StorageScanEntry rec;
            rec.path = localPath;
            rec.name = localPath.section(QLatin1Char('/'), -1);
            rec.type = StorageEntryType::RegularFile;
            rec.logicalSize = static_cast<quint64>(rootStat.st_size);
            rec.allocatedSize = static_cast<quint64>(rootStat.st_blocks) * 512;
            rec.deviceId = static_cast<quint64>(rootStat.st_dev);
            rec.inode = static_cast<quint64>(rootStat.st_ino);
            rec.linkCount = static_cast<quint32>(rootStat.st_nlink);
            rec.isHidden = rec.name.startsWith(QLatin1Char('.'));
            rec.mtimeSec = static_cast<qint64>(rootStat.st_mtim.tv_sec);
            rec.mtimeNsec = static_cast<qint64>(rootStat.st_mtim.tv_nsec);
            rec.ctimeSec = static_cast<qint64>(rootStat.st_ctim.tv_sec);
            rec.ctimeNsec = static_cast<qint64>(rootStat.st_ctim.tv_nsec);

            stats.files++;
            stats.uniquePhysicalFiles++;
            stats.scannedEntries++;
            stats.logicalBytes += rec.logicalSize;
            stats.allocatedBytes += rec.allocatedSize;
            entries.append(rec);

            Q_EMIT progress(stats, localPath);
            Q_EMIT finished(StorageScanState::Completed, stats, entries);
            return;
        }

        // If root is not a directory and not a file/symlink
        if (!S_ISDIR(rootStat.st_mode)) {
            stats.errors++;
            Q_EMIT finished(StorageScanState::Failed, stats, entries);
            return;
        }

        // Root is a directory: account it
        stats.directories++;
        stats.scannedEntries++;

        StorageScanEntry rootRec;
        rootRec.path = localPath;
        rootRec.name = localPath.section(QLatin1Char('/'), -1);
        rootRec.type = StorageEntryType::Directory;
        rootRec.logicalSize = static_cast<quint64>(rootStat.st_size);
        rootRec.allocatedSize = static_cast<quint64>(rootStat.st_blocks) * 512;
        rootRec.deviceId = static_cast<quint64>(rootStat.st_dev);
        rootRec.inode = static_cast<quint64>(rootStat.st_ino);
        rootRec.linkCount = static_cast<quint32>(rootStat.st_nlink);
        rootRec.isHidden = rootRec.name.startsWith(QLatin1Char('.'));
        rootRec.mtimeSec = static_cast<qint64>(rootStat.st_mtim.tv_sec);
        rootRec.mtimeNsec = static_cast<qint64>(rootStat.st_mtim.tv_nsec);
        rootRec.ctimeSec = static_cast<qint64>(rootStat.st_ctim.tv_sec);
        rootRec.ctimeNsec = static_cast<qint64>(rootStat.st_ctim.tv_nsec);
        entries.append(rootRec);

        // Tracking set for hardlinks: (dev, inode)
        QSet<QPair<quint64, quint64>> seenInodes;

        // BFS Directory queue
        QList<QString> dirQueue;
        dirQueue.append(localPath);

        QElapsedTimer progressTimer;
        progressTimer.start();
        int itemsSinceProgress = 0;
        const int progressBatch =
#ifdef THISPC_TEST_HARNESS
            m_options.progressUpdateBatch > 0 ? m_options.progressUpdateBatch : 500;
#else
            500;
#endif

        bool cancelled = false;

        while (!dirQueue.isEmpty()) {
            if (m_cancelled.load(std::memory_order_acquire)) {
                cancelled = true;
                break;
            }

            const QString currentDir = dirQueue.takeFirst();
            const QByteArray currentDirEncoded = QFile::encodeName(currentDir);

            DIR *dir = ::opendir(currentDirEncoded.constData());
            if (!dir) {
                const int err = errno;
                if (err == ENOENT) {
                    stats.disappeared++;
                } else if (err == EACCES || err == EPERM) {
                    stats.inaccessible++;
                } else {
                    stats.errors++;
                }
                stats.scannedEntries++;
                continue;
            }

            while (true) {
                if (m_cancelled.load(std::memory_order_acquire)) {
                    cancelled = true;
                    break;
                }

                errno = 0;
                struct dirent *de = ::readdir(dir);
                if (!de) {
                    if (errno != 0) {
                        stats.errors++;
                    }
                    break;
                }

                const char *dname = de->d_name;
                if (std::strcmp(dname, ".") == 0 || std::strcmp(dname, "..") == 0) {
                    continue;
                }

                const QString fileName = QString::fromUtf8(dname);
                const bool isHidden = fileName.startsWith(QLatin1Char('.'));
                if (!m_options.includeHidden && isHidden) {
                    // Default is true; if explicit opt-out is passed, ignore
                    continue;
                }

                QString childPath = currentDir;
                if (!childPath.endsWith(QLatin1Char('/'))) {
                    childPath += QLatin1Char('/');
                }
                childPath += fileName;

                struct stat st {};
                const QByteArray childEncoded = QFile::encodeName(childPath);
                if (::lstat(childEncoded.constData(), &st) != 0) {
                    const int err = errno;
                    if (err == ENOENT) {
                        stats.disappeared++;
                    } else if (err == EACCES || err == EPERM) {
                        stats.inaccessible++;
                    } else {
                        stats.errors++;
                    }
                    stats.scannedEntries++;
                    continue;
                }

                StorageScanEntry entry;
                entry.path = childPath;
                entry.name = fileName;
                entry.deviceId = static_cast<quint64>(st.st_dev);
                entry.inode = static_cast<quint64>(st.st_ino);
                entry.linkCount = static_cast<quint32>(st.st_nlink);
                entry.isHidden = isHidden;
                entry.mtimeSec = static_cast<qint64>(st.st_mtim.tv_sec);
                entry.mtimeNsec = static_cast<qint64>(st.st_mtim.tv_nsec);
                entry.ctimeSec = static_cast<qint64>(st.st_ctim.tv_sec);
                entry.ctimeNsec = static_cast<qint64>(st.st_ctim.tv_nsec);

                dev_t effectiveDev = st.st_dev;
#ifdef THISPC_TEST_HARNESS
                if (m_options.simulatedForeignDev != 0 &&
                    !m_options.simulatedForeignDir.isEmpty() &&
                    (childPath == m_options.simulatedForeignDir ||
                     childPath.startsWith(m_options.simulatedForeignDir + QLatin1Char('/')))) {
                    effectiveDev = m_options.simulatedForeignDev;
                    entry.deviceId = static_cast<quint64>(effectiveDev);
                }
#endif

                if (S_ISLNK(st.st_mode)) {
                    entry.type = StorageEntryType::Symlink;
                    entry.logicalSize = static_cast<quint64>(st.st_size);
                    entry.allocatedSize = static_cast<quint64>(st.st_blocks) * 512;
                    stats.symlinks++;
                    stats.scannedEntries++;
                    entries.append(std::move(entry));
                    // Never traverse symlink targets (no symlink follow)
                } else if (S_ISDIR(st.st_mode)) {
                    entry.type = StorageEntryType::Directory;
                    entry.logicalSize = static_cast<quint64>(st.st_size);
                    entry.allocatedSize = static_cast<quint64>(st.st_blocks) * 512;
                    stats.directories++;
                    stats.scannedEntries++;

                    // Check mount boundary
                    if (m_options.stayOnFilesystem && effectiveDev != rootDev) {
                        entry.isMountBoundary = true;
                        stats.skippedMounts++;
                        entries.append(std::move(entry));
                        // Do not queue foreign filesystem directory
                    } else {
                        entries.append(std::move(entry));
                        dirQueue.append(childPath);
                    }
                } else if (S_ISREG(st.st_mode)) {
                    entry.type = StorageEntryType::RegularFile;
                    entry.logicalSize = static_cast<quint64>(st.st_size);
                    entry.allocatedSize = static_cast<quint64>(st.st_blocks) * 512;
                    stats.files++;
                    stats.scannedEntries++;
                    stats.logicalBytes += entry.logicalSize;

                    const QPair<quint64, quint64> identity(entry.deviceId, entry.inode);
                    if (seenInodes.contains(identity)) {
                        stats.hardlinkAliases++;
                        // Do NOT double count physical allocatedBytes for hardlink alias
                    } else {
                        seenInodes.insert(identity);
                        stats.uniquePhysicalFiles++;
                        stats.allocatedBytes += entry.allocatedSize;
                    }
                    entries.append(std::move(entry));
                } else {
                    entry.type = StorageEntryType::Special;
                    entry.logicalSize = static_cast<quint64>(st.st_size);
                    entry.allocatedSize = static_cast<quint64>(st.st_blocks) * 512;
                    stats.scannedEntries++;
                    entries.append(std::move(entry));
                }

                ++itemsSinceProgress;
                if (itemsSinceProgress >= progressBatch || progressTimer.elapsed() >= 50) {
                    itemsSinceProgress = 0;
                    progressTimer.restart();
                    Q_EMIT progress(stats, childPath);
                }
            }

            ::closedir(dir);

            if (cancelled) {
                break;
            }
        }

        // Final progress update
        Q_EMIT progress(stats, QString());

        const StorageScanState finalState = cancelled ? StorageScanState::Cancelled : StorageScanState::Completed;
        Q_EMIT finished(finalState, stats, entries);
    }

Q_SIGNALS:
    void progress(const StorageScanStats &stats, const QString &currentPath);
    void finished(StorageScanState state, const StorageScanStats &stats, const QList<StorageScanEntry> &entries);

private:
    StorageScanOptions m_options;
    std::atomic<bool> m_cancelled{false};
};
