/*
 * Large temporary preview presentation backed by PreviewPane.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "previewpane.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

class QuickLookOverlay final : public QFrame
{
public:
    explicit QuickLookOverlay(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("quickLookOverlay"));
        setFrameShape(QFrame::StyledPanel);
        setAutoFillBackground(true);
        setFocusPolicy(Qt::NoFocus);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(1, 1, 1, 1);
        layout->setSpacing(0);

        auto *header = new QFrame(this);
        header->setObjectName(QStringLiteral("quickLookHeader"));
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(12, 6, 6, 6);
        auto *hint = new QLabel(trLocal("Szybki podgląd · Spacja lub Esc, aby zamknąć",
                                        "Quick Look · Space or Esc to close"), header);
        hint->setObjectName(QStringLiteral("quickLookHint"));
        headerLayout->addWidget(hint, 1);
        m_closeButton = new QToolButton(header);
        m_closeButton->setObjectName(QStringLiteral("quickLookCloseButton"));
        m_closeButton->setText(QStringLiteral("×"));
        m_closeButton->setToolTip(trLocal("Zamknij szybki podgląd", "Close Quick Look"));
        m_closeButton->setFocusPolicy(Qt::NoFocus);
        headerLayout->addWidget(m_closeButton);
        layout->addWidget(header);

        m_preview = new PreviewPane(this);
        m_preview->setQuickLookLayout(true);
        layout->addWidget(m_preview, 1);
        hide();
    }

    PreviewPane *previewPane() const { return m_preview; }
    QToolButton *closeButton() const { return m_closeButton; }

private:
    PreviewPane *m_preview = nullptr;
    QToolButton *m_closeButton = nullptr;
};
