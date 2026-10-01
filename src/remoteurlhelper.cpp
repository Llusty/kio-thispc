/*
 * Pure URL parsing, validation, sanitization, and navigation semantics for remote locations.
 * Free of GUI / widget dependencies.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "remoteurlhelper.h"
#include <QRegularExpression>

namespace RemoteUrlHelper {

bool isRemoteScheme(const QString &scheme)
{
    const QString s = scheme.toLower();
    return s == QStringLiteral("smb")
        || s == QStringLiteral("sftp")
        || s == QStringLiteral("ftp")
        || s == QStringLiteral("webdav")
        || s == QStringLiteral("webdavs");
}

bool isRemoteUrl(const QUrl &url)
{
    return url.isValid() && isRemoteScheme(url.scheme());
}

bool isRemoteDiscoveryUrl(const QUrl &url)
{
    if (!url.isValid()) return false;
    const QString s = url.scheme().toLower();
    if (s == QStringLiteral("remote")) return true;
    if (s == QStringLiteral("smb") && url.host().isEmpty()) return true;
    return false;
}

bool isValidRemoteUrl(const QUrl &url)
{
    return isRemoteUrl(url) && !url.host().isEmpty();
}

bool isSameRemoteLocation(const QUrl &a, const QUrl &b)
{
    if (a.scheme().compare(b.scheme(), Qt::CaseInsensitive) != 0) return false;
    if (a.host().compare(b.host(), Qt::CaseInsensitive) != 0) return false;
    if (a.port() != b.port()) return false;
    if (a.userName() != b.userName()) return false;

    QString pathA = a.path();
    QString pathB = b.path();
    while (pathA.size() > 1 && pathA.endsWith(QLatin1Char('/'))) pathA.chop(1);
    while (pathB.size() > 1 && pathB.endsWith(QLatin1Char('/'))) pathB.chop(1);
    if (pathA.isEmpty()) pathA = QStringLiteral("/");
    if (pathB.isEmpty()) pathB = QStringLiteral("/");
    return pathA == pathB;
}

QUrl sanitizeUrl(const QUrl &url)
{
    QUrl copy = url;
    if (!copy.password().isEmpty() || copy.userInfo().contains(QLatin1Char(':'))) {
        copy.setPassword(QString());
    }
    return copy;
}

QUrl parseUserInput(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) return {};

    const int schemeSep = trimmed.indexOf(QStringLiteral("://"));
    if (schemeSep <= 0) {
        // If the user typed "smb:something" or "sftp:something" without "://",
        // ensure it is rejected deterministically rather than treated as a relative local file.
        const int singleColon = trimmed.indexOf(QLatin1Char(':'));
        if (singleColon > 0) {
            const QString potentialScheme = trimmed.left(singleColon).toLower();
            if (isRemoteScheme(potentialScheme)) {
                return {};
            }
        }
        return {};
    }

    const QString scheme = trimmed.left(schemeSep).toLower();
    if (!isRemoteScheme(scheme)) {
        return {};
    }

    QUrl url(trimmed);
    if (!url.isValid() || url.scheme().toLower() != scheme || url.host().isEmpty()) {
        return {};
    }

    url = sanitizeUrl(url);

    // Normalize path: if empty, set to "/" so workers can list root/shares
    if (url.path().isEmpty()) {
        url.setPath(QStringLiteral("/"));
    }

    return url;
}

QUrl parentUrl(const QUrl &url)
{
    if (!isRemoteUrl(url)) return {};

    if (isRemoteDiscoveryUrl(url)) {
        return QUrl(QStringLiteral("remote:/"));
    }

    QString path = url.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }

    if (path.isEmpty() || path == QStringLiteral("/")) {
        return QUrl(QStringLiteral("thispc:/"));
    }

    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) {
        QUrl root = url;
        root.setPath(QStringLiteral("/"));
        return root;
    }

    QUrl parent = url;
    parent.setPath(path.left(slash));
    return parent;
}

QString rootLabel(const QUrl &url)
{
    QString label;
    if (!url.userName().isEmpty()) {
        label += url.userName() + QLatin1Char('@');
    }
    label += url.host();
    if (url.port() > 0) {
        label += QStringLiteral(":%1").arg(url.port());
    }
    return label;
}

QUrl rootUrl(const QUrl &url)
{
    QUrl root = sanitizeUrl(url);
    root.setPath(QStringLiteral("/"));
    root.setQuery(QString());
    root.setFragment(QString());
    return root;
}

QString iconForRemoteUrl(const QUrl &url)
{
    const QString s = url.scheme().toLower();
    const bool isRoot = url.path().isEmpty() || url.path() == QStringLiteral("/");
    if (s == QStringLiteral("smb")) {
        return isRoot ? QStringLiteral("network-server") : QStringLiteral("folder-remote");
    }
    if (s == QStringLiteral("sftp")) {
        return isRoot ? QStringLiteral("network-server") : QStringLiteral("folder-remote");
    }
    if (s == QStringLiteral("ftp")) {
        return QStringLiteral("folder-remote");
    }
    if (s == QStringLiteral("webdav") || s == QStringLiteral("webdavs")) {
        return isRoot ? QStringLiteral("network-workgroup") : QStringLiteral("folder-remote");
    }
    return QStringLiteral("folder-remote");
}

QString sanitizeErrorMessage(const QString &error, const QUrl &url)
{
    QString msg = error;
    if (!url.password().isEmpty()) {
        msg.replace(url.password(), QStringLiteral("***"));
    }
    static const QRegularExpression credsPattern(QStringLiteral("://([^:]+):([^@]+)@"));
    msg.replace(credsPattern, QStringLiteral("://\\1:***@"));
    return msg;
}

} // namespace RemoteUrlHelper
