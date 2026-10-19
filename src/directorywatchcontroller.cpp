/*
 * Centralized directory change monitoring controller for ThisPC.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "directorywatchcontroller.h"
#include "browsercommon.h"

#include <QDBusConnection>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace {

inline bool urlsMatchWatchedDirectory(const QUrl &watchedDirUrl, const QUrl &eventUrl)
{
    if (sameLocation(watchedDirUrl, eventUrl)) {
        return true;
    }
    const QUrl parentUrl = parentDirectoryUrl(eventUrl);
    if (sameLocation(watchedDirUrl, parentUrl)) {
        return true;
    }
    if (watchedDirUrl.isLocalFile() && eventUrl.isLocalFile()) {
        const QString watchedClean = QFileInfo(watchedDirUrl.toLocalFile()).canonicalFilePath();
        const QString eventClean = QFileInfo(eventUrl.toLocalFile()).canonicalFilePath();
        if (!watchedClean.isEmpty() && !eventClean.isEmpty() && watchedClean == eventClean) {
            return true;
        }
        const QString eventParentClean = QFileInfo(eventUrl.toLocalFile()).dir().canonicalPath();
        if (!watchedClean.isEmpty() && !eventParentClean.isEmpty() && watchedClean == eventParentClean) {
            return true;
        }
    }
    return false;
}

inline bool localPathMatchesWatchedDirectory(const QUrl &watchedDirUrl, const QString &dirtyPath)
{
    if (!watchedDirUrl.isLocalFile()) {
        return false;
    }
    const QString watchedLocal = QDir::cleanPath(watchedDirUrl.toLocalFile());
    const QString dirtyClean = QDir::cleanPath(dirtyPath);
    if (watchedLocal == dirtyClean) {
        return true;
    }
    const QString dirtyParent = QFileInfo(dirtyClean).path();
    if (watchedLocal == dirtyParent) {
        return true;
    }
    const QString watchedCanonical = QFileInfo(watchedLocal).canonicalFilePath();
    const QString dirtyCanonical = QFileInfo(dirtyClean).canonicalFilePath();
    if (!watchedCanonical.isEmpty() && !dirtyCanonical.isEmpty() && watchedCanonical == dirtyCanonical) {
        return true;
    }
    const QString dirtyParentCanonical = QFileInfo(dirtyClean).dir().canonicalPath();
    if (!watchedCanonical.isEmpty() && !dirtyParentCanonical.isEmpty() && watchedCanonical == dirtyParentCanonical) {
        return true;
    }
    return false;
}

} // namespace

DirectoryWatchController::DirectoryWatchController(QObject *parent)
    : QObject(parent)
{
    connect(&m_dirWatch, &KDirWatch::dirty, this, &DirectoryWatchController::onKDirWatchDirty);
    connect(&m_dirWatch, &KDirWatch::created, this, &DirectoryWatchController::onKDirWatchCreated);
    connect(&m_dirWatch, &KDirWatch::deleted, this, &DirectoryWatchController::onKDirWatchDeleted);

#ifdef QT_DBUS_LIB
    m_kdirNotify = new org::kde::KDirNotify(QString(), QString(), QDBusConnection::sessionBus(), this);
    connect(m_kdirNotify, &org::kde::KDirNotify::FilesAdded, this, &DirectoryWatchController::handleFilesAdded);
    connect(m_kdirNotify, &org::kde::KDirNotify::FilesRemoved, this, &DirectoryWatchController::handleFilesRemoved);
    connect(m_kdirNotify, &org::kde::KDirNotify::FilesChanged, this, &DirectoryWatchController::handleFilesChanged);
    connect(m_kdirNotify, &org::kde::KDirNotify::FileMoved, this, &DirectoryWatchController::handleFileMoved);
    connect(m_kdirNotify, &org::kde::KDirNotify::FileRenamed, this, &DirectoryWatchController::handleFileRenamed);
    connect(m_kdirNotify, &org::kde::KDirNotify::FileRenamedWithLocalPath, this,
            [this](const QString &src, const QString &dst, const QString &/*dstPath*/) {
                handleFileRenamed(src, dst);
            });
#endif
}

DirectoryWatchController::~DirectoryWatchController()
{
    clear();
}

void DirectoryWatchController::setWatchedUrl(const void *consumer, const QUrl &url)
{
    if (!consumer) {
        return;
    }

    const QUrl oldUrl = m_consumerUrls.value(consumer);
    const QUrl newNorm = normalizedUrl(url);

    if (oldUrl.isValid() && sameLocation(oldUrl, newNorm)) {
        return;
    }

    if (oldUrl.isValid()) {
        removeConsumer(consumer);
    }

    if (!newNorm.isValid() || newNorm.isEmpty()
        || sameLocation(newNorm, kThisPcUrl)
        || newNorm.scheme().startsWith(QStringLiteral("thispc-search"))) {
        return;
    }

    m_consumerUrls.insert(consumer, newNorm);
    const QString key = newNorm.toString();

    auto it = m_watchedEntries.find(key);
    if (it == m_watchedEntries.end()) {
        WatchEntry entry;
        entry.url = newNorm;
        entry.refCount = 1;
        if (newNorm.isLocalFile()) {
            entry.localPath = newNorm.toLocalFile();
            m_dirWatch.addDir(entry.localPath, KDirWatch::WatchFiles);
        }
        m_watchedEntries.insert(key, entry);
    } else {
        it->refCount++;
    }
}

void DirectoryWatchController::removeConsumer(const void *consumer)
{
    if (!consumer || !m_consumerUrls.contains(consumer)) {
        return;
    }

    const QUrl oldUrl = m_consumerUrls.take(consumer);
    const QString key = oldUrl.toString();

    auto it = m_watchedEntries.find(key);
    if (it != m_watchedEntries.end()) {
        it->refCount--;
        if (it->refCount <= 0) {
            if (it->debounceTimer) {
                it->debounceTimer->stop();
                delete it->debounceTimer;
                it->debounceTimer = nullptr;
            }
            if (!it->localPath.isEmpty() && m_dirWatch.contains(it->localPath)) {
                m_dirWatch.removeDir(it->localPath);
            }
            m_watchedEntries.erase(it);
        }
    }
}

void DirectoryWatchController::updateTabSubscriptions(const QList<QUrl> &tabUrls)
{
    for (int i = 0; i < tabUrls.size(); ++i) {
        const void *tag = reinterpret_cast<const void *>(kTabTagBase + static_cast<quintptr>(i));
        setWatchedUrl(tag, tabUrls.at(i));
    }

    // Clean up any stale tab tags that exceeded current size
    for (int j = tabUrls.size(); j < tabUrls.size() + 50; ++j) {
        const void *staleTag = reinterpret_cast<const void *>(kTabTagBase + static_cast<quintptr>(j));
        if (m_consumerUrls.contains(staleTag)) {
            removeConsumer(staleTag);
        } else {
            break;
        }
    }
}

void DirectoryWatchController::clear()
{
    for (auto it = m_watchedEntries.begin(); it != m_watchedEntries.end(); ++it) {
        if (it->debounceTimer) {
            it->debounceTimer->stop();
            delete it->debounceTimer;
            it->debounceTimer = nullptr;
        }
        if (!it->localPath.isEmpty() && m_dirWatch.contains(it->localPath)) {
            m_dirWatch.removeDir(it->localPath);
        }
    }
    m_watchedEntries.clear();
    m_consumerUrls.clear();
}

bool DirectoryWatchController::isWatched(const QUrl &url) const
{
    return m_watchedEntries.contains(normalizedUrl(url).toString());
}

int DirectoryWatchController::refCount(const QUrl &url) const
{
    return m_watchedEntries.value(normalizedUrl(url).toString()).refCount;
}

int DirectoryWatchController::watchedDirectoryCount() const
{
    return m_watchedEntries.size();
}

QList<QUrl> DirectoryWatchController::watchedUrls() const
{
    QList<QUrl> urls;
    urls.reserve(m_watchedEntries.size());
    for (const auto &entry : m_watchedEntries) {
        urls.append(entry.url);
    }
    return urls;
}

QUrl DirectoryWatchController::watchedUrlForConsumer(const void *consumer) const
{
    return m_consumerUrls.value(consumer);
}

void DirectoryWatchController::scheduleRefresh(const QUrl &url)
{
    checkAndScheduleMatching(url);
}

void DirectoryWatchController::handleFilesAdded(const QString &directory)
{
    const QUrl dirUrl = QUrl::fromUserInput(directory);
    checkAndScheduleMatching(dirUrl);
}

void DirectoryWatchController::handleFilesRemoved(const QStringList &fileList)
{
    for (const QString &fileStr : fileList) {
        checkAndScheduleMatching(QUrl::fromUserInput(fileStr));
    }
}

void DirectoryWatchController::handleFilesChanged(const QStringList &fileList)
{
    for (const QString &fileStr : fileList) {
        checkAndScheduleMatching(QUrl::fromUserInput(fileStr));
    }
}

void DirectoryWatchController::handleFileMoved(const QString &src, const QString &dst)
{
    const QUrl srcUrl = QUrl::fromUserInput(src);
    const QUrl dstUrl = QUrl::fromUserInput(dst);

    for (auto it = m_watchedEntries.begin(); it != m_watchedEntries.end(); ++it) {
        if (sameLocation(it->url, srcUrl)) {
            Q_EMIT directoryRenamed(srcUrl, dstUrl);
        }
    }

    checkAndScheduleMatching(srcUrl);
    checkAndScheduleMatching(dstUrl);
}

void DirectoryWatchController::handleFileRenamed(const QString &src, const QString &dst)
{
    handleFileMoved(src, dst);
}

void DirectoryWatchController::onKDirWatchDirty(const QString &path)
{
    for (auto it = m_watchedEntries.begin(); it != m_watchedEntries.end(); ++it) {
        if (localPathMatchesWatchedDirectory(it->url, path)) {
            scheduleDebouncedRefresh(it.key());
        }
    }
}

void DirectoryWatchController::onKDirWatchCreated(const QString &path)
{
    for (auto it = m_watchedEntries.begin(); it != m_watchedEntries.end(); ++it) {
        if (localPathMatchesWatchedDirectory(it->url, path)) {
            scheduleDebouncedRefresh(it.key());
        }
    }
}

void DirectoryWatchController::onKDirWatchDeleted(const QString &path)
{
    const QString cleanPath = QDir::cleanPath(path);
    for (auto it = m_watchedEntries.begin(); it != m_watchedEntries.end(); ++it) {
        if (!it->url.isLocalFile()) {
            continue;
        }
        const QString watchedClean = QDir::cleanPath(it->localPath);
        if (watchedClean == cleanPath) {
            // The watched directory itself was deleted from disk
            const QUrl deletedUrl = it->url;
            Q_EMIT directoryDeleted(deletedUrl);
        } else if (localPathMatchesWatchedDirectory(it->url, path)) {
            // A file or directory inside was deleted
            scheduleDebouncedRefresh(it.key());
        }
    }
}

void DirectoryWatchController::checkAndScheduleMatching(const QUrl &eventUrl)
{
    for (auto it = m_watchedEntries.begin(); it != m_watchedEntries.end(); ++it) {
        if (urlsMatchWatchedDirectory(it->url, eventUrl)) {
            scheduleDebouncedRefresh(it.key());
        }
    }
}

void DirectoryWatchController::scheduleDebouncedRefresh(const QString &key)
{
    auto it = m_watchedEntries.find(key);
    if (it == m_watchedEntries.end() || it->refCount <= 0) {
        return;
    }
    WatchEntry &entry = it.value();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (entry.firstEventEpochMs == 0) {
        entry.firstEventEpochMs = now;
    }
    entry.lastEventEpochMs = now;

    const qint64 elapsed = now - entry.firstEventEpochMs;
    const qint64 remainingUntilDeadline = m_debounceConfig.maxDeadlineMs - elapsed;

    if (remainingUntilDeadline <= 0) {
        // Starvation deadline expired! Fire immediately (DR-033)
        if (entry.debounceTimer) {
            entry.debounceTimer->stop();
        }
        entry.firstEventEpochMs = 0;
        entry.lastEventEpochMs = 0;
        const QUrl targetUrl = entry.url;
        Q_EMIT directoryChanged(targetUrl);
    } else {
        const qint64 delay = std::min(m_debounceConfig.nominalWindowMs, remainingUntilDeadline);
        if (!entry.debounceTimer) {
            entry.debounceTimer = new QTimer(this);
            entry.debounceTimer->setSingleShot(true);
            const QString capturedKey = key;
            connect(entry.debounceTimer, &QTimer::timeout, this, [this, capturedKey]() {
                auto timerIt = m_watchedEntries.find(capturedKey);
                if (timerIt != m_watchedEntries.end() && timerIt->refCount > 0) {
                    timerIt->firstEventEpochMs = 0;
                    timerIt->lastEventEpochMs = 0;
                    const QUrl targetUrl = timerIt->url;
                    Q_EMIT directoryChanged(targetUrl);
                }
            });
        }
        entry.debounceTimer->start(static_cast<int>(delay));
    }
}
