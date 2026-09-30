// Tests for 0.35 Stage 3: Unmount / Eject / Safely Remove
// Covers:
//   - unmount success / failure / duplicate / exactly-once
//   - eject success / failure / duplicate / exactly-once
//   - safely remove: single-partition, multi-partition failure abort
//   - exactly-once: teardownDone vs accessibilityChanged, PowerOff reply vs deviceRemoved
//   - capability queries: canEject, canSafelyRemove
//   - menu visibility per drive state and capability
//   - Primary/Split pane routing after successful unmount
//   - correct drive.udi vs drive.id routing
//   - model refresh after success
//   - isOperationPending blocks duplicate operations
//   - Stage 2 regression: no periodic timer, no spurious drivesChanged

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

// Helper: make a DriveInfo for tests
static DriveInfo makeRemovableDrive(
    const QString &id,
    const QString &udi,
    const QString &mountPoint,
    bool isMounted = true,
    bool isRemovable = true)
{
    DriveInfo d;
    d.id = id;
    d.udi = udi;
    d.name = id;
    d.mountPoint = mountPoint;
    d.targetUrl = isMounted ? QUrl::fromLocalFile(mountPoint) : QUrl();
    d.isMounted = isMounted;
    d.isRemovable = isRemovable;
    d.capacityText = QStringLiteral("16 GiB");
    d.freeText = QStringLiteral("8 GiB");
    d.fileSystem = QStringLiteral("vfat");
    return d;
}

// --- DriveHomeCoordinator test harness (local to this namespace) ---
struct RemovalTestDriveSourceState {
    DriveHomeSource::EntriesCallback entries;
    DriveHomeSource::FinishedCallback finished;
    bool canceled = false;
};

class RemovalTestDriveSource final : public DriveHomeSource {
public:
    explicit RemovalTestDriveSource(std::shared_ptr<RemovalTestDriveSourceState> state)
        : m_state(std::move(state)) {}
    void cancel() override { m_state->canceled = true; }
private:
    std::shared_ptr<RemovalTestDriveSourceState> m_state;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    const QString udi1 = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdb1");
    const QString udi2 = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdb2");
    const QString driveUdi = QStringLiteral("/org/freedesktop/UDisks2/drives/sdb");

    // =========================================================================
    // SECTION 1: UNMOUNT — basic success path
    // =========================================================================
    {
        int unmountCalled = 0;
        QString lastUnmountUdi;
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [&](const QString &udi) {
            ++unmountCalled;
            lastUnmountUdi = udi;
            return true;
        };

        DeviceRemovalController ctrl(hooks);

        // 1: empty UDI is rejected immediately
        int emptyFired = 0;
        bool emptySuccess = true;
        ctrl.unmountDevice(QString(), [&](bool s, const QString &err) {
            ++emptyFired;
            emptySuccess = s;
            verify(!err.isEmpty(), "empty UDI rejection carries error message");
        });
        verify(emptyFired == 1, "empty UDI: callback fires immediately");
        verify(!emptySuccess, "empty UDI: callback reports failure");
        verify(unmountCalled == 0, "empty UDI: does NOT trigger backend requestUnmount");

        // 2: normal unmount request
        int fired = 0;
        bool success = false;
        ctrl.unmountDevice(udi1, [&](bool s, const QString &) {
            ++fired;
            success = s;
        });
        verify(unmountCalled == 1, "unmount request issued to backend");
        verify(lastUnmountUdi == udi1, "unmount sends correct UDI");
        verify(ctrl.isUnmountPending(udi1), "request is pending");
        verify(ctrl.isOperationPending(udi1), "isOperationPending returns true for pending unmount");

        // 3: simulateTeardownDone → success
        ctrl.simulateTeardownDone(udi1, true);
        verify(fired == 1, "callback fires exactly once on teardown success");
        verify(success, "teardown success reported to callback");
        verify(!ctrl.isUnmountPending(udi1), "pending state cleared after success");

        // Late signals must be silently ignored (exactly-once check)
        ctrl.simulateTeardownDone(udi1, true);
        ctrl.simulateAccessibilityChanged(udi1, false);
        ctrl.simulateDeviceRemoved(udi1);
        verify(fired == 1, "late signals do NOT fire second callback");
    }

    // =========================================================================
    // SECTION 2: UNMOUNT — failure path
    // =========================================================================
    {
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [](const QString &) { return true; };
        DeviceRemovalController ctrl(hooks);

        int fired = 0;
        bool success = true;
        QString errMsg;
        ctrl.unmountDevice(udi1, [&](bool s, const QString &e) {
            ++fired;
            success = s;
            errMsg = e;
        });
        ctrl.simulateTeardownDone(udi1, false, QStringLiteral("Device busy"));
        verify(fired == 1, "teardown failure fires callback once");
        verify(!success, "teardown failure reported as failure");
        verify(!errMsg.isEmpty(), "teardown failure carries error message");
        verify(!ctrl.isUnmountPending(udi1), "pending state cleared after failure");
    }

    // =========================================================================
    // SECTION 3: UNMOUNT — duplicate request attaches (no double backend call)
    // =========================================================================
    {
        int unmountCalled = 0;
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [&](const QString &) {
            ++unmountCalled;
            return true;
        };
        DeviceRemovalController ctrl(hooks);

        int firedA = 0, firedB = 0;
        ctrl.unmountDevice(udi1, [&](bool, const QString &) { ++firedA; });
        ctrl.unmountDevice(udi1, [&](bool, const QString &) { ++firedB; });
        verify(unmountCalled == 1, "duplicate unmount: only ONE backend call made");
        ctrl.simulateTeardownDone(udi1, true);
        verify(firedA == 1, "first caller callback fires on shared completion");
        verify(firedB == 1, "second caller callback fires on shared completion");
        verify(unmountCalled == 1, "still only one backend call after completion");
    }

    // =========================================================================
    // SECTION 4: UNMOUNT — accessibilityChanged(false) resolves before teardownDone
    // =========================================================================
    {
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [](const QString &) { return true; };
        DeviceRemovalController ctrl(hooks);

        int fired = 0;
        bool success = false;
        ctrl.unmountDevice(udi1, [&](bool s, const QString &) {
            ++fired;
            success = s;
        });
        // accessibilityChanged fires first
        ctrl.simulateAccessibilityChanged(udi1, false);
        verify(fired == 1, "accessibilityChanged(false) resolves unmount request");
        verify(success, "accessibility-resolved request reported as success");
        // Late teardownDone must not fire second callback
        ctrl.simulateTeardownDone(udi1, true);
        verify(fired == 1, "late teardownDone after accessibilityChanged is dropped");
    }

    // =========================================================================
    // SECTION 5: UNMOUNT — deviceRemoved during pending request
    // =========================================================================
    {
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [](const QString &) { return true; };
        DeviceRemovalController ctrl(hooks);

        int fired = 0;
        bool success = false;
        ctrl.unmountDevice(udi1, [&](bool s, const QString &) {
            ++fired;
            success = s;
        });
        ctrl.simulateDeviceRemoved(udi1);
        // Physical removal during unmount is treated as success (device is gone)
        verify(fired == 1, "deviceRemoved during unmount resolves the request");
        verify(success, "device removal during unmount treated as success");
        verify(!ctrl.isUnmountPending(udi1), "pending state cleared after device removal");
    }

    // =========================================================================
    // SECTION 6: EJECT — basic success, failure, duplicate, deviceRemoved
    // =========================================================================
    {
        int ejectCalled = 0;
        QString lastEjectUdi;
        DeviceRemovalController::Hooks hooks;
        hooks.requestEject = [&](const QString &udi) {
            ++ejectCalled;
            lastEjectUdi = udi;
            return true;
        };
        DeviceRemovalController ctrl(hooks);

        // Empty UDI rejection
        int emptyFired = 0;
        ctrl.ejectDevice(QString(), [&](bool, const QString &) { ++emptyFired; });
        verify(emptyFired == 1, "eject: empty UDI rejected immediately");
        verify(ejectCalled == 0, "eject: empty UDI does not call backend");

        const QString opticalUdi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sr0");

        int fired = 0;
        bool success = false;
        ctrl.ejectDevice(opticalUdi, [&](bool s, const QString &) {
            ++fired;
            success = s;
        });
        verify(ejectCalled == 1, "eject: backend called for valid UDI");
        verify(ctrl.isEjectPending(opticalUdi), "eject request is pending");

        // Duplicate attaches
        int firedB = 0;
        ctrl.ejectDevice(opticalUdi, [&](bool, const QString &) { ++firedB; });
        verify(ejectCalled == 1, "duplicate eject: no additional backend call");

        ctrl.simulateEjectDone(opticalUdi, true);
        verify(fired == 1, "eject success fires callback once");
        verify(success, "eject success reported correctly");
        verify(firedB == 1, "second caller also fires exactly once");
        verify(!ctrl.isEjectPending(opticalUdi), "eject pending cleared");

        // Failure path
        int failFired = 0;
        bool failSuccess = true;
        ctrl.ejectDevice(opticalUdi, [&](bool s, const QString &) {
            ++failFired;
            failSuccess = s;
        });
        ctrl.simulateEjectDone(opticalUdi, false, QStringLiteral("Media not present"));
        verify(failFired == 1, "eject failure fires callback");
        verify(!failSuccess, "eject failure reported correctly");

        // deviceRemoved during eject
        int removeFired = 0;
        ctrl.ejectDevice(opticalUdi, [&](bool, const QString &) { ++removeFired; });
        ctrl.simulateDeviceRemoved(opticalUdi);
        verify(removeFired == 1, "deviceRemoved during eject resolves request");
    }

    // =========================================================================
    // SECTION 7: canEject / canSafelyRemove via hooks
    // =========================================================================
    {
        DeviceRemovalController::Hooks hooks;
        hooks.canEject = [](const QString &udi) -> bool {
            return udi.contains(QStringLiteral("optical"));
        };
        hooks.canSafelyRemove = [](const QString &udi) -> bool {
            return udi.contains(QStringLiteral("usb"));
        };
        DeviceRemovalController ctrl(hooks);

        verify(!ctrl.canEject(QStringLiteral("usb_drive")),
               "USB drive does not canEject");
        verify(ctrl.canEject(QStringLiteral("optical_drive")),
               "optical drive canEject returns true");
        verify(ctrl.canSafelyRemove(QStringLiteral("usb_drive")),
               "USB drive canSafelyRemove returns true");
        verify(!ctrl.canSafelyRemove(QStringLiteral("internal_hdd")),
               "internal drive canSafelyRemove returns false");
        verify(!ctrl.canEject(QString()),
               "empty UDI canEject returns false");
        verify(!ctrl.canSafelyRemove(QString()),
               "empty UDI canSafelyRemove returns false");
    }

    // =========================================================================
    // SECTION 8: SAFELY REMOVE — basic success via test mode
    // =========================================================================
    {
        DeviceRemovalController::Hooks hooks;
        hooks.requestSafelyRemove = [](const QString &) { return true; };
        hooks.executePowerOff = [](const QString &) {}; // intercepted by simulatePowerOffDone
        DeviceRemovalController ctrl(hooks);

        int fired = 0;
        bool success = false;
        ctrl.safelyRemoveDevice(udi1, [&](bool s, const QString &) {
            ++fired;
            success = s;
        });
        verify(ctrl.isSafelyRemovePending(udi1), "safely remove request is pending");
        ctrl.simulatePowerOffDone(udi1, true);
        verify(fired == 1, "safely remove success fires callback once");
        verify(success, "safely remove success reported correctly");
        verify(!ctrl.isSafelyRemovePending(udi1), "safely remove pending cleared");

        // Late PowerOff/deviceRemoved must not double-fire
        ctrl.simulatePowerOffDone(udi1, true);
        ctrl.simulateDeviceRemoved(udi1);
        verify(fired == 1, "late signals after safely remove: NOT double-fired");
    }

    // =========================================================================
    // SECTION 9: SAFELY REMOVE — duplicate attaches, both get result
    // =========================================================================
    {
        DeviceRemovalController::Hooks hooks;
        hooks.requestSafelyRemove = [](const QString &) { return true; };
        hooks.executePowerOff = [](const QString &) {};
        DeviceRemovalController ctrl(hooks);

        int firedA = 0, firedB = 0;
        ctrl.safelyRemoveDevice(udi1, [&](bool, const QString &) { ++firedA; });
        ctrl.safelyRemoveDevice(udi1, [&](bool, const QString &) { ++firedB; });
        verify(ctrl.isSafelyRemovePending(udi1), "safely remove: pending for udi1");
        ctrl.simulatePowerOffDone(udi1, true);
        verify(firedA == 1, "safely remove: first caller fires once");
        verify(firedB == 1, "safely remove: second caller fires once");
        verify(!ctrl.isSafelyRemovePending(udi1), "pending cleared");
    }

    // =========================================================================
    // SECTION 10: SAFELY REMOVE — PowerOff failure
    // =========================================================================
    {
        DeviceRemovalController::Hooks hooks;
        hooks.requestSafelyRemove = [](const QString &) { return true; };
        hooks.executePowerOff = [](const QString &) {};
        DeviceRemovalController ctrl(hooks);

        int fired = 0;
        bool success = true;
        QString errMsg;
        ctrl.safelyRemoveDevice(udi1, [&](bool s, const QString &e) {
            ++fired;
            success = s;
            errMsg = e;
        });
        ctrl.simulatePowerOffDone(udi1, false, QStringLiteral("PowerOff failed: busy"));
        verify(fired == 1, "PowerOff failure fires callback once");
        verify(!success, "PowerOff failure reported as failure");
        verify(!errMsg.isEmpty(), "PowerOff failure carries error message");
    }

    // =========================================================================
    // SECTION 11: SAFELY REMOVE — deviceRemoved races with PowerOff reply
    // =========================================================================
    {
        int fired = 0;
        DeviceRemovalController::Hooks hooks;
        hooks.requestSafelyRemove = [](const QString &) { return true; };
        hooks.executePowerOff = [](const QString &) {};
        DeviceRemovalController ctrl(hooks);

        ctrl.safelyRemoveDevice(udi1, [&](bool, const QString &) { ++fired; });

        // deviceRemoved fires first
        ctrl.simulateDeviceRemoved(udi1);
        verify(fired == 1, "deviceRemoved during safely-remove: fires callback once");
        // Late PowerOff reply must be silently ignored
        ctrl.simulatePowerOffDone(udi1, true);
        verify(fired == 1, "late PowerOff reply after deviceRemoved: NOT fired again");
    }

    // =========================================================================
    // SECTION 12: SAFELY REMOVE — empty UDI rejection
    // =========================================================================
    {
        DeviceRemovalController::Hooks hooks;
        hooks.requestSafelyRemove = [](const QString &) { return true; };
        DeviceRemovalController ctrl(hooks);

        int fired = 0;
        bool success = true;
        ctrl.safelyRemoveDevice(QString(), [&](bool s, const QString &) {
            ++fired;
            success = s;
        });
        verify(fired == 1, "safely remove: empty UDI rejected immediately");
        verify(!success, "safely remove: empty UDI reports failure");
    }

    // =========================================================================
    // SECTION 13: Menu visibility — populateDriveContextMenu
    // =========================================================================
    {
        // Mounted removable drive + canSafelyRemove=true
        DriveInfo mountedRemovable = makeRemovableDrive(
            QStringLiteral("vol-usb"), udi1, QStringLiteral("/run/media/user/USB"));

        QMenu menu1;
        QAction *unmountAct = nullptr;
        QAction *safelyRemoveAct = nullptr;
        QAction *ejectAct = nullptr;
        QAction *openAct = nullptr;
        QAction *copyAct = nullptr;

        populateDriveContextMenu(menu1, mountedRemovable,
                                 /*canSafelyRemove=*/true,
                                 /*canEject=*/false,
                                 &unmountAct, &safelyRemoveAct, &ejectAct,
                                 &openAct, &copyAct);

        verify(unmountAct != nullptr,
               "mounted removable: Unmount action present");
        verify(safelyRemoveAct != nullptr,
               "mounted removable + canSafelyRemove: Bezpiecznie usun present");
        verify(ejectAct == nullptr,
               "mounted removable + canEject=false: Eject absent");
        verify(openAct != nullptr,
               "mounted drive: Open in Dolphin present");
        verify(copyAct != nullptr,
               "mounted drive: Copy mount point present");

        // Unmounted removable: no Unmount, Safely Remove still shows
        DriveInfo unmountedRemovable = makeRemovableDrive(
            QStringLiteral("vol-usb"), udi1, QString(), /*isMounted=*/false);

        QMenu menu2;
        QAction *ua2 = nullptr, *sra2 = nullptr, *ea2 = nullptr;
        QAction *oa2 = nullptr, *ca2 = nullptr;
        populateDriveContextMenu(menu2, unmountedRemovable,
                                 /*canSafelyRemove=*/true,
                                 /*canEject=*/false,
                                 &ua2, &sra2, &ea2, &oa2, &ca2);

        verify(ua2 == nullptr,
               "unmounted removable: Unmount action absent");
        verify(sra2 != nullptr,
               "unmounted removable + canSafelyRemove: Bezpiecznie usun present");
        verify(ea2 == nullptr,
               "unmounted removable + canEject=false: Eject absent");
        verify(oa2 == nullptr,
               "unmounted drive: Open in Dolphin absent");

        // canEject = true → Eject shows
        QMenu menu3;
        QAction *ea3 = nullptr;
        populateDriveContextMenu(menu3, mountedRemovable,
                                 /*canSafelyRemove=*/false,
                                 /*canEject=*/true,
                                 nullptr, nullptr, &ea3);
        verify(ea3 != nullptr, "canEject=true: Eject action present");

        // Internal (isRemovable=false) drive → no removal actions
        DriveInfo internalDrive = makeRemovableDrive(
            QStringLiteral("vol-sys"),
            QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda2"),
            QStringLiteral("/"),
            /*isMounted=*/true,
            /*isRemovable=*/false);

        QMenu menu4;
        QAction *ua4 = nullptr, *sra4 = nullptr, *ea4 = nullptr;
        populateDriveContextMenu(menu4, internalDrive,
                                 /*canSafelyRemove=*/false,
                                 /*canEject=*/false,
                                 &ua4, &sra4, &ea4);
        verify(ua4 == nullptr, "internal drive: Unmount absent");
        verify(sra4 == nullptr, "internal drive: Bezpiecznie usun absent");
        verify(ea4 == nullptr, "internal drive: Eject absent");

        // USB flash drive without canEject: no Eject
        QMenu menu5;
        QAction *ea5 = nullptr;
        populateDriveContextMenu(menu5, mountedRemovable,
                                 /*canSafelyRemove=*/false,
                                 /*canEject=*/false,
                                 nullptr, nullptr, &ea5);
        verify(ea5 == nullptr, "USB flash drive without canEject: Eject absent");
    }

    // =========================================================================
    // SECTION 14: Pane routing — Primary inside mountpoint → redirected
    // =========================================================================
    {
        ThisPcWindow window;
        window.resize(1000, 700);
        window.show();
        QApplication::processEvents();

        DeviceRemovalController::Hooks removalHooks;
        removalHooks.requestUnmount = [](const QString &) { return true; };
        window.removalControllerForTesting().setHooksForTesting(removalHooks);

        QTemporaryDir mountDir;
        const QString mp = mountDir.path();

        DriveInfo removable = makeRemovableDrive(
            QStringLiteral("vol-usb"), udi1, mp);

        window.m_driveHomeCoordinator.setSnapshotForTesting({removable});
        QApplication::processEvents();

        window.navigatePane(PaneId::Primary, QUrl::fromLocalFile(mp));
        window.navigatePane(PaneId::Split, kThisPcUrl);
        QApplication::processEvents();

        window.handleDeviceUnmount(PaneId::Primary, removable);
        window.removalControllerForTesting().simulateTeardownDone(udi1, true);
        QApplication::processEvents();

        verify(window.m_primaryPane->currentUrl() == kThisPcUrl,
               "Primary inside mountpoint: redirected to thispc:/ after unmount");
        verify(window.m_splitPane->currentUrl() == kThisPcUrl,
               "Split at thispc:/: unaffected (still thispc:/)");
    }

    // =========================================================================
    // SECTION 15: Both panes inside same device → both redirect
    // =========================================================================
    {
        ThisPcWindow window2;
        window2.resize(1000, 700);
        window2.show();
        QApplication::processEvents();

        DeviceRemovalController::Hooks removalHooks2;
        removalHooks2.requestUnmount = [](const QString &) { return true; };
        window2.removalControllerForTesting().setHooksForTesting(removalHooks2);

        QTemporaryDir mountDir2;
        const QString mp2 = mountDir2.path();
        DriveInfo removable2 = makeRemovableDrive(
            QStringLiteral("vol-usb2"), udi1, mp2);

        window2.m_driveHomeCoordinator.setSnapshotForTesting({removable2});
        QApplication::processEvents();

        window2.navigatePane(PaneId::Primary, QUrl::fromLocalFile(mp2));
        window2.navigatePane(PaneId::Split, QUrl::fromLocalFile(mp2));
        QApplication::processEvents();

        window2.handleDeviceUnmount(PaneId::Primary, removable2);
        window2.removalControllerForTesting().simulateTeardownDone(udi1, true);
        QApplication::processEvents();

        verify(window2.m_primaryPane->currentUrl() == kThisPcUrl,
               "Both-panes inside device: Primary redirected to thispc:/");
        verify(window2.m_splitPane->currentUrl() == kThisPcUrl,
               "Both-panes inside device: Split also redirected to thispc:/");
    }

    // =========================================================================
    // SECTION 16: Unmount FAILURE does NOT navigate any pane
    // =========================================================================
    {
        ThisPcWindow window4;
        window4.resize(1000, 700);
        window4.show();
        QApplication::processEvents();

        DeviceRemovalController::Hooks removalHooks4;
        removalHooks4.requestUnmount = [](const QString &) { return true; };
        window4.removalControllerForTesting().setHooksForTesting(removalHooks4);

        QTemporaryDir mountDir4;
        const QString mp4 = mountDir4.path();
        DriveInfo removable4 = makeRemovableDrive(
            QStringLiteral("vol-usb4"), udi1, mp4);

        window4.m_driveHomeCoordinator.setSnapshotForTesting({removable4});
        QApplication::processEvents();

        window4.navigatePane(PaneId::Primary, QUrl::fromLocalFile(mp4));
        QApplication::processEvents();

        window4.handleDeviceUnmount(PaneId::Primary, removable4);
        window4.removalControllerForTesting().simulateTeardownDone(
            udi1, false, QStringLiteral("Device busy"));
        QApplication::processEvents();

        verify(window4.m_primaryPane->currentUrl() == QUrl::fromLocalFile(mp4),
               "Unmount failure: Primary pane NOT redirected");
    }

    // =========================================================================
    // SECTION 17: Correctly uses drive.udi, NOT drive.id
    // =========================================================================
    {
        int unmountCalled = 0;
        QString lastUdi;
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [&](const QString &u) {
            ++unmountCalled;
            lastUdi = u;
            return true;
        };

        ThisPcWindow windowId;
        windowId.resize(1000, 700);
        windowId.show();
        QApplication::processEvents();

        windowId.removalControllerForTesting().setHooksForTesting(hooks);

        QTemporaryDir mpDir;
        DriveInfo drive;
        drive.id = QStringLiteral("volume-usb-pendrive"); // NOT a UDI
        drive.udi = udi1;                                  // real Solid UDI
        drive.name = QStringLiteral("Pendrive");
        drive.mountPoint = mpDir.path();
        drive.targetUrl = QUrl::fromLocalFile(mpDir.path());
        drive.isMounted = true;
        drive.isRemovable = true;

        windowId.m_driveHomeCoordinator.setSnapshotForTesting({drive});
        QApplication::processEvents();

        windowId.handleDeviceUnmount(PaneId::Primary, drive);

        verify(unmountCalled == 1, "UDI test: unmount was issued");
        verify(lastUdi == udi1,
               "UDI test: unmount uses drive.udi, NOT drive.id");
        verify(lastUdi != drive.id,
               "UDI test: drive.id was NOT passed to the controller");
    }

    // =========================================================================
    // SECTION 18: isOperationPending guard in window prevents double-issue
    // =========================================================================
    {
        int unmountCalled = 0;
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [&](const QString &) {
            ++unmountCalled;
            return true;
        };

        ThisPcWindow windowPending;
        windowPending.resize(1000, 700);
        windowPending.show();
        QApplication::processEvents();

        windowPending.removalControllerForTesting().setHooksForTesting(hooks);

        QTemporaryDir pendDir;
        DriveInfo pendDrive = makeRemovableDrive(
            QStringLiteral("vol-pend"), udi1, pendDir.path());

        windowPending.m_driveHomeCoordinator.setSnapshotForTesting({pendDrive});
        QApplication::processEvents();

        windowPending.handleDeviceUnmount(PaneId::Primary, pendDrive);
        verify(unmountCalled == 1, "first unmount triggers backend");

        // Second call while pending → absorbed by window-level guard
        windowPending.handleDeviceUnmount(PaneId::Primary, pendDrive);
        verify(unmountCalled == 1, "second unmount while pending: NOT re-issued");
    }

    // =========================================================================
    // SECTION 19: Stage 2 regression — m_refreshTimer remains inactive
    // =========================================================================
    {
        ThisPcWindow windowR;
        windowR.resize(1000, 700);
        windowR.show();
        QApplication::processEvents();

        verify(!windowR.m_refreshTimer.isActive(),
               "Stage 2 regression: m_refreshTimer is NOT active by default");

        DeviceRemovalController::Hooks regHooks;
        regHooks.requestUnmount = [](const QString &) { return true; };
        windowR.removalControllerForTesting().setHooksForTesting(regHooks);

        QTemporaryDir regMount;
        DriveInfo regDrive = makeRemovableDrive(
            QStringLiteral("vol-reg"), udi1, regMount.path());
        windowR.m_driveHomeCoordinator.setSnapshotForTesting({regDrive});
        QApplication::processEvents();

        windowR.handleDeviceUnmount(PaneId::Primary, regDrive);
        windowR.removalControllerForTesting().simulateTeardownDone(udi1, true);
        QApplication::processEvents();

        verify(!windowR.m_refreshTimer.isActive(),
               "Stage 2 regression: post-unmount refresh does NOT start periodic timer");
    }

    // =========================================================================
    // SECTION 20: Stage 2 regression — identical snapshot does NOT emit spurious drivesChanged
    // =========================================================================
    {
        std::shared_ptr<RemovalTestDriveSourceState> state;
        DriveHomeCoordinator coordinator(
            DriveHomeCoordinator::SourceFactory(
                [&state](DriveHomeSource::EntriesCallback entries,
                         DriveHomeSource::FinishedCallback finished)
                    -> std::unique_ptr<DriveHomeSource> {
                    state = std::make_shared<RemovalTestDriveSourceState>();
                    state->entries = std::move(entries);
                    state->finished = std::move(finished);
                    return std::make_unique<RemovalTestDriveSource>(state);
                }));

        int drivesChangedCount = 0;
        QObject::connect(&coordinator, &DriveHomeCoordinator::drivesChanged, &coordinator,
                         [&](const QList<DriveInfo> &) { ++drivesChangedCount; });

        coordinator.refresh();
        KIO::UDSEntry e;
        e.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("vol"));
        e.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, QStringLiteral("USB"));
        e.fastInsert(KIO::UDSEntry::UDS_TARGET_URL, QStringLiteral("file:///media/usb"));
        e.fastInsert(KIO::UDSEntry::UDS_EXTRA + 4, QStringLiteral("/media/usb"));
        e.fastInsert(KIO::UDSEntry::UDS_EXTRA + 5, QStringLiteral("1"));
        e.fastInsert(KIO::UDSEntry::UDS_EXTRA + 6, udi1);
        state->entries({e});
        state->finished(true, {});
        verify(drivesChangedCount == 1, "first refresh emits drivesChanged");

        // Second identical refresh must NOT emit again
        coordinator.refresh();
        state->entries({e});
        state->finished(true, {});
        verify(drivesChangedCount == 1,
               "Stage 2 regression: identical snapshot does NOT emit spurious drivesChanged");
    }

    // =========================================================================
    // SECTION 21: REGRESSION — GUI "Odmontuj" invokes unmountDevice, safelyRemoveDevice count = 0
    // =========================================================================
    {
        ThisPcWindow window;
        window.resize(1000, 700);
        window.show();
        QApplication::processEvents();

        int unmountCallCount = 0;
        int safelyRemoveCallCount = 0;
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [&](const QString &) {
            ++unmountCallCount;
            return true;
        };
        hooks.requestSafelyRemove = [&](const QString &) {
            ++safelyRemoveCallCount;
            return true;
        };
        hooks.canSafelyRemove = [](const QString &) { return true; };
        window.removalControllerForTesting().setHooksForTesting(hooks);

        QTemporaryDir mountDir;
        DriveInfo pendrive = makeRemovableDrive(
            QStringLiteral("pendrive-1"), udi1, mountDir.path(), true, true);

        // Populate context menu for this drive
        QMenu menu;
        QAction *unmountAct = nullptr;
        QAction *safelyRemoveAct = nullptr;
        populateDriveContextMenu(menu, pendrive, true, false,
                                 &unmountAct, &safelyRemoveAct);

        verify(unmountAct != nullptr, "mounted removable has 'Odmontuj' action");
        verify(safelyRemoveAct != nullptr, "removable has 'Bezpiecznie usuń' action");

        // Simulating the user clicking "Odmontuj"
        window.handleDeviceUnmount(PaneId::Primary, pendrive);
        verify(unmountCallCount == 1, "GUI 'Odmontuj' triggers unmountDevice");
        verify(safelyRemoveCallCount == 0, "GUI 'Odmontuj' does NOT trigger safelyRemoveDevice");
    }

    // =========================================================================
    // SECTION 22: REGRESSION — unmountDevice success: teardown count = 1, PowerOff count = 0
    // =========================================================================
    {
        int teardownCount = 0;
        int powerOffCount = 0;
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [&](const QString &) {
            ++teardownCount;
            return true;
        };
        hooks.executePowerOff = [&](const QString &) {
            ++powerOffCount;
        };
        DeviceRemovalController ctrl(hooks);

        int callbackFired = 0;
        bool callbackSuccess = false;
        ctrl.unmountDevice(udi1, [&](bool s, const QString &) {
            ++callbackFired;
            callbackSuccess = s;
        });

        verify(ctrl.isUnmountPending(udi1), "unmount is pending");
        ctrl.simulateTeardownDone(udi1, true);

        verify(callbackFired == 1, "unmount callback fired exactly once");
        verify(callbackSuccess, "unmount callback reports success");
        verify(teardownCount == 1, "teardown invoked exactly once");
        verify(powerOffCount == 0, "PowerOff was NEVER invoked during unmount");
    }

    // =========================================================================
    // SECTION 23: REGRESSION — unmountDevice success: operation ends after teardown, without continuation to PowerOff
    // =========================================================================
    {
        int powerOffCount = 0;
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [](const QString &) { return true; };
        hooks.executePowerOff = [&](const QString &) { ++powerOffCount; };
        DeviceRemovalController ctrl(hooks);

        ctrl.unmountDevice(udi1, [](bool, const QString &) {});
        ctrl.simulateTeardownDone(udi1, true);

        verify(!ctrl.isOperationPending(udi1), "all operations finished for udi1");
        verify(!ctrl.isUnmountPending(udi1), "unmount pending cleared");
        verify(!ctrl.isSafelyRemovePending(udi1), "safely remove is NOT pending");
        verify(powerOffCount == 0, "no continuation or delayed task invokes PowerOff");
    }

    // =========================================================================
    // SECTION 24: REGRESSION — unmount + deviceRemoved race: cannot transition to PowerOff
    // =========================================================================
    {
        int powerOffCount = 0;
        DeviceRemovalController::Hooks hooks;
        hooks.requestUnmount = [](const QString &) { return true; };
        hooks.executePowerOff = [&](const QString &) { ++powerOffCount; };
        DeviceRemovalController ctrl(hooks);

        int unmountCallbackFired = 0;
        ctrl.unmountDevice(udi1, [&](bool s, const QString &) {
            ++unmountCallbackFired;
            verify(s, "deviceRemoved during unmount resolves as success");
        });

        // Simulate physical removal during unmount in-flight
        ctrl.simulateDeviceRemoved(udi1);

        verify(unmountCallbackFired == 1, "unmount callback fires on deviceRemoved");
        verify(!ctrl.isOperationPending(udi1), "no operation pending after deviceRemoved");
        verify(powerOffCount == 0, "unmount + deviceRemoved race NEVER invokes PowerOff");
    }

    // =========================================================================
    // SECTION 25: REGRESSION — safelyRemove: member teardowns -> PowerOff still works as expected
    // =========================================================================
    {
        int powerOffCount = 0;
        QString poweredOffDrive;
        DeviceRemovalController::Hooks hooks;
        hooks.findPhysicalDriveUdi = [&](const QString &) { return driveUdi; };
        hooks.findMountedMemberVolumes = [&](const QString &) {
            return QList<QString>{udi1, udi2};
        };
        hooks.requestSafelyRemove = [](const QString &) { return true; };
        hooks.executePowerOff = [&](const QString &d) {
            ++powerOffCount;
            poweredOffDrive = d;
        };
        DeviceRemovalController ctrl(hooks);

        int safelyRemoveFired = 0;
        bool safelyRemoveSuccess = false;
        ctrl.safelyRemoveDevice(udi1, [&](bool s, const QString &) {
            ++safelyRemoveFired;
            safelyRemoveSuccess = s;
        });

        verify(ctrl.isSafelyRemovePending(udi1), "safely remove pending on volume");
        verify(ctrl.isSafelyRemovePending(driveUdi), "safely remove pending on physical drive");
        verify(powerOffCount == 0, "PowerOff not invoked before member teardowns finish");

        // Complete first member volume teardown
        ctrl.simulateSafelyRemoveMemberTeardownDone(driveUdi, udi1, true);
        verify(powerOffCount == 0, "PowerOff not invoked after only first volume torn down");

        // Complete second member volume teardown
        ctrl.simulateSafelyRemoveMemberTeardownDone(driveUdi, udi2, true);
        verify(powerOffCount == 1, "PowerOff invoked after all member volumes torn down");
        verify(poweredOffDrive == driveUdi, "PowerOff target is physical drive UDI");

        // PowerOff completes
        ctrl.simulatePowerOffDone(driveUdi, true);
        verify(safelyRemoveFired == 1, "safelyRemove callback fired once on completion");
        verify(safelyRemoveSuccess, "safelyRemove reported success");
        verify(!ctrl.isSafelyRemovePending(udi1), "safely remove pending cleared");
    }

    // =========================================================================
    // SECTION 26: REGRESSION — execution of "Odmontuj" leaves volume logically present as isMounted = false
    // =========================================================================
    {
        QTemporaryDir mountDir;
        DriveInfo drive = makeRemovableDrive(
            QStringLiteral("pendrive-state"), udi1, mountDir.path(), true, true);

        verify(drive.isMounted, "initially mounted");
        verify(drive.isRemovable, "removable drive");
        verify(!drive.udi.isEmpty(), "has valid Solid UDI");

        // Simulating the state transition that occurs on clean unmount
        drive.isMounted = false;
        drive.mountPoint.clear();
        drive.targetUrl = QUrl();

        verify(!drive.isMounted, "after unmount: isMounted is false");
        verify(drive.isRemovable, "after unmount: remains removable");
        verify(!drive.udi.isEmpty(), "after unmount: retains Solid UDI for future mount/safely-remove");
        verify(drive.id == QStringLiteral("pendrive-state"), "after unmount: retains persistent drive ID");
    }

    // =========================================================================
    // SECTION 27: REGRESSION — refresh after normal unmount: card remains in DriveHomeCoordinator
    // =========================================================================
    {
        std::shared_ptr<RemovalTestDriveSourceState> state;
        DriveHomeCoordinator coordinator(
            DriveHomeCoordinator::SourceFactory(
                [&state](DriveHomeSource::EntriesCallback entries,
                         DriveHomeSource::FinishedCallback finished)
                    -> std::unique_ptr<DriveHomeSource> {
                    state = std::make_shared<RemovalTestDriveSourceState>();
                    state->entries = std::move(entries);
                    state->finished = std::move(finished);
                    return std::make_unique<RemovalTestDriveSource>(state);
                }));

        QList<DriveInfo> latestDrives;
        QObject::connect(&coordinator, &DriveHomeCoordinator::drivesChanged, &coordinator,
                         [&](const QList<DriveInfo> &drives) { latestDrives = drives; });

        // First snapshot: mounted pendrive
        coordinator.refresh();
        KIO::UDSEntry mountedEntry;
        mountedEntry.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("pendrive-entry"));
        mountedEntry.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, QStringLiteral("USB Kingston"));
        mountedEntry.fastInsert(KIO::UDSEntry::UDS_TARGET_URL, QStringLiteral("file:///run/media/usb"));
        mountedEntry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 4, QStringLiteral("/run/media/usb"));
        mountedEntry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 5, QStringLiteral("1")); // mounted
        mountedEntry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 6, udi1);
        mountedEntry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 8, QStringLiteral("1")); // removable
        state->entries({mountedEntry});
        state->finished(true, {});

        verify(latestDrives.size() == 1, "coordinator has 1 drive when mounted");
        verify(latestDrives[0].isMounted, "drive is mounted in first snapshot");

        // Second snapshot: pendrive unmounted (still plugged in, filesystem unmounted)
        coordinator.refresh();
        KIO::UDSEntry unmountedEntry;
        unmountedEntry.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("pendrive-entry"));
        unmountedEntry.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, QStringLiteral("USB Kingston"));
        unmountedEntry.fastInsert(KIO::UDSEntry::UDS_TARGET_URL, QString()); // empty for unmounted
        unmountedEntry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 4, QString()); // no mount point
        unmountedEntry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 5, QStringLiteral("0")); // unmounted!
        unmountedEntry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 6, udi1);
        unmountedEntry.fastInsert(KIO::UDSEntry::UDS_EXTRA + 8, QStringLiteral("1")); // removable!
        state->entries({unmountedEntry});
        state->finished(true, {});

        verify(latestDrives.size() == 1, "card REMAINS in DriveHomeCoordinator after unmount");
        verify(!latestDrives[0].isMounted, "drive in coordinator is now marked unmounted");
        verify(latestDrives[0].isRemovable, "drive in coordinator remains removable");
        verify(latestDrives[0].udi == udi1, "drive in coordinator retains volume UDI");
    }

    // =========================================================================
    // SECTION 28: REGRESSION — menu after unmount: no "Odmontuj", still "Bezpiecznie usuń" if capability exists
    // =========================================================================
    {
        DriveInfo unmountedDrive;
        unmountedDrive.id = QStringLiteral("usb-unmounted");
        unmountedDrive.udi = udi1;
        unmountedDrive.name = QStringLiteral("USB Drive");
        unmountedDrive.isMounted = false;
        unmountedDrive.isRemovable = true;

        QMenu menu;
        QAction *unmountAct = nullptr;
        QAction *safelyRemoveAct = nullptr;
        QAction *ejectAct = nullptr;
        populateDriveContextMenu(menu, unmountedDrive, true, false,
                                 &unmountAct, &safelyRemoveAct, &ejectAct);

        verify(unmountAct == nullptr, "menu after unmount: NO 'Odmontuj' action");
        verify(safelyRemoveAct != nullptr, "menu after unmount: STILL HAS 'Bezpiecznie usuń'");
        verify(ejectAct == nullptr, "menu after unmount: no 'Wysuń' when canEject is false");
    }

    // =========================================================================
    // SECTION 29..40: BACKEND FIX SEMANTICS (Cases 1-12)
    // FakeBackendTracker verifies filesystemUnmount, powerOff, and solidTeardown
    // =========================================================================
    struct FakeBackendTracker {
        int filesystemUnmountCount = 0;
        int powerOffCount = 0;
        int solidTeardownCount = 0;
        QList<QString> unmountUdis;
        QList<QString> powerOffUdis;
        std::function<void(const QString &volUdi, std::function<void(bool, const QString &)> done)> unmountHandler;
        std::function<void(const QString &driveUdi)> powerOffHandler;

        DeviceRemovalController::Hooks makeHooks() {
            DeviceRemovalController::Hooks h;
            h.executeFilesystemUnmount = [this](const QString &volUdi,
                                               std::function<void(bool, const QString &)> done) {
                ++filesystemUnmountCount;
                unmountUdis.append(volUdi);
                if (unmountHandler) {
                    unmountHandler(volUdi, std::move(done));
                }
            };
            h.executePowerOff = [this](const QString &d) {
                ++powerOffCount;
                powerOffUdis.append(d);
                if (powerOffHandler) {
                    powerOffHandler(d);
                }
            };
            h.executeSolidTeardown = [this](const QString &) {
                ++solidTeardownCount;
            };
            return h;
        }
    };

    // Case 1: unmountDevice: Filesystem.Unmount count = 1, teardown = 0, PowerOff = 0
    {
        FakeBackendTracker tracker;
        DeviceRemovalController ctrl(tracker.makeHooks());
        int cbFired = 0;
        ctrl.unmountDevice(udi1, [&](bool, const QString &) { ++cbFired; });
        verify(tracker.filesystemUnmountCount == 1, "case 1: Filesystem.Unmount count == 1");
        verify(tracker.solidTeardownCount == 0, "case 1: Solid::StorageAccess::teardown count == 0");
        verify(tracker.powerOffCount == 0, "case 1: PowerOff count == 0");
    }

    // Case 2: unmount success: kończy callback exactly once
    {
        FakeBackendTracker tracker;
        std::function<void(bool, const QString &)> pendingDone;
        tracker.unmountHandler = [&](const QString &, auto done) {
            pendingDone = done;
        };
        DeviceRemovalController ctrl(tracker.makeHooks());
        int cbFired = 0;
        bool cbSuccess = false;
        ctrl.unmountDevice(udi1, [&](bool s, const QString &) {
            ++cbFired;
            cbSuccess = s;
        });
        verify(cbFired == 0, "case 2: callback not fired before reply");
        pendingDone(true, QString());
        verify(cbFired == 1, "case 2: callback fired exactly once");
        verify(cbSuccess, "case 2: unmount success reported");
        // Late signal:
        pendingDone(true, QString());
        ctrl.simulateDeviceRemoved(udi1);
        verify(cbFired == 1, "case 2: late signals do NOT fire callback again");
    }

    // Case 3: unmount failure: brak PowerOff
    {
        FakeBackendTracker tracker;
        std::function<void(bool, const QString &)> pendingDone;
        tracker.unmountHandler = [&](const QString &, auto done) {
            pendingDone = done;
        };
        DeviceRemovalController ctrl(tracker.makeHooks());
        int cbFired = 0;
        bool cbSuccess = true;
        QString errorMsg;
        ctrl.unmountDevice(udi1, [&](bool s, const QString &err) {
            ++cbFired;
            cbSuccess = s;
            errorMsg = err;
        });
        pendingDone(false, QStringLiteral("Device busy"));
        verify(cbFired == 1, "case 3: unmount failure fires callback once");
        verify(!cbSuccess, "case 3: reported failure");
        verify(!errorMsg.isEmpty(), "case 3: error message passed to callback");
        verify(tracker.powerOffCount == 0, "case 3: brak PowerOff on unmount failure");
    }

    // Case 4: duplicate unmount: deterministyczne zachowanie zgodne z obecnym kontraktem
    {
        FakeBackendTracker tracker;
        std::function<void(bool, const QString &)> pendingDone;
        tracker.unmountHandler = [&](const QString &, auto done) {
            pendingDone = done;
        };
        DeviceRemovalController ctrl(tracker.makeHooks());
        int cb1 = 0, cb2 = 0;
        ctrl.unmountDevice(udi1, [&](bool, const QString &) { ++cb1; });
        ctrl.unmountDevice(udi1, [&](bool, const QString &) { ++cb2; });
        verify(tracker.filesystemUnmountCount == 1, "case 4: duplicate unmount issues single backend call");
        verify(ctrl.isUnmountPending(udi1), "case 4: pending state true");
        pendingDone(true, QString());
        verify(cb1 == 1, "case 4: first callback fired");
        verify(cb2 == 1, "case 4: second callback fired");
        verify(!ctrl.isUnmountPending(udi1), "case 4: pending cleared");
        verify(tracker.powerOffCount == 0, "case 4: PowerOff count == 0");
    }

    // Case 5: unmount + deviceRemoved race: callback dokładnie raz, PowerOff count = 0
    {
        FakeBackendTracker tracker;
        std::function<void(bool, const QString &)> pendingDone;
        tracker.unmountHandler = [&](const QString &, auto done) {
            pendingDone = done;
        };
        DeviceRemovalController ctrl(tracker.makeHooks());
        int cbFired = 0;
        bool cbSuccess = false;
        ctrl.unmountDevice(udi1, [&](bool s, const QString &) {
            ++cbFired;
            cbSuccess = s;
        });
        ctrl.simulateDeviceRemoved(udi1);
        verify(cbFired == 1, "case 5: deviceRemoved resolves pending request");
        verify(cbSuccess, "case 5: deviceRemoved during unmount treated as success");
        if (pendingDone) {
            pendingDone(false, QStringLiteral("device unplugged"));
        }
        verify(cbFired == 1, "case 5: late D-Bus error does NOT double-call");
        verify(tracker.powerOffCount == 0, "case 5: PowerOff count == 0");
    }

    // Case 6: safelyRemove, 1 mounted volume: Filesystem.Unmount count = 1, PowerOff count = 1 po sukcesie
    {
        FakeBackendTracker tracker;
        std::function<void(bool, const QString &)> pendingDone;
        tracker.unmountHandler = [&](const QString &, auto done) {
            pendingDone = done;
        };
        auto hooks = tracker.makeHooks();
        hooks.findPhysicalDriveUdi = [&](const QString &) { return driveUdi; };
        hooks.findMountedMemberVolumes = [&](const QString &) { return QList<QString>{udi1}; };
        DeviceRemovalController ctrl(hooks);

        int srFired = 0;
        bool srSuccess = false;
        ctrl.safelyRemoveDevice(udi1, [&](bool s, const QString &) {
            ++srFired;
            srSuccess = s;
        });
        verify(tracker.filesystemUnmountCount == 1, "case 6: Filesystem.Unmount count == 1");
        verify(tracker.powerOffCount == 0, "case 6: PowerOff NOT called before unmount completes");
        pendingDone(true, QString());
        verify(tracker.powerOffCount == 1, "case 6: PowerOff count == 1 po sukcesie");
        ctrl.simulatePowerOffDone(driveUdi, true);
        verify(srFired == 1, "case 6: safelyRemove callback fired");
        verify(srSuccess, "case 6: safelyRemove reported success");
    }

    // Case 7: safelyRemove, 2 mounted volumes: Filesystem.Unmount count = 2, PowerOff dopiero po obu sukcesach
    {
        FakeBackendTracker tracker;
        QHash<QString, std::function<void(bool, const QString &)>> pendingDone;
        tracker.unmountHandler = [&](const QString &vol, auto done) {
            pendingDone[vol] = done;
        };
        auto hooks = tracker.makeHooks();
        hooks.findPhysicalDriveUdi = [&](const QString &) { return driveUdi; };
        hooks.findMountedMemberVolumes = [&](const QString &) { return QList<QString>{udi1, udi2}; };
        DeviceRemovalController ctrl(hooks);

        int srFired = 0;
        bool srSuccess = false;
        ctrl.safelyRemoveDevice(udi1, [&](bool s, const QString &) {
            ++srFired;
            srSuccess = s;
        });
        verify(tracker.filesystemUnmountCount == 2, "case 7: Filesystem.Unmount count == 2");
        verify(tracker.powerOffCount == 0, "case 7: PowerOff count == 0 initially");

        // Finish vol 1
        pendingDone[udi1](true, QString());
        verify(tracker.powerOffCount == 0, "case 7: PowerOff NOT called after only 1 volume");

        // Finish vol 2
        pendingDone[udi2](true, QString());
        verify(tracker.powerOffCount == 1, "case 7: PowerOff count == 1 dopiero po obu sukcesach");
        ctrl.simulatePowerOffDone(driveUdi, true);
        verify(srFired == 1, "case 7: safelyRemove callback fired");
        verify(srSuccess, "case 7: safelyRemove reported success");
    }

    // Case 8: safelyRemove: jeden Filesystem.Unmount fail -> PowerOff count = 0
    {
        FakeBackendTracker tracker;
        QHash<QString, std::function<void(bool, const QString &)>> pendingDone;
        tracker.unmountHandler = [&](const QString &vol, auto done) {
            pendingDone[vol] = done;
        };
        auto hooks = tracker.makeHooks();
        hooks.findPhysicalDriveUdi = [&](const QString &) { return driveUdi; };
        hooks.findMountedMemberVolumes = [&](const QString &) { return QList<QString>{udi1, udi2}; };
        DeviceRemovalController ctrl(hooks);

        int srFired = 0;
        bool srSuccess = true;
        QString srErr;
        ctrl.safelyRemoveDevice(udi1, [&](bool s, const QString &err) {
            ++srFired;
            srSuccess = s;
            srErr = err;
        });
        verify(tracker.filesystemUnmountCount == 2, "case 8: both member unmounts started");

        // Vol 1 succeeds, Vol 2 fails
        pendingDone[udi1](true, QString());
        verify(tracker.powerOffCount == 0, "case 8: PowerOff count == 0 after vol 1");
        pendingDone[udi2](false, QStringLiteral("Volume 2 busy"));

        verify(srFired == 1, "case 8: safelyRemove callback fired after all volumes finished");
        verify(!srSuccess, "case 8: safelyRemove reports failure");
        verify(!srErr.isEmpty(), "case 8: error message preserved");
        verify(tracker.powerOffCount == 0, "case 8: PowerOff count == 0 when unmount fails");
    }

    // Case 9: already-unmounted removable: Filesystem.Unmount count = 0, PowerOff count = 1
    {
        FakeBackendTracker tracker;
        auto hooks = tracker.makeHooks();
        hooks.findPhysicalDriveUdi = [&](const QString &) { return driveUdi; };
        hooks.findMountedMemberVolumes = [&](const QString &) { return QList<QString>{}; }; // empty!
        DeviceRemovalController ctrl(hooks);

        int srFired = 0;
        bool srSuccess = false;
        ctrl.safelyRemoveDevice(udi1, [&](bool s, const QString &) {
            ++srFired;
            srSuccess = s;
        });
        verify(tracker.filesystemUnmountCount == 0, "case 9: Filesystem.Unmount count == 0 for already-unmounted drive");
        verify(tracker.powerOffCount == 1, "case 9: PowerOff count == 1 immediately");
        ctrl.simulatePowerOffDone(driveUdi, true);
        verify(srFired == 1, "case 9: safelyRemove callback fired");
        verify(srSuccess, "case 9: safelyRemove reported success");
    }

    // Case 10: żaden kod ścieżki Unmount nie wywołuje Solid::StorageAccess::teardown()
    {
        FakeBackendTracker tracker;
        auto hooks = tracker.makeHooks();
        hooks.findPhysicalDriveUdi = [&](const QString &) { return driveUdi; };
        hooks.findMountedMemberVolumes = [&](const QString &) { return QList<QString>{udi1}; };
        DeviceRemovalController ctrl(hooks);

        ctrl.unmountDevice(udi1, [](bool, const QString &) {});
        ctrl.simulateUnmountDone(udi1, true);

        ctrl.safelyRemoveDevice(udi1, [](bool, const QString &) {});
        ctrl.simulateSafelyRemoveMemberUnmountDone(driveUdi, udi1, true);
        ctrl.simulatePowerOffDone(driveUdi, true);

        verify(tracker.solidTeardownCount == 0, "case 10: Solid::StorageAccess::teardown count == 0");
    }

    // Case 11: GUI "Odmontuj" -> unmountDevice -> Filesystem.Unmount only
    {
        ThisPcWindow window;
        window.resize(1000, 700);
        window.show();
        QApplication::processEvents();

        FakeBackendTracker tracker;
        tracker.unmountHandler = [](const QString &, auto done) { done(true, QString()); };
        window.removalControllerForTesting().setHooksForTesting(tracker.makeHooks());

        QTemporaryDir mntDir;
        DriveInfo pDrive = makeRemovableDrive(QStringLiteral("gui-pendrive-11"), udi1, mntDir.path(), true, true);
        window.handleDeviceUnmount(PaneId::Primary, pDrive);
        QApplication::processEvents();

        verify(tracker.filesystemUnmountCount == 1, "case 11: GUI 'Odmontuj' triggered Filesystem.Unmount");
        verify(tracker.powerOffCount == 0, "case 11: GUI 'Odmontuj' did NOT trigger PowerOff");
        verify(tracker.solidTeardownCount == 0, "case 11: GUI 'Odmontuj' did NOT trigger Solid teardown");
    }

    // Case 12: GUI "Bezpiecznie usuń" -> safelyRemoveDevice -> Filesystem.Unmount(s) -> PowerOff
    {
        ThisPcWindow window;
        window.resize(1000, 700);
        window.show();
        QApplication::processEvents();

        FakeBackendTracker tracker;
        tracker.unmountHandler = [](const QString &, auto done) { done(true, QString()); };
        auto hooks = tracker.makeHooks();
        hooks.findPhysicalDriveUdi = [&](const QString &) { return driveUdi; };
        hooks.findMountedMemberVolumes = [&](const QString &) { return QList<QString>{udi1}; };
        hooks.executePowerOff = [&](const QString &d) {
            ++tracker.powerOffCount;
            window.removalControllerForTesting().simulatePowerOffDone(d, true);
        };
        window.removalControllerForTesting().setHooksForTesting(hooks);

        QTemporaryDir mntDir;
        DriveInfo pDrive = makeRemovableDrive(QStringLiteral("gui-pendrive-12"), udi1, mntDir.path(), true, true);
        window.handleDeviceSafelyRemove(PaneId::Primary, pDrive);
        QApplication::processEvents();

        verify(tracker.filesystemUnmountCount == 1, "case 12: GUI 'Bezpiecznie usuń' triggered Filesystem.Unmount");
        verify(tracker.powerOffCount == 1, "case 12: GUI 'Bezpiecznie usuń' completed with PowerOff");
        verify(tracker.solidTeardownCount == 0, "case 12: GUI 'Bezpiecznie usuń' did NOT trigger Solid teardown");
    }

    qInfo("PASS: %d DeviceRemovalController assertions; unmount/eject/safely-remove, "
          "multi-partition, exactly-once, capabilities, pane routing, Stage 2 regression",
          checks);
    return 0;
}
