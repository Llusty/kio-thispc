/* Asynchronous, cancellable SHA-256 job for local regular files. */
#pragma once

#include "checksumdata.h"

#include <QAtomicInteger>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

struct ChecksumJobOptions
{
    ChecksumAlgorithm algorithm = ChecksumAlgorithm::Sha256;
    qsizetype chunkSize = 4 * 1024 * 1024;
    int chunkDelayMilliseconds = 0;
    bool verifyExpectedSnapshot = false;
    quint64 expectedDevice = 0;
    quint64 expectedInode = 0;
    quint64 expectedSize = 0;
    qint64 expectedMtimeSec = 0;
    qint64 expectedMtimeNsec = 0;
    qint64 expectedCtimeSec = 0;
    qint64 expectedCtimeNsec = 0;
#ifdef THISPC_TEST_HARNESS
    int forcedOpenError = 0;
#endif
};

class ChecksumWorker final : public QObject
{
    Q_OBJECT

public:
    ChecksumWorker(QString path, ChecksumJobOptions options,
                   std::shared_ptr<std::atomic<bool>> cancelToken)
        : m_path(std::move(path))
        , m_options(options)
        , m_cancelToken(std::move(cancelToken))
    {
    }

public Q_SLOTS:
    void run()
    {
        ChecksumData result;
        result.url = QUrl::fromLocalFile(m_path);
        result.capability = ChecksumCapability::SupportedLocalFile;
        result.state = ChecksumState::Running;
        result.algorithm = m_options.algorithm;

#ifdef THISPC_TEST_HARNESS
        if (m_options.forcedOpenError) {
            finishOpenFailure(result, m_options.forcedOpenError);
            return;
        }
#endif

        if (isCancelled()) {
            result.state = ChecksumState::Cancelled;
            Q_EMIT finished(result);
            return;
        }

        const QByteArray encoded = QFile::encodeName(m_path);
        const int fd = ::open(encoded.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            finishOpenFailure(result, errno);
            return;
        }

        struct stat before {};
        if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)) {
            const int error = errno;
            ::close(fd);
            result.state = ChecksumState::Failed;
            result.errorMessage = errorText(error);
            Q_EMIT finished(result);
            return;
        }

        if (m_options.verifyExpectedSnapshot) {
            const bool snapshotMatches =
                static_cast<quint64>(before.st_dev) == m_options.expectedDevice &&
                static_cast<quint64>(before.st_ino) == m_options.expectedInode &&
                static_cast<quint64>(before.st_size) == m_options.expectedSize &&
                static_cast<qint64>(before.st_mtim.tv_sec) == m_options.expectedMtimeSec &&
                static_cast<qint64>(before.st_mtim.tv_nsec) == m_options.expectedMtimeNsec &&
                static_cast<qint64>(before.st_ctim.tv_sec) == m_options.expectedCtimeSec &&
                static_cast<qint64>(before.st_ctim.tv_nsec) == m_options.expectedCtimeNsec;

            if (!snapshotMatches) {
                ::close(fd);
                result.state = ChecksumState::ChangedDuringHash;
                result.errorMessage = tr("The file was modified or replaced since it was scanned.");
                Q_EMIT finished(result);
                return;
            }
        }

        result.totalBytes = static_cast<quint64>(before.st_size);
        Q_EMIT totalBytesKnown(result.totalBytes);

        QCryptographicHash hash(toCryptographicHashAlgorithm(m_options.algorithm));
        QByteArray buffer(qMax<qsizetype>(4096, m_options.chunkSize), Qt::Uninitialized);
        bool readFailed = false;
        int readError = 0;
        while (true) {
            if (isCancelled()) {
                ::close(fd);
                result.state = ChecksumState::Cancelled;
                Q_EMIT finished(result);
                return;
            }
            const ssize_t count = ::read(fd, buffer.data(), static_cast<size_t>(buffer.size()));
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                readFailed = true;
                readError = errno;
                break;
            }
            hash.addData(QByteArrayView(buffer.constData(), count));
            result.bytesProcessed += static_cast<quint64>(count);
            Q_EMIT progress(result.bytesProcessed, result.totalBytes);
            if (m_options.chunkDelayMilliseconds > 0)
                QThread::msleep(static_cast<unsigned long>(m_options.chunkDelayMilliseconds));
        }

        struct stat afterFd {};
        const bool afterFdOk = ::fstat(fd, &afterFd) == 0;
        ::close(fd);
        struct stat afterPath {};
        const bool afterPathOk = ::lstat(encoded.constData(), &afterPath) == 0;

        const bool changed = !afterFdOk || !afterPathOk
            || !sameSnapshot(before, afterFd)
            || before.st_dev != afterPath.st_dev
            || before.st_ino != afterPath.st_ino
            || !S_ISREG(afterPath.st_mode)
            || (!readFailed && result.bytesProcessed != static_cast<quint64>(before.st_size));

        if (isCancelled()) {
            result.state = ChecksumState::Cancelled;
        } else if (changed) {
            result.state = ChecksumState::ChangedDuringHash;
            result.errorMessage = tr("The file changed while its checksum was being calculated.");
        } else if (readFailed) {
            result.state = ChecksumState::Failed;
            result.errorMessage = errorText(readError);
        } else {
            result.state = ChecksumState::Completed;
            result.digest = QString::fromLatin1(hash.result().toHex());
            if (result.algorithm == ChecksumAlgorithm::Sha256) {
                result.sha256 = result.digest;
            } else {
                result.sha256.clear();
            }
        }
        Q_EMIT finished(result);
    }

Q_SIGNALS:
    void totalBytesKnown(quint64 totalBytes);
    void progress(quint64 bytesProcessed, quint64 totalBytes);
    void finished(const ChecksumData &result);

private:
    static bool sameSnapshot(const struct stat &a, const struct stat &b)
    {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino
            && a.st_size == b.st_size
            && a.st_mtim.tv_sec == b.st_mtim.tv_sec
            && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
            && a.st_ctim.tv_sec == b.st_ctim.tv_sec
            && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
    }

    static QString errorText(int error)
    {
        return qt_error_string(error);
    }

    void finishOpenFailure(ChecksumData &result, int error)
    {
        result.algorithm = m_options.algorithm;
        result.capability = error == EACCES || error == EPERM
            ? ChecksumCapability::Unreadable
            : ChecksumCapability::SupportedLocalFile;
        result.state = ChecksumState::Failed;
        result.errorMessage = errorText(error);
        Q_EMIT finished(result);
    }

    bool isCancelled() const
    {
        return m_cancelToken && m_cancelToken->load(std::memory_order_acquire);
    }

    QString m_path;
    ChecksumJobOptions m_options;
    std::shared_ptr<std::atomic<bool>> m_cancelToken;
};

class ChecksumJob final : public QObject
{
    Q_OBJECT

public:
    explicit ChecksumJob(const QUrl &url, QObject *parent = nullptr,
                         ChecksumJobOptions options = {})
        : QObject(parent)
        , m_url(url)
        , m_options(options)
    {
        qRegisterMetaType<ChecksumData>();
        m_data.url = url;
        m_data.algorithm = options.algorithm;
        m_data.capability = capabilityForUrl(url);
    }

    ~ChecksumJob() override
    {
        cancel();
        const auto threads = std::move(m_threads);
        m_threads.clear();
        for (const QPointer<QThread> &guard : threads) {
            QThread *thread = guard.data();
            if (!thread) {
                continue;
            }
            disconnect(thread, nullptr, this, nullptr);
            if (thread->isRunning()) {
                thread->quit();
                thread->wait();
            }
            delete thread;
        }
        QCoreApplication::removePostedEvents(this);
    }

    static ChecksumCapability capabilityForUrl(const QUrl &url)
    {
        if (!url.isLocalFile()) return ChecksumCapability::RemoteUnavailable;
        struct stat value {};
        if (::lstat(QFile::encodeName(url.toLocalFile()).constData(), &value) != 0)
            return ChecksumCapability::Unreadable;
        if (S_ISLNK(value.st_mode)) return ChecksumCapability::SymlinkUnavailable;
        if (S_ISDIR(value.st_mode)) return ChecksumCapability::DirectoryNotApplicable;
        if (!S_ISREG(value.st_mode)) return ChecksumCapability::Unreadable;
        return ChecksumCapability::SupportedLocalFile;
    }

    const ChecksumData &data() const { return m_data; }
    bool isRunning() const { return m_data.state == ChecksumState::Running; }
    ChecksumAlgorithm algorithm() const { return m_options.algorithm; }
    const ChecksumJobOptions &options() const { return m_options; }

    bool start()
    {
        if (isRunning() || m_data.capability != ChecksumCapability::SupportedLocalFile)
            return false;
        const bool preCancelled = (m_runId == 0) && m_cancelled.loadAcquire();
        const quint64 runId = ++m_runId;
        m_cancelled.storeRelease(preCancelled);
        m_data.state = ChecksumState::Running;
        m_data.algorithm = m_options.algorithm;
        m_data.digest.clear();
        m_data.sha256.clear();
        m_data.errorMessage.clear();
        m_data.bytesProcessed = 0;
        m_data.totalBytes = 0;
        Q_EMIT stateChanged(m_data);

        auto cancelToken = std::make_shared<std::atomic<bool>>(preCancelled);
        m_cancelToken = cancelToken;

        auto *thread = new QThread(nullptr);
        auto *worker = new ChecksumWorker(m_url.toLocalFile(), m_options, std::move(cancelToken));
        m_threads.append(thread);
        worker->moveToThread(thread);
        connect(thread, &QThread::started, worker, &ChecksumWorker::run);
        connect(worker, &ChecksumWorker::totalBytesKnown, this, [this, runId](quint64 total) {
            if (runId != m_runId) {
                return;
            }
            m_data.totalBytes = total;
            Q_EMIT progress(0, total);
        });
        connect(worker, &ChecksumWorker::progress, this, [this, runId](quint64 done, quint64 total) {
            if (runId != m_runId) {
                return;
            }
            m_data.bytesProcessed = done;
            m_data.totalBytes = total;
            Q_EMIT progress(done, total);
        });
        // Thread and worker teardown are owned directly by worker/thread signals,
        // never dependent on ChecksumJob remaining alive until thread->finished.
        connect(worker, &ChecksumWorker::finished, thread, &QThread::quit);
        connect(worker, &ChecksumWorker::finished, worker, &QObject::deleteLater);
        connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        connect(thread, &QThread::finished, this, [this, thread] {
            m_threads.removeIf([thread](const QPointer<QThread> &entry) {
                return entry.isNull() || entry.data() == thread;
            });
        });
        connect(worker, &ChecksumWorker::finished, this, [this, runId](const ChecksumData &result) {
            if (runId != m_runId) {
                return;
            }
            m_data = result;
            m_cancelled.storeRelease(false);
            Q_EMIT stateChanged(m_data);
        });
        thread->start();
        return true;
    }

    void cancel()
    {
        if (isRunning() || m_runId == 0) {
            m_cancelled.storeRelease(true);
        }
        if (m_cancelToken) {
            m_cancelToken->store(true, std::memory_order_release);
        }
    }

Q_SIGNALS:
    void progress(quint64 bytesProcessed, quint64 totalBytes);
    void stateChanged(const ChecksumData &data);

private:
    QUrl m_url;
    ChecksumJobOptions m_options;
    ChecksumData m_data;
    quint64 m_runId = 0;
    QAtomicInteger<bool> m_cancelled = false;
    std::shared_ptr<std::atomic<bool>> m_cancelToken;
    QList<QPointer<QThread>> m_threads;
};
