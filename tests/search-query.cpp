/*
 * Tests for SearchQuery model and SearchMatcher predicate engine.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

static int searchQueryChecks = 0;
static void verifyQuery(bool value, const char *description)
{
    if (!value) qFatal("FAIL (search-query): %s", description);
    ++searchQueryChecks;
}

static void testSearchQueryModel()
{
    // 1. Normalization & trimming
    SearchQuery query;
    query.setText(QStringLiteral("  Raport \u0041\u030A  ")); // decomposed A with ring above -> Å
    verifyQuery(query.text() == QStringLiteral("Raport \u00c5"), "text normalized to NFC and trimmed");

    // 2. Default scope is ThisPc
    verifyQuery(query.scope() == SearchQuery::Scope::ThisPc, "default scope is ThisPc");

    // 3. Setting legacy types and targetType independent
    query.setLegacyType(1);
    verifyQuery(query.legacyType() == 1, "legacy type set to 1");
    query.setLegacyType(2);
    verifyQuery(query.legacyType() == 2, "legacy type set to 2");
    query.setLegacyType(0);
    verifyQuery(query.legacyType() == 0, "legacy type set to 0");

    // 4. Clamping of legacy filters
    query.setLegacyType(99);
    verifyQuery(query.legacyType() == 6, "legacy type clamped to max 6");
    query.setLegacyDate(-5);
    verifyQuery(query.legacyDate() == 0, "legacy date clamped to min 0");
    query.setLegacyDate(10);
    verifyQuery(query.legacyDate() == 4, "legacy date clamped to max 4");
    query.setLegacySize(-1);
    verifyQuery(query.legacySize() == 0, "legacy size clamped to min 0");
    query.setLegacySize(100);
    verifyQuery(query.legacySize() == 4, "legacy size clamped to max 4");

    // 5. Extensions normalization
    query.setExtensions({QStringLiteral(".tar.gz"), QStringLiteral("JPG"), QStringLiteral("..png"), QStringLiteral("jpg")});
    verifyQuery(query.extensions().size() == 3, "extensions deduplicated case-insensitively");
    verifyQuery(query.extensions().contains(QStringLiteral("tar.gz")), "multi-part extension preserved");
    verifyQuery(query.extensions().contains(QStringLiteral("jpg")), "extension lowercased without leading dot");
    verifyQuery(query.extensions().contains(QStringLiteral("png")), "multiple leading dots stripped");

    // 6. MIME types normalization
    query.setMimeTypes({QStringLiteral(" IMAGE/PNG "), QStringLiteral("image/png"), QStringLiteral("application/pdf")});
    verifyQuery(query.mimeTypes().size() == 2, "MIME types deduplicated and lowercased");

    // 7. Validation of ranges
    query.setDateRange(1000, 500); // inverted
    verifyQuery(!query.isValid(), "inverted date range is invalid");
    query.normalize();
    verifyQuery(query.isValid() && query.dateMin() == 500 && query.dateMax() == 1000,
                "normalize swaps inverted date range");

    query.setSizeRange(2000, 1000); // inverted
    verifyQuery(!query.isValid(), "inverted size range is invalid");
    query.normalize();
    verifyQuery(query.isValid() && query.sizeMin() == 1000 && query.sizeMax() == 2000,
                "normalize swaps inverted size range");

    // Negative size bounds are rejected
    query.setSizeRange(-50, 100);
    verifyQuery(!query.sizeMin().has_value() && query.sizeMax() == 100,
                "negative size min is rejected");
}

static void testSearchQueryUrlCompatibility()
{
    // Round-trip 0.40.1 compatible URL
    const QUrl legacyUrl = makeSearchLocation(QStringLiteral("test"), 1, QUrl::fromLocalFile(QStringLiteral("/home")), 3, 2, 1);
    const SearchQuery parsed = SearchQuery::fromUrl(legacyUrl);
    verifyQuery(parsed.text() == QStringLiteral("test"), "parsed legacy text");
    verifyQuery(parsed.scope() == SearchQuery::Scope::CurrentDrive, "parsed legacy scope");
    verifyQuery(sameLocation(parsed.base(), QUrl::fromLocalFile(QStringLiteral("/home"))), "parsed legacy base");
    verifyQuery(parsed.legacyType() == 3, "parsed legacy type");
    verifyQuery(parsed.legacyDate() == 2, "parsed legacy date");
    verifyQuery(parsed.legacySize() == 1, "parsed legacy size");

    // Serialization to URL preserves 0.40.1 parameters
    const QUrl generatedUrl = parsed.toUrl();
    verifyQuery(isSearchLocation(generatedUrl), "generated url is thispcsearch");
    verifyQuery(searchQueryFromUrl(generatedUrl) == QStringLiteral("test"), "URL search query matches");
    verifyQuery(searchIntParameter(generatedUrl, QStringLiteral("scope"), -1) == 1, "URL scope matches");
    verifyQuery(sameLocation(searchBaseFromUrl(generatedUrl), QUrl::fromLocalFile(QStringLiteral("/home"))), "URL base matches");
    verifyQuery(searchIntParameter(generatedUrl, QStringLiteral("type"), -1) == 3, "URL type matches");
    verifyQuery(searchIntParameter(generatedUrl, QStringLiteral("date"), -1) == 2, "URL date matches");
    verifyQuery(searchIntParameter(generatedUrl, QStringLiteral("size"), -1) == 1, "URL size matches");

    // Extended parameters serialization and deserialization
    SearchQuery extQuery;
    extQuery.setText(QStringLiteral("dok"));
    extQuery.setScope(SearchQuery::Scope::CurrentFolder);
    extQuery.setBase(QUrl::fromLocalFile(QStringLiteral("/tmp")));
    extQuery.setExtensions({QStringLiteral("tar.gz"), QStringLiteral("pdf")});
    extQuery.setMimeTypes({QStringLiteral("application/*")});
    extQuery.setDateRange(1700000000, 1700005000);
    extQuery.setSizeRange(1024, 20480);
    extQuery.setMatchCase(true);

    const QUrl extUrl = extQuery.toUrl();
    const SearchQuery parsedExt = SearchQuery::fromUrl(extUrl);
    verifyQuery(parsedExt == extQuery, "extended query round-trip preserves all fields");

    // URL percent-decoding tests: %20, %2B, %26, %23, %25, %3F and Unicode
    // %3F -> '?'
    const QUrl qQuestUrl = QUrl(QStringLiteral("thispcsearch:/?search=archive-%3F.txt&match=glob"));
    const SearchQuery parsedQuest = SearchQuery::fromUrl(qQuestUrl);
    verifyQuery(parsedQuest.text() == QStringLiteral("archive-?.txt"), "percent-encoded %3F decoded to '?'");
    verifyQuery(searchQueryFromUrl(qQuestUrl) == QStringLiteral("archive-?.txt"), "searchQueryFromUrl decodes %3F");

    // %20 (space), %2B (+), %26 (&), %23 (#), %25 (%)
    const QUrl qSpecialEncUrl = QUrl(QStringLiteral("thispcsearch:/?search=hello%20world%2Bplus%26and%23hash%25percent"));
    const SearchQuery parsedSpecial = SearchQuery::fromUrl(qSpecialEncUrl);
    verifyQuery(parsedSpecial.text() == QStringLiteral("hello world+plus&and#hash%percent"),
                "percent-encoded %20, %2B, %26, %23, %25 correctly decoded without double decoding");
    verifyQuery(searchQueryFromUrl(qSpecialEncUrl) == QStringLiteral("hello world+plus&and#hash%percent"),
                "searchQueryFromUrl decodes all percent-encoded special characters");

    // Re-serialization round-trip with literal special characters
    const QUrl reSerializedSpecial = parsedSpecial.toUrl();
    const SearchQuery reParsedSpecial = SearchQuery::fromUrl(reSerializedSpecial);
    verifyQuery(reParsedSpecial.text() == parsedSpecial.text(),
                "special character round-trip preserves exact decoded values");

    // Unicode in URL (both UTF-8 percent-encoded and direct Unicode)
    const QUrl qUnicodeDirect = QUrl(QString::fromUtf8("thispcsearch:/?search=zażółć gęślą jaźń"));
    const SearchQuery parsedUniDirect = SearchQuery::fromUrl(qUnicodeDirect);
    verifyQuery(parsedUniDirect.text() == QString::fromUtf8("zażółć gęślą jaźń"),
                "direct Unicode search phrase parsed and normalized");

    const QUrl qUnicodeEncoded = QUrl(QStringLiteral("thispcsearch:/?search=%C5%BC%C3%B3%C5%82%C4%87"));
    const SearchQuery parsedUniEnc = SearchQuery::fromUrl(qUnicodeEncoded);
    verifyQuery(parsedUniEnc.text() == QString::fromUtf8("żółć"),
                "UTF-8 percent-encoded search phrase correctly decoded to Unicode");

    // base parameter decoding
    const QUrl qBaseEnc = QUrl(QStringLiteral("thispcsearch:/?search=test&base=file%3A%2F%2F%2Fhome%2Fuser%2Fmy%2520folder"));
    const SearchQuery parsedBase = SearchQuery::fromUrl(qBaseEnc);
    verifyQuery(parsedBase.base().toLocalFile() == QStringLiteral("/home/user/my folder"),
                "percent-encoded base parameter correctly decoded to target local path");
    verifyQuery(searchBaseFromUrl(qBaseEnc).toLocalFile() == QStringLiteral("/home/user/my folder"),
                "searchBaseFromUrl decodes base parameter to local path");
}

static void testSearchScopeSafety()
{
    // Disallowed network schemes
    for (const QString &scheme : {QStringLiteral("smb"), QStringLiteral("sftp"), QStringLiteral("fish"),
                                  QStringLiteral("ftp"), QStringLiteral("nfs"), QStringLiteral("remote")}) {
        QUrl netUrl;
        netUrl.setScheme(scheme);
        netUrl.setHost(QStringLiteral("nas.local"));
        netUrl.setPath(QStringLiteral("/share"));
        verifyQuery(SearchQuery::validateRootScope(netUrl) == SearchQuery::ScopeViolation::DisallowedNetworkScheme,
                    "network scheme correctly flagged as disallowed");
    }

    // Unmounted volume detection
    DriveInfo unmounted;
    unmounted.name = QStringLiteral("Backup");
    unmounted.isMounted = false;
    unmounted.targetUrl = QUrl::fromLocalFile(QStringLiteral("/mnt/backup"));

    DriveInfo mounted;
    mounted.name = QStringLiteral("Data");
    mounted.isMounted = true;
    mounted.targetUrl = QUrl::fromLocalFile(QStringLiteral("/mnt/data"));

    const QList<DriveInfo> drives{unmounted, mounted};

    verifyQuery(SearchQuery::validateRootScope(QUrl::fromLocalFile(QStringLiteral("/mnt/backup")), drives)
                    == SearchQuery::ScopeViolation::UnmountedVolume,
                "unmounted volume flagged as scope violation");
    verifyQuery(SearchQuery::validateRootScope(QUrl::fromLocalFile(QStringLiteral("/mnt/data")), drives)
                    == SearchQuery::ScopeViolation::None,
                "mounted volume passes scope validation");

    // Invalid target URL
    verifyQuery(SearchQuery::validateRootScope(QUrl()) == SearchQuery::ScopeViolation::InvalidTargetUrl,
                "invalid URL flagged");
}

static void testSearchMatcherConjunctionsAndAlternatives()
{
    QMimeDatabase db;
    FileInfo file;
    file.name = QStringLiteral("archive-2026.tar.gz");
    file.mimeType = QStringLiteral("application/x-compressed-tar");
    file.isDir = false;
    file.size = 5000;
    file.modificationTime = 1700002000;

    // 1. Text match (case-sensitive vs insensitive)
    SearchQuery q1;
    q1.setText(QStringLiteral("ARCHIVE"));
    q1.setMatchCase(false);
    verifyQuery(SearchMatcher::matches(file, db, q1), "case-insensitive text match");
    q1.setMatchCase(true);
    verifyQuery(!SearchMatcher::matches(file, db, q1), "case-sensitive text mismatch");

    // 2. Unicode normalization matching
    FileInfo unicodeFile;
    unicodeFile.name = QStringLiteral("zdj\u0119cie-\u017c\u00f3\u0142w.jpg");
    unicodeFile.isDir = false;
    unicodeFile.size = 1000;
    unicodeFile.modificationTime = 1700000000;

    SearchQuery qUnicode;
    // Input with decomposed letters
    qUnicode.setText(QStringLiteral("\u017c\u00f3\u0142w"));
    verifyQuery(SearchMatcher::matches(unicodeFile, db, qUnicode), "Unicode NFC matching succeeds");

    // 3. Multi-part extension matching with OR alternatives
    SearchQuery qExt;
    qExt.setExtensions({QStringLiteral("zip"), QStringLiteral("tar.gz"), QStringLiteral("7z")});
    verifyQuery(SearchMatcher::matches(file, db, qExt), "tar.gz matches multi-part extension list");

    qExt.setExtensions({QStringLiteral("tar"), QStringLiteral("gz")});
    verifyQuery(SearchMatcher::matches(file, db, qExt), ".gz matches single part of multi-dot name");

    qExt.setExtensions({QStringLiteral("zip"), QStringLiteral("rar")});
    verifyQuery(!SearchMatcher::matches(file, db, qExt), "unmatched extension list fails");

    // Extensions must exclude directories even if folder name ends with .zip
    FileInfo dirWithExt;
    dirWithExt.name = QStringLiteral("folder.zip");
    dirWithExt.isDir = true;
    verifyQuery(!SearchMatcher::matches(dirWithExt, db, qExt), "directories excluded from extension filter");

    // 4. TargetType (FilesOnly vs FoldersOnly)
    SearchQuery qTarget;
    qTarget.setTargetType(SearchQuery::TargetType::FoldersOnly);
    verifyQuery(!SearchMatcher::matches(file, db, qTarget), "file excluded when FoldersOnly requested");
    verifyQuery(SearchMatcher::matches(dirWithExt, db, qTarget), "directory accepted when FoldersOnly requested");

    qTarget.setTargetType(SearchQuery::TargetType::FilesOnly);
    verifyQuery(SearchMatcher::matches(file, db, qTarget), "file accepted when FilesOnly requested");
    verifyQuery(!SearchMatcher::matches(dirWithExt, db, qTarget), "directory excluded when FilesOnly requested");

    // 5. Explicit Date Range with controlled reference time and boundaries
    SearchQuery qDate;
    qDate.setDateRange(1700001000, 1700003000);
    verifyQuery(SearchMatcher::matches(file, db, qDate), "file modification time inside [min, max]");

    file.modificationTime = 1700001000; // exact lower boundary
    verifyQuery(SearchMatcher::matches(file, db, qDate), "exact lower date boundary included");

    file.modificationTime = 1700003000; // exact upper boundary
    verifyQuery(SearchMatcher::matches(file, db, qDate), "exact upper date boundary included");

    file.modificationTime = 1700000999;
    verifyQuery(!SearchMatcher::matches(file, db, qDate), "file before date range excluded");

    file.modificationTime = 1700003001;
    verifyQuery(!SearchMatcher::matches(file, db, qDate), "file after date range excluded");

    file.modificationTime = 0; // unknown date
    verifyQuery(!SearchMatcher::matches(file, db, qDate), "unknown date (0) excluded from date filter");

    // 6. Explicit Size Range with boundaries
    file.modificationTime = 1700002000;
    SearchQuery qSize;
    qSize.setSizeRange(1000, 5000);
    file.size = 5000;
    verifyQuery(SearchMatcher::matches(file, db, qSize), "exact upper size boundary included");
    file.size = 1000;
    verifyQuery(SearchMatcher::matches(file, db, qSize), "exact lower size boundary included");
    file.size = 999;
    verifyQuery(!SearchMatcher::matches(file, db, qSize), "size below min excluded");
    file.size = 5001;
    verifyQuery(!SearchMatcher::matches(file, db, qSize), "size above max excluded");
    file.size = -1; // unknown size
    verifyQuery(!SearchMatcher::matches(file, db, qSize), "unknown size (-1) excluded from size filter");

    // 7. Full Conjunction (AND) across multiple active filters
    file.size = 3000;
    file.name = QStringLiteral("report-2026.pdf");
    file.mimeType = QStringLiteral("application/pdf");
    file.isDir = false;
    file.modificationTime = 1700002000;

    SearchQuery qConj;
    qConj.setText(QStringLiteral("report"));
    qConj.setTargetType(SearchQuery::TargetType::FilesOnly);
    qConj.setExtensions({QStringLiteral("pdf"), QStringLiteral("docx")});
    qConj.setMimeTypes({QStringLiteral("application/*")});
    qConj.setDateRange(1700001000, 1700003000);
    qConj.setSizeRange(1000, 5000);

    verifyQuery(SearchMatcher::matches(file, db, qConj), "all conjuncts satisfied (AND)");

    // Break one condition at a time to verify strict AND conjunction
    file.size = 6000;
    verifyQuery(!SearchMatcher::matches(file, db, qConj), "size mismatch breaks conjunction");
    file.size = 3000;

    file.name = QStringLiteral("summary-2026.pdf");
    verifyQuery(!SearchMatcher::matches(file, db, qConj), "text mismatch breaks conjunction");
    file.name = QStringLiteral("report-2026.pdf");

    file.modificationTime = 1600000000;
    verifyQuery(!SearchMatcher::matches(file, db, qConj), "date mismatch breaks conjunction");
    file.modificationTime = 1700002000;

    file.name = QStringLiteral("report-2026.txt");
    verifyQuery(!SearchMatcher::matches(file, db, qConj), "extension mismatch breaks conjunction");
}

static void testSearchMatcherDeterministicTime()
{
    QMimeDatabase db;
    FileInfo file;
    file.name = QStringLiteral("log.txt");
    file.isDir = false;
    file.size = 100;
    const qint64 refTime = 1700000000; // Fixed deterministic reference time

    // Past 1 day: cutoff is refTime - 86400
    file.modificationTime = refTime - 3600; // 1 hour ago relative to refTime
    verifyQuery(SearchMatcher::matchesLegacy(file, db, 0, 1, 0, refTime),
                "file modified 1 hour before refTime matches PastDay");

    file.modificationTime = refTime - 90000; // > 1 day ago relative to refTime
    verifyQuery(!SearchMatcher::matchesLegacy(file, db, 0, 1, 0, refTime),
                "file modified > 1 day before refTime fails PastDay");
    verifyQuery(SearchMatcher::matchesLegacy(file, db, 0, 2, 0, refTime),
                "file modified > 1 day before refTime matches PastWeek");
}

// Verbatim logic of SearchController::matchesFile as shipped in v0.40.1.
static bool oracle0401(const FileInfo &file, QMimeDatabase &db, int type, int date, int size)
{
    const QMimeType mime = resolvedMimeType(file, db);
    if (type != 0) {
        const QString m = mime.isValid() ? mime.name() : file.mimeType;
        bool ok = false;
        switch (type) {
        case 1: ok = file.isDir; break;
        case 2: ok = !file.isDir && m.startsWith(QStringLiteral("image/")); break;
        case 3: ok = !file.isDir && (m.startsWith(QStringLiteral("text/")) || m == QStringLiteral("application/pdf")
                    || m.contains(QStringLiteral("officedocument")) || m.contains(QStringLiteral("opendocument"))
                    || m.contains(QStringLiteral("msword")) || m.contains(QStringLiteral("rtf"))); break;
        case 4: ok = !file.isDir && m.startsWith(QStringLiteral("audio/")); break;
        case 5: ok = !file.isDir && m.startsWith(QStringLiteral("video/")); break;
        case 6: ok = !file.isDir && (m.contains(QStringLiteral("zip")) || m.contains(QStringLiteral("archive"))
                    || m.contains(QStringLiteral("compressed")) || m.contains(QStringLiteral("rar"))
                    || m.contains(QStringLiteral("7z")) || m.contains(QStringLiteral("tar"))); break;
        default: ok = true; break;
        }
        if (!ok) return false;
    }
    if (date != 0) {
        if (file.modificationTime <= 0) return false;
        const qint64 days = date == 1 ? 1 : date == 2 ? 7 : date == 3 ? 30 : date == 4 ? 365 : 0;
        if (file.modificationTime < QDateTime::currentSecsSinceEpoch() - days * 86400) return false;
    }
    if (size != 0) {
        if (file.isDir || file.size < 0) return false;
        const qint64 mib = 1024LL * 1024LL, gib = 1024LL * mib;
        bool ok = false;
        switch (size) {
        case 1: ok = file.size < mib; break;
        case 2: ok = file.size >= mib && file.size < 100LL * mib; break;
        case 3: ok = file.size >= 100LL * mib && file.size < gib; break;
        case 4: ok = file.size >= gib; break;
        default: ok = true; break;
        }
        if (!ok) return false;
    }
    return true;
}

static void testProductionCompatibility()
{
    QMimeDatabase db;
    // 1. Differential matcher test: production SearchController::matchesFile,
    // SearchMatcher::matchesLegacy and SearchMatcher::matches(SearchQuery) must
    // all equal the 0.40.1 oracle over a grid of files x filters. Dates sit hours
    // away from preset boundaries so the wall-clock based oracle is not flaky.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 mib = 1024LL * 1024LL, gib = 1024 * mib;
    const QList<qint64> times{0, -5, now - 3600, now - 86400 - 7200, now - 7 * 86400 - 7200,
                              now - 30 * 86400 - 7200, now - 365 * 86400 - 7200, now - 3 * 365 * 86400};
    const QList<qint64> sizes{-1, 0, 1, mib - 1, mib, 100 * mib - 1, 100 * mib, gib - 1, gib, 5 * gib};
    const QList<QPair<QString, QString>> names{
        {"a.png", "image/png"}, {"a.txt", "text/plain"}, {"a.pdf", "application/pdf"},
        {"a.mp3", "audio/mpeg"}, {"a.mp4", "video/mp4"}, {"a.zip", "application/zip"},
        {"a.tar.gz", "application/x-compressed-tar"}, {"a.docx", ""}, {"noext", ""}, {"a.bin", "application/octet-stream"}};
    int compared = 0;
    for (bool isDir : {false, true})
    for (const auto &[name, mime] : names)
    for (qint64 t : times)
    for (qint64 s : sizes) {
        FileInfo f;
        f.name = name; f.mimeType = mime; f.isDir = isDir; f.size = s; f.modificationTime = t;
        f.url = QUrl::fromLocalFile(QStringLiteral("/nonexistent/") + name);
        for (int type = 0; type <= 6; ++type)
        for (int date = 0; date <= 4; ++date)
        for (int sz = 0; sz <= 4; ++sz) {
            const bool expected = oracle0401(f, db, type, date, sz);
            SearchQuery q;
            q.setLegacyType(type); q.setLegacyDate(date); q.setLegacySize(sz);
            const bool a = SearchController::matchesFile(f, db, {type, date, sz});
            const bool b = SearchMatcher::matches(f, db, q);
            ++compared;
            if (a != expected || b != expected) {
                qFatal("FAIL (search-query): legacy divergence name=%s dir=%d t=%lld s=%lld type=%d date=%d size=%d oracle=%d controller=%d query=%d",
                       qPrintable(name), isDir, t, s, type, date, sz, expected, a, b);
            }
        }
    }
    verifyQuery(compared > 100000, "differential grid executed");
    qInfo("legacy differential comparisons: %d", compared);

    // 2. URL generation parity with production makeSearchLocation.
    const QUrl base = QUrl::fromLocalFile(QStringLiteral("/home/user"));
    int urlCases = 0;
    for (const QString &text : {QStringLiteral("needle"), QStringLiteral("żółć ą"), QStringLiteral("a&b=c d")})
    for (int scope = 0; scope <= 2; ++scope)
    for (const QUrl &b : {QUrl(), base})
    for (int type = 0; type <= 6; ++type)
    for (int date = 0; date <= 4; ++date)
    for (int size = 0; size <= 4; ++size) {
        SearchQuery q;
        q.setText(text); q.setScope(static_cast<SearchQuery::Scope>(scope)); q.setBase(b);
        q.setLegacyType(type); q.setLegacyDate(date); q.setLegacySize(size);
        verifyQuery(q.toUrl() == makeSearchLocation(text, scope, b, type, date, size),
                    "SearchQuery::toUrl byte-identical to makeSearchLocation for legacy queries");
        ++urlCases;
    }
    qInfo("legacy URL generation comparisons: %d", urlCases);

    // 3. URL parsing parity with production PaneSearchState::loadLocation,
    // including malformed and out-of-range parameters.
    const QStringList odd{QStringLiteral(""), QStringLiteral("abc"), QStringLiteral("-1"), QStringLiteral("0"),
                          QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("7"), QStringLiteral("99"),
                          QStringLiteral("9999999999999"), QStringLiteral("1.5")};
    int parseCases = 0;
    for (const QString &sc : odd) for (const QString &ty : odd) for (const QString &da : odd) for (const QString &sz : odd) {
        QUrl url(QStringLiteral("thispcsearch:/"));
        QUrlQuery uq;
        uq.addQueryItem(QStringLiteral("search"), QStringLiteral("x"));
        if (!sc.isEmpty()) uq.addQueryItem(QStringLiteral("scope"), sc);
        if (!ty.isEmpty()) uq.addQueryItem(QStringLiteral("type"), ty);
        if (!da.isEmpty()) uq.addQueryItem(QStringLiteral("date"), da);
        if (!sz.isEmpty()) uq.addQueryItem(QStringLiteral("size"), sz);
        url.setQuery(uq);
        PaneSearchState state;
        state.loadLocation(url);
        const SearchQuery q = SearchQuery::fromUrl(url);
        if (static_cast<int>(q.scope()) != state.scope || q.legacyType() != state.type
            || q.legacyDate() != state.date || q.legacySize() != state.size) {
            qFatal("FAIL (search-query): parse divergence scope=%s type=%s date=%s size=%s",
                   qPrintable(sc), qPrintable(ty), qPrintable(da), qPrintable(sz));
        }
        ++parseCases;
    }
    verifyQuery(parseCases == 10000, "malformed-parameter parse grid executed");
    qInfo("legacy URL parse comparisons: %d", parseCases);

    // 4. Old URLs never gain new parameters when no new feature is used.
    const QUrl plain = SearchQuery::fromUrl(makeSearchLocation(QStringLiteral("x"), 0, base, 2, 1, 3)).toUrl();
    verifyQuery(!QUrlQuery(plain).hasQueryItem(QStringLiteral("target"))
                    && !QUrlQuery(plain).hasQueryItem(QStringLiteral("ext"))
                    && !QUrlQuery(plain).hasQueryItem(QStringLiteral("case")),
                "legacy URL round-trip adds no extended parameters");
    verifyQuery(plain == makeSearchLocation(QStringLiteral("x"), 0, base, 2, 1, 3),
                "legacy URL round-trip is byte-identical");
}

static void testStage2AdvancedQueryAndExecution()
{
    QMimeDatabase db;
    // 1. Strict Size parser with IEC units
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("512")) == 512, "parseSizeBytes bytes");
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("10KiB")) == 10 * 1024, "parseSizeBytes KiB");
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("1.5MiB")) == static_cast<qint64>(1.5 * 1024 * 1024), "parseSizeBytes MiB fractional");
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("2GiB")) == 2LL * 1024 * 1024 * 1024, "parseSizeBytes GiB");
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("0B")) == 0, "parseSizeBytes 0B");
    verifyQuery(!SearchQuery::parseSizeBytes(QStringLiteral("-10KiB")).has_value(), "parseSizeBytes negative rejected");
    verifyQuery(!SearchQuery::parseSizeBytes(QStringLiteral("10KB")).has_value(), "parseSizeBytes non-IEC rejected");
    verifyQuery(!SearchQuery::parseSizeBytes(QStringLiteral("invalid")).has_value(), "parseSizeBytes invalid string rejected");

    // 2. Date boundary parser with ISO 8601 local time
    const auto tsMin = SearchQuery::parseDateBoundary(QStringLiteral("2026-05-10"), false);
    const auto tsMax = SearchQuery::parseDateBoundary(QStringLiteral("2026-05-10"), true);
    verifyQuery(tsMin.has_value() && tsMax.has_value(), "parseDateBoundary min/max parsed");
    verifyQuery(*tsMin <= *tsMax, "start of day precedes end of day");
    const QDateTime dtMin = QDateTime::fromSecsSinceEpoch(*tsMin, QTimeZone::LocalTime);
    const QDateTime dtMax = QDateTime::fromSecsSinceEpoch(*tsMax, QTimeZone::LocalTime);
    verifyQuery(dtMin.time() == QTime(0, 0, 0), "min boundary is 00:00:00 local time");
    verifyQuery(dtMax.time() == QTime(23, 59, 59), "max boundary is 23:59:59 local time");

    // Epoch seconds string
    const auto epochTs = SearchQuery::parseDateBoundary(QStringLiteral("1700000000"), false);
    verifyQuery(epochTs.has_value() && *epochTs == 1700000000, "epoch seconds boundary parsed");

    // 3. Glob matching (SearchQuery::NameMode::Glob)
    FileInfo fLog; fLog.name = QStringLiteral("access-2026-04.log"); fLog.isDir = false; fLog.size = 100;
    FileInfo fTxt; fTxt.name = QStringLiteral("access-test.txt"); fTxt.isDir = false; fTxt.size = 100;

    SearchQuery qGlob;
    qGlob.setNameMode(SearchQuery::NameMode::Glob);
    qGlob.setText(QStringLiteral("access-*.log"));
    verifyQuery(SearchMatcher::matches(fLog, db, qGlob), "glob access-*.log matches access-2026-04.log");
    verifyQuery(!SearchMatcher::matches(fTxt, db, qGlob), "glob access-*.log rejects access-test.txt");

    // Substring default
    SearchQuery qSub;
    qSub.setText(QStringLiteral("access"));
    verifyQuery(SearchMatcher::matches(fLog, db, qSub) && SearchMatcher::matches(fTxt, db, qSub),
                "substring matches both");

    // 4. MIME categories & wildcard patterns union
    FileInfo fJpg; fJpg.name = QStringLiteral("photo.jpg"); fJpg.mimeType = QStringLiteral("image/jpeg"); fJpg.isDir = false;
    FileInfo fPdf; fPdf.name = QStringLiteral("doc.pdf"); fPdf.mimeType = QStringLiteral("application/pdf"); fPdf.isDir = false;
    FileInfo fMp3; fMp3.name = QStringLiteral("song.mp3"); fMp3.mimeType = QStringLiteral("audio/mpeg"); fMp3.isDir = false;

    SearchQuery qMime;
    qMime.setMimeTypes({QStringLiteral("image/*"), QStringLiteral("application/pdf")});
    verifyQuery(SearchMatcher::matches(fJpg, db, qMime), "image/* pattern matches image/jpeg");
    verifyQuery(SearchMatcher::matches(fPdf, db, qMime), "application/pdf matches exact mime");
    verifyQuery(!SearchMatcher::matches(fMp3, db, qMime), "audio/mpeg rejected");

    SearchQuery qCat;
    qCat.setCategories({SearchQuery::Category::Audio, SearchQuery::Category::Documents});
    verifyQuery(SearchMatcher::matches(fMp3, db, qCat), "audio category matches mp3");
    verifyQuery(SearchMatcher::matches(fPdf, db, qCat), "documents category matches pdf");
    verifyQuery(!SearchMatcher::matches(fJpg, db, qCat), "image rejected by audio+docs category");

    // 4b. Strict AND Conjunction between categories and mimeTypes (Manual Test 3B.2.7 regression)
    // Scenario:
    // ext=png,pdf,zip
    // cat=images,archives (Images, Archives)
    // mimes=image/png,application/pdf (PNG, PDF)
    //
    // Files:
    // - foto-a.png:      ext=png, cat=Images,   mime=image/png        -> MATCH (in cat AND in mimes)
    // - dokument.pdf:    ext=pdf, cat=Documents, mime=application/pdf -> NO MATCH (in mimes, but cat is Documents, not Images/Archives)
    // - archiwum.zip:    ext=zip, cat=Archives,  mime=application/zip -> NO MATCH (in cat, but mime is zip, not image/png/application/pdf)
    // - foto-b.jpg:      ext=jpg, cat=Images,   mime=image/jpeg       -> NO MATCH (ext is not png,pdf,zip; mime not matched)
    SearchQuery q3b2;
    q3b2.setNameMode(SearchQuery::NameMode::Glob);
    q3b2.setText(QStringLiteral("*"));
    q3b2.setExtensions({QStringLiteral("png"), QStringLiteral("pdf"), QStringLiteral("zip")});
    q3b2.setTargetType(SearchQuery::TargetType::FilesOnly);
    q3b2.setCategories({SearchQuery::Category::Images, SearchQuery::Category::Archives});
    q3b2.setMimeTypes({QStringLiteral("image/png"), QStringLiteral("application/pdf")});

    // Verify activeFilterCount = 4 (targetType, ext, cat, mimes)
    verifyQuery(q3b2.activeFilterCount() == 4, "q3b2 activeFilterCount is exactly 4 (targetType + ext + cat + mimes)");

    FileInfo fFotoA; fFotoA.name = QStringLiteral("foto-a.png"); fFotoA.mimeType = QStringLiteral("image/png"); fFotoA.isDir = false;
    FileInfo fDokument; fDokument.name = QStringLiteral("dokument.pdf"); fDokument.mimeType = QStringLiteral("application/pdf"); fDokument.isDir = false;
    FileInfo fArchiwum; fArchiwum.name = QStringLiteral("archiwum.zip"); fArchiwum.mimeType = QStringLiteral("application/zip"); fArchiwum.isDir = false;
    FileInfo fFotoC; fFotoC.name = QStringLiteral("foto-c.png"); fFotoC.mimeType = QStringLiteral("image/png"); fFotoC.isDir = false;

    verifyQuery(SearchMatcher::matches(fFotoA, db, q3b2), "foto-a.png matches cat(images) AND mime(image/png) AND ext(png)");
    verifyQuery(SearchMatcher::matches(fFotoC, db, q3b2), "foto-c.png matches cat(images) AND mime(image/png) AND ext(png)");
    verifyQuery(!SearchMatcher::matches(fDokument, db, q3b2), "dokument.pdf rejected because cat(documents) not in {images, archives}");
    verifyQuery(!SearchMatcher::matches(fArchiwum, db, q3b2), "archiwum.zip rejected because mime(application/zip) not in {image/png, application/pdf}");

    // 5. Non-recursive vs Recursive flag and SearchPlan
    SearchQuery qNonRec;
    qNonRec.setText(QStringLiteral("needle"));
    qNonRec.setRecursive(false);
    verifyQuery(!qNonRec.recursive(), "recursive flag is false");

    const QUrl nonRecUrl = qNonRec.toUrl();
    const SearchQuery parsedNonRec = SearchQuery::fromUrl(nonRecUrl);
    verifyQuery(!parsedNonRec.recursive(), "recursive=0 deserialized correctly from URL");

    const SearchPlan planNonRec = SearchPlanner::plan(parsedNonRec, {QUrl::fromLocalFile(QStringLiteral("/home"))});
    verifyQuery(!planNonRec.recursive, "SearchPlan recursive matches query");
    verifyQuery(planNonRec.ingestNameFilter != nullptr,
                "non-recursive substring search configures ingestNameFilter for fast KIO filtering");

    // 6. SearchPlanner::automaticRoots safety (rejects network and unmounted)
    DriveInfo dLocal; dLocal.isMounted = true; dLocal.targetUrl = QUrl::fromLocalFile(QStringLiteral("/home"));
    DriveInfo dUnmounted; dUnmounted.isMounted = false; dUnmounted.targetUrl = QUrl::fromLocalFile(QStringLiteral("/media/usb"));
    const QList<QUrl> safeRoots = SearchPlanner::automaticRoots({dLocal, dUnmounted});
    verifyQuery(safeRoots.size() == 1 && sameLocation(safeRoots.first(), dLocal.targetUrl),
                "automaticRoots includes only mounted local drives");

    // 7. SearchRequest backward compatibility check
    const QUrl legacyUrl = makeSearchLocation(QStringLiteral("needle"), 0, QUrl::fromLocalFile(QStringLiteral("/home")), 2, 1, 1);
    const SearchRequest reqLegacy = SearchRequest::fromLocation(legacyUrl);
    verifyQuery(!reqLegacy.isAdvanced(), "0.40.1 URL classified as not advanced");

    const QUrl advUrl = QUrl(QStringLiteral("thispcsearch:/?search=test&ext=png,jpg&sizemin=10KiB"));
    const SearchRequest reqAdv = SearchRequest::fromLocation(advUrl);
    verifyQuery(reqAdv.isAdvanced(), "URL with ext and sizemin classified as advanced");
    verifyQuery(reqAdv.query().extensions().size() == 2, "advanced extensions extracted");
    verifyQuery(reqAdv.query().sizeMin() == 10 * 1024, "advanced sizemin extracted");

    // 8. Strict validation and error handling on invalid URL parameters
    const QUrl invalidUrl = QUrl(QStringLiteral("thispcsearch:/?search=test&sizemin=invalid_size"));
    const SearchRequest reqInvalid = SearchRequest::fromLocation(invalidUrl);
    verifyQuery(!reqInvalid.query().isValid(), "query with invalid sizemin marked invalid");
    verifyQuery(!reqInvalid.query().errors().isEmpty(), "validation errors recorded");

    // =========================================================================
    // Stage 3B.3 Advanced Filters Model: Date & Size Ranges, ActiveFilterCount
    // =========================================================================
    SearchQuery q3b3;
    verifyQuery(q3b3.activeFilterCount() == 0, "initial activeFilterCount is 0");

    // Setting custom dateMin counts as 1 filter
    q3b3.setDateRange(1700000000, std::nullopt);
    verifyQuery(q3b3.activeFilterCount() == 1, "dateMin counts as 1 filter");

    // Setting both dateMin and dateMax still counts as 1 filter
    q3b3.setDateRange(1700000000, 1700010000);
    verifyQuery(q3b3.activeFilterCount() == 1, "dateMin and dateMax together count as 1 filter");

    // Adding custom size counts as +1
    q3b3.setSizeRange(1024, 2048);
    verifyQuery(q3b3.activeFilterCount() == 2, "custom date + custom size counts as 2 filters");

    // If legacy preset is also set, it should not double count
    q3b3.setLegacyDate(1);
    q3b3.setLegacySize(2);
    verifyQuery(q3b3.activeFilterCount() == 2, "custom date/size does not double count with legacy presets");

    // Test IEC binary size units in parseSizeBytes
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("100 B")) == 100LL, "parseSizeBytes 100 B");
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("2 KiB")) == 2LL * 1024LL, "parseSizeBytes 2 KiB");
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("5.5 MiB")) == static_cast<qint64>(5.5 * 1024.0 * 1024.0), "parseSizeBytes 5.5 MiB");
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("1 GiB")) == 1LL * 1024LL * 1024LL * 1024LL, "parseSizeBytes 1 GiB");
    verifyQuery(SearchQuery::parseSizeBytes(QStringLiteral("1.5 TiB")) == static_cast<qint64>(1.5 * 1024.0 * 1024.0 * 1024.0 * 1024.0), "parseSizeBytes 1.5 TiB");

    // Date boundaries: start of day vs end of day
    const QTimeZone sysZone = QTimeZone::systemTimeZone();
    const auto dtMinParsed = SearchQuery::parseDateBoundary(QStringLiteral("2026-10-13"), false, sysZone);
    const auto dtMaxParsed = SearchQuery::parseDateBoundary(QStringLiteral("2026-10-13"), true, sysZone);
    verifyQuery(dtMinParsed.has_value() && dtMaxParsed.has_value(), "parseDateBoundary parses YYYY-MM-DD");
    verifyQuery(*dtMaxParsed > *dtMinParsed, "upper bound is greater than lower bound for same date");
    verifyQuery(*dtMaxParsed - *dtMinParsed == 86399LL, "difference between start of day and end of day is 86399 seconds (23h 59m 59s)");

    // =========================================================================
    // Stage 3B.4 Comprehensive Integration & Boundary Matrix
    // =========================================================================
    // 1. All search criteria combined simultaneously (AND across groups, OR within groups)
    SearchQuery qAllCriteria;
    qAllCriteria.setText(QStringLiteral("report*"));
    qAllCriteria.setNameMode(SearchQuery::NameMode::Glob);
    qAllCriteria.setMatchCase(true);
    qAllCriteria.setTargetType(SearchQuery::TargetType::FilesOnly);
    qAllCriteria.setExtensions({QStringLiteral("pdf"), QStringLiteral("docx")});
    qAllCriteria.setCategories({SearchQuery::Category::Documents});
    qAllCriteria.setMimeTypes({QStringLiteral("application/pdf"), QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document")});
    qAllCriteria.setDateRange(1700000000, 1700050000);
    qAllCriteria.setSizeRange(1024, 10485760); // 1 KiB to 10 MiB
    qAllCriteria.setRecursive(true);

    verifyQuery(qAllCriteria.isValid(), "qAllCriteria is valid");
    verifyQuery(qAllCriteria.usesAdvancedFeatures(), "qAllCriteria uses advanced features");
    // activeFilterCount: targetType(1) + extensions(1) + categories(1) + mimeTypes(1) + dateRange(1) + sizeRange(1) = 6
    verifyQuery(qAllCriteria.activeFilterCount() == 6, "qAllCriteria activeFilterCount is 6");

    // Serialization and deserialization round-trip
    const QUrl urlAll = qAllCriteria.toUrl();
    const SearchQuery parsedAll = SearchQuery::fromUrl(urlAll);
    verifyQuery(parsedAll == qAllCriteria, "qAllCriteria URL round-trip retains exact query equality");
    verifyQuery(parsedAll.activeFilterCount() == 6, "parsedAll has activeFilterCount 6");

    // Matcher execution against files:
    QMimeDatabase mimeDb;
    SearchMatcher::Prepared matcherAll(qAllCriteria);

    FileInfo passFile;
    passFile.name = QStringLiteral("report_2026.pdf");
    passFile.isDir = false;
    passFile.size = 5000; // in [1024, 10485760]
    passFile.modificationTime = 1700020000; // in [1700000000, 1700050000]
    passFile.mimeType = QStringLiteral("application/pdf");
    verifyQuery(matcherAll.matches(passFile, mimeDb), "passFile satisfies all criteria simultaneously");

    // Test each failure dimension (AND between groups):
    // A. Case sensitivity failure ("Report_2026.pdf" vs case-sensitive "report*")
    FileInfo failCase = passFile; failCase.name = QStringLiteral("Report_2026.pdf");
    verifyQuery(!matcherAll.matches(failCase, mimeDb), "case mismatch fails match");

    // B. Target type failure (isDir = true)
    FileInfo failDir = passFile; failDir.isDir = true;
    verifyQuery(!matcherAll.matches(failDir, mimeDb), "isDir fails files-only target");

    // C. Extension failure ("report_2026.txt")
    FileInfo failExt = passFile; failExt.name = QStringLiteral("report_2026.txt");
    verifyQuery(!matcherAll.matches(failExt, mimeDb), "extension not in list fails match");

    // D. Category / MIME failure (wrong mime)
    FileInfo failMime = passFile; failMime.mimeType = QStringLiteral("image/png");
    verifyQuery(!matcherAll.matches(failMime, mimeDb), "mime not in list fails match");

    // E. Date range boundary: exactly at min passes, min-1 fails, max passes, max+1 fails
    FileInfo fDateMin = passFile; fDateMin.modificationTime = 1700000000;
    FileInfo fDateBelow = passFile; fDateBelow.modificationTime = 1699999999;
    FileInfo fDateMax = passFile; fDateMax.modificationTime = 1700050000;
    FileInfo fDateAbove = passFile; fDateAbove.modificationTime = 1700050001;
    verifyQuery(matcherAll.matches(fDateMin, mimeDb), "date exactly at dateMin passes");
    verifyQuery(!matcherAll.matches(fDateBelow, mimeDb), "date below dateMin fails");
    verifyQuery(matcherAll.matches(fDateMax, mimeDb), "date exactly at dateMax passes");
    verifyQuery(!matcherAll.matches(fDateAbove, mimeDb), "date above dateMax fails");

    // F. Size range boundary: exactly at min passes, min-1 fails, max passes, max+1 fails
    FileInfo fSizeMin = passFile; fSizeMin.size = 1024;
    FileInfo fSizeBelow = passFile; fSizeBelow.size = 1023;
    FileInfo fSizeMax = passFile; fSizeMax.size = 10485760;
    FileInfo fSizeAbove = passFile; fSizeAbove.size = 10485761;
    verifyQuery(matcherAll.matches(fSizeMin, mimeDb), "size exactly at sizeMin passes");
    verifyQuery(!matcherAll.matches(fSizeBelow, mimeDb), "size below sizeMin fails");
    verifyQuery(matcherAll.matches(fSizeMax, mimeDb), "size exactly at sizeMax passes");
    verifyQuery(!matcherAll.matches(fSizeAbove, mimeDb), "size above sizeMax fails");

    // G. Zero byte file test with custom size min = 0
    SearchQuery qZeroSize;
    qZeroSize.setSizeRange(0, 100);
    SearchMatcher::Prepared matcherZero(qZeroSize);
    FileInfo fZero; fZero.name = QStringLiteral("empty.txt"); fZero.size = 0;
    verifyQuery(matcherZero.matches(fZero, mimeDb), "0 byte file matches size range starting at 0");

    // 2. Inverted ranges validity
    SearchQuery qInvertedDate;
    qInvertedDate.setDateRange(2000, 1000);
    verifyQuery(!qInvertedDate.isValid(), "inverted date range is invalid");
    verifyQuery(!qInvertedDate.firstProblem().isEmpty(), "firstProblem returns error for inverted date");

    SearchQuery qInvertedSize;
    qInvertedSize.setSizeRange(5000, 1000);
    verifyQuery(!qInvertedSize.isValid(), "inverted size range is invalid");
    verifyQuery(!qInvertedSize.firstProblem().isEmpty(), "firstProblem returns error for inverted size");
}

static void testAutoTextGlobMatchingModes()
{
    // 1. Auto + plain text -> Substring mode
    SearchQuery q1;
    q1.setNameMode(SearchQuery::NameMode::Auto);
    q1.setText(QStringLiteral("report"));
    verifyQuery(q1.effectiveNameMode() == SearchQuery::NameMode::Substring, "Auto + plain text resolves to Substring");
    SearchMatcher::Prepared matcher1(q1);
    FileInfo f1_pass; f1_pass.name = QStringLiteral("annual_report_2026.pdf");
    FileInfo f1_fail; f1_fail.name = QStringLiteral("repor.txt");
    verifyQuery(matcher1.matchesName(f1_pass), "Auto + text matches substring in filename");
    verifyQuery(!matcher1.matchesName(f1_fail), "Auto + text does not match non-matching filename");

    // 2. Auto + * -> Glob mode (matches all files)
    SearchQuery q2;
    q2.setNameMode(SearchQuery::NameMode::Auto);
    q2.setText(QStringLiteral("*"));
    verifyQuery(q2.effectiveNameMode() == SearchQuery::NameMode::Glob, "Auto + * resolves to Glob");
    const QStringList p2 = SearchPlanner::workerPatterns(q2);
    verifyQuery(!p2.isEmpty(), "Worker patterns for Auto + * are non-empty");
    SearchMatcher::Prepared matcher2(q2);
    FileInfo f2_1; f2_1.name = QStringLiteral("anything.txt");
    FileInfo f2_2; f2_2.name = QStringLiteral("test_dir"); f2_2.isDir = true;
    verifyQuery(matcher2.matchesName(f2_1), "Auto + * matches anything.txt");
    verifyQuery(matcher2.matchesName(f2_2), "Auto + * matches directory");

    // 3. Auto + *.png -> Glob mode
    SearchQuery q3;
    q3.setNameMode(SearchQuery::NameMode::Auto);
    q3.setText(QStringLiteral("*.png"));
    verifyQuery(q3.effectiveNameMode() == SearchQuery::NameMode::Glob, "Auto + *.png resolves to Glob");
    SearchMatcher::Prepared matcher3(q3);
    FileInfo f3_pass; f3_pass.name = QStringLiteral("photo.png");
    FileInfo f3_fail; f3_fail.name = QStringLiteral("photo.jpg");
    verifyQuery(matcher3.matchesName(f3_pass), "Auto + *.png matches photo.png");
    verifyQuery(!matcher3.matchesName(f3_fail), "Auto + *.png rejects photo.jpg");

    // 4. Auto + foto* -> Glob mode
    SearchQuery q4;
    q4.setNameMode(SearchQuery::NameMode::Auto);
    q4.setText(QStringLiteral("foto*"));
    verifyQuery(q4.effectiveNameMode() == SearchQuery::NameMode::Glob, "Auto + foto* resolves to Glob");
    SearchMatcher::Prepared matcher4(q4);
    FileInfo f4_pass; f4_pass.name = QStringLiteral("foto_holiday.jpg");
    FileInfo f4_fail; f4_fail.name = QStringLiteral("my_foto.jpg");
    verifyQuery(matcher4.matchesName(f4_pass), "Auto + foto* matches foto_holiday.jpg");
    verifyQuery(!matcher4.matchesName(f4_fail), "Auto + foto* rejects my_foto.jpg (prefix glob)");

    // 5. Auto + *backup* -> Glob mode
    SearchQuery q5;
    q5.setNameMode(SearchQuery::NameMode::Auto);
    q5.setText(QStringLiteral("*backup*"));
    verifyQuery(q5.effectiveNameMode() == SearchQuery::NameMode::Glob, "Auto + *backup* resolves to Glob");
    SearchMatcher::Prepared matcher5(q5);
    FileInfo f5_pass; f5_pass.name = QStringLiteral("db_backup_2026.sql.gz");
    FileInfo f5_fail; f5_fail.name = QStringLiteral("restore.sql");
    verifyQuery(matcher5.matchesName(f5_pass), "Auto + *backup* matches db_backup_2026.sql.gz");
    verifyQuery(!matcher5.matchesName(f5_fail), "Auto + *backup* rejects restore.sql");

    // 6. Auto + plik?.txt -> Glob mode
    SearchQuery q6;
    q6.setNameMode(SearchQuery::NameMode::Auto);
    q6.setText(QStringLiteral("plik?.txt"));
    verifyQuery(q6.effectiveNameMode() == SearchQuery::NameMode::Glob, "Auto + plik?.txt resolves to Glob");
    SearchMatcher::Prepared matcher6(q6);
    FileInfo f6_pass1; f6_pass1.name = QStringLiteral("plik1.txt");
    FileInfo f6_pass2; f6_pass2.name = QStringLiteral("plika.txt");
    FileInfo f6_fail; f6_fail.name = QStringLiteral("plik12.txt");
    verifyQuery(matcher6.matchesName(f6_pass1), "Auto + plik?.txt matches plik1.txt");
    verifyQuery(matcher6.matchesName(f6_pass2), "Auto + plik?.txt matches plika.txt");
    verifyQuery(!matcher6.matchesName(f6_fail), "Auto + plik?.txt rejects plik12.txt");

    // 7. Tekst + * -> Substring mode (literal *)
    SearchQuery q7;
    q7.setNameMode(SearchQuery::NameMode::Substring);
    q7.setText(QStringLiteral("*"));
    verifyQuery(q7.effectiveNameMode() == SearchQuery::NameMode::Substring, "Explicit Substring preserves Substring with *");
    SearchMatcher::Prepared matcher7(q7);
    FileInfo f7_pass; f7_pass.name = QStringLiteral("file*name.txt");
    FileInfo f7_fail; f7_fail.name = QStringLiteral("regular_file.txt");
    verifyQuery(matcher7.matchesName(f7_pass), "Literal text * matches file containing *");
    verifyQuery(!matcher7.matchesName(f7_fail), "Literal text * does not match regular file without *");

    // 8. Tekst + ? -> Substring mode (literal ?)
    SearchQuery q8;
    q8.setNameMode(SearchQuery::NameMode::Substring);
    q8.setText(QStringLiteral("?"));
    verifyQuery(q8.effectiveNameMode() == SearchQuery::NameMode::Substring, "Explicit Substring preserves Substring with ?");
    SearchMatcher::Prepared matcher8(q8);
    FileInfo f8_pass; f8_pass.name = QStringLiteral("what?.txt");
    FileInfo f8_fail; f8_fail.name = QStringLiteral("what.txt");
    verifyQuery(matcher8.matchesName(f8_pass), "Literal text ? matches file containing ?");
    verifyQuery(!matcher8.matchesName(f8_fail), "Literal text ? does not match file without ?");

    // 9. Wymuszony Glob + zapytanie bez gwiazdek -> whole-name glob
    SearchQuery q9;
    q9.setNameMode(SearchQuery::NameMode::Glob);
    q9.setText(QStringLiteral("report"));
    verifyQuery(q9.effectiveNameMode() == SearchQuery::NameMode::Glob, "Explicit Glob preserves Glob");
    SearchMatcher::Prepared matcher9(q9);
    FileInfo f9_pass; f9_pass.name = QStringLiteral("report");
    FileInfo f9_fail; f9_fail.name = QStringLiteral("annual_report.txt");
    verifyQuery(matcher9.matchesName(f9_pass), "Forced Glob matches exact whole name");
    verifyQuery(!matcher9.matchesName(f9_fail), "Forced Glob rejects filename where query is merely a substring");

    // 17. Zachowanie trybu w URL (match=auto, match=glob, brak match)
    SearchQuery qAuto;
    qAuto.setNameMode(SearchQuery::NameMode::Auto);
    qAuto.setText(QStringLiteral("test"));
    const QUrl urlAuto = qAuto.toUrl();
    verifyQuery(urlAuto.toString().contains(QStringLiteral("match=auto")), "Auto mode serializes match=auto");
    const SearchQuery parsedAuto = SearchQuery::fromUrl(urlAuto);
    verifyQuery(parsedAuto.nameMode() == SearchQuery::NameMode::Auto, "fromUrl parses match=auto as Auto mode");

    SearchQuery qGlob;
    qGlob.setNameMode(SearchQuery::NameMode::Glob);
    qGlob.setText(QStringLiteral("test"));
    const QUrl urlGlob = qGlob.toUrl();
    verifyQuery(urlGlob.toString().contains(QStringLiteral("match=glob")), "Glob mode serializes match=glob");
    const SearchQuery parsedGlob = SearchQuery::fromUrl(urlGlob);
    verifyQuery(parsedGlob.nameMode() == SearchQuery::NameMode::Glob, "fromUrl parses match=glob as Glob mode");

    SearchQuery qSub;
    qSub.setNameMode(SearchQuery::NameMode::Substring);
    qSub.setText(QStringLiteral("test"));
    const QUrl urlSub = qSub.toUrl();
    verifyQuery(!urlSub.toString().contains(QStringLiteral("match=")), "Substring mode omits match parameter");
    const SearchQuery parsedSub = SearchQuery::fromUrl(urlSub);
    verifyQuery(parsedSub.nameMode() == SearchQuery::NameMode::Substring, "fromUrl without match parses as Substring mode");

    // 18. Backward compatibility: brak parametru match w URL -> domyślny legacy Substring
    const QUrl legacyUrl(QStringLiteral("thispcsearch:/?search=annual&scope=2&type=0&date=0&size=0"));
    const SearchQuery legacySq = SearchQuery::fromUrl(legacyUrl);
    verifyQuery(legacySq.nameMode() == SearchQuery::NameMode::Substring, "Legacy URL without match defaults to Substring");
    verifyQuery(legacySq.effectiveNameMode() == SearchQuery::NameMode::Substring, "Legacy URL effective mode is Substring");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    testProductionCompatibility();
    testSearchQueryModel();
    testSearchQueryUrlCompatibility();
    testSearchScopeSafety();
    testSearchMatcherConjunctionsAndAlternatives();
    testSearchMatcherDeterministicTime();
    testStage2AdvancedQueryAndExecution();
    testAutoTextGlobMatchingModes();

    qInfo("PASS: %d search query & matcher assertions; model, validation, legacy URL compatibility, scopes, predicates, stage 2 advanced queries, Auto/Text/Glob matching modes", searchQueryChecks);
    return 0;
}
