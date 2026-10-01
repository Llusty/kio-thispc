/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "directorylistingcore.h"

#include "remoteurlhelper.h"
#include "directoryview.h"
#include "directoryviewsettings.h"

#include <KIO/ListJob>
#include <KJob>
#include <QImageReader>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPixmap>

#include <algorithm>

#include <KIO/JobUiDelegateFactory>
#include <KJobUiDelegate>
#include <KJobWidgets>
#include <QWidget>

DirectoryListingCore::DirectoryListingCore(QObject *parent)
    : QObject(parent)
{
}

void DirectoryListingCore::startListing(const QUrl &url,
    const ListingOptions &options, const ListingCallbacks &callbacks)
{
    cancelListing();
    m_loading = true;
    m_currentUrl = url;
    m_callbacks = callbacks;
    m_stagedFiles.clear();

    KIO::ListJob *job = KIO::listDir(url, KIO::HideProgressInfo);
    QWidget *parentWidget = qobject_cast<QWidget *>(parent());
    QWidget *window = parentWidget ? parentWidget->window() : nullptr;
    if (window) {
        KJobWidgets::setWindow(job, window);
        if (KJobUiDelegate *delegate =
                KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingDisabled, window)) {
            job->setUiDelegate(delegate);
        }
    } else {
        job->setUiDelegate(nullptr);
    }
    m_job = job;

    connect(job, &KIO::ListJob::entries, this,
        [this, url, job, options](KIO::Job *, const KIO::UDSEntryList &entries) {
            if (m_job != job) return;
            appendEntries(m_stagedFiles, url, entries, options);
        });
    connect(job, &KJob::result, this, &DirectoryListingCore::slotJobFinished);

    Q_EMIT listingStarted(url);
}

void DirectoryListingCore::slotJobFinished(KJob *job)
{
    if (m_job != job) return;
    m_job = nullptr;
    m_loading = false;
    ListingCallbacks callbacks = m_callbacks;
    m_callbacks = {};

    if (job->error()) {
        m_stagedFiles.clear();
        const QString err = RemoteUrlHelper::sanitizeErrorMessage(job->errorString(), m_currentUrl);
        if (callbacks.failed) callbacks.failed(err);
        Q_EMIT listingFailed(err);
        return;
    }

    m_files = std::move(m_stagedFiles);
    m_stagedFiles.clear();
    if (callbacks.succeeded) callbacks.succeeded(qobject_cast<KIO::ListJob *>(job));
    Q_EMIT listingFinished();
}

void DirectoryListingCore::cancelListing()
{
    if (!m_loading && !m_job) {
        m_stagedFiles.clear();
        return;
    }
    m_stagedFiles.clear();
    m_loading = false;
    m_callbacks = {};

    if (m_job) {
        KIO::ListJob *job = m_job.data();
        m_job = nullptr;
        job->kill(KJob::Quietly);
    }
    Q_EMIT listingCanceled();
}

KIO::ListJob *DirectoryListingCore::listingJob() const
{
    return m_job.data();
}

FileInfo DirectoryListingCore::mapEntry(const QUrl &url, const KIO::UDSEntry &entry)
{
    return fileInfoForEntry(url, entry);
}

bool DirectoryListingCore::acceptsEntry(const KIO::UDSEntry &entry,
    const FileInfo &file, const ListingOptions &options)
{
    const QString rawName = entry.stringValue(KIO::UDSEntry::UDS_NAME);
    const bool emptyName = options.emptyNamePolicy == EmptyNamePolicy::RawName
        ? rawName.isEmpty() : file.name.isEmpty();
    return !emptyName
        && rawName != QStringLiteral(".")
        && rawName != QStringLiteral("..")
        && (options.showHiddenFiles || !file.isHidden);
}

void DirectoryListingCore::appendEntries(QList<FileInfo> &files, const QUrl &url,
    const KIO::UDSEntryList &entries, const ListingOptions &options)
{
    for (const KIO::UDSEntry &entry : entries) {
        const FileInfo file = mapEntry(url, entry);
        if (acceptsEntry(entry, file, options)) files.push_back(file);
    }
}

DirectoryListingCore::PreparedListing DirectoryListingCore::prepare(
    const RenderOptions &options, QMimeDatabase &mimeDatabase)
{
    sortDirectoryFiles(m_files, options.sortKey, options.sortAscending,
        [&mimeDatabase](const FileInfo &file) { return fileTypeLabel(file, mimeDatabase); });

    PreparedListing prepared;
    prepared.totalCount = m_files.size();
    for (const FileInfo &file : std::as_const(m_files)) {
        if (options.acceptsFile && !options.acceptsFile(file, mimeDatabase)) continue;
        ++prepared.visibleCount;
        const QString typeText = fileTypeLabel(file, mimeDatabase);
        if (options.groupMode == DirectoryViewSettings::GroupByDate) {
            const auto category = directory_view_detail::dateCategoryForModification(
                file.modificationTime);
            prepared.files.push_back({file, typeText, category.display, category.sortKey});
        } else if (options.groupMode == DirectoryViewSettings::GroupBySize) {
            const auto category = directory_view_detail::sizeCategoryForFile(file);
            prepared.files.push_back({file, typeText, category.display, category.sortKey});
        } else {
            prepared.files.push_back({file, typeText,
                file.isDir ? trLocal("Foldery", "Folders") : typeText,
                file.isDir ? QString() : typeText.toCaseFolded()});
        }
    }
    if (options.groupMode != DirectoryViewSettings::NoGrouping) {
        std::stable_sort(prepared.files.begin(), prepared.files.end(),
            [](const RenderedFile &left, const RenderedFile &right) {
                return left.categorySort.localeAwareCompare(right.categorySort) < 0;
            });
    }
    return prepared;
}

QIcon DirectoryListingCore::iconForFile(const FileInfo &file,
    QMimeDatabase &mimeDatabase, bool thumbnailsEnabled, bool cacheThumbnail)
{
    const QMimeType mime = resolvedMimeType(file, mimeDatabase);
    const QString iconName = resolvedIconName(file, mimeDatabase);
    const QIcon fallback = themedIcon(iconName,
        file.isDir ? QStringLiteral("folder") : QStringLiteral("text-x-generic"));
    const QString mimeName = mime.isValid() ? mime.name() : QString();
    if (!thumbnailsEnabled || file.isDir || !file.url.isLocalFile()
        || !mimeName.startsWith(QStringLiteral("image/"))
        || file.size > 64LL * 1024LL * 1024LL) return fallback;

    const QString path = file.url.toLocalFile();
    const QString cacheKey = QStringLiteral("%1|%2|%3")
        .arg(path).arg(file.modificationTime).arg(file.size);
    if (cacheThumbnail) {
        const auto cached = m_thumbnailCache.constFind(cacheKey);
        if (cached != m_thumbnailCache.constEnd()) return cached.value();
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize sourceSize = reader.size();
    const QSize targetSize(128, 128);
    if (sourceSize.isValid()
        && (sourceSize.width() > targetSize.width() || sourceSize.height() > targetSize.height()))
        reader.setScaledSize(sourceSize.scaled(targetSize, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull()) return fallback;
    const QIcon icon(QPixmap::fromImage(image).scaled(
        targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (cacheThumbnail) m_thumbnailCache.insert(cacheKey, icon);
    return icon;
}

QString DirectoryListingCore::resolvedIconName(const FileInfo &file,
    QMimeDatabase &mimeDatabase)
{
    const QMimeType mime = resolvedMimeType(file, mimeDatabase);
    QString iconName = file.iconName;
    if (iconName.isEmpty() || iconName == QStringLiteral("text-x-generic")) {
        if (mime.isValid() && !mime.isDefault()) {
            iconName = mime.iconName();
            if (iconName.isEmpty()) iconName = mime.genericIconName();
        }
    }
    if (iconName.isEmpty())
        iconName = file.isDir ? QStringLiteral("folder") : QStringLiteral("unknown");
    return iconName;
}

QList<int> DirectoryListingCore::rowsForSelection(
    const QList<RenderedFile> &files, const QSet<QString> &selectedUrls)
{
    QList<int> rows;
    for (int row = 0; row < files.size(); ++row) {
        if (selectedUrls.contains(files.at(row).file.url.toString())) rows.push_back(row);
    }
    return rows;
}
