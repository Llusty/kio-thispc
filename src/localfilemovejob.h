/*
 * Safe local-file move and completed-operation history for 0.23.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "localbatchmovejob.h"
#include "localdirectorymovejob.h"
#include "localfilecopyjob.h"

#include <KJob>

#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QLocale>
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
    Durability,
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
        , m_hasCustomRemoveSource(static_cast<bool>(removeSource))
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
    int systemError() const { return m_systemError; }
    bool destinationPublished() const { return m_destinationPublished; }
    bool directoryDurabilityConfirmed() const { return m_directoryDurabilityConfirmed; }

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
        const QString sourcePath = m_source.toLocalFile();
        const QString destinationPath = m_destination.toLocalFile();
        m_sourceIdentity = LocalFileIdentity::read(sourcePath);
        if (!m_sourceIdentity.valid) {
            finish(LocalFileMoveError::InvalidInput,
                   tr("The source or destination is not valid for a local file move."));
            return;
        }
        const QFileInfo sourceInfo(sourcePath);
        const QFileInfo destinationInfo(destinationPath);
        if (m_overwrite) {
            // Fail-closed: Move with overwrite via copy-and-remove is fundamentally non-atomic
            // and cannot guarantee safe exclusive ownership or non-destructive rollback.
            finish(LocalFileMoveError::InvalidInput,
                   QLocale().language() == QLocale::Polish
                       ? QStringLiteral("Bezpieczne przenoszenie z nadpisywaniem nie jest obecnie obsługiwane.")
                       : tr("Safe file move with overwrite is not currently supported."));
            return;
        }
        if (!sourceInfo.exists() || !sourceInfo.isFile()
            || sourceInfo.isSymLink()
            || !QFileInfo(destinationPath).dir().exists()) {
            finish(LocalFileMoveError::InvalidInput,
                   (sourceInfo.isSymLink() || sourceInfo.isDir())
                       ? moveContainmentErrorMessage()
                       : tr("The source or destination is not valid for a local file move."));
            return;
        }
        if (destinationInfo.exists() || destinationInfo.isSymLink()) {
            finish(LocalFileMoveError::DestinationExists,
                   tr("The destination already exists."));
            return;
        }

        m_sourceSize = static_cast<qulonglong>(m_sourceIdentity.value.st_size);
        setTotalAmount(KJob::Bytes, m_sourceSize);
        bool sameStorage =
            QStorageInfo(sourceInfo.absolutePath()).device()
            == QStorageInfo(destinationInfo.absolutePath()).device();
#ifdef THISPC_TEST_HARNESS
        if (simulateCrossFilesystem()) {
            sameStorage = false;
        }
#endif
        if (!m_overwrite && m_strategy == LocalFileMoveStrategy::Automatic && sameStorage) {
            m_usedAtomicMove = true;
            bool collision = false;
            int moveErr = 0;
            if (!publishLocalFileNoOverwrite(
                    sourcePath,
                    destinationPath,
                    &collision,
                    &moveErr)) {
                m_systemError = moveErr;
                if (collision) {
                    finish(LocalFileMoveError::DestinationExists,
                           tr("The destination already exists."));
                    return;
                }
                // Stage 5.3.2G.2B: Safe same-volume NTFS-3G Move fallback.
                // On NTFS-3G mounts, native RENAME_NOREPLACE returns EINVAL. If both paths reside
                // on the same verified NTFS-3G volume, safely fall back to Copy + Verify + Publish + Source Removal.
                if ((moveErr == EINVAL || moveErr == EOPNOTSUPP)
                    && isNtfs3gMount(sourcePath, destinationPath)) {
                    m_usedAtomicMove = false;
                    startCopyAndRemove();
                    return;
                }

                const QString errorMsg = (moveErr == EOPNOTSUPP || moveErr == ENOSYS || moveErr == EINVAL)
                    ? (QLocale().language() == QLocale::Polish
                        ? QStringLiteral("System plików nie obsługuje bezpiecznego atomowego przeniesienia bez nadpisania.")
                        : tr("The filesystem does not support atomic file move without overwrite."))
                    : (QLocale().language() == QLocale::Polish
                        ? QStringLiteral("Nie można przenieść pliku atomowo.")
                        : tr("Could not move the file atomically."));
                finish(LocalFileMoveError::AtomicMove, errorMsg);
                return;
            }
            m_destinationPublished = true;
            m_directoryDurabilityConfirmed = true;
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
            false,
            false,
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

                    m_destinationPublished = m_copyJob->destinationPublished();
                    m_directoryDurabilityConfirmed = m_copyJob->directoryDurabilityConfirmed();
                    m_systemError = m_copyJob->systemError();

                    if (completed->error() != KJob::NoError) {
                        if (m_destinationPublished) {
                            // Stage 5.3.2G.3A.1 Durability Gate:
                            // Destination was published on disk, but subsequent operation (e.g. directory fsync with EIO) failed.
                            // CRITICAL RULE: NEVER rollback or delete the published destination!
                            // CRITICAL RULE: NEVER remove the source file!
                            const QString errorMsg = (QLocale().language() == QLocale::Polish)
                                ? QStringLiteral("Plik docelowy został opublikowany, lecz synchronizacja katalogu docelowego zakończyła się błędem (%1). Plik źródłowy i docelowy zostały zachowane.")
                                      .arg(m_systemError != 0 ? QString::number(m_systemError) : QStringLiteral("EIO"))
                                : tr("The destination was published, but destination directory synchronization failed (%1). Both source and destination have been preserved.")
                                      .arg(m_systemError != 0 ? QString::number(m_systemError) : QStringLiteral("EIO"));
                            finish(LocalFileMoveError::Durability, errorMsg);
                            return;
                        }

                        const LocalFileMoveError moveErr =
                            completed->error() == KJob::KilledJobError
                                ? LocalFileMoveError::Cancelled
                                : (m_copyJob->transferError() == LocalTransferError::DestinationExists
                                       ? LocalFileMoveError::DestinationExists
                                       : LocalFileMoveError::Copy);
                        finish(moveErr, completed->errorText());
                        return;
                    }

                    const QString sourcePath = m_source.toLocalFile();
                    const QString destinationPath = m_destination.toLocalFile();
                    const QFileInfo destinationInfo(destinationPath);

                    // 1. Verify published destination entry
                    if (!destinationInfo.exists() || !destinationInfo.isFile()
                        || destinationInfo.isSymLink()
                        || static_cast<qulonglong>(destinationInfo.size()) != m_sourceSize) {
                        finish(LocalFileMoveError::Verification,
                               QLocale().language() == QLocale::Polish
                                   ? QStringLiteral("Nie można zweryfikować pliku docelowego po skopiowaniu; plik źródłowy został zachowany.")
                                   : tr("The copied destination file could not be verified; the source file has been preserved."));
                        return;
                    }

                    // 1b. Cryptographic destination data integrity verification:
                    // Read the published destination file on disk and verify that its SHA-256 matches
                    // the exact stream transferred from source. This guarantees actual on-disk data integrity.
                    QByteArray currentDestHash;
                    {
                        QFile dstFile(destinationPath);
                        if (!dstFile.open(QIODevice::ReadOnly)) {
                            finish(LocalFileMoveError::Verification,
                                   QLocale().language() == QLocale::Polish
                                       ? QStringLiteral("Nie można otworzyć pliku docelowego w celu weryfikacji integralności; cel i źródło zostały zachowane.")
                                       : tr("Could not open destination file to verify data integrity; destination and source preserved."));
                            return;
                        }
                        QCryptographicHash hash(QCryptographicHash::Sha256);
                        if (!hash.addData(&dstFile)) {
                            finish(LocalFileMoveError::Verification,
                                   QLocale().language() == QLocale::Polish
                                       ? QStringLiteral("Błąd odczytu pliku docelowego podczas weryfikacji integralności; cel i źródło zostały zachowane.")
                                       : tr("Failed reading destination file during integrity check; destination and source preserved."));
                            return;
                        }
                        currentDestHash = hash.result();
                    }

                    if (currentDestHash.isEmpty() || currentDestHash != m_copyJob->transferredChecksum()) {
                        finish(LocalFileMoveError::Verification,
                               QLocale().language() == QLocale::Polish
                                   ? QStringLiteral("Wykryto naruszenie integralności danych pliku docelowego; cel i źródło zostały zachowane.")
                                   : tr("Destination data integrity verification failed; both source and destination have been preserved."));
                        return;
                    }

                    // 2. Verify source identity (device, inode, size, mode, mtime, non-symlink)
                    if (!m_sourceIdentity.matches(sourcePath)) {
                        finish(LocalFileMoveError::Verification,
                               QLocale().language() == QLocale::Polish
                                   ? QStringLiteral("Plik źródłowy uległ zmianie w trakcie przenoszenia; oba pliki zostały zachowane.")
                                   : tr("The source file was modified or replaced during the move; both source and destination have been preserved."));
                        return;
                    }

                    // 2b. Cryptographic data integrity verification:
                    // Read the source file from disk and verify that its SHA-256 matches
                    // the exact stream transferred to destination. This detects any in-place byte
                    // modification even if mtime was forged/restored via utimensat.
                    QByteArray currentSourceHash;
                    {
                        QFile srcFile(sourcePath);
                        if (!srcFile.open(QIODevice::ReadOnly)) {
                            finish(LocalFileMoveError::Verification,
                                   QLocale().language() == QLocale::Polish
                                       ? QStringLiteral("Nie można otworzyć źródła w celu weryfikacji integralności; cel zachowano.")
                                       : tr("Could not open source to verify data integrity; destination preserved."));
                            return;
                        }
                        QCryptographicHash hash(QCryptographicHash::Sha256);
                        if (!hash.addData(&srcFile)) {
                            finish(LocalFileMoveError::Verification,
                                   QLocale().language() == QLocale::Polish
                                       ? QStringLiteral("Błąd odczytu źródła podczas weryfikacji integralności; cel zachowano.")
                                       : tr("Failed reading source during integrity check; destination preserved."));
                            return;
                        }
                        currentSourceHash = hash.result();
                    }

                    if (currentSourceHash.isEmpty() || currentSourceHash != m_copyJob->transferredChecksum()) {
                        finish(LocalFileMoveError::Verification,
                               QLocale().language() == QLocale::Polish
                                   ? QStringLiteral("Wykryto naruszenie integralności danych źródła; cel i źródło zostały zachowane.")
                                   : tr("Source data integrity verification failed; both source and destination have been preserved."));
                        return;
                    }

                    // 2c. Directory durability verification gate:
                    // If directory durability was not confirmed (fsync returned EINVAL, ENOTSUP, or failed):
                    // WE MUST NOT REMOVE THE SOURCE FILE!
                    if (!m_directoryDurabilityConfirmed) {
                        const int sysErr = m_systemError != 0 ? m_systemError : EOPNOTSUPP;
                        m_systemError = sysErr;
                        const QString errorMsg = (QLocale().language() == QLocale::Polish)
                            ? QStringLiteral("Nie można potwierdzić trwałości wpisu katalogu docelowego po awarii zasilania (%1). Plik docelowy został opublikowany, a plik źródłowy został zachowany.")
                                  .arg(sysErr)
                            : tr("Destination directory durability could not be confirmed (%1). The destination file was published, and the source file has been preserved.")
                                  .arg(sysErr);
                        finish(LocalFileMoveError::Durability, errorMsg);
                        return;
                    }

                    // 3. Safe source removal
                    const bool removed = m_hasCustomRemoveSource
                        ? m_removeSource(sourcePath)
                        : removeSourceVerified();

                    if (!removed) {
                        // MANDATORY RULE: NEVER delete or rollback published destination on source removal failure.
                        // Preserve BOTH source and destination intact.
                        finish(LocalFileMoveError::SourceRemove,
                               QLocale().language() == QLocale::Polish
                                   ? QStringLiteral("Nie można usunąć pliku źródłowego; plik docelowy został zachowany.")
                                   : tr("The source could not be removed; the destination file has been preserved."));
                        return;
                    }

                    setProcessedAmount(KJob::Bytes, m_sourceSize);
                    finish(LocalFileMoveError::None, {});
                });
        m_copyJob->start();
    }

    bool removeSourceVerified()
    {
        const QString sourcePath = m_source.toLocalFile();
        const QFileInfo srcInfo(sourcePath);
        const QString dirPath = srcInfo.dir().absolutePath();
        const QString fileName = srcInfo.fileName();

        const int dirFd = ::open(QFile::encodeName(dirPath).constData(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
        if (dirFd < 0) {
            m_systemError = errno;
            return false;
        }

        struct stat curStat {};
        if (::fstatat(dirFd, QFile::encodeName(fileName).constData(), &curStat, AT_SYMLINK_NOFOLLOW) != 0) {
            m_systemError = errno;
            ::close(dirFd);
            return false;
        }

        if (!S_ISREG(curStat.st_mode)
            || curStat.st_dev != m_sourceIdentity.value.st_dev
            || curStat.st_ino != m_sourceIdentity.value.st_ino
            || curStat.st_size != m_sourceIdentity.value.st_size
            || curStat.st_mtim.tv_sec != m_sourceIdentity.value.st_mtim.tv_sec
            || curStat.st_mtim.tv_nsec != m_sourceIdentity.value.st_mtim.tv_nsec) {
            ::close(dirFd);
            return false;
        }

#ifdef THISPC_TEST_HARNESS
        if (simulateRaceBeforeSourceUnlink()) {
            simulateRaceBeforeSourceUnlink()();
        }
#endif

        const int unlinkRes = ::unlinkat(dirFd, QFile::encodeName(fileName).constData(), 0);
        const int unlinkErr = (unlinkRes != 0) ? errno : 0;
        ::close(dirFd);

        if (unlinkRes != 0) {
            m_systemError = unlinkErr;
            return false;
        }
        return true;
    }

    void finish(LocalFileMoveError error, const QString &message)
    {
        if (m_finished) return;
        m_finished = true;
        m_moveError = error;
        setProperty("thispcMoveError", QVariant::fromValue(error));
        setProperty("thispcDestinationReplaced",
                    m_overwrite && !m_originalDestination.matches(m_destination.toLocalFile()));
        setProperty("thispcDestinationPublished", m_destinationPublished);
        setProperty("thispcDirectoryDurabilityConfirmed", m_directoryDurabilityConfirmed);
        if (error == LocalFileMoveError::Durability || (m_destinationPublished && error != LocalFileMoveError::None)) {
            setProperty("thispcPartialMoveOutcome", true);
        }
        if (m_systemError != 0) {
            setProperty("thispcSystemError", m_systemError);
        }
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
    LocalFileIdentity m_sourceIdentity;
    bool m_hasCustomRemoveSource = false;
    RemoveSource m_removeSource;
    LocalFileCopyJob *m_copyJob = nullptr;
    qulonglong m_sourceSize = 0;
    bool m_started = false;
    bool m_finished = false;
    bool m_usedAtomicMove = false;
    bool m_destinationPublished = false;
    bool m_directoryDurabilityConfirmed = false;
    LocalFileMoveError m_moveError = LocalFileMoveError::None;
    int m_systemError = 0;
};


class LocalMoveHistory final : public QObject
{
    Q_OBJECT

public:
    struct Record {
        bool copy = false;
        bool directory = false;
        bool batch = false;
        QUrl source;
        QUrl destination;
        quint64 serial = 0;
        quint64 internalOrder = 0;
        qulonglong destinationSize = 0;
        QDateTime destinationModified;
        LocalDirectoryIdentity directoryIdentity;
        QList<QPair<QUrl, QUrl>> batchPairs;
        QList<LocalRegularFileIdentity> batchIdentities;
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

    bool recordWhenCompleted(LocalDirectoryMoveJob *job, quint64 serial = 0)
    {
        if (!job) return false;
        connect(job, &KJob::result, this, [this, job, serial](KJob *completed) {
            if (completed->error() == KJob::NoError
                && job->destinationPublished()
                && job->directoryDurabilityConfirmed()
                && !job->property("thispcOverwritesDestination").toBool()
                && !job->property("thispcPartialMoveOutcome").toBool()) {
                recordCompletedDirectoryMove(
                    job->sourceUrl(),
                    job->destinationUrl(),
                    job->movedIdentity(),
                    serial);
            }
        });
        return true;
    }

    bool recordWhenCompleted(LocalBatchMoveJob *job, quint64 serial = 0)
    {
        if (!job) return false;
        connect(job, &KJob::result, this, [this, job, serial](KJob *completed) {
            const BatchMoveResult &res = job->batchResult();
            if (completed->error() == KJob::NoError
                && res.outcome == BatchMoveOutcome::FullSuccess
                && res.totalCount >= 2
                && res.publishedCount == res.totalCount
                && res.durableCount == res.totalCount
                && !res.partialMoveOutcome
                && !job->property("thispcOverwritesDestination").toBool()
                && !job->property("thispcPartialMoveOutcome").toBool()
                && !job->property("thispcBatchPartiallyCompleted").toBool()) {
                QList<QPair<QUrl, QUrl>> pairs;
                QList<LocalRegularFileIdentity> ids;
                pairs.reserve(res.items.size());
                ids.reserve(res.items.size());
                for (const auto &it : res.items) {
                    pairs.append({it.sourceUrl, it.destinationUrl});
                    ids.append(it.movedIdentity);
                }
                recordCompletedBatchMove(pairs, ids, serial);
            } else if (res.publishedCount > 0 || res.partialMoveOutcome
                       || job->property("thispcPartialMoveOutcome").toBool()
                       || job->property("thispcBatchPartiallyCompleted").toBool()) {
                fenceIrreversible();
            }
        });
        return true;
    }

    void fenceIrreversible()
    {
        if (m_nextOrder > 1) {
            m_irreversibleOrder = m_nextOrder - 1;
        }
        while (m_records.size() > m_position) {
            m_records.removeLast();
        }
        Q_EMIT availabilityChanged(canUndo(), canRedo());
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

    bool recordCompletedDirectoryMove(
        const QUrl &source,
        const QUrl &destination,
        const LocalDirectoryIdentity &identity,
        quint64 serial = 0)
    {
        if (m_busy || !source.isLocalFile() || !destination.isLocalFile() || !identity.valid) {
            return false;
        }
        struct stat srcSt {};
        if (localLstat(source.toLocalFile(), &srcSt) || errno != ENOENT) {
            return false;
        }
        struct stat dstSt {};
        if (!localLstat(destination.toLocalFile(), &dstSt) || !S_ISDIR(dstSt.st_mode)) {
            return false;
        }
        if (static_cast<quint64>(dstSt.st_dev) != identity.stDev
            || static_cast<quint64>(dstSt.st_ino) != identity.ino) {
            return false;
        }
        while (m_records.size() > m_position) m_records.removeLast();
        Record record;
        record.directory = true;
        record.source = source;
        record.destination = destination;
        record.serial = serial;
        record.internalOrder = m_nextOrder++;
        record.directoryIdentity = identity;
        m_records.push_back(record);
        m_position = m_records.size();
        Q_EMIT availabilityChanged(canUndo(), canRedo());
        return true;
    }

    bool recordCompletedBatchMove(
        const QList<QPair<QUrl, QUrl>> &pairs,
        const QList<LocalRegularFileIdentity> &identities,
        quint64 serial = 0)
    {
        if (m_busy || pairs.size() < 2 || pairs.size() != identities.size()) {
            return false;
        }
        for (int i = 0; i < pairs.size(); ++i) {
            const QUrl &src = pairs.at(i).first;
            const QUrl &dst = pairs.at(i).second;
            const LocalRegularFileIdentity &id = identities.at(i);
            if (!src.isLocalFile() || !dst.isLocalFile() || !id.valid) {
                return false;
            }
            struct stat srcSt {};
            if (localLstat(src.toLocalFile(), &srcSt) || errno != ENOENT) {
                return false;
            }
            struct statx dstStx {};
            struct stat dstSt {};
            const unsigned int mask =
                STATX_BASIC_STATS | STATX_MNT_ID | STATX_MNT_ID_UNIQUE | STATX_SUBVOL;
            const QByteArray dstEnc = QFile::encodeName(dst.toLocalFile());
            if (::syscall(SYS_statx, AT_FDCWD, dstEnc.constData(),
                          AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, mask, &dstStx) != 0
                || ::lstat(dstEnc.constData(), &dstSt) != 0) {
                return false;
            }
            const LocalRegularFileIdentity curId = LocalRegularFileIdentity::fromStat(dstStx, dstSt);
            if (!curId.strictlyUnchangedForHistory(id)) {
                return false;
            }
        }
        while (m_records.size() > m_position) m_records.removeLast();
        Record record;
        record.batch = true;
        record.batchPairs = pairs;
        record.batchIdentities = identities;
        record.serial = serial;
        record.internalOrder = m_nextOrder++;
        m_records.push_back(record);
        m_position = m_records.size();
        Q_EMIT availabilityChanged(canUndo(), canRedo());
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
        record.internalOrder = m_nextOrder++;
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
        Record record;
        record.copy = true;
        record.source = source;
        record.destination = destination;
        record.serial = serial;
        record.internalOrder = m_nextOrder++;
        record.destinationSize = static_cast<qulonglong>(destinationInfo.size());
        record.destinationModified = destinationInfo.lastModified();
        m_records.push_back(record);
        m_position = m_records.size();
        Q_EMIT availabilityChanged(canUndo(), canRedo());
        return true;
    }

    bool canUndo() const
    {
        return !m_busy && m_position > 0
            && m_records.at(m_position - 1).internalOrder > m_irreversibleOrder;
    }
    bool canRedo() const
    {
        return !m_busy && m_position < m_records.size()
            && m_records.at(m_position).internalOrder > m_irreversibleOrder;
    }
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
        if (record.directory) {
            return runDirectoryMove(record.destination, record.source, record.directoryIdentity, target, target);
        }
        if (record.batch) {
            return runBatchMove(target, true, target);
        }
        return runMove(record.destination, record.source, target);
    }

    KJob *redo()
    {
        if (!canRedo()) return nullptr;
        const qsizetype recordIndex = m_position;
        const qsizetype target = m_position + 1;
        const Record record = m_records.at(recordIndex);
        if (record.copy) return runCopyRedo(record, target);
        if (record.directory) {
            return runDirectoryMove(record.source, record.destination, record.directoryIdentity, target, recordIndex);
        }
        if (record.batch) {
            return runBatchMove(recordIndex, false, target);
        }
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

    KJob *runDirectoryMove(
        const QUrl &source,
        const QUrl &destination,
        const LocalDirectoryIdentity &expectedIdentity,
        qsizetype successPosition,
        qsizetype recordIndex)
    {
        if (!expectedIdentity.valid) {
            return nullptr;
        }
        m_busy = true;
        Q_EMIT availabilityChanged(false, false);
        auto *job = new LocalDirectoryMoveJob(source, destination, this, expectedIdentity);
        job->setAutoDelete(false);
        connect(job, &KJob::result, this,
                [this, job, successPosition, recordIndex](KJob *completed) {
                    if (completed->error() == KJob::NoError
                        && job->destinationPublished()
                        && job->directoryDurabilityConfirmed()
                        && !completed->property("thispcPartialMoveOutcome").toBool()) {
                        m_position = successPosition;
                    } else if (completed->property("thispcPartialMoveOutcome").toBool()) {
                        if (recordIndex >= 0 && recordIndex < m_records.size()) {
                            m_records.removeAt(recordIndex);
                        }
                        if (m_position > m_records.size()) {
                            m_position = m_records.size();
                        }
                    }
                    m_busy = false;
                    Q_EMIT availabilityChanged(canUndo(), canRedo());
                });
        job->start();
        return job;
    }

    KJob *runBatchMove(
        qsizetype recordIndex,
        bool isUndo,
        qsizetype successPosition)
    {
        if (recordIndex < 0 || recordIndex >= m_records.size()) {
            return nullptr;
        }
        const Record &rec = m_records.at(recordIndex);
        if (!rec.batch || rec.batchPairs.size() < 2
            || rec.batchPairs.size() != rec.batchIdentities.size()) {
            return nullptr;
        }
        QList<QPair<QUrl, QUrl>> pairs;
        QList<LocalRegularFileIdentity> expectedIds;
        const qsizetype n = rec.batchPairs.size();
        pairs.reserve(n);
        expectedIds.reserve(n);
        if (isUndo) {
            for (qsizetype k = n - 1; k >= 0; --k) {
                pairs.append({rec.batchPairs.at(k).second, rec.batchPairs.at(k).first});
                expectedIds.append(rec.batchIdentities.at(k));
            }
        } else {
            for (qsizetype k = 0; k < n; ++k) {
                pairs.append(rec.batchPairs.at(k));
                expectedIds.append(rec.batchIdentities.at(k));
            }
        }

        m_busy = true;
        Q_EMIT availabilityChanged(false, false);
        auto *job = new LocalBatchMoveJob(pairs, this, expectedIds, true);
        job->setAutoDelete(false);
        connect(job, &KJob::result, this,
                [this, job, successPosition, recordIndex, isUndo, n](KJob *completed) {
                    const BatchMoveResult &res = job->batchResult();
                    if (completed->error() == KJob::NoError
                        && res.outcome == BatchMoveOutcome::FullSuccess
                        && res.durableCount == res.totalCount
                        && !res.partialMoveOutcome
                        && recordIndex >= 0 && recordIndex < m_records.size()
                        && m_records.at(recordIndex).batchPairs.size() == n
                        && res.items.size() == n) {
                        for (qsizetype step = 0; step < n; ++step) {
                            const qsizetype origIdx = isUndo ? (n - 1 - step) : step;
                            m_records[recordIndex].batchIdentities[origIdx] =
                                res.items.at(step).movedIdentity;
                        }
                        m_position = successPosition;
                    } else if (res.publishedCount > 0 || res.partialMoveOutcome
                               || completed->property("thispcPartialMoveOutcome").toBool()
                               || completed->property("thispcBatchPartiallyCompleted").toBool()) {
                        if (recordIndex >= 0 && recordIndex < m_records.size()) {
                            m_records.removeAt(recordIndex);
                        }
                        if (m_position > m_records.size()) {
                            m_position = m_records.size();
                        }
                        fenceIrreversible();
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
    quint64 m_nextOrder = 1;
    quint64 m_irreversibleOrder = 0;
    bool m_busy = false;
};
