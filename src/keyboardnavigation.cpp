#include "keyboardnavigation.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>
#include <limits>

KeyboardNavigationRouter::KeyboardNavigationRouter(Callbacks callbacks,
                                                   QObject *parent)
    : QObject(parent)
    , m_callbacks(std::move(callbacks))
{
}

bool KeyboardNavigationRouter::isTextInput(QWidget *widget)
{
    for (QWidget *candidate = widget; candidate; candidate = candidate->parentWidget()) {
        if (qobject_cast<QLineEdit *>(candidate)
            || qobject_cast<QTextEdit *>(candidate)
            || qobject_cast<QPlainTextEdit *>(candidate)
            || qobject_cast<QAbstractSpinBox *>(candidate)) {
            return true;
        }
        if (auto *combo = qobject_cast<QComboBox *>(candidate); combo && combo->isEditable()) {
            return true;
        }
    }
    return false;
}

QWidget *KeyboardNavigationRouter::homeGridTarget(
    QWidget *current, int key, const QList<QWidget *> &cards)
{
    if (!current || !cards.contains(current)) {
        for (QWidget *candidate : cards) {
            if (candidate && candidate->isVisible()) return candidate;
        }
        return nullptr;
    }

    const QPoint center = current->mapToGlobal(current->rect().center());
    QWidget *best = nullptr;
    qint64 bestScore = std::numeric_limits<qint64>::max();
    const bool horizontal = key == Qt::Key_Left || key == Qt::Key_Right;
    const int direction = (key == Qt::Key_Left || key == Qt::Key_Up) ? -1 : 1;
    const int rowTolerance = qMax(1, current->height() / 2);

    for (QWidget *candidate : cards) {
        if (!candidate || candidate == current || !candidate->isVisible()) continue;
        const QPoint other = candidate->mapToGlobal(candidate->rect().center());
        const int primary = horizontal ? other.x() - center.x() : other.y() - center.y();
        const int cross = horizontal ? other.y() - center.y() : other.x() - center.x();
        if (primary * direction <= 0) continue;
        if (horizontal && qAbs(cross) > rowTolerance) continue;

        // Prefer the adjacent visual row/column, then the closest card within it.
        const qint64 score = qint64(qAbs(primary)) * 10000 + qAbs(cross);
        if (score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    // A directional boundary is a no-op. Returning the current card makes the
    // invariant explicit and prevents callers from translating an invalid
    // target into a cleared logical current item.
    return best ? best : current;
}

bool KeyboardNavigationRouter::routeKey(QWidget *focus, QKeyEvent *event)
{
    if (!focus || QApplication::activeModalWidget()
        || QApplication::activePopupWidget() || isTextInput(focus)) {
        return false;
    }
    for (QWidget *candidate = focus; candidate; candidate = candidate->parentWidget()) {
        if (qobject_cast<QMenu *>(candidate) || qobject_cast<QDialog *>(candidate)) {
            return false;
        }
    }

    const Context context = m_callbacks.contextForFocus
        ? m_callbacks.contextForFocus(focus) : Context::None;
    if (context == Context::None) return false;

    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool controlOnly = modifiers == Qt::ControlModifier;
    const bool shiftedControl = modifiers
        == (Qt::ControlModifier | Qt::ShiftModifier);
    if (context == Context::FileView && (controlOnly || shiftedControl)) {
        if ((event->key() == Qt::Key_Plus && (controlOnly || shiftedControl))
            || (event->key() == Qt::Key_Equal && controlOnly)) {
            if (m_callbacks.adjustIconSizeStep) m_callbacks.adjustIconSizeStep(1);
            return true;
        }
        if (event->key() == Qt::Key_Minus && controlOnly) {
            if (m_callbacks.adjustIconSizeStep) m_callbacks.adjustIconSizeStep(-1);
            return true;
        }
    }
    if (context == Context::FileView && modifiers == Qt::NoModifier
        && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        if (m_callbacks.activateCurrent) m_callbacks.activateCurrent();
        return true;
    }
    if (modifiers == Qt::NoModifier && event->key() == Qt::Key_Backspace) {
        if (m_callbacks.up) m_callbacks.up();
        return true;
    }
    if (modifiers == Qt::AltModifier) {
        if (event->key() == Qt::Key_Left) {
            if (m_callbacks.back) m_callbacks.back();
            return true;
        }
        if (event->key() == Qt::Key_Right) {
            if (m_callbacks.forward) m_callbacks.forward();
            return true;
        }
        if (event->key() == Qt::Key_Up) {
            if (m_callbacks.up) m_callbacks.up();
            return true;
        }
    }
    if (context == Context::Home && modifiers == Qt::NoModifier
        && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right
            || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        const QList<QWidget *> cards = m_callbacks.homeCards
            ? m_callbacks.homeCards() : QList<QWidget *>{};
        QWidget *current = m_callbacks.currentHomeCard
            ? m_callbacks.currentHomeCard() : focus;
        QWidget *target = homeGridTarget(current, event->key(), cards);
        if (target) {
            if (m_callbacks.setCurrentHomeCard) m_callbacks.setCurrentHomeCard(target);
            target->setFocus(Qt::TabFocusReason);
        }
        return true;
    }
    return false;
}

bool KeyboardNavigationRouter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress) return QObject::eventFilter(watched, event);
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    QWidget *focus = QApplication::focusWidget();
    if (!focus) focus = qobject_cast<QWidget *>(watched);
    if (!routeKey(focus, keyEvent)) return QObject::eventFilter(watched, event);
    keyEvent->accept();
    return true;
}
