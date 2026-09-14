/*
 * Asynchronous immutable planning for the 0.23 local transfer engine.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <KJob>

#include <QAtomicInteger>
#include <QDateTime>
#include <QDir>
#include <QFileDevice>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QObject>
#include <QThread>
#include <QUrl>

#include <utility>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

inline QByteArray readLocalLink(const QString &path)
{
    QByteArray target(256, Qt::Uninitialized);
    while (true) {
        const auto size = ::readlink(QFile::encodeName(path).constData(), target.data(), target.size());
        if (size < 0) return {};
        if (size < target.size()) { target.resize(size); return target; }
        target.resize(target.size() * 2);
    }
}


enum class LocalTransferEntryType {
    Directory,
    RegularFile,
    SymbolicLink,
};

enum class LocalTransferPlanError {
    None,
    Cancelled,
    NonLocalUrl,
    SourceMissing,
    SourceRead,
    DestinationInvalid,
    UnsupportedEntry,
};

struct LocalTransferPlanEntry
{
    LocalTransferEntryType type = LocalTransferEntryType::RegularFile;
    QString sourcePath;
    QString destinationPath;
    QString relativePath;
    QString symbolicLinkTarget;
    QByteArray symbolicLinkContents;
    struct stat sourceStat {};
    qulonglong size = 0;
    QFileDevice::Permissions permissions;
    QDateTime accessed;
    QDateTime modified;
    bool destinationExists = false;
    bool destinationIsDirectory = false;
    bool destinationIsSymbolicLink = false;
    bool removeSourceDirectory = true;
    qulonglong destinationSize = 0;
};

struct LocalTransferPlan
{
    QList<LocalTransferPlanEntry> entries;
    qulonglong totalBytes = 0;
    qsizetype regularFileCount = 0;
    qsizetype directoryCount = 0;
    qsizetype symbolicLinkCount = 0;
    qsizetype conflictCount = 0;
};

Q_DECLARE_METATYPE(LocalTransferPlan)
Q_DECLARE_METATYPE(LocalTransferPlanError)


class LocalTransferPlanWorker final : public QObject
{
    Q_OBJECT

public:
    LocalTransferPlanWorker(
        QList<QUrl> sources,
        QUrl destinationDirectory,
        int entryDelayMilliseconds)
        : m_sources(std::move(sources))
        , m_destinationDirectory(std::move(destinationDirectory))
        , m_entryDelayMilliseconds(qMax(0, entryDelayMilliseconds))
    {
    }

    void requestCancel()
    {
        m_cancelled.storeRelease(true);
    }

public Q_SLOTS:
    void run()
    {
        if (!m_destinationDirectory.isLocalFile()) {
            fail(LocalTransferPlanError::NonLocalUrl,
                 tr("The destination is not a local directory."));
            return;
        }
        const QFileInfo destinationInfo(
            m_destinationDirectory.toLocalFile());
        if (!destinationInfo.exists() || !destinationInfo.isDir()) {
            fail(LocalTransferPlanError::DestinationInvalid,
                 tr("The destination directory does not exist."));
            return;
        }

        LocalTransferPlan plan;
        for (const QUrl &sourceUrl : std::as_const(m_sources)) {
            if (cancelled()) {
                fail(LocalTransferPlanError::Cancelled, {});
                return;
            }
            if (!sourceUrl.isLocalFile()) {
                fail(LocalTransferPlanError::NonLocalUrl,
                     tr("A source is not local."));
                return;
            }
            const QFileInfo sourceInfo(sourceUrl.toLocalFile());
            if (!sourceInfo.exists() && !sourceInfo.isSymLink()) {
                fail(LocalTransferPlanError::SourceMissing,
                     tr("A source item does not exist."));
                return;
            }
            const QString rootName = sourceInfo.fileName();
            if (rootName.isEmpty()
                || !appendEntry(
                    sourceInfo,
                    rootName,
                    m_destinationDirectory.toLocalFile(),
                    plan)) {
                if (cancelled()) {
                    fail(LocalTransferPlanError::Cancelled, {});
                } else if (m_sourceReadFailed) {
                    fail(LocalTransferPlanError::SourceRead,
                         tr("Could not read a source directory completely."));
                } else {
                    fail(LocalTransferPlanError::UnsupportedEntry,
                         tr("The source tree contains an unsupported item."));
                }
                return;
            }
        }
        Q_EMIT completed(plan);
    }

Q_SIGNALS:
    void entryPlanned(qsizetype count, qulonglong bytes);
    void completed(const LocalTransferPlan &plan);
    void failed(LocalTransferPlanError error, const QString &message);

private:
    bool appendEntry(
        const QFileInfo &sourceInfo,
        const QString &relativePath,
        const QString &destinationRoot,
        LocalTransferPlan &plan)
    {
        if (cancelled()) {
            return false;
        }

        LocalTransferPlanEntry entry;
        entry.sourcePath = sourceInfo.absoluteFilePath();
        if (::lstat(QFile::encodeName(entry.sourcePath).constData(), &entry.sourceStat) != 0) {
            m_sourceReadFailed = true;
            return false;
        }
        entry.relativePath = relativePath;
        entry.destinationPath =
            QDir(destinationRoot).filePath(relativePath);
        entry.permissions = sourceInfo.permissions();
        entry.accessed = sourceInfo.fileTime(QFileDevice::FileAccessTime);
        entry.modified = sourceInfo.fileTime(QFileDevice::FileModificationTime);

        if (sourceInfo.isSymLink()) {
            entry.type = LocalTransferEntryType::SymbolicLink;
            entry.symbolicLinkContents = readLocalLink(entry.sourcePath);
            if (entry.symbolicLinkContents.isEmpty()) {
                m_sourceReadFailed = true;
                return false;
            }
            entry.symbolicLinkTarget = QFile::decodeName(entry.symbolicLinkContents);
            ++plan.symbolicLinkCount;
        } else if (sourceInfo.isDir()) {
            entry.type = LocalTransferEntryType::Directory;
            ++plan.directoryCount;
        } else if (sourceInfo.isFile()) {
            entry.type = LocalTransferEntryType::RegularFile;
            entry.size = static_cast<qulonglong>(sourceInfo.size());
            plan.totalBytes += entry.size;
            ++plan.regularFileCount;
        } else {
            return false;
        }

        const QFileInfo existing(entry.destinationPath);
        entry.destinationExists = existing.exists() || existing.isSymLink();
        if (entry.destinationExists) {
            entry.destinationIsDirectory = existing.isDir();
            entry.destinationIsSymbolicLink = existing.isSymLink();
            entry.destinationSize = existing.isFile()
                ? static_cast<qulonglong>(existing.size())
                : 0;
            ++plan.conflictCount;
        }
        plan.entries.push_back(entry);
        Q_EMIT entryPlanned(plan.entries.size(), plan.totalBytes);

        if (m_entryDelayMilliseconds > 0) {
            QThread::msleep(
                static_cast<unsigned long>(m_entryDelayMilliseconds));
        }
        if (entry.type != LocalTransferEntryType::Directory) {
            return true;
        }

        const QDir directory(sourceInfo.absoluteFilePath());
        DIR *stream = ::opendir(QFile::encodeName(directory.path()).constData());
        if (!stream) {
            m_sourceReadFailed = true;
            return false;
        }
        QStringList names;
        errno = 0;
        while (const auto *child = ::readdir(stream)) {
            const QByteArray name(child->d_name);
            if (name != "." && name != "..") names.push_back(QFile::decodeName(name));
            if (cancelled()) break;
        }
        const int readError = errno;
        ::closedir(stream);
        if (readError) {
            m_sourceReadFailed = true;
            return false;
        }
        names.sort();
        for (const QString &name : std::as_const(names)) {
            const QFileInfo child(directory.filePath(name));
            if (!appendEntry(
                    child,
                    relativePath + QLatin1Char('/') + child.fileName(),
                    destinationRoot,
                    plan)) {
                return false;
            }
        }
        return true;
    }

    bool cancelled() const
    {
        return m_cancelled.loadAcquire();
    }

    void fail(LocalTransferPlanError error, const QString &message)
    {
        Q_EMIT failed(error, message);
    }

    QList<QUrl> m_sources;
    QUrl m_destinationDirectory;
    int m_entryDelayMilliseconds;
    QAtomicInteger<bool> m_cancelled = false;
    bool m_sourceReadFailed = false;
};


class LocalTransferPlanJob final : public KJob
{
    Q_OBJECT

public:
    LocalTransferPlanJob(
        QList<QUrl> sources,
        const QUrl &destinationDirectory,
        QObject *parent = nullptr,
        int entryDelayMilliseconds = 0)
        : KJob(parent)
        , m_worker(new LocalTransferPlanWorker(
              std::move(sources),
              destinationDirectory,
              entryDelayMilliseconds))
    {
        qRegisterMetaType<LocalTransferPlan>();
        qRegisterMetaType<LocalTransferPlanError>();
        setCapabilities(KJob::Killable);
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::started,
                m_worker, &LocalTransferPlanWorker::run);
        connect(m_worker, &LocalTransferPlanWorker::entryPlanned,
                this, [this](qsizetype count, qulonglong bytes) {
                    m_plannedEntryCount = count;
                    setProcessedAmount(KJob::Bytes, bytes);
                });
        connect(m_worker, &LocalTransferPlanWorker::completed,
                this, [this](const LocalTransferPlan &plan) {
                    m_plan = plan;
                    setTotalAmount(KJob::Bytes, plan.totalBytes);
                    setProcessedAmount(KJob::Bytes, plan.totalBytes);
                    finish(LocalTransferPlanError::None, {});
                });
        connect(m_worker, &LocalTransferPlanWorker::failed,
                this, [this](LocalTransferPlanError error, const QString &message) {
                    finish(error, message);
                });
        connect(m_worker, &LocalTransferPlanWorker::completed,
                &m_thread, &QThread::quit);
        connect(m_worker, &LocalTransferPlanWorker::failed,
                &m_thread, &QThread::quit);
        connect(&m_thread, &QThread::finished,
                m_worker, &QObject::deleteLater);
    }

    ~LocalTransferPlanJob() override
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

    const LocalTransferPlan &plan() const { return m_plan; }
    LocalTransferPlanError planError() const { return m_error; }
    qsizetype plannedEntryCount() const { return m_plannedEntryCount; }

protected:
    bool doKill() override
    {
        if (!m_thread.isRunning()) {
            return false;
        }
        m_finished = true;
        m_error = LocalTransferPlanError::Cancelled;
        m_worker->requestCancel();
        return true;
    }

private:
    void finish(LocalTransferPlanError error, const QString &message)
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_error = error;
        if (error != LocalTransferPlanError::None) {
            setError(error == LocalTransferPlanError::Cancelled
                    ? KJob::KilledJobError
                    : KJob::UserDefinedError);
            setErrorText(message);
        }
        emitResult();
    }

    QThread m_thread;
    LocalTransferPlanWorker *m_worker;
    LocalTransferPlan m_plan;
    LocalTransferPlanError m_error = LocalTransferPlanError::None;
    qsizetype m_plannedEntryCount = 0;
    bool m_started = false;
    bool m_finished = false;
};
