/*
 * Shared directory preview adapter connecting views to PreviewController.
 *
 * Implements Stage 3 of 0.39.0: Viewport visible-item collection, scroll/resize
 * debounce, generation tracking, and non-destructive PreviewPixmapRole assignment.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QEvent>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QHash>
#include <QSize>
#include <QTimer>
#include <QUrl>

#include "previewcontroller.h"

class DirectoryListWidget;
class DirectoryTreeWidget;
class QAbstractItemView;

class DirectoryPreviewAdapter : public QObject
{
    Q_OBJECT

public:
    explicit DirectoryPreviewAdapter(QObject *parent = nullptr);
    ~DirectoryPreviewAdapter() override;

    void attachViews(DirectoryListWidget *list, DirectoryTreeWidget *details);

    void setCurrentDirectoryUrl(const QUrl &url);
    QUrl currentDirectoryUrl() const { return m_currentDirectoryUrl; }

    void scheduleUpdate();
    void updatePreviewsNow();
    void cancel();
    void clearPreviews();
    void invalidatePreviews();
    void invalidateUrl(const QUrl &url);

    PreviewController &controller() { return m_controller; }
    const PreviewController &controller() const { return m_controller; }

    enum class PreviewContentClass {
        Other,
        Image,
        Video,
        WindowsExecutable,
        Directory
    };

    static PreviewContentClass classifyContent(const QUrl &url, const QString &mimeType, bool isDir);
    static bool isPreviewEligible(const QUrl &url, const QString &mimeType, bool isDir);

    // Historical Stage 3 compatibility alias (Image and Video only)
    static bool isStage3Eligible(const QUrl &url, const QString &mimeType, bool isDir)
    {
        const auto cls = classifyContent(url, mimeType, isDir);
        return cls == PreviewContentClass::Image || cls == PreviewContentClass::Video;
    }

    void setDebounceIntervalMs(int ms);
    int debounceIntervalMs() const { return m_debounceMs; }

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    QList<QUrl> collectVisibleEligibleUrls(QAbstractItemView *view) const;
    qsizetype lastRowsExamined() const { return m_lastRowsExamined; }
    quint64 debounceFireCount() const { return m_debounceFireCount; }

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void previewsUpdated(int count);

private Q_SLOTS:
    void onDebounceTimeout();
    void onPreviewReady(const QUrl &url, const QPixmap &pixmap, quint64 generation);

private:
    struct VisibleCandidate {
        QUrl url;
        QPersistentModelIndex index;
    };

    void connectScrollBars();
    QList<VisibleCandidate> collectVisibleCandidates(QAbstractItemView *view) const;
    void clearTrackedPreviews();

    QPointer<DirectoryListWidget> m_list;
    QPointer<DirectoryTreeWidget> m_details;
    PreviewController m_controller;
    QTimer m_debounceTimer;
    QUrl m_currentDirectoryUrl;
    int m_debounceMs = 60;
    bool m_enabled = true;
    mutable qsizetype m_lastRowsExamined = 0;
    quint64 m_debounceFireCount = 0;
    quint64 m_targetGeneration = 0;
    QHash<QUrl, QPersistentModelIndex> m_requestedIndexes;
    QSet<QPersistentModelIndex> m_appliedIndexes;
};
