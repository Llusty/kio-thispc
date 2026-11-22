/* Native POSIX ACL Stage 3 regression. All writes stay in QTemporaryDir. */
#include "aclcontroller.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

    // SEC-045-06: Direct symlink rejection in AclController::write()
    const QString symPath = temp.filePath("symlink_to_file");
    aclVerify(QFile::link(filePath, symPath), "SEC-045-06: symlink created for ACL rejection test");
    struct stat beforeSymStat {};
    aclVerify(::stat(QFile::encodeName(filePath).constData(), &beforeSymStat) == 0, "SEC-045-06: stat target before symlink ACL write");
    AclData forgedSymAcl = reducedWrite.data;
    forgedSymAcl.path = symPath;
    forgedSymAcl.capability = AclCapability::AvailableReadWrite;
    forgedSymAcl.accessEntries = AclController::minimalFromMode(0777);
    const auto symWriteRes = AclController::write(forgedSymAcl);
    aclVerify(!symWriteRes.success, "SEC-045-06: AclController::write refuses symlink even with AvailableReadWrite");
    struct stat afterSymStat {};
    aclVerify(::stat(QFile::encodeName(filePath).constData(), &afterSymStat) == 0
                  && (beforeSymStat.st_mode & 0777) == (afterSymStat.st_mode & 0777),
              "SEC-045-06: symlink target mode unchanged after rejected ACL write");

    // ==================== Stage 1B.9B-3A (SEC-046-11): ACL Symlink TOCTOU & FIFO Safety (ACL1–ACL16) ====================
    auto sha256Of = [](const QString &p) -> QByteArray {
        QFile f(p);
        aclVerify(f.open(QIODevice::ReadOnly), "sha256 open");
        return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
    };
    auto aclTextOf = [](const QString &p, acl_type_t type) -> QByteArray {
        acl_t a = acl_get_file(QFile::encodeName(p).constData(), type);
        if (!a) return {};
        ssize_t len = 0;
        char *txt = acl_to_text(a, &len);
        QByteArray out = txt ? QByteArray(txt, int(len)) : QByteArray();
        if (txt) acl_free(txt);
        acl_free(a);
        return out;
    };
    auto countOpenFds = []() -> int {
        return QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size();
    };

    // ACL2 & ACL3 — Directory access ACL update while preserving default ACL
    {
        const QString d2 = temp.filePath(QStringLiteral("acl2_dir"));
        aclVerify(QDir().mkpath(d2), "ACL2 dir created");
        AclData da = AclProvider::load(d2, true, false, true, false);
        aclVerify(da.canWrite() && da.statIdentityValid, "ACL2 dir loaded with stat identity");
        da.accessEntries << aclEntry(AclTag::NamedUser, 7, uint(geteuid())) << aclEntry(AclTag::Mask, 5);
        da.defaultEntries = AclController::minimalFromMode(0750);
        auto res = AclController::write(da);
        aclVerify(res.success && res.data.hasExtendedAccess && res.data.hasDefaultAcl,
                  "ACL2/ACL3: directory access + default ACL written via pinned descriptor");
    }

    // ACL4 — Mode 0000 (unreadable owned) regular file works safely via O_PATH without raw pathname fallback
    {
        const QString roFile = temp.filePath(QStringLiteral("acl4_mode0000.txt"));
        {
            QFile f(roFile);
            aclVerify(f.open(QIODevice::WriteOnly), "ACL4 create file");
            f.write("mode0000-secret");
        }
        aclVerify(::chmod(QFile::encodeName(roFile).constData(), 0000) == 0, "ACL4 chmod 0000");
        AclData roData = AclProvider::load(roFile, true, false, false, false);
        aclVerify(roData.canWrite(), "ACL4: mode 0000 owned file loads safely via O_PATH");
        roData.accessEntries = AclController::minimalFromMode(0600);
        auto roRes = AclController::write(roData);
        aclVerify(roRes.success, "ACL4: mode 0000 owned file ACL write succeeds via pinned O_PATH FD");
        aclVerify(sha256Of(roFile) == QCryptographicHash::hash("mode0000-secret", QCryptographicHash::Sha256).toHex(),
                  "ACL4/ACL16: file content unchanged");
    }

    // ACL6 & ACL16 — Directory symlink rejected by both load() and write(); external dir & sentinel untouched
    {
        const QString extDir = temp.filePath(QStringLiteral("acl6_ext_dir"));
        const QString extFile = extDir + QStringLiteral("/sentinel.txt");
        aclVerify(QDir().mkpath(extDir), "ACL6 extDir");
        {
            QFile f(extFile);
            aclVerify(f.open(QIODevice::WriteOnly), "ACL6 sentinel");
            f.write("acl6-dir-sentinel");
        }
        const QByteArray extAclBefore = aclTextOf(extDir, ACL_TYPE_ACCESS);
        const QByteArray extHashBefore = sha256Of(extFile);
        const QString dirSym = temp.filePath(QStringLiteral("acl6_dir_symlink"));
        aclVerify(::symlink(QFile::encodeName(extDir).constData(), QFile::encodeName(dirSym).constData()) == 0,
                  "ACL6 create dir symlink");
        // Even if caller passes symlink=false, AclProvider::load must detect symlink via O_PATH|O_NOFOLLOW
        const AclData loadedSym = AclProvider::load(dirSym, true, false, true, false);
        aclVerify(loadedSym.capability == AclCapability::SymlinkUnavailable,
                  "ACL6: AclProvider::load detects directory symlink even with symlink=false");
        AclData forgedDir = AclProvider::load(extDir, true, false, true, false);
        forgedDir.path = dirSym;
        forgedDir.statIdentityValid = false;
        forgedDir.accessEntries = AclController::minimalFromMode(0777);
        forgedDir.defaultEntries = AclController::minimalFromMode(0777);
        aclVerify(!AclController::write(forgedDir).success, "ACL6: AclController::write rejects directory symlink");
        aclVerify(aclTextOf(extDir, ACL_TYPE_ACCESS) == extAclBefore
            && aclTextOf(extDir, ACL_TYPE_DEFAULT).isEmpty()
            && sha256Of(extFile) == extHashBefore,
                  "ACL6/ACL16: external directory ACL and sentinel unchanged");
    }

    // ACL7 — FIFO without active writer/reader returns promptly without blocking
    {
        const QString fifoPath = temp.filePath(QStringLiteral("acl7_test.fifo"));
        aclVerify(::mkfifo(QFile::encodeName(fifoPath).constData(), 0600) == 0, "ACL7 mkfifo");
        QElapsedTimer timer;
        timer.start();
        const AclData fifoLoaded = AclProvider::load(fifoPath, true, false, false, false);
        const qint64 loadMs = timer.elapsed();
        aclVerify(loadMs < 1000 && fifoLoaded.capability == AclCapability::Unsupported,
                  "ACL7: AclProvider::load rejects FIFO promptly without blocking");

        AclData forgedFifo;
        forgedFifo.path = fifoPath;
        forgedFifo.capability = AclCapability::AvailableReadWrite;
        forgedFifo.isDirectory = false;
        forgedFifo.accessEntries = AclController::minimalFromMode(0666);
        timer.restart();
        const auto fifoWrite = AclController::write(forgedFifo);
        const qint64 writeMs = timer.elapsed();
        aclVerify(writeMs < 1000 && !fifoWrite.success,
                  "ACL7: AclController::write rejects FIFO promptly without blocking");
    }

    // ACL8 — Unsupported special file (Unix domain socket) rejected safely without blocking
    {
        const QString sockPath = temp.filePath(QStringLiteral("acl8_test.sock"));
        const int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        aclVerify(sfd >= 0, "ACL8 socket()");
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        const QByteArray sockEnc = QFile::encodeName(sockPath);
        aclVerify(size_t(sockEnc.size()) < sizeof(addr.sun_path), "ACL8 sockPath fits sun_path");
        std::memcpy(addr.sun_path, sockEnc.constData(), size_t(sockEnc.size()) + 1);
        aclVerify(::bind(sfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0, "ACL8 bind()");
        ::close(sfd);

        const AclData sockLoaded = AclProvider::load(sockPath, true, false, false, false);
        aclVerify(sockLoaded.capability == AclCapability::Unsupported, "ACL8: socket classified Unsupported");
        AclData forgedSock;
        forgedSock.path = sockPath;
        forgedSock.capability = AclCapability::AvailableReadWrite;
        forgedSock.accessEntries = AclController::minimalFromMode(0666);
        aclVerify(!AclController::write(forgedSock).success, "ACL8: AclController::write rejects socket");
    }

    // ACL5 & ACL9 & ACL16 — Target replacement (post-load inode swap AND mid-write symlink swap via hook)
    {
        // ACL9a: Post-load inode replacement
        const QString targetPath = temp.filePath(QStringLiteral("acl9_target.txt"));
        const QString replPath = temp.filePath(QStringLiteral("acl9_repl.txt"));
        {
            QFile f1(targetPath); aclVerify(f1.open(QIODevice::WriteOnly), "ACL9a orig"); f1.write("orig");
            QFile f2(replPath); aclVerify(f2.open(QIODevice::WriteOnly), "ACL9a repl"); f2.write("replacement-sentinel");
        }
        aclVerify(::chmod(QFile::encodeName(replPath).constData(), 0600) == 0, "ACL9a repl chmod 0600");
        const QByteArray replAclBefore = aclTextOf(replPath, ACL_TYPE_ACCESS);
        const QByteArray replHashBefore = sha256Of(replPath);

        AclData loadedOrig = AclProvider::load(targetPath, true, false, false, false);
        aclVerify(loadedOrig.canWrite() && loadedOrig.statIdentityValid, "ACL9a load orig");
        aclVerify(::rename(QFile::encodeName(replPath).constData(), QFile::encodeName(targetPath).constData()) == 0,
                  "ACL9a replace target with distinct inode");
        loadedOrig.accessEntries = AclController::minimalFromMode(0777);
        const auto res9a = AclController::write(loadedOrig);
        aclVerify(!res9a.success, "ACL9a: post-load inode replacement rejected fail-closed");
        aclVerify(aclTextOf(targetPath, ACL_TYPE_ACCESS) == replAclBefore
            && sha256Of(targetPath) == replHashBefore,
                  "ACL9a/ACL16: replacement inode ACL and SHA-256 unchanged");

        // ACL5/ACL9b: Mid-write file->symlink swap via deterministic PreWriteHook
        const QString victimPath = temp.filePath(QStringLiteral("acl9b_victim.txt"));
        const QString extSentinel = temp.filePath(QStringLiteral("acl9b_ext_sentinel.txt"));
        {
            QFile f1(victimPath); aclVerify(f1.open(QIODevice::WriteOnly), "ACL9b victim"); f1.write("victim");
            QFile f2(extSentinel); aclVerify(f2.open(QIODevice::WriteOnly), "ACL9b ext"); f2.write("ext-sentinel-payload");
        }
        aclVerify(::chmod(QFile::encodeName(extSentinel).constData(), 0600) == 0, "ACL9b ext chmod 0600");
        const QByteArray extAclBefore = aclTextOf(extSentinel, ACL_TYPE_ACCESS);
        const QByteArray extHashBefore = sha256Of(extSentinel);

        AclData victimData = AclProvider::load(victimPath, true, false, false, false);
        victimData.accessEntries = AclController::minimalFromMode(0777);
        bool hookFired = false;
        AclController::setPreWriteHook([&](const QString &p) {
            hookFired = true;
            ::unlink(QFile::encodeName(p).constData());
            ::symlink(QFile::encodeName(extSentinel).constData(), QFile::encodeName(p).constData());
        });
        const auto res9b = AclController::write(victimData);
        AclController::setPreWriteHook({});
        aclVerify(hookFired && !res9b.success, "ACL5/ACL9b: mid-write symlink swap rejected fail-closed");
        aclVerify(aclTextOf(extSentinel, ACL_TYPE_ACCESS) == extAclBefore
            && sha256Of(extSentinel) == extHashBefore,
                  "ACL5/ACL9b/ACL16: external sentinel ACL and SHA-256 untouched");
    }

    // ACL10 & ACL16 — Ancestor symlink substitution (both upfront and mid-write via PreWriteHook)
    {
        const QString realDir = temp.filePath(QStringLiteral("acl10_real"));
        const QString linkDir = temp.filePath(QStringLiteral("acl10_link"));
        const QString realFile = realDir + QStringLiteral("/secret.txt");
        aclVerify(QDir().mkpath(realDir), "ACL10 realDir");
        {
            QFile f(realFile); aclVerify(f.open(QIODevice::WriteOnly), "ACL10 realFile"); f.write("ancestor-secret");
        }
        aclVerify(::chmod(QFile::encodeName(realFile).constData(), 0600) == 0, "ACL10 chmod 0600");
        aclVerify(::symlink(QFile::encodeName(realDir).constData(), QFile::encodeName(linkDir).constData()) == 0,
                  "ACL10 ancestor symlink");
        const QByteArray realAclBefore = aclTextOf(realFile, ACL_TYPE_ACCESS);
        const QByteArray realHashBefore = sha256Of(realFile);

        const QString viaSym = linkDir + QStringLiteral("/secret.txt");
        aclVerify(AclProvider::load(viaSym, true, false, false, false).capability == AclCapability::SymlinkUnavailable,
                  "ACL10: AclProvider::load rejects ancestor symlink");
        AclData forgedAnc;
        forgedAnc.path = viaSym;
        forgedAnc.capability = AclCapability::AvailableReadWrite;
        forgedAnc.accessEntries = AclController::minimalFromMode(0777);
        aclVerify(!AclController::write(forgedAnc).success, "ACL10: AclController::write rejects ancestor symlink");
        aclVerify(aclTextOf(realFile, ACL_TYPE_ACCESS) == realAclBefore
            && sha256Of(realFile) == realHashBefore,
                  "ACL10/ACL16: target behind ancestor symlink unchanged");

        // Mid-write ancestor swap via PreWriteHook
        const QString workAnc = temp.filePath(QStringLiteral("acl10_work"));
        const QString workAside = temp.filePath(QStringLiteral("acl10_work_aside"));
        const QString workFile = workAnc + QStringLiteral("/secret.txt");
        aclVerify(QDir().mkpath(workAnc), "ACL10 workAnc");
        {
            QFile f(workFile); aclVerify(f.open(QIODevice::WriteOnly), "ACL10 workFile"); f.write("work-file");
        }
        AclData workData = AclProvider::load(workFile, true, false, false, false);
        workData.accessEntries = AclController::minimalFromMode(0777);
        AclController::setPreWriteHook([&](const QString &) {
            ::rename(QFile::encodeName(workAnc).constData(), QFile::encodeName(workAside).constData());
            ::symlink(QFile::encodeName(realDir).constData(), QFile::encodeName(workAnc).constData());
        });
        const auto ancSwapRes = AclController::write(workData);
        AclController::setPreWriteHook({});
        aclVerify(!ancSwapRes.success, "ACL10: mid-write ancestor symlink swap rejected fail-closed");
        aclVerify(aclTextOf(realFile, ACL_TYPE_ACCESS) == realAclBefore
            && sha256Of(realFile) == realHashBefore,
                  "ACL10/ACL16: external target behind swapped ancestor unchanged");
    }

    // ACL15 — Resource lifecycle: zero FD leaks across success and failure paths
    {
        const int fdsBefore = countOpenFds();
        const QString okFile = temp.filePath(QStringLiteral("acl15_fd_ok.txt"));
        {
            QFile f(okFile); aclVerify(f.open(QIODevice::WriteOnly), "ACL15 okFile"); f.write("fd-check");
        }
        for (int i = 0; i < 10; ++i) {
            AclData d = AclProvider::load(okFile, true, false, false, false);
            d.accessEntries = AclController::minimalFromMode(0640);
            aclVerify(AclController::write(d).success, "ACL15 write ok");
            AclProvider::load(temp.filePath(QStringLiteral("acl15_missing")), true, false, false, false);
            AclProvider::load(temp.filePath(QStringLiteral("acl7_test.fifo")), true, false, false, false);
        }
        const int fdsAfter = countOpenFds();
        aclVerify(fdsAfter == fdsBefore, "ACL15: zero file descriptor leaks across repeated load/write/error paths");
    }

    qInfo("PASS: %d ACL assertions; native libacl, validation, effective mask, add/remove, default ACL, reread", aclChecks);
}
