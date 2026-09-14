/* Completed tree transfers share the application's ordered Undo/Redo history.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "localtransferjob.h"
#include <QScopeGuard>

struct LocalTreeRecord
{
    LocalTransferPlan plan;
    QList<struct stat> expected;
    bool move = false;
    bool snapshotValid = false;
    quint64 serial = 0;
};

class LocalTreeHistoryJob final : public KIO::Job
{
public:
    LocalTreeHistoryJob(LocalTreeRecord record, bool undo, QObject *parent)
        : m_record(std::move(record)), m_undo(undo)
    {
        setParent(parent);
        setCapabilities(KJob::Killable);
    }

    ~LocalTreeHistoryJob() override
    {
        m_cancelled.storeRelease(true);
        if (m_thread) m_thread->wait();
    }

    void start() override
    {
        if (m_thread) return;
        m_thread = QThread::create([this] {
            const QString error = validateAndRemove();
            QMetaObject::invokeMethod(this, [this, error] {
                if (m_finished) return;
                if (!error.isEmpty() || (m_undo && !m_record.move)) { finish(error); return; }
                auto *job = new LocalTransferJob(replayPlan(), m_record.move, this);
                m_transfer = job;
                job->setAutoDelete(false);
                connect(job, &KJob::result, this, [this](KJob *completed) {
                    finish(completed->error() ? completed->errorText() : QString());
                });
                job->start();
            }, Qt::QueuedConnection);
        });
        m_thread->setParent(this);
        m_thread->start();
    }

protected:
    bool doKill() override
    {
        if (m_finished || (m_transfer && !m_transfer->kill(KJob::Quietly))) return false;
        m_cancelled.storeRelease(true);
        m_finished = true;
        return true;
    }

private:
    QString pathFor(const LocalTransferPlanEntry &entry) const
    {
        return m_undo ? entry.destinationPath : entry.sourcePath;
    }

    static bool matches(const struct stat &expected, const QString &path)
    {
        struct stat actual {};
        if (::lstat(QFile::encodeName(path).constData(), &actual) != 0) return false;
        return expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino
            && expected.st_mode == actual.st_mode
            && (S_ISDIR(expected.st_mode)
                || (expected.st_size == actual.st_size
                    && expected.st_mtim.tv_sec == actual.st_mtim.tv_sec
                    && expected.st_mtim.tv_nsec == actual.st_mtim.tv_nsec));
    }

    QString validateAndRemove()
    {
        QHash<QString, QSet<QString>> children;
        for (const auto &entry : m_record.plan.entries) {
            const QFileInfo info(pathFor(entry));
            children[info.absolutePath()].insert(info.fileName());
        }
        for (qsizetype i = 0; i < m_record.plan.entries.size(); ++i) {
            if (m_cancelled.loadAcquire()) return tr("History operation cancelled.");
            const auto &entry = m_record.plan.entries.at(i);
            const QString path = pathFor(entry);
            if (!matches(m_record.expected.at(i), path))
                return tr("An item changed and was not modified: %1").arg(path);
            if (entry.type == LocalTransferEntryType::Directory && (!m_undo || !entry.destinationExists)) {
                const QStringList names = QDir(path).entryList(QDir::AllEntries | QDir::Hidden
                    | QDir::System | QDir::NoDotAndDotDot);
                if (QSet<QString>(names.begin(), names.end()) != children.value(path))
                    return tr("A directory contains changed contents and was not modified: %1").arg(path);
            }
        }
        if (!m_undo || m_record.move) return {};
        QList<QPair<QString, struct stat>> permissionsChanged;
        const auto restorePermissions = qScopeGuard([&] {
            for (auto it = permissionsChanged.crbegin(); it != permissionsChanged.crend(); ++it) {
                struct stat current {};
                const auto path = QFile::encodeName(it->first);
                if (::lstat(path.constData(), &current) == 0
                    && current.st_dev == it->second.st_dev && current.st_ino == it->second.st_ino
                    && current.st_mode == (it->second.st_mode | S_IWUSR | S_IXUSR))
                    ::chmod(path.constData(), it->second.st_mode & 07777);
            }
        });
        // Only directories created by this copy may need owner write/search
        // permission for Undo. Existing merged directories are never changed.
        for (qsizetype i = 0; i < m_record.plan.entries.size(); ++i) {
            const auto &entry = m_record.plan.entries.at(i);
            if (entry.type != LocalTransferEntryType::Directory || entry.destinationExists) continue;
            const auto original = m_record.expected.at(i);
            if ((original.st_mode & (S_IWUSR | S_IXUSR)) == (S_IWUSR | S_IXUSR)) continue;
            if (::chmod(QFile::encodeName(entry.destinationPath).constData(),
                        (original.st_mode & 07777) | S_IWUSR | S_IXUSR) != 0)
                return tr("Could not remove the recorded directory: %1").arg(entry.destinationPath);
            permissionsChanged.push_back({entry.destinationPath, original});
            m_record.expected[i].st_mode |= S_IWUSR | S_IXUSR;
        }
        // Remove only the recorded entries. An item added after validation
        // is never recursively deleted; a nonempty directory fails safely.
        for (qsizetype i = m_record.plan.entries.size(); i-- > 0;) {
            if (m_cancelled.loadAcquire()) return tr("History operation cancelled.");
            const auto &entry = m_record.plan.entries.at(i);
            if (entry.type == LocalTransferEntryType::Directory && entry.destinationExists) continue;
            const QString path = entry.destinationPath;
            if (!matches(m_record.expected.at(i), path)
                || !(entry.type == LocalTransferEntryType::Directory ? QDir().rmdir(path) : QFile::remove(path)))
                return tr("Could not remove the recorded item: %1").arg(path);
        }
        return {};
    }

    LocalTransferPlan replayPlan() const
    {
        LocalTransferPlan result = m_record.plan;
        result.totalBytes = 0;
        for (qsizetype i = 0; i < result.entries.size(); ++i) {
            auto &entry = result.entries[i];
            const bool originalDestinationExists = entry.destinationExists;
            if (m_undo) std::swap(entry.sourcePath, entry.destinationPath);
            entry.sourceStat = m_record.expected.at(i);
            entry.size = entry.type == LocalTransferEntryType::RegularFile ? entry.sourceStat.st_size : 0;
            result.totalBytes += entry.size;
            entry.modified = QDateTime::fromMSecsSinceEpoch(
                entry.sourceStat.st_mtim.tv_sec * 1000 + entry.sourceStat.st_mtim.tv_nsec / 1000000);
            if (entry.type == LocalTransferEntryType::SymbolicLink)
                entry.symbolicLinkContents = readLocalLink(entry.sourcePath);
            const QFileInfo destination(entry.destinationPath);
            entry.destinationExists = destination.exists() || destination.isSymLink();
            entry.destinationIsDirectory = destination.isDir();
            entry.destinationIsSymbolicLink = destination.isSymLink();
            entry.removeSourceDirectory = !m_undo || !originalDestinationExists;
        }
        return result;
    }

    void finish(const QString &error)
    {
        if (m_finished) return;
        m_finished = true;
        if (!error.isEmpty()) { setError(KJob::UserDefinedError); setErrorText(error); }
        emitResult();
    }

    LocalTreeRecord m_record;
    bool m_undo;
    QThread *m_thread = nullptr;
    QPointer<KJob> m_transfer;
    QAtomicInteger<bool> m_cancelled = false;
    bool m_finished = false;
};

class LocalTreeHistory final : public QObject
{
    Q_OBJECT
public:
    explicit LocalTreeHistory(QObject *parent = nullptr) : QObject(parent) {}

    void recordCompleted(LocalTransferJob *job, quint64 serial)
    {
        if (m_busy || !job || job->error() || job->usedKio()) return;
        LocalTreeRecord record;
        record.plan = job->transferPlan();
        record.plan.entries = job->completedEntries();
        record.move = job->movesSources();
        record.serial = serial;
        capture(record, true);
        if (!record.snapshotValid || record.plan.entries.isEmpty()) return;
        while (m_records.size() > m_position) m_records.removeLast();
        m_records.push_back(record);
        m_position = m_records.size();
        Q_EMIT availabilityChanged();
    }

    bool canUndo() const { return !m_busy && m_position > 0 && m_records.at(m_position - 1).snapshotValid; }
    bool canRedo() const { return !m_busy && m_position < m_records.size() && m_records.at(m_position).snapshotValid; }
    quint64 undoSerial() const { return canUndo() ? m_records.at(m_position - 1).serial : 0; }
    quint64 redoSerial() const { return canRedo() ? m_records.at(m_position).serial : 0; }
    KJob *undo() { return canUndo() ? run(true) : nullptr; }
    KJob *redo() { return canRedo() ? run(false) : nullptr; }

Q_SIGNALS:
    void availabilityChanged();

private:
    static void capture(LocalTreeRecord &record, bool destination)
    {
        record.expected.clear();
        record.snapshotValid = true;
        for (const auto &entry : record.plan.entries) {
            struct stat snapshot {};
            const QString path = destination ? entry.destinationPath : entry.sourcePath;
            if (::lstat(QFile::encodeName(path).constData(), &snapshot) != 0) {
                record.snapshotValid = false;
                return;
            }
            record.expected.push_back(snapshot);
        }
    }

    KJob *run(bool undo)
    {
        const qsizetype index = undo ? m_position - 1 : m_position;
        auto *job = new LocalTreeHistoryJob(m_records.at(index), undo, this);
        job->setAutoDelete(false);
        m_busy = true;
        Q_EMIT availabilityChanged();
        connect(job, &KJob::result, this, [this, undo, index](KJob *completed) {
            if (!completed->error()) {
                m_position += undo ? -1 : 1;
                capture(m_records[index], !undo);
            }
            m_busy = false;
            Q_EMIT availabilityChanged();
        });
        job->start();
        return job;
    }

    QList<LocalTreeRecord> m_records;
    qsizetype m_position = 0;
    bool m_busy = false;
};
