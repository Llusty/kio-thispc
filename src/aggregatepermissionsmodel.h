/* SPDX-License-Identifier: MIT */
#pragma once
#include "aggregatepropertiesmodel.h"
#include <QStorageInfo>
#include <QSet>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/statvfs.h>
#include <atomic>
#include <memory>

struct AggregatePermissionEdit {
    mode_t changeMask = 0, valueMask = 0;
    bool valid() const { return !(changeMask & ~0777) && !(valueMask & ~changeMask); }
    mode_t merged(mode_t current) const { return (current & 07777 & ~changeMask) | (valueMask & changeMask); }
};
struct AggregatePermissionItem {
    AggregateRoot root;
    struct stat parent {};
    quint64 mountId = 0;
    QByteArray mountRecord;
    QString canonicalParent;
    QString reason;
    bool editable() const { return reason.isEmpty(); }
};
class AggregatePermissionsModel {
public:
    QList<AggregatePermissionItem> items;
    bool allEditable() const {
        if (items.isEmpty()) return false;
        for (const auto &item : items) if (!item.editable()) return false;
        return true;
    }
    // -1 means mixed, 0 unset, 1 set. This is independent of the pending edit.
    int currentBit(mode_t bit) const {
        int value = -2;
        for (const auto &item : items) {
            if (!item.root.valid) return -1;
            const int next = bool(item.root.identity.st_mode & bit);
            if (value == -2) value = next;
            else if (value != next) return -1;
        }
        return value == -2 ? -1 : value;
    }
    static QByteArray mountRecord(quint64 id) {
        QFile f(QStringLiteral("/proc/self/mountinfo"));
        if (!f.open(QIODevice::ReadOnly)) return {};
        for (const auto &line : f.readAll().split('\n'))
            if (line.startsWith(QByteArray(QByteArray::number(id) + ' '))) return line;
        return {};
    }
    static quint64 mountId(int fd) {
        struct statx st {};
        if (::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &st)
            || !(st.stx_mask & STATX_MNT_ID)) return 0;
        return st.stx_mnt_id;
    }
    static bool same(const struct stat &a, const struct stat &b) {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode
            && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
    }
    static QByteArray parentPath(const AggregateRoot &root) {
        return QFile::encodeName(QFileInfo(root.url.toLocalFile()).absolutePath());
    }
    static AggregatePermissionsModel capture(const QList<QUrl> &urls) {
        AggregatePermissionsModel result;
        AggregateSnapshot snapshot;
        for (const auto &url : urls) {
            if (snapshot.roots.size() >= 4096) { ++snapshot.omitted; continue; }
            AggregateRoot root; root.url = url;
            // A file://host URL is ambiguous (UNC/KIO); never probe it with
            // local permission syscalls, even when QUrl calls it local.
            if (url.isLocalFile() && url.host().isEmpty()) root = AggregateSnapshot::capture({url}).roots.first();
            snapshot.roots.append(root);
        }
        for (const auto &root : snapshot.roots) {
            AggregatePermissionItem item; item.root = root;
            if (!root.url.isLocalFile() || !root.url.host().isEmpty()) item.reason = QStringLiteral("Unsupported backend (local POSIX only)");
            else if (!root.valid) item.reason = QStringLiteral("Missing or unreadable identity");
            else if (!S_ISREG(root.identity.st_mode) && !S_ISDIR(root.identity.st_mode))
                item.reason = QStringLiteral("Symlink or unsupported entry (no target access)");
            else {
                QStorageInfo storage(root.url.toLocalFile());
                const auto fs = storage.fileSystemType();
                if (!storage.isValid() || !storage.isReady()) item.reason = QStringLiteral("Mount unavailable");
                else if (storage.isReadOnly()) item.reason = QStringLiteral("Read-only mount");
                else if (fs != "btrfs" && fs != "ext4" && fs != "ext3" && fs != "ext2"
                         && fs != "xfs" && fs != "tmpfs") item.reason = QStringLiteral("Unverified POSIX filesystem: ") + QString::fromLatin1(fs);
                else if (root.identity.st_uid != ::geteuid()) item.reason = QStringLiteral("Not owned by current user; no bulk elevation");
                else {
                    const int fd = ::open(root.path.constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
                    struct stat opened {};
                    if (fd < 0 || ::fstat(fd, &opened) || !same(root.identity, opened)
                        || ::stat(parentPath(root).constData(), &item.parent)) item.reason = QStringLiteral("Cannot pin identity");
                    else {
                        item.canonicalParent = QFileInfo(QString::fromLocal8Bit(parentPath(root))).canonicalFilePath();
                        item.mountId = mountId(fd);
                        item.mountRecord = mountRecord(item.mountId);
                        if (item.canonicalParent.isEmpty() || !item.mountId || item.mountRecord.isEmpty()) item.reason = QStringLiteral("Cannot identify mount");
                    }
                    if (fd >= 0) ::close(fd);
                }
            }
            result.items.append(item);
        }
        if (snapshot.omitted) { AggregatePermissionItem item; item.reason = QStringLiteral("Selection exceeds safety limit"); result.items.append(item); }
        return result;
    }
};
