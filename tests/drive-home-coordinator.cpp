/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Appended to the temporary regression binary by run-pane-actions.py.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

#include "mtp-contract-spec.h"

struct FakeDriveSourceState
{
    DriveHomeSource::EntriesCallback entries;
    DriveHomeSource::FinishedCallback finished;
    bool canceled = false;
};

class FakeDriveSource final : public DriveHomeSource
{
public:
    explicit FakeDriveSource(std::shared_ptr<FakeDriveSourceState> state)
        : m_state(std::move(state)) {}
    void cancel() override { m_state->canceled = true; }
private:
    std::shared_ptr<FakeDriveSourceState> m_state;
};

static KIO::UDSEntry driveEntry(
    const QString &name, const QString &target, const QString &used,
    const QString &free = QStringLiteral("60 GiB"),
    const QString &capacity = QStringLiteral("100 GiB"),
    const QString &fileSystem = QStringLiteral("ext4"),
    const QString &mountPoint = QStringLiteral("/mnt/data"),
    const QString &icon = QStringLiteral("drive-harddisk"))
{
    KIO::UDSEntry entry;
    entry.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, name);
    entry.fastInsert(KIO::UDSEntry::UDS_TARGET_URL, target);
    entry.fastInsert(KIO::UDSEntry::UDS_ICON_NAME, icon);
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 0, free);
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 1, capacity);
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 2, used);
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 3, fileSystem);
    entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 4, mountPoint);
    return entry;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QList<std::shared_ptr<FakeDriveSourceState>> requests;
    DriveHomeCoordinator coordinator(
        [&requests](DriveHomeSource::EntriesCallback entries,
                    DriveHomeSource::FinishedCallback finished) {
            auto state = std::make_shared<FakeDriveSourceState>();
            state->entries = std::move(entries);
            state->finished = std::move(finished);
            requests.push_back(state);
            return std::make_unique<FakeDriveSource>(state);
        });

    int started = 0;
    int finished = 0;
    int errors = 0;
    int drivesChangedCount = 0;
    QList<DriveInfo> primarySnapshot;
    QList<DriveInfo> splitSnapshot;
    QObject::connect(&coordinator, &DriveHomeCoordinator::loadingStarted,
                     [&] { ++started; });
    QObject::connect(&coordinator, &DriveHomeCoordinator::loadingFinished,
                     [&](bool) { ++finished; });
    QObject::connect(&coordinator, &DriveHomeCoordinator::error,
                     [&](const QString &) { ++errors; });
    QObject::connect(&coordinator, &DriveHomeCoordinator::drivesChanged,
                     [&](const QList<DriveInfo> &drives) {
        ++drivesChangedCount;
        primarySnapshot = drives;
        splitSnapshot = drives;
    });

    verify(coordinator.drives().isEmpty() && !coordinator.isLoading(),
           "initial snapshot is empty and idle");
    coordinator.refresh();
    verify(started == 1 && coordinator.isLoading() && requests.size() == 1,
           "refresh starts one source");
    KIO::UDSEntry dotEntry;
    dotEntry.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("."));
    dotEntry.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, QStringLiteral("Ten komputer"));

    requests[0]->entries({
        dotEntry,
        driveEntry("Local", "file:///", "40%", "60 GiB", "100 GiB", "ext4", "/"),
        driveEntry("USB", "file:///media/usb", "125%", "0 B", "32 GiB", "vfat", "/media/usb", "drive-removable-media"),
        driveEntry("Duplicate", "file:///media/usb/", "20%"),
        driveEntry(QString(), "file:///invalid-name", "1%"),
        driveEntry("No target", QString(), "1%"),
        driveEntry("NTFS", "file:///mnt/windows", "not-a-percent", "8 GiB", "64 GiB", "ntfs3", "/mnt/windows")});
    requests[0]->finished(true, {});

    const auto &drives = coordinator.drives();
    verify(!coordinator.isLoading() && finished == 1 && errors == 0,
           "successful load finishes cleanly");
    verify(drivesChangedCount == 1, "initial load emits drivesChanged exactly once");
    verify(drives.size() == 3, "invalid and duplicate entries are suppressed");
    verify(drives[0].name == "Local" && drives[1].name == "USB" && drives[2].name == "NTFS",
           "source order is deterministic and stable");
    verify(drives[0].freeText == "60 GiB" && drives[0].capacityText == "100 GiB"
               && drives[0].usedText == "40%" && drives[0].usedPercent == 40,
           "capacity fields and used percentage are mapped");
    verify(drives[1].usedPercent == 100 && drives[2].usedPercent == 0,
           "percentage is clamped and invalid values are safe");
    verify(drives[1].iconName.contains("removable") && drives[2].fileSystem == "ntfs3"
               && drives[2].mountPoint == "/mnt/windows",
           "removable and NTFS classification data is preserved");
    verify(primarySnapshot.size() == splitSnapshot.size()
               && primarySnapshot[1].targetUrl == splitSnapshot[1].targetUrl,
           "primary and split consumers receive an identical snapshot");

    // Identical refresh: must NOT emit drivesChanged
    coordinator.refresh();
    verify(requests.size() == 2, "second refresh created request");
    requests[1]->entries({
        dotEntry,
        driveEntry("Local", "file:///", "40%", "60 GiB", "100 GiB", "ext4", "/"),
        driveEntry("USB", "file:///media/usb", "125%", "0 B", "32 GiB", "vfat", "/media/usb", "drive-removable-media"),
        driveEntry("NTFS", "file:///mnt/windows", "not-a-percent", "8 GiB", "64 GiB", "ntfs3", "/mnt/windows")});
    requests[1]->finished(true, {});
    verify(drivesChangedCount == 1,
           "identical snapshot does NOT emit drivesChanged or cause spurious rebuild");

    // Real change (mounted -> unmounted transition): MUST emit drivesChanged
    coordinator.refresh();
    verify(requests.size() == 3, "third refresh created request");
    KIO::UDSEntry unmountedUsb;
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("volume-usb"));
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, QStringLiteral("USB"));
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_ICON_NAME, QStringLiteral("drive-removable-media"));
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_EXTRA + 0, QStringLiteral("—"));
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_EXTRA + 1, QStringLiteral("32 GiB"));
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_EXTRA + 2, QStringLiteral("—"));
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_EXTRA + 3, QStringLiteral("vfat"));
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_EXTRA + 4, QString());
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_EXTRA + 5, QStringLiteral("0"));
    unmountedUsb.fastInsert(KIO::UDSEntry::UDS_EXTRA + 6, QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdb1"));
    requests[2]->entries({
        driveEntry("Local", "file:///", "40%", "60 GiB", "100 GiB", "ext4", "/"),
        unmountedUsb,
        driveEntry("NTFS", "file:///mnt/windows", "not-a-percent", "8 GiB", "64 GiB", "ntfs3", "/mnt/windows")});
    requests[2]->finished(true, {});
    verify(drivesChangedCount == 2,
           "real change in mount state emits drivesChanged for UI rebuild");
    verify(!coordinator.drives()[1].isMounted,
           "unmounted state correctly updated in snapshot");

    coordinator.refresh();
    auto replaced = requests[3];
    coordinator.refresh();
    auto current = requests[4];
    verify(replaced->canceled && coordinator.isLoading() && started == 5,
           "repeated refresh cancels and replaces the old source");
    replaced->entries({driveEntry("Late", "file:///late", "1%")});
    replaced->finished(true, {});
    verify(coordinator.drives().size() == 3 && finished == 3,
           "late callbacks from a canceled source are ignored");
    current->entries({driveEntry("Fresh", "file:///fresh", "7%")});
    current->finished(true, {});
    verify(coordinator.drives().size() == 1 && coordinator.drives()[0].name == "Fresh"
               && finished == 4,
           "replacement source publishes its own snapshot");

    coordinator.refresh();
    requests[5]->entries({driveEntry("Partial", "file:///partial", "9%")});
    requests[5]->finished(false, "fixture failure");
    verify(errors == 1 && finished == 5 && coordinator.drives().size() == 1
               && coordinator.drives()[0].name == "Fresh",
           "error preserves the previous safe snapshot");
    coordinator.cancel();
    verify(!coordinator.isLoading(), "explicit cancel returns to idle state");

    // =======================================================================
    // Stage 1.2: MTP Device Type & Capability Contract (Section 2)
    // =======================================================================
    {
        DriveInfo mountedVolume;
        mountedVolume.id = QStringLiteral("volume-root");
        mountedVolume.name = QStringLiteral("System");
        mountedVolume.udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/nvme0n1p2");
        mountedVolume.mountPoint = QStringLiteral("/");
        mountedVolume.targetUrl = QUrl::fromLocalFile(QStringLiteral("/"));
        mountedVolume.capacityText = QStringLiteral("500 GiB");
        mountedVolume.freeText = QStringLiteral("200 GiB");
        mountedVolume.usedText = QStringLiteral("60%");
        mountedVolume.usedPercent = 60;
        mountedVolume.fileSystem = QStringLiteral("btrfs");
        mountedVolume.isMounted = true;
        mountedVolume.isRemovable = false;

        DriveInfo unmountedUsb;
        unmountedUsb.id = QStringLiteral("volume-usb");
        unmountedUsb.name = QStringLiteral("Kingston USB");
        unmountedUsb.udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdb1");
        unmountedUsb.mountPoint.clear();
        unmountedUsb.targetUrl = QUrl();
        unmountedUsb.capacityText = QStringLiteral("32 GiB");
        unmountedUsb.freeText = QStringLiteral("—");
        unmountedUsb.usedText = QStringLiteral("—");
        unmountedUsb.fileSystem = QStringLiteral("vfat");
        unmountedUsb.isMounted = false;
        unmountedUsb.isRemovable = true;

        const QString mtpUdi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2");
        const DriveInfo mtpPhone = MtpUdsContract::makeMtpDriveInfo(
            QStringLiteral("mtp-pixel8-serial01"),
            QStringLiteral("Google Pixel 8"),
            mtpUdi);

        verify(MtpDriveContract::deviceKind(mountedVolume) == DriveDeviceKind::StorageVolume,
               "mounted block volume classified as StorageVolume");
        verify(MtpDriveContract::deviceKind(unmountedUsb) == DriveDeviceKind::StorageVolume,
               "unmounted removable volume classified as StorageVolume");
        verify(MtpDriveContract::deviceKind(mtpPhone) == DriveDeviceKind::PortableMediaMtp,
               "MTP phone classified as PortableMediaMtp");

        // Capability matrix for PortableMediaMtp vs StorageVolume
        verify(MtpDriveContract::canNavigate(mountedVolume), "mounted volume canNavigate");
        verify(!MtpDriveContract::canNavigate(unmountedUsb), "unmounted volume cannot navigate directly");
        verify(MtpDriveContract::canNavigate(mtpPhone), "MTP phone canNavigate directly via mtp:udi=");

        verify(!MtpDriveContract::canMount(mountedVolume), "mounted volume canMount is false");
        verify(MtpDriveContract::canMount(unmountedUsb), "unmounted volume canMount is true");
        verify(!MtpDriveContract::canMount(mtpPhone), "MTP phone canMount is false (no Solid::StorageAccess::setup)");

        verify(!MtpDriveContract::canUnmount(mountedVolume), "fixed volume canUnmount is false");
        verify(!MtpDriveContract::canUnmount(unmountedUsb), "unmounted volume canUnmount is false");
        verify(!MtpDriveContract::canUnmount(mtpPhone), "MTP phone canUnmount is false (no UDisks2 unmount)");

        verify(!MtpDriveContract::canEject(mtpPhone, true), "MTP phone canEject is always false");
        verify(!MtpDriveContract::canSafelyRemove(mtpPhone, true), "MTP phone canSafelyRemove is always false");

        verify(MtpDriveContract::canCopyMountPoint(mountedVolume), "mounted volume canCopyMountPoint is true");
        verify(!MtpDriveContract::canCopyMountPoint(unmountedUsb), "unmounted volume canCopyMountPoint is false");
        verify(!MtpDriveContract::canCopyMountPoint(mtpPhone), "MTP phone canCopyMountPoint is false");

        verify(MtpDriveContract::hasCapacityInformation(mountedVolume), "mounted volume hasCapacityInformation is true");
        verify(!MtpDriveContract::hasCapacityInformation(unmountedUsb), "unmounted volume hasCapacityInformation is false");
        verify(!MtpDriveContract::hasCapacityInformation(mtpPhone), "MTP phone without card-level capacity hasCapacityInformation is false");

        verify(MtpDriveContract::targetUrl(mtpPhone) == QUrl(QStringLiteral("mtp:udi=%1").arg(mtpUdi)),
               "MTP phone targetUrl uses mtp:udi=<UDI> scheme");
        verify(mtpPhone.mountPoint.isEmpty(), "MTP phone never gets a fake local mountPoint");
    }

    // =======================================================================
    // Stage 1.2: Solid Detection & Classification Contract (Section 3, Cases 1-7)
    // =======================================================================
    {
        MtpSolidCandidate blockVol;
        blockVol.udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/nvme0n1p2");
        blockVol.parentUdi = QStringLiteral("/org/freedesktop/UDisks2/drives/Samsung_SSD");
        blockVol.description = QStringLiteral("System");
        blockVol.hasStorageVolume = true;
        blockVol.hasStorageAccess = true;
        blockVol.isAccessible = true;
        blockVol.mountPoint = QStringLiteral("/");
        blockVol.fsType = QStringLiteral("btrfs");
        blockVol.uuid = QStringLiteral("uuid-root-01");

        MtpSolidCandidate mtpPhone1;
        mtpPhone1.udi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2");
        mtpPhone1.vendor = QStringLiteral("Google");
        mtpPhone1.product = QStringLiteral("Pixel 8 Pro");
        mtpPhone1.icon = QStringLiteral("smartphone");
        mtpPhone1.hasPortableMediaPlayer = true;
        mtpPhone1.supportedProtocols = {QStringLiteral("mtp")};
        mtpPhone1.mtpDriverHandle = QStringLiteral("SERIAL_PIXEL8_A");

        MtpSolidCandidate ptpCamera;
        ptpCamera.udi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-3");
        ptpCamera.vendor = QStringLiteral("Canon");
        ptpCamera.product = QStringLiteral("EOS R6");
        ptpCamera.hasPortableMediaPlayer = true;
        ptpCamera.supportedProtocols = {QStringLiteral("ptp")}; // No MTP!

        MtpSolidCandidate emptyPlayer;
        emptyPlayer.udi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-4");
        emptyPlayer.product = QStringLiteral("Generic Player");
        emptyPlayer.hasPortableMediaPlayer = true;
        emptyPlayer.supportedProtocols = {}; // Empty protocols

        // Hybrid device: exposes both StorageVolume and PortableMediaPlayer on same UDI
        MtpSolidCandidate hybridUsb;
        hybridUsb.udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdc1");
        hybridUsb.parentUdi = QStringLiteral("/org/freedesktop/UDisks2/drives/Walkman_USB");
        hybridUsb.description = QStringLiteral("Walkman Storage");
        hybridUsb.hasStorageVolume = true;
        hybridUsb.hasStorageAccess = true;
        hybridUsb.isAccessible = true;
        hybridUsb.isRemovable = true;
        hybridUsb.mountPoint = QStringLiteral("/run/media/user/WALKMAN");
        hybridUsb.fsType = QStringLiteral("vfat");
        hybridUsb.uuid = QStringLiteral("1234-ABCD");
        hybridUsb.hasPortableMediaPlayer = true;
        hybridUsb.supportedProtocols = {QStringLiteral("mtp")};

        // 1-4: StorageVolume kept, MTP kept, non-MTP skipped, hybrid deduplicated
        const QList<DriveInfo> pass1 = MtpDetectionSpec::enumerateCandidates({
            blockVol, mtpPhone1, ptpCamera, emptyPlayer, hybridUsb
        });
        verify(pass1.size() == 3, "Detection 1-4: exactly 3 devices (root volume, walkman volume, MTP phone); PTP/empty skipped, hybrid deduplicated");
        verify(pass1[0].name == QStringLiteral("System") && pass1[0].targetUrl == QUrl::fromLocalFile(QStringLiteral("/")),
               "Detection 1: StorageVolume path works unchanged");
        verify(pass1[1].name == QStringLiteral("Walkman Storage") && pass1[1].targetUrl == QUrl::fromLocalFile(QStringLiteral("/run/media/user/WALKMAN")),
               "Detection 4: Hybrid device represented once as StorageVolume without MTP duplicate");
        verify(pass1[2].name == QStringLiteral("Google Pixel 8 Pro")
                   && pass1[2].targetUrl == QUrl(QStringLiteral("mtp:udi=%1").arg(mtpPhone1.udi))
                   && pass1[2].mountPoint.isEmpty(),
               "Detection 2 & 3: PortableMediaPlayer with 'mtp' qualified as MTP; non-MTP skipped");

        // 5: Repeated enumeration -> identical logical DriveInfo
        const QList<DriveInfo> pass2 = MtpDetectionSpec::enumerateCandidates({
            blockVol, mtpPhone1, ptpCamera, emptyPlayer, hybridUsb
        });
        verify(pass1 == pass2, "Detection 5: repeated enumeration produces identical DriveInfo list (operator==)");

        // 6: Two different phones simultaneously (even same model name on different USB ports)
        MtpSolidCandidate mtpPhone2 = mtpPhone1;
        mtpPhone2.udi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-6");
        mtpPhone2.mtpDriverHandle = QStringLiteral("SERIAL_PIXEL8_B");
        const QList<DriveInfo> twoPhones = MtpDetectionSpec::enumerateCandidates({mtpPhone1, mtpPhone2});
        verify(twoPhones.size() == 2, "Detection 6: two phones produce two independent entries");
        verify(twoPhones[0].id != twoPhones[1].id
                   && twoPhones[0].udi != twoPhones[1].udi
                   && twoPhones[0].targetUrl != twoPhones[1].targetUrl,
               "Detection 6: two phones have distinct id, udi, and targetUrl");

        // 7: Reconnect on different USB port -> UDI updates to new port, no stale entry
        MtpSolidCandidate reconnectedPhone = mtpPhone1;
        reconnectedPhone.udi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-8");
        const QList<DriveInfo> afterReconnect = MtpDetectionSpec::enumerateCandidates({blockVol, reconnectedPhone});
        verify(afterReconnect.size() == 2, "Detection 7: reconnect produces clean list without stale card");
        verify(afterReconnect[1].udi == reconnectedPhone.udi
                   && afterReconnect[1].targetUrl == QUrl(QStringLiteral("mtp:udi=%1").arg(reconnectedPhone.udi)),
               "Detection 7: reconnected phone updates UDI and mtp:udi= targetUrl to new USB port");
        verify(afterReconnect[1].id == pass1[2].id,
               "Detection 7: cross-port USB serial preserves logical id while updating session UDI");
    }

    // =======================================================================
    // Stage 1.2: DriveHomeCoordinator MTP Snapshot, Stability & Hotplug (Section 4 & 6)
    // =======================================================================
    {
        const QString udiPhoneA = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2");
        const QString udiPhoneB = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-5");

        const int prevChangedCount = drivesChangedCount;

        // 1. Phone connected with 2 MTP devices + local ext4 drive
        coordinator.refresh();
        auto mtpReq1 = requests.back();
        mtpReq1->entries({
            dotEntry,
            driveEntry("Local", "file:///", "40%", "60 GiB", "100 GiB", "ext4", "/"),
            MtpUdsContract::makeMtpUdsEntry(
                QStringLiteral("mtp-pixel8-a"),
                QStringLiteral("Google Pixel 8"),
                udiPhoneA),
            MtpUdsContract::makeMtpUdsEntry(
                QStringLiteral("mtp-galaxy-b"),
                QStringLiteral("Samsung Galaxy S24"),
                udiPhoneB)
        });
        mtpReq1->finished(true, {});

        verify(drivesChangedCount == prevChangedCount + 1,
               "MTP snapshot load emits drivesChanged once");
        verify(coordinator.drives().size() == 3,
               "DriveHomeCoordinator accepts local drive + 2 MTP phones");

        const DriveInfo &pixel = coordinator.drives().at(1);
        const DriveInfo &galaxy = coordinator.drives().at(2);
        verify(pixel.name == QStringLiteral("Google Pixel 8")
                   && pixel.targetUrl == QUrl(QStringLiteral("mtp:udi=%1").arg(udiPhoneA))
                   && pixel.targetUrl.scheme() == QStringLiteral("mtp"),
               "MTP phone 1 has valid mtp:udi= targetUrl in coordinator snapshot");
        verify(pixel.mountPoint.isEmpty(),
               "MTP phone 1 has empty mountPoint (no fake local path)");
        verify(pixel.freeText == QStringLiteral("—")
                   && pixel.capacityText == QStringLiteral("—")
                   && pixel.usedText == QStringLiteral("—")
                   && pixel.usedPercent == 0,
               "MTP phone 1 missing capacity is represented as '—' and 0%, not 0 B");
        verify(pixel.isMounted && pixel.isRemovable && pixel.udi == udiPhoneA,
               "MTP phone 1 is flagged navigable/removable and preserves Solid UDI");
        verify(galaxy.name == QStringLiteral("Samsung Galaxy S24")
                   && galaxy.udi == udiPhoneB
                   && galaxy.targetUrl != pixel.targetUrl,
               "MTP phone 2 coexists independently in coordinator snapshot");

        // 2. Identical MTP refresh -> no spurious drivesChanged (no UI flicker)
        coordinator.refresh();
        auto mtpReq2 = requests.back();
        mtpReq2->entries({
            dotEntry,
            driveEntry("Local", "file:///", "40%", "60 GiB", "100 GiB", "ext4", "/"),
            MtpUdsContract::makeMtpUdsEntry(
                QStringLiteral("mtp-pixel8-a"),
                QStringLiteral("Google Pixel 8"),
                udiPhoneA),
            MtpUdsContract::makeMtpUdsEntry(
                QStringLiteral("mtp-galaxy-b"),
                QStringLiteral("Samsung Galaxy S24"),
                udiPhoneB)
        });
        mtpReq2->finished(true, {});
        verify(drivesChangedCount == prevChangedCount + 1,
               "identical MTP snapshot refresh does NOT emit drivesChanged (no UI flicker)");

        // 3. Disconnect Phone A (or switch Phone A to charging-only mode) -> removes Phone A, keeps Phone B
        coordinator.refresh();
        auto mtpReq3 = requests.back();
        mtpReq3->entries({
            dotEntry,
            driveEntry("Local", "file:///", "40%", "60 GiB", "100 GiB", "ext4", "/"),
            MtpUdsContract::makeMtpUdsEntry(
                QStringLiteral("mtp-galaxy-b"),
                QStringLiteral("Samsung Galaxy S24"),
                udiPhoneB)
        });
        mtpReq3->finished(true, {});
        verify(drivesChangedCount == prevChangedCount + 2,
               "unplugging Phone A emits drivesChanged");
        verify(coordinator.drives().size() == 2
                   && coordinator.drives().at(1).udi == udiPhoneB,
               "unplugged Phone A is removed without leaving stale card; Phone B remains intact");

        // 4. Reconnect Phone A on a different USB port -> new UDI replaces old cleanly
        const QString udiPhoneANewPort = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-9");
        coordinator.refresh();
        auto mtpReq4 = requests.back();
        mtpReq4->entries({
            dotEntry,
            driveEntry("Local", "file:///", "40%", "60 GiB", "100 GiB", "ext4", "/"),
            MtpUdsContract::makeMtpUdsEntry(
                QStringLiteral("mtp-pixel8-a"),
                QStringLiteral("Google Pixel 8"),
                udiPhoneANewPort),
            MtpUdsContract::makeMtpUdsEntry(
                QStringLiteral("mtp-galaxy-b"),
                QStringLiteral("Samsung Galaxy S24"),
                udiPhoneB)
        });
        mtpReq4->finished(true, {});
        verify(drivesChangedCount == prevChangedCount + 3,
               "reconnecting Phone A on new USB port emits drivesChanged");
        verify(coordinator.drives().size() == 3
                   && coordinator.drives().at(1).udi == udiPhoneANewPort
                   && coordinator.drives().at(1).targetUrl == QUrl(QStringLiteral("mtp:udi=%1").arg(udiPhoneANewPort)),
               "reconnected Phone A has updated UDI and mtp:udi= targetUrl");

        // Stage 1.3: DriveInfo and DriveHomeCoordinator deserialize UDS_EXTRA+9 ('mtp' / 'volume')
        // into explicit DriveDeviceKind and expose native capability helpers.
        const DriveInfo &localDrive = coordinator.drives().at(0);
        const DriveInfo &reconnectedPixel = coordinator.drives().at(1);
        verify(localDrive.deviceKind == DriveDeviceKind::StorageVolume && !localDrive.isMtpDevice(),
               "DriveHomeCoordinator deserializes local volume into DriveDeviceKind::StorageVolume");
        verify(reconnectedPixel.deviceKind == DriveDeviceKind::PortableMediaMtp && reconnectedPixel.isMtpDevice(),
               "DriveInfo and DriveHomeCoordinator deserialize UDS_EXTRA+9 ('mtp') into explicit DriveDeviceKind::PortableMediaMtp");
        verify(reconnectedPixel.canNavigate()
                   && !reconnectedPixel.canMount()
                   && !reconnectedPixel.canUnmount()
                   && !reconnectedPixel.canEject(true)
                   && !reconnectedPixel.canSafelyRemove(true)
                   && !reconnectedPixel.canCopyMountPoint()
                   && !reconnectedPixel.hasCapacityInformation(),
               "DriveInfo capability methods enforce MTP contract natively");

        DriveInfo diffKindCopy = reconnectedPixel;
        diffKindCopy.deviceKind = DriveDeviceKind::StorageVolume;
        verify(reconnectedPixel != diffKindCopy,
               "DriveInfo::operator== compares explicit deviceKind");
    }

    qInfo("PASS: %d DriveHomeCoordinator assertions (%d pending MTP specs); parsing, snapshots, dedup, replacement, MTP contract",
          checks, pendingMtpSpecs);
}
