/* Mount-point confinement helper for the thispc:/ KIO worker (SEC-046-17). */
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <KIO/Global>

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

namespace ThisPcConfinement {

struct ConfinementResult
{
    bool ok = false;
    QUrl redirectedUrl;
    int kioError = 0;
    QString errorDetail;
};

using PreResolveHook = std::function<void(const QString &candidatePath)>;

namespace Detail {

struct UniqueFd
{
    int fd = -1;
    UniqueFd() = default;
    explicit UniqueFd(int f) : fd(f) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&other) noexcept : fd(other.fd) { other.fd = -1; }
    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other) {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    bool valid() const { return fd >= 0; }
    void reset(int newFd = -1)
    {
        if (fd >= 0) {
            const int saved = errno;
            ::close(fd);
            errno = saved;
        }
        fd = newFd;
    }
};

inline bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

// Validate raw percent-encoded URL path before Qt decodes it once.
// Rejects malformed % sequences, %00 (NUL), and %2f/%2F (encoded slash separator).
inline bool validateRawEncodedPath(const QByteArray &rawPath, QString *errorOut)
{
    for (qsizetype i = 0; i < rawPath.size(); ++i) {
        const char ch = rawPath.at(i);
        if (ch == '\0') {
            if (errorOut) *errorOut = QStringLiteral("URL path contains embedded NUL byte.");
            return false;
        }
        if (ch == '%') {
            if (i + 2 >= rawPath.size()
                || !isHexDigit(rawPath.at(i + 1))
                || !isHexDigit(rawPath.at(i + 2))) {
                if (errorOut) *errorOut = QStringLiteral("URL path contains malformed percent-encoding.");
                return false;
            }
            const char h1 = rawPath.at(i + 1);
            const char h2 = rawPath.at(i + 2);
            if (h1 == '0' && h2 == '0') {
                if (errorOut) *errorOut = QStringLiteral("URL path contains encoded NUL byte (%00).");
                return false;
            }
            if (h1 == '2' && (h2 == 'f' || h2 == 'F')) {
                if (errorOut) *errorOut = QStringLiteral("URL path contains encoded slash separator (%2F).");
                return false;
            }
            i += 2;
        }
    }
    return true;
}

} // namespace Detail

inline bool captureMountIdentity(const QString &mountPoint, struct stat *outStat)
{
    if (!outStat) return false;
    const QString cleanMount = QDir::cleanPath(mountPoint);
    if (cleanMount.isEmpty() || !cleanMount.startsWith(QLatin1Char('/'))) {
        return false;
    }
    const QByteArray mountEnc = QFile::encodeName(cleanMount);
    Detail::UniqueFd fd(::open(mountEnc.constData(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd.valid()) {
        return false;
    }
    struct stat st {};
    if (::fstat(fd.fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_nlink == 0) {
        return false;
    }
    *outStat = st;
    return true;
}

inline bool isWithinDirectory(const QString &candidatePath, const QString &rootPath)
{
    const QString cleanRoot = QDir::cleanPath(rootPath);
    const QString cleanCand = QDir::cleanPath(candidatePath);
    if (cleanRoot.isEmpty() || !cleanRoot.startsWith(QLatin1Char('/'))
        || cleanCand.isEmpty() || !cleanCand.startsWith(QLatin1Char('/'))) {
        return false;
    }
    if (cleanRoot == QLatin1String("/")) {
        return true;
    }
    return cleanCand == cleanRoot
        || cleanCand.startsWith(cleanRoot + QLatin1Char('/'));
}

// Parse and validate a thispc:/ URL into volumeId + relative path components.
// Never performs a second percent-decoding pass.
inline bool parseThisPcUrl(
    const QUrl &url,
    QStringList *outParts,
    int *outKioError = nullptr,
    QString *outErrorDetail = nullptr)
{
    if (outParts) outParts->clear();
    if (!url.isValid()) {
        if (outKioError) *outKioError = KIO::ERR_MALFORMED_URL;
        if (outErrorDetail) *outErrorDetail = QStringLiteral("Malformed thispc:/ URL.");
        return false;
    }
    if (!url.query().isEmpty() || !url.fragment().isEmpty()) {
        if (outKioError) *outKioError = KIO::ERR_MALFORMED_URL;
        if (outErrorDetail) *outErrorDetail = QStringLiteral("Unexpected query or fragment in thispc:/ URL.");
        return false;
    }

    const QByteArray rawEncoded = url.path(QUrl::FullyEncoded).toUtf8();
    QString rawErr;
    if (!Detail::validateRawEncodedPath(rawEncoded, &rawErr)) {
        if (outKioError) *outKioError = KIO::ERR_MALFORMED_URL;
        if (outErrorDetail) *outErrorDetail = rawErr;
        return false;
    }

    // Single decoding pass via Qt. Double-encoded sequences like %252e%252e decode
    // once to literal "%2e%2e" and are never decoded a second time.
    const QString decodedPath = url.path(QUrl::FullyDecoded);
    if (decodedPath.contains(QChar(0))) {
        if (outKioError) *outKioError = KIO::ERR_MALFORMED_URL;
        if (outErrorDetail) *outErrorDetail = QStringLiteral("Decoded URL path contains NUL character.");
        return false;
    }

    const QStringList parts = decodedPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String("..")) {
            if (outKioError) *outKioError = KIO::ERR_ACCESS_DENIED;
            if (outErrorDetail) *outErrorDetail = QStringLiteral("Path traversal component rejected.");
            return false;
        }
    }

    if (outParts) *outParts = parts;
    return true;
}

// Resolve and confine a thispc:/ URL to a mounted volume's mountPoint.
// Enforces:
// 1. Strict URL & single-pass percent-decoding validation (no NUL, no %2F, no . or ..).
// 2. Mount-root directory existence, non-symlink check (O_PATH | O_DIRECTORY | O_NOFOLLOW),
//    and optional (st_dev, st_ino) identity verification.
// 3. Component-boundary lexical containment (prevents /mnt/disk vs /mnt/disk2 prefix confusion).
// 4. Component-by-component filesystem walk from the mount root detecting escaping symlinks.
inline ConfinementResult resolveConfinedLocalUrl(
    const QUrl &url,
    const QString &mountPoint,
    const struct stat *expectedMountStat = nullptr,
    const PreResolveHook &preResolveHook = {})
{
    ConfinementResult res;
    QStringList parts;
    if (!parseThisPcUrl(url, &parts, &res.kioError, &res.errorDetail)) {
        return res;
    }
    if (parts.isEmpty()) {
        res.kioError = KIO::ERR_MALFORMED_URL;
        res.errorDetail = QStringLiteral("Missing volume identifier in thispc:/ URL.");
        return res;
    }

    const QString cleanMount = QDir::cleanPath(mountPoint);
    if (cleanMount.isEmpty() || !cleanMount.startsWith(QLatin1Char('/'))) {
        res.kioError = KIO::ERR_CANNOT_ENTER_DIRECTORY;
        res.errorDetail = QStringLiteral("Invalid mount point.");
        return res;
    }

    auto verifyMountAndWalk = [&](QString *outFinalPath, int *outErr, QString *outMsg) -> bool {
        const QByteArray mountEnc = QFile::encodeName(cleanMount);
        Detail::UniqueFd mountFd(::open(mountEnc.constData(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!mountFd.valid()) {
            const int e = errno;
            bool isSymlink = (e == ELOOP);
            if (!isSymlink && e == ENOTDIR) {
                struct stat lst {};
                if (::lstat(mountEnc.constData(), &lst) == 0 && S_ISLNK(lst.st_mode)) {
                    isSymlink = true;
                }
            }
            if (outErr) *outErr = isSymlink ? KIO::ERR_ACCESS_DENIED : KIO::ERR_DOES_NOT_EXIST;
            if (outMsg) *outMsg = QStringLiteral("Mount point does not exist or is a symbolic link.");
            return false;
        }
        struct stat mountSt {};
        if (::fstat(mountFd.fd, &mountSt) != 0 || !S_ISDIR(mountSt.st_mode) || mountSt.st_nlink == 0) {
            if (outErr) *outErr = KIO::ERR_DOES_NOT_EXIST;
            if (outMsg) *outMsg = QStringLiteral("Mount point is unavailable.");
            return false;
        }
        if (expectedMountStat
            && (mountSt.st_dev != expectedMountStat->st_dev
                || mountSt.st_ino != expectedMountStat->st_ino)) {
            if (outErr) *outErr = KIO::ERR_DOES_NOT_EXIST;
            if (outMsg) *outMsg = QStringLiteral("Mount point identity changed.");
            return false;
        }

        const QString canonicalMount = QFileInfo(cleanMount).canonicalFilePath();
        if (canonicalMount.isEmpty() || !canonicalMount.startsWith(QLatin1Char('/'))) {
            if (outErr) *outErr = KIO::ERR_DOES_NOT_EXIST;
            if (outMsg) *outMsg = QStringLiteral("Mount point cannot be canonicalized.");
            return false;
        }

        if (parts.size() == 1) {
            *outFinalPath = canonicalMount;
            return true;
        }

        QStringList relParts = parts;
        relParts.removeFirst();

        // Lexical containment pre-check with strict component boundaries
        const QString joinedRel = relParts.join(QLatin1Char('/'));
        const QString lexicalCandidate = QDir::cleanPath(
            canonicalMount == QLatin1String("/")
                ? QString(QLatin1Char('/') + joinedRel)
                : QString(canonicalMount + QLatin1Char('/') + joinedRel));
        if (!isWithinDirectory(lexicalCandidate, canonicalMount)) {
            if (outErr) *outErr = KIO::ERR_ACCESS_DENIED;
            if (outMsg) *outMsg = QStringLiteral("Resolved path escapes mount point.");
            return false;
        }

        // Walk existing components from mountFd with AT_SYMLINK_NOFOLLOW / O_NOFOLLOW
        // to detect symlinks escaping canonicalMount.
        Detail::UniqueFd curFd(std::move(mountFd));
        QString curPath = canonicalMount;
        for (qsizetype i = 0; i < relParts.size(); ++i) {
            const QString &comp = relParts.at(i);
            const QByteArray compEnc = QFile::encodeName(comp);
            struct stat cst {};
            if (::fstatat(curFd.fd, compEnc.constData(), &cst, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno == ENOENT) {
                    // Missing leaf/subtree: all existing ancestors are already verified inside
                    // canonicalMount and remaining components contain no "." or "..".
                    break;
                }
                if (outErr) *outErr = (errno == EACCES || errno == EPERM)
                                          ? KIO::ERR_ACCESS_DENIED
                                          : KIO::ERR_DOES_NOT_EXIST;
                if (outMsg) *outMsg = QStringLiteral("Cannot access path component inside mount point.");
                return false;
            }

            const QString nextPath = (curPath == QLatin1String("/"))
                ? QString(QLatin1Char('/') + comp)
                : QString(curPath + QLatin1Char('/') + comp);

            if (S_ISLNK(cst.st_mode)) {
                const QString targetCanonical = QFileInfo(nextPath).canonicalFilePath();
                if (targetCanonical.isEmpty() || !isWithinDirectory(targetCanonical, canonicalMount)) {
                    if (outErr) *outErr = KIO::ERR_ACCESS_DENIED;
                    if (outMsg) *outMsg = QStringLiteral("Symbolic link escapes mount point.");
                    return false;
                }
                curPath = targetCanonical;
                if (i + 1 < relParts.size()) {
                    Detail::UniqueFd nextDir(
                        ::open(QFile::encodeName(targetCanonical).constData(),
                               O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                    if (!nextDir.valid()) {
                        if (outErr) *outErr = KIO::ERR_ACCESS_DENIED;
                        if (outMsg) *outMsg = QStringLiteral("Symbolic link directory target is invalid.");
                        return false;
                    }
                    curFd = std::move(nextDir);
                }
                continue;
            }

            curPath = nextPath;
            if (i + 1 < relParts.size()) {
                if (!S_ISDIR(cst.st_mode)) {
                    // Non-directory intermediate component: let KIO fail naturally or fail closed
                    break;
                }
                Detail::UniqueFd nextDir(
                    ::openat(curFd.fd, compEnc.constData(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (!nextDir.valid()) {
                    if (outErr) *outErr = KIO::ERR_ACCESS_DENIED;
                    if (outMsg) *outMsg = QStringLiteral("Intermediate directory changed during confinement check.");
                    return false;
                }
                curFd = std::move(nextDir);
            }
        }

        if (!isWithinDirectory(lexicalCandidate, canonicalMount)) {
            if (outErr) *outErr = KIO::ERR_ACCESS_DENIED;
            if (outMsg) *outMsg = QStringLiteral("Resolved path escapes mount point.");
            return false;
        }
        *outFinalPath = lexicalCandidate;
        return true;
    };

    QString finalPath;
    if (!verifyMountAndWalk(&finalPath, &res.kioError, &res.errorDetail)) {
        return res;
    }

    if (preResolveHook) {
        preResolveHook(finalPath);
        QString recheckedPath;
        if (!verifyMountAndWalk(&recheckedPath, &res.kioError, &res.errorDetail)) {
            return res;
        }
        finalPath = recheckedPath;
    }

    res.ok = true;
    res.redirectedUrl = QUrl::fromLocalFile(finalPath);
    return res;
}

// Production entry point used by ThisPcWorker::redirectedLocalUrl().
// Requires a valid mount identity snapshot for local volumes and never
// silently downgrades to unguarded redirection when the snapshot is missing.
inline ConfinementResult redirectVolumeUrl(
    const QUrl &url,
    const QString &mountPoint,
    const struct stat &expectedMountStat,
    bool expectedMountStatValid,
    bool isMtp,
    const QString &mtpTargetUrl,
    const PreResolveHook &preResolveHook = {})
{
    if (isMtp) {
        ConfinementResult mtpRes;
        mtpRes.ok = true;
        mtpRes.redirectedUrl = QUrl(mtpTargetUrl);
        return mtpRes;
    }
    if (!expectedMountStatValid) {
        ConfinementResult failRes;
        failRes.kioError = KIO::ERR_DOES_NOT_EXIST;
        failRes.errorDetail = QStringLiteral("Mount point identity snapshot unavailable.");
        return failRes;
    }
    return resolveConfinedLocalUrl(url, mountPoint, &expectedMountStat, preResolveHook);
}

} // namespace ThisPcConfinement
