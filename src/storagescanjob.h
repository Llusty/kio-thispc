/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "storagescandata.h"
#include "storagescanworker.h"

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>

class StorageScanJob final : public QObject
{
    Q_OBJECT

public:
    explicit StorageScanJob(StorageScanOptions options, QObject *parent = nullptr)
        : QObject(parent)
        , m_options(std::move(options))
    {
        qRegisterMetaType<StorageScanState>();
        qRegisterMetaType<StorageScanStats>();
        qRegisterMetaType<StorageScanEntry>();
        qRegisterMetaType<QList<StorageScanEntry>>();
    }

    ~StorageScanJob() override
    {
        cancel();
        if (m_thread) {
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
        }
    }

    const StorageScanOptions &options() const { return m_options; }
    StorageScanState state() const { return m_state; }
    const StorageScanStats &stats() const { return m_stats; }
    const QList<StorageScanEntry> &entries() const { return m_entries; }
    bool isRunning() const { return m_state == StorageScanState::Running; }

    static bool isRemoteUrl(const QUrl &url)
    {
        if (!url.isValid()) return false;
        if (url.isLocalFile()) return false;
        if (url.scheme().isEmpty()) return false;
        if (url.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) return false;
        return true;
    }

    bool start()
    {
        if (m_state == StorageScanState::Running) {
            return false;
        }

        // Check remote exclusion
        if (m_options.rootUrl.isValid() && isRemoteUrl(m_options.rootUrl)) {
            m_state = StorageScanState::RemoteUnsupported;
            Q_EMIT stateChanged(m_state);
            Q_EMIT finished(m_state, m_stats);
            return false;
        }

        if (!m_options.rootPath.isEmpty()) {
            const QUrl asUrl(m_options.rootPath);
            if (asUrl.isValid() && isRemoteUrl(asUrl)) {
                m_state = StorageScanState::RemoteUnsupported;
                Q_EMIT stateChanged(m_state);
                Q_EMIT finished(m_state, m_stats);
                return false;
            }
        }

        if (m_thread) {
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
        }
        if (m_state != StorageScanState::Idle) {
            // Preserve cancel() called before the initial start(); reset for subsequent runs.
            m_cancelFlag = std::make_shared<std::atomic<bool>>(false);
        }

        m_state = StorageScanState::Running;
        m_stats = StorageScanStats{};
        m_entries.clear();
        Q_EMIT stateChanged(m_state);

        auto *thread = new QThread(this);
        auto *worker = new StorageScanWorker(m_options, m_cancelFlag);
        m_thread = thread;
        worker->moveToThread(thread);

        connect(thread, &QThread::started, worker, &StorageScanWorker::run);
        connect(worker, &StorageScanWorker::progress, this, [this](const StorageScanStats &stats, const QString &currentPath) {
            m_stats = stats;
            Q_EMIT progress(stats, currentPath);
        });
        connect(worker, &StorageScanWorker::finished, thread, &QThread::quit);
        connect(worker, &StorageScanWorker::finished, worker, &QObject::deleteLater);
        connect(worker, &StorageScanWorker::finished, this, [this](StorageScanState state, const StorageScanStats &stats, const QList<StorageScanEntry> &entries) {
            m_state = state;
            m_stats = stats;
            m_entries = entries;
            Q_EMIT progress(m_stats, QString());
            Q_EMIT stateChanged(m_state);
            Q_EMIT finished(m_state, m_stats);
        });
        connect(thread, &QThread::finished, this, [this, thread] {
            if (m_thread == thread) {
                m_thread = nullptr;
            }
            thread->deleteLater();
        });

        thread->start();
        return true;
    }

    void cancel()
    {
        // Shared atomic token decouples cancellation from StorageScanWorker's thread-affine lifetime.
        m_cancelFlag->store(true, std::memory_order_release);
    }

Q_SIGNALS:
    void stateChanged(StorageScanState state);
    void progress(const StorageScanStats &stats, const QString &currentPath);
    void finished(StorageScanState state, const StorageScanStats &stats);

private:
    StorageScanOptions m_options;
    StorageScanState m_state = StorageScanState::Idle;
    StorageScanStats m_stats;
    QList<StorageScanEntry> m_entries;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag{std::make_shared<std::atomic<bool>>(false)};
    QPointer<QThread> m_thread;
};
