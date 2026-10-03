/* POSIX ACL data model for Properties 2.0 (0.37 Stage 3). */
#pragma once

#include <QList>
#include <QString>

enum class AclCapability {
    AvailableReadWrite,
    AvailableReadOnly,
    Unsupported,
    RemoteUnavailable,
    SymlinkUnavailable,
    PermissionDenied,
    Disappeared,
    InvalidData
};

enum class AclTag { UserObject, NamedUser, GroupObject, NamedGroup, Mask, Other };

struct AclPermissions
{
    bool read = false;
    bool write = false;
    bool execute = false;

    int bits() const { return (read ? 4 : 0) | (write ? 2 : 0) | (execute ? 1 : 0); }
    static AclPermissions fromBits(int bits) { return {bool(bits & 4), bool(bits & 2), bool(bits & 1)}; }
    bool operator==(const AclPermissions &) const = default;
};

struct AclEntryData
{
    AclTag tag = AclTag::Other;
    uint qualifier = 0;
    QString name;
    AclPermissions permissions;
    AclPermissions effectivePermissions;

    bool isNamed() const { return tag == AclTag::NamedUser || tag == AclTag::NamedGroup; }
    bool isMasked() const { return isNamed() || tag == AclTag::GroupObject; }
};

struct AclData
{
    QString path;
    AclCapability capability = AclCapability::InvalidData;
    QList<AclEntryData> accessEntries;
    QList<AclEntryData> defaultEntries;
    bool isDirectory = false;
    bool hasExtendedAccess = false;
    bool hasDefaultAcl = false;
    QString errorMessage;

    bool canRead() const { return capability == AclCapability::AvailableReadWrite || capability == AclCapability::AvailableReadOnly; }
    bool canWrite() const { return capability == AclCapability::AvailableReadWrite; }
};
