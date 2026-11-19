/*
 * Transactional journal, 0700 transaction containment, and fail-closed crash
 * recovery inspector for FOLDER-MERGE-01 Stage 3B.1.
 *
 * Solves:
 * - CRASH-RECOVERY-01: Durable intent/phase journal (.thispc-merge-tx.*) inside
 *   an owner-only 0700 directory (.thispc-tx.*) so an interrupted Overwrite after
 *   RENAME_EXCHANGE is unambiguously identified by actual (dev, ino, size, mtim)
 *   and OLD is never mistaken for disposable .thispc-part.* staging.
 * - SAFE-CLEANUP-01: Containment inside 0700 txDirFd + openat(O_NOFOLLOW) + fstat
 *   identity check before unlinkat(), failing closed on any foreign object or
 *   corrupted journal.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

enum class LocalMergeTxPhase {
    Prepared,
    StagingCreated,
    StagingDurable,
    ExchangeIntentDurable,
    ExchangePerformed,
    PreviousFileSecured,
    DirectoryDurable,
    Completed,
    RecoveryRequired,
    UncertainDurability,
};

inline QString localMergeTxPhaseName(LocalMergeTxPhase phase)
{
    switch (phase) {
    case LocalMergeTxPhase::Prepared: return QStringLiteral("Prepared");
    case LocalMergeTxPhase::StagingCreated: return QStringLiteral("StagingCreated");
    case LocalMergeTxPhase::StagingDurable: return QStringLiteral("StagingDurable");
    case LocalMergeTxPhase::ExchangeIntentDurable: return QStringLiteral("ExchangeIntentDurable");
    case LocalMergeTxPhase::ExchangePerformed: return QStringLiteral("ExchangePerformed");
    case LocalMergeTxPhase::PreviousFileSecured: return QStringLiteral("PreviousFileSecured");
    case LocalMergeTxPhase::DirectoryDurable: return QStringLiteral("DirectoryDurable");
    case LocalMergeTxPhase::Completed: return QStringLiteral("Completed");
    case LocalMergeTxPhase::RecoveryRequired: return QStringLiteral("RecoveryRequired");
    case LocalMergeTxPhase::UncertainDurability: return QStringLiteral("UncertainDurability");
    }
    return QStringLiteral("Unknown");
}

inline bool localMergeTxPhaseFromName(const QString &name, LocalMergeTxPhase *outPhase)
{
    if (!outPhase) return false;
    if (name == QLatin1String("Prepared")) { *outPhase = LocalMergeTxPhase::Prepared; return true; }
    if (name == QLatin1String("StagingCreated")) { *outPhase = LocalMergeTxPhase::StagingCreated; return true; }
    if (name == QLatin1String("StagingDurable")) { *outPhase = LocalMergeTxPhase::StagingDurable; return true; }
    if (name == QLatin1String("ExchangeIntentDurable")) { *outPhase = LocalMergeTxPhase::ExchangeIntentDurable; return true; }
    if (name == QLatin1String("ExchangePerformed")) { *outPhase = LocalMergeTxPhase::ExchangePerformed; return true; }
    if (name == QLatin1String("PreviousFileSecured")) { *outPhase = LocalMergeTxPhase::PreviousFileSecured; return true; }
    if (name == QLatin1String("DirectoryDurable")) { *outPhase = LocalMergeTxPhase::DirectoryDurable; return true; }
    if (name == QLatin1String("Completed")) { *outPhase = LocalMergeTxPhase::Completed; return true; }
    if (name == QLatin1String("RecoveryRequired")) { *outPhase = LocalMergeTxPhase::RecoveryRequired; return true; }
    if (name == QLatin1String("UncertainDurability")) { *outPhase = LocalMergeTxPhase::UncertainDurability; return true; }
    return false;
}

struct LocalMergeTxStatSnapshot
{
    qulonglong dev = 0;
    qulonglong ino = 0;
    qulonglong mode = 0;
    qulonglong size = 0;
    qlonglong mtimSec = 0;
    qlonglong mtimNsec = 0;
    bool valid = false;

    static LocalMergeTxStatSnapshot fromStat(const struct stat &st, bool isValid = true)
    {
        LocalMergeTxStatSnapshot s;
        s.dev = static_cast<qulonglong>(st.st_dev);
        s.ino = static_cast<qulonglong>(st.st_ino);
        s.mode = static_cast<qulonglong>(st.st_mode);
        s.size = static_cast<qulonglong>(st.st_size);
        s.mtimSec = static_cast<qlonglong>(st.st_mtim.tv_sec);
        s.mtimNsec = static_cast<qlonglong>(st.st_mtim.tv_nsec);
        s.valid = isValid;
        return s;
    }

    bool matchesStat(const struct stat &st) const
    {
        return valid
            && dev == static_cast<qulonglong>(st.st_dev)
            && ino == static_cast<qulonglong>(st.st_ino)
            && mode == static_cast<qulonglong>(st.st_mode)
            && size == static_cast<qulonglong>(st.st_size)
            && mtimSec == static_cast<qlonglong>(st.st_mtim.tv_sec)
            && mtimNsec == static_cast<qlonglong>(st.st_mtim.tv_nsec);
    }

    bool matchesDirectoryIdentity(const struct stat &st) const
    {
        return valid
            && S_ISDIR(st.st_mode)
            && dev == static_cast<qulonglong>(st.st_dev)
            && ino == static_cast<qulonglong>(st.st_ino);
    }

    QString encode() const
    {
        return QStringLiteral("%1,%2,%3,%4,%5,%6,%7")
            .arg(valid ? 1 : 0)
            .arg(dev)
            .arg(ino)
            .arg(mode)
            .arg(size)
            .arg(mtimSec)
            .arg(mtimNsec);
    }

    static bool decode(const QString &str, LocalMergeTxStatSnapshot *out)
    {
        if (!out) return false;
        const QStringList p = str.split(QLatin1Char(','));
        if (p.size() != 7) return false;
        bool ok[7] = {};
        const int v = p[0].toInt(&ok[0]);
        out->dev = p[1].toULongLong(&ok[1]);
        out->ino = p[2].toULongLong(&ok[2]);
        out->mode = p[3].toULongLong(&ok[3]);
        out->size = p[4].toULongLong(&ok[4]);
        out->mtimSec = p[5].toLongLong(&ok[5]);
        out->mtimNsec = p[6].toLongLong(&ok[6]);
        for (bool b : ok) {
            if (!b) return false;
        }
        out->valid = (v == 1);
        return true;
    }
};

struct LocalMergeTxJournalRecord
{
    QString txId;
    LocalMergeTxPhase phase = LocalMergeTxPhase::Prepared;
    QString approvedAction = QStringLiteral("Overwrite");
    QString sourcePath;
    QString destinationPath;
    QString destFileName;
    QString stageFileName = QStringLiteral("stage");
    QString replacedBackupName;
    QString newSha256Hex;
    LocalMergeTxStatSnapshot parentStat;
    LocalMergeTxStatSnapshot sourceStat;
    LocalMergeTxStatSnapshot expectedDestStat;  // OLD
    LocalMergeTxStatSnapshot stageStat;         // NEW
    bool durabilityConfirmed = false;

    QByteArray serializeSigned() const
    {
        QString body;
        body += QStringLiteral("TXID=%1\n").arg(txId);
        body += QStringLiteral("PHASE=%1\n").arg(localMergeTxPhaseName(phase));
        body += QStringLiteral("ACTION=%1\n").arg(approvedAction);
        body += QStringLiteral("SRC=%1\n").arg(QString::fromLatin1(sourcePath.toUtf8().toBase64()));
        body += QStringLiteral("DST=%1\n").arg(QString::fromLatin1(destinationPath.toUtf8().toBase64()));
        body += QStringLiteral("DSTNAME=%1\n").arg(QString::fromLatin1(destFileName.toUtf8().toBase64()));
        body += QStringLiteral("STAGENAME=%1\n").arg(QString::fromLatin1(stageFileName.toUtf8().toBase64()));
        body += QStringLiteral("BACKUPNAME=%1\n").arg(QString::fromLatin1(replacedBackupName.toUtf8().toBase64()));
        body += QStringLiteral("NEWSHA256=%1\n").arg(newSha256Hex);
        body += QStringLiteral("PARENTSTAT=%1\n").arg(parentStat.encode());
        body += QStringLiteral("SRCSTAT=%1\n").arg(sourceStat.encode());
        body += QStringLiteral("OLDSTAT=%1\n").arg(expectedDestStat.encode());
        body += QStringLiteral("NEWSTAT=%1\n").arg(stageStat.encode());
        body += QStringLiteral("DURABLE=%1\n").arg(durabilityConfirmed ? 1 : 0);

        const QByteArray bodyBytes = body.toUtf8();
        const QByteArray digest = QCryptographicHash::hash(bodyBytes, QCryptographicHash::Sha256).toHex();
        return QByteArray("THISPC_MERGE_TX_V1\n") + bodyBytes + QByteArray("CHECKSUM=") + digest + QByteArray("\nEND\n");
    }

    static bool deserializeSigned(const QByteArray &raw, LocalMergeTxJournalRecord *out)
    {
        if (!out) return false;
        const QList<QByteArray> lines = raw.split('\n');
        if (lines.size() < 17 || lines.first() != "THISPC_MERGE_TX_V1") return false;

        QByteArray bodyBytes;
        QByteArray expectedDigest;
        bool sawEnd = false;
        QHash<QString, QString> kv;

        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray &line = lines.at(i);
            if (line.isEmpty()) continue;
            if (line == "END") {
                sawEnd = true;
                break;
            }
            if (line.startsWith("CHECKSUM=")) {
                expectedDigest = line.mid(9);
                continue;
            }
            bodyBytes += line + '\n';
            const qsizetype eq = line.indexOf('=');
            if (eq <= 0) return false;
            kv.insert(QString::fromUtf8(line.left(eq)), QString::fromUtf8(line.mid(eq + 1)));
        }

        if (!sawEnd || expectedDigest.isEmpty()) return false;
        const QByteArray actualDigest = QCryptographicHash::hash(bodyBytes, QCryptographicHash::Sha256).toHex();
        if (actualDigest != expectedDigest) return false;

        auto b64 = [&](const QString &key, QString *dst) {
            if (!kv.contains(key)) return false;
            const QByteArray dec = QByteArray::fromBase64(kv.value(key).toLatin1());
            *dst = QString::fromUtf8(dec);
            return true;
        };

        LocalMergeTxJournalRecord rec;
        rec.txId = kv.value(QStringLiteral("TXID"));
        if (rec.txId.isEmpty() || !localMergeTxPhaseFromName(kv.value(QStringLiteral("PHASE")), &rec.phase)) return false;
        rec.approvedAction = kv.value(QStringLiteral("ACTION"));
        if (!b64(QStringLiteral("SRC"), &rec.sourcePath)
            || !b64(QStringLiteral("DST"), &rec.destinationPath)
            || !b64(QStringLiteral("DSTNAME"), &rec.destFileName)
            || !b64(QStringLiteral("STAGENAME"), &rec.stageFileName)
            || !b64(QStringLiteral("BACKUPNAME"), &rec.replacedBackupName)) {
            return false;
        }
        rec.newSha256Hex = kv.value(QStringLiteral("NEWSHA256"));
        if (!LocalMergeTxStatSnapshot::decode(kv.value(QStringLiteral("PARENTSTAT")), &rec.parentStat)
            || !LocalMergeTxStatSnapshot::decode(kv.value(QStringLiteral("SRCSTAT")), &rec.sourceStat)
            || !LocalMergeTxStatSnapshot::decode(kv.value(QStringLiteral("OLDSTAT")), &rec.expectedDestStat)
            || !LocalMergeTxStatSnapshot::decode(kv.value(QStringLiteral("NEWSTAT")), &rec.stageStat)) {
            return false;
        }
        rec.durabilityConfirmed = (kv.value(QStringLiteral("DURABLE")) == QLatin1String("1"));
        *out = std::move(rec);
        return true;
    }
};

enum class LocalMergeRecoveryStatus {
    CleanNoActiveTx,
    StagedOnlyAborted,        // Exchange never occurred; OLD intact at destination
    ExchangeOccurredOldSaved, // Exchange occurred; OLD located & secured as .thispc-replaced.*
    DurableCommitPendingCleanup,
    ManualInterventionRequired,
};

struct LocalMergeRecoveryFinding
{
    QString txDirName;
    QString txId;
    LocalMergeTxPhase recordedPhase = LocalMergeTxPhase::Prepared;
    LocalMergeRecoveryStatus status = LocalMergeRecoveryStatus::ManualInterventionRequired;
    QString destinationPath;
    QString recoveryOldPath;
    bool oldAvailable = false;
    bool newAvailable = false;
    bool manualInterventionRequired = true;
    QString diagnostic;
};

struct LocalMergeRecoveryReport
{
    QList<LocalMergeRecoveryFinding> findings;
    bool anyIncompleteFound = false;
    bool anyManualInterventionRequired = false;
    QStringList preservedUntouchedFiles;
};

namespace LocalMergeTxDetail {

inline QString generateTxId()
{
    static std::atomic<qulonglong> counter{1};
    return QStringLiteral("%1.%2").arg(static_cast<qulonglong>(::getpid())).arg(counter.fetch_add(1, std::memory_order_relaxed));
}

inline bool writeJournalAtomically(int txDirFd, const QString &journalName, const LocalMergeTxJournalRecord &rec)
{
    if (txDirFd < 0) return false;
    const QString tmpName = journalName + QStringLiteral(".tmp");
    const QByteArray tmpEnc = QFile::encodeName(tmpName);
    const QByteArray finalEnc = QFile::encodeName(journalName);
    const QByteArray data = rec.serializeSigned();

    ::unlinkat(txDirFd, tmpEnc.constData(), 0);
    const int fd = ::openat(txDirFd, tmpEnc.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;

    qsizetype off = 0;
    while (off < data.size()) {
        const ssize_t w = ::write(fd, data.constData() + off, static_cast<size_t>(data.size() - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlinkat(txDirFd, tmpEnc.constData(), 0);
            return false;
        }
        off += w;
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlinkat(txDirFd, tmpEnc.constData(), 0);
        return false;
    }
    ::close(fd);

    if (::renameat(txDirFd, tmpEnc.constData(), txDirFd, finalEnc.constData()) != 0) {
        ::unlinkat(txDirFd, tmpEnc.constData(), 0);
        return false;
    }
    return ::fsync(txDirFd) == 0;
}

inline bool readJournalFile(int txDirFd, const QString &journalName, LocalMergeTxJournalRecord *out)
{
    const int fd = ::openat(txDirFd, QFile::encodeName(journalName).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    QByteArray raw;
    char buf[4096];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        raw.append(buf, static_cast<qsizetype>(n));
        if (raw.size() > 256 * 1024) {
            ::close(fd);
            return false;
        }
    }
    ::close(fd);
    return LocalMergeTxJournalRecord::deserializeSigned(raw, out);
}

inline QStringList listDirEntries(int dirFd)
{
    QStringList out;
    const int duped = ::dup(dirFd);
    if (duped < 0) return out;
    DIR *d = ::fdopendir(duped);
    if (!d) {
        ::close(duped);
        return out;
    }
    ::rewinddir(d);
    while (const auto *ent = ::readdir(d)) {
        const QByteArray raw(ent->d_name);
        if (raw == "." || raw == "..") continue;
        out.append(QFile::decodeName(raw));
    }
    ::closedir(d);
    return out;
}

}  // namespace LocalMergeTxDetail

// Fail-closed, idempotent crash recovery inspector for Stage 3B.1.
// Never deletes unknown .thispc-part.*, unknown .thispc-replaced.*, or corrupted journals.
class LocalMergeRecoveryInspector
{
public:
    static LocalMergeRecoveryReport inspectDirectory(const QString &destDirPath, bool resolveUnambiguous = false)
    {
        LocalMergeRecoveryReport report;
        const int dirFd = ::open(QFile::encodeName(QDir::cleanPath(destDirPath)).constData(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dirFd < 0) {
            LocalMergeRecoveryFinding f;
            f.status = LocalMergeRecoveryStatus::ManualInterventionRequired;
            f.diagnostic = QStringLiteral("Could not open destination directory without following symlinks.");
            report.findings.append(f);
            report.anyManualInterventionRequired = true;
            return report;
        }

        const QStringList entries = LocalMergeTxDetail::listDirEntries(dirFd);
        for (const QString &name : entries) {
            if (name.startsWith(QStringLiteral(".thispc-part.")) || name.startsWith(QStringLiteral(".thispc-replaced."))) {
                report.preservedUntouchedFiles.append(QDir(destDirPath).filePath(name));
                continue;
            }
            if (!name.startsWith(QStringLiteral(".thispc-tx."))) {
                continue;
            }

            report.anyIncompleteFound = true;
            LocalMergeRecoveryFinding finding = inspectTxDir(dirFd, destDirPath, name, resolveUnambiguous);
            if (finding.manualInterventionRequired) {
                report.anyManualInterventionRequired = true;
            }
            report.findings.append(finding);
        }

        ::close(dirFd);
        return report;
    }

private:
    static LocalMergeRecoveryFinding inspectTxDir(
        int destParentFd,
        const QString &destDirPath,
        const QString &txDirName,
        bool resolveUnambiguous)
    {
        LocalMergeRecoveryFinding f;
        f.txDirName = txDirName;
        const QString txDirPath = QDir(destDirPath).filePath(txDirName);

        struct stat dirSt {};
        const QByteArray txDirEnc = QFile::encodeName(txDirName);
        if (::fstatat(destParentFd, txDirEnc.constData(), &dirSt, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISDIR(dirSt.st_mode)
            || dirSt.st_uid != ::geteuid()) {
            f.status = LocalMergeRecoveryStatus::ManualInterventionRequired;
            f.diagnostic = QStringLiteral("Transaction directory is missing, symlinked, or owned by another user.");
            return f;
        }

        const int txFd = ::openat(destParentFd, txDirEnc.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (txFd < 0) {
            f.status = LocalMergeRecoveryStatus::ManualInterventionRequired;
            f.diagnostic = QStringLiteral("Could not open transaction directory.");
            return f;
        }

        const QStringList txEntries = LocalMergeTxDetail::listDirEntries(txFd);
        QString journalName;
        QStringList unexpected;
        for (const QString &e : txEntries) {
            if (e.startsWith(QStringLiteral(".thispc-merge-tx."))) {
                if (e.endsWith(QStringLiteral(".tmp"))) continue;
                if (journalName.isEmpty()) journalName = e;
                else unexpected.append(e);
            } else if (e == QLatin1String("stage") || e.startsWith(QStringLiteral(".thispc-replaced."))) {
                continue;
            } else {
                unexpected.append(e);
            }
        }

        if (!unexpected.isEmpty()) {
            ::close(txFd);
            f.status = LocalMergeRecoveryStatus::ManualInterventionRequired;
            f.diagnostic = QStringLiteral("Foreign object(s) detected in transaction directory: %1").arg(unexpected.join(QStringLiteral(", ")));
            return f;
        }

        if (journalName.isEmpty()) {
            ::close(txFd);
            f.status = LocalMergeRecoveryStatus::ManualInterventionRequired;
            f.diagnostic = QStringLiteral("Transaction directory lacks a valid journal; preserving contents.");
            return f;
        }

        LocalMergeTxJournalRecord rec;
        if (!LocalMergeTxDetail::readJournalFile(txFd, journalName, &rec)) {
            ::close(txFd);
            f.status = LocalMergeRecoveryStatus::ManualInterventionRequired;
            f.diagnostic = QStringLiteral("Transaction journal is corrupted or truncated; preserving all objects.");
            return f;
        }

        f.txId = rec.txId;
        f.recordedPhase = rec.phase;
        f.destinationPath = rec.destinationPath;

        struct stat parentSt {};
        if (::fstat(destParentFd, &parentSt) != 0 || !rec.parentStat.matchesDirectoryIdentity(parentSt)) {
            ::close(txFd);
            f.status = LocalMergeRecoveryStatus::ManualInterventionRequired;
            f.diagnostic = QStringLiteral("Destination parent directory identity does not match journal.");
            return f;
        }

        struct stat dstSt {};
        const bool dstExists = (::fstatat(destParentFd, QFile::encodeName(rec.destFileName).constData(), &dstSt, AT_SYMLINK_NOFOLLOW) == 0);
        const bool dstIsOld = dstExists && S_ISREG(dstSt.st_mode) && rec.expectedDestStat.matchesStat(dstSt);
        const bool dstIsNew = dstExists && S_ISREG(dstSt.st_mode) && rec.stageStat.matchesStat(dstSt);

        struct stat stageEntrySt {};
        const bool stageExists = (::fstatat(txFd, QFile::encodeName(rec.stageFileName).constData(), &stageEntrySt, AT_SYMLINK_NOFOLLOW) == 0);
        const bool stageIsNew = stageExists && S_ISREG(stageEntrySt.st_mode) && rec.stageStat.matchesStat(stageEntrySt);
        const bool stageIsOld = stageExists && S_ISREG(stageEntrySt.st_mode) && rec.expectedDestStat.matchesStat(stageEntrySt);

        struct stat backupEntrySt {};
        const bool backupInTx = !rec.replacedBackupName.isEmpty()
            && (::fstatat(txFd, QFile::encodeName(rec.replacedBackupName).constData(), &backupEntrySt, AT_SYMLINK_NOFOLLOW) == 0)
            && S_ISREG(backupEntrySt.st_mode)
            && rec.expectedDestStat.matchesStat(backupEntrySt);
        const bool backupInParent = !rec.replacedBackupName.isEmpty()
            && (::fstatat(destParentFd, QFile::encodeName(rec.replacedBackupName).constData(), &backupEntrySt, AT_SYMLINK_NOFOLLOW) == 0)
            && S_ISREG(backupEntrySt.st_mode)
            && rec.expectedDestStat.matchesStat(backupEntrySt);

        // Determine real filesystem truth
        if (dstIsOld && (!stageExists || stageIsNew)) {
            f.oldAvailable = true;
            f.newAvailable = stageIsNew;
            f.status = LocalMergeRecoveryStatus::StagedOnlyAborted;
            f.manualInterventionRequired = false;
            f.diagnostic = QStringLiteral("Exchange did not occur; original destination OLD is intact.");
            if (resolveUnambiguous) {
                if (stageIsNew) {
                    ::unlinkat(txFd, QFile::encodeName(rec.stageFileName).constData(), 0);
                }
                ::unlinkat(txFd, QFile::encodeName(journalName).constData(), 0);
                ::close(txFd);
                ::unlinkat(destParentFd, txDirEnc.constData(), AT_REMOVEDIR);
                ::fsync(destParentFd);
                return f;
            }
            ::close(txFd);
            return f;
        }

        if (dstIsNew && (stageIsOld || backupInTx || backupInParent)) {
            f.newAvailable = true;
            f.oldAvailable = true;
            if (backupInParent) {
                f.recoveryOldPath = QDir(destDirPath).filePath(rec.replacedBackupName);
            } else if (backupInTx) {
                f.recoveryOldPath = QDir(txDirPath).filePath(rec.replacedBackupName);
            } else if (stageIsOld) {
                // CRASH-RECOVERY-01: Crash right after RENAME_EXCHANGE! Promote stage -> replacedBackupName
                if (resolveUnambiguous && !rec.replacedBackupName.isEmpty()
                    && ::syscall(SYS_renameat2, txFd, QFile::encodeName(rec.stageFileName).constData(),
                                 txFd, QFile::encodeName(rec.replacedBackupName).constData(), RENAME_NOREPLACE) == 0) {
                    ::fsync(txFd);
                    f.recoveryOldPath = QDir(txDirPath).filePath(rec.replacedBackupName);
                } else {
                    f.recoveryOldPath = QDir(txDirPath).filePath(rec.stageFileName);
                }
            }

            if (rec.phase == LocalMergeTxPhase::DirectoryDurable || rec.durabilityConfirmed) {
                f.status = LocalMergeRecoveryStatus::DurableCommitPendingCleanup;
                f.manualInterventionRequired = false;
                f.diagnostic = QStringLiteral("NEW is durable; displaced OLD backup still present at %1.").arg(f.recoveryOldPath);
            } else {
                f.status = LocalMergeRecoveryStatus::ExchangeOccurredOldSaved;
                f.manualInterventionRequired = true;
                f.diagnostic = QStringLiteral("Exchange occurred before crash/sync; NEW is at destination and OLD is preserved at %1.").arg(f.recoveryOldPath);
            }
            ::close(txFd);
            return f;
        }

        if (dstIsNew && !stageExists && !backupInTx && !backupInParent
            && (rec.phase == LocalMergeTxPhase::DirectoryDurable || rec.phase == LocalMergeTxPhase::Completed)) {
            f.newAvailable = true;
            f.oldAvailable = false;
            f.status = LocalMergeRecoveryStatus::DurableCommitPendingCleanup;
            f.manualInterventionRequired = false;
            f.diagnostic = QStringLiteral("Durable commit completed; only transaction journal remained.");
            ::close(txFd);
            return f;
        }

        ::close(txFd);
        f.oldAvailable = dstIsOld || stageIsOld || backupInTx || backupInParent;
        f.newAvailable = dstIsNew || stageIsNew;
        f.status = LocalMergeRecoveryStatus::ManualInterventionRequired;
        f.manualInterventionRequired = true;
        f.diagnostic = QStringLiteral("Inode state mismatch or missing object; preserving all files for manual recovery.");
        return f;
    }
};
