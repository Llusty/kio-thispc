/*
 * Safe Split View pane synchronization executor and preflight validator.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "localfilecopyjob.h"
#include "localfileoverwrite.h"
#include "splitsyncmodel.h"

#include <KIO/CopyJob>
#include <KIO/Global>
#include <KIO/Job>

#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <sys/stat.h>
#include <unistd.h>

inline bool isSafeSyncEntryName(const QString &name)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return false;
    }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        return false;
    }
    return true;
}

struct SyncPreflightIssue {
    QString name;
    QString reason;
};

struct SyncPreflightResult {
    bool isValid = true;
    QList<SyncPreflightIssue> issues;

    QString formatSummary() const
    {
        if (isValid) return QString();
        QStringList lines;
        for (const auto &issue : issues) {
            lines.append(QStringLiteral("• %1: %2").arg(issue.name, issue.reason));
        }
        return lines.join(QStringLiteral("\n"));
    }
};

inline SyncPreflightResult revalidateSyncPlan(
    const QUrl &sourceBaseUrl,
    const QUrl &destBaseUrl,
    const QList<SyncPlanEntry> &plan)
{
    SyncPreflightResult result;

    for (const SyncPlanEntry &entry : plan) {
        if (!entry.isAutoExecutable) {
            continue;
        }

        if (!isSafeSyncEntryName(entry.name)) {
            result.isValid = false;
            result.issues.append({entry.name, trLocal("Niebezpieczna lub nieprawidłowa nazwa pliku", "Unsafe or invalid filename")});
            continue;
        }

        const QUrl srcUrl = childUrlWithName(sourceBaseUrl, entry.name);
        const QUrl dstUrl = childUrlWithName(destBaseUrl, entry.name);

        if (srcUrl.isLocalFile() && dstUrl.isLocalFile()) {
            const QString srcPath = srcUrl.toLocalFile();
            const QString dstPath = dstUrl.toLocalFile();

            struct stat srcStat {};
            const bool srcExists = (::lstat(QFile::encodeName(srcPath).constData(), &srcStat) == 0);

            struct stat dstStat {};
            const bool dstExists = (::lstat(QFile::encodeName(dstPath).constData(), &dstStat) == 0);

            if (entry.action == SyncPlanAction::CopyFile) {
                // Source must exist and be regular file matching snapshot
                if (!srcExists) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik źródłowy nie istnieje (zniknął przed wykonaniem)", "Source file does not exist (disappeared before execution)")});
                    continue;
                }
                if (!S_ISREG(srcStat.st_mode)) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik źródłowy zmienił typ (nie jest zwykłym plikiem)", "Source file changed type (not a regular file)")});
                    continue;
                }
                if (entry.sourceInfo.size >= 0 && static_cast<qint64>(srcStat.st_size) != entry.sourceInfo.size) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik źródłowy zmienił rozmiar od czasu porównania", "Source file size changed since comparison")});
                    continue;
                }
                if (entry.sourceInfo.modificationTime > 0 && static_cast<qint64>(srcStat.st_mtim.tv_sec) != entry.sourceInfo.modificationTime) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik źródłowy został zmodyfikowany od czasu porównania", "Source file was modified since comparison")});
                    continue;
                }

                // Destination must NOT exist
                if (dstExists) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Element docelowy pojawił się po porównaniu", "Destination item appeared after comparison")});
                    continue;
                }
            } else if (entry.action == SyncPlanAction::UpdateFile) {
                // Source must exist and be regular file matching snapshot
                if (!srcExists) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik źródłowy nie istnieje (zniknął przed wykonaniem)", "Source file does not exist (disappeared before execution)")});
                    continue;
                }
                if (!S_ISREG(srcStat.st_mode)) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik źródłowy zmienił typ (nie jest zwykłym plikiem)", "Source file changed type (not a regular file)")});
                    continue;
                }
                if (entry.sourceInfo.size >= 0 && static_cast<qint64>(srcStat.st_size) != entry.sourceInfo.size) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik źródłowy zmienił rozmiar od czasu porównania", "Source file size changed since comparison")});
                    continue;
                }
                if (entry.sourceInfo.modificationTime > 0 && static_cast<qint64>(srcStat.st_mtim.tv_sec) != entry.sourceInfo.modificationTime) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik źródłowy został zmodyfikowany od czasu porównania", "Source file was modified since comparison")});
                    continue;
                }

                // Destination must exist and be regular file matching snapshot
                if (!dstExists) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik docelowy nie istnieje (zniknął przed wykonaniem)", "Destination file does not exist (disappeared before execution)")});
                    continue;
                }
                if (!S_ISREG(dstStat.st_mode)) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik docelowy zmienił typ (nie jest zwykłym plikiem)", "Destination file changed type (not a regular file)")});
                    continue;
                }
                if (entry.destinationInfo.size >= 0 && static_cast<qint64>(dstStat.st_size) != entry.destinationInfo.size) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik docelowy zmienił rozmiar od czasu porównania", "Destination file size changed since comparison")});
                    continue;
                }
                if (entry.destinationInfo.modificationTime > 0 && static_cast<qint64>(dstStat.st_mtim.tv_sec) != entry.destinationInfo.modificationTime) {
                    result.isValid = false;
                    result.issues.append({entry.name, trLocal("Plik docelowy został zmodyfikowany od czasu porównania", "Destination file was modified since comparison")});
                    continue;
                }
            }
        }
    }

    return result;
}

struct SyncItemError {
    QString name;
    SyncPlanAction action = SyncPlanAction::NoAction;
    QString errorMessage;
};

struct SyncExecutionReport {
    SyncDirection direction = SyncDirection::LeftToRight;
    int totalItemsInPlan = 0;
    int copiedCount = 0;
    int updatedCount = 0;
    int noActionCount = 0;
    int conflictCount = 0;
    int unsupportedCount = 0;
    int cancelledCount = 0;
    int errorCount = 0;
    bool wasCancelled = false;
    bool preflightFailed = false;
    QString preflightError;
    QList<SyncItemError> errors;
};

class SplitSyncExecutor : public QObject
{
    Q_OBJECT

public:
    explicit SplitSyncExecutor(
        const QUrl &sourceUrl,
        const QUrl &destUrl,
        const QList<SyncPlanEntry> &plan,
        SyncDirection direction,
        QObject *parent = nullptr)
        : QObject(parent)
        , m_sourceUrl(sourceUrl)
        , m_destUrl(destUrl)
        , m_plan(plan)
        , m_direction(direction)
    {
        for (const SyncPlanEntry &entry : m_plan) {
            if (entry.isAutoExecutable) {
                m_queue.append(entry);
            }
        }

        const SyncPlanSummary sum = summarizeSyncPlan(m_plan);
        m_report.direction = direction;
        m_report.totalItemsInPlan = sum.total;
        m_report.noActionCount = sum.noAction;
        m_report.conflictCount = sum.conflict;
        m_report.unsupportedCount = sum.unsupported;
    }

    ~SplitSyncExecutor() override
    {
        cancel();
    }

    SyncPreflightResult runPreflight() const
    {
        return revalidateSyncPlan(m_sourceUrl, m_destUrl, m_plan);
    }

public Q_SLOTS:
    void start()
    {
        if (m_started || m_finished) return;
        m_started = true;

        const SyncPreflightResult preflight = runPreflight();
        if (!preflight.isValid) {
            m_report.preflightFailed = true;
            m_report.preflightError = preflight.formatSummary();
            m_finished = true;
            Q_EMIT finished(m_report);
            return;
        }

        if (m_queue.isEmpty()) {
            m_finished = true;
            Q_EMIT finished(m_report);
            return;
        }

        processNext();
    }

    void cancel()
    {
        if (m_finished) return;
        m_cancelled = true;
        m_report.wasCancelled = true;

        if (m_currentJob) {
            // Request active job to stop and emit result to finalize accounting
            m_currentJob->kill(KJob::EmitResult);
            return;
        }

        // No active job running: account for all remaining items as cancelled
        const int remaining = m_queue.size() - m_currentIndex;
        if (remaining > 0) {
            m_report.cancelledCount += remaining;
        }

        m_finished = true;
        Q_EMIT finished(m_report);
    }

    bool isRunning() const { return m_started && !m_finished; }
    bool wasCancelled() const { return m_cancelled; }
    SyncExecutionReport report() const { return m_report; }

Q_SIGNALS:
    void progress(int current, int total, const SyncPlanEntry &entry, const QString &description);
    void finished(const SyncExecutionReport &report);

private:
    void processNext()
    {
        if (m_cancelled) {
            const int remaining = m_queue.size() - m_currentIndex;
            if (remaining > 0) {
                m_report.cancelledCount += remaining;
            }
            m_finished = true;
            Q_EMIT finished(m_report);
            return;
        }

        if (m_currentIndex >= m_queue.size()) {
            m_finished = true;
            Q_EMIT finished(m_report);
            return;
        }

        const SyncPlanEntry entry = m_queue.at(m_currentIndex);
        const int displayIndex = m_currentIndex + 1;
        const int totalCount = m_queue.size();

        const QString actionLabel = (entry.action == SyncPlanAction::CopyFile)
            ? trLocal("Kopiowanie", "Copying")
            : trLocal("Aktualizowanie", "Updating");
        const QString desc = QStringLiteral("%1: %2").arg(actionLabel, entry.name);

        Q_EMIT progress(displayIndex, totalCount, entry, desc);

        const QUrl srcFileUrl = childUrlWithName(m_sourceUrl, entry.name);
        const QUrl dstFileUrl = childUrlWithName(m_destUrl, entry.name);
        const bool isOverwrite = (entry.action == SyncPlanAction::UpdateFile);

        KIO::Job *job = nullptr;
        if (srcFileUrl.isLocalFile() && dstFileUrl.isLocalFile()) {
            LocalFileIdentity originalDest;
            if (isOverwrite) {
                originalDest = LocalFileIdentity::read(dstFileUrl.toLocalFile());
            }
            auto *copyJob = new LocalFileCopyJob(
                srcFileUrl, dstFileUrl, this,
                256 * 1024, 0, isOverwrite, false, originalDest);
            job = copyJob;
        } else {
            job = KIO::copyAs(srcFileUrl, dstFileUrl,
                              isOverwrite ? (KIO::Overwrite | KIO::HideProgressInfo) : KIO::HideProgressInfo);
        }

        m_currentJob = job;

        connect(job, &KJob::result, this, [this, entry](KJob *finishedJob) {
            if (m_currentJob != finishedJob) return;
            m_currentJob = nullptr;

            const bool hadError = (finishedJob->error() != 0);
            const bool wasJobCancelled = hadError && (finishedJob->error() == KIO::ERR_USER_CANCELED || finishedJob->error() == KJob::KilledJobError);

            if (!hadError) {
                // The transfer completed successfully and destination was published!
                if (entry.action == SyncPlanAction::CopyFile) {
                    ++m_report.copiedCount;
                } else if (entry.action == SyncPlanAction::UpdateFile) {
                    ++m_report.updatedCount;
                }
            } else if (wasJobCancelled || m_cancelled) {
                // The active transfer was cancelled before publication
                ++m_report.cancelledCount;
            } else {
                // Real transfer error
                ++m_report.errorCount;
                m_report.errors.append({entry.name, entry.action, finishedJob->errorString()});
            }

            ++m_currentIndex;

            if (m_cancelled) {
                // Account for strictly unstarted remaining items
                const int remaining = m_queue.size() - m_currentIndex;
                if (remaining > 0) {
                    m_report.cancelledCount += remaining;
                }
                m_finished = true;
                Q_EMIT finished(m_report);
                return;
            }

            processNext();
        });

        job->start();
    }

    QUrl m_sourceUrl;
    QUrl m_destUrl;
    QList<SyncPlanEntry> m_plan;
    SyncDirection m_direction;
    QList<SyncPlanEntry> m_queue;
    int m_currentIndex = 0;
    bool m_started = false;
    bool m_finished = false;
    bool m_cancelled = false;
    QPointer<KJob> m_currentJob;
    SyncExecutionReport m_report;
};
