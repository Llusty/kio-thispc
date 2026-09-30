/*
 * DeviceMountController — manages asynchronous mounting of storage volumes
 * via KDE Solid with strictly exactly-once completion semantics.
 *
 * Stage 2 of 0.35.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class DeviceMountController : public QObject
{
    Q_OBJECT

public:
    using MountCallback = std::function<void(bool success,
                                             const QString &mountPoint,
                                             const QString &errorMessage)>;

    struct Hooks {
        std::function<void(DeviceMountController *)> onReady;
        std::function<bool(const QString &udi)> requestMount;
    };

    // Production constructor: connects to real Solid interfaces.
    explicit DeviceMountController(QObject *parent = nullptr);

    // Test constructor: skips real Solid calls, allowing simulated lifecycle.
    explicit DeviceMountController(Hooks hooks, QObject *parent = nullptr);

    ~DeviceMountController() override;

    // Requests asynchronous mount for volume with the given Solid UDI.
    // If already mounted, invokes callback immediately.
    // Multiple requests for the same UDI while pending attach to the ongoing request.
    void mountDevice(const QString &udi, MountCallback callback);

    bool isMountPending(const QString &udi) const;

    // Test simulation helpers:
    void setHooksForTesting(Hooks hooks);
    void simulateSetupDone(const QString &udi,
                           bool success,
                           const QString &mountPoint,
                           const QString &errorMessage = {});
    void simulateAccessibilityChanged(const QString &udi,
                                      bool accessible,
                                      const QString &mountPoint);
    void simulateDeviceRemoved(const QString &udi);

Q_SIGNALS:
    void mountStarted(const QString &udi);
    void mountFinished(const QString &udi, bool success);

private:
    struct PendingRequest {
        QString udi;
        QList<MountCallback> callbacks;
    };

    void initSolid();
    void onSetupDone(int error, const QString &udi);
    void onAccessibilityChanged(bool accessible, const QString &udi);
    void onDeviceRemoved(const QString &udi);
    void finishRequest(const QString &udi,
                       bool success,
                       const QString &mountPoint,
                       const QString &errorMessage);

    bool m_solidActive = false;
    Hooks m_hooks;
    QHash<QString, PendingRequest> m_pendingRequests;
};
