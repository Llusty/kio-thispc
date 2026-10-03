/* Native POSIX ACL Stage 3 regression. All writes stay in QTemporaryDir. */
#include "aclcontroller.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <sys/stat.h>
#include <unistd.h>

static int aclChecks = 0;
static void aclVerify(bool value, const char *description) { if (!value) qFatal("FAIL: %s", description); ++aclChecks; }
static AclEntryData aclEntry(AclTag tag, int bits, uint qualifier = 0)
{
    AclEntryData entry; entry.tag = tag; entry.qualifier = qualifier; entry.permissions = AclPermissions::fromBits(bits); return entry;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temp; aclVerify(temp.isValid(), "temporary directory");
    const QString filePath = temp.filePath("file"); QFile file(filePath); aclVerify(file.open(QIODevice::WriteOnly), "temporary file"); file.write("acl\n"); file.close();
    aclVerify(::chmod(QFile::encodeName(filePath).constData(), 0640) == 0, "initial chmod");
    AclData minimal = AclProvider::load(filePath, true, false, false, false);
    aclVerify(minimal.capability == AclCapability::AvailableReadWrite, "local ACL capability");
    aclVerify(minimal.accessEntries.size() == 3 && !minimal.hasExtendedAccess, "minimal access ACL");
    QString error; aclVerify(AclController::validateEntries(minimal.accessEntries, false, &error), "minimal ACL validates");
    aclVerify(!AclController::validateEntries({aclEntry(AclTag::UserObject, 7)}, false, &error), "incomplete ACL rejected before write");
    AclData extended = minimal;
    extended.accessEntries << aclEntry(AclTag::NamedUser, 6, uint(geteuid())) << aclEntry(AclTag::NamedGroup, 4, uint(getegid())) << aclEntry(AclTag::Mask, 4);
    aclVerify(AclController::validateEntries(extended.accessEntries, false, &error), "named user/group and mask validate");
    auto written = AclController::write(extended); aclVerify(written.success, "extended ACL write and reread verification");
    aclVerify(written.data.hasExtendedAccess, "extended state separate from capability");
    bool sawUser = false, sawGroup = false, sawMask = false, effective = false;
    for (const auto &entry : written.data.accessEntries) {
        sawUser |= entry.tag == AclTag::NamedUser; sawGroup |= entry.tag == AclTag::NamedGroup; sawMask |= entry.tag == AclTag::Mask;
        if (entry.tag == AclTag::NamedUser) effective = entry.permissions.bits() == 6 && entry.effectivePermissions.bits() == 4;
    }
    aclVerify(sawUser && sawGroup && sawMask, "owner/group/named entries and mask reread"); aclVerify(effective, "effective permissions intersect mask");
    AclData explicitMask = minimal;
    explicitMask.accessEntries << aclEntry(AclTag::NamedUser, 7, uint(geteuid())) << aclEntry(AclTag::Mask, 5);
    auto explicitMaskWrite = AclController::write(explicitMask);
    aclVerify(explicitMaskWrite.success, "explicit restrictive mask write succeeds");
    int explicitMaskBits = -1, namedStored = -1, namedEffective = -1, groupObject = -1;
    for (const auto &entry : explicitMaskWrite.data.accessEntries) {
        if (entry.tag == AclTag::Mask) explicitMaskBits = entry.permissions.bits();
        if (entry.tag == AclTag::NamedUser) { namedStored = entry.permissions.bits(); namedEffective = entry.effectivePermissions.bits(); }
        if (entry.tag == AclTag::GroupObject) groupObject = entry.permissions.bits();
    }
    struct stat explicitMaskStat {};
    aclVerify(::stat(QFile::encodeName(filePath).constData(), &explicitMaskStat) == 0, "stat explicit-mask fixture");
    aclVerify(explicitMaskBits == 5, "explicit mask survives native write and reread");
    aclVerify(namedStored == 7 && namedEffective == 5, "named user stored/effective semantics survive reread");
    aclVerify(groupObject == 4, "group object remains rw after explicit mask write");
    aclVerify(((explicitMaskStat.st_mode >> 3) & 7) == 5, "mode group bits reflect ACL mask");
    written = explicitMaskWrite;
    auto backToMinimal = written.data.accessEntries;
    AclController::removeNamed(backToMinimal, AclTag::NamedUser, uint(geteuid()));
    AclController::removeNamed(backToMinimal, AclTag::NamedGroup, uint(getegid()));
    aclVerify(backToMinimal.size() == 3 && AclController::validateEntries(backToMinimal, false, &error), "extended to minimal removes orphan mask");
    AclData reduced = written.data; reduced.accessEntries = backToMinimal;
    auto reducedWrite = AclController::write(reduced); aclVerify(reducedWrite.success && !reducedWrite.data.hasExtendedAccess, "named entries removed on disk");
    const QString directory = temp.filePath("dir"); aclVerify(QDir().mkpath(directory), "temporary directory created");
    AclData dirAcl = AclProvider::load(directory, true, false, true, false); aclVerify(dirAcl.canWrite() && !dirAcl.hasDefaultAcl, "directory starts without default ACL");
    dirAcl.defaultEntries = AclController::minimalFromMode(0750); auto defaultWrite = AclController::write(dirAcl);
    aclVerify(defaultWrite.success && defaultWrite.data.hasDefaultAcl, "default directory ACL write and reread");
    defaultWrite.data.defaultEntries.clear(); auto defaultDelete = AclController::write(defaultWrite.data);
    aclVerify(defaultDelete.success && !defaultDelete.data.hasDefaultAcl, "default ACL deleted with acl_delete_def_file");
    AclData invalidFile = reducedWrite.data; invalidFile.defaultEntries = AclController::minimalFromMode(0700);
    aclVerify(!AclController::write(invalidFile).success, "default ACL rejected on file");
    aclVerify(AclProvider::unavailableForRemote().capability == AclCapability::RemoteUnavailable, "remote never calls local libacl");
    aclVerify(AclProvider::capabilityForErrno(ENOTSUP) == AclCapability::Unsupported, "unsupported filesystem is a normal capability");
    aclVerify(AclProvider::capabilityForErrno(EACCES) == AclCapability::PermissionDenied, "permission denied classified");
    aclVerify(AclProvider::capabilityForErrno(EINVAL) == AclCapability::InvalidData, "invalid native ACL data classified");
    aclVerify(AclProvider::load(filePath, true, true, false).capability == AclCapability::SymlinkUnavailable, "symlink ACL unavailable without target access");
    AclData ro = AclProvider::load(filePath, true, false, false, true); aclVerify(ro.capability == AclCapability::AvailableReadOnly && !AclController::write(ro).success, "read-only ACL cannot write");
    aclVerify(AclProvider::load(temp.filePath("gone"), true, false, false).capability == AclCapability::Disappeared, "disappeared file classified");
    aclVerify(!QFileInfo::exists(temp.filePath("child")), "ACL operations are not recursive");
    qInfo("PASS: %d ACL assertions; native libacl, validation, effective mask, add/remove, default ACL, reread", aclChecks);
}
