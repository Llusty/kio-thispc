/*
 * Sidebar UI and persistent Quick Access / Recent state.
 *
 * Extracted during the 0.21.0 architecture refactor. The sidebar owns its
 * presentation and persistence while the main window remains responsible for
 * navigation, tabs, windows and split-pane actions.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "directoryview.h"

#include <KProtocolManager>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

class SidebarButton : public QPushButton
{
    Q_OBJECT

public:
    SidebarButton(const QString &text,
                  const QString &iconName,
                  const QUrl &url,
                  QWidget *parent = nullptr)
        : QPushButton(themedIcon(iconName), text, parent)
        , m_url(url)
    {
        setObjectName(QStringLiteral("sidebarButton"));
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        setIconSize(QSize(18, 18));
        setMinimumHeight(31);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setToolTip(text);

        connect(this, &QPushButton::clicked, this, [this] {
            Q_EMIT activated(m_url);
        });
    }

    QUrl url() const
    {
        return m_url;
    }

    void setCurrent(bool current)
    {
        setProperty("current", current);
        style()->unpolish(this);
        style()->polish(this);
        update();
    }

    void setQuickAccessEntry(bool enabled)
    {
        m_quickAccessEntry = enabled;
    }

Q_SIGNALS:
    void activated(const QUrl &url);
    void openInNewTabRequested(const QUrl &url, bool makeCurrent);
    void openInNewWindowRequested(const QUrl &url);
    void openInSplitPaneRequested(const QUrl &url);
    void removeFromQuickAccessRequested(const QUrl &url);

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        int textWidth = style()->subElementRect(
            QStyle::SE_PushButtonContents, &option, this).width();
        if (!option.icon.isNull()) {
            // Match the icon/text spacing used by Qt's push-button styles.
            textWidth -= option.icon.actualSize(option.iconSize).width() + 4;
        }
        // Elide only the painted copy; keep the complete label for tooltip,
        // accessibility and subsequent repaints at a different sidebar width.
        option.text = option.fontMetrics.elidedText(
            option.text, Qt::ElideRight, std::max(0, textWidth),
            Qt::TextShowMnemonic);
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_PushButton, option);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton) {
            Q_EMIT openInNewTabRequested(m_url, false);
            event->accept();
            return;
        }

        QPushButton::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QMenu menu(this);

        QAction *openAction = menu.addAction(
            themedIcon(QStringLiteral("folder-open")),
            trLocal("Otwórz", "Open"));
        QAction *newTabAction = menu.addAction(
            themedIcon(QStringLiteral("tab-new")),
            trLocal("Otwórz w nowej karcie", "Open in new tab"));
        QAction *newWindowAction = menu.addAction(
            themedIcon(QStringLiteral("window-new")),
            trLocal("Otwórz w nowym oknie", "Open in new window"));
        QAction *splitPaneAction = menu.addAction(
            themedIcon(QStringLiteral("view-split-left-right"),
                       QStringLiteral("view-list-details")),
            trLocal("Otwórz w drugim panelu", "Open in other pane"));

        QAction *removeQuickAccessAction = nullptr;
        if (m_quickAccessEntry) {
            menu.addSeparator();
            removeQuickAccessAction = menu.addAction(
                themedIcon(QStringLiteral("list-remove")),
                trLocal("Odepnij od Szybkiego dostępu",
                        "Unpin from Quick access"));
        }

        QAction *chosen = menu.exec(event->globalPos());
        if (chosen == openAction) {
            Q_EMIT activated(m_url);
        } else if (chosen == newTabAction) {
            Q_EMIT openInNewTabRequested(m_url, true);
        } else if (chosen == newWindowAction) {
            Q_EMIT openInNewWindowRequested(m_url);
        } else if (chosen == splitPaneAction) {
            Q_EMIT openInSplitPaneRequested(m_url);
        } else if (removeQuickAccessAction
                   && chosen == removeQuickAccessAction) {
            Q_EMIT removeFromQuickAccessRequested(m_url);
        }

        event->accept();
    }

private:
    QUrl m_url;
    bool m_quickAccessEntry = false;
};


class QuickAccessSidebarButton : public SidebarButton
{
    Q_OBJECT

public:
    QuickAccessSidebarButton(const QString &text,
                             const QString &iconName,
                             const QUrl &url,
                             QWidget *parent = nullptr)
        : SidebarButton(text, iconName, url, parent)
    {
        setQuickAccessEntry(true);
        setAcceptDrops(true);
    }

Q_SIGNALS:
    void moveRequested(const QUrl &source,
                       const QUrl &target,
                       bool insertAfter);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragStart = event->position().toPoint();
        }
        SidebarButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)
            || (event->position().toPoint() - m_dragStart).manhattanLength()
                < QApplication::startDragDistance()) {
            SidebarButton::mouseMoveEvent(event);
            return;
        }

        auto *drag = new QDrag(this);
        auto *mime = new QMimeData;
        mime->setData(
            QStringLiteral("application/x-thispc-quick-access"),
            url().toString(QUrl::FullyEncoded).toUtf8());
        drag->setMimeData(mime);
        drag->setPixmap(icon().pixmap(18, 18));
        drag->exec(Qt::MoveAction);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasFormat(
                QStringLiteral("application/x-thispc-quick-access"))) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasFormat(
                QStringLiteral("application/x-thispc-quick-access"))) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
        const QByteArray encoded = event->mimeData()->data(
            QStringLiteral("application/x-thispc-quick-access"));
        const QUrl source = normalizedUrl(QUrl(QString::fromUtf8(encoded)));

        if (!source.isValid() || sameLocation(source, url())) {
            event->ignore();
            return;
        }

        const bool insertAfter = event->position().y() > height() / 2.0;
        Q_EMIT moveRequested(source, url(), insertAfter);
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

private:
    QPoint m_dragStart;
};


class SidebarDriveLabel : public QLabel
{
public:
    SidebarDriveLabel(const QString &text, QWidget *parent)
        : QLabel(text, parent)
    {
        setObjectName(QStringLiteral("sidebarDriveName"));
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        style()->drawItemText(
            &painter, contentsRect(),
            QStyle::visualAlignment(layoutDirection(), alignment()),
            palette(), isEnabled(),
            fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()),
            foregroundRole());
    }
};


class SidebarDriveButton : public QFrame
{
    Q_OBJECT

public:
    SidebarDriveButton(const DriveInfo &drive, QWidget *parent = nullptr)
        : QFrame(parent)
        , m_drive(drive)
    {
        setObjectName(QStringLiteral("sidebarDrive"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(driveTooltip(drive));

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setSpacing(7);

        QString iconName = QStringLiteral("drive-harddisk");
        if (drive.iconName.contains(QStringLiteral("removable"))) {
            iconName = QStringLiteral("drive-removable-media");
        }

        auto *icon = new QLabel(this);
        icon->setPixmap(
            themedIcon(iconName, QStringLiteral("drive-harddisk"))
                .pixmap(17, 17));
        icon->setFixedSize(19, 19);
        makePassive(icon);

        auto *body = new QVBoxLayout;
        body->setSpacing(2);
        body->setContentsMargins(0, 0, 0, 0);

        auto *name = new SidebarDriveLabel(drive.name, this);
        makePassive(name);

        auto *bar = new QProgressBar(this);
        bar->setObjectName(QStringLiteral("sidebarProgress"));
        bar->setRange(0, 100);
        bar->setValue(drive.usedPercent);
        bar->setTextVisible(false);
        bar->setFixedHeight(5);
        makePassive(bar);

        body->addWidget(name);
        body->addWidget(bar);
        layout->addWidget(icon, 0, Qt::AlignVCenter);
        layout->addLayout(body, 1);
    }

    QUrl url() const
    {
        return m_drive.targetUrl;
    }

    void setCurrent(bool current)
    {
        setProperty("current", current);
        style()->unpolish(this);
        style()->polish(this);
        update();
    }

Q_SIGNALS:
    void activated(const QUrl &url);
    void openInNewTabRequested(const QUrl &url, bool makeCurrent);
    void openInNewWindowRequested(const QUrl &url);
    void openInSplitPaneRequested(const QUrl &url);
    void openInDolphinRequested(const QUrl &url);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            setFocus(Qt::MouseFocusReason);
            Q_EMIT activated(m_drive.targetUrl);
            event->accept();
            return;
        }

        if (event->button() == Qt::MiddleButton) {
            Q_EMIT openInNewTabRequested(m_drive.targetUrl, false);
            event->accept();
            return;
        }

        QFrame::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter) {
            Q_EMIT activated(m_drive.targetUrl);
            event->accept();
            return;
        }
        QFrame::keyPressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QMenu menu(this);

        QAction *openAction = menu.addAction(
            themedIcon(QStringLiteral("folder-open")),
            trLocal("Otwórz", "Open"));
        QAction *newTabAction = menu.addAction(
            themedIcon(QStringLiteral("tab-new")),
            trLocal("Otwórz w nowej karcie", "Open in new tab"));
        QAction *newWindowAction = menu.addAction(
            themedIcon(QStringLiteral("window-new")),
            trLocal("Otwórz w nowym oknie", "Open in new window"));
        QAction *splitPaneAction = menu.addAction(
            themedIcon(QStringLiteral("view-split-left-right"),
                       QStringLiteral("view-list-details")),
            trLocal("Otwórz w drugim panelu", "Open in other pane"));

        menu.addSeparator();
        QAction *openDolphinAction = menu.addAction(
            themedIcon(QStringLiteral("system-file-manager")),
            trLocal("Otwórz w Dolphinie", "Open in Dolphin"));
        QAction *copyPathAction = menu.addAction(
            themedIcon(QStringLiteral("edit-copy")),
            trLocal("Kopiuj punkt montowania", "Copy mount point"));

        QAction *chosen = menu.exec(event->globalPos());
        if (chosen == openAction) {
            Q_EMIT activated(m_drive.targetUrl);
        } else if (chosen == newTabAction) {
            Q_EMIT openInNewTabRequested(m_drive.targetUrl, true);
        } else if (chosen == newWindowAction) {
            Q_EMIT openInNewWindowRequested(m_drive.targetUrl);
        } else if (chosen == splitPaneAction) {
            Q_EMIT openInSplitPaneRequested(m_drive.targetUrl);
        } else if (chosen == openDolphinAction) {
            Q_EMIT openInDolphinRequested(m_drive.targetUrl);
        } else if (chosen == copyPathAction) {
            QGuiApplication::clipboard()->setText(m_drive.mountPoint);
        }

        event->accept();
    }

private:
    static void makePassive(QWidget *widget)
    {
        widget->setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    static QString driveTooltip(const DriveInfo &drive)
    {
        return drive.name
            + QStringLiteral("\n")
            + drive.freeText
            + trLocal(" wolne z ", " free of ")
            + drive.capacityText
            + QStringLiteral("\n")
            + trLocal("System plików: ", "Filesystem: ")
            + drive.fileSystem
            + QStringLiteral("\n")
            + trLocal("Punkt montowania: ", "Mount point: ")
            + drive.mountPoint;
    }

    DriveInfo m_drive;
};


class CollapsibleSection : public QWidget
{
    Q_OBJECT

public:
    CollapsibleSection(const QString &title,
                       const QString &settingsKey,
                       QWidget *parent = nullptr)
        : QWidget(parent)
        , m_settingsKey(settingsKey)
    {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 2, 0, 3);
        root->setSpacing(1);

        m_header = new QToolButton(this);
        m_header->setObjectName(QStringLiteral("sidebarSectionButton"));
        m_header->setText(title);
        m_header->setCheckable(true);
        m_header->setChecked(true);
        m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_header->setArrowType(Qt::DownArrow);
        m_header->setIconSize(QSize(16, 16));
        m_header->setMinimumHeight(28);
        m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_content = new QWidget(this);
        m_contentLayout = new QVBoxLayout(m_content);
        m_contentLayout->setContentsMargins(0, 0, 0, 0);
        m_contentLayout->setSpacing(1);

        root->addWidget(m_header);
        root->addWidget(m_content);

        QSettings settings;
        const bool expanded = settings.value(
            QStringLiteral("sidebar/") + settingsKey,
            true).toBool();
        setExpanded(expanded);

        connect(m_header, &QToolButton::toggled, this, [this](bool checked) {
            setExpanded(checked);
            QSettings settings;
            settings.setValue(
                QStringLiteral("sidebar/") + m_settingsKey,
                checked);
        });
    }

    QVBoxLayout *contentLayout() const
    {
        return m_contentLayout;
    }

private:
    void setExpanded(bool expanded)
    {
        m_header->blockSignals(true);
        m_header->setChecked(expanded);
        m_header->setArrowType(
            expanded ? Qt::DownArrow : Qt::RightArrow);
        m_content->setVisible(expanded);
        m_header->blockSignals(false);
    }

    QString m_settingsKey;
    QToolButton *m_header = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;
};


class SidebarPanel : public QFrame
{
    Q_OBJECT

public:
    explicit SidebarPanel(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("sidebar"));
        // The enclosing scroll area owns the visible width. Ignore the
        // contents' horizontal size hint so labels follow the selected width;
        // retain the vertical hint for scrolling.
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(6, 5, 6, 8);
        m_layout->setSpacing(0);

        loadPersistentState();
        buildSections();
    }

    ~SidebarPanel() override
    {
        for (QObject *target : m_transferDropTargets.keys()) {
            if (target) {
                disconnect(target, &QObject::destroyed, this, nullptr);
            }
        }
    }

    bool canQuickAccessLocation(const QUrl &rawUrl) const
    {
        if (!rawUrl.isValid()) {
            return false;
        }

        const QUrl url = normalizedUrl(rawUrl);
        if (!url.isValid()
            || sameLocation(url, kThisPcUrl)
            || isSearchLocation(url)) {
            return false;
        }

        return KProtocolManager::supportsListing(url);
    }

    bool isQuickAccessPinned(const QUrl &rawUrl) const
    {
        if (!rawUrl.isValid()) {
            return false;
        }

        const QUrl url = normalizedUrl(rawUrl);
        for (const QUrl &favorite : m_quickAccessUrls) {
            if (sameLocation(favorite, url)) {
                return true;
            }
        }
        return false;
    }

    void toggleQuickAccessLocation(const QUrl &url)
    {
        if (isQuickAccessPinned(url)) {
            unpinQuickAccessLocation(url);
        } else {
            pinQuickAccessLocation(url);
        }
    }

    void recordRecentLocation(const QUrl &rawUrl)
    {
        if (!rawUrl.isValid()) {
            return;
        }

        const int scrollPosition = verticalScrollPosition();
        const QUrl url = normalizedUrl(rawUrl);
        if (!url.isValid()
            || sameLocation(url, kThisPcUrl)
            || isSearchLocation(url)
            || !KProtocolManager::supportsListing(url)) {
            return;
        }

        for (int i = m_recentLocationUrls.size() - 1; i >= 0; --i) {
            if (sameLocation(m_recentLocationUrls.at(i), url)) {
                m_recentLocationUrls.removeAt(i);
            }
        }

        m_recentLocationUrls.prepend(url);
        while (m_recentLocationUrls.size() > 10) {
            m_recentLocationUrls.removeLast();
        }

        saveRecentLocations();
        rebuildRecentLocations();
        restoreVerticalScrollPosition(scrollPosition);
    }

    void setDrives(const QList<DriveInfo> &drives)
    {
        const int scrollPosition = verticalScrollPosition();
        m_drives = drives;
        rebuildDevices();
        rebuildQuickAccess();
        rebuildRecentLocations();
        updateCurrent();
        restoreVerticalScrollPosition(scrollPosition);
    }

    void setCurrentLocation(const QUrl &url)
    {
        m_currentLocation = normalizedUrl(url);
        updateCurrent();
    }

Q_SIGNALS:
    void activated(const QUrl &url);
    void openInNewTabRequested(const QUrl &url, bool makeCurrent);
    void openInNewWindowRequested(const QUrl &url);
    void openInSplitPaneRequested(const QUrl &url);
    void openInDolphinRequested(const QUrl &url);
    void statusMessageRequested(const QString &message, int timeoutMs);
    void urlsDropped(const QList<QUrl> &urls,
                     const QUrl &destination,
                     const QPoint &globalPosition,
                     Qt::KeyboardModifiers modifiers);

public Q_SLOTS:
    void clearDropFeedback()
    {
        for (QObject *object : m_transferDropTargets.keys()) {
            setDropFeedback(qobject_cast<QWidget *>(object), false);
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto it = m_transferDropTargets.constFind(watched);
        if (it == m_transferDropTargets.cend()) {
            return QFrame::eventFilter(watched, event);
        }

        auto *target = qobject_cast<QWidget *>(watched);
        if (!target) {
            return QFrame::eventFilter(watched, event);
        }

        // Quick Access reorder owns this MIME even if a platform also exposes
        // URI data for the same drag. Never reinterpret it as a file transfer.
        auto hasQuickAccessMime = [](const QMimeData *mime) {
            return mime && mime->hasFormat(
                QStringLiteral("application/x-thispc-quick-access"));
        };

        switch (event->type()) {
        case QEvent::DragEnter: {
            auto *drag = static_cast<QDragEnterEvent *>(event);
            if (hasQuickAccessMime(drag->mimeData())) {
                setDropFeedback(target, false);
                return false;
            }
            return updateTransferDrag(target, it.value(), drag);
        }
        case QEvent::DragMove: {
            auto *drag = static_cast<QDragMoveEvent *>(event);
            if (hasQuickAccessMime(drag->mimeData())) {
                setDropFeedback(target, false);
                return false;
            }
            return updateTransferDrag(target, it.value(), drag);
        }
        case QEvent::DragLeave:
            setDropFeedback(target, false);
            event->accept();
            return true;
        case QEvent::Drop: {
            auto *drop = static_cast<QDropEvent *>(event);
            setDropFeedback(target, false);
            if (hasQuickAccessMime(drop->mimeData())) {
                return false;
            }
            if (!canTransferDrop(drop->mimeData(), it.value())) {
                drop->ignore();
                return true;
            }

            const QList<QUrl> urls = drop->mimeData()->urls();
            const QPoint globalPosition = target->mapToGlobal(
                drop->position().toPoint());
            drop->setDropAction(dropActionForUrls(
                urls, it.value(), drop->modifiers()));
            drop->accept();
            Q_EMIT urlsDropped(
                urls, it.value(), globalPosition, drop->modifiers());
            return true;
        }
        default:
            return QFrame::eventFilter(watched, event);
        }
    }

private:
    int verticalScrollPosition() const
    {
        const QScrollArea *scrollArea = enclosingScrollArea();
        return scrollArea ? scrollArea->verticalScrollBar()->value() : 0;
    }

    QScrollArea *enclosingScrollArea() const
    {
        QWidget *ancestor = parentWidget();
        while (ancestor) {
            if (auto *scrollArea = qobject_cast<QScrollArea *>(ancestor)) {
                return scrollArea;
            }
            ancestor = ancestor->parentWidget();
        }
        return nullptr;
    }

    void restoreVerticalScrollPosition(int position)
    {
        QTimer::singleShot(0, this, [this, position] {
            QScrollArea *scrollArea = enclosingScrollArea();
            if (!scrollArea) {
                return;
            }
            QScrollBar *bar = scrollArea->verticalScrollBar();
            bar->setValue(std::clamp(position, bar->minimum(), bar->maximum()));
        });
    }

    static bool isSearchLocation(const QUrl &url)
    {
        return url.scheme() == QStringLiteral("thispcsearch");
    }

    static bool isWithinLocation(const QUrl &childRaw,
                                 const QUrl &baseRaw)
    {
        const QUrl child = normalizedUrl(childRaw);
        const QUrl base = normalizedUrl(baseRaw);

        if (!child.isValid() || !base.isValid()) {
            return false;
        }
        if (sameLocation(child, base)) {
            return true;
        }
        if (child.scheme() != base.scheme()) {
            return false;
        }

        if (child.isLocalFile() && base.isLocalFile()) {
            const QString childPath = QDir::cleanPath(child.toLocalFile());
            const QString basePath = QDir::cleanPath(base.toLocalFile());
            if (basePath == QStringLiteral("/")) {
                return childPath.startsWith(QLatin1Char('/'));
            }
            return childPath.startsWith(basePath + QDir::separator());
        }

        QString childPath = child.path();
        QString basePath = base.path();
        if (!basePath.endsWith(QLatin1Char('/'))) {
            basePath += QLatin1Char('/');
        }
        return childPath.startsWith(basePath);
    }

    static int locationDepth(const QUrl &url)
    {
        if (url.isLocalFile()) {
            return QDir::cleanPath(url.toLocalFile())
                .split(QDir::separator(), Qt::SkipEmptyParts)
                .size();
        }
        return url.path()
            .split(QLatin1Char('/'), Qt::SkipEmptyParts)
            .size();
    }

    static bool isSupportedTransferDestination(const QUrl &rawUrl)
    {
        const QUrl url = normalizedUrl(rawUrl);
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || scheme.isEmpty()
            || scheme == QStringLiteral("thispc")
            || scheme == QStringLiteral("trash")
            || scheme == QStringLiteral("remote")
            || scheme == QStringLiteral("thispcsearch")
            || scheme == QStringLiteral("filenamesearch")) {
            return false;
        }

        if (url.isLocalFile()) {
            const QFileInfo info(url.toLocalFile());
            // Do not use isWritable(): NTFS and admin fallback are resolved by
            // the existing FileActions path after the Drop is dispatched.
            return info.exists() && info.isDir();
        }

        return KProtocolManager::supportsListing(url)
            && KProtocolManager::supportsWriting(url);
    }

    static bool canTransferDrop(const QMimeData *mime, const QUrl &destination)
    {
        if (!mime || !mime->hasUrls() || mime->urls().isEmpty()
            || !isSupportedTransferDestination(destination)) {
            return false;
        }
        for (const QUrl &url : mime->urls()) {
            if (!url.isValid()) {
                return false;
            }
        }
        return true;
    }

    template<typename DragEvent>
    static bool updateTransferDrag(QWidget *target,
                                   const QUrl &destination,
                                   DragEvent *event)
    {
        if (!canTransferDrop(event->mimeData(), destination)) {
            setDropFeedback(target, false);
            event->ignore();
            return true;
        }
        event->setDropAction(dropActionForUrls(
            event->mimeData()->urls(), destination, event->modifiers()));
        event->accept();
        setDropFeedback(target, true);
        return true;
    }

    static void setDropFeedback(QWidget *target, bool active)
    {
        if (!target || target->property("dropActive").toBool() == active) {
            return;
        }
        target->setProperty("dropActive", active);
        target->style()->unpolish(target);
        target->style()->polish(target);
        target->update();
    }

    void registerTransferDropTarget(QWidget *target, const QUrl &url)
    {
        if (!target || !url.isValid()) {
            return;
        }
        target->setAcceptDrops(true);
        target->installEventFilter(this);
        m_transferDropTargets.insert(target, normalizedUrl(url));
        connect(target, &QObject::destroyed, this, [this, target] {
            m_transferDropTargets.remove(target);
        });
    }

    static void clearLayout(QLayout *layout)
    {
        while (QLayoutItem *item = layout->takeAt(0)) {
            if (QWidget *widget = item->widget()) {
                widget->deleteLater();
            } else if (QLayout *child = item->layout()) {
                clearLayout(child);
                delete child;
            }
            delete item;
        }
    }

    static QStringList encodedUrlList(const QList<QUrl> &urls)
    {
        QStringList values;
        values.reserve(urls.size());
        for (const QUrl &url : urls) {
            values.push_back(url.toString(QUrl::FullyEncoded));
        }
        return values;
    }

    void loadPersistentState()
    {
        QSettings settings;

        auto decodeUnique = [](const QStringList &values) {
            QList<QUrl> urls;
            for (const QString &value : values) {
                const QUrl url = normalizedUrl(QUrl(value));
                if (!url.isValid()) {
                    continue;
                }

                bool duplicate = false;
                for (const QUrl &existing : std::as_const(urls)) {
                    if (sameLocation(existing, url)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    urls.push_back(url);
                }
            }
            return urls;
        };

        m_quickAccessUrls = decodeUnique(
            settings.value(QStringLiteral("quickAccess/favorites"))
                .toStringList());
        m_recentLocationUrls = decodeUnique(
            settings.value(QStringLiteral("quickAccess/recentLocations"))
                .toStringList());

        while (m_recentLocationUrls.size() > 10) {
            m_recentLocationUrls.removeLast();
        }
    }

    void saveQuickAccessUrls()
    {
        QSettings settings;
        settings.setValue(
            QStringLiteral("quickAccess/favorites"),
            encodedUrlList(m_quickAccessUrls));
    }

    void saveRecentLocations()
    {
        QSettings settings;
        settings.setValue(
            QStringLiteral("quickAccess/recentLocations"),
            encodedUrlList(m_recentLocationUrls));
    }

    void buildSections()
    {
        auto *quickAccessSection = new CollapsibleSection(
            trLocal("Szybki dostęp", "Quick access"),
            QStringLiteral("quickAccess"),
            this);
        m_layout->addWidget(quickAccessSection);
        m_quickAccessLayout = quickAccessSection->contentLayout();
        rebuildQuickAccess();

        auto *recentSection = new CollapsibleSection(
            trLocal("Ostatnie", "Recent"),
            QStringLiteral("recent"),
            this);
        m_layout->addWidget(recentSection);
        m_recentLocationsLayout = recentSection->contentLayout();
        rebuildRecentLocations();

        auto *placesSection = new CollapsibleSection(
            trLocal("Miejsca", "Places"),
            QStringLiteral("places"),
            this);
        m_layout->addWidget(placesSection);

        m_thisPcButton = addSidebarLocation(
            placesSection->contentLayout(),
            trLocal("Ten komputer", "This PC"),
            QStringLiteral("computer"),
            kThisPcUrl);

        const QString home = QDir::homePath();
        addSidebarLocation(
            placesSection->contentLayout(),
            trLocal("Katalog domowy", "Home"),
            QStringLiteral("user-home"),
            QUrl::fromLocalFile(home),
            true);
        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::DesktopLocation,
            trLocal("Pulpit", "Desktop"),
            QStringLiteral("folder"));
        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::DocumentsLocation,
            trLocal("Dokumenty", "Documents"),
            QStringLiteral("folder-documents"));
        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::DownloadLocation,
            trLocal("Pobrane", "Downloads"),
            QStringLiteral("folder-download"));
        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::MusicLocation,
            trLocal("Muzyka", "Music"),
            QStringLiteral("folder-music"));
        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::PicturesLocation,
            trLocal("Obrazy", "Pictures"),
            QStringLiteral("folder-pictures"));
        addStandardSidebarLocation(
            placesSection->contentLayout(),
            QStandardPaths::MoviesLocation,
            trLocal("Filmy", "Videos"),
            QStringLiteral("folder-videos"));

        const QString gamesPath =
            QDir(home).filePath(QStringLiteral("Games"));
        if (QFileInfo::exists(gamesPath)
            && QFileInfo(gamesPath).isDir()) {
            addSidebarLocation(
                placesSection->contentLayout(),
                QStringLiteral("Games"),
                QStringLiteral("applications-games"),
                QUrl::fromLocalFile(gamesPath),
                true);
        }

        addSidebarLocation(
            placesSection->contentLayout(),
            trLocal("Kosz", "Trash"),
            QStringLiteral("user-trash"),
            QUrl(QStringLiteral("trash:/")));

        auto *remoteSection = new CollapsibleSection(
            trLocal("Zdalne", "Remote"),
            QStringLiteral("remote"),
            this);
        m_layout->addWidget(remoteSection);
        addSidebarLocation(
            remoteSection->contentLayout(),
            trLocal("Sieć", "Network"),
            QStringLiteral("network-workgroup"),
            QUrl(QStringLiteral("remote:/")));

        auto *devicesSection = new CollapsibleSection(
            trLocal("Urządzenia", "Devices"),
            QStringLiteral("devices"),
            this);
        m_layout->addWidget(devicesSection);
        m_devicesLayout = devicesSection->contentLayout();

        m_layout->addStretch(1);
    }

    SidebarButton *addSidebarLocation(QVBoxLayout *layout,
                                      const QString &name,
                                      const QString &iconName,
                                      const QUrl &url,
                                      bool transferDropTarget = false)
    {
        auto *button = new SidebarButton(name, iconName, url, this);
        connectSidebarButton(button);
        layout->addWidget(button);
        m_staticSidebarButtons.push_back(button);
        if (transferDropTarget) {
            registerTransferDropTarget(button, url);
        }
        return button;
    }

    void addStandardSidebarLocation(QVBoxLayout *layout,
                                    QStandardPaths::StandardLocation location,
                                    const QString &name,
                                    const QString &iconName)
    {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty()) {
            addSidebarLocation(
                layout,
                name,
                iconName,
                QUrl::fromLocalFile(path),
                true);
        }
    }

    void connectSidebarButton(SidebarButton *button)
    {
        connect(button, &SidebarButton::activated,
                this, &SidebarPanel::activated);
        connect(button, &SidebarButton::openInNewTabRequested,
                this, &SidebarPanel::openInNewTabRequested);
        connect(button, &SidebarButton::openInNewWindowRequested,
                this, &SidebarPanel::openInNewWindowRequested);
        connect(button, &SidebarButton::openInSplitPaneRequested,
                this, &SidebarPanel::openInSplitPaneRequested);
    }

    void rebuildQuickAccess()
    {
        if (!m_quickAccessLayout) {
            return;
        }

        clearLayout(m_quickAccessLayout);
        m_quickAccessButtons.clear();

        for (const QUrl &url : std::as_const(m_quickAccessUrls)) {
            auto *button = new QuickAccessSidebarButton(
                displayNameForLocation(url),
                QStringLiteral("folder-favorites"),
                url,
                this);
            connectSidebarButton(button);
            registerTransferDropTarget(button, url);

            connect(button,
                    &SidebarButton::removeFromQuickAccessRequested,
                    this,
                    &SidebarPanel::unpinQuickAccessLocation);
            connect(button,
                    &QuickAccessSidebarButton::moveRequested,
                    this,
                    &SidebarPanel::moveQuickAccessLocation);

            m_quickAccessLayout->addWidget(button);
            m_quickAccessButtons.push_back(button);
        }
    }

    void rebuildRecentLocations()
    {
        if (!m_recentLocationsLayout) {
            return;
        }

        clearLayout(m_recentLocationsLayout);
        for (const QUrl &url : std::as_const(m_recentLocationUrls)) {
            auto *button = new SidebarButton(
                displayNameForLocation(url),
                QStringLiteral("document-open-recent"),
                url,
                this);
            connectSidebarButton(button);
            m_recentLocationsLayout->addWidget(button);
        }
    }

    void rebuildDevices()
    {
        if (!m_devicesLayout) {
            return;
        }

        clearLayout(m_devicesLayout);
        m_driveSidebarButtons.clear();

        for (const DriveInfo &drive : std::as_const(m_drives)) {
            auto *button = new SidebarDriveButton(drive, this);

            connect(button, &SidebarDriveButton::activated,
                    this, &SidebarPanel::activated);
            connect(button, &SidebarDriveButton::openInNewTabRequested,
                    this, &SidebarPanel::openInNewTabRequested);
            connect(button, &SidebarDriveButton::openInNewWindowRequested,
                    this, &SidebarPanel::openInNewWindowRequested);
            connect(button, &SidebarDriveButton::openInSplitPaneRequested,
                    this, &SidebarPanel::openInSplitPaneRequested);
            connect(button, &SidebarDriveButton::openInDolphinRequested,
                    this, &SidebarPanel::openInDolphinRequested);
            registerTransferDropTarget(button, drive.targetUrl);

            m_devicesLayout->addWidget(button);
            m_driveSidebarButtons.push_back(button);
        }
    }

    void pinQuickAccessLocation(const QUrl &rawUrl)
    {
        if (!canQuickAccessLocation(rawUrl)
            || isQuickAccessPinned(rawUrl)) {
            return;
        }

        const int scrollPosition = verticalScrollPosition();
        m_quickAccessUrls.push_back(normalizedUrl(rawUrl));
        saveQuickAccessUrls();
        rebuildQuickAccess();
        updateCurrent();
        restoreVerticalScrollPosition(scrollPosition);
        Q_EMIT statusMessageRequested(
            trLocal("Przypięto folder do Szybkiego dostępu.",
                    "Folder pinned to Quick access."),
            3000);
    }

    void unpinQuickAccessLocation(const QUrl &rawUrl)
    {
        const QUrl url = normalizedUrl(rawUrl);
        for (int i = 0; i < m_quickAccessUrls.size(); ++i) {
            if (!sameLocation(m_quickAccessUrls.at(i), url)) {
                continue;
            }

            const int scrollPosition = verticalScrollPosition();
            m_quickAccessUrls.removeAt(i);
            saveQuickAccessUrls();
            rebuildQuickAccess();
            updateCurrent();
            restoreVerticalScrollPosition(scrollPosition);
            Q_EMIT statusMessageRequested(
                trLocal("Odpięto folder od Szybkiego dostępu.",
                        "Folder unpinned from Quick access."),
                3000);
            return;
        }
    }

    void moveQuickAccessLocation(const QUrl &rawSource,
                                 const QUrl &rawTarget,
                                 bool insertAfter)
    {
        const int scrollPosition = verticalScrollPosition();
        const QUrl source = normalizedUrl(rawSource);
        const QUrl target = normalizedUrl(rawTarget);

        int sourceIndex = -1;
        int targetIndex = -1;
        for (int i = 0; i < m_quickAccessUrls.size(); ++i) {
            if (sameLocation(m_quickAccessUrls.at(i), source)) {
                sourceIndex = i;
            }
            if (sameLocation(m_quickAccessUrls.at(i), target)) {
                targetIndex = i;
            }
        }

        if (sourceIndex < 0
            || targetIndex < 0
            || sourceIndex == targetIndex) {
            return;
        }

        const QUrl moved = m_quickAccessUrls.takeAt(sourceIndex);
        if (sourceIndex < targetIndex) {
            --targetIndex;
        }
        if (insertAfter) {
            ++targetIndex;
        }

        targetIndex = std::clamp(
            targetIndex,
            0,
            static_cast<int>(m_quickAccessUrls.size()));
        m_quickAccessUrls.insert(targetIndex, moved);

        saveQuickAccessUrls();
        rebuildQuickAccess();
        updateCurrent();
        restoreVerticalScrollPosition(scrollPosition);
    }

    QString displayNameForLocation(const QUrl &url) const
    {
        for (const DriveInfo &drive : m_drives) {
            if (sameLocation(drive.targetUrl, url)) {
                return drive.name;
            }
        }

        if (url.isLocalFile()) {
            const QString path = url.toLocalFile();
            if (path == QDir::homePath()) {
                return trLocal("Katalog domowy", "Home");
            }

            const QString fileName = QFileInfo(path).fileName();
            return fileName.isEmpty() ? path : fileName;
        }

        if (url.scheme() == QStringLiteral("trash")) {
            return trLocal("Kosz", "Trash");
        }
        if (url.scheme() == QStringLiteral("remote")) {
            return trLocal("Sieć", "Network");
        }

        const QString last = QFileInfo(url.path()).fileName();
        return last.isEmpty() ? url.toDisplayString() : last;
    }

    void updateCurrent()
    {
        SidebarButton *bestStatic = nullptr;
        SidebarDriveButton *bestDrive = nullptr;
        int bestDepth = -1;

        for (QuickAccessSidebarButton *button :
             std::as_const(m_quickAccessButtons)) {
            button->setCurrent(false);
            if (button->url().isValid()
                && isWithinLocation(m_currentLocation, button->url())) {
                const int depth = locationDepth(button->url());
                if (depth > bestDepth) {
                    bestDepth = depth;
                    bestStatic = button;
                    bestDrive = nullptr;
                }
            }
        }

        for (SidebarButton *button :
             std::as_const(m_staticSidebarButtons)) {
            button->setCurrent(false);
            if (button->url().isValid()
                && isWithinLocation(m_currentLocation, button->url())) {
                const int depth = locationDepth(button->url());
                if (depth > bestDepth) {
                    bestDepth = depth;
                    bestStatic = button;
                    bestDrive = nullptr;
                }
            }
        }

        for (SidebarDriveButton *button :
             std::as_const(m_driveSidebarButtons)) {
            button->setCurrent(false);
            if (isWithinLocation(m_currentLocation, button->url())) {
                const int depth = locationDepth(button->url());
                if (depth > bestDepth) {
                    bestDepth = depth;
                    bestDrive = button;
                    bestStatic = nullptr;
                }
            }
        }

        if (sameLocation(m_currentLocation, kThisPcUrl)) {
            if (m_thisPcButton) {
                m_thisPcButton->setCurrent(true);
            }
            return;
        }

        if (bestStatic) {
            bestStatic->setCurrent(true);
        } else if (bestDrive) {
            bestDrive->setCurrent(true);
        }
    }

    QVBoxLayout *m_layout = nullptr;
    QVBoxLayout *m_quickAccessLayout = nullptr;
    QVBoxLayout *m_recentLocationsLayout = nullptr;
    QVBoxLayout *m_devicesLayout = nullptr;

    QList<QUrl> m_quickAccessUrls;
    QList<QUrl> m_recentLocationUrls;
    QList<DriveInfo> m_drives;
    QUrl m_currentLocation = kThisPcUrl;

    QList<QuickAccessSidebarButton *> m_quickAccessButtons;
    QList<SidebarButton *> m_staticSidebarButtons;
    QList<SidebarDriveButton *> m_driveSidebarButtons;
    SidebarButton *m_thisPcButton = nullptr;
    QHash<QObject *, QUrl> m_transferDropTargets;
};
