/*
 * Read-only capability and identity snapshot for file/folder Properties.
 *
 * Local paths are inspected with no-follow syscalls. Remote KIO URLs are
 * never converted to local paths and can only gain capabilities explicitly
 * advertised by a KIO UDSEntry.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <KIO/UDSEntry>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QString>
#include <QUrl>

#include <cerrno>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/xattr.h>
#endif

enum class PropertiesCapabilityState {
    Unknown,
    Supported,
    Unsupported,
    PermissionDenied,
    ReadOnly
};

enum class PropertiesEntryKind {
    RegularFile,
    Directory,
    SymbolicLink,
    Other,
    Missing,
    Unknown
};

enum class PropertiesSymlinkPolicy {
    EntryNoFollow,
    NotSymlink,
    Unknown
};

enum class PropertiesTargetStatus {
    NotApplicable,
    Present,
    Broken,
    Unknown
};

enum class PropertiesXattrProbe {
    Unknown,
    SupportedEmpty,
    SupportedPresent,
    Unsupported,
    PermissionDenied,
    Error
};

enum class PropertiesRevalidationResult {
    SameTarget,
    Missing,
    Replaced,
    Unsupported,
    Unknown
};

struct PropertiesObjectIdentity
{
    bool valid = false;
    quint64 device = 0;
    quint64 inode = 0;
    quint32 type = 0;

    bool operator==(const PropertiesObjectIdentity &other) const
    {
        return valid && other.valid
            && device == other.device
            && inode == other.inode
            && type == other.type;
    }
};

struct PropertiesTargetCapabilities
{
    QUrl requestedUrl;
    QUrl normalizedUrl;
    QString backendScheme;
    QString fileSystemType;

    bool isLocal = false;
    bool isRemote = false;
    PropertiesEntryKind entryKind = PropertiesEntryKind::Unknown;

    PropertiesCapabilityState posixModeReadable = PropertiesCapabilityState::Unknown;
    PropertiesCapabilityState posixModeEditable = PropertiesCapabilityState::Unknown;
    PropertiesCapabilityState xattrReadable = PropertiesCapabilityState::Unknown;
    PropertiesCapabilityState userXattrEditable = PropertiesCapabilityState::Unknown;
    PropertiesCapabilityState protectedXattrEditable = PropertiesCapabilityState::Unsupported;
    PropertiesCapabilityState nativeHiddenReadable = PropertiesCapabilityState::Unknown;
    PropertiesCapabilityState nativeHiddenEditable = PropertiesCapabilityState::Unknown;
    PropertiesCapabilityState dotNameHiddenApplicable = PropertiesCapabilityState::Unknown;
    PropertiesXattrProbe xattrProbe = PropertiesXattrProbe::Unknown;

    bool dotNameHidden = false;
    bool nativeHidden = false;
    PropertiesSymlinkPolicy symlinkPolicy = PropertiesSymlinkPolicy::Unknown;
    PropertiesTargetStatus symlinkTargetStatus = PropertiesTargetStatus::NotApplicable;
    PropertiesObjectIdentity entryIdentity;
    PropertiesObjectIdentity targetIdentity;
};

class PropertiesCapabilityResolver
{
public:
    struct LocalSyscalls {
        std::function<int(const char *, struct stat *)> lstatFn =
            [](const char *path, struct stat *value) { return ::lstat(path, value); };
        std::function<int(const char *, struct stat *)> statFn =
            [](const char *path, struct stat *value) { return ::stat(path, value); };
        std::function<int(const char *, int)> accessFn =
            [](const char *path, int mode) { return ::access(path, mode); };
        std::function<ssize_t(const char *, char *, size_t)> listXattrFn =
            [](const char *path, char *list, size_t size) -> ssize_t {
#ifdef __linux__
                return ::llistxattr(path, list, size);
#else
                Q_UNUSED(path);
                Q_UNUSED(list);
                Q_UNUSED(size);
                errno = ENOTSUP;
                return -1;
#endif
            };
    };

    static PropertiesTargetCapabilities resolve(
        const QUrl &url,
        const LocalSyscalls &syscalls)
    {
        PropertiesTargetCapabilities result;
        result.requestedUrl = url;
        result.backendScheme = url.scheme().toLower();

        if (!url.isLocalFile()) {
            result.normalizedUrl = url.adjusted(QUrl::NormalizePathSegments);
            result.isRemote = true;
            return result;
        }

        const QString path = QDir::cleanPath(QFileInfo(url.toLocalFile()).absoluteFilePath());
        result.normalizedUrl = QUrl::fromLocalFile(path);
        result.backendScheme = QStringLiteral("file");
        result.isLocal = true;
        result.dotNameHidden = QFileInfo(path).fileName().startsWith(QLatin1Char('.'));
        result.dotNameHiddenApplicable = PropertiesCapabilityState::Supported;
        result.nativeHiddenReadable = PropertiesCapabilityState::Unsupported;
        result.nativeHiddenEditable = PropertiesCapabilityState::Unsupported;

        const QByteArray encoded = QFile::encodeName(path);
        struct stat entry {};
        if (syscalls.lstatFn(encoded.constData(), &entry) != 0) {
            result.entryKind = errno == ENOENT || errno == ENOTDIR
                ? PropertiesEntryKind::Missing : PropertiesEntryKind::Unknown;
            result.symlinkPolicy = PropertiesSymlinkPolicy::Unknown;
            return result;
        }

        result.entryIdentity = identity(entry);
        result.entryKind = kind(entry.st_mode);
        result.symlinkPolicy = S_ISLNK(entry.st_mode)
            ? PropertiesSymlinkPolicy::EntryNoFollow
            : PropertiesSymlinkPolicy::NotSymlink;
        result.posixModeReadable = PropertiesCapabilityState::Supported;

        QStorageInfo storage(path);
        if (storage.isValid()) {
            result.fileSystemType = QString::fromLatin1(storage.fileSystemType()).toLower();
        }
        const bool readOnly = storage.isValid() && storage.isReadOnly();
        const bool symlink = S_ISLNK(entry.st_mode);
        if (symlink) {
            // Linux chmod follows symlinks and fchmodat(AT_SYMLINK_NOFOLLOW)
            // is not a portable/useful way to change a symlink's own mode.
            result.posixModeEditable = PropertiesCapabilityState::Unsupported;
            result.userXattrEditable = PropertiesCapabilityState::Unsupported;
            struct stat target {};
            if (syscalls.statFn(encoded.constData(), &target) == 0) {
                result.symlinkTargetStatus = PropertiesTargetStatus::Present;
                result.targetIdentity = identity(target);
            } else if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) {
                result.symlinkTargetStatus = PropertiesTargetStatus::Broken;
            } else {
                result.symlinkTargetStatus = PropertiesTargetStatus::Unknown;
            }
        } else if (readOnly) {
            result.posixModeEditable = PropertiesCapabilityState::ReadOnly;
        } else if (syscalls.accessFn(encoded.constData(), W_OK) == 0) {
            result.posixModeEditable = PropertiesCapabilityState::Supported;
        } else {
            result.posixModeEditable = PropertiesCapabilityState::PermissionDenied;
        }

        errno = 0;
        const ssize_t xattrSize = syscalls.listXattrFn(encoded.constData(), nullptr, 0);
        if (xattrSize >= 0) {
            result.xattrReadable = PropertiesCapabilityState::Supported;
            result.xattrProbe = xattrSize == 0
                ? PropertiesXattrProbe::SupportedEmpty
                : PropertiesXattrProbe::SupportedPresent;
            if (!symlink) {
                if (readOnly) {
                    result.userXattrEditable = PropertiesCapabilityState::ReadOnly;
                } else if (syscalls.accessFn(encoded.constData(), W_OK) == 0) {
                    result.userXattrEditable = PropertiesCapabilityState::Supported;
                } else {
                    result.userXattrEditable = PropertiesCapabilityState::PermissionDenied;
                }
            }
        } else if (errno == ENOTSUP
#if EOPNOTSUPP != ENOTSUP
                   || errno == EOPNOTSUPP
#endif
        ) {
            result.xattrReadable = PropertiesCapabilityState::Unsupported;
            result.userXattrEditable = PropertiesCapabilityState::Unsupported;
            result.xattrProbe = PropertiesXattrProbe::Unsupported;
        } else if (errno == EACCES || errno == EPERM) {
            result.xattrReadable = PropertiesCapabilityState::PermissionDenied;
            result.userXattrEditable = PropertiesCapabilityState::PermissionDenied;
            result.xattrProbe = PropertiesXattrProbe::PermissionDenied;
        } else {
            result.xattrProbe = PropertiesXattrProbe::Error;
        }
        return result;
    }

    static PropertiesTargetCapabilities resolve(const QUrl &url)
    {
        return resolve(url, LocalSyscalls {});
    }

    static void applyKioEntry(PropertiesTargetCapabilities *result,
                              const KIO::UDSEntry &entry)
    {
        if (!result || result->isLocal) return;
        if (entry.contains(KIO::UDSEntry::UDS_ACCESS)) {
            result->posixModeReadable = PropertiesCapabilityState::Supported;
        }
        if (entry.contains(KIO::UDSEntry::UDS_HIDDEN)) {
            result->nativeHiddenReadable = PropertiesCapabilityState::Supported;
            result->nativeHidden = entry.numberValue(KIO::UDSEntry::UDS_HIDDEN) != 0;
        }
        if (entry.contains(KIO::UDSEntry::UDS_LINK_DEST)) {
            result->entryKind = PropertiesEntryKind::SymbolicLink;
            result->symlinkPolicy = PropertiesSymlinkPolicy::EntryNoFollow;
            result->symlinkTargetStatus = PropertiesTargetStatus::Unknown;
        } else if (entry.contains(KIO::UDSEntry::UDS_FILE_TYPE)) {
            result->entryKind = kind(static_cast<mode_t>(
                entry.numberValue(KIO::UDSEntry::UDS_FILE_TYPE)));
            result->symlinkPolicy = PropertiesSymlinkPolicy::NotSymlink;
        }
        // KIO has no general xattr or native-hidden mutation capability API.
        result->posixModeEditable = PropertiesCapabilityState::Unknown;
        result->xattrReadable = PropertiesCapabilityState::Unknown;
        result->userXattrEditable = PropertiesCapabilityState::Unknown;
        result->nativeHiddenEditable = PropertiesCapabilityState::Unknown;
        result->dotNameHiddenApplicable = PropertiesCapabilityState::Unsupported;
    }

    static PropertiesRevalidationResult revalidate(
        const PropertiesTargetCapabilities &snapshot,
        const LocalSyscalls &syscalls)
    {
        if (!snapshot.isLocal || !snapshot.entryIdentity.valid) {
            return snapshot.isRemote
                ? PropertiesRevalidationResult::Unsupported
                : PropertiesRevalidationResult::Unknown;
        }
        const QByteArray path = QFile::encodeName(snapshot.normalizedUrl.toLocalFile());
        struct stat current {};
        if (syscalls.lstatFn(path.constData(), &current) != 0) {
            return errno == ENOENT || errno == ENOTDIR
                ? PropertiesRevalidationResult::Missing
                : PropertiesRevalidationResult::Unknown;
        }
        return snapshot.entryIdentity == identity(current)
            ? PropertiesRevalidationResult::SameTarget
            : PropertiesRevalidationResult::Replaced;
    }

    static PropertiesRevalidationResult revalidate(
        const PropertiesTargetCapabilities &snapshot)
    {
        return revalidate(snapshot, LocalSyscalls {});
    }

private:
    static PropertiesEntryKind kind(mode_t mode)
    {
        if (S_ISREG(mode)) return PropertiesEntryKind::RegularFile;
        if (S_ISDIR(mode)) return PropertiesEntryKind::Directory;
        if (S_ISLNK(mode)) return PropertiesEntryKind::SymbolicLink;
        return PropertiesEntryKind::Other;
    }

    static PropertiesObjectIdentity identity(const struct stat &value)
    {
        return {true,
                static_cast<quint64>(value.st_dev),
                static_cast<quint64>(value.st_ino),
                static_cast<quint32>(value.st_mode & S_IFMT)};
    }
};
