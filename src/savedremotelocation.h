/*
 * Data model and persistent store for Saved Remote Locations.
 * Free of GUI / widget dependencies.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <QList>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QUrl>

struct SavedRemoteLocation {
    QString id;
    QString displayName;
    QUrl url; // Guaranteed to be sanitized (no password)

    bool isValid() const;
};

class SavedRemoteLocationsStore final : public QObject
{
    Q_OBJECT

public:
    explicit SavedRemoteLocationsStore(QObject *parent = nullptr);

    static SavedRemoteLocationsStore &instance();

    const QList<SavedRemoteLocation> &locations() const { return m_locations; }
    int count() const { return m_locations.size(); }

    bool hasLocation(const QString &id) const;
    bool containsUrl(const QUrl &url) const;
    SavedRemoteLocation locationById(const QString &id) const;
    SavedRemoteLocation locationForUrl(const QUrl &url) const;

    bool addLocation(const QString &displayName, const QUrl &url, QString *outId = nullptr);
    bool renameLocation(const QString &id, const QString &newDisplayName);
    bool removeLocation(const QString &id);
    void clear();

    void load(QSettings &settings);
    void save(QSettings &settings) const;
    void reload();

    static QString defaultDisplayName(const QUrl &url);
    static QString generateId(const QUrl &sanitizedUrl);

Q_SIGNALS:
    void locationsChanged();

private:
    QList<SavedRemoteLocation> m_locations;
};
