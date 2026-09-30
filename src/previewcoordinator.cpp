/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "previewcoordinator.h"

#include "previewpane.h"
#include "quicklook.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>

#include <utility>

PreviewCoordinator::PreviewCoordinator(Widgets widgets,
                                       ContextProvider contextProvider,
                                       QObject *parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_contextProvider(std::move(contextProvider))
{
    if (qApp) qApp->installEventFilter(this);
}

void PreviewCoordinator::setPreviewVisible(bool visible)
{
    if (!m_widgets.previewPane) return;
    m_widgets.previewPane->setVisible(visible);
    if (visible) updatePreview();
}

void PreviewCoordinator::selectionChanged()
{
    updatePreview();
    updateQuickLook();
}

bool PreviewCoordinator::quickLookFocusIsEligible() const
{
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
    QWidget *focus = QApplication::focusWidget();
    const PaneContext context = m_contextProvider ? m_contextProvider() : PaneContext{};
    return focus && context.view
        && (focus == context.view || context.view->isAncestorOf(focus))
        && !qobject_cast<QLineEdit *>(focus)
        && !qobject_cast<QPlainTextEdit *>(focus)
        && !qobject_cast<QTextEdit *>(focus);
}

bool PreviewCoordinator::quickLookVisible() const
{
    return m_widgets.quickLook && m_widgets.quickLook->isVisible();
}

void PreviewCoordinator::setQuickLookVisible(bool visible)
{
    if (!m_widgets.quickLook) return;
    if (!visible) {
        m_widgets.quickLook->hide();
        return;
    }
    const PaneContext context = m_contextProvider ? m_contextProvider() : PaneContext{};
    if (context.items.size() != 1) return;
    repositionQuickLook();
    m_widgets.quickLook->show();
    m_widgets.quickLook->raise();
    updateQuickLook();
}

void PreviewCoordinator::updateQuickLook()
{
    if (!quickLookVisible()) return;
    const PaneContext context = m_contextProvider ? m_contextProvider() : PaneContext{};
    if (context.items.size() != 1) {
        m_widgets.quickLook->previewPane()->preview(QUrl(), false);
        return;
    }
    const PaneItem &item = context.items.first();
    m_widgets.quickLook->previewPane()->preview(item.url, item.isDir);
}

void PreviewCoordinator::updatePreview()
{
    if (!m_widgets.previewPane || !m_widgets.previewPane->isVisible()) return;
    const PaneContext context = m_contextProvider ? m_contextProvider() : PaneContext{};
    if (context.items.size() != 1) {
        m_widgets.previewPane->preview(QUrl(), false);
        return;
    }
    const PaneItem &item = context.items.first();
    m_widgets.previewPane->preview(item.url, item.isDir);
}

void PreviewCoordinator::repositionQuickLook()
{
    if (!m_widgets.quickLook || !m_widgets.geometryHost) return;
    const QRect area = m_widgets.geometryHost->rect().adjusted(36, 36, -36, -36);
    const int width = qMin(1100, qMax(420, area.width() * 4 / 5));
    const int height = qMin(760, qMax(300, area.height() * 4 / 5));
    m_widgets.quickLook->setGeometry(area.center().x() - width / 2,
                                     area.center().y() - height / 2, width, height);
}

bool PreviewCoordinator::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && m_widgets.window
        && m_widgets.window->isActiveWindow()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (quickLookVisible() && key->key() == Qt::Key_Escape
            && key->modifiers() == Qt::NoModifier) {
            setQuickLookVisible(false);
            return true;
        }
        if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier
            && !key->isAutoRepeat() && quickLookFocusIsEligible()) {
            setQuickLookVisible(!quickLookVisible());
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
