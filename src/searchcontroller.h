/*
 * Asynchronous KIO filename search, filtering and result batching.
 * Extracted during the 0.21.0 architecture refactor.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "browsercommon.h"
#include "searchquery.h"
#include "searchmatcher.h"
#include "searchplan.h"

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
#include <memory>
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
        QStringLiteral("search"),
        QUrl::FullyDecoded);
}

inline int searchIntParameter(
    const QUrl &url,
    const QString &name,
    int fallback)
{
    bool ok = false;
    const int value =
        QUrlQuery(url).queryItemValue(name, QUrl::FullyDecoded).toInt(&ok);

    return ok ? value : fallback;
}

inline QUrl searchBaseFromUrl(const QUrl &url)
{
    const QString base =
        QUrlQuery(url).queryItemValue(
            QStringLiteral("base"),
            QUrl::FullyDecoded);

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

    // Stage 3B.1 & 3B.2 & 3B.3 & 3D.1 Advanced filters
    SearchQuery::NameMode nameMode = SearchQuery::NameMode::Auto;
    bool matchCase = false;
    bool recursive = true;
    QStringList extensions;
    SearchQuery::TargetType targetType = SearchQuery::TargetType::All;
    QList<SearchQuery::Category> categories;
    QStringList mimeTypes;
    std::optional<qint64> dateMin;
    std::optional<qint64> dateMax;
    std::optional<qint64> sizeMin;
    std::optional<qint64> sizeMax;

    SearchQuery toQuery(const QUrl &base = QUrl()) const
    {
        SearchQuery query;
        query.setText(text);
        query.setScope(static_cast<SearchQuery::Scope>(std::clamp(scope, 0, 2)));
        query.setRecursive(recursive);
        if (base.isValid()) query.setBase(base);
        query.setLegacyType(type);
        query.setCategories(categories);
        query.setLegacyDate(date);
        query.setLegacySize(size);
        query.setNameMode(nameMode);
        query.setMatchCase(matchCase);
        query.setExtensions(extensions);
        query.setTargetType(targetType);
        query.setMimeTypes(mimeTypes);
        query.setDateRange(dateMin, dateMax);
        query.setSizeRange(sizeMin, sizeMax);
        return query;
    }

    void loadLocation(const QUrl &url)
    {
        const bool changed = !sameLocation(location, url);
        if (changed) text = searchQueryFromUrl(url);
        if (isSearchLocation(url)) {
            scope = std::clamp(searchIntParameter(url, QStringLiteral("scope"), 2), 0, 2);
            type = std::clamp(searchIntParameter(url, QStringLiteral("type"), 0), 0, 6);
            date = std::clamp(searchIntParameter(url, QStringLiteral("date"), 0), 0, 4);
            size = std::clamp(searchIntParameter(url, QStringLiteral("size"), 0), 0, 4);

            const SearchQuery sq = SearchQuery::fromUrl(url);
            recursive = sq.recursive();
            nameMode = sq.nameMode();
            matchCase = sq.matchCase();
            extensions = sq.extensions();
            targetType = sq.targetType();
            categories = sq.categories();
            mimeTypes = sq.mimeTypes();
            dateMin = sq.dateMin();
            dateMax = sq.dateMax();
            sizeMin = sq.sizeMin();
            sizeMax = sq.sizeMax();

            // Bidirectional synchronization between legacy preset type and categories
            if (categories.isEmpty() && type >= 2 && type <= 6) {
                categories = {static_cast<SearchQuery::Category>(type)};
            } else if (categories.size() == 1) {
                type = static_cast<int>(categories.first());
            } else if (categories.size() > 1) {
                type = 0;
            }
        } else if (changed) {
            scope = sameLocation(url, kThisPcUrl) ? 2 : 0;
            type = date = size = 0;
            recursive = true;
            nameMode = SearchQuery::NameMode::Auto;
            matchCase = false;
            extensions.clear();
            targetType = SearchQuery::TargetType::All;
            categories.clear();
            mimeTypes.clear();
            dateMin.reset();
            dateMax.reset();
            sizeMin.reset();
            sizeMax.reset();
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
    bool wasCanceled() const { return m_wasCanceled; }
    int totalRoots() const { return m_searchTotalRoots; }
    int completedRoots() const { return m_searchCompletedRoots; }
    int errorCount() const { return m_searchErrors; }
    const QList<FileInfo> &files() const { return m_pendingFiles; }
    void setShowHiddenFiles(bool show) { m_showHiddenFiles = show; }

    bool isDeterminateProgress() const
    {
        return m_searchInProgress && m_searchTotalRoots > 0 && progressPercent() > 0;
    }

    int progressPercent() const
    {
        // One job per root on the legacy path; advanced queries may run several
        // jobs per root (Unicode variants), so average over jobs, not roots.
        if (m_searchTotalRoots <= 0 || m_searchProgressValues.isEmpty()) return 0;
        unsigned long total = 0;
        for (int value : m_searchProgressValues)
            total += static_cast<unsigned long>(std::clamp(value, 0, 100));
        return std::clamp(static_cast<int>(total / static_cast<unsigned long>(m_searchProgressValues.size())), 0, 100);
    }

    static QString formatResultCount(int count, bool polish)
    {
        if (!polish) {
            return count == 1
                ? QStringLiteral("1 result")
                : QStringLiteral("%1 results").arg(count);
        }

        if (count == 1) {
            return QStringLiteral("1 wynik");
        }

        const int mod10 = count % 10;
        const int mod100 = count % 100;

        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 10 || mod100 >= 20)) {
            return QStringLiteral("%1 wyniki").arg(count);
        }

        return QStringLiteral("%1 wyników").arg(count);
    }

    QString statusText(int visibleCount, int scope, bool recursive = true) const
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
                    : (recursive
                        ? trLocal(
                            "Bieżący folder i podfoldery",
                            "Current folder and subfolders")
                        : trLocal(
                            "Tylko bieżący folder",
                            "Current folder only")));

        const bool polish = isPolish();

        if (isRunning()) {
            if (visibleCount == 0) {
                return polish
                    ? QStringLiteral(
                        "Wyszukiwanie… • %1/%2 lokalizacji • %3")
                        .arg(completedRoots())
                        .arg(totalRoots())
                        .arg(scopeLabel)
                    : QStringLiteral(
                        "Searching… • %1/%2 locations • %3")
                        .arg(completedRoots())
                        .arg(totalRoots())
                        .arg(scopeLabel);
            }
            const QString countStr = formatResultCount(visibleCount, polish);
            return polish
                ? QStringLiteral(
                    "%1 • %2/%3 lokalizacji • %4")
                    .arg(countStr)
                    .arg(completedRoots())
                    .arg(totalRoots())
                    .arg(scopeLabel)
                : QStringLiteral(
                    "%1 • %2/%3 locations • %4")
                    .arg(countStr)
                    .arg(completedRoots())
                    .arg(totalRoots())
                    .arg(scopeLabel);
        } else {
            QString message;
            if (m_wasCanceled) {
                if (visibleCount == 0) {
                    message = polish
                        ? QStringLiteral("Wyszukiwanie anulowane • brak wyników • zakres: %1").arg(scopeLabel)
                        : QStringLiteral("Search canceled • no results • scope: %1").arg(scopeLabel);
                } else {
                    const QString countStr = formatResultCount(visibleCount, polish);
                    message = polish
                        ? QStringLiteral("Wyszukiwanie anulowane • %1 • zakres: %2").arg(countStr).arg(scopeLabel)
                        : QStringLiteral("Search canceled • %1 • scope: %2").arg(countStr).arg(scopeLabel);
                }
            } else if (visibleCount == 0) {
                message = polish
                    ? QStringLiteral(
                        "Brak wyników • zakres: %1")
                        .arg(scopeLabel)
                    : QStringLiteral(
                        "No results • scope: %1")
                        .arg(scopeLabel);
            } else {
                const QString countStr = formatResultCount(visibleCount, polish);
                message = polish
                    ? QStringLiteral(
                        "%1 • zakres: %2")
                        .arg(countStr)
                        .arg(scopeLabel)
                    : QStringLiteral(
                        "%1 • scope: %2")
                        .arg(countStr)
                        .arg(scopeLabel);
            }

            if (errorCount() > 0) {
                message +=
                    polish
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

    void setRunningForTest(bool running, int totalRoots = 1, int completedRoots = 0, int errors = 0)
    {
        m_searchInProgress = running;
        m_searchTotalRoots = totalRoots;
        m_searchCompletedRoots = completedRoots;
        m_searchErrors = errors;
        if (running) m_wasCanceled = false;
    }

    void setCanceledForTest(bool canceled)
    {
        m_wasCanceled = canceled;
    }

    void setProgressPercentForTest(int percent)
    {
        m_searchProgressValues.clear();
        if (percent >= 0) {
            m_searchProgressValues.insert(reinterpret_cast<KJob *>(1), percent);
        }
        Q_EMIT progressChanged();
    }

    void addPendingFileForTest(const FileInfo &file)
    {
        m_pendingFiles.push_back(file);
        scheduleSearchRender();
    }

    bool isRenderTimerActiveForTest() const
    {
        return m_searchRenderTimer.isActive();
    }

    // Keep partial results available when the user stops a search. A new
    // generation makes queued signals from canceled workers harmless.
    bool cancel(bool userRequested = false)
    {
        if (!m_searchInProgress && m_searchJobs.isEmpty()) return false;
        ++m_searchGeneration;
        for (const QPointer<KIO::ListJob> &job : std::as_const(m_searchJobs)) {
            if (job) job->kill(KJob::Quietly);
        }
        m_searchJobs.clear();
        m_searchProgressValues.clear();
        m_searchInProgress = false;
        m_wasCanceled = userRequested;
        m_searchRenderTimer.stop();
        return true;
    }

    static bool matchesFile(
        const FileInfo &file,
        QMimeDatabase &mimeDatabase,
        const Filters &filters)
    {
        return SearchMatcher::matchesLegacy(
            file, mimeDatabase, filters.type, filters.date, filters.size);
    }

    // Unchanged 0.40.1 entry point: one recursive filenamesearch job per root.
    void start(const QString &query, const QList<QUrl> &roots, bool showHiddenFiles)
    {
        SearchPlan plan;
        plan.roots = roots;
        plan.workerPatterns = {query};
        start(plan, showHiddenFiles);
    }

    // Routes a thispcsearch:/ location through the planner. Returns false when
    // the query was rejected as invalid (nothing is searched in that case).
    bool startFromLocation(const QUrl &url, const QList<QUrl> &roots, bool showHiddenFiles)
    {
        const SearchRequest request = SearchRequest::fromLocation(url);
        if (!request.isRunnable()) {
            reject(request.problem());
            return false;
        }
        start(SearchPlanner::plan(request.query(), roots), showHiddenFiles);
        return true;
    }

    // An invalid query must not run with a criterion silently dropped (that would
    // widen the search). Finishes immediately with one reported error.
    void reject(const QString &reason)
    {
        cancel();
        m_pendingFiles.clear();
        m_searchSeenUrls.clear();
        m_ingestNameFilter.reset();
        m_searchTotalRoots = 0;
        m_searchCompletedRoots = 0;
        m_searchErrors = 1;
        m_lastError = reason;
        m_searchInProgress = false;
        ++m_searchGeneration;
        Q_EMIT progressChanged();
        Q_EMIT resultsChanged();
        Q_EMIT finished();
    }

    QString lastError() const { return m_lastError; }

    void start(const SearchPlan &plan, bool showHiddenFiles)
    {
        cancel();
        m_pendingFiles.clear();
        m_searchSeenUrls.clear();
        m_lastError.clear();
        m_ingestNameFilter = plan.ingestNameFilter;
        m_showHiddenFiles = showHiddenFiles;
        const QList<QUrl> &roots = plan.roots;
        m_searchTotalRoots = roots.size();
        m_searchCompletedRoots = 0;
        m_searchErrors = 0;
        m_searchInProgress = true;
        m_wasCanceled = false;
        const quint64 generation = ++m_searchGeneration;
        Q_EMIT progressChanged();

        if (roots.isEmpty()) {
            m_searchInProgress = false;
            Q_EMIT resultsChanged();
            Q_EMIT finished();
            return;
        }

        // Non-recursive: a single plain listing of each root (name criterion is
        // applied at ingestion). Recursive: one filenamesearch job per pattern.
        QStringList patterns = plan.recursive ? plan.workerPatterns : QStringList{QString()};
        if (patterns.isEmpty()) patterns = {QStringLiteral(".*")};
        m_rootPendingJobs.assign(static_cast<qsizetype>(roots.size()), static_cast<int>(patterns.size()));
        m_rootFailed.assign(static_cast<qsizetype>(roots.size()), false);

        for (int rootIndex = 0; rootIndex < roots.size(); ++rootIndex) {
            const QUrl root = roots.at(rootIndex);
            for (const QString &pattern : std::as_const(patterns)) {
            KIO::ListJob *job =
                KIO::listDir(
                    plan.recursive
                        ? filenameSearchUrl(pattern, root)
                        : root,
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
                        const FileInfo file = fileInfoForEntry(root, entry);

                        if (name.isEmpty()
                            || rawName == QStringLiteral(".")
                            || rawName == QStringLiteral("..")
                            || (!m_showHiddenFiles
                                && fileInfoForEntry(root, entry).isHidden)) {
                            continue;
                        }

                        if (!file.url.isValid()
                            || file.url.isEmpty()) {
                            continue;
                        }

                        if (m_ingestNameFilter
                            && !m_ingestNameFilter->matches(file, m_mimeDatabase)) {
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
                [this, generation, job, rootIndex](KJob *) {
                    if (generation
                        != m_searchGeneration) {
                        return;
                    }

                    m_searchProgressValues[job] = 100;
                    if (job->error()) {
                        m_rootFailed[rootIndex] = true;
                        if (m_lastError.isEmpty()) {
                            m_lastError = job->errorString();
                        }
                    }

                    // A root is complete (and counted once) when its last job ends.
                    if (--m_rootPendingJobs[rootIndex] == 0) {
                        ++m_searchCompletedRoots;
                        if (m_rootFailed[rootIndex]) {
                            ++m_searchErrors;
                        }
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
    std::shared_ptr<const SearchMatcher::Prepared> m_ingestNameFilter;
    QList<int> m_rootPendingJobs;
    QList<bool> m_rootFailed;
    QString m_lastError;
    QTimer m_searchRenderTimer;
    quint64 m_searchGeneration = 0;
    int m_searchTotalRoots = 0;
    int m_searchCompletedRoots = 0;
    int m_searchErrors = 0;
    bool m_searchInProgress = false;
    bool m_wasCanceled = false;
    bool m_showHiddenFiles = false;
    QMimeDatabase m_mimeDatabase;
};
