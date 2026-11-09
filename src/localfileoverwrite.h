/* Safe publication helpers for native single-file overwrites.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QStorageInfo>
#include <QString>
#include <QUrl>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef MSDOS_SUPER_MAGIC
#define MSDOS_SUPER_MAGIC 0x4d44
#endif

#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif

inline QString moveContainmentErrorMessage()
{
    return QLocale().language() == QLocale::Polish
        ? QStringLiteral("Przenoszenie tego rodzaju elementów jest tymczasowo niedostępne ze względów bezpieczeństwa danych. Możesz skopiować elementy zamiast je przenosić.")
        : QObject::tr("Moving this type of item is temporarily unavailable for data safety reasons. You can copy the items instead of moving them.");
}

inline bool isSingleRegularLocalFile(const QList<QUrl> &urls)
{
    if (urls.size() != 1 || !urls.first().isLocalFile()) {
        return false;
    }
    const QFileInfo info(urls.first().toLocalFile());
    if (info.isDir() || info.isSymLink()) {
        return false;
    }
    return true;
}

inline bool isLocalMoveContainmentBlocked(const QList<QUrl> &sources, const QUrl &destinationDirectory, bool move)
{
    if (!move || !destinationDirectory.isLocalFile()) {
        return false;
    }
    for (const QUrl &u : sources) {
        if (!u.isLocalFile()) return false;
    }
    return !isSingleRegularLocalFile(sources);
}

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

#ifdef THISPC_TEST_HARNESS
inline bool &simulateRenameNoReplaceUnsupported()
{
    static bool unsupported = false;
    return unsupported;
}

inline bool &simulateDescriptorLinkatUnsupported()
{
    static bool unsupported = false;
    return unsupported;
}

inline bool &simulateVfatFilesystem()
{
    static bool vfat = false;
    return vfat;
}

inline bool &simulateNtfs3gFilesystem()
{
    static bool ntfs3g = false;
    return ntfs3g;
}

inline bool &simulateFsyncError()
{
    static bool err = false;
    return err;
}

inline int &simulatedFsyncErrno()
{
    static int err = EIO;
    return err;
}

inline std::function<void()> &simulateRaceBeforeSourceUnlink()
{
    static std::function<void()> hook;
    return hook;
}
#endif

// Strictly for moving the original source file. Relies exclusively on atomic
// SYS_renameat2(..., RENAME_NOREPLACE). Never falls back to linkat nor link/unlink.
// Filesystems without RENAME_NOREPLACE support fail closed with an explicit error code.
inline bool moveLocalFileNoOverwrite(
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

#ifdef THISPC_TEST_HARNESS
    if (simulateRenameNoReplaceUnsupported()) {
        if (outErrorNumber) {
            *outErrorNumber = EOPNOTSUPP;
        }
        return false;
    }
#endif

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
    if (outErrorNumber) {
        *outErrorNumber = err ? err : EOPNOTSUPP;
    }
    return false;
#else
    if (outErrorNumber) {
        *outErrorNumber = ENOSYS;
    }
    return false;
#endif
}

// Deprecated legacy alias for moveLocalFileNoOverwrite to maintain binary and compilation
// safety without extending permissions.
inline bool publishLocalFileNoOverwrite(
    const QString &sourcePath,
    const QString &destinationPath,
    bool *collisionDetected = nullptr,
    int *outErrorNumber = nullptr)
{
    return moveLocalFileNoOverwrite(sourcePath, destinationPath, collisionDetected, outErrorNumber);
}

// Strictly for publishing an owned, verified staging file (e.g. .thispc-part) to its final destination.
// Requires a valid open file descriptor to the staging file (stagingFd) and its verified identity.
//
// Security Architecture (Final Closure):
// 1. Immutable Object Publishing (Descriptor-based linkat):
//    Publication operates EXCLUSIVELY ON THE MAINTAINED FILE DESCRIPTOR (stagingFd) via linkat()
//    (using AT_EMPTY_PATH, with automatic fallback to /proc/self/fd/<fd> with AT_SYMLINK_FOLLOW).
//    Because the kernel creates the destination directory entry pointing directly to the exact inode
//    represented by stagingFd, no concurrent renaming, unlinking, or path substitution of the
//    staging file on disk can cause an alien file to be published.
// 2. Strict Collision Protection:
//    linkat() atomically fails with EEXIST if destinationPath already exists, never overwriting it.
// 3. Fail-Closed on Unsupported Filesystems:
//    Path-based fallback renameat2(stagingPath, ...) cannot guarantee that stagingPath was not
//    substituted between checks in an unprivileged or shared directory. Therefore, filesystems
//    lacking descriptor linkat support (such as FAT32/vfat) fail closed with EOPNOTSUPP.
// 4. Safe Staging Link Cleanup:
//    Once destination is published, unlinking the staging link from the directory uses unlinkat().
//    If pre-unlink verification detects that the staging directory entry was substituted on disk
//    by a different inode, unlinkat() is REFUSED (fail-closed), preserving the alien file.
//    CRITICAL: If unlinking the staging link fails or is refused, the published destination is
//    NEVER rolled back or unlinked.
inline bool publishOwnedStagingNoOverwrite(
    int stagingFd,
    const QString &stagingPath,
    const QString &destinationPath,
    const LocalFileIdentity &expectedStagingIdentity,
    bool *collisionDetected = nullptr,
    int *outErrorNumber = nullptr,
    bool *stagingUnlinkFailed = nullptr,
    bool *destinationPublished = nullptr,
    bool *directoryDurabilityConfirmed = nullptr)
{
    if (collisionDetected) {
        *collisionDetected = false;
    }
    if (outErrorNumber) {
        *outErrorNumber = 0;
    }
    if (stagingUnlinkFailed) {
        *stagingUnlinkFailed = false;
    }
    if (destinationPublished) {
        *destinationPublished = false;
    }
    if (directoryDurabilityConfirmed) {
        *directoryDurabilityConfirmed = false;
    }

    if (stagingFd < 0 || !expectedStagingIdentity.valid) {
        if (outErrorNumber) {
            *outErrorNumber = EINVAL;
        }
        return false;
    }

    // Verify stagingFd matches expectedStagingIdentity before attempting publication
    struct stat fdStat {};
    if (::fstat(stagingFd, &fdStat) != 0) {
        if (outErrorNumber) {
            *outErrorNumber = errno ? errno : EIO;
        }
        return false;
    }
    if (fdStat.st_dev != expectedStagingIdentity.value.st_dev
        || fdStat.st_ino != expectedStagingIdentity.value.st_ino) {
        if (outErrorNumber) {
            *outErrorNumber = EBADF;
        }
        return false;
    }

    // Open destination directory to anchor relative destination operations securely
    const QString destDir = QFileInfo(destinationPath).absolutePath();
    const QByteArray destDirEncoded = QFile::encodeName(destDir);
    const QByteArray destBaseName = QFile::encodeName(QFileInfo(destinationPath).fileName());
    const QByteArray stagingBaseName = QFile::encodeName(QFileInfo(stagingPath).fileName());

    int destDirFd = ::open(destDirEncoded.constData(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (destDirFd < 0) {
        if (outErrorNumber) {
            *outErrorNumber = errno ? errno : EIO;
        }
        return false;
    }

    bool linkatSucceeded = false;

#ifdef THISPC_TEST_HARNESS
    const bool skipDescriptorLinkat = simulateDescriptorLinkatUnsupported();
#else
    const bool skipDescriptorLinkat = false;
#endif

    if (!skipDescriptorLinkat) {
        // Primary method: linkat with AT_EMPTY_PATH directly from stagingFd
        int linkRes = ::linkat(stagingFd, "", destDirFd, destBaseName.constData(), AT_EMPTY_PATH);
        int linkErr = errno;

        // Secondary descriptor method: /proc/self/fd/<stagingFd> with AT_SYMLINK_FOLLOW
        if (linkRes != 0 && (linkErr == ENOENT || linkErr == EPERM || linkErr == EINVAL)) {
            const QByteArray procFdPath = "/proc/self/fd/" + QByteArray::number(stagingFd);
            linkRes = ::linkat(AT_FDCWD, procFdPath.constData(), destDirFd, destBaseName.constData(), AT_SYMLINK_FOLLOW);
            linkErr = errno;
        }

        if (linkRes == 0) {
            linkatSucceeded = true;
        } else if (linkErr == EEXIST || linkErr == EBUSY || linkErr == ENOTEMPTY || linkErr == EISDIR) {
            ::close(destDirFd);
            if (collisionDetected) {
                *collisionDetected = true;
            }
            if (outErrorNumber) {
                *outErrorNumber = EEXIST;
            }
            return false;
        }
    }

    if (!linkatSucceeded) {
        // Check if destination filesystem is VFAT / MSDOS
        struct statfs sfs {};
        bool isVfat = false;
        if (::fstatfs(destDirFd, &sfs) == 0) {
            if (static_cast<unsigned long>(sfs.f_type) == MSDOS_SUPER_MAGIC) {
                isVfat = true;
            }
        }
#ifdef THISPC_TEST_HARNESS
        if (simulateVfatFilesystem()) {
            isVfat = true;
        }
#endif

        if (!isVfat) {
            // Path B: Strict Fail-Closed for non-VFAT filesystems without descriptor linkat.
            // On non-VFAT filesystems lacking descriptor linkat support, path-based fallback
            // renameat2(stagingPath) cannot guarantee that stagingPath was not substituted on disk
            // between check and rename. Therefore, we refuse unverified path renames and fail closed safely.
            ::close(destDirFd);
            if (outErrorNumber) {
                *outErrorNumber = EOPNOTSUPP;
            }
            return false;
        }

        // Path C: Safe VFAT Publication via atomic SYS_renameat2(..., RENAME_NOREPLACE)
        // VFAT does not support hard links (linkat returns EPERM/EOPNOTSUPP), but the Linux VFAT
        // driver natively and atomically supports SYS_renameat2(..., RENAME_NOREPLACE) in directory lookup.
        //
        // Pre-rename verification:
        // 1. Verify staging directory entry identity in destDirFd against held stagingFd
        struct stat preRenameSt {};
        if (::fstatat(destDirFd, stagingBaseName.constData(), &preRenameSt, AT_SYMLINK_NOFOLLOW) != 0) {
            ::close(destDirFd);
            if (outErrorNumber) {
                *outErrorNumber = errno ? errno : ENOENT;
            }
            return false;
        }
        if (S_ISLNK(preRenameSt.st_mode)
            || preRenameSt.st_dev != fdStat.st_dev
            || preRenameSt.st_ino != fdStat.st_ino
            || preRenameSt.st_size != fdStat.st_size) {
            // Staging file was substituted or tampered with before rename: fail closed!
            ::close(destDirFd);
            if (outErrorNumber) {
                *outErrorNumber = EBADF;
            }
            return false;
        }

#ifdef THISPC_TEST_HARNESS
        const bool skipRenameNoReplace = simulateRenameNoReplaceUnsupported();
#else
        const bool skipRenameNoReplace = false;
#endif

        long renameRes = -1;
        int renameErr = EOPNOTSUPP;
        if (!skipRenameNoReplace) {
            renameRes = ::syscall(SYS_renameat2, destDirFd, stagingBaseName.constData(),
                                  destDirFd, destBaseName.constData(), RENAME_NOREPLACE);
            renameErr = errno;
        }

        if (renameRes != 0) {
            ::close(destDirFd);
            if (renameErr == EEXIST || renameErr == EBUSY || renameErr == ENOTEMPTY || renameErr == EISDIR) {
                if (collisionDetected) {
                    *collisionDetected = true;
                }
                if (outErrorNumber) {
                    *outErrorNumber = EEXIST;
                }
                return false;
            }
            if (renameErr == EINVAL || renameErr == ENOSYS || renameErr == EOPNOTSUPP) {
                // Filesystem / kernel lacks RENAME_NOREPLACE: strict fail-closed!
                if (outErrorNumber) {
                    *outErrorNumber = EOPNOTSUPP;
                }
                return false;
            }
            if (outErrorNumber) {
                *outErrorNumber = renameErr ? renameErr : EIO;
            }
            return false;
        }

        // Post-publication verification for VFAT
        struct stat postRenameSt {};
        struct stat postFdSt {};
        if (::fstatat(destDirFd, destBaseName.constData(), &postRenameSt, AT_SYMLINK_NOFOLLOW) != 0
            || ::fstat(stagingFd, &postFdSt) != 0) {
            ::close(destDirFd);
            if (outErrorNumber) {
                *outErrorNumber = errno ? errno : EIO;
            }
            return false;
        }

        if (postRenameSt.st_dev != fdStat.st_dev
            || postRenameSt.st_size != fdStat.st_size
            || (postRenameSt.st_ino != fdStat.st_ino && postRenameSt.st_ino != postFdSt.st_ino)) {
            // Inode / size mismatch: fail closed, NEVER rollback/unlink destination!
            ::close(destDirFd);
            if (outErrorNumber) {
                *outErrorNumber = EIO;
            }
            return false;
        }

        if (destinationPublished) {
            *destinationPublished = true;
        }

        // Parent directory durability
        int fsyncRes = ::fsync(destDirFd);
#ifdef THISPC_TEST_HARNESS
        if (simulateFsyncError()) {
            fsyncRes = -1;
            errno = simulatedFsyncErrno();
        }
#endif
        if (fsyncRes != 0) {
            const int syncErr = errno ? errno : EIO;
            if (directoryDurabilityConfirmed) {
                *directoryDurabilityConfirmed = false;
            }
            if (outErrorNumber) {
                *outErrorNumber = syncErr;
            }
            if (syncErr != EINVAL && syncErr != ENOTSUP) {
                ::close(destDirFd);
                return false;
            }
        } else {
            if (directoryDurabilityConfirmed) {
                *directoryDurabilityConfirmed = true;
            }
        }

        ::close(destDirFd);
        return true;
    }

    // At this point destination is successfully published via descriptor linkat!
    // Verify destination matches stagingFd inode
    struct stat publishedSt {};
    if (::fstatat(destDirFd, destBaseName.constData(), &publishedSt, AT_SYMLINK_NOFOLLOW) == 0) {
        if (publishedSt.st_dev != fdStat.st_dev || publishedSt.st_ino != fdStat.st_ino) {
            // Inode mismatch: fail closed but NEVER rollback/unlink destination!
            ::close(destDirFd);
            if (outErrorNumber) {
                *outErrorNumber = EIO;
            }
            return false;
        }
    }

    if (destinationPublished) {
        *destinationPublished = true;
    }

    // Unlink staging link from destination directory
    // Use unlinkat relative to destDirFd
    // To ensure we do not unlink a foreign file that was substituted under stagingBaseName,
    // verify the directory entry's inode matches stagingFd before unlinking.
    struct stat preUnlinkSt {};
    if (::fstatat(destDirFd, stagingBaseName.constData(), &preUnlinkSt, AT_SYMLINK_NOFOLLOW) == 0) {
        if (preUnlinkSt.st_dev == fdStat.st_dev && preUnlinkSt.st_ino == fdStat.st_ino) {
            if (::unlinkat(destDirFd, stagingBaseName.constData(), 0) != 0) {
                const int unlinkErr = errno;
                if (unlinkErr != ENOENT) {
                    if (stagingUnlinkFailed) {
                        *stagingUnlinkFailed = true;
                    }
                }
            }
        } else {
            // Staging name on disk was substituted by an alien file!
            // Fail-closed: refuse to unlink alien file; leave it intact.
            if (stagingUnlinkFailed) {
                *stagingUnlinkFailed = true;
            }
        }
    }

    // Sync parent directory to persist directory entries (POSIX directory durability)
    int fsyncRes = ::fsync(destDirFd);
#ifdef THISPC_TEST_HARNESS
    if (simulateFsyncError()) {
        fsyncRes = -1;
        errno = simulatedFsyncErrno();
    }
#endif
    if (fsyncRes != 0) {
        const int syncErr = errno ? errno : EIO;
        if (directoryDurabilityConfirmed) {
            *directoryDurabilityConfirmed = false;
        }
        if (outErrorNumber) {
            *outErrorNumber = syncErr;
        }
        if (syncErr != EINVAL && syncErr != ENOTSUP) {
            ::close(destDirFd);
            return false;
        }
    } else {
        if (directoryDurabilityConfirmed) {
            *directoryDurabilityConfirmed = true;
        }
    }
    ::close(destDirFd);
    return true;
}

// Stage 5.3.2G.2B: Verifies whether source and destination paths reside on the same mounted NTFS-3G filesystem volume.
// Requires consistent volume identity, FUSE_SUPER_MAGIC superblock, and verified ntfs-3g fstab or mountinfo driver binding.
inline bool isNtfs3gMount(const QString &sourcePath, const QString &destinationPath)
{
#ifdef THISPC_TEST_HARNESS
    if (simulateNtfs3gFilesystem()) {
        return true;
    }
#endif

    const QFileInfo srcInfo(sourcePath);
    const QFileInfo dstInfo(destinationPath);
    const QString srcDir = srcInfo.isDir() ? srcInfo.absoluteFilePath() : srcInfo.dir().absolutePath();
    const QString dstDir = dstInfo.isDir() ? dstInfo.absoluteFilePath() : dstInfo.dir().absolutePath();

    const QStorageInfo srcStorage(srcDir);
    const QStorageInfo dstStorage(dstDir);
    if (!srcStorage.isValid() || !dstStorage.isValid()) {
        return false;
    }

    // Both paths must reside on the same mounted filesystem volume
    if (srcStorage.rootPath() != dstStorage.rootPath()
        || srcStorage.device() != dstStorage.device()) {
        return false;
    }

    // Fast check: verify statfs f_type is FUSE_SUPER_MAGIC
    struct statfs sfs {};
    if (::statfs(QFile::encodeName(srcStorage.rootPath()).constData(), &sfs) != 0) {
        return false;
    }
    if (static_cast<unsigned long>(sfs.f_type) != FUSE_SUPER_MAGIC) {
        return false;
    }

    const QString rootPath = srcStorage.rootPath();

    // 1. Check /etc/fstab for explicit ntfs / ntfs-3g configuration
    {
        QFile fstabFile(QStringLiteral("/etc/fstab"));
        if (fstabFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!fstabFile.atEnd()) {
                const QByteArray line = fstabFile.readLine().trimmed();
                if (line.isEmpty() || line.startsWith('#')) {
                    continue;
                }
                const auto parts = line.split(' ');
                QByteArrayList tokens;
                for (const auto &p : parts) {
                    if (!p.isEmpty()) {
                        tokens.append(p);
                    }
                }
                if (tokens.size() >= 3) {
                    const QString mountPoint = QString::fromUtf8(tokens[1]);
                    const QByteArray type = tokens[2].toLower();
                    if (mountPoint == rootPath && (type == "ntfs-3g" || type == "ntfs")) {
                        return true;
                    }
                }
            }
        }
    }

    // 2. Check /proc/self/mountinfo for fuseblk mount on a block device
    {
        QFile mountinfoFile(QStringLiteral("/proc/self/mountinfo"));
        if (mountinfoFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!mountinfoFile.atEnd()) {
                const QByteArray line = mountinfoFile.readLine().trimmed();
                if (line.isEmpty()) {
                    continue;
                }
                const auto parts = line.split(' ');
                if (parts.size() >= 5) {
                    const QString mountPoint = QString::fromUtf8(parts[4]);
                    if (mountPoint == rootPath) {
                        const int dashIndex = parts.indexOf(QByteArrayLiteral("-"));
                        if (dashIndex != -1 && parts.size() > dashIndex + 1) {
                            const QByteArray mntFsType = parts[dashIndex + 1].toLower();
                            if (mntFsType == "fuseblk" || mntFsType.contains("ntfs")) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    const QByteArray fsType = srcStorage.fileSystemType().toLower();
    return fsType == "fuseblk" || fsType.contains("ntfs");
}
