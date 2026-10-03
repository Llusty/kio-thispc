/* Validation and verified write path for local POSIX ACLs. */
#pragma once

#include "aclprovider.h"

class AclController
{
public:
    struct Result { bool success = false; AclData data; QString errorMessage; };

    static bool validateEntries(const QList<AclEntryData> &entries, bool allowEmpty, QString *error = nullptr)
    {
        if (entries.isEmpty()) { if (allowEmpty) return true; return fail(error, QStringLiteral("Access ACL cannot be empty.")); }
        int owner = 0, group = 0, other = 0, mask = 0; bool named = false;
        QSet<quint64> identities;
        for (const auto &entry : entries) {
            switch (entry.tag) {
            case AclTag::UserObject: ++owner; break; case AclTag::GroupObject: ++group; break;
            case AclTag::Other: ++other; break; case AclTag::Mask: ++mask; break;
            case AclTag::NamedUser: case AclTag::NamedGroup: {
                named = true; const quint64 key = (quint64(entry.tag == AclTag::NamedGroup) << 63) | entry.qualifier;
                if (identities.contains(key)) return fail(error, QStringLiteral("Duplicate named ACL entry."));
                identities.insert(key); break;
            }}
        }
        if (owner != 1 || group != 1 || other != 1) return fail(error, QStringLiteral("ACL requires exactly one owner, owning group, and other entry."));
        if ((named && mask != 1) || (!named && mask > 1)) return fail(error, QStringLiteral("Extended ACL requires exactly one mask entry."));
        if (!named && mask == 1) return fail(error, QStringLiteral("Minimal ACL must not contain an orphaned mask."));
        acl_t native = build(entries, error); if (!native) return false;
        const bool valid = acl_valid(native) == 0; if (!valid && error) *error = QString::fromLocal8Bit(strerror(errno)); acl_free(native); return valid;
    }

    static Result write(const AclData &requested)
    {
        Result out;
        if (requested.capability != AclCapability::AvailableReadWrite) { out.errorMessage = QStringLiteral("ACL is not writable for this item."); return out; }
        QString error;
        if (!validateEntries(requested.accessEntries, false, &error)) { out.errorMessage = error; return out; }
        if (requested.isDirectory && !validateEntries(requested.defaultEntries, true, &error)) { out.errorMessage = error; return out; }
        if (!requested.isDirectory && !requested.defaultEntries.isEmpty()) { out.errorMessage = QStringLiteral("Default ACL is only valid for directories."); return out; }
        const QByteArray path = QFile::encodeName(requested.path);
        acl_t access = build(requested.accessEntries, &error);
        if (!access) { out.errorMessage = error; return out; }
        if (acl_set_file(path.constData(), ACL_TYPE_ACCESS, access) != 0) { out.errorMessage = QString::fromLocal8Bit(strerror(errno)); acl_free(access); return out; }
        acl_free(access);
        if (requested.isDirectory) {
            if (requested.defaultEntries.isEmpty()) {
                if (acl_delete_def_file(path.constData()) != 0 && errno != ENODATA) { out.errorMessage = QString::fromLocal8Bit(strerror(errno)); return out; }
            } else {
                acl_t def = build(requested.defaultEntries, &error); if (!def) { out.errorMessage = error; return out; }
                if (acl_set_file(path.constData(), ACL_TYPE_DEFAULT, def) != 0) { out.errorMessage = QString::fromLocal8Bit(strerror(errno)); acl_free(def); return out; } acl_free(def);
            }
        }
        out.data = AclProvider::load(requested.path, true, false, requested.isDirectory, false);
        if (!out.data.canRead()) { out.errorMessage = out.data.errorMessage; return out; }
        if (!equivalent(requested.accessEntries, out.data.accessEntries) || (requested.isDirectory && !equivalent(requested.defaultEntries, out.data.defaultEntries))) {
            out.errorMessage = QStringLiteral("ACL read-back did not match the requested entries."); return out;
        }
        out.success = true; return out;
    }

    static QList<AclEntryData> minimalFromMode(int mode)
    {
        return {{AclTag::UserObject, 0, QStringLiteral("owner"), AclPermissions::fromBits((mode >> 6) & 7), {}},
                {AclTag::GroupObject, 0, QStringLiteral("group"), AclPermissions::fromBits((mode >> 3) & 7), {}},
                {AclTag::Other, 0, QStringLiteral("other"), AclPermissions::fromBits(mode & 7), {}}};
    }
    static void removeNamed(QList<AclEntryData> &entries, AclTag tag, uint qualifier)
    {
        entries.removeIf([&](const AclEntryData &e) { return e.tag == tag && e.qualifier == qualifier; });
        bool named = false; for (const auto &e : entries) named |= e.isNamed();
        if (!named) entries.removeIf([](const AclEntryData &e) { return e.tag == AclTag::Mask; });
        AclProvider::applyEffective(entries);
    }

private:
    static bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
    static acl_t build(const QList<AclEntryData> &entries, QString *error)
    {
        acl_t acl = acl_init(entries.size()); if (!acl) { if (error) *error = QString::fromLocal8Bit(strerror(errno)); return nullptr; }
        for (const auto &item : entries) {
            acl_entry_t entry{}; if (acl_create_entry(&acl, &entry) != 0) { acl_free(acl); return nullptr; }
            acl_tag_t tag = ACL_OTHER; switch (item.tag) { case AclTag::UserObject: tag=ACL_USER_OBJ; break; case AclTag::NamedUser: tag=ACL_USER; break; case AclTag::GroupObject: tag=ACL_GROUP_OBJ; break; case AclTag::NamedGroup: tag=ACL_GROUP; break; case AclTag::Mask: tag=ACL_MASK; break; case AclTag::Other: tag=ACL_OTHER; break; }
            if (acl_set_tag_type(entry, tag) != 0) { acl_free(acl); return nullptr; }
            if (item.tag == AclTag::NamedUser) { uid_t q = item.qualifier; if (acl_set_qualifier(entry, &q) != 0) { acl_free(acl); return nullptr; } }
            if (item.tag == AclTag::NamedGroup) { gid_t q = item.qualifier; if (acl_set_qualifier(entry, &q) != 0) { acl_free(acl); return nullptr; } }
            acl_permset_t set{}; if (acl_get_permset(entry, &set) != 0 || acl_clear_perms(set) != 0) { acl_free(acl); return nullptr; }
            if (item.permissions.read) acl_add_perm(set, ACL_READ);
            if (item.permissions.write) acl_add_perm(set, ACL_WRITE);
            if (item.permissions.execute) acl_add_perm(set, ACL_EXECUTE);
            if (acl_set_permset(entry, set) != 0) { acl_free(acl); return nullptr; }
        }
        return acl;
    }
    static bool equivalent(const QList<AclEntryData> &a, const QList<AclEntryData> &b)
    {
        if (a.size() != b.size()) return false;
        auto key = [](const AclEntryData &e) { return QStringLiteral("%1:%2:%3").arg(int(e.tag)).arg(e.qualifier).arg(e.permissions.bits()); };
        QStringList x, y; for (const auto &e : a) x << key(e); for (const auto &e : b) y << key(e); x.sort(); y.sort(); return x == y;
    }
};
