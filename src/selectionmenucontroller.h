#pragma once

#include "panecontext.h"

#include <QList>
#include <QUrl>

#include <functional>

class QAction;
class QMenu;
class QWidget;

class SelectionMenuController
{
public:
    enum class PrintableKind { None, Image, Text, Pdf };

    struct ViewState {
        PaneId pane = PaneId::Primary;
        int viewMode = 0;
        int iconSizeMode = 2;
        int sortKey = 0;
        bool sortAscending = true;
        int groupMode = 0;
        bool showHidden = false;
        bool thumbnails = true;
        QAction *previewAction = nullptr;
        QAction *fullNamesAction = nullptr;
    };

    struct ViewCallbacks {
        std::function<void(int)> setViewMode;
        std::function<void(int)> setIconSizeMode;
        std::function<void(int)> setSortKey;
        std::function<void(bool)> setSortAscending;
        std::function<void(int)> setGroupMode;
        std::function<void(bool)> setShowHidden;
        std::function<void(bool)> setThumbnails;
    };

    struct SendToCallbacks {
        std::function<void(const QList<QUrl> &, const QString &, const QString &)> copyToDirectory;
        std::function<void(const QList<QUrl> &)> createZip;
        std::function<void(const QList<QUrl> &)> createSevenZip;
        std::function<void(const QList<QUrl> &)> createTarGzip;
    };

    explicit SelectionMenuController(QWidget *parent);

    void addOpenWithSubmenu(QMenu &menu, const QList<QUrl> &urls) const;
    void addSendToSubmenu(QMenu &menu, const QList<QUrl> &urls,
                          const SendToCallbacks &callbacks) const;
    void addViewSubmenu(QMenu &menu, const ViewState &state,
                        const ViewCallbacks &callbacks) const;
    void addSortSubmenu(QMenu &menu, const ViewState &state,
                        const ViewCallbacks &callbacks) const;

    PrintableKind printableKindForUrl(const QUrl &url, bool isDir) const;
    bool canPrintUrl(const QUrl &url, bool isDir) const;
    bool isLocalImageUrl(const QUrl &url, bool isDir = false) const;
    bool canSetWallpaper(const QUrl &url, bool isDir) const;
    bool allUrlsAreLocalFiles(const QList<QUrl> &urls, bool allowDirectories) const;
    bool selectionHasCommonParent(const QList<QUrl> &urls, QString *parentPath = nullptr) const;

    void printUrl(const QUrl &url) const;
    void setAsDesktopWallpaper(const QUrl &url,
                               const std::function<void(const QString &)> &status) const;

private:
    void printImageUrl(const QUrl &url) const;
    void printTextUrl(const QUrl &url) const;
    void sendSelectionByEmail(const QList<QUrl> &urls) const;
    void sendSelectionByBluetooth(const QList<QUrl> &urls) const;

    QWidget *m_parent = nullptr;
};
