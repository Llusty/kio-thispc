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

    qInfo("PASS: %d DriveHomeCoordinator assertions; parsing, snapshots, dedup, replacement and stale callbacks", checks);
}
