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

// ---------------------------------------------------------------------------
// Tests for SolidDeviceMonitor
//
// All tests use the NotifierHooks constructor so no real Solid::DeviceNotifier
// or physical hardware is required. The debounce timer is set to 0 ms so
// QCoreApplication::processEvents() is sufficient to drain it.
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ------------------------------------------------------------------
    // Test 1: initial state — no signal emitted before any event
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            0 /* debounceMs */);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });
        app.processEvents();
        verify(changed == 0, "no devicesChanged on construction");
    }

    // ------------------------------------------------------------------
    // Test 2: hotplug add → devicesChanged emitted (after debounce)
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            0);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        monitor.simulateDeviceAdded(QStringLiteral("/org/kde/solid/udev/usb0"));
        app.processEvents();
        verify(changed == 1, "hotplug add triggers devicesChanged");
    }

    // ------------------------------------------------------------------
    // Test 3: hotplug remove → devicesChanged emitted
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            0);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        monitor.simulateDeviceRemoved(QStringLiteral("/org/kde/solid/udev/usb0"));
        app.processEvents();
        verify(changed == 1, "hotplug remove triggers devicesChanged");
    }

    // ------------------------------------------------------------------
    // Test 4: mount transition → devicesChanged emitted
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            0);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        monitor.simulateAccessibilityChanged(); // mount
        app.processEvents();
        verify(changed == 1, "mount transition triggers devicesChanged");
    }

    // ------------------------------------------------------------------
    // Test 5: unmount transition → devicesChanged emitted
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            0);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        monitor.simulateAccessibilityChanged(); // unmount
        app.processEvents();
        verify(changed == 1, "unmount transition triggers devicesChanged");
    }

    // ------------------------------------------------------------------
    // Test 6: debounce — rapid events collapse into a single signal
    // ------------------------------------------------------------------
    {
        int changed = 0;
        // Use a real debounce window to test the collapsing behaviour.
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            50 /* ms */);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        // Fire 5 rapid events — all should be collapsed into one signal.
        monitor.simulateDeviceAdded(QStringLiteral("/org/kde/solid/udev/usb0"));
        monitor.simulateDeviceAdded(QStringLiteral("/org/kde/solid/udev/usb1"));
        monitor.simulateAccessibilityChanged();
        monitor.simulateDeviceRemoved(QStringLiteral("/org/kde/solid/udev/usb1"));
        monitor.simulateAccessibilityChanged();

        // Wait for the debounce to fire.
        QEventLoop loop;
        QTimer::singleShot(120, &loop, &QEventLoop::quit);
        loop.exec();

        verify(changed == 1, "rapid burst of 5 events collapses into exactly 1 devicesChanged");
    }

    // ------------------------------------------------------------------
    // Test 7: second burst after debounce fires → exactly 2 signals total
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            20 /* ms */);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        monitor.simulateDeviceAdded(QStringLiteral("/org/kde/solid/udev/usb0"));

        // Wait for first debounce.
        QEventLoop loop1;
        QTimer::singleShot(60, &loop1, &QEventLoop::quit);
        loop1.exec();

        verify(changed == 1, "first burst fires exactly once");

        monitor.simulateDeviceRemoved(QStringLiteral("/org/kde/solid/udev/usb0"));

        // Wait for second debounce.
        QEventLoop loop2;
        QTimer::singleShot(60, &loop2, &QEventLoop::quit);
        loop2.exec();

        verify(changed == 2, "second burst fires a second devicesChanged");
    }

    // ------------------------------------------------------------------
    // Test 8: no event → no devicesChanged at all (no spurious fire)
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            20);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        QEventLoop loop;
        QTimer::singleShot(60, &loop, &QEventLoop::quit);
        loop.exec();

        verify(changed == 0, "no spurious devicesChanged without events");
    }

    // ------------------------------------------------------------------
    // Test 9: mixed add + accessibility events → single debounced signal
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            30);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        monitor.simulateDeviceAdded(QStringLiteral("/org/kde/solid/udev/sdb1"));
        monitor.simulateAccessibilityChanged();

        QEventLoop loop;
        QTimer::singleShot(80, &loop, &QEventLoop::quit);
        loop.exec();

        verify(changed == 1, "add + accessibility change collapse into 1 signal");
    }

    // ------------------------------------------------------------------
    // Test 10: onReady hook called with correct monitor pointer
    // ------------------------------------------------------------------
    {
        SolidDeviceMonitor *seenMonitor = nullptr;
        SolidDeviceMonitor::NotifierHooks hooks;
        hooks.onReady = [&](SolidDeviceMonitor *m) { seenMonitor = m; };

        SolidDeviceMonitor monitor(hooks, 0);
        verify(seenMonitor == &monitor,
               "onReady hook receives correct SolidDeviceMonitor pointer");
    }

    // ------------------------------------------------------------------
    // Test 11: Stage 1.2 — MTP UDev hotplug add & remove
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            0);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        const QString mtpUdi = QStringLiteral(
            "/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2");
        monitor.simulateDeviceAdded(mtpUdi);
        app.processEvents();
        verify(changed == 1, "MTP UDev hotplug add triggers devicesChanged");

        monitor.simulateDeviceRemoved(mtpUdi);
        app.processEvents();
        verify(changed == 2, "MTP UDev hotplug remove triggers devicesChanged");
    }

    // ------------------------------------------------------------------
    // Test 12: Stage 1.2 — Phone USB mode switch (charging -> USB reset -> MTP)
    // collapses into a single debounced devicesChanged signal
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            40 /* ms */);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        const QString chargingUdi = QStringLiteral(
            "/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2");
        const QString mtpInterfaceUdi = QStringLiteral(
            "/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0");

        // Android USB mode switch from Charging to File Transfer (MTP) causes a rapid
        // USB disconnect + re-enumeration burst on the bus:
        monitor.simulateDeviceRemoved(chargingUdi);
        monitor.simulateDeviceAdded(chargingUdi);
        monitor.simulateDeviceAdded(mtpInterfaceUdi);

        QEventLoop loop;
        QTimer::singleShot(100, &loop, &QEventLoop::quit);
        loop.exec();

        verify(changed == 1,
               "Android Charging -> MTP USB reset burst collapses into 1 devicesChanged");
    }

    // ------------------------------------------------------------------
    // Test 13: Stage 1.2 — Two MTP phones hotplugged and reconnected on new port
    // ------------------------------------------------------------------
    {
        int changed = 0;
        SolidDeviceMonitor monitor(
            SolidDeviceMonitor::NotifierHooks{},
            0);
        QObject::connect(&monitor, &SolidDeviceMonitor::devicesChanged,
                         [&] { ++changed; });

        const QString phoneAUdiPort1 = QStringLiteral(
            "/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-1");
        const QString phoneBUdiPort2 = QStringLiteral(
            "/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2");
        const QString phoneAUdiPort3 = QStringLiteral(
            "/org/kde/solid/udev/sys/devices/pci0000:00/0000:00:14.0/usb1/1-3");

        monitor.simulateDeviceAdded(phoneAUdiPort1);
        app.processEvents();
        verify(changed == 1, "first MTP phone connect triggers devicesChanged");

        monitor.simulateDeviceAdded(phoneBUdiPort2);
        app.processEvents();
        verify(changed == 2, "second MTP phone connect triggers devicesChanged");

        monitor.simulateDeviceRemoved(phoneAUdiPort1);
        app.processEvents();
        verify(changed == 3, "first MTP phone disconnect triggers devicesChanged");

        monitor.simulateDeviceAdded(phoneAUdiPort3);
        app.processEvents();
        verify(changed == 4, "first MTP phone reconnect on port 3 triggers devicesChanged");
    }

    qInfo("PASS: %d SolidDeviceMonitor assertions; debounce, hotplug, mount transitions, MTP mode-switch, spurious suppression", checks);
}
