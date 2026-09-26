/*
 * Neutral directory listing and render-data core shared by both browser panes.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"

#include <KIO/UDSEntry>

#include <QHash>
#include <QIcon>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <functional>

namespace KIO { class ListJob; }
class QMimeDatabase;

class DirectoryListingCore final : public QObject
{
public:
    enum class EmptyNamePolicy {
        DisplayName,
        RawName,
    };

    struct ListingOptions {
        bool showHiddenFiles = false;
        EmptyNamePolicy emptyNamePolicy = EmptyNamePolicy::DisplayName;
    };

    struct ListingCallbacks {
        std::function<void(KIO::ListJob *)> succeeded;
        std::function<void(const QString &)> failed;
    };

    struct RenderOptions {
        int sortKey = 0;
        bool sortAscending = true;
        int groupMode = 0;
        std::function<bool(const FileInfo &, QMimeDatabase &)> acceptsFile;
    };

    struct RenderedFile {
        FileInfo file;
        QString typeText;
        QString categoryDisplay;
        QString categorySort;
    };

    struct PreparedListing {
        QList<RenderedFile> files;
        int visibleCount = 0;
        int totalCount = 0;
    };

    explicit DirectoryListingCore(QObject *parent = nullptr);

    void startListing(const QUrl &, const ListingOptions &, const ListingCallbacks &);
    void cancelListing();
    KIO::ListJob *listingJob() const;

    const QList<FileInfo> &files() const { return m_files; }
    QList<FileInfo> &mutableFiles() { return m_files; }
    void setFiles(const QList<FileInfo> &files) { m_files = files; }

    PreparedListing prepare(const RenderOptions &, QMimeDatabase &);
    QIcon iconForFile(const FileInfo &, QMimeDatabase &, bool thumbnailsEnabled,
                      bool cacheThumbnail);
    void clearThumbnailCache() { m_thumbnailCache.clear(); }
    int thumbnailCacheSize() const { return m_thumbnailCache.size(); }

    static FileInfo mapEntry(const QUrl &, const KIO::UDSEntry &);
    static bool acceptsEntry(const KIO::UDSEntry &, const FileInfo &,
                             const ListingOptions &);
    static void appendEntries(QList<FileInfo> &, const QUrl &,
                              const KIO::UDSEntryList &, const ListingOptions &);
    static QString resolvedIconName(const FileInfo &, QMimeDatabase &);
    static QList<int> rowsForSelection(const QList<RenderedFile> &,
                                       const QSet<QString> &selectedUrls);

private:
    QList<FileInfo> m_files;
    QHash<QString, QIcon> m_thumbnailCache;
    QPointer<KIO::ListJob> m_job;
};
