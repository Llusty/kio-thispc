/*
 * Safe staged transfer job for remote -> local transfers.
 *
 * Ensures that bytes transferred from remote protocols (MTP, SMB, etc.)
 * to a local filesystem destination are always written to a designated
 * unique temporary staging file (`<target>.thispc-part.<pid>.<nonce>`)
 * and only published to the final destination filename via an atomic
 * kernel rename (SYS_renameat2 / rename) without copy-fallback upon verified
 * completion.
 *
 * Safety guarantees:
 *  - Unique per-job staging file created exclusively with 0600 mode and O_NOFOLLOW.
 *  - Alien files or symlinks are never overwritten or followed.
 *  - Only the task's own staging file is unlinked on error, cancellation, or shutdown.
 *  - Pre-existing destination files are never corrupted or deleted.
 *  - Atomic publication prevents TOCTOU races with alien files appearing after preflight.
 *  - Safe atomic exchange (RENAME_EXCHANGE) preserves original files until swap succeeds.
 *  - Move from remote/MTP is disabled in staged path to protect source data.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "localfileoverwrite.h"
#include "localdirectorymovejob.h"
#include "localbatchmovejob.h"

#include <KIO/CopyJob>
#include <KIO/DeleteJob>
#include <KIO/Job>
#include <KIO/JobUiDelegate>
#include <KIO/JobUiDelegateFactory>
#include <KIO/StatJob>
#include <KIO/UDSEntry>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QPointer>
#include <QRandomGenerator>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif

enum class MtpMoveDirection {
    None,
    MtpToLocal,
    LocalToMtp,
    MtpToMtpSameDevice,
    MtpToMtpCrossDevice,
    MixedOrProtectedMtp
};

enum class MtpMoveVerdict {
    NotMtp,
    AllowedStagedMove,
    RefusedUnsupported
};

struct MtpMoveSafetyEvaluation {
    MtpMoveDirection direction = MtpMoveDirection::None;
    MtpMoveVerdict verdict = MtpMoveVerdict::NotMtp;
    QString refusalReason;

    bool isAllowed() const
    {
        return verdict != MtpMoveVerdict::RefusedUnsupported;
    }
};

namespace MtpMoveSafety {

inline bool isMtpSchemeUrl(const QUrl &url)
{
    return url.isValid() && url.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0;
}

inline QStringList mtpPathSegments(const QUrl &url)
{
    if (!isMtpSchemeUrl(url)) {
        return {};
    }
    const QString rawPath = url.path();
    if (rawPath.startsWith(QStringLiteral("udi="), Qt::CaseInsensitive)
        || rawPath.startsWith(QStringLiteral("/udi="), Qt::CaseInsensitive)) {
        return {};
    }
    return rawPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

inline bool isValidMtpSourceFileUrl(const QUrl &url)
{
    if (!isMtpSchemeUrl(url) || url.fileName().isEmpty()) {
        return false;
    }
    const QStringList segments = mtpPathSegments(url);
    return segments.size() >= 2;
}

inline QString mtpDeviceRootSegment(const QUrl &url)
{
    const QStringList segments = mtpPathSegments(url);
    return segments.isEmpty() ? QString() : segments.first();
}

inline MtpMoveSafetyEvaluation evaluate(
    const QList<QUrl> &sources,
    const QUrl &destinationDirectory,
    bool move)
{
    MtpMoveSafetyEvaluation eval;
    if (!move || sources.isEmpty() || !destinationDirectory.isValid()) {
        return eval;
    }

    const bool destIsMtp = isMtpSchemeUrl(destinationDirectory);
    const bool destIsLocal = destinationDirectory.isLocalFile();

    bool anySourceMtp = false;
    bool allSourcesMtp = true;
    bool allSourcesValidMtpFiles = true;
    bool allSourcesLocal = true;
    QString firstMtpSourceDevice;

    for (const QUrl &src : sources) {
        if (isMtpSchemeUrl(src)) {
            anySourceMtp = true;
            allSourcesLocal = false;
            if (!isValidMtpSourceFileUrl(src)) {
                allSourcesValidMtpFiles = false;
            }
            const QString dev = mtpDeviceRootSegment(src);
            if (firstMtpSourceDevice.isEmpty()) {
                firstMtpSourceDevice = dev;
            } else if (dev != firstMtpSourceDevice) {
                allSourcesValidMtpFiles = false;
            }
        } else {
            allSourcesMtp = false;
            allSourcesValidMtpFiles = false;
            if (!src.isLocalFile()) {
                allSourcesLocal = false;
            }
        }
    }

    if (!anySourceMtp && !destIsMtp) {
        return eval;
    }

    const QString standardRefusalReason = trLocal(
        "Przenoszenie plików z udziałem urządzeń MTP jest niedostępne ze względów bezpieczeństwa danych. Użyj operacji Kopiuj.",
        "Moving files involving MTP devices is unavailable for data safety reasons. Please use Copy instead.");

    // Direction 1: MTP -> Local filesystem (blocked per Final MTP Move Release Block)
    if (allSourcesMtp && destIsLocal) {
        eval.direction = allSourcesValidMtpFiles
            ? MtpMoveDirection::MtpToLocal
            : MtpMoveDirection::MixedOrProtectedMtp;
        eval.verdict = MtpMoveVerdict::RefusedUnsupported;
        eval.refusalReason = standardRefusalReason;
        return eval;
    }

    // Direction 2: Local filesystem -> MTP
    if (allSourcesLocal && destIsMtp) {
        eval.direction = MtpMoveDirection::LocalToMtp;
        eval.verdict = MtpMoveVerdict::RefusedUnsupported;
        eval.refusalReason = standardRefusalReason;
        return eval;
    }

    // Direction 3 & 4: MTP -> MTP (same device vs cross device)
    if (allSourcesMtp && destIsMtp) {
        const QString destDevice = mtpDeviceRootSegment(destinationDirectory);
        const bool sameDevice = !firstMtpSourceDevice.isEmpty()
            && !destDevice.isEmpty()
            && firstMtpSourceDevice == destDevice
            && allSourcesValidMtpFiles;

        eval.direction = sameDevice
            ? MtpMoveDirection::MtpToMtpSameDevice
            : MtpMoveDirection::MtpToMtpCrossDevice;
        eval.verdict = MtpMoveVerdict::RefusedUnsupported;
        eval.refusalReason = standardRefusalReason;
        return eval;
    }

    // Mixed selection (e.g. local + MTP, or SMB + MTP)
    eval.direction = MtpMoveDirection::MixedOrProtectedMtp;
    eval.verdict = MtpMoveVerdict::RefusedUnsupported;
    eval.refusalReason = standardRefusalReason;
    return eval;
}

} // namespace MtpMoveSafety

enum class MovePolicyDecision {
    NotAMove,
    AllowedSingleRegularLocalFile,
    AllowedSingleSameVolumeBtrfsDirectory,
    AllowedBatchSameVolumeBtrfsRegularFiles,
    AllowedRemoteToRemote,
    RefusedMtp,
    RefusedInvalidInput,
    RefusedMixedSources,
    RefusedLocalToRemote,
    RefusedRemoteToLocal,
    RefusedLocalContainment,
};

struct MoveOperationClassification {
    MovePolicyDecision decision = MovePolicyDecision::NotAMove;
    bool allowed = true;
    MtpMoveSafetyEvaluation mtpEvaluation;
    QString refusalReason;
};

namespace MoveSafetyPolicy {

inline bool isVirtualNonTransferableScheme(const QUrl &url)
{
    if (!url.isValid()) {
        return true;
    }
    const QString scheme = url.scheme().toLower();
    return scheme.isEmpty()
        || scheme == QLatin1String("thispc")
        || scheme == QLatin1String("thispcsearch")
        || scheme == QLatin1String("filenamesearch")
        || scheme == QLatin1String("remote")
        || scheme == QLatin1String("trash")
        || scheme == QLatin1String("about")
        || scheme == QLatin1String("data");
}

inline MoveOperationClassification evaluate(
    const QList<QUrl> &sources,
    const QUrl &destinationDirectory,
    bool move)
{
    MoveOperationClassification result;
    if (!move) {
        result.decision = MovePolicyDecision::NotAMove;
        result.allowed = true;
        return result;
    }

    if (sources.isEmpty() || !destinationDirectory.isValid() || isVirtualNonTransferableScheme(destinationDirectory)) {
        result.decision = MovePolicyDecision::RefusedInvalidInput;
        result.allowed = false;
        result.refusalReason = moveContainmentErrorMessage();
        return result;
    }

    result.mtpEvaluation = MtpMoveSafety::evaluate(sources, destinationDirectory, true);
    if (!result.mtpEvaluation.isAllowed()) {
        result.decision = MovePolicyDecision::RefusedMtp;
        result.allowed = false;
        result.refusalReason = result.mtpEvaluation.refusalReason;
        return result;
    }

    bool hasLocalSource = false;
    bool hasRemoteSource = false;
    for (const QUrl &source : sources) {
        if (isVirtualNonTransferableScheme(source)) {
            result.decision = MovePolicyDecision::RefusedInvalidInput;
            result.allowed = false;
            result.refusalReason = moveContainmentErrorMessage();
            return result;
        }
        if (source.isLocalFile()) {
            hasLocalSource = true;
        } else {
            hasRemoteSource = true;
        }
    }

    const bool destIsLocal = destinationDirectory.isLocalFile();

    if (hasLocalSource && hasRemoteSource) {
        result.decision = MovePolicyDecision::RefusedMixedSources;
        result.allowed = false;
        result.refusalReason = trLocal(
            "Przenoszenie mieszanych elementów lokalnych i zdalnych jest niedostępne ze względów bezpieczeństwa danych. Użyj operacji Kopiuj.",
            "Moving mixed local and remote items is unavailable for data safety reasons. Use Copy instead.");
        return result;
    }

    if (hasLocalSource && !destIsLocal) {
        result.decision = MovePolicyDecision::RefusedLocalToRemote;
        result.allowed = false;
        result.refusalReason = trLocal(
            "Przenoszenie lokalnych elementów bezpośrednio do lokalizacji zdalnej jest niedostępne ze względów bezpieczeństwa danych. Użyj operacji Kopiuj.",
            "Moving local items directly to a remote location is unavailable for data safety reasons. Use Copy instead.");
        return result;
    }

    if (!hasLocalSource && destIsLocal) {
        result.decision = MovePolicyDecision::RefusedRemoteToLocal;
        result.allowed = false;
        result.refusalReason = trLocal(
            "Przenoszenie elementów ze źródeł zdalnych bezpośrednio na dysk lokalny jest niedostępne ze względów bezpieczeństwa danych. Użyj operacji Kopiuj.",
            "Moving items from remote sources directly to a local disk is unavailable for data safety reasons. Use Copy instead.");
        return result;
    }

    if (hasLocalSource && destIsLocal) {
        const QFileInfo destDirInfo(destinationDirectory.toLocalFile());
        if (!destDirInfo.exists() || !destDirInfo.isDir()) {
            result.decision = MovePolicyDecision::RefusedInvalidInput;
            result.allowed = false;
            result.refusalReason = trLocal(
                "Katalog docelowy jest nieprawidłowy lub nie istnieje.",
                "The destination directory is invalid or does not exist.");
            return result;
        }
        if (isSingleRegularLocalFile(sources)) {
            result.decision = MovePolicyDecision::AllowedSingleRegularLocalFile;
            result.allowed = true;
            return result;
        }
        if (sources.size() == 1) {
            const QString srcPath = sources.first().toLocalFile();
            struct stat srcSt {};
            if (localLstat(srcPath, &srcSt) && S_ISDIR(srcSt.st_mode)) {
                LocalBtrfsDirectoryQualification dirQual;
                if (LocalDirectoryMoveDetail::qualifyFastForPolicy(
                        srcPath, destinationDirectory.toLocalFile(), &dirQual)) {
                    result.decision = MovePolicyDecision::AllowedSingleSameVolumeBtrfsDirectory;
                    result.allowed = true;
                    return result;
                }
                result.decision = (dirQual.error == LocalDirectoryMoveError::InvalidInput)
                    ? MovePolicyDecision::RefusedInvalidInput
                    : MovePolicyDecision::RefusedLocalContainment;
                result.allowed = false;
                result.refusalReason = dirQual.reason.isEmpty() ? moveContainmentErrorMessage() : dirQual.reason;
                return result;
            }
        }
        if (sources.size() >= 2) {
            BatchMovePreflight batchPreflight;
            if (LocalBatchMoveDetail::qualifyFastForPolicy(
                    sources, destinationDirectory, &batchPreflight)) {
                result.decision = MovePolicyDecision::AllowedBatchSameVolumeBtrfsRegularFiles;
                result.allowed = true;
                return result;
            }
            result.decision = (batchPreflight.error == BatchMoveError::InvalidInput)
                ? MovePolicyDecision::RefusedInvalidInput
                : MovePolicyDecision::RefusedLocalContainment;
            result.allowed = false;
            result.refusalReason = batchPreflight.reason.isEmpty()
                ? moveContainmentErrorMessage()
                : batchPreflight.reason;
            return result;
        }
        result.decision = MovePolicyDecision::RefusedLocalContainment;
        result.allowed = false;
        result.refusalReason = moveContainmentErrorMessage();
        return result;
    }

    result.decision = MovePolicyDecision::AllowedRemoteToRemote;
    result.allowed = true;
    return result;
}

} // namespace MoveSafetyPolicy

namespace KioStagedDetail {

inline QString createExclusiveStagedFile(const QString &finalPath)
{
    const qint64 pid = QCoreApplication::applicationPid();

    for (int attempt = 0; attempt < 32; ++attempt) {
        const quint64 nonce = QRandomGenerator::global()->generate64();
        const QString candidate = QStringLiteral("%1.thispc-part.%2.%3")
            .arg(finalPath)
            .arg(pid)
            .arg(nonce, 16, 16, QLatin1Char('0'));
        const QByteArray candEncoded = QFile::encodeName(candidate);

        struct stat st;
        if (::lstat(candEncoded.constData(), &st) == 0) {
            continue; // Candidate exists or is a symlink; retry with another nonce
        }

        int fd = ::open(candEncoded.constData(),
                        O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC,
                        0600);
        if (fd >= 0) {
            ::close(fd);
            return candidate;
        }
    }
    return {};
}

inline bool atomicPublishNewFile(const QString &partPath, const QString &finalPath, bool *collisionDetected = nullptr, int *outError = nullptr)
{
    return publishLocalFileNoOverwrite(partPath, finalPath, collisionDetected, outError);
}

inline bool atomicPublishOverwrite(const QString &partPath, const QString &finalPath,
                                   const LocalFileIdentity &expectedIdentity)
{
    const QByteArray partEncoded = QFile::encodeName(partPath);
    const QByteArray finalEncoded = QFile::encodeName(finalPath);

    struct stat st;
    const bool finalExists = (::lstat(finalEncoded.constData(), &st) == 0);

    if (finalExists) {
        if (expectedIdentity.valid && !expectedIdentity.matches(finalPath)) {
            return false;
        }

        if (exchangeLocalFiles(partPath, finalPath)) {
            if (expectedIdentity.valid && !expectedIdentity.matches(partPath)) {
                exchangeLocalFiles(partPath, finalPath);
                return false;
            }
            ::unlink(partEncoded.constData());
            return true;
        }

        return false;
    } else {
        bool collision = false;
        return atomicPublishNewFile(partPath, finalPath, &collision);
    }
}

} // namespace KioStagedDetail

class KioStagedTransferJob final : public KIO::Job
{
    Q_OBJECT

public:
    struct RemoteSourceMetadata {
        bool exists = false;
        bool isDirectory = false;
        bool isRegularFile = false;
        bool isSymLink = false;
        QString name;
        qint64 size = -1;
        qint64 mtimeSeconds = -1;
        int errorCode = 0;
        QString errorText;

        bool hasReliableFileIdentity() const
        {
            return errorCode == 0
                && exists
                && isRegularFile
                && !isDirectory
                && !isSymLink
                && !name.isEmpty()
                && size >= 0;
        }

        bool matchesIdentity(const RemoteSourceMetadata &other) const
        {
            if (!hasReliableFileIdentity() || !other.hasReliableFileIdentity()) {
                return false;
            }
            if (name != other.name || size != other.size) {
                return false;
            }
            if (mtimeSeconds >= 0 && other.mtimeSeconds >= 0 && mtimeSeconds != other.mtimeSeconds) {
                return false;
            }
            return true;
        }
    };

    using SubJobFactory = std::function<KJob *(const QUrl &src, const QUrl &dst)>;
    using SourceStatProvider = std::function<RemoteSourceMetadata(const QUrl &src, bool postPublishRecheck)>;
    using SourceDeleteFactory = std::function<KJob *(const QUrl &src)>;

    KioStagedTransferJob(
        QList<QUrl> sources,
        QUrl destinationDirectory,
        bool move = false,
        QUrl singleTargetDestination = {},
        bool overwrite = false,
        LocalFileIdentity originalDestination = {},
        QWidget *parentWidget = nullptr,
        QObject *parent = nullptr)
        : KIO::Job()
        , m_sources(std::move(sources))
        , m_destinationDirectory(std::move(destinationDirectory))
        , m_move(move)
        , m_singleTargetDestination(std::move(singleTargetDestination))
        , m_overwrite(overwrite)
        , m_originalDestination(originalDestination)
        , m_parentWidget(parentWidget)
    {
        setParent(parent);
        setCapabilities(KJob::Killable | KJob::Suspendable);
        setProperty("thispcSourceUrls", QVariant::fromValue(m_sources));
        setProperty("thispcDestinationUrl", m_destinationDirectory);
        setProperty("thispcStagedTransfer", true);
        if (m_overwrite) {
            setProperty("thispcOverwritesDestination", true);
        }
    }

    ~KioStagedTransferJob() override
    {
        if (!m_published) {
            cleanupCurrentPart();
        }
        if (m_currentSubJob) {
            m_currentSubJob->kill(KJob::Quietly);
        }
        QCoreApplication::removePostedEvents(this);
    }

    void setSubJobFactory(SubJobFactory factory)
    {
        m_subJobFactory = std::move(factory);
    }

    void enableTwoPhaseMove(bool enabled = true)
    {
        m_twoPhaseMoveEnabled = enabled;
    }

    bool isTwoPhaseMoveEnabled() const
    {
        return m_twoPhaseMoveEnabled;
    }

    void setSourceStatProvider(SourceStatProvider provider)
    {
        m_sourceStatProvider = std::move(provider);
    }

    void setSourceDeleteFactory(SourceDeleteFactory factory)
    {
        m_sourceDeleteFactory = std::move(factory);
    }

    bool hasPartialMoveOutcome() const
    {
        return m_partialMoveOutcome;
    }

    int sourceDeleteAttempts() const
    {
        return m_sourceDeleteAttempts;
    }

    int sourceDeletedCount() const
    {
        return m_sourceDeletedCount;
    }

    QStringList publishedFiles() const
    {
        return m_publishedFiles;
    }

    QStringList phaseLog() const
    {
        return m_phaseLog;
    }

    QString currentPartPath() const
    {
        return m_currentPartPath;
    }

    QString currentFinalPath() const
    {
        return m_currentFinalPath;
    }

    void start() override
    {
        if (m_started) return;
        m_started = true;

        if (m_move) {
            setError(KIO::ERR_UNSUPPORTED_ACTION);
            setErrorText(trLocal(
                "Przenoszenie plików z udziałem urządzeń MTP jest niedostępne ze względów bezpieczeństwa danych. Użyj operacji Kopiuj.",
                "Moving files involving MTP devices is unavailable for data safety reasons. Please use Copy instead."));
            m_finished = true;
            emitResult();
            return;
        }

        if (m_sources.isEmpty()) {
            setError(KIO::ERR_DOES_NOT_EXIST);
            setErrorText(QStringLiteral("Brak plików źródłowych do przesłania."));
            emitResult();
            return;
        }

        if (!m_destinationDirectory.isLocalFile()) {
            setError(KIO::ERR_UNSUPPORTED_PROTOCOL);
            setErrorText(QStringLiteral("Katalog docelowy musi być ścieżką lokalną dla bezpiecznego transferu staged."));
            emitResult();
            return;
        }

        if (m_move) {
            const MtpMoveSafetyEvaluation eval = MtpMoveSafety::evaluate(m_sources, m_destinationDirectory, true);
            if (eval.verdict != MtpMoveVerdict::AllowedStagedMove) {
                setError(KIO::ERR_UNSUPPORTED_ACTION);
                setErrorText(eval.refusalReason.isEmpty()
                    ? QStringLiteral("Ten kierunek przenoszenia MTP nie jest obsługiwany ze względów bezpieczeństwa danych.")
                    : eval.refusalReason);
                emitResult();
                return;
            }
        }

        const QFileInfo destDirInfo(m_destinationDirectory.toLocalFile());
        if (!destDirInfo.exists() || !destDirInfo.isDir()) {
            setError(KIO::ERR_CANNOT_ENTER_DIRECTORY);
            setErrorText(QStringLiteral("Katalog docelowy nie istnieje."));
            emitResult();
            return;
        }

        QTimer::singleShot(0, this, [this] {
            startCurrentFile();
        });
    }

protected:
    bool doKill() override
    {
        if (m_finished) return false;
        m_killed = true;
        m_finished = true;
        if (m_currentSubJob) {
            m_currentSubJob->kill(KJob::Quietly);
        }
        cleanupCurrentPart();
        return true;
    }

    bool doSuspend() override
    {
        return m_currentSubJob && m_currentSubJob->suspend();
    }

    bool doResume() override
    {
        return m_currentSubJob && m_currentSubJob->resume();
    }

private:
    void cleanupCurrentPart()
    {
        if (m_partCreated && !m_currentPartPath.isEmpty()) {
            const QByteArray encoded = QFile::encodeName(m_currentPartPath);
            ::unlink(encoded.constData());
            m_partCreated = false;
        }
    }

    void querySourceMetadata(
        const QUrl &src,
        bool postPublishRecheck,
        std::function<void(const RemoteSourceMetadata &)> callback)
    {
        if (m_killed || m_finished) return;

        if (m_sourceStatProvider) {
            const RemoteSourceMetadata meta = m_sourceStatProvider(src, postPublishRecheck);
            if (m_killed || m_finished) return;
            callback(meta);
            return;
        }

        KIO::StatJob *statJob = KIO::stat(
            src,
            KIO::StatJob::SourceSide,
            KIO::StatBasic | KIO::StatTime | KIO::StatResolveSymlink,
            KIO::HideProgressInfo);
        if (!statJob) {
            RemoteSourceMetadata failed;
            failed.errorCode = KIO::ERR_CANNOT_STAT;
            failed.errorText = QStringLiteral("Nie można uruchomić odczytu metadanych źródła MTP.");
            callback(failed);
            return;
        }
        statJob->setUiDelegate(nullptr);
        m_currentSubJob = statJob;

        connect(statJob, &KJob::result, this, [this, src, callback](KJob *j) {
            if (m_killed || m_finished) return;
            auto *sj = qobject_cast<KIO::StatJob *>(j);
            RemoteSourceMetadata meta;
            if (!sj || sj->error() != 0) {
                meta.errorCode = sj ? sj->error() : KIO::ERR_CANNOT_STAT;
                meta.errorText = sj ? sj->errorString() : QStringLiteral("Błąd odczytu metadanych źródła MTP.");
                callback(meta);
                return;
            }
            const KIO::UDSEntry entry = sj->statResult();
            meta.exists = true;
            meta.name = entry.stringValue(KIO::UDSEntry::UDS_NAME);
            if (meta.name.isEmpty()) {
                meta.name = src.fileName();
            }
            const long long fileType = entry.numberValue(KIO::UDSEntry::UDS_FILE_TYPE, -1);
            meta.isDirectory = entry.isDir() || (fileType >= 0 && S_ISDIR(fileType));
            meta.isSymLink = entry.isLink();
            meta.isRegularFile = !meta.isDirectory && !meta.isSymLink
                && (fileType < 0 || S_ISREG(fileType));
            meta.size = entry.contains(KIO::UDSEntry::UDS_SIZE)
                ? static_cast<qint64>(entry.numberValue(KIO::UDSEntry::UDS_SIZE, -1))
                : -1;
            meta.mtimeSeconds = entry.contains(KIO::UDSEntry::UDS_MODIFICATION_TIME)
                ? static_cast<qint64>(entry.numberValue(KIO::UDSEntry::UDS_MODIFICATION_TIME, -1))
                : -1;
            callback(meta);
        });
    }

    void startCurrentFile()
    {
        if (m_killed || m_finished) return;

        if (m_currentIndex >= m_sources.size()) {
            m_finished = true;
            emitResult();
            return;
        }

        const QUrl src = m_sources.at(m_currentIndex);
        QString finalPath;
        if (m_sources.size() == 1 && m_singleTargetDestination.isValid() && m_singleTargetDestination.isLocalFile()) {
            finalPath = m_singleTargetDestination.toLocalFile();
        } else {
            finalPath = QDir(m_destinationDirectory.toLocalFile()).filePath(src.fileName());
        }

        m_currentFinalPath = finalPath;
        m_published = false;
        m_currentSource = src;
        m_currentPreCopyMeta = {};
        m_currentFileExpectedBytes = 0;

        if (m_move) {
            m_phaseLog.push_back(QStringLiteral("pre_stat"));
            querySourceMetadata(src, /*postPublishRecheck=*/false, [this, src](const RemoteSourceMetadata &meta) {
                if (m_killed || m_finished) return;
                if (meta.errorCode != 0) {
                    setError(meta.errorCode);
                    setErrorText(meta.errorText.isEmpty()
                        ? QStringLiteral("Nie udało się odczytać metadanych pliku źródłowego MTP przed przeniesieniem. Źródło zachowane.")
                        : meta.errorText);
                    m_finished = true;
                    emitResult();
                    return;
                }
                if (meta.isDirectory) {
                    setError(KIO::ERR_UNSUPPORTED_ACTION);
                    setErrorText(QStringLiteral(
                        "Bezpieczne przenoszenie całych folderów z urządzenia MTP nie jest obsługiwane (brak atomowej weryfikacji drzewa MTP). "
                        "Folder źródłowy pozostał nietknięty. Użyj operacji „Kopiuj”."));
                    m_finished = true;
                    emitResult();
                    return;
                }
                if (!meta.hasReliableFileIdentity() || meta.name != src.fileName()) {
                    setError(KIO::ERR_UNSUPPORTED_ACTION);
                    setErrorText(QStringLiteral(
                        "Metadane urządzenia MTP nie pozwalają niezawodnie zweryfikować tożsamości i rozmiaru pliku źródłowego. "
                        "Przenoszenie zostało odmówione — plik źródłowy pozostał nietknięty."));
                    m_finished = true;
                    emitResult();
                    return;
                }
                m_currentPreCopyMeta = meta;
                beginStagedCopyForCurrentFile();
            });
            return;
        }

        beginStagedCopyForCurrentFile();
    }

    void beginStagedCopyForCurrentFile()
    {
        if (m_killed || m_finished) return;

        const QUrl src = m_currentSource;
        const QString finalPath = m_currentFinalPath;

        // Safely generate and exclusively create a unique staging file owned by this task
        const QString partPath = KioStagedDetail::createExclusiveStagedFile(finalPath);
        if (partPath.isEmpty()) {
            setError(KIO::ERR_CANNOT_OPEN_FOR_WRITING);
            setErrorText(QStringLiteral("Nie można bezpiecznie utworzyć unikalnego pliku tymczasowego .thispc-part."));
            m_finished = true;
            emitResult();
            return;
        }

        m_currentPartPath = partPath;
        m_partCreated = true;
        if (m_move) {
            m_phaseLog.push_back(QStringLiteral("copy_to_part"));
        }

        const QUrl partUrl = QUrl::fromLocalFile(partPath);
        KJob *subJob = nullptr;
        if (m_subJobFactory) {
            subJob = m_subJobFactory(src, partUrl);
        } else {
            auto *copyJob = KIO::copyAs(src, partUrl, KIO::Overwrite | KIO::HideProgressInfo);
            if (copyJob && m_parentWidget) {
                if (KJobUiDelegate *delegate = KIO::createDefaultJobUiDelegate(
                        KJobUiDelegate::AutoHandlingDisabled, m_parentWidget)) {
                    copyJob->setUiDelegate(delegate);
                }
            }
            subJob = copyJob;
        }

        if (!subJob) {
            cleanupCurrentPart();
            setError(KIO::ERR_CANNOT_OPEN_FOR_WRITING);
            setErrorText(QStringLiteral("Nie można utworzyć zadania kopiowania KIO do pliku tymczasowego."));
            m_finished = true;
            emitResult();
            return;
        }

        m_currentSubJob = subJob;

        connect(subJob, &KJob::percentChanged, this, [this](KJob *, unsigned long p) {
            setPercent(p);
        });

        connect(subJob, &KJob::processedAmountChanged, this, [this](KJob *, KJob::Unit unit, qulonglong amount) {
            if (unit == KJob::Bytes) {
                setProcessedAmount(KJob::Bytes, m_completedBytesTotal + amount);
            }
        });

        connect(subJob, &KJob::totalAmountChanged, this, [this](KJob *, KJob::Unit unit, qulonglong amount) {
            if (unit == KJob::Bytes) {
                m_currentFileExpectedBytes = amount;
                setTotalAmount(KJob::Bytes, qMax(m_totalBytesKnown, amount));
            }
        });

        if (auto *kioJob = qobject_cast<KIO::Job*>(subJob)) {
            connect(kioJob, &KIO::Job::speed, this, [this](KJob *, unsigned long s) {
                emitSpeed(s);
            });
        }

        connect(subJob, &KJob::infoMessage, this, [this](KJob *, const QString &msg) {
            Q_EMIT infoMessage(this, msg);
        });

        connect(subJob, &KJob::result, this, [this](KJob *job) {
            onSubJobFinished(job);
        });

        if (m_subJobFactory) {
            subJob->start();
        }
    }

    void onSubJobFinished(KJob *job)
    {
        if (m_finished || m_killed) {
            cleanupCurrentPart();
            return;
        }

        const int err = job->error();
        if (err != 0) {
            cleanupCurrentPart();
            setError(err);
            setErrorText(job->errorString());
            m_finished = true;
            emitResult();
            return;
        }

        if (m_move) {
            m_phaseLog.push_back(QStringLiteral("verify_part"));
        }

        struct stat partStat {};
        const QByteArray partEncoded = QFile::encodeName(m_currentPartPath);
        if (::lstat(partEncoded.constData(), &partStat) != 0 || !S_ISREG(partStat.st_mode) || S_ISLNK(partStat.st_mode)) {
            cleanupCurrentPart();
            setError(KIO::ERR_CANNOT_WRITE);
            setErrorText(QStringLiteral("Plik tymczasowy transferu nie został zweryfikowany jako poprawny plik zwykły po zakończeniu zapisu."));
            m_finished = true;
            emitResult();
            return;
        }

        const qint64 partSize = partStat.st_size;

        if (m_move) {
            if (!m_currentPreCopyMeta.hasReliableFileIdentity()
                || partSize != m_currentPreCopyMeta.size
                || (m_currentFileExpectedBytes > 0 && partSize != static_cast<qint64>(m_currentFileExpectedBytes))) {
                cleanupCurrentPart();
                setError(KIO::ERR_CANNOT_READ);
                setErrorText(QStringLiteral(
                    "Rozmiar pobranego pliku tymczasowego nie zgadza się z rozmiarem źródła MTP. "
                    "Publikacja i usunięcie oryginału zostały zablokowane — plik źródłowy pozostał nietknięty."));
                m_finished = true;
                emitResult();
                return;
            }
        }

        if (m_move) {
            m_phaseLog.push_back(QStringLiteral("atomic_publish"));
        }

        bool published = false;
        bool collisionDetected = false;

        if (m_overwrite) {
            published = KioStagedDetail::atomicPublishOverwrite(
                m_currentPartPath, m_currentFinalPath, m_originalDestination);
            if (published) {
                setProperty("thispcDestinationReplaced", true);
            }
        } else {
            int publishErr = 0;
            published = KioStagedDetail::atomicPublishNewFile(
                m_currentPartPath, m_currentFinalPath, &collisionDetected, &publishErr);
            if (!published && (publishErr == EOPNOTSUPP || publishErr == ENOSYS || publishErr == EINVAL)) {
                setError(KIO::ERR_CANNOT_RENAME);
                setErrorText(QStringLiteral("System plików nie obsługuje bezpiecznej atomowej publikacji pliku bez nadpisania."));
                cleanupCurrentPart();
                m_finished = true;
                emitResult();
                return;
            }
        }

        if (!published) {
            cleanupCurrentPart();
            if (collisionDetected) {
                setError(KIO::ERR_FILE_ALREADY_EXIST);
                setErrorText(QStringLiteral("Plik docelowy pojawił się w trakcie transferu; publikacja została wstrzymana ze względów bezpieczeństwa."));
            } else {
                setError(KIO::ERR_CANNOT_RENAME);
                setErrorText(QStringLiteral("Nie można bezpiecznie opublikować gotowego pliku (błąd atomowej zmiany nazwy ze stagingu)."));
            }
            m_finished = true;
            emitResult();
            return;
        }

        // Verify the published local file on disk before ever considering source deletion
        struct stat finalStat {};
        const QByteArray finalEncoded = QFile::encodeName(m_currentFinalPath);
        if (::lstat(finalEncoded.constData(), &finalStat) != 0
            || !S_ISREG(finalStat.st_mode)
            || S_ISLNK(finalStat.st_mode)
            || finalStat.st_size != partSize) {
            setError(KIO::ERR_CANNOT_WRITE);
            setErrorText(QStringLiteral(
                "Nie udało się potwierdzić obecności i rozmiaru opublikowanego pliku docelowego na dysku lokalnym. "
                "Usunięcie źródła MTP zostało wstrzymane."));
            m_finished = true;
            emitResult();
            return;
        }

        m_published = true;
        m_partCreated = false;
        setProperty("thispcDestinationPublished", true);
        m_publishedFiles.push_back(m_currentFinalPath);
        m_completedBytesTotal += static_cast<qulonglong>(qMax<qint64>(0, partSize));

        if (!m_move) {
            advanceNext();
            return;
        }

        // Defense in depth: source deletion on MTP / non-local origins is blocked in 0.44.0.
        startVerifiedSourceDelete();
    }

    void startVerifiedSourceDelete()
    {
        if (m_killed || m_finished) return;

        // Hard safety gate: never execute KIO::del or custom source deletion in KioStagedTransferJob.
        setError(KIO::ERR_UNSUPPORTED_ACTION);
        setErrorText(trLocal(
            "Przenoszenie plików z udziałem urządzeń MTP jest niedostępne ze względów bezpieczeństwa danych. "
            "Użyj operacji Kopiuj.",
            "Moving files involving MTP devices is unavailable for data safety reasons. "
            "Please use Copy instead."));
        m_finished = true;
        emitResult();
    }

    void advanceNext()
    {
        m_currentIndex++;
        if (m_currentIndex < m_sources.size()) {
            startCurrentFile();
        } else {
            m_finished = true;
            emitResult();
        }
    }

    QList<QUrl> m_sources;
    QUrl m_destinationDirectory;
    bool m_move = false;
    bool m_twoPhaseMoveEnabled = false;
    QUrl m_singleTargetDestination;
    bool m_overwrite = false;
    LocalFileIdentity m_originalDestination;
    QWidget *m_parentWidget = nullptr;

    SubJobFactory m_subJobFactory;
    SourceStatProvider m_sourceStatProvider;
    SourceDeleteFactory m_sourceDeleteFactory;

    int m_currentIndex = 0;
    QString m_currentFinalPath;
    QString m_currentPartPath;
    QUrl m_currentSource;
    RemoteSourceMetadata m_currentPreCopyMeta;
    qulonglong m_currentFileExpectedBytes = 0;
    QPointer<KJob> m_currentSubJob;

    bool m_started = false;
    bool m_finished = false;
    bool m_killed = false;
    bool m_partCreated = false;
    bool m_published = false;
    bool m_partialMoveOutcome = false;

    int m_sourceDeleteAttempts = 0;
    int m_sourceDeletedCount = 0;
    QStringList m_publishedFiles;
    QStringList m_phaseLog;

    qulonglong m_completedBytesTotal = 0;
    qulonglong m_totalBytesKnown = 0;
};
