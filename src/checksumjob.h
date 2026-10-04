/* Asynchronous, cancellable SHA-256 job for local regular files. */
#pragma once

#include "checksumdata.h"

#include <QAtomicInteger>
#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
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
    ChecksumWorker(QString path, ChecksumJobOptions options)
        : m_path(std::move(path))
        , m_options(options)
    {
    }

    void requestCancel()
    {
        m_cancelled.storeRelease(true);
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

        const QByteArray encoded = QFile::encodeName(m_path);
        const int fd = ::open(encoded.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
            if (m_cancelled.loadAcquire()) {
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

        if (m_cancelled.loadAcquire()) {
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
        return QString::fromLocal8Bit(std::strerror(error));
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

    QString m_path;
    ChecksumJobOptions m_options;
    QAtomicInteger<bool> m_cancelled = false;
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
        if (m_thread && m_thread->isRunning()) {
            m_thread->quit();
            m_thread->wait();
        }
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
        m_data.state = ChecksumState::Running;
        m_data.algorithm = m_options.algorithm;
        m_data.digest.clear();
        m_data.sha256.clear();
        m_data.errorMessage.clear();
        m_data.bytesProcessed = 0;
        m_data.totalBytes = 0;
        Q_EMIT stateChanged(m_data);

        auto *thread = new QThread(this);
        auto *worker = new ChecksumWorker(m_url.toLocalFile(), m_options);
        m_thread = thread;
        m_worker = worker;
        worker->moveToThread(thread);
        connect(thread, &QThread::started, worker, &ChecksumWorker::run);
        connect(worker, &ChecksumWorker::totalBytesKnown, this, [this](quint64 total) {
            m_data.totalBytes = total;
            Q_EMIT progress(0, total);
        });
        connect(worker, &ChecksumWorker::progress, this, [this](quint64 done, quint64 total) {
            m_data.bytesProcessed = done;
            m_data.totalBytes = total;
            Q_EMIT progress(done, total);
        });
        if (m_cancelled.loadAcquire()) {
            worker->requestCancel();
        }
        connect(worker, &ChecksumWorker::finished, this, [this, thread](const ChecksumData &result) {
            m_data = result;
            m_cancelled.storeRelease(false);
            Q_EMIT stateChanged(m_data);
            thread->quit();
        });
        connect(worker, &ChecksumWorker::finished, worker, &QObject::deleteLater);
        connect(thread, &QThread::finished, this, [this, thread] {
            if (m_thread == thread) {
                m_thread = nullptr;
                m_worker = nullptr;
            }
            thread->deleteLater();
        });
        thread->start();
        return true;
    }

    void cancel()
    {
        m_cancelled.storeRelease(true);
        if (m_worker) m_worker->requestCancel();
    }

Q_SIGNALS:
    void progress(quint64 bytesProcessed, quint64 totalBytes);
    void stateChanged(const ChecksumData &data);

private:
    QUrl m_url;
    ChecksumJobOptions m_options;
    ChecksumData m_data;
    QAtomicInteger<bool> m_cancelled = false;
    QPointer<QThread> m_thread;
    QPointer<ChecksumWorker> m_worker;
};
