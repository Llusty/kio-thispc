/*
 * Shared Btrfs/openat2/statx/containment primitives for safe atomic local moves (H.2A / H.2B.1 / H.2B.2).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "localfileoverwrite.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <linux/stat.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef BTRFS_SUPER_MAGIC
#define BTRFS_SUPER_MAGIC 0x9123683EUL
#endif

#ifndef EXT4_SUPER_MAGIC
#define EXT4_SUPER_MAGIC 0xEF53UL
#endif

#ifndef NTFS3_SUPER_MAGIC
#define NTFS3_SUPER_MAGIC 0x7366746EUL
#endif

#ifndef BTRFS_FIRST_FREE_OBJECTID
#define BTRFS_FIRST_FREE_OBJECTID 256ULL
#endif

#ifndef FS_CASEFOLD_FL
#define FS_CASEFOLD_FL 0x10000000U
#endif

#ifndef FS_ENCRYPT_FL
#define FS_ENCRYPT_FL 0x00000800U
#endif

#ifndef FS_IOC_GETFLAGS
#define FS_IOC_GETFLAGS _IOR('f', 1, long)
#endif

#ifndef STATX_TYPE
#define STATX_TYPE 0x00000001U
#endif
#ifndef STATX_MODE
#define STATX_MODE 0x00000002U
#endif
#ifndef STATX_NLINK
#define STATX_NLINK 0x00000004U
#endif
#ifndef STATX_MTIME
#define STATX_MTIME 0x00000040U
#endif
#ifndef STATX_CTIME
#define STATX_CTIME 0x00000080U
#endif
#ifndef STATX_INO
#define STATX_INO 0x00000100U
#endif
#ifndef STATX_SIZE
#define STATX_SIZE 0x00000200U
#endif
#ifndef STATX_BASIC_STATS
#define STATX_BASIC_STATS 0x000007ffU
#endif
#ifndef STATX_MNT_ID
#define STATX_MNT_ID 0x00001000U
#endif
#ifndef STATX_MNT_ID_UNIQUE
#define STATX_MNT_ID_UNIQUE 0x00004000U
#endif
#ifndef STATX_SUBVOL
#define STATX_SUBVOL 0x00008000U
#endif
#ifndef AT_NO_AUTOMOUNT
#define AT_NO_AUTOMOUNT 0x800
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

struct LocalDirectoryIdentity {
    quint64 devMajor = 0;
    quint64 devMinor = 0;
    quint64 stDev = 0;
    quint64 ino = 0;
    quint64 mntId = 0;
    bool hasMntId = false;
    bool hasUniqueMntId = false;
    quint64 subvolId = 0;
    bool hasSubvol = false;
    qint64 mtimeSec = 0;
    quint32 mtimeNsec = 0;
    qint64 ctimeSec = 0;
    quint32 ctimeNsec = 0;
    bool hasTimestamps = false;
    bool valid = false;

    static LocalDirectoryIdentity fromStat(const struct statx &stx, const struct stat &st)
    {
        LocalDirectoryIdentity id;
        const bool hasType = (stx.stx_mask & STATX_TYPE) != 0;
        const bool hasIno = (stx.stx_mask & STATX_INO) != 0;
        const bool hasMnt = (stx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) != 0;
        if (!hasType || !hasIno || !hasMnt || !S_ISDIR(stx.stx_mode)) {
            return id;
        }
        id.devMajor = stx.stx_dev_major;
        id.devMinor = stx.stx_dev_minor;
        id.stDev = static_cast<quint64>(st.st_dev);
        id.ino = static_cast<quint64>(stx.stx_ino);
        id.mntId = static_cast<quint64>(stx.stx_mnt_id);
        id.hasMntId = hasMnt;
        id.hasUniqueMntId = (stx.stx_mask & STATX_MNT_ID_UNIQUE) != 0;
        id.hasSubvol = (stx.stx_mask & STATX_SUBVOL) != 0;
        id.subvolId = id.hasSubvol ? static_cast<quint64>(stx.stx_subvol) : 0;
        id.hasTimestamps = (stx.stx_mask & (STATX_MTIME | STATX_CTIME)) == (STATX_MTIME | STATX_CTIME);
        if (id.hasTimestamps) {
            id.mtimeSec = static_cast<qint64>(stx.stx_mtime.tv_sec);
            id.mtimeNsec = static_cast<quint32>(stx.stx_mtime.tv_nsec);
            id.ctimeSec = static_cast<qint64>(stx.stx_ctime.tv_sec);
            id.ctimeNsec = static_cast<quint32>(stx.stx_ctime.tv_nsec);
        }
        id.valid = true;
        return id;
    }

    bool sameObject(const LocalDirectoryIdentity &other) const
    {
        if (!valid || !other.valid) {
            return false;
        }
        if (devMajor != other.devMajor || devMinor != other.devMinor
            || stDev != other.stDev || ino != other.ino) {
            return false;
        }
        if (hasUniqueMntId == other.hasUniqueMntId && mntId != other.mntId) {
            return false;
        }
        if (hasSubvol && other.hasSubvol && subvolId != other.subvolId) {
            return false;
        }
        return true;
    }

    bool strictlyUnchangedForHistory(const LocalDirectoryIdentity &other) const
    {
        if (!sameObject(other)) {
            return false;
        }
        if (!hasTimestamps || !other.hasTimestamps) {
            return false;
        }
        return mtimeSec == other.mtimeSec
            && mtimeNsec == other.mtimeNsec
            && ctimeSec == other.ctimeSec
            && ctimeNsec == other.ctimeNsec;
    }

    bool sameMountScope(const LocalDirectoryIdentity &other) const
    {
        if (!valid || !other.valid) {
            return false;
        }
        if (!hasMntId || !other.hasMntId || hasUniqueMntId != other.hasUniqueMntId) {
            return false;
        }
        if (mntId != other.mntId || devMajor != other.devMajor
            || devMinor != other.devMinor || stDev != other.stDev) {
            return false;
        }
        if (hasSubvol != other.hasSubvol) {
            return false;
        }
        if (hasSubvol && subvolId != other.subvolId) {
            return false;
        }
        return true;
    }
};

struct LocalRegularFileIdentity {
    quint64 devMajor = 0;
    quint64 devMinor = 0;
    quint64 stDev = 0;
    quint64 ino = 0;
    quint64 mntId = 0;
    bool hasMntId = false;
    bool hasUniqueMntId = false;
    quint64 subvolId = 0;
    bool hasSubvol = false;
    quint64 size = 0;
    qint64 mtimeSec = 0;
    quint32 mtimeNsec = 0;
    qint64 ctimeSec = 0;
    quint32 ctimeNsec = 0;
    quint32 nlink = 0;
    bool valid = false;

    static LocalRegularFileIdentity fromStat(const struct statx &stx, const struct stat &st)
    {
        LocalRegularFileIdentity id;
        const bool hasType = (stx.stx_mask & STATX_TYPE) != 0;
        const bool hasIno = (stx.stx_mask & STATX_INO) != 0;
        const bool hasMnt = (stx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) != 0;
        const bool hasNlink = (stx.stx_mask & STATX_NLINK) != 0;
        const bool hasSize = (stx.stx_mask & STATX_SIZE) != 0;
        const bool hasMtime = (stx.stx_mask & STATX_MTIME) != 0;
        const bool hasCtime = (stx.stx_mask & STATX_CTIME) != 0;
        if (!hasType || !hasIno || !hasMnt || !hasNlink || !hasSize || !hasMtime || !hasCtime
            || !S_ISREG(stx.stx_mode)) {
            return id;
        }
        id.devMajor = stx.stx_dev_major;
        id.devMinor = stx.stx_dev_minor;
        id.stDev = static_cast<quint64>(st.st_dev);
        id.ino = static_cast<quint64>(stx.stx_ino);
        id.mntId = static_cast<quint64>(stx.stx_mnt_id);
        id.hasMntId = true;
        id.hasUniqueMntId = (stx.stx_mask & STATX_MNT_ID_UNIQUE) != 0;
        id.hasSubvol = (stx.stx_mask & STATX_SUBVOL) != 0;
        id.subvolId = id.hasSubvol ? static_cast<quint64>(stx.stx_subvol) : 0;
        id.size = static_cast<quint64>(stx.stx_size);
        id.mtimeSec = static_cast<qint64>(stx.stx_mtime.tv_sec);
        id.mtimeNsec = static_cast<quint32>(stx.stx_mtime.tv_nsec);
        id.ctimeSec = static_cast<qint64>(stx.stx_ctime.tv_sec);
        id.ctimeNsec = static_cast<quint32>(stx.stx_ctime.tv_nsec);
        id.nlink = static_cast<quint32>(stx.stx_nlink);
        id.valid = true;
        return id;
    }

    bool sameMountScope(const LocalDirectoryIdentity &dirId) const
    {
        if (!valid || !dirId.valid || !hasMntId || !dirId.hasMntId) {
            return false;
        }
        if (hasUniqueMntId != dirId.hasUniqueMntId || mntId != dirId.mntId) {
            return false;
        }
        if (devMajor != dirId.devMajor || devMinor != dirId.devMinor || stDev != dirId.stDev) {
            return false;
        }
        if (hasSubvol != dirId.hasSubvol) {
            return false;
        }
        if (hasSubvol && subvolId != dirId.subvolId) {
            return false;
        }
        return true;
    }

    bool sameObject(const LocalRegularFileIdentity &other) const
    {
        if (!valid || !other.valid) {
            return false;
        }
        if (devMajor != other.devMajor || devMinor != other.devMinor
            || stDev != other.stDev || ino != other.ino) {
            return false;
        }
        if (hasUniqueMntId == other.hasUniqueMntId && mntId != other.mntId) {
            return false;
        }
        if (hasSubvol && other.hasSubvol && subvolId != other.subvolId) {
            return false;
        }
        return true;
    }

    // Checks object identity plus nlink==1, byte size, and nanosecond mtime/ctime.
    // Note: catches in-place VFS content edits even when inode is unchanged; offline
    // raw block mutations or privileged clock spoofing outside VFS remain out of band.
    bool strictlyUnchangedForHistory(const LocalRegularFileIdentity &other) const
    {
        if (!sameObject(other)) {
            return false;
        }
        if (nlink != 1 || other.nlink != 1) {
            return false;
        }
        return size == other.size
            && mtimeSec == other.mtimeSec
            && mtimeNsec == other.mtimeNsec
            && ctimeSec == other.ctimeSec
            && ctimeNsec == other.ctimeNsec;
    }
};

struct LocalDirectoryTreeSnapshot {
    quint64 scannedBytes = 0;
    qsizetype scannedRegularFiles = 0;
    qsizetype scannedSubdirectories = 0;
    QByteArray treeDigest;
    bool strictContentDigest = false;
    bool valid = false;

    bool matches(const LocalDirectoryTreeSnapshot &other, bool requireContentDigest = false) const
    {
        if (!valid || !other.valid
            || scannedBytes != other.scannedBytes
            || scannedRegularFiles != other.scannedRegularFiles
            || scannedSubdirectories != other.scannedSubdirectories) {
            return false;
        }
        if (requireContentDigest || strictContentDigest || other.strictContentDigest) {
            return strictContentDigest && other.strictContentDigest
                && treeDigest.size() == 32
                && treeDigest == other.treeDigest;
        }
        return true;
    }
};

struct LocalFileHistorySnapshot {
    LocalRegularFileIdentity identity;
    quint64 size = 0;
    unsigned long fsMagic = 0;
    QByteArray contentDigest;
    bool strictContentDigest = false;
    bool valid = false;

    bool matches(const LocalFileHistorySnapshot &other, bool requireContentDigest = true) const
    {
        if (!valid || !other.valid
            || !identity.valid || !other.identity.valid
            || identity.hasMntId != other.identity.hasMntId
            || identity.hasUniqueMntId != other.identity.hasUniqueMntId
            || !identity.sameObject(other.identity)
            || identity.nlink != 1 || other.identity.nlink != 1
            || size != other.size
            || fsMagic == 0 || fsMagic != other.fsMagic) {
            return false;
        }
        if (requireContentDigest || strictContentDigest || other.strictContentDigest) {
            return strictContentDigest && other.strictContentDigest
                && contentDigest.size() == 32
                && contentDigest == other.contentDigest;
        }
        return true;
    }
};

namespace LocalBtrfsMoveCommon {

enum class ContainmentFailureKind {
    None,
    Cancelled,
    InvalidInput,
    UnsupportedKernelFeature,
    UnsupportedFilesystem,
    MountOrSubvolumeBoundary,
    ContainmentViolation,
};

enum class LocalMoveFsProfile {
    BtrfsOnly,
    Ext4Only,
};

enum class VerifiedMoveFsProvenance {
    Unknown = 0,
    Btrfs = 1,
    Ext4 = 2,
};

inline VerifiedMoveFsProvenance provenanceFromFsMagic(unsigned long magic)
{
    if (magic == BTRFS_SUPER_MAGIC) {
        return VerifiedMoveFsProvenance::Btrfs;
    }
    if (magic == EXT4_SUPER_MAGIC) {
        return VerifiedMoveFsProvenance::Ext4;
    }
    return VerifiedMoveFsProvenance::Unknown;
}

struct FilesystemMoveQualificationProfile {
    LocalMoveFsProfile fsProfile = LocalMoveFsProfile::BtrfsOnly;

    bool isExt4() const { return fsProfile == LocalMoveFsProfile::Ext4Only; }
    unsigned long expectedMagic() const
    {
        return isExt4() ? EXT4_SUPER_MAGIC : BTRFS_SUPER_MAGIC;
    }
    bool requireUniqueMountId() const { return isExt4(); }
    bool checkBtrfsSubvolRootInode() const { return !isExt4(); }
    bool checkExt4DirectoryFlags() const { return isExt4(); }
};

struct TestFaultHooks {
    bool openat2Unsupported = false;
    unsigned long unsupportedFsMagic = 0;
    bool statxMntIdUnsupported = false;
    bool simulateExt4Filesystem = false;
    bool simulateExt4StatxMntIdUniqueMissing = false;
    bool simulateExt4FlagsError = false;
    bool simulateExt4CasefoldAllDirs = false;
    quint64 simulateExt4CasefoldInode = 0;
    bool simulateExt4Encrypt = false;
    bool simulateFileMoveMntIdChange = false;
    std::atomic<int> *snapshotFilesHashedCounter = nullptr;
};

struct ScopedFd {
    int fd = -1;
    ScopedFd() = default;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd() { reset(); }
    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;
    ScopedFd(ScopedFd &&other) noexcept : fd(other.fd) { other.fd = -1; }
    ScopedFd &operator=(ScopedFd &&other) noexcept
    {
        if (this != &other) {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    bool isValid() const { return fd >= 0; }
    void reset(int next = -1)
    {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = next;
    }
};

inline QString trMsg(const char *pl, const char *en)
{
    return QLocale().language() == QLocale::Polish
        ? QString::fromUtf8(pl)
        : QString::fromUtf8(en);
}

inline bool pathsOverlap(const QString &leftClean, const QString &rightClean)
{
    if (leftClean == rightClean) {
        return true;
    }
    auto isAncestor = [](const QString &ancestor, const QString &path) {
        if (ancestor == QLatin1String("/")) {
            return true;
        }
        return path.size() > ancestor.size()
            && path.startsWith(ancestor)
            && path.at(ancestor.size()) == QLatin1Char('/');
    };
    return isAncestor(leftClean, rightClean) || isAncestor(rightClean, leftClean);
}

inline bool splitCleanLocalPath(
    const QString &rawPath,
    QString *parentPathOut,
    QString *leafNameOut)
{
    if (rawPath.isEmpty() || !QDir::isAbsolutePath(rawPath)) {
        return false;
    }
    const QStringList rawSegments = rawPath.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &seg : rawSegments) {
        if (seg == QLatin1String("..") || seg == QLatin1String(".")) {
            return false;
        }
    }
    const QString clean = QDir::cleanPath(rawPath);
    if (clean.isEmpty() || clean == QLatin1String("/")) {
        return false;
    }
    const int slash = clean.lastIndexOf(QLatin1Char('/'));
    if (slash < 0) {
        return false;
    }
    const QString parent = (slash == 0) ? QStringLiteral("/") : clean.left(slash);
    const QString leaf = clean.mid(slash + 1);
    if (leaf.isEmpty()
        || leaf == QLatin1String(".")
        || leaf == QLatin1String("..")
        || leaf.contains(QLatin1Char('/'))
        || leaf.contains(QChar(u'\0'))) {
        return false;
    }
    if (parentPathOut) *parentPathOut = parent;
    if (leafNameOut) *leafNameOut = leaf;
    return true;
}

inline int openat2Syscall(
    int dirFd,
    const char *path,
    quint64 flags,
    quint64 resolve,
    int *errOut,
    const TestFaultHooks *hooks = nullptr)
{
    if (hooks && hooks->openat2Unsupported) {
        if (errOut) *errOut = ENOSYS;
        return -1;
    }
#ifdef SYS_openat2
    struct open_how how {};
    how.flags = flags;
    how.mode = 0;
    how.resolve = resolve;
    const int fd = static_cast<int>(::syscall(SYS_openat2, dirFd, path, &how, sizeof(how)));
    if (fd < 0) {
        if (errOut) *errOut = errno ? errno : EIO;
        return -1;
    }
    if (errOut) *errOut = 0;
    return fd;
#else
    Q_UNUSED(dirFd);
    Q_UNUSED(path);
    Q_UNUSED(flags);
    Q_UNUSED(resolve);
    if (errOut) *errOut = ENOSYS;
    return -1;
#endif
}

inline int openDirectoryNoSymlinks(
    const QString &absDirPath,
    int *errOut,
    const TestFaultHooks *hooks = nullptr)
{
    const QByteArray encoded = QFile::encodeName(absDirPath);
    return openat2Syscall(
        AT_FDCWD,
        encoded.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS,
        errOut,
        hooks);
}

inline int openChildPathNoCrossing(
    int parentFd,
    const char *leafName,
    int *errOut,
    const TestFaultHooks *hooks = nullptr)
{
    return openat2Syscall(
        parentFd,
        leafName,
        O_PATH | O_NOFOLLOW | O_CLOEXEC,
        RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_BENEATH | RESOLVE_NO_XDEV,
        errOut,
        hooks);
}

inline int openChildDirectoryNoCrossing(
    int parentFd,
    const char *leafName,
    int *errOut,
    const TestFaultHooks *hooks = nullptr)
{
    return openat2Syscall(
        parentFd,
        leafName,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC,
        RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_BENEATH | RESOLVE_NO_XDEV,
        errOut,
        hooks);
}

inline bool queryFdMetadata(
    int fd,
    struct statx *stxOut,
    struct stat *stOut,
    unsigned long *fsMagicOut,
    int *errOut,
    const TestFaultHooks *hooks = nullptr)
{
    if (fd < 0 || !stxOut || !stOut || !fsMagicOut) {
        if (errOut) *errOut = EINVAL;
        return false;
    }
    if (::fstat(fd, stOut) != 0) {
        if (errOut) *errOut = errno ? errno : EIO;
        return false;
    }
    struct statfs sfs {};
    if (::fstatfs(fd, &sfs) != 0) {
        if (errOut) *errOut = errno ? errno : EIO;
        return false;
    }
    *fsMagicOut = static_cast<unsigned long>(sfs.f_type);
    if (hooks && hooks->simulateExt4Filesystem) {
        *fsMagicOut = EXT4_SUPER_MAGIC;
    }
    if (hooks && hooks->unsupportedFsMagic != 0) {
        *fsMagicOut = hooks->unsupportedFsMagic;
    }

#ifdef SYS_statx
    std::memset(stxOut, 0, sizeof(*stxOut));
    const unsigned int mask =
        STATX_BASIC_STATS | STATX_MNT_ID | STATX_MNT_ID_UNIQUE | STATX_SUBVOL;
    if (::syscall(SYS_statx, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, mask, stxOut) != 0) {
        if (errOut) *errOut = errno ? errno : EIO;
        return false;
    }
    if (hooks && hooks->simulateExt4Filesystem) {
        stxOut->stx_mask &= ~STATX_SUBVOL;
        stxOut->stx_subvol = 0;
    }
    if (hooks && hooks->simulateExt4StatxMntIdUniqueMissing) {
        stxOut->stx_mask &= ~STATX_MNT_ID_UNIQUE;
    }
    if (hooks && hooks->statxMntIdUnsupported) {
        stxOut->stx_mask &= ~(STATX_MNT_ID | STATX_MNT_ID_UNIQUE);
        stxOut->stx_mnt_id = 0;
    }
    if (errOut) *errOut = 0;
    return true;
#else
    if (errOut) *errOut = ENOSYS;
    return false;
#endif
}

inline bool verifyExt4DirectorySafetyFlags(
    int dirFd,
    quint64 dirIno,
    ContainmentFailureKind *failureOut,
    QString *reasonOut,
    const TestFaultHooks *hooks = nullptr)
{
    auto fail = [&](ContainmentFailureKind kind, const QString &reason) {
        if (failureOut) *failureOut = kind;
        if (reasonOut) *reasonOut = reason;
        return false;
    };

    if (hooks && hooks->simulateExt4FlagsError) {
        return fail(ContainmentFailureKind::ContainmentViolation,
                    trMsg("Nie można zweryfikować flag ext4 katalogu (FS_IOC_GETFLAGS) — operacja zablokowana.",
                          "Could not verify ext4 directory flags (FS_IOC_GETFLAGS) — blocked fail-closed."));
    }

    long rawFlags = 0;
    if (::ioctl(dirFd, FS_IOC_GETFLAGS, &rawFlags) != 0) {
        return fail(ContainmentFailureKind::ContainmentViolation,
                    trMsg("Nie można odczytać flag ext4 katalogu (FS_IOC_GETFLAGS) — operacja zablokowana.",
                          "Could not read ext4 directory flags (FS_IOC_GETFLAGS) — blocked fail-closed."));
    }

    const unsigned long flags = static_cast<unsigned long>(rawFlags);
    const bool hasCasefold = (flags & FS_CASEFOLD_FL) != 0
        || (hooks && hooks->simulateExt4CasefoldAllDirs)
        || (hooks && hooks->simulateExt4CasefoldInode != 0 && dirIno == hooks->simulateExt4CasefoldInode);
    if (hasCasefold) {
        return fail(ContainmentFailureKind::ContainmentViolation,
                    trMsg("Katalog ext4 z włączoną obsługą casefold (bez rozróżniania wielkości liter) jest zablokowany ze względów bezpieczeństwa.",
                          "ext4 directory with casefold enabled is blocked fail-closed for safety."));
    }

    const bool hasEncrypt = (flags & FS_ENCRYPT_FL) != 0
        || (hooks && hooks->simulateExt4Encrypt);
    if (hasEncrypt) {
        return fail(ContainmentFailureKind::ContainmentViolation,
                    trMsg("Zaszyfrowany katalog ext4 (FS_ENCRYPT_FL) jest zablokowany ze względów bezpieczeństwa.",
                          "Encrypted ext4 directory (FS_ENCRYPT_FL) is blocked fail-closed for safety."));
    }

    if (failureOut) *failureOut = ContainmentFailureKind::None;
    return true;
}

inline bool validateQualifiedMountAndFs(
    const struct statx &stx,
    unsigned long magic,
    const FilesystemMoveQualificationProfile &profile,
    ContainmentFailureKind *failureOut,
    QString *reasonOut)
{
    if (magic != profile.expectedMagic()) {
        if (failureOut) *failureOut = ContainmentFailureKind::UnsupportedFilesystem;
        if (reasonOut) {
            *reasonOut = profile.isExt4()
                ? trMsg("Ta operacja wymaga zakwalifikowanego woluminu ext4.",
                        "This operation requires a qualified ext4 volume.")
                : trMsg("Ta operacja wymaga zakwalifikowanego woluminu Btrfs.",
                        "This operation requires a qualified Btrfs volume.");
        }
        return false;
    }

    const bool hasRequiredMntId = profile.requireUniqueMountId()
        ? ((stx.stx_mask & STATX_MNT_ID_UNIQUE) != 0)
        : ((stx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) != 0);
    if (!hasRequiredMntId) {
        if (failureOut) *failureOut = ContainmentFailureKind::UnsupportedKernelFeature;
        if (reasonOut) {
            *reasonOut = profile.requireUniqueMountId()
                ? trMsg("Jądro nie zwróciło wymaganego unikalnego identyfikatora montowania statx(STATX_MNT_ID_UNIQUE).",
                        "Kernel did not provide required unique statx mount identity (STATX_MNT_ID_UNIQUE).")
                : trMsg("Jądro nie zwróciło identyfikatora montowania statx(STATX_MNT_ID).",
                        "Kernel did not provide statx mount identity (STATX_MNT_ID).");
        }
        return false;
    }

    if (failureOut) *failureOut = ContainmentFailureKind::None;
    return true;
}

inline QByteArray unescapeMountInfoField(const QByteArray &token)
{
    QByteArray out;
    out.reserve(token.size());
    for (int i = 0; i < token.size(); ++i) {
        if (token.at(i) == '\\' && i + 3 < token.size()
            && token.at(i + 1) >= '0' && token.at(i + 1) <= '7'
            && token.at(i + 2) >= '0' && token.at(i + 2) <= '7'
            && token.at(i + 3) >= '0' && token.at(i + 3) <= '7') {
            const int val = ((token.at(i + 1) - '0') << 6)
                | ((token.at(i + 2) - '0') << 3)
                | (token.at(i + 3) - '0');
            out.append(static_cast<char>(val));
            i += 3;
        } else {
            out.append(token.at(i));
        }
    }
    return out;
}

inline bool isMountPointInMountInfo(const QString &cleanPath)
{
    const QByteArray target = QFile::encodeName(QDir::cleanPath(cleanPath));
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return true; // fail closed if mount table cannot be inspected
    }
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty()) continue;
        const QList<QByteArray> parts = line.split(' ');
        if (parts.size() >= 5 && unescapeMountInfoField(parts.at(4)) == target) {
            return true;
        }
    }
    return false;
}

inline bool dstParentHasAncestor(
    int dstParentFd,
    const LocalDirectoryIdentity &dstParentId,
    const LocalDirectoryIdentity &forbiddenAncestor,
    const TestFaultHooks *hooks = nullptr)
{
    if (!dstParentId.valid || !forbiddenAncestor.valid) {
        return true; // ambiguous -> fail closed
    }
    if (dstParentId.sameObject(forbiddenAncestor)) {
        return true;
    }

    int dupErr = 0;
    const int initialDup = ::fcntl(dstParentFd, F_DUPFD_CLOEXEC, 3);
    if (initialDup < 0) {
        return true;
    }
    ScopedFd curFd(initialDup);
    quint64 prevDevMajor = dstParentId.devMajor;
    quint64 prevDevMinor = dstParentId.devMinor;
    quint64 prevIno = dstParentId.ino;

    for (int depth = 0; depth < 4096; ++depth) {
        int nextFd = ::openat(curFd.fd, "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nextFd < 0) {
            return true; // inaccessible parent -> fail closed
        }
        ScopedFd parentFd(nextFd);
        struct statx stx {};
        struct stat st {};
        unsigned long magic = 0;
        if (!queryFdMetadata(parentFd.fd, &stx, &st, &magic, &dupErr, hooks)) {
            return true;
        }
        const LocalDirectoryIdentity parentId = LocalDirectoryIdentity::fromStat(stx, st);
        if (!parentId.valid) {
            return true;
        }
        if (parentId.sameObject(forbiddenAncestor)) {
            return true;
        }
        // Reached root of mount or filesystem root
        if (!parentId.sameMountScope(dstParentId)) {
            return false;
        }
        if (parentId.devMajor == prevDevMajor
            && parentId.devMinor == prevDevMinor
            && parentId.ino == prevIno) {
            return false;
        }
        prevDevMajor = parentId.devMajor;
        prevDevMinor = parentId.devMinor;
        prevIno = parentId.ino;
        curFd = std::move(parentFd);
    }
    return true; // unexpectedly deep hierarchy -> fail closed
}

inline bool inspectDirectoryTreeContainment(
    int rootDirFd,
    const LocalDirectoryIdentity &rootIdentity,
    const std::atomic<bool> &cancelled,
    LocalDirectoryTreeSnapshot *snapshotOut,
    QSet<QPair<quint64, quint64>> *visitedObjectsOut,
    ContainmentFailureKind *failureOut,
    QString *reasonOut,
    const TestFaultHooks *hooks = nullptr,
    const FilesystemMoveQualificationProfile &profile = {},
    bool computeContentDigest = false,
    bool enforceStrictSnapshotLimits = false)
{
    constexpr qsizetype kMaxSnapshotEntries = 100000;
    constexpr int kMaxSnapshotDepth = 256;
    constexpr quint64 kMaxSnapshotHashedBytes = 512ULL * 1024ULL * 1024ULL;
    const bool checkStrictLimits = computeContentDigest || enforceStrictSnapshotLimits;

    struct SnapshotEntry {
        QByteArray relPath;
        quint8 type = 0; // 'D' or 'F'
        quint64 ino = 0;
        quint64 size = 0;
        QByteArray sha256;
    };

    auto fail = [&](ContainmentFailureKind kind, const QString &reason) {
        if (failureOut) *failureOut = kind;
        if (reasonOut) *reasonOut = reason;
        return false;
    };

    if (profile.checkExt4DirectoryFlags()) {
        ContainmentFailureKind flagFailure = ContainmentFailureKind::None;
        QString flagReason;
        if (!verifyExt4DirectorySafetyFlags(rootDirFd, rootIdentity.ino, &flagFailure, &flagReason, hooks)) {
            return fail(flagFailure, flagReason);
        }
    }

    const int initialFd = ::fcntl(rootDirFd, F_DUPFD_CLOEXEC, 3);
    if (initialFd < 0) {
        return fail(ContainmentFailureKind::InvalidInput,
                    trMsg("Nie udało się zduplikować deskryptora katalogu.",
                          "Could not duplicate directory descriptor."));
    }

    struct DirFrame {
        ScopedFd fd;
        QByteArray relPath;
        int depth = 0;
    };

    std::vector<DirFrame> dirStack;
    dirStack.push_back(DirFrame{ScopedFd(initialFd), QByteArray(), 0});
    QSet<QPair<quint64, quint64>> visitedDirs;
    visitedDirs.insert({rootIdentity.stDev, rootIdentity.ino});
    if (visitedObjectsOut) {
        visitedObjectsOut->insert({rootIdentity.stDev, rootIdentity.ino});
    }

    quint64 totalBytes = 0;
    qsizetype regularCount = 0;
    qsizetype subdirCount = 0;
    std::vector<SnapshotEntry> snapshotEntries;

    auto hashRegularFile = [&](int dirFd, const char *name, const struct statx &expectedStx,
                               QByteArray *digestOut, quint64 *verifiedInoOut, quint64 *verifiedSizeOut) -> bool {
        const quint64 expectedSize = static_cast<quint64>(expectedStx.stx_size);
        if (totalBytes > kMaxSnapshotHashedBytes || expectedSize > (kMaxSnapshotHashedBytes - totalBytes)) {
            return fail(ContainmentFailureKind::ContainmentViolation,
                        trMsg("Rozmiar poddrzewa przekracza bezpieczny limit weryfikacji snapshotu.",
                              "Directory subtree size exceeds strict snapshot verification limit."));
        }
        ScopedFd fileFd(::openat(dirFd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (!fileFd.isValid()) {
            return fail(ContainmentFailureKind::ContainmentViolation,
                        trMsg("Nie można bezpiecznie otworzyć pliku podczas weryfikacji snapshotu.",
                              "Could not safely open file during strict snapshot verification."));
        }
        struct stat preSt {};
        if (::fstat(fileFd.fd, &preSt) != 0
            || !S_ISREG(preSt.st_mode)
            || preSt.st_nlink != 1
            || static_cast<quint64>(preSt.st_dev) != rootIdentity.stDev
            || static_cast<quint64>(preSt.st_ino) != expectedStx.stx_ino
            || static_cast<quint64>(preSt.st_size) != expectedSize) {
            return fail(ContainmentFailureKind::ContainmentViolation,
                        trMsg("Wykryto zmianę tożsamości lub metadanych pliku podczas weryfikacji snapshotu.",
                              "File identity or metadata changed during strict snapshot verification."));
        }

        QCryptographicHash fileHasher(QCryptographicHash::Sha256);
        char buf[65536];
        quint64 readTotal = 0;
        while (true) {
            if (cancelled.load(std::memory_order_acquire)) {
                return fail(ContainmentFailureKind::Cancelled,
                            trMsg("Operacja anulowana.", "Operation cancelled."));
            }
            const ssize_t n = ::read(fileFd.fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return fail(ContainmentFailureKind::ContainmentViolation,
                            trMsg("Błąd odczytu pliku podczas weryfikacji snapshotu.",
                                  "File read error during strict snapshot verification."));
            }
            if (n == 0) {
                break;
            }
            const quint64 chunk = static_cast<quint64>(n);
            if (chunk > expectedSize - readTotal) {
                return fail(ContainmentFailureKind::ContainmentViolation,
                            trMsg("Plik uległ zmianie podczas odczytu snapshotu.",
                                  "File changed while reading strict snapshot."));
            }
            fileHasher.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
            readTotal += chunk;
        }
        if (readTotal != expectedSize) {
            return fail(ContainmentFailureKind::ContainmentViolation,
                        trMsg("Rozmiar odczytanego pliku zmienił się podczas weryfikacji snapshotu.",
                              "File size changed during strict snapshot read."));
        }

        struct stat postSt {};
        if (::fstat(fileFd.fd, &postSt) != 0
            || !S_ISREG(postSt.st_mode)
            || postSt.st_nlink != 1
            || postSt.st_dev != preSt.st_dev
            || postSt.st_ino != preSt.st_ino
            || postSt.st_size != preSt.st_size
            || postSt.st_mtim.tv_sec != preSt.st_mtim.tv_sec
            || postSt.st_mtim.tv_nsec != preSt.st_mtim.tv_nsec
            || postSt.st_ctim.tv_sec != preSt.st_ctim.tv_sec
            || postSt.st_ctim.tv_nsec != preSt.st_ctim.tv_nsec) {
            return fail(ContainmentFailureKind::ContainmentViolation,
                        trMsg("Plik został zmodyfikowany w trakcie weryfikacji snapshotu.",
                              "File was modified during strict snapshot verification."));
        }

        if (hooks && hooks->snapshotFilesHashedCounter) {
            hooks->snapshotFilesHashedCounter->fetch_add(1, std::memory_order_relaxed);
        }

        *digestOut = fileHasher.result();
        *verifiedInoOut = static_cast<quint64>(postSt.st_ino);
        *verifiedSizeOut = static_cast<quint64>(postSt.st_size);
        return true;
    };

    while (!dirStack.empty()) {
        if (cancelled.load(std::memory_order_acquire)) {
            return fail(ContainmentFailureKind::Cancelled,
                        trMsg("Operacja anulowana.", "Operation cancelled."));
        }

        DirFrame currentFrame = std::move(dirStack.back());
        dirStack.pop_back();

        DIR *dirStream = ::fdopendir(currentFrame.fd.fd);
        if (!dirStream) {
            return fail(ContainmentFailureKind::ContainmentViolation,
                        trMsg("Nie można bezpiecznie odczytać zawartości katalogu.",
                              "Could not safely inspect directory contents."));
        }
        // fdopendir takes ownership of currentFrame.fd.fd
        currentFrame.fd.fd = -1;
        const int dfd = ::dirfd(dirStream);

        struct dirent *entry = nullptr;
        while ((errno = 0, entry = ::readdir(dirStream)) != nullptr) {
            if (cancelled.load(std::memory_order_acquire)) {
                ::closedir(dirStream);
                return fail(ContainmentFailureKind::Cancelled,
                            trMsg("Operacja anulowana.", "Operation cancelled."));
            }
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            if (checkStrictLimits
                && (regularCount + subdirCount) >= kMaxSnapshotEntries) {
                ::closedir(dirStream);
                return fail(ContainmentFailureKind::ContainmentViolation,
                            trMsg("Liczba wpisów poddrzewa przekracza bezpieczny limit weryfikacji snapshotu.",
                                  "Directory entry count exceeds strict snapshot verification limit."));
            }

            const QByteArray entryName(entry->d_name);
            const QByteArray relEntryPath = currentFrame.relPath.isEmpty()
                ? entryName
                : (currentFrame.relPath + '/' + entryName);

            struct statx childStx {};
            const unsigned int childMask =
                STATX_BASIC_STATS | STATX_MNT_ID | STATX_MNT_ID_UNIQUE | STATX_SUBVOL;
            if (::syscall(SYS_statx, dfd, entry->d_name,
                          AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, childMask, &childStx) != 0) {
                ::closedir(dirStream);
                return fail(ContainmentFailureKind::ContainmentViolation,
                            trMsg("Nie udało się zweryfikować elementu wewnątrz katalogu — operacja zatrzymana.",
                                  "Could not inspect entry inside directory — blocked fail-closed."));
            }
            if (hooks && hooks->simulateExt4Filesystem) {
                childStx.stx_mask &= ~STATX_SUBVOL;
                childStx.stx_subvol = 0;
            }
            if (hooks && hooks->simulateExt4StatxMntIdUniqueMissing) {
                childStx.stx_mask &= ~STATX_MNT_ID_UNIQUE;
            }

            const unsigned int requiredBasic = STATX_TYPE | STATX_MODE | STATX_INO | STATX_NLINK;
            const bool hasBasic = (childStx.stx_mask & requiredBasic) == requiredBasic;
            const bool hasMnt = profile.requireUniqueMountId()
                ? ((childStx.stx_mask & STATX_MNT_ID_UNIQUE) != 0)
                : ((childStx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) != 0);
            if (!hasBasic || !hasMnt) {
                ::closedir(dirStream);
                return fail(ContainmentFailureKind::UnsupportedKernelFeature,
                            trMsg("Jądro nie zwróciło wymaganych metadanych statx dla zawartości katalogu.",
                                  "Kernel did not return required statx metadata for directory entry."));
            }

            const bool childUniqueMnt = (childStx.stx_mask & STATX_MNT_ID_UNIQUE) != 0;
            const bool childHasSubvol = (childStx.stx_mask & STATX_SUBVOL) != 0;
            const bool badSubvolRoot = profile.checkBtrfsSubvolRootInode()
                && childStx.stx_ino == BTRFS_FIRST_FREE_OBJECTID;
            if (childUniqueMnt != rootIdentity.hasUniqueMntId
                || childStx.stx_mnt_id != rootIdentity.mntId
                || childStx.stx_dev_major != rootIdentity.devMajor
                || childStx.stx_dev_minor != rootIdentity.devMinor
                || badSubvolRoot
                || childHasSubvol != rootIdentity.hasSubvol
                || (childHasSubvol && childStx.stx_subvol != rootIdentity.subvolId)) {
                ::closedir(dirStream);
                return fail(ContainmentFailureKind::MountOrSubvolumeBoundary,
                            trMsg("Przenoszony katalog zawiera punkt montowania lub zagnieżdżony podwolumin — operacja zablokowana.",
                                  "Directory contains a mount point or nested subvolume — blocked fail-closed."));
            }

            if (S_ISLNK(childStx.stx_mode)) {
                ::closedir(dirStream);
                return fail(ContainmentFailureKind::ContainmentViolation,
                            trMsg("Przenoszony katalog zawiera dowiązanie symboliczne — przenoszenie dowiązań jest zablokowane.",
                                  "Directory contains a symbolic link — symlink move is currently blocked."));
            }

            if (S_ISREG(childStx.stx_mode)) {
                if (childStx.stx_nlink > 1) {
                    ::closedir(dirStream);
                    return fail(ContainmentFailureKind::ContainmentViolation,
                                trMsg("Przenoszony katalog zawiera plik z wieloma dowiązaniami twardymi (hardlink) — operacja zablokowana.",
                                      "Directory contains a multi-link hardlinked file — blocked fail-closed."));
                }
                if (checkStrictLimits) {
                    const quint64 fileSize = static_cast<quint64>(childStx.stx_size);
                    if (totalBytes > kMaxSnapshotHashedBytes || fileSize > (kMaxSnapshotHashedBytes - totalBytes)) {
                        ::closedir(dirStream);
                        return fail(ContainmentFailureKind::ContainmentViolation,
                                    trMsg("Rozmiar poddrzewa przekracza bezpieczny limit weryfikacji snapshotu.",
                                          "Directory subtree size exceeds strict snapshot verification limit."));
                    }
                }
                if (computeContentDigest) {
                    QByteArray fileDigest;
                    quint64 verifiedIno = 0;
                    quint64 verifiedSize = 0;
                    if (!hashRegularFile(dfd, entry->d_name, childStx, &fileDigest, &verifiedIno, &verifiedSize)) {
                        ::closedir(dirStream);
                        return false;
                    }
                    snapshotEntries.push_back(SnapshotEntry{
                        relEntryPath,
                        static_cast<quint8>('F'),
                        verifiedIno,
                        verifiedSize,
                        fileDigest,
                    });
                }
                ++regularCount;
                totalBytes += static_cast<quint64>(childStx.stx_size);
                if (visitedObjectsOut) {
                    visitedObjectsOut->insert({rootIdentity.stDev, static_cast<quint64>(childStx.stx_ino)});
                }
                continue;
            }

            if (S_ISDIR(childStx.stx_mode)) {
                if (checkStrictLimits && currentFrame.depth + 1 > kMaxSnapshotDepth) {
                    ::closedir(dirStream);
                    return fail(ContainmentFailureKind::ContainmentViolation,
                                trMsg("Głębokość podkatalogów przekracza bezpieczny limit weryfikacji snapshotu.",
                                      "Directory nesting depth exceeds strict snapshot verification limit."));
                }
                int childErr = 0;
                ScopedFd childFd(openChildDirectoryNoCrossing(dfd, entry->d_name, &childErr, hooks));
                if (!childFd.isValid()) {
                    ::closedir(dirStream);
                    const bool mountBoundary = (childErr == EXDEV);
                    return fail(mountBoundary
                                    ? ContainmentFailureKind::MountOrSubvolumeBoundary
                                    : ContainmentFailureKind::ContainmentViolation,
                                mountBoundary
                                    ? trMsg("Przenoszony katalog zawiera punkt montowania lub podwolumin Btrfs — operacja zablokowana.",
                                            "Directory contains a mount point or Btrfs subvolume — blocked fail-closed.")
                                    : trMsg("Nie można bezpiecznie otworzyć podkatalogu bez podążania za dowiązaniami.",
                                            "Could not safely open subdirectory without crossing links or mounts."));
                }

                struct statx openedStx {};
                struct stat openedSt {};
                unsigned long openedMagic = 0;
                if (!queryFdMetadata(childFd.fd, &openedStx, &openedSt, &openedMagic, &childErr, hooks)) {
                    ::closedir(dirStream);
                    return fail(ContainmentFailureKind::ContainmentViolation,
                                trMsg("Nie udało się zweryfikować deskryptora podkatalogu.",
                                      "Could not verify subdirectory descriptor."));
                }
                if (openedMagic != profile.expectedMagic()) {
                    ::closedir(dirStream);
                    return fail(ContainmentFailureKind::MountOrSubvolumeBoundary,
                                trMsg("Podkatalog znajduje się poza obsługiwanym woluminem.",
                                      "Subdirectory lies outside the qualified volume."));
                }
                const LocalDirectoryIdentity openedId = LocalDirectoryIdentity::fromStat(openedStx, openedSt);
                const bool openedBadSubvolRoot = profile.checkBtrfsSubvolRootInode()
                    && openedId.ino == BTRFS_FIRST_FREE_OBJECTID;
                if (!openedId.valid || !openedId.sameMountScope(rootIdentity)
                    || openedId.ino != childStx.stx_ino
                    || openedBadSubvolRoot) {
                    ::closedir(dirStream);
                    return fail(ContainmentFailureKind::MountOrSubvolumeBoundary,
                                trMsg("Wykryto niezgodność topologii lub granicy montowania w podkatalogu.",
                                      "Subdirectory mount or subvolume boundary mismatch detected."));
                }
                if (profile.checkExt4DirectoryFlags()) {
                    ContainmentFailureKind subFlagFailure = ContainmentFailureKind::None;
                    QString subFlagReason;
                    if (!verifyExt4DirectorySafetyFlags(childFd.fd, openedId.ino, &subFlagFailure, &subFlagReason, hooks)) {
                        ::closedir(dirStream);
                        return fail(subFlagFailure, subFlagReason);
                    }
                }
                const QPair<quint64, quint64> dirKey(openedId.stDev, openedId.ino);
                if (!visitedDirs.contains(dirKey)) {
                    visitedDirs.insert(dirKey);
                    if (visitedObjectsOut) {
                        visitedObjectsOut->insert(dirKey);
                    }
                    if (computeContentDigest) {
                        snapshotEntries.push_back(SnapshotEntry{
                            relEntryPath,
                            static_cast<quint8>('D'),
                            openedId.ino,
                            0,
                            QByteArray(),
                        });
                    }
                    ++subdirCount;
                    dirStack.push_back(DirFrame{std::move(childFd), relEntryPath, currentFrame.depth + 1});
                }
                continue;
            }

            // FIFO, Unix socket, char device, block device, etc.
            ::closedir(dirStream);
            return fail(ContainmentFailureKind::ContainmentViolation,
                        trMsg("Przenoszony katalog zawiera plik specjalny (np. FIFO, gniazdo lub urządzenie) — operacja zablokowana.",
                              "Directory contains a special file (FIFO, socket, or device) — blocked fail-closed."));
        }
        const int readErr = errno;
        ::closedir(dirStream);
        if (readErr != 0) {
            return fail(ContainmentFailureKind::ContainmentViolation,
                        trMsg("Błąd odczytu listy wpisów katalogu podczas weryfikacji bezpieczeństwa.",
                              "Error reading directory entries during containment verification."));
        }
    }

    if (snapshotOut) {
        snapshotOut->scannedBytes = totalBytes;
        snapshotOut->scannedRegularFiles = regularCount;
        snapshotOut->scannedSubdirectories = subdirCount;
        if (computeContentDigest) {
            std::sort(snapshotEntries.begin(), snapshotEntries.end(),
                      [](const SnapshotEntry &a, const SnapshotEntry &b) {
                          return a.relPath < b.relPath;
                      });
            QCryptographicHash treeHasher(QCryptographicHash::Sha256);
            auto appendLe64 = [&](quint64 v) {
                const quint64 le = qToLittleEndian(v);
                treeHasher.addData(QByteArrayView(reinterpret_cast<const char *>(&le), sizeof(le)));
            };
            auto appendLe32 = [&](quint32 v) {
                const quint32 le = qToLittleEndian(v);
                treeHasher.addData(QByteArrayView(reinterpret_cast<const char *>(&le), sizeof(le)));
            };
            treeHasher.addData(QByteArrayView("THISPC-TREE-SNAP-V1", 19));
            appendLe64(static_cast<quint64>(snapshotEntries.size()));
            for (const SnapshotEntry &se : snapshotEntries) {
                treeHasher.addData(QByteArrayView(reinterpret_cast<const char *>(&se.type), 1));
                appendLe32(static_cast<quint32>(se.relPath.size()));
                treeHasher.addData(se.relPath);
                appendLe64(se.ino);
                appendLe64(se.size);
                appendLe32(static_cast<quint32>(se.sha256.size()));
                if (!se.sha256.isEmpty()) {
                    treeHasher.addData(se.sha256);
                }
            }
            appendLe64(totalBytes);
            appendLe64(static_cast<quint64>(regularCount));
            appendLe64(static_cast<quint64>(subdirCount));
            snapshotOut->treeDigest = treeHasher.result();
            snapshotOut->strictContentDigest = (snapshotOut->treeDigest.size() == 32);
        } else {
            snapshotOut->treeDigest.clear();
            snapshotOut->strictContentDigest = false;
        }
        snapshotOut->valid = true;
    }
    if (failureOut) *failureOut = ContainmentFailureKind::None;
    return true;
}

inline bool captureStrictRegularFileSnapshot(
    const QString &filePath,
    LocalFileHistorySnapshot *snapshotOut,
    QString *reasonOut = nullptr,
    const TestFaultHooks *hooks = nullptr,
    bool computeDigest = true)
{
    if (!snapshotOut) return false;
    *snapshotOut = {};

    const QFileInfo fi(filePath);
    const QString parentPath = fi.path();
    const QString fileName = fi.fileName();
    if (parentPath.isEmpty() || fileName.isEmpty() || fileName == QStringLiteral(".")
        || fileName == QStringLiteral("..")) {
        if (reasonOut) *reasonOut = QStringLiteral("Invalid file path for strict history snapshot.");
        return false;
    }

    int openErr = 0;
    ScopedFd parentFd(openDirectoryNoSymlinks(parentPath, &openErr, hooks));
    if (!parentFd.isValid()) {
        if (reasonOut) *reasonOut = QStringLiteral("Cannot open parent directory without symlinks (%1).").arg(openErr);
        return false;
    }

    const QByteArray leafBytes = QFile::encodeName(fileName);
    ScopedFd fileFd(::openat(parentFd.fd, leafBytes.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!fileFd.isValid()) {
        if (reasonOut) *reasonOut = QStringLiteral("Cannot open regular file without symlinks (%1).").arg(errno);
        return false;
    }

    struct statx stx1 {};
    struct stat st1 {};
    unsigned long magic1 = 0;
    int metaErr = 0;
    if (!queryFdMetadata(fileFd.fd, &stx1, &st1, &magic1, &metaErr, hooks)
        || !S_ISREG(st1.st_mode)) {
        if (reasonOut) *reasonOut = QStringLiteral("Target is not a regular file or metadata unavailable.");
        return false;
    }

    const LocalRegularFileIdentity id1 = LocalRegularFileIdentity::fromStat(stx1, st1);
    if (!id1.valid || id1.nlink != 1) {
        if (reasonOut) *reasonOut = QStringLiteral("Refused: file identity invalid or nlink != 1.");
        return false;
    }

    constexpr quint64 kMaxStrictFileBytes = 512ULL * 1024ULL * 1024ULL;
    if (computeDigest && id1.size > kMaxStrictFileBytes) {
        if (reasonOut) *reasonOut = QStringLiteral("Strict file history snapshot exceeds 512 MiB limit.");
        return false;
    }

    QByteArray digest;
    if (computeDigest) {
        QCryptographicHash hasher(QCryptographicHash::Sha256);
        QByteArray buf(256 * 1024, Qt::Uninitialized);
        quint64 totalRead = 0;
        for (;;) {
            const ssize_t n = ::read(fileFd.fd, buf.data(), static_cast<size_t>(buf.size()));
            if (n < 0) {
                if (errno == EINTR) continue;
                if (reasonOut) *reasonOut = QStringLiteral("Read error while hashing file (%1).").arg(errno);
                return false;
            }
            if (n == 0) break;
            totalRead += static_cast<quint64>(n);
            if (totalRead > kMaxStrictFileBytes) {
                if (reasonOut) *reasonOut = QStringLiteral("Strict file history snapshot exceeded 512 MiB during read.");
                return false;
            }
            hasher.addData(QByteArrayView(buf.constData(), n));
        }
        if (totalRead != id1.size) {
            if (reasonOut) *reasonOut = QStringLiteral("File size changed while hashing.");
            return false;
        }
        digest = hasher.result();
        if (digest.size() != 32) {
            if (reasonOut) *reasonOut = QStringLiteral("Invalid SHA-256 digest length.");
            return false;
        }
        if (hooks && hooks->snapshotFilesHashedCounter) {
            hooks->snapshotFilesHashedCounter->fetch_add(1, std::memory_order_relaxed);
        }
    }

    struct statx stx2 {};
    struct stat st2 {};
    unsigned long magic2 = 0;
    if (!queryFdMetadata(fileFd.fd, &stx2, &st2, &magic2, &metaErr, hooks)
        || !S_ISREG(st2.st_mode)) {
        if (reasonOut) *reasonOut = QStringLiteral("Post-read metadata verification failed.");
        return false;
    }
    const LocalRegularFileIdentity id2 = LocalRegularFileIdentity::fromStat(stx2, st2);
    if (!id2.valid || !id1.strictlyUnchangedForHistory(id2) || magic1 != magic2) {
        if (reasonOut) *reasonOut = QStringLiteral("File was modified concurrently during snapshot capture.");
        return false;
    }

    snapshotOut->identity = id2;
    if (hooks && hooks->simulateFileMoveMntIdChange) {
        snapshotOut->identity.mntId ^= 1ULL;
    }
    snapshotOut->size = id2.size;
    snapshotOut->fsMagic = magic2;
    snapshotOut->contentDigest = digest;
    snapshotOut->strictContentDigest = computeDigest;
    snapshotOut->valid = true;
    return true;
}

} // namespace LocalBtrfsMoveCommon
