/*
 * Centralized directory change monitoring controller for ThisPC.
 *
 * Coordinates filesystem events from KDirWatch (inotify / local) and
 * org.kde.KDirNotify (D-Bus / KIO / Dolphin / Plasma Desktop).
 * Manages reference-counted directory subscriptions, per-directory debounce
 * queues (150 ms nominal, 500 ms hard deadline starvation prevention),
 * and dispatches targeted quiet refresh notifications.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <KDirWatch>

#ifdef QT_DBUS_LIB
#include <KDirNotify>
#endif

class DirectoryWatchController : public QObject
{
    Q_OBJECT

public:
    struct DebounceConfig {
        qint64 nominalWindowMs = 150;
        qint64 toleranceMs = 30;
        qint64 maxDeadlineMs = 500;
    };

    explicit DirectoryWatchController(QObject *parent = nullptr);
    ~DirectoryWatchController() override;

    // Consumer subscription management (DR-056)
    void setWatchedUrl(const void *consumer, const QUrl &url);
    void removeConsumer(const void *consumer);
    void updateTabSubscriptions(const QList<QUrl> &tabUrls);
    void clear();

    // Query state
    bool isWatched(const QUrl &url) const;
    int refCount(const QUrl &url) const;
    int watchedDirectoryCount() const;
    QList<QUrl> watchedUrls() const;
    QUrl watchedUrlForConsumer(const void *consumer) const;

    // Debounce configuration
    DebounceConfig debounceConfig() const { return m_debounceConfig; }
    void setDebounceConfig(const DebounceConfig &config) { m_debounceConfig = config; }

    // Direct event dispatch (useful for tests and internal triggers)
    void scheduleRefresh(const QUrl &url);
    void handleFilesAdded(const QString &directory);
    void handleFilesRemoved(const QStringList &fileList);
    void handleFilesChanged(const QStringList &fileList);
    void handleFileMoved(const QString &src, const QString &dst);
    void handleFileRenamed(const QString &src, const QString &dst);

Q_SIGNALS:
    void directoryChanged(const QUrl &url);
    void directoryDeleted(const QUrl &url);
    void directoryRenamed(const QUrl &oldUrl, const QUrl &newUrl);

private Q_SLOTS:
    void onKDirWatchDirty(const QString &path);
    void onKDirWatchCreated(const QString &path);
    void onKDirWatchDeleted(const QString &path);

private:
    struct WatchEntry {
        QUrl url;
        QString localPath;
        int refCount = 0;
        QTimer *debounceTimer = nullptr;
        qint64 firstEventEpochMs = 0;
        qint64 lastEventEpochMs = 0;
    };

    void checkAndScheduleMatching(const QUrl &eventUrl);
    void scheduleDebouncedRefresh(const QString &key);

    DebounceConfig m_debounceConfig;
    KDirWatch m_dirWatch;
#ifdef QT_DBUS_LIB
    org::kde::KDirNotify *m_kdirNotify = nullptr;
#endif

    // Map normalized directory URL string -> WatchEntry
    QMap<QString, WatchEntry> m_watchedEntries;
    // Map consumer pointer -> normalized directory URL
    QMap<const void *, QUrl> m_consumerUrls;

    // Tags used for background tab subscriptions
    static constexpr quintptr kTabTagBase = 0x10000;
};
