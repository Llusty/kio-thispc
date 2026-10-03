/* Lazy asynchronous KFileMetaData extraction for local regular files. */
#pragma once

#include "browsercommon.h"
#include "metadatadata.h"

#include <KFileMetaData/ExtractorCollection>
#include <KFileMetaData/PropertyInfo>
#include <KFileMetaData/SimpleExtractionResult>
#include <KFileMetaData/UserMetaData>

#include <QFile>
#include <QLocale>
#include <QMimeDatabase>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

struct MetadataExtractionResult
{
    QVector<MetadataPropertyValue> properties;
    QString errorMessage;
    bool disappeared = false;
    QStringList userTags;
    int userRating = -1;
    QString userComment;
};
Q_DECLARE_METATYPE(MetadataExtractionResult)

using MetadataExtractor = std::function<MetadataExtractionResult(const QString &)>;

class MetadataWorker final : public QObject
{
    Q_OBJECT
public:
    MetadataWorker(QString path, MetadataExtractor extractor)
        : m_path(std::move(path)), m_extractor(std::move(extractor)) {}
public Q_SLOTS:
    void run() { Q_EMIT finished(m_extractor(m_path)); }
Q_SIGNALS:
    void finished(const MetadataExtractionResult &result);
private:
    QString m_path;
    MetadataExtractor m_extractor;
};

class MetadataProvider final : public QObject
{
    Q_OBJECT
public:
    explicit MetadataProvider(const QUrl &url, QObject *parent = nullptr,
                              MetadataExtractor extractor = {})
        : QObject(parent), m_url(url), m_extractor(extractor ? std::move(extractor) : realExtractor())
    {
        qRegisterMetaType<MetadataData>();
        qRegisterMetaType<MetadataExtractionResult>();
        m_data.url = url;
        m_data.capability = capabilityForUrl(url);
    }

    const MetadataData &data() const { return m_data; }
    int startCount() const { return m_startCount; }

    static QString dimensionsLabel()
    {
        return trLocal("Wymiary", "Dimensions");
    }

    static MetadataCapability capabilityForUrl(const QUrl &url)
    {
        if (!url.isLocalFile()) return MetadataCapability::RemoteUnavailable;
        struct stat st {};
        if (::lstat(QFile::encodeName(url.toLocalFile()).constData(), &st) != 0)
            return MetadataCapability::Unreadable;
        if (S_ISLNK(st.st_mode)) return MetadataCapability::SymlinkUnavailable;
        if (S_ISDIR(st.st_mode)) return MetadataCapability::DirectoryNotApplicable;
        if (!S_ISREG(st.st_mode)) return MetadataCapability::Unreadable;
        const int fd = ::open(QFile::encodeName(url.toLocalFile()).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return MetadataCapability::Unreadable;
        ::close(fd);
        return MetadataCapability::SupportedLocalFile;
    }

    bool start()
    {
        if (m_started || m_data.capability != MetadataCapability::SupportedLocalFile) return false;
        m_started = true;
        ++m_startCount;
        m_data.state = MetadataState::Loading;
        Q_EMIT dataChanged(m_data);

        auto *thread = new QThread;
        auto *worker = new MetadataWorker(m_url.toLocalFile(), m_extractor);
        worker->moveToThread(thread);
        connect(thread, &QThread::started, worker, &MetadataWorker::run);
        connect(worker, &MetadataWorker::finished, this, [this](const MetadataExtractionResult &raw) {
            if (raw.disappeared) {
                m_data.state = MetadataState::Disappeared;
            } else if (!raw.errorMessage.isEmpty()) {
                m_data.state = MetadataState::Failed;
                m_data.errorMessage = raw.errorMessage;
            } else {
                m_data.rows = formatRows(raw.properties, raw.userTags);
                m_data.state = m_data.rows.isEmpty() ? MetadataState::NoMetadata : MetadataState::Available;
            }
            Q_EMIT dataChanged(m_data);
        }, Qt::QueuedConnection);
        connect(worker, &MetadataWorker::finished, thread, &QThread::quit);
        connect(worker, &MetadataWorker::finished, worker, &QObject::deleteLater);
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return true;
    }

    static QVector<MetadataRow> formatRows(const QVector<MetadataPropertyValue> &values,
                                           const QStringList &userTags = {})
    {
        QVector<MetadataRow> rows;
        QMap<KFileMetaData::Property::Property, QVariantList> grouped;
        for (const auto &item : values) {
            if (item.property == KFileMetaData::Property::Empty || !item.value.isValid()) continue;
            const QString text = item.value.toString().trimmed();
            if (text.isEmpty() && item.value.metaType().id() == QMetaType::QString) continue;
            grouped[item.property].append(item.value);
        }
        const auto width = grouped.take(KFileMetaData::Property::Width);
        const auto height = grouped.take(KFileMetaData::Property::Height);
        if (!width.isEmpty() && !height.isEmpty()) {
            rows.append({dimensionsLabel(), QStringLiteral("%1 × %2").arg(width.first().toInt()).arg(height.first().toInt())});
        }
        if (!userTags.isEmpty()) rows.append({QObject::tr("Tags"), userTags.join(QStringLiteral("; "))});
        for (auto it = grouped.cbegin(); it != grouped.cend(); ++it) {
            KFileMetaData::PropertyInfo info(it.key());
            if (info.property() == KFileMetaData::Property::Empty || info.displayName().isEmpty()) continue;
            QStringList rendered;
            for (const QVariant &value : it.value()) {
                QString display = info.formatAsDisplayString(value).trimmed();
                if (display.isEmpty()) display = value.toString().trimmed();
                if (!display.isEmpty() && !rendered.contains(display)) rendered << display;
            }
            if (!rendered.isEmpty()) rows.append({info.displayName(), rendered.join(QStringLiteral("; "))});
        }
        return rows;
    }

Q_SIGNALS:
    void dataChanged(const MetadataData &data);

private:
    static MetadataExtractor realExtractor()
    {
        return [](const QString &path) {
            MetadataExtractionResult output;
            struct stat before {};
            const QByteArray encoded = QFile::encodeName(path);
            if (::lstat(encoded.constData(), &before) != 0 || !S_ISREG(before.st_mode)) {
                output.disappeared = errno == ENOENT;
                output.errorMessage = output.disappeared ? QString() : QString::fromLocal8Bit(std::strerror(errno));
                return output;
            }
            try {
                const QString mime = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchContent).name();
                KFileMetaData::ExtractorCollection collection;
                const auto extractors = collection.fetchExtractors(mime);
                for (auto *extractor : extractors) {
                    KFileMetaData::SimpleExtractionResult result(path, mime, KFileMetaData::ExtractionResult::ExtractMetaData);
                    extractor->extract(&result);
                    const auto properties = result.properties();
                    for (auto it = properties.cbegin(); it != properties.cend(); ++it)
                        output.properties.append({it.key(), it.value()});
                }
                const KFileMetaData::UserMetaData user(path);
                if (user.isSupported()) {
                    const auto attributes = user.queryAttributes(
                        KFileMetaData::UserMetaData::Tags
                        | KFileMetaData::UserMetaData::Rating
                        | KFileMetaData::UserMetaData::Comment);
                    if (attributes.testFlag(KFileMetaData::UserMetaData::Tags))
                        output.userTags = user.tags();
                    if (attributes.testFlag(KFileMetaData::UserMetaData::Rating)) {
                        output.userRating = user.rating();
                        output.properties.append({KFileMetaData::Property::Rating, output.userRating});
                    }
                    if (attributes.testFlag(KFileMetaData::UserMetaData::Comment)) {
                        output.userComment = user.userComment();
                        output.properties.append({KFileMetaData::Property::Comment, output.userComment});
                    }
                }
            } catch (const std::exception &error) {
                output.errorMessage = QString::fromLocal8Bit(error.what());
                return output;
            } catch (...) {
                output.errorMessage = QObject::tr("The metadata extractor failed.");
                return output;
            }
            struct stat after {};
            if (::lstat(encoded.constData(), &after) != 0) {
                output.properties.clear();
                output.disappeared = true;
            } else if (before.st_dev != after.st_dev || before.st_ino != after.st_ino
                       || before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec
                       || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
                output.properties.clear();
                output.errorMessage = QObject::tr("The file changed during metadata extraction.");
            }
            return output;
        };
    }

    QUrl m_url;
    MetadataExtractor m_extractor;
    MetadataData m_data;
    bool m_started = false;
    int m_startCount = 0;
};
