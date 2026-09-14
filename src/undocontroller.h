/*
 * Undo/redo controller for file operations.
 *
 * Extracted during the 0.21.0 architecture refactor so ThisPcWindow no
 * longer owns KIO::FileUndoManager state or signal handling.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "localfilemovejob.h"
#include "localtreehistory.h"

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
        , m_nativeHistory(new LocalMoveHistory(this))
        , m_treeHistory(new LocalTreeHistory(this))
    {
        connect(m_nativeHistory, &LocalMoveHistory::availabilityChanged,
                this, [this](bool, bool) { updateActions(); });
        connect(m_treeHistory, &LocalTreeHistory::availabilityChanged,
                this, [this] { updateActions(); });
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
                if (completedMode == 1) {
                    m_redoKinds.push_back(false);
                } else if (completedMode == 2 && !m_redoKinds.isEmpty()) {
                    m_redoKinds.removeLast();
                }

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
        if (m_busy) {
            return;
        }

        const bool kioAvailable = kioUndoAvailable();
        const bool nativeAvailable = nativeUndoAvailable();
        const bool treeAvailable = treeUndoAvailable();
        if (treeAvailable
            && (!nativeAvailable || m_treeHistory->undoSerial() > m_nativeHistory->undoSerial())
            && (!kioAvailable || m_treeHistory->undoSerial() > m_manager->currentCommandSerialNumber())) {
            startNativeHistoryJob(m_treeHistory->undo(), 1, 2);
            return;
        }
        if (!kioAvailable && !nativeAvailable) return;
        const bool useNative = nativeAvailable
            && (!kioAvailable
                || m_nativeHistory->undoSerial()
                    > m_manager->currentCommandSerialNumber());

        if (useNative) {
            startNativeHistoryJob(m_nativeHistory->undo(), 1);
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
        if (m_busy) {
            return;
        }

        const bool kioAvailable = kioRedoAvailable();
        const bool nativeAvailable = nativeRedoAvailable();
        const bool treeAvailable = treeRedoAvailable();
        if (treeAvailable && ((!kioAvailable && !nativeAvailable)
                || (!m_redoKinds.isEmpty() && m_redoKinds.constLast() == 2))) {
            startNativeHistoryJob(m_treeHistory->redo(), 2, 2);
            return;
        }
        if (!kioAvailable && !nativeAvailable) return;
        const bool useNative = nativeAvailable
            && (!kioAvailable
                || (!m_redoKinds.isEmpty() && m_redoKinds.constLast() == 1));
        if (useNative) {
            startNativeHistoryJob(m_nativeHistory->redo(), 2);
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
            m_redoKinds.clear();
            m_manager->recordCopyJob(job);
        }
    }

    void recordNativeTransfer(
        KIO::Job *job,
        bool move)
    {
        if (!job || !m_manager) return;
        m_redoKinds.clear();
        if (auto *tree = qobject_cast<LocalTransferJob *>(job)) {
            const quint64 serial = m_manager->newCommandSerialNumber();
            connect(tree, &KJob::result, this, [this, tree, serial] {
                m_treeHistory->recordCompleted(tree, serial);
                updateActions();
            });
            return;
        }
        if (job->property("thispcOverwritesDestination").toBool()) {
            const quint64 serial = m_manager->newCommandSerialNumber();
            connect(job, &KJob::result, this, [this, serial](KJob *completed) {
                if (completed->error() != KJob::NoError
                    && !completed->property("thispcDestinationReplaced").toBool()) return;
                // We do not retain backups after success. Earlier commands
                // might refer to the replaced path and are no longer safe.
                m_irreversibleSerial = qMax(m_irreversibleSerial, serial);
                m_redoKinds.clear();
                updateActions();
            });
            return;
        }
        if (move) {
            const quint64 serial = m_manager->newCommandSerialNumber();
            if (auto *moveJob = qobject_cast<LocalFileMoveJob *>(job)) {
                m_nativeHistory->recordWhenCompleted(moveJob, serial);
            }
            return;
        }
        const quint64 serial = m_manager->newCommandSerialNumber();
        if (auto *copyJob = qobject_cast<LocalFileCopyJob *>(job)) {
            m_nativeHistory->recordWhenCompleted(copyJob, serial);
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
    bool treeUndoAvailable() const
    {
        return m_treeHistory->canUndo() && m_treeHistory->undoSerial() > m_irreversibleSerial;
    }

    bool treeRedoAvailable() const
    {
        return m_treeHistory->canRedo() && m_treeHistory->redoSerial() > m_irreversibleSerial;
    }

    bool nativeUndoAvailable() const
    {
        return m_nativeHistory->canUndo()
            && m_nativeHistory->undoSerial() > m_irreversibleSerial;
    }

    bool kioUndoAvailable() const
    {
        return m_manager && m_manager->isUndoAvailable()
            && m_manager->currentCommandSerialNumber() > m_irreversibleSerial;
    }

    bool nativeRedoAvailable() const
    {
        return m_nativeHistory->canRedo()
            && m_nativeHistory->redoSerial() > m_irreversibleSerial;
    }

    bool kioRedoAvailable() const
    {
        return m_manager && m_manager->isRedoAvailable()
            && (!m_irreversibleSerial || !m_redoKinds.isEmpty());
    }

    void startNativeHistoryJob(KJob *job, int mode, int kind = 1)
    {
        if (!job) return;
        m_busy = true;
        m_mode = mode;
        updateActions();
        showStatus(
            mode == 1
                ? trLocal("Cofanie operacji…", "Undoing operation…")
                : trLocal("Ponawianie operacji…", "Redoing operation…"),
            0);
        connect(job, &KJob::result, this, [this, mode, kind](KJob *completed) {
            m_busy = false;
            m_mode = 0;
            if (completed->error() == KJob::NoError) {
                if (mode == 1) m_redoKinds.push_back(kind);
                else if (!m_redoKinds.isEmpty()) m_redoKinds.removeLast();
                showStatus(
                    mode == 1
                        ? trLocal("Cofanie zakończone", "Undo completed")
                        : trLocal("Ponawianie zakończone", "Redo completed"),
                    4000);
            } else {
                showStatus(completed->errorString(), 7000);
            }
            if (m_refreshViews) m_refreshViews();
            if (m_updateFileActions) m_updateFileActions();
            updateActions();
        });
    }

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
            !m_busy
            && (kioUndoAvailable() || nativeUndoAvailable() || treeUndoAvailable());
        const bool canRedo =
            !m_busy
            && (kioRedoAvailable() || nativeRedoAvailable() || treeRedoAvailable());

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
            m_redoKinds.clear();
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
    LocalMoveHistory *m_nativeHistory = nullptr;
    LocalTreeHistory *m_treeHistory = nullptr;
    QList<int> m_redoKinds;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    bool m_busy = false;
    quint64 m_irreversibleSerial = 0;
    int m_mode = 0;
};
