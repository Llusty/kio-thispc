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

    qInfo("PASS: %d launch url resolver assertions", checks);
    return 0;
}
