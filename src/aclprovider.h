/* Local-only native libacl provider. It deliberately has no KIO dependency. */
#pragma once

#include "acldata.h"

#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <acl/libacl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/acl.h>
#include <cerrno>

class AclProvider
{
public:
    static AclData unavailableForRemote()
    {
        AclData out; out.capability = AclCapability::RemoteUnavailable;
        out.errorMessage = QStringLiteral("POSIX ACL is unavailable for remote URLs."); return out;
    }

    static AclData load(const QString &path, bool local, bool symlink, bool directory, bool readOnly = false)
    {
        if (!local) return unavailableForRemote();
        AclData out; out.path = path; out.isDirectory = directory;
        if (symlink) { out.capability = AclCapability::SymlinkUnavailable; out.errorMessage = QStringLiteral("POSIX ACL is unavailable for symbolic links."); return out; }
        if (path.isEmpty()) { out.capability = AclCapability::InvalidData; out.errorMessage = QStringLiteral("Missing local path."); return out; }
        acl_t access = acl_get_file(QFile::encodeName(path).constData(), ACL_TYPE_ACCESS);
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
            acl_t def = acl_get_file(QFile::encodeName(path).constData(), ACL_TYPE_DEFAULT);
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
        out.errorMessage = QString::fromLocal8Bit(strerror(error));
    }
    static bool parse(acl_t acl, QList<AclEntryData> *result, QString *error)
    {
        result->clear(); acl_entry_t nativeEntry{}; int id = ACL_FIRST_ENTRY;
        while (true) {
            const int rc = acl_get_entry(acl, id, &nativeEntry); id = ACL_NEXT_ENTRY;
            if (rc == 0) break;
            if (rc < 0) { *error = QStringLiteral("acl_get_entry failed: %1").arg(QString::fromLocal8Bit(strerror(errno))); return false; }
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
