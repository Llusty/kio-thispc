/* Deterministic Properties capability/identity foundation regression. */
#include "propertiescapabilities.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <sys/stat.h>

static int checks = 0;
static void verify(bool condition, const char *message)
{
    ++checks;
    if (!condition) qFatal("FAIL: %s", message);
}

static QString makeFile(const QString &path, const QByteArray &bytes = "unchanged")
{
    QFile file(path);
    verify(file.open(QIODevice::WriteOnly), "fixture file opens");
    verify(file.write(bytes) == bytes.size(), "fixture bytes written");
    file.close();
    return path;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    verify(temp.isValid(), "temporary directory");

    const QString file = makeFile(temp.filePath(QStringLiteral("plain file.txt")));
    verify(::chmod(QFile::encodeName(file).constData(), 0640) == 0, "fixture mode");
    const auto localFile = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(file));
    verify(localFile.isLocal && !localFile.isRemote, "file is local only");
    verify(localFile.entryKind == PropertiesEntryKind::RegularFile, "regular file kind");
    verify(localFile.entryIdentity.valid, "file identity captured");
    verify(localFile.symlinkPolicy == PropertiesSymlinkPolicy::NotSymlink, "file no-link policy");
    verify(localFile.posixModeReadable == PropertiesCapabilityState::Supported, "file mode readable");
    verify(localFile.posixModeEditable == PropertiesCapabilityState::Supported, "file mode editable");
    verify(localFile.dotNameHiddenApplicable == PropertiesCapabilityState::Supported, "dot-name applies locally");
    verify(!localFile.dotNameHidden, "ordinary file not dot-hidden");
    verify(localFile.nativeHiddenReadable == PropertiesCapabilityState::Unsupported, "no invented local native hidden flag");
    verify(localFile.protectedXattrEditable == PropertiesCapabilityState::Unsupported, "protected xattr namespaces never editable");
    verify(PropertiesCapabilityResolver::revalidate(localFile) == PropertiesRevalidationResult::SameTarget,
           "unchanged file revalidates");

    const QString folder = temp.filePath(QStringLiteral("folder żółty"));
    verify(QDir().mkpath(folder), "unicode folder fixture");
    const auto localDir = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(folder));
    verify(localDir.entryKind == PropertiesEntryKind::Directory, "directory kind");
    verify(localDir.normalizedUrl.toLocalFile() == QDir::cleanPath(folder), "unicode/spaces preserved");

    const QString hidden = makeFile(temp.filePath(QStringLiteral(".secret")));
    const auto hiddenCaps = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(hidden));
    verify(hiddenCaps.dotNameHidden, "leading dot reported separately");

    const QString fileLink = temp.filePath(QStringLiteral("file-link"));
    verify(QFile::link(file, fileLink), "file symlink fixture");
    const auto fileLinkCaps = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(fileLink));
    verify(fileLinkCaps.entryKind == PropertiesEntryKind::SymbolicLink, "file symlink entry kind");
    verify(fileLinkCaps.symlinkPolicy == PropertiesSymlinkPolicy::EntryNoFollow, "file symlink no-follow policy");
    verify(fileLinkCaps.symlinkTargetStatus == PropertiesTargetStatus::Present, "file link target present");
    verify(fileLinkCaps.targetIdentity.valid, "file link target identity separate");
    verify(!(fileLinkCaps.entryIdentity == fileLinkCaps.targetIdentity), "link and target identities differ");
    verify(fileLinkCaps.posixModeEditable == PropertiesCapabilityState::Unsupported, "symlink own mode not editable");
    verify(fileLinkCaps.userXattrEditable == PropertiesCapabilityState::Unsupported, "symlink xattr writes not promised");

    const QString dirLink = temp.filePath(QStringLiteral("dir-link"));
    verify(QFile::link(folder, dirLink), "directory symlink fixture");
    const auto dirLinkCaps = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(dirLink));
    verify(dirLinkCaps.entryKind == PropertiesEntryKind::SymbolicLink, "directory symlink entry kind");
    verify(dirLinkCaps.symlinkTargetStatus == PropertiesTargetStatus::Present, "directory link target present");

    const QString broken = temp.filePath(QStringLiteral("broken-link"));
    verify(QFile::link(temp.filePath(QStringLiteral("absent")), broken), "broken symlink fixture");
    const auto brokenCaps = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(broken));
    verify(brokenCaps.entryKind == PropertiesEntryKind::SymbolicLink, "broken symlink remains an entry");
    verify(brokenCaps.symlinkTargetStatus == PropertiesTargetStatus::Broken, "broken target status");
    verify(brokenCaps.entryIdentity.valid && !brokenCaps.targetIdentity.valid, "broken link keeps only entry identity");

    const QString missing = temp.filePath(QStringLiteral("missing"));
    const auto missingCaps = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(missing));
    verify(missingCaps.entryKind == PropertiesEntryKind::Missing, "missing target classified");
    verify(!missingCaps.entryIdentity.valid, "missing target has no identity");
    verify(PropertiesCapabilityResolver::revalidate(missingCaps) == PropertiesRevalidationResult::Unknown,
           "no fabricated revalidation without snapshot");

    const QString replaced = makeFile(temp.filePath(QStringLiteral("replace-me")), "old");
    const auto replacedSnapshot = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(replaced));
    verify(QFile::remove(replaced), "old replacement target removed");
    makeFile(temp.filePath(QStringLiteral("inode-slot")), "slot");
    makeFile(replaced, "new");
    verify(PropertiesCapabilityResolver::revalidate(replacedSnapshot) == PropertiesRevalidationResult::Replaced,
           "same path new inode is replaced");

    const QString moved = makeFile(temp.filePath(QStringLiteral("move-me")));
    const auto movedSnapshot = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(moved));
    const QString movedTo = temp.filePath(QStringLiteral("moved-here"));
    verify(QFile::rename(moved, movedTo), "same inode renamed");
    verify(PropertiesCapabilityResolver::revalidate(movedSnapshot) == PropertiesRevalidationResult::Missing,
           "path-bound revalidation reports rename as missing");
    const auto movedNow = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(movedTo));
    verify(movedSnapshot.entryIdentity == movedNow.entryIdentity, "identity itself survives rename");

    int localCalls = 0;
    PropertiesCapabilityResolver::LocalSyscalls forbidden;
    forbidden.lstatFn = [&localCalls](const char *, struct stat *) { ++localCalls; errno = EIO; return -1; };
    forbidden.statFn = [&localCalls](const char *, struct stat *) { ++localCalls; errno = EIO; return -1; };
    forbidden.accessFn = [&localCalls](const char *, int) { ++localCalls; errno = EIO; return -1; };
    forbidden.listXattrFn = [&localCalls](const char *, char *, size_t) { ++localCalls; errno = EIO; return -1; };
    for (const QString &urlText : {QStringLiteral("sftp://host/a"), QStringLiteral("smb://host/a"),
                                  QStringLiteral("fish://host/a"), QStringLiteral("https://host/a")}) {
        const auto remote = PropertiesCapabilityResolver::resolve(QUrl(urlText), forbidden);
        verify(remote.isRemote && !remote.isLocal, "remote URL remains remote");
        verify(remote.normalizedUrl.scheme() == QUrl(urlText).scheme(), "remote scheme preserved");
        verify(remote.xattrReadable == PropertiesCapabilityState::Unknown, "remote xattr not fabricated");
        verify(PropertiesCapabilityResolver::revalidate(remote, forbidden) == PropertiesRevalidationResult::Unsupported,
               "remote identity revalidation unsupported");
    }
    verify(localCalls == 0, "remote resolution performs zero local syscalls");

    auto remote = PropertiesCapabilityResolver::resolve(QUrl(QStringLiteral("sftp://host/link")), forbidden);
    KIO::UDSEntry remoteEntry;
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG);
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_ACCESS, 0644);
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_HIDDEN, 1);
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_LINK_DEST, QStringLiteral("target"));
    PropertiesCapabilityResolver::applyKioEntry(&remote, remoteEntry);
    verify(remote.entryKind == PropertiesEntryKind::SymbolicLink, "KIO link uses UDS_LINK_DEST not file type");
    verify(remote.posixModeReadable == PropertiesCapabilityState::Supported, "KIO advertised mode readable");
    verify(remote.posixModeEditable == PropertiesCapabilityState::Unknown, "KIO mode write not promised");
    verify(remote.nativeHiddenReadable == PropertiesCapabilityState::Supported && remote.nativeHidden,
           "KIO native hidden flag read separately");
    verify(remote.nativeHiddenEditable == PropertiesCapabilityState::Unknown, "KIO hidden write not promised");
    verify(localCalls == 0, "KIO entry application performs zero local syscalls");

    PropertiesCapabilityResolver::LocalSyscalls xattrEmpty;
    xattrEmpty.listXattrFn = [](const char *, char *, size_t) { return ssize_t(0); };
    const auto emptyXattr = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(file), xattrEmpty);
    verify(emptyXattr.xattrProbe == PropertiesXattrProbe::SupportedEmpty, "supported empty xattr distinguished");
    verify(emptyXattr.xattrReadable == PropertiesCapabilityState::Supported, "empty xattr remains supported");
    verify(emptyXattr.userXattrEditable == PropertiesCapabilityState::Supported, "user namespace write model");

    PropertiesCapabilityResolver::LocalSyscalls xattrPresent;
    xattrPresent.listXattrFn = [](const char *, char *, size_t) { return ssize_t(12); };
    verify(PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(file), xattrPresent).xattrProbe
               == PropertiesXattrProbe::SupportedPresent,
           "present xattrs distinguished");
    PropertiesCapabilityResolver::LocalSyscalls xattrUnsupported;
    xattrUnsupported.listXattrFn = [](const char *, char *, size_t) { errno = ENOTSUP; return ssize_t(-1); };
    const auto unsupportedXattr = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(file), xattrUnsupported);
    verify(unsupportedXattr.xattrProbe == PropertiesXattrProbe::Unsupported, "unsupported xattr classified");
    verify(unsupportedXattr.userXattrEditable == PropertiesCapabilityState::Unsupported, "unsupported xattr not editable");
    PropertiesCapabilityResolver::LocalSyscalls xattrDenied;
    xattrDenied.listXattrFn = [](const char *, char *, size_t) { errno = EACCES; return ssize_t(-1); };
    verify(PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(file), xattrDenied).xattrProbe
               == PropertiesXattrProbe::PermissionDenied,
           "xattr permission error classified");

    // Resolution and revalidation themselves must not mutate content or metadata.
    struct stat before {}, after {};
    verify(::lstat(QFile::encodeName(file).constData(), &before) == 0, "pre-resolution stat");
#ifdef __linux__
    const QByteArray encodedFile = QFile::encodeName(file);
    const ssize_t xattrBeforeSize = ::llistxattr(encodedFile.constData(), nullptr, 0);
    QByteArray xattrsBefore;
    if (xattrBeforeSize >= 0) {
        xattrsBefore.resize(xattrBeforeSize);
        verify(::llistxattr(encodedFile.constData(), xattrsBefore.data(), xattrsBefore.size()) == xattrBeforeSize,
               "xattr names captured before");
    }
#endif
    QFile readBefore(file);
    verify(readBefore.open(QIODevice::ReadOnly), "read mutation fixture before");
    const QByteArray contentBefore = readBefore.readAll();
    readBefore.close();
    const auto noMutation = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(file));
    verify(PropertiesCapabilityResolver::revalidate(noMutation) == PropertiesRevalidationResult::SameTarget,
           "no-mutation snapshot revalidates");
    verify(::lstat(QFile::encodeName(file).constData(), &after) == 0, "post-resolution stat");
    QFile readAfter(file);
    verify(readAfter.open(QIODevice::ReadOnly), "read mutation fixture after");
    verify(readAfter.readAll() == contentBefore, "bytes unchanged by resolve/revalidate");
    verify((before.st_mode & 07777) == (after.st_mode & 07777), "mode unchanged by resolve/revalidate");
    verify(before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec,
           "mtime unchanged by resolve/revalidate");
#ifdef __linux__
    const ssize_t xattrAfterSize = ::llistxattr(encodedFile.constData(), nullptr, 0);
    verify(xattrAfterSize == xattrBeforeSize, "xattr list size unchanged by resolve/revalidate");
    if (xattrAfterSize >= 0) {
        QByteArray xattrsAfter(xattrAfterSize, Qt::Uninitialized);
        verify(::llistxattr(encodedFile.constData(), xattrsAfter.data(), xattrsAfter.size()) == xattrAfterSize,
               "xattr names captured after");
        verify(xattrsAfter == xattrsBefore, "xattr names unchanged by resolve/revalidate");
    }
#endif

    // Primary, Split, and Search all pass the selected URL to the same resolver.
    const auto primary = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(file));
    const auto split = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(file));
    const auto search = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(file));
    verify(primary.normalizedUrl == split.normalizedUrl && split.normalizedUrl == search.normalizedUrl,
           "Primary/Split/Search normalized URL parity");
    verify(primary.entryIdentity == split.entryIdentity && split.entryIdentity == search.entryIdentity,
           "Primary/Split/Search identity parity");

    qInfo("PASS: %d Properties capability assertions; local/remote, symlinks, identity, xattr, POSIX, hidden, parity, no mutation", checks);
}
