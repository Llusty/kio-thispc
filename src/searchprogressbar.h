/*
 * Animated progress bar for search views with determinate and indeterminate modes.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <QBasicTimer>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <algorithm>

class SearchProgressBar : public QProgressBar
{
    Q_OBJECT
public:
    explicit SearchProgressBar(QWidget *parent = nullptr)
        : QProgressBar(parent)
    {
        setTextVisible(false);
        setMaximumHeight(7);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    ~SearchProgressBar() override
    {
        if (m_timer.isActive()) {
            m_timer.stop();
        }
    }

    bool isIndeterminate() const
    {
        return minimum() == 0 && maximum() == 0;
    }

    bool isAnimationRunning() const
    {
        return m_timer.isActive();
    }

    qreal animationPhase() const
    {
        return m_phase;
    }

    void setRange(int minimum, int maximum)
    {
        QProgressBar::setRange(minimum, maximum);
        updateAnimationState();
    }

    void setMinimum(int minimum)
    {
        QProgressBar::setMinimum(minimum);
        updateAnimationState();
    }

    void setMaximum(int maximum)
    {
        QProgressBar::setMaximum(maximum);
        updateAnimationState();
    }

    void setValue(int value)
    {
        QProgressBar::setValue(value);
        updateAnimationState();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QProgressBar::showEvent(event);
        updateAnimationState();
    }

    void hideEvent(QHideEvent *event) override
    {
        QProgressBar::hideEvent(event);
        if (m_timer.isActive()) {
            m_timer.stop();
        }
        m_phase = 0.0;
    }

    void timerEvent(QTimerEvent *event) override
    {
        if (event->timerId() == m_timer.timerId()) {
            m_phase += 0.02;
            if (m_phase >= 1.0) {
                m_phase -= 1.0;
            }
            update();
            return;
        }
        QProgressBar::timerEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF bounds = rect();
        if (bounds.width() <= 0 || bounds.height() <= 0) {
            return;
        }

        const qreal radius = bounds.height() / 2.0;
        QPainterPath clipPath;
        clipPath.addRoundedRect(bounds, radius, radius);
        painter.setClipPath(clipPath);

        // Track background: use QPalette::Mid with subtle opacity
        QColor trackColor = palette().color(QPalette::Mid);
        if (!trackColor.isValid()) {
            trackColor = QColor(128, 128, 128, 64);
        } else {
            trackColor.setAlpha(trackColor.alpha() > 180 ? 140 : trackColor.alpha());
        }
        painter.fillPath(clipPath, trackColor);

        // Highlight color from palette
        const QColor highlight = palette().color(QPalette::Highlight);

        if (isIndeterminate()) {
            const qreal w = bounds.width();
            const qreal h = bounds.height();
            // Block length is approx 30% of bar width, clamped between 50px and 220px
            const qreal blockWidth = std::clamp(w * 0.30, 50.0, 220.0);
            // Travel distance from -blockWidth to w
            const qreal x = -blockWidth + m_phase * (w + blockWidth);

            const QRectF chunkRect(x, 0.0, blockWidth, h);
            QPainterPath chunkPath;
            chunkPath.addRoundedRect(chunkRect, radius, radius);
            painter.fillPath(chunkPath, highlight);
        } else {
            const int min = minimum();
            const int max = maximum();
            if (max > min) {
                const qreal fraction = std::clamp(static_cast<qreal>(value() - min) / static_cast<qreal>(max - min), 0.0, 1.0);
                if (fraction > 0.0) {
                    const QRectF filledRect(bounds.x(), bounds.y(), bounds.width() * fraction, bounds.height());
                    QPainterPath filledPath;
                    filledPath.addRoundedRect(filledRect, radius, radius);
                    painter.fillPath(filledPath, highlight);
                }
            }
        }
    }

private:
    void updateAnimationState()
    {
        const bool shouldAnimate = isIndeterminate() && !isHidden();
        if (shouldAnimate) {
            if (!m_timer.isActive()) {
                m_timer.start(25, this);
            }
        } else {
            if (m_timer.isActive()) {
                m_timer.stop();
            }
            if (m_phase != 0.0) {
                m_phase = 0.0;
                update();
            }
        }
    }

    QBasicTimer m_timer;
    qreal m_phase = 0.0;
};
