/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "drivehomecoordinator.h"

#include <KIO/ListJob>
#include <KIO/JobUiDelegateFactory>
#include <KJob>

#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace
{
class KioDriveHomeSource final : public DriveHomeSource
{
public:
    KioDriveHomeSource(EntriesCallback entriesCallback, FinishedCallback finishedCallback)
        : m_entriesCallback(std::move(entriesCallback))
        , m_finishedCallback(std::move(finishedCallback))
    {
        m_job = KIO::listDir(QUrl(QStringLiteral("thispc:/")), KIO::HideProgressInfo);
        m_job->setUiDelegate(nullptr);
        QObject::connect(m_job, &KIO::ListJob::entries, m_job,
            [this](KIO::Job *, const KIO::UDSEntryList &entries) {
                if (m_entriesCallback) m_entriesCallback(entries);
            });
        QObject::connect(m_job, &KJob::result, m_job, [this](KJob *job) {
            const bool success = !job->error();
            const QString message = job->errorText();
            const FinishedCallback finished = m_finishedCallback;
            m_job = nullptr;
            QTimer::singleShot(0, [finished, success, message] {
                if (finished) finished(success, message);
            });
        });
    }

    ~KioDriveHomeSource() override { cancel(); }

    void cancel() override
    {
        if (!m_job) return;
        QObject::disconnect(m_job, nullptr, nullptr, nullptr);
        m_job->kill();
        m_job = nullptr;
    }

private:
    QPointer<KIO::ListJob> m_job;
    EntriesCallback m_entriesCallback;
    FinishedCallback m_finishedCallback;
};

DriveHomeCoordinator::SourceFactory defaultSourceFactory()
{
    return [](DriveHomeSource::EntriesCallback entries,
              DriveHomeSource::FinishedCallback finished) {
        return std::make_unique<KioDriveHomeSource>(
            std::move(entries), std::move(finished));
    };
}

QString targetIdentity(const QUrl &url)
{
    return url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash)
        .toString(QUrl::FullyEncoded);
}
}

DriveHomeCoordinator::DriveHomeCoordinator(QObject *parent)
    : DriveHomeCoordinator(defaultSourceFactory(), parent)
{
}

DriveHomeCoordinator::DriveHomeCoordinator(SourceFactory sourceFactory, QObject *parent)
    : QObject(parent)
    , m_sourceFactory(std::move(sourceFactory))
{
}

DriveHomeCoordinator::~DriveHomeCoordinator()
{
    cancel();
}

void DriveHomeCoordinator::refresh()
{
    cancel();
    const quint64 generation = ++m_generation;
    m_pendingDrives.clear();
    m_pendingTargets.clear();
    Q_EMIT loadingStarted();

    QPointer<DriveHomeCoordinator> self(this);
    m_source = m_sourceFactory(
        [self, generation](const KIO::UDSEntryList &entries) {
            if (self) self->receiveEntries(generation, entries);
        },
        [self, generation](bool success, const QString &message) {
            if (self) self->finish(generation, success, message);
        });
}

void DriveHomeCoordinator::cancel()
{
    ++m_generation;
    if (m_source) {
        m_source->cancel();
        m_source.reset();
    }
    m_pendingDrives.clear();
    m_pendingTargets.clear();
}

#ifdef THISPC_TEST_HARNESS
void DriveHomeCoordinator::setSnapshotForTesting(QList<DriveInfo> drives)
{
    cancel();
    m_hasLoaded = true;
    m_drives = std::move(drives);
    Q_EMIT drivesChanged(m_drives);
}
#endif

void DriveHomeCoordinator::receiveEntries(
    quint64 generation, const KIO::UDSEntryList &entries)
{
    if (generation != m_generation || !m_source) return;

    for (const KIO::UDSEntry &entry : entries) {
        const QString name = entry.stringValue(KIO::UDSEntry::UDS_DISPLAY_NAME);
        const QString id = entry.stringValue(KIO::UDSEntry::UDS_NAME);
        const QString target = entry.stringValue(KIO::UDSEntry::UDS_TARGET_URL);
        const QString mountPoint = entry.stringValue(KIO::UDSEntry::UDS_EXTRA + 4);
        const QString mountedFlag = entry.stringValue(KIO::UDSEntry::UDS_EXTRA + 5);
        const QString udi = entry.stringValue(KIO::UDSEntry::UDS_EXTRA + 6);
        QString removableFlag = entry.stringValue(KIO::UDSEntry::UDS_EXTRA + 8);
        if (removableFlag.isEmpty()) {
            removableFlag = entry.stringValue(KIO::UDSEntry::UDS_EXTRA + 7);
        }

        if (id == QStringLiteral(".") || id == QStringLiteral("..")
            || name.isEmpty() || name == QStringLiteral(".")) {
            continue;
        }

        bool isMounted = true;
        if (!mountedFlag.isEmpty()) {
            isMounted = (mountedFlag == QStringLiteral("1"));
        } else {
            isMounted = !target.isEmpty() && !mountPoint.isEmpty();
        }

        QUrl targetUrl;
        if (isMounted) {
            targetUrl = QUrl(target);
            if (target.isEmpty() || !targetUrl.isValid()) {
                continue;
            }
        } else {
            // For unmounted volumes, targetUrl remains empty/invalid per Stage 2 requirements.
            // An unmounted volume requires a valid Solid UDI for device operations.
            if (udi.isEmpty()) {
                continue;
            }
        }

        QString identity;
        if (!id.isEmpty()) {
            identity = id;
        } else if (!udi.isEmpty()) {
            identity = udi;
        } else if (targetUrl.isValid()) {
            identity = targetIdentity(targetUrl);
        }

        if (identity.isEmpty() || m_pendingTargets.contains(identity)) continue;
        m_pendingTargets.insert(identity);

        DriveInfo drive;
        drive.id = id;
        drive.udi = udi;
        drive.isMounted = isMounted;
        drive.isRemovable = (removableFlag == QStringLiteral("1"));
        drive.name = name;
        drive.targetUrl = targetUrl;
        drive.iconName = entry.stringValue(KIO::UDSEntry::UDS_ICON_NAME);
        drive.freeText = entry.stringValue(KIO::UDSEntry::UDS_EXTRA + 0);
        drive.capacityText = entry.stringValue(KIO::UDSEntry::UDS_EXTRA + 1);
        drive.usedText = entry.stringValue(KIO::UDSEntry::UDS_EXTRA + 2);
        drive.fileSystem = entry.stringValue(KIO::UDSEntry::UDS_EXTRA + 3);
        drive.mountPoint = mountPoint;

        QString percentText = drive.usedText;
        percentText.remove(QLatin1Char('%'));
        bool ok = false;
        const int percent = percentText.toInt(&ok);
        drive.usedPercent = ok ? std::clamp(percent, 0, 100) : 0;
        m_pendingDrives.push_back(drive);
    }
}

void DriveHomeCoordinator::finish(
    quint64 generation, bool success, const QString &message)
{
    if (generation != m_generation || !m_source) return;
    m_source.reset();

    if (!success) {
        m_pendingDrives.clear();
        m_pendingTargets.clear();
        Q_EMIT error(message);
        Q_EMIT loadingFinished(false);
        return;
    }

    const bool changed = !m_hasLoaded || (m_drives != m_pendingDrives);
    m_hasLoaded = true;
    m_drives = std::move(m_pendingDrives);
    m_pendingTargets.clear();
    if (changed) {
        Q_EMIT drivesChanged(m_drives);
    }
    Q_EMIT loadingFinished(true);
}
