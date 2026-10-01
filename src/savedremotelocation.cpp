/*
 * Implementation of data model and persistent store for Saved Remote Locations.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "savedremotelocation.h"
#include "remoteurlhelper.h"

#include <QCryptographicHash>
#include <QStringList>

bool SavedRemoteLocation::isValid() const
{
    return !id.isEmpty()
        && !displayName.isEmpty()
        && RemoteUrlHelper::isRemoteUrl(url)
        && RemoteUrlHelper::isValidRemoteUrl(url)
        && url.password().isEmpty();
}

SavedRemoteLocationsStore::SavedRemoteLocationsStore(QObject *parent)
    : QObject(parent)
{
    reload();
}

SavedRemoteLocationsStore &SavedRemoteLocationsStore::instance()
{
    static SavedRemoteLocationsStore s_instance;
    return s_instance;
}

bool SavedRemoteLocationsStore::hasLocation(const QString &id) const
{
    for (const auto &loc : m_locations) {
        if (loc.id == id) return true;
    }
    return false;
}

bool SavedRemoteLocationsStore::containsUrl(const QUrl &url) const
{
    const QUrl sanitized = RemoteUrlHelper::sanitizeUrl(url);
    for (const auto &loc : m_locations) {
        if (RemoteUrlHelper::isSameRemoteLocation(loc.url, sanitized)) {
            return true;
        }
    }
    return false;
}

SavedRemoteLocation SavedRemoteLocationsStore::locationById(const QString &id) const
{
    for (const auto &loc : m_locations) {
        if (loc.id == id) return loc;
    }
    return {};
}

SavedRemoteLocation SavedRemoteLocationsStore::locationForUrl(const QUrl &url) const
{
    const QUrl sanitized = RemoteUrlHelper::sanitizeUrl(url);
    for (const auto &loc : m_locations) {
        if (RemoteUrlHelper::isSameRemoteLocation(loc.url, sanitized)) {
            return loc;
        }
    }
    return {};
}

bool SavedRemoteLocationsStore::addLocation(const QString &displayName, const QUrl &url, QString *outId)
{
    const QUrl sanitized = RemoteUrlHelper::sanitizeUrl(url);
    if (!RemoteUrlHelper::isRemoteUrl(sanitized) || !RemoteUrlHelper::isValidRemoteUrl(sanitized)) {
        return false;
    }

    if (containsUrl(sanitized)) {
        return false;
    }

    QString finalName = displayName.trimmed();
    if (finalName.isEmpty()) {
        finalName = defaultDisplayName(sanitized);
    }

    const QString id = generateId(sanitized);

    SavedRemoteLocation entry;
    entry.id = id;
    entry.displayName = finalName;
    entry.url = sanitized;

    m_locations.push_back(entry);

    if (outId) {
        *outId = id;
    }

    QSettings settings;
    save(settings);
    Q_EMIT locationsChanged();
    return true;
}

bool SavedRemoteLocationsStore::renameLocation(const QString &id, const QString &newDisplayName)
{
    const QString finalName = newDisplayName.trimmed();
    if (finalName.isEmpty()) {
        return false;
    }

    for (auto &loc : m_locations) {
        if (loc.id == id) {
            if (loc.displayName == finalName) {
                return true;
            }
            loc.displayName = finalName;
            QSettings settings;
            save(settings);
            Q_EMIT locationsChanged();
            return true;
        }
    }
    return false;
}

bool SavedRemoteLocationsStore::removeLocation(const QString &id)
{
    for (int i = 0; i < m_locations.size(); ++i) {
        if (m_locations.at(i).id == id) {
            m_locations.removeAt(i);
            QSettings settings;
            save(settings);
            Q_EMIT locationsChanged();
            return true;
        }
    }
    return false;
}

void SavedRemoteLocationsStore::clear()
{
    if (m_locations.isEmpty()) return;
    m_locations.clear();
    QSettings settings;
    save(settings);
    Q_EMIT locationsChanged();
}

void SavedRemoteLocationsStore::load(QSettings &settings)
{
    m_locations.clear();
    bool needsRewrite = false;

    const int size = settings.beginReadArray(QStringLiteral("savedRemoteLocations"));
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        QString id = settings.value(QStringLiteral("id")).toString();
        QString name = settings.value(QStringLiteral("name")).toString();
        const QString urlString = settings.value(QStringLiteral("url")).toString();

        const QUrl rawUrl = QUrl::fromUserInput(urlString);
        if (!rawUrl.password().isEmpty()) {
            needsRewrite = true;
        }

        const QUrl sanitized = RemoteUrlHelper::sanitizeUrl(rawUrl);
        if (!RemoteUrlHelper::isRemoteUrl(sanitized) || !RemoteUrlHelper::isValidRemoteUrl(sanitized)) {
            continue;
        }

        if (id.isEmpty()) {
            id = generateId(sanitized);
            needsRewrite = true;
        }
        if (name.trimmed().isEmpty()) {
            name = defaultDisplayName(sanitized);
            needsRewrite = true;
        }

        // Avoid storing in-memory duplicate from corrupted config
        bool duplicate = false;
        for (const auto &existing : m_locations) {
            if (existing.id == id || RemoteUrlHelper::isSameRemoteLocation(existing.url, sanitized)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        SavedRemoteLocation loc;
        loc.id = id;
        loc.displayName = name.trimmed();
        loc.url = sanitized;
        m_locations.push_back(loc);
    }
    settings.endArray();

    if (needsRewrite) {
        save(settings);
    }
}

void SavedRemoteLocationsStore::save(QSettings &settings) const
{
    settings.beginWriteArray(QStringLiteral("savedRemoteLocations"), m_locations.size());
    for (int i = 0; i < m_locations.size(); ++i) {
        settings.setArrayIndex(i);
        const auto &loc = m_locations.at(i);
        settings.setValue(QStringLiteral("id"), loc.id);
        settings.setValue(QStringLiteral("name"), loc.displayName);
        // Strict defensive guarantee: never write password under any circumstances
        settings.setValue(QStringLiteral("url"), RemoteUrlHelper::sanitizeUrl(loc.url).toString());
    }
    settings.endArray();
}

void SavedRemoteLocationsStore::reload()
{
    QSettings settings;
    load(settings);
    Q_EMIT locationsChanged();
}

QString SavedRemoteLocationsStore::defaultDisplayName(const QUrl &url)
{
    const QString path = url.path();
    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    // If it's a single top-level share or directory (e.g. smb://nas/Media -> "Media")
    if (segments.size() == 1) {
        return segments.first();
    }

    // For server roots or deep paths (e.g. sftp://alice@server.example/home/alice -> "server.example")
    if (!url.host().isEmpty()) {
        return url.host();
    }

    return RemoteUrlHelper::rootLabel(url);
}

QString SavedRemoteLocationsStore::generateId(const QUrl &sanitizedUrl)
{
    const QByteArray hash = QCryptographicHash::hash(
        sanitizedUrl.toString().toUtf8(),
        QCryptographicHash::Sha256
    ).toHex().left(16);
    return QStringLiteral("remote_") + QString::fromUtf8(hash);
}
