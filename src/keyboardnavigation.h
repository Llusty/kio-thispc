/*
 * Shared keyboard routing for directory panes and the This PC home grid.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <QObject>
#include <QList>
#include <functional>

class QWidget;
class QKeyEvent;

class KeyboardNavigationRouter final : public QObject
{
public:
    enum class Context { None, FileView, Home };

    struct Callbacks {
        std::function<Context(QWidget *)> contextForFocus;
        std::function<void()> activateCurrent;
        std::function<void()> back;
        std::function<void()> forward;
        std::function<void()> up;
        std::function<QList<QWidget *>()> homeCards;
        std::function<QWidget *()> currentHomeCard;
        std::function<void(QWidget *)> setCurrentHomeCard;
    };

    explicit KeyboardNavigationRouter(Callbacks callbacks,
                                      QObject *parent = nullptr);

    static bool isTextInput(QWidget *widget);
    static QWidget *homeGridTarget(QWidget *current, int key,
                                   const QList<QWidget *> &cards);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool routeKey(QWidget *focus, QKeyEvent *event);
    Callbacks m_callbacks;
};
