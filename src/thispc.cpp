/*
 * kio-thispc - KF6 KIO worker providing thispc:/
 *
 * Version 0.33.0
 * SPDX-License-Identifier: MIT
 */

#include <KIO/UDSEntry>
#include <KIO/WorkerBase>

#include <Solid/Device>
#include <Solid/DeviceInterface>
#include <Solid/StorageAccess>
#include <Solid/StorageDrive>
#include <Solid/StorageVolume>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QStorageInfo>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <optional>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

namespace
{

// Hide tiny *internal* filesystem partitions such as recovery/helper volumes.
// Root and removable media are always kept.
// 8 GiB hides the 4.1 GiB technical partition seen during v0.1 testing while
// still keeping normal internal data volumes.
constexpr qint64 kMinInternalVolumeBytes =
    8LL * 1024LL * 1024LL * 1024LL;

struct VolumeInfo
{
    QString id;
    QString name;
    QString mountPoint;
    QString fileSystem;
    qint64 totalBytes = 0;
    qint64 availableBytes = 0;
    bool removable = false;
};

bool isPolish()
{
    return QLocale().language() == QLocale::Polish;
}

QString rootDisplayName()
{
    return isPolish() ? QStringLiteral("Ten komputer")
                      : QStringLiteral("This PC");
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 0) {
        return QStringLiteral("—");
    }

    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;

    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }

    int precision = 0;
    if (unit > 0 && value < 10.0) {
        precision = 2;
    } else if (unit > 0 && value < 100.0) {
        precision = 1;
    }

    return QStringLiteral("%1 %2")
        .arg(QLocale().toString(value, 'f', precision),
             QString::fromLatin1(units[unit]));
}

int usedPercent(const VolumeInfo &volume)
{
    if (volume.totalBytes <= 0) {
        return 0;
    }

    const qint64 used =
        std::max<qint64>(0, volume.totalBytes - volume.availableBytes);

    const double percent =
        100.0 * static_cast<double>(used)
        / static_cast<double>(volume.totalBytes);

    return std::clamp(static_cast<int>(std::lround(percent)), 0, 100);
}

int iconBucket(int percent)
{
    // Nearest 5%: 0, 5, ... 100.
    return std::clamp(((percent + 2) / 5) * 5, 0, 100);
}

QString capacitySummary(const VolumeInfo &volume)
{
    if (isPolish()) {
        return QStringLiteral("%1 wolne z %2")
            .arg(formatBytes(volume.availableBytes),
                 formatBytes(volume.totalBytes));
    }

    return QStringLiteral("%1 free of %2")
        .arg(formatBytes(volume.availableBytes),
             formatBytes(volume.totalBytes));
}

QString shortFreeSummary(const VolumeInfo &volume)
{
    if (isPolish()) {
        return QStringLiteral("%1 wolne")
            .arg(formatBytes(volume.availableBytes));
    }

    return QStringLiteral("%1 free")
        .arg(formatBytes(volume.availableBytes));
}

QString detailedSummary(const VolumeInfo &volume)
{
    const int percent = usedPercent(volume);

    if (isPolish()) {
        return QStringLiteral("%1% zajęte • %2 • %3 • %4")
            .arg(percent)
            .arg(capacitySummary(volume))
            .arg(volume.fileSystem)
            .arg(volume.mountPoint);
    }

    return QStringLiteral("%1% used • %2 • %3 • %4")
        .arg(percent)
        .arg(capacitySummary(volume))
        .arg(volume.fileSystem)
        .arg(volume.mountPoint);
}

QString fallbackVolumeName(const QString &mountPoint)
{
    if (mountPoint == QStringLiteral("/")) {
        return QStringLiteral("System");
    }

    const QString leaf = QFileInfo(mountPoint).fileName();
    if (!leaf.isEmpty()) {
        return leaf;
    }

    return mountPoint;
}

QString stableId(const QString &key)
{
    return QStringLiteral("volume-")
        + QString::fromLatin1(
            QCryptographicHash::hash(
                key.toUtf8(),
                QCryptographicHash::Sha1)
                .toHex()
                .left(16));
}

bool isRemovableDrive(const Solid::Device &volumeDevice)
{
    Solid::Device current = volumeDevice;

    while (current.isValid()) {
        if (const auto *drive = current.as<Solid::StorageDrive>()) {
            return drive->isRemovable() || drive->isHotpluggable();
        }
        current = current.parent();
    }

    return false;
}

QString progressIconName(const VolumeInfo &volume)
{
    const int bucket = iconBucket(usedPercent(volume));

    if (volume.removable) {
        return QStringLiteral("kio-thispc-removable-%1").arg(bucket);
    }

    return QStringLiteral("kio-thispc-drive-%1").arg(bucket);
}

bool shouldShowVolume(const VolumeInfo &volume)
{
    if (volume.mountPoint == QStringLiteral("/")) {
        return true;
    }

    if (volume.removable) {
        return true;
    }

    return volume.totalBytes >= kMinInternalVolumeBytes;
}

std::optional<VolumeInfo> volumeFromSolid(const Solid::Device &device)
{
    const auto *volume = device.as<Solid::StorageVolume>();
    const auto *access = device.as<Solid::StorageAccess>();

    if (!volume || !access) {
        return std::nullopt;
    }

    if (volume->usage() != Solid::StorageVolume::FileSystem) {
        return std::nullopt;
    }

    if (!access->isAccessible()) {
        return std::nullopt;
    }

    const QString mountPoint = access->filePath();
    if (mountPoint.isEmpty()) {
        return std::nullopt;
    }

    // Deliberately do NOT filter on Solid::isIgnored().
    // Dolphin's Devices panel can expose useful fixed volumes (e.g. NTFS)
    // that Solid marks ignored for other presentation contexts.
    QStorageInfo storage(mountPoint);

    if (!storage.isValid()
        || !storage.isReady()
        || storage.bytesTotal() <= 0) {
        return std::nullopt;
    }

    VolumeInfo info;

    const QString key =
        !volume->uuid().isEmpty() ? volume->uuid() : device.udi();

    info.id = stableId(key);
    info.mountPoint = mountPoint;
    info.fileSystem =
        !volume->fsType().isEmpty()
            ? volume->fsType()
            : QString::fromUtf8(storage.fileSystemType());

    info.totalBytes = storage.bytesTotal();
    info.availableBytes = storage.bytesAvailable();
    info.removable = isRemovableDrive(device);

    if (mountPoint == QStringLiteral("/")) {
        info.name = QStringLiteral("System");
    } else if (!volume->label().trimmed().isEmpty()) {
        info.name = volume->label().trimmed();
    } else if (!device.displayName().trimmed().isEmpty()) {
        info.name = device.displayName().trimmed();
    } else {
        info.name = fallbackVolumeName(mountPoint);
    }

    if (!shouldShowVolume(info)) {
        return std::nullopt;
    }

    return info;
}

VolumeInfo rootFallback()
{
    const QStorageInfo storage = QStorageInfo::root();

    VolumeInfo info;
    info.id = stableId(QStringLiteral("root:/"));
    info.name = QStringLiteral("System");
    info.mountPoint = QStringLiteral("/");
    info.fileSystem = QString::fromUtf8(storage.fileSystemType());
    info.totalBytes = storage.bytesTotal();
    info.availableBytes = storage.bytesAvailable();
    info.removable = false;

    return info;
}

QList<VolumeInfo> mountedVolumes()
{
    QList<VolumeInfo> result;
    QSet<QString> seenMountPoints;

    const auto devices =
        Solid::Device::listFromType(
            Solid::DeviceInterface::StorageVolume);

    for (const Solid::Device &device : devices) {
        const auto info = volumeFromSolid(device);

        if (!info) {
            continue;
        }

        const QString canonicalPath =
            QFileInfo(info->mountPoint).canonicalFilePath();

        const QString canonical =
            canonicalPath.isEmpty()
                ? info->mountPoint
                : canonicalPath;

        if (seenMountPoints.contains(canonical)) {
            continue;
        }

        seenMountPoints.insert(canonical);
        result.push_back(*info);
    }

    // Root-on-Btrfs/LVM arrangements do not always appear through Solid in the
    // same way as normal visible volumes, so keep a fallback for "/".
    if (!seenMountPoints.contains(QStringLiteral("/"))) {
        const VolumeInfo root = rootFallback();

        if (root.totalBytes > 0) {
            result.push_back(root);
        }
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const VolumeInfo &a, const VolumeInfo &b) {
            if (a.mountPoint == QStringLiteral("/")) {
                return true;
            }
            if (b.mountPoint == QStringLiteral("/")) {
                return false;
            }
            if (a.removable != b.removable) {
                return !a.removable;
            }
            return a.name.localeAwareCompare(b.name) < 0;
        });

    return result;
}

std::optional<VolumeInfo> findVolume(const QString &id)
{
    const auto volumes = mountedVolumes();

    for (const auto &volume : volumes) {
        if (volume.id == id) {
            return volume;
        }
    }

    return std::nullopt;
}

KIO::UDSEntry rootEntry()
{
    KIO::UDSEntry entry;

    entry.fastInsert(
        KIO::UDSEntry::UDS_NAME,
        QStringLiteral("."));

    entry.fastInsert(
        KIO::UDSEntry::UDS_DISPLAY_NAME,
        rootDisplayName());

    entry.fastInsert(
        KIO::UDSEntry::UDS_MIME_TYPE,
        QStringLiteral("inode/directory"));

    entry.fastInsert(
        KIO::UDSEntry::UDS_ICON_NAME,
        QStringLiteral("computer"));

    entry.fastInsert(
        KIO::UDSEntry::UDS_DISPLAY_TYPE,
        rootDisplayName());

#ifndef Q_OS_WIN
    entry.fastInsert(
        KIO::UDSEntry::UDS_FILE_TYPE,
        static_cast<long long>(S_IFDIR));

    entry.fastInsert(
        KIO::UDSEntry::UDS_ACCESS,
        static_cast<long long>(S_IRUSR | S_IXUSR));
#endif

    return entry;
}

KIO::UDSEntry volumeEntry(const VolumeInfo &volume)
{
    KIO::UDSEntry entry;

    const int percent = usedPercent(volume);

    entry.fastInsert(
        KIO::UDSEntry::UDS_NAME,
        volume.id);

    // Keep UDS_DISPLAY_NAME strictly single-line.
    // Dolphin renders embedded newline characters in names as a visible
    // line-break marker, so free-space information stays in metadata/tooltips.
    entry.fastInsert(
        KIO::UDSEntry::UDS_DISPLAY_NAME,
        volume.name);

    entry.fastInsert(
        KIO::UDSEntry::UDS_MIME_TYPE,
        QStringLiteral("inode/directory"));

    entry.fastInsert(
        KIO::UDSEntry::UDS_ICON_NAME,
        progressIconName(volume));

    entry.fastInsert(
        KIO::UDSEntry::UDS_DISPLAY_TYPE,
        isPolish()
            ? QStringLiteral("Dysk • %1").arg(formatBytes(volume.totalBytes))
            : QStringLiteral("Drive • %1").arg(formatBytes(volume.totalBytes)));

    entry.fastInsert(
        KIO::UDSEntry::UDS_TARGET_URL,
        QUrl::fromLocalFile(volume.mountPoint).toString());

    // Do NOT set UDS_LOCAL_PATH here. With a local path Dolphin can decide to
    // generate a folder thumbnail/preview and replace the drive icon. The
    // TARGET_URL is sufficient for opening the real mount point.
    entry.fastInsert(
        KIO::UDSEntry::UDS_COMMENT,
        QStringLiteral("%1 • %2")
            .arg(shortFreeSummary(volume), detailedSummary(volume)));

    // The regular Size column represents total capacity.
    entry.fastInsert(
        KIO::UDSEntry::UDS_SIZE,
        volume.totalBytes);

    // Extra columns declared in thispc.json.
    entry.fastInsert(
        KIO::UDSEntry::UDS_EXTRA + 0,
        formatBytes(volume.availableBytes));

    entry.fastInsert(
        KIO::UDSEntry::UDS_EXTRA + 1,
        formatBytes(volume.totalBytes));

    entry.fastInsert(
        KIO::UDSEntry::UDS_EXTRA + 2,
        QStringLiteral("%1%").arg(percent));

    entry.fastInsert(
        KIO::UDSEntry::UDS_EXTRA + 3,
        volume.fileSystem);

    entry.fastInsert(
        KIO::UDSEntry::UDS_EXTRA + 4,
        volume.mountPoint);

#ifndef Q_OS_WIN
    entry.fastInsert(
        KIO::UDSEntry::UDS_FILE_TYPE,
        static_cast<long long>(S_IFDIR));

    entry.fastInsert(
        KIO::UDSEntry::UDS_ACCESS,
        static_cast<long long>(S_IRUSR | S_IXUSR));
#endif

    return entry;
}

QStringList pathParts(const QUrl &url)
{
    return url.path().split(
        QLatin1Char('/'),
        Qt::SkipEmptyParts);
}

QUrl redirectedLocalUrl(
    const VolumeInfo &volume,
    const QStringList &parts)
{
    QString localPath = volume.mountPoint;

    if (parts.size() > 1) {
        QStringList rest = parts;
        rest.removeFirst();

        localPath =
            QDir(volume.mountPoint)
                .filePath(rest.join(QLatin1Char('/')));
    }

    return QUrl::fromLocalFile(
        QDir::cleanPath(localPath));
}

} // namespace

class ThisPcWorker : public KIO::WorkerBase
{
public:
    ThisPcWorker(
        const QByteArray &pool,
        const QByteArray &app)
        : KIO::WorkerBase("thispc", pool, app)
    {
    }

    KIO::WorkerResult listDir(
        const QUrl &url) override
    {
        const QStringList parts = pathParts(url);

        if (parts.isEmpty()) {
            KIO::UDSEntryList entries;
            entries << rootEntry();

            const auto volumes = mountedVolumes();

            for (const auto &volume : volumes) {
                entries << volumeEntry(volume);
            }

            listEntries(entries);
            return KIO::WorkerResult::pass();
        }

        const auto volume = findVolume(parts.first());

        if (!volume) {
            return KIO::WorkerResult::fail(
                KIO::ERR_DOES_NOT_EXIST,
                url.toDisplayString());
        }

        redirection(
            redirectedLocalUrl(*volume, parts));

        return KIO::WorkerResult::pass();
    }

    KIO::WorkerResult stat(
        const QUrl &url) override
    {
        const QStringList parts = pathParts(url);

        if (parts.isEmpty()) {
            statEntry(rootEntry());
            return KIO::WorkerResult::pass();
        }

        const auto volume = findVolume(parts.first());

        if (!volume) {
            return KIO::WorkerResult::fail(
                KIO::ERR_DOES_NOT_EXIST,
                url.toDisplayString());
        }

        if (parts.size() == 1) {
            statEntry(volumeEntry(*volume));
            return KIO::WorkerResult::pass();
        }

        redirection(
            redirectedLocalUrl(*volume, parts));

        return KIO::WorkerResult::pass();
    }

    KIO::WorkerResult mimetype(
        const QUrl &url) override
    {
        const QStringList parts = pathParts(url);

        if (parts.isEmpty()) {
            mimeType(QStringLiteral("inode/directory"));
            return KIO::WorkerResult::pass();
        }

        const auto volume = findVolume(parts.first());

        if (!volume) {
            return KIO::WorkerResult::fail(
                KIO::ERR_DOES_NOT_EXIST,
                url.toDisplayString());
        }

        if (parts.size() == 1) {
            mimeType(QStringLiteral("inode/directory"));
            return KIO::WorkerResult::pass();
        }

        redirection(
            redirectedLocalUrl(*volume, parts));

        return KIO::WorkerResult::pass();
    }
};

class KIOPluginForMetaData : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(
        IID "org.kde.kio.worker.thispc"
        FILE "thispc.json")
};

extern "C" int Q_DECL_EXPORT
kdemain(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(
        QStringLiteral("kio_thispc"));

    if (argc != 4) {
        fprintf(
            stderr,
            "Usage: kio_thispc protocol "
            "domain-socket1 domain-socket2\n");
        return -1;
    }

    ThisPcWorker worker(argv[2], argv[3]);
    worker.dispatchLoop();

    return 0;
}

#include "thispc.moc"
