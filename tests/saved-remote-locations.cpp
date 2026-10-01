/*
 * Regression suite for Saved Remote Locations:
 * persistence, security, deduplication, Unicode, and validation.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "browsercommon.h"
#include "remoteurlhelper.h"
#include "savedremotelocation.h"
#include "savedremotelocationdialog.h"

#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

static int checks = 0;

static void verify(bool condition, const char *description)
{
    if (!condition) {
        qFatal("FAIL: %s", description);
    }
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "temporary directory created");

    const QString configPath = tempDir.path() + QStringLiteral("/test-config.ini");
    QSettings testSettings(configPath, QSettings::IniFormat);

    SavedRemoteLocationsStore store;
    store.clear();

    // 1. Initial empty state
    verify(store.count() == 0, "initial store count is 0");
    verify(store.locations().isEmpty(), "initial store locations list is empty");
    verify(!store.hasLocation(QStringLiteral("nonexistent")), "hasLocation for nonexistent is false");
    verify(!store.containsUrl(QUrl(QStringLiteral("smb://nas/share"))), "containsUrl for nonexistent is false");

    // 2. Add new valid location
    QString smbId;
    const bool addSmbOk = store.addLocation(
        QStringLiteral("NAS Media"),
        QUrl(QStringLiteral("smb://nas.local/Media")),
        &smbId);
    verify(addSmbOk, "adding valid smb location succeeded");
    verify(!smbId.isEmpty(), "returned ID is non-empty");
    verify(store.count() == 1, "store count is now 1");
    verify(store.hasLocation(smbId), "store has location by ID");
    verify(store.containsUrl(QUrl(QStringLiteral("smb://nas.local/Media"))), "store contains added URL");

    SavedRemoteLocation smbLoc = store.locationById(smbId);
    verify(smbLoc.displayName == QStringLiteral("NAS Media"), "displayName matches");
    verify(smbLoc.url == QUrl(QStringLiteral("smb://nas.local/Media")), "URL matches");

    // 3. Persistence: save to settings and reload
    store.save(testSettings);
    testSettings.sync();

    SavedRemoteLocationsStore store2;
    store2.load(testSettings);
    verify(store2.count() == 1, "reloaded store has count 1");
    verify(store2.hasLocation(smbId), "reloaded store has saved ID");
    verify(store2.locationById(smbId).displayName == QStringLiteral("NAS Media"), "reloaded displayName matches");
    verify(store2.locationById(smbId).url == QUrl(QStringLiteral("smb://nas.local/Media")), "reloaded URL matches");

    // 4. Stable ID generation and ordering
    QString sftpId;
    const bool addSftpOk = store2.addLocation(
        QStringLiteral("Backup Server"),
        QUrl(QStringLiteral("sftp://backup.example.org:2222/storage")),
        &sftpId);
    verify(addSftpOk, "adding sftp location succeeded");
    verify(store2.count() == 2, "store count is 2");
    verify(store2.locations().at(0).id == smbId, "order preserved: first is SMB");
    verify(store2.locations().at(1).id == sftpId, "order preserved: second is SFTP");
    verify(sftpId == SavedRemoteLocationsStore::generateId(QUrl(QStringLiteral("sftp://backup.example.org:2222/storage"))), "ID generation is deterministic");

    // 5. Unicode support in display name and path
    QString unicodeId;
    const QString unicodeName = QStringLiteral("Serwer Pracy: Zażółć Gęślą Jaźń 💾");
    const QUrl unicodeUrl(QStringLiteral("sftp://host.test/pobranie/zażółć"));
    const bool addUnicodeOk = store2.addLocation(unicodeName, unicodeUrl, &unicodeId);
    verify(addUnicodeOk, "adding unicode location succeeded");
    verify(store2.locationById(unicodeId).displayName == unicodeName, "unicode name preserved in memory");

    store2.save(testSettings);
    testSettings.sync();

    SavedRemoteLocationsStore storeUnicode;
    storeUnicode.load(testSettings);
    verify(storeUnicode.locationById(unicodeId).displayName == unicodeName, "unicode name preserved after reload");

    // 6. Username preservation
    QString userSftpId;
    const QUrl userUrl(QStringLiteral("sftp://alice@prod.example.com:2200/home/alice"));
    verify(store2.addLocation(QStringLiteral("Alice Home"), userUrl, &userSftpId), "add location with user");
    verify(store2.locationById(userSftpId).url.userName() == QStringLiteral("alice"), "userName is preserved");
    verify(store2.locationById(userSftpId).url.port() == 2200, "port is preserved");

    // 7. Strict password sanitization before write
    QString passSftpId;
    const QUrl passUrl(QStringLiteral("sftp://bob:SuperSecretPassword999@vault.example.com/data"));
    verify(store2.addLocation(QStringLiteral("Bob Vault"), passUrl, &passSftpId), "add location with password in URL");
    const SavedRemoteLocation passLoc = store2.locationById(passSftpId);
    verify(passLoc.url.password().isEmpty(), "password stripped in store model");
    verify(!passLoc.url.toString().contains(QStringLiteral("SuperSecretPassword999")), "password not in model URL toString");
    verify(!passLoc.url.userInfo().contains(QStringLiteral("SuperSecretPassword999")), "password not in model userInfo");

    // 8. Strict password absence in QSettings file
    store2.save(testSettings);
    testSettings.sync();

    QFile settingsFile(configPath);
    verify(settingsFile.open(QIODevice::ReadOnly | QIODevice::Text), "opened config file for inspection");
    const QString rawConfig = QString::fromUtf8(settingsFile.readAll());
    settingsFile.close();
    verify(!rawConfig.contains(QStringLiteral("SuperSecretPassword999")), "password is NOT present in config file");

    // Legacy password migration: config containing a password must strip it on load and re-save cleaned
    testSettings.beginWriteArray(QStringLiteral("savedRemoteLocations"), 1);
    testSettings.setArrayIndex(0);
    testSettings.setValue(QStringLiteral("id"), QStringLiteral("legacy-test"));
    testSettings.setValue(QStringLiteral("displayName"), QStringLiteral("Legacy Server"));
    testSettings.setValue(QStringLiteral("url"), QStringLiteral("sftp://legacyuser:leakedsecret@legacy.host/dir"));
    testSettings.endArray();
    testSettings.sync();

    SavedRemoteLocationsStore migrationStore;
    migrationStore.load(testSettings);
    verify(migrationStore.count() == 1, "loaded legacy item");
    const auto legacyLoc = migrationStore.locations().at(0);
    verify(legacyLoc.url.password().isEmpty(), "password stripped during legacy load");
    verify(!legacyLoc.url.toString().contains(QStringLiteral("leakedsecret")), "leakedsecret absent from URL");

    // Re-inspect config file to confirm auto-rewrite sanitized it on disk
    testSettings.sync();
    verify(settingsFile.open(QIODevice::ReadOnly | QIODevice::Text), "reopened config file for rewrite check");
    const QString cleanedConfig = QString::fromUtf8(settingsFile.readAll());
    settingsFile.close();
    verify(!cleanedConfig.contains(QStringLiteral("leakedsecret")), "leakedsecret auto-rewritten away from config on disk");

    // 9. Rejection of malformed / unsupported URLs
    verify(!store2.addLocation(QStringLiteral("Bad 1"), QUrl(QStringLiteral("smb://"))), "reject empty host smb://");
    verify(!store2.addLocation(QStringLiteral("Bad 2"), QUrl(QStringLiteral("sftp://"))), "reject empty host sftp://");
    verify(!store2.addLocation(QStringLiteral("Bad 3"), QUrl(QStringLiteral("file:///home/user"))), "reject file://");
    verify(!store2.addLocation(QStringLiteral("Bad 4"), QUrl(QStringLiteral("http://example.com"))), "reject http://");
    verify(!store2.addLocation(QStringLiteral("Bad 5"), QUrl(QStringLiteral("admin:///root"))), "reject admin://");
    verify(!store2.addLocation(QStringLiteral("Bad 6"), QUrl(QStringLiteral("thispc:/"))), "reject thispc:/");
    verify(!store2.addLocation(QStringLiteral("Bad 7"), QUrl(QStringLiteral("invalid:not-a-url"))), "reject invalid URL");

    // 10. Acceptance of all 5 remote schemes
    verify(store2.addLocation(QStringLiteral("SMB"), QUrl(QStringLiteral("smb://s1/share"))), "smb:// accepted");
    verify(store2.addLocation(QStringLiteral("SFTP"), QUrl(QStringLiteral("sftp://s2/share"))), "sftp:// accepted");
    verify(store2.addLocation(QStringLiteral("FTP"), QUrl(QStringLiteral("ftp://s3/share"))), "ftp:// accepted");
    verify(store2.addLocation(QStringLiteral("WebDAV"), QUrl(QStringLiteral("webdav://s4/share"))), "webdav:// accepted");
    verify(store2.addLocation(QStringLiteral("WebDAVS"), QUrl(QStringLiteral("webdavs://s5/share"))), "webdavs:// accepted");

    // 11. Duplicate protection
    const QUrl dupUrl(QStringLiteral("smb://cluster.lan/public"));
    verify(store2.addLocation(QStringLiteral("Cluster Public"), dupUrl), "initial add of cluster URL ok");
    verify(!store2.addLocation(QStringLiteral("Cluster Duplicate"), dupUrl), "exact duplicate URL rejected");
    verify(!store2.addLocation(QStringLiteral("Cluster Duplicate Slash"), QUrl(QStringLiteral("smb://cluster.lan/public/"))), "duplicate URL with trailing slash rejected");

    // 12. Rename location
    const QString renameId = store2.locationForUrl(dupUrl).id;
    verify(!renameId.isEmpty(), "found ID for cluster location");
    verify(store2.renameLocation(renameId, QStringLiteral("New Cluster Name")), "rename location succeeded");
    verify(store2.locationById(renameId).displayName == QStringLiteral("New Cluster Name"), "displayName updated");
    verify(store2.locationById(renameId).url == dupUrl, "URL untouched after rename");
    verify(!store2.renameLocation(renameId, QStringLiteral("   ")), "rename with blank rejected");
    verify(!store2.renameLocation(QStringLiteral("nonexistent-id"), QStringLiteral("Test")), "rename nonexistent rejected");

    // 13. Remove location
    const int countBeforeRemove = store2.count();
    verify(store2.removeLocation(renameId), "remove location succeeded");
    verify(store2.count() == countBeforeRemove - 1, "count decremented after remove");
    verify(!store2.hasLocation(renameId), "hasLocation returns false after remove");
    verify(!store2.removeLocation(renameId), "removing already removed location returns false");
    verify(!store2.removeLocation(QStringLiteral("nonexistent-id")), "removing nonexistent returns false");

    // 14. Offline host persistence (no network I/O during addition)
    // 192.0.2.1 is TEST-NET-1 (RFC 5737, guaranteed non-routable)
    const QUrl offlineUrl(QStringLiteral("sftp://192.0.2.1:9999/remote/path"));
    verify(store2.addLocation(QStringLiteral("Offline Node"), offlineUrl), "offline host added without network blocking");
    verify(store2.containsUrl(offlineUrl), "store contains offline host");

    // 15. Default display name generation
    verify(SavedRemoteLocationsStore::defaultDisplayName(QUrl(QStringLiteral("smb://nas/Media"))) == QStringLiteral("Media"), "default name for share is Media");
    verify(SavedRemoteLocationsStore::defaultDisplayName(QUrl(QStringLiteral("smb://nas/Media/"))) == QStringLiteral("Media"), "default name for share with slash is Media");
    verify(SavedRemoteLocationsStore::defaultDisplayName(QUrl(QStringLiteral("sftp://alice@myserver.com/"))) == QStringLiteral("myserver.com"), "default name for root is host");
    verify(SavedRemoteLocationsStore::defaultDisplayName(QUrl(QStringLiteral("sftp://myserver.com:8022/"))) == QStringLiteral("myserver.com"), "default name for root with port is host");

    // 16. Icon resolution for remote schemes
    verify(RemoteUrlHelper::iconForRemoteUrl(QUrl(QStringLiteral("smb://nas/"))) == QStringLiteral("network-server"), "smb root icon network-server");
    verify(RemoteUrlHelper::iconForRemoteUrl(QUrl(QStringLiteral("smb://nas/Share"))) == QStringLiteral("folder-remote"), "smb share icon folder-remote");
    verify(RemoteUrlHelper::iconForRemoteUrl(QUrl(QStringLiteral("sftp://host/"))) == QStringLiteral("network-server"), "sftp root icon network-server");
    verify(RemoteUrlHelper::iconForRemoteUrl(QUrl(QStringLiteral("sftp://host/dir"))) == QStringLiteral("folder-remote"), "sftp dir icon folder-remote");
    verify(RemoteUrlHelper::iconForRemoteUrl(QUrl(QStringLiteral("ftp://host/dir"))) == QStringLiteral("folder-remote"), "ftp icon folder-remote");
    verify(RemoteUrlHelper::iconForRemoteUrl(QUrl(QStringLiteral("webdav://host/"))) == QStringLiteral("network-workgroup"), "webdav root icon network-workgroup");
    verify(RemoteUrlHelper::iconForRemoteUrl(QUrl(QStringLiteral("webdavs://host/dir"))) == QStringLiteral("folder-remote"), "webdavs dir icon folder-remote");

    // 17. Save Current Remote Location predicate and sanitization
    verify(RemoteUrlHelper::isRemoteUrl(QUrl(QStringLiteral("smb://server/share"))), "smb is remote url");
    verify(RemoteUrlHelper::isRemoteUrl(QUrl(QStringLiteral("sftp://server/share"))), "sftp is remote url");
    verify(RemoteUrlHelper::isRemoteUrl(QUrl(QStringLiteral("ftp://server/share"))), "ftp is remote url");
    verify(RemoteUrlHelper::isRemoteUrl(QUrl(QStringLiteral("webdav://server/share"))), "webdav is remote url");
    verify(RemoteUrlHelper::isRemoteUrl(QUrl(QStringLiteral("webdavs://server/share"))), "webdavs is remote url");
    verify(!RemoteUrlHelper::isRemoteUrl(QUrl(QStringLiteral("file:///home/user"))), "file is not remote url");
    verify(!RemoteUrlHelper::isRemoteUrl(QUrl(QStringLiteral("trash:/"))), "trash is not remote url");
    verify(!RemoteUrlHelper::isRemoteUrl(QUrl(QStringLiteral("thispc:/"))), "thispc is not remote url");
    verify(!RemoteUrlHelper::isRemoteUrl(QUrl(QStringLiteral("remote:/"))), "remote:/ network root is not a remote scheme url");

    const QUrl dirtyCurrent(QStringLiteral("sftp://admin:VerySecretPass99@nas.corp.lan:222/var/data"));
    const QUrl cleanCurrent = RemoteUrlHelper::sanitizeUrl(dirtyCurrent);
    verify(cleanCurrent.password().isEmpty(), "cleanCurrent password stripped");
    verify(!cleanCurrent.toString().contains(QStringLiteral("VerySecretPass99")), "cleanCurrent string has no password");
    verify(cleanCurrent.userName() == QStringLiteral("admin"), "cleanCurrent user preserved");
    verify(cleanCurrent.host() == QStringLiteral("nas.corp.lan"), "cleanCurrent host preserved");
    verify(cleanCurrent.port() == 222, "cleanCurrent port preserved");
    verify(cleanCurrent.path() == QStringLiteral("/var/data"), "cleanCurrent path preserved");

    // 18. Dialog headless instantiation and validation check
    SavedRemoteLocationDialog addDialog(
        SavedRemoteLocationDialog::Mode::Add,
        QUrl(QStringLiteral("sftp://alice:pass@myhost/work")),
        QString(),
        QString(),
        nullptr);
    // Dialog should auto-sanitize URL on load
    verify(addDialog.url().password().isEmpty(), "addDialog URL has empty password");
    verify(addDialog.displayName() == QStringLiteral("work"), "addDialog default displayName generated from path");

    // 19. Stage 3 Security Contract: urlForDisplay and toString never leak password
    const QUrl passDirty(QStringLiteral("sftp://operator:SuperSecret999@vault.corp.net:222/srv/data"));
    const QUrl passClean = RemoteUrlHelper::sanitizeUrl(passDirty);
    verify(passClean.password().isEmpty(), "sanitized URL has empty password");
    verify(!urlForDisplay(passClean).contains(QStringLiteral("SuperSecret999")), "urlForDisplay never contains password");
    verify(!passClean.toDisplayString().contains(QStringLiteral("SuperSecret999")), "toDisplayString never contains password");
    verify(!passClean.toString().contains(QStringLiteral("SuperSecret999")), "toString never contains password");

    // 20. Config structure contract: keys inside savedRemoteLocations are only id, displayName/name, url
    store2.save(testSettings);
    testSettings.sync();
    testSettings.beginReadArray(QStringLiteral("savedRemoteLocations"));
    const QStringList childKeys = testSettings.childKeys();
    for (const QString &key : childKeys) {
        verify(key != QStringLiteral("password") && key != QStringLiteral("secret") &&
               key != QStringLiteral("credential") && key != QStringLiteral("token") &&
               key != QStringLiteral("authInfo"), "no credential keys in config");
    }
    testSettings.endArray();
    verify(!testSettings.contains(QStringLiteral("savedRemoteLocations/password")), "no top-level password in config");
    verify(!testSettings.contains(QStringLiteral("savedRemoteLocations/secret")), "no top-level secret in config");

    printf("saved_remote: %d assertions passed\n", checks);
    return 0;
}
