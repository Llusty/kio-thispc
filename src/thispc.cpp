/*
 * kio-thispc - KF6 KIO worker providing thispc:/
 *
 * Version 0.35.0
 * SPDX-License-Identifier: MIT
 */

#include <KIO/UDSEntry>
#include <KIO/WorkerBase>

#include <Solid/Device>
#include <Solid/DeviceInterface>
#include <Solid/DeviceNotifier>
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
    QString udi;
    QString name;
    QString mountPoint;
    QString fileSystem;
    qint64 totalBytes = 0;
    qint64 availableBytes = 0;
    bool removable = false;
    bool isMounted = true;
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

    const bool removable = isRemovableDrive(device);
    const bool accessible = access->isAccessible();

    if (!accessible) {
        if (!removable) {
            return std::nullopt;
        }

        VolumeInfo info;
        const QString key =
            !volume->uuid().isEmpty() ? volume->uuid() : device.udi();

        info.id = stableId(key);
        info.udi = device.udi();
        info.mountPoint.clear();
        info.fileSystem = volume->fsType();
        info.totalBytes = volume->size() > 0 ? static_cast<qint64>(volume->size()) : -1;
        info.availableBytes = -1;
        info.removable = true;
        info.isMounted = false;

        if (!volume->label().trimmed().isEmpty()) {
            info.name = volume->label().trimmed();
        } else if (!device.displayName().trimmed().isEmpty()) {
            info.name = device.displayName().trimmed();
        } else {
            info.name = isPolish()
                ? QStringLiteral("Wolumin wymienny")
                : QStringLiteral("Removable Volume");
        }

        return info;
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
    info.udi = device.udi();
    info.mountPoint = mountPoint;
    info.fileSystem =
        !volume->fsType().isEmpty()
            ? volume->fsType()
            : QString::fromUtf8(storage.fileSystemType());

    info.totalBytes = storage.bytesTotal();
    info.availableBytes = storage.bytesAvailable();
    info.removable = removable;
    info.isMounted = true;

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
    info.isMounted = true;

    return info;
}

QList<VolumeInfo> discoveredVolumes()
{
    // Process pending DBus messages from UDisks2 so Solid's device tree and
    // mount accessibility states are up-to-date in this worker process.
    QCoreApplication::processEvents();

    QList<VolumeInfo> result;
    QSet<QString> seenMountPoints;
    QSet<QString> seenIds;

    const auto devices =
        Solid::Device::listFromType(
            Solid::DeviceInterface::StorageVolume);

    for (const Solid::Device &device : devices) {
        const auto info = volumeFromSolid(device);

        if (!info) {
            continue;
        }

        if (seenIds.contains(info->id)) {
            continue;
        }

        if (info->isMounted) {
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
        }

        seenIds.insert(info->id);
        result.push_back(*info);
    }

    // Root-on-Btrfs/LVM arrangements do not always appear through Solid in the
    // same way as normal visible volumes, so keep a fallback for "/".
    if (!seenMountPoints.contains(QStringLiteral("/"))) {
        const VolumeInfo root = rootFallback();

        if (root.totalBytes > 0 && !seenIds.contains(root.id)) {
            seenIds.insert(root.id);
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
            if (a.isMounted != b.isMounted) {
                return a.isMounted;
            }
            return a.name.localeAwareCompare(b.name) < 0;
        });

    return result;
}

std::optional<VolumeInfo> findVolume(const QString &id)
{
    const auto volumes = discoveredVolumes();

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

    if (volume.isMounted) {
        const int percent = usedPercent(volume);

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

        entry.fastInsert(
            KIO::UDSEntry::UDS_COMMENT,
            QStringLiteral("%1 • %2")
                .arg(shortFreeSummary(volume), detailedSummary(volume)));

        entry.fastInsert(
            KIO::UDSEntry::UDS_SIZE,
            volume.totalBytes);

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

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 5,
            QStringLiteral("1"));

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 6,
            volume.udi);

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 7,
            volume.removable ? QStringLiteral("1") : QStringLiteral("0"));

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 8,
            volume.removable ? QStringLiteral("1") : QStringLiteral("0"));
    } else {
        entry.fastInsert(
            KIO::UDSEntry::UDS_ICON_NAME,
            QStringLiteral("drive-removable-media"));

        entry.fastInsert(
            KIO::UDSEntry::UDS_DISPLAY_TYPE,
            isPolish()
                ? QStringLiteral("Dysk (niezamontowany)")
                : QStringLiteral("Drive (unmounted)"));

        // Deliberately do NOT insert UDS_TARGET_URL for unmounted volumes.

        entry.fastInsert(
            KIO::UDSEntry::UDS_COMMENT,
            isPolish()
                ? QStringLiteral("Niezamontowany")
                : QStringLiteral("Unmounted"));

        entry.fastInsert(
            KIO::UDSEntry::UDS_SIZE,
            volume.totalBytes > 0 ? volume.totalBytes : -1);

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 0,
            QStringLiteral("—"));

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 1,
            volume.totalBytes > 0 ? formatBytes(volume.totalBytes) : QStringLiteral("—"));

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 2,
            QStringLiteral("—"));

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 3,
            volume.fileSystem);

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 4,
            QString());

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 5,
            QStringLiteral("0"));

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 6,
            volume.udi);

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 7,
            volume.removable ? QStringLiteral("1") : QStringLiteral("0"));

        entry.fastInsert(
            KIO::UDSEntry::UDS_EXTRA + 8,
            volume.removable ? QStringLiteral("1") : QStringLiteral("0"));
    }

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
        // Ensure Solid initializes its DBus notifier backend for live hotplug and mount updates
        Solid::DeviceNotifier::instance();
    }

    KIO::WorkerResult listDir(
        const QUrl &url) override
    {
        const QStringList parts = pathParts(url);

        if (parts.isEmpty()) {
            KIO::UDSEntryList entries;
            entries << rootEntry();

            const auto volumes = discoveredVolumes();

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

        if (!volume->isMounted) {
            return KIO::WorkerResult::fail(
                KIO::ERR_CANNOT_ENTER_DIRECTORY,
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

        if (!volume->isMounted) {
            return KIO::WorkerResult::fail(
                KIO::ERR_CANNOT_ENTER_DIRECTORY,
                url.toDisplayString());
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

        if (!volume->isMounted) {
            return KIO::WorkerResult::fail(
                KIO::ERR_CANNOT_ENTER_DIRECTORY,
                url.toDisplayString());
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
