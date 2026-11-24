/*
 * Headless smoke & integration test for the portable Qt6/Linux `thispc-core`
 * static library (0.46.0 Stage 1B / R1).
 *
 * Strictly uses only Qt6::Core + Qt6::Concurrent + C++20/POSIX APIs via
 * `thispc-core`. Zero KF6, KIO, Solid, ECM, GIO, QtGui, QtWidgets, or QtPdf.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "applicationlifecycle.h"
#include "applicationstyle.h"
#include "checksumjob.h"
#include "duplicatefinderjob.h"
#include "portablepaths.h"
#include "remoteurlhelper.h"
#include "savedremotelocation.h"
#include "storageanalysisaccumulator.h"
#include "storagescanjob.h"
#include "storagetreemaplayout.h"
#include "transferbandwidthlimiter.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QRectF>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {

int g_failures = 0;

void check(bool condition, const char *expr, int line)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL [line %d]: %s\n", line, expr);
        ++g_failures;
    }
}

#define CORE_CHECK(expr) check((expr), #expr, __LINE__)

struct ScopedEnvVar {
    QByteArray name;
    QByteArray original;
    bool hadValue = false;

    explicit ScopedEnvVar(const char *envName)
        : name(envName)
        , original(qgetenv(envName))
        , hadValue(qEnvironmentVariableIsSet(envName))
    {
    }

    ~ScopedEnvVar()
    {
        if (hadValue) {
            qputenv(name.constData(), original);
        } else {
            qunsetenv(name.constData());
        }
    }
};

QString tempBaseDir()
{
    const QByteArray overrideDir = qgetenv("THISPC_TEST_WORK_DIR");
    if (!overrideDir.isEmpty()) {
        const QString dirPath = QFile::decodeName(overrideDir);
        if (QDir(dirPath).exists() || QDir().mkpath(dirPath)) {
            return dirPath;
        }
    }
    return QDir::tempPath();
}

void testPortablePaths()
{
    ScopedEnvVar savedXdg("XDG_STATE_HOME");
    ScopedEnvVar savedHome("HOME");
    const QString origOrg = QCoreApplication::organizationName();
    const QString origApp = QCoreApplication::applicationName();
    const bool origTestMode = QStandardPaths::isTestModeEnabled();

    auto restoreMeta = [&]() {
        QCoreApplication::setOrganizationName(origOrg);
        QCoreApplication::setApplicationName(origApp);
        QStandardPaths::setTestModeEnabled(origTestMode);
    };

    QTemporaryDir tmpDir(tempBaseDir() + QStringLiteral("/thispc-paths-XXXXXX"));
    CORE_CHECK(tmpDir.isValid());

    const QString fakeHome = tmpDir.path() + QStringLiteral("/fake-home");
    CORE_CHECK(QDir().mkpath(fakeHome));
    qputenv("HOME", fakeHome.toUtf8());
    QStandardPaths::setTestModeEnabled(false);

    // 1. Normal ThisPC production metadata + explicit absolute XDG_STATE_HOME
    QCoreApplication::setOrganizationName(QStringLiteral("kio-thispc"));
    QCoreApplication::setApplicationName(QStringLiteral("thispc-view"));
    const QString customState = tmpDir.path() + QStringLiteral("/xdg-state");
    qputenv("XDG_STATE_HOME", customState.toUtf8());

    CORE_CHECK(ThisPc::PortablePaths::genericStateLocation() == QDir::cleanPath(customState));
    CORE_CHECK(ThisPc::PortablePaths::appStateLocation()
               == QDir::cleanPath(customState + QStringLiteral("/kio-thispc/thispc-view")));

    // Expected production history path contract
    const QString expectedHistoryFile =
        ThisPc::PortablePaths::appStateLocation(QStringLiteral("kio-thispc/thispc-view"))
        + QStringLiteral("/operations_history.json");
    CORE_CHECK(expectedHistoryFile
               == QDir::cleanPath(customState + QStringLiteral("/kio-thispc/thispc-view/operations_history.json")));

    // 2. Relative XDG_STATE_HOME must be rejected -> $HOME/.local/state
    qputenv("XDG_STATE_HOME", "relative/state/path");
    CORE_CHECK(ThisPc::PortablePaths::genericStateLocation()
               == QDir::cleanPath(fakeHome + QStringLiteral("/.local/state")));
    CORE_CHECK(ThisPc::PortablePaths::appStateLocation()
               == QDir::cleanPath(fakeHome + QStringLiteral("/.local/state/kio-thispc/thispc-view")));

    // 3. Leading whitespace in XDG_STATE_HOME must NOT be trimmed into an absolute path
    const QByteArray spacedPath = QByteArray(" ") + customState.toUtf8();
    qputenv("XDG_STATE_HOME", spacedPath);
    CORE_CHECK(ThisPc::PortablePaths::genericStateLocation()
               == QDir::cleanPath(fakeHome + QStringLiteral("/.local/state")));

    // 4. Empty / unset XDG_STATE_HOME -> $HOME/.local/state
    qunsetenv("XDG_STATE_HOME");
    CORE_CHECK(ThisPc::PortablePaths::genericStateLocation()
               == QDir::cleanPath(fakeHome + QStringLiteral("/.local/state")));
    CORE_CHECK(ThisPc::PortablePaths::appStateLocation()
               == QDir::cleanPath(fakeHome + QStringLiteral("/.local/state/kio-thispc/thispc-view")));

    qputenv("XDG_STATE_HOME", "");
    CORE_CHECK(ThisPc::PortablePaths::genericStateLocation()
               == QDir::cleanPath(fakeHome + QStringLiteral("/.local/state")));

    // 5. QStandardPaths::isTestModeEnabled() -> $HOME/.qttest/state (ignores XDG_STATE_HOME)
    qputenv("XDG_STATE_HOME", customState.toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    CORE_CHECK(ThisPc::PortablePaths::genericStateLocation()
               == QDir::cleanPath(fakeHome + QStringLiteral("/.qttest/state")));
    CORE_CHECK(ThisPc::PortablePaths::appStateLocation()
               == QDir::cleanPath(fakeHome + QStringLiteral("/.qttest/state/kio-thispc/thispc-view")));
    QStandardPaths::setTestModeEnabled(false);

    // 6. Empty metadata + valid fallbackSubdir -> identical on Qt 6.4.2 and Qt 6.7+
    CORE_CHECK(ThisPc::PortablePaths::resolveAppStateLocation(
                   customState, QString(), QString(), QStringLiteral("kio-thispc/thispc-view"))
               == QDir::cleanPath(customState + QStringLiteral("/kio-thispc/thispc-view")));

    // 7. Malformed / absolute / traversal fallbackSubdir and metadata must be rejected (stay inside customState)
    CORE_CHECK(ThisPc::PortablePaths::resolveAppStateLocation(
                   customState, QString(), QString(), QStringLiteral("/etc/passwd"))
               == QDir::cleanPath(customState));
    CORE_CHECK(ThisPc::PortablePaths::resolveAppStateLocation(
                   customState, QString(), QString(), QStringLiteral("../../escape"))
               == QDir::cleanPath(customState));
    CORE_CHECK(ThisPc::PortablePaths::resolveAppStateLocation(
                   customState, QString(), QString(), QStringLiteral("kio-thispc/../escape"))
               == QDir::cleanPath(customState));
    CORE_CHECK(ThisPc::PortablePaths::resolveAppStateLocation(
                   customState, QString(), QString(), QStringLiteral("a\\b"))
               == QDir::cleanPath(customState));
    CORE_CHECK(ThisPc::PortablePaths::resolveAppStateLocation(
                   customState, QStringLiteral(".."), QStringLiteral("../etc"), QStringLiteral("kio-thispc/thispc-view"))
               == QDir::cleanPath(customState + QStringLiteral("/kio-thispc/thispc-view")));

    // 8. Missing / invalid HOME with unset XDG_STATE_HOME must never resolve to "/.local/state"
    qunsetenv("XDG_STATE_HOME");
    qputenv("HOME", "");
    const QString missingHomeGeneric = ThisPc::PortablePaths::genericStateLocation();
    CORE_CHECK(missingHomeGeneric != QStringLiteral("/.local/state"));
    CORE_CHECK(missingHomeGeneric != QStringLiteral("/"));

    qputenv("HOME", "/");
    const QString rootHomeGeneric = ThisPc::PortablePaths::genericStateLocation();
    CORE_CHECK(rootHomeGeneric != QStringLiteral("/.local/state"));
    CORE_CHECK(rootHomeGeneric != QStringLiteral("/"));

    // Restore fakeHome for remaining location checks
    qputenv("HOME", fakeHome.toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("kio-thispc"));
    QCoreApplication::setApplicationName(QStringLiteral("thispc-core-smoke"));
    CORE_CHECK(!ThisPc::PortablePaths::appDataLocation().isEmpty());
    CORE_CHECK(!ThisPc::PortablePaths::appCacheLocation().isEmpty());
    CORE_CHECK(!ThisPc::PortablePaths::appConfigLocation().isEmpty());

    restoreMeta();
}

void testRemoteUrlAndSavedLocations(const QString &isolatedConfigDir)
{
    const QUrl sftpWithCreds(QStringLiteral("sftp://alice:secret@example.org:2222/srv/data/"));
    CORE_CHECK(RemoteUrlHelper::isRemoteUrl(sftpWithCreds));
    CORE_CHECK(!RemoteUrlHelper::isRemoteUrl(QUrl::fromLocalFile(QStringLiteral("/tmp"))));

    const QUrl sanitized = RemoteUrlHelper::sanitizeUrl(sftpWithCreds);
    CORE_CHECK(sanitized.password().isEmpty());
    CORE_CHECK(sanitized.host() == QStringLiteral("example.org"));
    CORE_CHECK(sanitized.port() == 2222);

    const QString id = SavedRemoteLocationsStore::generateId(sanitized);
    CORE_CHECK(!id.isEmpty());
    CORE_CHECK(!SavedRemoteLocationsStore::defaultDisplayName(sanitized).isEmpty());

    // Record real-home config mtime/existence BEFORE running SavedRemoteLocationsStore
    // to prove zero writes occur outside isolatedConfigDir (AUD-1B-01).
    const QString realHome = QFile::decodeName(qgetenv("HOME"));
    const QString realSmokeConf = realHome + QStringLiteral("/.config/kio-thispc/thispc-core-smoke.conf");
    const QFileInfo realBefore(realSmokeConf);
    const bool existedBefore = realBefore.exists();
    const qint64 mtimeBefore = existedBefore ? realBefore.lastModified().toMSecsSinceEpoch() : -1;
    const qint64 sizeBefore = existedBefore ? realBefore.size() : -1;

    QTemporaryDir tmpDir(tempBaseDir() + QStringLiteral("/thispc-remote-XXXXXX"));
    CORE_CHECK(tmpDir.isValid());
    const QString iniPath = tmpDir.path() + QStringLiteral("/saved-remotes.ini");

    SavedRemoteLocationsStore store;
    QString addedId;
    CORE_CHECK(store.addLocation(QStringLiteral("Test SFTP"), sftpWithCreds, &addedId));
    CORE_CHECK(!addedId.isEmpty());
    CORE_CHECK(store.count() == 1);
    CORE_CHECK(store.locations().first().url.password().isEmpty());

    // Verify default QSettings wrote ONLY inside isolatedConfigDir
    {
        QSettings defaultSettings;
        defaultSettings.sync();
        const QString actualFile = QDir::cleanPath(defaultSettings.fileName());
        CORE_CHECK(actualFile.startsWith(QDir::cleanPath(isolatedConfigDir) + QLatin1Char('/')));
        CORE_CHECK(QFileInfo::exists(actualFile));
    }

    {
        QSettings settings(iniPath, QSettings::IniFormat);
        store.save(settings);
        settings.sync();
    }

    SavedRemoteLocationsStore reloaded;
    {
        QSettings settings(iniPath, QSettings::IniFormat);
        reloaded.load(settings);
    }
    CORE_CHECK(reloaded.count() == 1);
    CORE_CHECK(reloaded.locations().first().url.host() == QStringLiteral("example.org"));
    CORE_CHECK(reloaded.locations().first().url.password().isEmpty());
    CORE_CHECK(reloaded.removeLocation(addedId));
    CORE_CHECK(reloaded.count() == 0);

    // Regression check: real ~/.config/kio-thispc/thispc-core-smoke.conf must NOT be created or modified
    const QFileInfo realAfter(realSmokeConf);
    CORE_CHECK(realAfter.exists() == existedBefore);
    if (existedBefore) {
        CORE_CHECK(realAfter.lastModified().toMSecsSinceEpoch() == mtimeBefore);
        CORE_CHECK(realAfter.size() == sizeBefore);
    }
}

void testApplicationStyle()
{
    const QString style = applicationStyleSheet();
    CORE_CHECK(!style.isEmpty());
    CORE_CHECK(style.contains(QStringLiteral("QMainWindow")) || style.contains(QStringLiteral("QWidget")));
}

void testLifecycleAndWorkerPools()
{
    ApplicationWorkerPools pools;
    ShutdownCoordinator coord;
    coord.setWorkerPools(&pools);

    // 1. Basic completed metadata task
    std::atomic<bool> taskRan{false};
    QFuture<int> future = pools.runMetadataTask([&taskRan]() -> int {
        taskRan.store(true, std::memory_order_release);
        return 42;
    });
    future.waitForFinished();
    CORE_CHECK(taskRan.load(std::memory_order_acquire));
    CORE_CHECK(future.result() == 42);

    // 2. Active-work shutdown draining scenario with deterministic barrier (AUD-1B-08)
    std::mutex barrierMutex;
    std::condition_variable barrierCv;
    bool workerStarted = false;
    bool releaseWorker = false;

    QFuture<int> activeFuture = pools.runXattrWrite([&]() -> int {
        {
            std::lock_guard<std::mutex> lock(barrierMutex);
            workerStarted = true;
        }
        barrierCv.notify_all();

        std::unique_lock<std::mutex> lock(barrierMutex);
        barrierCv.wait_for(lock, std::chrono::seconds(5), [&]() { return releaseWorker; });
        return 7;
    });

    // Wait deterministically for worker to enter active execution
    {
        std::unique_lock<std::mutex> lock(barrierMutex);
        const bool startedOk = barrierCv.wait_for(lock, std::chrono::seconds(5), [&]() {
            return workerStarted;
        });
        CORE_CHECK(startedOk);
    }

    bool shutdownCompleted = false;
    QObject::connect(&coord, &ShutdownCoordinator::shutdownCompleted, &coord, [&shutdownCompleted]() {
        shutdownCompleted = true;
    });

    // Initiate shutdown WHILE worker task is actively running
    const bool started = coord.beginShutdown(true);
    CORE_CHECK(started);
    CORE_CHECK(coord.isDraining());
    CORE_CHECK(coord.isEventLoopLocked());
    CORE_CHECK(!shutdownCompleted);

    // Release worker barrier and wait for asynchronous completion via QEventLoop
    {
        std::lock_guard<std::mutex> lock(barrierMutex);
        releaseWorker = true;
    }
    barrierCv.notify_all();

    if (!shutdownCompleted) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&coord, &ShutdownCoordinator::shutdownCompleted, &loop, &QEventLoop::quit);
        timeout.start(5000);
        loop.exec();
    }

    activeFuture.waitForFinished();
    CORE_CHECK(activeFuture.isFinished());
    CORE_CHECK(activeFuture.result() == 7);
    CORE_CHECK(shutdownCompleted);
    CORE_CHECK(coord.isCompleted());
    CORE_CHECK(!coord.isEventLoopLocked());
    coord.resetToIdle();
    CORE_CHECK(coord.isIdle());
}

void testChecksumStorageAndDuplicates()
{
    QTemporaryDir tmpDir(tempBaseDir() + QStringLiteral("/thispc-storage-XXXXXX"));
    CORE_CHECK(tmpDir.isValid());

    const QString subDir = tmpDir.path() + QStringLiteral("/nested");
    CORE_CHECK(QDir().mkpath(subDir));

    auto writeFile = [](const QString &path, const QByteArray &data) -> bool {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        return f.write(data) == data.size();
    };

    const QByteArray payloadA = "thispc-portable-core-payload-alpha-0123456789";
    const QByteArray payloadB = "thispc-portable-core-unique-beta-9876543210-extra";
    const QString fileA1 = tmpDir.path() + QStringLiteral("/alpha1.txt");
    const QString fileA2 = subDir + QStringLiteral("/alpha2.txt");
    const QString fileB = subDir + QStringLiteral("/beta.bin");
    CORE_CHECK(writeFile(fileA1, payloadA));
    CORE_CHECK(writeFile(fileA2, payloadA));
    CORE_CHECK(writeFile(fileB, payloadB));

    // 1. ChecksumJob (SHA-256)
    {
        ChecksumJobOptions opts;
        opts.algorithm = ChecksumAlgorithm::Sha256;
        ChecksumJob job(QUrl::fromLocalFile(fileA1), nullptr, opts);
        ChecksumData checksumResult;
        bool gotResult = false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&job, &ChecksumJob::stateChanged, &loop, [&](const ChecksumData &res) {
            checksumResult = res;
            if (res.state == ChecksumState::Completed || res.state == ChecksumState::Failed
                || res.state == ChecksumState::Cancelled || res.state == ChecksumState::ChangedDuringHash) {
                gotResult = true;
                loop.quit();
            }
        });
        CORE_CHECK(job.start());
        timeout.start(5000);
        loop.exec();

        const QString expectedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(payloadA, QCryptographicHash::Sha256).toHex());
        CORE_CHECK(gotResult);
        CORE_CHECK(checksumResult.state == ChecksumState::Completed);
        CORE_CHECK(checksumResult.digest == expectedSha256);
    }

    // 2. StorageScanJob -> StorageAnalysisAccumulator -> StorageTreemapLayout -> DuplicateFinderJob
    StorageScanState scanState = StorageScanState::Idle;
    StorageScanStats scanStats;
    QList<StorageScanEntry> scanEntries;
    {
        StorageScanOptions opts;
        opts.rootPath = tmpDir.path();
        StorageScanJob scanJob(opts);
        bool gotScan = false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&scanJob, &StorageScanJob::finished, &loop,
                         [&](StorageScanState st, const StorageScanStats &stats) {
            scanState = st;
            scanStats = stats;
            scanEntries = scanJob.entries();
            gotScan = true;
            loop.quit();
        });
        CORE_CHECK(scanJob.start());
        timeout.start(5000);
        loop.exec();

        CORE_CHECK(gotScan);
        CORE_CHECK(scanState == StorageScanState::Completed);
        CORE_CHECK(scanEntries.size() >= 3);
    }

    const StorageAnalysisResult analysis =
        StorageAnalysisAccumulator::analyze(tmpDir.path(), scanEntries, scanStats, scanState);
    CORE_CHECK(analysis.filesByLogical.size() == 3);
    CORE_CHECK(analysis.dirsByLogical.size() >= 1);

    const auto treemapRoot = StorageTreemapBuilder::buildTree(
        tmpDir.path(), scanEntries, scanStats, scanState);
    CORE_CHECK(treemapRoot != nullptr);
    const auto rects = StorageTreemapLayout::layoutSquarified(
        treemapRoot.get(), QRectF(0.0, 0.0, 800.0, 600.0), StorageTreemapMetric::Logical);
    CORE_CHECK(!rects.isEmpty());

    {
        DuplicateFinderJob dupJob(tmpDir.path(), scanEntries, scanStats, scanState);
        DuplicateFinderResult dupResult;
        bool gotDups = false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&dupJob, &DuplicateFinderJob::finished, &loop, [&](const DuplicateFinderResult &res) {
            dupResult = res;
            gotDups = true;
            loop.quit();
        });
        dupJob.start();
        timeout.start(5000);
        loop.exec();

        CORE_CHECK(gotDups);
        CORE_CHECK(dupResult.finderState == DuplicateFinderState::Completed);
        CORE_CHECK(dupResult.groups.size() == 1);
        CORE_CHECK(dupResult.groups.first().files.size() == 2);
    }
}

void testBandwidthLimiter()
{
    TransferBandwidthLimiter limiter;
    limiter.setLimit(true, 1024ULL * 1024ULL); // 1 MiB/s
    const qint64 granted = limiter.acquire(1, 16384);
    CORE_CHECK(granted > 0);
    limiter.cancelTask(1);
    limiter.cancelAll();
}

} // namespace

bool verifyPortableCoreHeaderOnlyContracts();

int main(int argc, char **argv)
{
    // AUD-1B-01: Isolate XDG_CONFIG_HOME and QSettings BEFORE constructing
    // QCoreApplication or any QSettings-backed store.
    ScopedEnvVar savedXdgConfig("XDG_CONFIG_HOME");
    QTemporaryDir isolatedConfig(tempBaseDir() + QStringLiteral("/thispc-core-smoke-cfg-XXXXXX"));
    if (!isolatedConfig.isValid()) {
        std::fprintf(stderr, "thispc-core-smoke: failed to create isolated config temp dir\n");
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", isolatedConfig.path().toUtf8());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, isolatedConfig.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolatedConfig.path());

    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("kio-thispc"));
    app.setApplicationName(QStringLiteral("thispc-core-smoke"));

    testPortablePaths();
    testRemoteUrlAndSavedLocations(isolatedConfig.path());
    testApplicationStyle();
    testLifecycleAndWorkerPools();
    testChecksumStorageAndDuplicates();
    testBandwidthLimiter();
    CORE_CHECK(verifyPortableCoreHeaderOnlyContracts());

    if (g_failures > 0) {
        std::fprintf(stderr, "thispc-core-smoke: %d check(s) FAILED\n", g_failures);
        return 1;
    }

    std::printf("thispc-core-smoke: ALL CHECKS PASSED (Qt %s)\n", qVersion());
    return 0;
}
