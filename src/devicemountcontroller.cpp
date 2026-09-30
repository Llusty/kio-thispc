/*
 * DeviceMountController — implementation of exactly-once asynchronous mount
 * via KDE Solid.
 *
 * Stage 2 of 0.35.
 * SPDX-License-Identifier: MIT
 */

#include "devicemountcontroller.h"
#include "browsercommon.h"

#include <Solid/Device>
#include <Solid/DeviceNotifier>
#include <Solid/SolidNamespace>
#include <Solid/StorageAccess>

#include <QPointer>
#include <QTimer>

namespace
{
QString solidErrorToString(int error)
{
    switch (static_cast<Solid::ErrorType>(error)) {
    case Solid::NoError:
        return {};
    case Solid::UnauthorizedOperation:
        return trLocal("Brak uprawnień do zamontowania urządzenia.",
                       "Unauthorized to mount device.");
    case Solid::DeviceBusy:
        return trLocal("Urządzenie jest zajęte.",
                       "Device is busy.");
    case Solid::UserCanceled:
        return trLocal("Operacja została anulowana.",
                       "Operation was canceled.");
    case Solid::MissingDriver:
        return trLocal("Brak wymaganego sterownika systemu plików.",
                       "Missing filesystem driver.");
    case Solid::OperationFailed:
    default:
        return trLocal("Operacja montowania nie powiodła się.",
                       "Mount operation failed.");
    }
}
} // namespace

DeviceMountController::DeviceMountController(QObject *parent)
    : QObject(parent)
    , m_solidActive(true)
{
    initSolid();
}

DeviceMountController::DeviceMountController(Hooks hooks, QObject *parent)
    : QObject(parent)
    , m_solidActive(false)
    , m_hooks(std::move(hooks))
{
    if (m_hooks.onReady) {
        m_hooks.onReady(this);
    }
}

DeviceMountController::~DeviceMountController()
{
    m_pendingRequests.clear();
}

void DeviceMountController::setHooksForTesting(Hooks hooks)
{
    m_hooks = std::move(hooks);
    m_solidActive = false;
    if (m_hooks.onReady) {
        m_hooks.onReady(this);
    }
}

void DeviceMountController::initSolid()
{
    Solid::DeviceNotifier *notifier = Solid::DeviceNotifier::instance();
    connect(notifier, &Solid::DeviceNotifier::deviceRemoved,
            this, &DeviceMountController::onDeviceRemoved);
}

bool DeviceMountController::isMountPending(const QString &udi) const
{
    return m_pendingRequests.contains(udi);
}

void DeviceMountController::mountDevice(const QString &udi, MountCallback callback)
{
    if (udi.isEmpty()) {
        if (callback) {
            callback(false, {}, trLocal("Nieprawidłowy identyfikator urządzenia.",
                                        "Invalid device identifier."));
        }
        return;
    }

    // If real Solid is active, inspect device state first.
    if (m_solidActive) {
        Solid::Device device(udi);
        if (!device.isValid()) {
            if (callback) {
                callback(false, {}, trLocal("Urządzenie nie zostało znalezione.",
                                            "Device not found."));
            }
            return;
        }

        auto *access = device.as<Solid::StorageAccess>();
        if (!access) {
            if (callback) {
                callback(false, {}, trLocal("Urządzenie nie obsługuje montowania.",
                                            "Device does not support mounting."));
            }
            return;
        }

        // Already mounted race condition check
        if (access->isAccessible() && !access->filePath().isEmpty()) {
            if (callback) {
                callback(true, access->filePath(), {});
            }
            return;
        }

        // Attach callback if request is already in-flight for this UDI
        if (m_pendingRequests.contains(udi)) {
            if (callback) {
                m_pendingRequests[udi].callbacks.append(std::move(callback));
            }
            return;
        }

        PendingRequest req;
        req.udi = udi;
        if (callback) {
            req.callbacks.append(std::move(callback));
        }
        m_pendingRequests.insert(udi, req);
        Q_EMIT mountStarted(udi);

        // Connect setupDone and accessibilityChanged on the specific StorageAccess
        QPointer<DeviceMountController> self(this);
        connect(access, &Solid::StorageAccess::setupDone, this,
                [self, udi](Solid::ErrorType error, const QVariant &, const QString &devUdi) {
            if (!self) return;
            const QString targetUdi = !devUdi.isEmpty() ? devUdi : udi;
            self->onSetupDone(static_cast<int>(error), targetUdi);
        });

        connect(access, &Solid::StorageAccess::accessibilityChanged, this,
                [self, udi](bool accessible, const QString &devUdi) {
            if (!self) return;
            const QString targetUdi = !devUdi.isEmpty() ? devUdi : udi;
            self->onAccessibilityChanged(accessible, targetUdi);
        });

        if (!access->setup()) {
            finishRequest(udi, false, {},
                          trLocal("Nie można rozpocząć montowania urządzenia.",
                                  "Cannot initiate mounting for this device."));
        }
        return;
    }

    // In test / simulated mode:
    if (m_pendingRequests.contains(udi)) {
        if (callback) {
            m_pendingRequests[udi].callbacks.append(std::move(callback));
        }
        return;
    }

    PendingRequest req;
    req.udi = udi;
    if (callback) {
        req.callbacks.append(std::move(callback));
    }
    m_pendingRequests.insert(udi, req);
    Q_EMIT mountStarted(udi);

    if (m_hooks.requestMount) {
        const bool started = m_hooks.requestMount(udi);
        if (!started) {
            finishRequest(udi, false, {},
                          trLocal("Nie można rozpocząć montowania urządzenia.",
                                  "Cannot initiate mounting for this device."));
        }
    }
}

void DeviceMountController::onSetupDone(int error, const QString &udi)
{
    if (!m_pendingRequests.contains(udi)) {
        return;
    }

    if (error != Solid::NoError) {
        finishRequest(udi, false, {}, solidErrorToString(error));
        return;
    }

    // Re-check StorageAccess
    Solid::Device device(udi);
    auto *access = device.as<Solid::StorageAccess>();
    if (access && access->isAccessible() && !access->filePath().isEmpty()) {
        finishRequest(udi, true, access->filePath(), {});
    }
    // If not accessible yet, wait for accessibilityChanged to arrive.
}

void DeviceMountController::onAccessibilityChanged(bool accessible, const QString &udi)
{
    if (!m_pendingRequests.contains(udi)) {
        return;
    }

    if (accessible) {
        Solid::Device device(udi);
        auto *access = device.as<Solid::StorageAccess>();
        if (access && !access->filePath().isEmpty()) {
            finishRequest(udi, true, access->filePath(), {});
        }
    }
}

void DeviceMountController::onDeviceRemoved(const QString &udi)
{
    if (!m_pendingRequests.contains(udi)) {
        return;
    }

    finishRequest(udi, false, {},
                  trLocal("Urządzenie zostało odłączone.",
                          "Device was disconnected."));
}

void DeviceMountController::finishRequest(const QString &udi,
                                          bool success,
                                          const QString &mountPoint,
                                          const QString &errorMessage)
{
    auto it = m_pendingRequests.find(udi);
    if (it == m_pendingRequests.end()) {
        return;
    }

    PendingRequest req = std::move(it.value());
    m_pendingRequests.erase(it);

    Q_EMIT mountFinished(udi, success);

    for (const auto &cb : req.callbacks) {
        if (cb) {
            cb(success, mountPoint, errorMessage);
        }
    }
}

void DeviceMountController::simulateSetupDone(const QString &udi,
                                              bool success,
                                              const QString &mountPoint,
                                              const QString &errorMessage)
{
    if (!m_pendingRequests.contains(udi)) {
        return;
    }

    if (success) {
        finishRequest(udi, true, mountPoint, {});
    } else {
        const QString err = !errorMessage.isEmpty()
            ? errorMessage
            : trLocal("Operacja montowania nie powiodła się.", "Mount operation failed.");
        finishRequest(udi, false, {}, err);
    }
}

void DeviceMountController::simulateAccessibilityChanged(const QString &udi,
                                                         bool accessible,
                                                         const QString &mountPoint)
{
    if (!m_pendingRequests.contains(udi)) {
        return;
    }

    if (accessible && !mountPoint.isEmpty()) {
        finishRequest(udi, true, mountPoint, {});
    }
}

void DeviceMountController::simulateDeviceRemoved(const QString &udi)
{
    onDeviceRemoved(udi);
}
