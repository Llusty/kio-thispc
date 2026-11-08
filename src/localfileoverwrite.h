/* Safe publication helpers for native single-file overwrites.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <QFile>
#include <QString>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

struct LocalFileIdentity
{
    struct stat value {};
    bool valid = false;

    static LocalFileIdentity read(const QString &path)
    {
        LocalFileIdentity identity;
        identity.valid = ::lstat(QFile::encodeName(path).constData(), &identity.value) == 0
            && S_ISREG(identity.value.st_mode);
        return identity;
    }

    bool sameFile(const LocalFileIdentity &other) const
    {
        return valid && other.valid && value.st_dev == other.value.st_dev
            && value.st_ino == other.value.st_ino;
    }

    bool matches(const QString &path) const
    {
        const auto other = read(path);
        return sameFile(other) && value.st_size == other.value.st_size
            && value.st_mode == other.value.st_mode
            && value.st_mtim.tv_sec == other.value.st_mtim.tv_sec
            && value.st_mtim.tv_nsec == other.value.st_mtim.tv_nsec;
    }
};

// Exchange keeps the old destination available for rollback. Filesystems
// without atomic exchange fail without changing either directory entry.
inline bool exchangeLocalFiles(const QString &first, const QString &second)
{
    return ::syscall(SYS_renameat2,
                     AT_FDCWD, QFile::encodeName(first).constData(),
                     AT_FDCWD, QFile::encodeName(second).constData(),
                     RENAME_EXCHANGE) == 0;
}

// Atomically publishes sourcePath at destinationPath only if destinationPath
// does not exist. Never falls back to non-atomic rename(2) which could silently
// overwrite a concurrently created destination file.
inline bool publishLocalFileNoOverwrite(
    const QString &sourcePath,
    const QString &destinationPath,
    bool *collisionDetected = nullptr,
    int *outErrorNumber = nullptr)
{
    if (collisionDetected) {
        *collisionDetected = false;
    }
    if (outErrorNumber) {
        *outErrorNumber = 0;
    }
    const QByteArray srcEncoded = QFile::encodeName(sourcePath);
    const QByteArray dstEncoded = QFile::encodeName(destinationPath);

#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
    if (::syscall(SYS_renameat2,
                  AT_FDCWD, srcEncoded.constData(),
                  AT_FDCWD, dstEncoded.constData(),
                  RENAME_NOREPLACE) == 0) {
        return true;
    }
    const int err = errno;
    if (err == EEXIST || err == EBUSY || err == ENOTEMPTY || err == EISDIR) {
        if (collisionDetected) {
            *collisionDetected = true;
        }
        if (outErrorNumber) {
            *outErrorNumber = EEXIST;
        }
        return false;
    }
    if (err != ENOSYS && err != EINVAL && err != EOPNOTSUPP) {
        if (outErrorNumber) {
            *outErrorNumber = err;
        }
        return false;
    }
#endif

    // Safe atomic fallback for filesystems that do not support RENAME_NOREPLACE:
    // POSIX link(2) fails atomically with EEXIST if destinationPath already exists.
    struct stat srcStat {};
    if (::lstat(srcEncoded.constData(), &srcStat) != 0 || !S_ISREG(srcStat.st_mode)) {
        if (outErrorNumber) {
            *outErrorNumber = errno ? errno : EINVAL;
        }
        return false;
    }
    if (::link(srcEncoded.constData(), dstEncoded.constData()) == 0) {
        if (::unlink(srcEncoded.constData()) == 0) {
            return true;
        }
        const int unlinkErr = errno;
        struct stat dstStat {};
        if (::lstat(dstEncoded.constData(), &dstStat) == 0
            && dstStat.st_dev == srcStat.st_dev
            && dstStat.st_ino == srcStat.st_ino) {
            ::unlink(dstEncoded.constData());
        }
        if (outErrorNumber) {
            *outErrorNumber = unlinkErr;
        }
        return false;
    }
    const int linkErr = errno;
    if (linkErr == EEXIST) {
        if (collisionDetected) {
            *collisionDetected = true;
        }
    }
    if (outErrorNumber) {
        *outErrorNumber = linkErr;
    }
    return false;
}
