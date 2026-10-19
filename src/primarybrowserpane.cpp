/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "primarybrowserpane.h"
#include "appwidgets.h"
#include "directoryview.h"
#include "directoryviewsettings.h"
#include "locationpresentation.h"
#include "searchcontroller.h"
#include <KIO/ListJob>
#include <QLabel>
#include <QMimeDatabase>
#include "directorypreviewadapter.h"
#include <QStackedWidget>
#include <QVBoxLayout>

PrimaryBrowserPane::PrimaryBrowserPane(QWidget *parent)
    : QFrame(parent), m_listingCore(this)
{
    setObjectName(QStringLiteral("primaryBrowserPane"));
    setProperty("active", true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_contentStack = new QStackedWidget(this);
    m_previewAdapter = new DirectoryPreviewAdapter(this);
    connect(&m_listingCore, &DirectoryListingCore::listingCanceled,
            this, &PrimaryBrowserPane::listingCanceled);
}

void PrimaryBrowserPane::setCurrentUrl(const QUrl &url)
{
    m_currentUrl = normalizedUrl(url);
    if (m_previewAdapter) {
        m_previewAdapter->setCurrentDirectoryUrl(m_currentUrl);
    }
}

void PrimaryBrowserPane::bindDirectoryViews(DirectoryListWidget *list,
    DirectoryTreeWidget *details, QLabel *title, QLabel *status)
{
    m_directoryList = list; m_directoryDetails = details;
    m_directoryTitle = title; m_directoryStatus = status;
    if (m_previewAdapter) {
        m_previewAdapter->attachViews(list, details);
    }
}

void PrimaryBrowserPane::cancelListing()
{
    m_listingCore.cancelListing();
    if (m_previewAdapter) {
        m_previewAdapter->cancel();
    }
}

void PrimaryBrowserPane::setStatusText(const QString &text)
{
    if (m_directoryStatus) {
        m_directoryStatus->setText(text);
    }
}

KIO::ListJob *PrimaryBrowserPane::listingJob() const
{
    return m_listingCore.listingJob();
}

void PrimaryBrowserPane::loadDirectory(const QUrl &url, bool preserveStatusMessage,
    bool showHiddenFiles, bool detailsActive, const std::function<void(bool)> &render,
    const std::function<void(const QString &)> &reportError,
    DirectoryReloadMode reloadMode)
{
    cancelListing();
    const bool isQuietRefresh = (reloadMode == DirectoryReloadMode::QuietRefresh);

    if (isQuietRefresh) {
        m_pendingViewState = captureDirectoryViewState(m_directoryList, m_directoryDetails, detailsActive);
    } else {
        m_pendingViewState.reset();
        m_pendingSelection = {};
        m_currentUrl = url;
        m_directoryList->clear(); m_directoryDetails->clear();
        m_directoryList->setDropDirectory(url); m_directoryDetails->setDropDirectory(url);
        m_directoryTitle->setText(LocationPresentation::contentHeaderText(url));
        m_directoryStatus->setText(trLocal("Wczytywanie…", "Loading…"));
    }
    Q_EMIT listingStarted(url);
    DirectoryListingCore::ListingOptions options;
    options.showHiddenFiles = showHiddenFiles;
    options.emptyNamePolicy = DirectoryListingCore::EmptyNamePolicy::DisplayName;
    m_listingCore.startListing(url, options, {
        [this, url, render](KIO::ListJob *job) {
            const QUrl effectiveUrl = normalizedUrl(m_listingCore.currentUrl().isValid()
                ? m_listingCore.currentUrl()
                : url);
            if (!sameLocation(effectiveUrl, m_currentUrl)) {
                m_currentUrl = effectiveUrl;
                m_directoryList->setDropDirectory(effectiveUrl);
                m_directoryDetails->setDropDirectory(effectiveUrl);
                m_directoryTitle->setText(LocationPresentation::contentHeaderText(effectiveUrl));
                if (m_previewAdapter) {
                    m_previewAdapter->setCurrentDirectoryUrl(effectiveUrl);
                }
            }
            render(job ? job->property("thispcPreserveStatusMessage").toBool() : false);
            Q_EMIT listingFinished(effectiveUrl, true);
        },
        [this, url, reportError, isQuietRefresh](const QString &error) {
            if (isQuietRefresh) {
                m_directoryList->clear();
                m_directoryDetails->clear();
            }
            m_directoryStatus->setText(trLocal("Nie udało się otworzyć tej lokalizacji.",
                                                "Could not open this location."));
            reportError(error);
            Q_EMIT listingFinished(url, false);
        }
    });
    if (KIO::ListJob *job = m_listingCore.listingJob())
        job->setProperty("thispcPreserveStatusMessage", preserveStatusMessage);
}

void PrimaryBrowserPane::loadRemoteDirectory(const QUrl &url,
    bool showHiddenFiles, bool detailsActive, const std::function<void(bool)> &render,
    const std::function<void(const QString &)> &reportError,
    DirectoryReloadMode reloadMode)
{
    cancelListing();
    const bool isQuietRefresh = (reloadMode == DirectoryReloadMode::QuietRefresh);

    if (isQuietRefresh) {
        m_pendingViewState = captureDirectoryViewState(m_directoryList, m_directoryDetails, detailsActive);
    } else {
        m_pendingViewState.reset();
        m_pendingSelection = {};
        m_directoryStatus->setText(trLocal("Wczytywanie…", "Loading…"));
    }
    Q_EMIT listingStarted(url);
    DirectoryListingCore::ListingOptions options;
    options.showHiddenFiles = showHiddenFiles;
    options.emptyNamePolicy = DirectoryListingCore::EmptyNamePolicy::DisplayName;
    m_listingCore.startListing(url, options, {
        [this, url, render, detailsActive, isQuietRefresh](KIO::ListJob *job) {
            if (!isQuietRefresh) {
                m_pendingSelection = sameLocation(m_currentUrl, url)
                    ? captureDirectorySelection(m_directoryList, m_directoryDetails, detailsActive)
                    : DirectorySelectionSnapshot{};
                m_currentUrl = url;
                m_directoryList->clear(); m_directoryDetails->clear();
                m_directoryList->setDropDirectory(url); m_directoryDetails->setDropDirectory(url);
                m_directoryTitle->setText(LocationPresentation::contentHeaderText(url));
            } else {
                m_currentUrl = url;
            }
            render(job ? job->property("thispcPreserveStatusMessage").toBool() : false);
            Q_EMIT listingFinished(url, true);
        },
        [this, url, reportError, isQuietRefresh](const QString &error) {
            if (isQuietRefresh) {
                m_directoryList->clear();
                m_directoryDetails->clear();
            }
            const QString sanitized = RemoteUrlHelper::sanitizeErrorMessage(error, url);
            if (m_directoryStatus) {
                m_directoryStatus->setText(sanitized);
            }
            if (reportError) reportError(sanitized);
            Q_EMIT listingFinished(url, false);
        }
    });
}

QIcon PrimaryBrowserPane::iconForFile(const FileInfo &file,
    QMimeDatabase &mimeDatabase, bool thumbnailsEnabled)
{
    return m_listingCore.iconForFile(file, mimeDatabase, thumbnailsEnabled, true);
}

void PrimaryBrowserPane::renderDirectoryItems(const RenderOptions &options)
{
    DirectoryViewStateSnapshot viewState;
    if (m_pendingViewState.has_value()) {
        viewState = m_pendingViewState.value();
        m_pendingViewState.reset();
        const DirectoryViewStateSnapshot liveState =
            captureDirectoryViewState(m_directoryList, m_directoryDetails, options.detailsActive);
        viewState.selection.urls.unite(liveState.selection.urls);
        if (liveState.listVScroll > 0) viewState.listVScroll = liveState.listVScroll;
        if (liveState.listHScroll > 0) viewState.listHScroll = liveState.listHScroll;
        if (liveState.detailsVScroll > 0) viewState.detailsVScroll = liveState.detailsVScroll;
        if (liveState.detailsHScroll > 0) viewState.detailsHScroll = liveState.detailsHScroll;
        if (!liveState.currentUrl.isEmpty()) viewState.currentUrl = liveState.currentUrl;
    } else {
        viewState = captureDirectoryViewState(m_directoryList, m_directoryDetails, options.detailsActive);
        viewState.selection.urls.unite(m_pendingSelection.urls);
        if (viewState.currentUrl.isEmpty())
            viewState.currentUrl = m_pendingSelection.currentUrl;
        m_pendingSelection = {};
    }

    const bool listUpdates = m_directoryList ? m_directoryList->updatesEnabled() : true;
    const bool detailsUpdates = m_directoryDetails ? m_directoryDetails->updatesEnabled() : true;
    if (m_directoryList) m_directoryList->setUpdatesEnabled(false);
    if (m_directoryDetails) m_directoryDetails->setUpdatesEnabled(false);

    m_directoryList->clear(); m_directoryDetails->clear();
    QMimeDatabase mimeDb;
    DirectoryListingCore::RenderOptions coreOptions;
    coreOptions.sortKey = options.sortKey;
    coreOptions.sortAscending = options.sortAscending;
    coreOptions.groupMode = options.groupMode;
    coreOptions.acceptsFile = options.acceptsFile;
    const auto prepared = m_listingCore.prepare(coreOptions, mimeDb);
    m_directoryList->setCategorized(options.groupMode != DirectoryViewSettings::NoGrouping);
    QString previousCategory; bool firstCategory = true;
    for (const auto &entry : prepared.files) {
        if (options.groupMode != DirectoryViewSettings::NoGrouping
            && (firstCategory || entry.categorySort != previousCategory)) {
            addDirectoryGroupHeader(m_directoryDetails, entry.categoryDisplay);
            previousCategory = entry.categorySort; firstCategory = false;
        }
        addDirectoryFileItems(m_directoryList, m_directoryDetails, entry.file,
            iconForFile(entry.file, mimeDb, options.thumbnailsEnabled), entry.typeText,
            formatFileSize(entry.file.size, entry.file.isDir),
            formatModificationTime(entry.file.modificationTime),
            {parentLocationForDisplay(entry.file.url)},
            options.groupMode != DirectoryViewSettings::NoGrouping ? entry.categoryDisplay : QString(),
            options.groupMode != DirectoryViewSettings::NoGrouping ? QVariant(entry.categorySort) : QVariant());
    }
    restoreDirectoryViewState(m_directoryList, m_directoryDetails, viewState);

    if (m_directoryList) m_directoryList->setUpdatesEnabled(listUpdates);
    if (m_directoryDetails) m_directoryDetails->setUpdatesEnabled(detailsUpdates);

    if (options.updateStatus) options.updateStatus(prepared.visibleCount, prepared.totalCount);
    if (m_previewAdapter) m_previewAdapter->scheduleUpdate();
}
