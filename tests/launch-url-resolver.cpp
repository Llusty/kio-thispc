/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Appended to the temporary regression binary by run-pane-actions.py.

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int checks = 0;
    auto verify = [&](bool condition, const char *message) {
        ++checks;
        if (!condition) qFatal("FAIL: %s", message);
    };

    const QUrl mountC = QUrl::fromLocalFile(QStringLiteral("/mnt/c"));
    const QUrl mountGames = QUrl::fromLocalFile(QStringLiteral("/mnt/games"));

    DriveInfo driveC;
    driveC.id = QStringLiteral("drive-c");
    driveC.name = QStringLiteral("Windows");
    driveC.udi = QStringLiteral("udi-123");
    driveC.targetUrl = mountC;
    driveC.isMounted = true;

    DriveInfo driveGames;
    driveGames.id = QStringLiteral("drive-games");
    driveGames.name = QStringLiteral("Games");
    driveGames.udi = QStringLiteral("udi-games");
    driveGames.targetUrl = mountGames;
    driveGames.isMounted = true;

    DriveInfo driveUnmounted;
    driveUnmounted.id = QStringLiteral("drive-unmounted");
    driveUnmounted.name = QStringLiteral("Unmounted");
    driveUnmounted.udi = QStringLiteral("udi-unmounted");
    driveUnmounted.targetUrl = QUrl::fromLocalFile(QStringLiteral("/mnt/unmounted"));
    driveUnmounted.isMounted = false;

    const QList<DriveInfo> drives{driveC, driveGames, driveUnmounted};

    // 1. Raw file URL preserved (home app)
    const QUrl url1(QStringLiteral("file:///home/user/app.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url1, drives) == url1,
           "1. Raw file URL preserved (home app)");

    // 2. Raw file URL preserved (games app)
    const QUrl url2(QStringLiteral("file:///mnt/games/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url2, drives) == url2,
           "2. Raw file URL preserved (games app)");

    // 3. Explicit targetUrl takes precedence over rawUrl
    const QUrl raw3(QStringLiteral("thispc:/drive-games/game.exe"));
    const QUrl target3(QStringLiteral("file:///mnt/games/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(raw3, drives, target3) == target3,
           "3. Explicit targetUrl takes precedence over rawUrl");

    // 4. Explicit targetUrl with invalid rawUrl
    const QUrl raw4;
    const QUrl target4(QStringLiteral("file:///mnt/games/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(raw4, drives, target4) == target4,
           "4. Explicit targetUrl with invalid rawUrl");

    // 5. thispc:/drive-c/game.exe resolves to file:///mnt/c/game.exe
    const QUrl url5(QStringLiteral("thispc:/drive-c/game.exe"));
    const QUrl expected5 = QUrl::fromLocalFile(QStringLiteral("/mnt/c/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url5, drives) == expected5,
           "5. thispc:/drive-c/game.exe resolves to local mount path");

    // 6. Subdirectory path thispc:/drive-c/subdir/game.exe
    const QUrl url6(QStringLiteral("thispc:/drive-c/subdir/game.exe"));
    const QUrl expected6 = QUrl::fromLocalFile(QStringLiteral("/mnt/c/subdir/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url6, drives) == expected6,
           "6. Subdirectory path resolves to local mount path");

    // 7. Root of drive without trailing slash
    const QUrl url7(QStringLiteral("thispc:/drive-c"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url7, drives) == mountC,
           "7. Root of drive without trailing slash resolves to mount root");

    // 8. Root of drive with trailing slash
    const QUrl url8(QStringLiteral("thispc:/drive-c/"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url8, drives) == mountC,
           "8. Root of drive with trailing slash resolves to mount root");

    // 9. Unmounted drive cannot resolve to local file URL
    const QUrl url9(QStringLiteral("thispc:/drive-unmounted/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url9, drives) == url9,
           "9. Unmounted drive cannot resolve to local file URL");

    // 10. Unknown drive id does not resolve
    const QUrl url10(QStringLiteral("thispc:/drive-unknown/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url10, drives) == url10,
           "10. Unknown drive id does not resolve");

    // 11. Root thispc:/ is not a local file
    const QUrl url11(QStringLiteral("thispc:/"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url11, drives) == url11,
           "11. Root thispc:/ remains unchanged");

    // 12. Case mismatch DRIVE-C rejected (strictly exact match)
    const QUrl url12(QStringLiteral("thispc:/DRIVE-C/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url12, drives) == url12,
           "12. Case mismatch DRIVE-C rejected");

    // 13. Drive name match attempt rejected (no name matching)
    const QUrl url13(QStringLiteral("thispc:/Windows/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url13, drives) == url13,
           "13. Drive name match attempt rejected");

    // 14. Drive UDI match attempt rejected (no UDI matching)
    const QUrl url14(QStringLiteral("thispc:/udi-123/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url14, drives) == url14,
           "14. Drive UDI match attempt rejected");

    // 15. Fuzzy drive- stripping rejected (c instead of drive-c)
    const QUrl url15(QStringLiteral("thispc:/c/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url15, drives) == url15,
           "15. Fuzzy drive- stripping rejected");

    // 16. Traversal attempt .. rejected
    const QUrl url16(QStringLiteral("thispc:/drive-c/../secret"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url16, drives) == url16,
           "16. Traversal attempt .. rejected");

    // 17. Deep traversal attempt foo/../../secret rejected
    const QUrl url17(QStringLiteral("thispc:/drive-c/foo/../../secret"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url17, drives) == url17,
           "17. Deep traversal attempt foo/../../secret rejected");

    // 18. Dot segment . rejected
    const QUrl url18(QStringLiteral("thispc:/drive-c/./game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url18, drives) == url18,
           "18. Dot segment . rejected");

    // 19. Lowercase percent-encoded traversal %2e%2e rejected
    const QUrl url19(QStringLiteral("thispc:/drive-c/%2e%2e/secret"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url19, drives) == url19,
           "19. Lowercase percent-encoded traversal %2e%2e rejected");

    // 20. Uppercase percent-encoded traversal %2E%2E rejected
    const QUrl url20(QStringLiteral("thispc:/drive-c/%2E%2E/secret"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url20, drives) == url20,
           "20. Uppercase percent-encoded traversal %2E%2E rejected");

    // 21. Percent-encoded slash %2f rejected
    const QUrl url21(QStringLiteral("thispc:/drive-c/foo%2fbar/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url21, drives) == url21,
           "21. Percent-encoded slash %2f rejected");

    // 22. Percent-encoded backslash %5c rejected
    const QUrl url22(QStringLiteral("thispc:/drive-c/foo%5cbar/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url22, drives) == url22,
           "22. Percent-encoded backslash %5c rejected");

    // 23. Literal backslash rejected
    const QUrl url23(QStringLiteral("thispc:/drive-c/foo\\bar"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url23, drives) == url23,
           "23. Literal backslash rejected");

    // 24. Remote URL sftp preserved
    const QUrl url24(QStringLiteral("sftp://user@host/path/app.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url24, drives) == url24,
           "24. Remote URL sftp preserved");

    // 25. Remote URL smb preserved
    const QUrl url25(QStringLiteral("smb://server/share/app.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url25, drives) == url25,
           "25. Remote URL smb preserved");

    // 26. Remote URL fish preserved
    const QUrl url26(QStringLiteral("fish://user@host/path/app.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url26, drives) == url26,
           "26. Remote URL fish preserved");

    // 27. Spaces and special chars in path preserved
    const QUrl url27(QStringLiteral("thispc:/drive-c/Program Files/My Game/game.exe"));
    const QUrl expected27 = QUrl::fromLocalFile(QStringLiteral("/mnt/c/Program Files/My Game/game.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url27, drives) == expected27,
           "27. Spaces and special chars in path preserved");

    // 28. Unicode characters in path preserved
    const QUrl url28(QStringLiteral("thispc:/drive-c/Zażółć gęślą/gra.exe"));
    const QUrl expected28 = QUrl::fromLocalFile(QStringLiteral("/mnt/c/Zażółć gęślą/gra.exe"));
    verify(LaunchUrlResolver::resolveLaunchUrl(url28, drives) == expected28,
           "28. Unicode characters in path preserved");

    // 29. Non-file targetUrl preserved, never converted to file://
    const QUrl raw29(QStringLiteral("thispc:/drive-c/file.txt"));
    const QUrl target29(QStringLiteral("sftp://server/share/file.txt"));
    verify(LaunchUrlResolver::resolveLaunchUrl(raw29, drives, target29) == target29,
           "29. Non-file targetUrl preserved, never converted to file://");

    // 30. Empty or invalid rawUrl handled safely
    const QUrl url30;
    verify(LaunchUrlResolver::resolveLaunchUrl(url30, drives) == url30,
           "30. Empty or invalid rawUrl handled safely");

    // =======================================================================
    // Stage 1.2: MTP URL Resolution & Non-Local Safety Contract (Section 4 & 5)
    // =======================================================================
    {
        const QString mtpUdi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2");
        DriveInfo mtpDrive;
        mtpDrive.id = QStringLiteral("mtp-pixel8");
        mtpDrive.name = QStringLiteral("Google Pixel 8 Pro");
        mtpDrive.udi = mtpUdi;
        mtpDrive.targetUrl = QUrl(QStringLiteral("mtp:udi=%1").arg(mtpUdi));
        mtpDrive.mountPoint.clear();
        mtpDrive.fileSystem = QStringLiteral("MTP");
        mtpDrive.isMounted = true;
        mtpDrive.isRemovable = true;

        QList<DriveInfo> drivesWithMtp = drives;
        drivesWithMtp.append(mtpDrive);

        // 31. Direct mtp:udi=<UDI> URL preserved untouched, never converted to file://
        const QUrl mtpUdiUrl = mtpDrive.targetUrl;
        const QUrl res31 = LaunchUrlResolver::resolveLaunchUrl(mtpUdiUrl, drivesWithMtp);
        verify(res31 == mtpUdiUrl && !res31.isLocalFile() && res31.scheme() == QStringLiteral("mtp"),
               "31. Direct mtp:udi=<UDI> URL preserved untouched");

        // 32. Redirected mtp:/<Device>/Internal shared storage/... preserved untouched
        const QUrl mtpInternalFile(QStringLiteral("mtp:/Google Pixel 8 Pro/Internal shared storage/DCIM/Camera/IMG_20261017.jpg"));
        const QUrl res32 = LaunchUrlResolver::resolveLaunchUrl(mtpInternalFile, drivesWithMtp);
        verify(res32 == mtpInternalFile && !res32.isLocalFile() && res32.scheme() == QStringLiteral("mtp"),
               "32. Redirected mtp:/ internal storage file URL preserved untouched");

        // 33. Redirected mtp:/<Device>/SD card/... preserved untouched
        const QUrl mtpSdFile(QStringLiteral("mtp:/Google Pixel 8 Pro/SD card/Music/Track 01.flac"));
        const QUrl res33 = LaunchUrlResolver::resolveLaunchUrl(mtpSdFile, drivesWithMtp);
        verify(res33 == mtpSdFile && !res33.isLocalFile(),
               "33. Redirected mtp:/ SD card file URL preserved untouched");

        // 34. Unicode and spaces in mtp:/ URL preserved untouched
        const QUrl mtpUnicode(QStringLiteral("mtp:/Telefon Żółć/Pamięć wewnętrzna/Wideo z wakacji 4K.mp4"));
        const QUrl res34 = LaunchUrlResolver::resolveLaunchUrl(mtpUnicode, drivesWithMtp);
        verify(res34 == mtpUnicode && !res34.isLocalFile(),
               "34. Unicode and spaces in mtp:/ URL preserved untouched");

        // 35. Percent-encoded characters in mtp:/ URL preserved untouched
        const QUrl mtpEncoded(QStringLiteral("mtp:/Phone%20%231%20%26%20Test/Internal%20storage/file%20%5B1%5D.pdf"));
        const QUrl res35 = LaunchUrlResolver::resolveLaunchUrl(mtpEncoded, drivesWithMtp);
        verify(res35 == mtpEncoded && !res35.isLocalFile(),
               "35. Percent-encoded characters in mtp:/ URL preserved untouched");

        // 36. Explicit mtp: targetUrl takes precedence over thispc:/ rawUrl and is never converted to file://
        const QUrl rawThisPcMtp(QStringLiteral("thispc:/mtp-pixel8"));
        const QUrl res36 = LaunchUrlResolver::resolveLaunchUrl(rawThisPcMtp, drivesWithMtp, mtpUdiUrl);
        verify(res36 == mtpUdiUrl && !res36.isLocalFile() && res36.scheme() == QStringLiteral("mtp"),
               "36. Explicit mtp: targetUrl takes precedence and is never converted to file://");

        // 37. Synthetic thispc:/<mtp-id>/subpath is NEVER converted to a fake local file:// path
        const QUrl syntheticThisPcSubpath(QStringLiteral("thispc:/mtp-pixel8/Internal shared storage/secret.txt"));
        const QUrl res37 = LaunchUrlResolver::resolveLaunchUrl(syntheticThisPcSubpath, drivesWithMtp);
        verify(!res37.isLocalFile() && res37.scheme() != QStringLiteral("file"),
               "37. thispc:/<mtp-id>/subpath is never converted to a fake local file:// path");
    }

    qInfo("PASS: %d launch url resolver assertions", checks);
    return 0;
}
