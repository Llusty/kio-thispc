/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <KIO/UDSEntry>

#include <QObject>
#include <QList>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

class DriveHomeSource
{
public:
    using EntriesCallback = std::function<void(const KIO::UDSEntryList &)>;
    using FinishedCallback = std::function<void(bool, const QString &)>;

    virtual ~DriveHomeSource() = default;
    virtual void cancel() = 0;
};

class DriveHomeCoordinator final : public QObject
{
    Q_OBJECT

public:
    using SourceFactory = std::function<std::unique_ptr<DriveHomeSource>(
        DriveHomeSource::EntriesCallback,
        DriveHomeSource::FinishedCallback)>;

    explicit DriveHomeCoordinator(QObject *parent = nullptr);
    DriveHomeCoordinator(SourceFactory sourceFactory, QObject *parent = nullptr);
    ~DriveHomeCoordinator() override;

    const QList<DriveInfo> &drives() const { return m_drives; }
    bool isLoading() const { return static_cast<bool>(m_source); }
    void refresh();
    void cancel();
#ifdef THISPC_TEST_HARNESS
    void setSnapshotForTesting(QList<DriveInfo> drives);
#endif

Q_SIGNALS:
    void loadingStarted();
    void loadingFinished(bool success);
    void drivesChanged(const QList<DriveInfo> &drives);
    void error(const QString &message);

private:
    void receiveEntries(quint64 generation, const KIO::UDSEntryList &entries);
    void finish(quint64 generation, bool success, const QString &message);

    SourceFactory m_sourceFactory;
    std::unique_ptr<DriveHomeSource> m_source;
    QList<DriveInfo> m_drives;
    QList<DriveInfo> m_pendingDrives;
    QSet<QString> m_pendingTargets;
    quint64 m_generation = 0;
    bool m_hasLoaded = false;
};
