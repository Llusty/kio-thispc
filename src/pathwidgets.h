// Width-independent presentation for path text in both browser panes.
// SPDX-License-Identifier: MIT
#pragma once

#include <QHelpEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>
#include <QVector>
#include <functional>
#include <utility>
#include <QScrollArea>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QToolTip>

class PathScrollArea : public QScrollArea
{
public:
    explicit PathScrollArea(QWidget *parent) : QScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    // Only the row height follows the contents. Its width belongs to the pane.
    QSize sizeHint() const override
    {
        return QSize(0, widget() ? widget()->sizeHint().height() : 0);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
};

class ElidedPathButton : public QToolButton
{
public:
    explicit ElidedPathButton(QWidget *parent) : QToolButton(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        const int margin = style()->pixelMetric(QStyle::PM_ButtonMargin, &option, this);
        int available = style()->subControlRect(
            QStyle::CC_ToolButton, &option, QStyle::SC_ToolButton, this).width() - 2 * margin;
        if (!option.icon.isNull()) available -= option.iconSize.width() + 4;
        option.text = option.fontMetrics.elidedText(option.text, Qt::ElideMiddle,
                                                   qMax(0, available), Qt::TextShowMnemonic);
        QStylePainter painter(this);
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
    }
};

// Segment-aware path for the secondary browser pane. Its natural width is
// clipped by the surrounding scroll area instead of eliding folder names;
// clicking a separator or blank space still opens the address editor.
class SegmentedPathButton : public ElidedPathButton
{
public:
    struct Segment {
        QString text;
        QUrl url;
    };

    explicit SegmentedPathButton(QWidget *parent) : ElidedPathButton(parent)
    {
        // The surrounding scroll area owns the available width. The button
        // keeps its natural width so no folder name is truncated.
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        setMouseTracking(true);
    }

    QSize sizeHint() const override
    {
        QSize hint = ElidedPathButton::sizeHint();
        if (!m_segments.isEmpty()) hint.setWidth(fullPathWidth());
        return hint;
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

    void setSegments(QVector<Segment> segments)
    {
        m_segments = std::move(segments);
        m_hovered = -1;
        m_pressed = -1;
        setCursor(m_segments.isEmpty() ? Qt::PointingHandCursor : Qt::IBeamCursor);
        updateGeometry();
        adjustSize();
        update();
    }

    void setNavigateCallback(std::function<void(const QUrl &)> callback)
    {
        m_navigate = std::move(callback);
    }

    int segmentCount() const { return m_segments.size(); }

    QRect segmentRect(int index) const
    {
        for (const auto &span : visibleSpans()) {
            if (span.index == index) return span.rect;
        }
        return {};
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (m_segments.isEmpty()) {
            ElidedPathButton::paintEvent(event);
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRect area = contentsRect();
        if (!icon().isNull()) {
            const int iconSide = qMin(iconSize().width(), area.height() - 4);
            if (iconSide > 0) {
                const QRect iconRect(area.left() + 2,
                                     area.top() + (area.height() - iconSide) / 2,
                                     iconSide, iconSide);
                icon().paint(&painter, iconRect);
            }
        }
        const QVector<Span> spans = visibleSpans();
        for (const Span &span : spans) {
            if (span.index == m_hovered) {
                painter.setPen(palette().color(QPalette::Highlight));
                // Match the primary breadcrumb: visible blue-grey fill on dark themes.
                painter.setBrush(QColor(93, 126, 155, 115));
                // Keep the antialiased outline away from the clipped row edges.
                painter.drawRoundedRect(span.rect.adjusted(0, 2, -1, -3), 4, 4);
            }
            painter.setPen(palette().color(QPalette::ButtonText));
            painter.drawText(span.rect, Qt::AlignCenter | Qt::TextSingleLine, span.label);
        }
        for (int i = 0; i + 1 < spans.size(); ++i) {
            const int betweenStart = spans.at(i).rect.right() + 1;
            const int betweenEnd = spans.at(i + 1).rect.left();
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRect(betweenStart, area.top(), betweenEnd - betweenStart,
                                   area.height()), Qt::AlignCenter, QStringLiteral("›"));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int hovered = segmentAt(event->position().toPoint());
        if (m_hovered != hovered) {
            m_hovered = hovered;
            setCursor(hovered >= 0 ? Qt::PointingHandCursor : Qt::IBeamCursor);
            update();
        }
        ElidedPathButton::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hovered = -1;
        setCursor(m_segments.isEmpty() ? Qt::PointingHandCursor : Qt::IBeamCursor);
        update();
        ElidedPathButton::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressed = event->button() == Qt::LeftButton
            ? segmentAt(event->position().toPoint()) : -1;
        ElidedPathButton::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int clicked = segmentAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton && clicked >= 0
            && clicked == m_pressed && m_navigate) {
            setDown(false);
            m_pressed = -1;
            const QUrl url = m_segments.at(clicked).url;
            m_navigate(url);
            event->accept();
            return;
        }
        m_pressed = -1;
        ElidedPathButton::mouseReleaseEvent(event);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip && !m_segments.isEmpty()) {
            const auto *help = static_cast<QHelpEvent *>(event);
            const int index = segmentAt(help->pos());
            if (index >= 0) {
                const QUrl &url = m_segments.at(index).url;
                QToolTip::showText(help->globalPos(), url.isLocalFile()
                                   ? url.toLocalFile() : url.toDisplayString(), this);
                return true;
            }
        }
        return ElidedPathButton::event(event);
    }

private:
    struct Span {
        QRect rect;
        QString label;
        int index;
    };

    static constexpr int SegmentInset = 6;

    int separatorWidth() const
    {
        return fontMetrics().horizontalAdvance(QStringLiteral(" › ")) + 4;
    }

    int fullPathWidth() const
    {
        if (m_segments.isEmpty()) return ElidedPathButton::sizeHint().width();
        int width = 6;
        if (!icon().isNull()) {
            const int iconSide = qMin(iconSize().width(),
                                      ElidedPathButton::sizeHint().height() - 4);
            width += qMax(0, iconSide) + 4;
        }
        for (const Segment &segment : m_segments) {
            width += fontMetrics().horizontalAdvance(segment.text) + 2 * SegmentInset;
        }
        width += separatorWidth() * (m_segments.size() - 1);
        return width;
    }

    QVector<Span> visibleSpans() const
    {
        QVector<Span> spans;
        if (m_segments.isEmpty()) return spans;
        const QRect area = contentsRect();
        int x = area.left() + 2;
        if (!icon().isNull()) x += qMax(0, qMin(iconSize().width(), area.height() - 4)) + 4;
        for (int index = 0; index < m_segments.size(); ++index) {
            if (index) x += separatorWidth();
            const QString &label = m_segments.at(index).text;
            const int width = fontMetrics().horizontalAdvance(label) + 2 * SegmentInset;
            spans.append({QRect(x, area.top() + 1, width, area.height() - 2), label, index});
            x += width;
        }
        return spans;
    }

    int segmentAt(const QPoint &point) const
    {
        for (const Span &span : visibleSpans()) {
            if (span.rect.contains(point)) return span.index;
        }
        return -1;
    }

    QVector<Segment> m_segments;
    std::function<void(const QUrl &)> m_navigate;
    int m_hovered = -1;
    int m_pressed = -1;
};

class ElidedPathLabel : public QLabel
{
public:
    explicit ElidedPathLabel(QWidget *parent) : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        style()->drawItemText(&painter, contentsRect(),
            QStyle::visualAlignment(layoutDirection(), alignment()), palette(), isEnabled(),
            fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()), foregroundRole());
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            QToolTip::showText(static_cast<QHelpEvent *>(event)->globalPos(), text(), this);
            return true;
        }
        return QLabel::event(event);
    }
};
