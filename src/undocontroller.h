/*
 * Undo/redo controller for file operations.
 *
 * Extracted during the 0.21.0 architecture refactor so ThisPcWindow no
 * longer owns KIO::FileUndoManager state or signal handling.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <KIO/CopyJob>
#include <KIO/FileUndoManager>
#include <KIO/Job>

#include <QAction>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <functional>
#include <utility>

class UndoController final : public QObject
{
public:
    using RefreshViews = std::function<void()>;
    using UpdateFileActions = std::function<void()>;
    using ShowStatus = std::function<void(const QString &, int)>;

    explicit UndoController(
        QWidget *parentWidget,
        RefreshViews refreshViews,
        UpdateFileActions updateFileActions,
        ShowStatus showStatus,
        QObject *parent = nullptr)
        : QObject(parent)
        , m_parentWidget(parentWidget)
        , m_refreshViews(std::move(refreshViews))
        , m_updateFileActions(std::move(updateFileActions))
        , m_showStatus(std::move(showStatus))
        , m_manager(KIO::FileUndoManager::self())
    {
        if (!m_manager) {
            return;
        }

        if (m_manager->uiInterface()) {
            m_manager->uiInterface()->setParentWidget(m_parentWidget);
            // thispc-view has its own status/operation UI; avoid a second
            // generic KIO progress dialog during undo/redo.
            m_manager->uiInterface()->setShowProgressInfo(false);
        }

        connect(
            m_manager,
            &KIO::FileUndoManager::undoAvailable,
            this,
            [this](bool) { updateActions(); });
        connect(
            m_manager,
            &KIO::FileUndoManager::redoAvailable,
            this,
            [this](bool) { updateActions(); });
        connect(
            m_manager,
            &KIO::FileUndoManager::undoTextChanged,
            this,
            [this](const QString &) { updateActions(); });
        connect(
            m_manager,
            &KIO::FileUndoManager::redoTextChanged,
            this,
            [this](const QString &) { updateActions(); });
        connect(
            m_manager,
            &KIO::FileUndoManager::jobRecordingFinished,
            this,
            [this](KIO::FileUndoManager::CommandType) {
                updateActions();
            });
        connect(
            m_manager,
            &KIO::FileUndoManager::undoJobFinished,
            this,
            [this] {
                const int completedMode = m_mode;
                m_mode = 0;
                m_busy = false;

                if (m_refreshViews) {
                    m_refreshViews();
                }
                if (m_updateFileActions) {
                    m_updateFileActions();
                }
                updateActions();

                if (completedMode == 1) {
                    this->showStatus(
                        trLocal(
                            "Cofanie zakończone",
                            "Undo completed"),
                        4000);
                } else if (completedMode == 2) {
                    this->showStatus(
                        trLocal(
                            "Ponawianie zakończone",
                            "Redo completed"),
                        4000);
                }
            });
    }

    void setActions(QAction *undoAction, QAction *redoAction)
    {
        m_undoAction = undoAction;
        m_redoAction = redoAction;
        updateActions();
    }

    void undo()
    {
        if (!m_manager
            || m_busy
            || !m_manager->isUndoAvailable()) {
            return;
        }

        prepareUiInterface();
        m_busy = true;
        m_mode = 1;
        updateActions();
        showStatus(
            trLocal("Cofanie operacji…", "Undoing operation…"),
            0);
        m_manager->undo();
    }

    void redo()
    {
        if (!m_manager
            || m_busy
            || !m_manager->isRedoAvailable()) {
            return;
        }

        prepareUiInterface();
        m_busy = true;
        m_mode = 2;
        updateActions();
        showStatus(
            trLocal("Ponawianie operacji…", "Redoing operation…"),
            0);
        m_manager->redo();
    }

    void recordCopyJob(KIO::CopyJob *job)
    {
        if (job && m_manager) {
            m_manager->recordCopyJob(job);
        }
    }

    void recordPutJob(const QUrl &destination, KIO::Job *job)
    {
        recordJob(
            KIO::FileUndoManager::Put,
            {},
            destination,
            job);
    }

    void recordMkdirJob(const QUrl &destination, KIO::Job *job)
    {
        recordJob(
            KIO::FileUndoManager::Mkdir,
            {},
            destination,
            job);
    }

    void recordTrashJob(const QList<QUrl> &sources, KIO::Job *job)
    {
        recordJob(
            KIO::FileUndoManager::Trash,
            sources,
            QUrl(QStringLiteral("trash:/")),
            job);
    }

private:
    void prepareUiInterface()
    {
        if (m_manager && m_manager->uiInterface()) {
            m_manager->uiInterface()->setParentWidget(m_parentWidget);
        }
    }

    void showStatus(const QString &message, int timeout)
    {
        if (m_showStatus) {
            m_showStatus(message, timeout);
        }
    }

    void updateActions()
    {
        const bool managerAvailable = m_manager != nullptr;
        const bool canUndo =
            managerAvailable
            && !m_busy
            && m_manager->isUndoAvailable();
        const bool canRedo =
            managerAvailable
            && !m_busy
            && m_manager->isRedoAvailable();

        if (m_undoAction) {
            m_undoAction->setEnabled(canUndo);

            QString text =
                managerAvailable
                    ? m_manager->undoText().trimmed()
                    : QString();
            if (text.isEmpty()) {
                text = trLocal("Cofnij", "Undo");
            }
            m_undoAction->setToolTip(
                QStringLiteral("%1 (%2)")
                    .arg(
                        text,
                        QStringLiteral("Ctrl+Z")));
        }

        if (m_redoAction) {
            m_redoAction->setEnabled(canRedo);

            QString text =
                managerAvailable
                    ? m_manager->redoText().trimmed()
                    : QString();
            if (text.isEmpty()) {
                text = trLocal("Ponów", "Redo");
            }
            m_redoAction->setToolTip(
                QStringLiteral("%1 (Ctrl+Y / Ctrl+Shift+Z)")
                    .arg(text));
        }
    }

    void recordJob(
        KIO::FileUndoManager::CommandType type,
        const QList<QUrl> &sources,
        const QUrl &destination,
        KIO::Job *job)
    {
        if (job && m_manager) {
            m_manager->recordJob(
                type,
                sources,
                destination,
                job);
        }
    }

    QWidget *m_parentWidget = nullptr;
    RefreshViews m_refreshViews;
    UpdateFileActions m_updateFileActions;
    ShowStatus m_showStatus;

    KIO::FileUndoManager *m_manager = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    bool m_busy = false;
    int m_mode = 0;
};
