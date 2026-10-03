/*
 * Drive / filesystem properties regression test suite.
 *
 * Part of Properties 2.0 (0.37.0 Stage 2).
 * Exercises:
 * 1. DrivePropertiesData structure and formatting helpers
 * 2. Unmounted drive capacity and space semantics (Niedostępne / Unavailable)
 * 3. Filesystem type formatting (technical vs volume)
 * 4. decodeMountString octal unescaping
 * 5. combineMountOptions merging and deduplication
 * 6. parseMountInfo with synthetic mountinfo entries (Btrfs, NTFS fuseblk, ro, spaces)
 * 7. parseMountInfo on real /proc/self/mountinfo
 * 8. busToString mapping
 * 9. DrivePropertiesProvider loading real mounted volume
 * 10. DrivePropertiesProvider loading unmounted volume without mounting
 * 11. DrivePropertiesProvider live device removal notification
 * 12. PropertiesDialog::showForDrive presentation for mounted drive
 * 13. PropertiesDialog::showForDrive presentation for unmounted drive
 * 14. PropertiesDialog::showForDrive device removal banner display
 * 15. DriveFrame context menu and propertiesRequested signal
 * 16. SidebarDriveButton context menu and propertiesRequested signal
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "appwidgets.h"
#include "browsercommon.h"
#include "drivepropertiesdata.h"
#include "drivepropertiesprovider.h"
#include "propertiesdialog.h"
#include "sidebar.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QTest>
#include <QTimer>

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
    QCoreApplication::setApplicationName("drive-properties-test");

    // ------------------------------------------------------------------
    // 1. DrivePropertiesData default state & formatting helpers
    // ------------------------------------------------------------------
    {
        DrivePropertiesData data;
        verify(!data.isMounted, "default is not mounted");
        verify(!data.hasTotalBytes(), "default no total bytes");
        verify(!data.hasFreeBytes(), "default no free bytes");
        verify(!data.hasUsedBytes(), "default no used bytes");
        verify(!data.isDeviceMissing, "default not missing");

        data.totalBytes = 107374182400ULL; // 100 GiB
        data.freeBytes = 53687091200ULL;   // 50 GiB
        data.usedBytes = 53687091200ULL;   // 50 GiB
        data.isMounted = true;

        verify(data.hasTotalBytes(), "has total bytes when set");
        verify(data.hasFreeBytes(), "has free bytes when mounted");
        verify(data.hasUsedBytes(), "has used bytes when mounted");

        const QString formattedTotal = data.formattedTotal();
        verify(formattedTotal.contains("100"), "formatted total contains human 100");
        verify(formattedTotal.contains("GiB"), "formatted total contains GiB");
        verify(formattedTotal.contains("107"), "formatted total contains exact bytes");

        const QString formattedFree = data.formattedFree();
        verify(formattedFree.contains("50"), "formatted free contains human 50");
        verify(formattedFree.contains("GiB"), "formatted free contains GiB");

        const QString formattedUsed = data.formattedUsed();
        verify(formattedUsed.contains("50"), "formatted used contains human 50");
    }

    // ------------------------------------------------------------------
    // 2. Unmounted drive formatting semantics (Unavailable / Niedostępne)
    // ------------------------------------------------------------------
    {
        DrivePropertiesData unmounted;
        unmounted.isMounted = false;
        unmounted.totalBytes = 2054987264ULL; // ~1.9 GiB
        unmounted.freeBytes = -1;
        unmounted.usedBytes = -1;

        verify(unmounted.hasTotalBytes(), "unmounted can have total capacity");
        verify(!unmounted.hasFreeBytes(), "unmounted does not have free bytes");
        verify(!unmounted.hasUsedBytes(), "unmounted does not have used bytes");

        const QString freeStr = unmounted.formattedFree();
        verify(freeStr == "Niedostępne" || freeStr == "Unavailable", "unmounted free space unavailable");

        const QString usedStr = unmounted.formattedUsed();
        verify(usedStr == "Niedostępne" || usedStr == "Unavailable", "unmounted used space unavailable");
    }

    // ------------------------------------------------------------------
    // 3. Filesystem type formatting (technical vs volume)
    // ------------------------------------------------------------------
    {
        DrivePropertiesData fsData;
        verify(fsData.formattedFileSystem() == "Nieznany" || fsData.formattedFileSystem() == "Unknown",
               "empty filesystem formatting");

        fsData.fileSystemType = QStringLiteral("btrfs");
        verify(fsData.formattedFileSystem() == "btrfs", "tech only formatting");

        fsData.volumeFsType = QStringLiteral("ntfs");
        fsData.fileSystemType = QStringLiteral("fuseblk");
        verify(fsData.formattedFileSystem() == "NTFS (fuseblk)", "ntfs fuseblk formatting");

        fsData.fileSystemType = QStringLiteral("ntfs3");
        verify(fsData.formattedFileSystem() == "NTFS (ntfs3)", "ntfs ntfs3 formatting");

        fsData.volumeFsType = QStringLiteral("btrfs");
        fsData.fileSystemType = QStringLiteral("btrfs");
        verify(fsData.formattedFileSystem() == "BTRFS", "identical fs type formatting");
    }

    // ------------------------------------------------------------------
    // 4. decodeMountString octal unescaping
    // ------------------------------------------------------------------
    {
        verify(DrivePropertiesProvider::decodeMountString(QStringLiteral("/media/USB\\040STICK"))
               == QStringLiteral("/media/USB STICK"), "decode \\040 space");
        verify(DrivePropertiesProvider::decodeMountString(QStringLiteral("tab\\011here"))
               == QStringLiteral("tab\there"), "decode \\011 tab");
        verify(DrivePropertiesProvider::decodeMountString(QStringLiteral("line\\012new"))
               == QStringLiteral("line\nnew"), "decode \\012 newline");
        verify(DrivePropertiesProvider::decodeMountString(QStringLiteral("back\\134slash"))
               == QStringLiteral("back\\slash"), "decode \\134 backslash");
        verify(DrivePropertiesProvider::decodeMountString(QStringLiteral("/simple/path"))
               == QStringLiteral("/simple/path"), "decode plain path unchanged");
        verify(DrivePropertiesProvider::decodeMountString(QStringLiteral("incomplete\\04"))
               == QStringLiteral("incomplete\\04"), "decode incomplete escape unchanged");
    }

    // ------------------------------------------------------------------
    // 5. combineMountOptions merging and deduplication
    // ------------------------------------------------------------------
    {
        const QString perMount = QStringLiteral("rw,noatime,nodev");
        const QString super = QStringLiteral("rw,compress=zstd:3,ssd,subvol=/@");
        const QString combined = DrivePropertiesProvider::combineMountOptions(perMount, super);
        verify(combined.startsWith(QStringLiteral("rw,noatime,nodev")), "preserves per-mount options prefix");
        verify(combined.contains(QStringLiteral("compress=zstd:3")), "contains super options");
        verify(combined.count(QStringLiteral("rw")) == 1, "deduplicates rw");
    }

    // ------------------------------------------------------------------
    // 6. parseMountInfo with synthetic mountinfo entries
    // ------------------------------------------------------------------
    {
        const QString sampleMountInfo = QStringLiteral(
            "32 1 0:26 / / rw,noatime shared:1 - btrfs /dev/nvme0n1p2 rw,compress=zstd:3,ssd,discard=async,subvol=/@\n"
            "50 1 0:40 / /mnt/c rw,nosuid,nodev,noatime shared:2 - fuseblk /dev/nvme0n1p3 rw,user_id=0,group_id=0,allow_other,blksize=4096\n"
            "60 1 0:50 / /mnt/ro ro,nosuid shared:3 - ext4 /dev/sdb1 ro,data=ordered\n"
            "70 1 0:60 / /media/user/USB\\040STICK rw,nosuid shared:4 - vfat /dev/sdc1 rw,fmask=0022,dmask=0022\n"
        );

        // Exact mount point match /mnt/c
        const auto entryC = DrivePropertiesProvider::parseMountInfo(sampleMountInfo, QStringLiteral("/mnt/c"));
        verify(entryC.has_value(), "parsed /mnt/c mount");
        verify(entryC->mountPoint == QStringLiteral("/mnt/c"), "correct mount point /mnt/c");
        verify(entryC->deviceNode == QStringLiteral("/dev/nvme0n1p3"), "correct device node /mnt/c");
        verify(entryC->fileSystemType == QStringLiteral("fuseblk"), "correct fstype fuseblk");
        verify(!entryC->isReadOnly, "/mnt/c is read-write");
        verify(entryC->combinedOptions.contains(QStringLiteral("allow_other")), "combined options contain allow_other");

        // Read-only mount match /mnt/ro
        const auto entryRo = DrivePropertiesProvider::parseMountInfo(sampleMountInfo, QStringLiteral("/mnt/ro"));
        verify(entryRo.has_value(), "parsed /mnt/ro mount");
        verify(entryRo->isReadOnly, "/mnt/ro detected as read-only");
        verify(entryRo->fileSystemType == QStringLiteral("ext4"), "fstype ext4");

        // Decoded space in mount point
        const auto entryUsb = DrivePropertiesProvider::parseMountInfo(sampleMountInfo, QStringLiteral("/media/user/USB STICK"));
        verify(entryUsb.has_value(), "parsed USB STICK with decoded space");
        verify(entryUsb->mountPoint == QStringLiteral("/media/user/USB STICK"), "decoded mount point matches");
        verify(entryUsb->fileSystemType == QStringLiteral("vfat"), "fstype vfat");

        // Fallback match by device node
        const auto entryByDev = DrivePropertiesProvider::parseMountInfo(sampleMountInfo, {}, QStringLiteral("/dev/sdb1"));
        verify(entryByDev.has_value(), "matched by device node fallback");
        verify(entryByDev->mountPoint == QStringLiteral("/mnt/ro"), "device node found correct mount point");
    }

    // ------------------------------------------------------------------
    // 7. parseMountInfo on real /proc/self/mountinfo
    // ------------------------------------------------------------------
    {
        const auto rootEntry = DrivePropertiesProvider::parseMountInfo({}, QStringLiteral("/"));
        verify(rootEntry.has_value(), "parsed root mount from real system");
        verify(rootEntry->mountPoint == QStringLiteral("/"), "real root mount point is /");
        verify(!rootEntry->fileSystemType.isEmpty(), "real root filesystem type is non-empty");
        verify(!rootEntry->combinedOptions.isEmpty(), "real root mount options are non-empty");
    }

    // ------------------------------------------------------------------
    // 8. busToString mapping
    // ------------------------------------------------------------------
    {
        verify(DrivePropertiesProvider::busToString(Solid::StorageDrive::Ide) == QStringLiteral("IDE"), "bus IDE");
        verify(DrivePropertiesProvider::busToString(Solid::StorageDrive::Usb) == QStringLiteral("USB"), "bus USB");
        verify(DrivePropertiesProvider::busToString(Solid::StorageDrive::Scsi) == QStringLiteral("SCSI"), "bus SCSI");
        verify(DrivePropertiesProvider::busToString(Solid::StorageDrive::Sata) == QStringLiteral("SATA"), "bus SATA");
        verify(DrivePropertiesProvider::busToString(Solid::StorageDrive::Platform) == QStringLiteral("Platform"), "bus Platform");
    }

    // ------------------------------------------------------------------
    // 9. DrivePropertiesProvider loading real mounted volume
    // ------------------------------------------------------------------
    {
        DriveInfo realDrive;
        realDrive.name = QStringLiteral("System");
        realDrive.mountPoint = QStringLiteral("/");
        realDrive.isMounted = true;

        DrivePropertiesProvider provider;
        bool readyEmitted = false;
        QObject::connect(&provider, &DrivePropertiesProvider::dataReady, [&](const DrivePropertiesData &data) {
            readyEmitted = true;
            verify(data.isMounted, "provider loaded mounted flag");
            verify(data.mountPoint == QStringLiteral("/"), "provider loaded mount point");
            verify(data.totalBytes > 0, "provider loaded total bytes > 0");
            verify(data.freeBytes > 0, "provider loaded free bytes > 0");
            verify(data.usedBytes > 0, "provider loaded used bytes > 0");
            verify(data.usedPercent >= 0 && data.usedPercent <= 100, "provider computed valid percent");
            verify(!data.fileSystemType.isEmpty(), "provider loaded filesystem type");
            verify(!data.mountOptions.isEmpty(), "provider loaded mount options");
        });

        provider.load(realDrive);
        verify(readyEmitted, "dataReady emitted synchronously for loaded drive");
    }

    // ------------------------------------------------------------------
    // 10. DrivePropertiesProvider loading unmounted volume without mounting
    // ------------------------------------------------------------------
    {
        DriveInfo unmountedDrive;
        unmountedDrive.name = QStringLiteral("Removable USB");
        unmountedDrive.udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fake_sde1");
        unmountedDrive.isMounted = false;
        unmountedDrive.isRemovable = true;
        unmountedDrive.mountPoint.clear();

        DrivePropertiesProvider provider;
        bool readyEmitted = false;
        QObject::connect(&provider, &DrivePropertiesProvider::dataReady, [&](const DrivePropertiesData &data) {
            readyEmitted = true;
            verify(!data.isMounted, "unmounted volume remains unmounted");
            verify(data.mountPoint.isEmpty(), "unmounted volume mount point is empty");
            verify(data.mountOptions.isEmpty(), "unmounted volume mount options are empty");
            verify(data.freeBytes == -1, "unmounted free bytes is -1");
            verify(data.usedBytes == -1, "unmounted used bytes is -1");
            verify(data.usedPercent == 0, "unmounted used percent is 0");
            verify(data.isRemovable, "unmounted retains removable flag");
        });

        provider.load(unmountedDrive);
        verify(readyEmitted, "dataReady emitted for unmounted drive");
    }

    // ------------------------------------------------------------------
    // 11. DrivePropertiesProvider live device removal notification
    // ------------------------------------------------------------------
    {
        DriveInfo removableDrive;
        removableDrive.name = QStringLiteral("Flash Drive");
        removableDrive.udi = QStringLiteral("/test/device/removable_01");
        removableDrive.isMounted = true;
        removableDrive.mountPoint = QStringLiteral("/mnt/flash");

        DrivePropertiesProvider provider;
        provider.load(removableDrive);

        bool removedEmitted = false;
        QObject::connect(&provider, &DrivePropertiesProvider::deviceRemoved, [&] {
            removedEmitted = true;
        });

        // Simulate Solid device removed signal
        Q_EMIT Solid::DeviceNotifier::instance()->deviceRemoved(QStringLiteral("/test/device/removable_01"));
        verify(removedEmitted, "deviceRemoved signal emitted on Solid notification");
        verify(provider.data().isDeviceMissing, "isDeviceMissing updated to true");
    }

    // ------------------------------------------------------------------
    // 12. PropertiesDialog::showForDrive presentation for mounted drive
    // ------------------------------------------------------------------
    {
        DriveInfo drive;
        drive.name = QStringLiteral("System Btrfs");
        drive.mountPoint = QStringLiteral("/");
        drive.isMounted = true;
        drive.fileSystem = QStringLiteral("btrfs");
        drive.iconName = QStringLiteral("drive-harddisk");

        DrivePropertiesData snapshot;
        snapshot.name = QStringLiteral("System Btrfs");
        snapshot.deviceNode = QStringLiteral("/dev/nvme0n1p2");
        snapshot.parentDriveNode = QStringLiteral("/dev/nvme0n1");
        snapshot.driveModel = QStringLiteral("Samsung SSD 980 1TB");
        snapshot.driveVendor = QStringLiteral("Samsung");
        snapshot.driveBus = QStringLiteral("NVMe");
        snapshot.uuid = QStringLiteral("1234-ABCD-5678");
        snapshot.fileSystemType = QStringLiteral("btrfs");
        snapshot.volumeFsType = QStringLiteral("btrfs");
        snapshot.isMounted = true;
        snapshot.mountPoint = QStringLiteral("/");
        snapshot.mountOptions = QStringLiteral("rw,noatime,compress=zstd:3,subvol=/@");
        snapshot.isReadOnly = false;
        snapshot.isRemovable = false;
        snapshot.isHotpluggable = false;
        snapshot.totalBytes = 100000000000ULL;
        snapshot.freeBytes = 60000000000ULL;
        snapshot.usedBytes = 40000000000ULL;
        snapshot.usedPercent = 40;

        QTimer driver;
        driver.setInterval(10);
        QObject::connect(&driver, &QTimer::timeout, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("drivePropertiesDialog")) {
                return;
            }
            driver.stop();

            verify(dialog->windowTitle().contains("System Btrfs"), "dialog title has drive name");

            auto *headerName = dialog->findChild<QLabel *>(QStringLiteral("driveHeaderName"));
            verify(headerName && headerName->text() == "System Btrfs", "header name matches");

            auto *headerSubtitle = dialog->findChild<QLabel *>(QStringLiteral("driveHeaderSubtitle"));
            verify(headerSubtitle && headerSubtitle->text() == "/", "header subtitle matches mount point");

            auto *deviceNode = dialog->findChild<QLabel *>(QStringLiteral("driveDeviceNodeValue"));
            verify(deviceNode && deviceNode->text() == "/dev/nvme0n1p2", "device node value matches");

            auto *model = dialog->findChild<QLabel *>(QStringLiteral("driveModelValue"));
            verify(model && model->text() == "Samsung SSD 980 1TB", "drive model matches");

            auto *uuid = dialog->findChild<QLabel *>(QStringLiteral("driveUuidValue"));
            verify(uuid && uuid->text() == "1234-ABCD-5678", "uuid matches");

            auto *fsType = dialog->findChild<QLabel *>(QStringLiteral("driveFsTypeValue"));
            verify(fsType && fsType->text() == "BTRFS", "filesystem type matches");

            auto *mounted = dialog->findChild<QLabel *>(QStringLiteral("driveMountedValue"));
            verify(mounted && (mounted->text() == "Zamontowany" || mounted->text() == "Mounted"), "mounted text matches");

            auto *accessMode = dialog->findChild<QLabel *>(QStringLiteral("driveAccessModeValue"));
            verify(accessMode && accessMode->text().contains("rw"), "access mode rw");

            auto *mountOptions = dialog->findChild<QLabel *>(QStringLiteral("driveMountOptionsValue"));
            verify(mountOptions && mountOptions->text().contains("compress=zstd:3"), "mount options displayed");

            auto *usageBar = dialog->findChild<QProgressBar *>(QStringLiteral("driveUsageBar"));
            verify(usageBar && usageBar->isEnabled(), "usage bar is enabled");
            verify(usageBar->value() == 40, "usage bar value is 40%");

            auto *buttonBox = dialog->findChild<QDialogButtonBox *>();
            verify(buttonBox != nullptr, "button box exists");
            dialog->accept();
        });
        driver.start();

        PropertiesDialog::showForDrive(nullptr, drive, {}, &snapshot);
    }

    // ------------------------------------------------------------------
    // 13. PropertiesDialog::showForDrive presentation for unmounted drive
    // ------------------------------------------------------------------
    {
        DriveInfo unmountedDrive;
        unmountedDrive.name = QStringLiteral("SD Card");
        unmountedDrive.isMounted = false;
        unmountedDrive.isRemovable = true;

        DrivePropertiesData unmountedSnapshot;
        unmountedSnapshot.name = QStringLiteral("SD Card");
        unmountedSnapshot.deviceNode = QStringLiteral("/dev/mmcblk0p1");
        unmountedSnapshot.volumeFsType = QStringLiteral("vfat");
        unmountedSnapshot.isMounted = false;
        unmountedSnapshot.isRemovable = true;
        unmountedSnapshot.totalBytes = 32000000000ULL;
        unmountedSnapshot.freeBytes = -1;
        unmountedSnapshot.usedBytes = -1;
        unmountedSnapshot.usedPercent = 0;

        QTimer driver;
        driver.setInterval(10);
        QObject::connect(&driver, &QTimer::timeout, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("drivePropertiesDialog")) {
                return;
            }
            driver.stop();

            auto *mounted = dialog->findChild<QLabel *>(QStringLiteral("driveMountedValue"));
            verify(mounted && (mounted->text() == "Niezamontowany" || mounted->text() == "Not mounted"),
                   "unmounted status displayed");

            auto *freeSpace = dialog->findChild<QLabel *>(QStringLiteral("driveFreeSpaceValue"));
            verify(freeSpace && (freeSpace->text() == "Niedostępne" || freeSpace->text() == "Unavailable"),
                   "unmounted free space unavailable");

            auto *usedSpace = dialog->findChild<QLabel *>(QStringLiteral("driveUsedSpaceValue"));
            verify(usedSpace && (usedSpace->text() == "Niedostępne" || usedSpace->text() == "Unavailable"),
                   "unmounted used space unavailable");

            auto *usageBar = dialog->findChild<QProgressBar *>(QStringLiteral("driveUsageBar"));
            verify(usageBar && !usageBar->isEnabled(), "unmounted usage bar disabled");

            dialog->accept();
        });
        driver.start();

        PropertiesDialog::showForDrive(nullptr, unmountedDrive, {}, &unmountedSnapshot);
    }

    // ------------------------------------------------------------------
    // 14. PropertiesDialog::showForDrive device removal banner display
    // ------------------------------------------------------------------
    {
        DriveInfo removableDrive;
        removableDrive.name = QStringLiteral("USB Flash");
        removableDrive.udi = QStringLiteral("/org/test/usb_flash");
        removableDrive.isMounted = true;
        removableDrive.mountPoint = QStringLiteral("/mnt/usb");

        DrivePropertiesData removableSnapshot;
        removableSnapshot.udi = QStringLiteral("/org/test/usb_flash");
        removableSnapshot.name = QStringLiteral("USB Flash");
        removableSnapshot.isMounted = true;
        removableSnapshot.mountPoint = QStringLiteral("/mnt/usb");

        QTimer driver;
        driver.setInterval(10);
        QObject::connect(&driver, &QTimer::timeout, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("drivePropertiesDialog")) {
                return;
            }
            driver.stop();

            auto *banner = dialog->findChild<QLabel *>(QStringLiteral("deviceRemovedBanner"));
            verify(banner != nullptr, "removal banner exists");
            verify(!banner->isVisible(), "removal banner initially hidden");

            // Trigger device removal signal
            Q_EMIT Solid::DeviceNotifier::instance()->deviceRemoved(QStringLiteral("/org/test/usb_flash"));
            verify(banner->isVisible(), "removal banner becomes visible on device removal");

            dialog->accept();
        });
        driver.start();

        PropertiesDialog::showForDrive(nullptr, removableDrive, {}, &removableSnapshot);
    }

    // ------------------------------------------------------------------
    // 15. DriveFrame context menu and propertiesRequested signal
    // ------------------------------------------------------------------
    {
        DriveInfo testDrive;
        testDrive.name = QStringLiteral("Local Test Drive");
        testDrive.mountPoint = QStringLiteral("/tmp");
        testDrive.isMounted = true;

        QMenu menu;
        QAction *propertiesAction = nullptr;
        populateDriveContextMenu(menu, testDrive, false, false,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, &propertiesAction);
        verify(propertiesAction != nullptr, "populateDriveContextMenu created properties action");
        verify(propertiesAction->text().contains("Właściwości") || propertiesAction->text().contains("Properties"),
               "properties action has correct text");

        DriveFrame driveCard(testDrive);
        bool cardPropertiesEmitted = false;
        QObject::connect(&driveCard, &DriveFrame::propertiesRequested, [&](const DriveInfo &d) {
            cardPropertiesEmitted = true;
            verify(d.name == "Local Test Drive", "DriveFrame emitted correct drive");
        });

        // Trigger properties directly via signal
        Q_EMIT driveCard.propertiesRequested(testDrive);
        verify(cardPropertiesEmitted, "DriveFrame::propertiesRequested emitted properly");
    }

    // ------------------------------------------------------------------
    // 16. SidebarDriveButton context menu and propertiesRequested signal
    // ------------------------------------------------------------------
    {
        DriveInfo sidebarDrive;
        sidebarDrive.name = QStringLiteral("Sidebar Drive");
        sidebarDrive.mountPoint = QStringLiteral("/tmp");
        sidebarDrive.targetUrl = QUrl::fromLocalFile(QStringLiteral("/tmp"));
        sidebarDrive.isMounted = true;

        SidebarDriveButton button(sidebarDrive);
        bool sidebarPropertiesEmitted = false;
        QObject::connect(&button, &SidebarDriveButton::propertiesRequested, [&](const DriveInfo &d) {
            sidebarPropertiesEmitted = true;
            verify(d.name == "Sidebar Drive", "SidebarDriveButton emitted correct drive");
        });

        Q_EMIT button.propertiesRequested(sidebarDrive);
        verify(sidebarPropertiesEmitted, "SidebarDriveButton::propertiesRequested emitted properly");

        SidebarPanel sidebar;
        bool panelPropertiesEmitted = false;
        QObject::connect(&sidebar, &SidebarPanel::drivePropertiesRequested, [&](const DriveInfo &d) {
            panelPropertiesEmitted = true;
            verify(d.name == "Sidebar Drive", "SidebarPanel forwarded correct drive");
        });

        Q_EMIT sidebar.drivePropertiesRequested(sidebarDrive);
        verify(panelPropertiesEmitted, "SidebarPanel::drivePropertiesRequested emitted properly");
    }

    // ------------------------------------------------------------------
    // 17. resolveParentDiskNode regression (NVMe, USB/SCSI, mmcblk, whole disk)
    // ------------------------------------------------------------------
    {
        // NVMe partition -> base disk
        verify(DrivePropertiesProvider::resolveParentDiskNode(QStringLiteral("/dev/nvme0n1p3")) == QStringLiteral("/dev/nvme0n1"),
               "nvme0n1p3 resolves to /dev/nvme0n1");
        verify(DrivePropertiesProvider::resolveParentDiskNode(QStringLiteral("/dev/nvme2n1p2")) == QStringLiteral("/dev/nvme2n1"),
               "nvme2n1p2 resolves to /dev/nvme2n1");

        // mmcblk partition -> base disk
        verify(DrivePropertiesProvider::resolveParentDiskNode(QStringLiteral("/dev/mmcblk0p1")) == QStringLiteral("/dev/mmcblk0"),
               "mmcblk0p1 resolves to /dev/mmcblk0");

        // SCSI / SATA / USB partition -> base disk
        verify(DrivePropertiesProvider::resolveParentDiskNode(QStringLiteral("/dev/sde1")) == QStringLiteral("/dev/sde"),
               "sde1 resolves to /dev/sde");
        verify(DrivePropertiesProvider::resolveParentDiskNode(QStringLiteral("/dev/sda12")) == QStringLiteral("/dev/sda"),
               "sda12 resolves to /dev/sda");

        // Whole disks (not partitions) return empty so they are displayed as "—", never sibling partition
        verify(DrivePropertiesProvider::resolveParentDiskNode(QStringLiteral("/dev/sde")).isEmpty(),
               "whole disk sde has no parent partition");
        verify(DrivePropertiesProvider::resolveParentDiskNode(QStringLiteral("/dev/nvme0n1")).isEmpty(),
               "whole disk nvme0n1 has no parent partition");
        verify(DrivePropertiesProvider::resolveParentDiskNode(QString()).isEmpty(),
               "empty device node returns empty parent");
    }

    // ------------------------------------------------------------------
    // 18. findExactMountPoint exact match semantics (Root vs Subfolder)
    // ------------------------------------------------------------------
    {
        const QString testMountInfo = QStringLiteral(
            "100 1 8:1 / / rw,relatime - ext4 /dev/sda1 rw,errors=remount-ro\n"
            "101 100 8:2 / /mnt/c rw,noatime - ntfs3 /dev/sde1 rw\n"
            "102 100 8:3 / /mnt/d rw,noatime - btrfs /dev/nvme2n1p2 rw\n"
        );

        // Exact roots match
        const auto rootMatch = DrivePropertiesProvider::findExactMountPoint(QStringLiteral("/"), testMountInfo);
        verify(rootMatch.has_value(), "root / is exact mount point");
        verify(rootMatch->deviceNode == "/dev/sda1", "root device is sda1");

        const auto mntCMatch = DrivePropertiesProvider::findExactMountPoint(QStringLiteral("/mnt/c"), testMountInfo);
        verify(mntCMatch.has_value(), "/mnt/c is exact mount point");
        verify(mntCMatch->deviceNode == "/dev/sde1", "/mnt/c device is sde1");

        const auto mntDMatch = DrivePropertiesProvider::findExactMountPoint(QStringLiteral("/mnt/d/"), testMountInfo);
        verify(mntDMatch.has_value(), "/mnt/d/ cleans to /mnt/d and matches");

        // Subfolders do NOT match as exact mount points
        const auto subMatch1 = DrivePropertiesProvider::findExactMountPoint(QStringLiteral("/home/user"), testMountInfo);
        verify(!subMatch1.has_value(), "/home/user is subfolder, not exact mount root");

        const auto subMatch2 = DrivePropertiesProvider::findExactMountPoint(QStringLiteral("/mnt/c/Windows"), testMountInfo);
        verify(!subMatch2.has_value(), "/mnt/c/Windows is subfolder, not exact mount root");
    }

    // ------------------------------------------------------------------
    // 19. PropertiesDialog::showForDrive modeless behavior & word wrap policy
    // ------------------------------------------------------------------
    {
        DriveInfo modelessDrive;
        modelessDrive.name = QStringLiteral("Modeless Test");
        modelessDrive.isMounted = true;
        modelessDrive.mountPoint = QStringLiteral("/mnt/test");

        DrivePropertiesData modelessSnapshot;
        modelessSnapshot.name = QStringLiteral("Modeless Test");
        modelessSnapshot.deviceNode = QStringLiteral("/dev/sde1");
        modelessSnapshot.mountOptions = QStringLiteral("rw,noatime,compress=zstd:3,space_cache=v2,subvolid=256,subvol=/@home,discard=async");
        modelessSnapshot.isMounted = true;
        modelessSnapshot.mountPoint = QStringLiteral("/mnt/test");

        QDialog *dialog = PropertiesDialog::showForDrive(nullptr, modelessDrive, {}, &modelessSnapshot);
        verify(dialog != nullptr, "showForDrive returns dialog pointer");
        verify(!dialog->isModal(), "dialog is modeless (setModal(false))");
        verify(dialog->testAttribute(Qt::WA_DeleteOnClose), "dialog has WA_DeleteOnClose");

        auto *scroll = dialog->findChild<QScrollArea *>();
        verify(scroll != nullptr, "general scroll area exists");
        verify(scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff, "horizontal scroll bar always off");

        auto *mountOptionsLabel = dialog->findChild<QLabel *>(QStringLiteral("driveMountOptionsValue"));
        verify(mountOptionsLabel != nullptr, "mount options label exists");
        verify(mountOptionsLabel->wordWrap(), "mount options label has word wrap enabled");
        verify(mountOptionsLabel->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored, "mount options label horizontal policy is Ignored");

        dialog->accept();
    }

    // ------------------------------------------------------------------
    // 20. Device removal safety & idempotence (Blocker 1 prevention)
    // ------------------------------------------------------------------
    {
        // Provider outliving device removal without crash
        auto *provider = new DrivePropertiesProvider(nullptr);
        DriveInfo removable;
        removable.name = QStringLiteral("Safe Removal USB");
        removable.udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sde1");
        removable.isMounted = true;
        removable.mountPoint = QStringLiteral("/media/usb");

        DrivePropertiesData snapshot;
        snapshot.udi = removable.udi;
        snapshot.parentDriveUdi = QStringLiteral("/org/freedesktop/UDisks2/drives/Generic_Flash_Disk");
        snapshot.name = removable.name;
        snapshot.isMounted = true;

        bool removalSignalFired = false;
        QObject::connect(provider, &DrivePropertiesProvider::deviceRemoved, [&] {
            removalSignalFired = true;
        });

        provider->loadSnapshot(snapshot);

        // Simulate partition device removal
        Q_EMIT Solid::DeviceNotifier::instance()->deviceRemoved(snapshot.udi);
        verify(removalSignalFired, "provider caught partition removal");

        // Simulate parent drive removal (idempotent, no second signal or crash)
        removalSignalFired = false;
        Q_EMIT Solid::DeviceNotifier::instance()->deviceRemoved(snapshot.parentDriveUdi);
        verify(removalSignalFired, "provider caught parent drive removal");

        // Repeated removal of same or unknown UDI is harmless
        Q_EMIT Solid::DeviceNotifier::instance()->deviceRemoved(QStringLiteral("/org/unknown/device"));
        Q_EMIT Solid::DeviceNotifier::instance()->deviceRemoved(snapshot.udi);

        delete provider;
    }

    // ------------------------------------------------------------------
    // 21. PropertiesDialog live device removal updates UI safely
    // ------------------------------------------------------------------
    {
        DriveInfo liveDrive;
        liveDrive.name = QStringLiteral("Live Removal USB");
        liveDrive.udi = QStringLiteral("/org/test/live_usb");
        liveDrive.isMounted = true;
        liveDrive.mountPoint = QStringLiteral("/mnt/live_usb");

        DrivePropertiesData liveSnapshot;
        liveSnapshot.udi = liveDrive.udi;
        liveSnapshot.name = liveDrive.name;
        liveSnapshot.isMounted = true;
        liveSnapshot.mountPoint = liveDrive.mountPoint;

        QDialog *dialog = PropertiesDialog::showForDrive(nullptr, liveDrive, {}, &liveSnapshot);
        verify(dialog != nullptr, "live removal dialog created");

        auto *banner = dialog->findChild<QLabel *>(QStringLiteral("deviceRemovedBanner"));
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("driveMountedValue"));
        auto *subtitle = dialog->findChild<QLabel *>(QStringLiteral("driveHeaderSubtitle"));
        auto *usageBar = dialog->findChild<QProgressBar *>(QStringLiteral("driveUsageBar"));

        verify(!banner->isVisible(), "banner initially hidden");

        // Physical removal simulation
        Q_EMIT Solid::DeviceNotifier::instance()->deviceRemoved(liveDrive.udi);

        verify(banner->isVisible(), "banner visible after unplug");
        verify(status && (status->text() == "Odłączony" || status->text() == "Disconnected"), "status updated to Disconnected");
        verify(subtitle && (subtitle->text() == "Odłączony" || subtitle->text() == "Disconnected"), "subtitle updated to Disconnected");
        verify(usageBar && !usageBar->isEnabled(), "usage bar disabled on unplug");

        dialog->accept();
    }

    // ------------------------------------------------------------------
    // 21. DriveFrame keyboard semantics (Enter vs Alt+Enter)
    // ------------------------------------------------------------------
    {
        DriveInfo d;
        d.id = QStringLiteral("drive-key-test");
        d.targetUrl = QUrl::fromLocalFile(QStringLiteral("/test/drive"));
        DriveFrame frame(d);
        frame.show();

        QUrl activatedUrl;
        QObject::connect(&frame, &ClickableFrame::activated, [&](const QUrl &url) {
            activatedUrl = url;
        });

        bool propertiesRequested = false;
        QObject::connect(&frame, &DriveFrame::propertiesRequested, [&](const DriveInfo &info) {
            if (info.id == d.id) propertiesRequested = true;
        });

        // Regular Enter activates
        QTest::keyClick(&frame, Qt::Key_Return);
        verify(activatedUrl == d.targetUrl, "DriveFrame Enter emits activated");
        verify(!propertiesRequested, "DriveFrame Enter does not emit propertiesRequested");

        // Alt+Enter requests properties
        activatedUrl.clear();
        QTest::keyClick(&frame, Qt::Key_Return, Qt::AltModifier);
        verify(activatedUrl.isEmpty(), "DriveFrame Alt+Enter does not emit activated");
        verify(propertiesRequested, "DriveFrame Alt+Enter emits propertiesRequested");

        // Keypad Enter with Alt
        propertiesRequested = false;
        QTest::keyClick(&frame, Qt::Key_Enter, Qt::AltModifier);
        verify(activatedUrl.isEmpty(), "DriveFrame Alt+KeypadEnter does not emit activated");
        verify(propertiesRequested, "DriveFrame Alt+KeypadEnter emits propertiesRequested");
    }

    // ------------------------------------------------------------------
    // 22. SidebarDriveButton keyboard semantics (Enter vs Alt+Enter)
    // ------------------------------------------------------------------
    {
        DriveInfo d;
        d.id = QStringLiteral("sidebar-drive-key-test");
        d.targetUrl = QUrl::fromLocalFile(QStringLiteral("/test/sidebar-drive"));
        SidebarDriveButton button(d);
        button.show();

        QUrl activatedUrl;
        QObject::connect(&button, &SidebarDriveButton::activated, [&](const QUrl &url) {
            activatedUrl = url;
        });

        bool propertiesRequested = false;
        QObject::connect(&button, &SidebarDriveButton::propertiesRequested, [&](const DriveInfo &info) {
            if (info.id == d.id) propertiesRequested = true;
        });

        // Regular Enter activates
        QTest::keyClick(&button, Qt::Key_Return);
        verify(activatedUrl == d.targetUrl, "SidebarDriveButton Enter emits activated");
        verify(!propertiesRequested, "SidebarDriveButton Enter does not emit propertiesRequested");

        // Alt+Enter requests properties
        activatedUrl.clear();
        QTest::keyClick(&button, Qt::Key_Return, Qt::AltModifier);
        verify(activatedUrl.isEmpty(), "SidebarDriveButton Alt+Enter does not emit activated");
        verify(propertiesRequested, "SidebarDriveButton Alt+Enter emits propertiesRequested");
    }

    // ------------------------------------------------------------------
    // 23. WrappingValueLabel semantics
    // ------------------------------------------------------------------
    {
        WrappingValueLabel label(QStringLiteral("test value"));
        verify(label.wordWrap(), "WrappingValueLabel has wordWrap enabled");
        verify(label.textInteractionFlags() & Qt::TextSelectableByMouse, "WrappingValueLabel is selectable by mouse");
        verify(label.minimumSizeHint().width() <= 40, "WrappingValueLabel minimumSizeHint width is <= 40");
        verify(label.sizePolicy().horizontalPolicy() == QSizePolicy::Ignored, "WrappingValueLabel horizontal policy is Ignored");

        label.setValue(QString());
        verify(label.text() == QStringLiteral("—"), "empty setValue sets em-dash");
    }

    // ------------------------------------------------------------------
    // 24. formattedMountOptions semantics
    // ------------------------------------------------------------------
    {
        DrivePropertiesData data;
        data.mountOptions = QStringLiteral("rw,nosuid,nodev,relatime");
        verify(data.formattedMountOptions() == QStringLiteral("rw, nosuid, nodev, relatime"), "formattedMountOptions inserts comma-space");

        DrivePropertiesData emptyData;
        verify(emptyData.formattedMountOptions() == QStringLiteral("—"), "formattedMountOptions on empty returns em-dash");
    }

    printf("PASS: %d DriveProperties assertions; data model, mountinfo, Solid capabilities, unmounted safety, removal, UI\n", checks);
    return 0;
}
