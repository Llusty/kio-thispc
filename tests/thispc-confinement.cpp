/* Stage 1B.9B-3B (SEC-046-17): KIO thispc:/ mount-point confinement security regressions (K1–K22). */
#include "thispcconfinement.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include <sys/stat.h>
#include <unistd.h>

static int kioConfChecks = 0;
static void kcVerify(bool cond, const char *desc)
{
    if (!cond) {
        qFatal("FAIL: %s", desc);
    }
    ++kioConfChecks;
}

static void kcWriteFile(const QString &path, const QByteArray &data)
{
    kcVerify(QDir().mkpath(QFileInfo(path).path()), "kcWriteFile mkpath");
    QFile f(path);
    kcVerify(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "kcWriteFile open");
    kcVerify(f.write(data) == data.size(), "kcWriteFile write");
    f.close();
}

static QByteArray kcSha256(const QString &path)
{
    QFile f(path);
    kcVerify(f.open(QIODevice::ReadOnly), "kcSha256 open");
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    kcVerify(tmp.isValid(), "synthetic temp sandbox valid");

    const QString mountRoot = tmp.filePath(QStringLiteral("mnt/disk"));
    const QString prefixSibling = tmp.filePath(QStringLiteral("mnt/disk2"));
    const QString outsideDir = tmp.filePath(QStringLiteral("outside"));
    const QString outsideSentinel = outsideDir + QStringLiteral("/sentinel.txt");
    const QString siblingSentinel = prefixSibling + QStringLiteral("/secret.txt");

    kcVerify(QDir().mkpath(mountRoot + QStringLiteral("/nested/sub")), "mountRoot nested");
    kcVerify(QDir().mkpath(prefixSibling), "prefixSibling");
    kcVerify(QDir().mkpath(outsideDir), "outsideDir");

    const QString childFile = mountRoot + QStringLiteral("/nested/sub/doc.txt");
    const QString unicodeFile = mountRoot + QStringLiteral("/Zażółć gęślą jaźń/Raport 2026.txt");
    kcWriteFile(childFile, "normal-child-payload");
    kcWriteFile(unicodeFile, "unicode-payload");
    kcWriteFile(outsideSentinel, "external-sentinel-do-not-touch");
    kcWriteFile(siblingSentinel, "prefix-sibling-sentinel");

    const QByteArray hOutsideBefore = kcSha256(outsideSentinel);
    const QByteArray hSiblingBefore = kcSha256(siblingSentinel);
    struct stat outsideStatBefore {};
    kcVerify(::lstat(QFile::encodeName(outsideSentinel).constData(), &outsideStatBefore) == 0,
             "lstat outside sentinel before");

    const QString canonMount = QFileInfo(mountRoot).canonicalFilePath();
    struct stat mountStat {};
    kcVerify(::lstat(QFile::encodeName(mountRoot).constData(), &mountStat) == 0, "lstat mountRoot");

    // K1 — Exact mount root succeeds
    {
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1")), mountRoot, &mountStat);
        kcVerify(res.ok && res.redirectedUrl == QUrl::fromLocalFile(canonMount),
                 "K1: exact mount root resolves cleanly");
    }

    // K2 — Normal nested child succeeds
    {
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/nested/sub/doc.txt")), mountRoot, &mountStat);
        kcVerify(res.ok && res.redirectedUrl == QUrl::fromLocalFile(canonMount + QStringLiteral("/nested/sub/doc.txt")),
                 "K2: normal nested child resolves inside mountRoot");
    }

    // K3 — Parent traversal (../outside) rejected
    {
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/../outside/sentinel.txt")), mountRoot, &mountStat);
        kcVerify(!res.ok && res.kioError == KIO::ERR_ACCESS_DENIED && !res.redirectedUrl.isValid(),
                 "K3: ../outside parent traversal rejected fail-closed");
    }

    // K4 — Repeated traversal rejected
    {
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/nested/../../../../../etc/passwd")), mountRoot, &mountStat);
        kcVerify(!res.ok && res.kioError == KIO::ERR_ACCESS_DENIED,
                 "K4: repeated parent traversal rejected fail-closed");
    }

    // K5 — Prefix confusion (/mnt/disk2 is NOT inside /mnt/disk)
    {
        kcVerify(!ThisPcConfinement::isWithinDirectory(prefixSibling, mountRoot),
                 "K5a: prefix sibling dir not within mountRoot");
        kcVerify(!ThisPcConfinement::isWithinDirectory(siblingSentinel, mountRoot),
                 "K5b: file in prefix sibling not within mountRoot");
        kcVerify(ThisPcConfinement::isWithinDirectory(mountRoot, mountRoot),
                 "K5c: mountRoot itself is within mountRoot");
        kcVerify(ThisPcConfinement::isWithinDirectory(childFile, mountRoot),
                 "K5d: nested child is within mountRoot");
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/../disk2/secret.txt")), mountRoot, &mountStat);
        kcVerify(!res.ok && res.kioError == KIO::ERR_ACCESS_DENIED,
                 "K5e: traversal to prefix sibling rejected");
    }

    // K6 — Absolute-path injection cannot override mountRoot
    {
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1///etc/passwd")), mountRoot, &mountStat);
        kcVerify(res.ok && ThisPcConfinement::isWithinDirectory(res.redirectedUrl.toLocalFile(), canonMount)
            && res.redirectedUrl.toLocalFile() == canonMount + QStringLiteral("/etc/passwd"),
                 "K6: repeated slashes stay confined inside mountRoot, never /etc/passwd");
    }

    // K7 — Encoded traversal (%2e%2e, %2f) using actual QUrl parsing
    {
        const auto resDots = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl::fromEncoded("thispc:/vol1/%2e%2e/%2e%2e/outside/sentinel.txt"), mountRoot, &mountStat);
        kcVerify(!resDots.ok && resDots.kioError == KIO::ERR_ACCESS_DENIED,
                 "K7a: %2e%2e encoded parent traversal rejected");

        const auto resSlash = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl::fromEncoded("thispc:/vol1/..%2f..%2foutside%2fsentinel.txt"), mountRoot, &mountStat);
        kcVerify(!resSlash.ok && resSlash.kioError == KIO::ERR_MALFORMED_URL,
                 "K7b: %2f encoded slash separator rejected");

        const auto resUpperSlash = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl::fromEncoded("thispc:/vol1/sub%2Ffile.txt"), mountRoot, &mountStat);
        kcVerify(!resUpperSlash.ok && resUpperSlash.kioError == KIO::ERR_MALFORMED_URL,
                 "K7c: %2F encoded slash rejected");
    }

    // K8 — Double encoding (%252e%252e) does NOT decode a second time into ".."
    {
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl::fromEncoded("thispc:/vol1/%252e%252e/file.txt"), mountRoot, &mountStat);
        kcVerify(res.ok && ThisPcConfinement::isWithinDirectory(res.redirectedUrl.toLocalFile(), canonMount)
            && res.redirectedUrl.toLocalFile() == canonMount + QStringLiteral("/%2e%2e/file.txt"),
                 "K8: double-encoded %252e%252e decodes once to literal %2e%2e inside mountRoot");
    }

    // K9 — Embedded NUL (%00), malformed percent-encoding, query/fragment rejected
    {
        const auto resNul = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl::fromEncoded("thispc:/vol1/file%00.txt"), mountRoot, &mountStat);
        kcVerify(!resNul.ok && resNul.kioError == KIO::ERR_MALFORMED_URL,
                 "K9a: %00 encoded NUL rejected");

        const auto resBadPctStrict = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl::fromEncoded("thispc:/vol1/bad%2Gname", QUrl::StrictMode), mountRoot, &mountStat);
        kcVerify(!resBadPctStrict.ok && resBadPctStrict.kioError == KIO::ERR_MALFORMED_URL,
                 "K9b: StrictMode malformed percent-encoding rejected");
        QString rawPctErr;
        kcVerify(!ThisPcConfinement::Detail::validateRawEncodedPath("/vol1/bad%2Gname", &rawPctErr),
                 "K9b2: validateRawEncodedPath rejects malformed percent-encoding");

        const auto resQuery = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/nested?foo=bar")), mountRoot, &mountStat);
        kcVerify(!resQuery.ok && resQuery.kioError == KIO::ERR_MALFORMED_URL,
                 "K9c: unexpected query rejected");

        const auto resFrag = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/nested#frag")), mountRoot, &mountStat);
        kcVerify(!resFrag.ok && resFrag.kioError == KIO::ERR_MALFORMED_URL,
                 "K9d: unexpected fragment rejected");
    }

    // K10 — Valid Unicode and spaces preserved
    {
        QUrl u;
        u.setScheme(QStringLiteral("thispc"));
        u.setPath(QStringLiteral("/vol1/Zażółć gęślą jaźń/Raport 2026.txt"));
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(u, mountRoot, &mountStat);
        kcVerify(res.ok && res.redirectedUrl == QUrl::fromLocalFile(canonMount + QStringLiteral("/Zażółć gęślą jaźń/Raport 2026.txt")),
                 "K10: valid Unicode and spaces resolve accurately inside mountRoot");
    }

    // K11 — Safe symlink staying inside mountRoot is permitted
    {
        const QString safeLink = mountRoot + QStringLiteral("/safe_link.txt");
        kcVerify(::symlink(QFile::encodeName(childFile).constData(), QFile::encodeName(safeLink).constData()) == 0,
                 "K11 create safe in-volume symlink");
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/safe_link.txt")), mountRoot, &mountStat);
        kcVerify(res.ok && ThisPcConfinement::isWithinDirectory(res.redirectedUrl.toLocalFile(), canonMount),
                 "K11: safe in-volume symlink allowed");
    }

    // K12 — Escaping symlink pointing outside mountRoot is rejected fail-closed
    {
        const QString escapeFileLink = mountRoot + QStringLiteral("/escape_file");
        const QString escapeDirLink = mountRoot + QStringLiteral("/escape_dir");
        kcVerify(::symlink(QFile::encodeName(outsideSentinel).constData(), QFile::encodeName(escapeFileLink).constData()) == 0,
                 "K12 create escaping file symlink");
        kcVerify(::symlink(QFile::encodeName(outsideDir).constData(), QFile::encodeName(escapeDirLink).constData()) == 0,
                 "K12 create escaping dir symlink");

        const auto resFile = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/escape_file")), mountRoot, &mountStat);
        kcVerify(!resFile.ok && resFile.kioError == KIO::ERR_ACCESS_DENIED,
                 "K12a: escaping file symlink rejected");

        const auto resDir = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/escape_dir/sentinel.txt")), mountRoot, &mountStat);
        kcVerify(!resDir.ok && resDir.kioError == KIO::ERR_ACCESS_DENIED,
                 "K12b: escaping intermediate dir symlink rejected");
    }

    // K13 — Deterministic symlink substitution between initial check and final re-check
    {
        const QString swapTarget = mountRoot + QStringLiteral("/swap_me.txt");
        kcWriteFile(swapTarget, "legitimate-inside");
        bool hookFired = false;
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/swap_me.txt")),
            mountRoot,
            &mountStat,
            [&](const QString &cand) {
                hookFired = true;
                ::unlink(QFile::encodeName(cand).constData());
                ::symlink(QFile::encodeName(outsideSentinel).constData(), QFile::encodeName(cand).constData());
            });
        kcVerify(hookFired && !res.ok && res.kioError == KIO::ERR_ACCESS_DENIED,
                 "K13: mid-resolution symlink swap caught by re-verification and rejected fail-closed");
    }

    // K14 — Missing child stays confined and does NOT create any file or fall back to / or $HOME
    {
        const QString missingPath = canonMount + QStringLiteral("/does/not/exist.txt");
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol1/does/not/exist.txt")), mountRoot, &mountStat);
        kcVerify(res.ok && res.redirectedUrl == QUrl::fromLocalFile(missingPath)
            && !QFileInfo::exists(missingPath),
                 "K14: missing child stays strictly confined and creates no file");
    }

    // K15 — Removed mount point fails safely
    {
        const QString goneMount = tmp.filePath(QStringLiteral("mnt/removed_mount"));
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol_gone/sub")), goneMount);
        kcVerify(!res.ok && res.kioError == KIO::ERR_DOES_NOT_EXIST,
                 "K15: removed mount point fails closed");
    }

    // K16 — Replaced mount-root directory (distinct inode) fails identity check
    {
        const QString origMount = tmp.filePath(QStringLiteral("mnt/repl_test"));
        const QString movedMount = tmp.filePath(QStringLiteral("mnt/repl_moved"));
        kcVerify(QDir().mkpath(origMount), "K16 origMount");
        struct stat origMountSt {};
        kcVerify(::lstat(QFile::encodeName(origMount).constData(), &origMountSt) == 0, "K16 lstat origMount");
        kcVerify(::rename(QFile::encodeName(origMount).constData(), QFile::encodeName(movedMount).constData()) == 0
            && ::mkdir(QFile::encodeName(origMount).constData(), 0700) == 0,
                 "K16 replace mount dir with new inode");
        const auto res = ThisPcConfinement::resolveConfinedLocalUrl(
            QUrl(QStringLiteral("thispc:/vol_repl")), origMount, &origMountSt);
        kcVerify(!res.ok && res.kioError == KIO::ERR_DOES_NOT_EXIST,
                 "K16: replaced mount root directory (new inode) rejected fail-closed");
    }

    // K17 — Root thispc:/ parses cleanly with empty parts (volume listing preserved)
    {
        QStringList rootParts;
        int err = 0;
        QString detail;
        kcVerify(ThisPcConfinement::parseThisPcUrl(QUrl(QStringLiteral("thispc:/")), &rootParts, &err, &detail)
            && rootParts.isEmpty(),
                 "K17: root thispc:/ parses cleanly for volume listing");
    }

    // K20 — External sentinels unchanged (SHA-256 + metadata)
    {
        struct stat outsideStatAfter {};
        kcVerify(::lstat(QFile::encodeName(outsideSentinel).constData(), &outsideStatAfter) == 0,
                 "K20 lstat outside sentinel after");
        kcVerify(kcSha256(outsideSentinel) == hOutsideBefore
            && outsideStatAfter.st_ino == outsideStatBefore.st_ino
            && outsideStatAfter.st_mode == outsideStatBefore.st_mode
            && kcSha256(siblingSentinel) == hSiblingBefore,
                 "K20: external and prefix-sibling sentinels remain byte-identical with unchanged metadata");
    }

    // ==================== Stage 1B.9B-3B-R2: Production Mount Identity (MI1–MI12) ====================

    // MI1 & MI7 & MI9 & MI10 — Production captureMountIdentity + redirectVolumeUrl on valid root, nested Unicode, and safe symlink
    {
        struct stat snap {};
        const bool snapOk = ThisPcConfinement::captureMountIdentity(mountRoot, &snap);
        kcVerify(snapOk, "MI1: captureMountIdentity succeeds for valid mountRoot");

        const auto rRoot = ThisPcConfinement::redirectVolumeUrl(
            QUrl(QStringLiteral("thispc:/vol1")), mountRoot, snap, snapOk, false, {});
        kcVerify(rRoot.ok && rRoot.redirectedUrl == QUrl::fromLocalFile(canonMount),
                 "MI1/MI10: redirectVolumeUrl succeeds for matching root identity");

        QUrl unicodeUrl;
        unicodeUrl.setScheme(QStringLiteral("thispc"));
        unicodeUrl.setPath(QStringLiteral("/vol1/Zażółć gęślą jaźń/Raport 2026.txt"));
        const auto rUni = ThisPcConfinement::redirectVolumeUrl(
            unicodeUrl, mountRoot, snap, snapOk, false, {});
        kcVerify(rUni.ok && rUni.redirectedUrl == QUrl::fromLocalFile(canonMount + QStringLiteral("/Zażółć gęślą jaźń/Raport 2026.txt")),
                 "MI7: redirectVolumeUrl preserves nested Unicode and spaces");

        const auto rSafeLink = ThisPcConfinement::redirectVolumeUrl(
            QUrl(QStringLiteral("thispc:/vol1/safe_link.txt")), mountRoot, snap, snapOk, false, {});
        kcVerify(rSafeLink.ok, "MI9a: in-volume symlink preserved through redirectVolumeUrl");

        const auto rEscLink = ThisPcConfinement::redirectVolumeUrl(
            QUrl(QStringLiteral("thispc:/vol1/escape_file")), mountRoot, snap, snapOk, false, {});
        kcVerify(!rEscLink.ok && rEscLink.kioError == KIO::ERR_ACCESS_DENIED,
                 "MI9b: escaping symlink rejected through redirectVolumeUrl");
    }

    // MI2 & MI3 — Replacement after discovery & re-created same-name path rejected fail-closed
    {
        const QString mi2Mount = tmp.filePath(QStringLiteral("mnt/mi2_mount"));
        const QString mi2Aside = tmp.filePath(QStringLiteral("mnt/mi2_aside"));
        const QString mi2Other = tmp.filePath(QStringLiteral("mnt/mi2_other"));
        kcVerify(QDir().mkpath(mi2Mount) && QDir().mkpath(mi2Other), "MI2 setup");
        struct stat mi2Snap {};
        const bool mi2SnapOk = ThisPcConfinement::captureMountIdentity(mi2Mount, &mi2Snap);
        kcVerify(mi2SnapOk, "MI2 captureMountIdentity");

        // Replace with distinct directory
        kcVerify(::rename(QFile::encodeName(mi2Mount).constData(), QFile::encodeName(mi2Aside).constData()) == 0
            && ::rename(QFile::encodeName(mi2Other).constData(), QFile::encodeName(mi2Mount).constData()) == 0,
                 "MI2 swap mount root");
        const auto rSwap = ThisPcConfinement::redirectVolumeUrl(
            QUrl(QStringLiteral("thispc:/mi2/file.txt")), mi2Mount, mi2Snap, mi2SnapOk, false, {});
        kcVerify(!rSwap.ok && rSwap.kioError == KIO::ERR_DOES_NOT_EXIST,
                 "MI2: replaced mount root rejected by redirectVolumeUrl");

        // Remove and recreate at same pathname (new st_ino)
        kcVerify(::rmdir(QFile::encodeName(mi2Mount).constData()) == 0
            && ::mkdir(QFile::encodeName(mi2Mount).constData(), 0700) == 0,
                 "MI3 recreate same-name root");
        const auto rRecreated = ThisPcConfinement::redirectVolumeUrl(
            QUrl(QStringLiteral("thispc:/mi2/file.txt")), mi2Mount, mi2Snap, mi2SnapOk, false, {});
        kcVerify(!rRecreated.ok && rRecreated.kioError == KIO::ERR_DOES_NOT_EXIST,
                 "MI3: recreated same-name mount root rejected via (st_dev, st_ino)");
    }

    // MI4 — Symlink replacement of mount root to external sentinel dir rejected without external mutation
    {
        const QString mi4Mount = tmp.filePath(QStringLiteral("mnt/mi4_mount"));
        const QString mi4Aside = tmp.filePath(QStringLiteral("mnt/mi4_aside"));
        kcVerify(QDir().mkpath(mi4Mount), "MI4 setup");
        struct stat mi4Snap {};
        const bool mi4SnapOk = ThisPcConfinement::captureMountIdentity(mi4Mount, &mi4Snap);
        kcVerify(mi4SnapOk, "MI4 captureMountIdentity");

        kcVerify(::rename(QFile::encodeName(mi4Mount).constData(), QFile::encodeName(mi4Aside).constData()) == 0
            && ::symlink(QFile::encodeName(outsideDir).constData(), QFile::encodeName(mi4Mount).constData()) == 0,
                 "MI4 symlink swap mount root");
        const auto rSym = ThisPcConfinement::redirectVolumeUrl(
            QUrl(QStringLiteral("thispc:/mi4/sentinel.txt")), mi4Mount, mi4Snap, mi4SnapOk, false, {});
        kcVerify(!rSym.ok && rSym.kioError == KIO::ERR_ACCESS_DENIED,
                 "MI4: symlink-swapped mount root rejected fail-closed");
    }

    // MI5 & MI10 — Missing root & invalid snapshot fail closed without silent downgrade
    {
        const QString mi5Missing = tmp.filePath(QStringLiteral("mnt/mi5_missing"));
        struct stat emptySnap {};
        const bool mi5SnapOk = ThisPcConfinement::captureMountIdentity(mi5Missing, &emptySnap);
        kcVerify(!mi5SnapOk, "MI5: captureMountIdentity returns false for missing mount");

        // Even if mountRoot exists on disk, missing snapshot (snapOk=false) MUST fail closed
        const auto rNoSnap = ThisPcConfinement::redirectVolumeUrl(
            QUrl(QStringLiteral("thispc:/vol1/nested/sub/doc.txt")), mountRoot, emptySnap, false, false, {});
        kcVerify(!rNoSnap.ok && rNoSnap.kioError == KIO::ERR_DOES_NOT_EXIST,
                 "MI5/MI10: missing mount identity snapshot fails closed without silent downgrade");
    }

    // MI6 — Root fallback "/" works cleanly with captureMountIdentity + redirectVolumeUrl
    {
        struct stat rootSnap {};
        const bool rootSnapOk = ThisPcConfinement::captureMountIdentity(QStringLiteral("/"), &rootSnap);
        kcVerify(rootSnapOk, "MI6: captureMountIdentity('/') succeeds");
        const auto rSysRoot = ThisPcConfinement::redirectVolumeUrl(
            QUrl(QStringLiteral("thispc:/sysroot/tmp")), QStringLiteral("/"), rootSnap, rootSnapOk, false, {});
        kcVerify(rSysRoot.ok && rSysRoot.redirectedUrl == QUrl::fromLocalFile(QStringLiteral("/tmp")),
                 "MI6: '/' root fallback resolves cleanly with identity snapshot");
    }

    // MI8 — MTP compatibility preserved without POSIX FD requirements
    {
        struct stat emptySnap {};
        const auto rMtp = ThisPcConfinement::redirectVolumeUrl(
            QUrl(QStringLiteral("thispc:/mtp_dev/DCIM")), QString(), emptySnap, false, true, QStringLiteral("mtp:udi=/org/kde/mtp/1"));
        kcVerify(rMtp.ok && rMtp.redirectedUrl == QUrl(QStringLiteral("mtp:udi=/org/kde/mtp/1")),
                 "MI8: MTP volume redirection preserved without POSIX FD requirement");
    }

    // MI12 — External sentinels remain byte-identical with unchanged metadata after MI1–MI11
    {
        struct stat outsideStatEnd {};
        kcVerify(::lstat(QFile::encodeName(outsideSentinel).constData(), &outsideStatEnd) == 0,
                 "MI12 lstat outside sentinel");
        kcVerify(kcSha256(outsideSentinel) == hOutsideBefore
            && outsideStatEnd.st_ino == outsideStatBefore.st_ino
            && outsideStatEnd.st_mode == outsideStatBefore.st_mode
            && kcSha256(siblingSentinel) == hSiblingBefore,
                 "MI12: external sentinels remain byte-identical with unchanged metadata");
    }

    qInfo("PASS: %d KIO thispc:/ mount confinement assertions verified (K1–K22 + MI1–MI12)", kioConfChecks);
    return 0;
}
