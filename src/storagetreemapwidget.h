/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "storagescandata.h"
#include "storagetreemapdata.h"
#include "storagetreemaplayout.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

class StorageTreemapCanvas final : public QWidget
{
    Q_OBJECT

public:
    explicit StorageTreemapCanvas(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

    void setLayoutRects(const QList<StorageTreemapLayoutRect> &rects)
    {
        m_rects = rects;
        m_hoverIndex = -1;
        m_selectedIndex = -1;
        update();
    }

    const QList<StorageTreemapLayoutRect> &layoutRects() const { return m_rects; }

    int selectedIndex() const { return m_selectedIndex; }
    void setSelectedIndex(int idx)
    {
        if (idx >= 0 && idx < m_rects.size()) {
            m_selectedIndex = idx;
        } else {
            m_selectedIndex = -1;
        }
        update();
    }

Q_SIGNALS:
    void itemHovered(const StorageTreemapLayoutRect *rect);
    void itemSelected(const StorageTreemapLayoutRect *rect);
    void itemActivated(const StorageTreemapNode *node);
    void navigateRequested(const QUrl &url);
    void drillDownRequested(const StorageTreemapNode *node);

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        if (m_rects.isEmpty()) {
            p.setPen(palette().color(QPalette::Disabled, QPalette::Text));
            p.drawText(rect(), Qt::AlignCenter, tr("Brak danych do wyświetlenia."));
            return;
        }

        const bool isDark = palette().color(QPalette::Window).lightness() < 128;

        for (int i = 0; i < m_rects.size(); ++i) {
            const auto &lr = m_rects[i];
            const QRectF &r = lr.rect;
            if (r.width() < 0.5 || r.height() < 0.5) {
                continue; // Skip sub-pixel painting for large root performance
            }

            QColor fillColor = computeColor(lr.node, isDark);

            if (i == m_hoverIndex) {
                fillColor = fillColor.lighter(isDark ? 125 : 110);
            }

            p.fillRect(r, fillColor);

            // Border
            QColor borderColor = isDark ? palette().color(QPalette::Mid) : palette().color(QPalette::Midlight);
            if (i == m_selectedIndex) {
                p.setPen(QPen(palette().color(QPalette::Highlight), 2.5));
            } else if (i == m_hoverIndex) {
                p.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
            } else {
                p.setPen(QPen(borderColor, 1.0));
            }
            p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));

            // Labels
            if (lr.showLabel && r.width() >= 36.0 && r.height() >= 16.0) {
                p.save();
                p.setClipRect(r.adjusted(2, 2, -2, -2));

                const int bgLightness = fillColor.lightness();
                p.setPen(bgLightness < 130 ? Qt::white : QColor(25, 25, 25));

                QFont f = font();
                f.setPointSize(qBound(8, static_cast<int>(r.height() / 4.0), 10));
                p.setFont(f);

                QString text = lr.label;
                if (r.height() >= 32.0) {
                    text += QStringLiteral("\n") + lr.formattedSize;
                }

                p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
                p.restore();
            }
        }

        if (hasFocus() && m_selectedIndex >= 0 && m_selectedIndex < m_rects.size()) {
            QStyleOptionFocusRect option;
            option.initFrom(this);
            option.rect = m_rects[m_selectedIndex].rect.toRect();
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &p, this);
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int prev = m_hoverIndex;
        m_hoverIndex = hitTest(e->position());

        if (m_hoverIndex != prev) {
            update();
            if (m_hoverIndex >= 0) {
                Q_EMIT itemHovered(&m_rects[m_hoverIndex]);
            } else {
                Q_EMIT itemHovered(nullptr);
            }
        }
    }

    void leaveEvent(QEvent *) override
    {
        if (m_hoverIndex != -1) {
            m_hoverIndex = -1;
            update();
            Q_EMIT itemHovered(nullptr);
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        setFocus();
        const int idx = hitTest(e->position());
        m_selectedIndex = idx;
        update();

        if (idx >= 0) {
            Q_EMIT itemSelected(&m_rects[idx]);
        } else {
            Q_EMIT itemSelected(nullptr);
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        const int idx = hitTest(e->position());
        if (idx >= 0 && idx < m_rects.size()) {
            const auto *node = m_rects[idx].node;
            if (node) {
                if (node->isDirectory()) {
                    if (!node->isMountBoundary) {
                        Q_EMIT drillDownRequested(node);
                    }
                } else {
                    // Double click on file: Never execute! Emit navigateRequested to show in folder
                    const QString parentDir = node->path.section(QLatin1Char('/'), 0, -2);
                    Q_EMIT navigateRequested(QUrl::fromLocalFile(parentDir.isEmpty() ? QStringLiteral("/") : parentDir));
                }
            }
        }
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (m_selectedIndex >= 0 && m_selectedIndex < m_rects.size()) {
                const auto *node = m_rects[m_selectedIndex].node;
                if (node) {
                    if (node->isDirectory()) {
                        if (!node->isMountBoundary) {
                            Q_EMIT drillDownRequested(node);
                        }
                    } else {
                        const QString parentDir = node->path.section(QLatin1Char('/'), 0, -2);
                        Q_EMIT navigateRequested(QUrl::fromLocalFile(parentDir.isEmpty() ? QStringLiteral("/") : parentDir));
                    }
                }
            }
            return;
        }

        if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Up) {
            if (!m_rects.isEmpty()) {
                m_selectedIndex = (m_selectedIndex <= 0) ? (m_rects.size() - 1) : (m_selectedIndex - 1);
                update();
                Q_EMIT itemSelected(&m_rects[m_selectedIndex]);
            }
            return;
        }

        if (e->key() == Qt::Key_Right || e->key() == Qt::Key_Down) {
            if (!m_rects.isEmpty()) {
                m_selectedIndex = (m_selectedIndex >= m_rects.size() - 1) ? 0 : (m_selectedIndex + 1);
                update();
                Q_EMIT itemSelected(&m_rects[m_selectedIndex]);
            }
            return;
        }

        QWidget::keyPressEvent(e);
    }

    void contextMenuEvent(QContextMenuEvent *e) override
    {
        const int idx = hitTest(e->pos());
        if (idx < 0 || idx >= m_rects.size()) {
            return;
        }
        const auto *node = m_rects[idx].node;
        if (!node) {
            return;
        }

        m_selectedIndex = idx;
        update();

        QMenu menu(this);
        auto *showAction = menu.addAction(tr("Pokaż w folderze"));
        connect(showAction, &QAction::triggered, this, [this, node] {
            const QString parentDir = node->isDirectory() ? node->path : node->path.section(QLatin1Char('/'), 0, -2);
            Q_EMIT navigateRequested(QUrl::fromLocalFile(parentDir.isEmpty() ? QStringLiteral("/") : parentDir));
        });

        if (node->isDirectory() && !node->isMountBoundary) {
            auto *drillAction = menu.addAction(tr("Ustaw jako katalog główny mapy"));
            connect(drillAction, &QAction::triggered, this, [this, node] {
                Q_EMIT drillDownRequested(node);
            });
        }

        menu.exec(e->globalPos());
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *helpEvent = static_cast<QHelpEvent *>(e);
            const int idx = hitTest(helpEvent->pos());
            if (idx >= 0 && idx < m_rects.size()) {
                const auto &lr = m_rects[idx];
                const auto *node = lr.node;
                QString tip = QStringLiteral("<b>%1</b><br/>%2<br/><hr/>"
                                             "<b>Typ:</b> %3<br/>"
                                             "<b>Rozmiar na dysku:</b> %4 (%5 B)<br/>"
                                             "<b>Rozmiar logiczny:</b> %6 (%7 B)<br/>"
                                             "<b>Udział w widoku:</b> %8%")
                    .arg(node->name.toHtmlEscaped())
                    .arg(node->path.toHtmlEscaped())
                    .arg(node->isDirectory() ? tr("Katalog") : tr("Plik"))
                    .arg(StorageScanStats::formatBytes(node->allocatedSize))
                    .arg(node->allocatedSize)
                    .arg(StorageScanStats::formatBytes(node->logicalSize))
                    .arg(node->logicalSize)
                    .arg(QString::number(lr.percentageOfRoot, 'f', 1));

                if (node->isHardlink) {
                    tip += QStringLiteral("<br/><b>Hardlink:</b> %1 (%2)")
                        .arg(node->isCanonicalHardlink ? tr("ścieżka kanoniczna") : tr("alias"))
                        .arg(tr("%n alias(ów)", "", node->hardlinkAliases.size()));
                }
                if (node->isMountBoundary) {
                    tip += QStringLiteral("<br/><font color='#e67e22'><b>%1</b></font>")
                        .arg(tr("Inny system plików (pominięty)"));
                }
                QToolTip::showText(helpEvent->globalPos(), tip, this);
            } else {
                QToolTip::hideText();
            }
            return true;
        }
        return QWidget::event(e);
    }

private:
    int hitTest(const QPointF &pt) const
    {
        for (int i = 0; i < m_rects.size(); ++i) {
            if (m_rects[i].rect.contains(pt)) {
                return i;
            }
        }
        return -1;
    }

    QColor computeColor(const StorageTreemapNode *node, bool isDark) const
    {
        if (!node) {
            return palette().color(QPalette::Window);
        }
        if (node->isDirectory()) {
            QColor base = palette().color(QPalette::Button);
            return isDark ? base.lighter(115) : base.darker(105);
        }

        const QString ext = node->name.section(QLatin1Char('.'), -1).toLower();
        const uint h = qHash(ext.isEmpty() ? node->name : ext) % 360;
        const int s = isDark ? 90 : 125;
        const int l = isDark ? 70 : 190;
        return QColor::fromHsl(h, s, l);
    }

    QList<StorageTreemapLayoutRect> m_rects;
    int m_hoverIndex = -1;
    int m_selectedIndex = -1;
};

class StorageTreemapWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit StorageTreemapWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("storageTreemapWidget"));
        setupUi();
    }

    void setData(const QString &rootPath,
                 const QList<StorageScanEntry> &entries,
                 const StorageScanStats &stats,
                 StorageScanState scanState)
    {
        m_rootPath = rootPath;
        m_stats = stats;
        m_scanState = scanState;

        // Build hierarchy ONCE per snapshot in memory
        m_treeRoot = StorageTreemapBuilder::buildTree(rootPath, entries, stats, scanState);
        m_visualRoot = m_treeRoot.get();

        // Update persistent snapshot warnings
        QStringList warnings;
        if (scanState == StorageScanState::Cancelled) {
            warnings << tr("Wyniki częściowe — skanowanie anulowane");
        }
        if (stats.inaccessible > 0 || stats.disappeared > 0 || stats.errors > 0 || stats.skippedMounts > 0) {
            warnings << tr("Wyniki mogą być niepełne");
        }
        if (!warnings.isEmpty()) {
            m_bannerLabel->setText(QStringLiteral("⚠️ ") + warnings.join(QStringLiteral(" | ")));
            m_bannerLabel->setVisible(true);
        } else {
            m_bannerLabel->setVisible(false);
        }

        updateView();
    }

    void setVisualRoot(const StorageTreemapNode *node)
    {
        if (node) {
            m_visualRoot = node;
            updateView();
        }
    }

    void setMetric(StorageTreemapMetric metric)
    {
        if (m_metric != metric) {
            m_metric = metric;
            m_metricCombo->setCurrentIndex(metric == StorageTreemapMetric::Allocated ? 0 : 1);
            relayout();
        }
    }

    StorageTreemapMetric metric() const { return m_metric; }
    const StorageTreemapNode *visualRoot() const { return m_visualRoot; }
    const StorageTreemapNode *rootNode() const { return m_treeRoot.get(); }
    const QList<StorageTreemapLayoutRect> &layoutRects() const { return m_canvas->layoutRects(); }

    QPushButton *upButton() const { return m_upButton; }
    QComboBox *metricCombo() const { return m_metricCombo; }
    QLabel *bannerLabel() const { return m_bannerLabel; }
    QLabel *pathLabel() const { return m_pathLabel; }
    QLabel *detailsLabel() const { return m_detailsLabel; }
    StorageTreemapCanvas *canvas() const { return m_canvas; }

Q_SIGNALS:
    void navigateRequested(const QUrl &url);

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        relayout();
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Backspace || (e->key() == Qt::Key_Up && (e->modifiers() & Qt::AltModifier))) {
            drillUp();
            return;
        }
        QWidget::keyPressEvent(e);
    }

private:
    void setupUi()
    {
        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(4, 4, 4, 4);
        mainLayout->setSpacing(4);

        // Toolbar
        auto *toolbar = new QHBoxLayout();
        m_upButton = new QPushButton(tr("⬆ Poziom wyżej"), this);
        m_upButton->setObjectName(QStringLiteral("treemapUpButton"));
        m_upButton->setEnabled(false);
        toolbar->addWidget(m_upButton);

        m_pathLabel = new QLabel(this);
        m_pathLabel->setObjectName(QStringLiteral("treemapPathLabel"));
        QFont f = m_pathLabel->font();
        f.setBold(true);
        m_pathLabel->setFont(f);
        toolbar->addWidget(m_pathLabel, 1);

        toolbar->addWidget(new QLabel(tr("Pokaż:"), this));
        m_metricCombo = new QComboBox(this);
        m_metricCombo->setObjectName(QStringLiteral("treemapMetricCombo"));
        m_metricCombo->addItem(tr("Rozmiar na dysku"), static_cast<int>(StorageTreemapMetric::Allocated));
        m_metricCombo->addItem(tr("Rozmiar logiczny"), static_cast<int>(StorageTreemapMetric::Logical));
        m_metricCombo->setCurrentIndex(0); // Default Allocated
        toolbar->addWidget(m_metricCombo);

        mainLayout->addLayout(toolbar);

        // Warning banner
        m_bannerLabel = new QLabel(this);
        m_bannerLabel->setObjectName(QStringLiteral("treemapBannerLabel"));
        m_bannerLabel->setStyleSheet(QStringLiteral("background: #ffeaa7; color: #2d3436; padding: 4px; border-radius: 4px; font-weight: bold;"));
        m_bannerLabel->setVisible(false);
        mainLayout->addWidget(m_bannerLabel);

        // Canvas
        m_canvas = new StorageTreemapCanvas(this);
        m_canvas->setObjectName(QStringLiteral("treemapCanvas"));
        mainLayout->addWidget(m_canvas, 1);

        // Footer details
        m_detailsLabel = new QLabel(tr("Wybierz element, aby zobaczyć szczegóły."), this);
        m_detailsLabel->setObjectName(QStringLiteral("treemapDetailsLabel"));
        m_detailsLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
        mainLayout->addWidget(m_detailsLabel);

        connect(m_upButton, &QPushButton::clicked, this, &StorageTreemapWidget::drillUp);
        connect(m_metricCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
            m_metric = static_cast<StorageTreemapMetric>(m_metricCombo->itemData(idx).toInt());
            relayout();
        });

        connect(m_canvas, &StorageTreemapCanvas::drillDownRequested, this, &StorageTreemapWidget::setVisualRoot);
        connect(m_canvas, &StorageTreemapCanvas::navigateRequested, this, &StorageTreemapWidget::navigateRequested);

        connect(m_canvas, &StorageTreemapCanvas::itemHovered, this, [this](const StorageTreemapLayoutRect *lr) {
            updateDetails(lr);
        });
        connect(m_canvas, &StorageTreemapCanvas::itemSelected, this, [this](const StorageTreemapLayoutRect *lr) {
            updateDetails(lr);
        });
    }

    void drillUp()
    {
        if (m_visualRoot && m_visualRoot->parent) {
            m_visualRoot = m_visualRoot->parent;
            updateView();
        }
    }

    void updateView()
    {
        if (!m_visualRoot) {
            m_pathLabel->setText(QString());
            m_upButton->setEnabled(false);
            m_canvas->setLayoutRects({});
            return;
        }

        m_pathLabel->setText(m_visualRoot->path);
        m_upButton->setEnabled(m_visualRoot->parent != nullptr);
        relayout();
    }

    void relayout()
    {
        if (!m_visualRoot || m_canvas->rect().width() <= 0 || m_canvas->rect().height() <= 0) {
            m_canvas->setLayoutRects({});
            return;
        }

        const QRectF bounds = m_canvas->rect().adjusted(1, 1, -1, -1);
        const auto rects = StorageTreemapLayout::layoutSquarified(m_visualRoot, bounds, m_metric);
        m_canvas->setLayoutRects(rects);
    }

    void updateDetails(const StorageTreemapLayoutRect *lr)
    {
        if (!lr || !lr->node) {
            m_detailsLabel->setText(tr("Wybierz element, aby zobaczyć szczegóły."));
            return;
        }

        const auto *node = lr->node;
        QString text = QStringLiteral("%1: %2 | Na dysku: %3 | Logiczny: %4 | Udział: %5%")
            .arg(node->isDirectory() ? tr("Katalog") : tr("Plik"))
            .arg(node->name)
            .arg(StorageScanStats::formatBytes(node->allocatedSize))
            .arg(StorageScanStats::formatBytes(node->logicalSize))
            .arg(QString::number(lr->percentageOfRoot, 'f', 1));

        if (node->isHardlink) {
            text += QStringLiteral(" | %1 (%2 aliasów)")
                .arg(node->isCanonicalHardlink ? tr("Hardlink kanoniczny") : tr("Alias"))
                .arg(node->hardlinkAliases.size());
        }

        m_detailsLabel->setText(text);
    }

    QString m_rootPath;
    StorageScanStats m_stats;
    StorageScanState m_scanState = StorageScanState::Idle;
    std::shared_ptr<StorageTreemapNode> m_treeRoot;
    const StorageTreemapNode *m_visualRoot = nullptr;
    StorageTreemapMetric m_metric = StorageTreemapMetric::Allocated;

    QPushButton *m_upButton = nullptr;
    QLabel *m_pathLabel = nullptr;
    QComboBox *m_metricCombo = nullptr;
    QLabel *m_bannerLabel = nullptr;
    StorageTreemapCanvas *m_canvas = nullptr;
    QLabel *m_detailsLabel = nullptr;
};
