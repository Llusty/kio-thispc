/* Stage 4 checksum capability and runtime state model. */
#pragma once

#include <QString>
#include <QUrl>
#include <QMetaType>

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
    QString sha256;
    QString errorMessage;
    quint64 bytesProcessed = 0;
    quint64 totalBytes = 0;

    bool hasValidResult() const
    {
        return state == ChecksumState::Completed
            && sha256.size() == 64;
    }
};

Q_DECLARE_METATYPE(ChecksumData)
