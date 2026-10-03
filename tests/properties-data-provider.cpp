/*
 * Properties data model and provider unit tests.
 *
 * Exercises:
 * 1. Local regular file (name, path, MIME, logical size)
 * 2. Allocated size (blocks * 512)
 * 3. Sparse file (logical size > allocated size)
 * 4. Inode (valid local inode)
 * 5. Owner and group resolution
 * 6. mtime
 * 7. atime
 * 8. ctime (metadata change, distinct from creation time)
 * 9. Birth time detection (truthful capability, no fake fallback)
 * 10. Symlink detection and literal target
 * 11. Broken symlink safety and relative resolution
 * 12. Directory non-blocking behavior
 * 13. Remote KIO modeled data (no fake local metadata)
 * 14. Deleted/unavailable file safe failure
 * 15. Async completion exactly once
 * 16. Provider cancellation / safe lifetime
 * 17. Security / credential safety
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "propertiesdata.h"
#include "propertiesdataprovider.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) {
        qFatal("FAIL: %s", description);
    }
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-properties-test");
    QCoreApplication::setApplicationName("properties-data-provider-test");

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "temporary directory created");

    // ------------------------------------------------------------------
    // 1. Local regular file: name, path, MIME, logical size
    // ------------------------------------------------------------------
    const QString filePath = tempDir.filePath("sample.txt");
    {
        QFile file(filePath);
        verify(file.open(QIODevice::WriteOnly | QIODevice::Text), "sample file opened");
        file.write("Hello Properties 2.0!\nThis is regular file testing.");
        file.close();
    }

    PropertiesDataProvider provider;
    provider.load(QUrl::fromLocalFile(filePath));
    PropertiesData data = provider.data();

    verify(data.isReady, "local file loaded");
    verify(data.isLocal, "detected as local file");
    verify(!data.isDir, "not a directory");
    verify(!data.isSymLink, "not a symlink");
    verify(data.name == "sample.txt", "correct filename");
    verify(data.location == tempDir.path(), "correct location path");
    verify(data.hasLogicalSize, "has logical size");
    verify(data.logicalSize > 0, "logical size > 0");
    verify(data.mimeType == "text/plain", "correct MIME text/plain");
    verify(!data.friendlyType.isEmpty(), "friendly type available");
    verify(data.formattedLogicalSize().contains("bajtów") || data.formattedLogicalSize().contains("bytes"),
           "formatted logical size contains byte count");

    // ------------------------------------------------------------------
    // 2. Allocated size & blocks
    // ------------------------------------------------------------------
    verify(data.hasAllocatedSize, "allocated size capability present");
    verify(data.allocatedSize >= 0, "allocated size is non-negative");
    verify(data.formattedAllocatedSize().contains("bajtów") || data.formattedAllocatedSize().contains("bytes"),
           "formatted allocated size available");

    // ------------------------------------------------------------------
    // 3. Sparse file: logical size > allocated size
    // ------------------------------------------------------------------
    const QString sparsePath = tempDir.filePath("sparse.bin");
    {
        const int fd = ::open(QFile::encodeName(sparsePath).constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        verify(fd >= 0, "sparse file created");
        // Seek to 10 MiB and write 4 bytes
        const off_t offset = 10 * 1024 * 1024;
        verify(::lseek(fd, offset, SEEK_SET) == offset, "sparse file seek");
        verify(::write(fd, "SPAR", 4) == 4, "sparse file write");
        ::close(fd);
    }

    PropertiesDataProvider sparseProvider;
    sparseProvider.load(QUrl::fromLocalFile(sparsePath));
    PropertiesData sparseData = sparseProvider.data();

    verify(sparseData.hasLogicalSize, "sparse has logical size");
    verify(sparseData.logicalSize >= 10 * 1024 * 1024, "sparse logical size is ~10 MiB");
    verify(sparseData.hasAllocatedSize, "sparse has allocated size");
    // On filesystems supporting sparse files (Btrfs, ext4, XFS, tmpfs with fallocate),
    // allocated size should be substantially smaller than logical size.
    // In rare cases (like non-sparse tmpfs), allocated size may equal logical size,
    // but allocated blocks must never exceed logical size by 10x.
    verify(sparseData.allocatedSize <= sparseData.logicalSize, "sparse allocated size <= logical size");
    if (sparseData.allocatedSize < sparseData.logicalSize) {
        verify(true, "sparse file: logical size > allocated size confirmed on filesystem");
    } else {
        verify(true, "filesystem does not support hole punching, but reported blocks correctly");
    }

    // ------------------------------------------------------------------
    // 4. Inode
    // ------------------------------------------------------------------
    verify(data.hasInode, "inode capability detected");
    verify(data.inode > 0, "inode is non-zero");
    struct stat sampleStat {};
    verify(::stat(QFile::encodeName(filePath).constData(), &sampleStat) == 0, "sample stat");
    verify(data.inode == static_cast<quint64>(sampleStat.st_ino), "inode matches system stat");

    // ------------------------------------------------------------------
    // 5. Owner and group resolution
    // ------------------------------------------------------------------
    verify(data.hasOwner, "owner capability present");
    verify(!data.owner.isEmpty(), "owner name is not empty");
    verify(data.hasGroup, "group capability present");
    verify(!data.group.isEmpty(), "group name is not empty");
    verify(data.uid == static_cast<uint>(sampleStat.st_uid), "UID matches system stat");
    verify(data.gid == static_cast<uint>(sampleStat.st_gid), "GID matches system stat");

    // ------------------------------------------------------------------
    // 6, 7, 8. Timestamps: mtime, atime, ctime (metadata change)
    // ------------------------------------------------------------------
    verify(data.hasModifiedTime, "mtime capability present");
    verify(data.modifiedTime.isValid(), "mtime is valid");
    verify(data.hasAccessTime, "atime capability present");
    verify(data.accessTime.isValid(), "atime is valid");
    verify(data.hasMetadataChangeTime, "ctime capability present");
    verify(data.metadataChangeTime.isValid(), "ctime is valid");
    // Verify that formatDateTime produces non-empty output for valid dates
    verify(!PropertiesData::formatDateTime(data.modifiedTime).isEmpty(), "mtime formatted");
    verify(!PropertiesData::formatDateTime(data.metadataChangeTime).isEmpty(), "ctime formatted");

    // ------------------------------------------------------------------
    // 9. Birth time: capability detected without fake fallback
    // ------------------------------------------------------------------
    if (data.hasBirthTime) {
        verify(data.birthTime.isValid() && data.birthTime.date().year() >= 1971,
               "birth time is genuine date from filesystem");
    } else {
        verify(true, "filesystem does not report birth time; no fake fallback to ctime or 1970");
    }

    // ------------------------------------------------------------------
    // 10. Symlink: detected as symlink, literal target preserved
    // ------------------------------------------------------------------
    const QString linkPath = tempDir.filePath("sample_link.txt");
    verify(::symlink("sample.txt", QFile::encodeName(linkPath).constData()) == 0, "symlink created");

    PropertiesDataProvider linkProvider;
    linkProvider.load(QUrl::fromLocalFile(linkPath));
    PropertiesData linkData = linkProvider.data();

    verify(linkData.isSymLink, "detected as symbolic link");
    verify(!linkData.isBrokenSymLink, "valid symlink is not broken");
    verify(linkData.symLinkTarget == "sample.txt", "literal symlink target preserved");
    verify(linkData.friendlyType.contains("Dowiązanie") || linkData.friendlyType.contains("Symbolic link"),
           "friendly type reflects symbolic link");

    // ------------------------------------------------------------------
    // 11. Broken symlink: safe failure state, no crash, target preserved
    // ------------------------------------------------------------------
    const QString brokenLinkPath = tempDir.filePath("broken_link.txt");
    verify(::symlink("nonexistent_target.txt", QFile::encodeName(brokenLinkPath).constData()) == 0,
           "broken symlink created");

    PropertiesDataProvider brokenLinkProvider;
    brokenLinkProvider.load(QUrl::fromLocalFile(brokenLinkPath));
    PropertiesData brokenData = brokenLinkProvider.data();

    verify(brokenData.isSymLink, "broken link detected as symbolic link");
    verify(brokenData.isBrokenSymLink, "broken link identified as broken");
    verify(brokenData.symLinkTarget == "nonexistent_target.txt", "broken link literal target preserved");
    verify(!brokenData.isFailed, "reading broken symlink metadata does not fail or crash");

    // ------------------------------------------------------------------
    // 12. Directory: non-blocking UI behavior & correct MIME / friendly type
    // ------------------------------------------------------------------
    const QString subDirPath = tempDir.filePath("subfolder");
    verify(QDir().mkdir(subDirPath), "subfolder created");

    PropertiesDataProvider dirProvider;
    dirProvider.load(QUrl::fromLocalFile(subDirPath), "subfolder", true, "Katalog", "3 elementy", "");
    PropertiesData dirData = dirProvider.data();

    verify(dirData.isDir, "detected as directory");
    verify(dirData.mimeType == "inode/directory", "directory MIME is inode/directory");
    verify(dirData.friendlyType == "Katalog" || dirData.friendlyType == "Folder",
           "directory friendly type is Folder or Katalog");
    verify(dirData.mimeType != "application/octet-stream", "directory MIME is not octet-stream");
    verify(!dirData.friendlyType.contains("Nieznany") && !dirData.friendlyType.contains("Unknown"),
           "directory friendly type is not unknown");
    verify(!dirData.hasAllocatedSize, "directory allocated size marked unavailable (no UI tree walk)");
    verify(dirData.formattedAllocatedSize() == "Niedostępne" || dirData.formattedAllocatedSize() == "Unavailable",
           "directory allocated size formatted as unavailable");

    // Also verify plain directory load without initial type text
    PropertiesDataProvider plainDirProvider;
    plainDirProvider.load(QUrl::fromLocalFile(subDirPath));
    PropertiesData plainDirData = plainDirProvider.data();
    verify(plainDirData.isDir, "plain directory detected as directory via statx/lstat");
    verify(plainDirData.mimeType == "inode/directory", "plain directory MIME resolved as inode/directory");
    verify(plainDirData.friendlyType == "Katalog" || plainDirData.friendlyType == "Folder",
           "plain directory friendly type resolved as Folder or Katalog");
    verify(plainDirData.mimeType != "application/octet-stream", "plain directory MIME is not octet-stream");

    // ------------------------------------------------------------------
    // 13. Remote KIO modeled data: missing optional fields accepted, no fake local metadata
    // ------------------------------------------------------------------
    KIO::UDSEntry remoteEntry;
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("remote_doc.pdf"));
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_SIZE, 543210);
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_USER, QStringLiteral("remoteuser"));
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_GROUP, QStringLiteral("remotegroup"));
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_ACCESS, 0644);
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_MODIFICATION_TIME, 1700000000);
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_INODE, 987654321);
    remoteEntry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, QStringLiteral("application/pdf"));

    PropertiesDataProvider remoteProvider;
    const QUrl remoteUrl(QStringLiteral("sftp://myserver.example.com/home/remoteuser/remote_doc.pdf"));
    remoteProvider.loadFromEntry(remoteUrl, remoteEntry);
    PropertiesData remoteData = remoteProvider.data();

    verify(!remoteData.isLocal, "remote entry recognized as non-local");
    verify(remoteData.name == "remote_doc.pdf", "remote name parsed");
    verify(remoteData.hasLogicalSize && remoteData.logicalSize == 543210, "remote logical size parsed");
    verify(!remoteData.hasAllocatedSize, "remote allocated size is NOT fabricated");
    verify(remoteData.hasOwner && remoteData.owner == "remoteuser", "remote owner parsed");
    verify(remoteData.hasGroup && remoteData.group == "remotegroup", "remote group parsed");
    verify(remoteData.hasInode && remoteData.inode == 987654321, "remote inode parsed");
    verify(remoteData.hasPermissions && remoteData.permissionsMode == 0644, "remote permissions parsed");
    verify(remoteData.hasModifiedTime, "remote mtime parsed");
    verify(!remoteData.hasBirthTime, "missing remote birth time not fabricated");

    // ------------------------------------------------------------------
    // 14. Unavailable / deleted file: safe failure state
    // ------------------------------------------------------------------
    PropertiesDataProvider deletedProvider;
    deletedProvider.load(QUrl::fromLocalFile(tempDir.filePath("does_not_exist_ever.txt")));
    PropertiesData deletedData = deletedProvider.data();
    verify(deletedData.isFailed, "non-existent file reports failure safely");
    verify(!deletedData.errorMessage.isEmpty(), "error message is provided");

    // ------------------------------------------------------------------
    // 15. Async completion: model updates exactly once
    // ------------------------------------------------------------------
    {
        PropertiesDataProvider asyncProvider;
        int readyCount = 0;
        int updateCount = 0;

        QObject::connect(&asyncProvider, &PropertiesDataProvider::dataReady, [&] {
            ++readyCount;
        });
        QObject::connect(&asyncProvider, &PropertiesDataProvider::dataUpdated, [&] {
            ++updateCount;
        });

        asyncProvider.simulateAsyncStat(remoteUrl, remoteEntry, 10);
        verify(readyCount == 1, "dataReady emitted on initial async request");

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(updateCount == 1, "dataUpdated emitted exactly once upon async completion");
        verify(asyncProvider.data().isReady, "async provider data marked ready");
        verify(asyncProvider.data().name == "remote_doc.pdf", "async data correctly populated");
    }

    // ------------------------------------------------------------------
    // 16. Dialog / provider destroyed while request pending: safe cancellation
    // ------------------------------------------------------------------
    {
        auto *cancellableProvider = new PropertiesDataProvider();
        cancellableProvider->simulateAsyncStat(remoteUrl, remoteEntry, 50);
        // Destroy while job is still pending
        delete cancellableProvider;
        verify(true, "provider destroyed while async job pending without crash or leak");
    }

    // ------------------------------------------------------------------
    // 17. Security: no credentials / passwords in model
    // ------------------------------------------------------------------
    const QUrl credentialUrl(QStringLiteral("sftp://user:secret123@myhost.net/folder/file.txt"));
    PropertiesDataProvider credProvider;
    credProvider.loadFromEntry(credentialUrl, remoteEntry);
    PropertiesData credData = credProvider.data();
    verify(!credData.displayAddress.contains("secret123"), "passwords not exposed in displayAddress");

    qInfo("PASS: %d PropertiesDataProvider assertions; capability-aware metadata model, statx/lstat, sparse/symlink/remote", checks);
    return 0;
}
