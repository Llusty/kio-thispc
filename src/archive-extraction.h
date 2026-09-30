/* Verified Ark batch extraction into private staging, followed by one atomic,
 * no-replace publication. Linux/KDE implementation; never extracts over user data.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"
#include "archive-detection.h"
#include <KIO/Job>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>
#include <archive.h>
#include <archive_entry.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

inline QString thispcArchiveIdentity(const QUrl &url)
{
    if (!url.isLocalFile()) return {};
    struct stat value {};
    if (::stat(QFile::encodeName(url.toLocalFile()).constData(), &value) || !S_ISREG(value.st_mode)) return {};
    return QString::number(value.st_dev) + QLatin1Char(':') + QString::number(value.st_ino);
}

// Call only in the QProcess child modifier (no Qt allocation, no locks).
// Write/truncate, removal, creation and cross-directory rename are denied outside
// the two private directories. Symlinks/special files are denied everywhere.
inline bool thispcRestrictArkWrites(int outputFd, int scratchFd)
{
    const int abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 3) return false; // ABI 3 is required to mediate truncate().
    landlock_ruleset_attr rules {};
    rules.handled_access_fs = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO
        | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER
        | LANDLOCK_ACCESS_FS_TRUNCATE;
    const auto writable = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG
        | LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE;
#ifdef LANDLOCK_ACCESS_FS_RESOLVE_UNIX
    if (abi >= 9) rules.handled_access_fs |= LANDLOCK_ACCESS_FS_RESOLVE_UNIX;
#endif
#ifdef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
    if (abi >= 6) rules.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
#endif
    const int fd = ::syscall(SYS_landlock_create_ruleset, &rules, sizeof(rules), 0);
    if (fd < 0) return false;
    bool ok = true;
    for (const int directory : {outputFd, scratchFd}) {
        landlock_path_beneath_attr rule {};
        rule.parent_fd = directory;
        rule.allowed_access = writable;
        if (::syscall(SYS_landlock_add_rule, fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0)) ok = false;
    }
    // Qt and subprocesses may open /dev/null for output; no other device is writable.
    const int nullFd = ::open("/dev/null", O_PATH | O_CLOEXEC);
    if (nullFd >= 0) {
        landlock_path_beneath_attr rule {};
        rule.parent_fd = nullFd;
        rule.allowed_access = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE;
        if (::syscall(SYS_landlock_add_rule, fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0)) ok = false;
        ::close(nullFd);
    } else ok = false;
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)
        || (ok && ::syscall(SYS_landlock_restrict_self, fd, 0))) ok = false;
    ::close(fd);
    return ok;
}

struct ThisPcArchiveEntry
{
    bool directory = false;
    qint64 size = 0;
    QByteArray hash;
};

class ArchiveExtractionJob final : public KIO::Job
{
public:
    ArchiveExtractionJob(QUrl source, QUrl destination, QString executable, QObject *parent = nullptr)
        : m_source(std::move(source)), m_destination(std::move(destination)), m_executable(std::move(executable)),
          m_identity(thispcArchiveIdentity(m_source))
    {
        setParent(parent);
        setCapabilities(KJob::Killable);
    }

    ~ArchiveExtractionJob() override
    {
        m_cancelled = true;
        if (m_thread) m_thread->wait(); // Worker kills/reaps Ark before removing its own staging.
    }

    void start() override
    {
        if (m_thread || m_finished) return;
        m_thread = QThread::create([this] {
            const QString error = extract();
            QMetaObject::invokeMethod(this, [this, error] {
                if (m_finished) return;
                m_finished = true;
                if (!error.isEmpty()) { setError(KJob::UserDefinedError); setErrorText(error); }
                emitResult();
            }, Qt::QueuedConnection);
        });
        m_thread->setParent(this);
        m_thread->start();
    }

    QUrl publishedUrl() const { return m_publishedUrl; } // Read after result only.

protected:
    bool doKill() override
    {
        QMutexLocker lock(&m_commitMutex);
        // Once publication succeeds, cancellation must not report a false cancel.
        if (m_finished || m_published) return false;
        m_cancelled = true;
        m_finished = true;
        return true;
    }

private:
    static constexpr qint64 MaxBytes = 64LL * 1024 * 1024 * 1024;
    static constexpr qsizetype MaxEntries = 100000;

    void phase(const QString &text)
    {
        QMetaObject::invokeMethod(this, [this, text] {
            if (!m_finished) Q_EMIT description(this, text,
                qMakePair(trLocal("Archiwum", "Archive"), m_source.toDisplayString()),
                qMakePair(trLocal("Cel", "Destination"), m_destination.toDisplayString()));
        }, Qt::QueuedConnection);
    }

    static QString invalidArchive()
    {
        return trLocal("Archiwum jest uszkodzone, zaszyfrowane lub zawiera nieobsługiwane wpisy.",
                       "The archive is damaged, encrypted or contains unsupported entries.");
    }

    QString inspect(const QString &snapshot, QString &suffix, QMap<QString, ThisPcArchiveEntry> &entries)
    {
        auto *reader = archive_read_new();
        if (!reader) return invalidArchive();
        const auto release = qScopeGuard([&] { archive_read_free(reader); });
        archive_read_support_filter_none(reader);
        archive_read_support_filter_gzip(reader);
        archive_read_support_format_zip(reader);
        archive_read_support_format_7zip(reader);
        archive_read_support_format_tar(reader);
        if (archive_read_open_filename(reader, QFile::encodeName(snapshot).constData(), 65536) != ARCHIVE_OK)
            return invalidArchive();
        QSet<QString> explicitEntries;
        qint64 total = 0;
        archive_entry *entry = nullptr;
        int result;
        while ((result = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
            if (m_cancelled) return {};
            const int format = archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK;
            if (format == ARCHIVE_FORMAT_ZIP) suffix = QStringLiteral(".zip");
            else if (format == ARCHIVE_FORMAT_7ZIP) suffix = QStringLiteral(".7z");
            else if (format == ARCHIVE_FORMAT_TAR)
                suffix = archive_filter_code(reader, 0) == ARCHIVE_FILTER_GZIP
                    ? QStringLiteral(".tar.gz") : QStringLiteral(".tar");
            else return invalidArchive();
            const bool directory = archive_entry_filetype(entry) == AE_IFDIR;
            if ((!directory && archive_entry_filetype(entry) != AE_IFREG)
                || archive_entry_symlink(entry) || archive_entry_hardlink(entry)
                || archive_entry_is_encrypted(entry) > 0) return invalidArchive();
            const char *utf8 = archive_entry_pathname_utf8(entry);
            if (!utf8) return invalidArchive();
            const QByteArray encoded(utf8);
            const QString raw = QString::fromUtf8(encoded);
            if (raw.toUtf8() != encoded) return invalidArchive();
            // Tar made with "tar ... ." has a harmless root directory record.
            if (directory && (raw == QLatin1String(".") || raw == QLatin1String("./"))) continue;
            const QString name = thispcSafeArchiveEntryName(raw, directory);
            if (name.isEmpty() || explicitEntries.contains(name)) return invalidArchive();
            explicitEntries.insert(name);
            if (entries.contains(name) && (!directory || !entries[name].directory)) return invalidArchive();
            QString parent = name.section(QLatin1Char('/'), 0, -2);
            while (!parent.isEmpty()) {
                if (entries.contains(parent) && !entries[parent].directory) return invalidArchive();
                entries[parent].directory = true;
                parent = parent.section(QLatin1Char('/'), 0, -2);
            }
            ThisPcArchiveEntry value;
            value.directory = directory;
            QCryptographicHash hash(QCryptographicHash::Sha256);
            char buffer[65536];
            la_ssize_t count;
            while ((count = archive_read_data(reader, buffer, sizeof(buffer))) > 0) {
                if (m_cancelled) return {};
                value.size += count;
                total += count;
                if (total > MaxBytes) return invalidArchive();
                hash.addData(QByteArrayView(buffer, count));
            }
            if (count < 0 || (directory && value.size != 0)
                || (!directory && value.size != archive_entry_size(entry))) return invalidArchive();
            value.hash = hash.result();
            entries.insert(name, value);
            if (entries.size() > MaxEntries) return invalidArchive();
        }
        if (result != ARCHIVE_EOF || entries.isEmpty() || archive_read_close(reader) != ARCHIVE_OK)
            return invalidArchive();
        const auto available = QStorageInfo(QFileInfo(snapshot).absolutePath()).bytesAvailable();
        if (available >= 0 && total > available)
            return trLocal("Za mało miejsca na wypakowanie archiwum.", "Not enough space to extract the archive.");
        return {};
    }

    QString verifyOutput(const QString &output, const QMap<QString, ThisPcArchiveEntry> &entries)
    {
        QSet<QString> seen;
        QDirIterator it(output, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories); // Does not follow symbolic links.
        while (it.hasNext()) {
            if (m_cancelled) return {};
            const QString path = it.next();
            const QString name = QDir(output).relativeFilePath(path);
            if (!entries.contains(name)) return invalidArchive();
            const auto expected = entries.value(name);
            struct stat info {};
            if (::lstat(QFile::encodeName(path).constData(), &info)
                || (expected.directory ? !S_ISDIR(info.st_mode)
                                       : (!S_ISREG(info.st_mode) || info.st_nlink != 1))) return invalidArchive();
            const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC
                                  | (expected.directory ? O_DIRECTORY : 0));
            if (fd < 0) return invalidArchive();
            const auto close = qScopeGuard([&] { ::close(fd); });
            // Retain executable bits on files, but never set-id/sticky or foreign ACL modes.
            if (::fchmod(fd, expected.directory ? 0700 : (0600 | (info.st_mode & 0111)))) return invalidArchive();
            if (!expected.directory) {
                if (info.st_size != expected.size) return invalidArchive();
                QFile file;
                if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) return invalidArchive();
                QCryptographicHash hash(QCryptographicHash::Sha256);
                while (!file.atEnd()) {
                    if (m_cancelled) return {};
                    const auto bytes = file.read(65536);
                    if (bytes.isEmpty() && file.error() != QFileDevice::NoError) return invalidArchive();
                    hash.addData(bytes);
                }
                if (hash.result() != expected.hash) return invalidArchive();
            }
            seen.insert(name);
        }
        if (seen.size() != entries.size())
            return trLocal("Ark nie wypakował wszystkich plików. Wynik nie został zapisany.",
                           "Ark did not extract all files. The result was not published.");
        return {};
    }

    QString runArk(const QString &snapshot, const QString &output, const QString &scratch, int destinationFd)
    {
        const int outputFd = ::open(QFile::encodeName(output).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        const int scratchFd = ::open(QFile::encodeName(scratch).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        const auto close = qScopeGuard([&] { if (outputFd >= 0) ::close(outputFd); if (scratchFd >= 0) ::close(scratchFd); });
        if (outputFd < 0 || scratchFd < 0) return invalidArchive();
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        // No desktop configuration, D-Bus delegation, portal, or invisible password UI.
        // Unattended supported archives are validated first; unexpected prompts remain cancellable.
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("QT_QPA_PLATFORMTHEME"), QString());
        env.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=") + scratch + QStringLiteral("/no-bus"));
        env.insert(QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"), QStringLiteral("unix:path=") + scratch + QStringLiteral("/no-system-bus"));
        env.insert(QStringLiteral("TMPDIR"), scratch);
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), scratch + QStringLiteral("/config"));
        env.insert(QStringLiteral("XDG_CACHE_HOME"), scratch + QStringLiteral("/cache"));
        env.insert(QStringLiteral("XDG_DATA_HOME"), scratch + QStringLiteral("/data"));
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), scratch);
        env.insert(QStringLiteral("KDE_DEBUG"), QStringLiteral("1")); // No crash reporter outside our process group.
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(output);
        process.setChildProcessModifier([outputFd, scratchFd, destinationFd] {
            // Paths use this inherited directory descriptor, so a renamed parent
            // cannot redirect extraction or later cleanup through a replacement symlink.
            if (::fcntl(destinationFd, F_SETFD, 0) < 0) ::_exit(125);
            if (::setsid() < 0 || !thispcRestrictArkWrites(outputFd, scratchFd)) ::_exit(125);
        });
        process.start(m_executable, {QStringLiteral("--batch"), QStringLiteral("--destination"), output,
                                    QStringLiteral("--"), snapshot});
        if (!process.waitForStarted(10000))
            return trLocal("Nie można uruchomić Ark: %1", "Cannot start Ark: %1").arg(process.errorString());
        const auto group = static_cast<pid_t>(process.processId());
        // Reap the direct child and stop helpers even if Ark crashes first.
        const auto stop = qScopeGuard([&] {
            ::kill(-group, SIGKILL);
            if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(5000); }
        });
        QByteArray diagnostics;
        while (process.state() != QProcess::NotRunning) {
            process.waitForFinished(50);
            diagnostics = QByteArray(diagnostics + process.readAllStandardError()).right(2000);
            process.readAllStandardOutput();
            if (m_cancelled) return {};
        }
        diagnostics = QByteArray(diagnostics + process.readAllStandardError()).right(2000);
        if (m_cancelled) return {};
        if (process.exitStatus() != QProcess::NormalExit)
            return trLocal("Ark uległ awarii. Wynik nie został zapisany.", "Ark crashed. The result was not published.");
        if (process.exitCode() == 125)
            return trLocal("Nie można ograniczyć zapisu Ark (wymagany Linux Landlock ABI 3+).",
                           "Cannot restrict Ark writes (Linux Landlock ABI 3+ required).");
        if (process.exitCode() != 0)
            return trLocal("Ark zakończył pracę z błędem %1. %2", "Ark exited with error %1. %2")
                .arg(process.exitCode()).arg(QString::fromLocal8Bit(diagnostics).trimmed());
        // Ark 26.08.1 calls quit() on ANY batch result. Zero is NOT success proof.
        return {};
    }

    QString extract()
    {
        if (!m_source.isLocalFile() || !m_destination.isLocalFile() || m_identity.isEmpty()) return invalidArchive();
        const QString destination = QFileInfo(m_destination.toLocalFile()).canonicalFilePath();
        const int destinationFd = ::open(QFile::encodeName(destination).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (destinationFd < 0)
            return trLocal("Katalog docelowy jest niedostępny.", "The destination directory is unavailable.");
        const auto close = qScopeGuard([&] { ::close(destinationFd); });
        struct stat originalDestination {};
        if (::fstat(destinationFd, &originalDestination)) return invalidArchive();
        const QString anchoredDestination = QStringLiteral("/proc/self/fd/%1").arg(destinationFd);
        QTemporaryDir staging(anchoredDestination + QStringLiteral("/.thispc-extract-XXXXXX"));
        if (!staging.isValid()) return trLocal("Nie można utworzyć katalogu roboczego.", "Cannot create the staging directory.");
        const QString output = staging.filePath(QStringLiteral("output"));
        const QString scratch = staging.filePath(QStringLiteral("scratch"));
        if (!QDir().mkdir(output) || !QDir().mkdir(scratch)) return invalidArchive();
        ::chmod(QFile::encodeName(scratch).constData(), 0700);
        QString snapshot = staging.filePath(QStringLiteral("input"));
        phase(trLocal("Sprawdzanie archiwum…", "Checking archive…"));
        QFile source, copy(snapshot);
        const int sourceFd = ::open(QFile::encodeName(m_source.toLocalFile()).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (sourceFd < 0) return invalidArchive();
        if (!source.open(sourceFd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            ::close(sourceFd);
            return invalidArchive();
        }
        if (!copy.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return invalidArchive();
        struct stat before {}, after {};
        if (::fstat(source.handle(), &before) || !S_ISREG(before.st_mode)
            || QString::number(before.st_dev) + QLatin1Char(':') + QString::number(before.st_ino) != m_identity
            || before.st_size > MaxBytes) return invalidArchive();
        while (!source.atEnd()) {
            if (m_cancelled) return {};
            const auto data = source.read(65536);
            if ((data.isEmpty() && source.error() != QFileDevice::NoError)
                || copy.pos() + data.size() > before.st_size || copy.write(data) != data.size()) return invalidArchive();
        }
        if (::fstat(source.handle(), &after) || before.st_size != after.st_size
            || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
            || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec
            || copy.size() != before.st_size || !copy.flush()) return invalidArchive();
        copy.close();
        source.close();
        QString suffix;
        QMap<QString, ThisPcArchiveEntry> entries;
        QString error = inspect(snapshot, suffix, entries);
        if (!error.isEmpty() || m_cancelled) return error;
        if (!QFile::rename(snapshot, snapshot + suffix)) return invalidArchive();
        snapshot += suffix; // Ark chooses a plugin from validated content, not a misleading source suffix.
        QSet<QString> roots;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) roots.insert(it.key().section(QLatin1Char('/'), 0, 0));
        QString name = QFileInfo(m_source.toLocalFile()).fileName();
        if (name.endsWith(QStringLiteral(".tar.gz"), Qt::CaseInsensitive)) name.chop(7);
        else if (name.endsWith(QStringLiteral(".tgz"), Qt::CaseInsensitive)) name.chop(4);
        else name = QFileInfo(name).completeBaseName();
        if (thispcSafeArchiveEntryName(name, false).isEmpty() || name.startsWith(QLatin1Char('.')))
            name = QStringLiteral("archive");
        const bool singleRoot = roots.size() == 1;
        if (singleRoot) name = *roots.cbegin();
        const QString published = QDir(destination).filePath(name);
        struct stat existing {};
        if (::fstatat(destinationFd, QFile::encodeName(name).constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
            return trLocal("Cel już istnieje lub jest niedostępny: %1. Niczego nie nadpisano.",
                           "The target exists or is unavailable: %1. Nothing was overwritten.").arg(published);
        phase(trLocal("Wypakowywanie przez Ark…", "Extracting with Ark…"));
        error = runArk(snapshot, output, scratch, destinationFd);
        if (!error.isEmpty() || m_cancelled) return error;
        phase(trLocal("Sprawdzanie wypakowanych plików…", "Verifying extracted files…"));
        // A tar "." record can change the output root's mode. It is our own
        // directory (Ark cannot replace it through its unwritable parent).
        if (::chmod(QFile::encodeName(output).constData(), 0700)) return invalidArchive();
        error = verifyOutput(output, entries);
        if (!error.isEmpty() || m_cancelled) return error;
        QMutexLocker lock(&m_commitMutex);
        if (m_cancelled) return {};
        struct stat currentDestination {};
        if (::stat(QFile::encodeName(destination).constData(), &currentDestination)
            || currentDestination.st_dev != originalDestination.st_dev || currentDestination.st_ino != originalDestination.st_ino)
            return trLocal("Katalog docelowy zmienił się. Wynik nie został zapisany.", "The destination changed. The result was not published.");
        const QString ready = singleRoot ? QDir(output).filePath(name) : output;
        // One publication, anchored at an open directory, with no merge or overwrite fallback.
        if (::syscall(SYS_renameat2, AT_FDCWD, QFile::encodeName(ready).constData(),
                      destinationFd, QFile::encodeName(name).constData(), RENAME_NOREPLACE))
            return trLocal("Nie można zapisać wyniku bez nadpisywania: %1.", "Cannot publish without overwriting: %1.").arg(published);
        m_published = true;
        m_publishedUrl = QUrl::fromLocalFile(published);
        return {};
    }

    QUrl m_source;
    QUrl m_destination;
    QString m_executable;
    QString m_identity;
    QThread *m_thread = nullptr;
    std::atomic_bool m_cancelled = false;
    QMutex m_commitMutex;
    bool m_published = false;
    bool m_finished = false;
    QUrl m_publishedUrl;
};
