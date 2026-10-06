/*
 * Dedicated preview and thumbnail coordinator module.
 *
 * Implements Stage 2 of 0.39.0: Memory-bounded LRU caching, KIO::PreviewJob
 * asynchronous scheduling, generation tokens, and stale callback protection.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QCache>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <kfileitem.h>
#include <KIO/PreviewJob>

class PreviewController : public QObject
{
    Q_OBJECT

public:
    using PreviewJobFactory = std::function<KIO::PreviewJob *(
        const KFileItemList &items,
        const QSize &size,
        const QStringList *enabledPlugins)>;

    static constexpr qsizetype kDefaultMaxCacheBytes = 32 * 1024 * 1024; // 32 MB RAM cache
    static constexpr int kDefaultMaxBatchSize = 100; // Bound simultaneous IPC items

    explicit PreviewController(QObject *parent = nullptr);
    ~PreviewController() override;

    // URL and path eligibility policy
    static bool isEligibleUrl(const QUrl &url);
    static QString makeCacheKey(const QUrl &url, const QSize &physicalSize);
    static QSize calculatePhysicalSize(const QSize &logicalSize, qreal dpr);

    // Request preview batch for visible items
    void requestPreviews(const QList<QUrl> &urls, const QSize &logicalSize, qreal dpr = 1.0);

    // Cache operations (memory-bounded LRU)
    bool hasCachedPreview(const QUrl &url, const QSize &logicalSize, qreal dpr = 1.0) const;
    QPixmap cachedPreview(const QUrl &url, const QSize &logicalSize, qreal dpr = 1.0) const;
    void insertCachedPreview(const QUrl &url, const QSize &physicalSize, const QPixmap &pixmap);
    void removeCachedPreview(const QUrl &url);
    void clearCache();

    qsizetype maxCacheBytes() const;
    void setMaxCacheBytes(qsizetype bytes);
    qsizetype currentCacheBytes() const;
    int cachedCount() const;

    int maxBatchSize() const { return m_maxBatchSize; }
    void setMaxBatchSize(int maxBatch) { m_maxBatchSize = qMax(1, maxBatch); }
    int pendingQueueCount() const { return static_cast<int>(m_pendingQueue.size()); }

    quint64 currentGeneration() const { return m_generation; }
    bool isRunning() const { return m_activeJob != nullptr; }

    QStringList enabledPlugins() const { return m_enabledPlugins; }
    void setEnabledPlugins(const QStringList &plugins) { m_enabledPlugins = plugins; }
    static QStringList queryDefaultPlugins();

    // Test hook for hermetic offscreen verification
    void setPreviewJobFactoryForTesting(PreviewJobFactory factory);

public Q_SLOTS:
    void cancel();

Q_SIGNALS:
    void previewReady(const QUrl &url, const QPixmap &pixmap, quint64 generation);
    void previewFailed(const QUrl &url, quint64 generation);
    void jobFinished(quint64 generation);
    void cancelled(quint64 generation);
    // Emitted immediately before a backend PreviewJob is created for a batch.
    void previewBatchRequested(const QList<QUrl> &urls, const QSize &physicalSize, quint64 generation);

private:
    void startNextBatch();
    void setupJobConnections(KIO::PreviewJob *job, quint64 generation, qreal dpr, const QSize &physicalSize);

    quint64 m_generation = 0;
    qsizetype m_maxCacheBytes = kDefaultMaxCacheBytes;
    int m_maxBatchSize = kDefaultMaxBatchSize;
    QSize m_requestedPhysicalSize;
    qreal m_requestedDpr = 1.0;

    QList<QUrl> m_pendingQueue;
    QStringList m_enabledPlugins;
    QPointer<KIO::PreviewJob> m_activeJob;
    PreviewJobFactory m_jobFactory;

    mutable QCache<QString, QPixmap> m_cache;
};
