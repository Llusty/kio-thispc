/*
 * Portable XDG/Qt6 path helpers compatible with Qt 6.4.2 and newer.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "portablepaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStringList>
#include <QtGlobal>

namespace ThisPc::PortablePaths {

namespace {

bool isUsableAbsoluteDir(const QString &path)
{
    if (path.isEmpty() || !path.startsWith(QLatin1Char('/')) || path.contains(QChar(u'\0'))) {
        return false;
    }
    const QString cleaned = QDir::cleanPath(path);
    return cleaned.startsWith(QLatin1Char('/')) && cleaned != QStringLiteral("/");
}

QString validHomeDir()
{
    const QString envHome = QFile::decodeName(qgetenv("HOME"));
    if (isUsableAbsoluteDir(envHome)) {
        return QDir::cleanPath(envHome);
    }
    return {};
}

bool isRootStatePath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(path);
    return cleaned == QStringLiteral("/")
        || cleaned == QStringLiteral("/.local/state")
        || cleaned == QStringLiteral("/.qttest/state");
}

QString sanitizeMetadataComponent(const QString &raw)
{
    if (raw.isEmpty() || raw.contains(QChar(u'\0'))
        || raw.contains(QLatin1Char('/')) || raw.contains(QLatin1Char('\\'))) {
        return {};
    }
    if (raw == QStringLiteral(".") || raw == QStringLiteral("..") || raw.trimmed().isEmpty()) {
        return {};
    }
    return raw;
}

QString sanitizeRelativeSubdir(const QString &raw)
{
    if (raw.isEmpty() || raw.contains(QChar(u'\0'))
        || raw.startsWith(QLatin1Char('/')) || raw.contains(QLatin1Char('\\'))
        || QDir::isAbsolutePath(raw)) {
        return {};
    }

    const QStringList rawSegments = raw.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    QStringList cleanSegments;
    cleanSegments.reserve(rawSegments.size());
    for (const QString &seg : rawSegments) {
        if (seg.isEmpty() || seg == QStringLiteral(".") || seg == QStringLiteral("..")
            || seg.trimmed().isEmpty()) {
            return {};
        }
        cleanSegments.append(seg);
    }
    if (cleanSegments.isEmpty()) {
        return {};
    }
    return cleanSegments.join(QLatin1Char('/'));
}

QString xdgFallbackGenericStateLocation()
{
    if (QStandardPaths::isTestModeEnabled()) {
        const QString home = validHomeDir();
        if (!home.isEmpty()) {
            return QDir::cleanPath(home + QStringLiteral("/.qttest/state"));
        }
        return {};
    }

    // Per XDG Base Directory Specification: do NOT trim leading/trailing spaces;
    // a value not starting with '/' is relative and must be ignored.
    const QString envStateHome = QFile::decodeName(qgetenv("XDG_STATE_HOME"));
    if (isUsableAbsoluteDir(envStateHome) && !isRootStatePath(envStateHome)) {
        return QDir::cleanPath(envStateHome);
    }

    const QString home = validHomeDir();
    if (!home.isEmpty()) {
        return QDir::cleanPath(home + QStringLiteral("/.local/state"));
    }

    return {};
}

QString joinAppRelativePath(const QString &baseDir, const QString &org, const QString &app)
{
    QString result = QDir::cleanPath(baseDir);
    if (!org.isEmpty()) {
        result += QLatin1Char('/') + org;
    }
    if (!app.isEmpty()) {
        result += QLatin1Char('/') + app;
    }
    return QDir::cleanPath(result);
}

} // namespace

QString genericStateLocation()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    if (isUsableAbsoluteDir(stateDir) && !isRootStatePath(stateDir)) {
        return QDir::cleanPath(stateDir);
    }
#endif
    return xdgFallbackGenericStateLocation();
}

QString resolveAppStateLocation(const QString &genericBase,
                                const QString &orgName,
                                const QString &appName,
                                const QString &fallbackSubdir)
{
    if (!isUsableAbsoluteDir(genericBase) || isRootStatePath(genericBase)) {
        return {};
    }

    const QString org = sanitizeMetadataComponent(orgName);
    const QString app = sanitizeMetadataComponent(appName);
    if (!org.isEmpty() || !app.isEmpty()) {
        return joinAppRelativePath(genericBase, org, app);
    }

    const QString safeFallback = sanitizeRelativeSubdir(fallbackSubdir);
    if (!safeFallback.isEmpty()) {
        return QDir::cleanPath(genericBase + QLatin1Char('/') + safeFallback);
    }
    return QDir::cleanPath(genericBase);
}

QString appStateLocation(const QString &fallbackSubdir)
{
    const QString rawOrg = QCoreApplication::organizationName();
    const QString rawApp = QCoreApplication::applicationName();
    const QString org = sanitizeMetadataComponent(rawOrg);
    const QString app = sanitizeMetadataComponent(rawApp);
    const bool hasMetadata = !org.isEmpty() || !app.isEmpty();

    if (hasMetadata) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        // Use Qt's native StateLocation when metadata is clean, matching
        // production Qt 6.7+ behavior identically.
        if (org == rawOrg && app == rawApp) {
            const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
            if (isUsableAbsoluteDir(stateDir) && !isRootStatePath(stateDir)) {
                return QDir::cleanPath(stateDir);
            }
        }
#endif
        const QString generic = genericStateLocation();
        const QString resolved = resolveAppStateLocation(generic, org, app, fallbackSubdir);
        if (!resolved.isEmpty()) {
            return resolved;
        }
    } else {
        const QString generic = genericStateLocation();
        const QString resolved = resolveAppStateLocation(generic, QString(), QString(), fallbackSubdir);
        if (!resolved.isEmpty()) {
            return resolved;
        }
    }

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return (isUsableAbsoluteDir(appData) && !isRootStatePath(appData)) ? QDir::cleanPath(appData) : QString();
}

QString appDataLocation()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString appCacheLocation()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString appConfigLocation()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

} // namespace ThisPc::PortablePaths
