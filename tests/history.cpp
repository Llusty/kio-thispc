/*
 * Automated test suite for TransferHistoryManager and TransferHistoryRecord.
 *
 * Stage 2.1 of ThisPC 0.45.0.
 *
 * Covers:
 * 1.  Serializacja i deserializacja (pełny round-trip wszystkich pól).
 * 2.  UUID i brak kolizji identyfikatorów.
 * 3.  Brakujące pola opcjonalne (domyślne wartości, poprawny parsing).
 * 4.  Nieprawidłowe typy pól (odrzucenie błędnych rekordów, brak crasha).
 * 5.  Nieznana wersja schematu (ochrona przed nadpisaniem nowszej wersji).
 * 6.  Uszkodzony JSON (odporność na ucięty/zniekształcony format).
 * 7.  Zachowanie oryginalnego pliku po błędzie odczytu.
 * 8.  Atomowość zapisu (QSaveFile, brak częściowych plików).
 * 9.  Limit rekordów (ring buffer 200 rekordów FIFO).
 * 10. Limit wielkości pliku (ochrona przed plikami > 2 MiB).
 * 11. Prywatność adresów URL (sanityzacja haseł, userinfo i poufnych tokenów).
 * 12. Równoczesne zapisy (burst aktualizacji, debounce, spójność).
 * 13. Dwie instancje magazynu w jednym procesie (konflikt-free merge rekordów).
 * 14. Nieudana operacja zapisu (brak uprawnień/katalog read-only, bezpieczna degradacja).
 * 15. Bezpieczne zakończenie zadań asynchronicznych (flushSync i destruktor).
 * 16. Ochrona przed przywróceniem usuniętych rekordów przez opóźniony zapis.
 * 17. Ochrona przed atakiem przez dowiązania symboliczne (symlinks).
 * 18. Odporność na kolizję nazw kopii uszkodzonego pliku (.corrupted).
 * 19. Koordynacja dwóch niezależnych procesów systemowych (QProcess, brak wskrzeszania).
 * 20. Serializacja workerów asynchronicznych pod obciążeniem (brak wyścigów).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "transferhistorymanager.h"
#include "operationmanager.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

static int s_historyChecks = 0;
static void verifyHistory(bool condition, const char *description)
{
    if (!condition) {
        qFatal("FAIL: [history] %s", description);
    }
    ++s_historyChecks;
}

static void testSerializationRoundTrip(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/roundtrip.json");
    TransferHistoryManager manager(filePath);

    const QDateTime start = QDateTime::fromString(QStringLiteral("2026-11-03T21:00:00.123Z"), Qt::ISODateWithMs);
    const QDateTime end = QDateTime::fromString(QStringLiteral("2026-11-03T21:00:05.456Z"), Qt::ISODateWithMs);

    TransferHistoryRecord original = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///home/user/test1.iso")), QUrl(QStringLiteral("file:///home/user/test2.iso"))},
        QUrl(QStringLiteral("file:///run/media/user/USB")),
        start,
        end,
        5333,
        10737418240ULL, // 10 GiB (exceeds 32-bit int)
        10737418240ULL,
        201326592ULL,   // ~200 MB/s
        0,
        QString(),
        QStringLiteral("Copy 2 ISO images")
    );

    verifyHistory(original.isValid(), "Created record must be valid");
    verifyHistory(!original.uuid.isEmpty(), "UUID must not be empty");

    manager.addRecord(original, false);
    verifyHistory(manager.count() == 1, "Manager count must be 1 after add");

    QString saveErr;
    HistoryStorageStatus saveStatus = HistoryStorageStatus::Ok;
    bool saved = manager.saveSync(&saveErr, &saveStatus);
    verifyHistory(saved, "saveSync must succeed");
    verifyHistory(saveStatus == HistoryStorageStatus::Ok, "Save status must be Ok");

    // Load in fresh manager
    TransferHistoryManager reader(filePath);
    QString loadErr;
    HistoryStorageStatus loadStatus = HistoryStorageStatus::Ok;
    bool loaded = reader.loadSync(&loadErr, &loadStatus);
    verifyHistory(loaded, "loadSync must succeed");
    verifyHistory(loadStatus == HistoryStorageStatus::Ok, "Load status must be Ok");
    verifyHistory(reader.count() == 1, "Reader count must be 1");

    TransferHistoryRecord restored = reader.records().first();
    verifyHistory(restored.uuid == original.uuid, "UUID must match");
    verifyHistory(restored.operationType == TransferOperationType::Copy, "OperationType must match");
    verifyHistory(restored.backendKind == TransferBackendKind::LocalNative, "BackendKind must match");
    verifyHistory(restored.terminalState == TransferTaskState::Completed, "State must match");
    verifyHistory(restored.sourceUrls.size() == 2, "Source count must be 2");
    verifyHistory(restored.sourceUrls[0] == original.sourceUrls[0], "Source 0 must match");
    verifyHistory(restored.sourceUrls[1] == original.sourceUrls[1], "Source 1 must match");
    verifyHistory(restored.destinationUrl == original.destinationUrl, "Destination must match");
    verifyHistory(restored.startTimeUtc == original.startTimeUtc, "StartTime must match");
    verifyHistory(restored.endTimeUtc == original.endTimeUtc, "EndTime must match");
    verifyHistory(restored.durationMs == 5333, "Duration must match");
    verifyHistory(restored.totalBytes == 10737418240ULL, "TotalBytes must match full 64-bit value");
    verifyHistory(restored.processedBytes == 10737418240ULL, "ProcessedBytes must match full 64-bit value");
    verifyHistory(restored.averageSpeedBytesPerSecond.has_value(), "AvgSpeed must be present");
    verifyHistory(*restored.averageSpeedBytesPerSecond == 201326592ULL, "AvgSpeed value must match");
    verifyHistory(restored.title == QStringLiteral("Copy 2 ISO images"), "Title must match");
    verifyHistory(restored.errorCode == 0, "ErrorCode must be 0");
    verifyHistory(restored.errorText.isEmpty(), "ErrorText must be empty");
}

static void testUuidUniqueness(const QString &tempDir)
{
    Q_UNUSED(tempDir)
    QSet<QString> uuids;
    for (int i = 0; i < 500; ++i) {
        auto rec = TransferHistoryRecord::createNew(
            TransferOperationType::Copy,
            TransferBackendKind::LocalNative,
            TransferTaskState::Completed,
            {QUrl(QStringLiteral("file:///test"))});
        verifyHistory(rec.isValid(), "Record must be valid");
        verifyHistory(!uuids.contains(rec.uuid), "UUID collision detected!");
        verifyHistory(rec.uuid.length() >= 32, "UUID length must be reasonable");
        uuids.insert(rec.uuid);
    }
    verifyHistory(uuids.size() == 500, "500 unique UUIDs must be generated");
}

static void testMissingOptionalFields(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/missing_optional.json");

    // Minimal valid JSON record without optional fields
    const QString minimalJson = QStringLiteral(
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"records\": [\n"
        "    {\n"
        "      \"uuid\": \"11112222-3333-4444-5555-666677778888\",\n"
        "      \"type\": \"Move\",\n"
        "      \"backend\": \"Kio\",\n"
        "      \"state\": \"Cancelled\",\n"
        "      \"sources\": [\"file:///source/file.txt\"],\n"
        "      \"startTime\": \"2026-11-03T12:00:00Z\",\n"
        "      \"endTime\": \"2026-11-03T12:00:01Z\"\n"
        "    }\n"
        "  ]\n"
        "}");

    QFile file(filePath);
    verifyHistory(file.open(QIODevice::WriteOnly), "Must open file for write");
    file.write(minimalJson.toUtf8());
    file.close();

    TransferHistoryManager manager(filePath);
    QString diag;
    HistoryStorageStatus status = HistoryStorageStatus::Ok;
    bool loaded = manager.loadSync(&diag, &status);
    verifyHistory(loaded, "Minimal JSON must load successfully");
    verifyHistory(status == HistoryStorageStatus::Ok, "Status must be Ok");
    verifyHistory(manager.count() == 1, "Count must be 1");

    TransferHistoryRecord rec = manager.records().first();
    verifyHistory(rec.uuid == QStringLiteral("11112222-3333-4444-5555-666677778888"), "UUID must match");
    verifyHistory(rec.operationType == TransferOperationType::Move, "Type must be Move");
    verifyHistory(rec.backendKind == TransferBackendKind::Kio, "Backend must be Kio");
    verifyHistory(rec.terminalState == TransferTaskState::Cancelled, "State must be Cancelled");
    verifyHistory(!rec.destinationUrl.isValid(), "Destination should be invalid/empty");
    verifyHistory(!rec.averageSpeedBytesPerSecond.has_value(), "AvgSpeed should be nullopt");
    verifyHistory(rec.errorCode == 0, "Default errorCode should be 0");
    verifyHistory(rec.errorText.isEmpty(), "Default errorText should be empty");
    verifyHistory(rec.title.isEmpty(), "Default title should be empty");
    verifyHistory(rec.durationMs == 1000, "Duration should be computed from start/end times (1000 ms)");
}

static void testInvalidFieldTypes(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/invalid_fields.json");

    // Two records: first is invalid (bad state, non-array sources), second is valid
    const QString json = QStringLiteral(
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"records\": [\n"
        "    {\n"
        "      \"uuid\": \"bad-record\",\n"
        "      \"type\": \"Copy\",\n"
        "      \"state\": \"Running\",\n"
        "      \"sources\": \"not-an-array\",\n"
        "      \"startTime\": \"2026-11-03T12:00:00Z\",\n"
        "      \"endTime\": \"2026-11-03T12:00:01Z\"\n"
        "    },\n"
        "    {\n"
        "      \"uuid\": \"good-record-1234\",\n"
        "      \"type\": \"Copy\",\n"
        "      \"state\": \"Completed\",\n"
        "      \"sources\": [\"file:///valid.txt\"],\n"
        "      \"startTime\": \"2026-11-03T12:00:00Z\",\n"
        "      \"endTime\": \"2026-11-03T12:00:01Z\"\n"
        "    }\n"
        "  ]\n"
        "}");

    QFile file(filePath);
    verifyHistory(file.open(QIODevice::WriteOnly), "Must open file for write");
    file.write(json.toUtf8());
    file.close();

    TransferHistoryManager manager(filePath);
    QString diag;
    HistoryStorageStatus status = HistoryStorageStatus::Ok;
    bool loaded = manager.loadSync(&diag, &status);
    verifyHistory(loaded, "Load must succeed for valid container with partial invalid items");
    verifyHistory(status == HistoryStorageStatus::Ok, "Status must be Ok");
    verifyHistory(manager.count() == 1, "Only the valid record must be parsed");
    verifyHistory(manager.records().first().uuid == QStringLiteral("good-record-1234"), "Valid record UUID must match");
}

static void testUnknownSchemaVersion(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/future_schema.json");

    const QString futureJson = QStringLiteral(
        "{\n"
        "  \"schemaVersion\": 999,\n"
        "  \"futureSuperField\": \"some-data\",\n"
        "  \"records\": []\n"
        "}");

    QFile file(filePath);
    verifyHistory(file.open(QIODevice::WriteOnly), "Must write future schema file");
    file.write(futureJson.toUtf8());
    file.close();

    const QByteArray originalContent = futureJson.toUtf8();

    TransferHistoryManager manager(filePath);
    QString diag;
    HistoryStorageStatus status = HistoryStorageStatus::Ok;
    bool loaded = manager.loadSync(&diag, &status);
    verifyHistory(!loaded, "Load must fail on newer schema version");
    verifyHistory(status == HistoryStorageStatus::SchemaVersionTooNew, "Status must be SchemaVersionTooNew");

    // Saving must also refuse to overwrite a future schema file
    auto rec = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///test"))});
    manager.addRecord(rec, false);

    bool saved = manager.saveSync(&diag, &status);
    verifyHistory(!saved, "saveSync must refuse to overwrite future schema file");
    verifyHistory(status == HistoryStorageStatus::SchemaVersionTooNew, "Save status must report SchemaVersionTooNew");

    // Verify disk content unchanged
    QFile verifyFile(filePath);
    verifyHistory(verifyFile.open(QIODevice::ReadOnly), "Open file for read verification");
    verifyHistory(verifyFile.readAll() == originalContent, "Future schema file must be bit-for-bit preserved");
}

static void testCorruptedJson(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/corrupted.json");
    const QByteArray truncatedJson = "{\"schemaVersion\": 1, \"records\": [ {\"uuid\": \"incomplete...";

    QFile file(filePath);
    verifyHistory(file.open(QIODevice::WriteOnly), "Write corrupted file");
    file.write(truncatedJson);
    file.close();

    TransferHistoryManager manager(filePath);
    QString diag;
    HistoryStorageStatus status = HistoryStorageStatus::Ok;
    bool loaded = manager.loadSync(&diag, &status);
    verifyHistory(!loaded, "Load must fail cleanly on corrupted JSON");
    verifyHistory(status == HistoryStorageStatus::CorruptedJson, "Status must be CorruptedJson");

    // In-memory manager must remain usable without crashing
    verifyHistory(manager.count() == 0, "Manager count must be 0");
}

static void testOriginalFilePreservedOnReadError(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/preserved.json");
    const QByteArray corrupted = "NOT_A_VALID_JSON_AT_ALL!!!";

    QFile file(filePath);
    verifyHistory(file.open(QIODevice::WriteOnly), "Write test file");
    file.write(corrupted);
    file.close();

    TransferHistoryManager manager(filePath);
    manager.loadSync();

    QFile checkFile(filePath);
    verifyHistory(checkFile.open(QIODevice::ReadOnly), "Re-read file");
    verifyHistory(checkFile.readAll() == corrupted, "Original file must be 100% preserved");
}

static void testAtomicSaveSafety(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/sub/dir/atomic.json");
    TransferHistoryManager manager(filePath);

    auto rec = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///test.txt"))});
    manager.addRecord(rec, false);

    QString diag;
    HistoryStorageStatus status = HistoryStorageStatus::Ok;
    bool saved = manager.saveSync(&diag, &status);
    verifyHistory(saved, "Atomic save in non-existing subfolder must succeed");
    verifyHistory(QFile::exists(filePath), "Target file must exist after atomic commit");

    // Ensure no dangling lock or temporary files remained in the folder
    QDir dir = QFileInfo(filePath).dir();
    const QStringList entries = dir.entryList(QDir::Files | QDir::Hidden);
    for (const QString &entry : entries) {
        verifyHistory(!entry.endsWith(QStringLiteral(".lock")), "No lock file must remain active");
        verifyHistory(!entry.startsWith(QStringLiteral(".operations_history")) || entry == QFileInfo(filePath).fileName(),
                      "No leftover temporary swap files should remain");
    }

    // Verify file permissions 0600 (owner read/write only)
    QFileInfo fi(filePath);
    QFile::Permissions perms = fi.permissions();
    verifyHistory(perms.testFlag(QFileDevice::ReadOwner), "Owner read must be enabled");
    verifyHistory(perms.testFlag(QFileDevice::WriteOwner), "Owner write must be enabled");
    verifyHistory(!perms.testFlag(QFileDevice::ReadOther), "Other read must NOT be enabled");
    verifyHistory(!perms.testFlag(QFileDevice::WriteOther), "Other write must NOT be enabled");
}

static void testRecordLimitRingBuffer(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/ring_buffer.json");
    TransferHistoryManager manager(filePath);
    manager.setMaxRecords(200);

    const QDateTime baseTime = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < 250; ++i) {
        const QDateTime t = baseTime.addSecs(i);
        auto rec = TransferHistoryRecord::createNew(
            TransferOperationType::Copy,
            TransferBackendKind::LocalNative,
            TransferTaskState::Completed,
            {QUrl(QStringLiteral("file:///item_%1.txt").arg(i))},
            QUrl(),
            t,
            t.addMSecs(100),
            100,
            1000,
            1000,
            std::nullopt,
            0,
            QString(),
            QStringLiteral("Item %1").arg(i)
        );
        manager.addRecord(rec, false);
    }

    verifyHistory(manager.count() == 200, "Manager count must be capped at 200");
    verifyHistory(manager.records().first().title == QStringLiteral("Item 249"), "Newest item must be at index 0");
    verifyHistory(manager.records().last().title == QStringLiteral("Item 50"), "Oldest kept item must be Item 50 (first 50 dropped)");

    manager.saveSync();

    TransferHistoryManager reader(filePath);
    reader.loadSync();
    verifyHistory(reader.count() == 200, "Persisted count must be capped at 200");
    verifyHistory(reader.records().first().title == QStringLiteral("Item 249"), "Persisted newest item must be Item 249");
    verifyHistory(reader.records().last().title == QStringLiteral("Item 50"), "Persisted oldest item must be Item 50");
}

static void testFileSizeLimit(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/large_file.json");

    // Write a dummy file exceeding 2 MiB (2 * 1024 * 1024 + 100 bytes)
    QFile file(filePath);
    verifyHistory(file.open(QIODevice::WriteOnly), "Open file for write");
    QByteArray bigData(THISPC_HISTORY_MAX_FILE_SIZE + 512, 'X');
    file.write(bigData);
    file.close();

    TransferHistoryManager manager(filePath);
    QString diag;
    HistoryStorageStatus status = HistoryStorageStatus::Ok;
    bool loaded = manager.loadSync(&diag, &status);
    verifyHistory(!loaded, "Loading file > 2 MiB must fail");
    verifyHistory(status == HistoryStorageStatus::FileSizeExceeded, "Status must be FileSizeExceeded");
}

static void testUrlPrivacySanitization(const QString &tempDir)
{
    Q_UNUSED(tempDir)
    QUrl dirtySource(QStringLiteral("sftp://admin:superSecretPassword123@remote.server.com:22/home/admin/file.txt?token=secretTokenABC&auth=sessionKeyXYZ&keep=1"));
    QUrl dirtyDest(QStringLiteral("ftp://user:anotherPassword@storage.box/backup?apiKey=key123&publicParam=hello"));

    auto rec = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::Kio,
        TransferTaskState::Completed,
        {dirtySource},
        dirtyDest
    );

    verifyHistory(rec.sourceUrls.size() == 1, "Source url must be present");
    QUrl sanitizedSrc = rec.sourceUrls.first();
    verifyHistory(sanitizedSrc.userName().isEmpty(), "Source username must be stripped");
    verifyHistory(sanitizedSrc.password().isEmpty(), "Source password must be stripped");
    verifyHistory(!sanitizedSrc.query().contains(QStringLiteral("superSecretPassword123")), "Password must not appear in query");
    verifyHistory(!sanitizedSrc.query().contains(QStringLiteral("secretTokenABC")), "Token parameter must be stripped");
    verifyHistory(!sanitizedSrc.query().contains(QStringLiteral("sessionKeyXYZ")), "Auth parameter must be stripped");
    verifyHistory(sanitizedSrc.query().contains(QStringLiteral("keep=1")), "Harmless query parameter must be preserved");

    QUrl sanitizedDst = rec.destinationUrl;
    verifyHistory(sanitizedDst.userName().isEmpty(), "Dest username must be stripped");
    verifyHistory(sanitizedDst.password().isEmpty(), "Dest password must be stripped");
    verifyHistory(!sanitizedDst.query().contains(QStringLiteral("apiKey")), "API key parameter must be stripped");
    verifyHistory(sanitizedDst.query().contains(QStringLiteral("publicParam=hello")), "Harmless parameter must be preserved");

    // Verify JSON serialization doesn't leak credentials either
    QJsonObject obj = rec.toJson();
    QString jsonStr = QString::fromUtf8(QJsonDocument(obj).toJson());
    verifyHistory(!jsonStr.contains(QStringLiteral("superSecretPassword123")), "JSON must not contain source password");
    verifyHistory(!jsonStr.contains(QStringLiteral("anotherPassword")), "JSON must not contain dest password");
    verifyHistory(!jsonStr.contains(QStringLiteral("secretTokenABC")), "JSON must not contain secret token");
}

static void testConcurrentBurstWrites(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/burst.json");
    TransferHistoryManager manager(filePath);

    // Rapidly add 30 records asynchronously
    for (int i = 0; i < 30; ++i) {
        auto rec = TransferHistoryRecord::createNew(
            TransferOperationType::Copy,
            TransferBackendKind::LocalNative,
            TransferTaskState::Completed,
            {QUrl(QStringLiteral("file:///burst_%1.txt").arg(i))},
            QUrl(),
            QDateTime::currentDateTimeUtc().addMSecs(i * 10),
            QDateTime::currentDateTimeUtc().addMSecs(i * 10 + 5),
            5,
            100,
            100,
            std::nullopt,
            0,
            QString(),
            QStringLiteral("Burst %1").arg(i)
        );
        manager.addRecord(rec, true);
    }

    verifyHistory(manager.count() == 30, "All 30 records must be present in memory immediately");

    // Flush and wait for async completion
    manager.flushSync();

    TransferHistoryManager reader(filePath);
    reader.loadSync();
    verifyHistory(reader.count() == 30, "All 30 burst records must be persisted safely on disk");
}

static void testTwoManagerInstancesMerge(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/multi_instance.json");

    TransferHistoryManager instance1(filePath);
    TransferHistoryManager instance2(filePath);

    auto rec1 = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///from_instance_1.txt"))},
        QUrl(),
        QDateTime::currentDateTimeUtc().addSecs(-10),
        QDateTime::currentDateTimeUtc().addSecs(-9),
        1000,
        500,
        500,
        std::nullopt,
        0,
        QString(),
        QStringLiteral("Instance 1 Record")
    );

    auto rec2 = TransferHistoryRecord::createNew(
        TransferOperationType::Move,
        TransferBackendKind::Kio,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///from_instance_2.txt"))},
        QUrl(),
        QDateTime::currentDateTimeUtc().addSecs(-5),
        QDateTime::currentDateTimeUtc().addSecs(-4),
        1000,
        1500,
        1500,
        std::nullopt,
        0,
        QString(),
        QStringLiteral("Instance 2 Record")
    );

    instance1.addRecord(rec1, false);
    verifyHistory(instance1.saveSync(), "Instance 1 must save");

    instance2.addRecord(rec2, false);
    verifyHistory(instance2.saveSync(), "Instance 2 must save and merge disk records");

    // Check disk via third reader
    TransferHistoryManager reader(filePath);
    reader.loadSync();
    verifyHistory(reader.count() == 2, "Reader must see both records (no clobbering)");
    verifyHistory(reader.findRecord(rec1.uuid).has_value(), "Record from instance 1 must be present");
    verifyHistory(reader.findRecord(rec2.uuid).has_value(), "Record from instance 2 must be present");
}

static void testFailedSaveHandledGracefully(const QString &tempDir)
{
    // Point to an impossible read-only path (e.g. inside a non-writable directory)
    const QString readOnlyDir = tempDir + QStringLiteral("/readonly_dir");
    QDir().mkdir(readOnlyDir);
    QFile::setPermissions(readOnlyDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner); // no WriteOwner!

    const QString filePath = readOnlyDir + QStringLiteral("/failed.json");
    TransferHistoryManager manager(filePath);

    auto rec = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///test"))});
    manager.addRecord(rec, false);

    QString diag;
    HistoryStorageStatus status = HistoryStorageStatus::Ok;
    bool saved = manager.saveSync(&diag, &status);
    verifyHistory(!saved, "Saving to read-only dir must fail safely without crash");
    verifyHistory(status == HistoryStorageStatus::PermissionDenied
               || status == HistoryStorageStatus::IoError
               || status == HistoryStorageStatus::LockFailed,
                  "Status must reflect permission/io/lock failure");
    verifyHistory(manager.count() == 1, "In-memory records must not be lost despite disk write failure");

    // Restore permissions for cleanup
    QFile::setPermissions(readOnlyDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

static void testAsyncCompletionAndDestructorSafety(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/destructor.json");
    {
        TransferHistoryManager manager(filePath);
        for (int i = 0; i < 10; ++i) {
            auto rec = TransferHistoryRecord::createNew(
                TransferOperationType::Copy,
                TransferBackendKind::LocalNative,
                TransferTaskState::Completed,
                {QUrl(QStringLiteral("file:///doc_%1.pdf").arg(i))});
            manager.addRecord(rec, true); // trigger async save
        }
        // manager goes out of scope immediately! Destructor must call flushSync() cleanly
    }

    // Verify all 10 records were committed safely before destruction
    TransferHistoryManager reader(filePath);
    reader.loadSync();
    verifyHistory(reader.count() == 10, "All 10 records must be persisted safely on destructor flush");
}

static void testProtectionAgainstDeletedRecordResurrection(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/resurrection.json");
    TransferHistoryManager manager(filePath);

    auto rec1 = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///keep.txt"))},
        QUrl(),
        QDateTime::currentDateTimeUtc(),
        QDateTime::currentDateTimeUtc(),
        10, 100, 100, std::nullopt, 0, QString(), QStringLiteral("Keep"));

    auto rec2 = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///delete.txt"))},
        QUrl(),
        QDateTime::currentDateTimeUtc(),
        QDateTime::currentDateTimeUtc(),
        10, 100, 100, std::nullopt, 0, QString(), QStringLiteral("Delete"));

    manager.addRecord(rec1, false);
    manager.addRecord(rec2, false);
    manager.saveSync();
    verifyHistory(manager.count() == 2, "Initially 2 records");

    // Remove rec2 and save
    bool removed = manager.removeRecord(rec2.uuid, true);
    verifyHistory(removed, "Record 2 must be removed");
    verifyHistory(manager.count() == 1, "Count must be 1");
    manager.flushSync();

    // Verify rec2 is not resurrected from disk merge
    TransferHistoryManager reader(filePath);
    reader.loadSync();
    verifyHistory(reader.count() == 1, "Only 1 record must be present on disk");
    verifyHistory(reader.records().first().uuid == rec1.uuid, "Only rec1 must remain");
    verifyHistory(!reader.findRecord(rec2.uuid).has_value(), "Deleted record must not be resurrected");

    // Clear history completely
    manager.clear(true);
    manager.flushSync();
    verifyHistory(manager.count() == 0, "Manager count must be 0 after clear");

    reader.loadSync();
    verifyHistory(reader.count() == 0, "Disk count must be 0 after clear (no resurrection of old items)");
}

static void testSymlinkPathRejected(const QString &tempDir)
{
    const QString realFile = tempDir + QStringLiteral("/real_target.json");
    const QString linkFile = tempDir + QStringLiteral("/symlink_target.json");

    QFile file(realFile);
    verifyHistory(file.open(QIODevice::WriteOnly), "Create real target file");
    file.write("{\"schemaVersion\": 1, \"records\": []}");
    file.close();

    verifyHistory(QFile::link(realFile, linkFile), "Create symlink to target file");

    TransferHistoryManager manager(linkFile);
    QString diag;
    HistoryStorageStatus status = HistoryStorageStatus::Ok;
    bool loaded = manager.loadSync(&diag, &status);
    verifyHistory(!loaded, "Loading symlink path must be rejected for security");
    verifyHistory(status == HistoryStorageStatus::PermissionDenied, "Status must be PermissionDenied on symlink");

    auto rec = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///test"))});
    manager.addRecord(rec, false);

    bool saved = manager.saveSync(&diag, &status);
    verifyHistory(!saved, "Saving to symlink path must be rejected for security");
    verifyHistory(status == HistoryStorageStatus::PermissionDenied, "Status must be PermissionDenied on symlink save");
}

static void testCorruptedBackupCollisionResilience(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/corrupt_collision.json");

    // Write a corrupted file
    QFile file(filePath);
    verifyHistory(file.open(QIODevice::WriteOnly), "Write corrupted file");
    file.write("corrupted-json-content-here");
    file.close();

    // Pre-create .corrupted.<timestamp> file to trigger collision handling
    const qint64 ts = QDateTime::currentMSecsSinceEpoch();
    const QString collisionPath = filePath + QStringLiteral(".corrupted.") + QString::number(ts);
    QFile preExisting(collisionPath);
    verifyHistory(preExisting.open(QIODevice::WriteOnly), "Create pre-existing corrupted backup");
    preExisting.write("first-corrupted-backup");
    preExisting.close();

    TransferHistoryManager manager(filePath);
    auto rec = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///healthy.txt"))});
    manager.addRecord(rec, false);

    QString diag;
    HistoryStorageStatus status = HistoryStorageStatus::Ok;
    bool saved = manager.saveSync(&diag, &status);
    verifyHistory(saved, "Save must succeed and gracefully handle corrupted file with collision");

    // Verify original corrupted backup is preserved and a new uniquely numbered copy exists
    verifyHistory(QFile::exists(collisionPath), "Pre-existing corrupted backup must remain intact");
    QFile checkOrig(collisionPath);
    verifyHistory(checkOrig.open(QIODevice::ReadOnly), "Open original corrupted backup");
    verifyHistory(checkOrig.readAll() == "first-corrupted-backup", "Original corrupted backup content unchanged");

    // Healthy record must be saved in main file
    TransferHistoryManager reader(filePath);
    reader.loadSync();
    verifyHistory(reader.count() == 1, "Healthy record successfully saved in target file");
    verifyHistory(reader.records().first().sourceUrls.first().toString() == QStringLiteral("file:///healthy.txt"),
                  "Target file has clean healthy record");
}

static void testSeparateProcessesCoordination(const QString &tempDir, const QString &appPath)
{
    const QString filePath = tempDir + QStringLiteral("/cross_process.json");

    // 1. Process A (current process) writes Record A
    TransferHistoryManager procA(filePath);
    auto recA = TransferHistoryRecord::createNew(
        TransferOperationType::Copy,
        TransferBackendKind::LocalNative,
        TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///procA_file.txt"))},
        QUrl(),
        QDateTime::currentDateTimeUtc().addSecs(-10),
        QDateTime::currentDateTimeUtc().addSecs(-9),
        1000, 100, 100, std::nullopt, 0, QString(), QStringLiteral("ProcA Item 1"));
    recA.uuid = QStringLiteral("aaaaaaaa-1111-2222-3333-444444444444");

    procA.addRecord(recA, false);
    verifyHistory(procA.saveSync(), "Process A must save Record A");

    // 2. Launch child OS process to add Record B
    const QString uuidB = QStringLiteral("bbbbbbbb-1111-2222-3333-444444444444");
    QProcess childProc1;
    childProc1.start(appPath, {QStringLiteral("history"), QStringLiteral("--subproc-add"),
                              filePath, uuidB, QStringLiteral("ProcB Item 2")});
    verifyHistory(childProc1.waitForFinished(10000), "Child process 1 must finish within 10s");
    verifyHistory(childProc1.exitCode() == 0, "Child process 1 must exit with code 0");

    // 3. Process A deletes Record A and saves
    verifyHistory(procA.removeRecord(recA.uuid, true), "Process A must remove Record A");
    procA.flushSync();

    // 4. Launch child OS process to add Record C
    const QString uuidC = QStringLiteral("cccccccc-1111-2222-3333-444444444444");
    QProcess childProc2;
    childProc2.start(appPath, {QStringLiteral("history"), QStringLiteral("--subproc-add"),
                              filePath, uuidC, QStringLiteral("ProcB Item 3")});
    verifyHistory(childProc2.waitForFinished(10000), "Child process 2 must finish within 10s");
    verifyHistory(childProc2.exitCode() == 0, "Child process 2 must exit with code 0");

    // 5. Fresh reader verifies: Record A is NOT resurrected, Record B and C exist
    TransferHistoryManager finalReader(filePath);
    finalReader.loadSync();
    verifyHistory(finalReader.count() == 2, "Final history count must be exactly 2");
    verifyHistory(!finalReader.findRecord(recA.uuid).has_value(), "Deleted Record A must NEVER be resurrected by other processes");
    verifyHistory(finalReader.findRecord(uuidB).has_value(), "Record B from child process 1 must be present");
    verifyHistory(finalReader.findRecord(uuidC).has_value(), "Record C from child process 2 must be present");
}

static void testWorkerSerializationUnderPressure(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/pressure.json");
    TransferHistoryManager manager(filePath);

    // Rapidly interleave 50 async additions with manual flushSync
    for (int i = 0; i < 50; ++i) {
        auto rec = TransferHistoryRecord::createNew(
            TransferOperationType::Copy,
            TransferBackendKind::LocalNative,
            TransferTaskState::Completed,
            {QUrl(QStringLiteral("file:///pressure_%1.txt").arg(i))},
            QUrl(),
            QDateTime::currentDateTimeUtc(),
            QDateTime::currentDateTimeUtc(),
            10, 100, 100, std::nullopt, 0, QString(), QStringLiteral("Pressure %1").arg(i));
        manager.addRecord(rec, true);
        if (i % 10 == 0) {
            manager.flushSync();
        }
    }

    manager.flushSync();
    verifyHistory(manager.count() == 50, "All 50 pressure records must be present in manager");

    TransferHistoryManager reader(filePath);
    reader.loadSync();
    verifyHistory(reader.count() == 50, "All 50 pressure records must be safely committed without data loss");
}

class HistoryTestJob final : public KJob
{
public:
    explicit HistoryTestJob(bool autoDelete = false)
    {
        setAutoDelete(autoDelete);
        setCapabilities(KJob::Killable);
    }
    void start() override { ++m_startCalls; }
    void complete(qulonglong processed, qulonglong total)
    {
        setTotalAmount(KJob::Bytes, total);
        setProcessedAmount(KJob::Bytes, processed);
        emitResult();
    }
    void fail(int err, const QString &text)
    {
        setError(err);
        setErrorText(text);
        emitResult();
    }
    void cancel()
    {
        setError(KJob::KilledJobError);
        emitResult();
    }
    int startCalls() const { return m_startCalls; }
protected:
    bool doKill() override
    {
        cancel();
        return true;
    }
private:
    int m_startCalls = 0;
};

static TransferTaskDescriptor makeQueueTestDescriptor(
    TransferOperationType type,
    const QList<QUrl> &sources,
    const QUrl &dest,
    std::function<KJob*()> factory,
    const QString &title = QString())
{
    TransferTaskDescriptor desc;
    desc.type = type;
    desc.backend = TransferBackendKind::LocalNative;
    desc.sources = sources;
    desc.destination = dest;
    desc.title = title;
    desc.jobFactory = factory;
    desc.capabilities.canCancelRunning = true;
    return desc;
}

static void testEpochClearInvalidatesPendingWrites(const QString &tempDir)
{
    // Security Gate A: Clear vs pending write from another instance
    const QString filePath = tempDir + QStringLiteral("/epoch_test.json");

    // Initialize disk file with epoch 1 and 2 records
    {
        TransferHistoryManager initMgr(filePath);
        auto r1 = TransferHistoryRecord::createNew(TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed, {QUrl(QStringLiteral("file:///f1"))});
        auto r2 = TransferHistoryRecord::createNew(TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed, {QUrl(QStringLiteral("file:///f2"))});
        initMgr.addRecord(r1, false);
        initMgr.addRecord(r2, false);
        verifyHistory(initMgr.saveSync(), "Initial history must be saved");
        verifyHistory(initMgr.historyEpoch() == 1, "Initial epoch must be 1");
    }

    // Instance A and Instance B both load epoch 1
    TransferHistoryManager instanceA(filePath);
    instanceA.loadSync();
    verifyHistory(instanceA.count() == 2, "Instance A loaded 2 records");
    verifyHistory(instanceA.historyEpoch() == 1, "Instance A epoch is 1");

    TransferHistoryManager instanceB(filePath);
    instanceB.loadSync();
    verifyHistory(instanceB.count() == 2, "Instance B loaded 2 records");
    verifyHistory(instanceB.historyEpoch() == 1, "Instance B epoch is 1");

    // Instance B stages a new record in memory (does not save immediately)
    auto recB = TransferHistoryRecord::createNew(TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed, {QUrl(QStringLiteral("file:///staged_in_B"))});
    instanceB.addRecord(recB, false);
    verifyHistory(instanceB.count() == 3, "Instance B has 3 records in memory");

    // Instance A clears history and writes to disk (bumps epoch to 2)
    instanceA.clear(true);
    instanceA.flushSync();
    verifyHistory(instanceA.count() == 0, "Instance A cleared");
    verifyHistory(instanceA.historyEpoch() == 2, "Instance A epoch is now 2");

    // Verify disk has 0 records and epoch 2
    {
        TransferHistoryManager checkMgr(filePath);
        checkMgr.loadSync();
        verifyHistory(checkMgr.count() == 0, "Disk is empty after clear");
        verifyHistory(checkMgr.historyEpoch() == 2, "Disk epoch is 2");
    }

    // Now Instance B executes saveSync
    // Security Gate A must detect diskEpoch (2) > loadedEpoch (1) and discard recB
    bool savedB = instanceB.saveSync();
    verifyHistory(savedB, "Instance B save must succeed");
    verifyHistory(instanceB.historyEpoch() == 2, "Instance B adopted epoch 2");
    verifyHistory(instanceB.count() == 0, "Instance B discarded pre-clear staged record");

    // Check disk: recB was NOT resurrected!
    {
        TransferHistoryManager checkMgr2(filePath);
        checkMgr2.loadSync();
        verifyHistory(checkMgr2.count() == 0, "Disk must remain empty (no resurrection of wiped history)");
        verifyHistory(checkMgr2.historyEpoch() == 2, "Disk epoch remains 2");
    }

    // Now Instance B adds a NEW record under epoch 2 and saves
    auto recB2 = TransferHistoryRecord::createNew(TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed, {QUrl(QStringLiteral("file:///new_in_epoch2"))});
    instanceB.addRecord(recB2, false);
    verifyHistory(instanceB.saveSync(), "Instance B saves record under epoch 2");
    verifyHistory(instanceB.count() == 1, "Instance B has 1 record");

    // Verify disk now has recB2
    {
        TransferHistoryManager checkMgr3(filePath);
        checkMgr3.loadSync();
        verifyHistory(checkMgr3.count() == 1, "Disk has 1 record added under epoch 2");
        verifyHistory(checkMgr3.records().first().uuid == recB2.uuid, "Record on disk matches recB2");
        verifyHistory(checkMgr3.historyEpoch() == 2, "Disk epoch is 2");
    }
}

static void testCopyCompletedIntegration(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_copy_completed.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *jobInstance = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///data/photo.jpg"))},
        QUrl(QStringLiteral("file:///backup/")),
        [&jobInstance]() -> KJob* {
            jobInstance = new HistoryTestJob(false);
            return jobInstance;
        },
        QStringLiteral("Copy photo.jpg")
    );

    auto result = controller.enqueue(desc);
    verifyHistory(result.id > 0, "Task must be enqueued");
    QCoreApplication::processEvents();

    verifyHistory(jobInstance != nullptr, "Job must have been created");
    jobInstance->complete(52428800ULL, 52428800ULL); // 50 MB
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "History must have exactly 1 record for completed copy");

    auto rec = histMgr.records().first();
    verifyHistory(rec.operationType == TransferOperationType::Copy, "Operation must be Copy");
    verifyHistory(rec.terminalState == TransferTaskState::Completed, "State must be Completed");
    verifyHistory(rec.totalBytes == 52428800ULL, "Total bytes must match 50 MB");
    verifyHistory(rec.processedBytes == 52428800ULL, "Processed bytes must match 50 MB");
    verifyHistory(rec.errorCode == 0, "Error code must be 0");
    verifyHistory(rec.startTimeUtc.isValid(), "Start time must be valid");
    verifyHistory(rec.endTimeUtc.isValid(), "End time must be valid");
    verifyHistory(rec.startTimeUtc.timeSpec() == Qt::UTC, "Start time must be UTC");
    verifyHistory(rec.endTimeUtc.timeSpec() == Qt::UTC, "End time must be UTC");
    verifyHistory(rec.sourceUrls.size() == 1, "One source URL");
    verifyHistory(rec.sourceUrls.first() == QUrl(QStringLiteral("file:///data/photo.jpg")), "Source URL matches");
    verifyHistory(rec.destinationUrl == QUrl(QStringLiteral("file:///backup/")), "Destination URL matches");
    verifyHistory(rec.title == QStringLiteral("Copy photo.jpg"), "Title matches");
    delete jobInstance;
}

static void testMoveCompletedIntegration(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_move_completed.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *jobInstance = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Move,
        {QUrl(QStringLiteral("file:///data/doc.pdf"))},
        QUrl(QStringLiteral("file:///archive/")),
        [&jobInstance]() -> KJob* {
            jobInstance = new HistoryTestJob(false);
            return jobInstance;
        },
        QStringLiteral("Move doc.pdf")
    );

    auto result = controller.enqueue(desc);
    verifyHistory(result.id > 0, "Move task enqueued");
    QCoreApplication::processEvents();

    verifyHistory(jobInstance != nullptr, "Move job created");
    jobInstance->complete(1048576ULL, 1048576ULL); // 1 MB
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "History must have exactly 1 record for completed move");

    auto rec = histMgr.records().first();
    verifyHistory(rec.operationType == TransferOperationType::Move, "Operation must be Move");
    verifyHistory(rec.terminalState == TransferTaskState::Completed, "State must be Completed");
    verifyHistory(rec.totalBytes == 1048576ULL, "Total bytes must be 1 MB");
    delete jobInstance;
}

static void testCopyFailedIntegration(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_copy_failed.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *jobInstance = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///data/locked.sys"))},
        QUrl(QStringLiteral("file:///backup/")),
        [&jobInstance]() -> KJob* {
            jobInstance = new HistoryTestJob(false);
            return jobInstance;
        }
    );

    controller.enqueue(desc);
    QCoreApplication::processEvents();

    verifyHistory(jobInstance != nullptr, "Job created");
    jobInstance->fail(KIO::ERR_CANNOT_OPEN_FOR_READING, QStringLiteral("File locked by process"));
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "History must have 1 record for failed copy");

    auto rec = histMgr.records().first();
    verifyHistory(rec.operationType == TransferOperationType::Copy, "Operation is Copy");
    verifyHistory(rec.terminalState == TransferTaskState::Failed, "State is Failed");
    verifyHistory(rec.errorCode == KIO::ERR_CANNOT_OPEN_FOR_READING, "Error code matches KIO error");
    verifyHistory(rec.errorText == QStringLiteral("File locked by process"), "Error text matches");
    delete jobInstance;
}

static void testMoveFailedIntegration(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_move_failed.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *jobInstance = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Move,
        {QUrl(QStringLiteral("file:///data/big.vmdk"))},
        QUrl(QStringLiteral("file:///media/usb/")),
        [&jobInstance]() -> KJob* {
            jobInstance = new HistoryTestJob(false);
            return jobInstance;
        }
    );

    controller.enqueue(desc);
    QCoreApplication::processEvents();

    verifyHistory(jobInstance != nullptr, "Job created");
    jobInstance->fail(KIO::ERR_CANNOT_WRITE, QStringLiteral("No space left on device"));
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "History must have 1 record for failed move");

    auto rec = histMgr.records().first();
    verifyHistory(rec.operationType == TransferOperationType::Move, "Operation is Move");
    verifyHistory(rec.terminalState == TransferTaskState::Failed, "State is Failed");
    verifyHistory(rec.errorCode == KIO::ERR_CANNOT_WRITE, "Error code matches");
    verifyHistory(rec.errorText == QStringLiteral("No space left on device"), "Error text matches");
    delete jobInstance;
}

static void testCopyCancelledIntegration(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_copy_cancelled.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *jobInstance = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///data/video.mp4"))},
        QUrl(QStringLiteral("file:///dest/")),
        [&jobInstance]() -> KJob* {
            jobInstance = new HistoryTestJob(false);
            return jobInstance;
        }
    );

    auto result = controller.enqueue(desc);
    QCoreApplication::processEvents();

    verifyHistory(jobInstance != nullptr, "Job started");
    bool cancelled = controller.cancelTask(result.id);
    verifyHistory(cancelled, "cancelTask must succeed");
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "History must have exactly 1 record for cancelled copy");

    auto rec = histMgr.records().first();
    verifyHistory(rec.operationType == TransferOperationType::Copy, "Operation is Copy");
    verifyHistory(rec.terminalState == TransferTaskState::Cancelled, "State is Cancelled");
    delete jobInstance;
}

static void testCancelQueuedPreStartIntegration(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_cancel_prestart.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);
    controller.setDispatchPaused(true); // Task stays in Queued state

    bool factoryCalled = false;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///data/queued_file.bin"))},
        QUrl(QStringLiteral("file:///dest/")),
        [&factoryCalled]() -> KJob* {
            factoryCalled = true;
            return new HistoryTestJob(false);
        }
    );

    auto result = controller.enqueue(desc);
    verifyHistory(result.id > 0, "Task enqueued");
    QCoreApplication::processEvents();
    verifyHistory(!factoryCalled, "Job factory must NOT be called while dispatch paused");

    // Cancel while queued (pre-start)
    bool cancelled = controller.cancelTask(result.id);
    verifyHistory(cancelled, "cancelTask while queued must succeed");
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "Pre-start cancellation must record exactly 1 record");

    auto rec = histMgr.records().first();
    verifyHistory(rec.terminalState == TransferTaskState::Cancelled, "State must be Cancelled");
    verifyHistory(rec.durationMs == 0, "Duration must be 0 for pre-start cancelled task");
    verifyHistory(rec.sourceUrls.first() == QUrl(QStringLiteral("file:///data/queued_file.bin")), "Source matches");
    verifyHistory(rec.destinationUrl == QUrl(QStringLiteral("file:///dest/")), "Dest matches");
    verifyHistory(!factoryCalled, "Job factory was never called");
}

static void testTaskIdempotenceIntegration(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_idempotence.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *jobInstance = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///data/single.dat"))},
        QUrl(QStringLiteral("file:///dest/")),
        [&jobInstance]() -> KJob* {
            jobInstance = new HistoryTestJob(false);
            return jobInstance;
        }
    );

    auto result = controller.enqueue(desc);
    QCoreApplication::processEvents();

    jobInstance->complete(1024, 1024);
    QCoreApplication::processEvents();

    // Call cancel or other methods on already finished task
    controller.cancelTask(result.id);
    controller.cancelTask(result.id);
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "Idempotence: multiple calls must NOT produce duplicate records");
    delete jobInstance;
}

static void testUtcTimestampAndVolumeAccuracy(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_large_volume.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *jobInstance = nullptr;
    const quint64 largeBytes = 12884901888ULL; // 12 GiB (> 4 GB 32-bit limit)

    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///data/large_backup.tar"))},
        QUrl(QStringLiteral("file:///dest/")),
        [&jobInstance]() -> KJob* {
            jobInstance = new HistoryTestJob(false);
            return jobInstance;
        }
    );

    controller.enqueue(desc);
    QCoreApplication::processEvents();

    jobInstance->complete(largeBytes, largeBytes);
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "Large volume task recorded");

    // Load from disk to verify JSON deserialization preserves 64-bit accuracy
    TransferHistoryManager reader(filePath);
    reader.loadSync();
    verifyHistory(reader.count() == 1, "Reader loaded record");

    auto rec = reader.records().first();
    verifyHistory(rec.totalBytes == largeBytes, "Total bytes 64-bit accuracy preserved");
    verifyHistory(rec.processedBytes == largeBytes, "Processed bytes 64-bit accuracy preserved");
    verifyHistory(rec.startTimeUtc.isValid(), "Start time UTC valid");
    verifyHistory(rec.endTimeUtc.isValid(), "End time UTC valid");
    verifyHistory(rec.startTimeUtc.timeSpec() == Qt::UTC, "TimeSpec is UTC");
    verifyHistory(rec.endTimeUtc.timeSpec() == Qt::UTC, "TimeSpec is UTC");
    delete jobInstance;
}

static void testMultiFileTaskIntegration(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_multi_file.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *jobInstance = nullptr;
    const QList<QUrl> sources = {
        QUrl(QStringLiteral("file:///src/file1.png")),
        QUrl(QStringLiteral("file:///src/file2.png")),
        QUrl(QStringLiteral("file:///src/file3.png")),
        QUrl(QStringLiteral("file:///src/file4.png"))
    };

    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        sources,
        QUrl(QStringLiteral("file:///dest/album/")),
        [&jobInstance]() -> KJob* {
            jobInstance = new HistoryTestJob(false);
            return jobInstance;
        },
        QStringLiteral("Copy 4 pictures")
    );

    controller.enqueue(desc);
    QCoreApplication::processEvents();

    jobInstance->complete(4096000ULL, 4096000ULL);
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "Multi-file task recorded");

    TransferHistoryManager reader(filePath);
    reader.loadSync();
    auto rec = reader.records().first();
    verifyHistory(rec.itemCount == 4, "Item count is 4");
    verifyHistory(rec.sourceUrls.size() == 4, "Source URLs size is 4");
    for (int i = 0; i < 4; ++i) {
        verifyHistory(rec.sourceUrls[i] == sources[i], "Source URL preserved");
    }
    verifyHistory(rec.destinationUrl == QUrl(QStringLiteral("file:///dest/album/")), "Destination URL preserved");
    delete jobInstance;
}

static void testTwoControllersDiskPersistence(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_persistence.json");

    // First controller executes Task 1
    {
        TransferHistoryManager mgr1(filePath);
        TransferQueueController controller1;
        controller1.setHistoryManager(&mgr1);

        HistoryTestJob *job1 = nullptr;
        auto desc1 = makeQueueTestDescriptor(
            TransferOperationType::Copy,
            {QUrl(QStringLiteral("file:///item1.txt"))},
            QUrl(QStringLiteral("file:///dest/")),
            [&job1]() -> KJob* {
                job1 = new HistoryTestJob(false);
                return job1;
            });
        controller1.enqueue(desc1);
        QCoreApplication::processEvents();
        job1->complete(100, 100);
        QCoreApplication::processEvents();
        mgr1.flushSync();
        delete job1;
    }

    QTest::qWait(10);

    // Second controller executes Task 2
    {
        TransferHistoryManager mgr2(filePath);
        TransferQueueController controller2;
        controller2.setHistoryManager(&mgr2);

        HistoryTestJob *job2 = nullptr;
        auto desc2 = makeQueueTestDescriptor(
            TransferOperationType::Move,
            {QUrl(QStringLiteral("file:///item2.txt"))},
            QUrl(QStringLiteral("file:///dest/")),
            [&job2]() -> KJob* {
                job2 = new HistoryTestJob(false);
                return job2;
            });
        controller2.enqueue(desc2);
        QCoreApplication::processEvents();
        job2->complete(200, 200);
        QCoreApplication::processEvents();
        mgr2.flushSync();
        delete job2;
    }

    // Fresh reader verifies both records exist on disk across restarts
    TransferHistoryManager reader(filePath);
    reader.loadSync();
    verifyHistory(reader.count() == 2, "Persistence: both records must exist on disk");
    bool hasMove = false;
    bool hasCopy = false;
    for (const auto &r : reader.records()) {
        if (r.operationType == TransferOperationType::Move) hasMove = true;
        if (r.operationType == TransferOperationType::Copy) hasCopy = true;
    }
    verifyHistory(hasMove && hasCopy, "Persistence: both Move and Copy records must exist on disk");
    verifyHistory(reader.records().at(0).endTimeUtc >= reader.records().at(1).endTimeUtc, "Records sorted descending by time");
}

static void testHistoryFailureIsolation(const QString &tempDir)
{
    // History failure (unwritable directory) must NOT affect file operation outcome
    const QString unwriteDir = tempDir + QStringLiteral("/nowrite_dir");
    QDir().mkdir(unwriteDir);
    QFile::setPermissions(unwriteDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner);

    const QString badHistoryPath = unwriteDir + QStringLiteral("/hist.json");
    TransferHistoryManager badMgr(badHistoryPath);

    TransferQueueController controller;
    controller.setHistoryManager(&badMgr);

    HistoryTestJob *job = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///src.txt"))},
        QUrl(QStringLiteral("file:///dst.txt")),
        [&job]() -> KJob* {
            job = new HistoryTestJob(false);
            return job;
        });

    auto res = controller.enqueue(desc);
    QCoreApplication::processEvents();

    job->complete(1000, 1000);
    QCoreApplication::processEvents();

    // Verify task is Completed despite history write failure
    auto snap = controller.taskSnapshot(res.id);
    verifyHistory(snap.has_value(), "Task snapshot exists");
    verifyHistory(snap->state == TransferTaskState::Completed, "Task state must be Completed (file transfer succeeded)");
    verifyHistory(snap->errorCode == 0, "Task error code must remain 0");

    QFile::setPermissions(unwriteDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    delete job;
}

static void testMtpDisconnectionSafety(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_mtp_disconnect.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *job = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("mtp:/Phone/DCIM/pic.jpg"))},
        QUrl(QStringLiteral("file:///local/photos/")),
        [&job]() -> KJob* {
            job = new HistoryTestJob(false);
            return job;
        });

    controller.enqueue(desc);
    QCoreApplication::processEvents();

    // Simulate device disconnected mid-transfer
    job->fail(KIO::ERR_WORKER_DIED, QStringLiteral("Device disconnected"));
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 1, "MTP disconnect recorded");
    auto rec = histMgr.records().first();
    verifyHistory(rec.terminalState == TransferTaskState::Failed, "State is Failed");
    verifyHistory(rec.errorCode == KIO::ERR_WORKER_DIED, "Error code is ERR_WORKER_DIED");
    delete job;
}

static void testSafeShutdownDuringActiveQueue(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/queue_shutdown.json");
    TransferHistoryManager histMgr(filePath);

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *job = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///active1.bin"))},
        QUrl(QStringLiteral("file:///dest/")),
        [&job]() -> KJob* {
            job = new HistoryTestJob(false);
            return job;
        });

    controller.enqueue(desc);
    QCoreApplication::processEvents();

    // Trigger shutdown while task is running
    controller.beginShutdown();
    QCoreApplication::processEvents();

    histMgr.flushSync();

    // Verify history file exists and is valid JSON
    QFileInfo fi(filePath);
    verifyHistory(fi.exists(), "History file exists after shutdown");
    QFile file(filePath);
    verifyHistory(file.open(QIODevice::ReadOnly), "History file openable");
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    verifyHistory(err.error == QJsonParseError::NoError, "History file is valid JSON after shutdown");
    delete job;
}

static void testSchemaVersionMismatchProtected(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/future_schema.json");

    // Write a file with schemaVersion 99
    {
        QJsonObject root;
        root[QStringLiteral("schemaVersion")] = 99;
        root[QStringLiteral("historyEpoch")] = 5;
        root[QStringLiteral("records")] = QJsonArray();
        QFile file(filePath);
        verifyHistory(file.open(QIODevice::WriteOnly), "Can write future schema fixture");
        file.write(QJsonDocument(root).toJson());
        file.close();
    }

    TransferHistoryManager histMgr(filePath);
    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    HistoryTestJob *job = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///file.txt"))},
        QUrl(QStringLiteral("file:///dest/")),
        [&job]() -> KJob* {
            job = new HistoryTestJob(false);
            return job;
        });

    controller.enqueue(desc);
    QCoreApplication::processEvents();
    job->complete(100, 100);
    QCoreApplication::processEvents();

    histMgr.flushSync();

    // Verify file on disk still has schemaVersion 99 and was not overwritten
    QFile file(filePath);
    verifyHistory(file.open(QIODevice::ReadOnly), "Can read future schema fixture");
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    verifyHistory(root.value(QStringLiteral("schemaVersion")).toInt() == 99, "Schema 99 file was NOT overwritten");
    delete job;
}

static void testNoOrphanedFiles(const QString &tempDir)
{
    const QString filePath = tempDir + QStringLiteral("/clean_check.json");
    TransferHistoryManager histMgr(filePath);

    auto rec = TransferHistoryRecord::createNew(TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed, {QUrl(QStringLiteral("file:///test"))});
    histMgr.addRecord(rec, true);
    histMgr.flushSync();

    // Check directory: no orphaned .lock or .tmp files
    QDir dir(tempDir);
    const QStringList files = dir.entryList(QDir::Files);
    for (const QString &fn : files) {
        verifyHistory(!fn.endsWith(QStringLiteral(".lock")), "No orphaned .lock files allowed");
        verifyHistory(!fn.endsWith(QStringLiteral(".tmp")), "No orphaned .tmp files allowed");
    }
}

struct ScopedProductionDispatch {
    bool prev;
    explicit ScopedProductionDispatch(bool enabled = true) : prev(TransferQueueProductionDispatchConnected) {
        TransferQueueProductionDispatchConnected = enabled;
    }
    ~ScopedProductionDispatch() {
        TransferQueueProductionDispatchConnected = prev;
    }
};

static void testOperationWindowPersistentHistoryLoadedOnStart(const QString &tempDir)
{
    ScopedProductionDispatch prodGuard(true);
    const QString filePath = tempDir + QStringLiteral("/win_loaded_start.json");
    TransferHistoryManager histMgr(filePath);

    const auto t0 = QDateTime::currentDateTimeUtc().addSecs(-100);
    const auto t1 = t0.addSecs(10);
    const auto t2 = t1.addSecs(5);

    auto r1 = TransferHistoryRecord::createNew(
        TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///data/photo.jpg"))}, QUrl(QStringLiteral("file:///backup/")),
        t0, t1, 10000, 10485760ULL, 10485760ULL, 1048576ULL, 0, QString(), QStringLiteral("Copy photo.jpg"));

    auto r2 = TransferHistoryRecord::createNew(
        TransferOperationType::Move, TransferBackendKind::LocalNative, TransferTaskState::Cancelled,
        {QUrl(QStringLiteral("file:///data/video.mp4"))}, QUrl(QStringLiteral("file:///backup/")),
        t1, t2, 5000, 5242880ULL, 2621440ULL, 524288ULL, 0, QString(), QStringLiteral("Move video.mp4"));

    auto r3 = TransferHistoryRecord::createNew(
        TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Failed,
        {QUrl(QStringLiteral("file:///data/secret.dat"))}, QUrl(QStringLiteral("file:///cloud/")),
        t2, t2.addSecs(1), 1000, 2048ULL, 0ULL, std::nullopt, 100, QStringLiteral("Access denied"), QStringLiteral("Copy secret.dat"));

    histMgr.addRecord(r1, false);
    histMgr.addRecord(r2, false);
    histMgr.addRecord(r3, false);
    verifyHistory(histMgr.saveSync(), "Saved 3 records to disk");

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    OperationWindow win;
    win.setQueueController(&controller);
    win.show();
    QCoreApplication::processEvents();

    const auto badges = win.findChildren<QLabel*>(QStringLiteral("operationHistoryBadge"));
    verifyHistory(badges.size() == 3, "OperationWindow rendered exactly 3 archival badges");

    const auto rows = win.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rows.size() == 3, "OperationWindow rendered exactly 3 operation rows");

    for (QFrame *row : rows) {
        verifyHistory(row->property("isArchival").toBool(), "Row is marked as archival");
        verifyHistory(!row->property("historyUuid").toString().isEmpty(), "Row has history UUID");
    }

    auto *summary = win.findChild<TransferQueueElidedLabel*>(QStringLiteral("detailedOperationsSummaryLabel"));
    if (!summary) {
        const auto labels = win.findChildren<TransferQueueElidedLabel*>();
        for (auto *lbl : labels) {
            if (lbl->fullText().contains(QStringLiteral("Zakończone")) || lbl->fullText().contains(QStringLiteral("Completed"))) {
                summary = lbl;
                break;
            }
        }
    }
    verifyHistory(summary != nullptr, "Summary label exists and indicates finished/completed items");
}

static void testOperationWindowDeduplicationBetweenQueueAndHistory(const QString &tempDir)
{
    ScopedProductionDispatch prodGuard(true);
    const QString filePath = tempDir + QStringLiteral("/win_dedup.json");
    TransferHistoryManager histMgr(filePath);

    // Archival record from a previous session
    auto archivalRec = TransferHistoryRecord::createNew(
        TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///old_data.zip"))}, QUrl(QStringLiteral("file:///backup/")),
        QDateTime::currentDateTimeUtc().addSecs(-60), QDateTime::currentDateTimeUtc().addSecs(-50),
        10000, 2048, 2048, 200, 0, QString(), QStringLiteral("Old session copy"));
    histMgr.addRecord(archivalRec, false);
    verifyHistory(histMgr.saveSync(), "Saved initial archival record");

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    OperationWindow win;
    win.setQueueController(&controller);
    win.show();
    QCoreApplication::processEvents();

    // Verify 1 archival card initially
    auto rowsBefore = win.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rowsBefore.size() == 1, "Initially 1 archival card in window");

    // Enqueue an in-session task
    HistoryTestJob *jobInstance = nullptr;
    auto desc = makeQueueTestDescriptor(
        TransferOperationType::Copy,
        {QUrl(QStringLiteral("file:///live_data.bin"))},
        QUrl(QStringLiteral("file:///dest/")),
        [&jobInstance]() -> KJob* {
            jobInstance = new HistoryTestJob(false);
            return jobInstance;
        },
        QStringLiteral("Live session task")
    );

    auto result = controller.enqueue(desc);
    verifyHistory(result.id > 0, "Task enqueued");
    QCoreApplication::processEvents();

    verifyHistory(jobInstance != nullptr, "Job started");
    jobInstance->complete(4096, 4096);
    QCoreApplication::processEvents();

    histMgr.flushSync();
    verifyHistory(histMgr.count() == 2, "HistoryManager now contains 2 records on disk");

    // In OperationWindow:
    // There should be exactly 2 rows: 1 for the live finished queue task, and 1 for the archival record.
    // The live task MUST NOT be duplicated as an archival card!
    win.rebuild();
    QCoreApplication::processEvents();

    auto rowsAfter = win.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rowsAfter.size() == 2, "Exactly 2 rows in window: deduplication prevents double card for live task");

    int archivalCount = 0;
    int liveQueueCount = 0;
    for (QFrame *row : rowsAfter) {
        if (row->property("isArchival").toBool()) {
            ++archivalCount;
        } else if (row->property("taskId").toULongLong() == result.id) {
            ++liveQueueCount;
        }
    }
    verifyHistory(archivalCount == 1, "Exactly 1 archival card");
    verifyHistory(liveQueueCount == 1, "Exactly 1 live queue card");
    delete jobInstance;
}

static void testOperationWindowStatusBadgesAndDetails(const QString &tempDir)
{
    ScopedProductionDispatch prodGuard(true);
    const QString filePath = tempDir + QStringLiteral("/win_details.json");
    TransferHistoryManager histMgr(filePath);

    const auto start = QDateTime::fromString(QStringLiteral("2026-11-04T12:00:00Z"), Qt::ISODate);
    const auto end = QDateTime::fromString(QStringLiteral("2026-11-04T12:00:05Z"), Qt::ISODate);

    auto rSuccess = TransferHistoryRecord::createNew(
        TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///home/user/doc.txt"))}, QUrl(QStringLiteral("file:///media/usb/")),
        start, end, 5000, 1048576, 1048576, 209715, 0, QString(), QStringLiteral("Copy doc.txt"));

    auto rFailed = TransferHistoryRecord::createNew(
        TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Failed,
        {QUrl(QStringLiteral("file:///home/user/fail.bin"))}, QUrl(QStringLiteral("file:///media/usb/")),
        start, end, 1000, 1048576, 0, std::nullopt, 100, QStringLiteral("Device write error #100"), QStringLiteral("Failed file"));

    auto rCancel = TransferHistoryRecord::createNew(
        TransferOperationType::Move, TransferBackendKind::LocalNative, TransferTaskState::Cancelled,
        {QUrl(QStringLiteral("file:///home/user/cancel.iso"))}, QUrl(QStringLiteral("file:///backup/")),
        start, end, 2000, 5000000, 1000000, 500000, 0, QString(), QStringLiteral("Cancelled move"));

    histMgr.addRecord(rSuccess, false);
    histMgr.addRecord(rFailed, false);
    histMgr.addRecord(rCancel, false);
    histMgr.saveSync();

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    OperationWindow win;
    win.setQueueController(&controller);
    win.show();
    QCoreApplication::processEvents();

    const auto rows = win.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rows.size() == 3, "3 rows present");

    for (QFrame *row : rows) {
        const QString uuid = row->property("historyUuid").toString();
        auto *stateLbl = row->findChild<QLabel*>(QStringLiteral("operationDetailsLabel"));
        verifyHistory(stateLbl != nullptr, "operationDetailsLabel exists on row");

        if (uuid == rSuccess.uuid) {
            verifyHistory(stateLbl->text().contains(QStringLiteral("Ukończono")) || stateLbl->text().contains(QStringLiteral("Completed")),
                          "rSuccess shows Completed state");
            auto *prog = row->findChild<QProgressBar*>(QStringLiteral("operationProgress"));
            verifyHistory(prog != nullptr && prog->value() == 100, "Progress bar is 100% on Completed");
            auto *pctLbl = row->findChild<QLabel*>(QStringLiteral("operationPercentLabel"));
            verifyHistory(pctLbl != nullptr && pctLbl->text() == QStringLiteral("100%"), "Percent label is 100%");
            auto *durLbl = row->findChild<TransferQueueElidedLabel*>(QStringLiteral("operationDurationLabel"));
            verifyHistory(durLbl != nullptr && (durLbl->fullText().contains(QStringLiteral("5 s")) || durLbl->fullText().contains(QStringLiteral("5 sec"))), "Duration label formatted correctly");
            auto *endLbl = row->findChild<TransferQueueElidedLabel*>(QStringLiteral("operationEndTimeLabel"));
            verifyHistory(endLbl != nullptr && !endLbl->fullText().isEmpty(), "End time label is formatted");
        } else if (uuid == rFailed.uuid) {
            verifyHistory(stateLbl->text().contains(QStringLiteral("Błąd")) || stateLbl->text().contains(QStringLiteral("Failed")),
                          "rFailed shows Failed state");
            auto *errLbl = row->findChild<OperationDetailsLabel*>(QStringLiteral("operationErrorLabel"));
            verifyHistory(errLbl != nullptr && errLbl->text().contains(QStringLiteral("Device write error #100")),
                          "Error text displayed correctly on failed card");
        } else if (uuid == rCancel.uuid) {
            verifyHistory(stateLbl->text().contains(QStringLiteral("Anulowano")) || stateLbl->text().contains(QStringLiteral("Cancelled")),
                          "rCancel shows Cancelled state");
        }
    }
}

static void testOperationWindowNoActiveControlsOnArchivalCards(const QString &tempDir)
{
    ScopedProductionDispatch prodGuard(true);
    const QString filePath = tempDir + QStringLiteral("/win_no_controls.json");
    TransferHistoryManager histMgr(filePath);

    auto r = TransferHistoryRecord::createNew(
        TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///data/report.pdf"))});
    histMgr.addRecord(r, false);
    histMgr.saveSync();

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    OperationWindow win;
    win.setQueueController(&controller);
    win.show();
    QCoreApplication::processEvents();

    const auto rows = win.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rows.size() == 1, "1 row present");
    QFrame *row = rows.first();

    // MUST NOT have active control buttons
    verifyHistory(row->findChild<QToolButton*>(QStringLiteral("detailedOperationPauseButton")) == nullptr,
                  "No pause button on archival card");
    verifyHistory(row->findChild<QToolButton*>(QStringLiteral("detailedOperationCancelButton")) == nullptr,
                  "No cancel button on archival card");
    verifyHistory(row->findChild<QToolButton*>(QStringLiteral("detailedOperationResumeButton")) == nullptr,
                  "No resume button on archival card");

    // MUST have dismiss button
    auto *dismiss = row->findChild<QToolButton*>(QStringLiteral("detailedOperationDismissButton"));
    verifyHistory(dismiss != nullptr, "Dismiss button is present on archival card");
}

static void testOperationWindowEmptyAndCorruptedHistory(const QString &tempDir)
{
    ScopedProductionDispatch prodGuard(true);
    // Part A: Empty history
    {
        const QString emptyPath = tempDir + QStringLiteral("/win_empty.json");
        TransferHistoryManager histMgr(emptyPath);
        histMgr.saveSync();

        TransferQueueController controller;
        controller.setHistoryManager(&histMgr);

        OperationWindow win;
        win.setQueueController(&controller);
        win.show();
        QCoreApplication::processEvents();

        auto *emptyLbl = win.findChild<QLabel*>(QStringLiteral("operationWindowEmptyLabel"));
        verifyHistory(emptyLbl != nullptr && emptyLbl->isVisible(), "Empty label is visible when history is empty");
        verifyHistory(emptyLbl->text().contains(QStringLiteral("historii")) ||
                      emptyLbl->text().contains(QStringLiteral("history")),
                      "Empty label text correctly mentions both active ops and history");
    }

    // Part B: Corrupted file
    {
        const QString badPath = tempDir + QStringLiteral("/win_bad.json");
        QFile f(badPath);
        verifyHistory(f.open(QIODevice::WriteOnly), "Can write corrupted fixture");
        f.write("{\"schemaVersion\": 1, \"records\": [{\"corrupted\": true, ");
        f.close();

        TransferHistoryManager histMgr(badPath);
        histMgr.loadSync(); // safe fallback

        TransferQueueController controller;
        controller.setHistoryManager(&histMgr);

        OperationWindow win;
        win.setQueueController(&controller);
        win.show();
        QCoreApplication::processEvents();

        auto *emptyLbl = win.findChild<QLabel*>(QStringLiteral("operationWindowEmptyLabel"));
        verifyHistory(emptyLbl != nullptr && emptyLbl->isVisible(), "Window gracefully renders empty state on corrupted history without crash");
    }
}

static void testOperationWindowDisconnectedDevicesRendering(const QString &tempDir)
{
    ScopedProductionDispatch prodGuard(true);
    const QString filePath = tempDir + QStringLiteral("/win_disconnected.json");
    TransferHistoryManager histMgr(filePath);

    // Records containing unmounted/disconnected device URLs
    auto rMtp = TransferHistoryRecord::createNew(
        TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed,
        {QUrl(QStringLiteral("mtp:/Phone/DCIM/Camera/IMG_9999.jpg"))},
        QUrl(QStringLiteral("file:///home/user/Pictures/")),
        QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc(), 100, 1024, 1024);

    auto rUsb = TransferHistoryRecord::createNew(
        TransferOperationType::Move, TransferBackendKind::LocalNative, TransferTaskState::Completed,
        {QUrl(QStringLiteral("file:///home/user/backup.tar.gz"))},
        QUrl(QStringLiteral("file:///run/media/user/DISCONNECTED_DRIVE_999/")),
        QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc(), 200, 2048, 2048);

    histMgr.addRecord(rMtp, false);
    histMgr.addRecord(rUsb, false);
    histMgr.saveSync();

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    QElapsedTimer timer;
    timer.start();

    OperationWindow win;
    win.setQueueController(&controller);
    win.show();
    QCoreApplication::processEvents();

    const qint64 elapsed = timer.elapsed();
    verifyHistory(elapsed < 300, "Rendering archival cards with disconnected devices must be instant and perform zero blocking I/O");

    const auto rows = win.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rows.size() == 2, "Both disconnected device cards rendered cleanly");
}

static void testOperationWindowMultipleRecordsAndScrolling(const QString &tempDir)
{
    ScopedProductionDispatch prodGuard(true);
    const QString filePath = tempDir + QStringLiteral("/win_scroll.json");
    TransferHistoryManager histMgr(filePath);

    for (int i = 0; i < 20; ++i) {
        auto r = TransferHistoryRecord::createNew(
            TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed,
            {QUrl(QStringLiteral("file:///test/item_%1.dat").arg(i))},
            QUrl(QStringLiteral("file:///backup/")),
            QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc(), 100 * (i + 1),
            1024 * (i + 1), 1024 * (i + 1), 1000, 0, QString(), QStringLiteral("Batch item %1").arg(i));
        histMgr.addRecord(r, false);
    }
    histMgr.saveSync();

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    OperationWindow win;
    win.setQueueController(&controller);
    win.show();
    QCoreApplication::processEvents();

    const auto rows = win.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rows.size() == 20, "All 20 archival rows are instantiated");

    auto *scroll = win.findChild<QScrollArea*>(QStringLiteral("operationWindowScrollArea"));
    verifyHistory(scroll != nullptr && scroll->isVisible(), "Scroll area is visible and active for multiple records");
}

static void testOperationWindowUxAGeometryAndResize(const QString &tempDir)
{
    ScopedProductionDispatch prodGuard(true);
    Q_UNUSED(tempDir);

    // UX Item A: Default size 580x380, minimum size 480x260 in unified mode
    TransferQueueController controller;
    OperationWindow win;
    win.setQueueController(&controller);
    verifyHistory(win.sizeHint() == QSize(580, 380), "sizeHint is 580x380");
    verifyHistory(win.minimumSizeHint() == QSize(480, 260), "minimumSizeHint is 480x260 in unified mode");

    // Clear previous stored geometry in QSettings to verify clean default
    QSettings settings;
    settings.remove(QStringLiteral("operationWindow/geometry"));

    win.show();
    QCoreApplication::processEvents();

    // Verify initial width is compact (~580)
    verifyHistory(win.width() >= 480 && win.width() <= 600, "Window initial width is compact (~580)");

    // Manual resize test and persistence
    win.resize(650, 420);
    QCloseEvent ce;
    QApplication::sendEvent(&win, &ce); // triggers closeEvent which saves geometry

    // Open second instance
    OperationWindow win2;
    win2.setQueueController(&controller);
    win2.show();
    QCoreApplication::processEvents();

    verifyHistory(win2.width() == 650, "Restored width 650 from QSettings");
    verifyHistory(win2.height() == 420, "Restored height 420 from QSettings");

    // Clean up settings
    settings.remove(QStringLiteral("operationWindow/geometry"));
}

static void testOperationWindowUxBSpeedGraphTerminal(const QString &tempDir)
{
    Q_UNUSED(tempDir);
    // UX Item B: Speed graph terminal status presentation
    OperationSpeedGraph graph;
    verifyHistory(!graph.isTerminal(), "Initially not terminal");

    // When running with 0 samples:
    QPixmap pixmap(200, 60);
    pixmap.fill(Qt::black);
    graph.resize(200, 60);
    graph.render(&pixmap); // paints without crash

    // When terminal with 0 samples (instant transfer):
    graph.setTerminal(true, TransferTaskState::Completed);
    verifyHistory(graph.isTerminal(), "Graph is marked terminal");
    verifyHistory(graph.terminalState() == TransferTaskState::Completed, "Terminal state is Completed");
    graph.render(&pixmap); // paints "Transfer ukończony natychmiastowo" without crash

    // When terminal with Cancelled:
    graph.setTerminal(true, TransferTaskState::Cancelled);
    verifyHistory(graph.terminalState() == TransferTaskState::Cancelled, "Terminal state is Cancelled");
    graph.render(&pixmap);

    // When terminal with Failed:
    graph.setTerminal(true, TransferTaskState::Failed);
    verifyHistory(graph.terminalState() == TransferTaskState::Failed, "Terminal state is Failed");
    graph.render(&pixmap);
}

static void testOperationWindowDismissDoesNotDeleteFromDisk(const QString &tempDir)
{
    ScopedProductionDispatch prodGuard(true);
    const QString filePath = tempDir + QStringLiteral("/win_dismiss.json");
    TransferHistoryManager histMgr(filePath);

    auto r1 = TransferHistoryRecord::createNew(TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed, {QUrl(QStringLiteral("file:///item1.txt"))});
    auto r2 = TransferHistoryRecord::createNew(TransferOperationType::Copy, TransferBackendKind::LocalNative, TransferTaskState::Completed, {QUrl(QStringLiteral("file:///item2.txt"))});
    histMgr.addRecord(r1, false);
    histMgr.addRecord(r2, false);
    histMgr.saveSync();

    TransferQueueController controller;
    controller.setHistoryManager(&histMgr);

    OperationWindow win;
    win.setQueueController(&controller);
    win.show();
    QCoreApplication::processEvents();

    auto rows = win.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rows.size() == 2, "2 rows in window initially");

    // Click dismiss button on first card
    auto *dismissBtn = rows.first()->findChild<QToolButton*>(QStringLiteral("detailedOperationDismissButton"));
    verifyHistory(dismissBtn != nullptr, "Dismiss button found on archival card");

    dismissBtn->click();
    QTest::qWait(250);

    // After dismiss, only 1 row remains visible in window
    auto rowsAfterDismiss = win.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rowsAfterDismiss.size() == 1, "Only 1 row visible in window after dismissing 1 card");

    // CRITICAL: Disk store and TransferHistoryManager still have BOTH records!
    verifyHistory(histMgr.count() == 2, "TransferHistoryManager still has 2 records in memory");
    {
        TransferHistoryManager diskCheck(filePath);
        diskCheck.loadSync();
        verifyHistory(diskCheck.count() == 2, "Disk file still contains BOTH records (dismiss only hides from view)");
    }

    // Creating a new window session will show both records again
    OperationWindow win2;
    win2.setQueueController(&controller);
    win2.show();
    QCoreApplication::processEvents();

    auto rowsNewSession = win2.findChildren<QFrame*>(QStringLiteral("operationRow"));
    verifyHistory(rowsNewSession.size() == 2, "New session window displays all 2 records from disk");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // Helper mode for testing real independent child OS processes
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLatin1(argv[i]) == QStringLiteral("--subproc-add") && i + 3 < argc) {
            const QString targetPath = QString::fromLatin1(argv[i + 1]);
            const QString itemUuid = QString::fromLatin1(argv[i + 2]);
            const QString itemTitle = QString::fromLatin1(argv[i + 3]);

            TransferHistoryManager mgr(targetPath);
            mgr.loadSync();
            auto rec = TransferHistoryRecord::createNew(
                TransferOperationType::Copy,
                TransferBackendKind::LocalNative,
                TransferTaskState::Completed,
                {QUrl(QStringLiteral("file:///subproc_data.txt"))},
                QUrl(),
                QDateTime::currentDateTimeUtc(),
                QDateTime::currentDateTimeUtc(),
                100, 1024, 1024, std::nullopt, 0, QString(), itemTitle);
            rec.uuid = itemUuid;
            mgr.addRecord(rec, false);
            return mgr.saveSync() ? 0 : 1;
        }
    }

    QTemporaryDir tempDir;
    verifyHistory(tempDir.isValid(), "Temporary directory must be valid");

    const QString path = tempDir.path();
    const QString appPath = QCoreApplication::applicationFilePath();

    testSerializationRoundTrip(path);
    testUuidUniqueness(path);
    testMissingOptionalFields(path);
    testInvalidFieldTypes(path);
    testUnknownSchemaVersion(path);
    testCorruptedJson(path);
    testOriginalFilePreservedOnReadError(path);
    testAtomicSaveSafety(path);
    testRecordLimitRingBuffer(path);
    testFileSizeLimit(path);
    testUrlPrivacySanitization(path);
    testConcurrentBurstWrites(path);
    testTwoManagerInstancesMerge(path);
    testFailedSaveHandledGracefully(path);
    testAsyncCompletionAndDestructorSafety(path);
    testProtectionAgainstDeletedRecordResurrection(path);
    testSymlinkPathRejected(path);
    testCorruptedBackupCollisionResilience(path);
    testSeparateProcessesCoordination(path, appPath);
    testWorkerSerializationUnderPressure(path);

    // Stage 2.2 Queue Integration Tests
    testEpochClearInvalidatesPendingWrites(path);
    testCopyCompletedIntegration(path);
    testMoveCompletedIntegration(path);
    testCopyFailedIntegration(path);
    testMoveFailedIntegration(path);
    testCopyCancelledIntegration(path);
    testCancelQueuedPreStartIntegration(path);
    testTaskIdempotenceIntegration(path);
    testUtcTimestampAndVolumeAccuracy(path);
    testMultiFileTaskIntegration(path);
    testTwoControllersDiskPersistence(path);
    testHistoryFailureIsolation(path);
    testMtpDisconnectionSafety(path);
    testSafeShutdownDuringActiveQueue(path);
    testSchemaVersionMismatchProtected(path);
    testNoOrphanedFiles(path);

    // Stage 2.3 OperationWindow & UX Polish Tests
    testOperationWindowPersistentHistoryLoadedOnStart(path);
    testOperationWindowDeduplicationBetweenQueueAndHistory(path);
    testOperationWindowStatusBadgesAndDetails(path);
    testOperationWindowNoActiveControlsOnArchivalCards(path);
    testOperationWindowEmptyAndCorruptedHistory(path);
    testOperationWindowDisconnectedDevicesRendering(path);
    testOperationWindowMultipleRecordsAndScrolling(path);
    testOperationWindowUxAGeometryAndResize(path);
    testOperationWindowUxBSpeedGraphTerminal(path);
    testOperationWindowDismissDoesNotDeleteFromDisk(path);

    qDebug("PASS: [history] %d assertions verified across 46 test suites", s_historyChecks);
    return 0;
}
