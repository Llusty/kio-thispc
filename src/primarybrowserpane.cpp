#include "primarybrowserpane.h"
#include "appwidgets.h"
#include "directoryview.h"
#include "directoryviewsettings.h"
#include "searchcontroller.h"
#include <KIO/ListJob>
#include <QLabel>
#include <QMimeDatabase>
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
}

void PrimaryBrowserPane::bindDirectoryViews(DirectoryListWidget *list,
    DirectoryTreeWidget *details, QLabel *title, QLabel *status)
{
    m_directoryList = list; m_directoryDetails = details;
    m_directoryTitle = title; m_directoryStatus = status;
}

void PrimaryBrowserPane::cancelListing()
{
    m_listingCore.cancelListing();
}

KIO::ListJob *PrimaryBrowserPane::listingJob() const
{
    return m_listingCore.listingJob();
}

void PrimaryBrowserPane::loadDirectory(const QUrl &url, bool preserveStatusMessage,
    bool showHiddenFiles, bool detailsActive, const std::function<void(bool)> &render,
    const std::function<void(const QString &)> &reportError)
{
    cancelListing();
    m_pendingSelection = sameLocation(m_currentUrl, url)
        ? captureDirectorySelection(m_directoryList, m_directoryDetails, detailsActive)
        : DirectorySelectionSnapshot{};
    m_currentUrl = url;
    m_directoryList->clear(); m_directoryDetails->clear();
    m_directoryList->setDropDirectory(url); m_directoryDetails->setDropDirectory(url);
    m_directoryTitle->setText(urlForDisplay(url));
    m_directoryStatus->setText(trLocal("Wczytywanie…", "Loading…"));
    Q_EMIT listingStarted(url);
    DirectoryListingCore::ListingOptions options;
    options.showHiddenFiles = showHiddenFiles;
    options.emptyNamePolicy = DirectoryListingCore::EmptyNamePolicy::DisplayName;
    m_listingCore.startListing(url, options, {
        [this, url, render](KIO::ListJob *job) {
            render(job->property("thispcPreserveStatusMessage").toBool());
            Q_EMIT listingFinished(url, true);
        },
        [this, url, reportError](const QString &error) {
            m_directoryStatus->setText(trLocal("Nie udało się otworzyć tej lokalizacji.",
                                                "Could not open this location."));
            reportError(error);
            Q_EMIT listingFinished(url, false);
        }
    });
    if (KIO::ListJob *job = m_listingCore.listingJob())
        job->setProperty("thispcPreserveStatusMessage", preserveStatusMessage);
}

QIcon PrimaryBrowserPane::iconForFile(const FileInfo &file,
    QMimeDatabase &mimeDatabase, bool thumbnailsEnabled)
{
    return m_listingCore.iconForFile(file, mimeDatabase, thumbnailsEnabled, true);
}

void PrimaryBrowserPane::renderDirectoryItems(const RenderOptions &options)
{
    DirectorySelectionSnapshot selection =
        captureDirectorySelection(
            m_directoryList, m_directoryDetails, options.detailsActive);
    selection.urls.unite(m_pendingSelection.urls);
    if (selection.currentUrl.isEmpty())
        selection.currentUrl = m_pendingSelection.currentUrl;
    m_pendingSelection = {};
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
    restoreDirectorySelection(m_directoryList, m_directoryDetails, selection);
    if (options.updateStatus) options.updateStatus(prepared.visibleCount, prepared.totalCount);
}
