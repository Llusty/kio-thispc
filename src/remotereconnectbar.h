/* SPDX-License-Identifier: MIT */
#pragma once
#include "browsercommon.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

// A small pane-local notice, also visible when the retained view is This PC.
class RemoteReconnectBar final : public QWidget
{
public:
    RemoteReconnectBar(QWidget *parent, std::function<void(const QUrl &)> retry)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("remoteReconnectBar"));
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(8, 4, 8, 4);
        m_message = new QLabel(this);
        m_message->setTextFormat(Qt::PlainText);
        m_message->setWordWrap(true);
        row->addWidget(m_message, 1);
        m_retry = new QPushButton(themedIcon(QStringLiteral("view-refresh")),
            trLocal("Połącz ponownie", "Reconnect"), this);
        row->addWidget(m_retry);
        connect(m_retry, &QPushButton::clicked, this, [this, retry] {
            const QUrl target = m_url;
            retry(target);
        });
        hide();
    }
    void showFailure(const QUrl &url, const QString &message)
    {
        m_url = normalizedUrl(url);
        m_message->setText(RemoteUrlHelper::sanitizeErrorMessage(message, url));
        show();
    }
    QUrl targetUrl() const { return m_url; }
    QPushButton *retryButton() const { return m_retry; }
private:
    QUrl m_url;
    QLabel *m_message = nullptr;
    QPushButton *m_retry = nullptr;
};
