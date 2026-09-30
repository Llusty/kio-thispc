/*
 * Coordinates the Preview pane and Quick Look presentation.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "panecontext.h"

#include <QObject>

#include <functional>

class PreviewPane;
class QuickLookOverlay;
class QWidget;

class PreviewCoordinator final : public QObject
{
public:
    struct Widgets {
        QWidget *window = nullptr;
        QWidget *geometryHost = nullptr;
        PreviewPane *previewPane = nullptr;
        QuickLookOverlay *quickLook = nullptr;
    };

    using ContextProvider = std::function<PaneContext()>;

    PreviewCoordinator(Widgets widgets, ContextProvider contextProvider,
                       QObject *parent = nullptr);

    void setPreviewVisible(bool visible);
    void selectionChanged();
    void setQuickLookVisible(bool visible);
    void repositionQuickLook();

    bool quickLookFocusIsEligible() const;
    bool quickLookVisible() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updatePreview();
    void updateQuickLook();

    Widgets m_widgets;
    ContextProvider m_contextProvider;
};
