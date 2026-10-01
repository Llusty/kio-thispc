/*
 * Shared browser data and lightweight helpers used by the main and split panes.
 *
 * Extracted during the 0.21.0 architecture refactor. Keep behavior changes
 * separate from structural moves so regressions are easy to isolate.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <KIO/UDSEntry>
#include "remoteurlhelper.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QList>
#include <QLocale>
#include <QMimeDatabase>
#include <QMimeType>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <functional>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

inline const QUrl kThisPcUrl(QStringLiteral("thispc:/"));

inline bool isAdminUrl(const QUrl &url)
{
    return url.scheme() == QStringLiteral("admin");
}


inline QString localPathForFileOrAdmin(const QUrl &url)
{
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }

    if (isAdminUrl(url)) {
        return url.path();
    }

    return {};
}

inline QUrl adminUrlForLocalPath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }

    QUrl url;
    url.setScheme(QStringLiteral("admin"));
    url.setPath(QDir::cleanPath(path));
    return url;
}

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

struct FileInfo
{
    QString name;
    QString mimeType;
    QString iconName;
    QUrl url;
    bool isDir = false;
    qint64 size = -1;
    qint64 modificationTime = 0;
    bool isHidden = false;
};


struct DriveInfo
{
    QString name;
    QString freeText;
    QString capacityText;
    QString usedText;
    QString fileSystem;
    QString mountPoint;
    QUrl targetUrl;
    QString iconName;
    int usedPercent = 0;
    QString id;
    QString udi;
    bool isMounted = true;
    bool isRemovable = false;
};

inline bool operator==(const DriveInfo &a, const DriveInfo &b)
{
    return a.id == b.id
        && a.name == b.name
        && a.freeText == b.freeText
        && a.capacityText == b.capacityText
        && a.usedText == b.usedText
        && a.fileSystem == b.fileSystem
        && a.mountPoint == b.mountPoint
        && a.targetUrl == b.targetUrl
        && a.iconName == b.iconName
        && a.usedPercent == b.usedPercent
        && a.udi == b.udi
        && a.isMounted == b.isMounted
        && a.isRemovable == b.isRemovable;
}

inline bool operator!=(const DriveInfo &a, const DriveInfo &b)
{
    return !(a == b);
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


inline QString formatModificationTime(qint64 seconds)
{
    if (seconds <= 0) {
        return QStringLiteral("—");
    }

    const QDateTime dateTime =
        QDateTime::fromSecsSinceEpoch(seconds);

    return QLocale().toString(
        dateTime,
        QLocale::ShortFormat);
}


inline QMimeType resolvedMimeType(
    const FileInfo &file,
    QMimeDatabase &mimeDatabase)
{
    if (file.isDir) {
        return mimeDatabase.mimeTypeForName(
            QStringLiteral("inode/directory"));
    }

    if (!file.mimeType.isEmpty()
        && file.mimeType != QStringLiteral("application/octet-stream")) {
        const QMimeType known =
            mimeDatabase.mimeTypeForName(file.mimeType);
        if (known.isValid() && !known.isDefault()) {
            return known;
        }
    }

    // Some KIO workers/filesystems do not provide UDS_MIME_TYPE. Resolve by
    // extension first; this is enough to get PNG/JPEG/PDF/archive/etc icons
    // without synchronously reading file contents.
    const QString probe =
        file.url.isLocalFile()
            ? file.url.toLocalFile()
            : file.name;

    QMimeType mime =
        mimeDatabase.mimeTypeForFile(
            probe,
            QMimeDatabase::MatchExtension);

    if (!mime.isValid() || mime.isDefault()) {
        mime = mimeDatabase.mimeTypeForFile(
            file.name,
            QMimeDatabase::MatchExtension);
    }

    return mime;
}


inline QString fileTypeLabel(
    const FileInfo &file,
    QMimeDatabase &mimeDatabase)
{
    if (file.isDir) {
        return trLocal("Folder", "Folder");
    }

    const QMimeType mime =
        resolvedMimeType(file, mimeDatabase);

    if (mime.isValid() && !mime.isDefault()) {
        if (!mime.comment().isEmpty()) {
            return mime.comment();
        }
        return mime.name();
    }

    return trLocal("Plik", "File");
}


inline QUrl normalizedUrl(QUrl url)
{
    url = RemoteUrlHelper::sanitizeUrl(url);
    if (RemoteUrlHelper::isRemoteUrl(url) && url.path().isEmpty())
        url.setPath(QStringLiteral("/"));
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


inline QString urlForDisplay(const QUrl &url)
{
    if (sameLocation(url, kThisPcUrl)) {
        return QStringLiteral("thispc:/");
    }

    if (url.isLocalFile()) {
        return url.toLocalFile();
    }

    return normalizedUrl(url).toDisplayString(QUrl::PreferLocalFile);
}


#include "remoteurlhelper.h"

inline QUrl urlFromUserText(const QString &text)
{
    const QString trimmed = text.trimmed();

    if (trimmed.isEmpty()) {
        return {};
    }

    const int colon = trimmed.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        const QString schemeCandidate = trimmed.left(colon).toLower();
        if (RemoteUrlHelper::isRemoteScheme(schemeCandidate)) {
            return RemoteUrlHelper::parseUserInput(trimmed);
        }
    }

    if (trimmed.startsWith(QLatin1Char('/'))) {
        return QUrl::fromLocalFile(trimmed);
    }

    if (trimmed.contains(QStringLiteral(":/"))) {
        return QUrl(trimmed);
    }

    return QUrl::fromUserInput(trimmed);
}


inline QIcon themedIcon(const QString &preferred,
                 const QString &fallback = QStringLiteral("folder"))
{
    QIcon icon = QIcon::fromTheme(preferred);
    if (icon.isNull()) {
        icon = QIcon::fromTheme(fallback);
    }
    return icon;
}


inline QUrl childUrlForEntry(const QUrl &base, const KIO::UDSEntry &entry)
{
    const QString target =
        entry.stringValue(KIO::UDSEntry::UDS_TARGET_URL);
    if (!target.isEmpty()) {
        return QUrl(target);
    }

    const QString explicitUrl =
        entry.stringValue(KIO::UDSEntry::UDS_URL);
    if (!explicitUrl.isEmpty()) {
        return QUrl(explicitUrl);
    }

    QUrl child = base;
    QString path = child.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }

    path += entry.stringValue(KIO::UDSEntry::UDS_NAME);
    child.setPath(path);
    return child;
}


inline bool entryIsDirectory(const KIO::UDSEntry &entry)
{
    const QString mime =
        entry.stringValue(KIO::UDSEntry::UDS_MIME_TYPE);

    if (mime == QStringLiteral("inode/directory")) {
        return true;
    }

#ifndef Q_OS_WIN
    const long long type =
        entry.numberValue(KIO::UDSEntry::UDS_FILE_TYPE, 0);
    if (type != 0 && S_ISDIR(static_cast<mode_t>(type))) {
        return true;
    }
#endif

    return false;
}


inline FileInfo fileInfoForEntry(
    const QUrl &base,
    const KIO::UDSEntry &entry)
{
    FileInfo file;
    file.name = entry.stringValue(
        KIO::UDSEntry::UDS_DISPLAY_NAME);
    if (file.name.isEmpty()) {
        file.name = entry.stringValue(
            KIO::UDSEntry::UDS_NAME);
    }
    file.mimeType = entry.stringValue(
        KIO::UDSEntry::UDS_MIME_TYPE);
    file.iconName = entry.stringValue(
        KIO::UDSEntry::UDS_ICON_NAME);
    file.url = childUrlForEntry(base, entry);
    file.isDir = entryIsDirectory(entry);
    file.size = entry.numberValue(
        KIO::UDSEntry::UDS_SIZE,
        -1);
    file.modificationTime = entry.numberValue(
        KIO::UDSEntry::UDS_MODIFICATION_TIME,
        0);
    const QString rawName = entry.stringValue(KIO::UDSEntry::UDS_NAME);
    file.isHidden = entry.contains(KIO::UDSEntry::UDS_HIDDEN)
        ? entry.numberValue(KIO::UDSEntry::UDS_HIDDEN, 0) != 0
        : rawName.startsWith(QLatin1Char('.'));
    return file;
}


inline void sortDirectoryFiles(
    QList<FileInfo> &files,
    int sortKey,
    bool ascending,
    const std::function<QString(const FileInfo &)> &typeKey)
{
    std::sort(
        files.begin(),
        files.end(),
        [sortKey, ascending, &typeKey](
            const FileInfo &a,
            const FileInfo &b) {
        // Keep folders together before files, like Explorer/Dolphin.
        if (a.isDir != b.isDir) {
            return a.isDir;
        }

        int comparison = 0;

        switch (sortKey) {
        case 1:
            comparison =
                typeKey(a).localeAwareCompare(typeKey(b));
            break;
        case 2:
            if (a.size < b.size) {
                comparison = -1;
            } else if (a.size > b.size) {
                comparison = 1;
            }
            break;
        case 3:
            if (a.modificationTime < b.modificationTime) {
                comparison = -1;
            } else if (a.modificationTime > b.modificationTime) {
                comparison = 1;
            }
            break;
        case 0:
        default:
            comparison = a.name.localeAwareCompare(b.name);
            break;
        }

        if (comparison == 0) {
            comparison = a.name.localeAwareCompare(b.name);
        }

        return ascending
            ? comparison < 0
            : comparison > 0;
    });
}

inline QUrl childUrlWithName(const QUrl &directory, const QString &name)
{
    QUrl child = directory;
    QString path = child.path();

    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }

    path += name;
    child.setPath(path);
    return child;
}
