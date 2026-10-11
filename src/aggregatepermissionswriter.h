/* SPDX-License-Identifier: MIT */
#pragma once
#include "aggregatepermissionsmodel.h"
#include <functional>
#include <cstring>

struct AggregatePermissionResult {
    QUrl url;
    bool success = false, changed = false;
    QString message;
};
class AggregatePermissionsWriter {
public:
    using Cancel = std::shared_ptr<std::atomic_bool>;
    // Seam for deterministic failures/races; production always uses descriptor chmod.
    struct Hooks {
        std::function<void(int)> beforeValidation;
        std::function<int(int, mode_t)> chmod = [](int fd, mode_t mode) {
#ifdef SYS_fchmodat2
            return int(::syscall(SYS_fchmodat2, fd, "", mode, AT_EMPTY_PATH));
#else
            Q_UNUSED(fd); Q_UNUSED(mode); errno = ENOTSUP; return -1;
#endif
        };
    };
    static QList<AggregatePermissionResult> apply(const AggregatePermissionsModel &model,
        AggregatePermissionEdit edit, const Cancel &cancel, const Hooks &hooks) {
        QList<AggregatePermissionResult> results;
        QSet<QPair<quint64, quint64>> written;
        for (int i = 0; i < model.items.size(); ++i) {
            const auto &item = model.items[i];
            AggregatePermissionResult out; out.url = item.root.url;
            if (cancel->load()) out.message = QStringLiteral("Cancelled; not attempted");
            else if (!edit.valid()) out.message = QStringLiteral("Invalid rwx masks");
            else if (!edit.changeMask) { out.success = true; out.message = QStringLiteral("Unchanged"); }
            else if (!model.allEditable()) out.message = QStringLiteral("Selection contains blocked items; no writes: ") + item.reason;
            else {
                if (hooks.beforeValidation) hooks.beforeValidation(i);
                const auto key = QPair<quint64, quint64>(item.root.identity.st_dev, item.root.identity.st_ino);
                if (written.contains(key)) {
                    struct stat now {};
                    if (::lstat(item.root.path.constData(), &now) || now.st_dev != item.root.identity.st_dev
                        || now.st_ino != item.root.identity.st_ino || (now.st_mode & S_IFMT) != (item.root.identity.st_mode & S_IFMT))
                        out.message = QStringLiteral("Duplicate selected alias changed; reopen Properties");
                    else { out.success = true; out.message = QStringLiteral("Same selected inode already applied"); }
                }
                else writeOne(item, edit, cancel, hooks, out);
                if (out.success) written.insert(key);
            }
            results.append(out);
        }
        return results;
    }
    static QList<AggregatePermissionResult> apply(const AggregatePermissionsModel &model,
        AggregatePermissionEdit edit, const Cancel &cancel) { return apply(model, edit, cancel, Hooks{}); }
private:
    static void writeOne(const AggregatePermissionItem &item, AggregatePermissionEdit edit,
        const Cancel &cancel, const Hooks &hooks, AggregatePermissionResult &out) {
        const auto &root = item.root;
        const int fd = ::open(root.path.constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { out.message = QString::fromLocal8Bit(strerror(errno)); return; }
        struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
        struct stat opened {}, path {}, parent {};
        struct statvfs fs {};
        if (::fstat(fd, &opened) || ::lstat(root.path.constData(), &path)
            || ::stat(AggregatePermissionsModel::parentPath(root).constData(), &parent)
            || !AggregatePermissionsModel::same(root.identity, opened)
            || !AggregatePermissionsModel::same(opened, path)
            || parent.st_dev != item.parent.st_dev || parent.st_ino != item.parent.st_ino
            || QFileInfo(QString::fromLocal8Bit(AggregatePermissionsModel::parentPath(root))).canonicalFilePath() != item.canonicalParent
            || AggregatePermissionsModel::mountId(fd) != item.mountId
            || AggregatePermissionsModel::mountRecord(item.mountId) != item.mountRecord) {
            out.message = QStringLiteral("Identity, path or mount changed; write blocked"); return;
        }
        if (::fstatvfs(fd, &fs) || (fs.f_flag & ST_RDONLY)) { out.message = QStringLiteral("Read-only or unverifiable mount"); return; }
        const mode_t requested = edit.merged(opened.st_mode);
        if (requested == (opened.st_mode & 07777)) { out.success = true; out.message = QStringLiteral("Unchanged"); return; }
        // Last no-follow path check immediately before descriptor mutation. A
        // concurrent rename after this check can still move the pinned inode.
        if (cancel->load() || ::lstat(root.path.constData(), &path)
            || !AggregatePermissionsModel::same(opened, path)
            || AggregatePermissionsModel::mountRecord(item.mountId) != item.mountRecord) {
            out.message = QStringLiteral("Cancelled or changed before write"); return;
        }
        if (hooks.chmod(fd, requested)) { out.message = QString::fromLocal8Bit(strerror(errno)); return; }
        out.changed = true;
        struct stat after {};
        if (::fstat(fd, &after) || (after.st_mode & 07777) != requested
            || ::lstat(root.path.constData(), &path) || !AggregatePermissionsModel::same(after, path)
            || AggregatePermissionsModel::mountRecord(item.mountId) != item.mountRecord) {
            out.message = QStringLiteral("Write occurred but verification failed; reopen Properties"); return;
        }
        out.success = true; out.message = QStringLiteral("Applied and verified");
    }
};
