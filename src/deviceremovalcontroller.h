/*
 * DeviceRemovalController — manages asynchronous unmounting, media ejection,
 * and safe physical removal of storage devices via KDE Solid and UDisks2 D-Bus
 * with strictly exactly-once completion semantics.
 *
 * Stage 3 of 0.35.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

class DeviceRemovalController : public QObject
{
    Q_OBJECT

public:
    using RemovalCallback = std::function<void(bool success, const QString &errorMessage)>;

    struct Hooks {
        std::function<void(DeviceRemovalController *)> onReady;
        std::function<bool(const QString &udi)> requestUnmount;
        std::function<bool(const QString &udi)> requestEject;
        std::function<bool(const QString &udi)> requestSafelyRemove;
        std::function<bool(const QString &udi)> canEject;
        std::function<bool(const QString &udi)> canSafelyRemove;
        std::function<QString(const QString &volumeUdi)> findPhysicalDriveUdi;
        std::function<QList<QString>(const QString &driveUdi)> findMountedMemberVolumes;
        std::function<void(const QString &driveUdi)> executePowerOff;
        std::function<void(const QString &volumeUdi,
                           std::function<void(bool success, const QString &errorMessage)> done)> executeFilesystemUnmount;
        std::function<void(const QString &udi)> executeSolidTeardown;
    };

    // Production constructor: connects to real Solid and system DBus.
    explicit DeviceRemovalController(QObject *parent = nullptr);

    // Test constructor: skips real Solid and DBus calls.
    explicit DeviceRemovalController(Hooks hooks, QObject *parent = nullptr);

    ~DeviceRemovalController() override;

    // Requests asynchronous filesystem unmount of a storage volume via UDisks2.
    void unmountDevice(const QString &udi, RemovalCallback callback);

    // Requests media eject (optical drive).
    void ejectDevice(const QString &udi, RemovalCallback callback);

    // Requests safe physical removal (unmounts all partitions, then powers off drive).
    void safelyRemoveDevice(const QString &udi, RemovalCallback callback);

    bool isOperationPending(const QString &udi) const;
    bool isUnmountPending(const QString &udi) const;
    bool isEjectPending(const QString &udi) const;
    bool isSafelyRemovePending(const QString &udi) const;

    // Capability queries:
    bool canEject(const QString &udi) const;
    bool canSafelyRemove(const QString &udi) const;

    // Test simulation helpers:
    void setHooksForTesting(Hooks hooks);
    void simulateUnmountDone(const QString &udi,
                             bool success,
                             const QString &errorMessage = {});
    void simulateTeardownDone(const QString &udi,
                              bool success,
                              const QString &errorMessage = {});
    void simulateAccessibilityChanged(const QString &udi, bool accessible);
    void simulateEjectDone(const QString &udi,
                           bool success,
                           const QString &errorMessage = {});
    void simulatePowerOffDone(const QString &driveUdi,
                              bool success,
                              const QString &errorMessage = {});
    void simulateSafelyRemoveMemberUnmountDone(const QString &driveUdi,
                                               const QString &volUdi,
                                               bool success,
                                               const QString &errorMessage = {});
    void simulateSafelyRemoveMemberTeardownDone(const QString &driveUdi,
                                                const QString &volUdi,
                                                bool success,
                                                const QString &errorMessage = {});
    void simulateDeviceRemoved(const QString &udi);

Q_SIGNALS:
    void unmountStarted(const QString &udi);
    void unmountFinished(const QString &udi, bool success);
    void ejectStarted(const QString &udi);
    void ejectFinished(const QString &udi, bool success);
    void safelyRemoveStarted(const QString &udi);
    void safelyRemoveFinished(const QString &udi, bool success);

private:
    struct PendingRequest {
        QString udi;
        QList<RemovalCallback> callbacks;
    };

    struct PendingSafelyRemove {
        QString volumeUdi;
        QString physicalDriveUdi;
        QSet<QString> volumesToUnmount;
        QSet<QString> unmountedVolumes;
        bool unmountFailed = false;
        QString failureMessage;
        QList<RemovalCallback> callbacks;
    };

    void initSolid();
    void onAccessibilityChanged(bool accessible, const QString &udi);
    void onEjectDone(int error, const QString &udi);
    void onPowerOffDone(const QString &driveUdi, bool success, const QString &errorMessage);
    void onDeviceRemoved(const QString &udi);

    void finishUnmountRequest(const QString &udi, bool success, const QString &errorMessage);
    void finishEjectRequest(const QString &udi, bool success, const QString &errorMessage);
    void finishSafelyRemoveRequest(const QString &key, bool success, const QString &errorMessage);

    QString findPhysicalDriveUdi(const QString &volumeUdi) const;
    QList<QString> findMountedMemberVolumes(const QString &physicalDriveUdi) const;

    void unmountFilesystemViaUDisks(
        const QString &volumeUdi,
        std::function<void(bool success, const QString &errorMessage)> completion);
    void unmountMemberForSafelyRemove(const QString &driveUdi, const QString &volUdi);
    void onMemberUnmountFinished(const QString &driveUdi,
                                 const QString &volUdi,
                                 bool success,
                                 const QString &errorMessage);
    void executePowerOff(const QString &driveUdi);

    bool m_solidActive = false;
    Hooks m_hooks;

    QHash<QString, PendingRequest> m_pendingUnmounts;
    QHash<QString, PendingRequest> m_pendingEjects;
    QHash<QString, PendingSafelyRemove> m_pendingSafelyRemoves;
    QHash<QString, QList<std::function<void(bool, const QString &)>>> m_pendingFilesystemUnmounts;
};
