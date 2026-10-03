/*
 * Asynchronous, capability-aware metadata provider for Properties.
 *
 * Part of Properties 2.0 (0.37.0 Stage 1).
 * Supports local statx/lstat with truthful birth-time detection,
 * accurate allocated blocks vs logical size, symlink detection,
 * and asynchronous, non-blocking remote KIO stats.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "propertiesdata.h"

#include <KFileItem>
#include <KIO/StatJob>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QObject>
#include <QPointer>
#include <QStorageInfo>
#include <QTimer>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#ifndef STATX_BASIC_STATS
#define STATX_BASIC_STATS 0x000007ffU
#endif
#ifndef STATX_BTIME
#define STATX_BTIME 0x00000800U
#endif
#endif

class PropertiesDataProvider : public QObject
{
    Q_OBJECT

public:
    explicit PropertiesDataProvider(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~PropertiesDataProvider() override
    {
        cancel();
    }

    PropertiesData data() const
    {
        return m_data;
    }

    void cancel()
    {
        if (m_simulationTimer) {
            m_simulationTimer->stop();
            m_simulationTimer->deleteLater();
            m_simulationTimer = nullptr;
        }
        if (m_activeJob) {
            m_activeJob->kill(KJob::Quietly);
            m_activeJob = nullptr;
        }
    }

    void load(
        const QUrl &url,
        const QString &initialName = {},
        bool isDir = false,
        const QString &initialType = {},
        const QString &initialSize = {},
        const QString &initialModified = {})
    {
        cancel();
        m_data = PropertiesData();
        m_data.url = url;
        m_data.name = initialName.isEmpty() ? url.fileName() : initialName;
        m_data.displayAddress = urlForDisplay(url);
        m_data.isDir = isDir;

        const QString physicalPath = localPathForFileOrAdmin(url);
        m_data.isLocal = !physicalPath.isEmpty() && (url.isLocalFile() || isAdminUrl(url));

        if (m_data.isLocal) {
            loadLocal(physicalPath, initialType, initialSize, initialModified);
            Q_EMIT dataReady(m_data);
            return;
        }

        // Remote KIO path: do not fake local metadata.
        m_data.location = url.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).toDisplayString();
        m_data.friendlyType = initialType;
        if (!initialSize.isEmpty()) {
            m_data.hasLogicalSize = true;
        }
        m_data.hasAllocatedSize = false; // Remote allocated size is not supported/provided
        m_data.hasBirthTime = false;
        m_data.hasInode = false;
        m_data.hasMetadataChangeTime = false;

        Q_EMIT dataReady(m_data);

        auto *job = KIO::stat(url, KIO::HideProgressInfo);
        m_activeJob = job;

        connect(job, &KJob::result, this, [this, job] {
            if (m_activeJob != job) {
                return;
            }
            m_activeJob = nullptr;

            if (job->error()) {
                m_data.isFailed = true;
                m_data.errorMessage = job->errorString();
                Q_EMIT loadFailed(m_data.errorMessage);
                return;
            }

            parseKioEntry(job->statResult());
            m_data.isReady = true;
            Q_EMIT dataUpdated(m_data);
        });
    }

    // Testing hook for deterministic offline verification of remote entry parsing
    void loadFromEntry(const QUrl &url, const KIO::UDSEntry &entry)
    {
        cancel();
        m_data = PropertiesData();
        m_data.url = url;
        m_data.name = url.fileName();
        m_data.displayAddress = urlForDisplay(url);
        m_data.location = url.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).toDisplayString();
        m_data.isLocal = false;
        m_data.hasAllocatedSize = false;

        parseKioEntry(entry);
        m_data.isReady = true;
        Q_EMIT dataReady(m_data);
    }

    // Testing hook for simulating async KIO stat completion and cancellation
    void simulateAsyncStat(const QUrl &url, const KIO::UDSEntry &entry, int delayMs = 15)
    {
        cancel();
        m_data = PropertiesData();
        m_data.url = url;
        m_data.name = url.fileName();
        m_data.displayAddress = urlForDisplay(url);
        m_data.location = url.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).toDisplayString();
        m_data.isLocal = false;
        m_data.hasAllocatedSize = false;

        Q_EMIT dataReady(m_data);

        m_simulationTimer = new QTimer(this);
        m_simulationTimer->setSingleShot(true);
        connect(m_simulationTimer, &QTimer::timeout, this, [this, entry] {
            m_simulationTimer = nullptr;
            parseKioEntry(entry);
            m_data.isReady = true;
            Q_EMIT dataUpdated(m_data);
        });
        m_simulationTimer->start(delayMs);
    }

    static QString userNameForUid(uint uid)
    {
        char buf[2048];
        struct passwd pwd {};
        struct passwd *result = nullptr;
        if (getpwuid_r(static_cast<uid_t>(uid), &pwd, buf, sizeof(buf), &result) == 0 && result && result->pw_name) {
            return QString::fromLocal8Bit(result->pw_name);
        }
        return QString::number(uid);
    }

    static QString groupNameForGid(uint gid)
    {
        char buf[2048];
        struct group grp {};
        struct group *result = nullptr;
        if (getgrgid_r(static_cast<gid_t>(gid), &grp, buf, sizeof(buf), &result) == 0 && result && result->gr_name) {
            return QString::fromLocal8Bit(result->gr_name);
        }
        return QString::number(gid);
    }

Q_SIGNALS:
    void dataReady(const PropertiesData &data);
    void dataUpdated(const PropertiesData &data);
    void loadFailed(const QString &errorMessage);

private:
    void loadLocal(
        const QString &path,
        const QString &initialType,
        const QString &initialSize,
        const QString &initialModified)
    {
        Q_UNUSED(initialSize);
        Q_UNUSED(initialModified);
        const QByteArray pathBytes = QFile::encodeName(path);
        const QFileInfo info(path);
        m_data.name = info.fileName();
        m_data.location = info.absolutePath();
        m_data.extension = info.suffix();

        QStorageInfo storage(path);
        if (storage.isValid()) {
            m_data.fileSystem = QString::fromLatin1(storage.fileSystemType()).toLower();
            m_data.isReadOnlyFileSystem = storage.isReadOnly();
        }

        bool statOk = false;
#ifdef __linux__
        struct statx stx {};
        const int statxRes = statx(AT_FDCWD, pathBytes.constData(),
                                  AT_SYMLINK_NOFOLLOW,
                                  STATX_BASIC_STATS | STATX_BTIME, &stx);
        if (statxRes == 0) {
            statOk = true;
            m_data.isDir = S_ISDIR(stx.stx_mode);
            m_data.isSymLink = S_ISLNK(stx.stx_mode);

            if (stx.stx_mask & STATX_MODE) {
                m_data.hasPermissions = true;
                m_data.permissionsMode = stx.stx_mode & 0777;
            }

            if (stx.stx_mask & STATX_SIZE) {
                m_data.hasLogicalSize = true;
                m_data.logicalSize = static_cast<qint64>(stx.stx_size);
            }

            if (!m_data.isDir && (stx.stx_mask & STATX_BLOCKS)) {
                m_data.hasAllocatedSize = true;
                m_data.allocatedSize = static_cast<qint64>(stx.stx_blocks) * 512;
            } else {
                m_data.hasAllocatedSize = false;
            }

            if (stx.stx_mask & STATX_INO) {
                m_data.hasInode = true;
                m_data.inode = static_cast<quint64>(stx.stx_ino);
            }

            if (stx.stx_mask & STATX_UID) {
                m_data.hasOwner = true;
                m_data.uid = stx.stx_uid;
                m_data.owner = userNameForUid(m_data.uid);
            }

            if (stx.stx_mask & STATX_GID) {
                m_data.hasGroup = true;
                m_data.gid = stx.stx_gid;
                m_data.group = groupNameForGid(m_data.gid);
            }

            if (stx.stx_mask & STATX_MTIME) {
                m_data.hasModifiedTime = true;
                m_data.modifiedTime = QDateTime::fromSecsSinceEpoch(stx.stx_mtime.tv_sec)
                                          .addMSecs(stx.stx_mtime.tv_nsec / 1000000);
            }

            if (stx.stx_mask & STATX_ATIME) {
                m_data.hasAccessTime = true;
                m_data.accessTime = QDateTime::fromSecsSinceEpoch(stx.stx_atime.tv_sec)
                                        .addMSecs(stx.stx_atime.tv_nsec / 1000000);
            }

            if (stx.stx_mask & STATX_CTIME) {
                m_data.hasMetadataChangeTime = true;
                m_data.metadataChangeTime = QDateTime::fromSecsSinceEpoch(stx.stx_ctime.tv_sec)
                                                .addMSecs(stx.stx_ctime.tv_nsec / 1000000);
            }

            if ((stx.stx_mask & STATX_BTIME) && stx.stx_btime.tv_sec > 0) {
                m_data.hasBirthTime = true;
                m_data.birthTime = QDateTime::fromSecsSinceEpoch(stx.stx_btime.tv_sec)
                                       .addMSecs(stx.stx_btime.tv_nsec / 1000000);
            } else {
                m_data.hasBirthTime = false;
            }
        }
#endif

        if (!statOk) {
            struct stat st {};
            if (lstat(pathBytes.constData(), &st) == 0) {
                statOk = true;
                m_data.isDir = S_ISDIR(st.st_mode);
                m_data.isSymLink = S_ISLNK(st.st_mode);
                m_data.hasPermissions = true;
                m_data.permissionsMode = st.st_mode & 0777;

                m_data.hasLogicalSize = true;
                m_data.logicalSize = static_cast<qint64>(st.st_size);

                if (!m_data.isDir) {
                    m_data.hasAllocatedSize = true;
                    m_data.allocatedSize = static_cast<qint64>(st.st_blocks) * 512;
                } else {
                    m_data.hasAllocatedSize = false;
                }

                m_data.hasInode = true;
                m_data.inode = static_cast<quint64>(st.st_ino);

                m_data.hasOwner = true;
                m_data.uid = static_cast<uint>(st.st_uid);
                m_data.owner = userNameForUid(m_data.uid);

                m_data.hasGroup = true;
                m_data.gid = static_cast<uint>(st.st_gid);
                m_data.group = groupNameForGid(m_data.gid);

                m_data.hasModifiedTime = true;
                m_data.modifiedTime = QDateTime::fromSecsSinceEpoch(st.st_mtim.tv_sec)
                                          .addMSecs(st.st_mtim.tv_nsec / 1000000);

                m_data.hasAccessTime = true;
                m_data.accessTime = QDateTime::fromSecsSinceEpoch(st.st_atim.tv_sec)
                                        .addMSecs(st.st_atim.tv_nsec / 1000000);

                m_data.hasMetadataChangeTime = true;
                m_data.metadataChangeTime = QDateTime::fromSecsSinceEpoch(st.st_ctim.tv_sec)
                                                .addMSecs(st.st_ctim.tv_nsec / 1000000);

                m_data.hasBirthTime = false;
            }
        }

        if (!statOk) {
            m_data.isFailed = true;
            m_data.errorMessage = isPolish()
                ? QStringLiteral("Nie można odczytać metadanych pliku.")
                : QStringLiteral("Could not read file metadata.");
            m_data.friendlyType = initialType;
            return;
        }

        // Symlink resolution
        if (m_data.isSymLink) {
            char targetBuf[PATH_MAX];
            const ssize_t len = ::readlink(pathBytes.constData(), targetBuf, sizeof(targetBuf) - 1);
            if (len >= 0) {
                targetBuf[len] = '\0';
                m_data.symLinkTarget = QString::fromLocal8Bit(targetBuf, len);

                QString resolvedTargetPath;
                if (m_data.symLinkTarget.startsWith(QLatin1Char('/'))) {
                    resolvedTargetPath = m_data.symLinkTarget;
                } else {
                    resolvedTargetPath = QDir(info.path()).filePath(m_data.symLinkTarget);
                }

                struct stat targetStat {};
                if (::stat(QFile::encodeName(resolvedTargetPath).constData(), &targetStat) != 0) {
                    m_data.isBrokenSymLink = true;
                } else {
                    m_data.isBrokenSymLink = false;
                }
            }
            m_data.friendlyType = m_data.isBrokenSymLink
                ? (isPolish() ? QStringLiteral("Dowiązanie symboliczne (przerwane)")
                              : QStringLiteral("Symbolic link (broken)"))
                : (isPolish() ? QStringLiteral("Dowiązanie symboliczne")
                              : QStringLiteral("Symbolic link"));
            m_data.mimeType = QStringLiteral("inode/symlink");
        } else if (m_data.isDir) {
            m_data.mimeType = QStringLiteral("inode/directory");
            QMimeDatabase db;
            const QString folderComment = db.mimeTypeForName(QStringLiteral("inode/directory")).comment();
            m_data.friendlyType = !initialType.isEmpty()
                ? initialType
                : (!folderComment.isEmpty() ? folderComment
                                           : (isPolish() ? QStringLiteral("Katalog") : QStringLiteral("Folder")));
        } else {
            // Fast MIME resolution via extension match (never reading contents synchronously on UI thread)
            QMimeDatabase db;
            const QMimeType mime = db.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
            m_data.mimeType = mime.name();
            m_data.friendlyType = mime.comment().isEmpty() ? initialType : mime.comment();
        }

        if (m_data.friendlyType.isEmpty()) {
            m_data.friendlyType = initialType;
        }

        m_data.isReady = true;
    }

    void parseKioEntry(const KIO::UDSEntry &entry)
    {
        if (entry.contains(KIO::UDSEntry::UDS_NAME)) {
            m_data.name = entry.stringValue(KIO::UDSEntry::UDS_NAME);
        }

        if (entry.contains(KIO::UDSEntry::UDS_FILE_TYPE)) {
            const mode_t type = static_cast<mode_t>(entry.numberValue(KIO::UDSEntry::UDS_FILE_TYPE));
            if (S_ISDIR(type)) {
                m_data.isDir = true;
            }
            if (S_ISLNK(type)) {
                m_data.isSymLink = true;
            }
        }

        if (entry.contains(KIO::UDSEntry::UDS_SIZE)) {
            m_data.hasLogicalSize = true;
            m_data.logicalSize = entry.numberValue(KIO::UDSEntry::UDS_SIZE);
        }

        if (entry.contains(KIO::UDSEntry::UDS_MIME_TYPE)) {
            m_data.mimeType = entry.stringValue(KIO::UDSEntry::UDS_MIME_TYPE);
        }

        if (entry.contains(KIO::UDSEntry::UDS_USER)) {
            m_data.hasOwner = true;
            m_data.owner = entry.stringValue(KIO::UDSEntry::UDS_USER);
        }

        if (entry.contains(KIO::UDSEntry::UDS_GROUP)) {
            m_data.hasGroup = true;
            m_data.group = entry.stringValue(KIO::UDSEntry::UDS_GROUP);
        }

        if (entry.contains(KIO::UDSEntry::UDS_ACCESS)) {
            m_data.hasPermissions = true;
            m_data.permissionsMode = static_cast<int>(entry.numberValue(KIO::UDSEntry::UDS_ACCESS)) & 0777;
        }

        if (entry.contains(KIO::UDSEntry::UDS_MODIFICATION_TIME)) {
            m_data.hasModifiedTime = true;
            m_data.modifiedTime = QDateTime::fromSecsSinceEpoch(
                entry.numberValue(KIO::UDSEntry::UDS_MODIFICATION_TIME));
        }

        if (entry.contains(KIO::UDSEntry::UDS_ACCESS_TIME)) {
            m_data.hasAccessTime = true;
            m_data.accessTime = QDateTime::fromSecsSinceEpoch(
                entry.numberValue(KIO::UDSEntry::UDS_ACCESS_TIME));
        }

        if (entry.contains(KIO::UDSEntry::UDS_CREATION_TIME)) {
            const qlonglong btime = entry.numberValue(KIO::UDSEntry::UDS_CREATION_TIME);
            if (btime > 0) {
                m_data.hasBirthTime = true;
                m_data.birthTime = QDateTime::fromSecsSinceEpoch(btime);
            }
        }

        if (entry.contains(KIO::UDSEntry::UDS_INODE)) {
            m_data.hasInode = true;
            m_data.inode = static_cast<quint64>(entry.numberValue(KIO::UDSEntry::UDS_INODE));
        }

        if (entry.contains(KIO::UDSEntry::UDS_LINK_DEST)) {
            m_data.isSymLink = true;
            m_data.symLinkTarget = entry.stringValue(KIO::UDSEntry::UDS_LINK_DEST);
            m_data.friendlyType = isPolish()
                ? QStringLiteral("Dowiązanie symboliczne")
                : QStringLiteral("Symbolic link");
        }

        if (m_data.isDir && m_data.mimeType.isEmpty()) {
            m_data.mimeType = QStringLiteral("inode/directory");
        }

        if (m_data.friendlyType.isEmpty() && !m_data.mimeType.isEmpty()) {
            QMimeDatabase db;
            m_data.friendlyType = db.mimeTypeForName(m_data.mimeType).comment();
        }
    }

    PropertiesData m_data;
    QPointer<KIO::StatJob> m_activeJob;
    QPointer<QTimer> m_simulationTimer;
};
