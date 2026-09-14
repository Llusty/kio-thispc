/*
 * Safe local-file move and completed-operation history for 0.23.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "localfilecopyjob.h"

#include <KJob>

#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <utility>


enum class LocalFileMoveError {
    None,
    Cancelled,
    InvalidInput,
    DestinationExists,
    AtomicMove,
    Copy,
    Verification,
    SourceRemove,
};

enum class LocalFileMoveStrategy {
    Automatic,
    ForceCopyAndRemove,
};

Q_DECLARE_METATYPE(LocalFileMoveError)


class LocalFileMoveJob final : public KIO::Job
{
    Q_OBJECT

public:
    using RemoveSource = std::function<bool(const QString &)>;

    LocalFileMoveJob(
        const QUrl &source,
        const QUrl &destination,
        QObject *parent = nullptr,
        LocalFileMoveStrategy strategy = LocalFileMoveStrategy::Automatic,
        qsizetype chunkSize = 256 * 1024,
        int chunkDelayMilliseconds = 0,
        RemoveSource removeSource = {},
        bool overwrite = false,
        LocalFileIdentity originalDestination = {})
        : KIO::Job()
        , m_source(source)
        , m_destination(destination)
        , m_strategy(strategy)
        , m_chunkSize(chunkSize)
        , m_chunkDelayMilliseconds(chunkDelayMilliseconds)
        , m_overwrite(overwrite)
        , m_originalDestination(originalDestination.valid
              ? originalDestination : LocalFileIdentity::read(destination.toLocalFile()))
        , m_removeSource(removeSource
              ? std::move(removeSource)
              : RemoveSource([](const QString &path) {
                    return QFile::remove(path);
                }))
    {
        setParent(parent);
        setProperty("thispcSourceUrls", QVariant::fromValue(QList<QUrl>{source}));
        setProperty("thispcDestinationUrl", destination);
        setProperty("thispcOverwritesDestination", overwrite);
        qRegisterMetaType<LocalFileMoveError>();
        setCapabilities(KJob::Killable | KJob::Suspendable);
    }

    void start() override
    {
        if (m_started) return;
        m_started = true;
        QTimer::singleShot(0, this, [this] { begin(); });
    }

    QUrl sourceUrl() const { return m_source; }
    QUrl destinationUrl() const { return m_destination; }
    LocalFileMoveError moveError() const { return m_moveError; }
    bool usedAtomicMove() const { return m_usedAtomicMove; }

protected:
    bool doKill() override
    {
        if (m_finished) return false;
        if (m_copyJob && !m_copyJob->kill(KJob::Quietly)) return false;
        m_finished = true;
        m_moveError = LocalFileMoveError::Cancelled;
        return true;
    }

    bool doSuspend() override
    {
        return m_copyJob && m_copyJob->suspend();
    }

    bool doResume() override
    {
        return m_copyJob && m_copyJob->resume();
    }

private:
    void begin()
    {
        if (m_finished) return;
        if (!m_source.isLocalFile() || !m_destination.isLocalFile()) {
            finish(LocalFileMoveError::InvalidInput,
                   tr("The move requires local source and destination URLs."));
            return;
        }
        const QFileInfo sourceInfo(m_source.toLocalFile());
        const QFileInfo destinationInfo(m_destination.toLocalFile());
        if (m_overwrite && !m_originalDestination.matches(m_destination.toLocalFile())) {
            finish(LocalFileMoveError::DestinationExists,
                   tr("The destination changed before the move started."));
            return;
        }
        if (!sourceInfo.exists() || !sourceInfo.isFile()
            || sourceInfo.isSymLink()
            || !QFileInfo(m_destination.toLocalFile()).dir().exists()) {
            finish(LocalFileMoveError::InvalidInput,
                   tr("The source or destination is not valid for a local file move."));
            return;
        }
        if (!m_overwrite && (destinationInfo.exists() || destinationInfo.isSymLink())) {
            finish(LocalFileMoveError::DestinationExists,
                   tr("The destination already exists."));
            return;
        }

        m_sourceSize = static_cast<qulonglong>(sourceInfo.size());
        setTotalAmount(KJob::Bytes, m_sourceSize);
        const bool sameStorage =
            QStorageInfo(sourceInfo.absolutePath()).device()
            == QStorageInfo(destinationInfo.absolutePath()).device();
        if (!m_overwrite && m_strategy == LocalFileMoveStrategy::Automatic && sameStorage) {
            m_usedAtomicMove = true;
            if (!QFile::rename(m_source.toLocalFile(), m_destination.toLocalFile())) {
                finish(LocalFileMoveError::AtomicMove,
                       tr("Could not move the file atomically."));
                return;
            }
            setProcessedAmount(KJob::Bytes, m_sourceSize);
            finish(LocalFileMoveError::None, {});
            return;
        }
        startCopyAndRemove();
    }

    void startCopyAndRemove()
    {
        m_copyJob = new LocalFileCopyJob(
            m_source,
            m_destination,
            this,
            m_chunkSize,
            m_chunkDelayMilliseconds,
            m_overwrite,
            m_overwrite,
            m_originalDestination);
        m_copyJob->setAutoDelete(false);
        connect(m_copyJob, &KJob::processedAmountChanged,
                this, [this](KJob *, KJob::Unit unit, qulonglong amount) {
                    if (unit == KJob::Bytes) {
                        setProcessedAmount(KJob::Bytes, amount);
                    }
                });
        connect(m_copyJob, &KJob::speed,
                this, [this](KJob *, unsigned long value) {
                    emitSpeed(value);
                });
        connect(m_copyJob, &KJob::result,
                this, [this](KJob *completed) {
                    if (m_finished) return;
                    if (completed->error() != KJob::NoError) {
                        finish(completed->error() == KJob::KilledJobError
                                   ? LocalFileMoveError::Cancelled
                                   : LocalFileMoveError::Copy,
                               completed->errorText());
                        return;
                    }
                    const QFileInfo sourceInfo(m_source.toLocalFile());
                    const QFileInfo destinationInfo(m_destination.toLocalFile());
                    if (!sourceInfo.exists() || !destinationInfo.exists()
                        || static_cast<qulonglong>(sourceInfo.size()) != m_sourceSize
                        || static_cast<qulonglong>(destinationInfo.size()) != m_sourceSize) {
                        const bool restored = rollbackDestination();
                        finish(LocalFileMoveError::Verification,
                               restored
                                   ? tr("The copied file could not be verified before removing the source.")
                                   : tr("Verification failed. Recovery data remains at %1.").arg(m_copyJob->partialPath()));
                        return;
                    }
                    if (!m_removeSource(m_source.toLocalFile())) {
                        const bool restored = rollbackDestination();
                        finish(LocalFileMoveError::SourceRemove,
                               restored
                                   ? tr("The source could not be removed; the copied destination was rolled back.")
                                   : tr("The source could not be removed. Recovery data remains at %1.").arg(m_copyJob->partialPath()));
                        return;
                    }
                    if (m_overwrite && !m_copyJob->commitOverwrite()) {
                        finish(LocalFileMoveError::SourceRemove,
                               tr("The file was moved, but the old destination remains at %1.").arg(m_copyJob->partialPath()));
                        return;
                    }
                    setProcessedAmount(KJob::Bytes, m_sourceSize);
                    finish(LocalFileMoveError::None, {});
                });
        m_copyJob->start();
    }

    bool rollbackDestination()
    {
        return m_overwrite ? m_copyJob->rollbackOverwrite()
                           : QFile::remove(m_destination.toLocalFile());
    }

    void finish(LocalFileMoveError error, const QString &message)
    {
        if (m_finished) return;
        m_finished = true;
        m_moveError = error;
        setProperty("thispcDestinationReplaced",
                    m_overwrite && !m_originalDestination.matches(m_destination.toLocalFile()));
        if (error == LocalFileMoveError::Cancelled) {
            setError(KJob::KilledJobError);
        } else if (error != LocalFileMoveError::None) {
            setError(KJob::UserDefinedError);
            setErrorText(message);
        }
        emitResult();
    }

    QUrl m_source;
    QUrl m_destination;
    LocalFileMoveStrategy m_strategy;
    qsizetype m_chunkSize;
    int m_chunkDelayMilliseconds;
    bool m_overwrite;
    LocalFileIdentity m_originalDestination;
    RemoveSource m_removeSource;
    LocalFileCopyJob *m_copyJob = nullptr;
    qulonglong m_sourceSize = 0;
    bool m_started = false;
    bool m_finished = false;
    bool m_usedAtomicMove = false;
    LocalFileMoveError m_moveError = LocalFileMoveError::None;
};


class LocalMoveHistory final : public QObject
{
    Q_OBJECT

public:
    struct Record {
        bool copy = false;
        QUrl source;
        QUrl destination;
        quint64 serial = 0;
        qulonglong destinationSize = 0;
        QDateTime destinationModified;
    };

    explicit LocalMoveHistory(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    bool recordWhenCompleted(LocalFileMoveJob *job, quint64 serial = 0)
    {
        if (!job) return false;
        connect(job, &KJob::result, this, [this, job, serial](KJob *completed) {
            if (completed->error() == KJob::NoError
                && !job->property("thispcOverwritesDestination").toBool()) {
                recordCompletedMove(job->sourceUrl(), job->destinationUrl(), serial);
            }
        });
        return true;
    }

    bool recordWhenCompleted(LocalFileCopyJob *job, quint64 serial = 0)
    {
        if (!job) return false;
        connect(job, &KJob::result, this, [this, job, serial](KJob *completed) {
            if (completed->error() == KJob::NoError
                && !job->property("thispcOverwritesDestination").toBool()) {
                recordCompletedCopy(job->sourceUrl(), job->destinationUrl(), serial);
            }
        });
        return true;
    }

    bool recordCompletedMove(
        const QUrl &source, const QUrl &destination, quint64 serial = 0)
    {
        if (m_busy || !source.isLocalFile() || !destination.isLocalFile()
            || QFileInfo::exists(source.toLocalFile())
            || !QFileInfo::exists(destination.toLocalFile())) {
            return false;
        }
        while (m_records.size() > m_position) m_records.removeLast();
        Record record;
        record.source = source;
        record.destination = destination;
        record.serial = serial;
        m_records.push_back(record);
        m_position = m_records.size();
        Q_EMIT availabilityChanged(canUndo(), canRedo());
        return true;
    }

    bool recordCompletedCopy(
        const QUrl &source, const QUrl &destination, quint64 serial = 0)
    {
        const QFileInfo sourceInfo(source.toLocalFile());
        const QFileInfo destinationInfo(destination.toLocalFile());
        if (m_busy || !source.isLocalFile() || !destination.isLocalFile()
            || !sourceInfo.exists() || !destinationInfo.exists()) {
            return false;
        }
        while (m_records.size() > m_position) m_records.removeLast();
        m_records.push_back({
            true,
            source,
            destination,
            serial,
            static_cast<qulonglong>(destinationInfo.size()),
            destinationInfo.lastModified()});
        m_position = m_records.size();
        Q_EMIT availabilityChanged(canUndo(), canRedo());
        return true;
    }

    bool canUndo() const { return !m_busy && m_position > 0; }
    bool canRedo() const { return !m_busy && m_position < m_records.size(); }
    qsizetype count() const { return m_records.size(); }
    qsizetype position() const { return m_position; }
    quint64 undoSerial() const
    {
        return canUndo() ? m_records.at(m_position - 1).serial : 0;
    }
    quint64 redoSerial() const
    {
        return canRedo() ? m_records.at(m_position).serial : 0;
    }

    KJob *undo()
    {
        if (!canUndo()) return nullptr;
        const qsizetype target = m_position - 1;
        const Record record = m_records.at(target);
        if (record.copy) return runCopyUndo(record, target);
        return runMove(record.destination, record.source, target);
    }

    KJob *redo()
    {
        if (!canRedo()) return nullptr;
        const qsizetype target = m_position + 1;
        const Record record = m_records.at(m_position);
        if (record.copy) return runCopyRedo(record, target);
        return runMove(record.source, record.destination, target);
    }

Q_SIGNALS:
    void availabilityChanged(bool undoAvailable, bool redoAvailable);

private:
    class CopyUndoJob final : public KIO::Job
    {
    public:
        explicit CopyUndoJob(const Record &record, QObject *parent)
            : m_record(record)
        {
            setParent(parent);
        }

        void start() override
        {
            QTimer::singleShot(0, this, [this] {
                const QFileInfo info(m_record.destination.toLocalFile());
                if (!info.exists()
                    || static_cast<qulonglong>(info.size())
                        != m_record.destinationSize
                    || qAbs(info.lastModified().msecsTo(
                           m_record.destinationModified)) > 1000
                    || !QFile::remove(m_record.destination.toLocalFile())) {
                    setError(KJob::UserDefinedError);
                    setErrorText(tr("The copied file changed and was not removed."));
                }
                emitResult();
            });
        }

    private:
        Record m_record;
    };

    KJob *runMove(
        const QUrl &source, const QUrl &destination, qsizetype successPosition)
    {
        m_busy = true;
        Q_EMIT availabilityChanged(false, false);
        auto *job = new LocalFileMoveJob(source, destination, this);
        job->setAutoDelete(false);
        connect(job, &KJob::result, this,
                [this, successPosition](KJob *completed) {
                    if (completed->error() == KJob::NoError) {
                        m_position = successPosition;
                    }
                    m_busy = false;
                    Q_EMIT availabilityChanged(canUndo(), canRedo());
                });
        job->start();
        return job;
    }

    KJob *runCopyUndo(const Record &record, qsizetype successPosition)
    {
        auto *job = new CopyUndoJob(record, this);
        return runHistoryJob(job, successPosition);
    }

    KJob *runCopyRedo(const Record &record, qsizetype successPosition)
    {
        auto *job = new LocalFileCopyJob(record.source, record.destination, this);
        job->setAutoDelete(false);
        return runHistoryJob(job, successPosition);
    }

    KJob *runHistoryJob(KJob *job, qsizetype successPosition)
    {
        m_busy = true;
        Q_EMIT availabilityChanged(false, false);
        job->setAutoDelete(false);
        connect(job, &KJob::result, this,
                [this, successPosition](KJob *completed) {
                    if (completed->error() == KJob::NoError) {
                        m_position = successPosition;
                    }
                    m_busy = false;
                    Q_EMIT availabilityChanged(canUndo(), canRedo());
                });
        job->start();
        return job;
    }

    QList<Record> m_records;
    qsizetype m_position = 0;
    bool m_busy = false;
};
