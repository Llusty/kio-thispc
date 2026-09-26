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
        primarySnapshot = drives;
        splitSnapshot = drives;
    });

    verify(coordinator.drives().isEmpty() && !coordinator.isLoading(),
           "initial snapshot is empty and idle");
    coordinator.refresh();
    verify(started == 1 && coordinator.isLoading() && requests.size() == 1,
           "refresh starts one source");
    requests[0]->entries({
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

    coordinator.refresh();
    auto replaced = requests[1];
    coordinator.refresh();
    auto current = requests[2];
    verify(replaced->canceled && coordinator.isLoading() && started == 3,
           "repeated refresh cancels and replaces the old source");
    replaced->entries({driveEntry("Late", "file:///late", "1%")});
    replaced->finished(true, {});
    verify(coordinator.drives().size() == 3 && finished == 1,
           "late callbacks from a canceled source are ignored");
    current->entries({driveEntry("Fresh", "file:///fresh", "7%")});
    current->finished(true, {});
    verify(coordinator.drives().size() == 1 && coordinator.drives()[0].name == "Fresh"
               && finished == 2,
           "replacement source publishes its own snapshot");

    coordinator.refresh();
    requests[3]->entries({driveEntry("Partial", "file:///partial", "9%")});
    requests[3]->finished(false, "fixture failure");
    verify(errors == 1 && finished == 3 && coordinator.drives().size() == 1
               && coordinator.drives()[0].name == "Fresh",
           "error preserves the previous safe snapshot");
    coordinator.cancel();
    verify(!coordinator.isLoading(), "explicit cancel returns to idle state");

    qInfo("PASS: %d DriveHomeCoordinator assertions; parsing, snapshots, dedup, replacement and stale callbacks", checks);
}
