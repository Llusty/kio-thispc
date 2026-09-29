#include "soliddevicemonitor.h"

#include <Solid/Device>
#include <Solid/DeviceNotifier>
#include <Solid/StorageAccess>

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
}

void SolidDeviceMonitor::onDeviceAdded(const QString &udi)
{
    // Connect the per-device accessibility signal before scheduling the
    // refresh so future mount/unmount transitions on this device are tracked.
    if (m_solidActive) {
        connectAccessSignal(udi);
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
    const auto *access = device.as<Solid::StorageAccess>();
    if (!access) {
        return;
    }

    // QObject::connect requires a non-const pointer; Solid returns const
    // interfaces, but the underlying QObject* is accessible via device.
    connect(device.asDeviceInterface(Solid::DeviceInterface::StorageAccess),
            SIGNAL(accessibilityChanged(bool, QString)),
            this, SLOT(scheduleRefresh()));

    m_connectedUdis.insert(udi);
}

void SolidDeviceMonitor::disconnectAccessSignal(const QString &udi)
{
    if (!m_connectedUdis.remove(udi)) {
        return;
    }

    Solid::Device device(udi);
    // The device may already be gone; disconnect is safe even if the object
    // has been destroyed (Solid keeps the QObject alive until after
    // deviceRemoved is emitted).
    const QObject *iface =
        device.asDeviceInterface(Solid::DeviceInterface::StorageAccess);
    if (iface) {
        disconnect(iface, SIGNAL(accessibilityChanged(bool, QString)),
                   this, SLOT(scheduleRefresh()));
    }
}

void SolidDeviceMonitor::scheduleRefresh()
{
    // Restart the timer on every event so rapid successive events (e.g. a USB
    // hub with multiple partitions) collapse into a single devicesChanged().
    m_debounce.start();
}

