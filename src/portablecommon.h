/*
 * Portable Qt6 Core-only shared helpers extracted from browsercommon.h
 * (0.46.0 Stage 2A.1).
 *
 * Strictly uses Qt6::Core only. Zero KF6, KIO, Solid, GIO, QtGui, or QtWidgets
 * dependencies. Included by browsercommon.h to preserve full backwards
 * compatibility for existing consumers.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "remoteurlhelper.h"

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QLocale>
#include <QSet>
#include <QString>
#include <QUrl>

inline QUrl siblingUrlWithName(const QUrl &source, const QString &name)
{
    QUrl destination = source;
    QString path = destination.path();

    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }

    const int slash = path.lastIndexOf(QLatin1Char('/'));

    QString parentPath;
    if (slash < 0) {
        parentPath = QStringLiteral("/");
    } else {
        parentPath = path.left(slash + 1);
    }

    destination.setPath(parentPath + name);
    return destination;
}

inline bool validNewName(const QString &name)
{
    const QString trimmed = name.trimmed();

    return !trimmed.isEmpty()
        && trimmed != QStringLiteral(".")
        && trimmed != QStringLiteral("..")
        && !trimmed.contains(QLatin1Char('/'));
}

inline bool isPolish()
{
    return QLocale().language() == QLocale::Polish;
}

inline QString trLocal(const char *polish, const char *english)
{
    return isPolish()
        ? QString::fromUtf8(polish)
        : QString::fromUtf8(english);
}

inline QString formatFileSize(qint64 bytes, bool isDirectory)
{
    if (isDirectory || bytes < 0) {
        return QStringLiteral("—");
    }

    static const char *units[] = {
        "B", "KiB", "MiB", "GiB", "TiB", "PiB"
    };

    double value = static_cast<double>(bytes);
    int unit = 0;

    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }

    int precision = 0;
    if (unit > 0 && value < 10.0) {
        precision = 2;
    } else if (unit > 0 && value < 100.0) {
        precision = 1;
    }

    return QStringLiteral("%1 %2")
        .arg(
            QLocale().toString(value, 'f', precision),
            QString::fromLatin1(units[unit]));
}

inline QUrl normalizedUrl(QUrl url)
{
    url = RemoteUrlHelper::sanitizeUrl(url);
    if (RemoteUrlHelper::isRemoteUrl(url) && url.path().isEmpty()) {
        url.setPath(QStringLiteral("/"));
    }
    if (url.isLocalFile()) {
        url = QUrl::fromLocalFile(QDir::cleanPath(url.toLocalFile()));
        return url;
    }

    QString path = url.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    url.setPath(path);
    return url;
}

inline bool sameLocation(const QUrl &a, const QUrl &b)
{
    return normalizedUrl(a) == normalizedUrl(b);
}

inline QUrl parentDirectoryUrl(const QUrl &url)
{
    const QUrl norm = normalizedUrl(url);
    if (norm.isLocalFile()) {
        const QString path = norm.toLocalFile();
        const QString parentPath = QFileInfo(path).path();
        return QUrl::fromLocalFile(parentPath);
    }
    return norm.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash);
}

enum class FileOperationType {
    Copy,
    Move,
    Rename,
    Trash,
    Restore,
    CreateFolder,
    CreateFile,
    EmptyTrash,
    Delete,
    Custom
};

struct FileOperationContext {
    FileOperationType type = FileOperationType::Custom;
    QList<QUrl> sources;
    QSet<QUrl> sourceParentDirectories;
    QUrl destinationDirectory;
    QList<QUrl> destinationUrls;

    static FileOperationContext create(FileOperationType opType,
                                       const QList<QUrl> &srcs,
                                       const QUrl &destDir = QUrl())
    {
        FileOperationContext ctx;
        ctx.type = opType;
        ctx.sources = srcs;
        ctx.destinationDirectory = destDir;
        for (const QUrl &u : srcs) {
            if (!u.isValid()) {
                continue;
            }
            const QUrl parent = parentDirectoryUrl(u);
            if (parent.isValid()) {
                ctx.sourceParentDirectories.insert(normalizedUrl(parent));
            }
        }
        return ctx;
    }
};
