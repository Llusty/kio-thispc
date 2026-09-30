/*
 * Read-only Batch Rename crash audit and process-wide mutation fence.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "batchrename.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <cerrno>
#include <atomic>
#include <memory>
#include <utility>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

enum class BatchRenameRecoveryKind { V1Manual, V2, Unknown, Corrupt, SuspiciousTemporary };
enum class BatchRenameRecoveryLock { None, Live, Stale, Ambiguous };
enum class BatchRenameRecoveryMapping { NotApplicable, Initial, Partial, Complete, ForeignOrOccupied, Ambiguous };

struct BatchRenameRecoveryItem {
    int row = -1;
    QString source;
    QString destination;
    quint64 device = 0;
    quint64 inode = 0;
    quint64 mode = 0;
    qint64 size = -1;
    qint64 mtimeNs = -1;
    QString type;
};

struct BatchRenameRecoveryEntry {
    QString path;
    BatchRenameRecoveryKind kind = BatchRenameRecoveryKind::Corrupt;
    QString error;
    QJsonObject object;
    QList<BatchRenameRecoveryItem> items;
    QString direction;
    int completedSteps = -1;
    quint64 directoryDevice = 0;
    quint64 directoryInode = 0;
    BatchRenameRecoveryMapping mapping = BatchRenameRecoveryMapping::NotApplicable;
};

struct BatchRenameRecoveryAudit {
    QString root;
    QList<BatchRenameRecoveryEntry> entries;
    BatchRenameRecoveryLock writerLock = BatchRenameRecoveryLock::None;
    bool inspectorLockHeld = false;
    bool blocked = false;
    QString summary;
};

namespace BatchRenameRecoveryDetail {
#ifdef Q_OS_UNIX
inline int openPrivateRecoveryRoot(const QString &path)
{
    const QByteArray encoded = QFile::encodeName(QDir::cleanPath(path));
    if (encoded.isEmpty() || encoded.at(0) != '/') return -1;
    int directory = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -1;
    const QList<QByteArray> components = encoded.split('/');
    for (const QByteArray &component : components) {
        if (component.isEmpty()) continue;
        int next = ::openat(directory, component.constData(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT) {
            if (::mkdirat(directory, component.constData(), 0700) != 0 && errno != EEXIST) {
                ::close(directory);
                return -1;
            }
            next = ::openat(directory, component.constData(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        ::close(directory);
        if (next < 0) return -1;
        directory = next;
    }
    struct stat status {};
    if (::fstat(directory, &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
        ::close(directory);
        return -1;
    }
    return directory;
}
#endif

inline bool decimalU64(const QJsonValue &value, quint64 *result)
{
    if (!value.isString() || value.toString().isEmpty()) return false;
    const QString text = value.toString();
    for (QChar c : text) if (!c.isDigit()) return false;
    bool ok = false;
    const quint64 parsed = text.toULongLong(&ok);
    if (ok && result) *result = parsed;
    return ok && parsed != 0;
}

inline QByteArray digestPayload(QJsonObject object)
{
    object.remove(QStringLiteral("digest"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

inline QString digest(QJsonObject object)
{
    return QString::fromLatin1(QCryptographicHash::hash(digestPayload(std::move(object)),
        QCryptographicHash::Sha256).toHex());
}

inline bool exactKeys(const QJsonObject &object, const QSet<QString> &required,
                      const QSet<QString> &optional = {})
{
    for (const QString &key : required) if (!object.contains(key)) return false;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!required.contains(it.key()) && !optional.contains(it.key())) return false;
    return true;
}

inline BatchRenameRecoveryMapping inspectMapping(const BatchRenameRecoveryEntry &entry)
{
#ifdef Q_OS_UNIX
    if (entry.object.value(QStringLiteral("kind")).toString() == QStringLiteral("swap")) {
        if (entry.items.size() != 2) return BatchRenameRecoveryMapping::Ambiguous;
        const auto matches = [](const BatchRenameRecoveryItem &item, const QString &path) {
            struct stat st {};
            if (::lstat(QFile::encodeName(path).constData(), &st) != 0) return false;
            const QString type = S_ISLNK(st.st_mode) ? QStringLiteral("symlink")
                : (S_ISDIR(st.st_mode) ? QStringLiteral("directory")
                                       : (S_ISREG(st.st_mode) ? QStringLiteral("file") : QString()));
            const qint64 mtime = qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
            return type == item.type && quint64(st.st_dev) == item.device
                && quint64(st.st_ino) == item.inode && quint64(st.st_mode) == item.mode
                && qint64(st.st_size) == item.size && mtime == item.mtimeNs;
        };
        const bool before = matches(entry.items.at(0), entry.items.at(0).source)
            && matches(entry.items.at(1), entry.items.at(1).source);
        const bool after = matches(entry.items.at(1), entry.items.at(0).source)
            && matches(entry.items.at(0), entry.items.at(1).source);
        if (before == after) return BatchRenameRecoveryMapping::ForeignOrOccupied;
        return before ? BatchRenameRecoveryMapping::Initial : BatchRenameRecoveryMapping::Complete;
    }
    if (entry.items.isEmpty() || entry.completedSteps < 0
        || entry.completedSteps > entry.items.size()) return BatchRenameRecoveryMapping::Ambiguous;
    QSet<QString> expectedOccupied;
    QHash<QString, QPair<quint64, quint64>> expected;
    QSet<QString> touched;
    for (int i = 0; i < entry.items.size(); ++i) {
        const auto &item = entry.items.at(i);
        const bool moved = entry.direction == QStringLiteral("undo")
            ? i >= entry.items.size() - entry.completedSteps : i < entry.completedSteps;
        const QString occupied = QDir::cleanPath(moved ? item.destination : item.source);
        touched.insert(QDir::cleanPath(item.source));
        touched.insert(QDir::cleanPath(item.destination));
        if (expectedOccupied.contains(occupied)) return BatchRenameRecoveryMapping::Ambiguous;
        expectedOccupied.insert(occupied);
        expected.insert(occupied, {item.device, item.inode});
    }
    for (const QString &path : touched) {
        struct stat st {};
        const QByteArray encoded = QFile::encodeName(path);
        if (::lstat(encoded.constData(), &st) == 0) {
            if (!expected.contains(path) || S_ISLNK(st.st_mode))
                return BatchRenameRecoveryMapping::ForeignOrOccupied;
            const auto identity = expected.value(path);
            if (quint64(st.st_dev) != identity.first || quint64(st.st_ino) != identity.second)
                return BatchRenameRecoveryMapping::ForeignOrOccupied;
        } else if (errno != ENOENT || expected.contains(path)) {
            return BatchRenameRecoveryMapping::Ambiguous;
        }
    }
    if (entry.completedSteps == 0) return BatchRenameRecoveryMapping::Initial;
    if (entry.completedSteps == entry.items.size()) return BatchRenameRecoveryMapping::Complete;
    return BatchRenameRecoveryMapping::Partial;
#else
    Q_UNUSED(entry)
    return BatchRenameRecoveryMapping::Ambiguous;
#endif
}
}

inline BatchRenameRecoveryEntry parseBatchRenameRecoveryJournal(const QString &path)
{
    BatchRenameRecoveryEntry result;
    result.path = path;
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() < 2 || info.size() > 1024 * 1024) {
        result.error = QStringLiteral("Journal is not a bounded regular file.");
        return result;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.read(1024 * 1024 + 1), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("Invalid JSON: ") + parseError.errorString();
        return result;
    }
    result.object = document.object();
    const QJsonValue schema = result.object.value(QStringLiteral("schema"));
    if (schema.isDouble() && schema.toInt(-1) == 1) { result.kind = BatchRenameRecoveryKind::V1Manual; return result; }
    if (!schema.isDouble() || schema.toInt(-1) != 2) {
        result.kind = BatchRenameRecoveryKind::Unknown;
        result.error = QStringLiteral("Unsupported journal schema.");
        return result;
    }
    const QSet<QString> keys{QStringLiteral("schema"), QStringLiteral("scope"), QStringLiteral("kind"),
        QStringLiteral("directory"), QStringLiteral("direction"), QStringLiteral("phase"),
        QStringLiteral("completedSteps"), QStringLiteral("directoryDevice"),
        QStringLiteral("directoryInode"), QStringLiteral("items"), QStringLiteral("digest")};
    if (!BatchRenameRecoveryDetail::exactKeys(result.object, keys)
        || result.object.value(QStringLiteral("scope")).toString() != QStringLiteral("recovery-audit-v2")
        || !result.object.value(QStringLiteral("digest")).isString()
        || result.object.value(QStringLiteral("digest")).toString().compare(
            BatchRenameRecoveryDetail::digest(result.object), Qt::CaseInsensitive) != 0) {
        result.error = QStringLiteral("V2 schema or digest validation failed.");
        return result;
    }
    const QString kind = result.object.value(QStringLiteral("kind")).toString();
    const QString directory = QDir::cleanPath(result.object.value(QStringLiteral("directory")).toString());
    result.direction = result.object.value(QStringLiteral("direction")).toString();
    result.completedSteps = result.object.value(QStringLiteral("completedSteps")).toInt(-1);
    if (!BatchRenameRecoveryDetail::decimalU64(result.object.value(QStringLiteral("directoryDevice")),
                                                &result.directoryDevice)
        || !BatchRenameRecoveryDetail::decimalU64(result.object.value(QStringLiteral("directoryInode")),
                                                   &result.directoryInode)) {
        result.error = QStringLiteral("V2 directory identity is invalid.");
        return result;
    }
    const QString phase = result.object.value(QStringLiteral("phase")).toString();
    const QJsonArray items = result.object.value(QStringLiteral("items")).toArray();
    const QSet<QString> linearPhases{QStringLiteral("prepared"), QStringLiteral("step-intent"),
        QStringLiteral("step-verified"), QStringLiteral("verified-complete")};
    const QSet<QString> swapPhases{QStringLiteral("prepared"), QStringLiteral("exchange-intent"),
        QStringLiteral("exchange-verified"), QStringLiteral("verified-complete")};
    const QSet<QString> phases = kind == QStringLiteral("swap") ? swapPhases : linearPhases;
    if ((kind != QStringLiteral("linear") && kind != QStringLiteral("cycle")
         && kind != QStringLiteral("swap"))
        || !QDir::isAbsolutePath(directory)
        || (result.direction != QStringLiteral("forward")
            && result.direction != QStringLiteral("undo")
            && result.direction != QStringLiteral("redo"))
        || !phases.contains(phase)
        || items.isEmpty() || items.size() > 10000 || result.completedSteps < 0
        || result.completedSteps > items.size()
        || (phase == QStringLiteral("prepared") && result.completedSteps != 0)
        || (phase == QStringLiteral("verified-complete")
            && result.completedSteps != (kind == QStringLiteral("swap") ? 1 : items.size()))
        || (kind == QStringLiteral("swap") && (items.size() != 2
            || result.completedSteps > 1
            || ((phase == QStringLiteral("prepared")
                 || phase == QStringLiteral("exchange-intent")) && result.completedSteps != 0)
            || (phase == QStringLiteral("exchange-verified") && result.completedSteps != 1)))) {
        result.error = QStringLiteral("V2 journal semantics are invalid.");
        return result;
    }
    QSet<int> rows; QSet<QString> sources; QSet<QString> destinations; QSet<QString> identities;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        const QSet<QString> itemKeys{QStringLiteral("row"), QStringLiteral("source"),
            QStringLiteral("destination"), QStringLiteral("device"), QStringLiteral("inode"),
            QStringLiteral("mode"), QStringLiteral("size"), QStringLiteral("mtimeNs"),
            QStringLiteral("type")};
        BatchRenameRecoveryItem parsed;
        parsed.row = item.value(QStringLiteral("row")).toInt(-1);
        parsed.source = QDir::cleanPath(item.value(QStringLiteral("source")).toString());
        parsed.destination = QDir::cleanPath(item.value(QStringLiteral("destination")).toString());
        if (!BatchRenameRecoveryDetail::exactKeys(item, itemKeys)
            || parsed.row < 0 || rows.contains(parsed.row)
            || QFileInfo(parsed.source).absolutePath() != directory
            || QFileInfo(parsed.destination).absolutePath() != directory
            || parsed.source == parsed.destination || sources.contains(parsed.source)
            || destinations.contains(parsed.destination)
            || !BatchRenameRecoveryDetail::decimalU64(item.value(QStringLiteral("device")), &parsed.device)
            || !BatchRenameRecoveryDetail::decimalU64(item.value(QStringLiteral("inode")), &parsed.inode)
            || !BatchRenameRecoveryDetail::decimalU64(item.value(QStringLiteral("mode")), &parsed.mode)
            || !item.value(QStringLiteral("size")).isString()
            || !item.value(QStringLiteral("mtimeNs")).isString()
            || identities.contains(QString::number(parsed.device) + QLatin1Char(':') + QString::number(parsed.inode))) {
            result.error = QStringLiteral("V2 item semantics are invalid.");
            return result;
        }
        bool sizeOk = false, mtimeOk = false;
        parsed.size = item.value(QStringLiteral("size")).toString().toLongLong(&sizeOk);
        parsed.mtimeNs = item.value(QStringLiteral("mtimeNs")).toString().toLongLong(&mtimeOk);
        parsed.type = item.value(QStringLiteral("type")).toString();
        if (!sizeOk || !mtimeOk || parsed.size < 0
            || (parsed.type != QStringLiteral("file")
                && parsed.type != QStringLiteral("directory")
                && parsed.type != QStringLiteral("symlink"))) {
            result.error = QStringLiteral("V2 item metadata is invalid.");
            return result;
        }
        rows.insert(parsed.row); sources.insert(parsed.source); destinations.insert(parsed.destination);
        identities.insert(QString::number(parsed.device) + QLatin1Char(':') + QString::number(parsed.inode));
        result.items.append(parsed);
    }
    if (kind == QStringLiteral("swap")
        && (result.items.at(0).destination != result.items.at(1).source
            || result.items.at(1).destination != result.items.at(0).source)) {
        result.error = QStringLiteral("V2 swap mapping is not a complete reciprocal pair.");
        return result;
    }
    result.kind = BatchRenameRecoveryKind::V2;
    result.mapping = BatchRenameRecoveryDetail::inspectMapping(result);
    return result;
}

namespace BatchRenameRecoveryDetail {
#ifdef Q_OS_LINUX
inline void recoveryFaultPoint(const char *name)
{
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
    if (qgetenv("THISPC_RECOVERY_RECOVER_KILL_AT") == QByteArray(name)) ::raise(SIGKILL);
    if (qgetenv("THISPC_RECOVERY_HOLD_AT") == QByteArray(name)) {
        const QByteArray ready = qgetenv("THISPC_RECOVERY_HOLD_READY");
        const QByteArray release = qgetenv("THISPC_RECOVERY_HOLD_RELEASE");
        if (!ready.isEmpty()) {
            const int fd = ::open(ready.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) { (void)::write(fd, "ready", 5); ::close(fd); }
        }
        for (int i = 0; !release.isEmpty() && i < 2000 && ::access(release.constData(), F_OK) != 0; ++i)
            ::usleep(5000);
    }
#else
    Q_UNUSED(name)
#endif
}

inline bool recoveryInjectedFailure(const char *name)
{
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
    if (qgetenv("THISPC_RECOVERY_FAIL_AT") != QByteArray(name)) return false;
    errno = EIO;
    return true;
#else
    Q_UNUSED(name)
    return false;
#endif
}

inline int recoveryRenameat2(int oldfd, const char *oldname, int newfd,
                             const char *newname, unsigned int flags)
{
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
    const QByteArray injected = qgetenv("THISPC_RECOVERY_RENAMEAT2_ERRNO");
    if (!injected.isEmpty()) {
        errno = injected == QByteArrayLiteral("ENOSYS") ? ENOSYS : EOPNOTSUPP;
        return -1;
    }
#endif
    return int(::syscall(SYS_renameat2, oldfd, oldname, newfd, newname, flags));
}

inline qint64 statMtimeNs(const struct stat &value)
{
    return qint64(value.st_mtim.tv_sec) * 1000000000LL + value.st_mtim.tv_nsec;
}

inline bool itemAt(const BatchRenameRecoveryItem &item, const QString &path)
{
    struct stat value {};
    if (::lstat(QFile::encodeName(path).constData(), &value) != 0) return false;
    const QString type = S_ISLNK(value.st_mode) ? QStringLiteral("symlink")
        : (S_ISDIR(value.st_mode) ? QStringLiteral("directory")
                                  : (S_ISREG(value.st_mode) ? QStringLiteral("file") : QString()));
    return !type.isEmpty() && type == item.type && quint64(value.st_dev) == item.device
        && quint64(value.st_ino) == item.inode && quint64(value.st_mode) == item.mode
        && qint64(value.st_size) == item.size && statMtimeNs(value) == item.mtimeNs;
}

inline bool absent(const QString &path)
{
    struct stat value {};
    return ::lstat(QFile::encodeName(path).constData(), &value) != 0 && errno == ENOENT;
}

inline bool mappingAt(const BatchRenameRecoveryEntry &entry, int completed)
{
    if (completed < 0 || completed > entry.items.size()) return false;
    QSet<QString> occupied;
    QSet<QString> touched;
    for (int i = 0; i < entry.items.size(); ++i) {
        const auto &item = entry.items.at(i);
        const bool moved = entry.direction == QStringLiteral("undo")
            ? i >= entry.items.size() - completed : i < completed;
        const QString expected = entry.direction == QStringLiteral("undo")
            ? (moved ? item.source : item.destination)
            : (moved ? item.destination : item.source);
        if (occupied.contains(expected) || !itemAt(item, expected)) return false;
        occupied.insert(expected);
        touched.insert(item.source);
        touched.insert(item.destination);
    }
    for (const QString &path : std::as_const(touched))
        if (!occupied.contains(path) && !absent(path)) return false;
    return true;
}

inline int uniqueCompletedPrefix(const BatchRenameRecoveryEntry &entry)
{
    int found = -1;
    for (int completed = 0; completed <= entry.items.size(); ++completed) {
        if (!mappingAt(entry, completed)) continue;
        if (found >= 0) return -1;
        found = completed;
    }
    return found;
}

inline bool fsyncDirectoryPath(const QString &path, const char *fault = "journal-dir-fsync")
{
    const int fd = ::open(QFile::encodeName(path).constData(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    recoveryFaultPoint("before-journal-dir-fsync");
    const bool ok = !recoveryInjectedFailure(fault) && ::fsync(fd) == 0;
    recoveryFaultPoint("after-journal-dir-fsync");
    ::close(fd);
    return ok;
}

inline bool writeV2Checkpoint(const BatchRenameRecoveryEntry &entry,
                              const QString &phase, int completed)
{
    recoveryFaultPoint("before-checkpoint");
    QJsonObject object = entry.object;
    object.insert(QStringLiteral("phase"), phase);
    object.insert(QStringLiteral("completedSteps"), completed);
    object.insert(QStringLiteral("digest"), digest(object));
    QSaveFile file(entry.path);
    file.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return false;
    const int fd = ::open(QFile::encodeName(entry.path).constData(),
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    recoveryFaultPoint("before-journal-file-fsync");
    const bool ok = !recoveryInjectedFailure("journal-file-fsync") && ::fsync(fd) == 0;
    recoveryFaultPoint("after-journal-file-fsync");
    ::close(fd);
    const bool durable = ok && fsyncDirectoryPath(QFileInfo(entry.path).absolutePath());
    if (durable) recoveryFaultPoint("after-checkpoint");
    return durable;
}

inline bool recoverLinearV2(const BatchRenameRecoveryEntry &entry, QString *error)
{
    const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    if (entry.kind != BatchRenameRecoveryKind::V2
        || entry.object.value(QStringLiteral("kind")).toString() != QStringLiteral("linear")
        || (entry.direction != QStringLiteral("forward")
            && entry.direction != QStringLiteral("undo")
            && entry.direction != QStringLiteral("redo")))
        return fail(QStringLiteral("Only linear forward/undo/redo v2 is recoverable."));
    const QString directory = entry.object.value(QStringLiteral("directory")).toString();
    const int dirfd = ::open(QFile::encodeName(directory).constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return fail(QStringLiteral("Cannot open the recorded data directory."));
    struct stat directoryStat {};
    if (::fstat(dirfd, &directoryStat) != 0
        || quint64(directoryStat.st_dev) != entry.directoryDevice
        || quint64(directoryStat.st_ino) != entry.directoryInode) {
        ::close(dirfd);
        return fail(QStringLiteral("Data directory identity changed."));
    }
    int done = uniqueCompletedPrefix(entry);
    if (done < 0) { ::close(dirfd); return fail(QStringLiteral("Filesystem mapping is ambiguous.")); }
    while (done < entry.items.size()) {
        recoveryFaultPoint("before-intent");
        if (!writeV2Checkpoint(entry, QStringLiteral("step-intent"), done)) {
            ::close(dirfd); return fail(QStringLiteral("Cannot persist recovery intent."));
        }
        recoveryFaultPoint("after-intent");
        const int position = entry.direction == QStringLiteral("undo")
            ? entry.items.size() - 1 - done : done;
        const auto &item = entry.items.at(position);
        const QString fromPath = entry.direction == QStringLiteral("undo")
            ? item.destination : item.source;
        const QString toPath = entry.direction == QStringLiteral("undo")
            ? item.source : item.destination;
        const QByteArray from = QFile::encodeName(QFileInfo(fromPath).fileName());
        const QByteArray to = QFile::encodeName(QFileInfo(toPath).fileName());
        recoveryFaultPoint("before-syscall");
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
        if (qgetenv("THISPC_RECOVERY_RACE_OCCUPY") == QByteArrayLiteral("1")) {
            const int outsider = ::openat(dirfd, to.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (outsider >= 0) { (void)::write(outsider, "FOREIGN", 7); ::close(outsider); }
        }
#endif
        if (recoveryRenameat2(dirfd, from.constData(), dirfd, to.constData(),
                             RENAME_NOREPLACE) != 0) {
            ::close(dirfd); return fail(QStringLiteral("Recovery no-overwrite rename failed: ")
                                        + QString::fromLocal8Bit(std::strerror(errno)));
        }
        ++done;
        recoveryFaultPoint("after-syscall");
        recoveryFaultPoint("before-data-dir-fsync");
        const bool dataDurable = !recoveryInjectedFailure("data-dir-fsync") && ::fsync(dirfd) == 0;
        recoveryFaultPoint("after-data-dir-fsync");
        if (!dataDurable || !mappingAt(entry, done)
            || !writeV2Checkpoint(entry, QStringLiteral("step-verified"), done)) {
            ::close(dirfd); return fail(QStringLiteral("Recovery postcheck or durability checkpoint failed."));
        }
        recoveryFaultPoint("after-verified");
    }
    ::close(dirfd);
    if (!mappingAt(entry, entry.items.size())
        || !writeV2Checkpoint(entry, QStringLiteral("verified-complete"), entry.items.size()))
        return fail(QStringLiteral("Final durable mapping could not be verified."));
    recoveryFaultPoint("after-goal-before-unlink");
    if (!QFile::remove(entry.path)
        || !fsyncDirectoryPath(QFileInfo(entry.path).absolutePath(), "cleanup-dir-fsync"))
        return fail(QStringLiteral("Goal is durable but journal cleanup is incomplete."));
    return true;
}

inline bool recoverSwapV2(const BatchRenameRecoveryEntry &entry, QString *error)
{
    const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    if (entry.kind != BatchRenameRecoveryKind::V2 || entry.items.size() != 2
        || entry.object.value(QStringLiteral("kind")).toString() != QStringLiteral("swap"))
        return fail(QStringLiteral("Only a strict two-name swap v2 is recoverable."));
    const QString directory = entry.object.value(QStringLiteral("directory")).toString();
    const int dirfd = ::open(QFile::encodeName(directory).constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return fail(QStringLiteral("Cannot open the recorded data directory."));
    const auto closeFd = qScopeGuard([&] { ::close(dirfd); });
    struct stat ds {};
    if (::fstat(dirfd, &ds) != 0 || quint64(ds.st_dev) != entry.directoryDevice
        || quint64(ds.st_ino) != entry.directoryInode)
        return fail(QStringLiteral("Data directory identity changed."));
    BatchRenameRecoveryMapping state = inspectMapping(entry);
    if (state != BatchRenameRecoveryMapping::Initial
        && state != BatchRenameRecoveryMapping::Complete)
        return fail(QStringLiteral("Swap mapping is foreign or ambiguous."));
    recoveryFaultPoint("before-intent");
    if (!writeV2Checkpoint(entry, QStringLiteral("exchange-intent"), 0))
        return fail(QStringLiteral("Cannot persist swap intent."));
    recoveryFaultPoint("after-intent");
    if (state == BatchRenameRecoveryMapping::Initial) {
        const QByteArray left = QFile::encodeName(QFileInfo(entry.items.at(0).source).fileName());
        const QByteArray right = QFile::encodeName(QFileInfo(entry.items.at(1).source).fileName());
        recoveryFaultPoint("before-syscall");
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
        if (qgetenv("THISPC_RECOVERY_RACE_SWAP") == QByteArrayLiteral("1")) {
            const QByteArray parked = left + QByteArrayLiteral(".foreign");
            (void)::renameat(dirfd, left.constData(), dirfd, parked.constData());
            const int fd = ::openat(dirfd, left.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) { (void)::write(fd, "FOREIGN", 7); ::close(fd); }
        }
#endif
        if (recoveryRenameat2(dirfd, left.constData(), dirfd, right.constData(), RENAME_EXCHANGE) != 0)
            return fail(QStringLiteral("Recovery exchange failed: ")
                        + QString::fromLocal8Bit(std::strerror(errno)));
        recoveryFaultPoint("after-syscall");
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
        if (qgetenv("THISPC_RECOVERY_RACE_AFTER_SWAP") == QByteArrayLiteral("1")) {
            const QByteArray parked = left + QByteArrayLiteral(".after-race");
            (void)::renameat(dirfd, left.constData(), dirfd, parked.constData());
            const int fd = ::openat(dirfd, left.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) { (void)::write(fd, "AFTER-FOREIGN", 13); ::close(fd); }
        }
#endif
    }
    recoveryFaultPoint("before-data-dir-fsync");
    if (recoveryInjectedFailure("data-dir-fsync") || ::fsync(dirfd) != 0)
        return fail(QStringLiteral("Cannot synchronize swap data directory."));
    recoveryFaultPoint("after-data-dir-fsync");
    if (inspectMapping(entry) != BatchRenameRecoveryMapping::Complete)
        return fail(QStringLiteral("Swap postcheck failed."));
    recoveryFaultPoint("after-postcheck");
    if (!writeV2Checkpoint(entry, QStringLiteral("exchange-verified"), 1))
        return fail(QStringLiteral("Cannot persist verified swap."));
    recoveryFaultPoint("after-verified");
    if (!writeV2Checkpoint(entry, QStringLiteral("verified-complete"), 1))
        return fail(QStringLiteral("Cannot persist swap goal."));
    recoveryFaultPoint("after-goal-before-unlink");
    if (recoveryInjectedFailure("cleanup-dir-fsync"))
        return fail(QStringLiteral("Cannot safely begin swap journal cleanup."));
    if (!QFile::remove(entry.path)) return fail(QStringLiteral("Cannot remove completed swap journal."));
    recoveryFaultPoint("after-unlink-before-journal-dir-fsync");
    if (!fsyncDirectoryPath(QFileInfo(entry.path).absolutePath()))
        return fail(QStringLiteral("Swap cleanup is not durable."));
    return true;
}

inline bool automaticDirectionEnabled(const QString &direction)
{
    return direction == QStringLiteral("forward")
        || direction == QStringLiteral("undo")
        || direction == QStringLiteral("redo");
}
#endif
}

class BatchRenameRecoveryGate final
{
public:
    explicit BatchRenameRecoveryGate(QString root) : m_root(std::move(root)) { refresh(); }
    ~BatchRenameRecoveryGate() {
#ifdef Q_OS_UNIX
        if (m_lockFd >= 0) ::close(m_lockFd);
#endif
    }
    static BatchRenameRecoveryGate &instance(const QString &root = {}) {
        QString selected = root;
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
        if (selected.isEmpty()) selected = QString::fromLocal8Bit(qgetenv("THISPC_RECOVERY_ROOT"));
#endif
        static BatchRenameRecoveryGate gate(selected.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                + QStringLiteral("/thispc-view/batch-rename-recovery") : selected, true);
        s_processGate = &gate;
        return gate;
    }
    BatchRenameRecoveryAudit audit() const {
        if (!m_mutex.tryLock()) return checkingAudit();
        const BatchRenameRecoveryAudit result = m_audit;
        m_mutex.unlock();
        return result;
    }
    // Stage 4A.2: these swaps have no production journal yet. Fence ALL
    // cooperating mutations for the whole batch containing an exchange,
    // including its asynchronous gaps. This is not crash recovery.
    bool mutationsBlocked() const {
        if (m_unjournaledSwapRunning.load() || m_unjournaledSwapUncertain.load()) return true;
        if (!m_mutex.tryLock()) return true;
        const bool blocked = m_audit.blocked;
        m_mutex.unlock();
        return blocked;
    }
    bool unjournaledSwapRunning() const { return m_unjournaledSwapRunning.load(); }
    bool beginUnjournaledSwap() {
        if (!m_mutex.tryLock()) return false;
        // Audit and acquire the process fence under ONE mutex critical section.
        // A failed/contended refresh must never use an older clean snapshot.
        if (m_startupScanPending) { m_mutex.unlock(); return false; }
        refreshUnlocked(true);
        const bool allowed = m_audit.inspectorLockHeld && !m_audit.blocked
            && !m_unjournaledSwapUncertain.load() && !m_unjournaledSwapRunning.load();
        if (allowed) m_unjournaledSwapRunning.store(true);
        m_mutex.unlock();
        return allowed;
    }
    // Used ONLY by the FileActions runner that already owns the exchange
    // fence. Re-audit external recovery evidence without rejecting our own
    // in-process fence during the gap between swaps/KIO/cycle steps.
    bool unjournaledSwapContinuationAllowed() {
        if (!m_unjournaledSwapRunning.load() || m_unjournaledSwapUncertain.load()) return false;
        if (!m_mutex.tryLock()) return false;
        if (m_startupScanPending) { m_mutex.unlock(); return false; }
        refreshUnlocked(true);
        const bool allowed = m_audit.inspectorLockHeld && !m_audit.blocked
            && m_unjournaledSwapRunning.load() && !m_unjournaledSwapUncertain.load();
        m_mutex.unlock();
        return allowed;
    }
    void finishUnjournaledSwap(bool uncertain = false) {
        if (uncertain) m_unjournaledSwapUncertain.store(true);
        m_unjournaledSwapRunning.store(false);
    }
    bool startupScanPending() const {
        if (!m_mutex.tryLock()) return true;
        const bool pending = m_startupScanPending;
        m_mutex.unlock();
        return pending;
    }
    bool ownsProtocolLock() const {
        if (!m_mutex.tryLock()) return false;
        const bool owns = m_lockFd >= 0 && m_audit.inspectorLockHeld;
        m_mutex.unlock();
        return owns;
    }
    QString root() const { return m_root; }
    static bool processGateOwns(const QString &root) {
        BatchRenameRecoveryGate *gate = s_processGate;
        if (!gate || QDir::cleanPath(gate->m_root) != QDir::cleanPath(root)) return false;
        if (!gate->m_mutex.tryLock()) return false;
        const bool owns = gate->m_lockFd >= 0;
        gate->m_mutex.unlock();
        return owns;
    }
    QString message() const {
        if (m_unjournaledSwapUncertain.load())
            return QStringLiteral("Swap result uncertain. Inspect both names manually; file changes are disabled in this process.");
        if (m_unjournaledSwapRunning.load())
            return QStringLiteral("Batch Rename exchange or continuation in progress; file changes are temporarily disabled. No swap crash recovery is available yet.");
        if (!m_mutex.tryLock()) {
            const QString direction = directionFromCode(m_activeDirection.load());
            return m_recoveryRunning.load() ? recoveringMessage(direction)
                                           : checkingMessage(direction);
        }
        const QString result = m_audit.summary;
        m_mutex.unlock();
        return result;
    }
    bool beginStartupScan() {
        if (!m_mutex.tryLock()) return false;
        if (m_startupWorkerStarted) {
            m_mutex.unlock();
            return false;
        }
        m_startupWorkerStarted = true;
        m_startupScanPending = true;
        m_audit = {};
        m_audit.root = m_root;
        m_audit.blocked = true;
        m_audit.summary = checkingMessage();
        m_mutex.unlock();
        return true;
    }
    void completeStartupScan() {
        QMutexLocker lock(&m_mutex);
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
        bool delayOk = false;
        const int delay = QString::fromLocal8Bit(qgetenv("THISPC_RECOVERY_STARTUP_DELAY_MS"))
                              .toInt(&delayOk);
        if (delayOk && delay > 0) QThread::msleep(unsigned(delay));
#endif
        refreshUnlocked(true);
        m_startupScanPending = false;
        m_startupWorkerStarted = false;
    }
    void refresh() {
        if (!m_mutex.tryLock()) return;
        if (m_startupScanPending) {
            m_mutex.unlock();
            return;
        }
        refreshUnlocked(true);
        m_mutex.unlock();
    }
private:
    static QString checkingMessage(const QString &direction = {}) {
        if (!direction.isEmpty())
            return QStringLiteral("Checking interrupted Batch Rename %1 recovery; file changes are temporarily disabled.")
                .arg(directionLabel(direction));
        return QStringLiteral("Checking crash recovery; file changes are temporarily disabled.");
    }
    static int directionCode(const QString &direction) {
        if (direction == QStringLiteral("undo")) return 2;
        if (direction == QStringLiteral("redo")) return 3;
        return 1;
    }
    static QString directionFromCode(int direction) {
        if (direction == 2) return QStringLiteral("undo");
        if (direction == 3) return QStringLiteral("redo");
        if (direction == 1) return QStringLiteral("forward");
        return {};
    }
    static QString directionLabel(const QString &direction) {
        if (direction == QStringLiteral("undo")) return QStringLiteral("Undo");
        if (direction == QStringLiteral("redo")) return QStringLiteral("Redo");
        return QStringLiteral("Execute");
    }
    static QString recoveringMessage(const QString &direction) {
        return QStringLiteral("Finishing interrupted Batch Rename %1 recovery; file changes remain disabled.")
            .arg(directionLabel(direction));
    }
    BatchRenameRecoveryAudit checkingAudit() const {
        BatchRenameRecoveryAudit result;
        result.root = m_root;
        result.blocked = true;
        result.summary = checkingMessage();
        return result;
    }
    BatchRenameRecoveryGate(QString root, bool deferStartupScan)
        : m_root(std::move(root)), m_startupScanPending(deferStartupScan)
    {
        if (deferStartupScan) {
            m_audit.root = m_root;
            m_audit.blocked = true;
            m_audit.summary = checkingMessage();
        } else {
            refresh();
        }
    }
    void refreshUnlocked(bool allowTestRecovery) {
#if !defined(Q_OS_LINUX)
        Q_UNUSED(allowTestRecovery)
#endif
        m_audit = {}; m_audit.root = m_root;
#ifdef Q_OS_UNIX
        if (m_lockFd < 0) {
            const int rootFd = BatchRenameRecoveryDetail::openPrivateRecoveryRoot(m_root);
            if (rootFd >= 0) {
                m_lockFd = ::openat(rootFd, "recovery-audit.lock",
                                    O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
                ::close(rootFd);
            }
            struct stat lockStatus {};
            if (m_lockFd >= 0
                && (::fstat(m_lockFd, &lockStatus) != 0 || !S_ISREG(lockStatus.st_mode)
                    || lockStatus.st_uid != ::geteuid() || (lockStatus.st_mode & 0077) != 0)) {
                ::close(m_lockFd);
                m_lockFd = -1;
            }
            if (m_lockFd >= 0 && ::flock(m_lockFd, LOCK_EX | LOCK_NB) == 0) {
                m_audit.inspectorLockHeld = true;
            } else {
                if (m_lockFd >= 0) { ::close(m_lockFd); m_lockFd = -1; }
                m_audit.writerLock = BatchRenameRecoveryLock::Live;
            }
        } else m_audit.inspectorLockHeld = true;
#else
        m_audit.writerLock = BatchRenameRecoveryLock::Ambiguous;
#endif
        const QDir dir(m_root);
        const QFileInfo rootInfo(m_root);
        if (!m_audit.inspectorLockHeld || !dir.exists() || rootInfo.isSymLink() || !rootInfo.isDir()) {
            m_audit.blocked = true;
            if (m_audit.writerLock == BatchRenameRecoveryLock::None)
                m_audit.writerLock = BatchRenameRecoveryLock::Ambiguous;
            m_audit.summary = QStringLiteral("Recovery directory or audit lock is unsafe; file changes are disabled.");
            return;
        }
        const QFileInfo lockInfo(dir.filePath(QStringLiteral("cycle.lock")));
        if (lockInfo.exists()) {
            QFile lock(lockInfo.absoluteFilePath());
            if (lockInfo.isSymLink() || !lock.open(QIODevice::ReadOnly)) m_audit.writerLock = BatchRenameRecoveryLock::Ambiguous;
            else {
                bool ok = false; const qint64 pid = QString::fromLatin1(lock.readLine()).trimmed().toLongLong(&ok);
#ifdef Q_OS_UNIX
                if (!ok || pid <= 1) m_audit.writerLock = BatchRenameRecoveryLock::Ambiguous;
                else if (::kill(pid_t(pid), 0) == 0 || errno == EPERM) m_audit.writerLock = BatchRenameRecoveryLock::Live;
                else if (errno == ESRCH) m_audit.writerLock = BatchRenameRecoveryLock::Stale;
                else m_audit.writerLock = BatchRenameRecoveryLock::Ambiguous;
#else
                m_audit.writerLock = BatchRenameRecoveryLock::Ambiguous;
#endif
            }
        }
        const QFileInfoList files = dir.entryInfoList(
            QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &file : files) {
            const QString name = file.fileName();
            if (name == QStringLiteral("cycle.lock") || name == QStringLiteral("recovery-audit.lock")) continue;
            if (name.startsWith(QStringLiteral("cycle-")) || name.startsWith(QStringLiteral("linear-"))
                || name.startsWith(QStringLiteral("swap-")))
                m_audit.entries.append(parseBatchRenameRecoveryJournal(file.absoluteFilePath()));
            else if (name.contains(QStringLiteral("journal"), Qt::CaseInsensitive)
                     || name.endsWith(QStringLiteral(".tmp")) || name.startsWith(QLatin1Char('.'))) {
                BatchRenameRecoveryEntry entry; entry.path = file.absoluteFilePath();
                entry.kind = BatchRenameRecoveryKind::SuspiciousTemporary;
                entry.error = QStringLiteral("Suspicious recovery artifact.");
                m_audit.entries.append(entry);
            }
        }
#ifdef Q_OS_LINUX
        if (m_audit.entries.isEmpty()
            && m_audit.writerLock == BatchRenameRecoveryLock::Stale) {
            QLockFile staleCleanup(dir.filePath(QStringLiteral("cycle.lock")));
            if (staleCleanup.tryLock(0)) m_audit.writerLock = BatchRenameRecoveryLock::None;
        }
        // Automatic policy-B recovery accepts exactly one strict linear v2
        // journal with at least two entries and never changes its direction.
        // v1, cycles, corrupt/unknown files and multi-journal states
        // remain manual-only. The protocol lock is already held; cycle.lock is
        // acquired second and retained for the entire recovery.
        if (allowTestRecovery && !m_unjournaledSwapRunning.load()
            && m_audit.entries.size() == 1
            && m_audit.entries.first().kind == BatchRenameRecoveryKind::V2
            && m_audit.entries.first().items.size() >= 2
            && BatchRenameRecoveryDetail::automaticDirectionEnabled(
                m_audit.entries.first().direction)
            && m_audit.entries.first().object.value(QStringLiteral("kind")).toString()
                == QStringLiteral("linear")
            && m_audit.writerLock != BatchRenameRecoveryLock::Live
            && m_audit.writerLock != BatchRenameRecoveryLock::Ambiguous) {
            QLockFile recoveryLock(dir.filePath(QStringLiteral("cycle.lock")));
            if (recoveryLock.tryLock(0)) {
                QString recoveryError;
                const QString recoveryDirection = m_audit.entries.first().direction;
                m_activeDirection.store(directionCode(recoveryDirection));
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
                bool directionDelayOk = false;
                const int directionDelay = QString::fromLocal8Bit(
                    qgetenv("THISPC_RECOVERY_DIRECTION_DELAY_MS")).toInt(&directionDelayOk);
                if (directionDelayOk && directionDelay > 0) QThread::msleep(unsigned(directionDelay));
#endif
                m_recoveryRunning.store(true);
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
                bool activeDelayOk = false;
                const int activeDelay = QString::fromLocal8Bit(
                    qgetenv("THISPC_RECOVERY_ACTIVE_DELAY_MS")).toInt(&activeDelayOk);
                if (activeDelayOk && activeDelay > 0) QThread::msleep(unsigned(activeDelay));
#endif
                if (BatchRenameRecoveryDetail::recoverLinearV2(m_audit.entries.first(),
                                                                &recoveryError)) {
                    m_lastCompletedDirection = recoveryDirection;
                    m_audit.entries.clear();
                    m_audit.writerLock = BatchRenameRecoveryLock::None;
                } else {
                    m_audit.entries.first().error = recoveryError;
                }
                m_recoveryRunning.store(false);
                m_activeDirection.store(0);
            }
        }
#endif
        m_audit.blocked = !m_audit.entries.isEmpty() || m_audit.writerLock != BatchRenameRecoveryLock::None;
        if (!m_audit.inspectorLockHeld) m_audit.blocked = true;
        if (m_audit.blocked) {
            const QString direction = m_audit.entries.size() == 1
                ? m_audit.entries.first().direction : QString();
            m_audit.summary = direction.isEmpty()
                ? QStringLiteral("Crash recovery requires intervention or has a conflict; file changes are disabled (%1 item(s)).")
                      .arg(m_audit.entries.size())
                : QStringLiteral("Interrupted Batch Rename %1 recovery has a conflict; file changes are disabled and the journal is preserved.")
                      .arg(directionLabel(direction));
        } else {
            m_audit.summary = m_lastCompletedDirection.isEmpty()
                ? QStringLiteral("Crash recovery check completed. Undo/Redo history from before restart is not restored.")
                : QStringLiteral("Interrupted Batch Rename %1 recovery completed. Undo/Redo history from before restart is not restored.")
                      .arg(directionLabel(m_lastCompletedDirection));
        }
    }
    QString m_root;
    BatchRenameRecoveryAudit m_audit;
    QString m_lastCompletedDirection;
    int m_lockFd = -1;
    bool m_startupScanPending = false;
    bool m_startupWorkerStarted = false;
    mutable QMutex m_mutex;
    std::atomic_bool m_unjournaledSwapRunning{false};
    std::atomic_bool m_unjournaledSwapUncertain{false};
    std::atomic_bool m_recoveryRunning{false};
    std::atomic_int m_activeDirection{0};
    inline static BatchRenameRecoveryGate *s_processGate = nullptr;
};
