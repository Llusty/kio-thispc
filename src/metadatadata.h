/* Read-only Stage 5 metadata presentation model. */
#pragma once

#include <KFileMetaData/Properties>

#include <QMetaType>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVector>

enum class MetadataCapability {
    SupportedLocalFile,
    DirectoryNotApplicable,
    SymlinkUnavailable,
    RemoteUnavailable,
    Unreadable
};

enum class MetadataState { Idle, Loading, Available, NoMetadata, Failed, Disappeared };

struct MetadataPropertyValue
{
    KFileMetaData::Property::Property property = KFileMetaData::Property::Empty;
    QVariant value;
};

struct MetadataRow
{
    QString label;
    QString value;
};

struct MetadataData
{
    QUrl url;
    MetadataCapability capability = MetadataCapability::Unreadable;
    MetadataState state = MetadataState::Idle;
    QVector<MetadataRow> rows;
    QString errorMessage;
};

Q_DECLARE_METATYPE(MetadataData)
Q_DECLARE_METATYPE(QVector<MetadataPropertyValue>)
