/* SPDX-License-Identifier: MIT */

#include "locationpresentation.h"
#include "browsercommon.h"
#include "searchcontroller.h"

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

namespace LocationPresentation
{
namespace
{
QString searchTitle(const QUrl &url, bool emptyQueryHasGenericTitle)
{
    const QString query = searchQueryFromUrl(url);
    if (query.isEmpty() && emptyQueryHasGenericTitle) {
        return trLocal("Wyniki wyszukiwania", "Search results");
    }
    return (isPolish()
            ? QStringLiteral("Wyniki dla: %1")
            : QStringLiteral("Results for: %1"))
        .arg(query);
}

bool matchingDrive(
    const QUrl &url,
    const QVector<DriveInfo> &drives,
    DriveInfo *match = nullptr)
{
    for (const DriveInfo &drive : drives) {
        if (drive.isMounted && drive.targetUrl.isValid() && sameLocation(drive.targetUrl, url)) {
            if (match) *match = drive;
            return true;
        }
    }
    return false;
}
} // namespace

QString primaryTitle(const QUrl &url, const QVector<DriveInfo> &drives)
{
    if (sameLocation(url, kThisPcUrl)) return trLocal("Ten komputer", "This PC");
    if (isSearchLocation(url)) return searchTitle(url, true);

    DriveInfo drive;
    if (matchingDrive(url, drives, &drive)) return drive.name;

    if (url.isLocalFile()) {
        const QString path = url.toLocalFile();
        if (path == QDir::homePath()) return trLocal("Katalog domowy", "Home");
        const QString fileName = QFileInfo(path).fileName();
        return fileName.isEmpty() ? path : fileName;
    }
    if (url.scheme() == QStringLiteral("trash")) return trLocal("Kosz", "Trash");
    if (url.scheme() == QStringLiteral("remote")) return trLocal("Sieć", "Network");

    const QString last = QFileInfo(url.path()).fileName();
    return last.isEmpty() ? url.toDisplayString() : last;
}

QString splitTitle(const QUrl &url)
{
    if (sameLocation(url, kThisPcUrl)) return trLocal("Ten komputer", "This PC");
    if (isSearchLocation(url)) return searchTitle(url, false);

    if (url.isLocalFile()) {
        const QString path = QDir::cleanPath(url.toLocalFile());
        const QFileInfo info(path);
        QStorageInfo storage(path);
        if (path == QDir::cleanPath(storage.rootPath())) {
            const QString label = storage.displayName();
            if (!label.trimmed().isEmpty()) return label;
        }
        if (!info.fileName().isEmpty()) return info.fileName();
    }

    const QString name = QFileInfo(url.path()).fileName();
    return name.isEmpty() ? urlForDisplay(url) : name;
}

QString splitLocationText(const QUrl &url)
{
    if (sameLocation(url, kThisPcUrl)) return trLocal("Ten komputer", "This PC");
    if (isSearchLocation(url)) return searchTitle(url, false);

    if (url.isLocalFile()) {
        const QString path = QDir::cleanPath(url.toLocalFile());
        QStorageInfo storage(path);
        const QString root = QDir::cleanPath(storage.rootPath());
        QString rootName = storage.displayName();
        if (rootName.trimmed().isEmpty()) rootName = QFileInfo(root).fileName();
        if (rootName.trimmed().isEmpty() || root == QStringLiteral("/")) {
            rootName = trLocal("System", "System");
        }

        QString result = trLocal("Ten komputer", "This PC")
            + QStringLiteral("  ›  ") + rootName;
        const QString relative = QDir(root).relativeFilePath(path);
        if (!relative.isEmpty() && relative != QStringLiteral(".")) {
            for (const QString &part : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                result += QStringLiteral("  ›  ") + part;
            }
        }
        return result;
    }

    if (isAdminUrl(url)) {
        QString result = trLocal("Administrator", "Administrator");
        for (const QString &part : url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            result += QStringLiteral("  ›  ") + part;
        }
        return result;
    }
    return urlForDisplay(url);
}

QString contentHeaderText(const QUrl &url)
{
    if (sameLocation(url, kThisPcUrl)) return trLocal("Ten komputer", "This PC");
    if (isSearchLocation(url)) return searchTitle(url, true);
    return urlForDisplay(url);
}

QString iconName(const QUrl &url)
{
    if (sameLocation(url, kThisPcUrl)) return QStringLiteral("computer");
    if (isSearchLocation(url)) return QStringLiteral("system-search");
    if (isAdminUrl(url)) return QStringLiteral("security-high");
    if (url.isLocalFile()) {
        const QString path = QDir::cleanPath(url.toLocalFile());
        if (QDir::cleanPath(QStorageInfo(path).rootPath()) == path) {
            return QStringLiteral("drive-harddisk");
        }
    }
    return QStringLiteral("folder");
}

QVector<Segment> localPathSegments(const QUrl &url, const QVector<DriveInfo> &drives)
{
    QVector<Segment> segments;
    if (!url.isLocalFile()) return segments;

    const QString path = QDir::cleanPath(url.toLocalFile());
    QString baseName;
    QUrl baseUrl;
    bool baseIsDrive = false;
    for (const DriveInfo &drive : drives) {
        if (!drive.targetUrl.isLocalFile()) continue;
        const QString drivePath = QDir::cleanPath(drive.targetUrl.toLocalFile());
        if (path == drivePath || path.startsWith(drivePath + QDir::separator())) {
            baseName = drive.name;
            baseUrl = drive.targetUrl;
            baseIsDrive = true;
            break;
        }
    }

    const QString homePath = QDir::cleanPath(QDir::homePath());
    if (baseUrl.isEmpty()
        && (path == homePath || path.startsWith(homePath + QDir::separator()))) {
        baseName = trLocal("Katalog domowy", "Home");
        baseUrl = QUrl::fromLocalFile(homePath);
    }
    if (baseUrl.isEmpty()) {
        baseName = QStringLiteral("/");
        baseUrl = QUrl::fromLocalFile(QStringLiteral("/"));
    }

    if (baseIsDrive) {
        segments.append({trLocal("Ten komputer", "This PC"), kThisPcUrl,
                         QStringLiteral("computer")});
    }
    segments.append({baseName, baseUrl,
                     baseName == QStringLiteral("/")
                         ? QStringLiteral("folder-root")
                         : (baseIsDrive ? QStringLiteral("drive-harddisk")
                                        : QStringLiteral("folder"))});

    const QString relative = QDir(baseUrl.toLocalFile()).relativeFilePath(path);
    if (!relative.isEmpty() && relative != QStringLiteral(".")) {
        QString cumulative = baseUrl.toLocalFile();
        for (const QString &part : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            cumulative = QDir(cumulative).filePath(part);
            segments.append({part, QUrl::fromLocalFile(cumulative), {}});
        }
    }
    return segments;
}

QVector<Segment> adminPathSegments(const QUrl &url)
{
    QVector<Segment> segments;
    if (!isAdminUrl(url)) return segments;
    QUrl cumulative = url;
    cumulative.setPath(QStringLiteral("/"));
    segments.append({trLocal("Administrator", "Administrator"), cumulative,
                     QStringLiteral("security-high")});
    QString path;
    for (const QString &part : url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        path += QLatin1Char('/') + part;
        cumulative.setPath(path);
        segments.append({part, cumulative, {}});
    }
    return segments;
}

QUrl parentUrl(const QUrl &url, const QVector<DriveInfo> &drives, ParentProfile profile)
{
    if ((profile == ParentProfile::Split && !url.isValid())
        || sameLocation(url, kThisPcUrl)) {
        return {};
    }
    if (isSearchLocation(url)) {
        const QUrl base = searchBaseFromUrl(url);
        return base.isValid() ? base : kThisPcUrl;
    }
    if (matchingDrive(url, drives)) return kThisPcUrl;

    QUrl parent = url;
    QString path = parent.path();
    if (path.isEmpty() || path == QStringLiteral("/")) return kThisPcUrl;
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) path.chop(1);
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    path = slash <= 0 ? QStringLiteral("/") : path.left(slash);
    parent.setPath(path);

    if (profile == ParentProfile::Split) {
        parent.setQuery(QString());
        if (parent.isLocalFile()
            && QFileInfo(parent.toLocalFile()).absoluteFilePath()
                == QFileInfo(url.toLocalFile()).absoluteFilePath()) {
            return kThisPcUrl;
        }
        return normalizedUrl(parent);
    }
    return parent;
}

} // namespace LocationPresentation
