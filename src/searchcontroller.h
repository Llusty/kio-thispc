/*
 * Asynchronous KIO filename search, filtering and result batching.
 * Extracted during the 0.21.0 architecture refactor.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"

#include <KIO/ListJob>
#include <KJob>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <utility>

inline bool isSearchLocation(const QUrl &url)
{
    return url.scheme() == QStringLiteral("thispcsearch");
}

inline QUrl filenameSearchUrl(
    const QString &query,
    const QUrl &root)
{
    QUrl searchUrl(QStringLiteral("filenamesearch:"));

    QUrlQuery parameters;
    parameters.addQueryItem(
        QStringLiteral("search"),
        query);
    parameters.addQueryItem(
        QStringLiteral("url"),
        root.toString());

    searchUrl.setQuery(parameters);
    return searchUrl;
}

inline QUrl makeSearchLocation(
    const QString &query,
    int scope,
    const QUrl &base,
    int typeFilter,
    int dateFilter,
    int sizeFilter)
{
    QUrl searchUrl(QStringLiteral("thispcsearch:/"));

    QUrlQuery parameters;
    parameters.addQueryItem(
        QStringLiteral("search"),
        query);
    parameters.addQueryItem(
        QStringLiteral("scope"),
        QString::number(scope));

    if (base.isValid()) {
        parameters.addQueryItem(
            QStringLiteral("base"),
            base.toString());
    }

    parameters.addQueryItem(
        QStringLiteral("type"),
        QString::number(typeFilter));
    parameters.addQueryItem(
        QStringLiteral("date"),
        QString::number(dateFilter));
    parameters.addQueryItem(
        QStringLiteral("size"),
        QString::number(sizeFilter));

    searchUrl.setQuery(parameters);
    return searchUrl;
}

inline QString searchQueryFromUrl(const QUrl &url)
{
    if (!isSearchLocation(url)
        && url.scheme() != QStringLiteral("filenamesearch")) {
        return {};
    }

    return QUrlQuery(url).queryItemValue(
        QStringLiteral("search"));
}

inline int searchIntParameter(
    const QUrl &url,
    const QString &name,
    int fallback)
{
    bool ok = false;
    const int value =
        QUrlQuery(url).queryItemValue(name).toInt(&ok);

    return ok ? value : fallback;
}

inline QUrl searchBaseFromUrl(const QUrl &url)
{
    const QString base =
        QUrlQuery(url).queryItemValue(
            QStringLiteral("base"));

    return base.isEmpty()
        ? QUrl()
        : QUrl(base);
}

// Each pane retains its own draft and filter choices while the toolbar is shared.
struct PaneSearchState {
    QUrl location;
    QString text;
    int scope = 2;
    int type = 0;
    int date = 0;
    int size = 0;

    void loadLocation(const QUrl &url)
    {
        const bool changed = !sameLocation(location, url);
        if (changed) text = searchQueryFromUrl(url);
        if (isSearchLocation(url)) {
            scope = std::clamp(searchIntParameter(url, QStringLiteral("scope"), 2), 0, 2);
            type = std::clamp(searchIntParameter(url, QStringLiteral("type"), 0), 0, 6);
            date = std::clamp(searchIntParameter(url, QStringLiteral("date"), 0), 0, 4);
            size = std::clamp(searchIntParameter(url, QStringLiteral("size"), 0), 0, 4);
        } else if (changed) {
            scope = sameLocation(url, kThisPcUrl) ? 2 : 0;
            type = date = size = 0;
        }
        location = url;
    }
};

inline QString parentLocationForDisplay(const QUrl &url)
{
    if (url.isLocalFile()) {
        return QFileInfo(url.toLocalFile()).absolutePath();
    }

    QUrl parent = url;
    QString path = parent.path();

    while (path.size() > 1
           && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }

    const int slash =
        path.lastIndexOf(QLatin1Char('/'));

    if (slash <= 0) {
        path = QStringLiteral("/");
    } else {
        path = path.left(slash);
    }

    parent.setPath(path);
    parent.setQuery(QString());
    return parent.toDisplayString(QUrl::PreferLocalFile);
}

class SearchController final : public QObject
{
    Q_OBJECT
public:
    struct Filters {
        int type = 0;
        int date = 0;
        int size = 0;
    };

    explicit SearchController(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_searchRenderTimer.setSingleShot(true);
        m_searchRenderTimer.setInterval(120);
        connect(&m_searchRenderTimer, &QTimer::timeout, this, &SearchController::resultsChanged);
    }

    ~SearchController() override { cancel(); }

    bool isRunning() const { return m_searchInProgress; }
    int totalRoots() const { return m_searchTotalRoots; }
    int completedRoots() const { return m_searchCompletedRoots; }
    int errorCount() const { return m_searchErrors; }
    const QList<FileInfo> &files() const { return m_pendingFiles; }
    void setShowHiddenFiles(bool show) { m_showHiddenFiles = show; }

    int progressPercent() const
    {
        if (m_searchTotalRoots <= 0) return 0;
        unsigned long total = 0;
        for (int value : m_searchProgressValues)
            total += static_cast<unsigned long>(std::clamp(value, 0, 100));
        return std::clamp(static_cast<int>(total / static_cast<unsigned long>(m_searchTotalRoots)), 0, 100);
    }

    QString statusText(int visibleCount, int scope) const
    {
        const QString scopeLabel =
            scope == 2
                ? trLocal(
                    "Ten komputer",
                    "This PC")
                : (scope == 1
                    ? trLocal(
                        "Bieżący dysk",
                        "Current drive")
                    : trLocal(
                        "Bieżący folder",
                        "Current folder"));

        if (isRunning()) {
            return
                isPolish()
                    ? QStringLiteral(
                        "%1 wyników • %2/%3 lokalizacji • %4")
                        .arg(visibleCount)
                        .arg(completedRoots())
                        .arg(totalRoots())
                        .arg(scopeLabel)
                    : QStringLiteral(
                        "%1 results • %2/%3 locations • %4")
                        .arg(visibleCount)
                        .arg(completedRoots())
                        .arg(totalRoots())
                        .arg(scopeLabel);
        } else {
            QString message =
                isPolish()
                    ? QStringLiteral(
                        "%1 wyników • zakres: %2")
                        .arg(visibleCount)
                        .arg(scopeLabel)
                    : QStringLiteral(
                        "%1 results • scope: %2")
                        .arg(visibleCount)
                        .arg(scopeLabel);

            if (errorCount() > 0) {
                message +=
                    isPolish()
                        ? QStringLiteral(
                            " • %1 lokalizacji z błędem")
                            .arg(errorCount())
                        : QStringLiteral(
                            " • %1 locations with errors")
                            .arg(errorCount());
            }

            return message;
        }
    }

    // Keep partial results available when the user stops a search. A new
    // generation makes queued signals from canceled workers harmless.
    bool cancel()
    {
        if (!m_searchInProgress && m_searchJobs.isEmpty()) return false;
        ++m_searchGeneration;
        for (const QPointer<KIO::ListJob> &job : std::as_const(m_searchJobs)) {
            if (job) job->kill(KJob::Quietly);
        }
        m_searchJobs.clear();
        m_searchProgressValues.clear();
        m_searchInProgress = false;
        m_searchRenderTimer.stop();
        return true;
    }

    static bool matchesFile(
        const FileInfo &file,
        QMimeDatabase &mimeDatabase,
        const Filters &filters)
    {
        const QMimeType mime =
            resolvedMimeType(
                file,
                mimeDatabase);

        if (filters.type != 0) {
            const QString mimeName =
                mime.isValid()
                    ? mime.name()
                    : file.mimeType;

            bool typeMatches = false;

            switch (filters.type) {
            case 1:
                typeMatches = file.isDir;
                break;
            case 2:
                typeMatches =
                    !file.isDir
                    && mimeName.startsWith(
                        QStringLiteral("image/"));
                break;
            case 3:
                typeMatches =
                    !file.isDir
                    && (mimeName.startsWith(
                            QStringLiteral("text/"))
                        || mimeName
                            == QStringLiteral("application/pdf")
                        || mimeName.contains(
                            QStringLiteral("officedocument"))
                        || mimeName.contains(
                            QStringLiteral("opendocument"))
                        || mimeName.contains(
                            QStringLiteral("msword"))
                        || mimeName.contains(
                            QStringLiteral("rtf")));
                break;
            case 4:
                typeMatches =
                    !file.isDir
                    && mimeName.startsWith(
                        QStringLiteral("audio/"));
                break;
            case 5:
                typeMatches =
                    !file.isDir
                    && mimeName.startsWith(
                        QStringLiteral("video/"));
                break;
            case 6:
                typeMatches =
                    !file.isDir
                    && (mimeName.contains(
                            QStringLiteral("zip"))
                        || mimeName.contains(
                            QStringLiteral("archive"))
                        || mimeName.contains(
                            QStringLiteral("compressed"))
                        || mimeName.contains(
                            QStringLiteral("rar"))
                        || mimeName.contains(
                            QStringLiteral("7z"))
                        || mimeName.contains(
                            QStringLiteral("tar")));
                break;
            default:
                typeMatches = true;
                break;
            }

            if (!typeMatches) {
                return false;
            }
        }

        if (filters.date != 0) {
            if (file.modificationTime <= 0) {
                return false;
            }

            qint64 days = 0;

            switch (filters.date) {
            case 1:
                days = 1;
                break;
            case 2:
                days = 7;
                break;
            case 3:
                days = 30;
                break;
            case 4:
                days = 365;
                break;
            default:
                break;
            }

            const qint64 cutoff =
                QDateTime::currentSecsSinceEpoch()
                - days * 24 * 60 * 60;

            if (file.modificationTime < cutoff) {
                return false;
            }
        }

        if (filters.size != 0) {
            if (file.isDir || file.size < 0) {
                return false;
            }

            const qint64 mib =
                1024LL * 1024LL;
            const qint64 gib =
                1024LL * mib;

            bool sizeMatches = false;

            switch (filters.size) {
            case 1:
                sizeMatches =
                    file.size < mib;
                break;
            case 2:
                sizeMatches =
                    file.size >= mib
                    && file.size < 100LL * mib;
                break;
            case 3:
                sizeMatches =
                    file.size >= 100LL * mib
                    && file.size < gib;
                break;
            case 4:
                sizeMatches =
                    file.size >= gib;
                break;
            default:
                sizeMatches = true;
                break;
            }

            if (!sizeMatches) {
                return false;
            }
        }

        return true;
    }

    void start(const QString &query, const QList<QUrl> &roots, bool showHiddenFiles)
    {
        cancel();
        m_pendingFiles.clear();
        m_searchSeenUrls.clear();
        m_showHiddenFiles = showHiddenFiles;
        m_searchTotalRoots = roots.size();
        m_searchCompletedRoots = 0;
        m_searchErrors = 0;
        m_searchInProgress = true;
        const quint64 generation = ++m_searchGeneration;
        Q_EMIT progressChanged();

        if (roots.isEmpty()) {
            m_searchInProgress = false;
            Q_EMIT resultsChanged();
            Q_EMIT finished();
            return;
        }

        for (const QUrl &root : roots) {
            KIO::ListJob *job =
                KIO::listDir(
                    filenameSearchUrl(
                        query,
                        root),
                    KIO::HideProgressInfo);

            job->setUiDelegate(nullptr);
            m_searchJobs.push_back(job);
            m_searchProgressValues.insert(
                job,
                0);

            connect(
                job,
                &KJob::percentChanged,
                this,
                [this, generation, job](
                    KJob *,
                    unsigned long percent) {
                    if (generation
                        != m_searchGeneration) {
                        return;
                    }

                    m_searchProgressValues[job] =
                        static_cast<int>(percent);

                    Q_EMIT progressChanged();
                });

            connect(
                job,
                &KIO::ListJob::entries,
                this,
                [this, generation, root](
                    KIO::Job *,
                    const KIO::UDSEntryList &entries) {
                    if (generation
                        != m_searchGeneration) {
                        return;
                    }

                    for (const KIO::UDSEntry &entry :
                         entries) {
                        QString name =
                            entry.stringValue(
                                KIO::UDSEntry::UDS_DISPLAY_NAME);

                        if (name.isEmpty()) {
                            name =
                                entry.stringValue(
                                    KIO::UDSEntry::UDS_NAME);
                        }

                        const QString rawName =
                            entry.stringValue(
                                KIO::UDSEntry::UDS_NAME);

                        if (name.isEmpty()
                            || rawName == QStringLiteral(".")
                            || rawName == QStringLiteral("..")
                            || (!m_showHiddenFiles
                                && rawName.startsWith(
                                    QLatin1Char('.')))) {
                            continue;
                        }

                        FileInfo file;
                        file.name = name;
                        file.mimeType =
                            entry.stringValue(
                                KIO::UDSEntry::UDS_MIME_TYPE);
                        file.iconName =
                            entry.stringValue(
                                KIO::UDSEntry::UDS_ICON_NAME);
                        file.url =
                            childUrlForEntry(
                                root,
                                entry);
                        file.isDir =
                            entryIsDirectory(entry);
                        file.size =
                            entry.numberValue(
                                KIO::UDSEntry::UDS_SIZE,
                                -1);
                        file.modificationTime =
                            entry.numberValue(
                                KIO::UDSEntry::UDS_MODIFICATION_TIME,
                                0);

                        if (!file.url.isValid()
                            || file.url.isEmpty()) {
                            continue;
                        }

                        const QString key =
                            normalizedUrl(file.url)
                                .toString(
                                    QUrl::FullyEncoded);

                        if (m_searchSeenUrls.contains(key)) {
                            continue;
                        }

                        m_searchSeenUrls.insert(key);
                        m_pendingFiles.push_back(file);
                    }

                    scheduleSearchRender();
                });

            connect(
                job,
                &KJob::result,
                this,
                [this, generation, job](KJob *) {
                    if (generation
                        != m_searchGeneration) {
                        return;
                    }

                    m_searchProgressValues[job] = 100;
                    ++m_searchCompletedRoots;

                    if (job->error()) {
                        ++m_searchErrors;
                    }

                    Q_EMIT progressChanged();

                    if (m_searchCompletedRoots
                        >= m_searchTotalRoots) {
                        m_searchInProgress = false;

                        m_searchRenderTimer.stop();
                        Q_EMIT resultsChanged();
                        Q_EMIT finished();
                    } else {
                        scheduleSearchRender();
                    }
                });
        }
    }

Q_SIGNALS:
    void resultsChanged();
    void progressChanged();
    void finished();

private:
    void scheduleSearchRender()
    {
        if (!m_searchRenderTimer.isActive()) m_searchRenderTimer.start();
    }

    QList<FileInfo> m_pendingFiles;
    QList<QPointer<KIO::ListJob>> m_searchJobs;
    QHash<KJob *, int> m_searchProgressValues;
    QSet<QString> m_searchSeenUrls;
    QTimer m_searchRenderTimer;
    quint64 m_searchGeneration = 0;
    int m_searchTotalRoots = 0;
    int m_searchCompletedRoots = 0;
    int m_searchErrors = 0;
    bool m_searchInProgress = false;
    bool m_showHiddenFiles = false;
};
