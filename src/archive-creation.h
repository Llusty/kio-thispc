/* Safe, asynchronous local archive creation with libarchive.
 * Produces a private output and publishes it only with RENAME_NOREPLACE.
 * Linux implementation; never writes to any selected source.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"

#include <KIO/Job>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <archive.h>
#include <archive_entry.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// Only formats which the linked libarchive is asked to write are advertised.
enum class ThisPcArchiveFormat { Zip, SevenZip, TarGzip };

inline QString thispcArchiveSuffix(ThisPcArchiveFormat format)
{
    switch (format) {
    case ThisPcArchiveFormat::Zip: return QStringLiteral(".zip");
    case ThisPcArchiveFormat::SevenZip: return QStringLiteral(".7z");
    case ThisPcArchiveFormat::TarGzip: return QStringLiteral(".tar.gz");
    }
    return {};
}

class ArchiveCreationJob final : public KIO::Job
{
public:
    ArchiveCreationJob(QList<QUrl> sources, QUrl destination, ThisPcArchiveFormat format,
                       QObject *parent = nullptr)
        : m_sources(std::move(sources)), m_destination(std::move(destination)), m_format(format)
    {
        setParent(parent);
        setCapabilities(KJob::Killable);
    }

    ~ArchiveCreationJob() override
    {
        m_cancelled = true;
        if (m_thread) m_thread->wait(); // Worker disposes the private file before destruction.
    }

    void start() override
    {
        if (m_thread || m_finished) return;
        m_thread = QThread::create([this] {
            const QString error = create();
            QMetaObject::invokeMethod(this, [this, error] {
                if (m_finished) return;
                m_finished = true;
                if (!error.isEmpty()) {
                    setError(KJob::UserDefinedError);
                    setErrorText(error);
                }
                emitResult();
            }, Qt::QueuedConnection);
        });
        m_thread->setParent(this);
        m_thread->start();
    }

    QUrl publishedUrl() const { return m_publishedUrl; } // Only read after completion.

protected:
    bool doKill() override
    {
        QMutexLocker lock(&m_commitMutex);
        if (m_finished || m_published) return false;
        m_cancelled = true;
        m_finished = true;
        return true;
    }

private:
    static constexpr qint64 MaxBytes = 64LL * 1024 * 1024 * 1024;
    static constexpr int MaxEntries = 100000;
    static constexpr int MaxDepth = 128;

    struct Item {
        QString name; // Relative to a common, anchored source parent.
        struct stat original {};
        bool directory = false;
    };

    static QString problem(const QString &name = {})
    {
        return trLocal("Nie można bezpiecznie utworzyć archiwum. Sprawdź uprawnienia, nazwy i rodzaj plików: %1.",
                       "Cannot safely create the archive. Check permissions, names and file types: %1.").arg(name);
    }

    static bool safeComponent(const QString &name)
    {
        if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
            || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
            || name.contains(QLatin1Char(':')) || name.size() > 255) return false;
        for (const QChar ch : name)
            if (ch.category() == QChar::Other_Control) return false;
        return true;
    }

    static QByteArray encoded(const QString &name)
    {
        return QFile::encodeName(name);
    }

    static bool unchanged(const struct stat &before, const struct stat &after)
    {
        return before.st_dev == after.st_dev && before.st_ino == after.st_ino
            && before.st_mode == after.st_mode && before.st_size == after.st_size
            && before.st_mtim.tv_sec == after.st_mtim.tv_sec
            && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec
            && before.st_ctim.tv_sec == after.st_ctim.tv_sec
            && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    }

    void announce(const QString &message)
    {
        QMetaObject::invokeMethod(this, [this, message] {
            if (!m_finished) Q_EMIT description(this, message,
                qMakePair(trLocal("Archiwum", "Archive"), m_destination.toDisplayString()));
        }, Qt::QueuedConnection);
    }

    // fdopendir owns a dup; a recursive walk never follows symlinks. The writer
    // reopens every entry via openat(O_NOFOLLOW) and compares the original stat.
    QString scan(int directory, const QString &prefix, QVector<Item> &items, qint64 &total, int depth)
    {
        if (depth > MaxDepth) return problem(prefix);
        const int copy = ::dup(directory);
        if (copy < 0) return problem(prefix);
        DIR *dir = ::fdopendir(copy);
        if (!dir) { ::close(copy); return problem(prefix); }
        const auto close = qScopeGuard([&] { ::closedir(dir); });
        errno = 0;
        while (dirent *entry = ::readdir(dir)) {
            if (m_cancelled) return {};
            const QByteArray raw(entry->d_name);
            if (raw == "." || raw == "..") { errno = 0; continue; }
            const QString part = QFile::decodeName(raw);
            if (encoded(part) != raw || !safeComponent(part)) return problem(prefix + part);
            const QString path = prefix + part;
            if (path.size() > 4096 || items.size() >= MaxEntries) return problem(path);
            struct stat current {};
            if (::fstatat(directory, raw.constData(), &current, AT_SYMLINK_NOFOLLOW)) return problem(path);
            if (!S_ISREG(current.st_mode) && !S_ISDIR(current.st_mode)) return problem(path);
            if (S_ISREG(current.st_mode) && (current.st_size < 0 || total > MaxBytes - current.st_size))
                return trLocal("Limit tworzenia archiwum wynosi 64 GiB.", "Archive creation is limited to 64 GiB.");
            items.push_back({path, current, S_ISDIR(current.st_mode)});
            if (S_ISDIR(current.st_mode)) {
                const int child = ::openat(directory, raw.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (child < 0) return problem(path);
                struct stat opened {};
                const bool same = !::fstat(child, &opened) && unchanged(current, opened);
                if (!same) { ::close(child); return problem(path); }
                const QString error = scan(child, path + QLatin1Char('/'), items, total, depth + 1);
                ::close(child);
                if (!error.isEmpty() || m_cancelled) return error;
            } else total += current.st_size;
            errno = 0;
        }
        if (errno) return problem(prefix);
        return {};
    }

    static int openItem(int root, const QString &name, bool directory)
    {
        const QStringList parts = name.split(QLatin1Char('/'));
        int current = ::dup(root);
        if (current < 0) return -1;
        for (int index = 0; index < parts.size(); ++index) {
            const bool last = index == parts.size() - 1;
            const int next = ::openat(current, encoded(parts[index]).constData(),
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC
                | (last && !directory ? O_NONBLOCK : O_DIRECTORY));
            ::close(current);
            if (next < 0) return -1;
            current = next;
        }
        return current;
    }

    QString writeItem(struct archive *writer, int root, const Item &item, qint64 total, qint64 &done)
    {
        const int fd = openItem(root, item.name, item.directory);
        if (fd < 0) return problem(item.name);
        const auto close = qScopeGuard([&] { ::close(fd); });
        struct stat initial {};
        if (::fstat(fd, &initial) || !unchanged(item.original, initial)
            || (item.directory ? !S_ISDIR(initial.st_mode) : !S_ISREG(initial.st_mode)))
            return trLocal("Plik źródłowy zmienił się w trakcie operacji: %1.",
                           "A source changed during archive creation: %1.").arg(item.name);
        auto *entry = archive_entry_new();
        if (!entry) return problem(item.name);
        const auto release = qScopeGuard([&] { archive_entry_free(entry); });
        const QByteArray name = (item.name + (item.directory ? QStringLiteral("/") : QString())).toUtf8();
        archive_entry_set_pathname_utf8(entry, name.constData());
        archive_entry_set_filetype(entry, item.directory ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry, item.directory ? 0755 : (0644 | (initial.st_mode & 0111)));
        archive_entry_set_size(entry, item.directory ? 0 : initial.st_size);
        archive_entry_set_mtime(entry, initial.st_mtim.tv_sec, initial.st_mtim.tv_nsec);
        if (archive_write_header(writer, entry) != ARCHIVE_OK) return problem(item.name);
        if (!item.directory) {
            char buffer[65536];
            qint64 readTotal = 0;
            while (readTotal < initial.st_size) {
                if (m_cancelled) return {};
                const ssize_t size = ::read(fd, buffer, static_cast<size_t>(qMin<qint64>(sizeof(buffer), initial.st_size - readTotal)));
                if (size <= 0) return problem(item.name);
                ssize_t written = 0;
                while (written < size) {
                    if (m_cancelled) return {};
                    const la_ssize_t count = archive_write_data(writer, buffer + written, size - written);
                    if (count <= 0) return problem(item.name);
                    written += count;
                }
                readTotal += size;
                done += size;
                if (done == total || done - m_reportedBytes >= 4 * 1024 * 1024) {
                    m_reportedBytes = done;
                    QMetaObject::invokeMethod(this, [this, done] {
                        if (!m_finished) setProcessedAmount(KJob::Bytes, static_cast<qulonglong>(done));
                    }, Qt::QueuedConnection);
                }
            }
        }
        struct stat finalState {};
        if (::fstat(fd, &finalState) || !unchanged(initial, finalState))
            return trLocal("Plik źródłowy zmienił się w trakcie operacji: %1.",
                           "A source changed during archive creation: %1.").arg(item.name);
        return {};
    }

    QString create()
    {
        // libarchive converts UTF-8 names using the calling thread's C locale.
        // Qt's Unicode locale is not sufficient if the C locale is still POSIX:
        // pax/7z may reject non-ASCII names (and some libarchive builds crash).
        // Set only the worker thread's LC_CTYPE; never change the UI's locale.
        const locale_t utf8Locale = ::newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
        if (!utf8Locale) return trLocal("Brak lokalizacji C.UTF-8 wymaganej do tworzenia archiwum.",
                                        "The C.UTF-8 locale required for archive creation is unavailable.");
        const locale_t oldLocale = ::uselocale(utf8Locale);
        if (!oldLocale) { ::freelocale(utf8Locale); return problem(); }
        const auto restoreLocale = qScopeGuard([&] {
            ::uselocale(oldLocale);
            ::freelocale(utf8Locale);
        });
        if (m_sources.isEmpty() || !m_destination.isLocalFile()) return problem();
        const QString target = m_destination.toLocalFile();
        const QString base = QFileInfo(target).fileName();
        if (!safeComponent(base) || !base.endsWith(thispcArchiveSuffix(m_format), Qt::CaseInsensitive)) return problem(base);
        const QString destination = QFileInfo(target).absolutePath();
        const QString realDestination = QFileInfo(destination).canonicalFilePath();
        const QString parent = QFileInfo(m_sources.first().toLocalFile()).absolutePath();
        const QString realParent = QFileInfo(parent).canonicalFilePath();
        if (realDestination.isEmpty() || realParent.isEmpty()) return problem();
        QSet<QString> names;
        QStringList orderedNames;
        for (const QUrl &source : m_sources) {
            if (!source.isLocalFile() || QFileInfo(source.toLocalFile()).absolutePath() != parent) return problem();
            const QString name = QFileInfo(source.toLocalFile()).fileName();
            if (!safeComponent(name) || names.contains(name)) return problem(name);
            names.insert(name);
            orderedNames.append(name);
            // The private output must never be recursively included in a selected folder.
            const QString selected = QDir(realParent).filePath(name);
            const QString canonical = QFileInfo(selected).canonicalFilePath();
            if (!canonical.isEmpty() && (realDestination == canonical
                || realDestination.startsWith(canonical + QLatin1Char('/')))) return trLocal(
                "Nie można zapisać archiwum wewnątrz wybranego katalogu: %1.",
                "Cannot create an archive inside a selected directory: %1.").arg(name);
        }
        const int destFd = ::open(encoded(realDestination).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (destFd < 0) return problem(realDestination);
        const auto closeDest = qScopeGuard([&] { ::close(destFd); });
        const int sourceFd = ::open(encoded(realParent).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sourceFd < 0) return problem(realParent);
        const auto closeSource = qScopeGuard([&] { ::close(sourceFd); });
        struct stat originalDestination {};
        if (::fstat(destFd, &originalDestination)) return problem(realDestination);
        struct stat existing {};
        if (::fstatat(destFd, encoded(base).constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
            return trLocal("Archiwum docelowe już istnieje. Niczego nie nadpisano: %1.",
                           "The destination archive already exists. Nothing was overwritten: %1.").arg(target);
        announce(trLocal("Sprawdzanie plików…", "Checking source files…"));
        QVector<Item> items;
        qint64 total = 0;
        for (const QString &name : orderedNames) {
            if (m_cancelled) return {};
            const QByteArray raw = encoded(name);
            struct stat info {};
            if (::fstatat(sourceFd, raw.constData(), &info, AT_SYMLINK_NOFOLLOW)
                || (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode))) return problem(name);
            if (items.size() >= MaxEntries) return problem(name);
            items.push_back({name, info, S_ISDIR(info.st_mode)});
            if (S_ISDIR(info.st_mode)) {
                const int fd = openItem(sourceFd, name, true);
                if (fd < 0) return problem(name);
                struct stat opened {};
                const bool stable = !::fstat(fd, &opened) && unchanged(info, opened);
                if (!stable) { ::close(fd); return problem(name); }
                const QString error = scan(fd, name + QLatin1Char('/'), items, total, 1);
                ::close(fd);
                if (!error.isEmpty() || m_cancelled) return error;
            } else {
                if (info.st_size < 0 || total > MaxBytes - info.st_size)
                    return trLocal("Limit tworzenia archiwum wynosi 64 GiB.", "Archive creation is limited to 64 GiB.");
                total += info.st_size;
            }
        }
        QMetaObject::invokeMethod(this, [this, total] {
            if (!m_finished) setTotalAmount(KJob::Bytes, static_cast<qulonglong>(total));
        }, Qt::QueuedConnection);
        if (m_cancelled) return {};
        const QString anchored = QStringLiteral("/proc/self/fd/%1").arg(destFd);
        QTemporaryDir privateDir(anchored + QStringLiteral("/.thispc-create-XXXXXX"));
        if (!privateDir.isValid()) return problem(target);
        const QString temporary = privateDir.filePath(QStringLiteral("output") + thispcArchiveSuffix(m_format));
        auto *writer = archive_write_new();
        if (!writer) return problem(target);
        const auto release = qScopeGuard([&] { archive_write_free(writer); });
        int configured = ARCHIVE_FATAL;
        switch (m_format) {
        case ThisPcArchiveFormat::Zip: configured = archive_write_set_format_zip(writer); break;
        case ThisPcArchiveFormat::SevenZip: configured = archive_write_set_format_7zip(writer); break;
        case ThisPcArchiveFormat::TarGzip:
            configured = archive_write_set_format_pax_restricted(writer);
            if (configured == ARCHIVE_OK) configured = archive_write_add_filter_gzip(writer);
            break;
        }
        if (configured != ARCHIVE_OK || archive_write_open_filename(writer, encoded(temporary).constData()) != ARCHIVE_OK)
            return problem(target);
        announce(trLocal("Tworzenie archiwum…", "Creating archive…"));
        qint64 done = 0;
        QString error;
        for (const auto &item : items) {
            if (m_cancelled) break;
            error = writeItem(writer, sourceFd, item, total, done);
            if (!error.isEmpty()) break;
        }
        // Even after a write failure, close/free the archive before staging is removed.
        const int closed = archive_write_close(writer);
        if (!error.isEmpty() || m_cancelled) return error;
        if (closed != ARCHIVE_OK || done != total) return problem(target);
        QMutexLocker lock(&m_commitMutex);
        if (m_cancelled) return {};
        struct stat currentDestination {};
        if (::fstat(destFd, &currentDestination)
            || originalDestination.st_dev != currentDestination.st_dev
            || originalDestination.st_ino != currentDestination.st_ino
            || ::stat(encoded(realDestination).constData(), &currentDestination)
            || originalDestination.st_dev != currentDestination.st_dev
            || originalDestination.st_ino != currentDestination.st_ino)
            return trLocal("Katalog docelowy zmienił się. Archiwum nie zostało zapisane.",
                           "Destination directory changed. The archive was not published.");
        if (::syscall(SYS_renameat2, AT_FDCWD, encoded(temporary).constData(), destFd,
                      encoded(base).constData(), RENAME_NOREPLACE))
            return trLocal("Nie można zapisać archiwum bez nadpisywania: %1.",
                           "Cannot publish the archive without overwriting: %1.").arg(target);
        m_published = true;
        m_publishedUrl = QUrl::fromLocalFile(target);
        return {};
    }

    QList<QUrl> m_sources;
    QUrl m_destination;
    ThisPcArchiveFormat m_format;
    QThread *m_thread = nullptr;
    std::atomic_bool m_cancelled = false;
    QMutex m_commitMutex;
    bool m_published = false;
    bool m_finished = false;
    qint64 m_reportedBytes = 0;
    QUrl m_publishedUrl;
};
