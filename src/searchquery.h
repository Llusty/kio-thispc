/*
 * Search query model: normalization, validation, serialization and scope contract.
 *
 * URL contract (thispcsearch:/?...). Parameters from 0.40.1 keep their meaning:
 *   search, scope(0..2), base, type(0..6), date(0..4), size(0..4)
 * Stage 2 adds optional parameters; a URL that uses none of them is
 * byte-identical to a 0.40.1 URL and is executed by the unchanged legacy path:
 *   target=files|folders       restrict to files or folders
 *   match=substring|glob       name matching mode (default substring)
 *   case=0|1                   case-sensitive name matching (default 0)
 *   ext=a,b.c                  extensions (multi-part allowed), OR, files only
 *   mimes=image/png,application/pdf MIME names/patterns (wildcards like image/<wildcard> allowed), OR
 *   cat=images,documents,audio,video,archives   MIME categories, OR with mimes
 *                              YYYY-MM-DD or YYYY-MM-DDTHH:mm[:ss] in local time
 *   sizemin / sizemax          bytes, inclusive; plain integer or IEC units
 *                              (B, KiB, MiB, GiB, TiB, PiB), files only
 * Different criteria combine with AND, alternatives inside one criterion with OR.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cstdint>
#include <optional>

class SearchQuery final
{
public:
    enum class Scope {
        CurrentFolder = 0,
        CurrentDrive = 1,
        ThisPc = 2,
        CustomFolder = 3
    };

    enum class TargetType {
        All = 0,
        FilesOnly = 1,
        FoldersOnly = 2
    };

    enum class NameMode {
        Auto = 0,
        Substring = 1,
        Glob = 2
    };

    static bool isWildcardPattern(const QString &text)
    {
        return text.contains(QLatin1Char('*')) || text.contains(QLatin1Char('?'));
    }

    // Values equal the legacy `type` indices so that both share one predicate.
    enum class Category {
        Images = 2,
        Documents = 3,
        Audio = 4,
        Video = 5,
        Archives = 6
    };

    enum class ScopeViolation {
        None,
        DisallowedNetworkScheme,
        UnmountedVolume,
        InvalidTargetUrl
    };

    SearchQuery() = default;

    // --- Query text (NFC-normalized and trimmed) ---
    QString text() const { return m_text; }
    void setText(const QString &text) { m_text = text.normalized(QString::NormalizationForm_C).trimmed(); }

    NameMode nameMode() const { return m_nameMode; }
    void setNameMode(NameMode mode) { m_nameMode = mode; }

    NameMode effectiveNameMode() const
    {
        if (m_nameMode == NameMode::Auto) {
            return isWildcardPattern(m_text) ? NameMode::Glob : NameMode::Substring;
        }
        return m_nameMode;
    }

    bool matchCase() const { return m_matchCase; }
    void setMatchCase(bool matchCase) { m_matchCase = matchCase; }

    // --- Scope contract ---
    Scope scope() const { return m_scope; }
    void setScope(Scope scope) { m_scope = scope; }

    QUrl base() const { return m_base; }
    void setBase(const QUrl &base) { m_base = normalizedUrl(base); }

    // false = only the direct children of each root (current folder only).
    bool recursive() const { return m_recursive; }
    void setRecursive(bool recursive) { m_recursive = recursive; }

    // --- Explicit files/folders restriction (independent of legacy type) ---
    TargetType targetType() const { return m_targetType; }
    void setTargetType(TargetType type) { m_targetType = type; }

    // --- 0.40.1 preset filters (independent AND criteria) ---
    int legacyType() const { return m_legacyType; }
    void setLegacyType(int type) { m_legacyType = std::clamp(type, 0, 6); }
    int legacyDate() const { return m_legacyDate; }
    void setLegacyDate(int date) { m_legacyDate = std::clamp(date, 0, 4); }
    int legacySize() const { return m_legacySize; }
    void setLegacySize(int size) { m_legacySize = std::clamp(size, 0, 4); }

    // --- Stage 2 criteria; alternatives inside each list combine with OR ---
    const QStringList &extensions() const { return m_extensions; }
    void setExtensions(const QStringList &extensions)
    {
        m_extensions.clear();
        for (const QString &ext : extensions) {
            const QString normalized = normalizeExtension(ext);
            if (!normalized.isEmpty() && !m_extensions.contains(normalized)) {
                m_extensions.append(normalized);
            }
        }
    }

    const QStringList &mimeTypes() const { return m_mimeTypes; }
    void setMimeTypes(const QStringList &mimeTypes)
    {
        m_mimeTypes.clear();
        for (const QString &mime : mimeTypes) {
            const QString trimmed = mime.trimmed().toLower();
            if (!trimmed.isEmpty() && !m_mimeTypes.contains(trimmed)) {
                m_mimeTypes.append(trimmed);
            }
        }
    }

    const QList<Category> &categories() const { return m_categories; }
    void setCategories(const QList<Category> &categories)
    {
        m_categories.clear();
        for (Category category : categories) {
            if (!m_categories.contains(category)) m_categories.append(category);
        }
    }

    // Modification time in seconds since epoch, inclusive bounds; nullopt = open.
    std::optional<qint64> dateMin() const { return m_dateMin; }
    std::optional<qint64> dateMax() const { return m_dateMax; }
    void setDateRange(std::optional<qint64> minSecs, std::optional<qint64> maxSecs)
    {
        m_dateMin = minSecs;
        m_dateMax = maxSecs;
    }

    // File size in bytes (>= 0), inclusive bounds; nullopt = open.
    std::optional<qint64> sizeMin() const { return m_sizeMin; }
    std::optional<qint64> sizeMax() const { return m_sizeMax; }
    void setSizeRange(std::optional<qint64> minBytes, std::optional<qint64> maxBytes)
    {
        m_sizeMin = (minBytes.has_value() && *minBytes >= 0) ? minBytes : std::nullopt;
        m_sizeMax = (maxBytes.has_value() && *maxBytes >= 0) ? maxBytes : std::nullopt;
    }

    // --- Validation ---
    // Parse problems recorded by fromUrl(); an invalid query must never be run
    // with the offending criterion silently dropped (that would widen the search).
    const QStringList &errors() const { return m_errors; }

    bool isValid() const
    {
        if (!m_errors.isEmpty()) return false;
        if (m_dateMin.has_value() && m_dateMax.has_value() && *m_dateMin > *m_dateMax) return false;
        if (m_sizeMin.has_value() && m_sizeMax.has_value() && *m_sizeMin > *m_sizeMax) return false;
        return true;
    }

    QString firstProblem() const
    {
        if (!m_errors.isEmpty()) return m_errors.first();
        if (m_dateMin.has_value() && m_dateMax.has_value() && *m_dateMin > *m_dateMax)
            return QStringLiteral("datemin is later than datemax");
        if (m_sizeMin.has_value() && m_sizeMax.has_value() && *m_sizeMin > *m_sizeMax)
            return QStringLiteral("sizemin is larger than sizemax");
        return {};
    }

    // Counts active filtering criteria (both legacy presets and Stage 2 advanced filters).
    int activeFilterCount() const
    {
        int count = 0;
        if (m_targetType != TargetType::All) ++count;
        if (m_legacyType == 1 || (m_legacyType >= 2 && m_categories.isEmpty())) ++count;
        if (!m_extensions.isEmpty()) ++count;
        if (!m_categories.isEmpty()) ++count;
        if (!m_mimeTypes.isEmpty()) ++count;
        if (m_dateMin.has_value() || m_dateMax.has_value()) ++count;
        else if (m_legacyDate != 0) ++count;
        if (m_sizeMin.has_value() || m_sizeMax.has_value()) ++count;
        else if (m_legacySize != 0) ++count;
        return count;
    }

    // True when any criterion beyond the 0.40.1 parameters is in use (or the
    // URL was malformed in a Stage 2 parameter). Such queries use the new path.
    bool usesAdvancedFeatures() const
    {
        return !m_errors.isEmpty() || m_targetType != TargetType::All
            || m_nameMode != NameMode::Substring || m_matchCase || !m_recursive
            || !m_extensions.isEmpty() || !m_mimeTypes.isEmpty() || !m_categories.isEmpty()
            || m_dateMin.has_value() || m_dateMax.has_value()
            || m_sizeMin.has_value() || m_sizeMax.has_value();
    }

    // Explicit normalization helper (swaps inverted ranges, clamps presets).
    void normalize()
    {
        m_text = m_text.normalized(QString::NormalizationForm_C).trimmed();
        m_legacyType = std::clamp(m_legacyType, 0, 6);
        m_legacyDate = std::clamp(m_legacyDate, 0, 4);
        m_legacySize = std::clamp(m_legacySize, 0, 4);
        if (m_base.isValid()) m_base = normalizedUrl(m_base);
        if (m_dateMin.has_value() && m_dateMax.has_value() && *m_dateMin > *m_dateMax)
            std::swap(m_dateMin, m_dateMax);
        if (m_sizeMin.has_value() && m_sizeMax.has_value() && *m_sizeMin > *m_sizeMax)
            std::swap(m_sizeMin, m_sizeMax);
    }

    // --- Scope safety checks ---
    static bool isNetworkScheme(const QString &scheme)
    {
        return scheme == QLatin1String("smb") || scheme == QLatin1String("sftp")
            || scheme == QLatin1String("fish") || scheme == QLatin1String("ftp")
            || scheme == QLatin1String("nfs") || scheme == QLatin1String("webdav")
            || scheme == QLatin1String("webdavs") || scheme == QLatin1String("remote");
    }

    static ScopeViolation validateRootScope(const QUrl &root, const QList<DriveInfo> &availableDrives = {})
    {
        if (!root.isValid() || root.isEmpty()) return ScopeViolation::InvalidTargetUrl;
        if (isNetworkScheme(root.scheme())) return ScopeViolation::DisallowedNetworkScheme;
        for (const DriveInfo &drive : availableDrives) {
            if (!drive.isMounted && drive.targetUrl.isValid() && sameLocation(drive.targetUrl, root))
                return ScopeViolation::UnmountedVolume;
        }
        return ScopeViolation::None;
    }

    // ".TAR.GZ" / "..png" -> "tar.gz" / "png"
    static QString normalizeExtension(const QString &raw)
    {
        QString ext = raw.trimmed();
        while (ext.startsWith(QLatin1Char('.'))) ext.remove(0, 1);
        return ext.toLower();
    }

    // --- Value parsers (strict; nullopt = invalid) ---

    // "1048576", "10MiB", "1.5 GiB". Only binary IEC units are accepted; decimal
    // SI units (KB/MB/GB) are rejected as ambiguous. Exact integer arithmetic
    // with overflow detection; fractions round down to whole bytes.
    static std::optional<qint64> parseSizeBytes(const QString &raw)
    {
        static const QRegularExpression re(
            QStringLiteral("^\\s*(\\d{1,19})(?:[.,](\\d{1,12}))?\\s*(B|KiB|MiB|GiB|TiB|PiB)?\\s*$"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = re.match(raw);
        if (!m.hasMatch()) return std::nullopt;
        const QString unit = m.captured(3).toLower();
        int shift = 0;
        if (unit == QLatin1String("kib")) shift = 10;
        else if (unit == QLatin1String("mib")) shift = 20;
        else if (unit == QLatin1String("gib")) shift = 30;
        else if (unit == QLatin1String("tib")) shift = 40;
        else if (unit == QLatin1String("pib")) shift = 50;
        bool ok = false;
        const quint64 whole = m.captured(1).toULongLong(&ok);
        if (!ok) return std::nullopt;
        // Result >= whole << shift, so anything above this can never fit in qint64;
        // rejecting early also keeps the 128-bit intermediate below (<= 103 bits) exact.
        if (shift > 0 && whole > (static_cast<quint64>(INT64_MAX) >> shift)) return std::nullopt;
        const QString fractionText = m.captured(2);
        if (!fractionText.isEmpty() && shift == 0) {
            // fractional bytes are meaningless
            bool zero = true;
            for (QChar c : fractionText) if (c != QLatin1Char('0')) zero = false;
            if (!zero) return std::nullopt;
        }
        quint64 baseBytes = whole << shift;
        if (!fractionText.isEmpty() && shift > 0) {
            quint64 denominator = 1;
            for (int i = 0; i < fractionText.size(); ++i) denominator *= 10;
            const quint64 fractionVal = fractionText.toULongLong();
            const quint64 addBytes = (fractionVal << shift) / denominator;
            if (baseBytes > static_cast<quint64>(INT64_MAX) - addBytes) return std::nullopt;
            baseBytes += addBytes;
        }
        if (baseBytes > static_cast<quint64>(INT64_MAX)) return std::nullopt;
        return static_cast<qint64>(baseBytes);
    }

    // Epoch seconds (digits only), YYYY-MM-DD or YYYY-MM-DDTHH:mm[:ss] in `zone`.
    // Date-only: lower bound = first instant of that local day, upper bound =
    // last second of that local day, computed from the next day's start so that
    // 23 h / 25 h DST days are handled correctly.
    static std::optional<qint64> parseDateBoundary(const QString &raw, bool upperBound,
                                                   const QTimeZone &zone = QTimeZone::systemTimeZone())
    {
        const QString text = raw.trimmed();
        static const QRegularExpression epochRe(QStringLiteral("^\\d{1,18}$"));
        if (epochRe.match(text).hasMatch()) {
            bool ok = false;
            const qint64 value = text.toLongLong(&ok);
            return ok ? std::optional<qint64>(value) : std::nullopt;
        }
        static const QRegularExpression isoRe(QStringLiteral(
            "^(\\d{4})-(\\d{2})-(\\d{2})(?:[T ](\\d{2}):(\\d{2})(?::(\\d{2}))?)?$"));
        const QRegularExpressionMatch m = isoRe.match(text);
        if (!m.hasMatch() || !zone.isValid()) return std::nullopt;
        const QDate date(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
        if (!date.isValid()) return std::nullopt;
        if (m.captured(4).isEmpty()) {
            if (!upperBound) {
                const QDateTime start = date.startOfDay(zone);
                return start.isValid() ? std::optional<qint64>(start.toSecsSinceEpoch()) : std::nullopt;
            }
            const QDateTime next = date.addDays(1).startOfDay(zone);
            return next.isValid() ? std::optional<qint64>(next.toSecsSinceEpoch() - 1) : std::nullopt;
        }
        const int hour = m.captured(4).toInt(), minute = m.captured(5).toInt();
        const bool hasSeconds = !m.captured(6).isEmpty();
        const int second = hasSeconds ? m.captured(6).toInt() : (upperBound ? 59 : 0);
        const QTime time(hour, minute, second);
        if (!time.isValid()) return std::nullopt;
        const QDateTime value(date, time, zone);
        return value.isValid() ? std::optional<qint64>(value.toSecsSinceEpoch()) : std::nullopt;
    }

    static std::optional<Category> categoryFromName(const QString &name)
    {
        const QString n = name.trimmed().toLower();
        if (n == QLatin1String("images")) return Category::Images;
        if (n == QLatin1String("documents")) return Category::Documents;
        if (n == QLatin1String("audio")) return Category::Audio;
        if (n == QLatin1String("video")) return Category::Video;
        if (n == QLatin1String("archives")) return Category::Archives;
        return std::nullopt;
    }

    static QString categoryName(Category category)
    {
        switch (category) {
        case Category::Images: return QStringLiteral("images");
        case Category::Documents: return QStringLiteral("documents");
        case Category::Audio: return QStringLiteral("audio");
        case Category::Video: return QStringLiteral("video");
        case Category::Archives: return QStringLiteral("archives");
        }
        return {};
    }

    // --- URL serialization ---
    QUrl toUrl() const
    {
        QUrl url(QStringLiteral("thispcsearch:/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("search"), m_text);
        query.addQueryItem(QStringLiteral("scope"), QString::number(static_cast<int>(m_scope)));
        if (m_base.isValid()) query.addQueryItem(QStringLiteral("base"), m_base.toString());
        query.addQueryItem(QStringLiteral("type"), QString::number(m_legacyType));
        query.addQueryItem(QStringLiteral("date"), QString::number(m_legacyDate));
        query.addQueryItem(QStringLiteral("size"), QString::number(m_legacySize));

        // Only non-default Stage 2 values are written, so legacy queries stay legacy URLs.
        if (m_targetType != TargetType::All)
            query.addQueryItem(QStringLiteral("target"),
                m_targetType == TargetType::FilesOnly ? QStringLiteral("files") : QStringLiteral("folders"));
        if (m_nameMode == NameMode::Glob) query.addQueryItem(QStringLiteral("match"), QStringLiteral("glob"));
        else if (m_nameMode == NameMode::Auto) query.addQueryItem(QStringLiteral("match"), QStringLiteral("auto"));
        if (m_matchCase) query.addQueryItem(QStringLiteral("case"), QStringLiteral("1"));
        if (!m_recursive) query.addQueryItem(QStringLiteral("recursive"), QStringLiteral("0"));
        if (!m_extensions.isEmpty()) query.addQueryItem(QStringLiteral("ext"), m_extensions.join(QLatin1Char(',')));
        if (!m_mimeTypes.isEmpty()) query.addQueryItem(QStringLiteral("mimes"), m_mimeTypes.join(QLatin1Char(',')));
        if (!m_categories.isEmpty()) {
            QStringList names;
            for (Category c : m_categories) names << categoryName(c);
            query.addQueryItem(QStringLiteral("cat"), names.join(QLatin1Char(',')));
        }
        if (m_dateMin.has_value()) query.addQueryItem(QStringLiteral("datemin"), QString::number(*m_dateMin));
        if (m_dateMax.has_value()) query.addQueryItem(QStringLiteral("datemax"), QString::number(*m_dateMax));
        if (m_sizeMin.has_value()) query.addQueryItem(QStringLiteral("sizemin"), QString::number(*m_sizeMin));
        if (m_sizeMax.has_value()) query.addQueryItem(QStringLiteral("sizemax"), QString::number(*m_sizeMax));

        url.setQuery(query);
        return url;
    }

    // Legacy parameters keep their lenient 0.40.1 parsing (malformed -> default,
    // numbers clamped). Stage 2 parameters are strict: a malformed value is
    // recorded in errors() instead of being ignored.
    static SearchQuery fromUrl(const QUrl &url, const QTimeZone &zone = QTimeZone::systemTimeZone())
    {
        SearchQuery sq;
        if (url.scheme() != QLatin1String("thispcsearch") && url.scheme() != QLatin1String("filenamesearch"))
            return sq;
        const QUrlQuery query(url);
        sq.setText(query.queryItemValue(QStringLiteral("search"), QUrl::FullyDecoded));

        bool ok = false;
        // 0.40.1 parity (PaneSearchState::loadLocation): missing/unparsable scope
        // means This PC; numbers are clamped to 0..2. CustomFolder is not URL-reachable.
        const int scopeVal = query.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded).toInt(&ok);
        sq.setScope(static_cast<Scope>(ok ? std::clamp(scopeVal, 0, 2) : 2));

        const QString baseVal = query.queryItemValue(QStringLiteral("base"), QUrl::FullyDecoded);
        if (!baseVal.isEmpty()) sq.setBase(QUrl(baseVal));

        const int typeVal = query.queryItemValue(QStringLiteral("type"), QUrl::FullyDecoded).toInt(&ok);
        sq.setLegacyType(ok ? typeVal : 0);
        const int dateVal = query.queryItemValue(QStringLiteral("date"), QUrl::FullyDecoded).toInt(&ok);
        sq.setLegacyDate(ok ? dateVal : 0);
        const int sizeVal = query.queryItemValue(QStringLiteral("size"), QUrl::FullyDecoded).toInt(&ok);
        sq.setLegacySize(ok ? sizeVal : 0);

        const auto present = [&](const char *name) { return query.hasQueryItem(QLatin1String(name)); };
        const auto value = [&](const char *name) { return query.queryItemValue(QLatin1String(name), QUrl::FullyDecoded); };
        const auto fail = [&](const char *name, const QString &raw) {
            sq.m_errors << QStringLiteral("invalid %1 value '%2'").arg(QLatin1String(name), raw);
        };

        if (present("target")) {
            const QString v = value("target");
            if (v == QLatin1String("files")) sq.setTargetType(TargetType::FilesOnly);
            else if (v == QLatin1String("folders")) sq.setTargetType(TargetType::FoldersOnly);
            else fail("target", v);
        }
        if (present("match")) {
            const QString v = value("match");
            if (v == QLatin1String("glob")) sq.setNameMode(NameMode::Glob);
            else if (v == QLatin1String("auto")) sq.setNameMode(NameMode::Auto);
            else if (v == QLatin1String("substring")) sq.setNameMode(NameMode::Substring);
            else fail("match", v);
        } else {
            sq.setNameMode(NameMode::Substring);
        }
        const auto boolParam = [&](const char *name, bool &target) {
            if (!present(name)) return;
            const QString v = value(name);
            if (v == QLatin1String("1")) target = true;
            else if (v == QLatin1String("0")) target = false;
            else fail(name, v);
        };
        bool matchCase = false, recursive = true;
        boolParam("case", matchCase);
        boolParam("recursive", recursive);
        sq.setMatchCase(matchCase);
        sq.setRecursive(recursive);

        if (present("ext")) {
            sq.setExtensions(value("ext").split(QLatin1Char(','), Qt::SkipEmptyParts));
            if (sq.m_extensions.isEmpty()) fail("ext", value("ext"));
        }
        if (present("mimes")) {
            sq.setMimeTypes(value("mimes").split(QLatin1Char(','), Qt::SkipEmptyParts));
            if (sq.m_mimeTypes.isEmpty()) fail("mimes", value("mimes"));
            for (const QString &mime : std::as_const(sq.m_mimeTypes)) {
                static const QRegularExpression mimeRe(QStringLiteral("^[a-z0-9][a-z0-9!#$&^_.+-]*/(\\*|[a-z0-9][a-z0-9!#$&^_.+-]*)$"));
                if (!mimeRe.match(mime).hasMatch()) fail("mimes", mime);
            }
        }
        if (present("cat")) {
            QList<Category> categories;
            for (const QString &name : value("cat").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                if (const auto c = categoryFromName(name)) categories.append(*c);
                else fail("cat", name);
            }
            if (categories.isEmpty() && sq.m_errors.isEmpty()) fail("cat", value("cat"));
            sq.setCategories(categories);
        }

        std::optional<qint64> dmin, dmax, smin, smax;
        if (present("datemin")) {
            dmin = parseDateBoundary(value("datemin"), false, zone);
            if (!dmin) fail("datemin", value("datemin"));
        }
        if (present("datemax")) {
            dmax = parseDateBoundary(value("datemax"), true, zone);
            if (!dmax) fail("datemax", value("datemax"));
        }
        if (present("sizemin")) {
            smin = parseSizeBytes(value("sizemin"));
            if (!smin) fail("sizemin", value("sizemin"));
        }
        if (present("sizemax")) {
            smax = parseSizeBytes(value("sizemax"));
            if (!smax) fail("sizemax", value("sizemax"));
        }
        sq.m_dateMin = dmin;
        sq.m_dateMax = dmax;
        sq.m_sizeMin = smin;
        sq.m_sizeMax = smax;
        // Inverted ranges are reported by isValid()/firstProblem(), never swapped.
        return sq;
    }

    bool operator==(const SearchQuery &other) const
    {
        return m_text == other.m_text && m_nameMode == other.m_nameMode && m_matchCase == other.m_matchCase
            && m_scope == other.m_scope && sameLocation(m_base, other.m_base)
            && m_recursive == other.m_recursive && m_targetType == other.m_targetType
            && m_legacyType == other.m_legacyType && m_legacyDate == other.m_legacyDate
            && m_legacySize == other.m_legacySize && m_extensions == other.m_extensions
            && m_mimeTypes == other.m_mimeTypes && m_categories == other.m_categories
            && m_dateMin == other.m_dateMin && m_dateMax == other.m_dateMax
            && m_sizeMin == other.m_sizeMin && m_sizeMax == other.m_sizeMax;
    }
    bool operator!=(const SearchQuery &other) const { return !(*this == other); }

private:
    QString m_text;
    NameMode m_nameMode = NameMode::Substring;
    bool m_matchCase = false;
    Scope m_scope = Scope::ThisPc;
    QUrl m_base;
    bool m_recursive = true;
    TargetType m_targetType = TargetType::All;
    int m_legacyType = 0;
    int m_legacyDate = 0;
    int m_legacySize = 0;
    QStringList m_extensions;
    QStringList m_mimeTypes;
    QList<Category> m_categories;
    std::optional<qint64> m_dateMin;
    std::optional<qint64> m_dateMax;
    std::optional<qint64> m_sizeMin;
    std::optional<qint64> m_sizeMax;
    QStringList m_errors;
};
