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
#include <QPointer>
#include <algorithm>
#include <utility>

namespace {

inline bool isStagingPath(const QString &path)
{
    const QString fileName = QFileInfo(path).fileName();
    return fileName.endsWith(QLatin1String(".thispc-part"))
        || fileName.contains(QLatin1String(".thispc-part."))
        || fileName.endsWith(QLatin1String(".part"));
}

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
        if (++m_nextGeneration == 0) {
            ++m_nextGeneration;
        }
        entry.generation = m_nextGeneration;
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
                it->debounceTimer->disconnect(this);
                it->debounceTimer->deleteLater();
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
            it->debounceTimer->disconnect(this);
            it->debounceTimer->deleteLater();
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
    const QStringList snapshot = fileList;
    const QPointer<DirectoryWatchController> guard(this);
    for (const QString &fileStr : snapshot) {
        if (!guard) {
            return;
        }
        if (!isStagingPath(fileStr)) {
            checkAndScheduleMatching(QUrl::fromUserInput(fileStr));
        }
    }
}

void DirectoryWatchController::handleFilesChanged(const QStringList &fileList)
{
    const QStringList snapshot = fileList;
    const QPointer<DirectoryWatchController> guard(this);
    for (const QString &fileStr : snapshot) {
        if (!guard) {
            return;
        }
        if (!isStagingPath(fileStr)) {
            checkAndScheduleMatching(QUrl::fromUserInput(fileStr));
        }
    }
}

void DirectoryWatchController::handleFileMoved(const QString &src, const QString &dst)
{
    const QUrl srcUrl = QUrl::fromUserInput(src);
    const QUrl dstUrl = QUrl::fromUserInput(dst);

    struct RenameTarget {
        QString key;
        quint64 generation = 0;
    };
    QList<RenameTarget> renameTargets;
    for (auto it = m_watchedEntries.cbegin(); it != m_watchedEntries.cend(); ++it) {
        if (sameLocation(it->url, srcUrl)) {
            renameTargets.append({it.key(), it->generation});
        }
    }

    const QPointer<DirectoryWatchController> guard(this);
    for (const RenameTarget &target : std::as_const(renameTargets)) {
        if (!guard) {
            return;
        }
        const auto currentIt = m_watchedEntries.constFind(target.key);
        if (currentIt == m_watchedEntries.cend()
            || currentIt->generation != target.generation
            || currentIt->refCount <= 0) {
            continue;
        }
        Q_EMIT directoryRenamed(srcUrl, dstUrl);
    }

    if (!guard) {
        return;
    }
    if (!isStagingPath(src)) {
        checkAndScheduleMatching(srcUrl);
    }
    if (!guard) {
        return;
    }
    if (!isStagingPath(dst)) {
        checkAndScheduleMatching(dstUrl);
    }
}

void DirectoryWatchController::handleFileRenamed(const QString &src, const QString &dst)
{
    handleFileMoved(src, dst);
}

void DirectoryWatchController::onKDirWatchDirty(const QString &path)
{
    if (isStagingPath(path)) {
        return;
    }
    scheduleMatchingLocalPath(path);
}

void DirectoryWatchController::onKDirWatchCreated(const QString &path)
{
    if (isStagingPath(path)) {
        return;
    }
    scheduleMatchingLocalPath(path);
}

void DirectoryWatchController::onKDirWatchDeleted(const QString &path)
{
    if (isStagingPath(path)) {
        return;
    }
    const QString cleanPath = QDir::cleanPath(path);
    struct DeleteTarget {
        QString key;
        quint64 generation = 0;
        QUrl url;
        bool isDeletedSelf = false;
    };
    QList<DeleteTarget> targets;
    for (auto it = m_watchedEntries.cbegin(); it != m_watchedEntries.cend(); ++it) {
        if (!it->url.isLocalFile()) {
            continue;
        }
        const QString watchedClean = QDir::cleanPath(it->localPath);
        if (watchedClean == cleanPath) {
            targets.append({it.key(), it->generation, it->url, true});
        } else if (localPathMatchesWatchedDirectory(it->url, path)) {
            targets.append({it.key(), it->generation, it->url, false});
        }
    }

    const QPointer<DirectoryWatchController> guard(this);
    for (const DeleteTarget &target : std::as_const(targets)) {
        if (!guard) {
            return;
        }
        const auto currentIt = m_watchedEntries.constFind(target.key);
        if (currentIt == m_watchedEntries.cend()
            || currentIt->generation != target.generation
            || currentIt->refCount <= 0) {
            continue;
        }
        if (target.isDeletedSelf) {
            Q_EMIT directoryDeleted(target.url);
        } else {
            scheduleDebouncedRefresh(target.key, target.generation);
        }
    }
}

void DirectoryWatchController::scheduleMatchingLocalPath(const QString &path)
{
    struct ScheduleTarget {
        QString key;
        quint64 generation = 0;
    };
    QList<ScheduleTarget> targets;
    for (auto it = m_watchedEntries.cbegin(); it != m_watchedEntries.cend(); ++it) {
        if (localPathMatchesWatchedDirectory(it->url, path)) {
            targets.append({it.key(), it->generation});
        }
    }

    const QPointer<DirectoryWatchController> guard(this);
    for (const ScheduleTarget &target : std::as_const(targets)) {
        if (!guard) {
            return;
        }
        scheduleDebouncedRefresh(target.key, target.generation);
    }
}

void DirectoryWatchController::checkAndScheduleMatching(const QUrl &eventUrl)
{
    struct ScheduleTarget {
        QString key;
        quint64 generation = 0;
    };
    QList<ScheduleTarget> targets;
    for (auto it = m_watchedEntries.cbegin(); it != m_watchedEntries.cend(); ++it) {
        if (urlsMatchWatchedDirectory(it->url, eventUrl)) {
            targets.append({it.key(), it->generation});
        }
    }

    const QPointer<DirectoryWatchController> guard(this);
    for (const ScheduleTarget &target : std::as_const(targets)) {
        if (!guard) {
            return;
        }
        scheduleDebouncedRefresh(target.key, target.generation);
    }
}

void DirectoryWatchController::scheduleDebouncedRefresh(const QString &key, quint64 expectedGeneration)
{
    auto it = m_watchedEntries.find(key);
    if (it == m_watchedEntries.end() || it->refCount <= 0) {
        return;
    }
    if (expectedGeneration != 0 && it->generation != expectedGeneration) {
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
        return;
    }

    const qint64 delay = std::min(m_debounceConfig.nominalWindowMs, remainingUntilDeadline);
    if (!entry.debounceTimer) {
        entry.debounceTimer = new QTimer(this);
        entry.debounceTimer->setSingleShot(true);
        const QString capturedKey = key;
        const quint64 capturedGen = entry.generation;
        connect(entry.debounceTimer, &QTimer::timeout, this, [this, capturedKey, capturedGen]() {
            auto timerIt = m_watchedEntries.find(capturedKey);
            if (timerIt != m_watchedEntries.end()
                && timerIt->generation == capturedGen
                && timerIt->refCount > 0) {
                timerIt->firstEventEpochMs = 0;
                timerIt->lastEventEpochMs = 0;
                const QUrl targetUrl = timerIt->url;
                Q_EMIT directoryChanged(targetUrl);
            }
        });
    }
    entry.debounceTimer->start(static_cast<int>(delay));
}
