/* SPDX-License-Identifier: MIT */
#pragma once
#include "aggregatepropertiesmodel.h"
#include <QSet>
#include <atomic>
#include <functional>
#include <memory>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <limits>

// Read-only descriptor-relative traversal. Memory and traversal are bounded;
// symlinks and foreign devices are never traversed. No per-entry result list.
class AggregatePropertiesScan {
public:
    using Cancel = std::shared_ptr<std::atomic_bool>;
    static AggregateSummary run(const AggregateSnapshot &snapshot, const Cancel &cancel,
                                quint64 maxEntries = 1000000,
                                std::function<void(const AggregateSummary &)> progress = {}) {
        AggregatePropertiesScan scan(cancel, maxEntries);
        scan.progress = std::move(progress);
        scan.result.unsupported = snapshot.omitted;
        scan.result.limited = snapshot.omitted > 0;
        for (const auto &root : snapshot.roots) {
            if (scan.stopped()) break;
            if (!root.url.isLocalFile()) { ++scan.result.unsupported; continue; }
            struct stat current {};
            if (!root.valid || ::lstat(root.path.constData(), &current) || !same(root.identity, current)) {
                ++scan.result.invalidated; continue;
            }
            scan.visit(AT_FDCWD, root.path.constData(), current.st_dev, 0, &root.identity);
        }
        // A changed selected root makes the result explicitly incomplete.
        for (const auto &root : snapshot.roots) {
            if (!root.valid) continue;
            struct stat current {};
            if (::lstat(root.path.constData(), &current) || !same(root.identity, current))
                ++scan.result.invalidated;
        }
        scan.result.cancelled = cancel->load();
        return scan.result;
    }
private:
    AggregatePropertiesScan(Cancel cancel, quint64 limit) : cancelled(std::move(cancel)), limit(limit) {}
    static bool same(const struct stat &a, const struct stat &b) {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode
            && a.st_size == b.st_size && a.st_blocks == b.st_blocks && a.st_mtim.tv_sec == b.st_mtim.tv_sec
            && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec
            && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
    }
    bool stopped() {
        if (cancelled->load()) return true;
        if (entries >= limit) { result.limited = true; return true; }
        return false;
    }
    void visit(int parent, const char *name, dev_t device, unsigned depth, const struct stat *expected = nullptr) {
        if (stopped()) return;
        struct stat st {};
        if (::fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) { ++result.skipped; return; }
        if (expected && !same(*expected, st)) { ++result.invalidated; return; }
        const auto key = QPair<quint64, quint64>(st.st_dev, st.st_ino);
        // Directory identity suppresses overlapping selected roots. Files count
        // each directory entry (hardlink aliases have logical size separately).
        if (S_ISDIR(st.st_mode) && directories.contains(key)) return;
        if (st.st_dev != device) { ++result.skipped; return; }
        ++entries;
        if (progress && entries % 256 == 0) progress(result);
        if (S_ISDIR(st.st_mode)) {
            if (depth >= 128) { result.limited = true; ++result.skipped; return; }
            const int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) { ++result.skipped; return; }
            struct stat opened {};
            if (::fstat(fd, &opened) || !same(st, opened)) { ::close(fd); ++result.invalidated; return; }
            DIR *dir = ::fdopendir(fd);
            if (!dir) { ::close(fd); ++result.skipped; return; }
            directories.insert(key);
            ++result.folders;
            while (!stopped()) {
                errno = 0;
                auto *entry = ::readdir(dir);
                if (!entry) { if (errno) ++result.skipped; break; }
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
                visit(fd, entry->d_name, device, depth + 1);
            }
            struct stat after {};
            if (::fstat(fd, &after) || !same(opened, after)) ++result.invalidated;
            ::closedir(dir);
        } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            // Directory-entry identity avoids a selected child counted again
            // after its parent. Unlike inode identity it preserves hardlinks.
            struct stat parentStat {};
            QByteArray pathKey;
            if (parent == AT_FDCWD) {
                const QByteArray path(name);
                const int slash = path.lastIndexOf('/');
                const QByteArray parentPath = slash == 0 ? QByteArray("/") : path.left(slash);
                if (::stat(parentPath.constData(), &parentStat)) { ++result.skipped; return; }
                pathKey = path.mid(slash + 1);
            } else {
                if (::fstat(parent, &parentStat)) { ++result.skipped; return; }
                pathKey = name;
            }
            pathKey.prepend(QByteArray::number(parentStat.st_dev) + ':' + QByteArray::number(parentStat.st_ino) + ':');
            if (files.contains(pathKey)) return;
            files.insert(pathKey);
            if (S_ISLNK(st.st_mode)) ++result.links;
            else {
                ++result.files;
                result.bytes += quint64(st.st_size);
                // Match Storage Tools: sum reported regular-file allocation once
                // per device/inode. Shared extents across inodes are not deduplicated.
                if (!allocatedInodes.contains(key)) {
                    allocatedInodes.insert(key);
                    const auto maximum = std::numeric_limits<quint64>::max();
                    if (st.st_blocks < 0 || quint64(st.st_blocks) > (maximum - result.allocatedBytes) / 512)
                        ++result.allocationUnavailable;
                    else {
                        result.allocatedBytes += quint64(st.st_blocks) * 512;
                        ++result.allocatedFiles;
                    }
                }
            }
        } else ++result.unsupported;
    }
    Cancel cancelled;
    quint64 limit, entries = 0;
    QSet<QPair<quint64, quint64>> directories;
    QSet<QByteArray> files;
    QSet<QPair<quint64, quint64>> allocatedInodes;
    AggregateSummary result;
    std::function<void(const AggregateSummary &)> progress;
};
