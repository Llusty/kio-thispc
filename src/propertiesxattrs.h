/* Bounded, read-only EntryNoFollow extended attribute snapshots. */
#pragma once
#include "propertiescapabilities.h"
#include <QFutureWatcher>
#include <QThreadPool>
#include <QtConcurrentRun>
#include <QStringDecoder>
#include <algorithm>

 enum class PropertiesXattrStatus { Ready, Empty, Unsupported, PermissionDenied, Error, Unknown, Missing, Replaced };
 enum class PropertiesXattrType { Text, Binary, TooLarge };
 struct PropertiesXattrEntry {
    QByteArray name, raw;
    PropertiesXattrStatus status = PropertiesXattrStatus::Ready;
    PropertiesXattrType type = PropertiesXattrType::Binary;
    QString preview;
    qint64 byteSize = -1;
    bool truncated = false;
 };
 struct PropertiesXattrSnapshot {
    PropertiesXattrStatus status = PropertiesXattrStatus::Unknown;
    QVector<PropertiesXattrEntry> entries;
    bool truncated = false;
    int errorNumber = 0;
 };
 class PropertiesXattrReader {
 public:
    // Linux values cannot be partially read: values over ReadLimit are size-only.
    static constexpr qsizetype ReadLimit = 65536, TotalReadLimit = 262144;
    static constexpr qsizetype ListLimit = 65536, EntryLimit = 256;
    static constexpr qsizetype TextPreview = 1024, BinaryPreview = 64;
    static constexpr int Attempts = 3;
    struct Backend {
        std::function<int(const char *, struct stat *)> identity = [](const char *p, struct stat *s) { return ::lstat(p, s); };
        std::function<ssize_t(const char *, char *, size_t)> list = [](const char *p, char *b, size_t n) { return ::llistxattr(p,b,n); };
        std::function<ssize_t(const char *, const char *, void *, size_t)> get = [](const char *p, const char *a, void *b, size_t n) { return ::lgetxattr(p,a,b,n); };
    };
    static PropertiesXattrStatus errorStatus(int e) {
        if (e == ENOTSUP
#if EOPNOTSUPP != ENOTSUP
            || e == EOPNOTSUPP
#endif
        ) return PropertiesXattrStatus::Unsupported;
        if (e == EACCES || e == EPERM) return PropertiesXattrStatus::PermissionDenied;
        if (e == ENOENT || e == ENOTDIR) return PropertiesXattrStatus::Missing;
        return PropertiesXattrStatus::Error;
    }
    static void present(PropertiesXattrEntry &e) {
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        const QString text = decoder(e.raw);
        bool valid = !decoder.hasError();
        for (QChar c : text) {
            const ushort u = c.unicode();
            if ((u < 32 && u != 9 && u != 10 && u != 13) || (u >= 127 && u <= 159)) valid = false;
        }
        e.type = valid ? PropertiesXattrType::Text : PropertiesXattrType::Binary;
        if (valid) {
            qsizetype n = std::min(text.size(), TextPreview);
            if (n < text.size() && n && text[n-1].isHighSurrogate()) --n;
            e.preview = text.left(n);
            e.truncated = n < text.size();
        } else {
            e.preview = QString::fromLatin1(e.raw.left(BinaryPreview).toHex(' '));
            e.truncated = e.raw.size() > BinaryPreview;
        }
    }
    static PropertiesXattrSnapshot read(const QUrl &url, const PropertiesObjectIdentity &expected, const Backend &b) {
        PropertiesXattrSnapshot out;
        // Must precede all backend calls, including identity checks.
        if (!url.isLocalFile()) { out.status = PropertiesXattrStatus::Unsupported; return out; }
        const QByteArray path = QFile::encodeName(url.toLocalFile());
        auto identityOf = [](const struct stat &s) { return PropertiesObjectIdentity{true, quint64(s.st_dev), quint64(s.st_ino), quint32(s.st_mode & S_IFMT)}; };
        struct stat before{};
        if (b.identity(path.constData(), &before)) { out.errorNumber = errno; out.status = errorStatus(errno); return out; }
        const auto original = identityOf(before);
        if (expected.valid && !(expected == original)) { out.status = PropertiesXattrStatus::Replaced; return out; }
        QByteArray names;
        bool listed = false;
        for (int attempt = 0; attempt < Attempts; ++attempt) {
            ssize_t n = b.list(path.constData(), nullptr, 0);
            if (n < 0) { out.errorNumber = errno; if (errno == ERANGE) continue; out.status = errorStatus(errno); return out; }
            if (n > ListLimit) { out.status = PropertiesXattrStatus::Error; out.errorNumber = E2BIG; return out; }
            names.resize(n);
            if (n == 0) { listed = true; break; }
            n = b.list(path.constData(), names.data(), names.size());
            if (n < 0) { out.errorNumber = errno; if (errno == ERANGE) continue; out.status = errorStatus(errno); return out; }
            if (n > names.size()) { out.errorNumber = ERANGE; continue; }
            names.resize(n); listed = true; break;
        }
        if (!listed) { out.status = PropertiesXattrStatus::Error; out.errorNumber = ERANGE; return out; }
        if (!names.isEmpty() && !names.endsWith('\0')) { out.status = PropertiesXattrStatus::Error; out.errorNumber = EIO; return out; }
        auto keys = names.split('\0'); keys.removeAll(QByteArray());
        std::sort(keys.begin(), keys.end()); keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
        qsizetype budget = TotalReadLimit;
        out.truncated = keys.size() > EntryLimit;
        for (const QByteArray &key : keys) {
            if (out.entries.size() >= EntryLimit) break;
            PropertiesXattrEntry e; e.name = key;
            bool done = false;
            for (int attempt = 0; attempt < Attempts; ++attempt) {
                ssize_t n = b.get(path.constData(), key.constData(), nullptr, 0);
                if (n < 0) { e.status = errorStatus(errno); if (errno == ERANGE) continue; done = true; break; }
                e.byteSize = n;
                if (n > ReadLimit || n > budget) { e.status = PropertiesXattrStatus::Ready; e.raw.clear(); e.type = PropertiesXattrType::TooLarge; e.truncated = true; done = true; break; }
                e.raw.resize(n);
                // A zero-size second call is another probe; detect growth and retry.
                ssize_t actual = b.get(path.constData(), key.constData(), e.raw.data(), n);
                if (actual < 0) { e.status = errorStatus(errno); if (errno == ERANGE) continue; done = true; break; }
                if (actual > n) continue;
                e.raw.resize(actual); e.byteSize = actual; budget -= actual;
                e.status = PropertiesXattrStatus::Ready; present(e); done = true; break;
            }
            if (!done) e.status = PropertiesXattrStatus::Error;
            if (e.status != PropertiesXattrStatus::Ready) e.raw.clear();
            out.entries.append(e);
        }
        struct stat after{};
        if (b.identity(path.constData(), &after)) { out.entries.clear(); out.errorNumber = errno; out.status = errorStatus(errno); return out; }
        if (!(original == identityOf(after))) { out.entries.clear(); out.status = PropertiesXattrStatus::Replaced; return out; }
        out.status = out.entries.isEmpty() ? PropertiesXattrStatus::Empty : PropertiesXattrStatus::Ready;
        return out;
    }
    static PropertiesXattrSnapshot read(const QUrl &u, const PropertiesObjectIdentity &i = {}) { return read(u,i,Backend{}); }
 };

 class PropertiesXattrProvider final : public QObject {
    Q_OBJECT
 public:
    using Reader = std::function<PropertiesXattrSnapshot(const QUrl &, const PropertiesObjectIdentity &)>;
    explicit PropertiesXattrProvider(QObject *parent = nullptr, Reader reader = {}) : QObject(parent), m_reader(reader ? std::move(reader) : Reader([](const QUrl &u,const PropertiesObjectIdentity &i) { return PropertiesXattrReader::read(u,i); })) {}
    void cancel() { ++m_generation; }
    void load(const QUrl &url, const PropertiesObjectIdentity &identity = {}) {
        const quint64 generation = ++m_generation;
        auto *watcher = new QFutureWatcher<PropertiesXattrSnapshot>(this);
        connect(watcher, &QFutureWatcher<PropertiesXattrSnapshot>::finished, this, [this,watcher,generation] {
            const auto result = watcher->result(); watcher->deleteLater();
            if (generation == m_generation) Q_EMIT ready(result);
        });
        const auto reader = m_reader;
        watcher->setFuture(QtConcurrent::run(pool(), [reader,url,identity] { return reader(url,identity); }));
    }
 Q_SIGNALS:
    void ready(const PropertiesXattrSnapshot &snapshot);
 private:
    static QThreadPool *pool() { static QThreadPool p; static const bool initialized = [] { p.setMaxThreadCount(2); return true; }(); Q_UNUSED(initialized); return &p; }
    Reader m_reader;
    quint64 m_generation = 0;
 };
