/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Appended to the temporary regression binary by run-pane-actions.py.
#include "mtp-contract-spec.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int checks = 0;
    auto verify = [&](bool condition, const char *message) {
        ++checks;
        if (!condition) qFatal("FAIL: %s", message);
    };

    const QUrl thisPc(QStringLiteral("thispc:/"));
    const QUrl root = QUrl::fromLocalFile(QStringLiteral("/"));
    const QUrl home = QUrl::fromLocalFile(QDir::homePath());
    const QUrl local = QUrl::fromLocalFile(QStringLiteral("/etc/hosts"));
    const QUrl driveRoot = QUrl::fromLocalFile(QStringLiteral("/mnt/stage2-drive"));
    const QUrl driveChild = QUrl::fromLocalFile(QStringLiteral("/mnt/stage2-drive/folder/file.txt"));
    const QUrl admin(QStringLiteral("admin:/etc/hosts"));
    const QUrl search = makeSearchLocation(
        QStringLiteral("needle"), 0, QUrl::fromLocalFile(QStringLiteral("/tmp/base")), 0, 0, 0);
    const QUrl searchWithoutBase = makeSearchLocation(
        QStringLiteral("needle"), 2, {}, 0, 0, 0);
    const QUrl remote(QStringLiteral("sftp://example.test/share/folder"));
    const QVector<DriveInfo> drives{{QStringLiteral("Stage 2 Drive"), {}, {}, {}, {}, {},
                                     driveRoot, QStringLiteral("drive-harddisk"), 0}};

    verify(LocationPresentation::primaryTitle(thisPc, drives) == trLocal("Ten komputer", "This PC"),
           "This PC primary title");
    verify(LocationPresentation::splitTitle(thisPc) == LocationPresentation::primaryTitle(thisPc, drives),
           "This PC title consistency");
    verify(LocationPresentation::iconName(thisPc) == QStringLiteral("computer"),
           "This PC icon");
    verify(!LocationPresentation::parentUrl(thisPc, drives, LocationPresentation::ParentProfile::Primary).isValid(),
           "This PC has no primary parent");
    verify(!LocationPresentation::parentUrl(thisPc, drives, LocationPresentation::ParentProfile::Split).isValid(),
           "This PC has no split parent");

    verify(LocationPresentation::primaryTitle(root, drives) == QStringLiteral("/"), "local root title");
    verify(LocationPresentation::parentUrl(root, drives, LocationPresentation::ParentProfile::Primary) == thisPc,
           "local root parent");
    verify(LocationPresentation::iconName(root) == QStringLiteral("drive-harddisk"), "local root icon");

    verify(LocationPresentation::primaryTitle(home, drives) == trLocal("Katalog domowy", "Home"),
           "home primary title");
    const auto homeSegments = LocationPresentation::localPathSegments(home, drives);
    verify(homeSegments.size() == 1 && homeSegments.first().url == home,
           "home is one breadcrumb segment");

    verify(LocationPresentation::primaryTitle(local, drives) == QStringLiteral("hosts"),
           "local leaf primary title");
    verify(LocationPresentation::splitTitle(local) == QStringLiteral("hosts"),
           "local leaf split title");
    verify(LocationPresentation::primaryTitle(local, drives) == LocationPresentation::splitTitle(local),
           "local leaf title consistency");
    verify(LocationPresentation::parentUrl(local, drives, LocationPresentation::ParentProfile::Primary)
               == QUrl::fromLocalFile(QStringLiteral("/etc")),
           "local file parent");
    const auto localSegments = LocationPresentation::localPathSegments(local, drives);
    verify(localSegments.size() == 3, "local breadcrumb segment count");
    verify(localSegments.at(0).text == QStringLiteral("/")
               && localSegments.at(1).text == QStringLiteral("etc")
               && localSegments.at(2).text == QStringLiteral("hosts"),
           "local breadcrumb labels");
    verify(localSegments.at(2).url == local, "local breadcrumb cumulative URL");
    verify(LocationPresentation::contentHeaderText(local) == urlForDisplay(local),
           "local content header uses the full canonical path");

    verify(LocationPresentation::primaryTitle(driveRoot, drives) == QStringLiteral("Stage 2 Drive"),
           "simulated drive title");
    verify(LocationPresentation::parentUrl(driveRoot, drives, LocationPresentation::ParentProfile::Primary) == thisPc,
           "simulated drive parent");
    const auto driveSegments = LocationPresentation::localPathSegments(driveChild, drives);
    verify(driveSegments.size() == 4, "drive breadcrumb segment count");
    verify(driveSegments.at(0).url == thisPc && driveSegments.at(1).url == driveRoot,
           "drive breadcrumbs start at This PC and drive root");
    verify(driveSegments.last().url == driveChild, "drive breadcrumb cumulative URL");

    verify(LocationPresentation::splitLocationText(admin).contains(QStringLiteral("etc")),
           "admin friendly location text");
    verify(LocationPresentation::iconName(admin) == QStringLiteral("security-high"), "admin icon");
    const auto adminSegments = LocationPresentation::adminPathSegments(admin);
    verify(adminSegments.size() == 3 && adminSegments.first().url == QUrl(QStringLiteral("admin:/"))
               && adminSegments.last().url == admin,
           "admin breadcrumb data");
    verify(LocationPresentation::contentHeaderText(admin) == urlForDisplay(admin),
           "admin content header preserves remote URL semantics");

    verify(LocationPresentation::primaryTitle(search, drives) == LocationPresentation::splitTitle(search),
           "search title consistency");
    verify(LocationPresentation::iconName(search) == QStringLiteral("system-search"), "search icon");
    verify(LocationPresentation::parentUrl(search, drives, LocationPresentation::ParentProfile::Primary)
               == QUrl::fromLocalFile(QStringLiteral("/tmp/base")),
           "search parent is encoded base");
    verify(LocationPresentation::parentUrl(searchWithoutBase, drives, LocationPresentation::ParentProfile::Split)
               == thisPc,
           "search without base parent is This PC");
    verify(LocationPresentation::contentHeaderText(search)
               == LocationPresentation::primaryTitle(search, drives),
           "search content header keeps its dedicated presentation");

    verify(LocationPresentation::primaryTitle(remote, drives) == QStringLiteral("folder"),
           "remote primary title");
    verify(LocationPresentation::splitTitle(remote) == LocationPresentation::primaryTitle(remote, drives),
           "remote title consistency");
    verify(LocationPresentation::splitLocationText(remote) == urlForDisplay(remote),
           "remote friendly text");
    verify(LocationPresentation::contentHeaderText(remote) == urlForDisplay(remote),
           "remote content header never assumes a local filesystem path");
    verify(LocationPresentation::parentUrl(remote, drives, LocationPresentation::ParentProfile::Primary)
               == QUrl(QStringLiteral("sftp://example.test/share")),
           "remote parent");
    verify(LocationPresentation::parentUrl(QUrl(QStringLiteral("sftp://example.test/")), drives,
                                           LocationPresentation::ParentProfile::Split) == thisPc,
           "remote root parent edge case");

    const QUrl queried(QStringLiteral("sftp://example.test/share/folder?keep=1"));
    verify(LocationPresentation::parentUrl(queried, drives, LocationPresentation::ParentProfile::Primary).hasQuery(),
           "primary parent preserves query");
    verify(!LocationPresentation::parentUrl(queried, drives, LocationPresentation::ParentProfile::Split).hasQuery(),
           "split parent clears query");
    verify(!LocationPresentation::parentUrl(QUrl(), drives, LocationPresentation::ParentProfile::Split).isValid(),
           "invalid split location has no parent");

    // Network discovery locations (remote:/ and smb://)
    const QUrl remoteDiscoveryRoot(QStringLiteral("remote:/"));
    const QUrl smbDiscoveryRoot(QStringLiteral("smb://"));
    verify(LocationPresentation::primaryTitle(remoteDiscoveryRoot, drives) == trLocal("Sieć", "Network"),
           "remote discovery root primary title");
    verify(LocationPresentation::splitTitle(remoteDiscoveryRoot) == trLocal("Sieć", "Network"),
           "remote discovery root split title");
    verify(LocationPresentation::iconName(remoteDiscoveryRoot) == QStringLiteral("network-workgroup"),
           "remote discovery root icon");
    verify(LocationPresentation::parentUrl(remoteDiscoveryRoot, drives, LocationPresentation::ParentProfile::Primary) == thisPc,
           "remote discovery root primary parent is This PC");
    verify(LocationPresentation::parentUrl(remoteDiscoveryRoot, drives, LocationPresentation::ParentProfile::Split) == thisPc,
           "remote discovery root split parent is This PC");

    verify(LocationPresentation::primaryTitle(smbDiscoveryRoot, drives) == trLocal("Katalogi współdzielone (SMB)", "Shared Folders (SMB)"),
           "smb discovery root primary title");
    verify(LocationPresentation::splitTitle(smbDiscoveryRoot) == trLocal("Katalogi współdzielone (SMB)", "Shared Folders (SMB)"),
           "smb discovery root split title");
    verify(LocationPresentation::iconName(smbDiscoveryRoot) == QStringLiteral("network-workgroup"),
           "smb discovery root icon");
    verify(LocationPresentation::parentUrl(smbDiscoveryRoot, drives, LocationPresentation::ParentProfile::Primary) == remoteDiscoveryRoot,
           "smb discovery root primary parent is remote:/");
    verify(LocationPresentation::parentUrl(smbDiscoveryRoot, drives, LocationPresentation::ParentProfile::Split) == remoteDiscoveryRoot,
           "smb discovery root split parent is remote:/");

    const auto smbSegments = LocationPresentation::remotePathSegments(smbDiscoveryRoot);
    verify(smbSegments.size() == 2, "smb discovery breadcrumb segments count");
    verify(smbSegments.at(0).url == remoteDiscoveryRoot && smbSegments.at(0).text == trLocal("Sieć", "Network"),
           "smb first crumb is remote:/");
    verify(smbSegments.at(1).url == smbDiscoveryRoot && smbSegments.at(1).text == trLocal("Katalogi współdzielone (SMB)", "Shared Folders (SMB)"),
           "smb second crumb is smb://");

    // =======================================================================
    // Stage 1.2: MTP Location Presentation, Parent URLs & Breadcrumbs (Section 4 & 5)
    // =======================================================================
    {
        const QString mtpUdi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2");
        const DriveInfo mtpDrive = MtpUdsContract::makeMtpDriveInfo(
            QStringLiteral("mtp-pixel8"),
            QStringLiteral("Google Pixel 8 Pro"),
            mtpUdi);
        QVector<DriveInfo> drivesWithMtp = drives;
        drivesWithMtp.append(mtpDrive);

        const QUrl mtpUdiUrl = mtpDrive.targetUrl;
        const QUrl mtpDeviceRoot(QStringLiteral("mtp:/Google Pixel 8 Pro"));
        const QUrl mtpInternalStorage(QStringLiteral("mtp:/Google Pixel 8 Pro/Internal shared storage"));
        const QUrl mtpSdCardDcim(QStringLiteral("mtp:/Google Pixel 8 Pro/SD card/DCIM/Camera"));
        const QUrl mtpUnicode(QStringLiteral("mtp:/Telefon Żółć/Pamięć wewnętrzna/Zdjęcia z wakacji"));
        const QUrl mtpEncoded(QStringLiteral("mtp:/Phone%20%231%20%26%20Test/Internal%20storage/Folder%20%5BA%5D"));

        // 1. Titles for mtp:udi= and redirected mtp:/<Device>/...
        verify(LocationPresentation::primaryTitle(mtpUdiUrl, drivesWithMtp) == QStringLiteral("Google Pixel 8 Pro"),
               "MTP UDI URL primary title resolves to DriveInfo display name");
        verify(LocationPresentation::primaryTitle(mtpDeviceRoot, drivesWithMtp) == QStringLiteral("Google Pixel 8 Pro"),
               "MTP redirected device root primary title is device name");
        verify(LocationPresentation::splitTitle(mtpDeviceRoot) == QStringLiteral("Google Pixel 8 Pro"),
               "MTP redirected device root split title is device name");
        verify(LocationPresentation::primaryTitle(mtpInternalStorage, drivesWithMtp) == QStringLiteral("Internal shared storage"),
               "MTP internal storage primary title");
        verify(LocationPresentation::splitTitle(mtpInternalStorage) == QStringLiteral("Internal shared storage"),
               "MTP internal storage split title");
        verify(LocationPresentation::primaryTitle(mtpSdCardDcim, drivesWithMtp) == QStringLiteral("Camera"),
               "MTP SD card leaf folder primary title");
        verify(LocationPresentation::splitTitle(mtpSdCardDcim) == QStringLiteral("Camera"),
               "MTP SD card leaf folder split title");

        // 2. Unicode, spaces, and URL-encoded characters in MTP URLs
        verify(LocationPresentation::primaryTitle(mtpUnicode, drivesWithMtp) == QStringLiteral("Zdjęcia z wakacji"),
               "MTP Unicode & spaces folder primary title");
        verify(LocationPresentation::splitTitle(mtpUnicode) == QStringLiteral("Zdjęcia z wakacji"),
               "MTP Unicode & spaces folder split title");
        verify(LocationPresentation::primaryTitle(mtpEncoded, drivesWithMtp) == QStringLiteral("Folder [A]"),
               "MTP percent-encoded folder primary title decodes cleanly");
        verify(LocationPresentation::splitTitle(mtpEncoded) == QStringLiteral("Folder [A]"),
               "MTP percent-encoded folder split title decodes cleanly");

        // 3. Content header preserves mtp: scheme (never assumes local path)
        verify(LocationPresentation::contentHeaderText(mtpInternalStorage) == urlForDisplay(mtpInternalStorage),
               "MTP content header preserves mtp: URL display string");
        verify(!LocationPresentation::contentHeaderText(mtpInternalStorage).startsWith(QLatin1Char('/')),
               "MTP content header never strips scheme into fake local path");

        // 4. Parent URL navigation for nested MTP folders
        verify(LocationPresentation::parentUrl(mtpSdCardDcim, drivesWithMtp, LocationPresentation::ParentProfile::Primary)
                   == QUrl(QStringLiteral("mtp:/Google Pixel 8 Pro/SD card/DCIM")),
               "MTP nested folder Primary parent is parent folder");
        verify(LocationPresentation::parentUrl(mtpSdCardDcim, drivesWithMtp, LocationPresentation::ParentProfile::Split)
                   == QUrl(QStringLiteral("mtp:/Google Pixel 8 Pro/SD card/DCIM")),
               "MTP nested folder Split parent is parent folder");
        verify(LocationPresentation::parentUrl(mtpInternalStorage, drivesWithMtp, LocationPresentation::ParentProfile::Primary)
                   == mtpDeviceRoot,
               "MTP storage root Primary parent is mtp:/<Device>");
        verify(LocationPresentation::parentUrl(mtpInternalStorage, drivesWithMtp, LocationPresentation::ParentProfile::Split)
                   == mtpDeviceRoot,
               "MTP storage root Split parent is mtp:/<Device>");
        verify(LocationPresentation::parentUrl(mtpUdiUrl, drivesWithMtp, LocationPresentation::ParentProfile::Primary)
                   == thisPc,
               "MTP UDI URL Primary parent is This PC when drive is in snapshot");
        verify(LocationPresentation::parentUrl(mtpUdiUrl, drivesWithMtp, LocationPresentation::ParentProfile::Split)
                   == thisPc,
               "MTP UDI URL Split parent is This PC when drive is in snapshot");

        // 5. Safe segment-based URL hierarchy matching (preventing naive prefix match bugs)
        const DriveInfo pixel8 = MtpUdsContract::makeMtpDriveInfo(
            QStringLiteral("mtp-p8"), QStringLiteral("Pixel 8"),
            QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/usb1/1-1"));
        const DriveInfo pixel8Pro = MtpUdsContract::makeMtpDriveInfo(
            QStringLiteral("mtp-p8p"), QStringLiteral("Pixel 8 Pro"),
            QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/usb1/1-2"));

        verify(MtpUrlContract::urlBelongsToMtpDevice(
                   QUrl(QStringLiteral("mtp:/Pixel 8/Internal shared storage/DCIM")),
                   pixel8, QUrl(QStringLiteral("mtp:/Pixel 8"))),
               "urlBelongsToMtpDevice matches exact device root segment 'Pixel 8'");
        verify(!MtpUrlContract::urlBelongsToMtpDevice(
                   QUrl(QStringLiteral("mtp:/Pixel 8 Pro/Internal shared storage/DCIM")),
                   pixel8, QUrl(QStringLiteral("mtp:/Pixel 8"))),
               "urlBelongsToMtpDevice does NOT falsely match 'Pixel 8 Pro' as prefix of 'Pixel 8'");
        verify(MtpUrlContract::urlBelongsToMtpDevice(
                   QUrl(QStringLiteral("mtp:/Pixel 8 Pro/Internal shared storage/DCIM")),
                   pixel8Pro, QUrl(QStringLiteral("mtp:/Pixel 8 Pro"))),
               "urlBelongsToMtpDevice matches 'Pixel 8 Pro' accurately");

        // 6. Expected breadcrumb contract and LocationPresentation::mtpPathSegments for mtp:/<Device>/<Storage>/<Subfolder>
        const QUrl dcimUrl(QStringLiteral("mtp:/Google Pixel 8 Pro/Internal shared storage/DCIM"));
        const auto expectedCrumbs = MtpUrlContract::expectedBreadcrumbSegments(dcimUrl);
        verify(expectedCrumbs.size() == 4, "MTP breadcrumb spec has 4 segments (This PC -> Device -> Storage -> DCIM)");
        verify(expectedCrumbs.at(0).url == thisPc && expectedCrumbs.at(1).url == mtpDeviceRoot
                   && expectedCrumbs.at(2).url == mtpInternalStorage,
               "MTP breadcrumb spec links each cumulative level cleanly");

        const auto actualCrumbs = LocationPresentation::mtpPathSegments(dcimUrl, drivesWithMtp);
        verify(actualCrumbs.size() == 4, "LocationPresentation::mtpPathSegments produces 4 segments for DCIM");
        verify(actualCrumbs.at(0).url == thisPc && actualCrumbs.at(0).iconName == QStringLiteral("computer")
                   && actualCrumbs.at(1).url == mtpDeviceRoot && actualCrumbs.at(1).text == QStringLiteral("Google Pixel 8 Pro")
                   && actualCrumbs.at(1).iconName == QStringLiteral("smartphone")
                   && actualCrumbs.at(2).url == mtpInternalStorage && actualCrumbs.at(2).text == QStringLiteral("Internal shared storage")
                   && actualCrumbs.at(2).iconName == QStringLiteral("folder")
                   && actualCrumbs.at(3).url == dcimUrl && actualCrumbs.at(3).text == QStringLiteral("DCIM"),
               "LocationPresentation::mtpPathSegments matches expected hierarchy, labels, and icons");

        const auto udiCrumbs = LocationPresentation::mtpPathSegments(mtpUdiUrl, drivesWithMtp);
        verify(udiCrumbs.size() == 2 && udiCrumbs.at(0).url == thisPc
                   && udiCrumbs.at(1).url == mtpUdiUrl && udiCrumbs.at(1).text == QStringLiteral("Google Pixel 8 Pro"),
               "LocationPresentation::mtpPathSegments handles pre-redirection mtp:udi=<UDI> cleanly without splitting sysfs path");

        const auto encodedCrumbs = LocationPresentation::mtpPathSegments(mtpEncoded, drivesWithMtp);
        verify(encodedCrumbs.size() == 4
                   && encodedCrumbs.at(1).text == QStringLiteral("Phone #1 & Test")
                   && encodedCrumbs.at(3).text == QStringLiteral("Folder [A]"),
               "LocationPresentation::mtpPathSegments decodes percent-encoded device and folder names cleanly");

        verify(LocationPresentation::splitLocationText(dcimUrl).contains(QStringLiteral("Google Pixel 8 Pro"))
                   && LocationPresentation::splitLocationText(dcimUrl).contains(QStringLiteral("DCIM")),
               "LocationPresentation::splitLocationText formats MTP hierarchy with chevron separators");

        // Solid vs KIO friendlyName mismatch alias test via MtpRedirectionRegistry
        const QString customUdi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/usb1/1-9");
        const DriveInfo customPhone = MtpUdsContract::makeMtpDriveInfo(
            QStringLiteral("mtp-custom"), QStringLiteral("Samsung SM-S928B"), customUdi, QStringLiteral("multimedia-player"));
        const QUrl customRedirected(QStringLiteral("mtp:/Telefon Jana (S24)/Internal storage/Photos"));
        MtpRedirectionRegistry::recordRedirection(customPhone.targetUrl, QUrl(QStringLiteral("mtp:/Telefon Jana (S24)")));
        verify(urlBelongsToMtpDrive(customRedirected, customPhone),
               "urlBelongsToMtpDrive matches redirected KIO friendlyName when it differs from Solid vendor+product name");
        verify(!urlBelongsToMtpDrive(customRedirected, mtpDrive),
               "urlBelongsToMtpDrive does not match unrelated MTP phone");

        // 7. Stage 1.4 active verifications:
        //    - parentUrl of redirected device root mtp:/<Device> returns thispc:/ (not mtp:/)
        //    - parentUrl of mtp:udi=<UDI> with empty drives list returns thispc:/ (not truncated sysfs path)
        //    - urlFromUserText("mtp:udi=...") returns valid mtp: QUrl
        //    - iconName("mtp:/<Device>") returns smartphone/multimedia-player at root, folder for subfolders
        verify(
            LocationPresentation::parentUrl(mtpDeviceRoot, drivesWithMtp, LocationPresentation::ParentProfile::Primary) == thisPc,
            "LocationPresentation::parentUrl(mtp:/<Device>, Primary) returns thispc:/ instead of mtp:/");
        verify(
            LocationPresentation::parentUrl(mtpDeviceRoot, drivesWithMtp, LocationPresentation::ParentProfile::Split) == thisPc,
            "LocationPresentation::parentUrl(mtp:/<Device>, Split) returns thispc:/ instead of mtp:/");
        verify(
            LocationPresentation::parentUrl(mtpUdiUrl, {}, LocationPresentation::ParentProfile::Primary) == thisPc,
            "LocationPresentation::parentUrl(mtp:udi=<UDI>, empty drives) returns thispc:/ instead of truncating UDI path");
        verify(
            LocationPresentation::parentUrl(mtpUdiUrl, {}, LocationPresentation::ParentProfile::Split) == thisPc,
            "LocationPresentation::parentUrl(mtp:udi=<UDI>, empty drives, Split) returns thispc:/ instead of truncating UDI path");
        const QUrl parsedMtpUdi = urlFromUserText(mtpUdiUrl.toString());
        verify(
            parsedMtpUdi.isValid() && parsedMtpUdi.scheme() == QStringLiteral("mtp") && parsedMtpUdi == mtpUdiUrl,
            "urlFromUserText('mtp:udi=...') preserves valid mtp:udi= QUrl without failing in QUrl::fromUserInput");
        verify(
            LocationPresentation::iconName(mtpDeviceRoot) == QStringLiteral("smartphone")
                || LocationPresentation::iconName(mtpDeviceRoot) == QStringLiteral("multimedia-player"),
            "LocationPresentation::iconName(mtp:/<Device>) returns smartphone/multimedia-player icon");
        verify(
            LocationPresentation::iconName(mtpInternalStorage) == QStringLiteral("folder"),
            "LocationPresentation::iconName(mtp:/<Device>/<Storage>) returns folder icon");
    }

    qInfo("PASS: %d location presentation assertions (%d pending MTP specs)", checks, pendingMtpSpecs);
    return 0;
}
