/*
 * Directory view widgets shared by the primary and split panes.
 *
 * Extracted during the 0.21.0 architecture refactor. Keep behavior changes
 * separate from structural moves so regressions are easy to isolate.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <KCategorizedSortFilterProxyModel>
#include <KCategorizedView>
#include <KCategoryDrawer>

#include <QAbstractItemView>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QStandardItemModel>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSet>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QTimeZone>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>

#include <functional>
#include <limits>
#include <utility>

namespace directory_view_detail
{
enum ItemRole {
    UrlRole = Qt::UserRole,
    DirectoryRole,
    TypeTextRole,
    SizeTextRole,
    ModifiedTextRole,
    FileItemRole,
    OriginalOrderRole,
};

struct DateCategory
{
    QString display;
    QString sortKey;
};

// Size is the non-recursive UDS_SIZE reported for this entry by KIO.
// Boundaries are binary multiples and lower-inclusive, upper-exclusive.
// Directories are separate regardless of a worker's reported size.
inline DateCategory sizeCategoryForFile(const FileInfo &file)
{
    if (file.isDir)
        return {trLocal("Foldery", "Folders"), QStringLiteral("00")};
    if (file.size < 0)
        return {trLocal("Nieznany rozmiar", "Unknown size"), QStringLiteral("90")};
    if (file.size == 0)
        return {trLocal("Puste (0 B)", "Empty (0 B)"), QStringLiteral("10")};
    if (file.size < 1024)
        return {trLocal("1 B – 1023 B", "1 B – 1023 B"), QStringLiteral("20")};
    if (file.size < 1024LL * 1024)
        return {trLocal("1 KiB – poniżej 1 MiB", "1 KiB – under 1 MiB"), QStringLiteral("30")};
    if (file.size < 1024LL * 1024 * 1024)
        return {trLocal("1 MiB – poniżej 1 GiB", "1 MiB – under 1 GiB"), QStringLiteral("40")};
    if (file.size < 1024LL * 1024 * 1024 * 1024)
        return {trLocal("1 GiB – poniżej 1 TiB", "1 GiB – under 1 TiB"), QStringLiteral("50")};
    return {trLocal("1 TiB i więcej", "1 TiB and larger"), QStringLiteral("60")};
}

// KIO exposes modification time as epoch seconds. Bucket it only after
// conversion to the system's local calendar. Today/Yesterday take precedence
// over ISO-week ranges, which keeps all ranges disjoint around Mondays.
inline DateCategory dateCategoryForModification(
    qint64 seconds,
    const QDateTime &reference = QDateTime::currentDateTime())
{
    if (seconds <= 0 || !reference.isValid()) {
        return {trLocal("Nieznana data", "Unknown date"), QStringLiteral("90")};
    }
    const QTimeZone zone = reference.timeZone().isValid()
        ? reference.timeZone() : QTimeZone::systemTimeZone();
    const QDate date = QDateTime::fromSecsSinceEpoch(seconds, zone).date();
    const QDate today = reference.toTimeZone(zone).date();
    if (!date.isValid() || !today.isValid()) {
        return {trLocal("Nieznana data", "Unknown date"), QStringLiteral("90")};
    }
    if (date > today)
        return {trLocal("Przyszłe", "Future"), QStringLiteral("00")};
    if (date == today)
        return {trLocal("Dzisiaj", "Today"), QStringLiteral("10")};
    if (date == today.addDays(-1))
        return {trLocal("Wczoraj", "Yesterday"), QStringLiteral("20")};

    const QDate weekStart = today.addDays(1 - today.dayOfWeek());
    if (date >= weekStart)
        return {trLocal("Ten tydzień", "This week"), QStringLiteral("30")};
    if (date >= weekStart.addDays(-7))
        return {trLocal("Ostatni tydzień", "Last week"), QStringLiteral("40")};
    if (date.year() == today.year() && date.month() == today.month())
        return {trLocal("Wcześniej w tym miesiącu", "Earlier this month"), QStringLiteral("50")};
    if (date.year() == today.year())
        return {trLocal("Wcześniej w tym roku", "Earlier this year"), QStringLiteral("60")};
    return {trLocal("Starsze", "Older"), QStringLiteral("70")};
}

inline int millisecondsUntilNextLocalDay(
    const QDateTime &reference = QDateTime::currentDateTime())
{
    const QTimeZone zone = reference.timeZone().isValid()
        ? reference.timeZone() : QTimeZone::systemTimeZone();
    const QDateTime local = reference.toTimeZone(zone);
    if (!local.isValid()) return 60 * 1000;
    const QDateTime next = local.date().addDays(1).startOfDay(zone);
    if (!next.isValid()) return 60 * 1000;
    return int(qBound<qint64>(qint64(1000), local.msecsTo(next) + 1000,
                              qint64(std::numeric_limits<int>::max())));
}

class StableCategoryProxy final : public KCategorizedSortFilterProxyModel
{
public:
    explicit StableCategoryProxy(QObject *parent = nullptr)
        : KCategorizedSortFilterProxyModel(parent)
    {
    }

protected:
    bool subSortLessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        return left.data(OriginalOrderRole).toInt()
            < right.data(OriginalOrderRole).toInt();
    }
};

inline int iconExtentForMode(int mode)
{
    static constexpr int extents[] = {96, 64, 48, 32};
    return extents[std::clamp(mode, 0, 3)];
}

inline QSize iconGridSize(int iconExtent, bool fullNames)
{
    const int width = qMax(104, iconExtent + 72);
    return QSize(width, iconExtent + (fullNames ? 86 : 52));
}

inline QColor blendedSelectionColor(
    const QPalette &palette,
    QPalette::ColorGroup group,
    int highlightPercent)
{
    const QColor base = palette.color(group, QPalette::Base);
    const QColor highlight = palette.color(group, QPalette::Highlight);
    const int amount = qBound(0, highlightPercent, 100);
    return QColor(
        (base.red() * (100 - amount) + highlight.red() * amount) / 100,
        (base.green() * (100 - amount) + highlight.green() * amount) / 100,
        (base.blue() * (100 - amount) + highlight.blue() * amount) / 100,
        255);
}

inline void synchronizeIconItemState(
    QStyle::State &state,
    bool selected,
    bool hovered)
{
    state.setFlag(QStyle::State_Selected, false);
    state.setFlag(QStyle::State_MouseOver, hovered && !selected);
    // The current index can outlive its selection after a background click.
    // Keeping State_HasFocus in that state makes Breeze draw a label-only
    // focus rectangle which looks like a stale selection outline.
    state.setFlag(QStyle::State_HasFocus, selected && state.testFlag(QStyle::State_HasFocus));
}

inline QRect selectedNameCalloutRect(
    const QRect &itemRect,
    int iconExtent,
    int textHeight,
    const QRect &viewportRect)
{
    const int width = qMax(1, itemRect.width() - 2);
    const int height = qMax(1, textHeight + 8);
    // Keep the label tied to the cell even when the item is partially clipped
    // by the viewport. Moving the overlay on its own makes its right edge
    // protrude by a few pixels for the first/last visible column.
    const int x = itemRect.left() + 1;
    const int iconBottom = itemRect.top() + iconExtent + 4;
    int y = qBound(viewportRect.top() + 2, iconBottom,
                   qMax(viewportRect.top() + 2, viewportRect.bottom() - height - 1));
    return QRect(x, y, width, height);
}

inline bool selectedNameNeedsCallout(
    const QString &text,
    const QFont &font,
    int innerWidth,
    int maxGridLines)
{
    if (text.isEmpty() || innerWidth <= 0) return false;
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    int lineCount = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(innerWidth);
        ++lineCount;
    }
    layout.endLayout();
    return lineCount > maxGridLines
        || lineCount > 1
        || QFontMetrics(font).horizontalAdvance(text) > innerWidth;
}

inline QPainterPath selectedNameOutlinePath(
    const QRect &itemRect,
    const QRect &calloutRect)
{
    QPainterPath itemPath;
    itemPath.addRoundedRect(
        QRectF(itemRect).adjusted(1.5, 1.5, -1.5, -1.5), 6.0, 6.0);
    QPainterPath calloutPath;
    calloutPath.addRect(QRectF(calloutRect).adjusted(0.5, -1.0, -0.5, -0.5));
    return itemPath.united(calloutPath).simplified();
}

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

    void setCompactMode(bool enabled)
    {
        if (m_compactMode == enabled) {
            return;
        }
        m_compactMode = enabled;
        if (m_view) {
            m_view->doItemsLayout();
            m_view->viewport()->update();
        }
    }

    void paint(
        QPainter *painter,
        const QStyleOptionViewItem &option,
        const QModelIndex &index) const override
    {
        if (!m_view) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        // KCategorizedView can hand the delegate a stale State_Selected flag
        // for a previously selected item even though QItemSelectionModel has
        // already moved the selection. Treat the selection model as the single
        // source of truth for painting in every grouped list mode.
        const bool modelSelected = m_view->selectionModel()
            && m_view->selectionModel()->isSelected(index);

        // KCategorizedView can also leave State_MouseOver on the item that was
        // hovered before a categorized layout/selection update. Breeze paints
        // that stale hover almost exactly like a second selection. Recompute
        // hover from the real cursor position instead of trusting option.state.
        const auto syncMouseOver = [this, &index](QStyleOptionViewItem &itemOption) {
            const QPoint viewportPos =
                m_view->viewport()->mapFromGlobal(QCursor::pos());
            const bool actuallyHovered =
                m_view->viewport()->rect().contains(viewportPos)
                && m_view->indexAt(viewportPos) == index;
            if (actuallyHovered)
                itemOption.state |= QStyle::State_MouseOver;
            else
                itemOption.state &= ~QStyle::State_MouseOver;
        };

        if (m_view->viewMode() != QListView::IconMode) {
            QStyleOptionViewItem adjusted(option);
            if (modelSelected)
                adjusted.state |= QStyle::State_Selected;
            else
                adjusted.state &= ~QStyle::State_Selected;
            syncMouseOver(adjusted);
            QStyledItemDelegate::paint(painter, adjusted, index);
            return;
        }

        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        // Never let the platform style use a stale selected bit. Icon-mode
        // selection is painted explicitly below from QItemSelectionModel.
        const QPoint viewportPos = m_view->viewport()->mapFromGlobal(QCursor::pos());
        const bool actuallyHovered =
            m_view->viewport()->rect().contains(viewportPos)
            && m_view->indexAt(viewportPos) == index;
        directory_view_detail::synchronizeIconItemState(
            opt.state, modelSelected, actuallyHovered);

        const QString fullText = opt.text;
        opt.text.clear();

        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();

        // 1. Paint one restrained selection surface for the entire cell.
        // Do not ask the platform style to paint State_Selected as well: KDE
        // styles differ in whether they highlight the icon, text or both and
        // can consequently add a second rectangle. An opaque palette blend
        // stays legible over thumbnails and works in light and dark themes.
        const bool selected = modelSelected;
        bool expandedName = false;
        QColor selectionBorder;
        if (selected) {
            const bool focusedPane = m_view->hasFocus();
            const QPalette::ColorGroup group = focusedPane
                ? QPalette::Active
                : QPalette::Inactive;
            selectionBorder = opt.palette.color(group, QPalette::Highlight);
            selectionBorder.setAlpha(focusedPane ? 190 : 130);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            expandedName = directory_view_detail::selectedNameNeedsCallout(
                fullText,
                opt.font,
                qMax(1, opt.rect.width() - 16),
                m_alwaysShowFullNames ? 4 : 2);
            // The style may paint over the bottom border (especially when the
            // filename fits on one line). Fill now, but stroke AFTER it paints.
            // Expanded names get their one unified outline in paintEvent().
            painter->setPen(Qt::NoPen);
            painter->setBrush(directory_view_detail::blendedSelectionColor(
                opt.palette, group, focusedPane ? 16 : 10));
            painter->drawRoundedRect(
                QRectF(opt.rect).adjusted(1.5, 1.5, -1.5, -1.5), 6.0, 6.0);
            painter->restore();
            opt.state &= ~QStyle::State_MouseOver;
        }
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const auto paintShortNameOutline = [&] {
            if (!selected || expandedName) return;
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(selectionBorder, 1.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(
                QRectF(opt.rect).adjusted(1.5, 1.5, -1.5, -1.5), 6.0, 6.0);
            painter->restore();
        };

        // 2. Determine text area
        QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        if (!textRect.isValid() || textRect.isEmpty()) {
            const int iconBottom = opt.rect.top() + opt.decorationSize.height() + 4;
            textRect = QRect(opt.rect.left() + 4, iconBottom, opt.rect.width() - 8, opt.rect.bottom() - iconBottom);
        }

        if (fullText.isEmpty() || textRect.width() <= 0 || textRect.height() <= 0) {
            paintShortNameOutline();
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
        const QColor textColor = opt.palette.color(cg, QPalette::Text);

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
        paintShortNameOutline();
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
            const int width = m_compactMode
                ? 240
                : qMax(180, m_view->viewport()->width() - 16);
            const int height = m_compactMode
                ? qMax(26, metrics.lineSpacing() + 6)
                : qMax(32, metrics.lineSpacing() + 10);
            return QSize(width, height);
        }

        return directory_view_detail::iconGridSize(
            m_view->iconSize().width(),
            m_alwaysShowFullNames);
    }

private:
    QListView *m_view = nullptr;
    bool m_alwaysShowFullNames = false;
    bool m_compactMode = false;
};

class DirectoryListWidget : public KCategorizedView
{
    Q_OBJECT

public:
    explicit DirectoryListWidget(QWidget *parent = nullptr)
        : KCategorizedView(parent)
    {
        m_sourceModel = new QStandardItemModel(this);
        m_proxyModel = new directory_view_detail::StableCategoryProxy(this);
        m_proxyModel->setSourceModel(m_sourceModel);
        m_proxyModel->setDynamicSortFilter(false);
        m_proxyModel->setCategorizedModel(false);
        m_proxyModel->sort(0, Qt::AscendingOrder);
        setModel(m_proxyModel);
        setCategoryDrawer(new KCategoryDrawer(this));
        setCategorySpacing(8);
        setCollapsibleBlocks(false);

        m_nameDelegate = new ExplorerNameDelegate(this);
        setItemDelegate(m_nameDelegate);
        setTextElideMode(Qt::ElideRight);
        setWordWrap(true);
        updateGridGeometry();

        connect(
            selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            [this] {
                viewport()->update();
                Q_EMIT itemSelectionChanged();
            });
        connect(
            selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            [this] {
                viewport()->update();
            });
        connect(this, &QAbstractItemView::doubleClicked, this,
                [this](const QModelIndex &index) { Q_EMIT itemDoubleClicked(index); });
    }

    void clear()
    {
        // KCategorizedView keeps its hovered QModelIndex internally. Clear it
        // while the proxy index is still valid; otherwise a later Leave event
        // can call visualRect() with an index invalidated by model clear().
        QEvent leave(QEvent::Leave);
        KCategorizedView::leaveEvent(&leave);
        m_sourceModel->clear();
    }

    int count() const
    {
        return m_proxyModel->rowCount();
    }

    QModelIndex item(int row) const
    {
        return m_proxyModel->index(row, 0);
    }

    QModelIndex itemFromIndex(const QModelIndex &index) const
    {
        return index;
    }

    QModelIndex itemAt(const QPoint &position) const
    {
        return indexAt(position);
    }

    QRect visualItemRect(const QModelIndex &index) const
    {
        return visualRect(index);
    }

    QList<QModelIndex> selectedItems() const
    {
        return selectedIndexes();
    }

    QModelIndex currentItem() const
    {
        return currentIndex();
    }

    int currentRow() const
    {
        return currentIndex().row();
    }

    void setCurrentRow(int row, QItemSelectionModel::SelectionFlags flags = QItemSelectionModel::ClearAndSelect)
    {
        const QModelIndex index = item(row);
        if (!index.isValid()) return;
        selectionModel()->setCurrentIndex(index, flags | QItemSelectionModel::Current);
    }

    void setRowSelected(int row, bool selected)
    {
        const QModelIndex index = item(row);
        if (!index.isValid()) return;
        selectionModel()->select(index, selected ? QItemSelectionModel::Select
                                                 : QItemSelectionModel::Deselect);
    }

    void addFileItem(const FileInfo &file, const QIcon &icon,
                     const QString &typeText, const QString &sizeText,
                     const QString &modifiedText, const QString &toolTip,
                     const QString &categoryDisplay = QString(),
                     const QVariant &categoryOrder = QVariant())
    {
        auto *item = new QStandardItem(icon, file.name);
        item->setData(file.url.toString(), directory_view_detail::UrlRole);
        item->setData(file.isDir, directory_view_detail::DirectoryRole);
        item->setData(typeText, directory_view_detail::TypeTextRole);
        item->setData(sizeText, directory_view_detail::SizeTextRole);
        item->setData(modifiedText, directory_view_detail::ModifiedTextRole);
        item->setData(true, directory_view_detail::FileItemRole);
        item->setData(m_sourceModel->rowCount(), directory_view_detail::OriginalOrderRole);
        item->setData(categoryDisplay, KCategorizedSortFilterProxyModel::CategoryDisplayRole);
        item->setData(categoryOrder, KCategorizedSortFilterProxyModel::CategorySortRole);
        item->setToolTip(toolTip);
        m_sourceModel->appendRow(item);
    }

    void addItem(const QString &text)
    {
        FileInfo file;
        file.name = text;
        addFileItem(file, QIcon(), QString(), QString(), QString(), QString());
    }

    void setCategorized(bool categorized)
    {
        m_proxyModel->setCategorizedModel(categorized);
        m_proxyModel->invalidate();
        m_proxyModel->sort(0, Qt::AscendingOrder);
        doItemsLayout();
    }

    bool isCategorized() const
    {
        return m_proxyModel->isCategorizedModel();
    }

    void updateGridGeometry()
    {
        if (viewMode() == QListView::IconMode) {
            setGridSize(directory_view_detail::iconGridSize(
                iconSize().width(), alwaysShowFullNames()));
            setUniformItemSizes(true);
        } else {
            setGridSize(QSize());
            setUniformItemSizes(false);
        }
    }

    void setCompactMode(bool enabled)
    {
        if (m_compactMode == enabled) {
            return;
        }
        m_compactMode = enabled;
        if (m_nameDelegate) {
            m_nameDelegate->setCompactMode(enabled);
        }
        setWordWrap(!enabled);
        setTextElideMode(Qt::ElideRight);
        doItemsLayout();
        viewport()->update();
    }

    bool compactMode() const
    {
        return m_compactMode;
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
    void itemSelectionChanged();
    void itemDoubleClicked(const QModelIndex &index);
    void urlsDropped(
        const QList<QUrl> &urls,
        const QUrl &destination,
        const QPoint &globalPosition,
        Qt::KeyboardModifiers modifiers);

protected:
    void paintEvent(QPaintEvent *event) override
    {
        KCategorizedView::paintEvent(event);

        if (viewMode() != QListView::IconMode) {
            return;
        }

        if (state() == QAbstractItemView::EditingState) {
            return;
        }

        const QModelIndex item = currentItem();
        if (!item.isValid() || !selectionModel()->isSelected(item)) {
            return;
        }

        const QRect itemRect = visualItemRect(item);
        if (!itemRect.isValid() || !viewport()->rect().intersects(itemRect)) {
            return;
        }

        const QString text = item.data(Qt::DisplayRole).toString();
        if (text.isEmpty()) {
            return;
        }

        const int maxGridLines = alwaysShowFullNames() ? 4 : 2;
        const int calloutWidth = qMax(1, itemRect.width() - 4);
        const int innerWidth = calloutWidth - 12;

        QTextLayout layout(text, font());
        QTextOption opt;
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        opt.setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        layout.setTextOption(opt);

        layout.beginLayout();
        qreal textHeight = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(innerWidth);
            line.setPosition(QPointF(6, textHeight));
            textHeight += line.height();
        }
        layout.endLayout();

        const bool needsCallout = directory_view_detail::selectedNameNeedsCallout(
            text, font(), innerWidth, maxGridLines);
        if (!needsCallout) {
            return;
        }

        const QRect calloutRect = directory_view_detail::selectedNameCalloutRect(
            itemRect,
            iconSize().height(),
            qRound(textHeight),
            viewport()->rect());

        QPainter painter(viewport());
        if (!painter.isActive()) {
            return;
        }
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QPalette pal = palette();
        const bool focusedPane = hasFocus();
        const QPalette::ColorGroup group = focusedPane
            ? QPalette::Active
            : QPalette::Inactive;
        const QColor bgColor = directory_view_detail::blendedSelectionColor(
            pal, group, focusedPane ? 16 : 10);
        QColor borderColor = pal.color(group, QPalette::Highlight);
        borderColor.setAlpha(focusedPane ? 190 : 130);
        const QColor textColor = pal.color(QPalette::Text);

        // Fill the expanded label, then stroke the union of cell and label
        // exactly once. This prevents the cell's bottom edge from becoming an
        // internal second rule for two-line and longer names.
        painter.setPen(Qt::NoPen);
        painter.setBrush(bgColor);
        painter.drawRect(calloutRect);
        painter.setPen(QPen(borderColor, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(directory_view_detail::selectedNameOutlinePath(
            itemRect, calloutRect));

        painter.setPen(textColor);
        layout.draw(&painter, QPointF(calloutRect.left(), calloutRect.top() + 4));
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions)

        QList<QUrl> urls;
        for (const QModelIndex &item : selectedItems()) {
            const QUrl url(item.data(directory_view_detail::UrlRole).toString());
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
        KCategorizedView::dragEnterEvent(event);
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
        KCategorizedView::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event->mimeData()->hasUrls()) {
            KCategorizedView::dropEvent(event);
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
        KCategorizedView::scrollContentsBy(dx, dy);
        viewport()->update();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        KCategorizedView::resizeEvent(event);
        viewport()->update();
    }

    void showEvent(QShowEvent *event) override
    {
        KCategorizedView::showEvent(event);
        viewport()->update();
    }

private:
    QUrl dropDestinationAt(const QPoint &position) const
    {
        QUrl destination = m_dropDirectory;
        const QModelIndex target = itemAt(position);
        if (target.isValid()
            && target.data(directory_view_detail::DirectoryRole).toBool()) {
            const QUrl targetUrl(
                target.data(directory_view_detail::UrlRole).toString());
            if (targetUrl.isValid()) {
                destination = targetUrl;
            }
        }
        return destination;
    }

    QUrl m_dropDirectory;
    QStandardItemModel *m_sourceModel = nullptr;
    directory_view_detail::StableCategoryProxy *m_proxyModel = nullptr;
    ExplorerNameDelegate *m_nameDelegate = nullptr;
    bool m_compactMode = false;
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
    int viewMode,
    int iconSizeMode = 1)
{
    if (!list || !details || !stack) {
        return;
    }

    if (viewMode == 0) {
        list->setCompactMode(false);
        list->setViewMode(QListView::IconMode);
        list->setFlow(QListView::LeftToRight);
        list->setWrapping(true);
        const int extent = directory_view_detail::iconExtentForMode(iconSizeMode);
        list->setIconSize(QSize(extent, extent));
        list->setSpacing(3);
        list->updateGridGeometry();
        stack->setCurrentWidget(list);

        if (viewButton) {
            viewButton->setIcon(
                themedIcon(QStringLiteral("view-list-icons")));
        }
    } else if (viewMode == 1) {
        list->setCompactMode(false);
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
    } else if (viewMode == 2) {
        list->setCompactMode(false);
        stack->setCurrentWidget(details);

        if (viewButton) {
            viewButton->setIcon(
                themedIcon(QStringLiteral("view-list-details")));
        }
    } else {
        list->setCompactMode(true);
        list->setViewMode(QListView::ListMode);
        list->setFlow(QListView::TopToBottom);
        list->setWrapping(true);
        list->setResizeMode(QListView::Adjust);
        list->setIconSize(QSize(20, 20));
        list->setSpacing(0);
        list->setGridSize(QSize());
        list->setUniformItemSizes(true);
        stack->setCurrentWidget(list);

        if (viewButton) {
            viewButton->setIcon(
                themedIcon(QStringLiteral("view-list-tree"), QStringLiteral("view-list-text")));
        }
    }

    // QListView layout changes reset parts of the drag/drop configuration.
    configureDirectoryDragDrop(list);
    configureDirectoryDragDrop(details);
}


inline QString directoryItemToolTip(
    const FileInfo &file,
    const QString &typeText,
    const QString &sizeText,
    const QString &modifiedText)
{
    QStringList lines{
        urlForDisplay(file.url),
        typeText
    };
    if (!file.isDir) {
        lines.push_back(
            QStringLiteral("%1: %2")
                .arg(
                    trLocal("Rozmiar", "Size"),
                    sizeText));
    }
    lines.push_back(modifiedText);
    return lines.join(QLatin1Char('\n'));
}


inline void addDirectoryFileItems(
    DirectoryListWidget *list,
    DirectoryTreeWidget *details,
    const FileInfo &file,
    const QIcon &icon,
    const QString &typeText,
    const QString &sizeText,
    const QString &modifiedText,
    const QStringList &extraDetailColumns = {},
    const QString &categoryDisplay = QString(),
    const QVariant &categoryOrder = QVariant())
{
    const QString toolTip = directoryItemToolTip(
        file,
        typeText,
        sizeText,
        modifiedText);
    list->addFileItem(file, icon, typeText, sizeText, modifiedText, toolTip,
                      categoryDisplay, categoryOrder);

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
    detailsItem->setData(
        0,
        directory_view_detail::FileItemRole,
        true);
    detailsItem->setToolTip(0, toolTip);
    detailsItem->setTextAlignment(
        2,
        Qt::AlignRight | Qt::AlignVCenter);
}

inline void addDirectoryGroupHeader(
    DirectoryTreeWidget *details,
    const QString &label)
{
    auto *header = new QTreeWidgetItem(details, QStringList{label});
    header->setFirstColumnSpanned(true);
    header->setFlags(Qt::ItemIsEnabled);
    QFont font = header->font(0);
    font.setBold(true);
    header->setFont(0, font);
    header->setData(0, directory_view_detail::FileItemRole, false);
}

inline QSet<QString> selectedDirectoryListUrls(const DirectoryListWidget *list)
{
    QSet<QString> urls;
    for (const QModelIndex &index : list->selectedItems()) {
        const QString url = index.data(directory_view_detail::UrlRole).toString();
        if (!url.isEmpty()) urls.insert(url);
    }
    return urls;
}

inline QSet<QString> selectedDirectoryDetailsUrls(const DirectoryTreeWidget *details)
{
    QSet<QString> urls;
    for (const QTreeWidgetItem *item : details->selectedItems()) {
        if (!item->data(0, directory_view_detail::FileItemRole).toBool()) continue;
        const QString url = item->data(0, Qt::UserRole).toString();
        if (!url.isEmpty()) urls.insert(url);
    }
    return urls;
}

inline void restoreDirectorySelections(
    DirectoryListWidget *list, DirectoryTreeWidget *details,
    const QSet<QString> &listUrls, const QSet<QString> &detailsUrls)
{
    for (int row = 0; row < list->count(); ++row) {
        const QModelIndex index = list->item(row);
        if (listUrls.contains(index.data(directory_view_detail::UrlRole).toString()))
            list->setRowSelected(row, true);
    }
    for (int row = 0; row < details->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = details->topLevelItem(row);
        if (item->data(0, directory_view_detail::FileItemRole).toBool()
            && detailsUrls.contains(item->data(0, Qt::UserRole).toString()))
            item->setSelected(true);
    }
}
