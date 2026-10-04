/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "checksumjob.h"
#include "duplicatefinderdata.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

class DuplicateFinderJob final : public QObject
{
    Q_OBJECT

public:
    using JobFactory = std::function<ChecksumJob*(const QUrl &, QObject *, const ChecksumJobOptions &)>;

    explicit DuplicateFinderJob(QString rootPath,
                                QList<StorageScanEntry> entries,
                                StorageScanStats sourceStats = {},
                                StorageScanState sourceState = StorageScanState::Completed,
                                QObject *parent = nullptr)
        : QObject(parent)
        , m_rootPath(std::move(rootPath))
        , m_entries(std::move(entries))
        , m_sourceStats(sourceStats)
        , m_sourceState(sourceState)
    {
        qRegisterMetaType<DuplicateFinderState>();
        qRegisterMetaType<DuplicateFinderStats>();
        qRegisterMetaType<DuplicateFinderResult>();

        m_jobFactory = [](const QUrl &url, QObject *p, const ChecksumJobOptions &opt) {
            return new ChecksumJob(url, p, opt);
        };

        m_plan = DuplicatePlanBuilder::buildPlan(m_rootPath, m_entries);
    }

    ~DuplicateFinderJob() override
    {
        cancelInternal(false);
    }

    void setJobFactory(JobFactory factory)
    {
        if (factory) {
            m_jobFactory = std::move(factory);
        }
    }

    const DuplicateCandidatePlan &plan() const { return m_plan; }
    DuplicateFinderState state() const { return m_state; }
    const DuplicateFinderStats &stats() const { return m_stats; }
    const DuplicateFinderResult &result() const { return m_result; }
    quint64 runId() const { return m_runId; }

    bool start()
    {
        if (m_state == DuplicateFinderState::Running) {
            return false;
        }

        m_runId++;
        const quint64 currentRunId = m_runId;

        m_state = DuplicateFinderState::Running;
        m_stats = DuplicateFinderStats();
        m_stats.totalCandidateFiles = m_plan.candidateFiles;
        m_stats.totalCandidateBytes = m_plan.candidateBytes;

        m_result = DuplicateFinderResult();
        m_result.rootPath = m_rootPath;
        m_result.sourceScanState = m_sourceState;
        m_result.sourceScanStats = m_sourceStats;

        if (m_sourceState == StorageScanState::Cancelled) {
            m_result.isSourcePartial = true;
            m_result.sourcePartialReason = tr("Wyniki częściowe — skanowanie anulowane");
        } else if (m_sourceStats.inaccessible > 0 || m_sourceStats.disappeared > 0 ||
                   m_sourceStats.errors > 0 || m_sourceStats.skippedMounts > 0) {
            m_result.isSourcePartial = true;
            m_result.sourcePartialReason = tr("Wyniki mogą być niepełne");
        }

        m_groupHashes.clear();
        m_queue.clear();

        for (int gIdx = 0; gIdx < m_plan.candidateSizeGroups.size(); ++gIdx) {
            const auto &group = m_plan.candidateSizeGroups.at(gIdx);
            for (const auto &file : group) {
                m_queue.append({gIdx, file});
            }
        }

        Q_EMIT stateChanged(m_state);
        Q_EMIT progress(0, m_stats.totalCandidateFiles, 0, m_stats.totalCandidateBytes, QString());

        if (m_queue.isEmpty()) {
            QTimer::singleShot(0, this, [this, currentRunId]() {
                finishRun(currentRunId);
            });
            return true;
        }

        processNextQueueItem(currentRunId);
        return true;
    }

    void cancel()
    {
        cancelInternal(true);
    }

Q_SIGNALS:
    void stateChanged(DuplicateFinderState state);
    void progress(quint64 resolvedFiles, quint64 totalFiles,
                  quint64 resolvedBytes, quint64 totalBytes,
                  const QString &currentFilePath);
    void finished(const DuplicateFinderResult &result);

private:
    struct QueueItem {
        int groupIndex = 0;
        DuplicatePhysicalFile file;
    };

    void cancelInternal(bool emitSignals)
    {
        if (m_state != DuplicateFinderState::Running) {
            return;
        }

        m_runId++; // Invalidates callbacks from any running job

        if (m_activeJob) {
            m_activeJob->disconnect(this);
            m_activeJob->cancel();
            m_activeJob->deleteLater();
            m_activeJob = nullptr;
        }

        m_queue.clear();
        m_state = DuplicateFinderState::Cancelled;

        assembleGroups();

        m_result.finderState = m_state;
        m_result.finderStats = m_stats;
        m_result.isFinderPartial = true;
        m_result.finderPartialReason = tr("Wyniki częściowe — wyszukiwanie duplikatów anulowane");

        if (emitSignals) {
            Q_EMIT stateChanged(m_state);
            Q_EMIT finished(m_result);
        }
    }

    void processNextQueueItem(quint64 runId)
    {
        if (m_runId != runId || m_state != DuplicateFinderState::Running) {
            return;
        }

        if (m_queue.isEmpty()) {
            finishRun(runId);
            return;
        }

        const QueueItem item = m_queue.takeFirst();
        const QString path = item.file.canonicalPath;

        ChecksumJobOptions options;
        options.algorithm = ChecksumAlgorithm::Sha256;
        options.verifyExpectedSnapshot = true;
        options.expectedDevice = item.file.deviceId;
        options.expectedInode = item.file.inode;
        options.expectedSize = item.file.logicalSize;
        options.expectedMtimeSec = item.file.mtimeSec;
        options.expectedMtimeNsec = item.file.mtimeNsec;
        options.expectedCtimeSec = item.file.ctimeSec;
        options.expectedCtimeNsec = item.file.ctimeNsec;

        ChecksumJob *job = m_jobFactory(QUrl::fromLocalFile(path), this, options);
        m_activeJob = job;

        connect(job, &ChecksumJob::progress, this, [this, runId, item](quint64 done, quint64 total) {
            Q_UNUSED(total);
            if (m_runId != runId || m_state != DuplicateFinderState::Running) {
                return;
            }
            const quint64 currentWork = saturatingAdd(m_stats.resolvedBytes, done);
            Q_EMIT progress(m_stats.resolvedFiles, m_stats.totalCandidateFiles,
                            currentWork, m_stats.totalCandidateBytes,
                            item.file.canonicalPath);
        });

        connect(job, &ChecksumJob::stateChanged, this, [this, runId, item, job](const ChecksumData &data) {
            if (m_runId != runId || m_state != DuplicateFinderState::Running) {
                return;
            }

            if (data.state == ChecksumState::Running) {
                return;
            }

            if (data.state == ChecksumState::Completed && data.hasValidResult()) {
                m_stats.hashedFiles++;
                m_stats.hashedBytes = saturatingAdd(m_stats.hashedBytes, item.file.logicalSize);
                m_groupHashes[item.groupIndex][data.digest].append(item.file);
            } else if (data.state == ChecksumState::ChangedDuringHash) {
                m_stats.changedDuringHash++;
                m_stats.staleFiles++;
            } else if (data.capability == ChecksumCapability::Unreadable) {
                m_stats.unreadableFiles++;
            } else if (data.errorMessage.contains(QStringLiteral("No such file"), Qt::CaseInsensitive)) {
                m_stats.disappearedFiles++;
            } else {
                m_stats.errors++;
            }

            m_stats.resolvedFiles++;
            m_stats.resolvedBytes = saturatingAdd(m_stats.resolvedBytes, item.file.logicalSize);

            Q_EMIT progress(m_stats.resolvedFiles, m_stats.totalCandidateFiles,
                            m_stats.resolvedBytes, m_stats.totalCandidateBytes,
                            item.file.canonicalPath);

            if (m_activeJob == job) {
                m_activeJob = nullptr;
            }
            job->deleteLater();

            QTimer::singleShot(0, this, [this, runId]() {
                processNextQueueItem(runId);
            });
        });

        if (!job->start()) {
            // If job failed to start immediately (e.g. unreadable or unsupported URL)
            m_stats.errors++;
            m_stats.resolvedFiles++;
            m_stats.resolvedBytes = saturatingAdd(m_stats.resolvedBytes, item.file.logicalSize);
            m_activeJob = nullptr;
            job->deleteLater();

            Q_EMIT progress(m_stats.resolvedFiles, m_stats.totalCandidateFiles,
                            m_stats.resolvedBytes, m_stats.totalCandidateBytes,
                            item.file.canonicalPath);

            QTimer::singleShot(0, this, [this, runId]() {
                processNextQueueItem(runId);
            });
        }
    }

    void assembleGroups()
    {
        m_result.groups.clear();

        for (auto gIt = m_groupHashes.begin(); gIt != m_groupHashes.end(); ++gIt) {
            const auto &hashBuckets = gIt.value();
            for (auto hIt = hashBuckets.begin(); hIt != hashBuckets.end(); ++hIt) {
                auto members = hIt.value();
                if (members.size() >= 2) {
                    // Sort members deterministically by canonicalPath ASC
                    std::sort(members.begin(), members.end(), [](const DuplicatePhysicalFile &a, const DuplicatePhysicalFile &b) {
                        return a.canonicalPath < b.canonicalPath;
                    });

                    DuplicateGroup grp;
                    grp.logicalSize = members.first().logicalSize;
                    grp.sha256 = hIt.key();
                    grp.files = std::move(members);
                    m_result.groups.append(std::move(grp));
                }
            }
        }

        // Deterministic sorting of duplicate groups:
        // 1. Recoverable bytes DESC
        // 2. Logical size DESC
        // 3. Smallest canonical path ASC
        std::sort(m_result.groups.begin(), m_result.groups.end(), [](const DuplicateGroup &a, const DuplicateGroup &b) {
            const quint64 recA = a.recoverableBytes();
            const quint64 recB = b.recoverableBytes();
            if (recA != recB) {
                return recA > recB; // Recoverable DESC
            }
            if (a.logicalSize != b.logicalSize) {
                return a.logicalSize > b.logicalSize; // Size DESC
            }
            const QString &pathA = a.files.isEmpty() ? QString() : a.files.first().canonicalPath;
            const QString &pathB = b.files.isEmpty() ? QString() : b.files.first().canonicalPath;
            return pathA < pathB; // Path ASC
        });
    }

    void finishRun(quint64 runId)
    {
        if (m_runId != runId || m_state != DuplicateFinderState::Running) {
            return;
        }

        m_state = DuplicateFinderState::Completed;
        assembleGroups();

        m_result.finderState = m_state;
        m_result.finderStats = m_stats;

        if (m_stats.staleFiles > 0 || m_stats.disappearedFiles > 0 ||
            m_stats.unreadableFiles > 0 || m_stats.changedDuringHash > 0 || m_stats.errors > 0) {
            m_result.isFinderPartial = true;
            m_result.finderPartialReason = tr("Niektóre pliki uległy zmianie lub były niedostępne");
        }

        Q_EMIT progress(m_stats.resolvedFiles, m_stats.totalCandidateFiles,
                        m_stats.totalCandidateBytes, m_stats.totalCandidateBytes,
                        QString());
        Q_EMIT stateChanged(m_state);
        Q_EMIT finished(m_result);
    }

    QString m_rootPath;
    QList<StorageScanEntry> m_entries;
    StorageScanStats m_sourceStats;
    StorageScanState m_sourceState;

    DuplicateCandidatePlan m_plan;
    DuplicateFinderState m_state = DuplicateFinderState::Idle;
    DuplicateFinderStats m_stats;
    DuplicateFinderResult m_result;

    quint64 m_runId = 0;
    JobFactory m_jobFactory;
    QPointer<ChecksumJob> m_activeJob;
    QList<QueueItem> m_queue;
    QHash<int, QHash<QString, QList<DuplicatePhysicalFile>>> m_groupHashes;
};
