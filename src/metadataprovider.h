/* Lazy asynchronous KFileMetaData extraction for local regular files. */
#pragma once

#include "applicationlifecycle.h"
#include "browsercommon.h"
#include "metadatadata.h"

#include <KFileMetaData/ExtractorCollection>
#include <KFileMetaData/PropertyInfo>
#include <KFileMetaData/SimpleExtractionResult>
#include <KFileMetaData/UserMetaData>

#include <QFile>
#include <QFutureWatcher>
#include <QLocale>
#include <QMimeDatabase>
#include <QObject>
#include <QPointer>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <memory>
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

    ~MetadataProvider() override
    {
        cancel();
    }

    void cancel()
    {
        if (m_cancelled) {
            m_cancelled->store(true, std::memory_order_release);
        }
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
        const int fd = ::open(QFile::encodeName(url.toLocalFile()).constData(),
                              O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return MetadataCapability::Unreadable;
        struct stat fst {};
        const bool isReg = (::fstat(fd, &fst) == 0 && S_ISREG(fst.st_mode));
        ::close(fd);
        return isReg ? MetadataCapability::SupportedLocalFile : MetadataCapability::Unreadable;
    }

    bool start()
    {
        if (m_started || m_data.capability != MetadataCapability::SupportedLocalFile) return false;
        auto *pools = ApplicationWorkerPools::forApplication();
        if (pools && pools->isShuttingDown()) return false;

        m_started = true;
        ++m_startCount;
        m_cancelled = std::make_shared<std::atomic<bool>>(false);
        m_data.state = MetadataState::Loading;
        Q_EMIT dataChanged(m_data);

        auto *watcher = new QFutureWatcher<MetadataExtractionResult>(this);
        const auto cancelled = m_cancelled;
        connect(watcher, &QFutureWatcher<MetadataExtractionResult>::finished, this, [this, watcher, cancelled] {
            const bool valid = !watcher->isCanceled() && watcher->future().resultCount() > 0;
            if (!valid || cancelled->load(std::memory_order_acquire)) {
                watcher->deleteLater();
                return;
            }
            const auto raw = watcher->result();
            watcher->deleteLater();
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
        });

        const QString path = m_url.toLocalFile();
        const MetadataExtractor extractor = m_extractor;
        auto work = [path, extractor, cancelled]() -> MetadataExtractionResult {
            if (cancelled->load(std::memory_order_acquire)) {
                return {};
            }
            return extractor(path);
        };
        if (pools) {
            watcher->setFuture(pools->runMetadataTask(std::move(work)));
        } else {
            watcher->setFuture(QtConcurrent::run(QThreadPool::globalInstance(), std::move(work)));
        }
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
                output.errorMessage = output.disappeared ? QString() : qt_error_string(errno);
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
    std::shared_ptr<std::atomic<bool>> m_cancelled;
};
