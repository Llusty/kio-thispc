/* Safe publication helpers for native single-file overwrites.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <QFile>
#include <QString>
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
