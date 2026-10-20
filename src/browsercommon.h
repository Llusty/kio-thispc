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

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QLocale>
#include <QMimeDatabase>
#include <QMimeType>
#include <QSet>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <vector>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

inline const QUrl kThisPcUrl(QStringLiteral("thispc:/"));

inline bool isAdminUrl(const QUrl &url)
{
    return url.scheme() == QStringLiteral("admin");
}

enum class DirectoryReloadMode {
    Navigation,   // Navigating to a new directory: clears previous view and displays loading status
    QuietRefresh  // Refreshing current directory: retains current items until new data arrives
};


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


enum class DriveDeviceKind {
    StorageVolume,
    PortableMediaMtp
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
    DriveDeviceKind deviceKind = DriveDeviceKind::StorageVolume;

    bool isMtpDevice() const
    {
        return deviceKind == DriveDeviceKind::PortableMediaMtp
            || (targetUrl.isValid()
                && targetUrl.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0);
    }

    bool canNavigate() const
    {
        if (isMtpDevice()) {
            return targetUrl.isValid()
                && targetUrl.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0;
        }
        return isMounted && targetUrl.isValid();
    }

    bool canMount() const
    {
        if (isMtpDevice()) {
            return false;
        }
        return !isMounted && !udi.isEmpty();
    }

    bool canUnmount() const
    {
        if (isMtpDevice()) {
            return false;
        }
        return isRemovable && isMounted && !udi.isEmpty();
    }

    bool canEject(bool solidCanEject) const
    {
        if (isMtpDevice()) {
            return false;
        }
        return isRemovable && solidCanEject && !udi.isEmpty();
    }

    bool canSafelyRemove(bool solidCanSafelyRemove) const
    {
        if (isMtpDevice()) {
            return false;
        }
        return isRemovable && solidCanSafelyRemove && !udi.isEmpty();
    }

    bool canCopyMountPoint() const
    {
        if (isMtpDevice()) {
            return false;
        }
        return isMounted && !mountPoint.isEmpty();
    }

    bool hasCapacityInformation() const
    {
        if (isMtpDevice()) {
            return !capacityText.isEmpty()
                && capacityText != QStringLiteral("—")
                && !freeText.isEmpty()
                && freeText != QStringLiteral("—");
        }
        return isMounted
            && !capacityText.isEmpty()
            && capacityText != QStringLiteral("—");
    }
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
        && a.isRemovable == b.isRemovable
        && a.deviceKind == b.deviceKind;
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

inline bool isDeletableMtpUrl(const QUrl &url)
{
    if (url.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    const QString clean = QDir::cleanPath(url.path());
    QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!url.host().isEmpty()) {
        parts.prepend(url.host());
    }
    return parts.size() >= 3;
}

inline bool isProtectedRootUrl(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty() || sameLocation(url, kThisPcUrl)) {
        return true;
    }
    if (url.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0) {
        return !isDeletableMtpUrl(url);
    }
    if (url.isLocalFile()) {
        const QString path = QDir::cleanPath(url.toLocalFile());
        return path.isEmpty() || path == QStringLiteral("/");
    }
    const QString path = QDir::cleanPath(url.path());
    return path.isEmpty() || path == QStringLiteral("/");
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
            if (!u.isValid()) continue;
            const QUrl parent = parentDirectoryUrl(u);
            if (parent.isValid()) {
                ctx.sourceParentDirectories.insert(normalizedUrl(parent));
            }
        }
        return ctx;
    }
};

namespace MtpRedirectionRegistry {
inline QHash<QString, QString> &udiToDeviceSegmentMap()
{
    static QHash<QString, QString> map;
    return map;
}

inline QHash<QString, QString> &deviceSegmentToUdiMap()
{
    static QHash<QString, QString> map;
    return map;
}

inline void recordRedirection(const QUrl &requestedUrl, const QUrl &redirectedUrl)
{
    if (!requestedUrl.isValid() || !redirectedUrl.isValid()) {
        return;
    }
    if (requestedUrl.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) != 0
        || redirectedUrl.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) != 0) {
        return;
    }
    if (!requestedUrl.path().startsWith(QStringLiteral("udi="))) {
        return;
    }
    const QString udi = requestedUrl.path().mid(4);
    const QStringList segments = redirectedUrl.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!udi.isEmpty() && !segments.isEmpty()) {
        const QString &segment = segments.first();
        const QString previousSegment = udiToDeviceSegmentMap().value(udi);
        if (!previousSegment.isEmpty() && previousSegment != segment
            && deviceSegmentToUdiMap().value(previousSegment) == udi) {
            deviceSegmentToUdiMap().remove(previousSegment);
        }
        const QString previousUdi = deviceSegmentToUdiMap().value(segment);
        if (!previousUdi.isEmpty() && previousUdi != udi
            && udiToDeviceSegmentMap().value(previousUdi) == segment) {
            udiToDeviceSegmentMap().remove(previousUdi);
        }
        udiToDeviceSegmentMap().insert(udi, segment);
        deviceSegmentToUdiMap().insert(segment, udi);
    }
}

inline QString redirectedDeviceSegmentForUdi(const QString &udi)
{
    return udiToDeviceSegmentMap().value(udi);
}

inline QString udiForRedirectedDeviceSegment(const QString &segment)
{
    return deviceSegmentToUdiMap().value(segment);
}

inline void removeRedirectionForUdi(const QString &udi)
{
    if (udi.isEmpty()) {
        return;
    }
    const QString segment = udiToDeviceSegmentMap().take(udi);
    if (!segment.isEmpty() && deviceSegmentToUdiMap().value(segment) == udi) {
        deviceSegmentToUdiMap().remove(segment);
    }
}

inline void clear()
{
    udiToDeviceSegmentMap().clear();
    deviceSegmentToUdiMap().clear();
}
} // namespace MtpRedirectionRegistry

inline bool urlBelongsToMtpDrive(const QUrl &url, const DriveInfo &drive)
{
    if (!url.isValid() || !drive.isMtpDevice()
        || url.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    if (url.path().startsWith(QStringLiteral("udi="))) {
        const QString udi = url.path().mid(4);
        return (!drive.udi.isEmpty() && udi == drive.udi)
            || sameLocation(url, drive.targetUrl);
    }
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }
    const QString &deviceSegment = parts.first();
    const QString mappedUdi = MtpRedirectionRegistry::udiForRedirectedDeviceSegment(deviceSegment);
    if (!mappedUdi.isEmpty() && !drive.udi.isEmpty()) {
        return drive.udi == mappedUdi;
    }
    if (!drive.udi.isEmpty()) {
        const QString mappedSegment = MtpRedirectionRegistry::redirectedDeviceSegmentForUdi(drive.udi);
        if (!mappedSegment.isEmpty() && deviceSegment == mappedSegment) {
            return true;
        }
    }
    return !drive.name.isEmpty() && deviceSegment == drive.name;
}

inline QUrl resolveReconnectedMtpUrl(const QUrl &url, const QList<DriveInfo> &drives)
{
    if (!url.isValid() || url.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) != 0) {
        return url;
    }
    if (url.path().startsWith(QStringLiteral("udi="))) {
        const QString oldUdi = url.path().mid(4);
        for (const DriveInfo &d : drives) {
            if (d.isMtpDevice() && d.udi == oldUdi) {
                return url;
            }
        }
        QList<DriveInfo> mtpDrives;
        for (const DriveInfo &d : drives) {
            if (d.isMtpDevice()) {
                mtpDrives.push_back(d);
            }
        }
        if (mtpDrives.size() == 1) {
            return mtpDrives.first().targetUrl;
        }
        const QString oldSegment = MtpRedirectionRegistry::redirectedDeviceSegmentForUdi(oldUdi);
        if (!oldSegment.isEmpty()) {
            QList<DriveInfo> matchingNamed;
            for (const DriveInfo &d : mtpDrives) {
                if (d.name == oldSegment) {
                    matchingNamed.push_back(d);
                }
            }
            if (matchingNamed.size() == 1) {
                return matchingNamed.first().targetUrl;
            }
        }
        return url;
    }
    return url;
}

inline QString mtpErrorMessageForUrl(const QUrl &url, const QList<DriveInfo> &drives)
{
    bool stillConnected = false;
    for (const DriveInfo &d : drives) {
        if (urlBelongsToMtpDrive(url, d)) {
            stillConnected = true;
            break;
        }
    }
    return stillConnected
        ? trLocal("Nie można uzyskać dostępu do urządzenia. Sprawdź połączenie i odblokowanie telefonu.",
                  "Could not access device. Check the connection and unlock the phone.")
        : trLocal("Urządzenie MTP zostało odłączone.", "MTP device disconnected.");
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
    if (files.size() <= 1) {
        return;
    }

    QLocale locale;
    if (locale.language() == QLocale::C) {
        locale = QLocale(QLocale::English);
    }
    QCollator collator(locale);
    collator.setNumericMode(true);

    if (sortKey == 1 && typeKey) {
        struct DecoratedItem {
            FileInfo file;
            QString type;
        };
        std::vector<DecoratedItem> decorated;
        decorated.reserve(files.size());
        for (auto &f : files) {
            QString t = typeKey(f);
            decorated.push_back({std::move(f), std::move(t)});
        }

        std::stable_sort(
            decorated.begin(),
            decorated.end(),
            [ascending, &collator](const DecoratedItem &a, const DecoratedItem &b) {
                // Keep folders together before files, like Explorer/Dolphin.
                if (a.file.isDir != b.file.isDir) {
                    return a.file.isDir;
                }

                int comparison = collator.compare(a.type, b.type);

                if (comparison == 0) {
                    comparison = collator.compare(a.file.name, b.file.name);
                }

                if (comparison == 0) {
                    comparison = collator.compare(a.file.url.toString(), b.file.url.toString());
                }

                return ascending
                    ? comparison < 0
                    : comparison > 0;
            });

        for (size_t i = 0; i < decorated.size(); ++i) {
            files[i] = std::move(decorated[i].file);
        }
        return;
    }

    std::stable_sort(
        files.begin(),
        files.end(),
        [sortKey, ascending, &typeKey, &collator](
            const FileInfo &a,
            const FileInfo &b) {
        // Keep folders together before files, like Explorer/Dolphin.
        if (a.isDir != b.isDir) {
            return a.isDir;
        }

        int comparison = 0;

        switch (sortKey) {
        case 1:
            if (typeKey) {
                comparison = collator.compare(typeKey(a), typeKey(b));
            }
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
            comparison = collator.compare(a.name, b.name);
            break;
        }

        if (comparison == 0) {
            comparison = collator.compare(a.name, b.name);
        }

        if (comparison == 0) {
            comparison = collator.compare(a.url.toString(), b.url.toString());
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
