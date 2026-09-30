/*
 * DeviceRemovalController — implementation of asynchronous unmounting, media ejection,
 * and safe physical removal of storage devices.
 *
 * Stage 3 of 0.35.
 * SPDX-License-Identifier: MIT
 */

#include "deviceremovalcontroller.h"
#include "browsercommon.h"

#include <Solid/Device>
#include <Solid/DeviceNotifier>
#include <Solid/OpticalDrive>
#include <Solid/SolidNamespace>
#include <Solid/StorageAccess>
#include <Solid/StorageDrive>
#include <Solid/StorageVolume>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QPointer>
#include <QVariantMap>

namespace
{
QString solidErrorToString(int error)
{
    switch (static_cast<Solid::ErrorType>(error)) {
    case Solid::NoError:
        return {};
    case Solid::UnauthorizedOperation:
        return trLocal("Brak uprawnień do wykonania operacji.",
                       "Unauthorized to perform operation.");
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
        return trLocal("Operacja nie powiodła się.",
                       "Operation failed.");
    }
}
} // namespace

DeviceRemovalController::DeviceRemovalController(QObject *parent)
    : QObject(parent)
    , m_solidActive(true)
{
    initSolid();
}

DeviceRemovalController::DeviceRemovalController(Hooks hooks, QObject *parent)
    : QObject(parent)
    , m_solidActive(false)
    , m_hooks(std::move(hooks))
{
    if (m_hooks.onReady) {
        m_hooks.onReady(this);
    }
}

DeviceRemovalController::~DeviceRemovalController()
{
    m_pendingUnmounts.clear();
    m_pendingEjects.clear();
    m_pendingSafelyRemoves.clear();
    m_pendingFilesystemUnmounts.clear();
}

void DeviceRemovalController::setHooksForTesting(Hooks hooks)
{
    m_hooks = std::move(hooks);
    m_solidActive = false;
    if (m_hooks.onReady) {
        m_hooks.onReady(this);
    }
}

void DeviceRemovalController::initSolid()
{
    Solid::DeviceNotifier *notifier = Solid::DeviceNotifier::instance();
    connect(notifier, &Solid::DeviceNotifier::deviceRemoved,
            this, &DeviceRemovalController::onDeviceRemoved);
}

bool DeviceRemovalController::isOperationPending(const QString &udi) const
{
    return isUnmountPending(udi) || isEjectPending(udi) || isSafelyRemovePending(udi);
}

bool DeviceRemovalController::isUnmountPending(const QString &udi) const
{
    return m_pendingUnmounts.contains(udi);
}

bool DeviceRemovalController::isEjectPending(const QString &udi) const
{
    return m_pendingEjects.contains(udi);
}

bool DeviceRemovalController::isSafelyRemovePending(const QString &udi) const
{
    if (m_pendingSafelyRemoves.contains(udi)) {
        return true;
    }
    const QString driveUdi = findPhysicalDriveUdi(udi);
    if (!driveUdi.isEmpty() && m_pendingSafelyRemoves.contains(driveUdi)) {
        return true;
    }
    for (auto it = m_pendingSafelyRemoves.constBegin(); it != m_pendingSafelyRemoves.constEnd(); ++it) {
        if (it->volumeUdi == udi || it->volumesToUnmount.contains(udi)) {
            return true;
        }
    }
    return false;
}

bool DeviceRemovalController::canEject(const QString &udi) const
{
    if (m_hooks.canEject) {
        return m_hooks.canEject(udi);
    }
    if (!m_solidActive || udi.isEmpty()) {
        return false;
    }

    Solid::Device current(udi);
    while (current.isValid()) {
        if (current.is<Solid::OpticalDrive>()) {
            return true;
        }
        current = current.parent();
    }
    return false;
}

bool DeviceRemovalController::canSafelyRemove(const QString &udi) const
{
    if (m_hooks.canSafelyRemove) {
        return m_hooks.canSafelyRemove(udi);
    }
    if (!m_solidActive || udi.isEmpty()) {
        return false;
    }

    Solid::Device dev(udi);
    if (!dev.isValid()) {
        return false;
    }

    // 1. Must be a removable or hotpluggable storage drive:
    Solid::Device driveDev;
    Solid::Device current = dev;
    while (current.isValid()) {
        if (const auto *drive = current.as<Solid::StorageDrive>()) {
            if (drive->isRemovable() || drive->isHotpluggable()) {
                driveDev = current;
                break;
            }
            return false; // internal fixed drive
        }
        current = current.parent();
    }

    if (!driveDev.isValid()) {
        return false;
    }

    // 2. Physical drive must have CanPowerOff == true on UDisks2:
    QDBusInterface driveIface(QStringLiteral("org.freedesktop.UDisks2"),
                              driveDev.udi(),
                              QStringLiteral("org.freedesktop.UDisks2.Drive"),
                              QDBusConnection::systemBus());
    if (!driveIface.isValid()) {
        return false;
    }

    return driveIface.property("CanPowerOff").toBool();
}

QString DeviceRemovalController::findPhysicalDriveUdi(const QString &volumeUdi) const
{
    if (m_hooks.findPhysicalDriveUdi) {
        return m_hooks.findPhysicalDriveUdi(volumeUdi);
    }
    if (volumeUdi.isEmpty()) {
        return {};
    }
    if (!m_solidActive) {
        return volumeUdi;
    }

    Solid::Device current(volumeUdi);
    while (current.isValid()) {
        if (current.is<Solid::StorageDrive>()) {
            return current.udi();
        }
        current = current.parent();
    }
    return {};
}

QList<QString> DeviceRemovalController::findMountedMemberVolumes(const QString &physicalDriveUdi) const
{
    if (m_hooks.findMountedMemberVolumes) {
        return m_hooks.findMountedMemberVolumes(physicalDriveUdi);
    }
    QList<QString> result;
    if (physicalDriveUdi.isEmpty() || !m_solidActive) {
        return result;
    }

    const auto volumes = Solid::Device::listFromType(Solid::DeviceInterface::StorageVolume);
    for (const auto &vol : volumes) {
        Solid::Device curr = vol;
        bool belongs = false;
        while (curr.isValid()) {
            if (curr.udi() == physicalDriveUdi) {
                belongs = true;
                break;
            }
            curr = curr.parent();
        }

        if (belongs) {
            if (const auto *access = vol.as<Solid::StorageAccess>()) {
                if (access->isAccessible()) {
                    result.append(vol.udi());
                }
            }
        }
    }
    return result;
}

void DeviceRemovalController::unmountFilesystemViaUDisks(
    const QString &volumeUdi,
    std::function<void(bool success, const QString &errorMessage)> completion)
{
    if (m_hooks.executeFilesystemUnmount) {
        m_hooks.executeFilesystemUnmount(volumeUdi, std::move(completion));
        return;
    }

    if (!m_solidActive) {
        if (m_hooks.requestUnmount) {
            const bool started = m_hooks.requestUnmount(volumeUdi);
            if (!started) {
                if (completion) {
                    completion(false, trLocal("Nie można rozpocząć odmontowywania urządzenia.",
                                              "Cannot initiate unmounting for this device."));
                }
                return;
            }
        }
        if (completion) {
            m_pendingFilesystemUnmounts[volumeUdi].append(std::move(completion));
        }
        return;
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.UDisks2"),
        volumeUdi,
        QStringLiteral("org.freedesktop.UDisks2.Filesystem"),
        QStringLiteral("Unmount"));

    QVariantMap options;
    msg << options;

    QDBusPendingCall async = QDBusConnection::systemBus().asyncCall(msg);
    auto *watcher = new QDBusPendingCallWatcher(async, this);

    QPointer<DeviceRemovalController> self(this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [self, volumeUdi, watcher, comp = std::move(completion)](QDBusPendingCallWatcher *call) {
        watcher->deleteLater();
        if (!self) return;

        QDBusPendingReply<> reply = *call;
        if (reply.isError()) {
            const auto err = reply.error();
            if (err.name() == QLatin1String("org.freedesktop.UDisks2.Error.NotMounted")) {
                if (comp) comp(true, QString());
            } else {
                if (comp) comp(false, err.message());
            }
        } else {
            if (comp) comp(true, QString());
        }
    });
}

void DeviceRemovalController::unmountDevice(const QString &udi, RemovalCallback callback)
{
    if (udi.isEmpty()) {
        if (callback) {
            callback(false, trLocal("Nieprawidłowy identyfikator urządzenia.",
                                    "Invalid device identifier."));
        }
        return;
    }

    if (m_solidActive) {
        Solid::Device device(udi);
        if (!device.isValid()) {
            if (callback) {
                callback(false, trLocal("Urządzenie nie zostało znalezione.",
                                        "Device not found."));
            }
            return;
        }

        const auto *access = device.as<Solid::StorageAccess>();
        if (!access) {
            if (callback) {
                callback(false, trLocal("Urządzenie nie obsługuje odmontowywania.",
                                        "Device does not support unmounting."));
            }
            return;
        }

        if (!access->isAccessible()) {
            // Already unmounted
            if (callback) {
                callback(true, {});
            }
            return;
        }
    }

    // Attach callback if request is already in-flight for this UDI
    if (m_pendingUnmounts.contains(udi)) {
        if (callback) {
            m_pendingUnmounts[udi].callbacks.append(std::move(callback));
        }
        return;
    }

    PendingRequest req;
    req.udi = udi;
    if (callback) {
        req.callbacks.append(std::move(callback));
    }
    m_pendingUnmounts.insert(udi, req);
    Q_EMIT unmountStarted(udi);

    QPointer<DeviceRemovalController> self(this);
    unmountFilesystemViaUDisks(udi, [self, udi](bool success, const QString &errorMessage) {
        if (!self) return;
        self->finishUnmountRequest(udi, success, errorMessage);
    });
}

void DeviceRemovalController::onAccessibilityChanged(bool accessible, const QString &udi)
{
    if (!m_pendingUnmounts.contains(udi)) {
        return;
    }

    if (!accessible) {
        finishUnmountRequest(udi, true, {});
    }
}

void DeviceRemovalController::finishUnmountRequest(const QString &udi,
                                                   bool success,
                                                   const QString &errorMessage)
{
    auto it = m_pendingUnmounts.find(udi);
    if (it == m_pendingUnmounts.end()) {
        return;
    }

    PendingRequest req = std::move(it.value());
    m_pendingUnmounts.erase(it);

    Q_EMIT unmountFinished(udi, success);

    for (const auto &cb : req.callbacks) {
        if (cb) {
            cb(success, errorMessage);
        }
    }
}

void DeviceRemovalController::ejectDevice(const QString &udi, RemovalCallback callback)
{
    if (udi.isEmpty()) {
        if (callback) {
            callback(false, trLocal("Nieprawidłowy identyfikator urządzenia.",
                                    "Invalid device identifier."));
        }
        return;
    }

    if (m_solidActive) {
        Solid::Device current(udi);
        Solid::OpticalDrive *optical = nullptr;
        while (current.isValid()) {
            if (current.is<Solid::OpticalDrive>()) {
                optical = current.as<Solid::OpticalDrive>();
                break;
            }
            current = current.parent();
        }

        if (!optical) {
            if (callback) {
                callback(false, trLocal("Urządzenie nie obsługuje wysuwania nośnika.",
                                        "Device does not support media ejection."));
            }
            return;
        }

        if (m_pendingEjects.contains(udi)) {
            if (callback) {
                m_pendingEjects[udi].callbacks.append(std::move(callback));
            }
            return;
        }

        PendingRequest req;
        req.udi = udi;
        if (callback) {
            req.callbacks.append(std::move(callback));
        }
        m_pendingEjects.insert(udi, req);
        Q_EMIT ejectStarted(udi);

        QPointer<DeviceRemovalController> self(this);
        connect(optical, &Solid::OpticalDrive::ejectDone, this,
                [self, udi](Solid::ErrorType error, const QVariant &, const QString &devUdi) {
            if (!self) return;
            const QString targetUdi = !devUdi.isEmpty() ? devUdi : udi;
            self->onEjectDone(static_cast<int>(error), targetUdi);
        });

        if (!optical->eject()) {
            finishEjectRequest(udi, false,
                               trLocal("Nie można rozpocząć wysuwania nośnika.",
                                       "Cannot initiate media ejection."));
        }
        return;
    }

    // Test mode:
    if (m_pendingEjects.contains(udi)) {
        if (callback) {
            m_pendingEjects[udi].callbacks.append(std::move(callback));
        }
        return;
    }

    PendingRequest req;
    req.udi = udi;
    if (callback) {
        req.callbacks.append(std::move(callback));
    }
    m_pendingEjects.insert(udi, req);
    Q_EMIT ejectStarted(udi);

    if (m_hooks.requestEject) {
        const bool started = m_hooks.requestEject(udi);
        if (!started) {
            finishEjectRequest(udi, false,
                               trLocal("Nie można rozpocząć wysuwania nośnika.",
                                       "Cannot initiate media ejection."));
        }
    }
}

void DeviceRemovalController::onEjectDone(int error, const QString &udi)
{
    if (!m_pendingEjects.contains(udi)) {
        return;
    }

    if (error != Solid::NoError) {
        finishEjectRequest(udi, false, solidErrorToString(error));
        return;
    }

    finishEjectRequest(udi, true, {});
}

void DeviceRemovalController::finishEjectRequest(const QString &udi,
                                                 bool success,
                                                 const QString &errorMessage)
{
    auto it = m_pendingEjects.find(udi);
    if (it == m_pendingEjects.end()) {
        return;
    }

    PendingRequest req = std::move(it.value());
    m_pendingEjects.erase(it);

    Q_EMIT ejectFinished(udi, success);

    for (const auto &cb : req.callbacks) {
        if (cb) {
            cb(success, errorMessage);
        }
    }
}

void DeviceRemovalController::safelyRemoveDevice(const QString &udi, RemovalCallback callback)
{
    if (udi.isEmpty()) {
        if (callback) {
            callback(false, trLocal("Nieprawidłowy identyfikator urządzenia.",
                                    "Invalid device identifier."));
        }
        return;
    }

    const QString driveUdi = findPhysicalDriveUdi(udi);
    const QString key = !driveUdi.isEmpty() ? driveUdi : udi;
    if (key.isEmpty()) {
        if (callback) {
            callback(false, trLocal("Nie znaleziono dysku fizycznego.",
                                    "Physical drive not found."));
        }
        return;
    }

    if (m_pendingSafelyRemoves.contains(key)) {
        if (callback) {
            m_pendingSafelyRemoves[key].callbacks.append(std::move(callback));
        }
        return;
    }

    PendingSafelyRemove req;
    req.volumeUdi = udi;
    req.physicalDriveUdi = key;
    if (callback) {
        req.callbacks.append(std::move(callback));
    }

    const QList<QString> mountedMemberVols = findMountedMemberVolumes(key);
    for (const auto &v : mountedMemberVols) {
        req.volumesToUnmount.insert(v);
    }

    m_pendingSafelyRemoves.insert(key, req);
    Q_EMIT safelyRemoveStarted(key);

    if (m_hooks.requestSafelyRemove) {
        const bool started = m_hooks.requestSafelyRemove(key);
        if (!started) {
            finishSafelyRemoveRequest(key, false,
                                      trLocal("Nie można rozpocząć bezpiecznego usuwania.",
                                              "Cannot initiate safe removal."));
            return;
        }
    }

    if (mountedMemberVols.isEmpty()) {
        // Already unmounted removable drive: proceed directly to PowerOff
        executePowerOff(key);
        return;
    }

    // Unmount all mounted member volumes using pure filesystem unmount
    for (const QString &volUdi : mountedMemberVols) {
        unmountMemberForSafelyRemove(key, volUdi);
    }
}

void DeviceRemovalController::unmountMemberForSafelyRemove(const QString &driveUdi, const QString &volUdi)
{
    QPointer<DeviceRemovalController> self(this);
    unmountFilesystemViaUDisks(volUdi, [self, driveUdi, volUdi](bool success, const QString &errorMessage) {
        if (!self) return;
        self->onMemberUnmountFinished(driveUdi, volUdi, success, errorMessage);
    });
}

void DeviceRemovalController::onMemberUnmountFinished(const QString &driveUdi,
                                                      const QString &volUdi,
                                                      bool success,
                                                      const QString &errorMessage)
{
    auto it = m_pendingSafelyRemoves.find(driveUdi);
    if (it == m_pendingSafelyRemoves.end()) {
        return;
    }

    if (it->unmountedVolumes.contains(volUdi)) {
        return;
    }

    it->unmountedVolumes.insert(volUdi);
    if (!success) {
        it->unmountFailed = true;
        if (it->failureMessage.isEmpty()) {
            it->failureMessage = errorMessage;
        }
    }

    if (it->unmountedVolumes.contains(it->volumesToUnmount)) {
        if (it->unmountFailed) {
            const QString msg = !it->failureMessage.isEmpty()
                ? it->failureMessage
                : trLocal("Odmontowanie partycji nie powiodło się.",
                          "Partition unmount failed.");
            finishSafelyRemoveRequest(driveUdi, false, msg);
        } else {
            executePowerOff(driveUdi);
        }
    }
}

void DeviceRemovalController::executePowerOff(const QString &driveUdi)
{
    if (m_hooks.executePowerOff) {
        m_hooks.executePowerOff(driveUdi);
        return;
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.UDisks2"),
        driveUdi,
        QStringLiteral("org.freedesktop.UDisks2.Drive"),
        QStringLiteral("PowerOff"));

    QVariantMap options;
    msg << options;

    QDBusPendingCall async = QDBusConnection::systemBus().asyncCall(msg);
    auto *watcher = new QDBusPendingCallWatcher(async, this);

    QPointer<DeviceRemovalController> self(this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [self, driveUdi, watcher](QDBusPendingCallWatcher *call) {
        watcher->deleteLater();
        if (!self) return;

        QDBusPendingReply<> reply = *call;
        if (reply.isError()) {
            self->onPowerOffDone(driveUdi, false, reply.error().message());
        } else {
            self->onPowerOffDone(driveUdi, true, {});
        }
    });
}

void DeviceRemovalController::onPowerOffDone(const QString &driveUdi, bool success, const QString &errorMessage)
{
    if (!m_pendingSafelyRemoves.contains(driveUdi)) {
        return;
    }

    finishSafelyRemoveRequest(driveUdi, success, errorMessage);
}

void DeviceRemovalController::onDeviceRemoved(const QString &udi)
{
    // If device was removed while unmount was in flight:
    if (m_pendingUnmounts.contains(udi)) {
        finishUnmountRequest(udi, true, {});
    }

    // If device was removed while eject was in flight:
    if (m_pendingEjects.contains(udi)) {
        finishEjectRequest(udi, true, {});
    }

    // If physical drive or member volume was removed while safely remove was in flight:
    if (m_pendingSafelyRemoves.contains(udi)) {
        finishSafelyRemoveRequest(udi, true, {});
    } else {
        // Check if any pending safely remove contains this volume or drive
        for (auto it = m_pendingSafelyRemoves.begin(); it != m_pendingSafelyRemoves.end(); ++it) {
            if (it->volumeUdi == udi || it->physicalDriveUdi == udi || it->volumesToUnmount.contains(udi)) {
                finishSafelyRemoveRequest(it.key(), true, {});
                break;
            }
        }
    }
}

void DeviceRemovalController::finishSafelyRemoveRequest(const QString &key,
                                                        bool success,
                                                        const QString &errorMessage)
{
    auto it = m_pendingSafelyRemoves.find(key);
    if (it == m_pendingSafelyRemoves.end()) {
        return;
    }

    PendingSafelyRemove req = std::move(it.value());
    m_pendingSafelyRemoves.erase(it);

    Q_EMIT safelyRemoveFinished(key, success);

    for (const auto &cb : req.callbacks) {
        if (cb) {
            cb(success, errorMessage);
        }
    }
}

void DeviceRemovalController::simulateUnmountDone(const QString &udi,
                                                  bool success,
                                                  const QString &errorMessage)
{
    if (m_pendingFilesystemUnmounts.contains(udi)) {
        auto cbs = std::move(m_pendingFilesystemUnmounts[udi]);
        m_pendingFilesystemUnmounts.remove(udi);
        for (const auto &cb : cbs) {
            if (cb) cb(success, errorMessage);
        }
    }

    if (m_pendingUnmounts.contains(udi)) {
        if (success) {
            finishUnmountRequest(udi, true, {});
        } else {
            const QString err = !errorMessage.isEmpty()
                ? errorMessage
                : trLocal("Operacja odmontowywania nie powiodła się.", "Unmount operation failed.");
            finishUnmountRequest(udi, false, err);
        }
    }
}

void DeviceRemovalController::simulateTeardownDone(const QString &udi,
                                                   bool success,
                                                   const QString &errorMessage)
{
    simulateUnmountDone(udi, success, errorMessage);
}

void DeviceRemovalController::simulateAccessibilityChanged(const QString &udi, bool accessible)
{
    onAccessibilityChanged(accessible, udi);
}

void DeviceRemovalController::simulateEjectDone(const QString &udi,
                                                bool success,
                                                const QString &errorMessage)
{
    if (!m_pendingEjects.contains(udi)) {
        return;
    }

    if (success) {
        finishEjectRequest(udi, true, {});
    } else {
        const QString err = !errorMessage.isEmpty()
            ? errorMessage
            : trLocal("Operacja wysuwania nie powiodła się.", "Eject operation failed.");
        finishEjectRequest(udi, false, err);
    }
}

void DeviceRemovalController::simulatePowerOffDone(const QString &driveUdi,
                                                   bool success,
                                                   const QString &errorMessage)
{
    if (m_pendingSafelyRemoves.contains(driveUdi)) {
        finishSafelyRemoveRequest(driveUdi, success, errorMessage);
        return;
    }

    for (auto it = m_pendingSafelyRemoves.begin(); it != m_pendingSafelyRemoves.end(); ++it) {
        if (it->volumeUdi == driveUdi || it->physicalDriveUdi == driveUdi) {
            finishSafelyRemoveRequest(it.key(), success, errorMessage);
            return;
        }
    }
}

void DeviceRemovalController::simulateSafelyRemoveMemberUnmountDone(const QString &driveUdi,
                                                                    const QString &volUdi,
                                                                    bool success,
                                                                    const QString &errorMessage)
{
    if (m_pendingFilesystemUnmounts.contains(volUdi)) {
        auto cbs = std::move(m_pendingFilesystemUnmounts[volUdi]);
        m_pendingFilesystemUnmounts.remove(volUdi);
        for (const auto &cb : cbs) {
            if (cb) cb(success, errorMessage);
        }
        return;
    }

    if (m_pendingSafelyRemoves.contains(driveUdi)) {
        onMemberUnmountFinished(driveUdi, volUdi, success, errorMessage);
        return;
    }

    for (auto it = m_pendingSafelyRemoves.begin(); it != m_pendingSafelyRemoves.end(); ++it) {
        if (it->volumeUdi == driveUdi || it->physicalDriveUdi == driveUdi) {
            onMemberUnmountFinished(it.key(), volUdi, success, errorMessage);
            return;
        }
    }
}

void DeviceRemovalController::simulateSafelyRemoveMemberTeardownDone(const QString &driveUdi,
                                                                    const QString &volUdi,
                                                                    bool success,
                                                                    const QString &errorMessage)
{
    simulateSafelyRemoveMemberUnmountDone(driveUdi, volUdi, success, errorMessage);
}

void DeviceRemovalController::simulateDeviceRemoved(const QString &udi)
{
    onDeviceRemoved(udi);
}
