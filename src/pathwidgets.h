// Width-independent presentation for path text in both browser panes.
// SPDX-License-Identifier: MIT
#pragma once

#include <QHelpEvent>
#include <QLabel>
#include <QPainter>
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
