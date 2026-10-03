/*
 * Properties data structure and formatting helpers.
 *
 * Part of Properties 2.0 (0.37.0 Stage 1).
 * Decouples metadata model and capability detection from presentation widgets.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QUrl>

struct PropertiesData
{
    QUrl url;
    QString name;
    QString location;
    QString displayAddress;
    QString extension;

    bool isLocal = false;
    bool isDir = false;
    bool isSymLink = false;
    bool isBrokenSymLink = false;
    QString symLinkTarget;

    QString mimeType;
    QString friendlyType;
    QString iconName;

    QString fileSystem;
    bool isReadOnlyFileSystem = false;

    // Logical size vs Size on disk (allocated blocks)
    bool hasLogicalSize = false;
    qint64 logicalSize = 0;

    bool hasAllocatedSize = false;
    qint64 allocatedSize = 0;

    // Filesystem inode number
    bool hasInode = false;
    quint64 inode = 0;

    // POSIX ownership
    bool hasOwner = false;
    QString owner;
    uint uid = 0;

    bool hasGroup = false;
    QString group;
    uint gid = 0;

    // Timestamps
    bool hasModifiedTime = false;
    QDateTime modifiedTime;

    bool hasAccessTime = false;
    QDateTime accessTime;

    // ctime is status/metadata change time, NEVER to be confused with creation/birth time
    bool hasMetadataChangeTime = false;
    QDateTime metadataChangeTime;

    // True creation / birth time (only when filesystem/backend actually provides it)
    bool hasBirthTime = false;
    QDateTime birthTime;

    // Permissions
    bool hasPermissions = false;
    int permissionsMode = -1;

    // State
    bool isReady = false;
    bool isFailed = false;
    QString errorMessage;

    QString formattedLogicalSize() const
    {
        if (!hasLogicalSize) {
            return QStringLiteral("—");
        }
        const QString human = formatFileSize(logicalSize, false);
        const QString exact = QLocale().toString(logicalSize);
        return QStringLiteral("%1 (%2 %3)")
            .arg(human, exact, isPolish() ? QStringLiteral("bajtów") : QStringLiteral("bytes"));
    }

    QString formattedAllocatedSize() const
    {
        if (!hasAllocatedSize) {
            return isPolish() ? QStringLiteral("Niedostępne") : QStringLiteral("Unavailable");
        }
        const QString human = formatFileSize(allocatedSize, false);
        const QString exact = QLocale().toString(allocatedSize);
        return QStringLiteral("%1 (%2 %3)")
            .arg(human, exact, isPolish() ? QStringLiteral("bajtów") : QStringLiteral("bytes"));
    }

    static QString formatDateTime(const QDateTime &dt)
    {
        if (!dt.isValid() || dt.date().year() <= 1970) {
            return QStringLiteral("—");
        }
        return QLocale().toString(dt, QLocale::LongFormat);
    }
};
