/*
 * Chunked local-file copy primitive for the 0.23 transfer engine.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "localfileoverwrite.h"
#include <KIO/Job>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QThread>
#include <QUrl>
#include <QWaitCondition>

#include <cerrno>
#include <limits>
#include <utility>


enum class LocalTransferError {
    None,
    Cancelled,
    SourceOpen,
    SourceRead,
    DestinationExists,
    DestinationChanged,
    PartialExists,
    DestinationCreate,
    DestinationWrite,
    DiskFull,
    Metadata,
    Publish,
};

Q_DECLARE_METATYPE(LocalTransferError)


class LocalFileCopyWorker final : public QObject
{
    Q_OBJECT

public:
    LocalFileCopyWorker(
        QString sourcePath,
        QString destinationPath,
        qsizetype chunkSize,
        int chunkDelayMilliseconds,
        bool overwrite,
        bool retainOverwrittenDestination,
        LocalFileIdentity originalDestination)
        : m_sourcePath(std::move(sourcePath))
        , m_destinationPath(std::move(destinationPath))
        , m_partialPath(m_destinationPath + QStringLiteral(".thispc-part"))
        , m_chunkSize(qMax<qsizetype>(4096, chunkSize))
        , m_chunkDelayMilliseconds(qMax(0, chunkDelayMilliseconds))
        , m_overwrite(overwrite)
        , m_retainOverwrittenDestination(retainOverwrittenDestination)
        , m_originalDestination(originalDestination)
    {
    }

    QString partialPath() const
    {
        return m_partialPath;
    }

    static LocalTransferError classifyWriteError(int errorNumber)
    {
        return errorNumber == ENOSPC || errorNumber == EDQUOT
            ? LocalTransferError::DiskFull
            : LocalTransferError::DestinationWrite;
    }

    bool requestPause(qulonglong *pausedBytes)
    {
        QMutexLocker locker(&m_mutex);
        if (m_finished || m_cancelRequested) {
            return false;
        }
        m_pauseRequested = true;
        while (!m_pauseAcknowledged && !m_finished) {
            m_stateChanged.wait(&m_mutex);
        }
        if (pausedBytes) {
            *pausedBytes = m_processedBytes;
        }
        return m_pauseAcknowledged;
    }

    bool requestResume()
    {
        QMutexLocker locker(&m_mutex);
        if (m_finished || !m_pauseRequested) {
            return false;
        }
        m_pauseRequested = false;
        m_pauseAcknowledged = false;
        m_stateChanged.wakeAll();
        return true;
    }

    bool requestCancel()
    {
        QMutexLocker locker(&m_mutex);
        if (m_finished) return false;
        m_cancelRequested = true;
        m_pauseRequested = false;
        m_stateChanged.wakeAll();
        return true;
    }

public Q_SLOTS:
    void run()
    {
        QFile source(m_sourcePath);
        QFile destination(m_partialPath);
        const QFileInfo sourceInfo(m_sourcePath);
        if (!sourceInfo.isFile() || !source.open(QIODevice::ReadOnly)) {
            finishWithError(
                LocalTransferError::SourceOpen,
                tr("Could not open the source file."));
            return;
        }
        if (m_overwrite
            && (!m_originalDestination.matches(m_destinationPath)
                || m_originalDestination.sameFile(LocalFileIdentity::read(m_sourcePath)))) {
            finishWithError(LocalTransferError::DestinationChanged,
                            tr("The destination changed or is the source file."));
            return;
        }
        if (!m_overwrite && QFileInfo::exists(m_destinationPath)) {
            finishWithError(
                LocalTransferError::DestinationExists,
                tr("The destination already exists."));
            return;
        }
        if (QFileInfo::exists(m_partialPath)) {
            finishWithError(
                LocalTransferError::PartialExists,
                tr("A partial destination already exists."));
            return;
        }
        if (!destination.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            finishWithError(
                LocalTransferError::DestinationCreate,
                tr("Could not create the partial destination."));
            return;
        }

        const qulonglong total = static_cast<qulonglong>(source.size());
        Q_EMIT totalBytesKnown(total);
        QElapsedTimer speedTimer;
        speedTimer.start();
        qulonglong processed = 0;
        qulonglong speedCheckpoint = 0;

        while (true) {
            {
                QMutexLocker locker(&m_mutex);
                while (m_pauseRequested && !m_cancelRequested) {
                    m_pauseAcknowledged = true;
                    m_stateChanged.wakeAll();
                    m_stateChanged.wait(&m_mutex);
                }
                m_pauseAcknowledged = false;
                if (m_cancelRequested) {
                    break;
                }
            }

            const QByteArray chunk = source.read(m_chunkSize);
            if (chunk.isEmpty()) {
                if (source.error() != QFile::NoError) {
                    destination.close();
                    QFile::remove(m_partialPath);
                    finishWithError(
                        LocalTransferError::SourceRead,
                        source.errorString());
                    return;
                }
                break;
            }

            qint64 written = 0;
            while (written < chunk.size()) {
                const qint64 amount = destination.write(
                    chunk.constData() + written,
                    chunk.size() - written);
                if (amount <= 0) {
                    const int writeErrorNumber = errno;
                    const QString error = destination.errorString();
                    destination.close();
                    QFile::remove(m_partialPath);
                    finishWithError(
                        classifyWriteError(writeErrorNumber),
                        error);
                    return;
                }
                written += amount;
            }
            processed += static_cast<qulonglong>(chunk.size());
            {
                QMutexLocker locker(&m_mutex);
                m_processedBytes = processed;
            }
            Q_EMIT processedBytesChanged(processed);

            const qint64 elapsed = speedTimer.elapsed();
            if (elapsed >= 250) {
                const qulonglong bytes = processed - speedCheckpoint;
                Q_EMIT speedChanged(
                    static_cast<unsigned long>(qMin<qulonglong>(
                        bytes * 1000 / static_cast<qulonglong>(elapsed),
                        std::numeric_limits<unsigned long>::max())));
                speedCheckpoint = processed;
                speedTimer.restart();
            }
            if (m_chunkDelayMilliseconds > 0) {
                QThread::msleep(
                    static_cast<unsigned long>(m_chunkDelayMilliseconds));
            }
        }

        if (cancelRequested()) {
            destination.close();
            source.close();
            QFile::remove(m_partialPath);
            markFinished();
            Q_EMIT cancelled();
            return;
        }
        const bool metadataApplied = destination.flush()
            && destination.setPermissions(sourceInfo.permissions())
            && applyFileTime(
                destination,
                sourceInfo.fileTime(QFileDevice::FileAccessTime),
                QFileDevice::FileAccessTime)
            && applyFileTime(
                destination,
                sourceInfo.fileTime(QFileDevice::FileModificationTime),
                QFileDevice::FileModificationTime);
        destination.close();
        source.close();
        if (!metadataApplied) {
            QFile::remove(m_partialPath);
            finishWithError(
                LocalTransferError::Metadata,
                tr("Could not preserve file metadata."));
            return;
        }
        QMutexLocker publicationLocker(&m_mutex);
        if (m_cancelRequested) {
            publicationLocker.unlock();
            QFile::remove(m_partialPath);
            markFinished();
            Q_EMIT cancelled();
            return;
        }
        if (m_overwrite) {
            if (!m_originalDestination.matches(m_destinationPath)
                || !exchangeLocalFiles(m_partialPath, m_destinationPath)) {
                QFile::remove(m_partialPath);
                publicationLocker.unlock();
                finishWithError(LocalTransferError::Publish,
                                tr("Could not safely replace the destination; the existing file was retained."));
                return;
            }
            // Verify the exchanged entry too, so a change between the last
            // identity check and exchange is not silently discarded.
            if (!m_originalDestination.matches(m_partialPath)
                || (!m_retainOverwrittenDestination && !QFile::remove(m_partialPath))) {
                const bool restored = exchangeLocalFiles(m_partialPath, m_destinationPath);
                if (restored) QFile::remove(m_partialPath);
                publicationLocker.unlock();
                finishWithError(LocalTransferError::Publish,
                                restored
                                    ? tr("Could not finish replacement; the existing file was restored.")
                                    : tr("Could not restore the destination. Recovery data remains at %1.").arg(m_partialPath));
                return;
            }
        } else if (!QFile::rename(m_partialPath, m_destinationPath)) {
            QFile::remove(m_partialPath);
            publicationLocker.unlock();
            finishWithError(
                LocalTransferError::Publish,
                tr("Could not publish the completed file."));
            return;
        }
        m_finished = true;
        m_pauseAcknowledged = false;
        m_stateChanged.wakeAll();
        publicationLocker.unlock();
        Q_EMIT completed();
    }

Q_SIGNALS:
    void totalBytesKnown(qulonglong bytes);
    void processedBytesChanged(qulonglong bytes);
    void speedChanged(unsigned long bytesPerSecond);
    void completed();
    void cancelled();
    void failed(LocalTransferError error, const QString &message);

private:
    bool cancelRequested()
    {
        QMutexLocker locker(&m_mutex);
        return m_cancelRequested;
    }

    void markFinished()
    {
        QMutexLocker locker(&m_mutex);
        m_finished = true;
        m_pauseAcknowledged = false;
        m_stateChanged.wakeAll();
    }

    static bool applyFileTime(
        QFile &file,
        const QDateTime &time,
        QFileDevice::FileTime type)
    {
        return !time.isValid() || file.setFileTime(time, type);
    }

    void finishWithError(LocalTransferError error, const QString &message)
    {
        markFinished();
        Q_EMIT failed(error, message);
    }

    QString m_sourcePath;
    QString m_destinationPath;
    QString m_partialPath;
    qsizetype m_chunkSize;
    int m_chunkDelayMilliseconds;
    bool m_overwrite;
    bool m_retainOverwrittenDestination;
    LocalFileIdentity m_originalDestination;
    QMutex m_mutex;
    QWaitCondition m_stateChanged;
    bool m_pauseRequested = false;
    bool m_pauseAcknowledged = false;
    bool m_cancelRequested = false;
    bool m_finished = false;
    qulonglong m_processedBytes = 0;
};


class LocalFileCopyJob final : public KIO::Job
{
    Q_OBJECT

public:
    LocalFileCopyJob(
        const QUrl &source,
        const QUrl &destination,
        QObject *parent = nullptr,
        qsizetype chunkSize = 256 * 1024,
        int chunkDelayMilliseconds = 0,
        bool overwrite = false,
        bool retainOverwrittenDestination = false,
        LocalFileIdentity originalDestination = {})
        : KIO::Job()
        , m_source(source)
        , m_destination(destination)
        , m_partialPath(
              destination.toLocalFile() + QStringLiteral(".thispc-part"))
        , m_overwrite(overwrite)
        , m_originalDestination(originalDestination.valid
              ? originalDestination : LocalFileIdentity::read(destination.toLocalFile()))
        , m_worker(new LocalFileCopyWorker(
              source.toLocalFile(),
              destination.toLocalFile(),
              chunkSize,
              chunkDelayMilliseconds,
              overwrite,
              retainOverwrittenDestination,
              m_originalDestination))
    {
        setParent(parent);
        setProperty("thispcSourceUrls", QVariant::fromValue(QList<QUrl>{source}));
        setProperty("thispcDestinationUrl", destination);
        setProperty("thispcOverwritesDestination", overwrite);
        qRegisterMetaType<LocalTransferError>();
        setCapabilities(KJob::Killable | KJob::Suspendable);
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::started,
                m_worker, &LocalFileCopyWorker::run);
        connect(m_worker, &LocalFileCopyWorker::totalBytesKnown,
                this, [this](qulonglong bytes) {
                    setTotalAmount(KJob::Bytes, bytes);
                });
        connect(m_worker, &LocalFileCopyWorker::processedBytesChanged,
                this, [this](qulonglong bytes) {
                    if (bytes > processedAmount(KJob::Bytes)) {
                        setProcessedAmount(KJob::Bytes, bytes);
                    }
                });
        connect(m_worker, &LocalFileCopyWorker::speedChanged,
                this, [this](unsigned long speed) { emitSpeed(speed); });
        connect(m_worker, &LocalFileCopyWorker::completed,
                this, [this] {
                    m_publishedDestination = LocalFileIdentity::read(m_destination.toLocalFile());
                    finish(false, LocalTransferError::None, {});
                });
        connect(m_worker, &LocalFileCopyWorker::cancelled,
                this, [this] {
                    finish(true, LocalTransferError::Cancelled, {});
                });
        connect(m_worker, &LocalFileCopyWorker::failed,
                this, [this](LocalTransferError error, const QString &message) {
                    finish(false, error, message);
                });
        connect(m_worker, &LocalFileCopyWorker::completed,
                &m_thread, &QThread::quit);
        connect(m_worker, &LocalFileCopyWorker::cancelled,
                &m_thread, &QThread::quit);
        connect(m_worker, &LocalFileCopyWorker::failed,
                &m_thread, &QThread::quit);
        connect(&m_thread, &QThread::finished,
                m_worker, &QObject::deleteLater);
    }

    ~LocalFileCopyJob() override
    {
        if (m_thread.isRunning()) {
            m_worker->requestCancel();
            m_thread.quit();
            m_thread.wait();
        }
    }

    void start() override
    {
        if (m_started) {
            return;
        }
        m_started = true;
        m_thread.start();
    }

    QUrl sourceUrl() const { return m_source; }
    QUrl destinationUrl() const { return m_destination; }
    QString partialPath() const { return m_partialPath; }
    LocalTransferError transferError() const { return m_transferError; }

    bool commitOverwrite()
    {
        return !m_overwrite
            || (m_originalDestination.matches(m_partialPath) && QFile::remove(m_partialPath));
    }

    bool rollbackOverwrite()
    {
        return m_overwrite && m_originalDestination.matches(m_partialPath)
            && m_publishedDestination.matches(m_destination.toLocalFile())
            && exchangeLocalFiles(m_partialPath, m_destination.toLocalFile())
            && QFile::remove(m_partialPath);
    }

protected:
    bool doKill() override
    {
        if (!m_thread.isRunning()) {
            return false;
        }
        if (!m_worker->requestCancel()) return false;
        m_finished = true;
        m_transferError = LocalTransferError::Cancelled;
        return true;
    }

    bool doSuspend() override
    {
        if (!m_thread.isRunning()) {
            return false;
        }
        qulonglong pausedBytes = 0;
        if (!m_worker->requestPause(&pausedBytes)) {
            return false;
        }
        if (pausedBytes > processedAmount(KJob::Bytes)) {
            setProcessedAmount(KJob::Bytes, pausedBytes);
        }
        return true;
    }

    bool doResume() override
    {
        return m_thread.isRunning() && m_worker->requestResume();
    }

private:
    void finish(
        bool wasCancelled,
        LocalTransferError transferError,
        const QString &message)
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        setProperty("thispcDestinationReplaced",
                    m_overwrite && !m_originalDestination.matches(m_destination.toLocalFile()));
        m_transferError = wasCancelled
            ? LocalTransferError::Cancelled
            : transferError;
        if (!message.isEmpty()) {
            setError(KJob::UserDefinedError);
            setErrorText(message);
        } else if (wasCancelled) {
            setError(KJob::KilledJobError);
        }
        emitResult();
    }

    QUrl m_source;
    QUrl m_destination;
    QString m_partialPath;
    bool m_overwrite;
    LocalFileIdentity m_originalDestination;
    LocalFileIdentity m_publishedDestination;
    QThread m_thread;
    LocalFileCopyWorker *m_worker;
    bool m_started = false;
    bool m_finished = false;
    LocalTransferError m_transferError = LocalTransferError::None;
};
