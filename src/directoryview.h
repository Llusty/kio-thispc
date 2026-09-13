/*
 * Directory view widgets shared by the primary and split panes.
 *
 * Extracted during the 0.21.0 architecture refactor. Keep behavior changes
 * separate from structural moves so regressions are easy to isolate.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>

#include <functional>
#include <utility>

namespace directory_view_detail
{
inline QString existingPathForStorage(const QString &rawPath)
{
    QFileInfo info(rawPath);
    if (info.exists()) {
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) {
            return canonical;
        }
    }

    QDir parent = info.dir();
    while (!parent.exists() && parent.cdUp()) {
    }
    const QString canonicalParent = parent.exists()
        ? QFileInfo(parent.absolutePath()).canonicalFilePath()
        : QString();
    return canonicalParent.isEmpty()
        ? QDir::cleanPath(info.absolutePath())
        : QDir(canonicalParent).filePath(info.fileName());
}

inline bool localPathsShareStorage(
    const QString &sourcePath,
    const QString &destinationPath)
{
    const QString sourceStoragePath = existingPathForStorage(sourcePath);
    const QString destinationStoragePath = existingPathForStorage(destinationPath);
    QStorageInfo sourceStorage(sourceStoragePath);
    QStorageInfo destinationStorage(destinationStoragePath);
    if (!sourceStorage.isValid() || !destinationStorage.isValid()) {
        return false;
    }

    const QString sourceRoot =
        QDir::cleanPath(sourceStorage.rootPath());
    const QString destinationRoot =
        QDir::cleanPath(destinationStorage.rootPath());

    if (!sourceStorage.device().isEmpty()
        && !destinationStorage.device().isEmpty()
        && sourceStorage.device() != destinationStorage.device()) {
        return false;
    }

    // Be conservative around bind mounts/subvolumes: an identical device
    // name alone does not guarantee that a rename-style move can cross
    // the two mounted roots. Copy is the safe default in that case.
    return sourceRoot == destinationRoot;
}

} // namespace directory_view_detail

inline Qt::DropAction dropActionForUrls(
    const QList<QUrl> &urls,
    const QUrl &destination,
    Qt::KeyboardModifiers modifiers)
{
    if (modifiers.testFlag(Qt::ControlModifier)) {
        return Qt::CopyAction;
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        return Qt::MoveAction;
    }

    if (!destination.isLocalFile() || urls.isEmpty()) {
        return Qt::CopyAction;
    }
    const QString destinationPath = destination.toLocalFile();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()
            || !directory_view_detail::localPathsShareStorage(
                url.toLocalFile(), destinationPath)) {
            return Qt::CopyAction;
        }
    }
    return Qt::MoveAction;
}

// QListView::setMovement() and setViewMode() reset drag/drop settings.
// Apply this after layout configuration, keeping Static item positioning.
inline void configureDirectoryDragDrop(QAbstractItemView *view)
{
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->viewport()->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDefaultDropAction(Qt::CopyAction);
}

class ExplorerNameDelegate : public QStyledItemDelegate
{
public:
    explicit ExplorerNameDelegate(QListView *view)
        : QStyledItemDelegate(view)
        , m_view(view)
    {
    }

    void setAlwaysShowFullNames(bool enabled)
    {
        if (m_alwaysShowFullNames == enabled) {
            return;
        }
        m_alwaysShowFullNames = enabled;
        if (m_view) {
            m_view->doItemsLayout();
            m_view->viewport()->update();
        }
    }

    bool alwaysShowFullNames() const
    {
        return m_alwaysShowFullNames;
    }

    void paint(
        QPainter *painter,
        const QStyleOptionViewItem &option,
        const QModelIndex &index) const override
    {
        if (!m_view || m_view->viewMode() != QListView::IconMode) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        const QString fullText = opt.text;
        opt.text.clear();

        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();

        // 1. Draw cell background, selection/hover highlight, focus rect and decoration icon
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        // 2. Determine text area
        QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        if (!textRect.isValid() || textRect.isEmpty()) {
            const int iconBottom = opt.rect.top() + opt.decorationSize.height() + 4;
            textRect = QRect(opt.rect.left() + 4, iconBottom, opt.rect.width() - 8, opt.rect.bottom() - iconBottom);
        }

        if (fullText.isEmpty() || textRect.width() <= 0 || textRect.height() <= 0) {
            return;
        }

        // 3. Layout text with QTextLayout using WrapAtWordBoundaryOrAnywhere
        const int maxLines = m_alwaysShowFullNames ? 4 : 2;
        const QFontMetrics fm(opt.font);

        QTextLayout layout(fullText, opt.font);
        QTextOption textOpt;
        textOpt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        textOpt.setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        layout.setTextOption(textOpt);

        layout.beginLayout();
        struct LineDrawInfo {
            QString text;
            bool isElided = false;
        };
        QList<LineDrawInfo> linesToDraw;

        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(textRect.width());

            if (linesToDraw.size() + 1 == maxLines) {
                const int textStart = line.textStart();
                const bool hasMore = (textStart + line.textLength()) < fullText.length();
                if (hasMore) {
                    const QString remainder = fullText.mid(textStart);
                    const QString elided = fm.elidedText(remainder, Qt::ElideRight, textRect.width());
                    linesToDraw.append({elided, true});
                } else {
                    linesToDraw.append({fullText.mid(textStart, line.textLength()), false});
                }
                break;
            } else {
                linesToDraw.append({fullText.mid(line.textStart(), line.textLength()), false});
            }
        }
        layout.endLayout();

        // 4. Draw text lines
        const QPalette::ColorGroup cg = (opt.state & QStyle::State_Enabled)
            ? ((opt.state & QStyle::State_Active) ? QPalette::Active : QPalette::Inactive)
            : QPalette::Disabled;
        const QPalette::ColorRole role = (opt.state & QStyle::State_Selected)
            ? QPalette::HighlightedText
            : QPalette::Text;
        const QColor textColor = opt.palette.color(cg, role);

        painter->save();
        painter->setFont(opt.font);
        painter->setPen(textColor);

        const int lineHeight = fm.lineSpacing();
        for (int i = 0; i < linesToDraw.size(); ++i) {
            const auto &info = linesToDraw.at(i);
            const QRect lineRect(
                textRect.left(),
                textRect.top() + i * lineHeight,
                textRect.width(),
                lineHeight);
            painter->drawText(lineRect, Qt::AlignHCenter | Qt::AlignVCenter, info.text);
        }
        painter->restore();
    }

    QSize sizeHint(
        const QStyleOptionViewItem &option,
        const QModelIndex &index) const override
    {
        if (!m_view) {
            return QStyledItemDelegate::sizeHint(option, index);
        }

        if (m_view->viewMode() == QListView::ListMode) {
            QStyleOptionViewItem adjusted(option);
            initStyleOption(&adjusted, index);
            const QFontMetrics metrics(adjusted.font);
            const int width = qMax(180, m_view->viewport()->width() - 16);
            const int height = qMax(32, metrics.lineSpacing() + 10);
            return QSize(width, height);
        }

        constexpr int itemWidth = 136;
        constexpr int compactHeight = 116;
        constexpr int fullHeight = 150;
        return QSize(
            itemWidth,
            m_alwaysShowFullNames ? fullHeight : compactHeight);
    }

private:
    QListView *m_view = nullptr;
    bool m_alwaysShowFullNames = false;
};

class DirectoryListWidget : public QListWidget
{
    Q_OBJECT

public:
    explicit DirectoryListWidget(QWidget *parent = nullptr)
        : QListWidget(parent)
    {
        m_nameDelegate = new ExplorerNameDelegate(this);
        setItemDelegate(m_nameDelegate);
        setTextElideMode(Qt::ElideRight);
        setWordWrap(true);
        updateGridGeometry();

        connect(
            this,
            &QListWidget::itemSelectionChanged,
            this,
            [this] {
                viewport()->update();
            });
        connect(
            this,
            &QListWidget::currentItemChanged,
            this,
            [this] {
                viewport()->update();
            });
    }

    void updateGridGeometry()
    {
        if (viewMode() == QListView::IconMode) {
            constexpr int itemWidth = 136;
            constexpr int compactHeight = 116;
            constexpr int fullHeight = 150;
            const int itemHeight = alwaysShowFullNames() ? fullHeight : compactHeight;
            setGridSize(QSize(itemWidth, itemHeight));
            setUniformItemSizes(true);
        } else {
            setGridSize(QSize());
            setUniformItemSizes(false);
        }
    }

    void setAlwaysShowFullNames(bool enabled)
    {
        if (m_nameDelegate) {
            m_nameDelegate->setAlwaysShowFullNames(enabled);
        }
        updateGridGeometry();
        viewport()->update();
    }

    bool alwaysShowFullNames() const
    {
        return m_nameDelegate
            && m_nameDelegate->alwaysShowFullNames();
    }

    void setDropDirectory(const QUrl &url)
    {
        m_dropDirectory = url;
    }

Q_SIGNALS:
    void urlsDropped(
        const QList<QUrl> &urls,
        const QUrl &destination,
        const QPoint &globalPosition,
        Qt::KeyboardModifiers modifiers);

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QListWidget::paintEvent(event);

        if (viewMode() != QListView::IconMode) {
            return;
        }

        if (state() == QAbstractItemView::EditingState) {
            return;
        }

        QListWidgetItem *item = currentItem();
        if (!item || !item->isSelected()) {
            return;
        }

        const QRect itemRect = visualItemRect(item);
        if (!itemRect.isValid() || !viewport()->rect().intersects(itemRect)) {
            return;
        }

        const QString text = item->text();
        if (text.isEmpty()) {
            return;
        }

        const int maxGridLines = alwaysShowFullNames() ? 4 : 2;
        const int calloutWidth = qMax(124, itemRect.width() - 4);
        const int innerWidth = calloutWidth - 12;

        QTextLayout layout(text, font());
        QTextOption opt;
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        opt.setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        layout.setTextOption(opt);

        layout.beginLayout();
        qreal textHeight = 0;
        int lineCount = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(innerWidth);
            line.setPosition(QPointF(6, textHeight));
            textHeight += line.height();
            lineCount++;
        }
        layout.endLayout();

        const bool needsCallout = (lineCount > maxGridLines)
            || (lineCount > 1)
            || (fontMetrics().horizontalAdvance(text) > innerWidth);
        if (!needsCallout) {
            return;
        }

        int calloutX = itemRect.center().x() - calloutWidth / 2;
        calloutX = qBound(2, calloutX, qMax(2, viewport()->width() - calloutWidth - 2));

        const int iconBottom = itemRect.top() + iconSize().height() + 4;
        const int calloutHeight = qRound(textHeight) + 8;
        int calloutY = qBound(2, iconBottom, qMax(2, viewport()->height() - calloutHeight - 2));

        const QRect calloutRect(calloutX, calloutY, calloutWidth, calloutHeight);

        QPainter painter(viewport());
        if (!painter.isActive()) {
            return;
        }
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QPalette pal = palette();
        const QColor bgColor = pal.color(QPalette::Base);
        const QColor borderColor = pal.color(QPalette::Highlight);
        const QColor textColor = pal.color(QPalette::Text);

        painter.setPen(QPen(borderColor, 1.2));
        painter.setBrush(bgColor);
        painter.drawRoundedRect(
            QRectF(calloutRect).adjusted(0.5, 0.5, -0.5, -0.5),
            4.0,
            4.0);

        painter.setPen(textColor);
        layout.draw(&painter, QPointF(calloutRect.left(), calloutRect.top() + 4));
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions)

        QList<QUrl> urls;
        for (QListWidgetItem *item : selectedItems()) {
            const QUrl url(item->data(Qt::UserRole).toString());
            if (url.isValid()) {
                urls.push_back(url);
            }
        }

        if (urls.isEmpty()) {
            return;
        }

        auto *mimeData = new QMimeData;
        mimeData->setUrls(urls);

        QDrag drag(this);
        drag.setMimeData(mimeData);
        drag.exec(
            Qt::CopyAction | Qt::MoveAction,
            Qt::CopyAction);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            const QUrl destination =
                dropDestinationAt(event->position().toPoint());
            if (!destination.isValid()) {
                event->ignore();
                return;
            }
            event->setDropAction(
                dropActionForUrls(
                    event->mimeData()->urls(),
                    destination,
                    event->modifiers()));
            event->accept();
            return;
        }
        QListWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            const QUrl destination =
                dropDestinationAt(event->position().toPoint());
            if (!destination.isValid()) {
                event->ignore();
                return;
            }
            event->setDropAction(
                dropActionForUrls(
                    event->mimeData()->urls(),
                    destination,
                    event->modifiers()));
            event->accept();
            return;
        }
        QListWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event->mimeData()->hasUrls()) {
            QListWidget::dropEvent(event);
            return;
        }

        const QUrl destination =
            dropDestinationAt(event->position().toPoint());
        if (!destination.isValid()) {
            event->ignore();
            return;
        }

        const QPoint globalPosition =
            viewport()->mapToGlobal(event->position().toPoint());

        Q_EMIT urlsDropped(
            event->mimeData()->urls(),
            destination,
            globalPosition,
            event->modifiers());

        event->setDropAction(
            dropActionForUrls(
                event->mimeData()->urls(),
                destination,
                event->modifiers()));
        event->accept();
    }

    void scrollContentsBy(int dx, int dy) override
    {
        QListWidget::scrollContentsBy(dx, dy);
        viewport()->update();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QListWidget::resizeEvent(event);
        viewport()->update();
    }

    void showEvent(QShowEvent *event) override
    {
        QListWidget::showEvent(event);
        viewport()->update();
    }

private:
    QUrl dropDestinationAt(const QPoint &position) const
    {
        QUrl destination = m_dropDirectory;
        QListWidgetItem *target = itemAt(position);
        if (target
            && target->data(Qt::UserRole + 1).toBool()) {
            const QUrl targetUrl(
                target->data(Qt::UserRole).toString());
            if (targetUrl.isValid()) {
                destination = targetUrl;
            }
        }
        return destination;
    }

    QUrl m_dropDirectory;
    ExplorerNameDelegate *m_nameDelegate = nullptr;
};

class DirectoryTreeWidget : public QTreeWidget
{
    Q_OBJECT

public:
    explicit DirectoryTreeWidget(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
    }

    void setDropDirectory(const QUrl &url)
    {
        m_dropDirectory = url;
    }

Q_SIGNALS:
    void urlsDropped(
        const QList<QUrl> &urls,
        const QUrl &destination,
        const QPoint &globalPosition,
        Qt::KeyboardModifiers modifiers);

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions)

        QList<QUrl> urls;
        for (QTreeWidgetItem *item : selectedItems()) {
            const QUrl url(
                item->data(0, Qt::UserRole).toString());
            if (url.isValid()) {
                urls.push_back(url);
            }
        }

        if (urls.isEmpty()) {
            return;
        }

        auto *mimeData = new QMimeData;
        mimeData->setUrls(urls);

        QDrag drag(this);
        drag.setMimeData(mimeData);
        drag.exec(
            Qt::CopyAction | Qt::MoveAction,
            Qt::CopyAction);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            const QUrl destination =
                dropDestinationAt(event->position().toPoint());
            if (!destination.isValid()) {
                event->ignore();
                return;
            }
            event->setDropAction(
                dropActionForUrls(
                    event->mimeData()->urls(),
                    destination,
                    event->modifiers()));
            event->accept();
            return;
        }
        QTreeWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            const QUrl destination =
                dropDestinationAt(event->position().toPoint());
            if (!destination.isValid()) {
                event->ignore();
                return;
            }
            event->setDropAction(
                dropActionForUrls(
                    event->mimeData()->urls(),
                    destination,
                    event->modifiers()));
            event->accept();
            return;
        }
        QTreeWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event->mimeData()->hasUrls()) {
            QTreeWidget::dropEvent(event);
            return;
        }

        const QUrl destination =
            dropDestinationAt(event->position().toPoint());
        if (!destination.isValid()) {
            event->ignore();
            return;
        }

        const QPoint globalPosition =
            viewport()->mapToGlobal(
                event->position().toPoint());

        Q_EMIT urlsDropped(
            event->mimeData()->urls(),
            destination,
            globalPosition,
            event->modifiers());

        event->setDropAction(
            dropActionForUrls(
                event->mimeData()->urls(),
                destination,
                event->modifiers()));
        event->accept();
    }

private:
    QUrl dropDestinationAt(const QPoint &position) const
    {
        QUrl destination = m_dropDirectory;
        QTreeWidgetItem *target = itemAt(position);
        if (target
            && target->data(
                0,
                Qt::UserRole + 1).toBool()) {
            const QUrl targetUrl(
                target->data(
                    0,
                    Qt::UserRole).toString());
            if (targetUrl.isValid()) {
                destination = targetUrl;
            }
        }
        return destination;
    }

    QUrl m_dropDirectory;
};


// File drags use the normal QWidget event path; tab reordering stays in QTabBar.
class ExplorerTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit ExplorerTabBar(QWidget *parent = nullptr)
        : QTabBar(parent)
    {
        setAcceptDrops(true);
        m_hoverTimer.setSingleShot(true);
        m_hoverTimer.setInterval(650);
        connect(&m_hoverTimer, &QTimer::timeout, this, [this] {
            const int index = m_hoverTab;
            const bool valid = index >= 0 && tabAt(m_hoverPosition) == index
                && isTabEnabled(index) && isTabVisible(index)
                && m_dropDirectory && m_dropDirectory(index).isValid();
            cancelHover();
            if (valid) setCurrentIndex(index);
        });
        connect(this, &QTabBar::tabMoved, this, [this] { cancelHover(); });
    }

    void setDropDirectoryResolver(std::function<QUrl(int)> resolver)
    {
        m_dropDirectory = std::move(resolver);
    }

Q_SIGNALS:
    void urlsDropped(const QList<QUrl> &urls, const QUrl &destination,
                     const QPoint &globalPosition,
                     Qt::KeyboardModifiers modifiers);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        updateDrag(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        updateDrag(event);
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        cancelHover();
        event->accept();
    }

    void dropEvent(QDropEvent *event) override
    {
        const QUrl destination = dropDestination(event);
        cancelHover();
        if (!destination.isValid()) {
            event->ignore();
            return;
        }
        const QList<QUrl> urls = event->mimeData()->urls();
        const QPoint globalPosition = mapToGlobal(event->position().toPoint());
        event->setDropAction(
            dropActionForUrls(urls, destination, event->modifiers()));
        event->accept();
        Q_EMIT urlsDropped(
            urls,
            destination,
            globalPosition,
            event->modifiers());
    }

    void tabInserted(int index) override
    {
        cancelHover();
        QTabBar::tabInserted(index);
    }

    void tabRemoved(int index) override
    {
        cancelHover();
        QTabBar::tabRemoved(index);
    }

private:
    QUrl dropDestination(const QDropEvent *event) const
    {
        if (!event->mimeData()->hasUrls() || event->mimeData()->urls().isEmpty()
            || event->proposedAction() == Qt::IgnoreAction || !m_dropDirectory) {
            return {};
        }
        const int index = tabAt(event->position().toPoint());
        if (index < 0 || !isTabEnabled(index) || !isTabVisible(index)) {
            return {};
        }
        return m_dropDirectory(index);
    }

    void updateDrag(QDragMoveEvent *event)
    {
        if (!dropDestination(event).isValid()) {
            cancelHover();
            event->ignore();
            return;
        }
        const QPoint position = event->position().toPoint();
        const int index = tabAt(position);
        if (index == currentIndex()) {
            cancelHover();
        } else if (index != m_hoverTab) {
            cancelHover();
            m_hoverTab = index;
            m_hoverTimer.start();
        }
        m_hoverPosition = position;
        event->setDropAction(
            dropActionForUrls(
                event->mimeData()->urls(),
                dropDestination(event),
                event->modifiers()));
        event->accept();
    }

    void cancelHover()
    {
        m_hoverTimer.stop();
        m_hoverTab = -1;
    }

    std::function<QUrl(int)> m_dropDirectory;
    QTimer m_hoverTimer;
    int m_hoverTab = -1;
    QPoint m_hoverPosition;
};

inline void applyDirectoryViewLayout(
    DirectoryListWidget *list,
    DirectoryTreeWidget *details,
    QStackedWidget *stack,
    QToolButton *viewButton,
    int viewMode)
{
    if (!list || !details || !stack) {
        return;
    }

    if (viewMode == 0) {
        list->setViewMode(QListView::IconMode);
        list->setFlow(QListView::LeftToRight);
        list->setWrapping(true);
        list->setIconSize(QSize(64, 64));
        list->setSpacing(3);
        list->updateGridGeometry();
        stack->setCurrentWidget(list);

        if (viewButton) {
            viewButton->setIcon(
                themedIcon(QStringLiteral("view-list-icons")));
        }
    } else if (viewMode == 1) {
        list->setViewMode(QListView::ListMode);
        list->setFlow(QListView::TopToBottom);
        list->setWrapping(false);
        list->setIconSize(QSize(24, 24));
        list->setSpacing(1);
        list->updateGridGeometry();
        stack->setCurrentWidget(list);

        if (viewButton) {
            viewButton->setIcon(
                themedIcon(QStringLiteral("view-list-text")));
        }
    } else {
        stack->setCurrentWidget(details);

        if (viewButton) {
            viewButton->setIcon(
                themedIcon(QStringLiteral("view-list-details")));
        }
    }

    // QListView layout changes reset parts of the drag/drop configuration.
    configureDirectoryDragDrop(list);
    configureDirectoryDragDrop(details);
}


inline void addDirectoryFileItems(
    DirectoryListWidget *list,
    DirectoryTreeWidget *details,
    const FileInfo &file,
    const QIcon &icon,
    const QString &typeText,
    const QString &sizeText,
    const QString &modifiedText,
    const QStringList &extraDetailColumns = {})
{
    auto *listItem = new QListWidgetItem(
        icon,
        file.name,
        list);

    listItem->setData(
        Qt::UserRole,
        file.url.toString());
    listItem->setData(
        Qt::UserRole + 1,
        file.isDir);
    listItem->setData(
        Qt::UserRole + 2,
        typeText);
    listItem->setData(
        Qt::UserRole + 3,
        sizeText);
    listItem->setData(
        Qt::UserRole + 4,
        modifiedText);
    listItem->setToolTip(
        QStringLiteral("%1\n%2\n%3")
            .arg(
                urlForDisplay(file.url),
                typeText,
                modifiedText));

    QStringList columns{
        file.name,
        typeText,
        sizeText,
        modifiedText
    };
    columns.append(extraDetailColumns);

    auto *detailsItem = new QTreeWidgetItem(
        details,
        columns);

    detailsItem->setIcon(0, icon);
    detailsItem->setData(
        0,
        Qt::UserRole,
        file.url.toString());
    detailsItem->setData(
        0,
        Qt::UserRole + 1,
        file.isDir);
    detailsItem->setToolTip(
        0,
        urlForDisplay(file.url));
    detailsItem->setTextAlignment(
        2,
        Qt::AlignRight | Qt::AlignVCenter);
}
