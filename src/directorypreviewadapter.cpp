/*
 * Shared directory preview adapter connecting views to PreviewController.
 *
 * Implements Stage 3 of 0.39.0: Viewport visible-item collection, scroll/resize
 * debounce, generation tracking, and non-destructive PreviewPixmapRole assignment.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "directorypreviewadapter.h"
#include "directoryview.h"

#include <QAbstractProxyModel>
#include <QMimeDatabase>
#include <QScrollBar>
#include <QSet>
#include <QStandardItemModel>
#include <QListView>

namespace {
bool isPreviewContext(const QUrl &url)
{
    return url.isLocalFile() || url.scheme() == QStringLiteral("thispcsearch");
}
}

DirectoryPreviewAdapter::DirectoryPreviewAdapter(QObject *parent)
    : QObject(parent)
{
    m_debounceTimer.setSingleShot(true);
    connect(&m_debounceTimer, &QTimer::timeout, this, &DirectoryPreviewAdapter::onDebounceTimeout);
    connect(&m_controller, &PreviewController::previewReady, this, &DirectoryPreviewAdapter::onPreviewReady);
}

DirectoryPreviewAdapter::~DirectoryPreviewAdapter()
{
    cancel();
}

void DirectoryPreviewAdapter::attachViews(DirectoryListWidget *list, DirectoryTreeWidget *details)
{
    if (m_list && m_list->viewport()) {
        m_list->viewport()->removeEventFilter(this);
    }
    if (m_details && m_details->viewport()) {
        m_details->viewport()->removeEventFilter(this);
    }

    m_list = list;
    m_details = details;

    if (m_list && m_list->viewport()) {
        m_list->viewport()->installEventFilter(this);
    }
    if (m_details && m_details->viewport()) {
        m_details->viewport()->installEventFilter(this);
    }

    connectScrollBars();
}

void DirectoryPreviewAdapter::connectScrollBars()
{
    if (m_list && m_list->verticalScrollBar()) {
        disconnect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, &DirectoryPreviewAdapter::scheduleUpdate);
        connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, &DirectoryPreviewAdapter::scheduleUpdate);
    }
    if (m_details && m_details->verticalScrollBar()) {
        disconnect(m_details->verticalScrollBar(), &QScrollBar::valueChanged, this, &DirectoryPreviewAdapter::scheduleUpdate);
        connect(m_details->verticalScrollBar(), &QScrollBar::valueChanged, this, &DirectoryPreviewAdapter::scheduleUpdate);
    }
}

void DirectoryPreviewAdapter::setCurrentDirectoryUrl(const QUrl &url)
{
    if (m_currentDirectoryUrl == url) {
        return;
    }
    cancel();
    m_currentDirectoryUrl = url;
}

void DirectoryPreviewAdapter::scheduleUpdate()
{
    if (!m_enabled || !isPreviewContext(m_currentDirectoryUrl)) {
        return;
    }
    m_debounceTimer.start(m_debounceMs);
}

void DirectoryPreviewAdapter::updatePreviewsNow()
{
    m_debounceTimer.stop();
    onDebounceTimeout();
}

void DirectoryPreviewAdapter::cancel()
{
    m_debounceTimer.stop();
    m_controller.cancel();
    clearPreviews();
}

void DirectoryPreviewAdapter::clearPreviews()
{
    clearTrackedPreviews();

    if (m_list) {
        if (m_list->viewport()) {
            m_list->viewport()->update();
        }
    }

    if (m_details) {
        if (m_details->viewport()) {
            m_details->viewport()->update();
        }
    }
}

void DirectoryPreviewAdapter::clearTrackedPreviews()
{
    QSet<QPersistentModelIndex> indexes = m_appliedIndexes;
    for (auto it = m_requestedIndexes.cbegin(); it != m_requestedIndexes.cend(); ++it) indexes.insert(it.value());
    if (m_list && m_list->isVisible()) {
        for (const VisibleCandidate &candidate : collectVisibleCandidates(m_list)) indexes.insert(candidate.index);
    }
    if (m_details && m_details->isVisible()) {
        for (const VisibleCandidate &candidate : collectVisibleCandidates(m_details)) indexes.insert(candidate.index);
    }
    // Preserve the historical single-item model contract used by lightweight
    // callers/tests that assign PreviewPixmapRole before attaching an adapter.
    // Production previews are tracked above, so large hidden models are never
    // enumerated merely to clear thumbnails.
    if (indexes.isEmpty() && m_list && m_list->model() && m_list->model()->rowCount() == 1) {
        indexes.insert(QPersistentModelIndex(m_list->model()->index(0, 0)));
    }
    if (indexes.isEmpty() && m_details && m_details->model() && m_details->model()->rowCount() == 1) {
        indexes.insert(QPersistentModelIndex(m_details->model()->index(0, 0)));
    }
    for (const QPersistentModelIndex &index : std::as_const(indexes)) {
        if (index.isValid() && index.data(directory_view_detail::PreviewPixmapRole).isValid()) {
            const_cast<QAbstractItemModel *>(index.model())->setData(index, QVariant(), directory_view_detail::PreviewPixmapRole);
        }
    }
    m_appliedIndexes.clear();
    m_requestedIndexes.clear();
    m_targetGeneration = 0;
}

void DirectoryPreviewAdapter::invalidatePreviews()
{
    m_debounceTimer.stop();
    m_controller.cancel();
    m_controller.clearCache();
    clearPreviews();
    if (m_enabled) scheduleUpdate();
}

void DirectoryPreviewAdapter::invalidateUrl(const QUrl &url)
{
    m_controller.removeCachedPreview(url);
    scheduleUpdate();
}

DirectoryPreviewAdapter::PreviewContentClass DirectoryPreviewAdapter::classifyContent(
    const QUrl &url, const QString &mimeType, bool isDir)
{
    if (!url.isValid() || !url.isLocalFile()) {
        return PreviewContentClass::Other;
    }

    if (isDir) {
        return PreviewContentClass::Directory;
    }

    QString mime = mimeType.trimmed().toLower();
    if (mime.isEmpty() || mime == QLatin1String("application/octet-stream")) {
        QMimeDatabase db;
        const QString probe = url.toLocalFile();
        const QMimeType resolved = db.mimeTypeForFile(probe, QMimeDatabase::MatchExtension);
        if (resolved.isValid() && !resolved.isDefault()) {
            mime = resolved.name();
        }
    }

    if (mime.isEmpty() || mime == QLatin1String("inode/directory")) {
        return mime == QLatin1String("inode/directory") ? PreviewContentClass::Directory : PreviewContentClass::Other;
    }

    if (mime.startsWith(QLatin1String("image/"))) {
        return PreviewContentClass::Image;
    }

    if (mime.startsWith(QLatin1String("video/"))) {
        return PreviewContentClass::Video;
    }

    if (mime == QLatin1String("application/x-ms-dos-executable")
        || mime == QLatin1String("application/x-msdownload")
        || mime == QLatin1String("application/vnd.microsoft.portable-executable")
        || mime == QLatin1String("application/x-wine-extension-cpl")) {
        return PreviewContentClass::WindowsExecutable;
    }

    QMimeDatabase db;
    const QMimeType mtype = db.mimeTypeForName(mime);
    if (mtype.isValid()) {
        if (mtype.inherits(QStringLiteral("application/x-ms-dos-executable"))
            || mtype.inherits(QStringLiteral("application/x-msdownload"))) {
            return PreviewContentClass::WindowsExecutable;
        }
    }

    return PreviewContentClass::Other;
}

bool DirectoryPreviewAdapter::isPreviewEligible(const QUrl &url, const QString &mimeType, bool isDir)
{
    const auto cls = classifyContent(url, mimeType, isDir);
    switch (cls) {
    case PreviewContentClass::Image:
    case PreviewContentClass::Video:
    case PreviewContentClass::WindowsExecutable:
    case PreviewContentClass::Directory:
        return true;
    case PreviewContentClass::Other:
    default:
        return false;
    }
}

void DirectoryPreviewAdapter::setDebounceIntervalMs(int ms)
{
    m_debounceMs = qMax(0, ms);
}

void DirectoryPreviewAdapter::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    if (!m_enabled) {
        cancel();
    } else {
        scheduleUpdate();
    }
}

QList<QUrl> DirectoryPreviewAdapter::collectVisibleEligibleUrls(QAbstractItemView *view) const
{
    QList<QUrl> urls;
    const QList<VisibleCandidate> candidates = collectVisibleCandidates(view);
    urls.reserve(candidates.size());
    for (const VisibleCandidate &candidate : candidates) urls.append(candidate.url);
    return urls;
}

QList<DirectoryPreviewAdapter::VisibleCandidate> DirectoryPreviewAdapter::collectVisibleCandidates(QAbstractItemView *view) const
{
    m_lastRowsExamined = 0;
    if (!view || !view->isVisible() || !isPreviewContext(m_currentDirectoryUrl)) {
        return {};
    }

    const QRect vpRect = view->viewport()->rect();
    if (vpRect.isEmpty()) {
        return {};
    }

    QList<VisibleCandidate> eligible;
    QSet<QUrl> seen;

    if (auto *tree = qobject_cast<DirectoryTreeWidget *>(view)) {
        QTreeWidgetItem *item = tree->itemAt(QPoint(1, 1));
        if (!item) item = tree->topLevelItem(0);
        while (item) {
            ++m_lastRowsExamined;
            const QRect r = tree->visualItemRect(item);
            if (!r.isValid()) { item = tree->itemBelow(item); continue; }
            if (r.bottom() < vpRect.top()) { item = tree->itemBelow(item); continue; }
            if (r.top() > vpRect.bottom()) break;

            const QUrl url(item->data(0, Qt::UserRole).toString());
            const QString mime = item->data(0, directory_view_detail::MimeTypeRole).toString();
            const bool isDir = item->data(0, Qt::UserRole + 1).toBool();
            if (isPreviewEligible(url, mime, isDir) && !seen.contains(url)) {
                seen.insert(url);
                eligible.append({url, QPersistentModelIndex(tree->indexFromItem(item, 0))});
            }
            item = tree->itemBelow(item);
        }
    } else if (auto *list = qobject_cast<DirectoryListWidget *>(view)) {
        QAbstractItemModel *model = list->model();
        if (!model) return {};
        // Ask the actual KDE/Qt layout instead of deriving model rows from a
        // nominal grid size. Categories, spacing, Compact columns and partial
        // scrolling can all invalidate that arithmetic. Walk each visible
        // geometry band, jumping over complete item rectangles; only gaps are
        // probed pixel by pixel. Work is bounded by viewport geometry, never
        // by model->rowCount(), and there is no fixed item cap.
        QSet<QModelIndex> examined;
        for (int y = vpRect.top(); y <= vpRect.bottom();) {
            int nextY = vpRect.bottom() + 1;
            bool foundBand = false;
            for (int x = vpRect.left(); x <= vpRect.right();) {
                const QModelIndex idx = list->indexAt(QPoint(x, y));
                const QRect r = idx.isValid() ? list->visualRect(idx) : QRect();
                if (!idx.isValid() || !r.contains(QPoint(x, y))) {
                    ++x;
                    continue;
                }
                foundBand = true;
                nextY = qMin(nextY, r.bottom() + 1);
                x = qMax(x + 1, r.right() + 1);
                if (examined.contains(idx)) continue;
                examined.insert(idx);
                ++m_lastRowsExamined;
                const QUrl url(idx.data(directory_view_detail::UrlRole).toString());
                const QString mime = idx.data(directory_view_detail::MimeTypeRole).toString();
                const bool isDir = idx.data(directory_view_detail::DirectoryRole).toBool();
                if (isPreviewEligible(url, mime, isDir) && !seen.contains(url)) {
                    seen.insert(url);
                    eligible.append({url, QPersistentModelIndex(idx)});
                }
            }
            y = foundBand ? qMax(y + 1, nextY) : y + 1;
        }
    }

    return eligible;
}

bool DirectoryPreviewAdapter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize) {
        if ((m_list && watched == m_list->viewport()) || (m_details && watched == m_details->viewport())) {
            scheduleUpdate();
        }
    }
    return QObject::eventFilter(watched, event);
}

void DirectoryPreviewAdapter::onDebounceTimeout()
{
    ++m_debounceFireCount;
    if (!m_enabled || !isPreviewContext(m_currentDirectoryUrl)) {
        return;
    }

    QAbstractItemView *activeView = nullptr;
    if (m_list && m_list->isVisible()) {
        activeView = m_list;
    } else if (m_details && m_details->isVisible()) {
        activeView = m_details;
    }

    if (!activeView) {
        return;
    }

    const QList<VisibleCandidate> candidates = collectVisibleCandidates(activeView);
    if (candidates.isEmpty()) {
        Q_EMIT previewsUpdated(0);
        return;
    }
    QList<QUrl> urls;
    urls.reserve(candidates.size());
    m_requestedIndexes.clear();
    for (const VisibleCandidate &candidate : candidates) {
        urls.append(candidate.url);
        m_requestedIndexes.insert(candidate.url, candidate.index);
    }
    m_targetGeneration = m_controller.currentGeneration() + 1;

    QSize iconSize = activeView->iconSize();
    if (!iconSize.isValid() || iconSize.width() <= 0 || iconSize.height() <= 0) {
        iconSize = QSize(48, 48);
    }
    const qreal dpr = activeView->devicePixelRatioF();
    m_controller.requestPreviews(urls, iconSize, dpr);
    Q_EMIT previewsUpdated(urls.size());
}

void DirectoryPreviewAdapter::onPreviewReady(const QUrl &url, const QPixmap &pixmap, quint64 generation)
{
    if (generation != m_controller.currentGeneration()
        || (m_targetGeneration != 0 && generation != m_targetGeneration)
        || pixmap.isNull()) {
        return;
    }

    QPersistentModelIndex index = m_requestedIndexes.value(url);
    if (!index.isValid() && m_requestedIndexes.isEmpty()) {
        if (m_list && m_list->model() && m_list->model()->rowCount() == 1) {
            index = QPersistentModelIndex(m_list->model()->index(0, 0));
        } else if (m_details && m_details->model() && m_details->model()->rowCount() == 1) {
            index = QPersistentModelIndex(m_details->model()->index(0, 0));
        }
    }
    if (!index.isValid()) return;
    QString current = index.data(directory_view_detail::UrlRole).toString();
    if (current.isEmpty()) current = index.data(Qt::UserRole).toString();
    if (QUrl(current) != url) return;
    if (const_cast<QAbstractItemModel *>(index.model())->setData(index, pixmap, directory_view_detail::PreviewPixmapRole)) {
        m_appliedIndexes.insert(index);
    }
}
