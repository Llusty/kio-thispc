/*
 * SolidDeviceMonitor — listens to Solid::DeviceNotifier and per-device
 * StorageAccess::accessibilityChanged signals and emits a debounced
 * devicesChanged() whenever the set of mounted/accessible volumes may have
 * changed.
 *
 * Only the KIO worker (thispc.cpp) knows which volumes are actually visible.
 * This class is deliberately decoupled from that logic: it simply triggers a
 * fresh KIO listing whenever Solid reports a storage event.
 *
 * Debounce delay (default 500 ms) collapses rapid successive events (e.g. a
 * USB hub with several partitions) into a single reload.
 *
 * Testing: pass a NotifierHooks struct to the second constructor to inject
 * fake notifier callbacks instead of the real Solid::DeviceNotifier. This
 * allows full unit-testing of debounce and connect/disconnect logic without
 * requiring physical hardware or a running Plasma session.
 *
 * Version 0.35.0
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>

class SolidDeviceMonitor : public QObject
{
    Q_OBJECT

public:
    // Default debounce: 500 ms — long enough to let udev+udisks settle after
    // a hotplug event, short enough to feel reactive to the user.
    static constexpr int kDefaultDebounceMs = 500;

    // Hooks injected by unit tests to replace real Solid calls.
    // Each hook receives a pointer to a SolidDeviceMonitor method so the test
    // can call onDeviceAdded/onDeviceRemoved/scheduleRefresh directly.
    struct NotifierHooks {
        // Called during construction so the test can save the callback pointers.
        std::function<void(SolidDeviceMonitor *)> onReady;
    };

    // Production constructor: uses real Solid::DeviceNotifier.
    explicit SolidDeviceMonitor(QObject *parent = nullptr,
                                int debounceMs = kDefaultDebounceMs);

    // Test constructor: skips all real Solid calls; calls hooks.onReady(this)
    // so the test wires up simulated signals.
    explicit SolidDeviceMonitor(NotifierHooks hooks,
                                int debounceMs = kDefaultDebounceMs,
                                QObject *parent = nullptr);

    ~SolidDeviceMonitor() override;

    // These are public only for the test constructor hook; do not call
    // directly in production code.
    void simulateDeviceAdded(const QString &udi) { onDeviceAdded(udi); }
    void simulateDeviceRemoved(const QString &udi) { onDeviceRemoved(udi); }
    void simulateAccessibilityChanged() { scheduleRefresh(); }

Q_SIGNALS:
    // Emitted at most once per debounce window after any storage change.
    void devicesChanged();

private:
    void initSolid();
    void onDeviceAdded(const QString &udi);
    void onDeviceRemoved(const QString &udi);
    void connectAccessSignal(const QString &udi);
    void disconnectAccessSignal(const QString &udi);
    Q_SLOT void scheduleRefresh();

    QTimer m_debounce;
    // UDIs of devices we have connected accessibilityChanged to, so we can
    // disconnect them cleanly when the device is removed.
    QSet<QString> m_connectedUdis;
    // True when using real Solid (production); false in test mode.
    bool m_solidActive = false;
};
