/*
 * Search matcher engine: pure predicates over FileInfo. No I/O, no KIO.
 *
 * Semantics (Stage 2):
 *  - Different criteria combine with AND; alternatives inside one criterion
 *    (extensions, MIME patterns + categories) combine with OR.
 *  - Inactive criteria impose no restriction.
 *  - Names: NFC-normalized on both sides; substring or whole-name glob
 *    (* ? [..]); case-insensitive unless the query asks for case sensitivity.
 *  - Extensions apply to files only and need a non-empty stem, so ".gitignore"
 *    has no extension. Multi-part extensions ("tar.gz") match as a suffix.
 *  - Folders never satisfy extension or size criteria.
 *  - Files whose size is unknown (< 0) or whose mtime is unknown (<= 0) never
 *    satisfy a size / date criterion.
 *  - Date and size ranges are inclusive on both ends.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "searchquery.h"

#include <QDateTime>
#include <QMimeDatabase>
#include <QMimeType>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>

class SearchMatcher final
{
public:
    // Category predicate shared by the legacy `type` filter (2..6) and the
    // Stage 2 `cat` criterion. Never matches folders.
    static bool matchesCategory(int category, bool isDir, const QString &mimeName)
    {
        if (isDir) return false;
        switch (category) {
        case 2:
            return mimeName.startsWith(QStringLiteral("image/"));
        case 3:
            return mimeName.startsWith(QStringLiteral("text/"))
                || mimeName == QStringLiteral("application/pdf")
                || mimeName.contains(QStringLiteral("officedocument"))
                || mimeName.contains(QStringLiteral("opendocument"))
                || mimeName.contains(QStringLiteral("msword"))
                || mimeName.contains(QStringLiteral("rtf"));
        case 4:
            return mimeName.startsWith(QStringLiteral("audio/"));
        case 5:
            return mimeName.startsWith(QStringLiteral("video/"));
        case 6:
            return mimeName.contains(QStringLiteral("zip"))
                || mimeName.contains(QStringLiteral("archive"))
                || mimeName.contains(QStringLiteral("compressed"))
                || mimeName.contains(QStringLiteral("rar"))
                || mimeName.contains(QStringLiteral("7z"))
                || mimeName.contains(QStringLiteral("tar"));
        default:
            return false;
        }
    }

    // 0.40.1 preset matching, behavior-identical (verified differentially in
    // tests/search-query.cpp). referenceSecsSinceEpoch > 0 pins "now".
    static bool matchesLegacy(
        const FileInfo &file,
        QMimeDatabase &mimeDatabase,
        int typeFilter,
        int dateFilter,
        int sizeFilter,
        qint64 referenceSecsSinceEpoch = 0)
    {
        const QMimeType mime = resolvedMimeType(file, mimeDatabase);

        if (typeFilter != 0) {
            const QString mimeName = mime.isValid() ? mime.name() : file.mimeType;
            bool typeMatches = false;
            if (typeFilter == 1) typeMatches = file.isDir;
            else if (typeFilter >= 2 && typeFilter <= 6)
                typeMatches = matchesCategory(typeFilter, file.isDir, mimeName);
            else typeMatches = true;
            if (!typeMatches) return false;
        }

        if (dateFilter != 0) {
            if (file.modificationTime <= 0) return false;
            qint64 days = 0;
            switch (dateFilter) {
            case 1: days = 1; break;
            case 2: days = 7; break;
            case 3: days = 30; break;
            case 4: days = 365; break;
            default: break;
            }
            const qint64 now = (referenceSecsSinceEpoch > 0)
                ? referenceSecsSinceEpoch
                : QDateTime::currentSecsSinceEpoch();
            if (file.modificationTime < now - days * 24 * 60 * 60) return false;
        }

        if (sizeFilter != 0) {
            if (file.isDir || file.size < 0) return false;
            const qint64 mib = 1024LL * 1024LL;
            const qint64 gib = 1024LL * mib;
            bool sizeMatches = false;
            switch (sizeFilter) {
            case 1: sizeMatches = file.size < mib; break;
            case 2: sizeMatches = file.size >= mib && file.size < 100LL * mib; break;
            case 3: sizeMatches = file.size >= 100LL * mib && file.size < gib; break;
            case 4: sizeMatches = file.size >= gib; break;
            default: sizeMatches = true; break;
            }
            if (!sizeMatches) return false;
        }

        return true;
    }

    static bool matchesExtension(const QString &fileName, const QString &normalizedExt)
    {
        if (fileName.isEmpty() || normalizedExt.isEmpty()) return false;
        const QString name = fileName.normalized(QString::NormalizationForm_C);
        const QString ext = QLatin1Char('.') + normalizedExt.normalized(QString::NormalizationForm_C);
        // The stem before the extension must be non-empty (".bashrc" has none).
        return name.size() > ext.size() && name.endsWith(ext, Qt::CaseInsensitive);
    }

    static bool matchesAnyExtension(const QString &fileName, const QStringList &extensions)
    {
        if (extensions.isEmpty()) return true;
        for (const QString &ext : extensions)
            if (matchesExtension(fileName, ext)) return true;
        return false;
    }

    // "type/subtype", "type/*", or any ancestor MIME (e.g. text/plain matches
    // text/x-python through inheritance).
    static bool matchesMimePattern(const QString &fileMime, const QMimeType &resolved, const QString &pattern)
    {
        const QString pat = pattern.toLower();
        const QString explicitName = fileMime.toLower();
        const QString resolvedName = resolved.isValid() ? resolved.name().toLower() : QString();
        if (pat.endsWith(QLatin1String("/*"))) {
            const QString prefix = pat.left(pat.size() - 1); // keeps trailing '/'
            return explicitName.startsWith(prefix) || (!resolvedName.isEmpty() && resolvedName.startsWith(prefix));
        }
        if (explicitName == pat || resolvedName == pat) return true;
        return resolved.isValid() && resolved.inherits(pat);
    }

    // Query compiled once per search/render (glob regex, normalized text).
    class Prepared final
    {
    public:
        explicit Prepared(const SearchQuery &query)
            : m_query(query)
        {
            const QString text = m_query.text().normalized(QString::NormalizationForm_C);
            m_text = text;
            if (m_query.effectiveNameMode() == SearchQuery::NameMode::Glob && !text.isEmpty()) {
                QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
                if (!m_query.matchCase()) options |= QRegularExpression::CaseInsensitiveOption;
                m_glob = QRegularExpression(QRegularExpression::wildcardToRegularExpression(text), options);
            }
        }

        const SearchQuery &query() const { return m_query; }

        bool matchesName(const FileInfo &file) const
        {
            if (m_text.isEmpty()) return true;
            const QString name = file.name.normalized(QString::NormalizationForm_C);
            if (m_glob.isValid() && !m_glob.pattern().isEmpty()) return m_glob.match(name).hasMatch();
            return name.contains(m_text, m_query.matchCase() ? Qt::CaseSensitive : Qt::CaseInsensitive);
        }

        bool matches(const FileInfo &file, QMimeDatabase &mimeDatabase, qint64 referenceSecsSinceEpoch = 0) const
        {
            if (!matchesName(file)) return false;

            if (m_query.targetType() == SearchQuery::TargetType::FilesOnly && file.isDir) return false;
            if (m_query.targetType() == SearchQuery::TargetType::FoldersOnly && !file.isDir) return false;

            if (m_query.legacyType() != 0 || m_query.legacyDate() != 0 || m_query.legacySize() != 0) {
                if (!matchesLegacy(file, mimeDatabase, m_query.legacyType(), m_query.legacyDate(),
                                   m_query.legacySize(), referenceSecsSinceEpoch))
                    return false;
            }

            if (!m_query.extensions().isEmpty()) {
                if (file.isDir || !matchesAnyExtension(file.name, m_query.extensions())) return false;
            }

            if (!m_query.categories().isEmpty()) {
                const QMimeType mime = resolvedMimeType(file, mimeDatabase);
                const QString mimeName = mime.isValid() ? mime.name() : file.mimeType;
                bool any = false;
                for (SearchQuery::Category category : m_query.categories()) {
                    if (matchesCategory(static_cast<int>(category), file.isDir, mimeName)) { any = true; break; }
                }
                if (!any) return false;
            }

            if (!m_query.mimeTypes().isEmpty()) {
                const QMimeType mime = resolvedMimeType(file, mimeDatabase);
                bool any = false;
                for (const QString &pattern : m_query.mimeTypes()) {
                    if (matchesMimePattern(file.mimeType, mime, pattern)) { any = true; break; }
                }
                if (!any) return false;
            }

            if (m_query.dateMin().has_value() || m_query.dateMax().has_value()) {
                if (file.modificationTime <= 0) return false;
                if (m_query.dateMin().has_value() && file.modificationTime < *m_query.dateMin()) return false;
                if (m_query.dateMax().has_value() && file.modificationTime > *m_query.dateMax()) return false;
            }

            if (m_query.sizeMin().has_value() || m_query.sizeMax().has_value()) {
                if (file.isDir || file.size < 0) return false;
                if (m_query.sizeMin().has_value() && file.size < *m_query.sizeMin()) return false;
                if (m_query.sizeMax().has_value() && file.size > *m_query.sizeMax()) return false;
            }

            return true;
        }

    private:
        SearchQuery m_query;
        QString m_text;
        QRegularExpression m_glob;
    };

    // Convenience wrapper; prefer Prepared when matching many files.
    static bool matches(
        const FileInfo &file,
        QMimeDatabase &mimeDatabase,
        const SearchQuery &query,
        qint64 referenceSecsSinceEpoch = 0)
    {
        return Prepared(query).matches(file, mimeDatabase, referenceSecsSinceEpoch);
    }
};
