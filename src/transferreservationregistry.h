/*
 * Path reservation model for the 0.44 transfer queue (Stage 1).
 *
 * SCOPE OF THE GUARANTEE (read before relying on it)
 *   The registry coordinates ONLY the tasks that register with it, i.e. the
 *   transfers submitted to TransferQueueController. It does not protect
 *   against other ThisPC operations that bypass the scheduler (Split Sync,
 *   Rename, Batch Rename, archives, Trash/Delete, Create) nor against other
 *   processes. Revalidation and O_EXCL-style creation narrow, but do not
 *   eliminate, time-of-check/time-of-use windows. Nothing here is atomic.
 *
 * FAIL-CLOSED POLICY
 *   Whenever a path identity cannot be established with confidence the
 *   request becomes "imprecise" and is treated as conflicting with EVERY other
 *   reservation (it runs alone). Imprecise cases: non-local URLs, symlinks
 *   anywhere in the existing part of a path (leaf or ancestor), ".." in the
 *   input, regular files with more than one hard link, empty file names and
 *   unreadable metadata. Hard links inside directory trees and bind mounts of
 *   not-yet-existing paths cannot be detected and are NOT covered.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "transfertaskdescriptor.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <optional>

#include <sys/stat.h>

enum class ReservationMode {
    SharedRead,
    ExclusiveWrite,
};

struct ResourceClaim {
    QString key;  // case-folded absolute path (conservative on case-insensitive filesystems)
    ReservationMode mode = ReservationMode::SharedRead;
    bool exists = false;
    quint64 device = 0;
    quint64 inode = 0;

    bool operator==(const ResourceClaim &other) const
    {
        return key == other.key && mode == other.mode && exists == other.exists
            && device == other.device && inode == other.inode;
    }
};

struct ReservationRequest {
    bool valid = false;
    bool precise = false;
    QString imprecisionReason;
    QList<ResourceClaim> claims;

    bool operator==(const ReservationRequest &other) const
    {
        return valid == other.valid && precise == other.precise
            && claims == other.claims;
    }
};

namespace TransferReservation {

struct ResolvedPath {
    bool ok = false;
    QString key;
    bool exists = false;
    quint64 device = 0;
    quint64 inode = 0;
    QString problem;  // non-empty => imprecise
};

inline bool pathsOverlap(const QString &left, const QString &right)
{
    if (left == right) return true;
    auto isAncestor = [](const QString &ancestor, const QString &path) {
        if (ancestor == QLatin1String("/")) return true;
        return path.size() > ancestor.size()
            && path.startsWith(ancestor)
            && path.at(ancestor.size()) == QLatin1Char('/');
    };
    return isAncestor(left, right) || isAncestor(right, left);
}

inline ResolvedPath resolveLocal(const QUrl &url)
{
    ResolvedPath resolved;
    if (!url.isValid() || !url.isLocalFile()) {
        resolved.problem = QStringLiteral("not a local file URL");
        return resolved;
    }
    const QString raw = url.toLocalFile();
    if (raw.isEmpty() || !QDir::isAbsolutePath(raw)) {
        resolved.problem = QStringLiteral("path is empty or relative");
        return resolved;
    }
    if (raw.split(QLatin1Char('/')).contains(QStringLiteral(".."))) {
        resolved.problem = QStringLiteral("path contains '..' (identity ambiguous)");
        return resolved;
    }
    const QString clean = QDir::cleanPath(raw);

    // Deepest existing ancestor, found with lstat so that a broken symlink
    // counts as existing (and is then rejected below).
    QString existing = clean;
    struct stat info {};
    bool leafExists = false;
    while (true) {
        if (::lstat(QFile::encodeName(existing).constData(), &info) == 0) {
            leafExists = existing == clean;
            break;
        }
        if (existing == QLatin1String("/")) {
            resolved.problem = QStringLiteral("filesystem root is not accessible");
            return resolved;
        }
        const int slash = existing.lastIndexOf(QLatin1Char('/'));
        existing = slash <= 0 ? QStringLiteral("/") : existing.left(slash);
    }
    if (S_ISLNK(info.st_mode)) {
        resolved.problem = QStringLiteral("symbolic link in path");
        return resolved;
    }
    const QString canonical = QFileInfo(existing).canonicalFilePath();
    if (canonical.isEmpty() || canonical != existing) {
        resolved.problem = QStringLiteral("symbolic link component in path");
        return resolved;
    }
    if (leafExists && S_ISREG(info.st_mode) && info.st_nlink > 1) {
        resolved.problem = QStringLiteral("file has multiple hard links");
        return resolved;
    }

    resolved.ok = true;
    resolved.key = clean.toCaseFolded();
    resolved.exists = leafExists;
    if (leafExists) {
        resolved.device = static_cast<quint64>(info.st_dev);
        resolved.inode = static_cast<quint64>(info.st_ino);
    }
    return resolved;
}

inline ReservationRequest imprecise(const QString &reason)
{
    ReservationRequest request;
    request.valid = true;
    request.precise = false;
    request.imprecisionReason = reason;
    return request;
}

// Builds the claim set for a transfer. Sources are SharedRead for Copy and
// ExclusiveWrite for Move (a moved source disappears); every target is
// ExclusiveWrite. Claims cover whole subtrees.
inline ReservationRequest build(
    TransferOperationType type,
    const QList<QUrl> &sources,
    const QUrl &destination,
    const QList<QUrl> &targetUrls = {})
{
    ReservationRequest request;
    if (sources.isEmpty() || !destination.isValid() || destination.isEmpty()) {
        return request;  // valid == false
    }
    if (!targetUrls.isEmpty() && targetUrls.size() != sources.size()) {
        return request;
    }
    request.valid = true;
    request.precise = true;

    auto fail = [&request](const QString &reason) {
        request.precise = false;
        if (request.imprecisionReason.isEmpty()) request.imprecisionReason = reason;
    };

    for (qsizetype index = 0; index < sources.size(); ++index) {
        const QUrl &source = sources.at(index);
        const QString name = source.fileName();
        if (name.isEmpty()) {
            fail(QStringLiteral("source has no file name"));
            break;
        }
        const ResolvedPath src = resolveLocal(source);
        if (!src.ok) {
            fail(QStringLiteral("source: ") + src.problem);
        } else {
            ResourceClaim sourceClaim;
            sourceClaim.key = src.key;
            sourceClaim.mode = type == TransferOperationType::Move
                ? ReservationMode::ExclusiveWrite : ReservationMode::SharedRead;
            sourceClaim.exists = src.exists;
            sourceClaim.device = src.device;
            sourceClaim.inode = src.inode;
            request.claims.push_back(sourceClaim);
        }
        QUrl target;
        if (!targetUrls.isEmpty()) {
            target = targetUrls.at(index);
        } else {
            target = destination;
            target.setPath(
                QDir::cleanPath(destination.path() + QLatin1Char('/') + name),
                QUrl::DecodedMode);
        }
        const ResolvedPath dst = resolveLocal(target);
        if (!dst.ok) {
            fail(QStringLiteral("target: ") + dst.problem);
        } else {
            ResourceClaim targetClaim;
            targetClaim.key = dst.key;
            targetClaim.mode = ReservationMode::ExclusiveWrite;
            targetClaim.exists = dst.exists;
            targetClaim.device = dst.device;
            targetClaim.inode = dst.inode;
            request.claims.push_back(targetClaim);
        }
    }
    return request;
}

// True when the two requests must not run at the same time. Invalid and
// imprecise requests conflict with everything (fail closed).
inline bool conflicts(const ReservationRequest &left, const ReservationRequest &right)
{
    if (!left.valid || !right.valid || !left.precise || !right.precise) {
        return true;
    }
    for (const ResourceClaim &a : left.claims) {
        for (const ResourceClaim &b : right.claims) {
            if (a.mode == ReservationMode::SharedRead
                && b.mode == ReservationMode::SharedRead) {
                continue;
            }
            if (pathsOverlap(a.key, b.key)) return true;
            if (a.exists && b.exists && a.device == b.device && a.inode == b.inode
                && a.inode != 0) {
                return true;  // same existing object under different names
            }
        }
    }
    return false;
}

// Re-reads the filesystem and compares with the reservation computed at
// enqueue time. Returns an empty string when nothing relevant changed, else a
// diagnostic. A previously absent target that now exists (or the reverse), a
// replaced source/target (different inode), or any new symlink/hard-link
// ambiguity invalidates the task: an old overwrite decision is never reused.
inline QString revalidate(
    const ReservationRequest &original,
    TransferOperationType type,
    const QList<QUrl> &sources,
    const QUrl &destination,
    const QList<QUrl> &targetUrls = {})
{
    const ReservationRequest current = build(type, sources, destination, targetUrls);
    if (!current.valid) {
        return QStringLiteral("request is no longer valid");
    }
    if (original.precise && !current.precise) {
        return QStringLiteral("path identity became ambiguous: ") + current.imprecisionReason;
    }
    if (!(current == original)) {
        return QStringLiteral("source or target identity changed while the task was waiting");
    }
    return {};
}

}  // namespace TransferReservation

// Process-local registry of active reservations. Not thread-safe by design:
// it is owned and used only by TransferQueueController on the GUI thread.
class TransferReservationRegistry
{
public:
    using Holder = quint64;

    struct Conflict {
        Holder holder = 0;
    };

    // First (lowest id, deterministic) holder that conflicts with `request`.
    std::optional<Conflict> findConflict(const ReservationRequest &request) const
    {
        QList<Holder> ids = m_holders.keys();
        std::sort(ids.begin(), ids.end());
        for (Holder id : std::as_const(ids)) {
            if (TransferReservation::conflicts(request, m_holders.value(id))) {
                return Conflict{id};
            }
        }
        return std::nullopt;
    }

    bool tryAcquire(Holder holder, const ReservationRequest &request)
    {
        if (holder == 0 || m_holders.contains(holder) || findConflict(request)) {
            return false;
        }
        m_holders.insert(holder, request);
        return true;
    }

    // Idempotent: releasing an unknown holder is a no-op returning false.
    bool release(Holder holder) { return m_holders.remove(holder) > 0; }

    bool holds(Holder holder) const { return m_holders.contains(holder); }
    int holderCount() const { return static_cast<int>(m_holders.size()); }

private:
    QHash<Holder, ReservationRequest> m_holders;
};
