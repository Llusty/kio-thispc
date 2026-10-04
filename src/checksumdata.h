/* Stage 4 checksum capability and runtime state model. */
#pragma once

#include <QCryptographicHash>
#include <QMetaType>
#include <QString>
#include <QUrl>

enum class ChecksumAlgorithm
{
    Sha256,
    Sha1,
    Md5
};

inline QCryptographicHash::Algorithm toCryptographicHashAlgorithm(ChecksumAlgorithm alg)
{
    switch (alg) {
    case ChecksumAlgorithm::Sha256:
        return QCryptographicHash::Sha256;
    case ChecksumAlgorithm::Sha1:
        return QCryptographicHash::Sha1;
    case ChecksumAlgorithm::Md5:
        return QCryptographicHash::Md5;
    }
    return QCryptographicHash::Sha256;
}

inline QString algorithmDisplayName(ChecksumAlgorithm alg)
{
    switch (alg) {
    case ChecksumAlgorithm::Sha256:
        return QStringLiteral("SHA-256");
    case ChecksumAlgorithm::Sha1:
        return QStringLiteral("SHA-1");
    case ChecksumAlgorithm::Md5:
        return QStringLiteral("MD5");
    }
    return QStringLiteral("SHA-256");
}

inline int expectedDigestLength(ChecksumAlgorithm alg)
{
    switch (alg) {
    case ChecksumAlgorithm::Sha256:
        return 64;
    case ChecksumAlgorithm::Sha1:
        return 40;
    case ChecksumAlgorithm::Md5:
        return 32;
    }
    return 64;
}

inline bool isLegacyAlgorithm(ChecksumAlgorithm alg)
{
    return alg == ChecksumAlgorithm::Sha1 || alg == ChecksumAlgorithm::Md5;
}

inline QString algorithmNotice(ChecksumAlgorithm alg)
{
    if (isLegacyAlgorithm(alg)) {
        return QStringLiteral("Do zgodności i kontroli integralności. Niezalecany do zastosowań wymagających bezpieczeństwa.");
    }
    return QString();
}

enum class ChecksumCapability
{
    SupportedLocalFile,
    DirectoryNotApplicable,
    RemoteUnavailable,
    SymlinkUnavailable,
    Unreadable
};

enum class ChecksumState
{
    Idle,
    Running,
    Completed,
    Cancelled,
    Failed,
    ChangedDuringHash
};

struct ChecksumData
{
    QUrl url;
    ChecksumCapability capability = ChecksumCapability::RemoteUnavailable;
    ChecksumState state = ChecksumState::Idle;
    ChecksumAlgorithm algorithm = ChecksumAlgorithm::Sha256;
    QString digest;
    QString sha256; // Backward compatibility for 0.37 Properties: only populated when algorithm == Sha256
    QString errorMessage;
    quint64 bytesProcessed = 0;
    quint64 totalBytes = 0;

    bool hasValidResult() const
    {
        return state == ChecksumState::Completed
            && !digest.isEmpty()
            && digest.size() == expectedDigestLength(algorithm);
    }
};

Q_DECLARE_METATYPE(ChecksumAlgorithm)
Q_DECLARE_METATYPE(ChecksumCapability)
Q_DECLARE_METATYPE(ChecksumState)
Q_DECLARE_METATYPE(ChecksumData)
