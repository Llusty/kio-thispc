/*
 * Search planning and request classification (no KIO, no widgets).
 *
 *  - SearchRequest: a thispcsearch:/ location parsed once; knows whether it is a
 *    plain 0.40.1 query (executed by the unchanged legacy path) or an advanced
 *    one, whether it is runnable, and accepts/rejects result files.
 *  - SearchPlanner: turns an advanced query + roots into the worker jobs to run
 *    (name patterns for the KIO filenamesearch worker, recursion mode).
 *  - automaticRoots(): the only drives a "This PC" / "current drive" search may
 *    start from on its own: mounted, local, non-network filesystems.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "searchmatcher.h"
#include "searchquery.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>

// What the KIO worker is asked to do for one root.
struct SearchPlan {
    // One filenamesearch job per (root, pattern); patterns are NFC/NFD variants
    // or the match-everything pattern "*". Unused when !recursive.
    QStringList workerPatterns;
    // false: list only the direct children of each root with a plain listing job
    // and apply the name criterion at ingestion.
    bool recursive = true;
    QList<QUrl> roots;
    // Name criterion applied to entries as they arrive (non-recursive mode).
    std::shared_ptr<const SearchMatcher::Prepared> ingestNameFilter;
};

class SearchRequest final
{
public:
    struct LegacyFilters {
        int type = 0;
        int date = 0;
        int size = 0;
    };

    // `legacyOverride` carries the pane's current preset filters, which can change
    // after the URL was built (the URL is re-synced right after).
    static SearchRequest fromLocation(const QUrl &url, const LegacyFilters *legacyOverride = nullptr)
    {
        SearchRequest request;
        request.m_query = SearchQuery::fromUrl(url);
        request.m_advanced = request.m_query.usesAdvancedFeatures();
        if (legacyOverride) {
            request.m_legacy = *legacyOverride;
        } else {
            request.m_legacy = {request.m_query.legacyType(), request.m_query.legacyDate(),
                                request.m_query.legacySize()};
        }
        if (request.m_advanced) {
            SearchQuery effective = request.m_query;
            effective.setLegacyType(request.m_legacy.type);
            effective.setLegacyDate(request.m_legacy.date);
            effective.setLegacySize(request.m_legacy.size);
            request.m_prepared = std::make_shared<SearchMatcher::Prepared>(effective);
        }
        return request;
    }

    // False for plain 0.40.1 URLs: those run through the unchanged legacy code.
    bool isAdvanced() const { return m_advanced; }
    bool isRunnable() const { return !m_advanced || m_query.isValid(); }
    QString problem() const { return m_query.firstProblem(); }
    const SearchQuery &query() const { return m_query; }

    bool accepts(const FileInfo &file, QMimeDatabase &database) const
    {
        if (m_prepared) return m_prepared->matches(file, database);
        return SearchMatcher::matchesLegacy(file, database, m_legacy.type, m_legacy.date, m_legacy.size);
    }

private:
    SearchQuery m_query;
    LegacyFilters m_legacy;
    bool m_advanced = false;
    std::shared_ptr<const SearchMatcher::Prepared> m_prepared;
};

namespace SearchPlanner {

inline bool hasWildcardCharacters(const QString &text)
{
    return text.contains(QLatin1Char('*')) || text.contains(QLatin1Char('?')) || text.contains(QLatin1Char('['));
}

inline QStringList workerPatterns(const SearchQuery &query)
{
    const QString text = query.text();
    // The filenamesearch worker compiles its `search` query with QRegularExpression.
    // If the text is empty, the valid match-everything regex is ".*".
    if (text.isEmpty()) return {QStringLiteral(".*")};

    const bool glob = query.effectiveNameMode() == SearchQuery::NameMode::Glob;
    if (glob) {
        // Glob pattern: convert wildcard syntax (*, ?, [...]) to a valid regular expression.
        const QString regexPattern = QRegularExpression::wildcardToRegularExpression(text);
        const QRegularExpression testRe(regexPattern);
        if (testRe.isValid()) {
            QStringList patterns{regexPattern};
            const QString nfd = text.normalized(QString::NormalizationForm_D);
            if (nfd != text) {
                const QString nfdRegex = QRegularExpression::wildcardToRegularExpression(nfd);
                if (QRegularExpression(nfdRegex).isValid() && nfdRegex != regexPattern) {
                    patterns.append(nfdRegex);
                }
            }
            return patterns;
        }
        // If wildcard conversion produced an invalid regex (e.g. malformed syntax),
        // fallback to ".*" so KIO returns entries without error, and SearchMatcher filters them.
        return {QStringLiteral(".*")};
    }

    // Substring pattern: escape regular expression metacharacters so that filenames
    // with '+', '(', '[', etc. are matched literally as substrings by QRegularExpression,
    // avoiding PCRE parse errors or unintentional quantifier interpretation.
    const QString escaped = QRegularExpression::escape(text);
    QStringList patterns{escaped};
    const QString nfd = text.normalized(QString::NormalizationForm_D);
    if (nfd != text) {
        const QString nfdEscaped = QRegularExpression::escape(nfd);
        if (nfdEscaped != escaped) {
            patterns.append(nfdEscaped);
        }
    }
    return patterns;
}

inline SearchPlan plan(const SearchQuery &query, const QList<QUrl> &roots)
{
    SearchPlan result;
    result.roots = roots;
    result.recursive = query.recursive();
    if (result.recursive) {
        result.workerPatterns = workerPatterns(query);
    }
    // Always attach the prepared filter at ingestion so that criteria not filtered
    // by filenamesearch (or all files in search.files()) are strictly matched.
    if (query.usesAdvancedFeatures()) {
        result.ingestNameFilter = std::make_shared<SearchMatcher::Prepared>(query);
    }
    return result;
}

inline bool isNetworkFilesystem(const QString &fileSystem)
{
    static const QSet<QString> network{
        QStringLiteral("nfs"), QStringLiteral("nfs4"), QStringLiteral("cifs"), QStringLiteral("smb"),
        QStringLiteral("smb2"), QStringLiteral("smb3"), QStringLiteral("smbfs"), QStringLiteral("afs"),
        QStringLiteral("ceph"), QStringLiteral("glusterfs"), QStringLiteral("9p"), QStringLiteral("lustre"),
        QStringLiteral("ncpfs"), QStringLiteral("davfs"), QStringLiteral("sshfs"),
        QStringLiteral("fuse.sshfs"), QStringLiteral("fuse.davfs"), QStringLiteral("fuse.rclone"),
        QStringLiteral("fuse.curlftpfs"), QStringLiteral("curlftpfs"), QStringLiteral("fuse.gvfsd-fuse")};
    return network.contains(fileSystem.trimmed().toLower());
}

// Roots a whole-computer / current-drive search may use without the user naming
// a location: mounted local drives only. Unmounted volumes have no target URL;
// network filesystems and non-file schemes are never scanned automatically.
inline QList<QUrl> automaticRoots(const QList<DriveInfo> &drives)
{
    QList<QUrl> roots;
    for (const DriveInfo &drive : drives) {
        if (!drive.isMounted || !drive.targetUrl.isValid() || !drive.targetUrl.isLocalFile()) continue;
        if (isNetworkFilesystem(drive.fileSystem)) continue;
        roots.push_back(drive.targetUrl);
    }
    return roots;
}

} // namespace SearchPlanner
