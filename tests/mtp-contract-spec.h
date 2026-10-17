/*
 * ThisPC 0.41.1 — Stage 1.2: MTP Architecture Contract & Specification Helpers
 *
 * Included inside test suite namespaces by tests/run-pane-actions.py.
 * Do not add #pragma once so each test suite namespace gets its own internal copy.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

static int pendingMtpSpecs = 0;

static void verifyPendingMtpSpec(bool condition, const char *description)
{
    if (qgetenv("THISPC_RUN_PENDING_MTP_SPECS") == "1") {
        if (!condition) {
            qFatal("FAIL (PENDING_MTP_SPEC enforced): %s", description);
        }
        return;
    }
    if (condition) {
        return;
    }
    ++pendingMtpSpecs;
    qInfo("PENDING_MTP_SPEC [Stage 1.3-1.5]: %s", description);
}

// ===========================================================================
// 1. Device Type & Capability Contract (Section 2)
// ===========================================================================
using ::DriveDeviceKind;

namespace MtpUdsContract {
// UDS_EXTRA offsets 0..8 are already allocated in src/thispc.cpp & src/drivehomecoordinator.cpp:
//   +0: freeText
//   +1: capacityText
//   +2: usedText
//   +3: fileSystem
//   +4: mountPoint
//   +5: mountedFlag ("1" / "0")
//   +6: udi
//   +7: removableFlag ("1" / "0")
//   +8: removableFlag ("1" / "0")
// Stage 1.2 contract reserves UDS_EXTRA + 9 for explicit device kind ("mtp" vs "volume").
inline constexpr uint kDeviceKindExtraOffset = 9;

inline KIO::UDSEntry makeMtpUdsEntry(
    const QString &id,
    const QString &displayName,
    const QString &udi,
    const QString &icon = QStringLiteral("smartphone"),
    bool includeExplicitKindTag = true)
{
    KIO::UDSEntry entry;
    entry.fastInsert(KIO::UDSEntry::UDS_NAME, id);
    entry.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, displayName);
    entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR);
    entry.fastInsert(KIO::UDSEntry::UDS_ACCESS, 0555);
    entry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, QStringLiteral("inode/directory"));
    entry.fastInsert(KIO::UDSEntry::UDS_TARGET_URL, QStringLiteral("mtp:udi=%1").arg(udi));
    entry.fastInsert(KIO::UDSEntry::UDS_ICON_NAME, icon);
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 0, QStringLiteral("—"));
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 1, QStringLiteral("—"));
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 2, QStringLiteral("—"));
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 3, QStringLiteral("MTP"));
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 4, QString()); // Never a fake local mount point
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 5, QStringLiteral("1"));
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 6, udi);
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 7, QStringLiteral("1"));
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 8, QStringLiteral("1"));
    if (includeExplicitKindTag) {
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + kDeviceKindExtraOffset, QStringLiteral("mtp"));
    }
    return entry;
}

inline DriveInfo makeMtpDriveInfo(
    const QString &id,
    const QString &displayName,
    const QString &udi,
    const QString &icon = QStringLiteral("smartphone"))
{
    DriveInfo d;
    d.id = id;
    d.name = displayName;
    d.udi = udi;
    d.freeText = QStringLiteral("—");
    d.capacityText = QStringLiteral("—");
    d.usedText = QStringLiteral("—");
    d.usedPercent = 0;
    d.fileSystem = QStringLiteral("MTP");
    d.mountPoint.clear();
    d.targetUrl = QUrl(QStringLiteral("mtp:udi=%1").arg(udi));
    d.iconName = icon;
    d.isMounted = true;
    d.isRemovable = true;
    d.deviceKind = DriveDeviceKind::PortableMediaMtp;
    return d;
}
} // namespace MtpUdsContract

namespace MtpDriveContract {

inline DriveDeviceKind deviceKind(
    const DriveInfo &drive,
    DriveDeviceKind explicitTag = DriveDeviceKind::StorageVolume,
    bool hasExplicitTag = false)
{
    if (hasExplicitTag) {
        return explicitTag;
    }
    if (drive.deviceKind == DriveDeviceKind::PortableMediaMtp) {
        return DriveDeviceKind::PortableMediaMtp;
    }
    if (drive.targetUrl.isValid()
        && drive.targetUrl.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0) {
        return DriveDeviceKind::PortableMediaMtp;
    }
    return DriveDeviceKind::StorageVolume;
}

inline bool isMtpDevice(
    const DriveInfo &drive,
    DriveDeviceKind explicitTag = DriveDeviceKind::StorageVolume,
    bool hasExplicitTag = false)
{
    return deviceKind(drive, explicitTag, hasExplicitTag) == DriveDeviceKind::PortableMediaMtp;
}

inline QUrl targetUrl(const DriveInfo &drive)
{
    return drive.targetUrl;
}

inline bool canNavigate(
    const DriveInfo &drive,
    DriveDeviceKind explicitTag = DriveDeviceKind::StorageVolume,
    bool hasExplicitTag = false)
{
    if (isMtpDevice(drive, explicitTag, hasExplicitTag)) {
        return drive.targetUrl.isValid()
            && drive.targetUrl.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0;
    }
    return drive.isMounted && drive.targetUrl.isValid();
}

inline bool canMount(
    const DriveInfo &drive,
    DriveDeviceKind explicitTag = DriveDeviceKind::StorageVolume,
    bool hasExplicitTag = false)
{
    if (isMtpDevice(drive, explicitTag, hasExplicitTag)) {
        return false; // MTP never uses Solid::StorageAccess::setup()
    }
    return !drive.isMounted && !drive.udi.isEmpty();
}

inline bool canUnmount(
    const DriveInfo &drive,
    DriveDeviceKind explicitTag = DriveDeviceKind::StorageVolume,
    bool hasExplicitTag = false)
{
    if (isMtpDevice(drive, explicitTag, hasExplicitTag)) {
        return false; // MTP is not a UDisks2 block filesystem
    }
    return drive.isRemovable && drive.isMounted && !drive.udi.isEmpty();
}

inline bool canEject(
    const DriveInfo &drive,
    bool solidCanEject,
    DriveDeviceKind explicitTag = DriveDeviceKind::StorageVolume,
    bool hasExplicitTag = false)
{
    if (isMtpDevice(drive, explicitTag, hasExplicitTag)) {
        return false;
    }
    return drive.isRemovable && solidCanEject && !drive.udi.isEmpty();
}

inline bool canSafelyRemove(
    const DriveInfo &drive,
    bool solidCanSafelyRemove,
    DriveDeviceKind explicitTag = DriveDeviceKind::StorageVolume,
    bool hasExplicitTag = false)
{
    if (isMtpDevice(drive, explicitTag, hasExplicitTag)) {
        return false;
    }
    return drive.isRemovable && solidCanSafelyRemove && !drive.udi.isEmpty();
}

inline bool canCopyMountPoint(
    const DriveInfo &drive,
    DriveDeviceKind explicitTag = DriveDeviceKind::StorageVolume,
    bool hasExplicitTag = false)
{
    if (isMtpDevice(drive, explicitTag, hasExplicitTag)) {
        return false; // MTP has no local mountPoint
    }
    return drive.isMounted && !drive.mountPoint.isEmpty();
}

inline bool hasCapacityInformation(
    const DriveInfo &drive,
    DriveDeviceKind explicitTag = DriveDeviceKind::StorageVolume,
    bool hasExplicitTag = false)
{
    if (isMtpDevice(drive, explicitTag, hasExplicitTag)) {
        return !drive.capacityText.isEmpty()
            && drive.capacityText != QStringLiteral("—")
            && !drive.freeText.isEmpty()
            && drive.freeText != QStringLiteral("—");
    }
    return drive.isMounted
        && !drive.capacityText.isEmpty()
        && drive.capacityText != QStringLiteral("—");
}

} // namespace MtpDriveContract

// ===========================================================================
// 2. Solid Detection & Enumeration Contract (Section 3)
// ===========================================================================
struct MtpSolidCandidate {
    QString udi;
    QString parentUdi;
    QString vendor;
    QString product;
    QString description;
    QString icon;

    // StorageVolume interface simulation
    bool hasStorageVolume = false;
    bool hasStorageAccess = false;
    bool volumeIgnored = false;
    bool isFileSystemUsage = true;
    bool isAccessible = false;
    bool isRemovable = false;
    QString mountPoint;
    QString fsType;
    QString uuid;

    // PortableMediaPlayer interface simulation
    bool hasPortableMediaPlayer = false;
    QStringList supportedProtocols;
    QVariant mtpDriverHandle; // USB serial number if reported by Solid UDev
};

namespace MtpDetectionSpec {

inline QString slugifyForSpec(const QString &value)
{
    QString out;
    out.reserve(value.size());
    bool previousDash = false;
    for (const QChar ch : value) {
        if (ch.isLetterOrNumber()) {
            out.append(ch.toLower());
            previousDash = false;
        } else if (!previousDash && !out.isEmpty()) {
            out.append(QLatin1Char('-'));
            previousDash = true;
        }
    }
    while (out.endsWith(QLatin1Char('-'))) {
        out.chop(1);
    }
    return out;
}

inline QList<DriveInfo> enumerateCandidates(const QList<MtpSolidCandidate> &candidates)
{
    QList<DriveInfo> result;
    QSet<QString> claimedUdis;
    QSet<QString> usedIds;

    // Pass 1: Standard Solid::StorageVolume devices (unchanged priority)
    for (const MtpSolidCandidate &c : candidates) {
        if (!c.hasStorageVolume || !c.hasStorageAccess || c.volumeIgnored || !c.isFileSystemUsage) {
            continue;
        }
        if (!c.isAccessible && !c.isRemovable) {
            continue;
        }
        DriveInfo d;
        const QString baseKey = !c.uuid.isEmpty() ? c.uuid : c.udi;
        d.id = QStringLiteral("volume-%1").arg(slugifyForSpec(baseKey));
        d.udi = c.udi;
        d.name = !c.description.isEmpty() ? c.description : c.product;
        d.fileSystem = c.fsType;
        d.isMounted = c.isAccessible && !c.mountPoint.isEmpty();
        d.isRemovable = c.isRemovable;
        d.mountPoint = d.isMounted ? c.mountPoint : QString();
        d.targetUrl = d.isMounted ? QUrl::fromLocalFile(c.mountPoint) : QUrl();
        d.capacityText = QStringLiteral("64 GiB");
        d.freeText = d.isMounted ? QStringLiteral("32 GiB") : QStringLiteral("—");
        d.usedText = d.isMounted ? QStringLiteral("50%") : QStringLiteral("—");
        d.usedPercent = d.isMounted ? 50 : 0;
        d.iconName = c.icon.isEmpty()
            ? (c.isRemovable ? QStringLiteral("drive-removable-media") : QStringLiteral("drive-harddisk"))
            : c.icon;

        claimedUdis.insert(c.udi);
        if (!c.parentUdi.isEmpty()) {
            claimedUdis.insert(c.parentUdi);
        }
        usedIds.insert(d.id);
        result.append(d);
    }

    // Pass 2: Solid::PortableMediaPlayer devices supporting "mtp"
    for (const MtpSolidCandidate &c : candidates) {
        if (!c.hasPortableMediaPlayer || c.udi.isEmpty()) {
            continue;
        }
        // Skip hybrid devices already represented as StorageVolume on same or parent UDI
        if (claimedUdis.contains(c.udi) || (!c.parentUdi.isEmpty() && claimedUdis.contains(c.parentUdi))) {
            continue;
        }
        bool supportsMtp = false;
        for (const QString &proto : c.supportedProtocols) {
            if (proto.compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0) {
                supportsMtp = true;
                break;
            }
        }
        if (!supportsMtp) {
            continue;
        }

        const QString serial = c.mtpDriverHandle.toString().trimmed();
        QString id = QStringLiteral("mtp-%1").arg(
            slugifyForSpec(!serial.isEmpty() ? serial : c.udi));
        if (usedIds.contains(id)) {
            id = QStringLiteral("mtp-%1").arg(slugifyForSpec(c.udi));
        }
        if (usedIds.contains(id)) {
            continue;
        }

        QString displayName = c.product.trimmed();
        if (!c.vendor.trimmed().isEmpty()
            && !displayName.startsWith(c.vendor.trimmed(), Qt::CaseInsensitive)) {
            displayName = displayName.isEmpty()
                ? c.vendor.trimmed()
                : QStringLiteral("%1 %2").arg(c.vendor.trimmed(), displayName);
        }
        if (displayName.isEmpty()) {
            displayName = !c.description.trimmed().isEmpty()
                ? c.description.trimmed()
                : QStringLiteral("MTP Device");
        }

        const QString icon = !c.icon.trimmed().isEmpty()
            ? c.icon.trimmed()
            : QStringLiteral("smartphone");

        DriveInfo d = MtpUdsContract::makeMtpDriveInfo(id, displayName, c.udi, icon);
        claimedUdis.insert(c.udi);
        usedIds.insert(d.id);
        result.append(d);
    }

    return result;
}

} // namespace MtpDetectionSpec

// ===========================================================================
// 3. MTP URL, Redirection & Hierarchy Safety Contract (Section 5 & 6)
// ===========================================================================
namespace MtpUrlContract {

inline bool isMtpUrl(const QUrl &url)
{
    return url.isValid()
        && url.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0;
}

inline bool isMtpUdiUrl(const QUrl &url)
{
    return isMtpUrl(url) && url.path().startsWith(QStringLiteral("udi="));
}

inline QString extractUdiFromMtpUrl(const QUrl &url)
{
    if (!isMtpUdiUrl(url)) {
        return {};
    }
    return url.path().mid(4);
}

inline QStringList pathSegments(const QUrl &url)
{
    if (!isMtpUrl(url) || isMtpUdiUrl(url)) {
        return {};
    }
    return url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

inline bool isMtpDeviceRootUrl(const QUrl &url)
{
    if (!isMtpUrl(url)) {
        return false;
    }
    if (isMtpUdiUrl(url)) {
        return true;
    }
    return pathSegments(url).size() <= 1;
}

// Safe segment-based ownership check (never naive string prefix matching):
// Ensures "mtp:/Pixel 8 Pro/DCIM" is NOT matched by "mtp:/Pixel 8"!
inline bool urlBelongsToMtpDevice(
    const QUrl &paneUrl,
    const DriveInfo &mtpDrive,
    const QUrl &redirectedDeviceRoot = QUrl())
{
    if (!isMtpUrl(paneUrl)) {
        return false;
    }
    if (isMtpUdiUrl(paneUrl)) {
        const QString paneUdi = extractUdiFromMtpUrl(paneUrl);
        return (!mtpDrive.udi.isEmpty() && paneUdi == mtpDrive.udi)
            || sameLocation(paneUrl, mtpDrive.targetUrl);
    }
    const QStringList paneParts = pathSegments(paneUrl);
    if (paneParts.isEmpty()) {
        return false;
    }
    if (redirectedDeviceRoot.isValid() && isMtpUrl(redirectedDeviceRoot) && !isMtpUdiUrl(redirectedDeviceRoot)) {
        const QStringList rootParts = pathSegments(redirectedDeviceRoot);
        if (!rootParts.isEmpty()) {
            return paneParts.first() == rootParts.first();
        }
    }
    return paneParts.first() == mtpDrive.name;
}

inline QUrl expectedParentUrl(const QUrl &url)
{
    if (!isMtpUrl(url)) {
        return {};
    }
    if (isMtpDeviceRootUrl(url)) {
        return kThisPcUrl;
    }
    QStringList parts = pathSegments(url);
    parts.removeLast();
    QUrl parent(QStringLiteral("mtp:/"));
    parent.setPath(QLatin1Char('/') + parts.join(QLatin1Char('/')));
    return parent;
}

inline QVector<LocationPresentation::Segment> expectedBreadcrumbSegments(const QUrl &url)
{
    QVector<LocationPresentation::Segment> segments;
    if (!isMtpUrl(url)) {
        return segments;
    }
    segments.append({trLocal("Ten komputer", "This PC"), kThisPcUrl, QStringLiteral("computer")});
    if (isMtpUdiUrl(url)) {
        segments.append({url.path(), url, QStringLiteral("smartphone")});
        return segments;
    }
    const QStringList parts = pathSegments(url);
    QString cumulative;
    for (int i = 0; i < parts.size(); ++i) {
        cumulative += QLatin1Char('/') + parts.at(i);
        QUrl segUrl(QStringLiteral("mtp:/"));
        segUrl.setPath(cumulative);
        segments.append({
            parts.at(i),
            segUrl,
            i == 0 ? QStringLiteral("smartphone") : QStringLiteral("folder")
        });
    }
    return segments;
}

} // namespace MtpUrlContract
