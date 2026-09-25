#include "primarybrowserpane.h"
#include "appwidgets.h"
#include "directoryview.h"
#include "directoryviewsettings.h"
#include "searchcontroller.h"
#include <KIO/ListJob>
#include <KJob>
#include <QImageReader>
#include <QLabel>
#include <QMimeDatabase>
#include <QStackedWidget>
#include <QVBoxLayout>

PrimaryBrowserPane::PrimaryBrowserPane(QWidget *parent) : QFrame(parent)
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
    if (m_directoryJob) { m_directoryJob->kill(); m_directoryJob = nullptr; }
}

KIO::ListJob *PrimaryBrowserPane::listingJob() const
{
    return m_directoryJob.data();
}

void PrimaryBrowserPane::loadDirectory(const QUrl &url, bool preserveStatusMessage,
    bool showHiddenFiles, const std::function<void(bool)> &render,
    const std::function<void(const QString &)> &reportError)
{
    cancelListing();
    m_currentUrl = url;
    m_pendingFiles.clear();
    m_directoryList->clear(); m_directoryDetails->clear();
    m_directoryList->setDropDirectory(url); m_directoryDetails->setDropDirectory(url);
    m_directoryTitle->setText(urlForDisplay(url));
    m_directoryStatus->setText(trLocal("Wczytywanie…", "Loading…"));
    KIO::ListJob *job = KIO::listDir(url, KIO::HideProgressInfo);
    job->setUiDelegate(nullptr);
    job->setProperty("thispcPreserveStatusMessage", preserveStatusMessage);
    m_directoryJob = job;
    Q_EMIT listingStarted(url);
    connect(job, &KIO::ListJob::entries, this,
        [this, url, job, showHiddenFiles](KIO::Job *, const KIO::UDSEntryList &entries) {
        if (m_directoryJob != job) return;
        for (const KIO::UDSEntry &entry : entries) {
            const QString rawName = entry.stringValue(KIO::UDSEntry::UDS_NAME);
            const FileInfo file = fileInfoForEntry(url, entry);
            if (file.name.isEmpty() || rawName == QStringLiteral(".")
                || rawName == QStringLiteral("..")
                || (!showHiddenFiles && rawName.startsWith(QLatin1Char('.')))) continue;
            m_pendingFiles.push_back(file);
        }
    });
    connect(job, &KJob::result, this, [this, job, url, render, reportError](KJob *) {
        if (m_directoryJob != job) return;
        m_directoryJob = nullptr;
        if (job->error()) {
            m_directoryStatus->setText(trLocal("Nie udało się otworzyć tej lokalizacji.",
                                                "Could not open this location."));
            reportError(job->errorString());
            Q_EMIT listingFinished(url, false);
            return;
        }
        render(job->property("thispcPreserveStatusMessage").toBool());
        Q_EMIT listingFinished(url, true);
    });
}

QIcon PrimaryBrowserPane::iconForFile(const FileInfo &file,
    QMimeDatabase &mimeDatabase, bool thumbnailsEnabled)
{
    const QMimeType mime = resolvedMimeType(file, mimeDatabase);
    QString iconName = file.iconName;
    if (iconName.isEmpty() || iconName == QStringLiteral("text-x-generic")) {
        if (mime.isValid() && !mime.isDefault()) {
            iconName = mime.iconName();
            if (iconName.isEmpty()) iconName = mime.genericIconName();
        }
    }
    if (iconName.isEmpty()) iconName = file.isDir ? QStringLiteral("folder") : QStringLiteral("unknown");
    QIcon fallback = themedIcon(iconName,
        file.isDir ? QStringLiteral("folder") : QStringLiteral("text-x-generic"));
    const QString mimeName = mime.isValid() ? mime.name() : QString();
    if (!thumbnailsEnabled || file.isDir || !file.url.isLocalFile()
        || !mimeName.startsWith(QStringLiteral("image/"))
        || file.size > 64LL * 1024LL * 1024LL) return fallback;
    const QString path = file.url.toLocalFile();
    const QString cacheKey = QStringLiteral("%1|%2|%3")
        .arg(path).arg(file.modificationTime).arg(file.size);
    const auto cached = m_thumbnailCache.constFind(cacheKey);
    if (cached != m_thumbnailCache.constEnd()) return cached.value();
    QImageReader reader(path); reader.setAutoTransform(true);
    const QSize sourceSize = reader.size(); const QSize targetSize(128, 128);
    if (sourceSize.isValid() && (sourceSize.width() > 128 || sourceSize.height() > 128))
        reader.setScaledSize(sourceSize.scaled(targetSize, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull()) return fallback;
    const QIcon icon(QPixmap::fromImage(image).scaled(
        targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_thumbnailCache.insert(cacheKey, icon);
    return icon;
}

void PrimaryBrowserPane::renderDirectoryItems(const RenderOptions &options)
{
    const auto selectedListUrls = selectedDirectoryListUrls(m_directoryList);
    const auto selectedDetailsUrls = selectedDirectoryDetailsUrls(m_directoryDetails);
    sortDirectoryFiles(m_pendingFiles, options.sortKey, options.sortAscending,
                       [](const FileInfo &file) { return file.mimeType; });
    m_directoryList->clear(); m_directoryDetails->clear();
    QMimeDatabase mimeDb;
    struct RenderedFile { FileInfo file; QString typeText; QString categoryDisplay; QString categorySort; };
    QList<RenderedFile> rendered; int visibleCount = 0;
    for (const FileInfo &file : std::as_const(m_pendingFiles)) {
        if (options.acceptsFile && !options.acceptsFile(file, mimeDb)) continue;
        ++visibleCount;
        const QString typeText = fileTypeLabel(file, mimeDb);
        if (options.groupMode == DirectoryViewSettings::GroupByDate) {
            const auto category = directory_view_detail::dateCategoryForModification(file.modificationTime);
            rendered.push_back({file, typeText, category.display, category.sortKey});
        } else if (options.groupMode == DirectoryViewSettings::GroupBySize) {
            const auto category = directory_view_detail::sizeCategoryForFile(file);
            rendered.push_back({file, typeText, category.display, category.sortKey});
        } else {
            rendered.push_back({file, typeText, file.isDir ? trLocal("Foldery", "Folders") : typeText,
                                file.isDir ? QString() : typeText.toCaseFolded()});
        }
    }
    if (options.groupMode != DirectoryViewSettings::NoGrouping)
        std::stable_sort(rendered.begin(), rendered.end(), [](const auto &a, const auto &b) {
            return a.categorySort.localeAwareCompare(b.categorySort) < 0;
        });
    m_directoryList->setCategorized(options.groupMode != DirectoryViewSettings::NoGrouping);
    QString previousCategory; bool firstCategory = true;
    for (const RenderedFile &entry : std::as_const(rendered)) {
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
    restoreDirectorySelections(m_directoryList, m_directoryDetails,
                               selectedListUrls, selectedDetailsUrls);
    if (options.updateStatus) options.updateStatus(visibleCount, m_pendingFiles.size());
}
