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

#include "mtp-contract-spec.h"

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

        // ===================================================================
        // Stage 1.2: MTP Activation, KIO Redirection & Navigation (Section 4, 5, 6)
        // ===================================================================
        const QString mtpUdi = QStringLiteral("/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2");
        const DriveInfo mtpPhone = MtpUdsContract::makeMtpDriveInfo(
            QStringLiteral("mtp-pixel8"),
            QStringLiteral("Google Pixel 8 Pro"),
            mtpUdi);
        const QUrl redirectedRoot(QStringLiteral("mtp:/Google Pixel 8 Pro"));
        const QUrl internalStorageUrl(QStringLiteral("mtp:/Google Pixel 8 Pro/Internal shared storage"));
        const QUrl sdCardUrl(QStringLiteral("mtp:/Google Pixel 8 Pro/SD card"));

        // 1. Live Solid validation on an MTP UDev UDI rejects Solid::StorageAccess::setup()
        {
            DeviceMountController liveSolidController;
            int mtpMountFired = 0;
            bool mtpMountSuccess = true;
            liveSolidController.mountDevice(mtpUdi, [&](bool s, const QString &, const QString &) {
                ++mtpMountFired;
                mtpMountSuccess = s;
            });
            verify(mtpMountFired == 1 && !mtpMountSuccess,
                   "DeviceMountController rejects MTP UDev UDI because it has no Solid::StorageAccess");
        }

        // 2. Activating an MTP card in Primary or Split NEVER calls DeviceMountController::mountDevice
        windowMountRequestsStarted = 0;
        lastRequestedWindowUdi.clear();
        window.m_driveHomeCoordinator.setSnapshotForTesting({mountedDrive, unmountedDrive, mtpPhone});
        QApplication::processEvents();

        if (window.m_sidebar) {
            verify(window.m_sidebar->drives().size() == 2,
                   "sidebar includes mounted volume and navigable MTP phone, excludes unmounted volume");
        }

        window.handleDriveActivation(PaneId::Primary, mtpPhone);
        verify(windowMountRequestsStarted == 0 && !window.mountControllerForTesting().isMountPending(mtpUdi),
               "activating MTP phone in Primary NEVER calls Solid::StorageAccess::setup()");
        verify(window.m_primaryPane->listingCoreForTesting().isLoading(),
               "activating MTP phone starts KIO listing for mtp:udi=<UDI> in Primary");

        // 3. Simulate KIO MTP redirection (mtp:udi=<UDI> -> mtp:/Google Pixel 8 Pro) + storage entries
        {
            auto &core = window.m_primaryPane->listingCoreForTesting();
            QPointer<KIO::ListJob> job = core.listingJob();
            verify(job != nullptr, "Primary KIO ListJob exists for MTP listing");

            KIO::UDSEntry internalEntry;
            internalEntry.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("Internal shared storage"));
            internalEntry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR);
            internalEntry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, QStringLiteral("inode/directory"));

            KIO::UDSEntry sdEntry;
            sdEntry.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("SD card"));
            sdEntry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR);
            sdEntry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, QStringLiteral("inode/directory"));

            const quint64 listingCountBeforeFinish = core.startListingCount();
            const int historySizeBeforeFinish = window.m_navigation.history().size();

            Q_EMIT job->redirection(job, redirectedRoot);
            verify(core.currentUrl() == redirectedRoot,
                   "DirectoryListingCore updates its internal currentUrl upon KIO redirection");

            Q_EMIT job->entries(job, {internalEntry, sdEntry});
            core.slotJobFinished(job);
            if (job) job->kill(KJob::Quietly);
            QApplication::processEvents();

            verify(core.files().size() == 2,
                   "DirectoryListingCore commits 2 MTP storage entries after redirection");
            verify(core.files().at(0).url == internalStorageUrl
                       && core.files().at(1).url == sdCardUrl,
                   "MTP storage child URLs use redirected mtp:/<Device>/<Storage> base, not mtp:udi=");

            // Stage 1.4 active verification: PrimaryBrowserPane and ThisPcWindow::m_navigation
            // synchronize their currentUrl to core.currentUrl() after KIO redirection!
            verify(
                window.m_primaryPane->currentUrl() == redirectedRoot
                    && window.m_navigation.currentUrl() == redirectedRoot,
                "PrimaryBrowserPane and ThisPcWindow::m_navigation update currentUrl from mtp:udi= to redirected mtp:/<Device>");
            verify(core.startListingCount() == listingCountBeforeFinish,
                   "MTP redirection synchronization in Primary does not trigger a duplicate KIO startListing");
            verify(window.m_navigation.history().size() == historySizeBeforeFinish
                       && window.m_navigation.history().at(window.m_navigation.historyIndex()) == redirectedRoot,
                   "MTP redirection updates Primary current history entry in-place without leaving raw mtp:udi=");
            verify(window.m_navigation.parentUrl(window.m_driveHomeCoordinator.drives()) == kThisPcUrl,
                   "Primary parentUrl at redirected MTP device root is thispc:/");
        }

        // 4. Split View MTP activation & KIO redirection
        window.setSplitViewEnabled(true);
        QApplication::processEvents();
        window.handleDriveActivation(PaneId::Split, mtpPhone);
        verify(windowMountRequestsStarted == 0,
               "activating MTP phone in Split NEVER calls Solid::StorageAccess::setup()");
        if (window.m_splitPane) {
            auto &splitCore = window.m_splitPane->m_listingCore;
            const quint64 splitListingsBefore = splitCore.startListingCount();
            const int splitHistBefore = window.m_splitPane->m_history.size();
            QPointer<KIO::ListJob> splitJob = splitCore.listingJob();
            verify(splitJob != nullptr, "Split KIO ListJob exists for MTP listing");
            Q_EMIT splitJob->redirection(splitJob, redirectedRoot);
            splitCore.slotJobFinished(splitJob);
            if (splitJob) splitJob->kill(KJob::Quietly);
            QApplication::processEvents();

            verify(
                window.m_splitPane->currentUrl() == redirectedRoot,
                "SplitBrowserPane updates currentUrl from mtp:udi= to redirected mtp:/<Device>");
            verify(splitCore.startListingCount() == splitListingsBefore,
                   "MTP redirection synchronization in Split does not trigger a duplicate KIO startListing");
            verify(window.m_splitPane->m_history.size() == splitHistBefore
                       && window.m_splitPane->m_history.at(window.m_splitPane->m_historyIndex) == redirectedRoot,
                   "MTP redirection updates Split current history entry in-place without leaving raw mtp:udi=");
            verify(window.m_splitPane->parentUrl() == kThisPcUrl,
                   "Split parentUrl at redirected MTP device root is thispc:/");
        }

        // 5. Locked phone / charging-only KIO failure (0 storages / ERR_CANNOT_ENTER_DIRECTORY)
        {
            auto &core = window.m_primaryPane->listingCoreForTesting();
            bool failureReported = false;
            QString capturedError;
            core.startListing(mtpPhone.targetUrl, {}, {
                [](KIO::ListJob *) {},
                [&](const QString &err) {
                    failureReported = true;
                    capturedError = err;
                }
            });
            QPointer<KIO::ListJob> lockedJob = core.listingJob();
            verify(lockedJob != nullptr, "locked phone listing starts KIO ListJob");
            Q_EMIT lockedJob->redirection(lockedJob, redirectedRoot);
            // Cleanly cancel the test job and verify cancellation / failure state safety
            core.cancelListing();
            verify(!core.isLoading() && core.listingJob() == nullptr,
                   "canceling active MTP KIO job resets loading state without dangling job pointer");
            verify(core.currentUrl() == mtpPhone.targetUrl,
                   "canceling redirected MTP KIO job rolls DirectoryListingCore::currentUrl back to requested URL");
        }

        // 6. NavigationHistory Back/Forward with KIO redirection updateCurrent
        {
            NavigationHistory nav;
            nav.navigate(kThisPcUrl);
            nav.navigate(mtpPhone.targetUrl);
            // Simulate KIO redirection updating the current entry in-place:
            nav.updateCurrent(redirectedRoot, true);
            nav.navigate(internalStorageUrl);

            verify(nav.currentUrl() == internalStorageUrl, "MTP nav at internal storage");
            verify(nav.back() == redirectedRoot, "MTP Back returns to redirected mtp:/<Device>, not raw mtp:udi=");
            verify(nav.back() == kThisPcUrl, "MTP second Back returns to thispc:/");
            verify(nav.forward() == redirectedRoot, "MTP Forward goes to mtp:/<Device>");
            verify(nav.forward() == internalStorageUrl, "MTP second Forward goes to internal storage");
        }

        // 7. Pane Swap (swapSplitPanes) between MTP URL and local directory
        {
            QTemporaryDir swapLocalDir;
            const QUrl swapLocalUrl = QUrl::fromLocalFile(swapLocalDir.path());
            window.m_primaryPane->setCurrentUrl(internalStorageUrl);
            window.m_navigation.navigate(internalStorageUrl);
            window.m_splitPane->setCurrentUrl(swapLocalUrl, true);

            window.swapSplitPanes();
            // Cancel any in-flight KIO job started by swapSplitPanes
            window.m_primaryPane->cancelListing();
            window.m_splitPane->cancelListing();
            QApplication::processEvents();

            verify(window.m_navigation.currentUrl() == swapLocalUrl,
                   "swapSplitPanes moves local directory URL into Primary pane");
            verify(window.m_splitPane->currentUrl() == internalStorageUrl,
                   "swapSplitPanes moves MTP internal storage URL into Split pane");
        }

        // 8. Session save and restore with a stale/disconnected MTP URL
        {
            const QUrl staleMtpUrl(QStringLiteral("mtp:/Disconnected Phone/Internal shared storage/DCIM"));
            const QUrl staleMtpUdiUrl(QStringLiteral("mtp:udi=/org/kde/solid/udev/sys/devices/usb9/9-9"));
            QTemporaryDir sessionLocalDir;
            const QUrl sessionLocalUrl = QUrl::fromLocalFile(sessionLocalDir.path());

            // 8a. NavigationHistory::safeRestoreBaseline strips unverified/stale MTP URLs
            const NavigationHistory::Snapshot rawMtpSnap{
                staleMtpUrl, {kThisPcUrl, staleMtpUrl}, 1};
            const NavigationHistory::Snapshot safeMtpSnap =
                NavigationHistory::safeRestoreBaseline(rawMtpSnap);
            verify(
                safeMtpSnap.currentUrl == kThisPcUrl && safeMtpSnap.history == QList<QUrl>{kThisPcUrl}
                    && safeMtpSnap.historyIndex == 0,
                "Session restore with stale/disconnected MTP URL uses safe fallback or transactional rollback");

            const NavigationHistory::Snapshot rawLocalThenMtpSnap{
                staleMtpUrl,
                {kThisPcUrl, sessionLocalUrl, QUrl(QStringLiteral("mtp:/Disconnected Phone")), staleMtpUrl},
                3};
            const NavigationHistory::Snapshot safeLocalFallbackSnap =
                NavigationHistory::safeRestoreBaseline(rawLocalThenMtpSnap);
            verify(
                safeLocalFallbackSnap.currentUrl == sessionLocalUrl
                    && !safeLocalFallbackSnap.history.contains(staleMtpUrl)
                    && !safeLocalFallbackSnap.history.contains(QUrl(QStringLiteral("mtp:/Disconnected Phone"))),
                "safeRestoreBaseline falls back to last valid local URL and strips stale MTP entries from history");

            // 8b. Full ThisPcWindow session restore with stale MTP URLs in Tab 0 (Primary + Split) and local URL in Tab 1
            SessionSnapshot saved;
            TabState tab0;
            tab0.currentUrl = staleMtpUrl;
            tab0.history = {kThisPcUrl, staleMtpUrl};
            tab0.historyIndex = 1;
            tab0.splitEnabled = true;
            tab0.splitUrl = staleMtpUdiUrl;
            tab0.splitHistory = {kThisPcUrl, staleMtpUdiUrl};
            tab0.splitHistoryIndex = 1;

            TabState tab1;
            tab1.currentUrl = sessionLocalUrl;
            tab1.history = {kThisPcUrl, sessionLocalUrl};
            tab1.historyIndex = 1;
            tab1.splitEnabled = true;
            tab1.splitUrl = sessionLocalUrl;
            tab1.splitHistory = {kThisPcUrl, sessionLocalUrl};
            tab1.splitHistoryIndex = 1;

            saved.tabs = {tab0, tab1};
            saved.activeTab = 0;
            saved.splitPaneActive = true;
            SessionManager::save(saved);

            const SessionSnapshot loaded = SessionManager::load(0, true);
            verify(loaded.tabs.size() == 2 && loaded.tabs.first().currentUrl == staleMtpUrl,
                   "SessionManager round-trips MTP URL in saved tab state");

            const bool prevRestorePref = SessionManager::restorePreviousSessionEnabled();
            SessionManager::setRestorePreviousSessionEnabled(true);
            {
                ThisPcWindow restoredWindow(kThisPcUrl, true);
                restoredWindow.show();
                QApplication::processEvents();

                verify(restoredWindow.m_navigation.currentUrl() == kThisPcUrl,
                       "restored Tab 0 Primary with disconnected MTP URL falls back to thispc:/");
                verify(restoredWindow.m_primaryPane->currentUrl() == kThisPcUrl,
                       "restored Tab 0 PrimaryBrowserPane currentUrl is thispc:/");
                verify(!restoredWindow.m_navigation.canGoBack() && !restoredWindow.m_navigation.canGoForward(),
                       "restored Tab 0 Primary history has no dangling stale MTP entries");
                verify(restoredWindow.m_splitPane->currentUrl() == kThisPcUrl,
                       "restored Tab 0 Split with disconnected mtp:udi= URL falls back to thispc:/");
                verify(!restoredWindow.m_splitPane->canGoBack() && !restoredWindow.m_splitPane->canGoForward(),
                       "restored Tab 0 Split history has no dangling stale MTP entries");

                // Switch to Tab 1 (local directory in both Primary and Split)
                restoredWindow.switchToTab(1);
                QApplication::processEvents();
                verify(restoredWindow.m_navigation.currentUrl() == sessionLocalUrl,
                       "restored Tab 1 with local URL opens normally without interference from Tab 0 stale MTP");
                verify(restoredWindow.m_splitPane->currentUrl() == sessionLocalUrl,
                       "restored Tab 1 Split with local URL opens normally");
            }
            SessionManager::setRestorePreviousSessionEnabled(prevRestorePref);
        }
    }

    qInfo("PASS: %d DeviceMountController assertions (%d pending MTP specs); unmounted discovery, stable ID, exactly-once signals, pane routing, MTP redirection",
          checks, pendingMtpSpecs);
    return 0;
}
