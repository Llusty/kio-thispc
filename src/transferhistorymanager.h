/*
 * Persistent Transfer History Model & Safe Storage for ThisPC 0.45.0.
 *
 * Stage 2.1 (History Model & Safe Storage).
 *
 * Provides:
 * - TransferHistoryRecord: value-type descriptor of a completed,
 *   cancelled, or failed transfer with full temporal, volume, and error metadata.
 * - TransferHistoryManager: thread-safe, non-blocking manager for storing,
 *   retrieving, ring-buffering (200 records FIFO), and atomically persisting transfer
 *   history using QSaveFile, QLockFile, CRDT delta synchronization, and XDG StateLocation.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "transfertaskdescriptor.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>
#include <optional>
#include <tuple>

static constexpr int THISPC_HISTORY_SCHEMA_VERSION = 1;
static constexpr int THISPC_HISTORY_DEFAULT_MAX_RECORDS = 200;
static constexpr qint64 THISPC_HISTORY_MAX_FILE_SIZE = 2 * 1024 * 1024; // 2 MiB
static constexpr int THISPC_HISTORY_MAX_STRING_LENGTH = 4096;
static constexpr int THISPC_HISTORY_MAX_TITLE_LENGTH = 256;

enum class HistoryStorageStatus {
    Ok = 0,
    FileNotFound,
    CorruptedJson,
    SchemaVersionTooNew,
    FileSizeExceeded,
    LockFailed,
    IoError,
    PermissionDenied
};

inline const char *historyStorageStatusString(HistoryStorageStatus status)
{
    switch (status) {
    case HistoryStorageStatus::Ok: return "Ok";
    case HistoryStorageStatus::FileNotFound: return "FileNotFound";
    case HistoryStorageStatus::CorruptedJson: return "CorruptedJson";
    case HistoryStorageStatus::SchemaVersionTooNew: return "SchemaVersionTooNew";
    case HistoryStorageStatus::FileSizeExceeded: return "FileSizeExceeded";
    case HistoryStorageStatus::LockFailed: return "LockFailed";
    case HistoryStorageStatus::IoError: return "IoError";
    case HistoryStorageStatus::PermissionDenied: return "PermissionDenied";
    }
    return "Unknown";
}

struct TransferHistoryRecord {
    QString uuid;
    TransferOperationType operationType = TransferOperationType::Copy;
    TransferBackendKind backendKind = TransferBackendKind::LocalNative;
    TransferTaskState terminalState = TransferTaskState::Completed;

    QList<QUrl> sourceUrls;
    QUrl destinationUrl;
    QList<QUrl> targetUrls;

    QDateTime startTimeUtc;
    QDateTime endTimeUtc;
    qint64 durationMs = 0;

    quint64 totalBytes = 0;
    quint64 processedBytes = 0;

    std::optional<quint64> averageSpeedBytesPerSecond;

    int errorCode = 0;
    QString errorText;

    QString title;
    int itemCount = 0;

    static QUrl sanitizeUrl(const QUrl &url)
    {
        if (!url.isValid()) {
            return url;
        }
        QUrl clean = url.adjusted(QUrl::RemovePassword | QUrl::RemoveUserInfo);
        if (clean.hasQuery()) {
            QUrlQuery query(clean.query());
            QUrlQuery cleanQuery;
            const auto items = query.queryItems(QUrl::FullyDecoded);
            for (const auto &item : items) {
                const QString keyLower = item.first.toLower();
                if (keyLower.contains(QStringLiteral("token")) ||
                    keyLower.contains(QStringLiteral("password")) ||
                    keyLower.contains(QStringLiteral("secret")) ||
                    keyLower.contains(QStringLiteral("auth")) ||
                    keyLower.contains(QStringLiteral("apikey")) ||
                    keyLower.contains(QStringLiteral("api_key")) ||
                    keyLower.contains(QStringLiteral("session"))) {
                    continue;
                }
                cleanQuery.addQueryItem(item.first, item.second);
            }
            clean.setQuery(cleanQuery);
        }
        return clean;
    }

    static QList<QUrl> sanitizeUrls(const QList<QUrl> &urls)
    {
        QList<QUrl> sanitized;
        sanitized.reserve(urls.size());
        for (const QUrl &u : urls) {
            sanitized.append(sanitizeUrl(u));
        }
        return sanitized;
    }

    static TransferHistoryRecord createNew(
        TransferOperationType opType,
        TransferBackendKind backend,
        TransferTaskState state,
        const QList<QUrl> &sources,
        const QUrl &destination = QUrl(),
        const QDateTime &startUtc = QDateTime::currentDateTimeUtc(),
        const QDateTime &endUtc = QDateTime::currentDateTimeUtc(),
        qint64 duration = 0,
        quint64 total = 0,
        quint64 processed = 0,
        std::optional<quint64> avgSpeed = std::nullopt,
        int errCode = 0,
        const QString &errText = QString(),
        const QString &recordTitle = QString())
    {
        TransferHistoryRecord rec;
        rec.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        rec.operationType = opType;
        rec.backendKind = backend;
        rec.terminalState = state;
        rec.sourceUrls = sanitizeUrls(sources);
        rec.destinationUrl = sanitizeUrl(destination);
        rec.startTimeUtc = startUtc.toUTC();
        rec.endTimeUtc = endUtc.toUTC();
        rec.durationMs = duration >= 0 ? duration : 0;
        rec.totalBytes = total;
        rec.processedBytes = processed;
        rec.averageSpeedBytesPerSecond = avgSpeed;
        rec.errorCode = errCode;
        rec.errorText = errText.left(THISPC_HISTORY_MAX_STRING_LENGTH);
        rec.title = recordTitle.left(THISPC_HISTORY_MAX_TITLE_LENGTH);
        rec.itemCount = sources.size();
        return rec;
    }

    static TransferHistoryRecord fromTaskSnapshot(
        const TransferTaskSnapshot &snapshot,
        const QDateTime &startUtc,
        const QDateTime &endUtc)
    {
        std::optional<quint64> avgSpeed;
        if (snapshot.averageSpeedBytesPerSecond > 0) {
            avgSpeed = snapshot.averageSpeedBytesPerSecond;
        }
        qint64 dur = 0;
        if (startUtc.isValid() && endUtc.isValid()) {
            dur = std::max<qint64>(0, startUtc.msecsTo(endUtc));
        }

        TransferHistoryRecord rec = createNew(
            snapshot.type,
            snapshot.backend,
            snapshot.state,
            snapshot.sources,
            snapshot.destination,
            startUtc,
            endUtc,
            dur,
            snapshot.totalBytes,
            snapshot.processedBytes,
            avgSpeed,
            snapshot.errorCode,
            snapshot.errorText,
            snapshot.title);
        rec.targetUrls = sanitizeUrls(snapshot.targetUrls);
        return rec;
    }

    bool isValid() const
    {
        if (uuid.isEmpty() || uuid.length() > 128) {
            return false;
        }
        if (!isTerminalTransferState(terminalState)) {
            return false;
        }
        if (!endTimeUtc.isValid()) {
            return false;
        }
        // startTimeUtc is allowed to be invalid only if state is Cancelled (e.g. cancelled while queued)
        if (terminalState != TransferTaskState::Cancelled && !startTimeUtc.isValid()) {
            return false;
        }
        return true;
    }

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj[QStringLiteral("uuid")] = uuid;
        obj[QStringLiteral("type")] = (operationType == TransferOperationType::Move)
            ? QStringLiteral("Move") : QStringLiteral("Copy");

        QString backendStr;
        switch (backendKind) {
        case TransferBackendKind::LocalNative: backendStr = QStringLiteral("LocalNative"); break;
        case TransferBackendKind::Kio:         backendStr = QStringLiteral("Kio"); break;
        default:                               backendStr = QStringLiteral("Unknown"); break;
        }
        obj[QStringLiteral("backend")] = backendStr;

        QString stateStr;
        switch (terminalState) {
        case TransferTaskState::Completed: stateStr = QStringLiteral("Completed"); break;
        case TransferTaskState::Cancelled: stateStr = QStringLiteral("Cancelled"); break;
        case TransferTaskState::Failed:    stateStr = QStringLiteral("Failed"); break;
        default:                           stateStr = QStringLiteral("Unknown"); break;
        }
        obj[QStringLiteral("state")] = stateStr;

        QJsonArray srcArr;
        for (const QUrl &u : sourceUrls) {
            srcArr.append(sanitizeUrl(u).toString());
        }
        obj[QStringLiteral("sources")] = srcArr;

        if (destinationUrl.isValid()) {
            obj[QStringLiteral("destination")] = sanitizeUrl(destinationUrl).toString();
        }

        if (!targetUrls.isEmpty()) {
            QJsonArray tgtArr;
            for (const QUrl &u : targetUrls) {
                tgtArr.append(sanitizeUrl(u).toString());
            }
            obj[QStringLiteral("targetUrls")] = tgtArr;
        }

        if (startTimeUtc.isValid()) {
            obj[QStringLiteral("startTime")] = startTimeUtc.toUTC().toString(Qt::ISODateWithMs);
        } else {
            obj[QStringLiteral("startTime")] = QString();
        }
        obj[QStringLiteral("endTime")] = endTimeUtc.toUTC().toString(Qt::ISODateWithMs);
        obj[QStringLiteral("durationMs")] = durationMs;

        obj[QStringLiteral("totalBytes")] = static_cast<qint64>(std::min<quint64>(totalBytes, 9007199254740991ULL));
        obj[QStringLiteral("totalBytesStr")] = QString::number(totalBytes);
        obj[QStringLiteral("processedBytes")] = static_cast<qint64>(std::min<quint64>(processedBytes, 9007199254740991ULL));
        obj[QStringLiteral("processedBytesStr")] = QString::number(processedBytes);

        if (averageSpeedBytesPerSecond.has_value()) {
            obj[QStringLiteral("averageSpeedBps")] = static_cast<qint64>(*averageSpeedBytesPerSecond);
        }

        obj[QStringLiteral("errorCode")] = errorCode;
        if (!errorText.isEmpty()) {
            obj[QStringLiteral("errorText")] = errorText.left(THISPC_HISTORY_MAX_STRING_LENGTH);
        }
        if (!title.isEmpty()) {
            obj[QStringLiteral("title")] = title.left(THISPC_HISTORY_MAX_TITLE_LENGTH);
        }
        if (itemCount > 0) {
            obj[QStringLiteral("itemCount")] = itemCount;
        }

        return obj;
    }

    static std::optional<TransferHistoryRecord> fromJson(const QJsonObject &obj)
    {
        TransferHistoryRecord rec;

        if (!obj.contains(QStringLiteral("uuid")) || !obj.value(QStringLiteral("uuid")).isString()) {
            return std::nullopt;
        }
        rec.uuid = obj.value(QStringLiteral("uuid")).toString();
        if (rec.uuid.isEmpty() || rec.uuid.length() > 128) {
            return std::nullopt;
        }

        const QString typeStr = obj.value(QStringLiteral("type")).toString();
        if (typeStr == QStringLiteral("Move")) {
            rec.operationType = TransferOperationType::Move;
        } else if (typeStr == QStringLiteral("Copy")) {
            rec.operationType = TransferOperationType::Copy;
        } else {
            return std::nullopt;
        }

        const QString backendStr = obj.value(QStringLiteral("backend")).toString();
        if (backendStr == QStringLiteral("LocalNative")) {
            rec.backendKind = TransferBackendKind::LocalNative;
        } else if (backendStr == QStringLiteral("Kio")) {
            rec.backendKind = TransferBackendKind::Kio;
        } else {
            rec.backendKind = TransferBackendKind::Unknown;
        }

        const QString stateStr = obj.value(QStringLiteral("state")).toString();
        if (stateStr == QStringLiteral("Completed")) {
            rec.terminalState = TransferTaskState::Completed;
        } else if (stateStr == QStringLiteral("Cancelled")) {
            rec.terminalState = TransferTaskState::Cancelled;
        } else if (stateStr == QStringLiteral("Failed")) {
            rec.terminalState = TransferTaskState::Failed;
        } else {
            return std::nullopt;
        }

        const QJsonValue srcVal = obj.value(QStringLiteral("sources"));
        if (!srcVal.isArray()) {
            return std::nullopt;
        }
        const QJsonArray srcArr = srcVal.toArray();
        for (const QJsonValue &item : srcArr) {
            if (item.isString()) {
                const QString str = item.toString();
                if (str.length() <= THISPC_HISTORY_MAX_STRING_LENGTH) {
                    rec.sourceUrls.append(sanitizeUrl(QUrl(str)));
                }
            }
        }

        if (obj.contains(QStringLiteral("destination")) && obj.value(QStringLiteral("destination")).isString()) {
            const QString dstStr = obj.value(QStringLiteral("destination")).toString();
            if (dstStr.length() <= THISPC_HISTORY_MAX_STRING_LENGTH) {
                rec.destinationUrl = sanitizeUrl(QUrl(dstStr));
            }
        }

        if (obj.contains(QStringLiteral("targetUrls")) && obj.value(QStringLiteral("targetUrls")).isArray()) {
            const QJsonArray tgtArr = obj.value(QStringLiteral("targetUrls")).toArray();
            for (const QJsonValue &item : tgtArr) {
                if (item.isString()) {
                    const QString str = item.toString();
                    if (str.length() <= THISPC_HISTORY_MAX_STRING_LENGTH) {
                        rec.targetUrls.append(sanitizeUrl(QUrl(str)));
                    }
                }
            }
        }

        if (obj.contains(QStringLiteral("startTime")) && obj.value(QStringLiteral("startTime")).isString()) {
            const QString startStr = obj.value(QStringLiteral("startTime")).toString();
            if (!startStr.isEmpty()) {
                rec.startTimeUtc = QDateTime::fromString(startStr, Qt::ISODateWithMs);
                if (!rec.startTimeUtc.isValid()) {
                    rec.startTimeUtc = QDateTime::fromString(startStr, Qt::ISODate);
                }
            }
        }
        if (!rec.startTimeUtc.isValid() && rec.terminalState != TransferTaskState::Cancelled) {
            return std::nullopt;
        }

        if (!obj.contains(QStringLiteral("endTime")) || !obj.value(QStringLiteral("endTime")).isString()) {
            return std::nullopt;
        }
        rec.endTimeUtc = QDateTime::fromString(obj.value(QStringLiteral("endTime")).toString(), Qt::ISODateWithMs);
        if (!rec.endTimeUtc.isValid()) {
            rec.endTimeUtc = QDateTime::fromString(obj.value(QStringLiteral("endTime")).toString(), Qt::ISODate);
        }
        if (!rec.endTimeUtc.isValid()) {
            return std::nullopt;
        }

        if (obj.contains(QStringLiteral("durationMs"))) {
            rec.durationMs = obj.value(QStringLiteral("durationMs")).toInteger();
            if (rec.durationMs < 0) rec.durationMs = 0;
        } else if (rec.startTimeUtc.isValid() && rec.endTimeUtc.isValid()) {
            rec.durationMs = std::max<qint64>(0, rec.startTimeUtc.msecsTo(rec.endTimeUtc));
        } else {
            rec.durationMs = 0;
        }

        if (obj.contains(QStringLiteral("totalBytesStr")) && obj.value(QStringLiteral("totalBytesStr")).isString()) {
            rec.totalBytes = obj.value(QStringLiteral("totalBytesStr")).toString().toULongLong();
        } else if (obj.contains(QStringLiteral("totalBytes"))) {
            rec.totalBytes = static_cast<quint64>(std::max<qint64>(0, obj.value(QStringLiteral("totalBytes")).toInteger()));
        }

        if (obj.contains(QStringLiteral("processedBytesStr")) && obj.value(QStringLiteral("processedBytesStr")).isString()) {
            rec.processedBytes = obj.value(QStringLiteral("processedBytesStr")).toString().toULongLong();
        } else if (obj.contains(QStringLiteral("processedBytes"))) {
            rec.processedBytes = static_cast<quint64>(std::max<qint64>(0, obj.value(QStringLiteral("processedBytes")).toInteger()));
        }

        if (obj.contains(QStringLiteral("averageSpeedBps"))) {
            rec.averageSpeedBytesPerSecond = static_cast<quint64>(std::max<qint64>(0, obj.value(QStringLiteral("averageSpeedBps")).toInteger()));
        }

        if (obj.contains(QStringLiteral("errorCode"))) {
            rec.errorCode = obj.value(QStringLiteral("errorCode")).toInt();
        }
        if (obj.contains(QStringLiteral("errorText")) && obj.value(QStringLiteral("errorText")).isString()) {
            rec.errorText = obj.value(QStringLiteral("errorText")).toString().left(THISPC_HISTORY_MAX_STRING_LENGTH);
        }

        if (obj.contains(QStringLiteral("title")) && obj.value(QStringLiteral("title")).isString()) {
            rec.title = obj.value(QStringLiteral("title")).toString().left(THISPC_HISTORY_MAX_TITLE_LENGTH);
        }

        if (obj.contains(QStringLiteral("itemCount"))) {
            rec.itemCount = obj.value(QStringLiteral("itemCount")).toInt();
        } else {
            rec.itemCount = rec.sourceUrls.size();
        }

        return rec;
    }

    bool operator==(const TransferHistoryRecord &other) const
    {
        return uuid == other.uuid
            && operationType == other.operationType
            && backendKind == other.backendKind
            && terminalState == other.terminalState
            && sourceUrls == other.sourceUrls
            && destinationUrl == other.destinationUrl
            && targetUrls == other.targetUrls
            && startTimeUtc == other.startTimeUtc
            && endTimeUtc == other.endTimeUtc
            && durationMs == other.durationMs
            && totalBytes == other.totalBytes
            && processedBytes == other.processedBytes
            && averageSpeedBytesPerSecond == other.averageSpeedBytesPerSecond
            && errorCode == other.errorCode
            && errorText == other.errorText
            && title == other.title
            && itemCount == other.itemCount;
    }

    bool operator!=(const TransferHistoryRecord &other) const
    {
        return !(*this == other);
    }
};

// ------------------------------------------------------------------
// Stage 4: Show in Folder ("Pokaż w folderze") eligibility & resolution
// ------------------------------------------------------------------

enum class ShowInFolderIneligibilityReason {
    None = 0,
    NotCompleted,
    InvalidDestination,
    RemoteLocationUnsupported,
    DisconnectedOrMtpMedia,
    UnsupportedScheme,
    UnsafePath,
    CorruptedRecord,
    DestinationDirectoryMissing,
    TargetItemMissing
};

struct ShowInFolderResolution {
    bool eligible = false;
    ShowInFolderIneligibilityReason reason = ShowInFolderIneligibilityReason::None;
    QUrl folderUrl;
    QList<QUrl> selectUrls;
    QString message;
};

namespace ShowInFolderHelper {

inline bool containsDotOrDotDotSegment(const QString &rawPath)
{
    const QString normalized = QString(rawPath).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QLatin1String("..") || part == QLatin1String(".")) {
            return true;
        }
    }
    return false;
}

inline bool containsDotOrDotDotSegment(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty()) {
        return false;
    }
    const QString rawPath = url.isLocalFile() ? url.toLocalFile() : url.path(QUrl::FullyDecoded);
    return containsDotOrDotDotSegment(rawPath) || containsDotOrDotDotSegment(url.path(QUrl::FullyDecoded));
}

inline QString extractSafeEntryName(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty() || containsDotOrDotDotSegment(url)) {
        return {};
    }
    const QString rawPath = url.isLocalFile() ? url.toLocalFile() : url.path();
    if (rawPath.isEmpty() || containsDotOrDotDotSegment(rawPath)) {
        return {};
    }
    if (url.isLocalFile() && !QDir::isAbsolutePath(rawPath)) {
        return {};
    }
    QString trimmed = rawPath;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/'))) {
        trimmed.chop(1);
    }
    if (trimmed.isEmpty() || trimmed == QLatin1String("/")) {
        return {};
    }
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    const QString name = (slash >= 0) ? trimmed.mid(slash + 1) : trimmed;
    if (name.isEmpty()
        || name == QLatin1String(".")
        || name == QLatin1String("..")
        || name.contains(QLatin1Char('/'))
        || name.contains(QLatin1Char('\\'))
        || name.contains(QChar(0))) {
        return {};
    }
    return name;
}

} // namespace ShowInFolderHelper

inline ShowInFolderResolution resolveShowInFolder(
    TransferTaskState state,
    const QList<QUrl> &sourceUrls,
    const QUrl &destinationUrl,
    const QList<QUrl> &targetUrls,
    int itemCount)
{
    ShowInFolderResolution res;
    auto reject = [&res](ShowInFolderIneligibilityReason reason, const QString &msg) {
        res.eligible = false;
        res.reason = reason;
        res.message = msg;
        return res;
    };

    // Rule 1: Only Completed operations are eligible.
    if (state != TransferTaskState::Completed) {
        return reject(ShowInFolderIneligibilityReason::NotCompleted,
                      trLocal("Akcja „Pokaż w folderze” jest dostępna wyłącznie dla pomyślnie zakończonych operacji",
                              "\"Show in folder\" is only available for successfully completed operations"));
    }

    if (!destinationUrl.isValid() || destinationUrl.isEmpty()) {
        return reject(ShowInFolderIneligibilityReason::InvalidDestination,
                      trLocal("Brak prawidłowego katalogu docelowego w zapisie operacji",
                              "No valid destination directory in operation record"));
    }

    const QString scheme = destinationUrl.scheme().toLower();
    if (scheme == QLatin1String("mtp")) {
        return reject(ShowInFolderIneligibilityReason::DisconnectedOrMtpMedia,
                      trLocal("Lokalizacje MTP nie wspierają bezpośredniego otwarcia i zaznaczenia pliku",
                              "MTP locations do not support direct folder opening and file selection"));
    }

    if (RemoteUrlHelper::isRemoteUrl(destinationUrl)
        || scheme == QLatin1String("fish")
        || scheme == QLatin1String("nfs")
        || scheme == QLatin1String("ssh")) {
        return reject(ShowInFolderIneligibilityReason::RemoteLocationUnsupported,
                      trLocal("Lokalizacje zdalne (%1://) nie wspierają bezpiecznego otwarcia i zaznaczenia elementu",
                              "Remote locations (%1://) do not support safe folder opening and item selection")
                          .arg(scheme));
    }

    if (!destinationUrl.isLocalFile()) {
        return reject(ShowInFolderIneligibilityReason::UnsupportedScheme,
                      trLocal("Nieobsługiwany schemat docelowy (%1:) dla akcji „Pokaż w folderze”",
                              "Unsupported destination scheme (%1:) for \"Show in folder\"")
                          .arg(scheme.isEmpty() ? QStringLiteral("unknown") : scheme));
    }

    const QString rawDst = destinationUrl.toLocalFile();
    if (rawDst.isEmpty()
        || !QDir::isAbsolutePath(rawDst)
        || ShowInFolderHelper::containsDotOrDotDotSegment(destinationUrl)
        || ShowInFolderHelper::containsDotOrDotDotSegment(rawDst)) {
        return reject(ShowInFolderIneligibilityReason::UnsafePath,
                      trLocal("Ścieżka docelowa jest względna lub zawiera niedozwolone segmenty „..”",
                              "Destination path is relative or contains forbidden \"..\" segments"));
    }

    const QString cleanDst = QDir::cleanPath(rawDst);
    if (cleanDst.isEmpty() || !QDir::isAbsolutePath(cleanDst)) {
        return reject(ShowInFolderIneligibilityReason::UnsafePath,
                      trLocal("Nieprawidłowa ścieżka katalogu docelowego",
                              "Invalid destination directory path"));
    }

#ifndef Q_OS_WIN
    struct stat dstSt {};
    if (::lstat(QFile::encodeName(cleanDst).constData(), &dstSt) != 0) {
        return reject(ShowInFolderIneligibilityReason::DestinationDirectoryMissing,
                      trLocal("Katalog docelowy nie istnieje lub nośnik został odłączony",
                              "Destination directory does not exist or medium was disconnected"));
    }
    if (S_ISLNK(dstSt.st_mode)) {
        return reject(ShowInFolderIneligibilityReason::UnsafePath,
                      trLocal("Katalog docelowy jest dowiązaniem symbolicznym — nawigacja zablokowana dla bezpieczeństwa",
                              "Destination directory is a symbolic link — navigation refused for safety"));
    }
    if (!S_ISDIR(dstSt.st_mode)) {
        return reject(ShowInFolderIneligibilityReason::DestinationDirectoryMissing,
                      trLocal("Ścieżka docelowa nie jest katalogiem",
                              "Destination path is not a directory"));
    }
#endif

    // Validate sourceUrls and itemCount consistency
    if (sourceUrls.isEmpty() || itemCount <= 0 || itemCount != sourceUrls.size()) {
        return reject(ShowInFolderIneligibilityReason::CorruptedRecord,
                      trLocal("Rekord historii operacji jest niekompletny lub uszkodzony",
                              "Operation history record is incomplete or corrupted"));
    }

    if (!targetUrls.isEmpty() && targetUrls.size() != sourceUrls.size()) {
        return reject(ShowInFolderIneligibilityReason::CorruptedRecord,
                      trLocal("Rekord docelowych ścieżek operacji jest niespójny",
                              "Operation target paths record is inconsistent"));
    }

    QStringList candidateTargetPaths;
    candidateTargetPaths.reserve(sourceUrls.size());

    for (int i = 0; i < sourceUrls.size(); ++i) {
        const QString srcName = ShowInFolderHelper::extractSafeEntryName(sourceUrls.at(i));
        if (srcName.isEmpty()) {
            return reject(ShowInFolderIneligibilityReason::CorruptedRecord,
                          trLocal("Nieprawidłowa ścieżka elementu źródłowego w zapisie operacji",
                                  "Invalid source item path in operation record"));
        }

        QString targetPath;
        if (!targetUrls.isEmpty()) {
            const QUrl &tgtUrl = targetUrls.at(i);
            if (!tgtUrl.isValid() || !tgtUrl.isLocalFile()) {
                return reject(ShowInFolderIneligibilityReason::CorruptedRecord,
                              trLocal("Nieprawidłowy docelowy URL elementu",
                                      "Invalid target item URL"));
            }
            const QString rawTgt = tgtUrl.toLocalFile();
            if (rawTgt.isEmpty()
                || !QDir::isAbsolutePath(rawTgt)
                || ShowInFolderHelper::containsDotOrDotDotSegment(rawTgt)) {
                return reject(ShowInFolderIneligibilityReason::UnsafePath,
                              trLocal("Docelowa ścieżka elementu zawiera niedozwolone segmenty",
                                      "Target item path contains forbidden segments"));
            }
            const QString tgtName = ShowInFolderHelper::extractSafeEntryName(tgtUrl);
            if (tgtName.isEmpty()) {
                return reject(ShowInFolderIneligibilityReason::CorruptedRecord,
                              trLocal("Nieprawidłowa nazwa pliku docelowego",
                                      "Invalid target file name"));
            }
            const QString cleanTgt = QDir::cleanPath(rawTgt);
            const QString parentDir = QDir::cleanPath(QFileInfo(cleanTgt).path());
            if (parentDir != cleanDst) {
                return reject(ShowInFolderIneligibilityReason::UnsafePath,
                              trLocal("Ścieżka docelowego pliku wykracza poza katalog docelowy",
                                      "Target file path escapes destination directory"));
            }
            targetPath = cleanTgt;
        } else {
            targetPath = QDir(cleanDst).filePath(srcName);
        }

        if (targetPath.endsWith(QStringLiteral(".thispc-part"))) {
            return reject(ShowInFolderIneligibilityReason::TargetItemMissing,
                          trLocal("Wskazany element jest niekompletnym plikiem tymczasowym (.thispc-part)",
                                  "Target item is an incomplete temporary staging file (.thispc-part)"));
        }

        candidateTargetPaths.append(targetPath);
    }

    // Rule 2: Single-item transfer -> verify the exact target item exists (regular file, dir, or symlink)
    if (candidateTargetPaths.size() == 1) {
        const QString &itemPath = candidateTargetPaths.first();
#ifndef Q_OS_WIN
        struct stat itemSt {};
        if (::lstat(QFile::encodeName(itemPath).constData(), &itemSt) != 0) {
            return reject(ShowInFolderIneligibilityReason::TargetItemMissing,
                          trLocal("Plik lub katalog docelowy nie istnieje już w folderze docelowym",
                                  "Target file or directory no longer exists in destination folder"));
        }
#endif
        res.eligible = true;
        res.reason = ShowInFolderIneligibilityReason::None;
        res.folderUrl = QUrl::fromLocalFile(cleanDst);
        res.selectUrls = {QUrl::fromLocalFile(itemPath)};
        res.message = trLocal("Otwórz folder docelowy i zaznacz przeniesiony/skopiowany element",
                              "Open destination folder and select transferred item");
        return res;
    }

    // Rule 3: Multi-item transfer -> open destination directory without selecting a single random item,
    // provided at least one transferred item still exists in the destination directory.
    bool anyItemExists = false;
#ifndef Q_OS_WIN
    for (const QString &itemPath : candidateTargetPaths) {
        struct stat itemSt {};
        if (::lstat(QFile::encodeName(itemPath).constData(), &itemSt) == 0) {
            anyItemExists = true;
            break;
        }
    }
#endif
    if (!anyItemExists) {
        return reject(ShowInFolderIneligibilityReason::TargetItemMissing,
                      trLocal("Żaden z przetransferowanych elementów nie znajduje się już w katalogu docelowym",
                              "None of the transferred items exist in the destination directory anymore"));
    }

    res.eligible = true;
    res.reason = ShowInFolderIneligibilityReason::None;
    res.folderUrl = QUrl::fromLocalFile(cleanDst);
    res.selectUrls.clear();
    res.message = trLocal("Otwórz folder docelowy operacji wieloelementowej",
                          "Open destination folder of multi-item operation");
    return res;
}

inline ShowInFolderResolution resolveShowInFolder(const TransferTaskSnapshot &task)
{
    return resolveShowInFolder(
        task.state,
        task.sources,
        task.destination,
        task.targetUrls,
        static_cast<int>(task.sources.size()));
}

inline ShowInFolderResolution resolveShowInFolder(const TransferHistoryRecord &rec)
{
    return resolveShowInFolder(
        rec.terminalState,
        rec.sourceUrls,
        rec.destinationUrl,
        rec.targetUrls,
        rec.itemCount);
}

class TransferHistoryManager final : public QObject
{
    Q_OBJECT

public:
    explicit TransferHistoryManager(const QString &filePath = QString(), QObject *parent = nullptr)
        : QObject(parent)
        , m_filePath(filePath.isEmpty() ? defaultHistoryFilePath() : filePath)
        , m_debounceTimer(new QTimer(this))
        , m_reloadDebounceTimer(new QTimer(this))
    {
        m_debounceTimer->setSingleShot(true);
        connect(m_debounceTimer, &QTimer::timeout, this, [this]() {
            saveAsyncInternal();
        });

        m_reloadDebounceTimer->setSingleShot(true);
        m_reloadDebounceTimer->setInterval(150);
        connect(m_reloadDebounceTimer, &QTimer::timeout, this, [this]() {
            reloadFromDiskAsync();
        });

        setupWatcher();
    }

    ~TransferHistoryManager() override
    {
        if (m_reloadDebounceTimer) {
            m_reloadDebounceTimer->stop();
        }
        if (m_activeReloadFuture.isRunning()) {
            m_activeReloadFuture.waitForFinished();
        }
        flushSync();
    }

    static QString defaultHistoryDirectory()
    {
        QString stateDir = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
        if (stateDir.isEmpty()) {
            QString generic = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
            if (!generic.isEmpty()) {
                stateDir = generic + QStringLiteral("/kio-thispc/thispc-view");
            } else {
                stateDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            }
        }
        return stateDir;
    }

    static QString defaultHistoryFilePath()
    {
        return defaultHistoryDirectory() + QStringLiteral("/operations_history.json");
    }

    QString filePath() const
    {
        QMutexLocker locker(&m_mutex);
        return m_filePath;
    }

    void setFilePath(const QString &path)
    {
        {
            QMutexLocker locker(&m_mutex);
            flushSyncLocked();
            m_filePath = path.isEmpty() ? defaultHistoryFilePath() : path;
            m_loaded = false;
            m_dirty = false;
            m_records.clear();
            m_locallyAddedUuids.clear();
            m_locallyDeletedUuids.clear();
            m_clearRequested = false;
            m_historyEpoch = 1;
            m_lastDiskMtimeUtc = QDateTime();
            m_lastDiskFileSize = -1;
        }
        updateWatcherRegistration();
    }

    bool syncWithDiskIfNeeded()
    {
        {
            QMutexLocker locker(&m_mutex);
            if (m_saveWorkerRunning || m_dirty) {
                return false;
            }
            QFileInfo fi(m_filePath);
            if (!fi.exists()) {
                if (!m_records.isEmpty() && m_lastDiskFileSize > 0) {
                    m_records.clear();
                    m_locallyAddedUuids.clear();
                    m_locallyDeletedUuids.clear();
                    m_lastDiskMtimeUtc = QDateTime();
                    m_lastDiskFileSize = 0;
                    locker.unlock();
                    Q_EMIT historyCleared();
                    Q_EMIT recordsChanged();
                    return true;
                }
                return false;
            }
            if (m_loaded && fi.lastModified() == m_lastDiskMtimeUtc && fi.size() == m_lastDiskFileSize) {
                return false;
            }
        }
        return loadSync();
    }

    int maxRecords() const
    {
        QMutexLocker locker(&m_mutex);
        return m_maxRecords;
    }

    bool isEnabled() const
    {
        QMutexLocker locker(&m_mutex);
        return m_enabled;
    }

    void setEnabled(bool enabled)
    {
        bool changed = false;
        int currentMax = 0;
        {
            QMutexLocker locker(&m_mutex);
            if (m_enabled != enabled) {
                m_enabled = enabled;
                changed = true;
            }
            currentMax = m_maxRecords;
        }
        if (changed) {
            Q_EMIT settingsChanged(enabled, currentMax);
        }
    }

    quint64 historyEpoch() const
    {
        QMutexLocker locker(&m_mutex);
        return m_historyEpoch;
    }

    void setMaxRecords(int count)
    {
        bool changed = false;
        bool enabled = true;
        int newMax = 0;
        {
            QMutexLocker locker(&m_mutex);
            const int clamped = std::clamp(count, 1, 5000);
            if (m_maxRecords != clamped) {
                m_maxRecords = clamped;
                trimRecordsLocked();
                m_dirty = true;
                changed = true;
                scheduleSaveAsyncLocked();
            }
            enabled = m_enabled;
            newMax = m_maxRecords;
        }
        if (changed) {
            Q_EMIT recordsChanged();
            Q_EMIT settingsChanged(enabled, newMax);
        }
    }

    void loadSettings(QSettings &settings)
    {
        settings.beginGroup(QStringLiteral("TransferHistory"));
        const QVariant enabledVar = settings.value(QStringLiteral("HistoryEnabled"));
        const QVariant maxVar = settings.value(QStringLiteral("HistoryMaxRecords"));
        settings.endGroup();

        if (enabledVar.isValid()) {
            setEnabled(enabledVar.toBool());
        }
        if (maxVar.isValid()) {
            bool ok = false;
            const int maxRec = maxVar.toInt(&ok);
            if (ok && (maxRec == 50 || maxRec == 100 || maxRec == 200 || maxRec == 500)) {
                setMaxRecords(maxRec);
            } else if (ok && maxRec > 0) {
                setMaxRecords(std::clamp(maxRec, 1, 5000));
            }
        }
    }

    void saveSettings(QSettings &settings) const
    {
        settings.beginGroup(QStringLiteral("TransferHistory"));
        settings.setValue(QStringLiteral("HistoryEnabled"), isEnabled());
        settings.setValue(QStringLiteral("HistoryMaxRecords"), maxRecords());
        settings.endGroup();
    }

    int count() const
    {
        QMutexLocker locker(&m_mutex);
        ensureLoadedLocked();
        return m_records.size();
    }

    bool isEmpty() const
    {
        QMutexLocker locker(&m_mutex);
        ensureLoadedLocked();
        return m_records.isEmpty();
    }

    QList<TransferHistoryRecord> records() const
    {
        QMutexLocker locker(&m_mutex);
        ensureLoadedLocked();
        return m_records;
    }

    std::optional<TransferHistoryRecord> findRecord(const QString &uuid) const
    {
        QMutexLocker locker(&m_mutex);
        ensureLoadedLocked();
        for (const auto &rec : m_records) {
            if (rec.uuid == uuid) {
                return rec;
            }
        }
        return std::nullopt;
    }

    void addRecord(const TransferHistoryRecord &record, bool persistAsync = true)
    {
        if (!record.isValid()) {
            THISPC_TRACE("TransferHistoryManager::addRecord: rejected invalid record");
            return;
        }

        TransferHistoryRecord recCopy = record;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_enabled) {
                return;
            }
            ensureLoadedLocked();

            // Check if record already exists by UUID
            for (int i = 0; i < m_records.size(); ++i) {
                if (m_records[i].uuid == recCopy.uuid) {
                    m_records[i] = recCopy;
                    m_dirty = true;
                    m_locallyAddedUuids.insert(recCopy.uuid);
                    m_locallyDeletedUuids.remove(recCopy.uuid);
                    if (persistAsync) {
                        scheduleSaveAsyncLocked();
                    }
                    return;
                }
            }

            // Insert newest first
            m_records.prepend(recCopy);
            m_locallyAddedUuids.insert(recCopy.uuid);
            m_locallyDeletedUuids.remove(recCopy.uuid);
            trimRecordsLocked();
            m_dirty = true;

            if (persistAsync) {
                scheduleSaveAsyncLocked();
            }
        }

        Q_EMIT recordAdded(recCopy);
        Q_EMIT recordsChanged();
    }

    bool removeRecord(const QString &uuid, bool syncToDisk = true)
    {
        bool found = false;
        {
            QMutexLocker locker(&m_mutex);
            ensureLoadedLocked();

            for (int i = 0; i < m_records.size(); ++i) {
                if (m_records[i].uuid == uuid) {
                    m_records.removeAt(i);
                    found = true;
                    m_dirty = true;
                    m_locallyDeletedUuids.insert(uuid);
                    m_locallyAddedUuids.remove(uuid);
                    break;
                }
            }

            if (found && syncToDisk) {
                scheduleSaveAsyncLocked();
            }
        }

        if (found) {
            Q_EMIT recordRemoved(uuid);
            Q_EMIT recordsChanged();
        }
        return found;
    }

    void clear(bool syncToDisk = true)
    {
        {
            QMutexLocker locker(&m_mutex);
            m_records.clear();
            m_locallyAddedUuids.clear();
            m_locallyDeletedUuids.clear();
            m_clearRequested = true;
            m_dirty = true;

            if (syncToDisk) {
                scheduleSaveAsyncLocked();
            }
        }

        Q_EMIT historyCleared();
        Q_EMIT recordsChanged();
    }

    bool loadSync(QString *errorDiagnostic = nullptr, HistoryStorageStatus *statusOut = nullptr)
    {
        bool changed = false;
        bool cleared = false;
        bool ok = false;
        {
            QMutexLocker locker(&m_mutex);
            ok = loadSyncLocked(&changed, &cleared, errorDiagnostic, statusOut);
        }
        if (cleared) {
            Q_EMIT historyCleared();
        }
        if (changed) {
            Q_EMIT recordsChanged();
        }
        return ok;
    }

    bool saveSync(QString *errorDiagnostic = nullptr, HistoryStorageStatus *statusOut = nullptr)
    {
        QMutexLocker locker(&m_mutex);
        return saveSyncLocked(errorDiagnostic, statusOut);
    }

    void flushSync()
    {
        QMutexLocker locker(&m_mutex);
        flushSyncLocked();
    }

    HistoryStorageStatus lastStatus() const
    {
        QMutexLocker locker(&m_mutex);
        return m_lastStatus;
    }

    QString lastDiagnostic() const
    {
        QMutexLocker locker(&m_mutex);
        return m_lastDiagnostic;
    }

    bool hasPendingSaves() const
    {
        QMutexLocker locker(&m_mutex);
        return m_debounceTimer->isActive() || m_saveWorkerRunning || m_dirty || m_clearRequested;
    }

Q_SIGNALS:
    void recordsChanged();
    void recordAdded(const TransferHistoryRecord &record);
    void recordRemoved(const QString &uuid);
    void historyCleared();
    void storageError(HistoryStorageStatus status, const QString &diagnostic);
    void saveCompleted(bool success);
    void settingsChanged(bool enabled, int maxRecords);

private:
    void ensureLoadedLocked() const
    {
        if (!m_loaded) {
            const_cast<TransferHistoryManager*>(this)->loadSyncLocked();
        }
    }

    void trimRecordsLocked()
    {
        while (m_records.size() > m_maxRecords) {
            m_records.removeLast();
        }
    }

    void scheduleSaveAsyncLocked()
    {
        if (!m_debounceTimer->isActive()) {
            m_debounceTimer->start(100);
        }
    }

    void flushSyncLocked()
    {
        if (m_debounceTimer->isActive()) {
            m_debounceTimer->stop();
        }
        if (m_activeSaveFuture.isRunning()) {
            m_mutex.unlock();
            m_activeSaveFuture.waitForFinished();
            m_mutex.lock();
        }
        if (m_dirty || m_clearRequested) {
            saveSyncLocked();
        }
    }

    void updateMemoryAfterSaveLocked(const QList<TransferHistoryRecord> &merged, quint64 newEpoch)
    {
        QFileInfo fi(m_filePath);
        if (fi.exists()) {
            m_lastDiskMtimeUtc = fi.lastModified();
            m_lastDiskFileSize = fi.size();
        }
        if (m_fileWatcher && fi.exists() && !fi.isSymLink() && !m_fileWatcher->files().contains(fi.absoluteFilePath())) {
            m_fileWatcher->addPath(fi.absoluteFilePath());
        }

        if (newEpoch > m_historyEpoch) {
            // An external clear was observed; drop obsolete local pending operations
            m_records = merged;
            m_historyEpoch = newEpoch;
            m_locallyAddedUuids.clear();
            m_locallyDeletedUuids.clear();
            trimRecordsLocked();
            return;
        }

        m_historyEpoch = newEpoch;

        // Any record added to m_locallyAddedUuids while worker was saving must be retained
        const QSet<QString> freshlyAddedUuids = m_locallyAddedUuids;
        m_locallyDeletedUuids.clear();

        QList<TransferHistoryRecord> freshList = merged;
        QSet<QString> seen;
        for (const auto &r : freshList) {
            seen.insert(r.uuid);
        }

        // Keep any freshly added records that arrived while worker was executing
        for (const auto &r : m_records) {
            if (freshlyAddedUuids.contains(r.uuid) && !seen.contains(r.uuid)) {
                freshList.prepend(r);
                seen.insert(r.uuid);
            }
        }

        // Clear added set for items that were merged
        for (const auto &uuid : seen) {
            m_locallyAddedUuids.remove(uuid);
        }

        m_records = freshList;
        trimRecordsLocked();
    }

    void saveAsyncInternal()
    {
        QList<TransferHistoryRecord> recs;
        QSet<QString> added;
        QSet<QString> deleted;
        bool isClear = false;
        QString path;
        int maxRecs = THISPC_HISTORY_DEFAULT_MAX_RECORDS;
        quint64 loadedEpoch = 1;

        {
            QMutexLocker locker(&m_mutex);
            if (!m_dirty && !m_clearRequested) {
                return;
            }
            if (m_saveWorkerRunning) {
                // A save worker is already executing. Mark that another save is needed
                // immediately after it completes so requests are never dropped or reordered.
                m_savePendingAfterWorker = true;
                return;
            }

            m_saveWorkerRunning = true;
            m_savePendingAfterWorker = false;

            recs = m_records;
            added = m_locallyAddedUuids;
            deleted = m_locallyDeletedUuids;
            isClear = m_clearRequested;
            path = m_filePath;
            maxRecs = m_maxRecords;
            loadedEpoch = m_historyEpoch;

            m_dirty = false;
            m_clearRequested = false;
        }

        m_activeSaveFuture = QtConcurrent::run([path, recs, added, deleted, isClear, maxRecs, loadedEpoch]()
            -> std::tuple<bool, HistoryStorageStatus, QString, QList<TransferHistoryRecord>, quint64> {
            QString diag;
            HistoryStorageStatus status = HistoryStorageStatus::Ok;
            QList<TransferHistoryRecord> merged;
            quint64 newEpoch = loadedEpoch;
            bool ok = executeDiskSave(path, recs, added, deleted, isClear, maxRecs, loadedEpoch, &merged, &newEpoch, &diag, &status);
            return {ok, status, diag, merged, newEpoch};
        });

        auto watcher = new QFutureWatcher<std::tuple<bool, HistoryStorageStatus, QString, QList<TransferHistoryRecord>, quint64>>(this);
        connect(watcher, &QFutureWatcher<std::tuple<bool, HistoryStorageStatus, QString, QList<TransferHistoryRecord>, quint64>>::finished, this, [this, watcher]() {
            auto [ok, status, diag, merged, newEpoch] = watcher->result();
            watcher->deleteLater();

            bool needReschedule = false;
            {
                QMutexLocker locker(&m_mutex);
                m_saveWorkerRunning = false;
                m_lastStatus = status;
                m_lastDiagnostic = diag;
                if (ok) {
                    updateMemoryAfterSaveLocked(merged, newEpoch);
                } else {
                    m_dirty = true;
                }
                if (m_savePendingAfterWorker || m_dirty) {
                    needReschedule = true;
                }
            }

            if (!ok) {
                Q_EMIT storageError(status, diag);
            }
            Q_EMIT saveCompleted(ok);

            if (needReschedule) {
                scheduleSaveAsyncLocked();
            }
        });
        watcher->setFuture(m_activeSaveFuture);
    }

    bool loadSyncLocked(bool *outChanged = nullptr, bool *outCleared = nullptr,
                        QString *errorDiagnostic = nullptr, HistoryStorageStatus *statusOut = nullptr)
    {
        m_loaded = true;
        QString diag;
        HistoryStorageStatus status = HistoryStorageStatus::Ok;
        quint64 epoch = 1;

        QList<TransferHistoryRecord> loaded = executeDiskLoad(m_filePath, &diag, &status, &epoch);
        m_lastStatus = status;
        m_lastDiagnostic = diag;

        if (statusOut) {
            *statusOut = status;
        }
        if (errorDiagnostic) {
            *errorDiagnostic = diag;
        }

        if (status == HistoryStorageStatus::Ok || status == HistoryStorageStatus::FileNotFound) {
            QFileInfo fi(m_filePath);
            if (fi.exists()) {
                m_lastDiskMtimeUtc = fi.lastModified();
                m_lastDiskFileSize = fi.size();
            } else {
                m_lastDiskMtimeUtc = QDateTime();
                m_lastDiskFileSize = 0;
            }

            const QList<TransferHistoryRecord> oldRecords = m_records;
            const quint64 oldEpoch = m_historyEpoch;

            if (epoch > oldEpoch) {
                m_records = loaded;
                m_historyEpoch = epoch;
                m_locallyAddedUuids.clear();
                m_locallyDeletedUuids.clear();
            } else {
                QSet<QString> diskUuids;
                QList<TransferHistoryRecord> merged;
                for (const auto &rec : loaded) {
                    if (!m_locallyDeletedUuids.contains(rec.uuid)) {
                        merged.append(rec);
                        diskUuids.insert(rec.uuid);
                    }
                }
                for (const auto &memRec : m_records) {
                    if (m_locallyAddedUuids.contains(memRec.uuid) && !diskUuids.contains(memRec.uuid)) {
                        merged.prepend(memRec);
                        diskUuids.insert(memRec.uuid);
                    }
                }
                std::sort(merged.begin(), merged.end(), [](const TransferHistoryRecord &a, const TransferHistoryRecord &b) {
                    if (a.endTimeUtc != b.endTimeUtc) return a.endTimeUtc > b.endTimeUtc;
                    return a.startTimeUtc > b.startTimeUtc;
                });
                m_records = merged;
                m_historyEpoch = epoch;
            }
            trimRecordsLocked();

            const bool changed = (oldRecords != m_records);
            const bool cleared = (epoch > oldEpoch && m_records.isEmpty() && !oldRecords.isEmpty());

            if (m_fileWatcher && fi.exists() && !fi.isSymLink() && !m_fileWatcher->files().contains(fi.absoluteFilePath())) {
                m_fileWatcher->addPath(fi.absoluteFilePath());
            }

            if (outCleared) *outCleared = cleared;
            if (outChanged) *outChanged = changed;
            return true;
        }

        THISPC_TRACE("TransferHistoryManager::loadSync: %s - %s", historyStorageStatusString(status), qPrintable(diag));
        Q_EMIT storageError(status, diag);
        return false;
    }

    bool saveSyncLocked(QString *errorDiagnostic = nullptr, HistoryStorageStatus *statusOut = nullptr)
    {
        QString diag;
        HistoryStorageStatus status = HistoryStorageStatus::Ok;
        QList<TransferHistoryRecord> merged;
        quint64 newEpoch = m_historyEpoch;

        bool ok = executeDiskSave(m_filePath, m_records, m_locallyAddedUuids, m_locallyDeletedUuids,
                                  m_clearRequested, m_maxRecords, m_historyEpoch, &merged, &newEpoch, &diag, &status);
        m_lastStatus = status;
        m_lastDiagnostic = diag;

        if (statusOut) {
            *statusOut = status;
        }
        if (errorDiagnostic) {
            *errorDiagnostic = diag;
        }

        if (ok) {
            m_dirty = false;
            m_clearRequested = false;
            updateMemoryAfterSaveLocked(merged, newEpoch);
        } else {
            THISPC_TRACE("TransferHistoryManager::saveSync: failed %s - %s", historyStorageStatusString(status), qPrintable(diag));
            Q_EMIT storageError(status, diag);
        }
        Q_EMIT saveCompleted(ok);
        return ok;
    }

    static QList<TransferHistoryRecord> executeDiskLoad(
        const QString &filePath,
        QString *diagnostic,
        HistoryStorageStatus *status,
        quint64 *outEpoch = nullptr)
    {
        QFileInfo fi(filePath);
        if (fi.isSymLink()) {
            if (status) *status = HistoryStorageStatus::PermissionDenied;
            if (diagnostic) *diagnostic = QStringLiteral("Symlink history storage paths are prohibited for security");
            return {};
        }

        if (!QFile::exists(filePath)) {
            if (status) *status = HistoryStorageStatus::FileNotFound;
            if (outEpoch) *outEpoch = 1;
            return {};
        }

        if (fi.size() > THISPC_HISTORY_MAX_FILE_SIZE) {
            if (status) *status = HistoryStorageStatus::FileSizeExceeded;
            if (diagnostic) *diagnostic = QStringLiteral("History file size (%1 B) exceeds maximum limit (2 MiB)").arg(fi.size());
            return {};
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            if (status) *status = HistoryStorageStatus::IoError;
            if (diagnostic) *diagnostic = file.errorString();
            return {};
        }

        const QByteArray data = file.readAll();
        file.close();

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (status) *status = HistoryStorageStatus::CorruptedJson;
            if (diagnostic) *diagnostic = QStringLiteral("JSON parse error: %1").arg(parseError.errorString());
            return {};
        }

        const QJsonObject rootObj = doc.object();
        const int schemaVer = rootObj.value(QStringLiteral("schemaVersion")).toInt(1);
        if (schemaVer > THISPC_HISTORY_SCHEMA_VERSION) {
            if (status) *status = HistoryStorageStatus::SchemaVersionTooNew;
            if (diagnostic) *diagnostic = QStringLiteral("Schema version %1 is newer than supported %2")
                .arg(schemaVer).arg(THISPC_HISTORY_SCHEMA_VERSION);
            return {};
        }

        quint64 epoch = 1;
        if (rootObj.contains(QStringLiteral("historyEpoch"))) {
            epoch = static_cast<quint64>(rootObj.value(QStringLiteral("historyEpoch")).toInteger(1));
            if (epoch == 0) epoch = 1;
        }
        if (outEpoch) {
            *outEpoch = epoch;
        }

        const QJsonValue recVal = rootObj.value(QStringLiteral("records"));
        if (!recVal.isArray()) {
            if (status) *status = HistoryStorageStatus::CorruptedJson;
            if (diagnostic) *diagnostic = QStringLiteral("Missing or invalid 'records' array in JSON root");
            return {};
        }

        QList<TransferHistoryRecord> result;
        const QJsonArray recArr = recVal.toArray();
        for (const QJsonValue &item : recArr) {
            if (item.isObject()) {
                auto parsed = TransferHistoryRecord::fromJson(item.toObject());
                if (parsed.has_value()) {
                    result.append(*parsed);
                } else {
                    THISPC_TRACE("TransferHistoryManager: skipped invalid record object in history file");
                }
            }
        }

        if (status) *status = HistoryStorageStatus::Ok;
        return result;
    }

    static bool executeDiskSave(
        const QString &filePath,
        const QList<TransferHistoryRecord> &inMemoryRecords,
        const QSet<QString> &locallyAddedUuids,
        const QSet<QString> &locallyDeletedUuids,
        bool isClearOperation,
        int maxRecords,
        quint64 loadedEpoch,
        QList<TransferHistoryRecord> *outMergedRecords,
        quint64 *outNewEpoch,
        QString *diagnostic,
        HistoryStorageStatus *status)
    {
        // 0. Prohibit symlinks in target path
        QFileInfo fi(filePath);
        if (fi.isSymLink()) {
            if (status) *status = HistoryStorageStatus::PermissionDenied;
            if (diagnostic) *diagnostic = QStringLiteral("Symlink history storage paths are prohibited for security");
            return false;
        }

        // 1. Ensure directory exists and has 0700 permissions
        QDir dir = fi.dir();
        if (!dir.exists()) {
            if (!dir.mkpath(QStringLiteral("."))) {
                if (status) *status = HistoryStorageStatus::IoError;
                if (diagnostic) *diagnostic = QStringLiteral("Could not create directory: ") + dir.absolutePath();
                return false;
            }
            QFile::setPermissions(dir.absolutePath(),
                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }

        // 2. Multi-instance lock
        QLockFile lock(filePath + QStringLiteral(".lock"));
        lock.setStaleLockTime(3000);
        if (!lock.tryLock(500)) {
            if (status) *status = HistoryStorageStatus::LockFailed;
            if (diagnostic) *diagnostic = QStringLiteral("Failed to acquire history lock within 500 ms");
            return false;
        }

        // 3. Inspect existing disk file and merge deltas
        QList<TransferHistoryRecord> merged;
        quint64 diskEpoch = loadedEpoch;

        if (QFile::exists(filePath)) {
            QString loadDiag;
            HistoryStorageStatus loadStatus = HistoryStorageStatus::Ok;
            quint64 epochFromDisk = 1;
            QList<TransferHistoryRecord> diskRecords = executeDiskLoad(filePath, &loadDiag, &loadStatus, &epochFromDisk);

            if (loadStatus == HistoryStorageStatus::SchemaVersionTooNew) {
                lock.unlock();
                if (status) *status = HistoryStorageStatus::SchemaVersionTooNew;
                if (diagnostic) *diagnostic = QStringLiteral("Aborting save: disk file schema version is newer than supported");
                return false;
            }

            diskEpoch = epochFromDisk;

            if (isClearOperation) {
                merged.clear();
                diskEpoch = std::max(epochFromDisk, loadedEpoch) + 1;
            } else if (loadStatus == HistoryStorageStatus::CorruptedJson) {
                // Preserve corrupted file non-destructively before proceeding
                const qint64 ts = QDateTime::currentMSecsSinceEpoch();
                QString backupPath = filePath + QStringLiteral(".corrupted.") + QString::number(ts);
                int counter = 1;
                while (QFile::exists(backupPath) && counter < 1000) {
                    backupPath = filePath + QStringLiteral(".corrupted.") + QString::number(ts)
                        + QStringLiteral(".") + QString::number(counter++);
                }
                if (QFile::copy(filePath, backupPath)) {
                    QFile::setPermissions(backupPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                    THISPC_TRACE("TransferHistoryManager: backed up corrupted history file to %s", qPrintable(backupPath));
                }
                merged = inMemoryRecords;
            } else if (loadStatus == HistoryStorageStatus::Ok) {
                if (diskEpoch > loadedEpoch) {
                    // SECURITY GATE A:
                    // An external clear() bumped diskEpoch on disk.
                    // Local additions that were staged under the old epoch MUST be discarded
                    // to prevent resurrection of cleared history.
                    merged = diskRecords;
                } else {
                    // Keep disk records unless they were explicitly deleted locally
                    QSet<QString> diskUuids;
                    for (const auto &diskRec : diskRecords) {
                        if (!locallyDeletedUuids.contains(diskRec.uuid)) {
                            merged.append(diskRec);
                            diskUuids.insert(diskRec.uuid);
                        }
                    }

                    // Append or update records added in this session
                    for (const auto &memRec : inMemoryRecords) {
                        if (locallyAddedUuids.contains(memRec.uuid)) {
                            if (!diskUuids.contains(memRec.uuid)) {
                                merged.append(memRec);
                                diskUuids.insert(memRec.uuid);
                            } else {
                                // Update existing record
                                for (int i = 0; i < merged.size(); ++i) {
                                    if (merged[i].uuid == memRec.uuid) {
                                        merged[i] = memRec;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                merged = inMemoryRecords;
            }
        } else {
            if (isClearOperation) {
                merged.clear();
                diskEpoch = loadedEpoch + 1;
            } else {
                merged = inMemoryRecords;
            }
        }

        // 4. Sort descending by endTimeUtc / startTimeUtc and trim
        std::sort(merged.begin(), merged.end(), [](const TransferHistoryRecord &a, const TransferHistoryRecord &b) {
            if (a.endTimeUtc != b.endTimeUtc) {
                return a.endTimeUtc > b.endTimeUtc;
            }
            return a.startTimeUtc > b.startTimeUtc;
        });

        while (merged.size() > maxRecords) {
            merged.removeLast();
        }

        // 5. Serialize JSON
        QJsonObject rootObj;
        rootObj[QStringLiteral("schemaVersion")] = THISPC_HISTORY_SCHEMA_VERSION;
        rootObj[QStringLiteral("historyEpoch")] = static_cast<qint64>(diskEpoch);
        QJsonArray arr;
        for (const auto &r : merged) {
            arr.append(r.toJson());
        }
        rootObj[QStringLiteral("records")] = arr;

        QByteArray jsonBytes = QJsonDocument(rootObj).toJson(QJsonDocument::Compact);
        if (jsonBytes.size() > THISPC_HISTORY_MAX_FILE_SIZE) {
            // Stage 2.5: Enforce hard size limit (2 MiB) with oldest-record trimming priority.
            while (merged.size() > 1 && jsonBytes.size() > THISPC_HISTORY_MAX_FILE_SIZE) {
                merged.removeLast();
                QJsonArray smallerArr;
                for (const auto &r : merged) {
                    smallerArr.append(r.toJson());
                }
                rootObj[QStringLiteral("records")] = smallerArr;
                jsonBytes = QJsonDocument(rootObj).toJson(QJsonDocument::Compact);
            }
            if (jsonBytes.size() > THISPC_HISTORY_MAX_FILE_SIZE) {
                lock.unlock();
                if (status) *status = HistoryStorageStatus::FileSizeExceeded;
                if (diagnostic) *diagnostic = QStringLiteral("Payload size (%1 B) exceeds 2 MiB limit").arg(jsonBytes.size());
                return false;
            }
        }

        if (outMergedRecords) {
            *outMergedRecords = merged;
        }
        if (outNewEpoch) {
            *outNewEpoch = diskEpoch;
        }

        // 6. Write atomically with QSaveFile
        QSaveFile saveFile(filePath);
        saveFile.setDirectWriteFallback(false);

        if (!saveFile.open(QIODevice::WriteOnly)) {
            lock.unlock();
            const auto err = saveFile.error();
            if (err == QFileDevice::PermissionsError) {
                if (status) *status = HistoryStorageStatus::PermissionDenied;
            } else {
                if (status) *status = HistoryStorageStatus::IoError;
            }
            if (diagnostic) *diagnostic = saveFile.errorString();
            return false;
        }

        if (saveFile.write(jsonBytes) != jsonBytes.size()) {
            saveFile.cancelWriting();
            lock.unlock();
            if (status) *status = HistoryStorageStatus::IoError;
            if (diagnostic) *diagnostic = saveFile.errorString();
            return false;
        }

        if (!saveFile.commit()) {
            lock.unlock();
            if (status) *status = HistoryStorageStatus::IoError;
            if (diagnostic) *diagnostic = saveFile.errorString();
            return false;
        }

        // Set private permissions 0600 on saved history file
        QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        lock.unlock();
        if (status) *status = HistoryStorageStatus::Ok;
        return true;
    }

    void setupWatcher()
    {
        if (!m_fileWatcher) {
            m_fileWatcher = new QFileSystemWatcher(this);
            connect(m_fileWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
                onFileSystemChanged(path);
            });
            connect(m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
                onFileSystemChanged(path);
            });
        }
        updateWatcherRegistration();
    }

    void updateWatcherRegistration()
    {
        if (!m_fileWatcher) return;

        const QStringList watchedFiles = m_fileWatcher->files();
        if (!watchedFiles.isEmpty()) m_fileWatcher->removePaths(watchedFiles);
        const QStringList watchedDirs = m_fileWatcher->directories();
        if (!watchedDirs.isEmpty()) m_fileWatcher->removePaths(watchedDirs);

        QString path;
        {
            QMutexLocker locker(&m_mutex);
            path = m_filePath;
        }

        QFileInfo fi(path);
        QDir dir = fi.dir();
        if (dir.exists()) {
            m_fileWatcher->addPath(dir.absolutePath());
        }
        if (fi.exists() && !fi.isSymLink()) {
            m_fileWatcher->addPath(fi.absoluteFilePath());
        }
    }

    void onFileSystemChanged(const QString &changedPath)
    {
        Q_UNUSED(changedPath)
        {
            QMutexLocker locker(&m_mutex);
            if (m_saveWorkerRunning) {
                return;
            }
        }

        QString path;
        {
            QMutexLocker locker(&m_mutex);
            path = m_filePath;
        }

        QFileInfo fi(path);
        if (fi.exists() && !fi.isSymLink() && m_fileWatcher && !m_fileWatcher->files().contains(fi.absoluteFilePath())) {
            m_fileWatcher->addPath(fi.absoluteFilePath());
        }

        if (fi.exists()) {
            QMutexLocker locker(&m_mutex);
            if (m_loaded && fi.lastModified() == m_lastDiskMtimeUtc && fi.size() == m_lastDiskFileSize) {
                return;
            }
        }

        if (m_reloadDebounceTimer && !m_reloadDebounceTimer->isActive()) {
            m_reloadDebounceTimer->start();
        }
    }

    void reloadFromDiskAsync()
    {
        QString path;
        quint64 currentEpoch = 1;
        QList<TransferHistoryRecord> currentRecords;
        QSet<QString> locallyAdded;
        QSet<QString> locallyDeleted;
        int maxRecs = THISPC_HISTORY_DEFAULT_MAX_RECORDS;
        {
            QMutexLocker locker(&m_mutex);
            if (m_saveWorkerRunning || m_reloadWorkerRunning || m_dirty) {
                return;
            }
            QFileInfo fi(m_filePath);
            if (fi.exists() && m_loaded && fi.lastModified() == m_lastDiskMtimeUtc && fi.size() == m_lastDiskFileSize) {
                return;
            }
            m_reloadWorkerRunning = true;
            path = m_filePath;
            currentEpoch = m_historyEpoch;
            currentRecords = m_records;
            locallyAdded = m_locallyAddedUuids;
            locallyDeleted = m_locallyDeletedUuids;
            maxRecs = m_maxRecords;
        }

        m_activeReloadFuture = QtConcurrent::run([path, currentEpoch, currentRecords, locallyAdded, locallyDeleted, maxRecs]()
            -> std::tuple<bool, QList<TransferHistoryRecord>, quint64, QDateTime, qint64> {
            QString diag;
            HistoryStorageStatus status = HistoryStorageStatus::Ok;
            quint64 diskEpoch = 1;
            QList<TransferHistoryRecord> diskRecords = executeDiskLoad(path, &diag, &status, &diskEpoch);
            if (status != HistoryStorageStatus::Ok && status != HistoryStorageStatus::FileNotFound) {
                return {false, {}, currentEpoch, QDateTime(), -1};
            }
            QFileInfo fi(path);
            QDateTime mtime = fi.exists() ? fi.lastModified() : QDateTime();
            qint64 size = fi.exists() ? fi.size() : 0;

            QList<TransferHistoryRecord> merged;
            if (diskEpoch > currentEpoch) {
                merged = diskRecords;
            } else {
                QSet<QString> diskUuids;
                for (const auto &rec : diskRecords) {
                    if (!locallyDeleted.contains(rec.uuid)) {
                        merged.append(rec);
                        diskUuids.insert(rec.uuid);
                    }
                }
                for (const auto &memRec : currentRecords) {
                    if (locallyAdded.contains(memRec.uuid) && !diskUuids.contains(memRec.uuid)) {
                        merged.prepend(memRec);
                        diskUuids.insert(memRec.uuid);
                    }
                }
            }
            std::sort(merged.begin(), merged.end(), [](const TransferHistoryRecord &a, const TransferHistoryRecord &b) {
                if (a.endTimeUtc != b.endTimeUtc) return a.endTimeUtc > b.endTimeUtc;
                return a.startTimeUtc > b.startTimeUtc;
            });
            while (merged.size() > maxRecs) {
                merged.removeLast();
            }
            return {true, merged, diskEpoch, mtime, size};
        });

        auto watcher = new QFutureWatcher<std::tuple<bool, QList<TransferHistoryRecord>, quint64, QDateTime, qint64>>(this);
        connect(watcher, &QFutureWatcher<std::tuple<bool, QList<TransferHistoryRecord>, quint64, QDateTime, qint64>>::finished,
                this, [this, watcher]() {
            auto [ok, merged, diskEpoch, mtime, size] = watcher->result();
            watcher->deleteLater();

            bool hasChanged = false;
            bool cleared = false;
            QString currentPath;
            {
                QMutexLocker locker(&m_mutex);
                m_reloadWorkerRunning = false;
                if (ok) {
                    m_lastDiskMtimeUtc = mtime;
                    m_lastDiskFileSize = size;
                    m_loaded = true;
                    if (diskEpoch > m_historyEpoch) {
                        m_historyEpoch = diskEpoch;
                        m_locallyAddedUuids.clear();
                        m_locallyDeletedUuids.clear();
                        m_records = merged;
                        hasChanged = true;
                        cleared = true;
                    } else {
                        if (m_records != merged) {
                            m_records = merged;
                            hasChanged = true;
                        }
                        m_historyEpoch = diskEpoch;
                    }
                }
                currentPath = m_filePath;
            }

            QFileInfo fi(currentPath);
            if (fi.exists() && !fi.isSymLink() && m_fileWatcher && !m_fileWatcher->files().contains(fi.absoluteFilePath())) {
                m_fileWatcher->addPath(fi.absoluteFilePath());
            }

            if (cleared) {
                Q_EMIT historyCleared();
            }
            if (hasChanged) {
                Q_EMIT recordsChanged();
            }
        });
        watcher->setFuture(m_activeReloadFuture);
    }

    mutable QMutex m_mutex;
    QString m_filePath;
    bool m_enabled = true;
    int m_maxRecords = THISPC_HISTORY_DEFAULT_MAX_RECORDS;
    quint64 m_historyEpoch = 1;
    QList<TransferHistoryRecord> m_records;
    QSet<QString> m_locallyAddedUuids;
    QSet<QString> m_locallyDeletedUuids;
    bool m_clearRequested = false;
    bool m_dirty = false;
    mutable bool m_loaded = false;

    bool m_saveWorkerRunning = false;
    bool m_savePendingAfterWorker = false;
    bool m_reloadWorkerRunning = false;

    QTimer *m_debounceTimer = nullptr;
    QTimer *m_reloadDebounceTimer = nullptr;
    QFileSystemWatcher *m_fileWatcher = nullptr;
    QDateTime m_lastDiskMtimeUtc;
    qint64 m_lastDiskFileSize = -1;

    QFuture<std::tuple<bool, HistoryStorageStatus, QString, QList<TransferHistoryRecord>, quint64>> m_activeSaveFuture;
    QFuture<std::tuple<bool, QList<TransferHistoryRecord>, quint64, QDateTime, qint64>> m_activeReloadFuture;

    HistoryStorageStatus m_lastStatus = HistoryStorageStatus::Ok;
    QString m_lastDiagnostic;
};
