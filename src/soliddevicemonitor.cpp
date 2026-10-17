/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "soliddevicemonitor.h"

#include <Solid/Device>
#include <Solid/DeviceNotifier>
#include <Solid/PortableMediaPlayer>
#include <Solid/StorageAccess>

#include <QDBusConnection>

// Production constructor: connects to real Solid::DeviceNotifier.
SolidDeviceMonitor::SolidDeviceMonitor(QObject *parent, int debounceMs)
    : QObject(parent)
    , m_solidActive(true)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(debounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &SolidDeviceMonitor::devicesChanged);

    initSolid();
}

// Test constructor: skips all real Solid calls.
SolidDeviceMonitor::SolidDeviceMonitor(NotifierHooks hooks, int debounceMs, QObject *parent)
    : QObject(parent)
    , m_solidActive(false)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(debounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &SolidDeviceMonitor::devicesChanged);

    if (hooks.onReady) {
        hooks.onReady(this);
    }
}

SolidDeviceMonitor::~SolidDeviceMonitor() = default;

void SolidDeviceMonitor::initSolid()
{
    Solid::DeviceNotifier *notifier = Solid::DeviceNotifier::instance();

    connect(notifier, &Solid::DeviceNotifier::deviceAdded,
            this, &SolidDeviceMonitor::onDeviceAdded);
    connect(notifier, &Solid::DeviceNotifier::deviceRemoved,
            this, &SolidDeviceMonitor::onDeviceRemoved);

    // Connect accessibilityChanged for all currently-known StorageAccess
    // devices so we react to mount/unmount of already-present volumes.
    const auto devices =
        Solid::Device::listFromType(Solid::DeviceInterface::StorageAccess);
    for (const Solid::Device &device : devices) {
        connectAccessSignal(device.udi());
    }

    // Critical for MTP: prime UDevManager's internal m_devicesOfInterest list
    // for already-connected MTP devices. UDevManager only emits deviceRemoved
    // on physical USB disconnect if the device was in m_devicesOfInterest prior
    // to sysfs removal.
    Solid::Device::listFromType(Solid::DeviceInterface::PortableMediaPlayer);

    // Also listen to KDE's KMTPd daemon (kio-extras) on the session bus, which
    // monitors MTP hotplug and emits devicesChanged whenever a phone is connected,
    // disconnected, or changes USB transfer mode.
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.kde.kmtpd5"),
        QStringLiteral("/modules/kmtpd"),
        QStringLiteral("org.kde.kmtp.Daemon"),
        QStringLiteral("devicesChanged"),
        this,
        SLOT(scheduleRefresh()));
}

void SolidDeviceMonitor::onDeviceAdded(const QString &udi)
{
    // Connect the per-device accessibility signal before scheduling the
    // refresh so future mount/unmount transitions on this device are tracked.
    if (m_solidActive) {
        connectAccessSignal(udi);
        const auto devices =
            Solid::Device::listFromType(Solid::DeviceInterface::StorageAccess);
        for (const Solid::Device &device : devices) {
            connectAccessSignal(device.udi());
        }
        Solid::Device::listFromType(Solid::DeviceInterface::PortableMediaPlayer);
    }
    scheduleRefresh();
}

void SolidDeviceMonitor::onDeviceRemoved(const QString &udi)
{
    // The device object no longer exists, so disconnect the tracking entry.
    if (m_solidActive) {
        disconnectAccessSignal(udi);
    }
    scheduleRefresh();
}

void SolidDeviceMonitor::connectAccessSignal(const QString &udi)
{
    if (m_connectedUdis.contains(udi)) {
        return;
    }

    Solid::Device device(udi);
    auto *access = device.as<Solid::StorageAccess>();
    if (!access) {
        return;
    }

    connect(access, &Solid::StorageAccess::accessibilityChanged,
            this, &SolidDeviceMonitor::scheduleRefresh);

    m_connectedUdis.insert(udi);
}

void SolidDeviceMonitor::disconnectAccessSignal(const QString &udi)
{
    if (!m_connectedUdis.remove(udi)) {
        return;
    }

    Solid::Device device(udi);
    auto *access = device.as<Solid::StorageAccess>();
    if (access) {
        disconnect(access, &Solid::StorageAccess::accessibilityChanged,
                   this, &SolidDeviceMonitor::scheduleRefresh);
    }
}

void SolidDeviceMonitor::scheduleRefresh()
{
    // Keep UDevManager's interest list warm for any new or transitioning MTP devices
    if (m_solidActive) {
        Solid::Device::listFromType(Solid::DeviceInterface::PortableMediaPlayer);
    }
    // Restart the timer on every event so rapid successive events (e.g. a USB
    // hub with multiple partitions) collapse into a single devicesChanged().
    m_debounce.start();
}

