/* Local-only native libacl provider. It deliberately has no KIO dependency. */
#pragma once

#include "acldata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <acl/libacl.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

namespace AclFsDetail {

struct UniqueFd
{
    int fd = -1;
    UniqueFd() = default;
    explicit UniqueFd(int f) : fd(f) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&other) noexcept : fd(other.fd) { other.fd = -1; }
    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other) {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    bool valid() const { return fd >= 0; }
    void reset(int newFd = -1)
    {
        if (fd >= 0) {
            const int saved = errno;
            ::close(fd);
            errno = saved;
        }
        fd = newFd;
    }
};

inline QByteArray procFdPath(int fd)
{
    return QByteArrayLiteral("/proc/self/fd/") + QByteArray::number(fd);
}

// SEC-046-11: Component-by-component symlink-free open anchored from "/".
// Uses O_PATH | O_NOFOLLOW | O_CLOEXEC so:
// - Ancestor symlinks cannot redirect path traversal (ACL10).
// - Leaf symlinks are never followed (ACL5/ACL6).
// - FIFOs / devices / sockets never block on open() and are rejected via fstat() (ACL7/ACL8).
// - Owned unreadable (mode 0000) regular files and directories still open safely without EACCES (ACL4).
inline UniqueFd openAnchoredNoSymlinks(
    const QString &absPath,
    struct stat *outStat,
    int *outErrno)
{
    if (outErrno) *outErrno = 0;
    const QString clean = QDir::cleanPath(absPath);
    if (clean.isEmpty() || !clean.startsWith(QLatin1Char('/'))) {
        if (outErrno) *outErrno = EINVAL;
        return {};
    }

    constexpr int kAncFlags = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    constexpr int kLeafFlags = O_PATH | O_NOFOLLOW | O_CLOEXEC;

    UniqueFd cur(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!cur.valid()) {
        if (outErrno) *outErrno = errno;
        return {};
    }

    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QByteArray comp = QFile::encodeName(parts.at(i));
        const bool isLeaf = (i + 1 == parts.size());
        UniqueFd next(::openat(cur.fd, comp.constData(), isLeaf ? kLeafFlags : kAncFlags));
        if (!next.valid()) {
            int err = errno;
            if (!isLeaf && err == ENOTDIR) {
                struct stat ast {};
                if (::fstatat(cur.fd, comp.constData(), &ast, AT_SYMLINK_NOFOLLOW) == 0
                    && S_ISLNK(ast.st_mode)) {
                    err = ELOOP;
                }
            }
            if (outErrno) *outErrno = err;
            return {};
        }
        cur = std::move(next);
    }

    struct stat st {};
    if (::fstat(cur.fd, &st) != 0) {
        if (outErrno) *outErrno = errno;
        return {};
    }
    if (S_ISLNK(st.st_mode)) {
        if (outErrno) *outErrno = ELOOP;
        return {};
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        if (outErrno) *outErrno = ENOTSUP;
        return {};
    }
    if (outStat) *outStat = st;
    return cur;
}

} // namespace AclFsDetail

class AclProvider
{
public:
    static AclData unavailableForRemote()
    {
        AclData out; out.capability = AclCapability::RemoteUnavailable;
        out.errorMessage = QStringLiteral("POSIX ACL is unavailable for remote URLs."); return out;
    }

    static AclData loadFromFd(int fd, const QString &path, const struct stat &st, bool directory, bool readOnly = false)
    {
        AclData out;
        out.path = path;
        out.isDirectory = directory;
        out.statDev = quint64(st.st_dev);
        out.statIno = quint64(st.st_ino);
        out.statIdentityValid = true;

        if (bool(S_ISDIR(st.st_mode)) != directory) {
            out.capability = AclCapability::InvalidData;
            out.errorMessage = QStringLiteral("Target file type does not match requested ACL type.");
            return out;
        }

        const QByteArray procPath = AclFsDetail::procFdPath(fd);
        acl_t access = acl_get_file(procPath.constData(), ACL_TYPE_ACCESS);
        if (!access) { setError(out, errno); return out; }
        QString parseError;
        if (!parse(access, &out.accessEntries, &parseError)) {
            acl_free(access); out.capability = AclCapability::InvalidData; out.errorMessage = parseError; return out;
        }
        acl_free(access);
        out.hasExtendedAccess = hasExtended(out.accessEntries);
        applyEffective(out.accessEntries);

        if (directory) {
            errno = 0;
            acl_t def = acl_get_file(procPath.constData(), ACL_TYPE_DEFAULT);
            if (def) {
                if (!parse(def, &out.defaultEntries, &parseError)) {
                    acl_free(def); out.capability = AclCapability::InvalidData; out.errorMessage = parseError; return out;
                }
                acl_free(def); out.hasDefaultAcl = !out.defaultEntries.isEmpty(); applyEffective(out.defaultEntries);
            } else if (errno != ENODATA) {
                setError(out, errno); return out;
            }
        }
        out.capability = readOnly ? AclCapability::AvailableReadOnly : AclCapability::AvailableReadWrite;
        return out;
    }

    static AclData load(const QString &path, bool local, bool symlink, bool directory, bool readOnly = false)
    {
        if (!local) return unavailableForRemote();
        AclData out; out.path = path; out.isDirectory = directory;
        if (symlink) { out.capability = AclCapability::SymlinkUnavailable; out.errorMessage = QStringLiteral("POSIX ACL is unavailable for symbolic links."); return out; }
        if (path.isEmpty()) { out.capability = AclCapability::InvalidData; out.errorMessage = QStringLiteral("Missing local path."); return out; }

        struct stat st {};
        int openErr = 0;
        AclFsDetail::UniqueFd fd = AclFsDetail::openAnchoredNoSymlinks(path, &st, &openErr);
        if (!fd.valid()) {
            if (openErr == ELOOP) {
                out.capability = AclCapability::SymlinkUnavailable;
                out.errorMessage = QStringLiteral("POSIX ACL is unavailable for symbolic links.");
                return out;
            }
            setError(out, openErr ? openErr : EINVAL);
            return out;
        }
        return loadFromFd(fd.fd, path, st, directory, readOnly);
    }

    static QString userName(uint uid)
    {
        long n = sysconf(_SC_GETPW_R_SIZE_MAX); if (n < 1024) n = 16384;
        QByteArray buffer(n, Qt::Uninitialized); passwd pwd{}; passwd *result = nullptr;
        if (getpwuid_r(uid, &pwd, buffer.data(), size_t(buffer.size()), &result) == 0 && result) return QString::fromLocal8Bit(result->pw_name);
        return QString::number(uid);
    }
    static QString groupName(uint gid)
    {
        long n = sysconf(_SC_GETGR_R_SIZE_MAX); if (n < 1024) n = 16384;
        QByteArray buffer(n, Qt::Uninitialized); group grp{}; group *result = nullptr;
        if (getgrgid_r(gid, &grp, buffer.data(), size_t(buffer.size()), &result) == 0 && result) return QString::fromLocal8Bit(result->gr_name);
        return QString::number(gid);
    }

    static void applyEffective(QList<AclEntryData> &entries)
    {
        int mask = 7; for (const auto &entry : entries) if (entry.tag == AclTag::Mask) mask = entry.permissions.bits();
        for (auto &entry : entries) entry.effectivePermissions = AclPermissions::fromBits(entry.permissions.bits() & (entry.isMasked() ? mask : 7));
    }
    static bool hasExtended(const QList<AclEntryData> &entries)
    {
        for (const auto &entry : entries) if (entry.isNamed() || entry.tag == AclTag::Mask) return true;
        return false;
    }
    static AclCapability capabilityForErrno(int error)
    {
        if (error == ENOTSUP
#if EOPNOTSUPP != ENOTSUP
            || error == EOPNOTSUPP
#endif
        ) return AclCapability::Unsupported;
        if (error == EACCES || error == EPERM) return AclCapability::PermissionDenied;
        if (error == ENOENT) return AclCapability::Disappeared;
        return AclCapability::InvalidData;
    }

private:
    static void setError(AclData &out, int error)
    {
        out.capability = capabilityForErrno(error);
        out.errorMessage = qt_error_string(error);
    }
    static bool parse(acl_t acl, QList<AclEntryData> *result, QString *error)
    {
        result->clear(); acl_entry_t nativeEntry{}; int id = ACL_FIRST_ENTRY;
        while (true) {
            const int rc = acl_get_entry(acl, id, &nativeEntry); id = ACL_NEXT_ENTRY;
            if (rc == 0) break;
            if (rc < 0) { *error = QStringLiteral("acl_get_entry failed: %1").arg(qt_error_string(errno)); return false; }
            acl_tag_t nativeTag{}; if (acl_get_tag_type(nativeEntry, &nativeTag) != 0) { *error = QStringLiteral("Invalid ACL tag."); return false; }
            AclEntryData item;
            switch (nativeTag) {
            case ACL_USER_OBJ: item.tag = AclTag::UserObject; item.name = QStringLiteral("owner"); break;
            case ACL_USER: { item.tag = AclTag::NamedUser; auto *q = static_cast<uid_t *>(acl_get_qualifier(nativeEntry)); if (!q) return false; item.qualifier = *q; acl_free(q); item.name = userName(item.qualifier); break; }
            case ACL_GROUP_OBJ: item.tag = AclTag::GroupObject; item.name = QStringLiteral("group"); break;
            case ACL_GROUP: { item.tag = AclTag::NamedGroup; auto *q = static_cast<gid_t *>(acl_get_qualifier(nativeEntry)); if (!q) return false; item.qualifier = *q; acl_free(q); item.name = groupName(item.qualifier); break; }
            case ACL_MASK: item.tag = AclTag::Mask; item.name = QStringLiteral("mask"); break;
            case ACL_OTHER: item.tag = AclTag::Other; item.name = QStringLiteral("other"); break;
            default: *error = QStringLiteral("Unknown ACL entry tag."); return false;
            }
            acl_permset_t set{}; if (acl_get_permset(nativeEntry, &set) != 0) { *error = QStringLiteral("Invalid ACL permission set."); return false; }
            item.permissions = {acl_get_perm(set, ACL_READ) == 1, acl_get_perm(set, ACL_WRITE) == 1, acl_get_perm(set, ACL_EXECUTE) == 1};
            result->append(item);
        }
        return true;
    }
};
