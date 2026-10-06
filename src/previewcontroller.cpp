/*
 * Dedicated preview and thumbnail coordinator module.
 *
 * Implements Stage 2 of 0.39.0: Memory-bounded LRU caching, KIO::PreviewJob
 * asynchronous scheduling, generation tokens, and stale callback protection.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "previewcontroller.h"

// Plugin policy: KIO's neutral default set (plugins whose metadata marks them
// enabled by default). No application-specific config (e.g. another file
// manager's rc file) is consulted at runtime. setEnabledPlugins() remains the
// explicit override for tests and future settings UI.
QStringList PreviewController::queryDefaultPlugins()
{
    return KIO::PreviewJob::defaultPlugins();
}

PreviewController::PreviewController(QObject *parent)
    : QObject(parent)
{
    m_cache.setMaxCost(m_maxCacheBytes);
    m_enabledPlugins = queryDefaultPlugins();
}

PreviewController::~PreviewController()
{
    cancel();
}

bool PreviewController::isEligibleUrl(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty()) {
        return false;
    }
    // Only local file:// URLs are eligible for previews in 0.39
    if (!url.isLocalFile()) {
        return false;
    }
    const QString localPath = url.toLocalFile();
    if (localPath.isEmpty()) {
        return false;
    }
    return true;
}

QString PreviewController::makeCacheKey(const QUrl &url, const QSize &physicalSize)
{
    return url.toString() + QLatin1Char('@')
        + QString::number(physicalSize.width())
        + QLatin1Char('x')
        + QString::number(physicalSize.height());
}

QSize PreviewController::calculatePhysicalSize(const QSize &logicalSize, qreal dpr)
{
    if (logicalSize.isEmpty() || logicalSize.width() <= 0 || logicalSize.height() <= 0) {
        return QSize();
    }
    const qreal effectiveDpr = qMax(qreal(0.1), dpr);
    return QSize(
        qMax(1, qRound(logicalSize.width() * effectiveDpr)),
        qMax(1, qRound(logicalSize.height() * effectiveDpr)));
}

void PreviewController::cancel()
{
    const quint64 oldGen = m_generation++;
    m_pendingQueue.clear();
    if (m_activeJob) {
        disconnect(m_activeJob, nullptr, this, nullptr);
        m_activeJob->kill(KJob::Quietly);
        m_activeJob = nullptr;
    }
    Q_EMIT cancelled(oldGen);
}

void PreviewController::requestPreviews(const QList<QUrl> &urls, const QSize &logicalSize, qreal dpr)
{
    cancel();
    const quint64 gen = m_generation;

    if (urls.isEmpty() || logicalSize.isEmpty() || logicalSize.width() <= 0 || logicalSize.height() <= 0 || dpr <= 0.0) {
        Q_EMIT jobFinished(gen);
        return;
    }

    const QSize physicalSize = calculatePhysicalSize(logicalSize, dpr);
    if (!physicalSize.isValid()) {
        Q_EMIT jobFinished(gen);
        return;
    }

    m_requestedPhysicalSize = physicalSize;
    m_requestedDpr = dpr;

    QSet<QUrl> seen;
    for (const QUrl &url : urls) {
        if (!isEligibleUrl(url) || seen.contains(url)) {
            continue;
        }
        seen.insert(url);

        const QString key = makeCacheKey(url, physicalSize);
        if (QPixmap *cached = m_cache.object(key)) {
            Q_EMIT previewReady(url, *cached, gen);
            continue;
        }

        m_pendingQueue.append(url);
    }

    if (m_pendingQueue.isEmpty()) {
        Q_EMIT jobFinished(gen);
        return;
    }

    startNextBatch();
}

void PreviewController::startNextBatch()
{
    if (m_activeJob != nullptr) {
        return; // Only 1 active PreviewJob per controller
    }

    while (!m_pendingQueue.isEmpty()) {
        const int batchCount = qMin(m_maxBatchSize, static_cast<int>(m_pendingQueue.size()));
        KFileItemList batchItems;
        batchItems.reserve(batchCount);
        for (int i = 0; i < batchCount; ++i) {
            batchItems.append(KFileItem(m_pendingQueue.at(i)));
        }
        m_pendingQueue.erase(m_pendingQueue.begin(), m_pendingQueue.begin() + batchCount);

        const quint64 gen = m_generation;
        const qreal dpr = m_requestedDpr;
        const QSize physicalSize = m_requestedPhysicalSize;
        Q_EMIT previewBatchRequested(batchItems.urlList(), physicalSize, gen);

        KIO::PreviewJob *job = nullptr;
        if (m_jobFactory) {
            job = m_jobFactory(batchItems, physicalSize, &m_enabledPlugins);
        } else {
            job = KIO::filePreview(batchItems, physicalSize, &m_enabledPlugins);
        }

        if (job) {
            m_activeJob = job;
            setupJobConnections(job, gen, dpr, physicalSize);
            return;
        }
    }

    // Queue exhausted and no active job
    Q_EMIT jobFinished(m_generation);
}

void PreviewController::setupJobConnections(
    KIO::PreviewJob *job,
    quint64 generation,
    qreal dpr,
    const QSize &physicalSize)
{
    job->setScaleType(KIO::PreviewJob::ScaledAndCached);
    job->setDevicePixelRatio(dpr);

    connect(job, &KIO::PreviewJob::gotPreview, this,
        [this, generation, dpr, physicalSize](const KFileItem &item, const QPixmap &pixmap) {
            if (generation != m_generation) {
                return; // Stale callback dropped
            }
            QPixmap scaled = pixmap;
            scaled.setDevicePixelRatio(dpr);
            insertCachedPreview(item.url(), physicalSize, scaled);
            Q_EMIT previewReady(item.url(), scaled, generation);
        });

    connect(job, &KIO::PreviewJob::failed, this,
        [this, generation](const KFileItem &item) {
            if (generation != m_generation) {
                return; // Stale callback dropped
            }
            Q_EMIT previewFailed(item.url(), generation);
        });

    connect(job, &KJob::result, this,
        [this, generation, job](KJob *) {
            if (m_activeJob == job) {
                m_activeJob = nullptr;
            }
            if (generation != m_generation) {
                return; // Stale callback dropped
            }
            startNextBatch();
        });
}

bool PreviewController::hasCachedPreview(const QUrl &url, const QSize &logicalSize, qreal dpr) const
{
    const QSize physicalSize = calculatePhysicalSize(logicalSize, dpr);
    if (!physicalSize.isValid()) {
        return false;
    }
    return m_cache.contains(makeCacheKey(url, physicalSize));
}

QPixmap PreviewController::cachedPreview(const QUrl &url, const QSize &logicalSize, qreal dpr) const
{
    const QSize physicalSize = calculatePhysicalSize(logicalSize, dpr);
    if (!physicalSize.isValid()) {
        return QPixmap();
    }
    QPixmap *p = m_cache.object(makeCacheKey(url, physicalSize));
    return p ? *p : QPixmap();
}

void PreviewController::insertCachedPreview(
    const QUrl &url,
    const QSize &physicalSize,
    const QPixmap &pixmap)
{
    if (pixmap.isNull()) {
        return;
    }
    const QString key = makeCacheKey(url, physicalSize);
    qsizetype cost = static_cast<qsizetype>(pixmap.width()) * pixmap.height() * 4;
    if (cost <= 0) {
        cost = 1;
    }
    m_cache.insert(key, new QPixmap(pixmap), cost);
}

void PreviewController::removeCachedPreview(const QUrl &url)
{
    const QString prefix = url.toString() + QLatin1Char('@');
    const auto allKeys = m_cache.keys();
    for (const QString &k : allKeys) {
        if (k.startsWith(prefix) || k == url.toString()) {
            m_cache.remove(k);
        }
    }
}

void PreviewController::clearCache()
{
    m_cache.clear();
}

qsizetype PreviewController::maxCacheBytes() const
{
    return m_maxCacheBytes;
}

void PreviewController::setMaxCacheBytes(qsizetype bytes)
{
    m_maxCacheBytes = qMax(qsizetype(1024), bytes);
    m_cache.setMaxCost(m_maxCacheBytes);
}

qsizetype PreviewController::currentCacheBytes() const
{
    return m_cache.totalCost();
}

int PreviewController::cachedCount() const
{
    return m_cache.size();
}

void PreviewController::setPreviewJobFactoryForTesting(PreviewJobFactory factory)
{
    m_jobFactory = std::move(factory);
}
