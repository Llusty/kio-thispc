/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Tests for 0.35 Stage 2: unmounted removable volumes, exactly-once mount controller, and pane routing.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static KIO::UDSEntry makeEntry(
    const QString &id,
    const QString &name,
    const QString &target,
    bool isMounted,
    const QString &mountPoint,
    const QString &udi,
    const QString &capacity = QStringLiteral("16 GiB"),
    const QString &free = QStringLiteral("—"),
    const QString &fileSystem = QStringLiteral("vfat"))
{
    KIO::UDSEntry entry;
    entry.fastInsert(KIO::UDSEntry::UDS_NAME, id);
    entry.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, name);
    if (isMounted && !target.isEmpty()) {
        entry.fastInsert(KIO::UDSEntry::UDS_TARGET_URL, target);
        entry.fastInsert(KIO::UDSEntry::UDS_ICON_NAME, QStringLiteral("drive-removable-media"));
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 0, free);
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 1, capacity);
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 2, QStringLiteral("50%"));
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 3, fileSystem);
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 4, mountPoint);
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 5, QStringLiteral("1"));
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 6, udi);
    } else {
        // Unmounted: no UDS_TARGET_URL
        entry.fastInsert(KIO::UDSEntry::UDS_ICON_NAME, QStringLiteral("drive-removable-media"));
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 0, QStringLiteral("—"));
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 1, capacity);
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 2, QStringLiteral("—"));
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 3, fileSystem);
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 4, QString());
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 5, QStringLiteral("0"));
        entry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 6, udi);
    }
    return entry;
}

struct TestDriveSourceState {
    DriveHomeSource::EntriesCallback entries;
    DriveHomeSource::FinishedCallback finished;
    bool canceled = false;
};

class TestDriveSource final : public DriveHomeSource {
public:
    explicit TestDriveSource(std::shared_ptr<TestDriveSourceState> state)
        : m_state(std::move(state)) {}
    void cancel() override { m_state->canceled = true; }
private:
    std::shared_ptr<TestDriveSourceState> m_state;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // =======================================================================
    // 1-6: MODEL / DISCOVERY & COORDINATOR
    // =======================================================================
    {
        std::shared_ptr<TestDriveSourceState> state;
        DriveHomeCoordinator coordinator(
            [&state](DriveHomeSource::EntriesCallback entries,
                     DriveHomeSource::FinishedCallback finished) {
                state = std::make_shared<TestDriveSourceState>();
                state->entries = std::move(entries);
                state->finished = std::move(finished);
                return std::make_unique<TestDriveSource>(state);
            });

        coordinator.refresh();
        verify(state != nullptr, "coordinator started source");

        // 1: mounted fixed volume
        // 2: mounted removable volume
        // 3: unmounted removable volume
        // 4: unmounted volume has empty targetUrl and empty mountPoint
        const QString udiUsb = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdb1");
        const QString idUsb = QStringLiteral("volume-usb-pendrive");

        state->entries({
            makeEntry(QStringLiteral("."), QStringLiteral("Ten komputer"),
                      QString(), false, QString(), QString()),
            makeEntry(QStringLiteral("volume-sys"), QStringLiteral("System"),
                      QStringLiteral("file:///"), true, QStringLiteral("/"),
                      QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda2"),
                      QStringLiteral("100 GiB"), QStringLiteral("40 GiB"), QStringLiteral("ext4")),
            makeEntry(QStringLiteral("volume-mounted-usb"), QStringLiteral("MountedUSB"),
                      QStringLiteral("file:///media/usb"), true, QStringLiteral("/media/usb"),
                      QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdc1")),
            makeEntry(idUsb, QStringLiteral("UnmountedPen"),
                      QString(), false, QString(), udiUsb, QStringLiteral("32 GiB"))
        });
        state->finished(true, {});

        const auto &drives = coordinator.drives();
        verify(drives.size() == 3, "all 3 volumes discovered; root dot entry suppressed");

        // Fixed mounted
        verify(drives[0].name == QStringLiteral("System") && drives[0].isMounted
                   && drives[0].targetUrl == QUrl(QStringLiteral("file:///")),
               "mounted fixed volume is visible with valid targetUrl");

        // Removable mounted
        verify(drives[1].name == QStringLiteral("MountedUSB") && drives[1].isMounted
                   && drives[1].targetUrl == QUrl(QStringLiteral("file:///media/usb")),
               "mounted removable volume is visible with valid targetUrl");

        // Removable unmounted
        verify(drives[2].name == QStringLiteral("UnmountedPen") && !drives[2].isMounted,
               "unmounted removable volume is visible and flagged unmounted");
        verify(drives[2].name != QStringLiteral("Ten komputer"),
               "unmounted volume does not fall back to root display name");
        verify(drives[2].targetUrl.isEmpty() && !drives[2].targetUrl.isValid(),
               "unmounted volume has empty/invalid targetUrl");
        verify(drives[2].mountPoint.isEmpty(),
               "unmounted volume has no fake mount path");
        verify(drives[2].id != drives[2].udi,
               "id and udi are distinct and not conflated");
        verify(drives[2].id == idUsb && drives[2].udi == udiUsb,
               "stable ID and Solid UDI are preserved");

        // 5 & 6 & 16: transition unmounted -> mounted maintains stable ID and updates single card
        coordinator.refresh();
        state->entries({
            makeEntry(idUsb, QStringLiteral("UnmountedPen"),
                      QStringLiteral("file:///run/media/user/PEN"), true,
                      QStringLiteral("/run/media/user/PEN"), udiUsb, QStringLiteral("32 GiB"))
        });
        state->finished(true, {});

        const auto &updatedDrives = coordinator.drives();
        verify(updatedDrives.size() == 1, "transitioned volume replaces old state");
        verify(updatedDrives[0].id == idUsb, "volume retains exact same stable ID after mount");
        verify(updatedDrives[0].isMounted, "volume is now flagged mounted");
        verify(updatedDrives[0].targetUrl == QUrl(QStringLiteral("file:///run/media/user/PEN")),
               "volume now has real mount targetUrl");

        // 17: remove deletes card cleanly
        coordinator.refresh();
        state->entries({});
        state->finished(true, {});
        verify(coordinator.drives().isEmpty(), "removal empties the snapshot without residue");

        // 18: duplicate suppression under refresh burst
        coordinator.refresh();
        state->entries({
            makeEntry(idUsb, QStringLiteral("UnmountedPen"), QString(), false, QString(), udiUsb),
            makeEntry(idUsb, QStringLiteral("UnmountedPen Duplicate"), QString(), false, QString(), udiUsb)
        });
        state->finished(true, {});
        verify(coordinator.drives().size() == 1, "refresh burst collapses duplicate IDs into single card");
    }

    // =======================================================================
    // 7-12: DEVICE MOUNT CONTROLLER (EXACTLY-ONCE SEMANTICS)
    // =======================================================================
    {
        QString lastRequestedUdi;
        int mountRequestsStarted = 0;
        DeviceMountController::Hooks hooks;
        hooks.requestMount = [&](const QString &udi) {
            lastRequestedUdi = udi;
            ++mountRequestsStarted;
            return true;
        };

        DeviceMountController controller(hooks);

        const QString udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdd1");

        // Empty UDI rejection
        int emptyFired = 0;
        bool emptySuccess = true;
        QString emptyErr;
        controller.mountDevice(QString(), [&](bool success, const QString &, const QString &err) {
            ++emptyFired;
            emptySuccess = success;
            emptyErr = err;
        });
        verify(emptyFired == 1 && !emptySuccess && !emptyErr.isEmpty(),
               "empty device UDI is rejected immediately with error");
        verify(mountRequestsStarted == 0,
               "empty device UDI does not trigger backend requestMount");

        // Non-Solid / invalid ID rejection via live Solid interface
        {
            DeviceMountController solidController;
            int nonSolidFired = 0;
            bool nonSolidSuccess = true;
            QString nonSolidErr;
            solidController.mountDevice(QStringLiteral("volume-sys-fake-id"),
                                        [&](bool success, const QString &, const QString &err) {
                ++nonSolidFired;
                nonSolidSuccess = success;
                nonSolidErr = err;
            });
            verify(nonSolidFired == 1 && !nonSolidSuccess && !nonSolidErr.isEmpty(),
                   "non-Solid ID is rejected by Solid::Device validation");
        }

        // 7 & 8: mount request and success returns correct mount point
        int callbacksFired = 0;
        bool resultSuccess = false;
        QString resultMountPoint;
        QString resultError;

        controller.mountDevice(udi, [&](bool success, const QString &mp, const QString &err) {
            ++callbacksFired;
            resultSuccess = success;
            resultMountPoint = mp;
            resultError = err;
        });

        verify(mountRequestsStarted == 1 && lastRequestedUdi == udi, "mount request initiates via backend");
        verify(controller.isMountPending(udi), "mount is flagged as pending");

        // 12: exactly-once test: simulate setupDone, then accessibilityChanged, then deviceRemoved
        controller.simulateSetupDone(udi, true, QStringLiteral("/media/usb_test"));

        verify(callbacksFired == 1, "callback fired exactly once on success");
        verify(resultSuccess && resultMountPoint == QStringLiteral("/media/usb_test") && resultError.isEmpty(),
               "success returns correct mount point");
        verify(!controller.isMountPending(udi), "request is no longer pending");

        // Subsequent signals must be ignored
        controller.simulateAccessibilityChanged(udi, true, QStringLiteral("/media/usb_test_late"));
        controller.simulateSetupDone(udi, false, {}, QStringLiteral("Late failure"));
        controller.simulateDeviceRemoved(udi);

        verify(callbacksFired == 1, "duplicate Solid signals do NOT trigger second callback");

        // 9: failure path
        callbacksFired = 0;
        resultSuccess = true;
        controller.mountDevice(udi, [&](bool success, const QString &mp, const QString &err) {
            ++callbacksFired;
            resultSuccess = success;
            resultMountPoint = mp;
            resultError = err;
        });
        verify(controller.isMountPending(udi), "second request is pending");

        controller.simulateSetupDone(udi, false, {}, QStringLiteral("Access denied"));
        verify(callbacksFired == 1 && !resultSuccess && !resultError.isEmpty(),
               "failure invokes callback with error message");
        verify(!controller.isMountPending(udi), "failed request cleared pending state");

        // 10: device disappears during request
        callbacksFired = 0;
        resultSuccess = true;
        controller.mountDevice(udi, [&](bool success, const QString &mp, const QString &err) {
            ++callbacksFired;
            resultSuccess = success;
            resultMountPoint = mp;
            resultError = err;
        });
        controller.simulateDeviceRemoved(udi);
        verify(callbacksFired == 1 && !resultSuccess,
               "device disconnect during request terminates with failure");
        verify(!controller.isMountPending(udi), "disconnected request cleared pending state");

        // AccessibilityChanged terminal before setupDone
        callbacksFired = 0;
        resultSuccess = false;
        controller.mountDevice(udi, [&](bool success, const QString &mp, const QString &err) {
            ++callbacksFired;
            resultSuccess = success;
            resultMountPoint = mp;
            resultError = err;
        });
        controller.simulateAccessibilityChanged(udi, true, QStringLiteral("/media/fast_mount"));
        verify(callbacksFired == 1 && resultSuccess && resultMountPoint == QStringLiteral("/media/fast_mount"),
               "accessibilityChanged can resolve request before setupDone");
        // Late setupDone dropped:
        controller.simulateSetupDone(udi, true, QStringLiteral("/media/fast_mount"));
        verify(callbacksFired == 1, "late setupDone after accessibilityChanged is dropped");

        // Multiple simultaneous callers for same UDI
        int callerA = 0;
        int callerB = 0;
        mountRequestsStarted = 0;
        controller.mountDevice(udi, [&](bool, const QString &, const QString &) { ++callerA; });
        controller.mountDevice(udi, [&](bool, const QString &, const QString &) { ++callerB; });
        verify(mountRequestsStarted == 1, "second mount call attaches to in-flight request without re-issuing setup");
        controller.simulateSetupDone(udi, true, QStringLiteral("/media/multi"));
        verify(callerA == 1 && callerB == 1, "both attached callbacks fired exactly once");
    }

    // =======================================================================
    // 13-15: PANE ROUTING IN THISPCWINDOW
    // =======================================================================
    {
        ThisPcWindow window;
        window.resize(1000, 700);
        window.show();
        QApplication::processEvents();

        QString lastRequestedWindowUdi;
        int windowMountRequestsStarted = 0;
        DeviceMountController::Hooks windowHooks;
        windowHooks.requestMount = [&](const QString &reqUdi) {
            lastRequestedWindowUdi = reqUdi;
            ++windowMountRequestsStarted;
            return true;
        };
        window.mountControllerForTesting().setHooksForTesting(windowHooks);

        DriveInfo unmountedDrive;
        unmountedDrive.id = QStringLiteral("volume-usb-pendrive");
        unmountedDrive.udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdb1");
        unmountedDrive.name = QStringLiteral("FlashDrive");
        unmountedDrive.capacityText = QStringLiteral("16 GiB");
        unmountedDrive.freeText = QStringLiteral("—");
        unmountedDrive.fileSystem = QStringLiteral("vfat");
        unmountedDrive.mountPoint.clear();
        unmountedDrive.targetUrl = QUrl();
        unmountedDrive.isMounted = false;

        DriveInfo mountedDrive;
        mountedDrive.id = QStringLiteral("volume-sys");
        mountedDrive.udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda2");
        mountedDrive.name = QStringLiteral("System");
        mountedDrive.capacityText = QStringLiteral("100 GiB");
        mountedDrive.freeText = QStringLiteral("50 GiB");
        mountedDrive.fileSystem = QStringLiteral("ext4");
        mountedDrive.mountPoint = QStringLiteral("/");
        mountedDrive.targetUrl = QUrl::fromLocalFile(QStringLiteral("/"));
        mountedDrive.isMounted = true;

        // Set snapshot on window
        window.m_driveHomeCoordinator.setSnapshotForTesting({mountedDrive, unmountedDrive});
        QApplication::processEvents();

        // Check sidebar receives only mounted drives
        if (window.m_sidebar) {
            verify(window.m_sidebar->drives().size() == 1,
                   "sidebar displays exclusively mounted drives; unmounted drive excluded");
            verify(window.m_sidebar->drives()[0].isMounted,
                   "sidebar drive is mounted");
        }

        // Test Primary pane activation -> mounts and navigates Primary pane
        QTemporaryDir primaryMount;
        const QString primaryMountPath = primaryMount.path();

        window.setActivePane(PaneId::Primary);
        window.handleDriveActivation(PaneId::Primary, unmountedDrive);

        verify(windowMountRequestsStarted == 1,
               "activation triggers exactly one mount request");
        verify(lastRequestedWindowUdi == unmountedDrive.udi,
               "activation mounts using drive.udi, not drive.id");
        verify(lastRequestedWindowUdi != unmountedDrive.id,
               "activation does NOT pass drive.id to mount");

        // Duplicate activation while mount is pending must attach without re-issuing setup
        window.handleDriveActivation(PaneId::Primary, unmountedDrive);
        verify(windowMountRequestsStarted == 1,
               "duplicate activation while pending does NOT trigger additional setup request");

        verify(window.mountControllerForTesting().isMountPending(unmountedDrive.udi),
               "activation of unmounted card starts mount request");

        // Switch active pane to Split while mount is in flight to test crosstalk protection
        window.setActivePane(PaneId::Split);

        // Mount finishes
        window.mountControllerForTesting().simulateSetupDone(
            unmountedDrive.udi, true, primaryMountPath);
        QApplication::processEvents();

        // 13 & 15: Primary request navigates Primary, not Split!
        verify(window.m_primaryPane->currentUrl() == QUrl::fromLocalFile(primaryMountPath),
               "Primary activation opens mount point in Primary pane");
        verify(window.m_splitPane->currentUrl() == kThisPcUrl,
               "Split pane did not navigate on Primary mount completion (no cross-pane crosstalk)");

        // 14: Test Split pane activation -> mounts and navigates Split pane
        QTemporaryDir splitMount;
        const QString splitMountPath = splitMount.path();

        // Reset Primary back to home
        window.navigatePane(PaneId::Primary, kThisPcUrl);
        QApplication::processEvents();

        window.handleDriveActivation(PaneId::Split, unmountedDrive);
        verify(window.mountControllerForTesting().isMountPending(unmountedDrive.udi),
               "Split activation starts mount request");

        // Switch active pane to Primary while mount is in flight
        window.setActivePane(PaneId::Primary);

        window.mountControllerForTesting().simulateSetupDone(
            unmountedDrive.udi, true, splitMountPath);
        QApplication::processEvents();

        verify(window.m_splitPane->currentUrl() == QUrl::fromLocalFile(splitMountPath),
               "Split activation opens mount point in Split pane");
        verify(window.m_primaryPane->currentUrl() == kThisPcUrl,
               "Primary pane remained unchanged during Split mount (no cross-pane crosstalk)");

        // Test Mount Failure: does not navigate
        window.navigatePane(PaneId::Primary, kThisPcUrl);
        QApplication::processEvents();

        window.handleDriveActivation(PaneId::Primary, unmountedDrive);
        verify(!window.m_refreshTimer.isActive(),
               "refreshTimer is stopped by default to prevent idle cyclic polling flicker");

        window.mountControllerForTesting().simulateSetupDone(
            unmountedDrive.udi, false, {}, QStringLiteral("Simulated mount failure"));
        QApplication::processEvents();

        verify(window.m_primaryPane->currentUrl() == kThisPcUrl,
               "Mount failure does not navigate pane to fake path");

        // Verify post-mount refresh does NOT activate periodic polling timer
        verify(!window.m_refreshTimer.isActive(),
               "post-mount refresh does not start cyclic timer or introduce periodic polling");
    }

    qInfo("PASS: %d DeviceMountController assertions; unmounted discovery, stable ID, exactly-once signals, and pane routing", checks);
    return 0;
}
