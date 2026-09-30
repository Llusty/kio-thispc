/* Executes immutable local transfer plans without blocking directory scans.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "localfilemovejob.h"
#include "localtransferplan.h"
#include <QPointer>
#include <QSet>
#include <KIO/CopyJob>
#include <functional>

class LocalTransferJob final : public KIO::Job
{
    Q_OBJECT
public:
    LocalTransferJob(QList<QUrl> sources, QUrl destinationDirectory, bool move,
                     QObject *parent = nullptr, qsizetype chunkSize = 256 * 1024,
                     int chunkDelayMilliseconds = 0)
        : m_sources(std::move(sources))
        , m_destinationDirectory(std::move(destinationDirectory))
        , m_move(move)
        , m_chunkSize(chunkSize)
        , m_chunkDelayMilliseconds(chunkDelayMilliseconds)
    {
        setParent(parent);
        setCapabilities(KJob::Killable | KJob::Suspendable);
        setProperty("thispcSourceUrls", QVariant::fromValue(m_sources));
        setProperty("thispcDestinationUrl", m_destinationDirectory);
        connect(this, &LocalTransferJob::currentFileChanged, this,
                [this](const QUrl &source, const QUrl &destination) {
                    setProperty("thispcCurrentSourceUrl", source);
                    setProperty("thispcCurrentDestinationUrl", destination);
                    Q_EMIT description(this, {}, {}, {});
                });
    }

    LocalTransferJob(LocalTransferPlan plan, bool move, QObject *parent)
        : LocalTransferJob({}, {}, move, parent)
    {
        m_plan = std::move(plan);
        m_preplanned = true;
    }

    void setFallbackFactory(std::function<KIO::CopyJob *()> factory)
    {
        m_fallbackFactory = std::move(factory);
    }

    bool usedKio() const { return m_usedKio; }

    void start() override
    {
        if (m_started) return;
        m_started = true;
        QTimer::singleShot(0, this, [this] { plan(); });
    }

    const LocalTransferPlan &transferPlan() const { return m_plan; }
    const QList<LocalTransferPlanEntry> &completedEntries() const { return m_completedEntries; }
    bool movesSources() const { return m_move; }

Q_SIGNALS:
    void currentFileChanged(const QUrl &source, const QUrl &destination);

protected:
    bool doSuspend() override
    {
        return !m_finished && (!m_activeJob || m_activeJob->suspend());
    }

    bool doResume() override
    {
        if (m_finished || (m_activeJob && !m_activeJob->resume())) return false;
        QTimer::singleShot(0, this, [this] {
            if (m_fallbackPending) { m_fallbackPending = false; startFallback(); }
            else next();
        });
        return true;
    }

    bool doKill() override
    {
        if (m_finished || (m_activeJob && !m_activeJob->kill(KJob::Quietly))) return false;
        m_finished = true;
        if (m_planner) m_planner->kill(KJob::Quietly);
        return true;
    }

private:
    void plan()
    {
        if (m_finished) return;
        if (m_preplanned) {
            if (!validatePlan()) return;
            setTotalAmount(KJob::Bytes, m_plan.totalBytes);
            m_planned = true;
            scheduleNext();
            return;
        }
        if (m_sources.isEmpty() || !m_destinationDirectory.isLocalFile()) {
            fail(tr("The transfer requires local sources and a local destination."));
            return;
        }
        const QString destination = QFileInfo(m_destinationDirectory.toLocalFile()).canonicalFilePath();
        for (const QUrl &source : std::as_const(m_sources)) {
            const QFileInfo info(source.toLocalFile());
            const QString canonical = info.canonicalFilePath();
            if (!source.isLocalFile()
                || (info.isDir() && !info.isSymLink()
                    && (destination == canonical || destination.startsWith(canonical + QLatin1Char('/'))))) {
                fail(tr("A directory cannot be transferred into itself."));
                return;
            }
        }
        m_planner = new LocalTransferPlanJob(m_sources, m_destinationDirectory, this);
        m_planner->setAutoDelete(false);
        connect(m_planner, &KJob::result, this, [this](KJob *job) {
            auto *planner = static_cast<LocalTransferPlanJob *>(job);
            m_planner = nullptr;
            planner->deleteLater();
            if (m_finished) return;
            if (job->error()) {
                if (planner->planError() == LocalTransferPlanError::UnsupportedEntry
                    && startFallback()) return;
                fail(job->errorText());
                return;
            }
            m_plan = planner->plan();
            if (!validatePlan()) return;
            setTotalAmount(KJob::Bytes, m_plan.totalBytes);
            m_planned = true;
            scheduleNext();
        });
        m_planner->start();
    }

    bool validatePlan()
    {
        QSet<QString> destinations;
        QSet<QString> sources;
        for (const auto &entry : std::as_const(m_plan.entries)) {
            const QString destination = QDir::cleanPath(entry.destinationPath);
            const QString source = QDir::cleanPath(entry.sourcePath);
            if (sources.contains(source) || destinations.contains(destination)) {
                fail(tr("The transfer contains overlapping sources or duplicate destinations."));
                return false;
            }
            if (entry.destinationExists
                    && (entry.type != LocalTransferEntryType::Directory
                        || !entry.destinationIsDirectory || entry.destinationIsSymbolicLink)) {
                if (startFallback()) return false;
                fail(tr("This plan contains a conflict or an unsupported item."));
                return false;
            }
            sources.insert(source);
            destinations.insert(destination);
        }
        return true;
    }

    bool startFallback()
    {
        if (m_finished || !m_fallbackFactory || m_preplanned) return false;
        if (isSuspended()) { m_fallbackPending = true; return true; }
        auto *job = m_fallbackFactory();
        if (!job) return false;
        m_usedKio = true;
        m_activeJob = job;
        job->setParent(this);
        job->setAutoDelete(false);
        setCapabilities(KJob::Killable);
        Q_EMIT description(this, {}, {}, {});
        connect(job, &KIO::CopyJob::copying, this,
                [this](KIO::Job *, const QUrl &source, const QUrl &destination) {
                    Q_EMIT currentFileChanged(source, destination);
                });
        connect(job, &KIO::CopyJob::moving, this,
                [this](KIO::Job *, const QUrl &source, const QUrl &destination) {
                    Q_EMIT currentFileChanged(source, destination);
                });
        connect(job, &KJob::totalAmountChanged, this,
                [this](KJob *, KJob::Unit unit, qulonglong amount) { setTotalAmount(unit, amount); });
        connect(job, &KJob::processedAmountChanged, this,
                [this](KJob *, KJob::Unit unit, qulonglong amount) { setProcessedAmount(unit, amount); });
        connect(job, &KJob::speed, this,
                [this](KJob *, unsigned long speed) { emitSpeed(speed); });
        connect(job, &KJob::result, this, [this](KJob *completed) {
            m_activeJob = nullptr;
            completed->deleteLater();
            if (m_finished) return;
            m_finished = true;
            setError(completed->error());
            setErrorText(completed->errorText());
            emitResult();
        });
        return true;
    }

    void scheduleNext()
    {
        QTimer::singleShot(0, this, [this] { next(); });
    }

    void next()
    {
        if (m_finished || isSuspended() || !m_planned || m_activeJob) return;
        if (m_index >= m_plan.entries.size()) {
            finishDirectories();
            return;
        }
        auto entry = m_plan.entries.at(m_index++);
        const QFileInfo source(entry.sourcePath);
        const QFileInfo destination(entry.destinationPath);
        const bool validSource = entry.type == LocalTransferEntryType::SymbolicLink
            ? source.isSymLink() && readLocalLink(entry.sourcePath) == entry.symbolicLinkContents
            : source.exists() && !source.isSymLink()
                && (entry.type == LocalTransferEntryType::Directory ? source.isDir()
                    : source.isFile() && static_cast<qulonglong>(source.size()) == entry.size
                        && source.lastModified() == entry.modified);
        if (!validSource) {
            fail(tr("A source changed after the transfer was planned: %1").arg(entry.sourcePath));
            return;
        }
        if (entry.type == LocalTransferEntryType::Directory) {
            entry.destinationExists = destination.exists();
            if (destination.isSymLink()
                || (destination.exists() ? !destination.isDir() : !QDir().mkdir(entry.destinationPath))) {
                fail(tr("Could not create the destination directory: %1").arg(entry.destinationPath));
                return;
            }
            m_directories.push_back(entry);
            m_completedEntries.push_back(entry);
            scheduleNext();
            return;
        }
        const auto sourceUrl = QUrl::fromLocalFile(entry.sourcePath);
        const auto destinationUrl = QUrl::fromLocalFile(entry.destinationPath);
        Q_EMIT currentFileChanged(sourceUrl, destinationUrl);
        if (entry.type == LocalTransferEntryType::SymbolicLink) {
            transferLink(entry);
            return;
        }
        m_activeJob = m_move
            ? static_cast<KJob *>(new LocalFileMoveJob(sourceUrl, destinationUrl, this,
                  LocalFileMoveStrategy::Automatic, m_chunkSize, m_chunkDelayMilliseconds))
            : static_cast<KJob *>(new LocalFileCopyJob(sourceUrl, destinationUrl, this,
                  m_chunkSize, m_chunkDelayMilliseconds));
        m_activeJob->setAutoDelete(false);
        connect(m_activeJob, &KJob::processedAmountChanged, this,
                [this](KJob *, KJob::Unit unit, qulonglong amount) {
                    if (!m_finished && unit == KJob::Bytes)
                        setProcessedAmount(KJob::Bytes, m_completedBytes + amount);
                });
        connect(m_activeJob, &KJob::speed, this,
                [this](KJob *, unsigned long speed) { if (!m_finished) emitSpeed(speed); });
        connect(m_activeJob, &KJob::result, this, [this, entry](KJob *job) {
            m_activeJob = nullptr;
            job->deleteLater();
            if (m_finished) return;
            if (job->error()) {
                fail(job->errorText());
                return;
            }
            m_completedBytes += entry.size;
            m_completedEntries.push_back(entry);
            setProcessedAmount(KJob::Bytes, m_completedBytes);
            scheduleNext();
        });
        m_activeJob->start();
    }

    void finishDirectories()
    {
        if (!m_directories.isEmpty()) {
            const auto directory = m_directories.takeLast();
            if (!directory.destinationExists
                && (::chmod(QFile::encodeName(directory.destinationPath).constData(),
                            directory.sourceStat.st_mode & 07777) != 0
                    || !applyTimes(directory.destinationPath, directory.sourceStat, false))) {
                fail(tr("Could not preserve directory metadata: %1").arg(directory.destinationPath));
                return;
            }
            if (m_move && directory.removeSourceDirectory && (QFileInfo(directory.sourcePath).isSymLink()
                || !QDir().rmdir(directory.sourcePath))) {
                fail(tr("Could not remove the source directory: %1").arg(directory.sourcePath));
                return;
            }
            scheduleNext();
            return;
        }
        m_finished = true;
        setPercent(100);
        emitResult();
    }

    static bool applyTimes(const QString &path, const struct stat &source, bool link)
    {
        const timespec times[]{source.st_atim, source.st_mtim};
        return ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times,
                           link ? AT_SYMLINK_NOFOLLOW : 0) == 0;
    }

    void transferLink(const LocalTransferPlanEntry &entry)
    {
        const QString partial = entry.destinationPath + QStringLiteral(".thispc-part");
        const auto encodedPartial = QFile::encodeName(partial);
        if (::symlink(entry.symbolicLinkContents.constData(), encodedPartial.constData()) != 0) {
            fail(tr("Could not create the partial symbolic link: %1").arg(partial));
            return;
        }
        if (!applyTimes(partial, entry.sourceStat, true)
            || ::syscall(SYS_renameat2, AT_FDCWD, encodedPartial.constData(),
                         AT_FDCWD, QFile::encodeName(entry.destinationPath).constData(), RENAME_NOREPLACE) != 0) {
            QFile::remove(partial);
            fail(tr("Could not publish the symbolic link: %1").arg(entry.destinationPath));
            return;
        }
        if (m_move && (readLocalLink(entry.sourcePath) != entry.symbolicLinkContents
                       || !QFile::remove(entry.sourcePath))) {
            QFile::remove(entry.destinationPath);
            fail(tr("Could not remove the source symbolic link: %1").arg(entry.sourcePath));
            return;
        }
        m_completedEntries.push_back(entry);
        scheduleNext();
    }

    void fail(const QString &message)
    {
        if (m_finished) return;
        m_finished = true;
        setError(KJob::UserDefinedError);
        setErrorText(message);
        emitResult();
    }

    QList<QUrl> m_sources;
    QUrl m_destinationDirectory;
    bool m_move;
    qsizetype m_chunkSize;
    int m_chunkDelayMilliseconds;
    LocalTransferPlan m_plan;
    QList<LocalTransferPlanEntry> m_completedEntries;
    QList<LocalTransferPlanEntry> m_directories;
    QPointer<LocalTransferPlanJob> m_planner;
    QPointer<KJob> m_activeJob;
    qsizetype m_index = 0;
    qulonglong m_completedBytes = 0;
    bool m_started = false;
    bool m_planned = false;
    bool m_finished = false;
    bool m_preplanned = false;
    bool m_usedKio = false;
    bool m_fallbackPending = false;
    std::function<KIO::CopyJob *()> m_fallbackFactory;
};
