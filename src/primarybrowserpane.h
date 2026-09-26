/* Primary browser pane extracted from ThisPcWindow during 0.32 Stage 8. */
#pragma once

#include "browsercommon.h"
#include "directorylistingcore.h"
#include "directoryview.h"
#include <QFrame>
#include <functional>

class DirectoryListWidget;
class DirectoryTreeWidget;
class QLabel;
class QMimeDatabase;
class QStackedWidget;

class PrimaryBrowserPane : public QFrame
{
    Q_OBJECT
public:
    struct RenderOptions {
        int sortKey = 0;
        bool sortAscending = true;
        int groupMode = 0;
        bool thumbnailsEnabled = true;
        bool detailsActive = false;
        std::function<bool(const FileInfo &, QMimeDatabase &)> acceptsFile;
        std::function<void(int, int)> updateStatus;
    };
    explicit PrimaryBrowserPane(QWidget *parent = nullptr);
    QStackedWidget *contentStack() const { return m_contentStack; }
    QUrl currentUrl() const { return m_currentUrl; }
    void setCurrentUrl(const QUrl &url) { m_currentUrl = url; }
    void bindDirectoryViews(DirectoryListWidget *, DirectoryTreeWidget *, QLabel *, QLabel *);
    DirectoryListWidget *listView() const { return m_directoryList; }
    DirectoryTreeWidget *detailsView() const { return m_directoryDetails; }
    const QList<FileInfo> &files() const { return m_listingCore.files(); }
    QList<FileInfo> &mutableFiles() { return m_listingCore.mutableFiles(); }
    void setFiles(const QList<FileInfo> &files) { m_listingCore.setFiles(files); }
    KIO::ListJob *listingJob() const;
    void clearThumbnailCache() { m_listingCore.clearThumbnailCache(); }
    void loadDirectory(const QUrl &, bool, bool, bool,
                       const std::function<void(bool)> &,
                       const std::function<void(const QString &)> &);
    void renderDirectoryItems(const RenderOptions &);
    QIcon iconForFile(const FileInfo &, QMimeDatabase &, bool);
    void cancelListing();
Q_SIGNALS:
    void listingStarted(const QUrl &);
    void listingFinished(const QUrl &, bool);
private:
    QStackedWidget *m_contentStack = nullptr;
    DirectoryListWidget *m_directoryList = nullptr;
    DirectoryTreeWidget *m_directoryDetails = nullptr;
    QLabel *m_directoryTitle = nullptr;
    QLabel *m_directoryStatus = nullptr;
    QUrl m_currentUrl = kThisPcUrl;
    DirectoryListingCore m_listingCore;
    DirectorySelectionSnapshot m_pendingSelection;
};
