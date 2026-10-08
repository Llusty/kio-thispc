/* Single user.* mutations, pinned to a verified no-follow file descriptor.
 * No path-based write fallback, privilege elevation, or Undo contract. */
#pragma once
#include "propertiesxattrs.h"
#include <fcntl.h>

enum class PropertiesXattrOperation { Add, Edit, Remove };
enum class PropertiesXattrWriteStatus {
    Success, InvalidName, TooLarge, Unsupported, PermissionDenied, ReadOnly,
    NoSpace, ConflictExists, ConflictMissing, Missing, Replaced, Unknown, Error
};
struct PropertiesXattrWriteResult {
    PropertiesXattrWriteStatus status = PropertiesXattrWriteStatus::Unknown;
    int errorNumber = 0;
};
class PropertiesXattrWriter {
public:
    static constexpr qsizetype NameLimit = 255, ValueLimit = 65536;
    struct Backend {
        PropertiesCapabilityResolver::LocalSyscalls identity;
        std::function<int(const char *, int)> open = [](const char *p,int flags) { return ::open(p,flags); };
        std::function<int(int,struct stat *)> inspect = [](int fd,struct stat *s) { return ::fstat(fd,s); };
        std::function<int(int)> close = [](int fd) { return ::close(fd); };
        std::function<int(int,const char *,const void *,size_t,int)> set = [](int fd,const char *n,const void *v,size_t s,int f) { return ::fsetxattr(fd,n,v,s,f); };
        std::function<int(int,const char *)> remove = [](int fd,const char *n) { return ::fremovexattr(fd,n); };
    };
    static bool validName(const QByteArray &name) {
        // Require lossless UTF-8 representation, including names read from disk.
        return name.startsWith("user.") && name.size()>5 && name.size()<=NameLimit
            && !name.contains('\0') && QString::fromUtf8(name).toUtf8()==name;
    }
    static bool encodeText(const QString &text, QByteArray *bytes) {
        const auto value=text.toUtf8();
        if(value.size()>ValueLimit || QString::fromUtf8(value)!=text) return false;
        *bytes=value; return true;
    }
    static bool parseHex(const QString &text, QByteArray *bytes) {
        QByteArray digits;
        for(QChar c:text) {
            if(c==QLatin1Char(' ')) continue;
            const ushort u=c.unicode();
            if(!((u>='0' && u<='9') || (u>='a' && u<='f') || (u>='A' && u<='F'))) return false;
            digits.append(char(u));
            if(digits.size()>ValueLimit*2) return false;
        }
        if(digits.size()%2) return false;
        *bytes=QByteArray::fromHex(digits); return true;
    }
    static PropertiesXattrWriteStatus errorStatus(int e) {
        using S=PropertiesXattrWriteStatus;
        if(e==EACCES || e==EPERM) return S::PermissionDenied;
        if(e==EROFS) return S::ReadOnly;
        if(e==ENOTSUP
#if EOPNOTSUPP != ENOTSUP
            || e==EOPNOTSUPP
#endif
        ) return S::Unsupported;
        if(e==ENOSPC || e==EDQUOT) return S::NoSpace;
        if(e==ENOENT || e==ENOTDIR) return S::Missing;
        if(e==EEXIST) return S::ConflictExists;
        if(e==ENODATA) return S::ConflictMissing;
        if(e==ERANGE || e==E2BIG) return S::TooLarge;
        return S::Error;
    }
    static PropertiesXattrWriteResult write(const PropertiesTargetCapabilities &target,
        PropertiesXattrOperation operation, const QByteArray &name, const QByteArray &value, const Backend &b) {
        using S=PropertiesXattrWriteStatus;
        if(!validName(name)) return {S::InvalidName};
        if(operation!=PropertiesXattrOperation::Remove && value.size()>ValueLimit) return {S::TooLarge};
        // The requested URL gate precedes EVERY local backend call, even if a
        // caller accidentally supplies capabilities resolved from admin://.
        if(!target.requestedUrl.isLocalFile() || !target.normalizedUrl.isLocalFile() || !target.isLocal || target.isRemote) return {S::Unsupported};
        if(!target.entryIdentity.valid) return {S::Unknown};
        if(target.entryIdentity.type!=S_IFREG && target.entryIdentity.type!=S_IFDIR) return {S::Unsupported};
        if(target.userXattrEditable==PropertiesCapabilityState::ReadOnly) return {S::ReadOnly};
        if(target.userXattrEditable==PropertiesCapabilityState::PermissionDenied) return {S::PermissionDenied};
        if(target.userXattrEditable!=PropertiesCapabilityState::Supported) return {S::Unsupported};
        auto validate=[&]() -> S {
            switch(PropertiesCapabilityResolver::revalidate(target,b.identity)) {
            case PropertiesRevalidationResult::SameTarget: return S::Success;
            case PropertiesRevalidationResult::Missing: return S::Missing;
            case PropertiesRevalidationResult::Replaced: return S::Replaced;
            case PropertiesRevalidationResult::Unsupported: return S::Unsupported;
            case PropertiesRevalidationResult::Unknown: return S::Unknown;
            }
            return S::Unknown;
        };
        const auto preflight=validate(); if(preflight!=S::Success) return {preflight};
        const auto path=QFile::encodeName(target.normalizedUrl.toLocalFile());
        const int flags=O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK|O_NOCTTY
            | (target.entryIdentity.type==S_IFDIR ? O_DIRECTORY : 0);
        const int fd=b.open(path.constData(),flags);
        if(fd<0) { const int e=errno; return {e==ELOOP ? S::Replaced : errorStatus(e),e}; }
        struct Guard { int fd; const Backend &b; ~Guard() { b.close(fd); } } guard{fd,b};
        struct stat current{};
        if(b.inspect(fd,&current)) { const int e=errno; return {errorStatus(e),e}; }
        const PropertiesObjectIdentity actual{true,quint64(current.st_dev),quint64(current.st_ino),quint32(current.st_mode&S_IFMT)};
        if(!(target.entryIdentity==actual)) return {S::Replaced};
        // Also reject a path replacement after open. A replacement after this
        // check cannot redirect the FD mutation: it still affects the captured
        // inode, possibly renamed/unlinked. Path presence and write are NOT atomic.
        const auto beforeWrite=validate(); if(beforeWrite!=S::Success) return {beforeWrite};
        const int result=operation==PropertiesXattrOperation::Remove
            ? b.remove(fd,name.constData())
            : b.set(fd,name.constData(),value.constData(),size_t(value.size()),
                operation==PropertiesXattrOperation::Add ? XATTR_CREATE : XATTR_REPLACE);
        if(result==0) return {S::Success};
        const int e=errno; return {errorStatus(e),e};
    }
    static PropertiesXattrWriteResult write(const PropertiesTargetCapabilities &t,PropertiesXattrOperation o,const QByteArray &n,const QByteArray &v={}) { return write(t,o,n,v,Backend{}); }
};
class PropertiesXattrMutationController final : public QObject {
    Q_OBJECT
public:
    using Writer=std::function<PropertiesXattrWriteResult(const PropertiesTargetCapabilities &,PropertiesXattrOperation,const QByteArray &,const QByteArray &)>;
    explicit PropertiesXattrMutationController(QObject *parent=nullptr,Writer writer={}) : QObject(parent),m_writer(writer ? std::move(writer) : Writer([](const auto &t,auto o,const auto &n,const auto &v) { return PropertiesXattrWriter::write(t,o,n,v); })) {}
    bool busy() const { return m_busy; }
    // Invalidation discards delivery, never promises to undo an in-flight syscall.
    void invalidate() { ++m_generation; }
    bool submit(const PropertiesTargetCapabilities &target,PropertiesXattrOperation op,const QByteArray &name,const QByteArray &value={}) {
        if(m_busy || !PropertiesXattrWriter::validName(name)
            || (op!=PropertiesXattrOperation::Remove && value.size()>PropertiesXattrWriter::ValueLimit)
            || !target.requestedUrl.isLocalFile()) return false;
        m_busy=true; Q_EMIT busyChanged(true);
        const auto generation=m_generation;
        auto *watcher=new QFutureWatcher<PropertiesXattrWriteResult>(this);
        connect(watcher,&QFutureWatcher<PropertiesXattrWriteResult>::finished,this,[this,watcher,generation] {
            const auto result=watcher->result(); watcher->deleteLater(); m_busy=false;
            // Keep controls locked through completion delivery and its fresh read.
            if(generation==m_generation) Q_EMIT completed(result);
            Q_EMIT busyChanged(false);
        });
        const auto writer=m_writer;
        watcher->setFuture(QtConcurrent::run(pool(),[writer,target,op,name,value] { return writer(target,op,name,value); }));
        return true;
    }
Q_SIGNALS:
    void completed(const PropertiesXattrWriteResult &result);
    void busyChanged(bool busy);
private:
    static QThreadPool *pool() { static QThreadPool p; static const bool initialized=[] { p.setMaxThreadCount(2); return true; }(); Q_UNUSED(initialized); return &p; }
    Writer m_writer; bool m_busy=false; quint64 m_generation=0;
};
