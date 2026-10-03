/*
 * Asynchronous / capability-aware drive and filesystem properties provider.
 *
 * Part of Properties 2.0 (0.37.0 Stage 2).
 * Reads drive capabilities, UUID, filesystem types, storage info,
 * and mount options from /proc/self/mountinfo without external processes.
 * Strictly avoids mounting unmounted storage.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "drivepropertiesdata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QStringList>
#include <QTextStream>

#include <Solid/Block>
#include <Solid/Device>
#include <Solid/DeviceNotifier>
#include <Solid/StorageAccess>
#include <Solid/StorageDrive>
#include <Solid/StorageVolume>

#include <optional>

class DrivePropertiesProvider : public QObject
{
    Q_OBJECT

public:
    struct MountEntry
    {
        QString mountPoint;
        QString deviceNode;
        QString fileSystemType;
        QString perMountOptions;
        QString superOptions;
        QString combinedOptions;
        bool isReadOnly = false;
    };

    explicit DrivePropertiesProvider(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    DrivePropertiesData data() const
    {
        return m_data;
    }

    static QString decodeMountString(const QString &input)
    {
        QString result;
        result.reserve(input.size());
        for (int i = 0; i < input.size(); ++i) {
            if (input[i] == QLatin1Char('\\') && i + 3 < input.size()
                && input[i + 1].isDigit() && input[i + 2].isDigit() && input[i + 3].isDigit()) {
                bool ok = false;
                const int code = input.mid(i + 1, 3).toInt(&ok, 8);
                if (ok && code > 0 && code < 256) {
                    result.append(QChar(code));
                    i += 3;
                    continue;
                }
            }
            result.append(input[i]);
        }
        return result;
    }

    static QString combineMountOptions(const QString &perMount, const QString &super)
    {
        QStringList parts = perMount.split(QLatin1Char(','), Qt::SkipEmptyParts);
        const QStringList superParts = super.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &opt : superParts) {
            if (!parts.contains(opt)) {
                parts.append(opt);
            }
        }
        return parts.join(QLatin1Char(','));
    }

    static QList<MountEntry> parseAllMountInfo(const QString &customContent = {})
    {
        QString content = customContent;
        if (content.isEmpty()) {
            QFile file(QStringLiteral("/proc/self/mountinfo"));
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                content = QString::fromUtf8(file.readAll());
            }
        }

        QList<MountEntry> entries;
        if (content.isEmpty()) {
            return entries;
        }

        const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }

            const QStringList tokens = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            // Minimum fields required: 0..5 (mount options), then optional fields, '-', fstype, mountSource, superOptions
            if (tokens.size() < 7) {
                continue;
            }

            int hyphenIndex = -1;
            for (int i = 6; i < tokens.size(); ++i) {
                if (tokens[i] == QLatin1String("-")) {
                    hyphenIndex = i;
                    break;
                }
            }

            if (hyphenIndex < 0 || hyphenIndex + 3 >= tokens.size()) {
                continue;
            }

            MountEntry entry;
            entry.mountPoint = decodeMountString(tokens[4]);
            entry.perMountOptions = decodeMountString(tokens[5]);
            entry.fileSystemType = decodeMountString(tokens[hyphenIndex + 1]);
            entry.deviceNode = decodeMountString(tokens[hyphenIndex + 2]);
            entry.superOptions = decodeMountString(tokens[hyphenIndex + 3]);
            entry.combinedOptions = combineMountOptions(entry.perMountOptions, entry.superOptions);

            const QStringList perList = entry.perMountOptions.split(QLatin1Char(','));
            const QStringList superList = entry.superOptions.split(QLatin1Char(','));
            entry.isReadOnly = perList.contains(QStringLiteral("ro")) || superList.contains(QStringLiteral("ro"));

            entries.append(entry);
        }

        return entries;
    }

    static std::optional<MountEntry> parseMountInfo(
        const QString &customContent,
        const QString &targetMountPoint,
        const QString &targetDeviceNode = {})
    {
        const QList<MountEntry> entries = parseAllMountInfo(customContent);
        if (entries.isEmpty()) {
            return std::nullopt;
        }

        // 1. Try exact mount point match
        if (!targetMountPoint.isEmpty()) {
            for (const auto &entry : entries) {
                if (entry.mountPoint == targetMountPoint) {
                    return entry;
                }
            }
        }

        // 2. Try device node match if mount point did not match
        if (!targetDeviceNode.isEmpty()) {
            for (const auto &entry : entries) {
                if (entry.deviceNode == targetDeviceNode) {
                    return entry;
                }
            }
        }

        return std::nullopt;
    }

    static std::optional<MountEntry> findExactMountPoint(
        const QString &path,
        const QString &customContent = {})
    {
        const QString clean = QDir::cleanPath(path);
        if (clean.isEmpty()) {
            return std::nullopt;
        }

        const QList<MountEntry> entries = parseAllMountInfo(customContent);
        for (const auto &entry : entries) {
            if (entry.mountPoint == clean) {
                return entry;
            }
        }
        return std::nullopt;
    }

    static QString resolveParentDiskNode(const QString &partitionDevNode)
    {
        if (partitionDevNode.isEmpty()) {
            return {};
        }

        const QString devName = QFileInfo(partitionDevNode).fileName();
        if (devName.isEmpty()) {
            return {};
        }

        // 1. Try Linux sysfs /sys/class/block/<devName>/partition
        const QString partPath = QStringLiteral("/sys/class/block/%1/partition").arg(devName);
        if (QFile::exists(partPath)) {
            const QFileInfo parentInfo(QStringLiteral("/sys/class/block/%1/..").arg(devName));
            const QString parentCanonical = parentInfo.canonicalFilePath();
            if (!parentCanonical.isEmpty()) {
                const QString parentName = QFileInfo(parentCanonical).fileName();
                if (!parentName.isEmpty() && parentName != devName) {
                    const QString parentDev = QStringLiteral("/dev/") + parentName;
                    return parentDev;
                }
            }
        }

        // 2. Algorithmic fallback for NVMe / mmcblk / SCSI / SATA / USB / VirtIO
        // NVMe: e.g. nvme0n1p3 -> nvme0n1, nvme2n1p2 -> nvme2n1
        static const QRegularExpression nvmeRegex(QStringLiteral("^(nvme\\d+n\\d+)p\\d+$"));
        const auto nvmeMatch = nvmeRegex.match(devName);
        if (nvmeMatch.hasMatch()) {
            return QStringLiteral("/dev/") + nvmeMatch.captured(1);
        }

        // mmcblk: e.g. mmcblk0p1 -> mmcblk0
        static const QRegularExpression mmcRegex(QStringLiteral("^(mmcblk\\d+)p\\d+$"));
        const auto mmcMatch = mmcRegex.match(devName);
        if (mmcMatch.hasMatch()) {
            return QStringLiteral("/dev/") + mmcMatch.captured(1);
        }

        // sd/hd/vd: e.g. sde1 -> sde, sda12 -> sda
        static const QRegularExpression sdRegex(QStringLiteral("^([a-z]+)\\d+$"));
        const auto sdMatch = sdRegex.match(devName);
        if (sdMatch.hasMatch()) {
            return QStringLiteral("/dev/") + sdMatch.captured(1);
        }

        return {};
    }

    static QString busToString(Solid::StorageDrive::Bus bus)
    {
        switch (bus) {
        case Solid::StorageDrive::Ide:
            return QStringLiteral("IDE");
        case Solid::StorageDrive::Usb:
            return QStringLiteral("USB");
        case Solid::StorageDrive::Ieee1394:
            return QStringLiteral("IEEE 1394 (FireWire)");
        case Solid::StorageDrive::Scsi:
            return QStringLiteral("SCSI");
        case Solid::StorageDrive::Sata:
            return QStringLiteral("SATA");
        case Solid::StorageDrive::Platform:
            return QStringLiteral("Platform");
        default:
            return isPolish() ? QStringLiteral("Inna") : QStringLiteral("Other");
        }
    }

    void load(const DriveInfo &drive, const QString &customMountInfo = {})
    {
        m_udi = drive.udi;
        m_data = DrivePropertiesData();
        m_data.udi = drive.udi;
        m_data.name = drive.name;
        m_data.mountPoint = drive.mountPoint;
        m_data.isMounted = drive.isMounted;
        m_data.isRemovable = drive.isRemovable;
        m_data.volumeFsType = drive.fileSystem;

        // Connect removal notification using UDI string (no dangling pointers)
        setupRemovalNotifier();

        Solid::Device dev;
        if (!drive.udi.isEmpty()) {
            dev = Solid::Device(drive.udi);
        }

        if (dev.isValid()) {
            if (const auto *vol = dev.as<Solid::StorageVolume>()) {
                if (!vol->uuid().isEmpty()) {
                    m_data.uuid = vol->uuid();
                }
                if (!vol->fsType().isEmpty()) {
                    m_data.volumeFsType = vol->fsType();
                }
                if (vol->size() > 0 && m_data.totalBytes <= 0) {
                    m_data.totalBytes = static_cast<qint64>(vol->size());
                }
            }

            if (const auto *block = dev.as<Solid::Block>()) {
                m_data.deviceNode = block->device();
            }

            if (const auto *access = dev.as<Solid::StorageAccess>()) {
                m_data.isMounted = access->isAccessible();
                if (m_data.isMounted && !access->filePath().isEmpty()) {
                    m_data.mountPoint = access->filePath();
                }
                // SAFETY: Never call access->setup()!
            }

            Solid::Device current = dev.parent();
            while (current.isValid()) {
                if (const auto *driveDev = current.as<Solid::StorageDrive>()) {
                    m_data.parentDriveUdi = current.udi();
                    m_data.driveModel = current.product();
                    m_data.driveVendor = current.vendor();
                    m_data.driveBus = busToString(driveDev->bus());
                    m_data.isRemovable = m_data.isRemovable || driveDev->isRemovable();
                    m_data.isHotpluggable = driveDev->isHotpluggable();
                    break;
                }
                current = current.parent();
            }

            if (m_data.driveModel.isEmpty()) {
                m_data.driveModel = dev.product();
            }
            if (m_data.driveVendor.isEmpty()) {
                m_data.driveVendor = dev.vendor();
            }
        }

        // Query mount info if mounted
        if (m_data.isMounted) {
            const auto mountEntry = parseMountInfo(customMountInfo, m_data.mountPoint, m_data.deviceNode);
            if (mountEntry.has_value()) {
                m_data.mountOptions = mountEntry->combinedOptions;
                m_data.isReadOnly = mountEntry->isReadOnly;
                if (m_data.deviceNode.isEmpty() && !mountEntry->deviceNode.isEmpty()) {
                    m_data.deviceNode = mountEntry->deviceNode;
                }
                if (!mountEntry->fileSystemType.isEmpty()) {
                    m_data.fileSystemType = mountEntry->fileSystemType;
                }
            }

            if (!m_data.mountPoint.isEmpty()) {
                const QStorageInfo storage(m_data.mountPoint);
                if (storage.isValid() && storage.isReady()) {
                    if (m_data.fileSystemType.isEmpty()) {
                        m_data.fileSystemType = QString::fromUtf8(storage.fileSystemType());
                    }
                    m_data.isReadOnly = m_data.isReadOnly || storage.isReadOnly();
                    m_data.totalBytes = storage.bytesTotal();
                    m_data.freeBytes = storage.bytesAvailable();
                    m_data.usedBytes = std::max<qint64>(0, m_data.totalBytes - m_data.freeBytes);
                    m_data.usedPercent = m_data.totalBytes > 0
                        ? static_cast<int>((m_data.usedBytes * 100) / m_data.totalBytes)
                        : 0;
                }
            }
        } else {
            // Unmounted device: never mount, clear mount attributes, free/used are unavailable
            m_data.mountPoint.clear();
            m_data.mountOptions.clear();
            m_data.freeBytes = -1;
            m_data.usedBytes = -1;
            m_data.usedPercent = 0;
        }

        // Truthfully resolve whole-disk parent device node (never a sibling partition)
        m_data.parentDriveNode = resolveParentDiskNode(m_data.deviceNode);

        setupRemovalNotifier();
        Q_EMIT dataReady(m_data);
    }

    void loadSnapshot(const DrivePropertiesData &snapshot)
    {
        m_data = snapshot;
        m_udi = snapshot.udi;
        setupRemovalNotifier();
        Q_EMIT dataReady(m_data);
    }

Q_SIGNALS:
    void dataReady(const DrivePropertiesData &data);
    void deviceRemoved();

private Q_SLOTS:
    void handleDeviceRemoved(const QString &udi)
    {
        if (udi.isEmpty()) {
            return;
        }
        if (udi == m_udi
            || (!m_data.udi.isEmpty() && udi == m_data.udi)
            || (!m_data.parentDriveUdi.isEmpty() && udi == m_data.parentDriveUdi)) {
            m_data.isDeviceMissing = true;
            Q_EMIT deviceRemoved();
        }
    }

private:
    void setupRemovalNotifier()
    {
        if (!m_udi.isEmpty() || !m_data.parentDriveUdi.isEmpty() || !m_data.udi.isEmpty()) {
            connect(
                Solid::DeviceNotifier::instance(),
                &Solid::DeviceNotifier::deviceRemoved,
                this,
                &DrivePropertiesProvider::handleDeviceRemoved,
                Qt::UniqueConnection);
        }
    }

    DrivePropertiesData m_data;
    QString m_udi;
};
